/*
** Buffer handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_BUF_H
#define _LJ_BUF_H

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_str.h"
#include "lj_tg.h"

/* Resizable string buffers. */

/* The SBuf struct definition is in lj_obj.h:
**   char *w;	Write pointer.
**   char *e;	End pointer.
**   char *b;	Base pointer.
**   MRef L;	lua_State, used for buffer resizing. Extension bits in 3 LSB.
*/

/* Extended string buffer. */
typedef struct SBufExt {
  SBufHeader;
  union {
    GCRef cowref;	/* Copy-on-write object reference. */
    MRef bsb;		/* Borrowed string buffer. */
  };
  char *r;		/* Read pointer. */
  GCRef dict_str;	/* Serialization string dictionary table. */
  GCRef dict_mt;	/* Serialization metatable dictionary table. */
  int depth;		/* Remaining recursion depth. */
} SBufExt;

static LJ_AINLINE char *lj_buf_ptr_load_acq(char *const *p)
{
  /* 06 section 6.6: shared string.buffer pointer snapshot. */
  return (char *)la_loadptr_acq((void *const *)(const void *)p);
}

static LJ_AINLINE void lj_buf_ptr_store_rel(char **p, char *v)
{
  /* 06 section 6.6: publish string.buffer pointer/length change. */
  la_storeptr_rel((void **)(void *)p, v);
}

static LJ_AINLINE char *lj_buf_bptr_acq(const SBuf *sb)
{
  return lj_buf_ptr_load_acq(&sb->b);
}

static LJ_AINLINE char *lj_buf_wptr_acq(const SBuf *sb)
{
  return lj_buf_ptr_load_acq(&sb->w);
}

static LJ_AINLINE char *lj_buf_eptr_acq(const SBuf *sb)
{
  return lj_buf_ptr_load_acq(&sb->e);
}

static LJ_AINLINE char *lj_buf_rptr_acq(const SBufExt *sbx)
{
  return lj_buf_ptr_load_acq((char *const *)&sbx->r);
}

static LJ_AINLINE void lj_buf_bptr_rel(SBuf *sb, char *p)
{
  lj_buf_ptr_store_rel(&sb->b, p);
}

static LJ_AINLINE void lj_buf_wptr_rel(SBuf *sb, char *p)
{
  lj_buf_ptr_store_rel(&sb->w, p);
}

static LJ_AINLINE void lj_buf_eptr_rel(SBuf *sb, char *p)
{
  lj_buf_ptr_store_rel(&sb->e, p);
}

static LJ_AINLINE void lj_buf_rptr_rel(SBufExt *sbx, char *p)
{
  lj_buf_ptr_store_rel(&sbx->r, p);
}

static LJ_AINLINE int lj_buf_ptr_range(char *p, char *b, char *e)
{
  uintptr_t up = (uintptr_t)(void *)p;
  uintptr_t ub = (uintptr_t)(void *)b;
  uintptr_t ue = (uintptr_t)(void *)e;
  return ub <= ue && ub <= up && up <= ue;
}

static LJ_AINLINE void lj_buf_bounds_rel(SBuf *sb, char *b, char *w, char *e)
{
  lj_buf_bptr_rel(sb, b);
  lj_buf_eptr_rel(sb, e);
  lj_buf_wptr_rel(sb, w);
}

static LJ_AINLINE MSize lj_buf_size_acq(const SBuf *sb)
{
  char *b = lj_buf_bptr_acq(sb);
  char *e = lj_buf_eptr_acq(sb);
  uintptr_t ub = (uintptr_t)(void *)b;
  uintptr_t ue = (uintptr_t)(void *)e;
  return ub <= ue ? (MSize)(ue - ub) : 0;
}

static LJ_AINLINE MSize lj_buf_len_acq(const SBuf *sb)
{
  char *b = lj_buf_bptr_acq(sb);
  char *e = lj_buf_eptr_acq(sb);
  char *w = lj_buf_wptr_acq(sb);
  uintptr_t ub = (uintptr_t)(void *)b;
  uintptr_t uw = (uintptr_t)(void *)w;
  return lj_buf_ptr_range(w, b, e) ? (MSize)(uw - ub) : 0;
}

static LJ_AINLINE MSize lj_buf_left_acq(const SBuf *sb)
{
  char *b = lj_buf_bptr_acq(sb);
  char *e = lj_buf_eptr_acq(sb);
  char *w = lj_buf_wptr_acq(sb);
  uintptr_t uw = (uintptr_t)(void *)w;
  uintptr_t ue = (uintptr_t)(void *)e;
  UNUSED(b);
  return lj_buf_ptr_range(w, b, e) ? (MSize)(ue - uw) : 0;
}

static LJ_AINLINE MSize lj_bufx_len_acq(const SBufExt *sbx)
{
  char *b = lj_buf_bptr_acq((const SBuf *)sbx);
  char *e = lj_buf_eptr_acq((const SBuf *)sbx);
  char *r = lj_buf_rptr_acq(sbx);
  char *w = lj_buf_wptr_acq((const SBuf *)sbx);
  uintptr_t ur = (uintptr_t)(void *)r;
  uintptr_t uw = (uintptr_t)(void *)w;
  return lj_buf_ptr_range(r, b, e) && lj_buf_ptr_range(w, b, e) && ur <= uw ?
	 (MSize)(uw - ur) : 0;
}

static LJ_AINLINE MSize lj_bufx_slack_acq(const SBufExt *sbx)
{
  char *b = lj_buf_bptr_acq((const SBuf *)sbx);
  char *e = lj_buf_eptr_acq((const SBuf *)sbx);
  char *r = lj_buf_rptr_acq(sbx);
  uintptr_t ub = (uintptr_t)(void *)b;
  uintptr_t ur = (uintptr_t)(void *)r;
  return lj_buf_ptr_range(r, b, e) ? (MSize)(ur - ub) : 0;
}

static LJ_AINLINE const char *lj_bufx_data_acq(const SBufExt *sbx,
					       MSize *lenp)
{
  char *b = lj_buf_bptr_acq((const SBuf *)sbx);
  char *e = lj_buf_eptr_acq((const SBuf *)sbx);
  char *r = lj_buf_rptr_acq(sbx);
  char *w = lj_buf_wptr_acq((const SBuf *)sbx);
  uintptr_t ur = (uintptr_t)(void *)r;
  uintptr_t uw = (uintptr_t)(void *)w;
  if (lj_buf_ptr_range(r, b, e) && lj_buf_ptr_range(w, b, e) && ur <= uw) {
    *lenp = (MSize)(uw - ur);
    return r ? r : "";
  }
  *lenp = 0;
  return "";
}

#define sbufsz(sb)		lj_buf_size_acq((const SBuf *)(sb))
#define sbuflen(sb)		lj_buf_len_acq((const SBuf *)(sb))
#define sbufleft(sb)		lj_buf_left_acq((const SBuf *)(sb))
#define sbufxlen(sbx)		lj_bufx_len_acq((const SBufExt *)(sbx))
#define sbufxslack(sbx)		lj_bufx_slack_acq((const SBufExt *)(sbx))

#define SBUF_MASK_FLAG		(7)
#define SBUF_MASK_L		(~(GCSize)SBUF_MASK_FLAG)
#define SBUF_FLAG_EXT		1	/* Extended string buffer. */
#define SBUF_FLAG_COW		2	/* Copy-on-write buffer. */
#define SBUF_FLAG_BORROW	4	/* Borrowed string buffer. */

#define sbufL(sb) \
  ((lua_State *)(void *)(uintptr_t)(mrefu((sb)->L) & SBUF_MASK_L))
#define setsbufL(sb, l)		(setmref((sb)->L, (l)))
#define setsbufXL(sb, l, flag) \
  (setmrefu((sb)->L, (GCSize)(uintptr_t)(void *)(l) + (flag)))
#define setsbufXL_(sb, l) \
  (setmrefu((sb)->L, (GCSize)(uintptr_t)(void *)(l) | (mrefu((sb)->L) & SBUF_MASK_FLAG)))

#define sbufflag(sb)		(mrefu((sb)->L))
#define sbufisext(sb)		(sbufflag((sb)) & SBUF_FLAG_EXT)
#define sbufiscow(sb)		(sbufflag((sb)) & SBUF_FLAG_COW)
#define sbufisborrow(sb)	(sbufflag((sb)) & SBUF_FLAG_BORROW)
#define sbufiscoworborrow(sb)	(sbufflag((sb)) & (SBUF_FLAG_COW|SBUF_FLAG_BORROW))
#define sbufX(sb) \
  (lj_assertG_(G(sbufL(sb)), sbufisext(sb), "not an SBufExt"), (SBufExt *)(sb))
#define setsbufflag(sb, flag)	(setmrefu((sb)->L, (flag)))

#define tvisbuf(o) \
  (LJ_HASBUFFER && tvisudata(o) && \
   lj_udata_udtype_acq(udataV(o)) == UDTYPE_BUFFER)
#define bufV(o)		check_exp(tvisbuf(o), ((SBufExt *)uddata(udataV(o))))

/* Buffer management */
LJ_FUNC char *LJ_FASTCALL lj_buf_need2(SBuf *sb, MSize sz);
LJ_FUNC char *LJ_FASTCALL lj_buf_more2(SBuf *sb, MSize sz);
LJ_FUNC void LJ_FASTCALL lj_buf_shrink(lua_State *L, SBuf *sb);
LJ_FUNC char * LJ_FASTCALL lj_buf_tmp(lua_State *L, MSize sz);
#if LJ_HASJIT
LJ_FUNC SBuf * LJ_FASTCALL lj_buf_tmp_reset(lua_State *L);
LJ_FUNC int32_t LJ_FASTCALL lj_buf_len_tg_forjit(SBuf *sb);
#endif

static LJ_AINLINE void lj_buf_init(lua_State *L, SBuf *sb)
{
  setsbufL(sb, L);
  lj_buf_bounds_rel(sb, NULL, NULL, NULL);
}

static LJ_AINLINE void lj_buf_reset(SBuf *sb)
{
  lj_buf_wptr_rel(sb, lj_buf_bptr_acq(sb));
}

static LJ_AINLINE SBuf *lj_buf_tmp_(lua_State *L)
{
  SBuf *sb = &L2TG(L)->tmpbuf;
  setsbufL(sb, L);
  lj_buf_reset(sb);
  return sb;
}

static LJ_AINLINE void lj_buf_free(global_State *g, SBuf *sb)
{
  lj_assertG(!sbufisext(sb), "bad free of SBufExt");
  lj_mem_free(g, lj_buf_bptr_acq(sb), sbufsz(sb));
}

static LJ_AINLINE char *lj_buf_need(SBuf *sb, MSize sz)
{
  if (LJ_UNLIKELY(sz > sbufsz(sb)))
    return lj_buf_need2(sb, sz);
  return lj_buf_bptr_acq(sb);
}

static LJ_AINLINE char *lj_buf_more(SBuf *sb, MSize sz)
{
  if (LJ_UNLIKELY(sz > sbufleft(sb)))
    return lj_buf_more2(sb, sz);
  return lj_buf_wptr_acq(sb);
}

#if LJ_HASJIT
/* Raw accessors are only for the JIT-proven runtime TG temporary buffer. */
static LJ_AINLINE MSize lj_buf_left_tg(const SBuf *sb)
{
  char *w = sb->w;
  char *e = sb->e;
  uintptr_t uw = (uintptr_t)(void *)w;
  uintptr_t ue = (uintptr_t)(void *)e;
  return uw <= ue ? (MSize)(ue - uw) : 0;
}

static LJ_AINLINE MSize lj_buf_len_tg(const SBuf *sb)
{
  char *b = sb->b;
  char *w = sb->w;
  char *e = sb->e;
  uintptr_t ub = (uintptr_t)(void *)b;
  uintptr_t uw = (uintptr_t)(void *)w;
  return lj_buf_ptr_range(w, b, e) ? (MSize)(uw - ub) : 0;
}

static LJ_AINLINE char *lj_buf_more_tg(SBuf *sb, MSize sz)
{
  if (LJ_UNLIKELY(sz > lj_buf_left_tg(sb)))
    return lj_buf_more2(sb, sz);
  return sb->w;
}

static LJ_AINLINE void lj_buf_wptr_tg(SBuf *sb, char *p)
{
  sb->w = p;
}
#endif

/* Extended buffer management */
static LJ_AINLINE void lj_bufx_init(lua_State *L, SBufExt *sbx)
{
  memset(sbx, 0, sizeof(SBufExt));
  setsbufXL(sbx, L, SBUF_FLAG_EXT);
}

static LJ_AINLINE void lj_bufx_set_borrow(lua_State *L, SBufExt *sbx, SBuf *sb)
{
  char *b = lj_buf_bptr_acq(sb);
  char *e = lj_buf_eptr_acq(sb);
  setsbufXL(sbx, L, SBUF_FLAG_EXT | SBUF_FLAG_BORROW);
  setmref(sbx->bsb, sb);
  lj_buf_rptr_rel(sbx, b);
  lj_buf_bounds_rel((SBuf *)sbx, b, b, e);
}

static LJ_AINLINE void lj_bufx_set_cow(lua_State *L, SBufExt *sbx,
				       const char *p, MSize len)
{
  char *b = (char *)p;
  char *e = (char *)p + len;
  setsbufXL(sbx, L, SBUF_FLAG_EXT | SBUF_FLAG_COW);
  lj_buf_rptr_rel(sbx, b);
  lj_buf_bounds_rel((SBuf *)sbx, b, e, e);
}

static LJ_AINLINE void lj_bufx_reset(SBufExt *sbx)
{
  if (sbufiscow(sbx)) {
    setmrefu(sbx->L, (mrefu(sbx->L) & ~(GCSize)SBUF_FLAG_COW));
    setgcrefnullrel(sbx->cowref);
    lj_buf_bptr_rel((SBuf *)sbx, NULL);
    lj_buf_eptr_rel((SBuf *)sbx, NULL);
  }
  {
    char *b = lj_buf_bptr_acq((SBuf *)sbx);
    lj_buf_rptr_rel(sbx, b);
    lj_buf_wptr_rel((SBuf *)sbx, b);
  }
}

static LJ_AINLINE void lj_bufx_free(lua_State *L, SBufExt *sbx)
{
  if (!sbufiscoworborrow(sbx))
    lj_mem_free(G(L), lj_buf_bptr_acq((SBuf *)sbx), sbufsz(sbx));
  setsbufXL(sbx, L, SBUF_FLAG_EXT);
  setgcrefnullrel(sbx->cowref);
  lj_buf_rptr_rel(sbx, NULL);
  lj_buf_bounds_rel((SBuf *)sbx, NULL, NULL, NULL);
}

#if LJ_HASBUFFER && LJ_HASJIT
LJ_FUNC void lj_bufx_set(SBufExt *sbx, const char *p, MSize len, GCobj *o);
LJ_FUNC int32_t LJ_FASTCALL lj_bufx_len_forjit(SBufExt *sbx);
LJ_FUNC GCstr *LJ_FASTCALL lj_bufx_tostr_forjit(lua_State *L, SBufExt *sbx);
#if LJ_HASFFI
LJ_FUNC MSize LJ_FASTCALL lj_bufx_more(SBufExt *sbx, MSize sz);
#endif
#endif

/* Low-level buffer put operations */
LJ_FUNC SBuf *lj_buf_putmem(SBuf *sb, const void *q, MSize len);
#if LJ_HASJIT || LJ_HASFFI
LJ_FUNC SBuf * LJ_FASTCALL lj_buf_putchar(SBuf *sb, int c);
#endif
LJ_FUNC SBuf * LJ_FASTCALL lj_buf_putstr(SBuf *sb, GCstr *s);
#if LJ_HASJIT
LJ_FUNC SBuf *lj_buf_putmem_tg(SBuf *sb, const void *q, MSize len);
LJ_FUNC SBuf * LJ_FASTCALL lj_buf_putchar_tg(SBuf *sb, int c);
LJ_FUNC SBuf * LJ_FASTCALL lj_buf_putstr_tg(SBuf *sb, GCstr *s);
#endif

static LJ_AINLINE char *lj_buf_wmem(char *p, const void *q, MSize len)
{
  return (char *)memcpy(p, q, len) + len;
}

static LJ_AINLINE void lj_buf_putb(SBuf *sb, int c)
{
  char *w = lj_buf_more(sb, 1);
  *w++ = (char)c;
  lj_buf_wptr_rel(sb, w);
}

/* High-level buffer put operations */
LJ_FUNCA SBuf * LJ_FASTCALL lj_buf_putstr_reverse(SBuf *sb, GCstr *s);
LJ_FUNCA SBuf * LJ_FASTCALL lj_buf_putstr_lower(SBuf *sb, GCstr *s);
LJ_FUNCA SBuf * LJ_FASTCALL lj_buf_putstr_upper(SBuf *sb, GCstr *s);
LJ_FUNC SBuf *lj_buf_putstr_rep(SBuf *sb, GCstr *s, int32_t rep);
LJ_FUNC SBuf *lj_buf_puttab(SBuf *sb, GCtab *t, GCstr *sep,
			    int32_t i, int32_t e);

/* Miscellaneous buffer operations */
LJ_FUNCA GCstr * LJ_FASTCALL lj_buf_tostr(SBuf *sb);
#if LJ_HASJIT
LJ_FUNCA GCstr * LJ_FASTCALL lj_buf_tostr_tg(SBuf *sb);
#endif
LJ_FUNC GCstr *lj_buf_cat2str(lua_State *L, GCstr *s1, GCstr *s2);
LJ_FUNC uint32_t LJ_FASTCALL lj_buf_ruleb128(const char **pp);

static LJ_AINLINE GCstr *lj_buf_str(lua_State *L, SBuf *sb)
{
  return lj_str_new(L, lj_buf_bptr_acq(sb), sbuflen(sb));
}

#endif
