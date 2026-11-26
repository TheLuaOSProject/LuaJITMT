/*
** Buffer handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_buf_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_state.h"
#include "lj_strfmt.h"

/* -- Buffer management --------------------------------------------------- */

static void buf_grow(SBuf *sb, MSize sz)
{
  MSize osz = sbufsz(sb), len = sbuflen(sb), nsz = osz;
  MSize slack = sbufisext(sb) ? sbufxslack(sbufX(sb)) : 0;
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
    memcpy(b, oldb, osz);
    setsbufflag(sb, flag & ~(GCSize)SBUF_FLAG_COW);
    lj_bufx_cowref_clear_rel(sbufX(sb));
  } else {
    b = (char *)lj_mem_realloc(sbufL(sb), oldb, osz, nsz);
  }
  lj_buf_bounds_rel(sb, b, b + len, b + nsz);
  if ((flag & SBUF_FLAG_EXT))
    lj_buf_rptr_rel(sbufX(sb), b + slack);
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
	char *w = lj_buf_wptr_acq((SBuf *)sbx);
	/* A consumer can have advanced since the earlier size check. */
	len = lj_buf_ptr_range(r, b, w) ? (MSize)(w - r) : 0;
	if (len) memmove(b, r, len);
	lj_buf_wptr_rel((SBuf *)sbx, b + len);
	lj_buf_rptr_rel(sbx, b);
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

#if LJ_HASJIT
SBuf * LJ_FASTCALL lj_buf_tmp_reset(lua_State *L)
{
  return lj_buf_tmp_(L);
}
#endif

#if LJ_HASBUFFER
static SBufExt *bufx_userdata(lua_State *L, cTValue *o, LJGC2Lease *lease,
			     int retry)
{
  GCudata *ud;
  int status;
  if (!lease)
    return NULL;
  memset(lease, 0, sizeof(*lease));
  if (!tvisudata(o))
    return NULL;
  /*
  ** A racy snapshot can retain a userdata tag after the cell was retired and
  ** reused. Validate the cell and the fixed buffer payload shape before reading
  ** SBufExt fields; plain tvisbuf() only checks the published udtype byte.
  */
  for (;;) {
    status = lj_gc2_tv_lease_acquire(G(L), o, lease);
    if (status != LJ_GC2_TV_EDGE_RETRY || !retry)
      break;
    lj_tab_wait_l(L);
  }
  if (status != LJ_GC2_TV_EDGE_VALID)
    return NULL;
  ud = udataV(o);
  if ((MSize)la_load32_acq(&ud->len) != sizeof(SBufExt) ||
      lj_udata_udtype_acq(ud) != UDTYPE_BUFFER) {
    lj_gc2_lease_release(lease);
    return NULL;
  }
  return (SBufExt *)uddata(ud);
}

int lj_bufx_isbuf(lua_State *L, cTValue *o)
{
  LJGC2Lease lease;
  /* Storage contention affects the copied contents, not the operand type. */
  if (!bufx_userdata(L, o, &lease, 1))
    return 0;
  lj_gc2_lease_release(&lease);
  return 1;
}

typedef struct BufReadSnapshot {
  char *b;
  char *e;
  char *r;
  char *w;
  GCobj *ref;
  GCSize flags;
  uint64_t generation;
} BufReadSnapshot;

static void bufx_snapshot(const SBufExt *sbx, BufReadSnapshot *s)
{
  s->flags = sbufflag(sbx) & SBUF_MASK_FLAG;
  s->b = lj_buf_bptr_acq((const SBuf *)sbx);
  s->e = lj_buf_eptr_acq((const SBuf *)sbx);
  {
    la_u128 cursor = la_load128_acq(&sbx->cursor);
    s->r = (char *)(uintptr_t)cursor.lo;
    s->generation = cursor.hi;
  }
  s->w = lj_buf_wptr_acq((const SBuf *)sbx);
  s->ref = lj_bufx_cowref_acq(sbx);
}

static int bufx_snapshot_same(const BufReadSnapshot *a, const BufReadSnapshot *b)
{
  return a->flags == b->flags && a->b == b->b && a->e == b->e &&
    a->r == b->r && a->w == b->w && a->ref == b->ref &&
    a->generation == b->generation;
}

static int bufx_snapshot_valid(const BufReadSnapshot *s, MSize *lenp)
{
  uintptr_t ur = (uintptr_t)(void *)s->r;
  uintptr_t uw = (uintptr_t)(void *)s->w;
  if (!(s->flags & SBUF_FLAG_EXT) ||
      !lj_buf_ptr_range(s->r, s->b, s->e) ||
      !lj_buf_ptr_range(s->w, s->b, s->e) || ur > uw || uw - ur > LJ_MAX_BUF)
    return 0;
  *lenp = (MSize)(uw - ur);
  return 1;
}

static int bufx_read(lua_State *L, cTValue *o, const char **pp,
		     MSize *lenp, LJGC2Lease *holdp, la_u128 *cursor)
{
  LJGC2Lease bodylease;
  SBufExt *sbx;
  uint32_t attempt;
  if (holdp)
    memset(holdp, 0, sizeof(*holdp));
  sbx = bufx_userdata(L, o, &bodylease, 0);
  if (!sbx)
    return 0;

  /* Individual SBuf fields are atomic, but they are not one transaction.
  ** Admit the storage selected by a first snapshot, then require the complete
  ** descriptor (including COW identity) to match a second snapshot. If a
  ** writer moved away and back, the retained allocation at the repeated
  ** address is still the exact storage named by the accepted descriptor. */
  for (attempt = 0; attempt < 8u; attempt++) {
    LJGC2Lease storage;
    BufReadSnapshot first, second;
    MSize len;
    uint32_t gct = 0;
    memset(&storage, 0, sizeof(storage));
    bufx_snapshot(sbx, &first);
    if (!bufx_snapshot_valid(&first, &len))
      continue;
    if (len != 0) {
      if (first.flags & SBUF_FLAG_COW) {
	/* Stock string.buffer COW storage is owned by a string or cdata edge.
	** A cdata pointer may intentionally name external FFI memory; retaining
	** the cdata preserves runtime identity but cannot validate user memory. */
	if (first.ref == NULL ||
	    lj_gc2_obj_lease_acquire(G(L), first.ref, 0, &gct, &storage) < 0)
	  continue;
	if (gct != (uint32_t)~LJ_TSTR
#if LJ_HASFFI
	    && gct != (uint32_t)~LJ_TCDATA
#endif
	   ) {
	  lj_gc2_lease_release(&storage);
	  continue;
	}
      } else if (first.b == NULL ||
		 !lj_gc2_storage_lease_try(G(L), first.b,
		   (uintptr_t)first.w - (uintptr_t)first.b, &storage, NULL)) {
	continue;
      }
    }
    bufx_snapshot(sbx, &second);
    if (!bufx_snapshot_same(&first, &second) ||
	!bufx_snapshot_valid(&second, &len)) {
      lj_gc2_lease_release(&storage);
      continue;
    }
    if ((second.flags & SBUF_FLAG_COW) &&
	gct == (uint32_t)~LJ_TSTR) {
      GCstr *s = gco2str(second.ref);
      if (second.b != (char *)strdata(s) ||
	  (uintptr_t)(void *)second.e - (uintptr_t)(void *)second.b !=
	    (uintptr_t)s->len) {
	lj_gc2_lease_release(&storage);
	continue;
      }
    }
    if (pp) *pp = second.r ? second.r : "";
    if (lenp) *lenp = len;
    if (cursor) {
      cursor->lo = (uintptr_t)second.r;
      cursor->hi = second.generation;
    }
    lj_gc2_lease_release(&bodylease);
    if (holdp) {
      *holdp = storage;
      memset(&storage, 0, sizeof(storage));
    }
    lj_gc2_lease_release(&storage);
    return 1;
  }

  lj_gc2_lease_release(&bodylease);
  return 0;
}

MSize lj_bufx_readlen(lua_State *L, cTValue *o)
{
  MSize len = 0;
  (void)bufx_read(L, o, NULL, &len, NULL, NULL);
  return len;
}

/* Copy one shared buffer without a throwing/growing operation while its body
** lease is live. Capacity growth happens after release, then the source is
** resnapshotted and re-admitted. The loop is bounded so a racing writer cannot
** turn concatenation into a wait; an unstable buffer contributes an empty
** racy snapshot, which is memory-safe and within the allowed racy semantics. */
static MSize bufx_copy(lua_State *L, SBuf *sb, cTValue *o, MSize limit,
                       la_u128 *cursor)
{
  TValue source;
  uint32_t attempt;
  lj_tv_load_acq(&source, o);
  for (attempt = 0; attempt < 8u; attempt++) {
    LJGC2Lease lease;
    const char *p;
    char *w;
    MSize len;
    memset(&lease, 0, sizeof(lease));
    if (!bufx_read(L, &source, &p, &len, &lease, cursor))
      return 0;
    if (len > limit) len = limit;
    if (len > sbufleft(sb)) {
      lj_gc2_lease_release(&lease);
      (void)lj_buf_more(sb, len);  /* May throw only with no source lease. */
      continue;
    }
    if (len) {
      w = lj_buf_wptr_acq(sb);
      lj_buf_wptr_rel(sb, lj_buf_wmem(w, p, len));
    }
    lj_gc2_lease_release(&lease);
    return len;
  }
  return 0;
}

void lj_bufx_putbuf(lua_State *L, SBuf *sb, cTValue *o)
{
  (void)bufx_copy(L, sb, o, LJ_MAX_BUF, NULL);
}

static int bufx_advance(SBufExt *sbx, la_u128 cursor, MSize n)
{
  la_u128 next = {cursor.lo + n, cursor.hi + 1};
  return la_cas128(&sbx->cursor, &cursor, next);
}

void LJ_FASTCALL lj_bufx_skip(SBufExt *sbx, MSize n)
{
  while (n) {
    BufReadSnapshot view;
    la_u128 cursor;
    MSize len;
    bufx_snapshot(sbx, &view);
    if (!bufx_snapshot_valid(&view, &len) || !len)
      return;
    cursor.lo = (uintptr_t)view.r;
    cursor.hi = view.generation;
    if (bufx_advance(sbx, cursor, n < len ? n : len))
      return;
  }
}

/* The caller supplies the current temporary buffer, already reset. Generated
** code expresses that reset in IR so adjacent concatenations cannot reuse
** the bytes overwritten here. The source may exist only in a native register. */
static GCstr *bufx_string(SBuf *sb, SBufExt *src, MSize limit, int consume)
{
  lua_State *L = sbufL(sb);
  TGState *tg = L2TG(L);
  TValue source, *anchor;
  uint32_t index;
  GCstr *s;
  setgcVraw(&source, obj2gco((GCudata *)src - 1), LJ_TUDATA);
  anchor = lj_tg_root_anchor_push(L, tg, &source, &index);
  if (LJ_UNLIKELY(!anchor)) lj_err_mem(L);
  lj_gc_pubroot(L, anchor);
  for (;;) {
    la_u128 cursor;
    MSize n = bufx_copy(L, sb, anchor, limit, consume ? &cursor : NULL);
    s = lj_buf_str(L, sb);
    /* Intern before consuming so an allocation error leaves the bytes in
    ** place. A reset, even to the same address, invalidates the old cursor. */
    if (!n || !consume || bufx_advance(src, cursor, n))
      break;
    lj_buf_reset(sb);
  }
  lj_tg_root_anchor_pop(tg, index);
  return s;
}

GCstr *LJ_FASTCALL lj_bufx_tostr(SBuf *sb, SBufExt *src)
{
  return bufx_string(sb, src, LJ_MAX_BUF, 0);
}

GCstr *lj_bufx_get(SBuf *scratch, SBufExt *src, MSize n)
{
  return bufx_string(scratch, src, n, 1);
}
#endif

#if LJ_HASBUFFER && LJ_HASJIT
void lj_bufx_set(SBufExt *sbx, const char *p, MSize len, GCobj *ref)
{
  lua_State *L = sbufL(sbx);
  lj_bufx_free(L, sbx);
  lj_bufx_set_cow(L, sbx, p, len);
  lj_bufx_cowref_rel(sbx, ref);
  lj_gc_pubobjobj(L, (GCudata *)sbx - 1, ref);
}

void LJ_FASTCALL lj_bufx_reset_forjit(SBufExt *sbx)
{
  lj_bufx_reset(sbx);
}

void LJ_FASTCALL lj_bufx_putstr_forjit(lua_State *L, SBufExt *sbx, GCstr *str)
{
  setsbufXL_(sbx, L);
  lj_buf_putstr((SBuf *)sbx, str);
  lj_gc_check(L);
}

int32_t LJ_FASTCALL lj_bufx_len_forjit(SBufExt *sbx)
{
  return (int32_t)sbufxlen(sbx);
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

#if LJ_HASJIT
SBuf *lj_buf_putmem_tg(SBuf *sb, const void *q, MSize len)
{
  if (len) {
    char *w = lj_buf_more_tg(sb, len);
    w = lj_buf_wmem(w, q, len);
    lj_buf_wptr_tg(sb, w);
  }
  return sb;
}

SBuf * LJ_FASTCALL lj_buf_putchar_tg(SBuf *sb, int c)
{
  char *w = lj_buf_more_tg(sb, 1);
  *w++ = (char)c;
  lj_buf_wptr_tg(sb, w);
  return sb;
}

SBuf * LJ_FASTCALL lj_buf_putstr_tg(SBuf *sb, GCstr *s)
{
  MSize len = s->len;
  if (len) {
    char *w = lj_buf_more_tg(sb, len);
    w = lj_buf_wmem(w, strdata(s), len);
    lj_buf_wptr_tg(sb, w);
  }
  return sb;
}
#endif

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

static LJ_AINLINE int buf_tv_on_stack(lua_State *L, cTValue *tv)
{
  uintptr_t p, lo, hi;
  if (!L || !tv)
    return 0;
  p = (uintptr_t)(const void *)tv;
  lo = (uintptr_t)(const void *)tvref(L->stack);
  hi = (uintptr_t)(const void *)tvref(L->maxstack);
  return p >= lo && p < hi;
}

static SBuf *buf_puttab_impl(SBuf *sb, cTValue *tabroot, GCtab *trusted_t,
			     GCstr *sep, int32_t i, int32_t e,
			     int32_t *badtype)
{
  lua_State *L = sbufL(sb);
  TGState *tg = L2TG(L);
  int tabstack = buf_tv_on_stack(L, tabroot);
  ptrdiff_t tabofs = tabstack ? savestack(L, tabroot) : 0;
  MSize seplen = sep ? sep->len : 0;
  TValue nilv;
  TValue *resultroot;
  uint32_t rootidx;
  if (badtype)
    *badtype = ~LJ_TNIL;
  if (i <= e) {
    setnilV(&nilv);
    resultroot = lj_tg_root_anchor_push(L, tg, &nilv, &rootidx);
    for (;;) {
      cTValue *curtab = tabstack ? restorestack(L, tabofs) : tabroot;
      char *w;
      resultroot = lj_tg_root_anchor_slot_acq(tg, rootidx);
      if (curtab) {
	(void)lj_tab_getinttv_rooted(L, curtab, i, resultroot);
      } else {
	TValue keytv;
	setintV(&keytv, i);
	(void)lj_tab_gettv_forjit(L, trusted_t, &keytv, resultroot);
      }
      resultroot = lj_tg_root_anchor_slot_acq(tg, rootidx);
      if (tvisstr(resultroot)) {
	MSize len = strV(resultroot)->len;
	w = lj_buf_more(sb, len + seplen);
	resultroot = lj_tg_root_anchor_slot_acq(tg, rootidx);
	w = lj_buf_wmem(w, strVdata(resultroot), len);
      } else if (tvisint(resultroot)) {
	w = lj_strfmt_wint(lj_buf_more(sb, STRFMT_MAXBUF_INT+seplen),
			      intV(resultroot));
      } else if (tvisnum(resultroot)) {
	w = lj_buf_more(lj_strfmt_putfnum(sb, STRFMT_G14,
					 numV(resultroot)), seplen);
      } else {
	if (badtype)
	  *badtype = itypemap(resultroot);
	lj_tg_root_anchor_pop(tg, rootidx);
	lj_buf_wptr_rel(sb, (char *)(intptr_t)i);  /* Store failing index. */
	return NULL;
      }
      if (i++ == e) {
	lj_buf_wptr_rel(sb, w);
	break;
      }
      if (seplen) w = lj_buf_wmem(w, strdata(sep), seplen);
      lj_buf_wptr_rel(sb, w);
    }
    lj_tg_root_anchor_pop(tg, rootidx);
  }
  return sb;
}

SBuf *lj_buf_puttab_rooted(SBuf *sb, cTValue *tabroot, GCstr *sep,
			   int32_t i, int32_t e, int32_t *badtype)
{
  return buf_puttab_impl(sb, tabroot, NULL, sep, i, e, badtype);
}

/* Keep the generated-trace ABI stable. The traced table IR is a naked GCtab
** pointer, but each semantic read still acquires the compatibility helper's
** exact parent lease and publishes its result through the same TG anchor. */
SBuf *lj_buf_puttab(SBuf *sb, GCtab *t, GCstr *sep, int32_t i, int32_t e)
{
  return buf_puttab_impl(sb, NULL, t, sep, i, e, NULL);
}

/* -- Miscellaneous buffer operations ------------------------------------- */

GCstr * LJ_FASTCALL lj_buf_tostr(SBuf *sb)
{
  return lj_str_new(sbufL(sb), lj_buf_bptr_acq(sb), sbuflen(sb));
}

#if LJ_HASJIT
int32_t LJ_FASTCALL lj_buf_len_tg_forjit(SBuf *sb)
{
  return (int32_t)lj_buf_len_tg(sb);
}

GCstr * LJ_FASTCALL lj_buf_tostr_tg(SBuf *sb)
{
  MSize len = lj_buf_len_tg(sb);
  return lj_str_new(sbufL(sb), len ? sb->b : "", len);
}
#endif

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
