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
#include "lj_udata.h"
#include "lj_serialize.h"

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

/* Readers retain the exact arena allocation before inspecting its header.
** Internal writers never overwrite published bytes. A suspended tail writer
** can be overtaken by copying the committed range to a different allocation. */
#define BUFX_WRITING (UINT64_C(1) << 32)
#define BUFX_RESERVED (UINT64_C(2) << 32)

#ifdef LJ_BUF_TEST_HELPERS
static LJBufTestHook buf_test_hook;

void lj_buf_test_set_hook(LJBufTestHook hook)
{
  buf_test_hook = hook;
}

static void bufx_test(lua_State *L, LJBuffer *buf, uint32_t stage)
{
  if (buf_test_hook) buf_test_hook(L, buf, stage);
}
#else
#define bufx_test(L, buf, stage) ((void)0)
#endif

typedef struct BufPin {
  la_u128 range;
  LJBufStorage *store;
  LJGC2Lease header, ref;
} BufPin;

static LJ_AINLINE la_u128 bufx_range(LJBufStorage *store, MSize r, MSize w)
{
  la_u128 range = { (uintptr_t)store, ((uint64_t)w << 32) | r };
  return range;
}

static LJ_AINLINE MSize bufx_r(la_u128 range) { return (MSize)range.hi; }
static LJ_AINLINE MSize bufx_w(la_u128 range) { return (MSize)(range.hi >> 32); }

static LJ_AINLINE int bufx_same(la_u128 a, la_u128 b)
{
  return a.lo == b.lo && a.hi == b.hi;
}

static size_t bufx_bytes(MSize capacity, const GCobj *ref)
{
  return sizeof(LJBufStorage) + (ref ? 0 : (size_t)capacity);
}

static MSize bufx_capacity(MSize old, MSize needed)
{
  MSize size = old < LJ_MIN_SBUF ? LJ_MIN_SBUF : old;
  while (size < needed)
    size = size <= LJ_MAX_BUF / 2 ? 2 * size : LJ_MAX_BUF;
  return size;
}

static LJBufStorage *bufx_alloc(lua_State *L, LJBuffer *buf, MSize capacity,
                                const char *data, GCobj *ref)
{
  LJBufStorage *s = (LJBufStorage *)lj_mem_new_nothrow(L, bufx_bytes(capacity, ref));
  if (s) {
    s->serial = la_add64_rlx(&buf->serial, 1) + 1;
    s->claimed = 0;
    la_store32_rlx(&s->written, 0);
    s->capacity = capacity;
    s->ref = ref;
    s->reftype = ref ? ref->gch.gct : 0;
    s->data = ref ? data : (const char *)(s+1);
  }
  return s;
}

static void bufx_unpin(BufPin *pin)
{
  lj_gc2_lease_release(&pin->ref);
  lj_gc2_lease_release(&pin->header);
}

static int bufx_pin_try(lua_State *L, LJBuffer *buf, int bytes, BufPin *pin)
{
  LJBufStorage *s;
  memset(pin, 0, sizeof(*pin));
  pin->range = la_load128_acq(&buf->range);
  pin->store = s = (LJBufStorage *)(uintptr_t)pin->range.lo;
  if (!s) return 1;
  if (lj_gc2_storage_lease_try(G(L), s, sizeof(*s), &pin->header, NULL) &&
      bufx_same(pin->range, la_load128_acq(&buf->range)) &&
      la_load32_acq(&s->written) > bufx_w(pin->range)) {
    TValue ref;
    if (!bytes || !s->ref || bufx_r(pin->range) == bufx_w(pin->range))
      return 1;
    setgcVraw(&ref, s->ref, ~s->reftype);
    if ((s->reftype == (uint32_t)~LJ_TSTR &&
         lj_gc2_small_lease_try(G(L), s->ref, sizeof(GCstr), s->reftype,
                                &pin->ref, NULL)) ||
        lj_gc2_tv_lease_acquire(G(L), &ref, &pin->ref) == LJ_GC2_TV_EDGE_VALID) {
      if (bufx_same(pin->range, la_load128_acq(&buf->range)))
        return 1;
    }
  }
  bufx_unpin(pin);
  return 0;
}

static void bufx_pin(lua_State *L, LJBuffer *buf, int bytes, BufPin *pin)
{
  while (!bufx_pin_try(L, buf, bytes, pin))
    lj_tab_wait_l(L);
}

/* Replacement consumes the private candidate on both outcomes. The winner
** retires the previous backing while its lease still pins the allocator
** header. Keep the new backing retained through its publication barriers,
** even if another writer immediately replaces it. */
static int bufx_replace(lua_State *L, LJBuffer *buf, BufPin *pin,
                        LJBufStorage *fresh, MSize w)
{
  la_u128 old = pin->range, next = bufx_range(fresh, 0, w);
  LJBufStorage *removed = pin->store;
  LJGC2Lease lease;
  size_t size = removed ? bufx_bytes(removed->capacity, removed->ref) : 0;
  if (fresh) {
    if (!lj_gc2_storage_lease_try(G(L), fresh, sizeof(*fresh), &lease, NULL)) {
      lj_mem_free(G(L), fresh, bufx_bytes(fresh->capacity, fresh->ref));
      return 0;
    }
    (void)lj_gc2_markmem_registered_publish_try(G(L), fresh);
    la_store32_rel(&fresh->written, w + 1);
    bufx_test(L, buf, LJ_BUF_TEST_BEFORE_PUBLISH);
  }
  if (!la_cas128(&buf->range, &old, next)) {
    if (fresh) {
      lj_mem_free(G(L), fresh, bufx_bytes(fresh->capacity, fresh->ref));
      lj_gc2_lease_release(&lease);
    }
    return 0;
  }
  if (fresh) {
    if (fresh->ref) lj_gc_pubobjobj(L, (GCudata *)buf - 1, fresh->ref);
    (void)lj_gc2_markmem_registered_publish_try(G(L), fresh);
    lj_udata_rescan(L, (GCudata *)buf - 1);
    lj_gc2_lease_release(&lease);
  }
  if (removed) lj_mem_free(G(L), removed, size);
  bufx_unpin(pin);
  return 1;
}

static void bufx_publish(lua_State *L, LJBuffer *buf, const char *data,
                         MSize w, GCobj *ref)
{
  BufPin pin;
  for (;;) {
    LJBufStorage *fresh = NULL;
    bufx_pin(L, buf, 0, &pin);
    /* No private allocation may cross the safepoint in bufx_pin(). Allocate
    ** only after admission, and retire a losing candidate before retrying. */
    if (ref) {
      fresh = bufx_alloc(L, buf, w, data, ref);
      if (!fresh) { bufx_unpin(&pin); lj_err_mem(L); }
    }
    if (bufx_replace(L, buf, &pin, fresh, w)) return;
    bufx_unpin(&pin);
  }
}

int lj_bufx_init_storage(lua_State *L, LJBuffer *buf, MSize capacity)
{
  LJBufStorage *s = bufx_alloc(L, buf, bufx_capacity(0, capacity), NULL, NULL);
  if (!s) return 0;
  la_store32_rel(&s->written, 1);
  buf->range = bufx_range(s, 0, 0);  /* Constructor still owns the payload. */
  (void)lj_gc2_markmem_registered_publish_try(G(L), s);
  return 1;
}

void lj_bufx_free(lua_State *L, LJBuffer *buf)
{
  bufx_publish(L, buf, NULL, 0, NULL);
}

void LJ_FASTCALL lj_bufx_reset(lua_State *L, LJBuffer *buf)
{
  for (;;) {
    BufPin pin;
    la_u128 expected, empty;
    bufx_pin(L, buf, 0, &pin);
    if (!pin.store) return;
    if (pin.store->ref) {
      if (bufx_replace(L, buf, &pin, NULL, 0)) return;
    } else {
      expected = pin.range;
      empty = bufx_range(pin.store, bufx_w(expected), bufx_w(expected));
      if (la_cas128(&buf->range, &expected, empty)) {
        bufx_unpin(&pin);
        return;
      }
    }
    bufx_unpin(&pin);
  }
}

static uint32_t bufx_anchor(lua_State *L, GCobj *obj, uint32_t tag)
{
  TValue tv, *slot;
  uint32_t index;
  setgcVraw(&tv, obj, tag);
  slot = lj_tg_root_anchor_push(L, L2TG(L), &tv, &index);
  if (!slot) lj_err_mem(L);
  lj_gc_pubroot(L, slot);
  return index;
}

void lj_bufx_set(lua_State *L, LJBuffer *buf, const char *p, MSize len, GCobj *ref)
{
  uint32_t root = bufx_anchor(L, obj2gco((GCudata *)buf - 1), LJ_TUDATA);
  uint32_t value = bufx_anchor(L, ref, ~(uint32_t)ref->gch.gct);
  bufx_publish(L, buf, p, len, ref);
  lj_tg_root_anchor_pop(L2TG(L), value);
  lj_tg_root_anchor_pop(L2TG(L), root);
}

static int bufx_endwrite(LJBuffer *buf, BufPin *pin, MSize w)
{
  la_u128 cur = pin->range;
  la_store32_rel(&pin->store->written, w + 1);
  do {
    la_u128 next = bufx_range(pin->store, bufx_r(cur), w);
    if (la_cas128(&buf->range, &cur, next)) {
      la_store64_rel(&pin->store->claimed, w);
      return 1;
    }
  } while (cur.lo == pin->range.lo && bufx_w(cur) == bufx_w(pin->range));
  return 0;
}

/* Grow or compact from retained bytes into a private allocation. All
** fallible input conversions finish before this operation can be retried. */
static LJBufStorage *bufx_copy_store(lua_State *L, LJBuffer *buf, BufPin *pin,
                                     MSize more, MSize *len)
{
  LJBufStorage *old = pin->store, *s;
  MSize cap = old && !old->ref ? old->capacity : 0;
  *len = bufx_w(pin->range) - bufx_r(pin->range);
  if (more > LJ_MAX_BUF - *len) return NULL;
  cap = bufx_capacity(cap, *len + more);
  s = bufx_alloc(L, buf, cap, NULL, NULL);
  if (s && *len) memcpy(s+1, old->data + bufx_r(pin->range), *len);
  return s;
}

void lj_bufx_append(lua_State *L, LJBuffer *buf, const char *p, MSize n)
{
  while (n) {
    BufPin pin;
    LJBufStorage *s;
    MSize w, len;
    uint64_t expected;
    bufx_pin(L, buf, 1, &pin);
    w = bufx_w(pin.range);
    expected = w;
    if (pin.store && !pin.store->ref && n <= pin.store->capacity - w &&
        la_cas64(&pin.store->claimed, &expected, BUFX_WRITING | w, LA_ACQ_REL, LA_ACQ)) {
      bufx_test(L, buf, LJ_BUF_TEST_TAIL_CLAIMED);
      memcpy((char *)(pin.store+1) + w, p, n);
      if (bufx_endwrite(buf, &pin, w + n)) {
        bufx_unpin(&pin);
        return;
      }
      bufx_unpin(&pin);
      continue;
    }
    s = bufx_copy_store(L, buf, &pin, n, &len);
    if (!s) { bufx_unpin(&pin); lj_err_mem(L); }
    memcpy((char *)(s+1) + len, p, n);
    s->claimed = len + n;
    if (bufx_replace(L, buf, &pin, s, len+n)) return;
    bufx_unpin(&pin);
  }
}

void lj_bufx_putstr(lua_State *L, LJBuffer *buf, GCstr *s)
{
  uint32_t root = bufx_anchor(L, obj2gco((GCudata *)buf - 1), LJ_TUDATA);
  uint32_t value = bufx_anchor(L, obj2gco(s), LJ_TSTR);
  lj_bufx_append(L, buf, strdata(s), s->len);
  lj_tg_root_anchor_pop(L2TG(L), value);
  lj_tg_root_anchor_pop(L2TG(L), root);
}

static LJBuffer *bufx_userdata(lua_State *L, cTValue *o, LJGC2Lease *lease)
{
  GCudata *ud;
  int status;
  memset(lease, 0, sizeof(*lease));
  if (!tvisudata(o)) return NULL;
  ud = udataV(o);
  if (!lj_gc2_small_lease_try(G(L), ud, sizeof(*ud) + sizeof(LJBuffer),
                              (uint32_t)~LJ_TUDATA, lease, NULL)) {
    for (;;) {
      status = lj_gc2_tv_lease_acquire(G(L), o, lease);
      if (status != LJ_GC2_TV_EDGE_RETRY) break;
      lj_tab_wait_l(L);
    }
    if (status != LJ_GC2_TV_EDGE_VALID) return NULL;
  }
  if (ud->len == sizeof(LJBuffer) && lj_udata_udtype_acq(ud) == UDTYPE_BUFFER)
    return (LJBuffer *)uddata(ud);
  lj_gc2_lease_release(lease);
  return NULL;
}

int lj_bufx_isbuf(lua_State *L, cTValue *o)
{
  LJGC2Lease lease;
  LJBuffer *buf = bufx_userdata(L, o, &lease);
  lj_gc2_lease_release(&lease);
  return buf != NULL;
}

MSize lj_bufx_readlen(lua_State *L, cTValue *o)
{
  LJGC2Lease lease;
  LJBuffer *buf = bufx_userdata(L, o, &lease);
  MSize len = buf ? lj_bufx_length(buf) : 0;
  lj_gc2_lease_release(&lease);
  return len;
}

static MSize bufx_copy(lua_State *L, SBuf *sb, LJBuffer *buf, MSize limit,
                       LJBufPosition *pos)
{
  for (;;) {
    BufPin pin;
    MSize len;
    bufx_pin(L, buf, 1, &pin);
    len = bufx_w(pin.range) - bufx_r(pin.range);
    if (len > limit) len = limit;
    if (len <= sbufleft(sb)) {
      if (len) {
        char *w = lj_buf_wptr_acq(sb);
        lj_buf_wptr_rel(sb, lj_buf_wmem(w, pin.store->data + bufx_r(pin.range), len));
      }
      if (pos) {
        pos->range = pin.range;
        pos->serial = pin.store ? pin.store->serial : 0;
      }
      bufx_unpin(&pin);
      return len;
    }
    bufx_unpin(&pin);
    (void)lj_buf_more(sb, len);
  }
}

void lj_bufx_putbuf(lua_State *L, SBuf *dst, cTValue *source)
{
  TValue snap, nil, *slot;
  uint32_t root;
  LJGC2Lease lease;
  LJBuffer *buf;
  setnilV(&nil);
  lj_tv_load_acq(&snap, source);
  slot = lj_tg_root_anchor_push(L, L2TG(L), &nil, &root);
  if (!slot) lj_err_mem(L);
  buf = bufx_userdata(L, &snap, &lease);
  if (buf) {
    copyTVrel(L, slot, &snap);
    lj_gc_pubroot(L, slot);
    lj_gc2_lease_release(&lease);
    (void)bufx_copy(L, dst, buf, LJ_MAX_BUF, NULL);
  }
  lj_tg_root_anchor_pop(L2TG(L), root);
}

int lj_bufx_consume(lua_State *L, LJBuffer *buf, LJBufPosition *pos, MSize n)
{
  BufPin pin;
  int ok = 0;
  if (!n) return 1;
  /* get() has interned its result but has not returned it to a Lua root yet.
  ** Refuse admission without a safepoint; the caller retries the whole read. */
  if (!bufx_pin_try(L, buf, 0, &pin)) return 0;
  if (pin.store && pin.store->serial == pos->serial &&
      pin.range.lo == pos->range.lo && bufx_r(pin.range) == bufx_r(pos->range)) {
    la_u128 old = pin.range;
    la_u128 next = bufx_range(pin.store, bufx_r(old) + n, bufx_w(old));
    ok = la_cas128(&buf->range, &old, next);
  }
  bufx_unpin(&pin);
  return ok;
}

void LJ_FASTCALL lj_bufx_skip(LJBuffer *buf, MSize n)
{
  la_u128 old = la_load128_acq(&buf->range);
  while (n) {
    MSize r = bufx_r(old), w = bufx_w(old), len = w - r;
    la_u128 next = old;
    if (!len) break;
    next.hi = ((uint64_t)w << 32) | (r + (n < len ? n : len));
    if (la_cas128(&buf->range, &old, next)) break;
  }
}

static GCstr *bufx_string(SBuf *sb, LJBuffer *buf, MSize limit,
                          LJBufPosition *pos, int consume)
{
  lua_State *L = sbufL(sb);
  uint32_t root = bufx_anchor(L, obj2gco((GCudata *)buf - 1), LJ_TUDATA);
  GCstr *s;
  for (;;) {
    MSize n = bufx_copy(L, sb, buf, limit, pos);
    s = lj_buf_str(L, sb);
    if (!consume || lj_bufx_consume(L, buf, pos, n)) break;
    lj_buf_reset(sb);
  }
  lj_tg_root_anchor_pop(L2TG(L), root);
  return s;
}

GCstr *LJ_FASTCALL lj_bufx_tostr(SBuf *scratch, LJBuffer *buf)
{
  return bufx_string(scratch, buf, LJ_MAX_BUF, NULL, 0);
}

GCstr *lj_bufx_get(SBuf *scratch, LJBuffer *buf, MSize n)
{
  LJBufPosition pos;
  return bufx_string(scratch, buf, n, &pos, 1);
}

void lj_bufx_decode(lua_State *L, LJBuffer *buf, TValue *o)
{
  ptrdiff_t out = savestack(L, o);
  for (;;) {
    BufPin pin;
    MSize used, len;
    const char *data;
    int status, consumed;
    bufx_pin(L, buf, 1, &pin);
    len = bufx_w(pin.range) - bufx_r(pin.range);
    data = len ? pin.store->data + bufx_r(pin.range) : "";
    /* The serializer catches errors before these retained input bytes are
    ** released. Decode one record directly; copying the whole suffix here
    ** would make a stream of small records quadratic. */
    status = lj_serialize_decode_buf(L, buf, restorestack(L, out),
                                    data, len, &used);
    consumed = 0;
    if (status == LUA_OK) {
      la_u128 old = la_load128_acq(&buf->range);
      /* The input lease still excludes address reuse. Accept an intervening
      ** append, but never consume after another reader or replacement. */
      if (old.lo == pin.range.lo && bufx_r(old) == bufx_r(pin.range)) {
        la_u128 next = bufx_range(pin.store, bufx_r(old) + used, bufx_w(old));
        consumed = la_cas128(&buf->range, &old, next);
      }
    }
    bufx_unpin(&pin);
    if (status != LUA_OK) lj_err_throw(L, status);
    if (consumed) return;
    setnilV(restorestack(L, out));
  }
}

#if LJ_HASFFI
char *lj_bufx_reserve(lua_State *L, LJBuffer *buf, MSize n, MSize *left)
{
  for (;;) {
    BufPin pin;
    LJBufStorage *s;
    MSize w, len;
    char *p;
    uint64_t expected;
    bufx_pin(L, buf, 1, &pin);
    w = bufx_w(pin.range);
    expected = w;
    if (!pin.store && !n) { *left = 0; return NULL; }
    if (pin.store && !pin.store->ref && n <= pin.store->capacity - w &&
        la_cas64(&pin.store->claimed, &expected, BUFX_RESERVED | w, LA_ACQ_REL, LA_ACQ)) {
      *left = pin.store->capacity - w;
      p = (char *)(pin.store+1) + w;
      bufx_unpin(&pin);
      return p;
    }
    s = bufx_copy_store(L, buf, &pin, n, &len);
    if (!s) { bufx_unpin(&pin); lj_err_mem(L); }
    s->claimed = BUFX_RESERVED | len;
    p = (char *)(s+1) + len;
    *left = s->capacity - len;
    if (bufx_replace(L, buf, &pin, s, len)) return p;
    bufx_unpin(&pin);
  }
}

int lj_bufx_commit(lua_State *L, LJBuffer *buf, MSize n)
{
  BufPin pin;
  uint64_t expected;
  MSize w;
  int ok = n == 0;
  if (ok) return 1;
  bufx_pin(L, buf, 0, &pin);
  w = bufx_w(pin.range);
  if (pin.store && !pin.store->ref && n <= pin.store->capacity - w) {
    expected = la_load64_acq(&pin.store->claimed);
    if ((expected == w || expected == (BUFX_RESERVED | w)) &&
        la_cas64(&pin.store->claimed, &expected, BUFX_WRITING | w, LA_ACQ_REL, LA_ACQ))
      ok = bufx_endwrite(buf, &pin, w + n);
  }
  bufx_unpin(&pin);
  return ok;
}

const char *lj_bufx_ref(lua_State *L, LJBuffer *buf, MSize *len)
{
  BufPin pin;
  const char *p;
  bufx_pin(L, buf, 1, &pin);
  *len = bufx_w(pin.range) - bufx_r(pin.range);
  p = pin.store && pin.store->data ? pin.store->data + bufx_r(pin.range) : "";
  bufx_unpin(&pin);
  return p;
}

const char *LJ_FASTCALL lj_bufx_ptr(lua_State *L, LJBuffer *buf)
{
  MSize len;
  return lj_bufx_ref(L, buf, &len);
}
#endif

#if LJ_HASJIT
int32_t LJ_FASTCALL lj_bufx_len_forjit(LJBuffer *buf)
{
  return (int32_t)lj_bufx_length(buf);
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
