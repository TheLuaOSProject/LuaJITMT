/*
** String handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_str_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_char.h"
#include "lj_prng.h"
#include "lj_thr.h"
#include "lj_tg.h"
#include "lj_arena.h"

/* -- String helpers ------------------------------------------------------ */

/* Ordered compare of strings. Assumes string data is 4-byte aligned. */
int32_t LJ_FASTCALL lj_str_cmp(GCstr *a, GCstr *b)
{
  MSize i, n = a->len > b->len ? b->len : a->len;
  for (i = 0; i < n; i += 4) {
    /* Note: innocuous access up to end of string + 3. */
    uint32_t va = *(const uint32_t *)(strdata(a)+i);
    uint32_t vb = *(const uint32_t *)(strdata(b)+i);
    if (va != vb) {
#if LJ_LE
      va = lj_bswap(va); vb = lj_bswap(vb);
#endif
      i -= n;
      if ((int32_t)i >= -3) {
	va >>= 32+(i<<3); vb >>= 32+(i<<3);
	if (va == vb) break;
      }
      return va < vb ? -1 : 1;
    }
  }
  return (int32_t)(a->len - b->len);
}

/* Find fixed string p inside string s. */
const char *lj_str_find(const char *s, const char *p, MSize slen, MSize plen)
{
  if (plen <= slen) {
    if (plen == 0) {
      return s;
    } else {
      int c = *(const uint8_t *)p++;
      plen--; slen -= plen;
      while (slen) {
	const char *q = (const char *)memchr(s, c, slen);
	if (!q) break;
	if (memcmp(q+1, p, plen) == 0) return q;
	q++; slen -= (MSize)(q-s); s = q;
      }
    }
  }
  return NULL;
}

/* Check whether a string has a pattern matching character. */
int lj_str_haspattern(GCstr *s)
{
  const char *p = strdata(s), *q = p + s->len;
  while (p < q) {
    int c = *(const uint8_t *)p++;
    if (lj_char_ispunct(c) && strchr("^$*+?.([%-", c))
      return 1;  /* Found a pattern matching char. */
  }
  return 0;  /* No pattern matching chars found. */
}

/* -- String hashing ------------------------------------------------------ */

/* Keyed sparse ARX string hash. Constant time. */
static StrHash hash_sparse(uint64_t seed, const char *str, MSize len)
{
  /* Constants taken from lookup3 hash by Bob Jenkins. */
  StrHash a, b, h = len ^ (StrHash)seed;
  if (len >= 4) {  /* Caveat: unaligned access! */
    a = lj_getu32(str);
    h ^= lj_getu32(str+len-4);
    b = lj_getu32(str+(len>>1)-2);
    h ^= b; h -= lj_rol(b, 14);
    b += lj_getu32(str+(len>>2)-1);
  } else {
    a = *(const uint8_t *)str;
    h ^= *(const uint8_t *)(str+len-1);
    b = *(const uint8_t *)(str+(len>>1));
    h ^= b; h -= lj_rol(b, 14);
  }
  a ^= h; a -= lj_rol(h, 11);
  b ^= a; b -= lj_rol(a, 25);
  h ^= b; h -= lj_rol(b, 16);
  return h;
}

#if LUAJIT_SECURITY_STRHASH
/* Keyed dense ARX string hash. Linear time. */
static LJ_NOINLINE StrHash hash_dense(uint64_t seed, StrHash h,
				      const char *str, MSize len)
{
  StrHash b = lj_bswap(lj_rol(h ^ (StrHash)(seed >> 32), 4));
  if (len > 12) {
    StrHash a = (StrHash)seed;
    const char *pe = str+len-12, *p = pe, *q = str;
    do {
      a += lj_getu32(p);
      b += lj_getu32(p+4);
      h += lj_getu32(p+8);
      p = q; q += 12;
      h ^= b; h -= lj_rol(b, 14);
      a ^= h; a -= lj_rol(h, 11);
      b ^= a; b -= lj_rol(a, 25);
    } while (p < pe);
    h ^= b; h -= lj_rol(b, 16);
    a ^= h; a -= lj_rol(h, 4);
    b ^= a; b -= lj_rol(a, 14);
  }
  return b;
}
#endif

/* -- String interning ---------------------------------------------------- */

#define LJ_STR_MAXCOLL		32
#define LJ_STRTAB_RESIZE	((MSize)0x80000000u)
#define LJ_STRTAB_SWEEP		((MSize)0x40000000u)
#define LJ_STRTAB_OWNER_MASK	(LJ_STRTAB_RESIZE|LJ_STRTAB_SWEEP)
#define LJ_STRTAB_OWNER_LOBITS	(LJ_STRTAB_SWEEP - 1u)
#define LJ_STRID_BLOCK		64u
#define LJ_STRNUM_BLOCK		64u
#define LJ_STR_SWEEP_GRACE_EPOCHS LJ_GC2_GRACE_EPOCHS

#ifdef LJ_STR_TEST_HELPERS
static uint32_t str_test_id_refills;
static uint32_t str_test_num_refills;
static LJStrTestMatchHook str_test_match_hook;
static LJStrTestCanonHook str_test_canon_hook;

void lj_str_test_set_match_hook(LJStrTestMatchHook hook)
{
  str_test_match_hook = hook;
}

void lj_str_test_set_canon_hook(LJStrTestCanonHook hook)
{
  str_test_canon_hook = hook;
}

static LJ_AINLINE void str_test_canon(lua_State *L, GCstr *s,
				      StrCanonRec *rec, uint32_t stage)
{
  LJStrTestCanonHook hook = str_test_canon_hook;
  if (hook)
    hook(L, s, rec, stage);
}

static LJ_AINLINE void str_test_match(lua_State *L, GCRef *link,
				      GCobj *target, uintptr_t observed,
				      uint32_t stage)
{
  LJStrTestMatchHook hook = str_test_match_hook;
  if (hook)
    hook(L, link, target, observed, stage);
}

static LJ_AINLINE int str_test_match_enabled(void)
{
  return str_test_match_hook != NULL;
}

uint32_t lj_str_test_id_refills(void)
{
  return la_load32_acq(&str_test_id_refills);
}

void lj_str_test_reset_id_refills(void)
{
  la_store32_rel(&str_test_id_refills, 0);
}

uint32_t lj_str_test_num_refills(void)
{
  return la_load32_acq(&str_test_num_refills);
}

void lj_str_test_reset_num_refills(void)
{
  la_store32_rel(&str_test_num_refills, 0);
}

static LJ_AINLINE void str_test_id_refill(void)
{
  (void)la_add32_acqrel(&str_test_id_refills, 1);
}

static LJ_AINLINE void str_test_num_refill(void)
{
  (void)la_add32_acqrel(&str_test_num_refills, 1);
}
#else
#define str_test_match(L, link, target, observed, stage) \
  ((void)(L), (void)(link), (void)(target), (void)(observed), (void)(stage))
#define str_test_match_enabled()	0
#define str_test_id_refill()	((void)0)
#define str_test_num_refill()	((void)0)
#endif

static void strtab_wait(lua_State *L)
{
  /*
  ** String-table claim/enter waits are reached from string interning, resize,
  ** and secondary rehash paths with a current Lua state. Keep the wait native
  ** so handshakes can observe/ack this TG, but do not raise STOPREQ here:
  ** callers may already own a resize claim or hold an unpublished GCstr/
  ** StrTabHdr that must be cleaned up by the surrounding control flow.
  */
  (void)lj_thr_retry_yield(L);
}

static LJ_NOINLINE StrID strid_refill(global_State *g, TGState *tg)
{
  /*
  ** `sid` is a unique hash discriminator for table string keys. IDs do not
  ** need to be dense: duplicate-intern CAS losers already consume IDs before
  ** freeing their unpublished string. Reserve a small per-TG range to remove
  ** the global `g->str.id` cache-line hit from every successful string
  ** allocation while preserving atomic uniqueness and normal uint32 wrap.
  */
  StrID base = lj_str_id_add_rlx(g, LJ_STRID_BLOCK);
  tg->strid_next = base + 1u;
  tg->strid_end = base + LJ_STRID_BLOCK;
  str_test_id_refill();
  return base;
}

static LJ_AINLINE StrID strid_next(lua_State *L, global_State *g)
{
  TGState *tg = L2TG(L);
  StrID sid = tg->strid_next;
  if (sid != tg->strid_end) {
    tg->strid_next = sid + 1u;
    return sid;
  }
  return strid_refill(g, tg);
}

static LJ_NOINLINE MSize strnum_refill(global_State *g, TGState *tg)
{
  /*
  ** `g->str.num` is the shared resize/shrink and close-time string count.
  ** Successful interns reserve a small count block per TG and consume one
  ** credit only after the bucket CAS linearizes a new string. The global count
  ** is therefore conservative until unused credits are flushed on TG exit or
  ** close, removing a contended RMW from the common successful-intern path
  ** while preserving exact final accounting.
  */
  MSize n = lj_str_num_add_rlx(g, LJ_STRNUM_BLOCK) + LJ_STRNUM_BLOCK;
  tg->strnum_credit = LJ_STRNUM_BLOCK - 1u;
  str_test_num_refill();
  return n;
}

static LJ_AINLINE int strnum_publish_success(global_State *g, TGState *tg,
					     MSize mask)
{
  uint32_t credit = tg->strnum_credit;
  if (credit != 0) {
    tg->strnum_credit = credit - 1u;
    return 0;
  }
  return strnum_refill(g, tg) > mask;
}

void lj_str_flush_num_credit(global_State *g, TGState *tg)
{
  uint32_t credit;
  if (!g || !tg)
    return;
  credit = tg->strnum_credit;
  if (credit != 0) {
    tg->strnum_credit = 0;
    (void)lj_str_num_sub_acqrel(g, credit);
  }
}

static void strtab_retired_push(global_State *g, StrTabHdr *hdr)
{
  StrTabHdr *head = lj_str_retired_head_acq(g);
  /* 06 section 6.5: RCU retire. */
  do {
    lj_str_retired_next_rel(hdr, head);
  } while (!lj_str_retired_head_cas(g, &head, hdr));
}

static void strtab_retire(global_State *g, StrTabHdr *hdr)
{
  lj_str_retire_epoch_rel(hdr, lj_gc2_retire_epoch(g));
  strtab_retired_push(g, hdr);
}

static void strq_retired_push(global_State *g, StrCanonHdr *hdr)
{
  StrCanonHdr *head = lj_str_qretired_head_acq(g);
  do {
    lj_str_qretired_next_rel(hdr, head);
  } while (!lj_str_qretired_head_cas(g, &head, hdr));
}

static void strq_retire(global_State *g, StrCanonHdr *hdr)
{
  lj_str_qretire_epoch_rel(hdr, lj_gc2_retire_epoch(g));
  strq_retired_push(g, hdr);
}

static LJ_AINLINE MSize strtab_resize_acq(StrTabHdr *hdr)
{
  MSize state = la_load32_acq(&hdr->resize);
  lj_assertX((state & LJ_STRTAB_OWNER_LOBITS) == 0 &&
	     (state & ~(MSize)LJ_STRTAB_OWNER_MASK) == 0,
	     "stale string table reader-count bits");
  return state;
}

static LJ_AINLINE int strtab_resizing(StrTabHdr *hdr)
{
  /* Sweep mutates links with exact CASes, so ordinary interners keep running. */
  return (strtab_resize_acq(hdr) & LJ_STRTAB_RESIZE) != 0;
}

static LJ_AINLINE int strtab_sweeping(StrTabHdr *hdr)
{
  return (strtab_resize_acq(hdr) & LJ_STRTAB_SWEEP) != 0;
}

static void strtab_active_enter(TGState *tg, StrTabHdr *hdr)
{
  uint32_t depth = lj_tg_strtab_active_depth_acq(tg);
  if (depth == 0) {
    lj_tg_strtab_active_hdr_rel(tg, hdr);
    lj_tg_strtab_active_epoch_rel(tg, gc2_hs_epoch_acq(tg->gl));
  } else {
    lj_assertX(lj_tg_strtab_active_hdr_acq(tg) == hdr,
	       "nested string-table enter changed header");
  }
  lj_tg_strtab_active_depth_rel(tg, depth + 1u);
}

static void strtab_active_leave(TGState *tg, StrTabHdr *hdr)
{
  uint32_t depth = lj_tg_strtab_active_depth_acq(tg);
  lj_assertX(depth != 0, "bad string table active depth");
  lj_assertX(lj_tg_strtab_active_hdr_acq(tg) == hdr,
	     "string table active header mismatch");
  depth--;
  lj_tg_strtab_active_depth_rel(tg, depth);
  if (depth == 0)
    lj_tg_strtab_active_hdr_rel(tg, NULL);
}

static LJ_AINLINE int strtab_active_on_tg(global_State *g, TGState *tg,
					  StrTabHdr *hdr)
{
  return tg && tg->gl == g &&
    lj_tg_strtab_active_depth_acq(tg) != 0 &&
    lj_tg_strtab_active_hdr_acq(tg) == hdr;
}

static int strtab_active_on_hdr(global_State *g, StrTabHdr *hdr)
{
  TGState *tg, *main_tg, *self;
  int saw_main = 0, saw_self = 0;
  main_tg = g->main_tg;
  self = lj_thr_get_tg();
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    if (tg == main_tg)
      saw_main = 1;
    if (tg == self)
      saw_self = 1;
    if (strtab_active_on_tg(g, tg, hdr))
      return 1;
  }
  /*
  ** Attach/detach-adjacent code can have a same-state TG that is not yet, or
  ** no longer, reachable from gc2.tg_list. A resize claim must still honor
  ** those active pins before publishing and retiring the old header.
  */
  if (!saw_main && strtab_active_on_tg(g, main_tg, hdr))
    return 1;
  if (!saw_self && self != main_tg && strtab_active_on_tg(g, self, hdr))
    return 1;
  return 0;
}

static LJ_AINLINE int strtab_active_on_tg_before(global_State *g,
						 TGState *tg, StrTabHdr *hdr,
						 uint64_t epoch)
{
  return strtab_active_on_tg(g, tg, hdr) &&
    lj_tg_strtab_active_epoch_acq(tg) <= epoch;
}

static int strtab_active_on_hdr_before(global_State *g, StrTabHdr *hdr,
					       uint64_t epoch)
{
  TGState *tg, *main_tg, *self;
  int saw_main = 0, saw_self = 0;
  main_tg = g->main_tg;
  self = lj_thr_get_tg();
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (tg == main_tg)
      saw_main = 1;
    if (tg == self)
      saw_self = 1;
    if (strtab_active_on_tg_before(g, tg, hdr, epoch))
      return 1;
  }
  if (!saw_main && strtab_active_on_tg_before(g, main_tg, hdr, epoch))
    return 1;
  if (!saw_self && self != main_tg &&
      strtab_active_on_tg_before(g, self, hdr, epoch))
    return 1;
  return 0;
}

static void strq_active_enter(TGState *tg, StrCanonHdr *hdr)
{
  uint32_t depth = lj_tg_strq_active_depth_acq(tg);
  if (depth == 0) {
    lj_tg_strq_active_hdr_rel(tg, hdr);
    lj_tg_strq_active_epoch_rel(tg, gc2_hs_epoch_acq(tg->gl));
  } else {
    lj_assertX(lj_tg_strq_active_hdr_acq(tg) == hdr,
	       "nested canonical quarantine enter changed header");
  }
  lj_tg_strq_active_depth_rel(tg, depth + 1u);
}

static void strq_active_leave(TGState *tg, StrCanonHdr *hdr)
{
  uint32_t depth = lj_tg_strq_active_depth_acq(tg);
  lj_assertX(depth != 0, "bad canonical quarantine active depth");
  lj_assertX(lj_tg_strq_active_hdr_acq(tg) == hdr,
	     "canonical quarantine active header mismatch");
  depth--;
  lj_tg_strq_active_depth_rel(tg, depth);
  if (depth == 0)
    lj_tg_strq_active_hdr_rel(tg, NULL);
}

static LJ_AINLINE int strq_active_on_tg(global_State *g, TGState *tg,
					 StrCanonHdr *hdr)
{
  return tg && tg->gl == g && lj_tg_strq_active_depth_acq(tg) != 0 &&
    lj_tg_strq_active_hdr_acq(tg) == hdr;
}

static int strq_active_on_hdr(global_State *g, StrCanonHdr *hdr)
{
  TGState *tg, *main_tg = g->main_tg, *self = lj_thr_get_tg();
  int saw_main = 0, saw_self = 0;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (tg == main_tg) saw_main = 1;
    if (tg == self) saw_self = 1;
    if (strq_active_on_tg(g, tg, hdr))
      return 1;
  }
  if (!saw_main && strq_active_on_tg(g, main_tg, hdr))
    return 1;
  if (!saw_self && self != main_tg && strq_active_on_tg(g, self, hdr))
    return 1;
  return 0;
}

static StrCanonHdr *strq_enter(lua_State *L, global_State *g)
{
  TGState *tg = L2TG(L);
  for (;;) {
    StrCanonHdr *hdr = lj_str_qtabh_acq(g);
    if (!hdr)
      return NULL;
    if (lj_tg_strq_active_depth_acq(tg) != 0 &&
	lj_tg_strq_active_hdr_acq(tg) == hdr) {
      strq_active_enter(tg, hdr);
      return hdr;
    }
    if (la_load32_acq(&hdr->resize) != 0) {
      strtab_wait(L);
      continue;
    }
    strq_active_enter(tg, hdr);
    if (LJ_LIKELY(lj_str_qtabh_acq(g) == hdr &&
		  la_load32_acq(&hdr->resize) == 0))
      return hdr;
    strq_active_leave(tg, hdr);
  }
}

static void strq_leave(lua_State *L, StrCanonHdr *hdr)
{
  strq_active_leave(L2TG(L), hdr);
}

static int strtab_claim(lua_State *L, StrTabHdr *hdr)
{
  global_State *g = G(L);
  MSize expect = 0;
  /* Resize and secondary rehash never wait behind either destructive owner. */
  if (strtab_resize_acq(hdr) != 0 ||
      !la_cas32(&hdr->resize, &expect, LJ_STRTAB_RESIZE,
		LA_ACQ_REL, LA_ACQ)) {
    lj_assertX((expect & LJ_STRTAB_OWNER_LOBITS) == 0 &&
	       (expect & ~(MSize)LJ_STRTAB_OWNER_MASK) == 0,
	       "stale string table reader-count bits");
    return 0;
  }
  while (strtab_active_on_hdr(g, hdr))
    strtab_wait(L);
  return 1;
}

static void strtab_release(StrTabHdr *hdr)
{
  la_store32_rel(&hdr->resize, 0);
}

int lj_str_sweep_claim(lua_State *L, StrTabHdr *hdr)
{
  MSize expect = 0;
  /*
  ** Sweep owns stable bucket topology against resize/secondary rehash, but it
  ** never excludes or drains ordinary interners. All shared-link changes use
  ** exact CAS and body reclamation is deferred to a later grace period.
  */
  if (!hdr || strtab_resize_acq(hdr) != 0 ||
      !la_cas32(&hdr->resize, &expect, LJ_STRTAB_SWEEP,
		LA_ACQ_REL, LA_ACQ))
    return 0;
  if (LJ_UNLIKELY(lj_str_tabh_acq(G(L)) != hdr)) {
    strtab_release(hdr);
    return 0;
  }
  return 1;
}

void lj_str_sweep_release(StrTabHdr *hdr)
{
  strtab_release(hdr);
}

static int strtab_gc2_claim(global_State *g, StrTabHdr *hdr)
{
  MSize expect = 0;
  if (!g || !hdr || strtab_resize_acq(hdr) != 0 ||
      !la_cas32(&hdr->resize, &expect, LJ_STRTAB_SWEEP,
		LA_ACQ_REL, LA_ACQ))
    return 0;
  if (LJ_UNLIKELY(lj_str_tabh_acq(g) != hdr)) {
    strtab_release(hdr);
    return 0;
  }
  return 1;
}

static LJ_AINLINE void strtab_leave(lua_State *L, StrTabHdr *hdr)
{
  strtab_active_leave(L2TG(L), hdr);
}

static StrTabHdr *strtab_enter(lua_State *L, global_State *g)
{
  TGState *tg = L2TG(L);
  for (;;) {
    StrTabHdr *hdr = lj_str_tabh_acq(g);
    if (hdr == NULL)
      return NULL;
    if (lj_tg_strtab_active_depth_acq(tg) != 0 &&
	lj_tg_strtab_active_hdr_acq(tg) == hdr) {
      strtab_active_enter(tg, hdr);
      return hdr;
    }
    if (strtab_resizing(hdr)) {
      strtab_wait(L);
      continue;
    }
    strtab_active_enter(tg, hdr);  /* 06 section 6.5 RCU header pin. */
    if (LJ_LIKELY(lj_str_tabh_acq(g) == hdr && !strtab_resizing(hdr)))
      return hdr;
    strtab_active_leave(tg, hdr);
  }
}

/* Resize the string interning hash table (grow and shrink). */
void lj_str_resize(lua_State *L, MSize newmask)
{
  global_State *g = G(L);
  StrTabHdr *newhdr;
  StrTabHdr *oldhdr;
  GCRef *newtab, *oldtab;
  GCSize newsize;
  MSize i, oldmask;

  /* The atomic header claim below serializes every GC2 topology owner. The
  ** retired color collector state byte is not a resize authority. */
  if (newmask >= LJ_MAX_STRTAB-1)
    return;

  newsize = lj_str_tabsize(newmask);
  newhdr = (StrTabHdr *)lj_mem_new(L, newsize);
  memset(newhdr, 0, newsize);
  newhdr->mask = newmask;
  newtab = newhdr->bucket;

restart:
  oldhdr = lj_str_tabh_acq(g);
  oldtab = oldhdr ? oldhdr->bucket : NULL;
  oldmask = oldhdr ? oldhdr->mask : ~(MSize)0;

  if (oldhdr) {
    if (!strtab_claim(L, oldhdr)) {
      lj_mem_free(g, newhdr, newsize);
      return;
    }
    if (LJ_UNLIKELY(lj_str_tabh_acq(g) != oldhdr)) {
      StrTabHdr *curhdr = lj_str_tabh_acq(g);
      strtab_release(oldhdr);
      if (!curhdr || curhdr->mask >= newmask) {
	lj_mem_free(g, newhdr, newsize);
	return;
      }
      strtab_wait(L);
      goto restart;
    }
  }

#if LUAJIT_SECURITY_STRHASH
  /* Check which chains need secondary hashes. */
  if (lj_str_second_acq(g)) {
    int newsecond = 0;
    /* Compute primary chain lengths. */
    for (i = oldmask; i != ~(MSize)0; i--) {
      GCobj *o = lj_str_hashhead_acq(&oldtab[i]);
      while (o) {
	GCstr *s = gco2str(o);
	MSize hash = s->hashalg ? hash_sparse(g->str.seed, strdata(s), s->len) :
				  s->hash;
	hash &= newmask;
	lj_str_ref_store_rel(&newtab[hash],
			     lj_str_ref_load_acq(&newtab[hash]) + 1u);
	o = lj_str_next_acq(o);
      }
    }
    /* Mark secondary chains. */
    for (i = newmask; i != ~(MSize)0; i--) {
      int secondary = lj_str_ref_load_acq(&newtab[i]) > LJ_STR_MAXCOLL;
      newsecond |= secondary;
      lj_str_ref_store_rel(&newtab[i],
			   secondary ? LJ_STRHASH_SECONDARY : (uintptr_t)0);
    }
    lj_str_second_rel(g, (uint8_t)newsecond);
  }
#endif

  /* Reinsert all strings from the old table into the new table. */
  for (i = oldmask; i != ~(MSize)0; i--) {
    uintptr_t oldlink = lj_str_link_load_acq(&oldtab[i]);
    GCobj *o;
    while ((o = lj_str_link_target(oldlink)) != NULL) {
      uintptr_t nextlink = lj_str_next_link_acq(o);
      uintptr_t dead = oldlink & LJ_STRHASH_DEAD;
      GCstr *s = gco2str(o);
      MSize hash = s->hash;
      uintptr_t u;
#if LUAJIT_SECURITY_STRHASH
      if (LJ_LIKELY(!s->hashalg)) {  /* String hashed with primary hash. */
	hash &= newmask;
	u = lj_str_ref_load_acq(&newtab[hash]);
	if (LJ_UNLIKELY(u & LJ_STRHASH_SECONDARY)) {  /* Switch string to secondary hash. */
	  s->hash = hash = hash_dense(g->str.seed, s->hash, strdata(s), s->len);
	  s->hashalg = 1;
	  hash &= newmask;
	  u = lj_str_ref_load_acq(&newtab[hash]);
	}
      } else {  /* String hashed with secondary hash. */
	MSize shash = hash_sparse(g->str.seed, strdata(s), s->len);
	u = lj_str_ref_load_acq(&newtab[shash & newmask]);
	if (u & LJ_STRHASH_SECONDARY) {
	  hash &= newmask;
	  u = lj_str_ref_load_acq(&newtab[hash]);
	} else {  /* Revert string back to primary hash. */
	  s->hash = shash;
	  s->hashalg = 0;
	  hash = (shash & newmask);
	}
      }
      /* NOBARRIER: The string table is a GC root. */
      lj_str_next_store_rel(o, u);
      lj_str_bucket_store_rel(&newtab[hash], o,
			      (u & LJ_STRHASH_SECONDARY) | dead);
#else
      hash &= newmask;
      u = lj_str_ref_load_acq(&newtab[hash]);
      /* NOBARRIER: The string table is a GC root. */
      lj_str_next_store_rel(o, u);
      lj_str_bucket_store_rel(&newtab[hash], o, dead);
#endif
      oldlink = nextlink;
    }
  }

  /* Retire old table and replace with new table. */
  lj_str_mask_rel(g, newmask);
  lj_str_tabh_rel(g, newhdr);
  if (oldhdr)
    strtab_retire(g, oldhdr);
}

/* Replace the quarantine header only after claiming its topology and observing
** no published header pins. A late reader which loaded the old header before
** the claim rechecks resize after publishing its pin and never dereferences a
** bucket. Failed claims are opportunistic; lookup remains available. */
int lj_str_quarantine_resize(lua_State *L, MSize newmask)
{
  global_State *g = G(L);
  StrCanonHdr *oldhdr, *newhdr;
  GCSize newsize;
  MSize i, expect = 0;
  if (newmask >= LJ_MAX_STRTAB-1 || (newmask & (newmask + 1u)) != 0)
    return 0;
  newsize = lj_str_qtabsize(newmask);
  newhdr = (StrCanonHdr *)lj_mem_new(L, newsize);
  memset(newhdr, 0, newsize);
  newhdr->mask = newmask;
  /* Allocation may safepoint and let another generation retire. Snapshot the
  ** current header only after the allocating step; everything below is one
  ** no-safepoint topology-claim sequence protected by header retirement. */
  oldhdr = lj_str_qtabh_acq(g);
  if (!oldhdr || newmask <= oldhdr->mask) {
    lj_mem_free(g, newhdr, newsize);
    return 0;
  }
  if (la_load32_acq(&oldhdr->resize) != 0 ||
      !la_cas32(&oldhdr->resize, &expect, 1, LA_ACQ_REL, LA_ACQ)) {
    lj_mem_free(g, newhdr, newsize);
    return 0;
  }
  if (lj_str_qtabh_acq(g) != oldhdr || strq_active_on_hdr(g, oldhdr)) {
    la_store32_rel(&oldhdr->resize, 0);
    lj_mem_free(g, newhdr, newsize);
    return 0;
  }
  for (i = 0; i <= oldhdr->mask; i++) {
    StrCanonRec *rec = lj_str_qbucket_acq(&oldhdr->bucket[i]);
    while (rec) {
      StrCanonRec *next = lj_str_qnext_acq(rec);
      StrCanonRec **bucket = &newhdr->bucket[rec->hash & newmask];
      StrCanonRec *head = lj_str_qbucket_acq(bucket);
      lj_str_qnext_rel(rec, head);
      lj_str_qbucket_rel(bucket, rec);
      rec = next;
    }
  }
  lj_str_qmask_rel(g, newmask);
  lj_str_qtabh_rel(g, newhdr);
  strq_retire(g, oldhdr);
  return 1;
}

#if LUAJIT_SECURITY_STRHASH
/* Rehash and rechain all strings in a chain. */
static LJ_NOINLINE int lj_str_rehash_chain(lua_State *L, StrHash hashc)
{
  global_State *g = G(L);
  StrTabHdr *hdr = lj_str_tabh_acq(g);
  GCRef *strtab;
  MSize strmask;
  uintptr_t oldlink;
  GCobj *o;
  if (!hdr || !strtab_claim(L, hdr))
    return 0;
  if (lj_str_tabh_acq(g) != hdr) {
    strtab_release(hdr);
    return 0;
  }
  strtab = hdr->bucket;
  strmask = hdr->mask;
  oldlink = lj_str_link_load_acq(&strtab[hashc & strmask]);
  lj_str_ref_store_rel(&strtab[hashc & strmask], LJ_STRHASH_SECONDARY);
  lj_str_second_rel(g, 1);
  while ((o = lj_str_link_target(oldlink)) != NULL) {
    uintptr_t u;
    uintptr_t nextlink = lj_str_next_link_acq(o);
    uintptr_t dead = oldlink & LJ_STRHASH_DEAD;
    GCstr *s = gco2str(o);
    StrHash hash;
    hash = s->hash;
    if (!s->hashalg) {  /* Rehash with secondary hash. */
      hash = hash_dense(g->str.seed, hash, strdata(s), s->len);
      s->hash = hash;
      s->hashalg = 1;
    }
    /* Rechain. */
    hash &= strmask;
    u = lj_str_ref_load_acq(&strtab[hash]);
    lj_str_next_store_rel(o, u);
    lj_str_bucket_store_rel(&strtab[hash], o,
			    (u & LJ_STRHASH_SECONDARY) | dead);
    oldlink = nextlink;
  }
  strtab_release(hdr);
  return 1;
}
#endif

/* Allocate a new, unpublished string object. */
static GCstr *lj_str_alloc(lua_State *L, const char *str, MSize len,
			   StrHash hash, int hashalg)
{
  GCstr *s = lj_mem_newt(L, lj_str_size(len), GCstr);
  global_State *g = G(L);
  newwhite(g, s);
  s->gct = ~LJ_TSTR;
  s->len = len;
  s->hash = hash;
  s->sid = strid_next(L, g);
  s->reserved = 0;
  s->hashalg = (uint8_t)hashalg;
  lj_str_canon_store_rlx(s, LJ_STR_CANON_LIVE);
  /* Clear last 4 bytes of allocated memory. Implies zero-termination, too. */
  *(uint32_t *)(strdatawr(s)+(len & ~(MSize)3)) = 0;
  memcpy(strdatawr(s), str, len);
  return s;
}

/* Validate and, if needed, rescue the exact incoming link for a match. */
static LJ_AINLINE int strtab_match_link(lua_State *L, GCRef *link,
					GCobj *o, uintptr_t observed)
{
  global_State *g = G(L);
  uint32_t phase;
  uintptr_t current, want;

  /* A tagger or prepender may run after the matching bytes were inspected. */
  str_test_match(L, link, o, observed, LJ_STR_TEST_MATCH_AFTER_COMPARE);
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK)
    (void)lj_gc2_markobj(g, o);
  else if (phase == LJ_GC2_SWEEP)
    (void)lj_gc2_preserve_sweep_root(g, o);
  else if (!(observed & LJ_STRHASH_DEAD) && !str_test_match_enabled())
    return 1;  /* DEAD is only published by the sweep owner. */

  /* Mark/preserve while the pinned edge still protects the body, then validate. */
  current = lj_str_link_load_acq(link);
  if (lj_str_link_target(current) != o)
    return 0;  /* The incoming edge moved: restart from the bucket head. */
  if (!(current & LJ_STRHASH_DEAD) ||
      (phase != LJ_GC2_SWEEP && !str_test_match_enabled()))
    return 1;

  want = current & ~(uintptr_t)LJ_STRHASH_DEAD;
  str_test_match(L, link, o, current,
		 LJ_STR_TEST_MATCH_BEFORE_RESCUE_CAS);
  if (!lj_str_link_cas_acqrel(link, &current, want))
    return 0;  /* Rescue or unlink won: never follow a stale incoming edge. */

  lj_str_sweep_rescued_add(g, 1);
  return 1;
}

/* Preserve reachability before following a non-matching object's next link.
** Without this, UNLINK could remove the predecessor and its successor while an
** interner continued through the predecessor's now-stale next word, allowing a
** later byte match to validate an edge which no longer belongs to the table. */
static LJ_AINLINE int strtab_protect_walk_link(lua_State *L, GCRef *link,
					       GCobj *o)
{
  global_State *g = G(L);
  uint32_t phase = gc2_phase_acq(g);
  uintptr_t current, want;
  if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK)
    (void)lj_gc2_markobj(g, o);
  else if (phase == LJ_GC2_SWEEP)
    (void)lj_gc2_preserve_sweep_root(g, o);
  else
    return 1;
  current = lj_str_link_load_acq(link);
  if (lj_str_link_target(current) != o)
    return 0;
  if (phase == LJ_GC2_SWEEP && (current & LJ_STRHASH_DEAD)) {
    want = current & ~(uintptr_t)LJ_STRHASH_DEAD;
    if (!lj_str_link_cas_acqrel(link, &current, want))
      return 0;
    lj_str_sweep_rescued_add(g, 1);
  }
  return 1;
}

static LJ_AINLINE void strtab_mark_before_publish(global_State *g, GCobj *o)
{
  uint32_t phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK)
    (void)lj_gc2_markobj(g, o);
  else if (phase == LJ_GC2_SWEEP)
    (void)lj_gc2_preserve_sweep_root(g, o);
}

/* Consult the authoritative secondary canonical directory after a true main
** table miss. The per-TG header pin keeps the selected quarantine generation
** stable while immutable bytes are compared; Stage A retains bucket records
** until terminal shutdown. QCOMMIT/FREEING are deliberately not
** acquirable; those states are enabled only after read epochs can substitute
** the current canonical body. */
static GCstr *strcanon_lookup(lua_State *L, const char *str, MSize len,
			      StrHash hash)
{
  global_State *g = G(L);
  StrCanonHdr *hdr;
  StrCanonRec *rec;
  GCstr *found = NULL;
  if (lj_str_qcount_acq(g) == 0)
    return NULL;
  hdr = strq_enter(L, g);
  if (!hdr)
    return NULL;
  for (rec = lj_str_qbucket_acq(&hdr->bucket[hash & hdr->mask]);
       rec != NULL;
       rec = lj_str_qnext_acq(rec)) {
    GCstr *s;
    uintptr_t canon, want;
    uint32_t state;
    if (rec->hash != hash || rec->len != len)
      continue;
    s = (GCstr *)la_loadptr_acq((void *const *)&rec->str);
    if (!s || !lj_gc2_mem_registered(g, s) ||
	la_load8_acq(&s->gct) != (uint8_t)~LJ_TSTR ||
	s->len != len || memcmp(str, strdata(s), len) != 0)
      continue;
    canon = lj_str_canon_acq(s);
    for (;;) {
      state = lj_str_canon_state(canon);
      if (lj_str_canon_record(canon) != rec ||
	  (state != LJ_STR_CANON_QACTIVE &&
	   state != LJ_STR_CANON_QRESCUED &&
	   state != LJ_STR_CANON_QCLOSING))
	break;
      if (state == LJ_STR_CANON_QRESCUED) {
	found = s;
	break;
      }
      want = lj_str_canon_pack(rec, LJ_STR_CANON_QRESCUED);
      if (lj_str_canon_cas(s, &canon, want)) {
	found = s;  /* Rescue and logical-death arbitration LP. */
	break;
      }
    }
    if (found) {
      strtab_mark_before_publish(g, obj2gco(found));
      canon = lj_str_canon_acq(found);
      if (lj_str_canon_state(canon) != LJ_STR_CANON_QRESCUED ||
	  lj_str_canon_record(canon) != rec)
	found = NULL;
      else
	break;
    }
  }
  strq_leave(L, hdr);
  return found;
}

/* Intern a string and return string object. */
GCstr *lj_str_new(lua_State *L, const char *str, size_t lenx)
{
  global_State *g = G(L);
  if (lenx-1 < LJ_MAX_STR-1) {
    MSize len = (MSize)lenx;
    StrHash hash, qhash = hash_sparse(g->str.seed, str, len);
    GCstr *news = NULL;
    int hashalg = 0;
#if LUAJIT_SECURITY_STRHASH
    int skip_rehash = 0;
#endif
    for (;;) {
      StrTabHdr *hdr = strtab_enter(L, g);
      GCRef *strtab;
      GCRef *head, *link;
      GCobj *o;
      MSize mask, coll = 0;
      uintptr_t headu, u;
      int grow = 0;
      if (LJ_UNLIKELY(hdr == NULL)) {
	if (news)
	  lj_mem_free(g, news, lj_str_size(news->len));
	hash = hash_sparse(g->str.seed, str, len);
	return lj_str_alloc(L, str, len, hash, 0);
      }

      mask = hdr->mask;
      strtab = hdr->bucket;
      hashalg = 0;
      hash = qhash;
      head = &strtab[hash & mask];
      u = lj_str_link_load_acq(head);
#if LUAJIT_SECURITY_STRHASH
      if (LJ_UNLIKELY(u & LJ_STRHASH_SECONDARY)) {
	hashalg = 1;
	hash = hash_dense(g->str.seed, hash, str, len);
	head = &strtab[hash & mask];
      }
#endif

retry_lookup:
      coll = 0;
      link = head;
      while ((o = lj_str_link_target(
		u = lj_str_link_load_acq(link))) != NULL) {
	GCstr *sx = gco2str(o);
	if (sx->hash == hash && sx->len == len) {
	  if (memcmp(str, strdata(sx), len) == 0) {
	    if (!strtab_match_link(L, link, o, u))
	      goto retry_lookup;
	    strtab_leave(L, hdr);
	    if (news)
	      lj_mem_free(g, news, lj_str_size(news->len));
	    return sx;  /* Return existing string. */
	  }
	  coll++;
	}
	coll++;
	if (!strtab_protect_walk_link(L, link, o))
	  goto retry_lookup;
	link = lj_obj_gcwref(o);
      }
#if LUAJIT_SECURITY_STRHASH
      /* Rehash chain if there are too many collisions. */
      if (LJ_UNLIKELY(coll > LJ_STR_MAXCOLL) && !hashalg && !skip_rehash) {
	if (strtab_sweeping(hdr)) {
	  /* Keep this insertion nonrecursive while the CAS sweeper owns topology. */
	  skip_rehash = 1;
	} else {
	  strtab_leave(L, hdr);
	  skip_rehash = !lj_str_rehash_chain(L, hash);
	  continue;
	}
      }
#endif
      if (news == NULL) {
	GCstr *qs;
	strtab_leave(L, hdr);
	qs = strcanon_lookup(L, str, len, qhash);
	if (qs)
	  return qs;
	news = lj_str_alloc(L, str, len, hash, hashalg);
	continue;
      }

      news->hash = hash;
      news->hashalg = (uint8_t)hashalg;
      for (;;) {
	uintptr_t want;

retry_insert_lookup:
	headu = lj_str_link_load_acq(head);  /* 06 section 6.5 snapshot. */
	link = head;
	while ((o = lj_str_link_target(
		  u = lj_str_link_load_acq(link))) != NULL) {
	  GCstr *sx = gco2str(o);
	  if (sx->hash == hash && sx->len == len &&
	      memcmp(str, strdata(sx), len) == 0) {
	    if (!strtab_match_link(L, link, o, u))
	      goto retry_insert_lookup;
	    strtab_leave(L, hdr);
	    lj_mem_free(g, news, lj_str_size(news->len));
	    return sx;
	  }
	  if (!strtab_protect_walk_link(L, link, o))
	    goto retry_insert_lookup;
	  link = lj_obj_gcwref(o);
	}
	if (LJ_UNLIKELY(lj_str_qcount_acq(g) != 0)) {
	  GCstr *qs = strcanon_lookup(L, str, len, qhash);
	  if (qs) {
	    strtab_leave(L, hdr);
	    lj_mem_free(g, news, lj_str_size(news->len));
	    return qs;
	  }
	}
	/* Allocation-black is the fallback; make sweep publication explicit. */
	strtab_mark_before_publish(g, obj2gco(news));
	lj_str_next_store_rel(obj2gco(news), headu);
	want = (uintptr_t)news | (headu & LJ_STRHASH_SECONDARY);
	u = headu;
	if (lj_str_link_cas_acqrel(head, &u, want)) {  /* Intern linearization. */
	  grow = strnum_publish_success(g, L2TG(L), mask);
	  break;
	}
      }
      strtab_leave(L, hdr);
      if (grow)
	lj_str_resize(L, (mask << 1) + 1u);  /* Grow string table. */
      return news;  /* Return newly interned string. */
    }
  } else {
    if (lenx)
      lj_err_msg(L, LJ_ERR_STROV);
    return &g->strempty;
  }
}

void LJ_FASTCALL lj_str_free(global_State *g, GCstr *s)
{
  uint8_t gct = la_load8_acq(&s->gct);
  for (;;) {
    if (LJ_UNLIKELY(gct != (uint8_t)~LJ_TSTR))
      return;
    if (la_cas8(&s->gct, &gct, 0, LA_ACQ_REL, LA_ACQ))
      break;
  }
  /*
  ** Current-header string sweep owns the strtab claim, but close-time and
  ** retired-header races can still expose stale bucket links. Claiming the
  ** destructor through the type byte keeps the string count tied to the first
  ** successful free of a published string body; later stale observations are
  ** unlinked by their caller without consuming another count slot.
  */
  #if LUA_USE_ASSERT
  {
    MSize old = lj_str_num_sub_acqrel(g, 1);
    lj_assertG(old != 0, "string count underflow");
  }
  #else
  (void)lj_str_num_sub_acqrel(g, 1);
  #endif
  lj_mem_free(g, s, lj_str_size(s->len));
}

/* -- GC2 intern-table string reclamation ------------------------------- */

static LJ_AINLINE int str_sweep_target_live(global_State *g, GCobj *o)
{
  int marked = lj_gc2_ismarked(g, o);
  if (marked != 0)  /* Invalid/retiring memory is retained conservatively. */
    return 1;
  return (lj_obj_gcflags(o) & (LJ_GC_FIXED|LJ_GC_SFIXED)) != 0;
}

static void str_body_retired_push(global_State *g, StrBodyRetire *ret)
{
  StrBodyRetire *head = lj_str_body_retired_head_acq(g);
  do {
    lj_str_body_retired_next_rel(ret, head);
  } while (!lj_str_body_retired_head_cas(g, &head, ret));
}

static StrBodyRetire *str_body_retire_new(global_State *g, GCstr *s,
					   StrTabHdr *hdr)
{
  TGState *tg = lj_thr_get_tg();
  StrBodyRetire *ret;
  if (!g || !s || !hdr || !tg || tg->gl != g ||
      !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      g->allocf != lj_arena_allocf)
    return NULL;
  /*
  ** A parked worker has no borrowable Lua stack. Allocate this tiny ownership
  ** record from its own TG-local plain arena, so failure is non-throwing and no
  ** shared allocator cursor is touched. Dead worker arenas are transferred to
  ** the main TG before their TG registry node can be reclaimed.
  */
  ret = (StrBodyRetire *)lj_arena_allocd_alloc(&tg->allocd,
					       sizeof(*ret), 0);
  if (!ret)
    return NULL;
  lj_assertG(checkptrGC(ret),
	     "string retirement record outside required address range");
  ret->next = NULL;
  ret->qnext = NULL;
  ret->str = s;
  ret->hdr = hdr;
  ret->retire_epoch = 0;
  ret->main_unlink_epoch = 0;
  ret->close_epoch = 0;
  ret->q_unlink_epoch = 0;
  ret->size = lj_str_size(s->len);
  /* The quarantine index is independent of mutable main-table secondary
  ** hashing. Always key it by the canonical sparse hash of immutable bytes. */
  ret->hash = hash_sparse(g->str.seed, strdata(s), s->len);
  ret->len = s->len;
  ret->main_linked = 1;
  ret->status = 0;
  lj_gc_total_add(g, sizeof(*ret));
  lj_gc2_account_alloc(g, tg, sizeof(*ret));
  return ret;
}

#ifdef LJ_STR_TEST_HELPERS
static void strcanon_bucket_publish(lua_State *L, global_State *g,
				    StrCanonRec *rec)
{
  StrCanonHdr *hdr = strq_enter(L, g);
  StrCanonRec **bucket;
  StrCanonRec *head;
  lj_assertG(hdr != NULL, "canonical quarantine is not initialized");
  str_test_canon(L, lj_str_body_retired_str_acq(rec), rec,
		 LJ_STR_TEST_CANON_Q_PINNED_BEFORE_PUBLISH);
  bucket = &hdr->bucket[rec->hash & hdr->mask];
  head = lj_str_qbucket_acq(bucket);
  do {
    lj_str_qnext_rel(rec, head);
  } while (!lj_str_qbucket_cas(bucket, &head, rec));
  strq_leave(L, hdr);
}

int lj_str_test_quarantine_detach(lua_State *L, GCstr *s)
{
  global_State *g;
  StrTabHdr *hdr;
  StrCanonRec *rec;
  GCRef *link;
  uintptr_t u, next, want, canon;
  uint64_t epoch;
  GCobj *o;
  if (!L || !s)
    return 0;
  g = G(L);
  if (s == &g->strempty || lj_str_canon_acq(s) != LJ_STR_CANON_LIVE)
    return 0;
  hdr = lj_str_tabh_acq(g);
  if (!hdr)
    return 0;
  rec = str_body_retire_new(g, s, hdr);
  if (!rec)
    return 0;
  /* Establish durable metadata ownership before the Q-header pin can wait or
  ** service a safepoint. LIST_ONLY never owns the still-main-linked body. */
  la_store32_rel(&rec->status, LJ_STR_CANONREC_LIST_ONLY);
  lj_str_body_retired_epoch_rel(rec, lj_gc2_retire_epoch(g));
  str_body_retired_push(g, rec);
  strcanon_bucket_publish(L, g, rec);  /* Reservation precedes main unlink. */

  /* Snapshot and claim main topology only after every allocating/waiting Q
  ** operation. The remainder is one no-safepoint exact-edge sequence. */
  hdr = lj_str_tabh_acq(g);
  if (!hdr || !lj_str_sweep_claim(L, hdr)) {
    la_store32_rel(&rec->status,
	LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_CANCELLED);
    la_storeptr_rel((void **)&rec->str, NULL);
    return 0;
  }
  la_storeptr_rel((void **)&rec->hdr, hdr);
  link = &hdr->bucket[s->hash & hdr->mask];
  while ((o = lj_str_link_target(u = lj_str_link_load_acq(link))) != NULL &&
	 o != obj2gco(s))
    link = lj_obj_gcwref(o);
  if (!o) {
    la_store32_rel(&rec->status,
	LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_CANCELLED);
    la_storeptr_rel((void **)&rec->str, NULL);
    lj_str_sweep_release(hdr);
    return 0;
  }
  canon = LJ_STR_CANON_LIVE;
  want = lj_str_canon_pack(rec, LJ_STR_CANON_QACTIVE);
  if (!lj_str_canon_cas(s, &canon, want)) {
    la_store32_rel(&rec->status,
	LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_CANCELLED);
    la_storeptr_rel((void **)&rec->str, NULL);
    lj_str_sweep_release(hdr);
    return 0;
  }
  la_store32_rel(&rec->status,
	LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_BODY_OWNED);
  (void)lj_str_qcount_inc_sat_acqrel(g);
  epoch = lj_gc2_retire_epoch(g);
  la_store64_rel(&rec->main_unlink_epoch, epoch);
  lj_str_body_retired_epoch_rel(rec, epoch);
  /* Durable list ownership precedes this phase-aware retention barrier. If a
  ** new cycle starts after the barrier, its metadata scan sees the list; if it
  ** started before list publication, this barrier observes the active phase. */
  strtab_mark_before_publish(g, obj2gco(s));
  next = lj_str_next_link_acq(obj2gco(s));
  want = (next & ~(uintptr_t)LJ_STRHASH_SECONDARY) |
	 (u & LJ_STRHASH_SECONDARY);
  if (!lj_str_link_cas_acqrel(link, &u, want)) {
    canon = lj_str_canon_pack(rec, LJ_STR_CANON_QACTIVE);
    want = lj_str_canon_pack(rec, LJ_STR_CANON_QRESCUED);
    (void)lj_str_canon_cas(s, &canon, want);
    /* The main table still owns the body. Quarantine reserves identity but
    ** must not run the terminal body destructor a second time. */
    la_store32_rel(&rec->status, LJ_STR_CANONREC_Q_LINKED);
    strtab_mark_before_publish(g, obj2gco(s));
    lj_str_sweep_release(hdr);
    return 0;
  }
  la_store32_rel(&rec->main_linked, 0);
  strtab_mark_before_publish(g, obj2gco(s));
  lj_str_sweep_release(hdr);
  /* This hook is deliberately after releasing the main topology claim: a
  ** concurrent interner can now observe a true main miss and must acquire the
  ** already-published non-zero Q count before consulting the directory. */
  str_test_canon(L, s, rec, LJ_STR_TEST_CANON_MAIN_UNLINKED);
  return 1;
}
#endif

static void str_sweep_cursor_start(global_State *g, StrTabHdr *hdr,
				   uint32_t phase)
{
  lj_str_sweep_bucket_rel(g, 0);
  lj_str_sweep_link_rel(g, &hdr->bucket[0]);
  lj_str_sweep_phase_rel(g, phase);
}

static uint32_t str_sweep_finish_tag(global_State *g)
{
  lj_str_sweep_link_rel(g, NULL);
  lj_str_sweep_grace_epoch_rel(g, lj_gc2_retire_epoch(g));
  lj_str_sweep_phase_rel(g, LJ_STR_SWEEP_TAG_GRACE);
  gc2_sweep_grace_needed_rel(g, 1);
  return 1;
}

static uint32_t str_sweep_finish_unlink(global_State *g, StrTabHdr *hdr)
{
  lj_str_sweep_link_rel(g, NULL);
  lj_str_sweep_grace_epoch_rel(g, lj_gc2_retire_epoch(g));
  lj_str_sweep_phase_rel(g, LJ_STR_SWEEP_UNLINK_GRACE);
  /* Resize may proceed during the post-unlink body grace. */
  strtab_release(hdr);
  lj_str_sweep_hdr_rel(g, NULL);
  gc2_sweep_grace_needed_rel(g, 1);
  return 1;
}

static uint32_t str_sweep_advance_bucket(global_State *g, StrTabHdr *hdr,
					 uint32_t phase)
{
  MSize bucket = lj_str_sweep_bucket_acq(g);
  if (bucket >= hdr->mask)
    return phase == LJ_STR_SWEEP_TAG ? str_sweep_finish_tag(g) :
	   str_sweep_finish_unlink(g, hdr);
  bucket++;
  lj_str_sweep_bucket_rel(g, bucket);
  lj_str_sweep_link_rel(g, &hdr->bucket[bucket]);
  return 1;
}

static uint32_t str_sweep_tag_one(global_State *g, StrTabHdr *hdr)
{
  GCRef *link = lj_str_sweep_link_acq(g);
  uintptr_t u, want, current;
  GCobj *o;
  if (!link)
    return str_sweep_finish_tag(g);
  u = lj_str_link_load_acq(link);
  o = lj_str_link_target(u);
  if (!o)
    return str_sweep_advance_bucket(g, hdr, LJ_STR_SWEEP_TAG);

  if (str_sweep_target_live(g, o)) {
    if (u & LJ_STRHASH_DEAD) {
      current = u;
      want = u & ~(uintptr_t)LJ_STRHASH_DEAD;
      if (!lj_str_link_cas_acqrel(link, &current, want))
	return 1;
    }
    lj_str_sweep_link_rel(g, lj_obj_gcwref(o));
    return 1;
  }

  if (!(u & LJ_STRHASH_DEAD)) {
    current = u;
    want = u | LJ_STRHASH_DEAD;
    if (!lj_str_link_cas_acqrel(link, &current, want))
      return 1;  /* A prepend moved this incoming edge: retry it. */
    lj_str_sweep_tagged_add(g, 1);
    u = want;
    /* A late root can race the tag CAS. Never leave it for unlink unchecked. */
    if (str_sweep_target_live(g, o)) {
      current = u;
      want = u & ~(uintptr_t)LJ_STRHASH_DEAD;
      (void)lj_str_link_cas_acqrel(link, &current, want);
    }
  }
  /* UNLINK restarts at every bucket head, so a later prepend cannot be missed. */
  lj_str_sweep_link_rel(g, lj_obj_gcwref(o));
  return 1;
}

static uint32_t str_sweep_unlink_one(global_State *g, StrTabHdr *hdr)
{
  GCRef *link = lj_str_sweep_link_acq(g);
  uintptr_t u, current, want, next;
  StrBodyRetire *ret;
  GCobj *o;
  if (!link)
    return str_sweep_finish_unlink(g, hdr);
  u = lj_str_link_load_acq(link);
  o = lj_str_link_target(u);
  if (!o)
    return str_sweep_advance_bucket(g, hdr, LJ_STR_SWEEP_UNLINK);
  if (!(u & LJ_STRHASH_DEAD)) {
    lj_str_sweep_link_rel(g, lj_obj_gcwref(o));
    return 1;
  }

  /* A rescued/fixed/invalid target wins over an earlier tag observation. */
  if (str_sweep_target_live(g, o)) {
    current = u;
    want = u & ~(uintptr_t)LJ_STRHASH_DEAD;
    if (!lj_str_link_cas_acqrel(link, &current, want))
      return 1;
    lj_str_sweep_link_rel(g, lj_obj_gcwref(o));
    return 1;
  }

  ret = str_body_retire_new(g, gco2str(o), hdr);
  if (!ret) {
    /* Metadata OOM retains canonical identity and retries in a later cycle. */
    current = u;
    want = u & ~(uintptr_t)LJ_STRHASH_DEAD;
    if (!lj_str_link_cas_acqrel(link, &current, want))
      return 1;
    lj_str_sweep_link_rel(g, lj_obj_gcwref(o));
    return 1;
  }

  /* Publish the fully initialized side owner before the unlink linearization. */
  lj_str_sweep_pending_rel(g, ret);
  next = lj_str_next_link_acq(o);
  want = (next & ~(uintptr_t)LJ_STRHASH_SECONDARY) |
	 (u & LJ_STRHASH_SECONDARY);
  current = u;
  if (!lj_str_link_cas_acqrel(link, &current, want)) {
    lj_str_sweep_pending_rel(g, NULL);
    lj_mem_free(g, ret, sizeof(*ret));
    return 1;
  }

  lj_str_body_retired_epoch_rel(ret, lj_gc2_retire_epoch(g));
  str_body_retired_push(g, ret);
  lj_str_sweep_pending_rel(g, NULL);
  lj_str_sweep_unlinked_add(g, 1);
  /* Stay on the exact incoming edge: it now names the successor. */
  return 1;
}

static LJ_AINLINE int str_sweep_grace_complete(global_State *g)
{
  uint64_t start = lj_str_sweep_grace_epoch_acq(g);
  uint64_t now = lj_gc2_retire_epoch(g);
  return now >= start && now - start >= LJ_STR_SWEEP_GRACE_EPOCHS;
}

void lj_str_gc2_sweep_begin(global_State *g, int major)
{
  if (!g)
    return;
  if (lj_str_sweep_phase_acq(g) != LJ_STR_SWEEP_IDLE)
    lj_str_gc2_sweep_abort(g);
  lj_str_sweep_hdr_rel(g, NULL);
  lj_str_sweep_link_rel(g, NULL);
  lj_str_sweep_pending_rel(g, NULL);
  lj_str_sweep_bucket_rel(g, 0);
  lj_str_sweep_grace_epoch_rel(g, 0);
  la_store32_rel(&g->str.sweep_cycle, gc2_cycle_acq(g));
  major = major && LJ_GC2_STRING_BODY_RECLAIM;
  lj_str_sweep_phase_rel(g, major ? LJ_STR_SWEEP_ACQUIRE :
					 LJ_STR_SWEEP_DONE);
}

int lj_str_gc2_sweep_pending(global_State *g)
{
  uint32_t phase;
  if (!g)
    return 0;
  phase = lj_str_sweep_phase_acq(g);
  return phase != LJ_STR_SWEEP_IDLE && phase != LJ_STR_SWEEP_DONE;
}

uint32_t lj_str_gc2_sweep_step(global_State *g, uint32_t limit)
{
  uint32_t phase, n = 0;
  StrTabHdr *hdr;
  if (!g || limit == 0 || gc2_phase_acq(g) != LJ_GC2_SWEEP)
    return 0;
  while (n < limit) {
    phase = lj_str_sweep_phase_acq(g);
    if (phase == LJ_STR_SWEEP_IDLE || phase == LJ_STR_SWEEP_DONE)
      break;
    if (phase == LJ_STR_SWEEP_ACQUIRE) {
      hdr = lj_str_tabh_acq(g);
      if (!hdr) {
	lj_str_sweep_phase_rel(g, LJ_STR_SWEEP_DONE);
	n++;
	continue;
      }
      if (!strtab_gc2_claim(g, hdr)) {
	/* Failed claims are retry work, not a reason to park forever. */
	n++;
	continue;
      }
      lj_str_sweep_hdr_rel(g, hdr);
      str_sweep_cursor_start(g, hdr, LJ_STR_SWEEP_TAG);
      n++;
      continue;
    }
    hdr = lj_str_sweep_hdr_acq(g);
    if (phase == LJ_STR_SWEEP_TAG) {
      if (LJ_UNLIKELY(!hdr || lj_str_tabh_acq(g) != hdr)) {
	lj_str_gc2_sweep_abort(g);
	break;
      }
      n += str_sweep_tag_one(g, hdr);
      continue;
    }
    if (phase == LJ_STR_SWEEP_TAG_GRACE) {
      if (gc2_sweep_grace_needed_acq(g))
	break;
      if (!str_sweep_grace_complete(g)) {
	gc2_sweep_grace_needed_rel(g, 1);
	n++;
	continue;
      }
      /* Handshake ACKs may be remote while native code still holds a table
      ** pin. Only pre-tag readers must drain; newer readers see the published
      ** tags and protect every traversed incoming edge before following it. */
      if (strtab_active_on_hdr_before(g, hdr,
	    lj_str_sweep_grace_epoch_acq(g))) {
	(void)lj_thr_retry_yield(NULL);
	n++;
	continue;
      }
      str_sweep_cursor_start(g, hdr, LJ_STR_SWEEP_UNLINK);
      n++;
      continue;
    }
    if (phase == LJ_STR_SWEEP_UNLINK) {
      if (LJ_UNLIKELY(!hdr || lj_str_tabh_acq(g) != hdr)) {
	lj_str_gc2_sweep_abort(g);
	break;
      }
      n += str_sweep_unlink_one(g, hdr);
      continue;
    }
    if (phase == LJ_STR_SWEEP_UNLINK_GRACE) {
      if (gc2_sweep_grace_needed_acq(g))
	break;
      if (!str_sweep_grace_complete(g)) {
	gc2_sweep_grace_needed_rel(g, 1);
	n++;
	continue;
      }
      lj_str_sweep_phase_rel(g, LJ_STR_SWEEP_DONE);
      n++;
      continue;
    }
    lj_assertG(0, "bad GC2 string sweep phase");
    lj_str_gc2_sweep_abort(g);
    break;
  }
  return n;
}

void lj_str_gc2_sweep_abort(global_State *g)
{
  StrBodyRetire *pending;
  StrTabHdr *hdr;
  if (!g)
    return;
  hdr = lj_str_sweep_hdr_acq(g);
  if (hdr && (strtab_resize_acq(hdr) & LJ_STRTAB_SWEEP))
    strtab_release(hdr);
  lj_str_sweep_hdr_rel(g, NULL);
  lj_str_sweep_link_rel(g, NULL);
  pending = lj_str_sweep_pending_acq(g);
  lj_str_sweep_pending_rel(g, NULL);
  if (pending && lj_gc2_mem_registered(g, pending))
    lj_mem_free(g, pending, sizeof(*pending));
  lj_str_sweep_bucket_rel(g, 0);
  lj_str_sweep_grace_epoch_rel(g, 0);
  lj_str_sweep_phase_rel(g, LJ_STR_SWEEP_IDLE);
}

void lj_str_gc2_sweep_finish(global_State *g)
{
  if (!g)
    return;
  lj_assertG(lj_str_sweep_phase_acq(g) == LJ_STR_SWEEP_DONE,
	     "unfinished string sweep reached GC2 close");
  lj_str_gc2_sweep_abort(g);
}

static void strcanon_init(lua_State *L)
{
  global_State *g = G(L);
  GCSize size = lj_str_qtabsize(LJ_STR_CANON_MINMASK);
  StrCanonHdr *hdr = (StrCanonHdr *)lj_mem_new(L, size);
  memset(hdr, 0, size);
  hdr->mask = LJ_STR_CANON_MINMASK;
  lj_str_qmask_rel(g, hdr->mask);
  lj_str_qtabh_rel(g, hdr);
}

void LJ_FASTCALL lj_str_init(lua_State *L)
{
  global_State *g = G(L);
  g->str.seed = lj_prng_u64(&g->prng);
  strcanon_init(L);
  lj_str_resize(L, LJ_MIN_STRTAB-1);
}

uint32_t lj_str_reclaim_retired(global_State *g, uint64_t completed_epoch)
{
  StrBodyRetire *ret;
  StrTabHdr *hdr;
  StrCanonHdr *qhdr;
  uint32_t reclaimed = 0;
  if (!g || completed_epoch == 0)
    return 0;
  /* Body records are processed before header records. A deferred current-header
  ** body still needs that generation name for the active-pin check below. */
  ret = lj_str_body_retired_head_xchg_acqrel(g, NULL);
  while (ret && lj_gc2_mem_registered(g, ret)) {
    StrBodyRetire *next = lj_str_body_retired_next_acq(ret);
    uint64_t retire_epoch = lj_str_body_retired_epoch_acq(ret);
    StrTabHdr *rethdr = lj_str_body_retired_hdr_acq(ret);
    uint32_t status = la_load32_acq(&ret->status);
    int old_enough = completed_epoch >= retire_epoch &&
	completed_epoch - retire_epoch >= LJ_STR_SWEEP_GRACE_EPOCHS;
    lj_str_body_retired_next_rel(ret, NULL);
    /* Stage A quarantine records remain authoritative until the later
    ** QCOMMIT/E2 protocol lands. Never remove a bucket-visible record or body
    ** through the legacy prototype retire drain. */
    if (status & (LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_LIST_ONLY)) {
      str_body_retired_push(g, ret);
      ret = next;
      continue;
    }
    if (old_enough &&
	(lj_str_tabh_acq(g) != rethdr || !strtab_active_on_hdr(g, rethdr))) {
      GCstr *s = lj_str_body_retired_str_acq(ret);
      if (s && lj_gc2_mem_registered(g, s) &&
	  la_load8_acq(&s->gct) == (uint8_t)~LJ_TSTR) {
	lj_str_free(g, s);
	lj_str_sweep_reclaimed_add(g, 1);
      }
      lj_mem_free(g, ret, sizeof(*ret));
      reclaimed++;
    } else {
      str_body_retired_push(g, ret);
    }
    ret = next;
  }
  hdr = lj_str_retired_head_xchg_acqrel(g, NULL);
  while (hdr && lj_gc2_mem_registered(g, hdr)) {
    StrTabHdr *next = lj_str_retired_next_acq(hdr);
    lj_str_retired_next_rel(hdr, NULL);
    if (lj_str_retire_epoch_acq(hdr) < completed_epoch) {
      lj_mem_free(g, hdr, lj_str_tabbytes(hdr));
      reclaimed++;
    } else {
      strtab_retired_push(g, hdr);
    }
    hdr = next;
  }
  qhdr = lj_str_qretired_head_xchg_acqrel(g, NULL);
  while (qhdr && lj_gc2_mem_registered(g, qhdr)) {
    StrCanonHdr *next = lj_str_qretired_next_acq(qhdr);
    lj_str_qretired_next_rel(qhdr, NULL);
    if (lj_str_qretire_epoch_acq(qhdr) < completed_epoch &&
	!strq_active_on_hdr(g, qhdr)) {
      lj_mem_free(g, qhdr, lj_str_qtabbytes(qhdr));
      reclaimed++;
    } else {
      strq_retired_push(g, qhdr);
    }
    qhdr = next;
  }
  return reclaimed;
}

void lj_str_free_retired_bodies(global_State *g)
{
  StrBodyRetire *ret, *pending;
  if (!g)
    return;
  /* Workers are stopped before the terminal GC2 drain, so an unlink CAS cannot
  ** still be between its discoverable pending slot and retired-list publish. */
  pending = lj_str_sweep_pending_acq(g);
  lj_str_sweep_pending_rel(g, NULL);
  if (pending && lj_gc2_mem_registered(g, pending))
    lj_mem_free(g, pending, sizeof(*pending));
  ret = lj_str_body_retired_head_xchg_acqrel(g, NULL);
  while (ret && lj_gc2_mem_registered(g, ret)) {
    StrBodyRetire *next = lj_str_body_retired_next_acq(ret);
    GCstr *s = lj_str_body_retired_str_acq(ret);
    uint32_t status = la_load32_acq(&ret->status);
    if (!(status & (LJ_STR_CANONREC_Q_LINKED|
		    LJ_STR_CANONREC_LIST_ONLY)) ||
	(status & LJ_STR_CANONREC_BODY_OWNED)) {
      if (s && lj_gc2_mem_registered(g, s) &&
	la_load8_acq(&s->gct) == (uint8_t)~LJ_TSTR)
        lj_str_free(g, s);
    }
    lj_mem_free(g, ret, sizeof(*ret));
    ret = next;
  }
}

#ifdef LJ_STR_TEST_HELPERS
void lj_str_test_reset_sweep_counters(global_State *g)
{
  if (!g)
    return;
  la_store64_rlx(&g->str.sweep_tagged, 0);
  la_store64_rlx(&g->str.sweep_rescued, 0);
  la_store64_rlx(&g->str.sweep_unlinked, 0);
  la_store64_rlx(&g->str.sweep_reclaimed, 0);
}

void lj_str_test_sweep_snapshot(global_State *g,
				LJStrTestSweepSnapshot *snapshot)
{
  uint64_t unlinked, reclaimed;
  if (!snapshot)
    return;
  memset(snapshot, 0, sizeof(*snapshot));
  if (!g)
    return;
  unlinked = la_load64_acq(&g->str.sweep_unlinked);
  reclaimed = la_load64_acq(&g->str.sweep_reclaimed);
  snapshot->tagged = la_load64_acq(&g->str.sweep_tagged);
  snapshot->rescued = la_load64_acq(&g->str.sweep_rescued);
  snapshot->unlinked = unlinked;
  snapshot->reclaimed = reclaimed;
  snapshot->retired = unlinked >= reclaimed ? unlinked - reclaimed : 0;
  snapshot->phase = lj_str_sweep_phase_acq(g);
  snapshot->pending = (uint32_t)lj_str_gc2_sweep_pending(g);
}
#endif

void lj_str_freetab(global_State *g)
{
  StrTabHdr *hdr = lj_str_tabh_xchg_acqrel(g, NULL);
  StrCanonHdr *qhdr;
  if (hdr) {
    lj_mem_free(g, hdr, lj_str_tabbytes(hdr));
  }
  hdr = lj_str_retired_head_xchg_acqrel(g, NULL);
  while (hdr && lj_gc2_mem_registered(g, hdr)) {
    StrTabHdr *next = lj_str_retired_next_acq(hdr);
    lj_mem_free(g, hdr, lj_str_tabbytes(hdr));
    hdr = next;
  }
  qhdr = lj_str_qtabh_xchg_acqrel(g, NULL);
  if (qhdr)
    lj_mem_free(g, qhdr, lj_str_qtabbytes(qhdr));
  qhdr = lj_str_qretired_head_xchg_acqrel(g, NULL);
  while (qhdr && lj_gc2_mem_registered(g, qhdr)) {
    StrCanonHdr *next = lj_str_qretired_next_acq(qhdr);
    lj_mem_free(g, qhdr, lj_str_qtabbytes(qhdr));
    qhdr = next;
  }
}
