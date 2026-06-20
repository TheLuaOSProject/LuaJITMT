/*
** Buffer handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_buf_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_strfmt.h"

/* -- Buffer management --------------------------------------------------- */

static void buf_grow(SBuf *sb, MSize sz)
{
  MSize osz = sbufsz(sb), len = sbuflen(sb), nsz = osz;
  char *oldb = lj_buf_bptr_acq(sb);
  char *b;
  GCSize flag;
  if (nsz < LJ_MIN_SBUF) nsz = LJ_MIN_SBUF;
  while (nsz < sz) nsz += nsz;
  flag = sbufflag(sb);
  if ((flag & SBUF_FLAG_COW)) {  /* Copy-on-write semantics. */
    lj_assertG_(G(sbufL(sb)), lj_buf_wptr_acq(sb) == lj_buf_eptr_acq(sb),
		"bad SBuf COW");
    b = (char *)lj_mem_new(sbufL(sb), nsz);
    setsbufflag(sb, flag & ~(GCSize)SBUF_FLAG_COW);
    setgcrefnullrel(sbufX(sb)->cowref);
    memcpy(b, oldb, osz);
  } else {
    b = (char *)lj_mem_realloc(sbufL(sb), oldb, osz, nsz);
  }
  if ((flag & SBUF_FLAG_EXT)) {
    char *oldr = lj_buf_rptr_acq(sbufX(sb));
    lj_buf_rptr_rel(sbufX(sb), oldb ? oldr - oldb + b : b);
  }
  /* Adjust buffer pointers. */
  lj_buf_bounds_rel(sb, b, b + len, b + nsz);
  if ((flag & SBUF_FLAG_BORROW)) {  /* Adjust borrowed buffer pointers. */
    SBuf *bsb = mref(sbufX(sb)->bsb, SBuf);
    lj_buf_bounds_rel(bsb, b, b + len, b + nsz);
  }
}

LJ_NOINLINE char *LJ_FASTCALL lj_buf_need2(SBuf *sb, MSize sz)
{
  lj_assertG_(G(sbufL(sb)), sz > sbufsz(sb), "SBuf overflow");
  if (LJ_UNLIKELY(sz > LJ_MAX_BUF))
    lj_err_mem(sbufL(sb));
  buf_grow(sb, sz);
  return lj_buf_bptr_acq(sb);
}

LJ_NOINLINE char *LJ_FASTCALL lj_buf_more2(SBuf *sb, MSize sz)
{
  if (sbufisext(sb)) {
    SBufExt *sbx = (SBufExt *)sb;
    MSize len = sbufxlen(sbx);
    if (LJ_UNLIKELY(sz > LJ_MAX_BUF || len + sz > LJ_MAX_BUF))
      lj_err_mem(sbufL(sbx));
    if (len + sz > sbufsz(sbx)) {  /* Must grow. */
      buf_grow((SBuf *)sbx, len + sz);
    } else if (sbufiscow(sb) || sbufxslack(sbx) < (sbufsz(sbx) >> 3)) {
      /* Also grow to avoid excessive compactions, if slack < size/8. */
      buf_grow((SBuf *)sbx, sbuflen(sbx) + sz);  /* Not sbufxlen! */
      return lj_buf_wptr_acq((SBuf *)sbx);
    }
    {
      char *b = lj_buf_bptr_acq((SBuf *)sbx);
      char *r = lj_buf_rptr_acq(sbx);
      if (r != b) {  /* Compact by moving down. */
	memmove(b, r, len);
	lj_buf_rptr_rel(sbx, b);
	lj_buf_wptr_rel((SBuf *)sbx, b + len);
      }
      lj_assertG_(G(sbufL(sbx)), len + sz <= sbufsz(sbx), "bad SBuf compact");
    }
  } else {
    MSize len = sbuflen(sb);
    lj_assertG_(G(sbufL(sb)), sz > sbufleft(sb), "SBuf overflow");
    if (LJ_UNLIKELY(sz > LJ_MAX_BUF || len + sz > LJ_MAX_BUF))
      lj_err_mem(sbufL(sb));
    buf_grow(sb, len + sz);
  }
  return lj_buf_wptr_acq(sb);
}

void LJ_FASTCALL lj_buf_shrink(lua_State *L, SBuf *sb)
{
  char *b = lj_buf_bptr_acq(sb);
  MSize osz = sbufsz(sb);
  if (osz > 2*LJ_MIN_SBUF) {
    b = lj_mem_realloc(L, b, osz, (osz >> 1));
    /* Not supposed to keep data across shrinks. */
    lj_buf_bounds_rel(sb, b, b, b + (osz >> 1));
  }
  lj_assertG_(G(sbufL(sb)), !sbufisext(sb), "YAGNI shrink SBufExt");
}

char * LJ_FASTCALL lj_buf_tmp(lua_State *L, MSize sz)
{
  SBuf *sb = &L2TG(L)->tmpbuf;
  setsbufL(sb, L);
  return lj_buf_need(sb, sz);
}

#if LJ_HASBUFFER && LJ_HASJIT
void lj_bufx_set(SBufExt *sbx, const char *p, MSize len, GCobj *ref)
{
  lua_State *L = sbufL(sbx);
  lj_bufx_free(L, sbx);
  lj_bufx_set_cow(L, sbx, p, len);
  setgcrefrel(sbx->cowref, ref);
  lj_gc_pubobjobj(L, (GCudata *)sbx - 1, ref);
}

#if LJ_HASFFI
MSize LJ_FASTCALL lj_bufx_more(SBufExt *sbx, MSize sz)
{
  lj_buf_more((SBuf *)sbx, sz);
  return sbufleft(sbx);
}
#endif
#endif

/* -- Low-level buffer put operations ------------------------------------- */

SBuf *lj_buf_putmem(SBuf *sb, const void *q, MSize len)
{
  char *w = lj_buf_more(sb, len);
  w = lj_buf_wmem(w, q, len);
  lj_buf_wptr_rel(sb, w);
  return sb;
}

#if LJ_HASJIT || LJ_HASFFI
static LJ_NOINLINE SBuf * LJ_FASTCALL lj_buf_putchar2(SBuf *sb, int c)
{
  char *w = lj_buf_more2(sb, 1);
  *w++ = (char)c;
  lj_buf_wptr_rel(sb, w);
  return sb;
}

SBuf * LJ_FASTCALL lj_buf_putchar(SBuf *sb, int c)
{
  char *w = lj_buf_wptr_acq(sb);
  if (LJ_LIKELY(w < lj_buf_eptr_acq(sb))) {
    *w++ = (char)c;
    lj_buf_wptr_rel(sb, w);
    return sb;
  }
  return lj_buf_putchar2(sb, c);
}
#endif

SBuf * LJ_FASTCALL lj_buf_putstr(SBuf *sb, GCstr *s)
{
  MSize len = s->len;
  char *w = lj_buf_more(sb, len);
  w = lj_buf_wmem(w, strdata(s), len);
  lj_buf_wptr_rel(sb, w);
  return sb;
}

/* -- High-level buffer put operations ------------------------------------ */

SBuf * LJ_FASTCALL lj_buf_putstr_reverse(SBuf *sb, GCstr *s)
{
  MSize len = s->len;
  char *w = lj_buf_more(sb, len), *e = w+len;
  const char *q = strdata(s)+len-1;
  while (w < e)
    *w++ = *q--;
  lj_buf_wptr_rel(sb, w);
  return sb;
}

SBuf * LJ_FASTCALL lj_buf_putstr_lower(SBuf *sb, GCstr *s)
{
  MSize len = s->len;
  char *w = lj_buf_more(sb, len), *e = w+len;
  const char *q = strdata(s);
  for (; w < e; w++, q++) {
    uint32_t c = *(unsigned char *)q;
#if LJ_TARGET_PPC
    *w = c + ((c >= 'A' && c <= 'Z') << 5);
#else
    if (c >= 'A' && c <= 'Z') c += 0x20;
    *w = c;
#endif
  }
  lj_buf_wptr_rel(sb, w);
  return sb;
}

SBuf * LJ_FASTCALL lj_buf_putstr_upper(SBuf *sb, GCstr *s)
{
  MSize len = s->len;
  char *w = lj_buf_more(sb, len), *e = w+len;
  const char *q = strdata(s);
  for (; w < e; w++, q++) {
    uint32_t c = *(unsigned char *)q;
#if LJ_TARGET_PPC
    *w = c - ((c >= 'a' && c <= 'z') << 5);
#else
    if (c >= 'a' && c <= 'z') c -= 0x20;
    *w = c;
#endif
  }
  lj_buf_wptr_rel(sb, w);
  return sb;
}

SBuf *lj_buf_putstr_rep(SBuf *sb, GCstr *s, int32_t rep)
{
  MSize len = s->len;
  if (rep > 0 && len) {
    uint64_t tlen = (uint64_t)rep * len;
    char *w;
    if (LJ_UNLIKELY(tlen > LJ_MAX_STR))
      lj_err_mem(sbufL(sb));
    w = lj_buf_more(sb, (MSize)tlen);
    if (len == 1) {  /* Optimize a common case. */
      uint32_t c = strdata(s)[0];
      do { *w++ = c; } while (--rep > 0);
    } else {
      const char *e = strdata(s) + len;
      do {
	const char *q = strdata(s);
	do { *w++ = *q++; } while (q < e);
      } while (--rep > 0);
    }
    lj_buf_wptr_rel(sb, w);
  }
  return sb;
}

SBuf *lj_buf_puttab(SBuf *sb, GCtab *t, GCstr *sep, int32_t i, int32_t e)
{
  MSize seplen = sep ? sep->len : 0;
  if (i <= e) {
    for (;;) {
      cTValue *o = lj_tab_getint(t, i);
      TValue tv;
      char *w;
      if (!o) {
      badtype:  /* Error: bad element type. */
	lj_buf_wptr_rel(sb, (char *)(intptr_t)i);  /* Store failing index. */
	return NULL;
      }
      lj_tv_load_acq(&tv, o);
      if (tvisstr(&tv)) {
	MSize len = strV(&tv)->len;
	w = lj_buf_wmem(lj_buf_more(sb, len + seplen), strVdata(&tv), len);
      } else if (tvisint(&tv)) {
	w = lj_strfmt_wint(lj_buf_more(sb, STRFMT_MAXBUF_INT+seplen), intV(&tv));
      } else if (tvisnum(&tv)) {
	w = lj_buf_more(lj_strfmt_putfnum(sb, STRFMT_G14, numV(&tv)), seplen);
      } else {
	goto badtype;
      }
      if (i++ == e) {
	lj_buf_wptr_rel(sb, w);
	break;
      }
      if (seplen) w = lj_buf_wmem(w, strdata(sep), seplen);
      lj_buf_wptr_rel(sb, w);
    }
  }
  return sb;
}

/* -- Miscellaneous buffer operations ------------------------------------- */

GCstr * LJ_FASTCALL lj_buf_tostr(SBuf *sb)
{
  return lj_str_new(sbufL(sb), lj_buf_bptr_acq(sb), sbuflen(sb));
}

/* Concatenate two strings. */
GCstr *lj_buf_cat2str(lua_State *L, GCstr *s1, GCstr *s2)
{
  MSize len1 = s1->len, len2 = s2->len;
  char *buf = lj_buf_tmp(L, len1 + len2);
  memcpy(buf, strdata(s1), len1);
  memcpy(buf+len1, strdata(s2), len2);
  return lj_str_new(L, buf, len1 + len2);
}

/* Read ULEB128 from buffer. */
uint32_t LJ_FASTCALL lj_buf_ruleb128(const char **pp)
{
  const uint8_t *w = (const uint8_t *)*pp;
  uint32_t v = *w++;
  if (LJ_UNLIKELY(v >= 0x80)) {
    int sh = 0;
    v &= 0x7f;
    do { v |= ((*w & 0x7f) << (sh += 7)); } while (*w++ >= 0x80);
  }
  *pp = (const char *)w;
  return v;
}
