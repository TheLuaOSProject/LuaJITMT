/*
** Trace recorder for C data operations.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_ffrecord_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT && LJ_HASFFI

#include "lj_err.h"
#include "lj_tab.h"
#include "lj_frame.h"
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lj_cparse.h"
#include "lj_cconv.h"
#include "lj_carith.h"
#include "lj_clib.h"
#include "lj_ccall.h"
#include "lj_ff.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_ircall.h"
#include "lj_iropt.h"
#include "lj_trace.h"
#include "lj_record.h"
#include "lj_ffrecord.h"
#include "lj_snap.h"
#include "lj_crecord.h"
#include "lj_dispatch.h"
#include "lj_str.h"
#include "lj_strfmt.h"
#include "lj_strscan.h"

/* Some local macros to save typing. Undef'd at the end. */
#define IR(ref)			(&J->cur.ir[(ref)])

/* Pass IR on to next optimization in chain (FOLD). */
#define emitir(ot, a, b)	(lj_ir_set(J, (ot), (a), (b)), lj_opt_fold(J))

#define emitconv(a, dt, st, flags) \
  emitir(IRT(IR_CONV, (dt)), (a), (st)|((dt) << 5)|(flags))

/*
** Safety bridge for 11.5: ordinary FFI C calls must not run from traced mcode
** until IR_CALLXS has a native-state enter/leave protocol. The interpreted
** ccall path already marks native and checks STOPREQ after result conversion.
*/
#ifndef LJ_FFI_RECORD_CALLS
#define LJ_FFI_RECORD_CALLS 0
#endif
#if LJ_FFI_RECORD_CALLS
#error "LJ_FFI_RECORD_CALLS requires an IR_CALLXS native-state protocol"
#endif

/* -- C type checks ------------------------------------------------------- */

static GCcdata *argv2cdata(jit_State *J, TRef tr, cTValue *o)
{
  GCcdata *cd;
  TRef trtypeid;
  if (!tref_iscdata(tr))
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  cd = cdataV(o);
  /* Specialize to the CTypeID. */
  trtypeid = emitir(IRT(IR_FLOAD, IRT_U16), tr, IRFL_CDATA_CTYPEID);
  emitir(IRTG(IR_EQ, IRT_INT), trtypeid, lj_ir_kint(J, (int32_t)cd->ctypeid));
  return cd;
}

/* Specialize to the CTypeID held by a cdata constructor. */
static CTypeID crec_constructor(jit_State *J, GCcdata *cd, TRef tr)
{
  CTypeID id;
  lj_assertJ(tref_iscdata(tr) && cd->ctypeid == CTID_CTYPEID,
	     "expected CTypeID cdata");
  id = *(CTypeID *)cdataptr(cd);
  tr = emitir(IRT(IR_FLOAD, IRT_INT), tr, IRFL_CDATA_INT);
  emitir(IRTG(IR_EQ, IRT_INT), tr, lj_ir_kint(J, (int32_t)id));
  return id;
}

static int crec_cspace(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	 c == '\f' || c == '\v';
}

#define CREC_DIRECT_MAX_DECL_SUFFIXES 16

static int crec_isident(uint8_t c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	 (c >= '0' && c <= '9') || c == '_';
}

static int crec_strlit(const char *p, MSize plen, const char *lit, MSize len)
{
  if (plen != len)
    return 0;
  while (len-- != 0)
    if (*p++ != *lit++)
      return 0;
  return 1;
}

static int crec_qual_token(const char *p, MSize len, CTInfo *qualp)
{
  if (len == 5 && crec_strlit(p, len, "const", 5)) {
    *qualp = CTF_CONST;
    return 1;
  }
  if (len == 8 && crec_strlit(p, len, "volatile", 8)) {
    *qualp = CTF_VOLATILE;
    return 1;
  }
  if (len == 7 && crec_strlit(p, len, "__const", 7)) {
    *qualp = CTF_CONST;
    return 1;
  }
  if (len == 9 && crec_strlit(p, len, "__const__", 9)) {
    *qualp = CTF_CONST;
    return 1;
  }
  if (len == 10 && crec_strlit(p, len, "__volatile", 10)) {
    *qualp = CTF_VOLATILE;
    return 1;
  }
  if (len == 12 && crec_strlit(p, len, "__volatile__", 12)) {
    *qualp = CTF_VOLATILE;
    return 1;
  }
  if (len == 8 && crec_strlit(p, len, "restrict", 8)) {
    *qualp = 0;
    return 1;
  }
  if (len == 10 && crec_strlit(p, len, "__restrict", 10)) {
    *qualp = 0;
    return 1;
  }
  if (len == 12 && crec_strlit(p, len, "__restrict__", 12)) {
    *qualp = 0;
    return 1;
  }
  if (len == 13 && crec_strlit(p, len, "__extension__", 13)) {
    *qualp = 0;
    return 1;
  }
  return 0;
}

static int crec_ident_string(const char *p, MSize len)
{
  MSize i;
  if (len == 0 || (p[0] >= '0' && p[0] <= '9'))
    return 0;
  for (i = 0; i < len; i++)
    if (!crec_isident((uint8_t)p[i]))
      return 0;
  return 1;
}

static int crec_sign_token(const char *p, MSize len, int *signp)
{
  if (len == 6 && crec_strlit(p, len, "signed", 6)) {
    *signp = 1;
    return 1;
  }
  if (len == 8 && crec_strlit(p, len, "__signed", 8)) {
    *signp = 1;
    return 1;
  }
  if (len == 10 && crec_strlit(p, len, "__signed__", 10)) {
    *signp = 1;
    return 1;
  }
  if (len == 8 && crec_strlit(p, len, "unsigned", 8)) {
    *signp = 2;
    return 1;
  }
  return 0;
}

static int crec_int_keyword_token(const char *p, MSize len, int *bitsp)
{
  if (len == 6 && crec_strlit(p, len, "__int8", 6)) {
    *bitsp = 8;
    return 1;
  }
  if (len == 7 && crec_strlit(p, len, "__int16", 7)) {
    *bitsp = 16;
    return 1;
  }
  if (len == 7 && crec_strlit(p, len, "__int32", 7)) {
    *bitsp = 32;
    return 1;
  }
  if (len == 7 && crec_strlit(p, len, "__int64", 7)) {
    *bitsp = 64;
    return 1;
  }
  if (len == 8 && crec_strlit(p, len, "__int128", 8)) {
    *bitsp = 128;
    return 1;
  }
  return 0;
}

static int crec_integer_spec_token(const char *p, MSize len, int *tokp)
{
  if (len == 4 && crec_strlit(p, len, "char", 4)) {
    *tokp = 1;
    return 1;
  }
  if (len == 5 && crec_strlit(p, len, "short", 5)) {
    *tokp = 2;
    return 1;
  }
  if (len == 3 && crec_strlit(p, len, "int", 3)) {
    *tokp = 3;
    return 1;
  }
  if (len == 4 && crec_strlit(p, len, "long", 4)) {
    *tokp = 4;
    return 1;
  }
  return 0;
}

static int crec_direct_int_keyword_ctype(jit_State *J, CTState *cts,
					 const char *p, MSize len,
					 CTypeID *idp)
{
  int sign = 0, bits = 0;
  while (len != 0 && crec_cspace(*p)) { p++; len--; }
  while (len != 0 && crec_cspace(p[len-1])) len--;
  while (len != 0) {
    MSize toklen = 0;
    int tok;
    while (toklen < len && !crec_cspace(p[toklen])) toklen++;
    if (crec_int_keyword_token(p, toklen, &tok)) {
      if (bits != 0)
	return 0;
      bits = tok;
    } else if (crec_sign_token(p, toklen, &tok)) {
      if (sign != 0)
	return 0;
      sign = tok;
    } else {
      return 0;
    }
    p += toklen;
    len -= toklen;
    while (len != 0 && crec_cspace(*p)) { p++; len--; }
  }
  if (bits == 0)
    return 0;
  if (sign == 2) {
    if (bits == 8) *idp = CTID_UINT8;
    else if (bits == 16) *idp = CTID_UINT16;
    else if (bits == 32) *idp = CTID_UINT32;
    else if (bits == 128) *idp = CTID_UINT128;
    else *idp = lj_ctype_intern_l(J->L, cts,
				  CTINFO(CT_NUM, CTF_UNSIGNED|CTALIGN(3)), 8);
  } else {
    if (bits == 8) *idp = CTID_INT8;
    else if (bits == 16) *idp = CTID_INT16;
    else if (bits == 32) *idp = CTID_INT32;
    else if (bits == 128) *idp = CTID_INT128;
    else *idp = lj_ctype_intern_l(J->L, cts, CTINFO(CT_NUM, CTALIGN(3)), 8);
  }
  return 1;
}

static int crec_direct_integer_spec_ctype(jit_State *J, CTState *cts,
					  const char *p, MSize len,
					  CTypeID *idp)
{
  int sign = 0, seen_char = 0, seen_short = 0, seen_int = 0, nlong = 0;
  int seen = 0;
  while (len != 0 && crec_cspace(*p)) { p++; len--; }
  while (len != 0 && crec_cspace(p[len-1])) len--;
  while (len != 0) {
    MSize toklen = 0;
    int tok;
    while (toklen < len && !crec_cspace(p[toklen])) toklen++;
    if (crec_sign_token(p, toklen, &tok)) {
      if (sign != 0)
	return 0;
      sign = tok;
    } else if (crec_integer_spec_token(p, toklen, &tok)) {
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
    while (len != 0 && crec_cspace(*p)) { p++; len--; }
  }
  if (!seen)
    return 0;
  if (seen_char) {
    *idp = sign == 2 ? CTID_UINT8 : CTID_INT8;
  } else if (seen_short) {
    *idp = sign == 2 ? CTID_UINT16 : CTID_INT16;
  } else if (nlong == 2) {
    *idp = lj_ctype_intern_l(J->L, cts,
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

static int crec_direct_numeric_ctype(jit_State *J, CTState *cts,
				     const char *p, MSize len, CTypeID *idp)
{
  CTInfo info;
  CTSize size;
  while (len != 0 && crec_cspace(*p)) { p++; len--; }
  while (len != 0 && crec_cspace(p[len-1])) len--;
  if (crec_strlit(p, len, "long long", 9) ||
      crec_strlit(p, len, "long long int", 13) ||
      crec_strlit(p, len, "signed long long", 16) ||
      crec_strlit(p, len, "signed long long int", 20)) {
    info = CTINFO(CT_NUM, CTALIGN(3));
    size = 8;
  } else if (crec_strlit(p, len, "unsigned long long", 18) ||
	     crec_strlit(p, len, "unsigned long long int", 22)) {
    info = CTINFO(CT_NUM, CTF_UNSIGNED|CTALIGN(3));
    size = 8;
  } else if (crec_strlit(p, len, "long double", 11)) {
    size = (CTSize)sizeof(long double);
    info = CTINFO(CT_NUM, CTF_FP|CTALIGN(lj_fls(size)));
  } else {
    return 0;
  }
  *idp = lj_ctype_intern_l(J->L, cts, info, size);
  return 1;
}

static int crec_qual_prefix(const char *p, MSize len, MSize *toklenp,
			    CTInfo *qualp)
{
  MSize i = 0;
  while (i < len && crec_isident((uint8_t)p[i])) i++;
  if (i == 0 || (i < len && !crec_cspace(p[i])))
    return 0;
  if (!crec_qual_token(p, i, qualp))
    return 0;
  *toklenp = i;
  return 1;
}

static int crec_qual_suffix(const char *p, MSize len, MSize *startp,
			    CTInfo *qualp)
{
  MSize start = len;
  while (start != 0 && crec_isident((uint8_t)p[start-1])) start--;
  if (start == len || (start != 0 && !crec_cspace(p[start-1])))
    return 0;
  if (!crec_qual_token(p + start, len - start, qualp))
    return 0;
  *startp = start;
  return 1;
}

static int crec_direct_qual_part(const char **pp, MSize *lenp,
				 CTInfo *qualp)
{
  const char *p = *pp;
  MSize len = *lenp;
  CTInfo qual = 0;
  int seen = 0;
  for (;;) {
    CTInfo q;
    MSize toklen;
    while (len != 0 && crec_cspace(*p)) { p++; len--; }
    if (!crec_qual_prefix(p, len, &toklen, &q))
      break;
    qual |= q;
    seen = 1;
    p += toklen;
    len -= toklen;
  }
  for (;;) {
    CTInfo q;
    MSize start;
    while (len != 0 && crec_cspace(p[len-1])) len--;
    if (!crec_qual_suffix(p, len, &start, &q))
      break;
    qual |= q;
    seen = 1;
    len = start;
  }
  while (len != 0 && crec_cspace(*p)) { p++; len--; }
  while (len != 0 && crec_cspace(p[len-1])) len--;
  *pp = p;
  *lenp = len;
  *qualp = qual;
  return seen;
}

static int crec_lookup_named_ctype(jit_State *J, CTState *cts, GCstr *name,
				   uint32_t tmask, CTypeID *idp, CType *out)
{
  int ok = lj_ctype_getname_snapshot(cts, name, tmask, idp, out, NULL);
  if (ok < 0)
    lj_trace_err(J, LJ_TRERR_CTBUSY);
  return ok;
}

static int crec_direct_ctype_info(jit_State *J, CTState *cts, CTypeID id,
				  CTInfo *infop, CTSize *szp,
				  CTypeID *ridp, CType *rawp)
{
  int ok = lj_ctype_info_predefined(cts, id, infop, szp, ridp, rawp);
  if (ok)
    return ok;
  ok = lj_ctype_info_snapshot(cts, id, infop, szp, ridp, rawp);
  if (ok < 0)
    lj_trace_err(J, LJ_TRERR_CTBUSY);
  return ok;
}

static int crec_direct_qualified_ctype(jit_State *J, CTState *cts,
				       CTypeID baseid, CTInfo qual,
				       CTypeID *idp)
{
  CType raw;
  CTypeID rid;
  CTInfo info;
  CTSize size;
  int ok = crec_direct_ctype_info(J, cts, baseid, &info, &size, &rid, &raw);
  if (ok <= 0)
    return 0;
  if (qual == 0) {
    *idp = baseid;
    return 1;
  }
  info = ctype_info_acq(&raw);
  size = ctype_size_acq(&raw);
  if (ctype_isstruct(info) || ctype_isenum(info)) {
    *idp = lj_ctype_intern_l(J->L, cts,
			     CTINFO(CT_ATTRIB, CTATTRIB(CTA_QUAL)|rid),
			     qual);
    return 1;
  }
  if (ctype_isattrib(info))
    return 0;
  *idp = lj_ctype_intern_l(J->L, cts, info|qual, size);
  return 1;
}

static int crec_direct_ctype_base_unqualified(jit_State *J, CTState *cts,
					      GCstr *s, const char *p,
					      MSize len, CTypeID *idp)
{
  CType ct;
  CTypeID id;
  CTInfo info;
  if (lj_ctype_predefined_string(p, len, idp))
    return 1;
  if (crec_direct_int_keyword_ctype(J, cts, p, len, idp))
    return 1;
  if (crec_direct_integer_spec_ctype(J, cts, p, len, idp))
    return 1;
  if (crec_direct_numeric_ctype(J, cts, p, len, idp))
    return 1;
  while (len != 0 && crec_cspace(*p)) { p++; len--; }
  while (len != 0 && crec_cspace(p[len-1])) len--;
  if (crec_ident_string(p, len)) {
    GCstr *name = (p == strdata(s) && len == s->len) ? s :
		  lj_str_new(J->L, p, len);
    int ok = crec_lookup_named_ctype(J, cts, name, (1u << CT_TYPEDEF),
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
    if (len >= 6 && crec_strlit(p, 6, "struct", 6)) {
      kwlen = 6;
      tmask = (1u << CT_STRUCT);
    } else if (len >= 5 && crec_strlit(p, 5, "union", 5)) {
      kwlen = 5;
      tmask = (1u << CT_STRUCT);
      wantunion = 1;
    } else if (len >= 4 && crec_strlit(p, 4, "enum", 4)) {
      kwlen = 4;
      tmask = (1u << CT_ENUM);
      wantenum = 1;
    } else {
      return 0;
    }
    if (len == kwlen || !crec_cspace(p[kwlen]))
      return 0;
    p += kwlen + 1;
    len -= kwlen + 1;
    while (len != 0 && crec_cspace(*p)) { p++; len--; }
    if (!crec_ident_string(p, len))
      return 0;
    {
      GCstr *name = lj_str_new(J->L, p, len);
      int ok = crec_lookup_named_ctype(J, cts, name, tmask, &id, &ct);
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

static int crec_direct_ctype_base_string(jit_State *J, CTState *cts,
					 GCstr *s, const char *p,
					 MSize len, CTypeID *idp)
{
  const char *q = p;
  MSize qlen = len;
  CTInfo qual;
  if (crec_direct_qual_part(&q, &qlen, &qual)) {
    CTypeID baseid;
    if (qlen == 0)
      return 0;
    if (crec_direct_ctype_base_unqualified(J, cts, s, q, qlen, &baseid) &&
	crec_direct_qualified_ctype(J, cts, baseid, qual, idp))
      return 1;
    return 0;
  }
  return crec_direct_ctype_base_unqualified(J, cts, s, p, len, idp);
}

static int crec_pointer_qual_suffix(const char *p, MSize *lenp,
				    CTInfo *qualp)
{
  MSize len = *lenp;
  MSize start = len;
  while (start != 0 && crec_isident((uint8_t)p[start-1])) start--;
  if (start == len)
    return 0;
  if (start != 0 && !crec_cspace(p[start-1]) && p[start-1] != '*')
    return 0;
  if (!crec_qual_token(p + start, len - start, qualp))
    return 0;
  *lenp = start;
  return 1;
}

static int crec_direct_pointer_suffix(const char *p, MSize *lenp,
				      CTInfo *qualp)
{
  MSize len = *lenp;
  CTInfo qual = 0;
  for (;;) {
    CTInfo q;
    while (len != 0 && crec_cspace(p[len-1])) len--;
    if (!crec_pointer_qual_suffix(p, &len, &q))
      break;
    qual |= q;
  }
  while (len != 0 && crec_cspace(p[len-1])) len--;
  if (len == 0 || p[len-1] != '*')
    return 0;
  len--;
  while (len != 0 && crec_cspace(p[len-1])) len--;
  *lenp = len;
  *qualp = qual;
  return 1;
}

static int crec_direct_array_suffix(const char *p, MSize *lenp, CTSize *nelemp)
{
  MSize len = *lenp, i, dstart, dend;
  uint64_t nelem = 0;
  while (len != 0 && crec_cspace(p[len-1])) len--;
  if (len == 0 || p[len-1] != ']')
    return 0;
  i = len - 1;
  while (i != 0 && crec_cspace(p[i-1])) i--;
  dend = i;
  while (i != 0 && p[i-1] >= '0' && p[i-1] <= '9') i--;
  dstart = i;
  if (dstart == dend)
    return 0;
  if (dend - dstart > 1 && p[dstart] == '0')
    return 0;
  for (i = dstart; i < dend; i++) {
    nelem = nelem * 10u + (uint32_t)(p[i] - '0');
    if (nelem >= 0x80000000u)
      return 0;
  }
  i = dstart;
  while (i != 0 && crec_cspace(p[i-1])) i--;
  if (i == 0 || p[i-1] != '[')
    return 0;
  i--;
  while (i != 0 && crec_cspace(p[i-1])) i--;
  if (i == 0)
    return 0;
  *lenp = i;
  *nelemp = (CTSize)nelem;
  return 1;
}

static int crec_direct_ctype_string(jit_State *J, GCstr *s, CTypeID *idp)
{
  CTState *cts = ctype_ctsG(J2G(J));
  const char *p = strdata(s);
  MSize len = s->len;
  MSize baselen, narr = 0, i;
  CTSize nelem[CREC_DIRECT_MAX_DECL_SUFFIXES], esize;
  CTInfo einfo;
  CTypeID elemid;
  while (len != 0 && crec_cspace(*p)) { p++; len--; }
  while (len != 0 && crec_cspace(p[len-1])) len--;
  baselen = len;
  for (;;) {
    CTSize n;
    MSize nextlen = baselen;
    if (!crec_direct_array_suffix(p, &nextlen, &n))
      break;
    if (narr == CREC_DIRECT_MAX_DECL_SUFFIXES)
      return 0;
    nelem[narr++] = n;
    baselen = nextlen;
  }
  {
    CTInfo pqual[CREC_DIRECT_MAX_DECL_SUFFIXES];
    MSize nptr = 0;
    for (;;) {
      CTInfo qual;
      MSize nextlen = baselen;
      if (!crec_direct_pointer_suffix(p, &nextlen, &qual))
	break;
      if (nptr == CREC_DIRECT_MAX_DECL_SUFFIXES)
	return 0;
      pqual[nptr++] = qual;
      baselen = nextlen;
    }
    if (narr == 0 && nptr == 0)
      return crec_direct_ctype_base_string(J, cts, s, p, len, idp);
    if (!crec_direct_ctype_base_string(J, cts, s, p, baselen, &elemid))
      return 0;
    if (nptr != 0) {
      while (nptr-- != 0) {
	einfo = CTINFO(CT_PTR, CTALIGN_PTR|pqual[nptr]|elemid);
	elemid = lj_ctype_intern_l(J->L, cts, einfo, CTSIZE_PTR);
      }
      esize = CTSIZE_PTR;
    } else {
      if (crec_direct_ctype_info(J, cts, elemid, &einfo, &esize, NULL, NULL) <= 0 ||
	  ctype_isref(einfo) || ctype_isvltype(einfo) ||
	  esize == CTSIZE_INVALID)
	return 0;
    }
  }
  for (i = 0; i < narr; i++) {
    uint64_t asize = (uint64_t)nelem[i] * esize;
    if (asize >= 0x80000000u)
      return 0;
    einfo = CTINFO(CT_ARRAY, elemid) | (einfo & (CTF_ALIGN|CTF_QUAL));
    esize = (CTSize)asize;
    elemid = lj_ctype_intern_l(J->L, cts, einfo, esize);
  }
  *idp = elemid;
  return 1;
}

static CTypeID argv2ctype_direct(jit_State *J, TRef tr, cTValue *o,
				 int *directp)
{
  if (directp) *directp = 0;
  if (tref_isstr(tr)) {
    GCstr *s = strV(o);
    CPState cp;
    int errcode;
    CTypeID id;
    /* Specialize to the string containing the C type declaration. */
    emitir(IRTG(IR_EQ, IRT_STR), tr, lj_ir_kstr(J, s));
    if (lj_ctype_predefined_string(strdata(s), s->len, &id)) {
      if (directp) *directp = 1;
      return id;
    }
    if (crec_direct_ctype_string(J, s, &id)) {
      if (directp) *directp = 1;
      return id;
    }
    cp.L = J->L;
    cp.cts = ctype_cts(J->L);
    {
      uint32_t seq = ctype_parse_token_acq(cp.cts);
      uint32_t expect = seq;
      if ((seq & 1u) || !ctype_parse_token_cas(cp.cts, &expect, seq + 1u))
	lj_trace_err(J, LJ_TRERR_CTBUSY);
    }
    cp.srcname = strdata(s);
    cp.p = strdata(s);
    cp.param = NULL;
    cp.mode = CPARSE_MODE_ABSTRACT|CPARSE_MODE_NOIMPLICIT;
    errcode = lj_cparse(&cp);
    lj_ctype_parse_unlock(cp.cts);
    if (errcode || cp.newtype)  /* Avoid parser-defined unique types. */
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    return cp.val.id;
  } else {
    GCcdata *cd = argv2cdata(J, tr, o);
    return cd->ctypeid == CTID_CTYPEID ? crec_constructor(J, cd, tr) :
					cd->ctypeid;
  }
}

static CTypeID argv2ctype(jit_State *J, TRef tr, cTValue *o)
{
  return argv2ctype_direct(J, tr, o, NULL);
}

static CType *crec_ctype_snapshot(jit_State *J, CTState *cts, CTypeID id,
				  CType *out)
{
  int ok = lj_ctype_snapshot(cts, id, out);
  if (ok < 0)
    lj_trace_err(J, LJ_TRERR_CTBUSY);
  if (!ok)
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  return out;
}

static int crec_ctype_predefined_child(CTInfo info)
{
  CTypeID cid = ctype_cid(info);
  return cid > CTID_NONE && cid <= CTID_CTYPEID;
}

static int crec_ctype_shallow_predef_ptr(CTState *cts, CTypeID id,
					 CTypeID *ridp, CType *out)
{
  CTypeTab *tabh;
  CType *ct;
  CTInfo info;
  CTSize size;
  if (id == 0 || out == NULL)
    return 0;
  tabh = ctype_tabh_acq(cts);
  if (id >= ctype_top_acq(cts) || (MSize)id >= ctype_tab_sizetab_acq(tabh))
    return 0;
  ct = ctype_tab_slot(tabh, id);
  info = ctype_info_acq(ct);
  size = ctype_size_acq(ct);
  if (!ctype_isptr(info) || !crec_ctype_predefined_child(info) ||
      ctype_isabandoned(info))
    return 0;
  ctype_info_rel(out, info);
  ctype_size_rel(out, size);
  ctype_sib_rel(out, 0);
  ctype_next_rel(out, 0);
  ctype_clearname(out);
  if (ridp)
    *ridp = id;
  return 1;
}

static CType *crec_ctype_rawid(jit_State *J, CTState *cts, CTypeID id,
			       CTypeID *ridp, CType *out)
{
  CTInfo info;
  CTSize size;
  int ok = lj_ctype_info_predefined(cts, id, &info, &size, ridp, out);
  if (ok <= 0)
    ok = crec_ctype_shallow_predef_ptr(cts, id, ridp, out);
  if (ok <= 0)
    ok = lj_ctype_info_snapshot(cts, id, &info, &size, ridp, out);
  if (ok < 0)
    ok = crec_ctype_shallow_predef_ptr(cts, id, ridp, out);
  UNUSED(info); UNUSED(size);
  if (ok < 0)
    lj_trace_err(J, LJ_TRERR_CTBUSY);
  if (!ok)
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  return out;
}

static CType *crec_ctype_rawrefid(jit_State *J, CTState *cts, CTypeID id,
				  CTypeID *ridp, CType *out)
{
  int ok = lj_ctype_rawref_predefined(cts, id, ridp, out);
  if (ok < 0)
    ok = lj_ctype_rawref_snapshot(cts, id, ridp, out);
  if (ok < 0)
    lj_trace_err(J, LJ_TRERR_CTBUSY);
  if (!ok)
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  return out;
}

static CType *crec_ctype_rawref(jit_State *J, CTState *cts, CTypeID id,
				CType *out)
{
  return crec_ctype_rawrefid(J, cts, id, NULL, out);
}

static int crec_cconv_multi_init(jit_State *J, CTState *cts, CTypeID did,
				 CType *d, TValue *o)
{
  CTInfo dinfo = ctype_info_acq(d);
  if (!(ctype_isrefarray(dinfo) || ctype_isstruct(dinfo)))
    return 0;  /* Destination is not an aggregate. */
  if (tvistab(o) || (tvisstr(o) && !ctype_isstruct(dinfo)))
    return 0;  /* Initializer is not a value. */
  if (tviscdata(o)) {
    CType snap;
    CTypeID sid;
    crec_ctype_rawrefid(J, cts, cdataV(o)->ctypeid, &sid, &snap);
    if (sid == did)
      return 0;  /* Source and destination are identical aggregates. */
  }
  return 1;  /* Otherwise the initializer is a value. */
}

static CType *crec_ctype_rawchildid(jit_State *J, CTState *cts, CType *ct,
				    CTypeID *ridp, CType *out)
{
  CTInfo parent = ctype_info_acq(ct);
  CTInfo info;
  CTSize size;
  int ok = lj_ctype_info_predefined(cts, ctype_cid(parent), &info, &size,
				    ridp, out);
  if (ok <= 0)
    ok = lj_ctype_info_snapshot(cts, ctype_cid(parent), &info, &size,
				ridp, out);
  UNUSED(info); UNUSED(size);
  if (ok < 0)
    lj_trace_err(J, LJ_TRERR_CTBUSY);
  if (!ok)
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  return out;
}

static CType *crec_ctype_rawchild(jit_State *J, CTState *cts, CType *ct,
				  CType *out)
{
  return crec_ctype_rawchildid(J, cts, ct, NULL, out);
}

static CTSize crec_ctype_size(jit_State *J, CTState *cts, CTypeID id)
{
  CTSize sz;
  int ok = lj_ctype_size_predefined(cts, id, &sz);
  if (ok < 0)
    ok = lj_ctype_size_snapshot(cts, id, &sz);
  if (ok < 0)
    lj_trace_err(J, LJ_TRERR_CTBUSY);
  return ok ? sz : CTSIZE_INVALID;
}

static IRType crec_ct2irt_snapshot(jit_State *J, CTState *cts, CType *ct)
{
  CTInfo info = ctype_info_acq(ct);
  CTSize size;
  CType child;
  if (ctype_isenum(info)) {
    ct = crec_ctype_snapshot(J, cts, ctype_cid(info), &child);
    info = ctype_info_acq(ct);
  }
  size = ctype_size_acq(ct);
  if (LJ_LIKELY(ctype_isnum(info))) {
    if ((info & CTF_FP)) {
      if (size == sizeof(double))
	return IRT_NUM;
      else if (size == sizeof(float))
	return IRT_FLOAT;
    } else {
      uint32_t b = lj_fls(size);
      if (b <= 3)
	return IRT_I8 + 2*b + ((info & CTF_UNSIGNED) ? 1 : 0);
    }
  } else if (ctype_isptr(info)) {
    return (LJ_64 && size == 8) ? IRT_P64 : IRT_P32;
  } else if (ctype_iscomplex(info)) {
    if (size == 2*sizeof(double))
      return IRT_NUM;
    else if (size == 2*sizeof(float))
      return IRT_FLOAT;
  }
  return IRT_CDATA;
}

/* Convert CType to IRType (if possible). */
static IRType crec_ct2irt(CTState *cts, CType *ct)
{
  CTInfo info = ctype_info_acq(ct);
  CTSize size;
  if (ctype_isenum(info)) {
    ct = ctype_child(cts, ct);
    info = ctype_info_acq(ct);
  }
  size = ctype_size_acq(ct);
  if (LJ_LIKELY(ctype_isnum(info))) {
    if ((info & CTF_FP)) {
      if (size == sizeof(double))
	return IRT_NUM;
      else if (size == sizeof(float))
	return IRT_FLOAT;
    } else {
      uint32_t b = lj_fls(size);
      if (b <= 3)
	return IRT_I8 + 2*b + ((info & CTF_UNSIGNED) ? 1 : 0);
    }
  } else if (ctype_isptr(info)) {
    return (LJ_64 && size == 8) ? IRT_P64 : IRT_P32;
  } else if (ctype_iscomplex(info)) {
    if (size == 2*sizeof(double))
      return IRT_NUM;
    else if (size == 2*sizeof(float))
      return IRT_FLOAT;
  }
  return IRT_CDATA;
}

/* -- Optimized memory fill and copy -------------------------------------- */

/* Maximum length and unroll of inlined copy/fill. */
#define CREC_COPY_MAXUNROLL		16
#define CREC_COPY_MAXLEN		128

#define CREC_FILL_MAXUNROLL		16

/* Number of windowed registers used for optimized memory copy. */
#if LJ_TARGET_X86
#define CREC_COPY_REGWIN		2
#elif LJ_TARGET_PPC || LJ_TARGET_MIPS
#define CREC_COPY_REGWIN		8
#else
#define CREC_COPY_REGWIN		4
#endif

/* List of memory offsets for copy/fill. */
typedef struct CRecMemList {
  CTSize ofs;		/* Offset in bytes. */
  IRType tp;		/* Type of load/store. */
  TRef trofs;		/* TRef of interned offset. */
  TRef trval;		/* TRef of load value. */
} CRecMemList;

/* Generate copy list for element-wise struct copy. */
static MSize crec_copy_struct(jit_State *J, CRecMemList *ml, CTState *cts,
			      CType *ct)
{
  CTypeID fid = ctype_sib_acq(ct);
  MSize mlp = 0;
  while (fid) {
    CType dfcopy, child;
    CType *df = crec_ctype_snapshot(J, cts, fid, &dfcopy);
    CTInfo dfinfo = ctype_info_acq(df);
    CTSize dfsize = ctype_size_acq(df);
    fid = ctype_sib_acq(df);
    if (ctype_isfield(dfinfo)) {
      CType *cct;
      CTInfo cctinfo;
      CTSize cctsize;
      IRType tp;
      if (!ctype_name_acq(df)) continue;  /* Ignore unnamed fields. */
      cct = crec_ctype_rawchild(J, cts, df, &child);  /* Field type. */
      cctinfo = ctype_info_acq(cct);
      cctsize = ctype_size_acq(cct);
      tp = crec_ct2irt_snapshot(J, cts, cct);
      if (tp == IRT_CDATA) return 0;  /* NYI: aggregates. */
      if (mlp >= CREC_COPY_MAXUNROLL) return 0;
      ml[mlp].ofs = dfsize;
      ml[mlp].tp = tp;
      mlp++;
      if (ctype_iscomplex(cctinfo)) {
	if (mlp >= CREC_COPY_MAXUNROLL) return 0;
	ml[mlp].ofs = dfsize + (cctsize >> 1);
	ml[mlp].tp = tp;
	mlp++;
      }
    } else if (!ctype_isconstval(dfinfo)) {
      /* NYI: bitfields and sub-structures. */
      return 0;
    }
  }
  return mlp;
}

/* Generate unrolled copy list, from highest to lowest step size/alignment. */
static MSize crec_copy_unroll(CRecMemList *ml, CTSize len, CTSize step,
			      IRType tp)
{
  CTSize ofs = 0;
  MSize mlp = 0;
  if (tp == IRT_CDATA) tp = IRT_U8 + 2*lj_fls(step);
  do {
    while (ofs + step <= len) {
      if (mlp >= CREC_COPY_MAXUNROLL) return 0;
      ml[mlp].ofs = ofs;
      ml[mlp].tp = tp;
      mlp++;
      ofs += step;
    }
    step >>= 1;
    tp -= 2;
  } while (ofs < len);
  return mlp;
}

static int crec_const_len(jit_State *J, TRef trlen)
{
  if (!tref_isk(trlen))
    return 0;
  return IR(tref_ref(trlen))->i >= 0;
}

static int crec_nonneg_const_or_dynamic_len(jit_State *J, TRef trlen)
{
  return !tref_isk(trlen) || crec_const_len(J, trlen);
}

/*
** Emit copy list with windowed loads/stores.
** LJ_TARGET_UNALIGNED: may emit unaligned loads/stores (not marked as such).
*/
static void crec_copy_emit(jit_State *J, CRecMemList *ml, MSize mlp,
			   TRef trdst, TRef trsrc)
{
  MSize i, j, rwin = 0;
  for (i = 0, j = 0; i < mlp; ) {
    TRef trofs = lj_ir_kintp(J, ml[i].ofs);
    TRef trsptr = emitir(IRT(IR_ADD, IRT_PTR), trsrc, trofs);
    ml[i].trval = emitir(IRT(IR_XLOAD, ml[i].tp), trsptr, 0);
    ml[i].trofs = trofs;
    i++;
    rwin += (LJ_SOFTFP32 && ml[i].tp == IRT_NUM) ? 2 : 1;
    if (rwin >= CREC_COPY_REGWIN || i >= mlp) {  /* Flush buffered stores. */
      rwin = 0;
      for ( ; j < i; j++) {
	TRef trdptr = emitir(IRT(IR_ADD, IRT_PTR), trdst, ml[j].trofs);
	emitir(IRT(IR_XSTORE, ml[j].tp), trdptr, ml[j].trval);
      }
    }
  }
}

/* Optimized memory copy. */
static void crec_copy(jit_State *J, TRef trdst, TRef trsrc, TRef trlen,
		      CType *ct, int allow_memcall)
{
  if (tref_isk(trlen)) {  /* Length must be constant. */
    CRecMemList ml[CREC_COPY_MAXUNROLL];
    MSize mlp = 0;
    CTSize step = 1, len = (CTSize)IR(tref_ref(trlen))->i;
    IRType tp = IRT_CDATA;
    int needxbar = 0;
    if (len == 0) return;  /* Shortcut. */
    if (len > CREC_COPY_MAXLEN) goto fallback;
    if (ct) {
      CTState *cts = ctype_ctsG(J2G(J));
      CTInfo info = ctype_info_acq(ct);
      lj_assertJ(ctype_isarray(info) || ctype_isstruct(info),
		 "copy of non-aggregate");
      if (ctype_isarray(info)) {
	CType child, *cct = crec_ctype_rawchild(J, cts, ct, &child);
	tp = crec_ct2irt_snapshot(J, cts, cct);
	if (tp == IRT_CDATA) goto rawcopy;
	step = lj_ir_type_size[tp];
	lj_assertJ((len & (step-1)) == 0, "copy of fractional size");
      } else if ((info & CTF_UNION)) {
	step = (1u << ctype_align(info));
	goto rawcopy;
      } else {
	mlp = crec_copy_struct(J, ml, cts, ct);
	goto emitcopy;
      }
    } else {
    rawcopy:
      needxbar = 1;
      if (LJ_TARGET_UNALIGNED || step >= CTSIZE_PTR)
	step = CTSIZE_PTR;
    }
    mlp = crec_copy_unroll(ml, len, step, tp);
  emitcopy:
    if (mlp) {
      crec_copy_emit(J, ml, mlp, trdst, trsrc);
      if (needxbar)
	emitir(IRT(IR_XBAR, IRT_NIL), 0, 0);
      return;
    }
  }
fallback:
  if (!allow_memcall)
    lj_trace_err(J, LJ_TRERR_NYICALL);
  /* Call memcpy through the native-state bridge. */
  lj_ir_call(J, IRCALL_lj_ffi_jit_memcpy, trdst, trsrc, trlen);
  emitir(IRT(IR_XBAR, IRT_NIL), 0, 0);
}

/* Generate unrolled fill list, from highest to lowest step size/alignment. */
static MSize crec_fill_unroll(CRecMemList *ml, CTSize len, CTSize step)
{
  CTSize ofs = 0;
  MSize mlp = 0;
  IRType tp = IRT_U8 + 2*lj_fls(step);
  do {
    while (ofs + step <= len) {
      if (mlp >= CREC_COPY_MAXUNROLL) return 0;
      ml[mlp].ofs = ofs;
      ml[mlp].tp = tp;
      mlp++;
      ofs += step;
    }
    step >>= 1;
    tp -= 2;
  } while (ofs < len);
  return mlp;
}

/*
** Emit stores for fill list.
** LJ_TARGET_UNALIGNED: may emit unaligned stores (not marked as such).
*/
static void crec_fill_emit(jit_State *J, CRecMemList *ml, MSize mlp,
			   TRef trdst, TRef trfill)
{
  MSize i;
  for (i = 0; i < mlp; i++) {
    TRef trofs = lj_ir_kintp(J, ml[i].ofs);
    TRef trdptr = emitir(IRT(IR_ADD, IRT_PTR), trdst, trofs);
    emitir(IRT(IR_XSTORE, ml[i].tp), trdptr, trfill);
  }
}

/* Optimized memory fill. */
static void crec_fill(jit_State *J, TRef trdst, TRef trlen, TRef trfill,
		      CTSize step, int allow_memcall)
{
  if (tref_isk(trlen)) {  /* Length must be constant. */
    CRecMemList ml[CREC_FILL_MAXUNROLL];
    MSize mlp;
    CTSize len = (CTSize)IR(tref_ref(trlen))->i;
    if (len == 0) return;  /* Shortcut. */
    if (LJ_TARGET_UNALIGNED || step >= CTSIZE_PTR)
      step = CTSIZE_PTR;
    if (step * CREC_FILL_MAXUNROLL < len) goto fallback;
    mlp = crec_fill_unroll(ml, len, step);
    if (!mlp) goto fallback;
    if (tref_isk(trfill) || ml[0].tp != IRT_U8)
      trfill = emitconv(trfill, IRT_INT, IRT_U8, 0);
    if (ml[0].tp != IRT_U8) {  /* Scatter U8 to U16/U32/U64. */
      if (CTSIZE_PTR == 8 && ml[0].tp == IRT_U64) {
	if (tref_isk(trfill))  /* Pointless on x64 with zero-extended regs. */
	  trfill = emitconv(trfill, IRT_U64, IRT_U32, 0);
	trfill = emitir(IRT(IR_MUL, IRT_U64), trfill,
			lj_ir_kint64(J, U64x(01010101,01010101)));
      } else {
	trfill = emitir(IRTI(IR_MUL), trfill,
		   lj_ir_kint(J, ml[0].tp == IRT_U16 ? 0x0101 : 0x01010101));
      }
    }
    crec_fill_emit(J, ml, mlp, trdst, trfill);
  } else {
fallback:
    if (!allow_memcall)
      lj_trace_err(J, LJ_TRERR_NYICALL);
    /* Call memset through the native-state bridge. */
    lj_ir_call(J, IRCALL_lj_ffi_jit_memset, trdst, trfill, trlen);
  }
  emitir(IRT(IR_XBAR, IRT_NIL), 0, 0);
}

/* -- Convert C type to C type -------------------------------------------- */

/*
** This code mirrors the code in lj_cconv.c. It performs the same steps
** for the trace recorder that lj_cconv.c does for the interpreter.
**
** One major difference is that we can get away with much fewer checks
** here. E.g. checks for casts, constness or correct types can often be
** omitted, even if they might fail. The interpreter subsequently throws
** an error, which aborts the trace.
**
** All operations are specialized to their C types, so the on-trace
** outcome must be the same as the outcome in the interpreter. If the
** interpreter doesn't throw an error, then the trace is correct, too.
** Care must be taken not to generate invalid (temporary) IR or to
** trigger asserts.
*/

/* Determine whether a passed number or cdata number is non-zero. */
static int crec_isnonzero(CType *s, void *p)
{
  CTInfo sinfo;
  CTSize ssize;
  if (p == (void *)0)
    return 0;
  if (p == (void *)1)
    return 1;
  sinfo = ctype_info_acq(s);
  ssize = ctype_size_acq(s);
  if ((sinfo & CTF_FP)) {
    if (ssize == sizeof(float))
      return (*(float *)p != 0);
    else
      return (*(double *)p != 0);
  } else {
    if (ssize == 1)
      return (*(uint8_t *)p != 0);
    else if (ssize == 2)
      return (*(uint16_t *)p != 0);
    else if (ssize == 4)
      return (*(uint32_t *)p != 0);
    else
      return (*(uint64_t *)p != 0);
  }
}

static TRef crec_ct_ct(jit_State *J, CType *d, CType *s, TRef dp, TRef sp,
		       void *svisnz)
{
  IRType dt = crec_ct2irt(ctype_ctsG(J2G(J)), d);
  IRType st = crec_ct2irt(ctype_ctsG(J2G(J)), s);
  CTSize dsize = ctype_size_acq(d), ssize = ctype_size_acq(s);
  CTInfo dinfo = ctype_info_acq(d), sinfo = ctype_info_acq(s);

  if (ctype_type(dinfo) > CT_MAYCONVERT || ctype_type(sinfo) > CT_MAYCONVERT)
    goto err_conv;

  /*
  ** Note: Unlike lj_cconv_ct_ct(), sp holds the _value_ of pointers and
  ** numbers up to 8 bytes. Otherwise sp holds a pointer.
  */

  switch (cconv_idx2(dinfo, sinfo)) {
  /* Destination is a bool. */
  case CCX(B, B):
    goto xstore;  /* Source operand is already normalized. */
  case CCX(B, I):
  case CCX(B, F):
    if (st != IRT_CDATA) {
      /* Specialize to the result of a comparison against 0. */
      TRef zero = (st == IRT_NUM  || st == IRT_FLOAT) ? lj_ir_knum(J, 0) :
		  (st == IRT_I64 || st == IRT_U64) ? lj_ir_kint64(J, 0) :
		  lj_ir_kint(J, 0);
      int isnz = crec_isnonzero(s, svisnz);
      emitir(IRTG(isnz ? IR_NE : IR_EQ, st), sp, zero);
      sp = lj_ir_kint(J, isnz);
      goto xstore;
    }
    goto err_nyi;

  /* Destination is an integer. */
  case CCX(I, B):
  case CCX(I, I):
  conv_I_I:
    if (dt == IRT_CDATA || st == IRT_CDATA) goto err_nyi;
    /* Extend 32 to 64 bit integer. */
    if (dsize == 8 && ssize < 8 && !(LJ_64 && (sinfo & CTF_UNSIGNED)))
      sp = emitconv(sp, dt, ssize < 4 ? IRT_INT : st,
		    (sinfo & CTF_UNSIGNED) ? 0 : IRCONV_SEXT);
    else if (dsize < 8 && ssize == 8)  /* Truncate from 64 bit integer. */
      sp = emitconv(sp, dsize < 4 ? IRT_INT : dt, st, 0);
    else if (st == IRT_INT)
      sp = lj_opt_narrow_toint(J, sp);
  xstore:
    if (dt == IRT_I64 || dt == IRT_U64) lj_needsplit(J);
    if (dp == 0) return sp;
    emitir(IRT(IR_XSTORE, dt), dp, sp);
    break;
  case CCX(I, C):
    sp = emitir(IRT(IR_XLOAD, st), sp, 0);  /* Load re. */
    /* fallthrough */
  case CCX(I, F):
    if (dt == IRT_CDATA || st == IRT_CDATA) goto err_nyi;
  conv_I_F:
#if LJ_SOFTFP || LJ_32
    if (st == IRT_FLOAT) {  /* Uncommon. Simplify split backends. */
      sp = emitconv(sp, IRT_NUM, IRT_FLOAT, 0);
      st = IRT_NUM;
    }
#endif
    if (dsize < 8) {
      lj_needsplit(J);
      sp = emitconv(sp, IRT_I64, st, IRCONV_ANY);
      sp = emitconv(sp, dsize < 4 ? IRT_INT : dt, IRT_I64, 0);
    } else {
      sp = emitconv(sp, dt, st, IRCONV_ANY);
    }
    goto xstore;
  case CCX(I, P):
  case CCX(I, A):
    sinfo = CTINFO(CT_NUM, CTF_UNSIGNED);
    ssize = CTSIZE_PTR;
    st = IRT_UINTP;
    if (((dsize ^ ssize) & 8) == 0) {  /* Must insert no-op type conversion. */
      sp = emitconv(sp, dsize < 4 ? IRT_INT : dt, IRT_PTR, 0);
      goto xstore;
    }
    goto conv_I_I;

  /* Destination is a floating-point number. */
  case CCX(F, B):
  case CCX(F, I):
  conv_F_I:
    if (dt == IRT_CDATA || st == IRT_CDATA) goto err_nyi;
    sp = emitconv(sp, dt, ssize < 4 ? IRT_INT : st, 0);
    goto xstore;
  case CCX(F, C):
    sp = emitir(IRT(IR_XLOAD, st), sp, 0);  /* Load re. */
    /* fallthrough */
  case CCX(F, F):
  conv_F_F:
    if (dt == IRT_CDATA || st == IRT_CDATA) goto err_nyi;
    if (dt != st) sp = emitconv(sp, dt, st, 0);
    goto xstore;

  /* Destination is a complex number. */
  case CCX(C, I):
  case CCX(C, F):
    {  /* Clear im. */
      TRef ptr = emitir(IRT(IR_ADD, IRT_PTR), dp, lj_ir_kintp(J, (dsize >> 1)));
      emitir(IRT(IR_XSTORE, dt), ptr, lj_ir_knum(J, 0));
    }
    /* Convert to re. */
    if ((sinfo & CTF_FP)) goto conv_F_F; else goto conv_F_I;

  case CCX(C, C):
    if (dt == IRT_CDATA || st == IRT_CDATA) goto err_nyi;
    {
      TRef re, im, ptr;
      re = emitir(IRT(IR_XLOAD, st), sp, 0);
      ptr = emitir(IRT(IR_ADD, IRT_PTR), sp, lj_ir_kintp(J, (ssize >> 1)));
      im = emitir(IRT(IR_XLOAD, st), ptr, 0);
      if (dt != st) {
	re = emitconv(re, dt, st, 0);
	im = emitconv(im, dt, st, 0);
      }
      emitir(IRT(IR_XSTORE, dt), dp, re);
      ptr = emitir(IRT(IR_ADD, IRT_PTR), dp, lj_ir_kintp(J, (dsize >> 1)));
      emitir(IRT(IR_XSTORE, dt), ptr, im);
    }
    break;

  /* Destination is a vector. */
  case CCX(V, I):
  case CCX(V, F):
  case CCX(V, C):
  case CCX(V, V):
    goto err_nyi;

  /* Destination is a pointer. */
  case CCX(P, P):
  case CCX(P, A):
  case CCX(P, S):
    /* There are only 32 bit pointers/addresses on 32 bit machines.
    ** Also ok on x64, since all 32 bit ops clear the upper part of the reg.
    */
    goto xstore;
  case CCX(P, I):
    if (st == IRT_CDATA) goto err_nyi;
    if (!LJ_64 && ssize == 8)  /* Truncate from 64 bit integer. */
      sp = emitconv(sp, IRT_U32, st, 0);
    goto xstore;
  case CCX(P, F):
    if (st == IRT_CDATA) goto err_nyi;
    /* The signed 64 bit conversion is cheaper. */
    dt = (LJ_64 && dsize == 8) ? IRT_I64 : IRT_U32;
    goto conv_I_F;

  /* Destination is an array. */
  case CCX(A, A):
  /* Destination is a struct/union. */
  case CCX(S, S):
    if (dp == 0) goto err_conv;
    crec_copy(J, dp, sp, lj_ir_kint(J, dsize), d, 1);
    break;

  default:
  err_conv:
  err_nyi:
    lj_trace_err(J, LJ_TRERR_NYICONV);
    break;
  }
  return 0;
}

/* -- Convert C type to TValue (load) ------------------------------------- */

static TRef crec_tv_ct(jit_State *J, CType *s, CTypeID sid, TRef sp)
{
  CTState *cts = ctype_ctsG(J2G(J));
  IRType t = crec_ct2irt_snapshot(J, cts, s);
  CTInfo sinfo = ctype_info_acq(s);
  CTSize ssize = ctype_size_acq(s);
  if (ctype_isnum(sinfo)) {
    TRef tr;
    if (t == IRT_CDATA)
      goto err_nyi;  /* NYI: copyval of >64 bit integers. */
    tr = emitir(IRT(IR_XLOAD, t), sp, 0);
    if (t == IRT_FLOAT || t == IRT_U32) {  /* Keep uint32_t/float as numbers. */
      return emitconv(tr, IRT_NUM, t, 0);
    } else if (t == IRT_I64 || t == IRT_U64) {  /* Box 64 bit integer. */
      sp = tr;
      lj_needsplit(J);
    } else if ((sinfo & CTF_BOOL)) {
      /* Assume not equal to zero. Fixup and emit pending guard later. */
      lj_ir_set(J, IRTGI(IR_NE), tr, lj_ir_kint(J, 0));
      J->postproc = LJ_POST_FIXGUARD;
      return TREF_TRUE;
    } else {
      return tr;
    }
  } else if (ctype_isptr(sinfo) || ctype_isenum(sinfo)) {
    sp = emitir(IRT(IR_XLOAD, t), sp, 0);  /* Box pointers and enums. */
  } else if (ctype_isrefarray(sinfo) || ctype_isstruct(sinfo)) {
    sid = lj_ctype_intern_l(J->L, cts, CTINFO_REF(sid), CTSIZE_PTR);
  } else if (ctype_iscomplex(sinfo)) {  /* Unbox/box complex. */
    ptrdiff_t esz = (ptrdiff_t)(ssize >> 1);
    TRef ptr, tr1, tr2, dp;
    dp = emitir(IRTG(IR_CNEW, IRT_CDATA), lj_ir_kint(J, sid), TREF_NIL);
    tr1 = emitir(IRT(IR_XLOAD, t), sp, 0);
    ptr = emitir(IRT(IR_ADD, IRT_PTR), sp, lj_ir_kintp(J, esz));
    tr2 = emitir(IRT(IR_XLOAD, t), ptr, 0);
    ptr = emitir(IRT(IR_ADD, IRT_PTR), dp, lj_ir_kintp(J, sizeof(GCcdata)));
    emitir(IRT(IR_XSTORE, t), ptr, tr1);
    ptr = emitir(IRT(IR_ADD, IRT_PTR), dp, lj_ir_kintp(J, sizeof(GCcdata)+esz));
    emitir(IRT(IR_XSTORE, t), ptr, tr2);
    return dp;
  } else {
    /* NYI: copyval of vectors. */
  err_nyi:
    lj_trace_err(J, LJ_TRERR_NYICONV);
  }
  /* Box pointer, ref, enum or 64 bit integer. */
  return emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, sid), sp);
}

/* -- Convert TValue to C type (store) ------------------------------------ */

static TRef crec_ct_tv(jit_State *J, CType *d, TRef dp, TRef sp, cTValue *sval)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTInfo dinfo = ctype_info_acq(d);
  CType scopy, dcopy;
  CTypeID sid = CTID_P_VOID;
  void *svisnz = 0;
  CType *s;
  if (LJ_LIKELY(tref_isinteger(sp))) {
    sid = CTID_INT32;
    svisnz = (void *)(intptr_t)(tvisint(sval)?(intV(sval)!=0):!tviszero(sval));
  } else if (tref_isnum(sp)) {
    sid = CTID_DOUBLE;
    svisnz = (void *)(intptr_t)(tvisint(sval)?(intV(sval)!=0):!tviszero(sval));
  } else if (tref_isbool(sp)) {
    sp = lj_ir_kint(J, tref_istrue(sp) ? 1 : 0);
    sid = CTID_BOOL;
  } else if (tref_isnil(sp)) {
    sp = lj_ir_kptr(J, NULL);
  } else if (tref_isudata(sp)) {
    GCudata *ud = udataV(sval);
    uint8_t udtype = lj_udata_udtype_acq(ud);
    if (udtype == UDTYPE_IO_FILE || udtype == UDTYPE_BUFFER) {
      TRef tr = emitir(IRT(IR_FLOAD, IRT_U8), sp, IRFL_UDATA_UDTYPE);
      emitir(IRTGI(IR_EQ), tr, lj_ir_kint(J, udtype));
      sp = emitir(IRT(IR_FLOAD, IRT_PTR), sp,
		  udtype == UDTYPE_IO_FILE ? IRFL_UDATA_FILE : IRFL_SBUF_R);
    } else {
      sp = emitir(IRT(IR_ADD, IRT_PTR), sp, lj_ir_kintp(J, sizeof(GCudata)));
    }
  } else if (tref_isstr(sp)) {
    if (ctype_isenum(dinfo)) {  /* Match string against enum constant. */
      GCstr *str = strV(sval);
      CTSize val;
      CTypeID ecid;
      int ok = lj_ctype_enumconst_snapshot(cts, d, str, &val, &ecid);
      if (ok < 0) {
	lj_trace_err(J, LJ_TRERR_CTBUSY);
      }
      /* Specialize to the name of the enum constant. */
      emitir(IRTG(IR_EQ, IRT_STR), sp, lj_ir_kstr(J, str));
      if (ok > 0) {
	sid = ecid;
	s = crec_ctype_snapshot(J, cts, sid, &scopy);
	lj_assertJ(ctype_size_acq(s) == 4,
		   "only 32 bit const supported");  /* NYI */
	svisnz = (void *)(intptr_t)(val != 0);
	sp = lj_ir_kint(J, (int32_t)val);
      }  /* else: interpreter will throw. */
    } else if (ctype_isrefarray(dinfo)) {  /* Copy string to array. */
      lj_trace_err(J, LJ_TRERR_BADTYPE);  /* NYI */
    } else {  /* Otherwise pass the string data as a const char[]. */
      /* Don't use STRREF. It folds with SNEW, which loses the trailing NUL. */
      sp = emitir(IRT(IR_ADD, IRT_PTR), sp, lj_ir_kintp(J, sizeof(GCstr)));
      sid = CTID_A_CCHAR;
    }
  } else if (tref_islightud(sp)) {
#if LJ_64
    lj_trace_err(J, LJ_TRERR_NYICONV);
#endif
  } else {  /* NYI: tref_istab(sp). */
    IRType t;
    CTInfo sinfo;
    sid = argv2cdata(J, sp, sval)->ctypeid;
    s = crec_ctype_rawrefid(J, cts, sid, &sid, &scopy);
    sinfo = ctype_info_acq(s);
    svisnz = cdataptr(cdataV(sval));
    if (ctype_isfunc(sinfo)) {
      sid = lj_ctype_intern_l(J->L, cts, CTINFO(CT_PTR, CTALIGN_PTR|sid),
			      CTSIZE_PTR);
      s = crec_ctype_snapshot(J, cts, sid, &scopy);
      sinfo = ctype_info_acq(s);
      t = IRT_PTR;
    } else {
      t = crec_ct2irt_snapshot(J, cts, s);
    }
    if (ctype_isptr(sinfo)) {
      sp = emitir(IRT(IR_FLOAD, t), sp, IRFL_CDATA_PTR);
      if (ctype_isref(sinfo)) {
	svisnz = *(void **)svisnz;
	s = crec_ctype_rawchild(J, cts, s, &scopy);
	sinfo = ctype_info_acq(s);
	if (ctype_isenum(sinfo)) {
	  s = crec_ctype_snapshot(J, cts, ctype_cid(sinfo), &scopy);
	  sinfo = ctype_info_acq(s);
	}
	t = crec_ct2irt_snapshot(J, cts, s);
      } else {
	goto doconv;
      }
    } else if (t == IRT_I64 || t == IRT_U64) {
      sp = emitir(IRT(IR_FLOAD, t), sp, IRFL_CDATA_INT64);
      lj_needsplit(J);
      goto doconv;
    } else if (t == IRT_INT || t == IRT_U32) {
      if (ctype_isenum(sinfo)) {
	s = crec_ctype_snapshot(J, cts, ctype_cid(sinfo), &scopy);
	sinfo = ctype_info_acq(s);
      }
      sp = emitir(IRT(IR_FLOAD, t), sp, IRFL_CDATA_INT);
      goto doconv;
    } else {
      sp = emitir(IRT(IR_ADD, IRT_PTR), sp, lj_ir_kintp(J, sizeof(GCcdata)));
    }
    if (ctype_isnum(sinfo) && t != IRT_CDATA)
      sp = emitir(IRT(IR_XLOAD, t), sp, 0);  /* Load number value. */
    goto doconv;
  }
  s = crec_ctype_rawid(J, cts, sid, NULL, &scopy);
doconv:
  if (ctype_isenum(dinfo))
    d = crec_ctype_snapshot(J, cts, ctype_cid(dinfo), &dcopy);
  return crec_ct_ct(J, d, s, dp, sp, svisnz);
}

static TRef crec_ct_tv_id(jit_State *J, CTState *cts, CTypeID did,
			  TRef dp, TRef sp, cTValue *sval)
{
  CType snap;
  CType *d = crec_ctype_rawid(J, cts, did, NULL, &snap);
  return crec_ct_tv(J, d, dp, sp, sval);
}

/* -- C data metamethods -------------------------------------------------- */

/* This would be rather difficult in FOLD, so do it here:
** (base+k)+(idx*sz)+ofs ==> (base+idx*sz)+(ofs+k)
** (base+(idx+k)*sz)+ofs ==> (base+idx*sz)+(ofs+k*sz)
*/
static TRef crec_reassoc_ofs(jit_State *J, TRef tr, ptrdiff_t *ofsp, MSize sz)
{
  IRIns *ir = IR(tref_ref(tr));
  if (LJ_LIKELY(jit_flags_acq(J) & JIT_F_OPT_FOLD) && irref_isk(ir->op2) &&
      (ir->o == IR_ADD || ir->o == IR_ADDOV || ir->o == IR_SUBOV)) {
    IRIns *irk = IR(ir->op2);
    ptrdiff_t k;
    if (LJ_64 && irk->o == IR_KINT64)
      k = (ptrdiff_t)ir_kint64(irk)->u64 * sz;
    else
      k = (ptrdiff_t)irk->i * sz;
    if (ir->o == IR_SUBOV) *ofsp -= k; else *ofsp += k;
    tr = ir->op1;  /* Not a TRef, but the caller doesn't care. */
  }
  return tr;
}

/* Tailcall to function. */
static void crec_tailcall(jit_State *J, RecordFFData *rd, cTValue *tv)
{
  TRef kfunc = lj_ir_kfunc(J, funcV(tv));
#if LJ_FR2
  J->base[-2] = kfunc;
  J->base[-1] = TREF_FRAME;
#else
  J->base[-1] = kfunc | TREF_FRAME;
#endif
  rd->nres = -1;  /* Pending tailcall. */
}

static cTValue *crec_ctype_metatv(jit_State *J, CTState *cts, TValue *out,
				  CTypeID id, MMS mm)
{
  int ok;
  if (lj_ctype_predefined_nometa(cts, id)) {
    setnilV(out);
    return NULL;
  }
  ok = lj_ctype_metatv_snapshot(cts, out, id, mm);
  if (ok < 0)
    lj_trace_err(J, LJ_TRERR_CTBUSY);
  return ok ? out : NULL;
}

static CTypeID crec_ctype_ptr_metaid(jit_State *J, CTState *cts, CTypeID id)
{
  CType snap;
  CTypeID rid;
  CTInfo info;
  CTSize size;
  int ok;
  if (lj_ctype_predefined_nometa(cts, id))
    return id;
  ok = lj_ctype_info_snapshot(cts, id, &info, &size, &rid, &snap);
  if (ok < 0)
    lj_trace_err(J, LJ_TRERR_CTBUSY);
  if (!ok)
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  info = ctype_info_acq(&snap);
  return ctype_isptr(info) ? ctype_cid(info) : id;
}

/* Record ctype __index/__newindex metamethods. */
static void crec_index_meta(jit_State *J, CTState *cts, CTypeID id,
			    RecordFFData *rd)
{
  TValue metatv;
  cTValue *tv = crec_ctype_metatv(J, cts, &metatv, id,
				  rd->data ? MM_newindex : MM_index);
  if (!tv)
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  if (tvisfunc(tv)) {
    crec_tailcall(J, rd, tv);
  } else if (rd->data == 0 && tvistab(tv) && tref_isstr(J->base[1])) {
    /* Specialize to result of __index lookup. */
    cTValue *o = lj_tab_get(J->L, tabV(tv), &rd->argv[1]);
    J->base[0] = lj_record_constify(J, o);
    if (!J->base[0])
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    /* Always specialize to the key. */
    emitir(IRTG(IR_EQ, IRT_STR), J->base[1], lj_ir_kstr(J, strV(&rd->argv[1])));
  } else {
    /* NYI: resolving of non-function metamethods. */
    /* NYI: non-string keys for __index table. */
    /* NYI: stores to __newindex table. */
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  }
}

/* Record bitfield load/store. */
static void crec_index_bf(jit_State *J, RecordFFData *rd, TRef ptr, CTInfo info)
{
  IRType t = IRT_I8 + 2*lj_fls(ctype_bitcsz(info)) + ((info&CTF_UNSIGNED)?1:0);
  TRef tr = emitir(IRT(IR_XLOAD, t), ptr, 0);
  CTSize pos = ctype_bitpos(info), bsz = ctype_bitbsz(info), shift = 32 - bsz;
  lj_assertJ(t <= IRT_U32, "only 32 bit bitfields supported");  /* NYI */
  if (rd->data == 0) {  /* __index metamethod. */
    if ((info & CTF_BOOL)) {
      tr = emitir(IRTI(IR_BAND), tr, lj_ir_kint(J, (int32_t)((1u << pos))));
      /* Assume not equal to zero. Fixup and emit pending guard later. */
      lj_ir_set(J, IRTGI(IR_NE), tr, lj_ir_kint(J, 0));
      J->postproc = LJ_POST_FIXGUARD;
      tr = TREF_TRUE;
    } else if (!(info & CTF_UNSIGNED)) {
      tr = emitir(IRTI(IR_BSHL), tr, lj_ir_kint(J, shift - pos));
      tr = emitir(IRTI(IR_BSAR), tr, lj_ir_kint(J, shift));
    } else {
      lj_assertJ(bsz < 32, "unexpected full bitfield index");
      tr = emitir(IRTI(IR_BSHR), tr, lj_ir_kint(J, pos));
      tr = emitir(IRTI(IR_BAND), tr, lj_ir_kint(J, (int32_t)((1u << bsz)-1)));
      /* We can omit the U32 to NUM conversion, since bsz < 32. */
    }
    J->base[0] = tr;
  } else {  /* __newindex metamethod. */
    CTState *cts = ctype_ctsG(J2G(J));
    CType ctsnap, *ct = crec_ctype_rawid(J, cts,
			  (info & CTF_BOOL) ? CTID_BOOL :
			  (info & CTF_UNSIGNED) ? CTID_UINT32 : CTID_INT32,
			  NULL, &ctsnap);
    int32_t mask = (int32_t)(((1u << bsz)-1) << pos);
    TRef sp = crec_ct_tv(J, ct, 0, J->base[2], &rd->argv[2]);
    sp = emitir(IRTI(IR_BSHL), sp, lj_ir_kint(J, pos));
    /* Use of the target type avoids forwarding conversions. */
    sp = emitir(IRT(IR_BAND, t), sp, lj_ir_kint(J, mask));
    tr = emitir(IRT(IR_BAND, t), tr, lj_ir_kint(J, (int32_t)~mask));
    tr = emitir(IRT(IR_BOR, t), tr, sp);
    emitir(IRT(IR_XSTORE, t), ptr, tr);
    rd->nres = 0;
    J->needsnap = 1;
  }
}

void LJ_FASTCALL recff_cdata_index(jit_State *J, RecordFFData *rd)
{
  TRef idx, ptr = J->base[0];
  ptrdiff_t ofs = sizeof(GCcdata);
  GCcdata *cd = argv2cdata(J, ptr, &rd->argv[0]);
  CTState *cts = ctype_ctsG(J2G(J));
  CType ctsnap, childsnap;
  CTypeID id = cd->ctypeid;
  CType *ct = crec_ctype_rawid(J, cts, id, &id, &ctsnap);
  CTInfo ctinfo = ctype_info_acq(ct);
  CTSize ctsize = ctype_size_acq(ct);
  CTypeID sid = 0;

  /* Resolve pointer or reference for cdata object. */
  if (ctype_isptr(ctinfo)) {
    IRType t = (LJ_64 && ctsize == 8) ? IRT_P64 : IRT_P32;
    if (ctype_isref(ctinfo)) {
      ct = crec_ctype_rawchildid(J, cts, ct, &id, &ctsnap);
      ctinfo = ctype_info_acq(ct);
      ctsize = ctype_size_acq(ct);
    }
    ptr = emitir(IRT(IR_FLOAD, t), ptr, IRFL_CDATA_PTR);
    ofs = 0;
    ptr = crec_reassoc_ofs(J, ptr, &ofs, 1);
  }

again:
  idx = J->base[1];
  if (tref_isnumber(idx)) {
    idx = lj_opt_narrow_cindex(J, idx);
    if (ctype_ispointer(ctinfo)) {
      CTSize sz;
      CTInfo pinfo;
  integer_key:
      pinfo = ctype_info_acq(ct);
      sid = ctype_cid(pinfo);
      sz = crec_ctype_size(J, cts, sid);
      if (sz == CTSIZE_INVALID)
	lj_trace_err(J, LJ_TRERR_BADTYPE);
      if ((pinfo & CTF_COMPLEX))
	idx = emitir(IRT(IR_BAND, IRT_INTP), idx, lj_ir_kintp(J, 1));
      idx = crec_reassoc_ofs(J, idx, &ofs, sz);
#if LJ_TARGET_ARM || LJ_TARGET_PPC
      /* Hoist base add to allow fusion of index/shift into operands. */
      if (LJ_LIKELY(jit_flags_acq(J) & JIT_F_OPT_LOOP) && ofs
#if LJ_TARGET_ARM
	  && (sz == 1 || sz == 4)
#endif
	  ) {
	ptr = emitir(IRT(IR_ADD, IRT_PTR), ptr, lj_ir_kintp(J, ofs));
	ofs = 0;
      }
#endif
      idx = emitir(IRT(IR_MUL, IRT_INTP), idx, lj_ir_kintp(J, sz));
      ptr = emitir(IRT(IR_ADD, IRT_PTR), idx, ptr);
    }
  } else if (tref_iscdata(idx)) {
    GCcdata *cdk = cdataV(&rd->argv[1]);
    CType cksnap, *ctk = crec_ctype_rawid(J, cts, cdk->ctypeid,
					  NULL, &cksnap);
    IRType t = crec_ct2irt_snapshot(J, cts, ctk);
    CTInfo ctkinfo = ctype_info_acq(ctk);
    CTSize ctksize = ctype_size_acq(ctk);
    if (ctype_ispointer(ctype_info_acq(ct)) && t >= IRT_I8 && t <= IRT_U64) {
      if (ctksize == 8) {
	idx = emitir(IRT(IR_FLOAD, t), idx, IRFL_CDATA_INT64);
      } else if (ctksize == 4) {
	idx = emitir(IRT(IR_FLOAD, t), idx, IRFL_CDATA_INT);
      } else {
	idx = emitir(IRT(IR_ADD, IRT_PTR), idx,
		     lj_ir_kintp(J, sizeof(GCcdata)));
	idx = emitir(IRT(IR_XLOAD, t), idx, 0);
      }
      if (LJ_64 && ctksize < sizeof(intptr_t) && !(ctkinfo & CTF_UNSIGNED))
	idx = emitconv(idx, IRT_INTP, IRT_INT, IRCONV_SEXT);
      if (!LJ_64 && ctksize > sizeof(intptr_t)) {
	idx = emitconv(idx, IRT_INTP, t, 0);
	lj_needsplit(J);
      }
      goto integer_key;
    }
  } else if (tref_isstr(idx)) {
    GCstr *name = strV(&rd->argv[1]);
    CTInfo cinfo, finfo = 0, childinfo = 0;
    CTSize csize, fofs = 0, fsize = 0;
    CTypeID fid = 0;
    int found = 0;
    CType fsnap;
    if (cd && cd->ctypeid == CTID_CTYPEID) {
      id = crec_constructor(J, cd, ptr);
    }
    ct = crec_ctype_rawid(J, cts, id, &id, &ctsnap);
    cinfo = ctype_info_acq(ct);
    csize = ctype_size_acq(ct);
    if (ctype_isstruct(cinfo)) {
      CType *fct;
      int ok = lj_ctype_getfieldq_snapshot(cts, ct, name, &fofs, NULL,
					   &fsnap);
      if (ok < 0) {
	lj_trace_err(J, LJ_TRERR_CTBUSY);
      } else {
	fct = ok ? &fsnap : NULL;
      }
      found = fct != NULL;
      if (found) {
	finfo = ctype_info_acq(fct);
	fsize = ctype_size_acq(fct);
	fid = ctype_cid(finfo);
	if (ctype_isconstval(finfo)) {
	  CType fchild;
	  childinfo = ctype_info_acq(crec_ctype_rawid(J, cts, fid, NULL,
						      &fchild));
	}
      }
    }
    if (ctype_isstruct(cinfo)) {
      if (found) {
	ofs += (ptrdiff_t)fofs;
	/* Always specialize to the field name. */
	emitir(IRTG(IR_EQ, IRT_STR), idx, lj_ir_kstr(J, name));
	if (ctype_isconstval(finfo)) {
	  if (fsize >= 0x80000000u && (childinfo & CTF_UNSIGNED)) {
	    J->base[0] = lj_ir_knum(J, (lua_Number)(uint32_t)fsize);
	    return;
	  }
	  J->base[0] = lj_ir_kint(J, (int32_t)fsize);
	  return;  /* Interpreter will throw for newindex. */
	} else if (cd && cd->ctypeid == CTID_CTYPEID) {
	  /* Only resolve constants and metamethods for constructors. */
	} else if (ctype_isbitfield(finfo)) {
	  if (ofs)
	    ptr = emitir(IRT(IR_ADD, IRT_PTR), ptr, lj_ir_kintp(J, ofs));
	  crec_index_bf(J, rd, ptr, finfo);
	  return;
	} else {
	  lj_assertJ(ctype_isfield(finfo), "field expected");
	  sid = fid;
	}
      }
    } else if (ctype_iscomplex(cinfo)) {
      if (name->len == 2 &&
	  ((strdata(name)[0] == 'r' && strdata(name)[1] == 'e') ||
	   (strdata(name)[0] == 'i' && strdata(name)[1] == 'm'))) {
	/* Always specialize to the field name. */
	emitir(IRTG(IR_EQ, IRT_STR), idx, lj_ir_kstr(J, name));
	if (strdata(name)[0] == 'i') ofs += (csize >> 1);
	sid = ctype_cid(cinfo);
      }
    }
  }
  if (!sid) {
    if (tref_isstr(idx)) {
      CTypeID cid = 0;
      int ptrstruct = 0;
      int ok = lj_ctype_ptrstruct_snapshot(cts, id, &cid);
      if (ok < 0) {
	lj_trace_err(J, LJ_TRERR_CTBUSY);
      } else {
	ptrstruct = ok > 0;
      }
      if (ptrstruct) {
	id = cid;
	cd = NULL;
	goto again;
      }
    } else if (ctype_isptr(ctype_info_acq(ct))) {  /* Automatically perform '->'. */
      CTypeID cid = 0;
      CType *cct = crec_ctype_rawchildid(J, cts, ct, &cid, &childsnap);
      if (ctype_isstruct(ctype_info_acq(cct))) {
	ct = cct;
	id = cid;
      }
    }
    crec_index_meta(J, cts, id, rd);
    return;
  }

  if (ofs)
    ptr = emitir(IRT(IR_ADD, IRT_PTR), ptr, lj_ir_kintp(J, ofs));

  /* Resolve reference for field. */
  ct = crec_ctype_rawid(J, cts, sid, &sid, &ctsnap);
  ctinfo = ctype_info_acq(ct);
  if (ctype_isref(ctinfo)) {
    ptr = emitir(IRT(IR_XLOAD, IRT_PTR), ptr, 0);
    ct = crec_ctype_rawchildid(J, cts, ct, &sid, &ctsnap);
    ctinfo = ctype_info_acq(ct);
  }

  while (ctype_isattrib(ctinfo)) {
    sid = ctype_cid(ctinfo);
    ct = crec_ctype_snapshot(J, cts, sid, &ctsnap);  /* Skip attributes. */
    ctinfo = ctype_info_acq(ct);
  }

  if (rd->data == 0) {  /* __index metamethod. */
    J->base[0] = crec_tv_ct(J, ct, sid, ptr);
  } else {  /* __newindex metamethod. */
    rd->nres = 0;
    J->needsnap = 1;
    crec_ct_tv(J, ct, ptr, J->base[2], &rd->argv[2]);
  }
}

/* Record setting a finalizer. */
static void crec_finalizer(jit_State *J, TRef trcd, TRef trfin, cTValue *fin)
{
  TRef trobj;
  if (!(tvisgcv(fin) || tvisnil(fin)))
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  if (tvisnil(fin)) {
    trobj = lj_ir_kptr(J, NULL);
  } else if (trfin) {
    trobj = trfin;
  } else {
    trobj = lj_ir_kgc(J, gcV(fin), itype2irt(fin));
  }
  lj_ir_call(J, IRCALL_lj_cdata_setfin, trcd, trobj,
	     lj_ir_kint(J, (int32_t)itype(fin)));
}

/* Record cdata allocation. */
static void crec_alloc(jit_State *J, RecordFFData *rd, CTypeID id)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTSize sz;
  CTInfo info;
  CType dsnap, *d;
  CTypeID rid;
  TRef trcd, trid = lj_ir_kint(J, id);
  cTValue *fin;
  TValue fintv;
  {
    int ok = lj_ctype_info_predefined(cts, id, &info, &sz, &rid, &dsnap);
    if (ok <= 0) {
      ok = lj_ctype_info_snapshot(cts, id, &info, &sz, &rid, &dsnap);
      if (ok < 0) {
	lj_trace_err(J, LJ_TRERR_CTBUSY);
      } else if (!ok) {
	lj_trace_err(J, LJ_TRERR_BADTYPE);
      }
    }
    d = &dsnap;
  }
  if (sz == CTSIZE_INVALID)
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  /* Use special instruction to box pointer or 32/64 bit integer. */
  if (ctype_isptr(info) || (ctype_isinteger(info) && (sz == 4 || sz == 8))) {
    TRef sp = J->base[1] ? crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]) :
	      ctype_isptr(info) ? lj_ir_kptr(J, NULL) :
	      sz == 4 ? lj_ir_kint(J, 0) :
	      (lj_needsplit(J), lj_ir_kint64(J, 0));
    J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), trid, sp);
    return;
  } else {
    TRef trsz = TREF_NIL;
    if ((info & CTF_VLA)) {  /* Calculate VLA/VLS size at runtime. */
      CTSize sz0, sz1;
      if (!J->base[1] || J->base[2])
	lj_trace_err(J, LJ_TRERR_NYICONV);  /* NYI: init VLA/VLS. */
      trsz = crec_ct_tv_id(J, cts, CTID_INT32, 0, J->base[1],
			   &rd->argv[1]);
      sz0 = lj_ctype_vlsize(cts, d, 0);
      sz1 = lj_ctype_vlsize(cts, d, 1);
      trsz = emitir(IRTGI(IR_MULOV), trsz, lj_ir_kint(J, (int32_t)(sz1-sz0)));
      trsz = emitir(IRTGI(IR_ADDOV), trsz, lj_ir_kint(J, (int32_t)sz0));
      J->base[1] = 0;  /* Simplify logic below. */
    } else if (ctype_align(info) > CT_MEMALIGN) {
      trsz = lj_ir_kint(J, sz);
    }
    trcd = emitir(IRTG(IR_CNEW, IRT_CDATA), trid, trsz);
    if (sz > 128 || (info & CTF_VLA)) {
      TRef dp;
      CTSize align;
    special:  /* Only handle bulk zero-fill for large/VLA/VLS types. */
      if (J->base[1])
	lj_trace_err(J, LJ_TRERR_NYICONV);  /* NYI: init large/VLA/VLS types. */
      dp = emitir(IRT(IR_ADD, IRT_PTR), trcd, lj_ir_kintp(J, sizeof(GCcdata)));
      if (trsz == TREF_NIL) trsz = lj_ir_kint(J, sz);
      align = ctype_align(info);
      if (align < CT_MEMALIGN) align = CT_MEMALIGN;
      crec_fill(J, dp, trsz, lj_ir_kint(J, 0), (1u << align), 1);
    } else if (J->base[1] && !J->base[2] &&
	       !crec_cconv_multi_init(J, cts, rid, d, &rd->argv[1])) {
      goto single_init;
    } else if (ctype_isarray(info)) {
      CType dcsnap, *dc = crec_ctype_rawchild(J, cts, d, &dcsnap);
      CTInfo dcinfo = ctype_info_acq(dc);
      CTSize ofs, esize = ctype_size_acq(dc);
      TRef sp = 0;
      TValue tv;
      TValue *sval = &tv;
      MSize i;
      tv.u64 = 0;
      if (!(ctype_isnum(dcinfo) || ctype_isptr(dcinfo)) ||
	  esize * CREC_FILL_MAXUNROLL < sz)
	goto special;
      for (i = 1, ofs = 0; ofs < sz; ofs += esize) {
	TRef dp = emitir(IRT(IR_ADD, IRT_PTR), trcd,
			 lj_ir_kintp(J, ofs + sizeof(GCcdata)));
	if (J->base[i]) {
	  sp = J->base[i];
	  sval = &rd->argv[i];
	  i++;
	} else if (i != 2) {
	  sp = ctype_isnum(dcinfo) ? lj_ir_kint(J, 0) : TREF_NIL;
	}
	crec_ct_tv(J, dc, dp, sp, sval);
      }
    } else if (ctype_isstruct(info)) {
      CTypeID fid;
      MSize i = 1;
      if (!J->base[1]) {  /* Handle zero-fill of struct-of-NYI. */
	fid = ctype_sib_acq(d);
	while (fid) {
	  CType dfsnap, dcsnap, *df = crec_ctype_snapshot(J, cts, fid, &dfsnap);
	  CTInfo dfinfo = ctype_info_acq(df);
	  fid = ctype_sib_acq(df);
	  if (ctype_isfield(dfinfo)) {
	    CType *dc;
	    CTInfo dcinfo;
	    if (!ctype_name_acq(df)) continue;  /* Ignore unnamed fields. */
	    dc = crec_ctype_rawchild(J, cts, df, &dcsnap);
	    dcinfo = ctype_info_acq(dc);
	    if (!(ctype_isnum(dcinfo) || ctype_isptr(dcinfo) ||
		  ctype_isenum(dcinfo)))
	      goto special;
	  } else if (!ctype_isconstval(dfinfo)) {
	    goto special;
	  }
	}
      }
      fid = ctype_sib_acq(d);
      while (fid) {
	CType dfsnap, dcsnap, *df = crec_ctype_snapshot(J, cts, fid, &dfsnap);
	CTInfo dfinfo = ctype_info_acq(df);
	fid = ctype_sib_acq(df);
	if (ctype_isfield(dfinfo)) {
	  CType *dc;
	  CTInfo dcinfo;
	  CTSize dcsize, dfsize;
	  TRef sp, dp;
	  TValue tv;
	  TValue *sval = &tv;
	  setintV(&tv, 0);
	  if (!ctype_name_acq(df)) continue;  /* Ignore unnamed fields. */
	  dc = crec_ctype_rawchild(J, cts, df, &dcsnap);
	  dcinfo = ctype_info_acq(dc);
	  dcsize = ctype_size_acq(dc);
	  dfsize = ctype_size_acq(df);
	  if (!(ctype_isnum(dcinfo) || ctype_isptr(dcinfo) ||
		ctype_isenum(dcinfo)))
	    lj_trace_err(J, LJ_TRERR_NYICONV);  /* NYI: init aggregates. */
	  if (J->base[i]) {
	    sp = J->base[i];
	    sval = &rd->argv[i];
	    i++;
	  } else {
	    sp = ctype_isptr(dcinfo) ? TREF_NIL : lj_ir_kint(J, 0);
	  }
	  dp = emitir(IRT(IR_ADD, IRT_PTR), trcd,
		      lj_ir_kintp(J, dfsize + sizeof(GCcdata)));
	  crec_ct_tv(J, dc, dp, sp, sval);
	  if ((info & CTF_UNION)) {
	    if (sz != dcsize)  /* NYI: partial init of union. */
	      lj_trace_err(J, LJ_TRERR_NYICONV);
	    break;
	  }
	} else if (!ctype_isconstval(dfinfo)) {
	  /* NYI: init bitfields and sub-structures. */
	  lj_trace_err(J, LJ_TRERR_NYICONV);
	}
      }
    } else {
      TRef dp;
    single_init:
      dp = emitir(IRT(IR_ADD, IRT_PTR), trcd, lj_ir_kintp(J, sizeof(GCcdata)));
      if (J->base[1]) {
	crec_ct_tv(J, d, dp, J->base[1], &rd->argv[1]);
      } else {
	TValue tv;
	tv.u64 = 0;
	crec_ct_tv(J, d, dp, lj_ir_kint(J, 0), &tv);
      }
    }
  }
  J->base[0] = trcd;
  /* Handle __gc metamethod. */
  fin = crec_ctype_metatv(J, cts, &fintv, id, MM_gc);
  if (fin)
    crec_finalizer(J, trcd, 0, fin);
}

/* Record argument conversions.
** Note: may reallocate the C type table and invalidate CType pointers.
*/
#if LJ_FFI_RECORD_CALLS
static TRef crec_call_args(jit_State *J, RecordFFData *rd,
			   CTState *cts, CType *ct)
{
  TRef args[CCI_NARGS_MAX];
  CType ctfcopy, dcopy;
  CTypeID fid;
  CTInfo info = ctype_info_acq(ct);  /* lj_ccall_ctid_vararg may invalidate ct pointer. */
  MSize i, n;
  TRef tr, *base;
  cTValue *o;
#if LJ_TARGET_X86
#if LJ_ABI_WIN
  TRef *arg0 = NULL, *arg1 = NULL;
#endif
  int ngpr = 0;
  if (ctype_cconv(info) == CTCC_THISCALL)
    ngpr = 1;
  else if (ctype_cconv(info) == CTCC_FASTCALL)
    ngpr = 2;
#elif LJ_TARGET_ARM64 && LJ_TARGET_OSX
  int ngpr = CCALL_NARG_GPR;
#endif

  /* Skip initial attributes. */
  fid = ctype_sib_acq(ct);
  while (fid) {
    CType *ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    CTInfo ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  args[0] = TREF_NIL;
  for (n = 0, base = J->base+1, o = rd->argv+1; *base; n++, base++, o++) {
    CTypeID did;
    CType *d;
    CTInfo dinfo;
    CTSize dsize;

    if (n >= CCI_NARGS_MAX)
      lj_trace_err(J, LJ_TRERR_NYICALL);

    if (fid) {  /* Get argument type from field. */
      CType *ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
      CTInfo ctfinfo = ctype_info_acq(ctf);
      fid = ctype_sib_acq(ctf);
      lj_assertJ(ctype_isfield(ctfinfo), "field expected");
      did = ctype_cid(ctfinfo);
    } else {
      if (!(info & CTF_VARARG))
	lj_trace_err(J, LJ_TRERR_NYICALL);  /* Too many arguments. */
#if LJ_TARGET_ARM64 && LJ_TARGET_OSX
      if (ngpr >= 0) {
	ngpr = -1;
	args[n++] = TREF_NIL;  /* Marker for start of varargs. */
	if (n >= CCI_NARGS_MAX)
	  lj_trace_err(J, LJ_TRERR_NYICALL);
      }
#endif
      did = lj_ccall_ctid_vararg(J->L, cts, o);  /* Infer vararg type. */
    }
    d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
    dinfo = ctype_info_acq(d);
    dsize = ctype_size_acq(d);
    if (!(ctype_isnum(dinfo) || ctype_isptr(dinfo) ||
	  ctype_isenum(dinfo)))
      lj_trace_err(J, LJ_TRERR_NYICALL);
    tr = crec_ct_tv(J, d, 0, *base, o);
    if (ctype_isinteger_or_bool(dinfo)) {
#if LJ_TARGET_ARM64 && LJ_TARGET_OSX
      if (!ngpr) {
	/* Fixed args passed on the stack use their unpromoted size. */
	if (dsize != lj_ir_type_size[tref_type(tr)]) {
	  lj_assertJ(dsize == 1 || dsize==2, "unexpected size %d", dsize);
	  tr = emitconv(tr, dsize==1 ? IRT_U8 : IRT_U16, tref_type(tr), 0);
	}
      } else
#endif
      if (dsize < 4) {
	if ((dinfo & CTF_UNSIGNED))
	  tr = emitconv(tr, IRT_INT, dsize==1 ? IRT_U8 : IRT_U16, 0);
	else
	  tr = emitconv(tr, IRT_INT, dsize==1 ? IRT_I8 : IRT_I16,IRCONV_SEXT);
      }
    } else if (LJ_SOFTFP32 && ctype_isfp(dinfo) && dsize > 4) {
      lj_needsplit(J);
    }
#if LJ_TARGET_X86
    /* 64 bit args must not end up in registers for fastcall/thiscall. */
#if LJ_ABI_WIN
    if (!ctype_isfp(dinfo)) {
      /* Sigh, the Windows/x86 ABI allows reordering across 64 bit args. */
      if (tref_typerange(tr, IRT_I64, IRT_U64)) {
	if (ngpr) {
	  arg0 = &args[n]; args[n++] = TREF_NIL; ngpr--;
	  if (ngpr) {
	    arg1 = &args[n]; args[n++] = TREF_NIL; ngpr--;
	  }
	}
      } else {
	if (arg0) { *arg0 = tr; arg0 = NULL; n--; continue; }
	if (arg1) { *arg1 = tr; arg1 = NULL; n--; continue; }
	if (ngpr) ngpr--;
      }
    }
#else
    if (!ctype_isfp(dinfo) && ngpr) {
      if (tref_typerange(tr, IRT_I64, IRT_U64)) {
	/* No reordering for other x86 ABIs. Simply add alignment args. */
	do { args[n++] = TREF_NIL; } while (--ngpr);
      } else {
	ngpr--;
      }
    }
#endif
#elif LJ_TARGET_ARM64 && LJ_TARGET_OSX
    if (!ctype_isfp(dinfo) && ngpr) {
      ngpr--;
    }
#endif
    args[n] = tr;
  }
  tr = args[0];
  for (i = 1; i < n; i++)
    tr = emitir(IRT(IR_CARG, IRT_NIL), tr, args[i]);
  return tr;
}

/* Create a snapshot for the caller, simulating a 'false' return value. */
static void crec_snap_caller(jit_State *J)
{
  lua_State *L = J->L;
  TValue *base = L->base, *top = L->top;
  const BCIns *pc = J->pc;
  TRef ftr = J->base[-1-LJ_FR2];
  ptrdiff_t delta;
  if (!frame_islua(base-1) || J->framedepth <= 0)
    lj_trace_err(J, LJ_TRERR_NYICALL);
  J->pc = frame_pc(base-1); delta = 1+LJ_FR2+bc_a(J->pc[-1]);
  L->top = base; L->base = base - delta;
  J->base[-1-LJ_FR2] = TREF_FALSE;
  J->base -= delta; J->baseslot -= (BCReg)delta;
  J->maxslot = (BCReg)delta-LJ_FR2; J->framedepth--;
  lj_snap_add(J);
  L->base = base; L->top = top;
  J->framedepth++; J->maxslot = 1;
  J->base += delta; J->baseslot += (BCReg)delta;
  J->base[-1-LJ_FR2] = ftr; J->pc = pc;
}
#endif

#if LJ_TARGET_X64
static uint32_t crec_call_jit_num_sig(MSize narg)
{
  if (narg == 0)
    return LJ_CCALL_JIT_NUM_SIG0;
  return narg == 1 ? LJ_CCALL_JIT_NUM_SIG_NUM :
		     LJ_CCALL_JIT_NUM_SIG_NUM_NUM;
}

static uint32_t crec_call_jit_64_sig(MSize narg, const int *uns)
{
  if (narg == 0)
    return LJ_CCALL_JIT_64_SIG0;
  if (narg == 1)
    return uns[0] ? LJ_CCALL_JIT_64_SIG_U64 : LJ_CCALL_JIT_64_SIG_I64;
  return LJ_CCALL_JIT_64_SIG_I64_I64 +
	 (uint32_t)(uns[0] * 2 + uns[1]);
}

static int crec_call_jit_num_fpr(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, args[2], noarg = lj_ir_knum(J, 0);
  MSize i, narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isfp(ctr_info) || ctype_size_acq(ctr) != sizeof(double))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  for (i = 0; i < narg; i++) {
    if (!fid)
      return 0;
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isfield(ctfinfo))
      return 0;
    fid = ctype_sib_acq(ctf);
    did = ctype_cid(ctfinfo);
    d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
    dinfo = ctype_info_acq(d);
    if (!ctype_isfp(dinfo) || ctype_size_acq(d) != sizeof(double))
      return 0;
    args[i] = crec_ct_tv(J, d, 0, J->base[1+i], &rd->argv[1+i]);
    if (!tref_isnum(args[i]))
      lj_trace_err(J, LJ_TRERR_NYICALL);
  }
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  if (narg < 1) args[0] = noarg;
  if (narg < 2) args[1] = noarg;
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_num_fpr, func, args[0],
			  args[1],
			  lj_ir_kint(J, (int32_t)crec_call_jit_num_sig(narg)));
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_flt_fpr(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, args[2];
  MSize i, narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isfp(ctr_info) || ctype_size_acq(ctr) != sizeof(float))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  for (i = 0; i < narg; i++) {
    if (!fid)
      return 0;
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isfield(ctfinfo))
      return 0;
    fid = ctype_sib_acq(ctf);
    did = ctype_cid(ctfinfo);
    d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
    dinfo = ctype_info_acq(d);
    if (!ctype_isfp(dinfo) || ctype_size_acq(d) != sizeof(float))
      return 0;
    args[i] = crec_ct_tv(J, d, 0, J->base[1+i], &rd->argv[1+i]);
    if (!tref_istype(args[i], IRT_FLOAT))
      lj_trace_err(J, LJ_TRERR_NYICALL);
  }
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  if (narg < 1)
    args[0] = emitconv(lj_ir_knum(J, 0), IRT_FLOAT, IRT_NUM, 0);
  if (narg < 2)
    args[1] = emitconv(lj_ir_knum(J, 0), IRT_FLOAT, IRT_NUM, 0);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_flt_fpr, func, args[0],
			  args[1],
			  lj_ir_kint(J, (int32_t)crec_call_jit_num_sig(narg)));
  J->base[0] = emitconv(J->base[0], IRT_NUM, IRT_FLOAT, 0);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_num_i32(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 1)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isfp(ctr_info) || ctype_size_acq(ctr) != sizeof(double))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 4 ||
      (dinfo & CTF_UNSIGNED))
    return 0;
  if (fid)
    return 0;

  arg = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_isint(arg))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_num_i32, func, arg);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_num_ptr(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 1)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isfp(ctr_info) || ctype_size_acq(ctr) != sizeof(double))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isptr(dinfo) || ctype_size_acq(d) != CTSIZE_PTR)
    return 0;
  if (fid)
    return 0;

  arg = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_istype(arg, IRT_PTR))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_num_ptr, func, arg);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_num_flt(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 1)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isfp(ctr_info) || ctype_size_acq(ctr) != sizeof(double))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isfp(dinfo) || ctype_size_acq(d) != sizeof(float))
    return 0;
  if (fid)
    return 0;

  arg = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_istype(arg, IRT_FLOAT))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_num_flt, func, arg);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_i32_num(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 1)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 4 ||
      (ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isfp(dinfo) || ctype_size_acq(d) != sizeof(double))
    return 0;
  if (fid)
    return 0;

  arg = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_isnum(arg))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_i32_num, func, arg);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_i32_flt(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 1)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 4 ||
      (ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isfp(dinfo) || ctype_size_acq(d) != sizeof(float))
    return 0;
  if (fid)
    return 0;

  arg = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_istype(arg, IRT_FLOAT))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_i32_flt, func, arg);
  J->needsnap = 1;
  return 1;
}

static TRef crec_call_jit_i8_arg(jit_State *J, TRef arg)
{
  IRType t = tref_type(arg);
  if (!tref_typerange(arg, IRT_I8, IRT_U32))
    lj_trace_err(J, LJ_TRERR_NYICALL);
  if (t != IRT_I8)
    arg = emitconv(arg, IRT_I8, t, 0);
  return emitconv(arg, IRT_INT, IRT_I8, IRCONV_SEXT);
}

static TRef crec_call_jit_u32_arg(jit_State *J, TRef arg);

static int crec_call_jit_i32_i8(jit_State *J, RecordFFData *rd, CTState *cts,
				CType *ct, CTInfo info, GCcdata *cd,
				IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 1)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 4 ||
      (ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  if (fid)
    return 0;
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 1 ||
      (dinfo & CTF_UNSIGNED))
    return 0;

  arg = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  arg = crec_call_jit_i8_arg(J, arg);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_i32_i8, func, arg);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_i32_u32(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 1)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 4 ||
      (ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  if (fid)
    return 0;
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 4 ||
      !(dinfo & CTF_UNSIGNED))
    return 0;

  arg = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  arg = crec_call_jit_u32_arg(J, arg);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_i32_u32, func, arg);
  J->needsnap = 1;
  return 1;
}

static TRef crec_call_jit_ulong_arg(jit_State *J, TRef arg, CTSize size)
{
  IRType t = tref_type(arg);
  if (size == 4) {
    if (t == IRT_I64 || t == IRT_U64)
      arg = emitconv(arg, IRT_U32, t, 0);
    else if (t == IRT_I8 || t == IRT_I16)
      arg = emitconv(arg, IRT_INT, t, IRCONV_SEXT);
    else if (t == IRT_U8 || t == IRT_U16)
      arg = emitconv(arg, IRT_INT, t, 0);
    else if (t != IRT_INT && t != IRT_U32)
      lj_trace_err(J, LJ_TRERR_NYICALL);
    t = tref_type(arg);
    return t == IRT_UINTP ? arg : emitconv(arg, IRT_UINTP, t, 0);
  }
  if (size != 8)
    lj_trace_err(J, LJ_TRERR_NYICALL);
  if (t == IRT_UINTP)
    return arg;
  if (t == IRT_I64)
    return emitconv(arg, IRT_UINTP, IRT_I64, 0);
  if (t == IRT_U32)
    return emitconv(arg, IRT_UINTP, IRT_U32, 0);
  if (t == IRT_INT)
    return emitconv(arg, IRT_UINTP, IRT_INT, IRCONV_SEXT);
  if (t == IRT_I8 || t == IRT_I16) {
    arg = emitconv(arg, IRT_INT, t, IRCONV_SEXT);
    return emitconv(arg, IRT_UINTP, IRT_INT, IRCONV_SEXT);
  }
  if (t == IRT_U8 || t == IRT_U16) {
    arg = emitconv(arg, IRT_INT, t, 0);
    return emitconv(arg, IRT_UINTP, IRT_INT, 0);
  }
  lj_trace_err(J, LJ_TRERR_NYICALL);
  return 0;
}

static int crec_call_jit_i64_i32_ptr_u64(jit_State *J, RecordFFData *rd,
					 CTState *cts, CType *ct,
					 CTInfo info, GCcdata *cd,
					 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg0, arg1, arg2, tr;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 3)
      return 0;
    narg++;
  }
  if (narg != 3)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 8 ||
      (ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 4 ||
      (dinfo & CTF_UNSIGNED))
    return 0;
  arg0 = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_isint(arg0))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isptr(dinfo) || ctype_size_acq(d) != CTSIZE_PTR)
    return 0;
  arg1 = crec_ct_tv(J, d, 0, J->base[2], &rd->argv[2]);
  if (!tref_istype(arg1, IRT_PTR))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 8 ||
      !(dinfo & CTF_UNSIGNED))
    return 0;
  arg2 = crec_ct_tv(J, d, 0, J->base[3], &rd->argv[3]);
  arg2 = crec_call_jit_ulong_arg(J, arg2, ctype_size_acq(d));
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  tr = lj_ir_call(J, IRCALL_lj_ccall_jit_i64_i32_ptr_u64,
		  func, arg0, arg1, arg2);
  J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, ctype_cid(info)),
		      tr);
  lj_needsplit(J);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_i64_i32_i64_i32(jit_State *J, RecordFFData *rd,
					 CTState *cts, CType *ct,
					 CTInfo info, GCcdata *cd,
					 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg0, arg1, arg2, tr;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 3)
      return 0;
    narg++;
  }
  if (narg != 3)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 8 ||
      (ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 4 ||
      (dinfo & CTF_UNSIGNED))
    return 0;
  arg0 = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_isint(arg0))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 8 ||
      (dinfo & CTF_UNSIGNED))
    return 0;
  arg1 = crec_ct_tv(J, d, 0, J->base[2], &rd->argv[2]);
  if (!tref_istype(arg1, IRT_I64))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 4 ||
      (dinfo & CTF_UNSIGNED))
    return 0;
  arg2 = crec_ct_tv(J, d, 0, J->base[3], &rd->argv[3]);
  if (!tref_isint(arg2))
    lj_trace_err(J, LJ_TRERR_NYICALL);
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  tr = lj_ir_call(J, IRCALL_lj_ccall_jit_i64_i32_i64_i32,
		  func, arg0, arg1, arg2);
  J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, ctype_cid(info)),
		      tr);
  lj_needsplit(J);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_i32_i32_ptr_u32(jit_State *J, RecordFFData *rd,
					 CTState *cts, CType *ct,
					 CTInfo info, GCcdata *cd,
					 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg0, arg1, arg2;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 3)
      return 0;
    narg++;
  }
  if (narg != 3)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 4 ||
      (ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 4 ||
      (dinfo & CTF_UNSIGNED))
    return 0;
  arg0 = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_isint(arg0))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isptr(dinfo) || ctype_size_acq(d) != CTSIZE_PTR)
    return 0;
  arg1 = crec_ct_tv(J, d, 0, J->base[2], &rd->argv[2]);
  if (!tref_istype(arg1, IRT_PTR))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 4 ||
      !(dinfo & CTF_UNSIGNED))
    return 0;
  arg2 = crec_ct_tv(J, d, 0, J->base[3], &rd->argv[3]);
  arg2 = crec_call_jit_u32_arg(J, arg2);
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_i32_i32_ptr_u32,
			  func, arg0, arg1, arg2);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_i32_ptr_ptr_u64(jit_State *J, RecordFFData *rd,
					 CTState *cts, CType *ct,
					 CTInfo info, GCcdata *cd,
					 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg0, arg1, arg2;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 3)
      return 0;
    narg++;
  }
  if (narg != 3)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 4 ||
      (ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isptr(dinfo) || ctype_size_acq(d) != CTSIZE_PTR)
    return 0;
  arg0 = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_istype(arg0, IRT_PTR))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isptr(dinfo) || ctype_size_acq(d) != CTSIZE_PTR)
    return 0;
  arg1 = crec_ct_tv(J, d, 0, J->base[2], &rd->argv[2]);
  if (!tref_istype(arg1, IRT_PTR))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 8 ||
      !(dinfo & CTF_UNSIGNED))
    return 0;
  arg2 = crec_ct_tv(J, d, 0, J->base[3], &rd->argv[3]);
  arg2 = crec_call_jit_ulong_arg(J, arg2, ctype_size_acq(d));
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_i32_ptr_ptr_u64,
			  func, arg0, arg1, arg2);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_void_ptr_ptr_u64(jit_State *J, RecordFFData *rd,
					  CTState *cts, CType *ct,
					  CTInfo info, GCcdata *cd,
					  IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg0, arg1, arg2;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 3)
      return 0;
    narg++;
  }
  if (narg != 3)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isvoid(ctr_info))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isptr(dinfo) || ctype_size_acq(d) != CTSIZE_PTR)
    return 0;
  arg0 = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_istype(arg0, IRT_PTR))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isptr(dinfo) || ctype_size_acq(d) != CTSIZE_PTR)
    return 0;
  arg1 = crec_ct_tv(J, d, 0, J->base[2], &rd->argv[2]);
  if (!tref_istype(arg1, IRT_PTR))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 8 ||
      !(dinfo & CTF_UNSIGNED))
    return 0;
  arg2 = crec_ct_tv(J, d, 0, J->base[3], &rd->argv[3]);
  arg2 = crec_call_jit_ulong_arg(J, arg2, ctype_size_acq(d));
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  lj_ir_call(J, IRCALL_lj_ccall_jit_void_ptr_ptr_u64,
	     func, arg0, arg1, arg2);
  rd->nres = 0;
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_64_ptr_ptr_u64(jit_State *J, RecordFFData *rd,
					CTState *cts, CType *ct,
					CTInfo info, GCcdata *cd,
					IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg0, arg1, arg2, tr;
  int retuns;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 3)
      return 0;
    narg++;
  }
  if (narg != 3)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 8)
    return 0;
  retuns = (ctr_info & CTF_UNSIGNED) != 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isptr(dinfo) || ctype_size_acq(d) != CTSIZE_PTR)
    return 0;
  arg0 = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_istype(arg0, IRT_PTR))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isptr(dinfo) || ctype_size_acq(d) != CTSIZE_PTR)
    return 0;
  arg1 = crec_ct_tv(J, d, 0, J->base[2], &rd->argv[2]);
  if (!tref_istype(arg1, IRT_PTR))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 8 ||
      !(dinfo & CTF_UNSIGNED))
    return 0;
  arg2 = crec_ct_tv(J, d, 0, J->base[3], &rd->argv[3]);
  arg2 = crec_call_jit_ulong_arg(J, arg2, ctype_size_acq(d));
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  if (retuns)
    tr = lj_ir_call(J, IRCALL_lj_ccall_jit_u64_ptr_ptr_u64,
		    func, arg0, arg1, arg2);
  else
    tr = lj_ir_call(J, IRCALL_lj_ccall_jit_i64_ptr_ptr_u64,
		    func, arg0, arg1, arg2);
  J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, ctype_cid(info)),
		      tr);
  lj_needsplit(J);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_ptr_ptr_ptr_u64(jit_State *J, RecordFFData *rd,
					 CTState *cts, CType *ct,
					 CTInfo info, GCcdata *cd,
					 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg0, arg1, arg2, tr;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 3)
      return 0;
    narg++;
  }
  if (narg != 3)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isptr(ctr_info) || ctype_size_acq(ctr) != CTSIZE_PTR)
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isptr(dinfo) || ctype_size_acq(d) != CTSIZE_PTR)
    return 0;
  arg0 = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_istype(arg0, IRT_PTR))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isptr(dinfo) || ctype_size_acq(d) != CTSIZE_PTR)
    return 0;
  arg1 = crec_ct_tv(J, d, 0, J->base[2], &rd->argv[2]);
  if (!tref_istype(arg1, IRT_PTR))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 8 ||
      !(dinfo & CTF_UNSIGNED))
    return 0;
  arg2 = crec_ct_tv(J, d, 0, J->base[3], &rd->argv[3]);
  arg2 = crec_call_jit_ulong_arg(J, arg2, ctype_size_acq(d));
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  tr = lj_ir_call(J, IRCALL_lj_ccall_jit_ptr_ptr_ptr_u64,
		  func, arg0, arg1, arg2);
  J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, ctype_cid(info)),
		      tr);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_i32_ptr_ulong_i32(jit_State *J, RecordFFData *rd,
					   CTState *cts, CType *ct,
					   CTInfo info, GCcdata *cd,
					   IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg0, arg1, arg2;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 3)
      return 0;
    narg++;
  }
  if (narg != 3)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 4 ||
      (ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isptr(dinfo) || ctype_size_acq(d) != CTSIZE_PTR)
    return 0;
  arg0 = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_istype(arg0, IRT_PTR))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != sizeof(unsigned long) ||
      !(dinfo & CTF_UNSIGNED))
    return 0;
  arg1 = crec_ct_tv(J, d, 0, J->base[2], &rd->argv[2]);
  arg1 = crec_call_jit_ulong_arg(J, arg1, ctype_size_acq(d));

  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 4 ||
      (dinfo & CTF_UNSIGNED))
    return 0;
  arg2 = crec_ct_tv(J, d, 0, J->base[3], &rd->argv[3]);
  if (!tref_isint(arg2))
    lj_trace_err(J, LJ_TRERR_NYICALL);
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_i32_ptr_ulong_i32,
			  func, arg0, arg1, arg2);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_ptr_num(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg, tr;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 1)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isptr(ctr_info) || ctype_size_acq(ctr) != CTSIZE_PTR)
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isfp(dinfo) || ctype_size_acq(d) != sizeof(double))
    return 0;
  if (fid)
    return 0;

  arg = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_isnum(arg))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  tr = lj_ir_call(J, IRCALL_lj_ccall_jit_ptr_num, func, arg);
  J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, ctype_cid(info)),
		      tr);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_void_num(jit_State *J, RecordFFData *rd, CTState *cts,
				  CType *ct, CTInfo info, GCcdata *cd,
				  IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 1)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isvoid(ctr_info))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isfp(dinfo) || ctype_size_acq(d) != sizeof(double))
    return 0;
  if (fid)
    return 0;

  arg = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_isnum(arg))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  lj_ir_call(J, IRCALL_lj_ccall_jit_void_num, func, arg);
  rd->nres = 0;
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_void_flt(jit_State *J, RecordFFData *rd,
				  CTState *cts, CType *ct, CTInfo info,
				  GCcdata *cd, IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 1)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isvoid(ctr_info))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isfp(dinfo) || ctype_size_acq(d) != sizeof(float))
    return 0;
  if (fid)
    return 0;

  arg = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_istype(arg, IRT_FLOAT))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  lj_ir_call(J, IRCALL_lj_ccall_jit_void_flt, func, arg);
  rd->nres = 0;
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_flt_num(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 1)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isfp(ctr_info) || ctype_size_acq(ctr) != sizeof(float))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isfp(dinfo) || ctype_size_acq(d) != sizeof(double))
    return 0;
  if (fid)
    return 0;

  arg = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  if (!tref_isnum(arg))
    lj_trace_err(J, LJ_TRERR_NYICALL);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_flt_num, func, arg);
  J->base[0] = emitconv(J->base[0], IRT_NUM, IRT_FLOAT, 0);
  J->needsnap = 1;
  return 1;
}

#define CREC_CALL_JIT_KIND_I32 0
#define CREC_CALL_JIT_KIND_U32 1
#define CREC_CALL_JIT_KIND_PTR 2
#define CREC_CALL_JIT_KIND_I64 3
#define CREC_CALL_JIT_KIND_U64 4
#define CREC_CALL_JIT_RETKIND_VOID 5

static int crec_call_jit_gpr_kind(CTInfo info, CTSize size)
{
  if (ctype_isinteger(info) && size == 4 && !(info & CTF_UNSIGNED))
    return CREC_CALL_JIT_KIND_I32;
  if (ctype_isinteger(info) && size == 4 && (info & CTF_UNSIGNED))
    return CREC_CALL_JIT_KIND_U32;
  if (ctype_isptr(info) && size == CTSIZE_PTR)
    return CREC_CALL_JIT_KIND_PTR;
  if (ctype_isinteger(info) && size == 8 && !(info & CTF_UNSIGNED))
    return CREC_CALL_JIT_KIND_I64;
  if (ctype_isinteger(info) && size == 8 && (info & CTF_UNSIGNED))
    return CREC_CALL_JIT_KIND_U64;
  return -1;
}

static int crec_call_jit_gpr_retkind(CTInfo info, CTSize size)
{
  if (ctype_isvoid(info))
    return CREC_CALL_JIT_RETKIND_VOID;
  if (ctype_isinteger(info) && size == 4 && !(info & CTF_UNSIGNED))
    return CREC_CALL_JIT_KIND_I32;
  if (ctype_isptr(info) && size == CTSIZE_PTR)
    return CREC_CALL_JIT_KIND_PTR;
  return -1;
}

static int crec_call_jit_i64_kind(CTInfo info, CTSize size)
{
  return ctype_isinteger(info) && size == 8 && !(info & CTF_UNSIGNED);
}

static TRef crec_call_jit_u32_arg(jit_State *J, TRef arg)
{
  IRType t = tref_type(arg);
  if (t == IRT_INT || t == IRT_U32)
    return arg;
  if (t == IRT_I8 || t == IRT_I16)
    return emitconv(arg, IRT_INT, t, IRCONV_SEXT);
  if (t == IRT_U8 || t == IRT_U16)
    return emitconv(arg, IRT_INT, t, 0);
  lj_trace_err(J, LJ_TRERR_NYICALL);
  return 0;
}

static TRef crec_call_jit_gpr_arg(jit_State *J, int kind, TRef arg)
{
  if (kind == CREC_CALL_JIT_KIND_I32) {
    if (!tref_isint(arg))
      lj_trace_err(J, LJ_TRERR_NYICALL);
  } else if (kind == CREC_CALL_JIT_KIND_U32) {
    arg = crec_call_jit_u32_arg(J, arg);
  } else if (kind == CREC_CALL_JIT_KIND_PTR) {
    if (!tref_istype(arg, IRT_PTR))
      lj_trace_err(J, LJ_TRERR_NYICALL);
  } else {
    if (!tref_istype(arg, kind == CREC_CALL_JIT_KIND_I64 ? IRT_I64 : IRT_U64))
      lj_trace_err(J, LJ_TRERR_NYICALL);
  }
  return arg;
}

static int crec_call_jit_narrow_sig(CTInfo info, CTSize size)
{
  if (!ctype_isinteger(info))
    return -1;
  if (size == 1)
    return (info & CTF_UNSIGNED) ? LJ_CCALL_JIT_NARROW_U8 :
				   LJ_CCALL_JIT_NARROW_I8;
  if (size == 2)
    return (info & CTF_UNSIGNED) ? LJ_CCALL_JIT_NARROW_U16 :
				   LJ_CCALL_JIT_NARROW_I16;
  return -1;
}

static int crec_call_jit_narrow_0(jit_State *J, RecordFFData *rd,
				  CTState *cts, CType *ct, CTInfo info,
				  GCcdata *cd, IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy;
  CType *ctr, *ctf;
  CTypeID fid;
  CTInfo ctr_info, ctfinfo;
  TRef func;
  int sig;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 1)
      return 0;
    narg++;
  }
  if (narg != 0)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  sig = crec_call_jit_narrow_sig(ctr_info, ctype_size_acq(ctr));
  if (sig < 0)
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_narrow_0, func,
			  lj_ir_kint(J, sig));
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_u32_0(jit_State *J, RecordFFData *rd, CTState *cts,
			       CType *ct, CTInfo info, GCcdata *cd,
			       IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy;
  CType *ctr, *ctf;
  CTypeID fid;
  CTInfo ctr_info, ctfinfo;
  TRef func;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 1)
      return 0;
    narg++;
  }
  if (narg != 0)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 4 ||
      !(ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_u32_0, func);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_u64_0(jit_State *J, RecordFFData *rd, CTState *cts,
			       CType *ct, CTInfo info, GCcdata *cd,
			       IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy;
  CType *ctr, *ctf;
  CTypeID fid;
  CTInfo ctr_info, ctfinfo;
  TRef func, tr;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 1)
      return 0;
    narg++;
  }
  if (narg != 0)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 8 ||
      !(ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  tr = lj_ir_call(J, IRCALL_lj_ccall_jit_u64_0, func);
  J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, ctype_cid(info)),
		      tr);
  lj_needsplit(J);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_i64_i64(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, args[2], tr;
  int uns[2];
  MSize i, narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg == 0 || narg > 2)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 8 ||
      (ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  for (i = 0; i < narg; i++) {
    if (!fid)
      return 0;
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isfield(ctfinfo))
      return 0;
    fid = ctype_sib_acq(ctf);
    did = ctype_cid(ctfinfo);
    d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
    dinfo = ctype_info_acq(d);
    if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 8)
      return 0;
    uns[i] = (dinfo & CTF_UNSIGNED) != 0;
    args[i] = crec_ct_tv(J, d, 0, J->base[1+i], &rd->argv[1+i]);
    if (!tref_istype(args[i], uns[i] ? IRT_U64 : IRT_I64))
      lj_trace_err(J, LJ_TRERR_NYICALL);
  }
  if (fid)
    return 0;
  if (narg < 2)
    args[1] = lj_ir_kint64(J, 0);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  tr = lj_ir_call(J, IRCALL_lj_ccall_jit_i64_gpr, func, args[0],
		  args[1], lj_ir_kint(J, (int32_t)crec_call_jit_64_sig(narg, uns)));
  J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, ctype_cid(info)),
		      tr);
  lj_needsplit(J);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_u64_u64(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, args[2], tr;
  int uns[2];
  MSize i, narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg == 0 || narg > 2)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 8 ||
      !(ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  for (i = 0; i < narg; i++) {
    if (!fid)
      return 0;
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isfield(ctfinfo))
      return 0;
    fid = ctype_sib_acq(ctf);
    did = ctype_cid(ctfinfo);
    d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
    dinfo = ctype_info_acq(d);
    if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 8)
      return 0;
    uns[i] = (dinfo & CTF_UNSIGNED) != 0;
    args[i] = crec_ct_tv(J, d, 0, J->base[1+i], &rd->argv[1+i]);
    if (!tref_istype(args[i], uns[i] ? IRT_U64 : IRT_I64))
      lj_trace_err(J, LJ_TRERR_NYICALL);
  }
  if (fid)
    return 0;
  if (narg < 2)
    args[1] = lj_ir_kint64(J, 0);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  tr = lj_ir_call(J, IRCALL_lj_ccall_jit_u64_u64, func, args[0],
		  args[1], lj_ir_kint(J, (int32_t)crec_call_jit_64_sig(narg, uns)));
  J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, ctype_cid(info)),
		      tr);
  lj_needsplit(J);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_i64_gpr(jit_State *J, RecordFFData *rd, CTState *cts,
				 CType *ct, CTInfo info, GCcdata *cd,
				 IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy;
  CType *ctr, *ctf;
  CTypeID fid;
  CTInfo ctr_info, ctfinfo;
  TRef func, args[2], tr;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 0)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!crec_call_jit_i64_kind(ctr_info, ctype_size_acq(ctr)))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (fid)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  args[0] = lj_ir_kint64(J, 0);
  args[1] = lj_ir_kint64(J, 0);
  tr = lj_ir_call(J, IRCALL_lj_ccall_jit_i64_gpr, func, args[0],
		  args[1], lj_ir_kint(J, (int32_t)LJ_CCALL_JIT_64_SIG0));
  J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, ctype_cid(info)),
		      tr);
  lj_needsplit(J);
  J->needsnap = 1;
  return 1;
}

static uint32_t crec_call_jit_sig(MSize narg, const int *kind)
{
  if (narg == 0)
    return LJ_CCALL_JIT_SIG0;
  if (narg == 1) {
    if (kind[0] <= CREC_CALL_JIT_KIND_PTR)
      return LJ_CCALL_JIT_SIG_I32 + (uint32_t)kind[0];
    return kind[0] == CREC_CALL_JIT_KIND_I64 ? LJ_CCALL_JIT_SIG_I64 :
					       LJ_CCALL_JIT_SIG_U64;
  }
  if (kind[0] == CREC_CALL_JIT_KIND_PTR &&
      kind[1] == CREC_CALL_JIT_KIND_I64)
    return LJ_CCALL_JIT_SIG_PTR_I64;
  if (kind[0] == CREC_CALL_JIT_KIND_PTR &&
      kind[1] == CREC_CALL_JIT_KIND_U64)
    return LJ_CCALL_JIT_SIG_PTR_U64;
  if (kind[0] == CREC_CALL_JIT_KIND_I32 &&
      kind[1] == CREC_CALL_JIT_KIND_I64)
    return LJ_CCALL_JIT_SIG_I32_I64;
  if (kind[0] == CREC_CALL_JIT_KIND_I32 &&
      kind[1] == CREC_CALL_JIT_KIND_U64)
    return LJ_CCALL_JIT_SIG_I32_U64;
  if (kind[0] == CREC_CALL_JIT_KIND_U32 &&
      kind[1] == CREC_CALL_JIT_KIND_I64)
    return LJ_CCALL_JIT_SIG_U32_I64;
  if (kind[0] == CREC_CALL_JIT_KIND_U32 &&
      kind[1] == CREC_CALL_JIT_KIND_U64)
    return LJ_CCALL_JIT_SIG_U32_U64;
  if (kind[0] == CREC_CALL_JIT_KIND_I64 &&
      kind[1] == CREC_CALL_JIT_KIND_I32)
    return LJ_CCALL_JIT_SIG_I64_I32;
  if (kind[0] == CREC_CALL_JIT_KIND_I64 &&
      kind[1] == CREC_CALL_JIT_KIND_U32)
    return LJ_CCALL_JIT_SIG_I64_U32;
  if (kind[0] == CREC_CALL_JIT_KIND_U64 &&
      kind[1] == CREC_CALL_JIT_KIND_I32)
    return LJ_CCALL_JIT_SIG_U64_I32;
  if (kind[0] == CREC_CALL_JIT_KIND_U64 &&
      kind[1] == CREC_CALL_JIT_KIND_U32)
    return LJ_CCALL_JIT_SIG_U64_U32;
  if (kind[0] == CREC_CALL_JIT_KIND_I64 &&
      kind[1] == CREC_CALL_JIT_KIND_PTR)
    return LJ_CCALL_JIT_SIG_I64_PTR;
  if (kind[0] == CREC_CALL_JIT_KIND_U64 &&
      kind[1] == CREC_CALL_JIT_KIND_PTR)
    return LJ_CCALL_JIT_SIG_U64_PTR;
  if (kind[0] == CREC_CALL_JIT_KIND_I64 &&
      kind[1] == CREC_CALL_JIT_KIND_I64)
    return LJ_CCALL_JIT_SIG_I64_I64;
  if (kind[0] == CREC_CALL_JIT_KIND_I64 &&
      kind[1] == CREC_CALL_JIT_KIND_U64)
    return LJ_CCALL_JIT_SIG_I64_U64;
  if (kind[0] == CREC_CALL_JIT_KIND_U64 &&
      kind[1] == CREC_CALL_JIT_KIND_I64)
    return LJ_CCALL_JIT_SIG_U64_I64;
  if (kind[0] == CREC_CALL_JIT_KIND_U64 &&
      kind[1] == CREC_CALL_JIT_KIND_U64)
    return LJ_CCALL_JIT_SIG_U64_U64;
  if (kind[0] > CREC_CALL_JIT_KIND_PTR || kind[1] > CREC_CALL_JIT_KIND_PTR)
    return UINT32_MAX;
  return LJ_CCALL_JIT_SIG_I32_I32 +
	 (uint32_t)(kind[0] * 3 + kind[1]);
}

static int crec_call_jit_u32_gpr(jit_State *J, RecordFFData *rd,
				 CTState *cts, CType *ct, CTInfo info,
				 GCcdata *cd, IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, args[2], noarg = lj_ir_kptr(J, NULL);
  int kind[2];
  uint32_t sig;
  MSize i, narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg == 0)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 4 ||
      !(ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  for (i = 0; i < narg; i++) {
    if (!fid)
      return 0;
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isfield(ctfinfo))
      return 0;
    fid = ctype_sib_acq(ctf);
    did = ctype_cid(ctfinfo);
    d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
    dinfo = ctype_info_acq(d);
    kind[i] = crec_call_jit_gpr_kind(dinfo, ctype_size_acq(d));
    if (kind[i] < 0)
      return 0;
    args[i] = crec_ct_tv(J, d, 0, J->base[1+i], &rd->argv[1+i]);
    args[i] = crec_call_jit_gpr_arg(J, kind[i], args[i]);
  }
  if (fid)
    return 0;
  sig = crec_call_jit_sig(narg, kind);
  if (sig == UINT32_MAX)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  if (narg < 1) args[0] = noarg;
  if (narg < 2) args[1] = noarg;
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_u32_gpr, func, args[0],
			  args[1], lj_ir_kint(J, (int32_t)sig));
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_u32_u32(jit_State *J, RecordFFData *rd,
				 CTState *cts, CType *ct, CTInfo info,
				 GCcdata *cd, IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, arg;
  MSize narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg != 1)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 4 ||
      !(ctr_info & CTF_UNSIGNED))
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  if (!fid)
    return 0;
  ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
  ctfinfo = ctype_info_acq(ctf);
  if (!ctype_isfield(ctfinfo))
    return 0;
  fid = ctype_sib_acq(ctf);
  if (fid)
    return 0;
  did = ctype_cid(ctfinfo);
  d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
  dinfo = ctype_info_acq(d);
  if (!ctype_isinteger(dinfo) || ctype_size_acq(d) != 4 ||
      !(dinfo & CTF_UNSIGNED))
    return 0;

  arg = crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]);
  arg = crec_call_jit_u32_arg(J, arg);

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_u32_u32, func, arg);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_narrow_gpr(jit_State *J, RecordFFData *rd,
				    CTState *cts, CType *ct, CTInfo info,
				    GCcdata *cd, IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, args[2], noarg = lj_ir_kptr(J, NULL);
  int kind[2], retsig;
  uint32_t sig;
  MSize i, narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg == 0)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  retsig = crec_call_jit_narrow_sig(ctr_info, ctype_size_acq(ctr));
  if (retsig < 0)
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  for (i = 0; i < narg; i++) {
    if (!fid)
      return 0;
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isfield(ctfinfo))
      return 0;
    fid = ctype_sib_acq(ctf);
    did = ctype_cid(ctfinfo);
    d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
    dinfo = ctype_info_acq(d);
    kind[i] = crec_call_jit_gpr_kind(dinfo, ctype_size_acq(d));
    if (kind[i] < 0)
      return 0;
    args[i] = crec_ct_tv(J, d, 0, J->base[1+i], &rd->argv[1+i]);
    args[i] = crec_call_jit_gpr_arg(J, kind[i], args[i]);
  }
  if (fid)
    return 0;
  sig = crec_call_jit_sig(narg, kind);
  if (sig == UINT32_MAX)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  sig |= (uint32_t)retsig << 8;
  if (narg < 1) args[0] = noarg;
  if (narg < 2) args[1] = noarg;
  J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_narrow_gpr, func, args[0],
			  args[1], lj_ir_kint(J, (int32_t)sig));
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_64ret_gpr(jit_State *J, RecordFFData *rd,
				   CTState *cts, CType *ct, CTInfo info,
				   GCcdata *cd, IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, args[2], noarg = lj_ir_kptr(J, NULL), tr;
  int kind[2], uns;
  uint32_t sig;
  MSize i, narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }
  if (narg == 0)
    return 0;

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  if (!ctype_isinteger(ctr_info) || ctype_size_acq(ctr) != 8)
    return 0;
  uns = (ctr_info & CTF_UNSIGNED) != 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  for (i = 0; i < narg; i++) {
    if (!fid)
      return 0;
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isfield(ctfinfo))
      return 0;
    fid = ctype_sib_acq(ctf);
    did = ctype_cid(ctfinfo);
    d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
    dinfo = ctype_info_acq(d);
    kind[i] = crec_call_jit_gpr_kind(dinfo, ctype_size_acq(d));
    if (kind[i] < 0)
      return 0;
    args[i] = crec_ct_tv(J, d, 0, J->base[1+i], &rd->argv[1+i]);
    args[i] = crec_call_jit_gpr_arg(J, kind[i], args[i]);
  }
  if (fid)
    return 0;
  sig = crec_call_jit_sig(narg, kind);
  if (sig == UINT32_MAX)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  if (narg < 1) args[0] = noarg;
  if (narg < 2) args[1] = noarg;
  tr = uns ? lj_ir_call(J, IRCALL_lj_ccall_jit_u64_gpr, func, args[0],
			args[1], lj_ir_kint(J, (int32_t)sig)) :
	     lj_ir_call(J, IRCALL_lj_ccall_jit_i64_ret_gpr, func, args[0],
			args[1], lj_ir_kint(J, (int32_t)sig));
  J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, ctype_cid(info)),
		      tr);
  lj_needsplit(J);
  J->needsnap = 1;
  return 1;
}

static int crec_call_jit_gpr(jit_State *J, RecordFFData *rd, CTState *cts,
			     CType *ct, CTInfo info, GCcdata *cd,
			     IRType tp, CTSize fsz)
{
  CType ctrsnap, ctfcopy, dcopy;
  CType *ctr, *ctf, *d;
  CTypeID fid, did;
  CTInfo ctr_info, ctfinfo, dinfo;
  TRef func, args[2], noarg = lj_ir_kptr(J, NULL), tr;
  int retkind, kind[2];
  uint32_t sig;
  MSize i, narg = 0;

  if ((info & CTF_VARARG))
    return 0;
  while (J->base[1+narg]) {
    if (narg >= 2)
      return 0;
    narg++;
  }

  ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
  ctr_info = ctype_info_acq(ctr);
  retkind = crec_call_jit_gpr_retkind(ctr_info, ctype_size_acq(ctr));
  if (retkind < 0)
    return 0;

  fid = ctype_sib_acq(ct);
  while (fid) {
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(ctfinfo)) break;
    fid = ctype_sib_acq(ctf);
  }
  for (i = 0; i < narg; i++) {
    if (!fid)
      return 0;
    ctf = crec_ctype_snapshot(J, cts, fid, &ctfcopy);
    ctfinfo = ctype_info_acq(ctf);
    if (!ctype_isfield(ctfinfo))
      return 0;
    fid = ctype_sib_acq(ctf);
    did = ctype_cid(ctfinfo);
    d = crec_ctype_rawrefid(J, cts, did, &did, &dcopy);
    dinfo = ctype_info_acq(d);
    kind[i] = crec_call_jit_gpr_kind(dinfo, ctype_size_acq(d));
    if (kind[i] < 0)
      return 0;
    args[i] = crec_ct_tv(J, d, 0, J->base[1+i], &rd->argv[1+i]);
    args[i] = crec_call_jit_gpr_arg(J, kind[i], args[i]);
  }
  if (fid)
    return 0;
  sig = crec_call_jit_sig(narg, kind);
  if (sig == UINT32_MAX)
    return 0;

  if (lj_ctype_cb_isblacklisted(cts, cdata_getptr(cdataptr(cd), fsz)))
    lj_trace_err(J, LJ_TRERR_BLACKL);

  func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
  if (narg < 1) args[0] = noarg;
  if (narg < 2) args[1] = noarg;
  if (retkind == CREC_CALL_JIT_RETKIND_VOID) {
    lj_ir_call(J, IRCALL_lj_ccall_jit_void_gpr, func, args[0],
	       args[1], lj_ir_kint(J, (int32_t)sig));
    rd->nres = 0;
  } else if (retkind == CREC_CALL_JIT_KIND_PTR) {
    TRef trid = lj_ir_kint(J, ctype_cid(info));
    tr = lj_ir_call(J, IRCALL_lj_ccall_jit_ptr_gpr, func, args[0],
		    args[1], lj_ir_kint(J, (int32_t)sig));
    J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), trid, tr);
  } else {
    J->base[0] = lj_ir_call(J, IRCALL_lj_ccall_jit_i32_gpr, func, args[0],
			    args[1], lj_ir_kint(J, (int32_t)sig));
  }
  J->needsnap = 1;
  return 1;
}
#endif

/* Record function call. */
static int crec_call(jit_State *J, RecordFFData *rd, GCcdata *cd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CType ctsnap;
  CTypeID id;
  CType *ct = crec_ctype_rawrefid(J, cts, cd->ctypeid, &id, &ctsnap);
  CTInfo info = ctype_info_acq(ct);
#if LJ_TARGET_X64 || LJ_FFI_RECORD_CALLS
  IRType tp = IRT_PTR;
  CTSize fsz = CTSIZE_PTR;
#endif
  if (ctype_isptr(info)) {
#if LJ_TARGET_X64 || LJ_FFI_RECORD_CALLS
    fsz = ctype_size_acq(ct);
    tp = (LJ_64 && fsz == 8) ? IRT_P64 : IRT_P32;
#endif
    ct = crec_ctype_rawrefid(J, cts, ctype_cid(info), &id, &ctsnap);
    info = ctype_info_acq(ct);  /* crec_call_args may invalidate ct pointer. */
  }
  if (ctype_isfunc(info)) {
#if LJ_TARGET_X64
    if (crec_call_jit_num_fpr(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_flt_fpr(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_num_i32(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_num_ptr(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_num_flt(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_i32_num(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_i32_flt(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_i32_i8(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_i32_u32(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_i64_i32_ptr_u64(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_i64_i32_i64_i32(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_i32_i32_ptr_u32(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_i32_ptr_ptr_u64(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_void_ptr_ptr_u64(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_64_ptr_ptr_u64(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_ptr_ptr_ptr_u64(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_i32_ptr_ulong_i32(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_ptr_num(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_void_num(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_void_flt(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_flt_num(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_narrow_0(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_u32_0(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_u64_0(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_i64_i64(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_u64_u64(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_i64_gpr(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_narrow_gpr(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_u32_u32(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_u32_gpr(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_64ret_gpr(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
    if (crec_call_jit_gpr(J, rd, cts, ct, info, cd, tp, fsz))
      return 1;
#endif
#if !LJ_FFI_RECORD_CALLS
    lj_trace_err(J, LJ_TRERR_BLACKL);
#else
    TRef func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
    CType ctrsnap;
    CType *ctr = crec_ctype_rawchild(J, cts, ct, &ctrsnap);
    CTInfo ctr_info = ctype_info_acq(ctr);  /* crec_call_args may invalidate ctr. */
    IRType t = crec_ct2irt(cts, ctr);
    TRef tr;
    /* Check for blacklisted C functions that might call a callback. */
    if (lj_ctype_cb_isblacklisted(cts,
	  cdata_getptr(cdataptr(cd), (LJ_64 && tp == IRT_P64) ? 8 : 4)))
      lj_trace_err(J, LJ_TRERR_BLACKL);
    if (ctype_isvoid(ctr_info)) {
      t = IRT_NIL;
      rd->nres = 0;
    } else if (!(ctype_isnum(ctr_info) || ctype_isptr(ctr_info) ||
		 ctype_isenum(ctr_info)) || t == IRT_CDATA) {
      lj_trace_err(J, LJ_TRERR_NYICALL);
    }
    if ((info & CTF_VARARG)
#if LJ_TARGET_X86
	|| ctype_cconv(info) != CTCC_CDECL
#endif
	)
      func = emitir(IRT(IR_CARG, IRT_NIL), func,
		    lj_ir_kint(J, id));
    tr = emitir(IRT(IR_CALLXS, t), crec_call_args(J, rd, cts, ct), func);
    if (ctype_isbool(ctr_info)) {
      if (frame_islua(J->L->base-1) && bc_b(frame_pc(J->L->base-1)[-1]) == 1) {
	/* Don't check result if ignored. */
	tr = TREF_NIL;
      } else {
	crec_snap_caller(J);
#if LJ_TARGET_X86ORX64
	/* Note: only the x86/x64 backend supports U8 and only for EQ(tr, 0). */
	lj_ir_set(J, IRTG(IR_NE, IRT_U8), tr, lj_ir_kint(J, 0));
#else
	lj_ir_set(J, IRTGI(IR_NE), tr, lj_ir_kint(J, 0));
#endif
	J->postproc = LJ_POST_FIXGUARDSNAP;
	tr = TREF_TRUE;
      }
    } else if (t == IRT_PTR || (LJ_64 && t == IRT_P32) ||
	       t == IRT_I64 || t == IRT_U64 || ctype_isenum(ctr_info)) {
      TRef trid = lj_ir_kint(J, ctype_cid(info));
      tr = emitir(IRTG(IR_CNEWI, IRT_CDATA), trid, tr);
      if (t == IRT_I64 || t == IRT_U64) lj_needsplit(J);
    } else if (t == IRT_FLOAT || t == IRT_U32) {
      tr = emitconv(tr, IRT_NUM, t, 0);
    } else if (t == IRT_I8 || t == IRT_I16) {
      tr = emitconv(tr, IRT_INT, t, IRCONV_SEXT);
    } else if (t == IRT_U8 || t == IRT_U16) {
      tr = emitconv(tr, IRT_INT, t, 0);
    }
    J->base[0] = tr;
    J->needsnap = 1;
    return 1;
#endif
  }
  return 0;
}

void LJ_FASTCALL recff_cdata_call(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  GCcdata *cd = argv2cdata(J, J->base[0], &rd->argv[0]);
  CTypeID id = cd->ctypeid;
  TValue metatv;
  cTValue *tv;
  MMS mm = MM_call;
  if (id == CTID_CTYPEID) {
    id = crec_constructor(J, cd, J->base[0]);
    mm = MM_new;
  } else if (crec_call(J, rd, cd)) {
    return;
  }
  /* Record ctype __call/__new metamethod. */
  tv = crec_ctype_metatv(J, cts, &metatv,
			 crec_ctype_ptr_metaid(J, cts, id), mm);
  if (tv) {
    if (tvisfunc(tv)) {
      crec_tailcall(J, rd, tv);
      return;
    }
  } else if (mm == MM_new) {
    crec_alloc(J, rd, id);
    return;
  }
  /* No metamethod or NYI: non-function metamethods. */
  lj_trace_err(J, LJ_TRERR_BADTYPE);
}

static TRef crec_arith_int64(jit_State *J, TRef *sp, CType **s, MMS mm)
{
  CTInfo sinfo[2];
  CTSize ssize[2];
  if (!sp[0] || !sp[1])
    return 0;
  sinfo[0] = ctype_info_acq(s[0]);
  sinfo[1] = ctype_info_acq(s[1]);
  if (ctype_isnum(sinfo[0]) && ctype_isnum(sinfo[1])) {
    IRType dt;
    CTypeID id;
    TRef tr;
    MSize i;
    IROp op;
    lj_needsplit(J);
    ssize[0] = ctype_size_acq(s[0]);
    ssize[1] = ctype_size_acq(s[1]);
    if (((sinfo[0] & CTF_UNSIGNED) && ssize[0] == 8) ||
	((sinfo[1] & CTF_UNSIGNED) && ssize[1] == 8)) {
      dt = IRT_U64; id = CTID_UINT64;
    } else {
      dt = IRT_I64; id = CTID_INT64;
      if (mm < MM_add &&
	  !((sinfo[0] | sinfo[1]) & CTF_FP) &&
	  ssize[0] == 4 && ssize[1] == 4) {  /* Try to narrow comparison. */
	if (!((sinfo[0] ^ sinfo[1]) & CTF_UNSIGNED) ||
	    (tref_isk(sp[1]) && IR(tref_ref(sp[1]))->i >= 0)) {
	  dt = (sinfo[0] & CTF_UNSIGNED) ? IRT_U32 : IRT_INT;
	  goto comp;
	} else if (tref_isk(sp[0]) && IR(tref_ref(sp[0]))->i >= 0) {
	  dt = (sinfo[1] & CTF_UNSIGNED) ? IRT_U32 : IRT_INT;
	  goto comp;
	}
      }
    }
    for (i = 0; i < 2; i++) {
      IRType st = tref_type(sp[i]);
      if (st == IRT_NUM || st == IRT_FLOAT)
	sp[i] = emitconv(sp[i], dt, st, IRCONV_ANY);
      else if (!(st == IRT_I64 || st == IRT_U64))
	sp[i] = emitconv(sp[i], dt, IRT_INT,
			 (sinfo[i] & CTF_UNSIGNED) ? 0 : IRCONV_SEXT);
    }
    if (mm < MM_add) {
    comp:
      /* Assume true comparison. Fixup and emit pending guard later. */
      if (mm == MM_eq) {
	op = IR_EQ;
      } else {
	op = mm == MM_lt ? IR_LT : IR_LE;
	if (dt == IRT_U32 || dt == IRT_U64)
	  op += (IR_ULT-IR_LT);
      }
      lj_ir_set(J, IRTG(op, dt), sp[0], sp[1]);
      J->postproc = LJ_POST_FIXGUARD;
      return TREF_TRUE;
    } else {
      tr = emitir(IRT(mm+(int)IR_ADD-(int)MM_add, dt), sp[0], sp[1]);
    }
    return emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, id), tr);
  }
  return 0;
}

static TRef crec_arith_ptr(jit_State *J, TRef *sp, CType **s, MMS mm)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CType *ctp = s[0];
  CTInfo ctpinfo, s1info;
  if (!(sp[0] && sp[1])) return 0;
  ctpinfo = ctype_info_acq(ctp);
  s1info = ctype_info_acq(s[1]);
  if (ctype_isptr(ctpinfo) || ctype_isrefarray(ctpinfo)) {
    if ((mm == MM_sub || mm == MM_eq || mm == MM_lt || mm == MM_le) &&
	(ctype_isptr(s1info) || ctype_isrefarray(s1info))) {
      if (mm == MM_sub) {  /* Pointer difference. */
	TRef tr;
	CTSize sz;
	CTypeID cid = ctype_cid(ctpinfo);
	sz = crec_ctype_size(J, cts, cid);
	if (sz == 0 || sz == CTSIZE_INVALID || (sz & (sz-1)) != 0)
	  return 0;  /* NYI: integer division. */
	tr = emitir(IRT(IR_SUB, IRT_INTP), sp[0], sp[1]);
	tr = emitir(IRT(IR_BSAR, IRT_INTP), tr, lj_ir_kint(J, lj_fls(sz)));
#if LJ_64
	tr = emitconv(tr, IRT_NUM, IRT_INTP, 0);
#endif
	return tr;
      } else {  /* Pointer comparison (unsigned). */
	/* Assume true comparison. Fixup and emit pending guard later. */
	IROp op = mm == MM_eq ? IR_EQ : mm == MM_lt ? IR_ULT : IR_ULE;
	lj_ir_set(J, IRTG(op, IRT_PTR), sp[0], sp[1]);
	J->postproc = LJ_POST_FIXGUARD;
	return TREF_TRUE;
      }
    }
    if (!((mm == MM_add || mm == MM_sub) && ctype_isnum(s1info)))
      return 0;
  } else if (mm == MM_add && ctype_isnum(ctpinfo) &&
	     (ctype_isptr(s1info) || ctype_isrefarray(s1info))) {
    TRef tr = sp[0]; sp[0] = sp[1]; sp[1] = tr;  /* Swap pointer and index. */
    ctp = s[1];
    ctpinfo = s1info;
  } else {
    return 0;
  }
  {
    TRef tr = sp[1];
    IRType t = tref_type(tr);
    CTSize sz;
    CTypeID id;
    CTypeID cid = ctype_cid(ctpinfo);
    sz = crec_ctype_size(J, cts, cid);
    if (sz == CTSIZE_INVALID)
      return 0;
#if LJ_64
    if (t == IRT_NUM || t == IRT_FLOAT)
      tr = emitconv(tr, IRT_INTP, t, IRCONV_ANY);
    else if (!(t == IRT_I64 || t == IRT_U64))
      tr = emitconv(tr, IRT_INTP, IRT_INT,
		    ((t - IRT_I8) & 1) ? 0 : IRCONV_SEXT);
#else
    if (!tref_typerange(sp[1], IRT_I8, IRT_U32)) {
      tr = emitconv(tr, IRT_INTP, t,
		    (t == IRT_NUM || t == IRT_FLOAT) ? IRCONV_ANY : 0);
    }
#endif
    tr = emitir(IRT(IR_MUL, IRT_INTP), tr, lj_ir_kintp(J, sz));
    tr = emitir(IRT(mm+(int)IR_ADD-(int)MM_add, IRT_PTR), sp[0], tr);
    id = lj_ctype_intern_l(J->L, cts,
			   CTINFO(CT_PTR, CTALIGN_PTR|cid),
			   CTSIZE_PTR);
    return emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, id), tr);
  }
}

/* Record ctype arithmetic metamethods. */
static TRef crec_arith_meta(jit_State *J, TRef *sp, CType **s, CTState *cts,
			    RecordFFData *rd)
{
  TValue metatv;
  cTValue *tv = NULL;
  if (J->base[0]) {
    if (tviscdata(&rd->argv[0])) {
      CTypeID id = argv2cdata(J, J->base[0], &rd->argv[0])->ctypeid;
      id = crec_ctype_ptr_metaid(J, cts, id);
      tv = crec_ctype_metatv(J, cts, &metatv, id, (MMS)rd->data);
    }
    if (!tv && J->base[1] && tviscdata(&rd->argv[1])) {
      CTypeID id = argv2cdata(J, J->base[1], &rd->argv[1])->ctypeid;
      id = crec_ctype_ptr_metaid(J, cts, id);
      tv = crec_ctype_metatv(J, cts, &metatv, id, (MMS)rd->data);
    }
  }
  if (tv) {
    if (tvisfunc(tv)) {
      crec_tailcall(J, rd, tv);
      return 0;
    }  /* NYI: non-function metamethods. */
  } else if ((MMS)rd->data == MM_eq) {  /* Fallback cdata pointer comparison. */
    if (sp[0] && sp[1]) {
      CTInfo info0 = ctype_info_acq(s[0]);
      CTInfo info1 = ctype_info_acq(s[1]);
      if (ctype_isnum(info0) == ctype_isnum(info1)) {
	/* Assume true comparison. Fixup and emit pending guard later. */
	lj_ir_set(J, IRTG(IR_EQ, IRT_PTR), sp[0], sp[1]);
	J->postproc = LJ_POST_FIXGUARD;
	return TREF_TRUE;
      }
    }
    return TREF_FALSE;
  }
  lj_trace_err(J, LJ_TRERR_BADTYPE);
  return 0;
}

void LJ_FASTCALL recff_cdata_arith(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_cts(J->L);
  MMS mm = (MMS)rd->data;
  TRef sp[2];
  CType *s[2];
  CType ctsnap[2];
  MSize i;
  for (i = 0; i < 2; i++) {
    TRef tr = J->base[i];
    CTypeID id = CTID_DOUBLE;
    CType *ct = crec_ctype_rawid(J, cts, id, NULL, &ctsnap[i]);
    if (!tr) {
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    } else if (tref_iscdata(tr)) {
      IRType t;
      CTInfo info;
      id = argv2cdata(J, tr, &rd->argv[i])->ctypeid;
      ct = crec_ctype_rawid(J, cts, id, &id, &ctsnap[i]);
      t = crec_ct2irt_snapshot(J, cts, ct);
      info = ctype_info_acq(ct);
      if (ctype_isptr(info)) {  /* Resolve pointer or reference. */
	tr = emitir(IRT(IR_FLOAD, t), tr, IRFL_CDATA_PTR);
	if (ctype_isref(info)) {
	  ct = crec_ctype_rawchildid(J, cts, ct, &id, &ctsnap[i]);
	  t = crec_ct2irt_snapshot(J, cts, ct);
	  info = ctype_info_acq(ct);
	}
      } else if (t == IRT_I64 || t == IRT_U64) {
	tr = emitir(IRT(IR_FLOAD, t), tr, IRFL_CDATA_INT64);
	lj_needsplit(J);
	goto ok;
      } else if (t == IRT_INT || t == IRT_U32) {
	tr = emitir(IRT(IR_FLOAD, t), tr, IRFL_CDATA_INT);
	if (ctype_isenum(info)) {
	  ct = crec_ctype_rawchildid(J, cts, ct, &id, &ctsnap[i]);
	  info = ctype_info_acq(ct);
	}
	goto ok;
      } else if (ctype_isfunc(info)) {
	tr = emitir(IRT(IR_FLOAD, IRT_PTR), tr, IRFL_CDATA_PTR);
	id = lj_ctype_intern_l(J->L, cts, CTINFO(CT_PTR, CTALIGN_PTR|id),
			       CTSIZE_PTR);
	ct = crec_ctype_rawid(J, cts, id, &id, &ctsnap[i]);
	goto ok;
      } else {
	tr = emitir(IRT(IR_ADD, IRT_PTR), tr, lj_ir_kintp(J, sizeof(GCcdata)));
      }
      if (ctype_isenum(info)) {
	ct = crec_ctype_rawchildid(J, cts, ct, &id, &ctsnap[i]);
	info = ctype_info_acq(ct);
      }
      if (ctype_isnum(info)) {
	if (t == IRT_CDATA) {
	  tr = 0;
	} else {
	  if (t == IRT_I64 || t == IRT_U64) lj_needsplit(J);
	  tr = emitir(IRT(IR_XLOAD, t), tr, 0);
	}
      }
    } else if (tref_isnil(tr)) {
      if (!(mm == MM_len || mm == MM_eq || mm == MM_lt || mm == MM_le))
	lj_trace_err(J, LJ_TRERR_BADTYPE);
      tr = lj_ir_kptr(J, NULL);
      id = CTID_P_VOID;
      ct = crec_ctype_rawid(J, cts, id, NULL, &ctsnap[i]);
    } else if (tref_isinteger(tr)) {
      id = CTID_INT32;
      ct = crec_ctype_rawid(J, cts, id, NULL, &ctsnap[i]);
    } else if (tref_isstr(tr)) {
      TRef tr2 = J->base[1-i];
      CTInfo info;
      id = argv2cdata(J, tr2, &rd->argv[1-i])->ctypeid;
      ct = crec_ctype_rawid(J, cts, id, &id, &ctsnap[i]);
      info = ctype_info_acq(ct);
      if (ctype_isenum(info)) {  /* Match string against enum constant. */
	GCstr *str = strV(&rd->argv[i]);
	CTSize val;
	CTypeID ecid;
	int ok = lj_ctype_enumconst_snapshot(cts, ct, str, &val, &ecid);
	if (ok < 0) {
	  lj_trace_err(J, LJ_TRERR_CTBUSY);
	}
	if (ok > 0) {
	  id = ecid;
	  /* Specialize to the name of the enum constant. */
	  emitir(IRTG(IR_EQ, IRT_STR), tr, lj_ir_kstr(J, str));
	  ct = crec_ctype_rawid(J, cts, id, NULL, &ctsnap[i]);
	  tr = lj_ir_kint(J, (int32_t)val);
	} else {  /* Interpreter will throw or return false. */
	  lj_trace_err(J, LJ_TRERR_BADTYPE);
	}
      } else if (ctype_isptr(info)) {
	tr = emitir(IRT(IR_ADD, IRT_PTR), tr, lj_ir_kintp(J, sizeof(GCstr)));
      } else {
	lj_trace_err(J, LJ_TRERR_BADTYPE);
      }
    } else if (!tref_isnum(tr)) {
      tr = 0;
      id = CTID_P_VOID;
      ct = crec_ctype_rawid(J, cts, id, NULL, &ctsnap[i]);
    }
  ok:
    s[i] = ct;
    sp[i] = tr;
  }
  {
    TRef tr;
    if ((mm == MM_len || mm == MM_concat ||
	 (!(tr = crec_arith_int64(J, sp, s, mm)) &&
	  !(tr = crec_arith_ptr(J, sp, s, mm)))) &&
	!(tr = crec_arith_meta(J, sp, s, cts, rd)))
      return;
    J->base[0] = tr;
    /* Fixup cdata comparisons, too. Avoids some cdata escapes. */
    if (J->postproc == LJ_POST_FIXGUARD && frame_iscont(J->L->base-1) &&
	!irt_isguard(J->guardemit)) {
      const BCIns *pc = frame_contpc(J->L->base-1) - 1;
      if (bc_op(*pc) <= BC_ISNEP) {
	J2TG(J)->tmptv.u64 = (uint64_t)(uintptr_t)pc;
	J->postproc = LJ_POST_FIXCOMP;
      }
    }
  }
}

/* -- C library namespace metamethods ------------------------------------- */

void LJ_FASTCALL recff_clib_index(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  if (tref_isudata(J->base[0]) && tref_isstr(J->base[1]) &&
      lj_udata_udtype_acq(udataV(&rd->argv[0])) == UDTYPE_FFI_CLIB) {
    CLibrary *cl = (CLibrary *)uddata(udataV(&rd->argv[0]));
    GCtab *env = lj_clib_cache_env_acq(cl);
    GCstr *name = strV(&rd->argv[1]);
    CType snap, *ct = &snap;
    CTypeID id;
    cTValue *envtv = env ? lj_tab_getstr(env, name) : NULL;
    cTValue *ctv = lj_clib_cache_get(cl, name);
    TValue tv;
    int ok;
    rd->nres = rd->data;
    ok = lj_ctype_getname_snapshot(cts, name, CLNS_INDEX, &id, &snap, NULL);
    if (ok < 0) {
      lj_trace_err(J, LJ_TRERR_CTBUSY);
    } else if (!ok) {
      id = 0;
    }
    if (envtv && !lj_tv_isnil_acq(envtv)) {
      if (!ctv || lj_tv_isnil_acq(ctv) || !lj_obj_equal(envtv, ctv))
	lj_trace_err(J, LJ_TRERR_NOCACHE);
    }
    if (ctv)
      lj_tv_load_acq(&tv, ctv);
    if (id && ctv && !tvisnil(&tv)) {
      CTInfo info = ctype_info_acq(ct);
      /* Specialize to the symbol name and make the result a constant. */
      emitir(IRTG(IR_EQ, IRT_STR), J->base[1], lj_ir_kstr(J, name));
      if (ctype_isconstval(info)) {
	CTSize size = ctype_size_acq(ct);
	CType childsnap;
	CType *cct = crec_ctype_rawchild(J, cts, ct, &childsnap);
	if (size >= 0x80000000u &&
	    (ctype_info_acq(cct) & CTF_UNSIGNED))
	  J->base[0] = lj_ir_knum(J, (lua_Number)(uint32_t)size);
	else
	  J->base[0] = lj_ir_kint(J, (int32_t)size);
      } else if (ctype_isextern(info)) {
	CTypeID sid = ctype_cid(info);
	CType ctsnap;
	void *sp = *(void **)cdataptr(cdataV(&tv));
	TRef ptr;
	ct = crec_ctype_rawid(J, cts, sid, NULL, &ctsnap);
	if (LJ_64 && !checkptr32(sp))
	  ptr = lj_ir_kintp(J, (uintptr_t)sp);
	else
	  ptr = lj_ir_kptr(J, sp);
	if (rd->data) {
	  J->base[0] = crec_tv_ct(J, ct, sid, ptr);
	} else {
	  J->needsnap = 1;
	  crec_ct_tv(J, ct, ptr, J->base[2], &rd->argv[2]);
	}
      } else {
	J->base[0] = lj_ir_kgc(J, obj2gco(cdataV(&tv)), IRT_CDATA);
      }
    } else {
      lj_trace_err(J, LJ_TRERR_NOCACHE);
    }
  }  /* else: interpreter will throw. */
}

/* -- FFI library functions ----------------------------------------------- */

static TRef crec_toint(jit_State *J, CTState *cts, TRef sp, TValue *sval)
{
  return crec_ct_tv_id(J, cts, CTID_INT32, 0, sp, sval);
}

void LJ_FASTCALL recff_ffi_new(jit_State *J, RecordFFData *rd)
{
  crec_alloc(J, rd, argv2ctype(J, J->base[0], &rd->argv[0]));
}

void LJ_FASTCALL recff_ffi_errno(jit_State *J, RecordFFData *rd)
{
  UNUSED(rd);
  if (J->base[0])
    lj_trace_err(J, LJ_TRERR_NYICALL);
  J->base[0] = lj_ir_call(J, IRCALL_lj_vm_errno);
}

void LJ_FASTCALL recff_ffi_string(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  TRef tr = J->base[0];
  if (tr) {
    TRef trlen = J->base[1];
    if (!tref_isnil(trlen)) {
      trlen = crec_toint(J, cts, trlen, &rd->argv[1]);
      if (!crec_nonneg_const_or_dynamic_len(J, trlen))
	lj_trace_err(J, LJ_TRERR_NYICALL);
      tr = crec_ct_tv_id(J, cts, CTID_P_CVOID, 0, tr, &rd->argv[0]);
    } else {
      tr = crec_ct_tv_id(J, cts, CTID_P_CCHAR, 0, tr, &rd->argv[0]);
      trlen = lj_ir_call(J, IRCALL_lj_ffi_jit_strlen, tr);
    }
    J->base[0] = emitir(IRT(IR_XSNEW, IRT_STR), tr, trlen);
  }  /* else: interpreter will throw. */
}

void LJ_FASTCALL recff_ffi_copy(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  TRef trdst = J->base[0], trsrc = J->base[1], trlen = J->base[2];
  if (trdst && trsrc && (trlen || tref_isstr(trsrc))) {
    trdst = crec_ct_tv_id(J, cts, CTID_P_VOID, 0, trdst, &rd->argv[0]);
    trsrc = crec_ct_tv_id(J, cts, CTID_P_CVOID, 0, trsrc, &rd->argv[1]);
    if (trlen) {
      trlen = crec_toint(J, cts, trlen, &rd->argv[2]);
    } else {
      trlen = emitir(IRTI(IR_FLOAD), J->base[1], IRFL_STR_LEN);
      trlen = emitir(IRTI(IR_ADD), trlen, lj_ir_kint(J, 1));
    }
    rd->nres = 0;
    crec_copy(J, trdst, trsrc, trlen, NULL,
	      crec_nonneg_const_or_dynamic_len(J, trlen));
  }  /* else: interpreter will throw. */
}

void LJ_FASTCALL recff_ffi_fill(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  TRef trdst = J->base[0], trlen = J->base[1], trfill = J->base[2];
  if (trdst && trlen) {
    CTSize step = 1;
    if (tviscdata(&rd->argv[0])) {  /* Get alignment of original destination. */
      CType snap, child;
      CType *ct = crec_ctype_rawref(J, cts, cdataV(&rd->argv[0])->ctypeid,
				    &snap);
      CTInfo info = ctype_info_acq(ct);
      if (ctype_isptr(info)) {
	ct = crec_ctype_rawref(J, cts, ctype_cid(info), &child);
	info = ctype_info_acq(ct);
      }
      step = (1u << ctype_align(info));
    }
    trdst = crec_ct_tv_id(J, cts, CTID_P_VOID, 0, trdst, &rd->argv[0]);
    trlen = crec_toint(J, cts, trlen, &rd->argv[1]);
    if (trfill)
      trfill = crec_toint(J, cts, trfill, &rd->argv[2]);
    else
      trfill = lj_ir_kint(J, 0);
    rd->nres = 0;
    crec_fill(J, trdst, trlen, trfill, step,
	      crec_nonneg_const_or_dynamic_len(J, trlen));
  }  /* else: interpreter will throw. */
}

void LJ_FASTCALL recff_ffi_typeof(jit_State *J, RecordFFData *rd)
{
  if (tref_iscdata(J->base[0])) {
    TRef trid = lj_ir_kint(J, argv2ctype(J, J->base[0], &rd->argv[0]));
    J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA),
			lj_ir_kint(J, CTID_CTYPEID), trid);
  } else {
    setfuncV(J->L, &J->errinfo, J->fn);
    lj_trace_err_info(J, LJ_TRERR_NYIFFU);
  }
}

static int crec_ffi_istype_predefined_ids(CTypeID id1, CTypeID id2)
{
  return id1 > CTID_NONE && id1 <= CTID_CTYPEID &&
	 id2 > CTID_NONE && id2 <= CTID_CTYPEID;
}

static void crec_ffi_istype_snapshot_guard(jit_State *J, CTState *cts,
					   CTypeID id1, CTypeID id2)
{
  CType ct1, ct2, child;
  CTypeID rid1, rid2;
  CTInfo info1, info2;
  CTSize size1, size2;

  if (id1 == id2 || crec_ffi_istype_predefined_ids(id1, id2))
    return;

  crec_ctype_rawrefid(J, cts, id1, &rid1, &ct1);
  crec_ctype_rawrefid(J, cts, id2, &rid2, &ct2);
  info1 = ctype_info_acq(&ct1);
  info2 = ctype_info_acq(&ct2);
  size1 = ctype_size_acq(&ct1);
  size2 = ctype_size_acq(&ct2);

  if (rid1 == rid2)
    return;
  if (ctype_type(info1) == ctype_type(info2) && size1 == size2) {
    if (ctype_ispointer(info1)) {
      crec_ctype_rawchild(J, cts, &ct1, &child);
      crec_ctype_rawchild(J, cts, &ct2, &child);
    }
  } else if (ctype_isstruct(info1) && ctype_isptr(info2)) {
    crec_ctype_rawchild(J, cts, &ct2, &child);
  }
}

void LJ_FASTCALL recff_ffi_istype(jit_State *J, RecordFFData *rd)
{
  CTypeID id1 = argv2ctype(J, J->base[0], &rd->argv[0]);
  if (tref_iscdata(J->base[1])) {
    CTypeID id2 = argv2ctype(J, J->base[1], &rd->argv[1]);
    crec_ffi_istype_snapshot_guard(J, ctype_ctsG(J2G(J)), id1, id2);
    J->postproc = LJ_POST_FIXBOOL;
    J->base[0] = TREF_TRUE;
  } else {
    J->base[0] = TREF_FALSE;
  }
}

void LJ_FASTCALL recff_ffi_abi(jit_State *J, RecordFFData *rd)
{
  if (tref_isstr(J->base[0])) {
    /* Specialize to the ABI string to make the boolean result a constant. */
    emitir(IRTG(IR_EQ, IRT_STR), J->base[0], lj_ir_kstr(J, strV(&rd->argv[0])));
    J->postproc = LJ_POST_FIXBOOL;
    J->base[0] = TREF_TRUE;
  } else {
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  }
}

/* Record ffi.sizeof(), ffi.alignof(), ffi.offsetof(). */
void LJ_FASTCALL recff_ffi_xof(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  int direct = 0;
  CTypeID id = argv2ctype_direct(J, J->base[0], &rd->argv[0], &direct);
  if (rd->data == FF_ffi_sizeof) {
    if (!direct) {
      CType snap;
      CType *ct = crec_ctype_rawref(J, cts, id, &snap);
      if (ctype_isvltype(ctype_info_acq(ct)))
	lj_trace_err(J, LJ_TRERR_BADTYPE);
    }
  } else if (rd->data == FF_ffi_offsetof) {  /* Specialize to the field name. */
    if (!tref_isstr(J->base[1]))
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    emitir(IRTG(IR_EQ, IRT_STR), J->base[1], lj_ir_kstr(J, strV(&rd->argv[1])));
    rd->nres = 3;  /* Just in case. */
  }
  J->postproc = LJ_POST_FIXCONST;
  J->base[0] = J->base[1] = J->base[2] = TREF_NIL;
}

void LJ_FASTCALL recff_ffi_gc(jit_State *J, RecordFFData *rd)
{
  argv2cdata(J, J->base[0], &rd->argv[0]);
  if (!J->base[1])
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  crec_finalizer(J, J->base[0], J->base[1], &rd->argv[1]);
}

/* -- 64 bit bit.* library functions -------------------------------------- */

/* Determine bit operation type from argument type. */
static CTypeID crec_bit64_type(jit_State *J, CTState *cts, cTValue *tv)
{
  if (tviscdata(tv)) {
    CType snap, child;
    CType *ct = crec_ctype_rawref(J, cts, cdataV(tv)->ctypeid, &snap);
    CTInfo info = ctype_info_acq(ct);
    if (ctype_isenum(info)) {
      ct = crec_ctype_snapshot(J, cts, ctype_cid(info), &child);
      info = ctype_info_acq(ct);
    }
    if ((info & (CTMASK_NUM|CTF_BOOL|CTF_FP|CTF_UNSIGNED)) ==
	CTINFO(CT_NUM, CTF_UNSIGNED) && ctype_size_acq(ct) == 8)
      return CTID_UINT64;  /* Use uint64_t, since it has the highest rank. */
    return CTID_INT64;  /* Otherwise use int64_t. */
  }
  return 0;  /* Use regular 32 bit ops. */
}

static TRef crec_bit64_arg(jit_State *J, CType *d, TRef sp, TValue *sval)
{
  if (LJ_UNLIKELY(tref_isstr(sp))) {
    if (lj_strscan_num(strV(sval), sval)) {
      sp = emitir(IRTG(IR_STRTO, IRT_NUM), sp, 0);
    }  /* else: interpreter will throw. */
  }
  return crec_ct_tv(J, d, 0, sp, sval);
}

static TRef crec_bit64_argid(jit_State *J, CTState *cts, CTypeID did,
			     TRef sp, TValue *sval)
{
  CType snap;
  CType *d = crec_ctype_rawid(J, cts, did, NULL, &snap);
  return crec_bit64_arg(J, d, sp, sval);
}

void LJ_FASTCALL recff_bit64_tobit(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  TRef tr = crec_bit64_argid(J, cts, CTID_INT64, J->base[0], &rd->argv[0]);
  if (!tref_isinteger(tr))
    tr = emitconv(tr, IRT_INT, tref_type(tr), 0);
  J->base[0] = tr;
}

int LJ_FASTCALL recff_bit64_unary(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTypeID id = crec_bit64_type(J, cts, &rd->argv[0]);
  if (id) {
    TRef tr = crec_bit64_argid(J, cts, id, J->base[0], &rd->argv[0]);
    tr = emitir(IRT(rd->data, id-CTID_INT64+IRT_I64), tr, 0);
    J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, id), tr);
    return 1;
  }
  return 0;
}

int LJ_FASTCALL recff_bit64_nary(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTypeID id = 0;
  MSize i;
  for (i = 0; J->base[i] != 0; i++) {
    CTypeID aid = crec_bit64_type(J, cts, &rd->argv[i]);
    if (id < aid) id = aid;  /* Determine highest type rank of all arguments. */
  }
  if (id) {
    CType snap, *ct = crec_ctype_rawid(J, cts, id, NULL, &snap);
    uint32_t ot = IRT(rd->data, id-CTID_INT64+IRT_I64);
    TRef tr = crec_bit64_arg(J, ct, J->base[0], &rd->argv[0]);
    for (i = 1; J->base[i] != 0; i++) {
      TRef tr2 = crec_bit64_arg(J, ct, J->base[i], &rd->argv[i]);
      tr = emitir(ot, tr, tr2);
    }
    J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, id), tr);
    return 1;
  }
  return 0;
}

int LJ_FASTCALL recff_bit64_shift(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTypeID id;
  TRef tsh = 0;
  if (J->base[0] && tref_iscdata(J->base[1])) {
    tsh = crec_bit64_argid(J, cts, CTID_INT64, J->base[1], &rd->argv[1]);
    if (LJ_32 && !tref_isinteger(tsh))
      tsh = emitconv(tsh, IRT_INT, tref_type(tsh), 0);
    J->base[1] = tsh;
  }
  id = crec_bit64_type(J, cts, &rd->argv[0]);
  if (id) {
    TRef tr = crec_bit64_argid(J, cts, id, J->base[0], &rd->argv[0]);
    uint32_t op = rd->data;
    IRType t;
    if (!tsh) tsh = lj_opt_narrow_tobit(J, J->base[1]);
    t = tref_isinteger(tsh) ? IRT_INT : tref_type(tsh);
    if (!(op < IR_BROL ? LJ_TARGET_MASKSHIFT : LJ_TARGET_MASKROT) &&
	!tref_isk(tsh))
      tsh = emitir(IRT(IR_BAND, t), tsh, lj_ir_kint(J, 63));
#ifdef LJ_TARGET_UNIFYROT
    if (op == (LJ_TARGET_UNIFYROT == 1 ? IR_BROR : IR_BROL)) {
      op = LJ_TARGET_UNIFYROT == 1 ? IR_BROL : IR_BROR;
      tsh = emitir(IRT(IR_NEG, t), tsh, tsh);
    }
#endif
    tr = emitir(IRT(op, id-CTID_INT64+IRT_I64), tr, tsh);
    J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, id), tr);
    return 1;
  }
  return 0;
}

TRef recff_bit64_tohex(jit_State *J, RecordFFData *rd, TRef hdr)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTypeID id = crec_bit64_type(J, cts, &rd->argv[0]);
  TRef tr, trsf = J->base[1];
  SFormat sf = (STRFMT_UINT|STRFMT_T_HEX);
  int32_t n;
  if (trsf) {
    CTypeID id2 = 0;
    n = (int32_t)lj_carith_check64(J->L, 2, &id2);
    if (id2)
      trsf = crec_bit64_argid(J, cts, CTID_INT32, trsf, &rd->argv[1]);
    else
      trsf = lj_opt_narrow_tobit(J, trsf);
    emitir(IRTGI(IR_EQ), trsf, lj_ir_kint(J, n));  /* Specialize to n. */
  } else {
    n = id ? 16 : 8;
  }
  if (n < 0) { n = (int32_t)(~n+1u); sf |= STRFMT_F_UPPER; }
  if ((uint32_t)n > 254) n = 254;
  sf |= ((SFormat)((n+1)&255) << STRFMT_SH_PREC);
  if (id) {
    tr = crec_bit64_argid(J, cts, id, J->base[0], &rd->argv[0]);
    if (n < 16)
      tr = emitir(IRT(IR_BAND, IRT_U64), tr,
		  lj_ir_kint64(J, ((uint64_t)1 << 4*n)-1));
  } else {
    tr = lj_opt_narrow_tobit(J, J->base[0]);
    if (n < 8)
      tr = emitir(IRTI(IR_BAND), tr, lj_ir_kint(J, (int32_t)((1u << 4*n)-1)));
    tr = emitconv(tr, IRT_U64, IRT_INT, 0);  /* No sign-extension. */
    lj_needsplit(J);
  }
  return lj_ir_call(J, IRCALL_lj_strfmt_putfxint, hdr, lj_ir_kint(J, sf), tr);
}

/* -- Miscellaneous library functions ------------------------------------- */

void LJ_FASTCALL lj_crecord_tonumber(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CType snap, child;
  CType *ct = crec_ctype_rawref(J, cts, cdataV(&rd->argv[0])->ctypeid,
				&snap);
  CTInfo info = ctype_info_acq(ct);
  if (ctype_isenum(info)) {
    ct = crec_ctype_snapshot(J, cts, ctype_cid(info), &child);
    info = ctype_info_acq(ct);
  }
  if (ctype_isnum(info) || ctype_iscomplex(info)) {
    CTSize size = ctype_size_acq(ct);
    CTypeID did;
    if (ctype_isinteger_or_bool(info) && size <= 4 &&
	!(size == 4 && (info & CTF_UNSIGNED)))
      did = CTID_INT32;
    else
      did = CTID_DOUBLE;
    J->base[0] = crec_ct_tv_id(J, cts, did, 0, J->base[0], &rd->argv[0]);
  } else {
    /* Specialize to the ctype that couldn't be converted. */
    argv2cdata(J, J->base[0], &rd->argv[0]);
    J->base[0] = TREF_NIL;
  }
}

TRef lj_crecord_loadiu64(jit_State *J, TRef tr, cTValue *o)
{
  CTypeID id = argv2cdata(J, tr, o)->ctypeid;
  if (!(id == CTID_INT64 || id == CTID_UINT64))
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  lj_needsplit(J);
  return emitir(IRT(IR_FLOAD, id == CTID_INT64 ? IRT_I64 : IRT_U64), tr,
		IRFL_CDATA_INT64);
}

#if LJ_HASBUFFER
TRef lj_crecord_topcvoid(jit_State *J, TRef tr, cTValue *o)
{
  CTState *cts = ctype_ctsG(J2G(J));
  if (!tref_iscdata(tr)) lj_trace_err(J, LJ_TRERR_BADTYPE);
  return crec_ct_tv_id(J, cts, CTID_P_CVOID, 0, tr, o);
}

TRef lj_crecord_topuint8(jit_State *J, TRef tr)
{
  return emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, CTID_P_UINT8), tr);
}
#endif

#undef IR
#undef emitir
#undef emitconv

#endif
