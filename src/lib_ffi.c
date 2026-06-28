/*
** FFI library.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lib_ffi_c
#define LUA_LIB

#include <errno.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"

#if LJ_HASFFI

#include "lj_atomic.h"
#include "lj_char.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_ctype.h"
#include "lj_cparse.h"
#include "lj_cdata.h"
#include "lj_cconv.h"
#include "lj_carith.h"
#include "lj_udata.h"
#include "lj_ccall.h"
#include "lj_ccallback.h"
#include "lj_clib.h"
#include "lj_strfmt.h"
#include "lj_ff.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lj_lib.h"

/* -- C type checks ------------------------------------------------------- */

static CTypeID ffi_parse_ctype_locked(lua_State *L, CTState *cts,
				      TValue *param, int *errcode)
{
  GCstr *s = strV(L->base);
  CPState cp;
  cp.L = L;
  cp.cts = cts;
  cp.srcname = strdata(s);
  cp.p = strdata(s);
  cp.param = param;
  cp.mode = CPARSE_MODE_ABSTRACT|CPARSE_MODE_NOIMPLICIT;
  *errcode = lj_cparse(&cp);
  return cp.val.id;
}

static int ffi_cspace(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	 c == '\f' || c == '\v';
}

static int ffi_strlit(const char *p, MSize plen, const char *lit, MSize len)
{
  if (plen != len)
    return 0;
  while (len-- != 0)
    if (*p++ != *lit++)
      return 0;
  return 1;
}

#define ffi_ctype_match(lit) \
  ffi_strlit(p, len, "" lit, (MSize)(sizeof(lit)-1))

static int ffi_predefined_ctype_part(const char *p, MSize len, CTypeID *idp)
{
  return lj_ctype_predefined_string(p, len, idp);
}

static int ffi_predefined_ctype_string(GCstr *s, CTypeID *idp)
{
  return ffi_predefined_ctype_part(strdata(s), s->len, idp);
}

static int ffi_ident_string(const char *p, MSize len)
{
  MSize i;
  if (len == 0 || lj_char_isdigit((uint8_t)p[0]))
    return 0;
  for (i = 0; i < len; i++)
    if (!lj_char_isident((uint8_t)p[i]))
      return 0;
  return 1;
}

static int ffi_lookup_named_ctype(lua_State *L, CTState *cts, GCstr *name,
				  uint32_t tmask, CTypeID *idp, CType *out)
{
  int ok = lj_ctype_getname_snapshot(cts, name, tmask, idp, out, NULL);
  if (ok < 0)
    ok = lj_ctype_getname_wait(L, cts, name, tmask, idp, out, NULL);
  return ok;
}

static int ffi_ctype_info_read(lua_State *L, CTState *cts, CTypeID id,
			       CTInfo *infop, CTSize *szp, CTypeID *ridp,
			       CType *rawp);

static int ffi_qual_token(const char *p, MSize len, CTInfo *qualp)
{
  if (len == 5 && ffi_strlit(p, len, "const", 5)) {
    *qualp = CTF_CONST;
    return 1;
  }
  if (len == 8 && ffi_strlit(p, len, "volatile", 8)) {
    *qualp = CTF_VOLATILE;
    return 1;
  }
  if (len == 7 && ffi_strlit(p, len, "__const", 7)) {
    *qualp = CTF_CONST;
    return 1;
  }
  if (len == 9 && ffi_strlit(p, len, "__const__", 9)) {
    *qualp = CTF_CONST;
    return 1;
  }
  if (len == 10 && ffi_strlit(p, len, "__volatile", 10)) {
    *qualp = CTF_VOLATILE;
    return 1;
  }
  if (len == 12 && ffi_strlit(p, len, "__volatile__", 12)) {
    *qualp = CTF_VOLATILE;
    return 1;
  }
  if (len == 8 && ffi_strlit(p, len, "restrict", 8)) {
    *qualp = 0;
    return 1;
  }
  if (len == 10 && ffi_strlit(p, len, "__restrict", 10)) {
    *qualp = 0;
    return 1;
  }
  if (len == 12 && ffi_strlit(p, len, "__restrict__", 12)) {
    *qualp = 0;
    return 1;
  }
  if (len == 13 && ffi_strlit(p, len, "__extension__", 13)) {
    *qualp = 0;
    return 1;
  }
  return 0;
}

static int ffi_sign_token(const char *p, MSize len, int *signp)
{
  if (len == 6 && ffi_strlit(p, len, "signed", 6)) {
    *signp = 1;
    return 1;
  }
  if (len == 8 && ffi_strlit(p, len, "__signed", 8)) {
    *signp = 1;
    return 1;
  }
  if (len == 10 && ffi_strlit(p, len, "__signed__", 10)) {
    *signp = 1;
    return 1;
  }
  if (len == 8 && ffi_strlit(p, len, "unsigned", 8)) {
    *signp = 2;
    return 1;
  }
  return 0;
}

static int ffi_int_keyword_token(const char *p, MSize len, int *bitsp)
{
  if (len == 6 && ffi_strlit(p, len, "__int8", 6)) {
    *bitsp = 8;
    return 1;
  }
  if (len == 7 && ffi_strlit(p, len, "__int16", 7)) {
    *bitsp = 16;
    return 1;
  }
  if (len == 7 && ffi_strlit(p, len, "__int32", 7)) {
    *bitsp = 32;
    return 1;
  }
  if (len == 7 && ffi_strlit(p, len, "__int64", 7)) {
    *bitsp = 64;
    return 1;
  }
  if (len == 8 && ffi_strlit(p, len, "__int128", 8)) {
    *bitsp = 128;
    return 1;
  }
  return 0;
}

static int ffi_integer_spec_token(const char *p, MSize len, int *tokp)
{
  if (len == 4 && ffi_strlit(p, len, "char", 4)) {
    *tokp = 1;
    return 1;
  }
  if (len == 5 && ffi_strlit(p, len, "short", 5)) {
    *tokp = 2;
    return 1;
  }
  if (len == 3 && ffi_strlit(p, len, "int", 3)) {
    *tokp = 3;
    return 1;
  }
  if (len == 4 && ffi_strlit(p, len, "long", 4)) {
    *tokp = 4;
    return 1;
  }
  return 0;
}

static int ffi_direct_int_keyword_ctype(lua_State *L, CTState *cts,
					const char *p, MSize len,
					CTypeID *idp)
{
  int sign = 0, bits = 0;
  while (len != 0 && ffi_cspace(*p)) { p++; len--; }
  while (len != 0 && ffi_cspace(p[len-1])) len--;
  while (len != 0) {
    MSize toklen = 0;
    int tok;
    while (toklen < len && !ffi_cspace(p[toklen])) toklen++;
    if (ffi_int_keyword_token(p, toklen, &tok)) {
      if (bits != 0)
	return 0;
      bits = tok;
    } else if (ffi_sign_token(p, toklen, &tok)) {
      if (sign != 0)
	return 0;
      sign = tok;
    } else {
      return 0;
    }
    p += toklen;
    len -= toklen;
    while (len != 0 && ffi_cspace(*p)) { p++; len--; }
  }
  if (bits == 0)
    return 0;
  if (sign == 2) {
    if (bits == 8) *idp = CTID_UINT8;
    else if (bits == 16) *idp = CTID_UINT16;
    else if (bits == 32) *idp = CTID_UINT32;
    else if (bits == 128) *idp = CTID_UINT128;
    else *idp = lj_ctype_intern_l(L, cts,
				  CTINFO(CT_NUM, CTF_UNSIGNED|CTALIGN(3)), 8);
  } else {
    if (bits == 8) *idp = CTID_INT8;
    else if (bits == 16) *idp = CTID_INT16;
    else if (bits == 32) *idp = CTID_INT32;
    else if (bits == 128) *idp = CTID_INT128;
    else *idp = lj_ctype_intern_l(L, cts, CTINFO(CT_NUM, CTALIGN(3)), 8);
  }
  return 1;
}

static int ffi_direct_integer_spec_ctype(lua_State *L, CTState *cts,
					 const char *p, MSize len,
					 CTypeID *idp)
{
  int sign = 0, seen_char = 0, seen_short = 0, seen_int = 0, nlong = 0;
  int seen = 0;
  while (len != 0 && ffi_cspace(*p)) { p++; len--; }
  while (len != 0 && ffi_cspace(p[len-1])) len--;
  while (len != 0) {
    MSize toklen = 0;
    int tok;
    while (toklen < len && !ffi_cspace(p[toklen])) toklen++;
    if (ffi_sign_token(p, toklen, &tok)) {
      if (sign != 0)
	return 0;
      sign = tok;
    } else if (ffi_integer_spec_token(p, toklen, &tok)) {
      if (tok == 1) {
	if (seen_char || seen_short || seen_int || nlong)
	  return 0;
	seen_char = 1;
      } else if (tok == 2) {
	if (seen_char || seen_short || nlong)
	  return 0;
	seen_short = 1;
      } else if (tok == 3) {
	if (seen_char || seen_int)
	  return 0;
	seen_int = 1;
      } else {
	if (seen_char || seen_short || nlong == 2)
	  return 0;
	nlong++;
      }
    } else {
      return 0;
    }
    seen = 1;
    p += toklen;
    len -= toklen;
    while (len != 0 && ffi_cspace(*p)) { p++; len--; }
  }
  if (!seen)
    return 0;
  if (seen_char) {
    *idp = sign == 2 ? CTID_UINT8 : CTID_INT8;
  } else if (seen_short) {
    *idp = sign == 2 ? CTID_UINT16 : CTID_INT16;
  } else if (nlong == 2) {
    *idp = lj_ctype_intern_l(L, cts,
			     CTINFO(CT_NUM,
				    (sign == 2 ? CTF_UNSIGNED : 0)|CTALIGN(3)),
			     8);
  } else if (nlong == 1) {
    if (sizeof(long) != 8)
      return 0;
    *idp = sign == 2 ? CTID_UINT64 : CTID_INT64;
  } else {
    *idp = sign == 2 ? CTID_UINT32 : CTID_INT32;
  }
  return 1;
}

static int ffi_direct_numeric_ctype(lua_State *L, CTState *cts,
				    const char *p, MSize len, CTypeID *idp)
{
  CTInfo info;
  CTSize size;
  while (len != 0 && ffi_cspace(*p)) { p++; len--; }
  while (len != 0 && ffi_cspace(p[len-1])) len--;
  if (ffi_ctype_match("long long") ||
      ffi_ctype_match("long long int") ||
      ffi_ctype_match("signed long long") ||
      ffi_ctype_match("signed long long int")) {
    info = CTINFO(CT_NUM, CTALIGN(3));
    size = 8;
  } else if (ffi_ctype_match("unsigned long long") ||
	     ffi_ctype_match("unsigned long long int")) {
    info = CTINFO(CT_NUM, CTF_UNSIGNED|CTALIGN(3));
    size = 8;
  } else if (ffi_ctype_match("long double")) {
    size = (CTSize)sizeof(long double);
    info = CTINFO(CT_NUM, CTF_FP|CTALIGN(lj_fls(size)));
  } else {
    return 0;
  }
  *idp = lj_ctype_intern_l(L, cts, info, size);
  return 1;
}

static int ffi_qual_prefix(const char *p, MSize len, MSize *toklenp,
			   CTInfo *qualp)
{
  MSize i = 0;
  while (i < len && lj_char_isident((uint8_t)p[i])) i++;
  if (i == 0 || (i < len && !ffi_cspace(p[i])))
    return 0;
  if (!ffi_qual_token(p, i, qualp))
    return 0;
  *toklenp = i;
  return 1;
}

static int ffi_qual_suffix(const char *p, MSize len, MSize *startp,
			   CTInfo *qualp)
{
  MSize start = len;
  while (start != 0 && lj_char_isident((uint8_t)p[start-1])) start--;
  if (start == len || (start != 0 && !ffi_cspace(p[start-1])))
    return 0;
  if (!ffi_qual_token(p + start, len - start, qualp))
    return 0;
  *startp = start;
  return 1;
}

static int ffi_direct_qual_part(const char **pp, MSize *lenp, CTInfo *qualp)
{
  const char *p = *pp;
  MSize len = *lenp;
  CTInfo qual = 0;
  int seen = 0;
  for (;;) {
    CTInfo q;
    MSize toklen;
    while (len != 0 && ffi_cspace(*p)) { p++; len--; }
    if (!ffi_qual_prefix(p, len, &toklen, &q))
      break;
    qual |= q;
    seen = 1;
    p += toklen;
    len -= toklen;
  }
  for (;;) {
    CTInfo q;
    MSize start;
    while (len != 0 && ffi_cspace(p[len-1])) len--;
    if (!ffi_qual_suffix(p, len, &start, &q))
      break;
    qual |= q;
    seen = 1;
    len = start;
  }
  while (len != 0 && ffi_cspace(*p)) { p++; len--; }
  while (len != 0 && ffi_cspace(p[len-1])) len--;
  *pp = p;
  *lenp = len;
  *qualp = qual;
  return seen;
}

static int ffi_direct_qualified_ctype(lua_State *L, CTState *cts,
				      CTypeID baseid, CTInfo qual,
				      CTypeID *idp)
{
  CType raw;
  CTypeID rid;
  CTInfo info;
  CTSize size;
  int ok = ffi_ctype_info_read(L, cts, baseid, &info, &size, &rid, &raw);
  if (ok <= 0)
    return 0;
  if (qual == 0) {
    *idp = baseid;
    return 1;
  }
  info = ctype_info_acq(&raw);
  size = ctype_size_acq(&raw);
  if (ctype_isstruct(info) || ctype_isenum(info)) {
    *idp = lj_ctype_intern_l(L, cts,
			     CTINFO(CT_ATTRIB, CTATTRIB(CTA_QUAL)|rid),
			     qual);
    return 1;
  }
  if (ctype_isattrib(info))
    return 0;
  *idp = lj_ctype_intern_l(L, cts, info|qual, size);
  return 1;
}

static int ffi_direct_ctype_base_unqualified(lua_State *L, CTState *cts,
					     GCstr *s, const char *p,
					     MSize len, CTypeID *idp)
{
  CType ct;
  CTypeID id;
  CTInfo info;
  if (ffi_predefined_ctype_part(p, len, idp))
    return 1;
  if (ffi_direct_int_keyword_ctype(L, cts, p, len, idp))
    return 1;
  if (ffi_direct_integer_spec_ctype(L, cts, p, len, idp))
    return 1;
  if (ffi_direct_numeric_ctype(L, cts, p, len, idp))
    return 1;
  while (len != 0 && ffi_cspace(*p)) { p++; len--; }
  while (len != 0 && ffi_cspace(p[len-1])) len--;
  if (ffi_ident_string(p, len)) {
    GCstr *name = (p == strdata(s) && len == s->len) ? s :
		  lj_str_new(L, p, len);
    int ok = ffi_lookup_named_ctype(L, cts, name, (1u << CT_TYPEDEF),
				    &id, &ct);
    if (ok > 0) {
      info = ctype_info_acq(&ct);
      if (ctype_istypedef(info)) {
	*idp = ctype_cid(info);
	return 1;
      }
    }
  } else {
    uint32_t tmask;
    int wantunion = 0, wantenum = 0;
    MSize kwlen;
    if (len >= 6 && ffi_strlit(p, 6, "struct", 6)) {
      kwlen = 6;
      tmask = (1u << CT_STRUCT);
    } else if (len >= 5 && ffi_strlit(p, 5, "union", 5)) {
      kwlen = 5;
      tmask = (1u << CT_STRUCT);
      wantunion = 1;
    } else if (len >= 4 && ffi_strlit(p, 4, "enum", 4)) {
      kwlen = 4;
      tmask = (1u << CT_ENUM);
      wantenum = 1;
    } else {
      return 0;
    }
    if (len == kwlen || !ffi_cspace(p[kwlen]))
      return 0;
    p += kwlen + 1;
    len -= kwlen + 1;
    while (len != 0 && ffi_cspace(*p)) { p++; len--; }
    if (!ffi_ident_string(p, len))
      return 0;
    {
      GCstr *name = lj_str_new(L, p, len);
      int ok = ffi_lookup_named_ctype(L, cts, name, tmask, &id, &ct);
      if (ok > 0) {
	info = ctype_info_acq(&ct);
	if (wantenum ? ctype_isenum(info) :
	    ctype_isstruct(info) && (((info & CTF_UNION) != 0) == wantunion)) {
	  *idp = id;
	  return 1;
	}
      }
    }
  }
  return 0;
}

static int ffi_direct_ctype_base_string(lua_State *L, CTState *cts, GCstr *s,
					const char *p, MSize len,
					CTypeID *idp)
{
  const char *q = p;
  MSize qlen = len;
  CTInfo qual;
  if (ffi_direct_qual_part(&q, &qlen, &qual)) {
    CTypeID baseid;
    if (qlen == 0)
      return 0;
    if (ffi_direct_ctype_base_unqualified(L, cts, s, q, qlen, &baseid) &&
	ffi_direct_qualified_ctype(L, cts, baseid, qual, idp))
      return 1;
    return 0;
  }
  return ffi_direct_ctype_base_unqualified(L, cts, s, p, len, idp);
}

static int ffi_pointer_qual_suffix(const char *p, MSize *lenp, CTInfo *qualp)
{
  MSize len = *lenp;
  MSize start = len;
  while (start != 0 && lj_char_isident((uint8_t)p[start-1])) start--;
  if (start == len)
    return 0;
  if (start != 0 && !ffi_cspace(p[start-1]) && p[start-1] != '*')
    return 0;
  if (!ffi_qual_token(p + start, len - start, qualp))
    return 0;
  *lenp = start;
  return 1;
}

static int ffi_direct_pointer_suffix(const char *p, MSize *lenp,
				     CTInfo *qualp)
{
  MSize len = *lenp;
  CTInfo qual = 0;
  for (;;) {
    CTInfo q;
    while (len != 0 && ffi_cspace(p[len-1])) len--;
    if (!ffi_pointer_qual_suffix(p, &len, &q))
      break;
    qual |= q;
  }
  while (len != 0 && ffi_cspace(p[len-1])) len--;
  if (len == 0 || p[len-1] != '*')
    return 0;
  len--;
  while (len != 0 && ffi_cspace(p[len-1])) len--;
  *lenp = len;
  *qualp = qual;
  return 1;
}

static int ffi_direct_array_suffix(const char *p, MSize *lenp, CTSize *nelemp)
{
  MSize len = *lenp, i, dstart, dend;
  uint64_t nelem = 0;
  while (len != 0 && ffi_cspace(p[len-1])) len--;
  if (len == 0 || p[len-1] != ']')
    return 0;
  i = len - 1;
  while (i != 0 && ffi_cspace(p[i-1])) i--;
  dend = i;
  while (i != 0 && lj_char_isdigit((uint8_t)p[i-1])) i--;
  dstart = i;
  if (dstart == dend)
    return 0;
  if (dend - dstart > 1 && p[dstart] == '0')
    return 0;  /* Keep C octal/hex integer spellings on the parser path. */
  for (i = dstart; i < dend; i++) {
    nelem = nelem * 10u + (uint32_t)(p[i] - '0');
    if (nelem >= 0x80000000u)
      return 0;
  }
  i = dstart;
  while (i != 0 && ffi_cspace(p[i-1])) i--;
  if (i == 0 || p[i-1] != '[')
    return 0;
  i--;
  while (i != 0 && ffi_cspace(p[i-1])) i--;
  if (i == 0)
    return 0;
  *lenp = i;
  *nelemp = (CTSize)nelem;
  return 1;
}

static int ffi_direct_array_ctype(lua_State *L, CTState *cts, CTypeID elemid,
				  CTSize nelem, CTypeID *idp)
{
  CTInfo einfo, ainfo;
  CTSize esize;
  uint64_t asize;
  int ok = ffi_ctype_info_read(L, cts, elemid, &einfo, &esize, NULL, NULL);
  if (ok <= 0 || ctype_isref(einfo) || ctype_isvltype(einfo) ||
      esize == CTSIZE_INVALID)
    return 0;
  asize = (uint64_t)nelem * esize;
  if (asize >= 0x80000000u)
    return 0;
  ainfo = CTINFO(CT_ARRAY, elemid);
  ainfo |= (einfo & (CTF_ALIGN|CTF_QUAL));
  *idp = lj_ctype_intern_l(L, cts, ainfo, (CTSize)asize);
  return 1;
}

static int ffi_direct_ctype_part(lua_State *L, CTState *cts, GCstr *s,
				 const char *p, MSize len, CTypeID *idp)
{
  enum { FFI_DIRECT_MAX_POINTERS = 8 };
  while (len != 0 && ffi_cspace(*p)) { p++; len--; }
  while (len != 0 && ffi_cspace(p[len-1])) len--;
  {
    MSize baselen = len;
    CTInfo pqual[FFI_DIRECT_MAX_POINTERS];
    MSize nptr = 0;
    CTypeID baseid;
    for (;;) {
      CTInfo qual;
      MSize nextlen = baselen;
      if (!ffi_direct_pointer_suffix(p, &nextlen, &qual))
	break;
      if (nptr == FFI_DIRECT_MAX_POINTERS) {
	nptr = 0;
	break;
      }
      pqual[nptr++] = qual;
      baselen = nextlen;
    }
    if (nptr != 0 && baselen != 0 &&
	ffi_direct_ctype_base_string(L, cts, s, p, baselen, &baseid)) {
      while (nptr-- != 0)
	baseid = lj_ctype_intern_l(L, cts,
				   CTINFO(CT_PTR,
					  CTALIGN_PTR|pqual[nptr]|baseid),
				   CTSIZE_PTR);
      *idp = baseid;
      return 1;
    }
  }
  return ffi_direct_ctype_base_string(L, cts, s, p, len, idp);
}

static int ffi_direct_ctype_string(lua_State *L, CTState *cts, GCstr *s,
				   CTypeID *idp)
{
  enum { FFI_DIRECT_MAX_ARRAYS = 8 };
  const char *p = strdata(s);
  MSize len = s->len;
  while (len != 0 && ffi_cspace(*p)) { p++; len--; }
  while (len != 0 && ffi_cspace(p[len-1])) len--;
  {
    MSize baselen = len;
    CTSize nelem[FFI_DIRECT_MAX_ARRAYS];
    CTypeID elemid;
    MSize narr = 0;
    for (;;) {
      CTSize n;
      MSize nextlen = baselen;
      if (!ffi_direct_array_suffix(p, &nextlen, &n))
	break;
      if (narr == FFI_DIRECT_MAX_ARRAYS) {
	narr = 0;
	break;
      }
      nelem[narr++] = n;
      baselen = nextlen;
    }
    if (narr != 0 && ffi_direct_ctype_part(L, cts, s, p, baselen, &elemid)) {
      MSize i;
      for (i = 0; i < narr; i++) {
	if (!ffi_direct_array_ctype(L, cts, elemid, nelem[i], &elemid))
	  return 0;
      }
      *idp = elemid;
      return 1;
    }
  }
  return ffi_direct_ctype_part(L, cts, s, p, len, idp);
}

/* Check first argument for a C type and returns its ID. */
static CTypeID ffi_checkctype(lua_State *L, CTState *cts, TValue *param)
{
  TValue *o = L->base;
  if (!(o < L->top)) {
  err_argtype:
    lj_err_argtype(L, 1, "C type");
  }
  if (tvisstr(o)) {  /* Parse an abstract C type declaration. */
    GCstr *s = strV(o);
    int errcode;
    CTypeID id;
    if ((!param || param >= L->top) && ffi_predefined_ctype_string(s, &id))
      return id;  /* 11.2: immutable predefined ctype names need no parser. */
    if ((!param || param >= L->top) &&
	ffi_direct_ctype_string(L, cts, s, &id))
      return id;
    lj_ctype_parse_lock(cts, L);
    id = ffi_parse_ctype_locked(L, cts, param, &errcode);
    lj_ctype_parse_unlock(cts);
    if (errcode) lj_err_throw(L, errcode);  /* Propagate errors. */
    return id;
  } else {
    GCcdata *cd;
    if (!tviscdata(o)) goto err_argtype;
    if (param && param < L->top) lj_err_arg(L, 1, LJ_ERR_FFI_NUMPARAM);
    cd = cdataV(o);
    return cd->ctypeid == CTID_CTYPEID ? *(CTypeID *)cdataptr(cd) : cd->ctypeid;
  }
}

static CTypeID ffi_checkctype_noparse(lua_State *L, TValue *param, int *isstr)
{
  TValue *o = L->base;
  if (!(o < L->top)) {
  err_argtype:
    lj_err_argtype(L, 1, "C type");
  }
  if (tvisstr(o)) {
    *isstr = 1;
    return 0;
  } else {
    GCcdata *cd;
    *isstr = 0;
    if (!tviscdata(o)) goto err_argtype;
    if (param && param < L->top) lj_err_arg(L, 1, LJ_ERR_FFI_NUMPARAM);
    cd = cdataV(o);
    return cd->ctypeid == CTID_CTYPEID ? *(CTypeID *)cdataptr(cd) : cd->ctypeid;
  }
}

static int ffi_new_layout_snapshot(CTState *cts, CTypeID id, CTSize nelem,
				   int hasnelem, CTypeID *ridp,
				   CTInfo *infop, CTSize *szp,
				   int *neednelem);

static int ffi_new_layout_wait(lua_State *L, CTState *cts, CTypeID id,
			       CTSize nelem, int hasnelem, CTypeID *ridp,
			       CTInfo *infop, CTSize *szp, int *neednelem);

/* Check argument for C data and return it. */
static GCcdata *ffi_checkcdata(lua_State *L, int narg)
{
  TValue *o = L->base + narg-1;
  if (!(o < L->top && tviscdata(o)))
    lj_err_argt(L, narg, LUA_TCDATA);
  return cdataV(o);
}

/* Convert argument to C pointer. */
static void *ffi_checkptr(lua_State *L, int narg, CTypeID id)
{
  CTState *cts = ctype_cts(L);
  TValue *o = L->base + narg-1;
  void *p;
  if (o >= L->top)
    lj_err_arg(L, narg, LJ_ERR_NOVAL);
  lj_cconv_ct_tv_l(L, cts, ctype_get(cts, id), id, (uint8_t *)&p, o,
		   CCF_ARG(narg));
  return p;
}

/* Convert argument to int32_t. */
static int32_t ffi_checkint(lua_State *L, int narg)
{
  CTState *cts = ctype_cts(L);
  TValue *o = L->base + narg-1;
  int32_t i;
  if (o >= L->top)
    lj_err_arg(L, narg, LJ_ERR_NOVAL);
  lj_cconv_ct_tv_l(L, cts, ctype_get(cts, CTID_INT32), CTID_INT32,
		   (uint8_t *)&i, o, CCF_ARG(narg));
  return i;
}

/* -- C type metamethods -------------------------------------------------- */

#define LJLIB_MODULE_ffi_meta

#if LJ_HASJIT
static jit_State *ffi_active_recorder(lua_State *L)
{
  jit_State *J = G2J(G(L));
  return J->L == L && lj_jit_token_held(J) &&
	 lj_trace_state_load(J) != LJ_TRACE_IDLE ? J : NULL;
}
#endif

static int ffi_ctype_predefined_id(CTypeID id)
{
  return id > CTID_NONE && id <= CTID_CTYPEID;
}

static void ffi_ctype_slot_snapshot(CTypeTab *tabh, CTypeID id, CType *out)
{
  CType *ct = ctype_tab_slot(tabh, id);
  GCobj *name;
  out->info = ctype_info_acq(ct);
  out->size = ctype_size_acq(ct);
  out->sib = (CTypeID1)ctype_sib_acq(ct);
  out->next = (CTypeID1)ctype_next_acq(ct);
  name = ctype_nameobj_acq(ct);
  setgcrefp(out->name, name);
}

static int ffi_ctype_predefined_snapshot(CTState *cts, CTypeID id, CType *out)
{
  CTypeTab *tabh;
  if (!ffi_ctype_predefined_id(id))
    return 0;
  tabh = ctype_tabh_acq(cts);
  if ((MSize)CTID_CTYPEID >= ctype_tab_sizetab_acq(tabh))
    return 0;
  ffi_ctype_slot_snapshot(tabh, id, out);
  return !ctype_isabandoned(ctype_info_acq(out));
}

static int ffi_ctype_info_read(lua_State *L, CTState *cts, CTypeID id,
			       CTInfo *infop, CTSize *szp, CTypeID *ridp,
			       CType *rawp)
{
  int ok = lj_ctype_info_predefined(cts, id, infop, szp, ridp, rawp);
  if (ok)
    return ok;
  ok = lj_ctype_info_snapshot(cts, id, infop, szp, ridp, rawp);
  if (ok < 0) {
#if LJ_HASJIT
    jit_State *J = ffi_active_recorder(L);
    if (J)
      lj_trace_err(J, LJ_TRERR_CTBUSY);
#endif
    ok = lj_ctype_info_wait(L, cts, id, infop, szp, ridp, rawp);
  }
  return ok;
}

static cTValue *ffi_ctype_metatv_read(lua_State *L, CTState *cts,
				      TValue *out, CTypeID id, MMS mm)
{
  int ok;
  if (lj_ctype_predefined_nometa(cts, id)) {
    setnilV(out);
    return NULL;
  }
  ok = lj_ctype_metatv_snapshot(cts, out, id, mm);
  if (ok < 0) {
#if LJ_HASJIT
    jit_State *J = ffi_active_recorder(L);
    if (J)
      lj_trace_err(J, LJ_TRERR_CTBUSY);
#endif
    return lj_ctype_metatv_wait(L, cts, out, id, mm);
  }
  return ok ? out : NULL;
}

/* Handle ctype __index/__newindex metamethods. */
static int ffi_index_meta(lua_State *L, CTState *cts, CTypeID id, MMS mm)
{
  TValue metatv;
  cTValue *tv = ffi_ctype_metatv_read(L, cts, &metatv, id, mm);
  TValue *base = L->base;
  if (!tv) {
    const char *s;
  err_index:
    s = strdata(lj_ctype_repr(L, id, NULL));
    if (tvisstr(L->base+1)) {
      lj_err_callerv(L, LJ_ERR_FFI_BADMEMBER, s, strVdata(L->base+1));
    } else {
      const char *key = tviscdata(L->base+1) ?
	strdata(lj_ctype_repr(L, cdataV(L->base+1)->ctypeid, NULL)) :
	lj_typename(L->base+1);
      lj_err_callerv(L, LJ_ERR_FFI_BADIDXW, s, key);
    }
  }
  if (!tvisfunc(tv)) {
    if (mm == MM_index) {
      cTValue *o = lj_meta_tget(L, tv, base+1);
      if (o) {
	if (tvisnil(o)) goto err_index;
	copyTV(L, L->top-1, o);
	return 1;
      }
    } else {
      GCtab *owner;
      TValue *o = lj_meta_tset_owner(L, tv, base+1, &owner);
      if (o) {
	copyTVrel(L, o, base+2);
	lj_gc2_barrier_weak_write(L, owner, base+1, base+2);
	lj_gc2_barrier_tv_pair(L, obj2gco(owner), o);
	return 0;
      }
    }
    copyTV(L, base, L->top);
    tv = L->top-1-LJ_FR2;
  }
  return lj_meta_tailcall(L, tv);
}

LJLIB_CF(ffi_meta___index)	LJLIB_REC(cdata_index 0)
{
  CTState *cts = ctype_cts(L);
  CTInfo qual = 0;
  CTypeID id = 0;
  CType snap;
  CType *ct;
  uint8_t *p;
  TValue *o = L->base;
  if (!(o+1 < L->top && tviscdata(o)))  /* Also checks for presence of key. */
    lj_err_argt(L, 1, LUA_TCDATA);
  ct = lj_cdata_index_l(L, cts, cdataV(o), o+1, &p, &qual, &snap, &id);
  if ((qual & 1))
    return ffi_index_meta(L, cts, id, MM_index);
  if (lj_cdata_get_l(L, cts, ct, L->top-1, p))
    lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_meta___newindex)	LJLIB_REC(cdata_index 1)
{
  CTState *cts = ctype_cts(L);
  CTInfo qual = 0;
  CTypeID id = 0;
  CType snap;
  CType *ct;
  uint8_t *p;
  TValue *o = L->base;
  if (!(o+2 < L->top && tviscdata(o)))  /* Also checks for key and value. */
    lj_err_argt(L, 1, LUA_TCDATA);
  ct = lj_cdata_index_l(L, cts, cdataV(o), o+1, &p, &qual, &snap, &id);
  if ((qual & 1)) {
    if ((qual & CTF_CONST))
      lj_err_caller(L, LJ_ERR_FFI_WRCONST);
    return ffi_index_meta(L, cts, id, MM_newindex);
  }
  lj_cdata_set_l(L, cts, ct, id, p, o+2, qual);
  return 0;
}

/* Common handler for cdata arithmetic. */
static int ffi_arith(lua_State *L)
{
  MMS mm = (MMS)(curr_func(L)->c.ffid - (int)FF_ffi_meta___eq + (int)MM_eq);
  return lj_carith_op(L, mm);
}

/* The following functions must be in contiguous ORDER MM. */
LJLIB_CF(ffi_meta___eq)		LJLIB_REC(cdata_arith MM_eq)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___len)	LJLIB_REC(cdata_arith MM_len)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___lt)		LJLIB_REC(cdata_arith MM_lt)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___le)		LJLIB_REC(cdata_arith MM_le)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___concat)	LJLIB_REC(cdata_arith MM_concat)
{
  return ffi_arith(L);
}

/* Forward declaration. */
static int lj_cf_ffi_new(lua_State *L);

LJLIB_CF(ffi_meta___call)	LJLIB_REC(cdata_call)
{
  CTState *cts = ctype_cts(L);
  GCcdata *cd = ffi_checkcdata(L, 1);
  CTypeID id = cd->ctypeid;
  TValue metatv;
  cTValue *tv;
  MMS mm = MM_call;
  if (cd->ctypeid == CTID_CTYPEID) {
    id = *(CTypeID *)cdataptr(cd);
    mm = MM_new;
  } else {
    int ret = lj_ccall_func(L, cd);
    if (ret >= 0)
      return ret;
  }
  /* Handle ctype __call/__new metamethod. */
  {
    CType snap;
    CTypeID rid;
    CTInfo info;
    CTSize size;
    int ok = ffi_ctype_info_read(L, cts, id, &info, &size, &rid, &snap);
    if (ok <= 0)
      lj_err_callerv(L, LJ_ERR_FFI_BADCALL,
		     strdata(lj_ctype_repr(L, id, NULL)));
    info = ctype_info_acq(&snap);
    if (ctype_isptr(info)) id = ctype_cid(info);
  }
  tv = ffi_ctype_metatv_read(L, cts, &metatv, id, mm);
  if (tv)
    return lj_meta_tailcall(L, tv);
  else if (mm == MM_call)
    lj_err_callerv(L, LJ_ERR_FFI_BADCALL, strdata(lj_ctype_repr(L, id, NULL)));
  return lj_cf_ffi_new(L);
}

LJLIB_CF(ffi_meta___add)	LJLIB_REC(cdata_arith MM_add)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___sub)	LJLIB_REC(cdata_arith MM_sub)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___mul)	LJLIB_REC(cdata_arith MM_mul)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___div)	LJLIB_REC(cdata_arith MM_div)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___mod)	LJLIB_REC(cdata_arith MM_mod)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___pow)	LJLIB_REC(cdata_arith MM_pow)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___unm)	LJLIB_REC(cdata_arith MM_unm)
{
  return ffi_arith(L);
}
/* End of contiguous ORDER MM. */

LJLIB_CF(ffi_meta___tostring)
{
  GCcdata *cd = ffi_checkcdata(L, 1);
  const char *msg = "cdata<%s>: %p";
  CTypeID id = cd->ctypeid;
  void *p = cdataptr(cd);
  if (id == CTID_CTYPEID) {
    msg = "ctype<%s>";
    id = *(CTypeID *)p;
  } else {
    CTState *cts = ctype_cts(L);
    CTypeID rid;
    CType snap;
    CTInfo info;
    CTSize size;
    int ok = ffi_ctype_info_read(L, cts, id, &info, &size, &rid, &snap);
    if (ok <= 0)
      lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
    info = ctype_info_acq(&snap);
    size = ctype_size_acq(&snap);
    if (ctype_isref(info)) {
      CTypeID refid = ctype_cid(info);
      p = *(void **)p;
      ok = ffi_ctype_info_read(L, cts, refid, &info, &size, &rid, &snap);
      if (ok <= 0)
	lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
      info = ctype_info_acq(&snap);
      size = ctype_size_acq(&snap);
    }
    if (ctype_iscomplex(info)) {
      setstrV(L, L->top-1, lj_ctype_repr_complex(L, cdataptr(cd), size));
      goto checkgc;
    } else if (size == 8 && ctype_isinteger(info)) {
      setstrV(L, L->top-1, lj_ctype_repr_int64(L, *(uint64_t *)cdataptr(cd),
					       (info & CTF_UNSIGNED)));
      goto checkgc;
    } else if (ctype_isfunc(info)) {
      p = *(void **)p;
    } else if (ctype_isenum(info)) {
      msg = "cdata<%s>: %d";
      p = (void *)(uintptr_t)*(uint32_t *)p;
    } else {
      if (ctype_isptr(info)) {
	CTypeID childid = ctype_cid(info);
	p = cdata_getptr(p, size);
	ok = ffi_ctype_info_read(L, cts, childid, &info, &size, &rid,
				 &snap);
	if (ok <= 0)
	  lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
	info = ctype_info_acq(&snap);
      }
      if (ctype_isstruct(info) || ctype_isvector(info)) {
	/* Handle ctype __tostring metamethod. */
	TValue metatv;
	cTValue *tv = ffi_ctype_metatv_read(L, cts, &metatv, rid,
					    MM_tostring);
	if (tv)
	  return lj_meta_tailcall(L, tv);
      }
    }
  }
  lj_strfmt_pushf(L, msg, strdata(lj_ctype_repr(L, id, NULL)), p);
checkgc:
  lj_gc_check(L);
  return 1;
}

static int ffi_pairs(lua_State *L, MMS mm)
{
  CTState *cts = ctype_cts(L);
  CTypeID id = ffi_checkcdata(L, 1)->ctypeid;
  TValue metatv;
  cTValue *tv;
  {
    CType snap;
    CTypeID rid;
    CTInfo info;
    CTSize size;
    int ok = ffi_ctype_info_read(L, cts, id, &info, &size, &rid, &snap);
    if (ok <= 0)
      lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
    info = ctype_info_acq(&snap);
    if (ctype_isptr(info)) id = ctype_cid(info);
  }
  tv = ffi_ctype_metatv_read(L, cts, &metatv, id, mm);
  if (!tv)
    lj_err_callerv(L, LJ_ERR_FFI_BADMM, strdata(lj_ctype_repr(L, id, NULL)),
		   strdata(mmname_str(G(L), mm)));
  return lj_meta_tailcall(L, tv);
}

LJLIB_CF(ffi_meta___pairs)
{
  return ffi_pairs(L, MM_pairs);
}

LJLIB_CF(ffi_meta___ipairs)
{
  return ffi_pairs(L, MM_ipairs);
}

LJLIB_PUSH("ffi") LJLIB_SET(__metatable)

#include "lj_libdef.h"

/* -- C library metamethods ----------------------------------------------- */

#define LJLIB_MODULE_ffi_clib

/* Index C library by a name. */
static TValue *ffi_clib_index(lua_State *L)
{
  TValue *o = L->base;
  CLibrary *cl;
  if (!(o < L->top && tvisudata(o) &&
	lj_udata_udtype_acq(udataV(o)) == UDTYPE_FFI_CLIB))
    lj_err_argt(L, 1, LUA_TUSERDATA);
  cl = (CLibrary *)uddata(udataV(o));
  if (!(o+1 < L->top && tvisstr(o+1)))
    lj_err_argt(L, 2, LUA_TSTRING);
  return lj_clib_index(L, cl, strV(o+1));
}

LJLIB_CF(ffi_clib___index)	LJLIB_REC(clib_index 1)
{
  TValue tv;
  lj_tv_load_acq(&tv, ffi_clib_index(L));
  if (tviscdata(&tv)) {
    CTState *cts = ctype_cts(L);
    GCcdata *cd = cdataV(&tv);
    CType esnap;
    CTInfo sinfo, erawinfo;
    CTSize ssize;
    CTypeID rid;
    int ok = ffi_ctype_info_read(L, cts, cd->ctypeid, &sinfo, &ssize, &rid,
				 &esnap);
    if (ok <= 0)
      lj_err_arg(L, 2, LJ_ERR_FFI_INVTYPE);
    erawinfo = ctype_info_acq(&esnap);
    if (ctype_isextern(erawinfo)) {
      CType tsnap;
      CTypeID sid = ctype_cid(erawinfo);
      void *sp = *(void **)cdataptr(cd);
      ok = ffi_ctype_info_read(L, cts, sid, &sinfo, &ssize, &rid, &tsnap);
      if (ok <= 0)
	lj_err_arg(L, 2, LJ_ERR_FFI_INVTYPE);
      if (lj_cconv_tv_ct_l(L, cts, &tsnap, rid, L->top-1, sp))
	lj_gc_check(L);
      return 1;
    }
  }
  copyTV(L, L->top-1, &tv);
  return 1;
}

LJLIB_CF(ffi_clib___newindex)	LJLIB_REC(clib_index 0)
{
  TValue tv;
  TValue *o = L->base+2;
  lj_tv_load_acq(&tv, ffi_clib_index(L));
  if (o < L->top && tviscdata(&tv)) {
    CTState *cts = ctype_cts(L);
    GCcdata *cd = cdataV(&tv);
    CTypeID did = cd->ctypeid;
    CType esnap;
    CTInfo dinfo, erawinfo;
    CTSize dsize;
    CTypeID rid;
    int ok = ffi_ctype_info_read(L, cts, did, &dinfo, &dsize, &rid, &esnap);
    if (ok <= 0)
      lj_err_arg(L, 2, LJ_ERR_FFI_INVTYPE);
    erawinfo = ctype_info_acq(&esnap);
    if (ctype_isextern(erawinfo)) {
      CType dsnap;
      did = ctype_cid(erawinfo);
      ok = ffi_ctype_info_read(L, cts, did, &dinfo, &dsize, &rid, &dsnap);
      if (ok <= 0)
	lj_err_arg(L, 2, LJ_ERR_FFI_INVTYPE);
      if (!(dinfo & CTF_CONST)) {
	lj_cconv_ct_tv_l(L, cts, &dsnap, rid, *(void **)cdataptr(cd), o, 0);
	return 0;
      }
    }
  }
  lj_err_caller(L, LJ_ERR_FFI_WRCONST);
  return 0;  /* unreachable */
}

LJLIB_CF(ffi_clib___gc)
{
  TValue *o = L->base;
  if (o < L->top && tvisudata(o) &&
      lj_udata_udtype_acq(udataV(o)) == UDTYPE_FFI_CLIB)
    lj_clib_unload(L, G(L), (CLibrary *)uddata(udataV(o)));
  return 0;
}

#include "lj_libdef.h"

/* -- Callback function metamethods --------------------------------------- */

#define LJLIB_MODULE_ffi_callback

static LJ_AINLINE void *ffi_callback_ptr_acq(GCcdata *cd)
{
  return la_loadptr_acq((void *const *)cdataptr(cd));
}

static LJ_AINLINE int ffi_callback_isfree_acq(GCcdata *cd)
{
  return (cdata_flags_acq(cd) & LJ_CDATA_CALLBACK_FREE) != 0;
}

static LJ_AINLINE void ffi_callback_markfree(GCcdata *cd)
{
  cdata_flags_or_atomic(cd, LJ_CDATA_CALLBACK_FREE);
}

static int ffi_callback_set(lua_State *L, GCfunc *fn)
{
  GCcdata *cd = ffi_checkcdata(L, 1);
  CTState *cts = ctype_cts(L);
  CTInfo info;
  CTSize size;
  int ok;
  if (ffi_callback_isfree_acq(cd))
    goto bad_callback;
  ok = ffi_ctype_info_read(L, cts, cd->ctypeid, &info, &size, NULL, NULL);
  if (ok <= 0)
    goto bad_callback;
  if (ctype_isptr(info) && (LJ_32 || size == 8)) {
    MSize slot = lj_ccallback_ptr2slot(cts, ffi_callback_ptr_acq(cd));
    CTypeID1 *cbid = NULL;
    lua_State **owner = NULL;
    if (slot < ctype_cb_sizeid_acq(cts) &&
	(cbid = ctype_cb_cbid_acq(cts)) != NULL &&
	ctype_cb_cbid_slot_acq(cbid, slot) != 0) {
      if (fn) {
	lj_ccallback_func_store_l(L, cts, slot, fn);
      } else {
	ffi_callback_markfree(cd);  /* Tombstone cdata; keep C trampoline ptr. */
	owner = ctype_cb_owner_acq(cts);
	if (owner && ctype_cb_owner_slot_acq(owner, slot) == NULL) {
	  /* 11.5 disowned callback free: nil function before cbid release. */
	  lj_ccallback_func_clear(cts, slot);
	  ctype_cb_cbid_slot_rel(cbid, slot, 0);
	} else {
	  /* 11.5 owned callback free: cbid release before owner release. */
	  ctype_cb_cbid_slot_rel(cbid, slot, 0);
	  lj_ccallback_func_clear(cts, slot);
	  if (owner)
	    ctype_cb_owner_slot_rel(owner, slot, NULL);  /* 11.5 slot reusable. */
	}
      }
      return 0;
    }
  }
bad_callback:
  lj_err_caller(L, LJ_ERR_FFI_BADCBACK);
  return 0;
}

LJLIB_CF(ffi_callback_free)
{
  return ffi_callback_set(L, NULL);
}

LJLIB_CF(ffi_callback_set)
{
  GCfunc *fn = lj_lib_checkfunc(L, 2);
  return ffi_callback_set(L, fn);
}

LJLIB_PUSH(top-1) LJLIB_SET(__index)

#include "lj_libdef.h"

/* -- ffi.pin() handle methods ------------------------------------------- */

#define LJLIB_MODULE_ffi_pin

static GCudata *ffi_pin_check(lua_State *L)
{
  TValue *o = L->base;
  if (!(o < L->top && tvisudata(o) &&
	lj_udata_udtype_acq(udataV(o)) == UDTYPE_FFI_PIN))
    lj_err_argtype(L, 1, "ffi.pin");
  return udataV(o);
}

static void ffi_pin_release_l(lua_State *L, GCudata *ud)
{
  TValue nilv;
  setnilV(&nilv);
  copyTVrel(L, (TValue *)uddata(ud), &nilv);
}

LJLIB_CF(ffi_pin_release)
{
  ffi_pin_release_l(L, ffi_pin_check(L));
  return 0;
}

LJLIB_CF(ffi_pin___gc)
{
  TValue *o = L->base;
  if (o < L->top && tvisudata(o) &&
      lj_udata_udtype_acq(udataV(o)) == UDTYPE_FFI_PIN)
    ffi_pin_release_l(L, udataV(o));
  return 0;
}

LJLIB_CF(ffi_pin___tostring)
{
  (void)ffi_pin_check(L);
  lua_pushliteral(L, "ffi.pin");
  return 1;
}

LJLIB_PUSH("ffi.pin") LJLIB_SET(__metatable)
LJLIB_PUSH(top-1) LJLIB_SET(__index)

#include "lj_libdef.h"

/* -- FFI library functions ----------------------------------------------- */

#define LJLIB_MODULE_ffi

LJLIB_CF(ffi_cdef)
{
  GCstr *s = lj_lib_checkstr(L, 1);
  CPState cp;
  int errcode;
  cp.L = L;
  cp.cts = ctype_cts(L);
  cp.srcname = strdata(s);
  cp.p = strdata(s);
  cp.param = L->base+1;
  cp.mode = CPARSE_MODE_MULTI|CPARSE_MODE_DIRECT;
  lj_ctype_parse_lock(cp.cts, L);
  errcode = lj_cparse(&cp);
  lj_ctype_parse_unlock(cp.cts);
  if (errcode) lj_err_throw(L, errcode);  /* Propagate errors. */
  lj_gc_check(L);
  return 0;
}

LJLIB_CF(ffi_new)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  CTypeID id, rid;
  CType *ct;
  CTSize sz = CTSIZE_INVALID;
  CTInfo info = 0;
  MSize ofs = 1;
  TValue *o;
  GCcdata *cd;
  int isstr, neednelem = 0;
  id = ffi_checkctype_noparse(L, NULL, &isstr);
  if (isstr)
    id = ffi_checkctype(L, cts, NULL);
  {
    int ok = ffi_new_layout_snapshot(cts, id, 0, 0, &rid, &info, &sz,
				    &neednelem);
    if (ok < 0)
      ok = ffi_new_layout_wait(L, cts, id, 0, 0, &rid, &info, &sz,
			       &neednelem);
    if (ok > 0 && neednelem) {
      CTSize nelem = (CTSize)ffi_checkint(L, 2);
      ofs = 2;
      ok = ffi_new_layout_snapshot(cts, id, nelem, 1, &rid, &info, &sz,
				   &neednelem);
      if (ok < 0)
	ok = ffi_new_layout_wait(L, cts, id, nelem, 1, &rid, &info, &sz,
				 &neednelem);
    }
    if (ok > 0)
      goto got_layout;
    if (ok == 0) {
      sz = CTSIZE_INVALID;
      goto got_layout;  /* Invalid/abandoned ID: report as invalid size. */
    }
  }
got_layout:
  if (sz == CTSIZE_INVALID)
    lj_err_arg(L, 1, LJ_ERR_FFI_INVSIZE);
  o = L->base + ofs;
  cd = lj_cdata_newx_l(L, cts, id, sz, info);
  setcdataV(L, o-1, cd);  /* Anchor the uninitialized cdata. */
  ct = ctype_get(cts, rid);  /* Table may have been reallocated. */
  lj_cconv_ct_init_l(L, cts, ct, rid, sz, cdataptr(cd),
		     o, (MSize)(L->top - o));  /* Initialize cdata. */
  if (ctype_isstruct(ctype_info_acq(ct))) {
    /* Handle ctype __gc metamethod. Use the fast lookup here. */
    TValue gctv;
    cTValue *tv = ffi_ctype_metatv_read(L, cts, &gctv, id, MM_gc);
    if (tv)
      lj_cdata_setfin(L, cd, gcV(tv), itype(tv));
  }
  L->top = o;  /* Only return the cdata itself. */
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_cast)	LJLIB_REC(ffi_new)
{
  CTState *cts = ctype_cts(L);
  CTypeID id, rid;
  CType dsnap, *d;
  CTInfo info;
  CTSize sz;
  TValue *o = lj_lib_checkany(L, 2);
  ptrdiff_t ofs = o - L->base;
  int isstr;
  int ok;
  id = ffi_checkctype_noparse(L, NULL, &isstr);
  if (isstr)
    id = ffi_checkctype(L, cts, NULL);
  ok = ffi_ctype_info_read(L, cts, id, &info, &sz, &rid, &dsnap);
  if (ok <= 0)
    lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  d = &dsnap;
  L->top = o+1;  /* Make sure this is the last item on the stack. */
  lj_state_checkstack(L, 1);
  o = L->base + ofs;
  if (!(ctype_isnum(info) || ctype_isptr(info) || ctype_isenum(info)))
    lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  if (!(tviscdata(o) && cdataV(o)->ctypeid == id)) {
    GCcdata *cd = lj_cdata_new_l(L, cts, id, sz);
    setcdataV(L, L->top++, cd);  /* Anchor across callback allocation. */
    lj_cconv_ct_tv_l(L, cts, d, rid, cdataptr(cd), o, CCF_CAST);
    L->top = o+1;
    setcdataV(L, o, cd);
    lj_gc_check(L);
  }
  return 1;
}

LJLIB_CF(ffi_typeof)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  CTypeID id = ffi_checkctype(L, cts, L->base+1);
  GCcdata *cd = lj_cdata_new_(L, CTID_CTYPEID, 4);
  *(CTypeID *)cdataptr(cd) = id;
  setcdataV(L, L->top-1, cd);
  lj_gc_check(L);
  return 1;
}

static void ffi_typeinfo_storeint(lua_State *L, GCtab *tab, GCstr *key,
				  int32_t val)
{
  TValue keytv, tv, *dst;
  setintV(&tv, val);
  setstrV(L, &keytv, key);
  for (;;) {
    dst = lj_tab_setstr(L, tab, key);
    if (lj_tab_trystoretv_cas_keyed(L, tab, dst, &keytv, &tv) ==
	LJ_TAB_STORE_CAS_OK)
      return;
    lj_tab_store_wait_no_l();  /* FFI typeinfo int store saw stale/FORWARD slot. */
  }
}

static void ffi_typeinfo_storestr(lua_State *L, GCtab *tab, GCstr *key,
				  GCstr *val)
{
  TValue keytv, tv, *dst;
  setstrV(L, &tv, val);
  setstrV(L, &keytv, key);
  for (;;) {
    dst = lj_tab_setstr(L, tab, key);
    if (lj_tab_trystoretv_cas_keyed(L, tab, dst, &keytv, &tv) ==
	LJ_TAB_STORE_CAS_OK)
      return;
    lj_tab_store_wait_no_l();  /* FFI typeinfo string store saw stale/FORWARD slot. */
  }
}

/* Internal and unsupported API. */
LJLIB_CF(ffi_typeinfo)
{
  CTState *cts = ctype_cts(L);
  CTypeID id = (CTypeID)ffi_checkint(L, 1);
  CType snap;
  CTInfo info;
  CTSize size;
  CTypeID sib;
  GCstr *name;
  int ok = ffi_ctype_predefined_snapshot(cts, id, &snap);
  if (!ok)
    ok = lj_ctype_snapshot(cts, id, &snap);
  if (ok > 0) {
    GCtab *t;
    info = snap.info;
    size = snap.size;
    sib = snap.sib;
    name = ctype_name_acq(&snap);
    lua_createtable(L, 0, 4);  /* Increment hash size if fields are added. */
    t = tabV(L->top-1);
    ffi_typeinfo_storeint(L, t, lj_str_newlit(L, "info"), (int32_t)info);
    if (size != CTSIZE_INVALID)
      ffi_typeinfo_storeint(L, t, lj_str_newlit(L, "size"), (int32_t)size);
    if (sib)
      ffi_typeinfo_storeint(L, t, lj_str_newlit(L, "sib"), (int32_t)sib);
    if (name) {
      if (isdead(G(L), obj2gco(name))) flipwhite(obj2gco(name));
      ffi_typeinfo_storestr(L, t, lj_str_newlit(L, "name"), name);
    }
    lj_gc_pubtab(L, t);
    lj_gc_check(L);
    return 1;
  }
  return 0;
}

typedef struct FFITypeCmpSnap {
  CTState *cts;
  CTypeTab *tabh;
  CTypeID top;
  uint32_t seq;
  MSize budget;
} FFITypeCmpSnap;

static int ffi_typecmp_begin(CTState *cts, FFITypeCmpSnap *ts)
{
  uint32_t seq = ctype_parse_token_acq(cts);
  if (seq & 1u)
    return -1;
  ts->cts = cts;
  ts->top = ctype_top_acq(cts);
  ts->tabh = ctype_tabh_acq(cts);
  ts->seq = seq;
  ts->budget = ts->top ? (MSize)ts->top * 6u : 1u;
  return 1;
}

static int ffi_typecmp_end(FFITypeCmpSnap *ts)
{
  uint32_t seq = ctype_parse_token_acq(ts->cts);
  return (seq == ts->seq && !(seq & 1u)) ? 1 : -1;
}

static int ffi_typecmp_get(FFITypeCmpSnap *ts, CTypeID id, CType *out)
{
  CType *ct;
  CTInfo info;
  if (id == 0 || id >= ts->top)
    return 0;
  if ((MSize)id >= ctype_tab_sizetab_acq(ts->tabh))
    return -1;
  if (ts->budget-- == 0)
    return -1;
  ct = ctype_tab_slot(ts->tabh, id);
  info = ctype_info_acq(ct);
  out->info = info;
  out->size = ctype_size_acq(ct);
  out->sib = (CTypeID1)ctype_sib_acq(ct);
  out->next = (CTypeID1)ctype_next_acq(ct);
  setgcrefnull(out->name);
  return !ctype_isabandoned(info);
}

static int ffi_typecmp_rawid(FFITypeCmpSnap *ts, CTypeID id, CTypeID *ridp,
			     CType *out)
{
  int ok;
  for (;;) {
    CTInfo info;
    ok = ffi_typecmp_get(ts, id, out);
    if (ok <= 0)
      return ok;
    info = ctype_info_acq(out);
    if (!ctype_isattrib(info)) {
      *ridp = id;
      return 1;
    }
    id = ctype_cid(info);
  }
}

static int ffi_typecmp_rawrefid(FFITypeCmpSnap *ts, CTypeID id, CTypeID *ridp,
				CType *out)
{
  int ok = ffi_typecmp_get(ts, id, out);
  if (ok <= 0)
    return ok;
  {
    CTInfo info = ctype_info_acq(out);
    if (ctype_isref(info))
      id = ctype_cid(info);
  }
  return ffi_typecmp_rawid(ts, id, ridp, out);
}

static int ffi_typecmp_childqual(FFITypeCmpSnap *ts, const CType *root,
				 CTypeID *idp, CType *out, CTInfo *qualp)
{
  CTypeID id = ctype_cid(ctype_info_acq(root));
  int ok;
  for (;;) {
    CTInfo info;
    ok = ffi_typecmp_get(ts, id, out);
    if (ok <= 0)
      return ok;
    info = ctype_info_acq(out);
    if (ctype_isattrib(info)) {
      if (ctype_attrib(info) == CTA_QUAL)
	*qualp |= ctype_size_acq(out);
    } else if (!ctype_isenum(info)) {
      *qualp |= (info & CTF_QUAL);
      *idp = id;
      return 1;
    }
    id = ctype_cid(info);
  }
}

static int ffi_typecmp_compatptr(FFITypeCmpSnap *ts, CTypeID did,
				 const CType *d0, CTypeID sid,
				 const CType *s0, CTInfo flags, int *bp)
{
  CType d = *d0, s = *s0;
  if (!((flags & CCF_CAST) || did == sid)) {
    CTInfo dinfo, sinfo;
    CTSize dsize, ssize;
    CTInfo dqual = 0, squal = 0;
    int ok = ffi_typecmp_childqual(ts, &d, &did, &d, &dqual);
    if (ok <= 0)
      return ok;
    sinfo = ctype_info_acq(&s);
    if (!ctype_isstruct(sinfo)) {
      ok = ffi_typecmp_childqual(ts, &s, &sid, &s, &squal);
      if (ok <= 0)
	return ok;
    }
    dinfo = ctype_info_acq(&d);
    sinfo = ctype_info_acq(&s);
    dsize = ctype_size_acq(&d);
    ssize = ctype_size_acq(&s);
    if ((flags & CCF_SAME)) {
      if (dqual != squal) {
	*bp = 0;
	return 1;
      }
    } else if (!(flags & CCF_IGNQUAL)) {
      if ((dqual & squal) != squal) {
	*bp = 0;
	return 1;
      }
      if (ctype_isvoid(dinfo) || ctype_isvoid(sinfo)) {
	*bp = 1;
	return 1;
      }
    }
    if (ctype_type(dinfo) != ctype_type(sinfo) || dsize != ssize) {
      *bp = 0;
      return 1;
    }
    if (ctype_isnum(dinfo)) {
      *bp = ((dinfo ^ sinfo) & (CTF_BOOL|CTF_FP)) == 0;
      return 1;
    } else if (ctype_ispointer(dinfo)) {
      return ffi_typecmp_compatptr(ts, did, &d, sid, &s,
				   flags|CCF_SAME, bp);
    } else if (ctype_isstruct(dinfo)) {
      *bp = did == sid;
      return 1;
    }
  }
  *bp = 1;
  return 1;
}

static int ffi_istype_compare(FFITypeCmpSnap *ts, CTypeID id1, CTypeID id2,
			      int *bp)
{
  CType ct1, ct2, child;
  CTypeID rid1, rid2, cid;
  int ok;
  int b = 0;
  ok = ffi_typecmp_rawrefid(ts, id1, &rid1, &ct1);
  if (ok > 0)
    ok = ffi_typecmp_rawrefid(ts, id2, &rid2, &ct2);
  if (ok > 0) {
    CTInfo info1 = ctype_info_acq(&ct1);
    CTInfo info2 = ctype_info_acq(&ct2);
    CTSize size1 = ctype_size_acq(&ct1);
    CTSize size2 = ctype_size_acq(&ct2);
    if (rid1 == rid2) {
      b = 1;
    } else if (ctype_type(info1) == ctype_type(info2) &&
	       size1 == size2) {
      if (ctype_ispointer(info1)) {
	ok = ffi_typecmp_compatptr(ts, rid1, &ct1, rid2, &ct2,
				   CCF_IGNQUAL, &b);
      } else if (ctype_isnum(info1) || ctype_isvoid(info1)) {
	b = ((info1 ^ info2) & ~(CTF_QUAL|CTF_LONG)) == 0;
      }
    } else if (ctype_isstruct(info1) && ctype_isptr(info2)) {
      ok = ffi_typecmp_rawid(ts, ctype_cid(info2), &cid, &child);
      if (ok > 0 && rid1 == cid)
	b = 1;
    }
  } else if (ok == 0) {
    b = 0;
  }
  if (ok < 0)
    return -1;
  *bp = b;
  return 1;
}

static int ffi_istype_snapshot(CTState *cts, CTypeID id1, CTypeID id2, int *bp)
{
  FFITypeCmpSnap ts;
  int ok = ffi_typecmp_begin(cts, &ts);
  if (ok < 0)
    return -1;
  ok = ffi_istype_compare(&ts, id1, id2, bp);
  if (ok >= 0 && ffi_typecmp_end(&ts) < 0)
    return -1;
  return ok;
}

static int ffi_istype_predefined(CTState *cts, CTypeID id1, CTypeID id2,
				 int *bp)
{
  FFITypeCmpSnap ts;
  if (!(id1 > CTID_NONE && id1 <= CTID_CTYPEID &&
	id2 > CTID_NONE && id2 <= CTID_CTYPEID))
    return 0;
  ts.cts = cts;
  ts.top = CTID_CTYPEID + 1;
  ts.tabh = ctype_tabh_acq(cts);
  ts.seq = 0;
  ts.budget = (MSize)ts.top * 6u;
  if ((MSize)CTID_CTYPEID >= ctype_tab_sizetab_acq(ts.tabh))
    return 0;
  return ffi_istype_compare(&ts, id1, id2, bp) > 0;
}

static void ffi_istype_snapshot_wait(lua_State *L, CTState *cts,
				     CTypeID id1, CTypeID id2, int *bp)
{
  if (ffi_istype_predefined(cts, id1, id2, bp))
    return;
  for (;;) {
    int ok = ffi_istype_snapshot(cts, id1, id2, bp);
    if (ok >= 0)
      return;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

LJLIB_CF(ffi_istype)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  TValue *o = lj_lib_checkany(L, 2);
  CTypeID id1;
  int b = 0;
  int isstr;
  id1 = ffi_checkctype_noparse(L, NULL, &isstr);
  if (tviscdata(o)) {
    GCcdata *cd = cdataV(o);
    CTypeID id2 = cd->ctypeid == CTID_CTYPEID ? *(CTypeID *)cdataptr(cd) :
						cd->ctypeid;
    if (!isstr) {
      ffi_istype_snapshot_wait(L, cts, id1, id2, &b);
      goto done;
    }
    id1 = ffi_checkctype(L, cts, NULL);
    ffi_istype_snapshot_wait(L, cts, id1, id2, &b);
  } else if (isstr) {
    id1 = ffi_checkctype(L, cts, NULL);
  }
done:
  setboolV(L->top-1, b);
  setboolV(&L2TG(L)->tmptv2, b);  /* Remember for trace recorder. */
  return 1;
}

typedef struct FFILayoutSnap {
  CTState *cts;
  CTypeTab *tabh;
  CTypeID top;
  uint32_t seq;
  MSize budget;
} FFILayoutSnap;

static int ffi_layout_begin(CTState *cts, FFILayoutSnap *ls)
{
  uint32_t seq = ctype_parse_token_acq(cts);
  if (seq & 1u)
    return -1;
  ls->cts = cts;
  ls->top = ctype_top_acq(cts);
  ls->tabh = ctype_tabh_acq(cts);
  ls->seq = seq;
  ls->budget = ls->top ? (MSize)ls->top * 2u : 1u;
  return 1;
}

static int ffi_layout_begin_predefined(CTState *cts, CTypeID id,
				       FFILayoutSnap *ls)
{
  if (!ffi_ctype_predefined_id(id))
    return 0;
  ls->cts = cts;
  ls->top = CTID_CTYPEID + 1;
  ls->tabh = ctype_tabh_acq(cts);
  ls->seq = 0;
  ls->budget = (MSize)ls->top * 2u;
  if ((MSize)CTID_CTYPEID >= ctype_tab_sizetab_acq(ls->tabh))
    return 0;
  return 1;
}

static int ffi_layout_end(FFILayoutSnap *ls)
{
  uint32_t seq = ctype_parse_token_acq(ls->cts);
  return (seq == ls->seq && !(seq & 1u)) ? 1 : -1;
}

static int ffi_layout_get(FFILayoutSnap *ls, CTypeID id, CType *out)
{
  CType *ct;
  GCobj *name;
  if (id == 0 || id >= ls->top ||
      (MSize)id >= ctype_tab_sizetab_acq(ls->tabh))
    return 0;
  if (ls->budget-- == 0)
    return -1;
  ct = ctype_tab_slot(ls->tabh, id);
  out->info = ctype_info_acq(ct);
  out->size = ctype_size_acq(ct);
  out->sib = (CTypeID1)ctype_sib_acq(ct);
  out->next = (CTypeID1)ctype_next_acq(ct);
  name = ctype_nameobj_acq(ct);
  setgcrefp(out->name, name);
  return ctype_isabandoned(out->info) ? 0 : 1;
}

static int ffi_layout_rawref(FFILayoutSnap *ls, CTypeID id, CType *out)
{
  int ok;
  for (;;) {
    CTInfo info;
    ok = ffi_layout_get(ls, id, out);
    if (ok <= 0)
      return ok;
    info = ctype_info_acq(out);
    if (!(ctype_isattrib(info) || ctype_isref(info)))
      return 1;
    id = ctype_cid(info);
  }
}

static int ffi_layout_rawid(FFILayoutSnap *ls, CTypeID id, CTypeID *ridp,
			    CType *out)
{
  int ok;
  for (;;) {
    CTInfo info;
    ok = ffi_layout_get(ls, id, out);
    if (ok <= 0)
      return ok;
    info = ctype_info_acq(out);
    if (!ctype_isattrib(info)) {
      *ridp = id;
      return 1;
    }
    id = ctype_cid(info);
  }
}

static int ffi_layout_raw(FFILayoutSnap *ls, CTypeID id, CType *out)
{
  int ok;
  for (;;) {
    CTInfo info;
    ok = ffi_layout_get(ls, id, out);
    if (ok <= 0)
      return ok;
    info = ctype_info_acq(out);
    if (!ctype_isattrib(info))
      return 1;
    id = ctype_cid(info);
  }
}

static int ffi_layout_rawchild(FFILayoutSnap *ls, const CType *ct, CType *out)
{
  CTypeID id = ctype_cid(ctype_info_acq(ct));
  int ok;
  do {
    CTInfo info;
    ok = ffi_layout_get(ls, id, out);
    if (ok <= 0)
      return ok;
    info = ctype_info_acq(out);
    if (!ctype_isattrib(info))
      return 1;
    id = ctype_cid(info);
  } while (1);
}

static int ffi_layout_info(FFILayoutSnap *ls, CTypeID id,
			   CTInfo *infop, CTSize *szp)
{
  CTInfo qual = 0;
  CType ct;
  int ok = ffi_layout_get(ls, id, &ct);
  if (ok <= 0)
    return ok;
  for (;;) {
    CTInfo info = ctype_info_acq(&ct);
    CTSize size = ctype_size_acq(&ct);
    if (ctype_isenum(info)) {
      /* Follow child. Need to look at its attributes, too. */
    } else if (ctype_isattrib(info)) {
      if (ctype_isxattrib(info, CTA_QUAL))
	qual |= size;
      else if (ctype_isxattrib(info, CTA_ALIGN) && !(qual & CTFP_ALIGNED))
	qual |= CTFP_ALIGNED + CTALIGN(size);
    } else {
      if (!(qual & CTFP_ALIGNED)) qual |= (info & CTF_ALIGN);
      qual |= (info & ~(CTF_ALIGN|CTMASK_CID));
      *infop = qual;
      *szp = ctype_isfunc(info) ? CTSIZE_INVALID : size;
      return 1;
    }
    ok = ffi_layout_get(ls, ctype_cid(info), &ct);
    if (ok <= 0)
      return ok;
  }
}

static int ffi_layout_info_raw(FFILayoutSnap *ls, CTypeID id,
			       CTInfo *infop, CTSize *szp)
{
  CTInfo qual = 0;
  CType ct;
  int ok = ffi_layout_get(ls, id, &ct);
  if (ok <= 0)
    return ok;
  {
    CTInfo info = ctype_info_acq(&ct);
    if (ctype_isref(info)) {
      id = ctype_cid(info);
      ok = ffi_layout_get(ls, id, &ct);
      if (ok <= 0)
	return ok;
    }
  }
  for (;;) {
    CTInfo info = ctype_info_acq(&ct);
    CTSize size = ctype_size_acq(&ct);
    if (ctype_isenum(info)) {
      /* Follow child. Need to look at its attributes, too. */
    } else if (ctype_isattrib(info)) {
      if (ctype_isxattrib(info, CTA_QUAL))
	qual |= size;
      else if (ctype_isxattrib(info, CTA_ALIGN) && !(qual & CTFP_ALIGNED))
	qual |= CTFP_ALIGNED + CTALIGN(size);
    } else {
      if (!(qual & CTFP_ALIGNED)) qual |= (info & CTF_ALIGN);
      qual |= (info & ~(CTF_ALIGN|CTMASK_CID));
      *infop = qual;
      *szp = ctype_isfunc(info) ? CTSIZE_INVALID : size;
      return 1;
    }
    ok = ffi_layout_get(ls, ctype_cid(info), &ct);
    if (ok <= 0)
      return ok;
  }
}

static int ffi_layout_vlsize(FFILayoutSnap *ls, const CType *ct,
			     CTSize nelem, CTSize *szp)
{
  CType cur = *ct, elem;
  CTInfo info = ctype_info_acq(&cur);
  CTSize size = ctype_size_acq(&cur);
  uint64_t xsz = 0;
  int ok;
  if (ctype_isstruct(info)) {
    CTypeID arrid = 0, fid = ctype_sib_acq(&cur);
    xsz = size;
    while (fid) {
      ok = ffi_layout_get(ls, fid, &cur);
      if (ok <= 0)
	return ok;
      info = ctype_info_acq(&cur);
      if (ctype_type(info) == CT_FIELD)
	arrid = ctype_cid(info);
      fid = ctype_sib_acq(&cur);
    }
    if (arrid == 0)
      return 0;
    ok = ffi_layout_raw(ls, arrid, &cur);
    if (ok <= 0)
      return ok;
    info = ctype_info_acq(&cur);
  }
  if (!ctype_isvlarray(info))
    return 0;
  ok = ffi_layout_rawchild(ls, &cur, &elem);
  if (ok <= 0)
    return ok;
  info = ctype_info_acq(&elem);
  size = ctype_size_acq(&elem);
  if (!ctype_hassize(info))
    return 0;
  xsz += (uint64_t)size * nelem;
  *szp = xsz < 0x80000000u ? (CTSize)xsz : CTSIZE_INVALID;
  return 1;
}

static int ffi_new_layout_read(FFILayoutSnap *ls, CTypeID id, CTSize nelem,
			       int hasnelem, CTypeID *ridp, CTInfo *infop,
			       CTSize *szp, int *neednelem)
{
  CType raw;
  int ok = ffi_layout_rawid(ls, id, ridp, &raw);
  if (ok > 0) {
    ok = ffi_layout_info(ls, id, infop, szp);
    if (ok > 0) {
      if ((*infop & CTF_VLA)) {
	if (!hasnelem) {
	  *neednelem = 1;
	} else {
	  *neednelem = 0;
	  ok = ffi_layout_vlsize(ls, &raw, nelem, szp);
	}
      } else {
	*neednelem = 0;
      }
    }
  }
  return ok;
}

static int ffi_new_layout_snapshot(CTState *cts, CTypeID id, CTSize nelem,
				   int hasnelem, CTypeID *ridp,
				   CTInfo *infop, CTSize *szp,
				   int *neednelem)
{
  FFILayoutSnap ls;
  int ok = ffi_layout_begin(cts, &ls);
  if (ok < 0)
    return -1;
  ok = ffi_new_layout_read(&ls, id, nelem, hasnelem, ridp, infop, szp,
			   neednelem);
  if (ok >= 0 && ffi_layout_end(&ls) < 0)
    return -1;
  return ok;
}

static int ffi_new_layout_predefined(CTState *cts, CTypeID id, CTSize nelem,
				     int hasnelem, CTypeID *ridp,
				     CTInfo *infop, CTSize *szp,
				     int *neednelem)
{
  FFILayoutSnap ls;
  if (!ffi_layout_begin_predefined(cts, id, &ls))
    return -1;
  return ffi_new_layout_read(&ls, id, nelem, hasnelem, ridp, infop, szp,
			     neednelem);
}

static int ffi_new_layout_wait(lua_State *L, CTState *cts, CTypeID id,
			       CTSize nelem, int hasnelem, CTypeID *ridp,
			       CTInfo *infop, CTSize *szp, int *neednelem)
{
  int ok = ffi_new_layout_predefined(cts, id, nelem, hasnelem, ridp,
				     infop, szp, neednelem);
  if (ok >= 0)
    return ok;
  for (;;) {
    ok = ffi_new_layout_snapshot(cts, id, nelem, hasnelem, ridp,
				 infop, szp, neednelem);
    if (ok >= 0)
      return ok;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

static int ffi_layout_sizeof_read(FFILayoutSnap *ls, CTypeID id, CTSize nelem,
				  int hasnelem, CTSize *szp, int *neednelem)
{
  CType ct;
  int ok = ffi_layout_rawref(ls, id, &ct);
  if (ok > 0) {
    CTInfo info = ctype_info_acq(&ct);
    CTSize size = ctype_size_acq(&ct);
    if (ctype_isvltype(info)) {
      if (!hasnelem) {
	*neednelem = 1;
      } else {
	ok = ffi_layout_vlsize(ls, &ct, nelem, szp);
      }
    } else {
      *neednelem = 0;
      *szp = ctype_hassize(info) ? size : CTSIZE_INVALID;
    }
  }
  return ok;
}

static int ffi_layout_sizeof_snapshot(CTState *cts, CTypeID id, CTSize nelem,
				      int hasnelem, CTSize *szp, int *neednelem)
{
  FFILayoutSnap ls;
  int ok = ffi_layout_begin(cts, &ls);
  if (ok < 0)
    return -1;
  ok = ffi_layout_sizeof_read(&ls, id, nelem, hasnelem, szp, neednelem);
  if (ok >= 0 && ffi_layout_end(&ls) < 0)
    return -1;
  return ok;
}

static int ffi_layout_sizeof_predefined(CTState *cts, CTypeID id,
					CTSize nelem, int hasnelem,
					CTSize *szp, int *neednelem)
{
  FFILayoutSnap ls;
  if (!ffi_layout_begin_predefined(cts, id, &ls))
    return -1;
  return ffi_layout_sizeof_read(&ls, id, nelem, hasnelem, szp, neednelem);
}

static int ffi_layout_sizeof_wait(lua_State *L, CTState *cts, CTypeID id,
				  CTSize nelem, int hasnelem, CTSize *szp,
				  int *neednelem)
{
  int ok = ffi_layout_sizeof_predefined(cts, id, nelem, hasnelem, szp,
					neednelem);
  if (ok >= 0)
    return ok;
  for (;;) {
    ok = ffi_layout_sizeof_snapshot(cts, id, nelem, hasnelem, szp,
				    neednelem);
    if (ok >= 0)
      return ok;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

static int ffi_layout_alignof_read(FFILayoutSnap *ls, CTypeID id,
				   CTSize *alignp)
{
  CTInfo info;
  CTSize sz;
  int ok = ffi_layout_info_raw(ls, id, &info, &sz);
  if (ok > 0)
    *alignp = (CTSize)1u << ctype_align(info);
  return ok;
}

static int ffi_layout_alignof_snapshot(CTState *cts, CTypeID id, CTSize *alignp)
{
  FFILayoutSnap ls;
  int ok = ffi_layout_begin(cts, &ls);
  if (ok < 0)
    return -1;
  ok = ffi_layout_alignof_read(&ls, id, alignp);
  if (ok >= 0 && ffi_layout_end(&ls) < 0)
    return -1;
  return ok;
}

static int ffi_layout_alignof_predefined(CTState *cts, CTypeID id,
					 CTSize *alignp)
{
  FFILayoutSnap ls;
  if (!ffi_layout_begin_predefined(cts, id, &ls))
    return -1;
  return ffi_layout_alignof_read(&ls, id, alignp);
}

static int ffi_layout_alignof_wait(lua_State *L, CTState *cts, CTypeID id,
				   CTSize *alignp)
{
  int ok = ffi_layout_alignof_predefined(cts, id, alignp);
  if (ok >= 0)
    return ok;
  for (;;) {
    ok = ffi_layout_alignof_snapshot(cts, id, alignp);
    if (ok >= 0)
      return ok;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

static int ffi_layout_getfield(FFILayoutSnap *ls, const CType *root,
			       GCstr *name, CTSize *ofs, CType *out)
{
  CType ct = *root;
  CTypeID sid = ctype_sib_acq(&ct);
  while (sid) {
    int ok = ffi_layout_get(ls, sid, &ct);
    CTInfo info;
    CTSize size;
    CTypeID sib;
    if (ok <= 0)
      return ok;
    info = ctype_info_acq(&ct);
    size = ctype_size_acq(&ct);
    sib = ctype_sib_acq(&ct);
    if (ctype_name_acq(&ct) == name) {
      *ofs = size;
      *out = ct;
      return 1;
    }
    if (ctype_isxattrib(info, CTA_SUBTYPE)) {
      CType cct, fct;
      CTSize subofs;
      CTInfo cinfo;
      ok = ffi_layout_get(ls, ctype_cid(info), &cct);
      if (ok <= 0)
	return ok;
      cinfo = ctype_info_acq(&cct);
      while (ctype_isattrib(cinfo)) {
	ok = ffi_layout_get(ls, ctype_cid(cinfo), &cct);
	if (ok <= 0)
	  return ok;
	cinfo = ctype_info_acq(&cct);
      }
      ok = ffi_layout_getfield(ls, &cct, name, &subofs, &fct);
      if (ok != 0) {
	if (ok > 0) {
	  *ofs = subofs + size;
	  *out = fct;
	}
	return ok;
      }
    }
    sid = sib;
  }
  return 0;
}

static int ffi_layout_offsetof_read(FFILayoutSnap *ls, CTypeID id, GCstr *name,
				    CTSize *ofs, CType *out)
{
  CType ct;
  int ok = ffi_layout_rawref(ls, id, &ct);
  if (ok > 0) {
    CTInfo info = ctype_info_acq(&ct);
    CTSize size = ctype_size_acq(&ct);
    if (ctype_isstruct(info) && size != CTSIZE_INVALID)
      ok = ffi_layout_getfield(ls, &ct, name, ofs, out);
    else
      ok = 0;
  }
  return ok;
}

static int ffi_layout_offsetof_snapshot(CTState *cts, CTypeID id, GCstr *name,
					CTSize *ofs, CType *out)
{
  FFILayoutSnap ls;
  int ok = ffi_layout_begin(cts, &ls);
  if (ok < 0)
    return -1;
  ok = ffi_layout_offsetof_read(&ls, id, name, ofs, out);
  if (ok >= 0 && ffi_layout_end(&ls) < 0)
    return -1;
  return ok;
}

static int ffi_layout_offsetof_predefined(CTState *cts, CTypeID id,
					  GCstr *name, CTSize *ofs,
					  CType *out)
{
  FFILayoutSnap ls;
  if (!ffi_layout_begin_predefined(cts, id, &ls))
    return -1;
  return ffi_layout_offsetof_read(&ls, id, name, ofs, out);
}

static int ffi_layout_offsetof_wait(lua_State *L, CTState *cts, CTypeID id,
				    GCstr *name, CTSize *ofs, CType *out)
{
  int ok = ffi_layout_offsetof_predefined(cts, id, name, ofs, out);
  if (ok >= 0)
    return ok;
  for (;;) {
    ok = ffi_layout_offsetof_snapshot(cts, id, name, ofs, out);
    if (ok >= 0)
      return ok;
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

LJLIB_CF(ffi_sizeof)	LJLIB_REC(ffi_xof FF_ffi_sizeof)
{
  CTState *cts = ctype_cts(L);
  CTypeID id;
  CTSize sz;
  if (LJ_UNLIKELY(tviscdata(L->base) && cdataisv(cdataV(L->base)))) {
    sz = cdatavlen(cdataV(L->base));
  } else {
    int isstr, neednelem = 0;
    id = ffi_checkctype_noparse(L, NULL, &isstr);
    if (isstr)
      id = ffi_checkctype(L, cts, NULL);
    {
      int ok = ffi_layout_sizeof_snapshot(cts, id, 0, 0, &sz, &neednelem);
      if (ok < 0)
	ok = ffi_layout_sizeof_wait(L, cts, id, 0, 0, &sz, &neednelem);
      if (ok > 0 && neednelem) {
	CTSize nelem = (CTSize)ffi_checkint(L, 2);
	neednelem = 0;
	ok = ffi_layout_sizeof_snapshot(cts, id, nelem, 1, &sz, &neednelem);
	if (ok < 0)
	  ok = ffi_layout_sizeof_wait(L, cts, id, nelem, 1, &sz,
				      &neednelem);
      }
      if (ok > 0) {
	if (LJ_UNLIKELY(sz == CTSIZE_INVALID)) {
	  setnilV(L->top-1);
	  return 1;
	}
	goto got_size;
      }
      if (ok == 0) {
	setnilV(L->top-1);
	return 1;
      }
    }
  }
got_size:
  setintV(L->top-1, (int32_t)sz);
  return 1;
}

LJLIB_CF(ffi_alignof)	LJLIB_REC(ffi_xof FF_ffi_alignof)
{
  CTState *cts = ctype_cts(L);
  CTypeID id;
  CTSize align;
  int isstr;
  id = ffi_checkctype_noparse(L, NULL, &isstr);
  if (isstr)
    id = ffi_checkctype(L, cts, NULL);
  {
    int ok = ffi_layout_alignof_snapshot(cts, id, &align);
    if (ok < 0)
      ok = ffi_layout_alignof_wait(L, cts, id, &align);
    if (ok > 0) {
      setintV(L->top-1, (int32_t)align);
      return 1;
    }
    setnilV(L->top-1);
  }
  return 1;
}

LJLIB_CF(ffi_offsetof)	LJLIB_REC(ffi_xof FF_ffi_offsetof)
{
  CTState *cts = ctype_cts(L);
  CTypeID id;
  GCstr *name;
  CTSize ofs;
  int isstr;
  id = ffi_checkctype_noparse(L, NULL, &isstr);
  if (isstr)
    id = ffi_checkctype(L, cts, NULL);
  name = lj_lib_checkstr(L, 2);
  {
    CType snap;
    int ok = ffi_layout_offsetof_snapshot(cts, id, name, &ofs, &snap);
    if (ok < 0)
      ok = ffi_layout_offsetof_wait(L, cts, id, name, &ofs, &snap);
    if (ok > 0) {
      setintV(L->top-1, ofs);
      if (ctype_isfield(snap.info)) {
	return 1;
      } else if (ctype_isbitfield(snap.info)) {
	setintV(L->top++, ctype_bitpos(snap.info));
	setintV(L->top++, ctype_bitbsz(snap.info));
	return 3;
      }
    } else if (ok == 0) {
      return 0;
    }
  }
  return 0;
}

LJLIB_CF(ffi_errno)	LJLIB_REC(.)
{
  int err = errno;
  if (L->top > L->base)
    errno = ffi_checkint(L, 1);
  setintV(L->top++, err);
  return 1;
}

LJLIB_CF(ffi_string)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  TValue *o = lj_lib_checkany(L, 1);
  const char *p;
  size_t len;
  if (o+1 < L->top && !tvisnil(o+1)) {
    len = (size_t)ffi_checkint(L, 2);
    lj_cconv_ct_tv_l(L, cts, ctype_get(cts, CTID_P_CVOID), CTID_P_CVOID,
		     (uint8_t *)&p, o, CCF_ARG(1));
  } else {
    lj_cconv_ct_tv_l(L, cts, ctype_get(cts, CTID_P_CCHAR), CTID_P_CCHAR,
		     (uint8_t *)&p, o, CCF_ARG(1));
    len = strlen(p);
  }
  L->top = o+1;  /* Make sure this is the last item on the stack. */
  setstrV(L, o, lj_str_new(L, p, len));
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_copy)	LJLIB_REC(.)
{
  void *dp = ffi_checkptr(L, 1, CTID_P_VOID);
  void *sp = ffi_checkptr(L, 2, CTID_P_CVOID);
  TValue *o = L->base+1;
  CTSize len;
  if (tvisstr(o) && o+1 >= L->top)
    len = strV(o)->len+1;  /* Copy Lua string including trailing '\0'. */
  else
    len = (CTSize)ffi_checkint(L, 3);
  memcpy(dp, sp, len);
  return 0;
}

LJLIB_CF(ffi_fill)	LJLIB_REC(.)
{
  void *dp = ffi_checkptr(L, 1, CTID_P_VOID);
  CTSize len = (CTSize)ffi_checkint(L, 2);
  int32_t fill = 0;
  if (L->base+2 < L->top && !tvisnil(L->base+2)) fill = ffi_checkint(L, 3);
  memset(dp, fill, len);
  return 0;
}

/* Test ABI string. */
LJLIB_CF(ffi_abi)	LJLIB_REC(.)
{
  GCstr *s = lj_lib_checkstr(L, 1);
  int b = lj_cparse_case(s,
#if LJ_64
    "\00564bit"
#else
    "\00532bit"
#endif
#if LJ_ARCH_HASFPU
    "\003fpu"
#endif
#if LJ_ABI_SOFTFP
    "\006softfp"
#else
    "\006hardfp"
#endif
#if LJ_ABI_EABI
    "\004eabi"
#endif
#if LJ_ABI_WIN
    "\003win"
#endif
#if LJ_ABI_PAUTH
    "\005pauth"
#endif
#if LJ_TARGET_UWP
    "\003uwp"
#endif
#if LJ_LE
    "\002le"
#else
    "\002be"
#endif
#if LJ_GC64
    "\004gc64"
#endif
#if LJ_DUALNUM
    "\007dualnum"
#endif
  ) >= 0;
  setboolV(L->top-1, b);
  setboolV(&L2TG(L)->tmptv2, b);  /* Remember for trace recorder. */
  return 1;
}

LJLIB_PUSH(top-7) LJLIB_SET(!)  /* Store reference to miscmap table. */

LJLIB_CF(ffi_metatype)
{
  CTState *cts = ctype_cts(L);
  CTypeID id;
  GCtab *mt = lj_lib_checktab(L, 2);
  CTypeID rid;
  CTInfo info;
  int isstr;
  TValue tmp;
  GCcdata *cd;
  CTSize sz;
  int ok;
  id = ffi_checkctype_noparse(L, NULL, &isstr);
  if (isstr)
    id = ffi_checkctype(L, cts, NULL);
  ok = ffi_ctype_info_read(L, cts, id, &info, &sz, &rid, NULL);
  if (ok <= 0)
    lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  if (!(ctype_isstruct(info) || ctype_iscomplex(info) || ctype_isvector(info)))
    lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  if (!lj_ctype_setmeta(cts, rid, mt))
    lj_err_caller(L, LJ_ERR_PROTMT);
  settabV(L, &tmp, mt);
  lj_gc_pubroot(L, &tmp);  /* 11.2 metatype side root. */
  cd = lj_cdata_new_(L, CTID_CTYPEID, 4);
  *(CTypeID *)cdataptr(cd) = id;
  setcdataV(L, L->top-1, cd);
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_gc)	LJLIB_REC(.)
{
  GCcdata *cd = ffi_checkcdata(L, 1);
  TValue *fin = lj_lib_checkany(L, 2);
  CTState *cts = ctype_cts(L);
  CTInfo info;
  CTSize sz;
  int ok = ffi_ctype_info_read(L, cts, cd->ctypeid, &info, &sz, NULL, NULL);
  if (ok <= 0)
    lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  if (!(ctype_isptr(info) || ctype_isstruct(info) || ctype_isrefarray(info)))
    lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  lj_cdata_setfin(L, cd, gcval(fin), itype(fin));
  L->top = L->base+1;  /* Pass through the cdata object. */
  return 1;
}

LJLIB_CF(ffi_pin)
{
  TValue *o = lj_lib_checkany(L, 1);
  CTState *cts = ctype_cts(L);
  GCtab *mt = ctype_pinmt_acq(cts);
  GCudata *ud = lj_udata_new(L, sizeof(TValue), mt);
  setgcrefmt(ud->metatable, obj2gco(mt));
  lj_gc_pubobjobj(L, ud, mt);
  lj_gc2_finreg_udata_register_mt(L, G(L), ud, mt);
  copyTVrel(L, (TValue *)uddata(ud), o);
  lj_gc_pubobjtv(L, ud, (TValue *)uddata(ud));
  lj_udata_udtype_rel(ud, UDTYPE_FFI_PIN);
  setudataV(L, L->top++, ud);
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_blocking)
{
  GCcdata *cd = ffi_checkcdata(L, 1);
  CTState *cts = ctype_cts(L);
  CTypeID id = cd->ctypeid;
  CTInfo info;
  CTSize sz = CTSIZE_PTR;
  CTSize snap_size;
  int ok = ffi_ctype_info_read(L, cts, id, &info, &snap_size, NULL, NULL);
  if (ok <= 0)
    lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  if (ctype_isptr(info)) {
    id = ctype_cid(info);
    sz = snap_size;
    ok = ffi_ctype_info_read(L, cts, id, &info, &snap_size, NULL, NULL);
    if (ok <= 0)
      lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  }
  if (!ctype_isfunc(info))
    lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  lj_ctype_cb_blacklist(cts, cdata_getptr(cdataptr(cd), sz));
  (void)lj_trace_flushall_hs(L);
  L->top = L->base+1;  /* Pass through the function pointer. */
  return 1;
}

LJLIB_PUSH(top-5) LJLIB_SET(!)  /* Store clib metatable in func environment. */

LJLIB_CF(ffi_load)
{
  GCstr *name = lj_lib_checkstr(L, 1);
  int global = (L->base+1 < L->top && tvistruecond(L->base+1));
  lj_clib_load(L, tabref_acq(curr_func(L)->c.env), name, global);
  return 1;
}

LJLIB_PUSH(top-4) LJLIB_SET(C)
LJLIB_PUSH(top-3) LJLIB_SET(os)
LJLIB_PUSH(top-2) LJLIB_SET(arch)

#include "lj_libdef.h"

/* ------------------------------------------------------------------------ */

static TValue *ffi_loaded_store(lua_State *L, GCtab *t, GCstr *name,
				cTValue *src)
{
  TValue keytv, *dst;
  setstrV(L, &keytv, name);
  for (;;) {
    dst = lj_tab_setstr(L, t, name);
    if (lj_tab_trystoretv_cas_keyed(L, t, dst, &keytv, src) ==
	LJ_TAB_STORE_CAS_OK)
      return dst;
    lj_tab_store_wait_no_l();  /* FFI module registry saw stale/FORWARD slot. */
  }
}

static TValue *ffi_miscmap_store(lua_State *L, CTState *cts, GCstr *key,
				 cTValue *src)
{
  GCtab *miscmap = ctype_miscmap_acq(cts);
  TValue keytv, *dst;
  setstrV(L, &keytv, key);
  for (;;) {
    dst = lj_tab_setstr(L, miscmap, key);
    if (lj_tab_trystoretv_cas_keyed(L, miscmap, dst, &keytv, src) ==
	LJ_TAB_STORE_CAS_OK)
      return dst;
    lj_tab_store_wait_no_l();  /* FFI miscmap store saw stale/FORWARD slot. */
  }
}

/* Register FFI module as loaded. */
static void ffi_register_module(lua_State *L)
{
  cTValue *tmp = lj_tab_getstr(tabV(registry(L)), lj_str_newlit(L, "_LOADED"));
  if (tmp) {
    TValue loaded;
    lj_tv_load_acq(&loaded, tmp);
    if (tvistab(&loaded)) {
      GCtab *t = tabV(&loaded);
      GCstr *name = lj_str_newlit(L, LUA_FFILIBNAME);
      TValue key;
      setstrV(L, &key, name);
      ffi_loaded_store(L, t, name, L->top-1);
      lj_gc2_barrier_weak_write(L, t, &key, L->top-1);
      lj_gc_pubtab(L, t);
    }
  }
}

LUALIB_API int luaopen_ffi(lua_State *L)
{
  CTState *cts = lj_ctype_init(L);
  GCtab *miscmap;
  lj_ccallback_init_l(L, cts);
  miscmap = lj_tab_new(L, 0, 1);
  ctype_miscmap_rel(cts, miscmap);
  settabV(L, L->top++, miscmap);
  LJ_LIB_REG(L, NULL, ffi_meta);
  /* NOBARRIER: basemt is a GC root. */
  setgcrefroot(basemt_it(G(L), LJ_TCDATA), obj2gco(tabV(L->top-1)));
  LJ_LIB_REG(L, NULL, ffi_clib);
  LJ_LIB_REG(L, NULL, ffi_callback);
  ffi_miscmap_store(L, cts, &cts->g->strempty, L->top-1);
  lj_gc_pubtabobj(L, miscmap, tabV(L->top-1));
  L->top--;
  LJ_LIB_REG(L, NULL, ffi_pin);
  ctype_pinmt_rel(cts, tabV(L->top-1));
  L->top--;
  lj_clib_default(L, tabV(L->top-1));  /* Create ffi.C default namespace. */
  lua_pushliteral(L, LJ_OS_NAME);
  lua_pushliteral(L, LJ_ARCH_NAME);
  LJ_LIB_REG(L, NULL, ffi);  /* Note: no global "ffi" created! */
  ffi_register_module(L);
  return 1;
}

#endif
