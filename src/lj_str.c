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
#if LJ_HASFFI && LJ_HASJIT
#include "lj_ccall.h"
#endif

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
LJ_STATIC_ASSERT(LJ_STRTAB_BUCKET_MASK < LJ_STRTAB_REHASH);
#define LJ_STRID_BLOCK		64u
#define LJ_STRNUM_BLOCK		64u
#define LJ_STR_SWEEP_GRACE_EPOCHS LJ_GC2_GRACE_EPOCHS

#ifdef LJ_STR_TEST_HELPERS
static uint32_t str_test_id_refills;
static uint32_t str_test_num_refills;
static LJStrTestMatchHook str_test_match_hook;
static LJStrTestCanonHook str_test_canon_hook;
static LJStrTestReclaimHook str_test_reclaim_hook;

void lj_str_test_set_match_hook(LJStrTestMatchHook hook)
{
  str_test_match_hook = hook;
}

void lj_str_test_set_canon_hook(LJStrTestCanonHook hook)
{
  str_test_canon_hook = hook;
}

void lj_str_test_set_reclaim_hook(LJStrTestReclaimHook hook)
{
  str_test_reclaim_hook = hook;
}

static LJ_AINLINE void str_test_canon(lua_State *L, GCstr *s,
				      StrCanonRec *rec, uint32_t stage)
{
  LJStrTestCanonHook hook = str_test_canon_hook;
  if (hook)
    hook(L, s, rec, stage);
}

static LJ_AINLINE void str_test_reclaim(global_State *g, uint32_t stage)
{
  LJStrTestReclaimHook hook = str_test_reclaim_hook;
  if (hook)
    hook(g, stage);
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
#define str_test_reclaim(g, stage) \
  ((void)(g), (void)(stage))
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
  MSize state = lj_str_resize_state_acq(hdr);
  lj_assertX(state == 0 || state == LJ_STRTAB_RESIZE ||
	     state == LJ_STRTAB_SWEEP ||
	     (state & ~LJ_STRTAB_BUCKET_MASK) == LJ_STRTAB_REHASH,
	     "invalid string table topology claim");
  return state;
}

static LJ_AINLINE int strtab_intern_blocked(global_State *g, StrTabHdr *hdr,
					    MSize owner, StrHash sparse,
					    const char *bytes, MSize len)
{
  if (owner & LJ_STRTAB_RESIZE)
    return 1;
#if LUAJIT_SECURITY_STRHASH
  if (owner & LJ_STRTAB_REHASH) {
    MSize source = owner & LJ_STRTAB_BUCKET_MASK;
    MSize primary = sparse & hdr->mask;
    /* Every access to the source chain is excluded, either as its primary
    ** bucket or as the dense destination of an already-rehashed primary.
    ** Secondary flags on all other primary buckets remain stable. */
    if (primary == source)
      return 1;
    if (lj_str_link_load_acq(&hdr->bucket[primary]) & LJ_STRHASH_SECONDARY)
      return (hash_dense(g->str.seed, sparse, bytes, len) & hdr->mask) == source;
  }
#else
  UNUSED(g); UNUSED(hdr); UNUSED(sparse); UNUSED(bytes); UNUSED(len);
#endif
  return 0;
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

/* Claim link rewrites without waiting behind a pinned lookup or publisher. */
static int strq_claim(global_State *g, StrCanonHdr *hdr)
{
  MSize expect = 0;
  if (!hdr || !la_cas32(&hdr->resize, &expect, 1, LA_ACQ_REL, LA_ACQ))
    return 0;
  la_fence_seq();
  if (lj_str_qtabh_acq(g) != hdr || strq_active_on_hdr(g, hdr)) {
    la_store32_rel(&hdr->resize, 0);
    return 0;
  }
  return 1;
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
    strq_active_enter(tg, hdr);
    /* Validate the generation before dereferencing the sampled header.
    ** The topology claimant must see the pin or this actor must see its claim. */
    la_fence_seq();
    if (LJ_LIKELY(lj_str_qtabh_acq(g) == hdr &&
		  la_load32_acq(&hdr->resize) == 0))
      return hdr;
    strq_active_leave(tg, hdr);
    strtab_wait(L);
  }
}

static void strq_leave(lua_State *L, StrCanonHdr *hdr)
{
  strq_active_leave(L2TG(L), hdr);
}

static int strtab_topology_claim(StrTabHdr *hdr, MSize state)
{
  uintptr_t expect = lj_str_topology_acq(hdr);
  uintptr_t claim = ((uintptr_t)state << LJ_STRTAB_TOPO_SHIFT) | expect;
  return expect <= LJ_STRTAB_TOPO_SECOND &&
    (expect & LJ_STRTAB_TOPO_EXPAND) == 0 &&
    la_casuptr(&hdr->topology, &expect, claim, LA_ACQ_REL, LA_ACQ);
}

static void strtab_release(StrTabHdr *hdr)
{
  la_storeuptr_rel(&hdr->topology, hdr->has_secondary ?
                  LJ_STRTAB_TOPO_SECOND : 0);
}

static int strtab_claim(lua_State *L, StrTabHdr *hdr, MSize state)
{
  global_State *g = G(L);
  /* Resize and secondary rehash never wait behind either destructive owner. */
  if (!strtab_topology_claim(hdr, state))
    return 0;
  /* Growth and secondary rehash are opportunistic. A paused reader keeps its
  ** generation, but must not keep the claim held against new interners.
  ** The paired fences prevent both sides from missing the other's
  ** publication. */
  la_fence_seq();
  if (strtab_active_on_hdr(g, hdr)) {
    strtab_release(hdr);
    return 0;
  }
  return 1;
}

int lj_str_sweep_claim(lua_State *L, StrTabHdr *hdr)
{
  /*
  ** Sweep owns stable bucket topology against resize/secondary rehash, but it
  ** never excludes or drains ordinary interners. All shared-link changes use
  ** exact CAS and body reclamation is deferred to a later grace period.
  */
  if (!hdr || strtab_resize_acq(hdr) != 0 ||
      !strtab_topology_claim(hdr, LJ_STRTAB_SWEEP))
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
  if (!g || !hdr || strtab_resize_acq(hdr) != 0 ||
      !strtab_topology_claim(hdr, LJ_STRTAB_SWEEP))
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

static StrTabHdr *strtab_enter(lua_State *L, global_State *g, StrHash sparse,
			       const char *bytes, MSize len, int *blocked)
{
  TGState *tg = L2TG(L);
  for (;;) {
    StrTabHdr *hdr = lj_str_tabh_acq(g);
    MSize owner;
    int nested;
    if (hdr == NULL)
      return NULL;
    nested = lj_tg_strtab_active_depth_acq(tg) != 0 &&
	     lj_tg_strtab_active_hdr_acq(tg) == hdr;
    if (!nested && (strtab_resize_acq(hdr) & LJ_STRTAB_RESIZE)) {
      *blocked = 1;
      return NULL;
    }
    strtab_active_enter(tg, hdr);  /* 06 section 6.5 RCU header pin. */
    /* Order pin publication before checking the destructive topology claim.
    ** Only inspect a primary bucket's mode after retaining this header. */
    if (!nested)
      la_fence_seq();
    if (LJ_LIKELY(nested || lj_str_tabh_acq(g) == hdr)) {
      owner = strtab_resize_acq(hdr);
      if ((nested && !(owner & LJ_STRTAB_REHASH)) ||
	  !strtab_intern_blocked(g, hdr, owner, sparse, bytes, len))
	return hdr;
      strtab_active_leave(tg, hdr);
      *blocked = 1;
      return NULL;
    }
    strtab_active_leave(tg, hdr);
  }
}

/* Freeze the incoming edge before any helper uses the old bucket. A
** prior interner can still finish a hit, but its old-head insert CAS loses. */
static uintptr_t strtab_freeze_bucket(lua_State *L, GCRef *head)
{
  uintptr_t edge = lj_str_link_load_acq(head);
  while (!(edge & LJ_STRHASH_FROZEN)) {
    if (lj_str_link_cas_acqrel(head, &edge, edge | LJ_STRHASH_FROZEN)) {
#ifdef LJ_STR_TEST_HELPERS
      str_test_canon(L, NULL, NULL, LJ_STR_TEST_CANON_EXPAND_FROZEN);
#else
      UNUSED(L);
#endif
      break;
    }
  }
  return edge;
}

/* The frozen sources remain readable and every helper sees the same order.
** For doubling one source maps to two destinations. For plain shrink, all
** source buckets in a group must be frozen before their merged head opens. */
static void strtab_help_expand_bucket(lua_State *L, StrTabHdr *source,
                                      StrTabHdr *target, MSize cursor)
{
  MSize i = cursor - 1u;
  int grow = source->mask < target->mask;
  MSize step = (grow ? source->mask : target->mask) + 1u;
  GCobj *first[2] = { NULL, NULL }, *last[2] = { NULL, NULL }, *o;
  uintptr_t edge, marker = 0;
  MSize k;
  for (k = i; k <= source->mask; k += step) {
    edge = strtab_freeze_bucket(L, &source->bucket[k]);
    marker |= edge & LJ_STRHASH_SECONDARY;
  }
  for (k = i; k <= source->mask; k += step) {
    edge = lj_str_link_load_acq(&source->bucket[k]);
    for (o = lj_str_link_target(edge); o;
         o = lj_str_hashhead_acq(lj_str_nodelink(source, o))) {
      MSize side = grow && (gco2str(o)->hash & step);
      if (last[side])
        lj_str_ref_store_rel(lj_str_nodelink(target, last[side]), (uintptr_t)o);
      else
        first[side] = o;
      last[side] = o;
    }
  }
  /* Only first publication may replace the unset sentinel. Target chains
  ** cannot be rehashed or swept while an old-generation helper is pinned. */
  for (k = 0; k <= (MSize)grow; k++) {
    uintptr_t unset = LJ_STRHASH_FROZEN;
    if (last[k])
      lj_str_ref_store_rel(lj_str_nodelink(target, last[k]), 0);
    (void)lj_str_link_cas_acqrel(&target->bucket[i + k * step],
                                 &unset, (uintptr_t)first[k] | marker);
  }
  if (la_cas32(&target->copy_cursor, &cursor, i, LA_ACQ_REL, LA_ACQ)) {
#ifdef LJ_STR_TEST_HELPERS
    str_test_canon(L, NULL, NULL, LJ_STR_TEST_CANON_EXPAND_BUCKET_DONE);
#endif
  }
}

static void strtab_help_expand(lua_State *L, StrTabHdr *source,
                               StrTabHdr *target, MSize until)
{
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  MSize cursor;
  if (la_load32_acq(&target->copy_cursor) <= until)
    goto finish;
  strtab_active_enter(tg, source);
  la_fence_seq();
  if (lj_str_tabh_acq(g) == source) {
    while ((cursor = la_load32_acq(&target->copy_cursor)) > until)
      strtab_help_expand_bucket(L, source, target, cursor);
  }
  strtab_active_leave(tg, source);

finish:
  if (la_load32_acq(&target->copy_cursor) == 0) {
    void *expect = source;
    /* Once cursor reaches zero no new helper can start reading source
    ** links. Admission to the ready target needs no old-header
    ** pin; if a prior reader remains, leave the handoff for a later call. */
    la_fence_seq();
    if (!strtab_active_on_hdr(g, source) &&
        la_casptr((void **)&g->str.tabh, &expect, target, LA_ACQ_REL, LA_ACQ)) {
      strtab_retire(g, source);
    }
  }
}

/* Terminal shutdown joins mutators and collector workers before dropping
** string bodies. Finalize a copy left unpublished by an earlier reader. */
void lj_str_expand_terminal(lua_State *L)
{
  global_State *g = G(L);
  StrTabHdr *source = lj_str_tabh_acq(g);
  StrTabHdr *target = source ? lj_str_expand_target(lj_str_topology_acq(source)) :
                               NULL;
  if (!target)
    return;
  if (!lj_gc2_smr_read_try(g))
    abort();
  strtab_help_expand(L, source, target, 0);
  lj_gc2_smr_read_leave(g);
  if (lj_str_tabh_acq(g) == source)
    abort();
}

/* During a helpable expansion the target primary marker may still be unset
** even when its dense route is ready. Select the hash with the source marker
** until global handoff. During plain shrink the hash mode stays sparse. */
static StrTabHdr *strtab_enter_completed(lua_State *L, global_State *g,
                                        StrHash sparse, const char *bytes,
                                        MSize len, int *selected_alg)
{
  TGState *tg = L2TG(L);
  StrTabHdr *source, *target, *result = NULL;
  int hashalg = -1;
  if (lj_tg_strtab_active_depth_acq(tg) != 0 || !lj_gc2_smr_read_try(g))
    return NULL;
  source = lj_str_tabh_acq(g);
  if (source && strtab_resize_acq(source) == LJ_STRTAB_RESIZE) {
    uintptr_t topology = lj_str_topology_acq(source);
    MSize i = sparse & source->mask;
    target = lj_str_expand_target(topology);
    if (target) {
#if LUAJIT_SECURITY_STRHASH
      if (lj_str_hashsecondary_acq(&source->bucket[i])) {
        hashalg = 1;
        i = hash_dense(g->str.seed, sparse, bytes, len) & source->mask;
      } else {
        hashalg = 0;
      }
#endif
      strtab_help_expand(L, source, target, i & target->mask);
    } else {
      target = (StrTabHdr *)la_loadptr_acq((void *const *)&source->expand_to);
      if (i < la_load32_acq(&source->copy_cursor))
        target = NULL;
    }

    if (target) {
      StrTabHdr *current;
      strtab_active_enter(tg, target);
      /* The target can become current during admission. Any following
      ** destructive owner must observe this pin or be seen on recheck. */
      la_fence_seq();
      current = lj_str_tabh_acq(g);
      if ((current == source || current == target) &&
          !strtab_intern_blocked(g, target, strtab_resize_acq(target),
                                 sparse, bytes, len)) {
        *selected_alg = current == source ? hashalg : -1;
        result = target;
      } else {
        strtab_active_leave(tg, target);
      }
    }
  }
  lj_gc2_smr_read_leave(g);
  return result;
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
  int expand;

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

  if (oldhdr && (oldmask & (oldmask + 1u)) == 0 &&
      (newmask & (newmask + 1u)) == 0 &&
      (newmask == (oldmask << 1) + 1u ||
       (newmask < oldmask &&
        !(lj_str_topology_acq(oldhdr) & LJ_STRTAB_TOPO_SECOND)))) {
    uintptr_t expect = lj_str_topology_acq(oldhdr);
    MSize routemask = oldmask < newmask ? oldmask : newmask;
    if ((expect & ~LJ_STRTAB_TOPO_SECOND) != 0 ||
        (newmask < oldmask && expect != 0) ||
        lj_tg_strtab_active_depth_acq(L2TG(L)) != 0 ||
        !lj_gc2_smr_read_try(g)) {
      lj_mem_free(g, newhdr, newsize);
      return;
    }
    newhdr->link_slot = 1u - oldhdr->link_slot;
    newhdr->has_secondary = (expect & LJ_STRTAB_TOPO_SECOND) != 0;
    newhdr->topology = expect;
    newhdr->copy_cursor = routemask + 1u;
    for (i = 0; i <= newmask; i++)
      lj_str_ref_store_rel(&newtab[i], LJ_STRHASH_FROZEN);
    if (lj_str_tabh_acq(g) == oldhdr &&
        la_casuptr(&oldhdr->topology, &expect,
                   (uintptr_t)newhdr | LJ_STRTAB_TOPO_EXPAND, LA_ACQ_REL, LA_ACQ)) {
#ifdef LJ_STR_TEST_HELPERS
      str_test_canon(L, NULL, NULL, LJ_STR_TEST_CANON_EXPAND_PUBLISHED);
#endif
      lj_gc2_markmem(g, newhdr);
      strtab_help_expand(L, oldhdr, newhdr, 0);
      lj_gc2_smr_read_leave(g);
      return;
    }
    lj_gc2_smr_read_leave(g);
    lj_mem_free(g, newhdr, newsize);
    return;
  }

  if (oldhdr) {
    if (!strtab_claim(L, oldhdr, LJ_STRTAB_RESIZE)) {
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

  newhdr->link_slot = oldhdr ? oldhdr->link_slot : 0;

  expand = oldhdr && oldmask < newmask &&
	   (oldmask & (oldmask + 1u)) == 0 &&
	   (newmask & (newmask + 1u)) == 0 && !lj_str_second_acq(g);
  if (expand) {
    la_store32_rlx(&oldhdr->copy_cursor, oldmask + 1u);
    la_storeptr_rel((void **)&oldhdr->expand_to, newhdr);
    /* Keep the pending target visible across an asynchronous root scan
    ** racing the ownership-edge publication. */
    lj_gc2_markmem(g, newhdr);
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
	o = lj_str_hashhead_acq(lj_str_nodelink(oldhdr, o));
      }
    }
    /* Mark secondary chains. */
    for (i = newmask; i != ~(MSize)0; i--) {
      int secondary = lj_str_ref_load_acq(&newtab[i]) > LJ_STR_MAXCOLL;
      newsecond |= secondary;
      lj_str_ref_store_rel(&newtab[i],
			   secondary ? LJ_STRHASH_SECONDARY : (uintptr_t)0);
    }
    newhdr->has_secondary = (uint8_t)newsecond;
    strtab_release(newhdr);
    lj_str_second_rel(g, (uint8_t)newsecond);
  }
#endif

  /* Reinsert all strings from the old table into the new table. */
  for (i = oldmask; i != ~(MSize)0; i--) {
    uintptr_t oldlink = lj_str_link_load_acq(&oldtab[i]);
    GCobj *o;
    while ((o = lj_str_link_target(oldlink)) != NULL) {
      uintptr_t nextlink = lj_str_link_load_acq(lj_str_nodelink(oldhdr, o));
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
      lj_str_ref_store_rel(lj_str_nodelink(newhdr, o), u & ~LJ_STRHASH_SECONDARY);
      lj_str_bucket_store_rel(&newtab[hash], o,
			      (u & LJ_STRHASH_SECONDARY) | dead);
#else
      hash &= newmask;
      u = lj_str_ref_load_acq(&newtab[hash]);
      /* NOBARRIER: The string table is a GC root. */
      lj_str_ref_store_rel(lj_str_nodelink(newhdr, o), u & ~LJ_STRHASH_SECONDARY);
      lj_str_bucket_store_rel(&newtab[hash], o, dead);
#endif
#ifdef LJ_STR_TEST_HELPERS
      str_test_canon(L, s, NULL, LJ_STR_TEST_CANON_RESIZE_MOVED);
#endif
      oldlink = nextlink;
    }
    if (expand) {
      /* Release publishes the complete target chains for this source. */
      la_store32_rel(&oldhdr->copy_cursor, i);
#ifdef LJ_STR_TEST_HELPERS
      str_test_canon(L, NULL, NULL, LJ_STR_TEST_CANON_EXPAND_BUCKET_DONE);
#endif
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
  MSize i;
  if (newmask >= LJ_MAX_STRTAB-1 || (newmask & (newmask + 1u)) != 0)
    return 0;
  newsize = lj_str_qtabsize(newmask);
  newhdr = (StrCanonHdr *)lj_mem_new(L, newsize);
  memset(newhdr, 0, newsize);
  newhdr->mask = newmask;
  /* Hold the sampled header until claim validation, even if another owner
  ** replaces it while this actor is paused before the claim. */
  if (!lj_gc2_smr_read_try(g)) {
    lj_mem_free(g, newhdr, newsize);
    return 0;
  }
  oldhdr = lj_str_qtabh_acq(g);
  if (!oldhdr || newmask <= oldhdr->mask || !strq_claim(g, oldhdr)) {
    lj_gc2_smr_read_leave(g);
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
  lj_gc2_smr_read_leave(g);
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
  if (!hdr || !strtab_claim(L, hdr, LJ_STRTAB_REHASH | (hashc & hdr->mask)))
    return 0;
  if (lj_str_tabh_acq(g) != hdr) {
    strtab_release(hdr);
    return 0;
  }
  strtab = hdr->bucket;
  strmask = hdr->mask;
  oldlink = lj_str_link_load_acq(&strtab[hashc & strmask]);
  lj_str_ref_store_rel(&strtab[hashc & strmask], LJ_STRHASH_SECONDARY);
  hdr->has_secondary = 1;
  lj_str_second_rel(g, 1);
  while ((o = lj_str_link_target(oldlink)) != NULL) {
    uintptr_t u;
    uintptr_t nextlink = lj_str_link_load_acq(lj_str_nodelink(hdr, o));
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
#ifdef LJ_STR_TEST_HELPERS
    str_test_canon(L, s, NULL, LJ_STR_TEST_CANON_REHASH_DEST_SAMPLED);
#endif
    /* Unrelated interners may prepend concurrently to this destination.
    ** The source chain has already been detached under the rehash claim. */
    do {
      lj_str_ref_store_rel(lj_str_nodelink(hdr, o), u & ~LJ_STRHASH_SECONDARY);
    } while (!lj_str_link_cas_acqrel(&strtab[hash], &u,
				    (uintptr_t)o |
				    (u & LJ_STRHASH_SECONDARY) | dead));
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
  GCstr *s = (GCstr *)lj_mem_newgco_raw(L, lj_str_size(len),
					 LJ_AF_TRAVERSABLE);
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
  /* The string table is the ownership root, but typed readers still need the
  ** same exact header-ready lifetime certificate as root-spine GC objects. */
  lj_gc_publishobj_header(g, obj2gco(s));
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
  if (lj_str_canon_acq(gco2str(o)) != LJ_STR_CANON_LIVE &&
      !lj_str_gc2_rescue_held(gco2str(o), NULL))
    return 0;
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

/* The hazard is visible to string retirement even while generic SMR rejects
** lookups. A source pointer is only a scalar until the caller revalidates its
** strong edge. Do not publish it as a semantic root before that recheck. */
TGState *lj_str_source_pin(lua_State *L, GCstr *s)
{
  global_State *g = G(L);
  TGState *tg = lj_thr_get_tg_fallback(g);
  if (!tg || la_loadptr_acq((void *const *)&tg->str_source_pin))
    return NULL;
  if (tg != g->main_tg) {
    LJTGSlotSnap snap;
    if (lj_tgregistry_key_snapshot(&tg->registry_key, &snap) !=
	LJ_TGSLOT_OK || snap.state != LJ_TGSLOT_LIVE)
      return NULL;
  }
  la_storeptr_rel((void **)&tg->str_source_pin, s);
  la_fence_seq();
  return tg;
}

void lj_str_source_unpin(TGState *tg)
{
  la_storeptr_rel((void **)&tg->str_source_pin, NULL);
}

static int str_source_pinned(global_State *g, GCstr *s)
{
  TGState *tg;
  /* Pairs with the reader's pre-validation fence. A reader may only accept
  ** an exact source edge; retirement may only destroy a body whose strong
  ** edges have already been closed. */
  la_fence_seq();
  if (la_loadptr_acq((void *const *)&g->main_tg->str_source_pin) == s)
    return 1;
  for (tg = gc2_tg_list_acq(g); tg != NULL;) {
    TGState *next = lj_tg_next_acq(tg);
    if (la_loadptr_acq((void *const *)&tg->str_source_pin) == s)
      return 1;
    if (next == tg)
      break;
    tg = next;
  }
  return 0;
}

/* The caller retains the exact body across this arbitration. Directory
** readers also bind the reservation they observed; a strong semantic edge
** can rescue whichever record still owns the exact retained body. */
int lj_str_gc2_rescue_held(GCstr *s, StrCanonRec *expected)
{
  uintptr_t canon = lj_str_canon_acq(s);
  for (;;) {
    StrCanonRec *rec = lj_str_canon_record(canon);
    uint32_t state = lj_str_canon_state(canon);
    uintptr_t want;
    if (expected && expected != rec)
      return 0;
    if (canon == LJ_STR_CANON_LIVE ||
	(rec && state == LJ_STR_CANON_QRESCUED))
      return 1;
    if (canon == LJ_STR_CANON_CANDIDATE)
      want = LJ_STR_CANON_LIVE;
    else if (rec && (state == LJ_STR_CANON_QACTIVE ||
		     state == LJ_STR_CANON_QCLOSING))
      want = lj_str_canon_pack(rec, LJ_STR_CANON_QRESCUED);
    else
      return 0;
    if (lj_str_canon_cas(s, &canon, want))
      return 1;  /* Rescue and logical-death arbitration LP. */
  }
}

/* Consult the authoritative secondary canonical directory after a true main
** table miss. The pin retains records in the sampled generation. An exact
** body lease holds a candidate while inspecting its header and bytes.
** A losing body admission cannot certify absence: interning must retry after
** releasing any main-table pin. */
static GCstr *strcanon_lookup(lua_State *L, const char *str, MSize len,
			     StrHash hash, int *retry)
{
  global_State *g = G(L);
  StrCanonHdr *hdr;
  StrCanonRec *rec;
  GCstr *found = NULL;
  *retry = 0;
  if (lj_str_qcount_acq(g) == 0)
    return NULL;
  hdr = strq_enter(L, g);
  if (!hdr)
    return NULL;
  for (rec = lj_str_qbucket_acq(&hdr->bucket[hash & hdr->mask]);
       rec != NULL;
       rec = lj_str_qnext_acq(rec)) {
    GCstr *s;
    LJGC2Lease lease;
    TValue value;
    uintptr_t canon;
    int status;
    if (rec->hash != hash || rec->len != len)
      continue;
    s = (GCstr *)la_loadptr_acq((void *const *)&rec->str);
    if (!s)
      continue;
    setgcVraw(&value, obj2gco(s), LJ_TSTR);
    status = lj_gc2_tv_lease_acquire(g, &value, &lease);
    if (status != LJ_GC2_TV_EDGE_VALID) {
      if (la_loadptr_acq((void *const *)&rec->str) == s) {
	*retry = 1;
	break;
      }
      continue;
    }
    canon = lj_str_canon_acq(s);
    if (la_loadptr_acq((void *const *)&rec->str) == s &&
	lj_str_canon_record(canon) == rec &&
	lj_str_canon_state(canon) < LJ_STR_CANON_QCOMMIT &&
	s->len == len) {
#ifdef LJ_STR_TEST_HELPERS
      str_test_canon(L, s, rec, LJ_STR_TEST_CANON_Q_BODY_HELD);
#endif
      if (memcmp(str, strdata(s), len) == 0 && lj_str_gc2_rescue_held(s, rec)) {
	strtab_mark_before_publish(g, obj2gco(s));
	canon = lj_str_canon_acq(s);
	if (lj_str_canon_state(canon) == LJ_STR_CANON_QRESCUED &&
	    lj_str_canon_record(canon) == rec)
	  found = s;
      }
    }
    lj_gc2_lease_release(&lease);
    if (found)
      break;
  }
  strq_leave(L, hdr);
  return found;
}

static LJ_AINLINE GCstr **strtab_resize_hit_slot(global_State *g, StrHash hash)
{
  return &g->str.resize_hits[hash & (LJ_STR_RESIZE_HIT_COUNT - 1u)];
}

/* Destructive owners only rewrite topology and derived hashes. An already
** published canonical hit has immutable bytes independent of those fields.
** The cache edge is invalidated before destruction. An exact body lease
** followed by edge revalidation admits only the currently cached incarnation,
** even if the address was reused between the first load and admission.
** A cache miss cannot prove that the canonical body is absent. */
static GCstr *strtab_resize_hit(lua_State *L, const char *str, MSize len,
			       StrHash hash)
{
  global_State *g = G(L);
  GCstr **slot = strtab_resize_hit_slot(g, hash);
  GCstr *s, *found = NULL;
  LJGC2Lease lease;
  TValue value;
  /* Use the alternate lookup only while the live-peer predicate itself
  ** excludes the existing sole-body retirement path. */
  if (mt_live_acq(g) == 0 || lj_str_reclaim_exclusive_acq(g) != 0)
    return NULL;
  s = (GCstr *)la_loadptr_acq((void *const *)slot);
  if (!s)
    return NULL;
  setgcVraw(&value, obj2gco(s), LJ_TSTR);
  if (lj_gc2_tv_lease_acquire(g, &value, &lease) != LJ_GC2_TV_EDGE_VALID)
    return NULL;
  if (la_loadptr_acq((void *const *)slot) == s && s->len == len &&
      memcmp(str, strdata(s), len) == 0 && lj_str_gc2_rescue_held(s, NULL)) {
    strtab_mark_before_publish(g, obj2gco(s));
    found = s;
  }
  lj_gc2_lease_release(&lease);
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
      int blocked = 0, selected_alg = -1;
      StrTabHdr *hdr = strtab_enter(L, g, qhash, str, len, &blocked);
      GCRef *strtab;
      GCRef *head, *link;
      GCobj *o;
      MSize mask, coll = 0;
      uintptr_t headu, u;
      uintptr_t want;
      int grow, qretry;
      if (!hdr && blocked)
	hdr = strtab_enter_completed(L, g, qhash, str, len, &selected_alg);
      if (LJ_UNLIKELY(hdr == NULL)) {
	if (blocked) {
	  GCstr *hit = strtab_resize_hit(L, str, len, qhash);
	  if (hit) {
	    if (news)
	      lj_mem_free(g, news, lj_str_size(news->len));
	    return hit;
	  }
	  strtab_wait(L);
	  continue;
	}
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
      headu = lj_str_link_load_acq(head);
#if LUAJIT_SECURITY_STRHASH
      if (LJ_UNLIKELY(selected_alg < 0 ? (headu & LJ_STRHASH_SECONDARY) :
                      selected_alg != 0)) {
	hashalg = 1;
	hash = hash_dense(g->str.seed, hash, str, len);
	head = &strtab[hash & mask];
	headu = lj_str_link_load_acq(head);
      }
#endif
      goto lookup;

retry_lookup:
      headu = lj_str_link_load_acq(head);
lookup:
      if (headu & LJ_STRHASH_FROZEN) {
        strtab_leave(L, hdr);
        continue;
      }
      coll = 0;
      link = head;
      u = headu;
      while ((o = lj_str_link_target(u)) != NULL) {
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
	link = lj_str_nodelink(hdr, o);
	u = lj_str_link_load_acq(link);
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
	qs = strcanon_lookup(L, str, len, qhash, &qretry);
	if (qs)
	  return qs;
	if (qretry) {
	  strtab_wait(L);
	  continue;
	}
	news = lj_str_alloc(L, str, len, hash, hashalg);
	continue;
      }

      news->hash = hash;
      news->hashalg = (uint8_t)hashalg;
      if (LJ_UNLIKELY(lj_str_qcount_acq(g) != 0)) {
	GCstr *qs = strcanon_lookup(L, str, len, qhash, &qretry);
	if (qs) {
	  strtab_leave(L, hdr);
	  lj_mem_free(g, news, lj_str_size(news->len));
	  return qs;
	}
	if (qretry) {
	  strtab_leave(L, hdr);
	  strtab_wait(L);
	  continue;
	}
      }
      /* The head snapshot covers the complete miss search. An intervening
      ** prepend invalidates it, so a losing CAS must search again. */
      strtab_mark_before_publish(g, obj2gco(news));
      lj_str_ref_store_rel(lj_str_nodelink(hdr, obj2gco(news)),
			   headu & ~LJ_STRHASH_SECONDARY);
      want = (uintptr_t)news | (headu & LJ_STRHASH_SECONDARY);
      u = headu;
      if (!lj_str_link_cas_acqrel(head, &u, want))  /* Intern linearization. */
	goto retry_lookup;
      la_storeptr_rel((void **)strtab_resize_hit_slot(g, qhash), news);
      grow = strnum_publish_success(g, L2TG(L), mask);
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

static int str_free_prepare_impl(global_State *g, GCstr *s,
				 LJGCDestructCtx *dctx, int reclaim_held)
{
  uint8_t gct = la_load8_acq(&s->gct);
  GCSize size;
  if (LJ_UNLIKELY(gct != (uint8_t)~LJ_TSTR))
    return LJ_GC_DESTRUCT_OWNED;
  size = lj_str_size(s->len);
  return reclaim_held ?
    lj_gc_destructor_enter_reclaim_held(g, s, size, dctx) :
    lj_gc_destructor_enter(g, s, size, dctx);
}

void lj_str_free_commit(global_State *g, GCstr *s, LJGCDestructCtx *dctx)
{
  uint8_t gct = la_load8_acq(&s->gct);
  GCSize size;
  if (LJ_UNLIKELY(!dctx || dctx->base != (void *)s || dctx->size == 0)) {
    lj_assertG(0, "invalid prepared string destructor context");
    abort();
  }
  size = dctx->size;
  /* Stop nonowning resize lookups before clearing the registered body. A
  ** colliding cache publication names a different body and is left alone. */
  if (gct == (uint8_t)~LJ_TSTR && s->len != 0) {
    StrHash hash = hash_sparse(g->str.seed, strdata(s), s->len);
    void *expected = s;
    (void)la_casptr((void **)strtab_resize_hit_slot(g, hash),
		    &expected, NULL, LA_ACQ_REL, LA_ACQ);
  }
  for (;;) {
    if (LJ_UNLIKELY(gct != (uint8_t)~LJ_TSTR)) {
      lj_assertG(0, "string type claim lost after destructor ownership");
      abort();
    }
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
  lj_mem_free(g, s, size);
  lj_gc_destructor_leave(g, dctx);
}

int lj_str_free_prepare(global_State *g, GCstr *s, LJGCDestructCtx *dctx)
{
  return str_free_prepare_impl(g, s, dctx, 0);
}

static int str_free_try_impl(global_State *g, GCstr *s, int reclaim_held)
{
  LJGCDestructCtx dctx;
  int acquired = str_free_prepare_impl(g, s, &dctx, reclaim_held);
  if (acquired != LJ_GC_DESTRUCT_ACQUIRED)
    return acquired;  /* RESCUE/loss or another terminal owner won. */
  lj_str_free_commit(g, s, &dctx);
  return LJ_GC_DESTRUCT_ACQUIRED;
}

int LJ_FASTCALL lj_str_free_try(global_State *g, GCstr *s)
{
  return str_free_try_impl(g, s, 0);
}

void LJ_FASTCALL lj_str_free(global_State *g, GCstr *s)
{
  (void)lj_str_free_try(g, s);
}

/* -- GC2 intern-table string reclamation ------------------------------- */

static LJ_AINLINE void str_sweep_cancel_candidate(GCobj *o)
{
  uintptr_t canon = lj_str_canon_acq(gco2str(o));
  if (canon == LJ_STR_CANON_CANDIDATE)
    (void)lj_str_canon_cas(gco2str(o), &canon, LJ_STR_CANON_LIVE);
}

static LJ_AINLINE int str_sweep_target_live(global_State *g, GCobj *o)
{
  int marked = lj_gc2_ismarked(g, o);
  /* Invalid/retiring bodies do not provide canonical-word authority. */
  if (marked < 0 || la_load8_acq(&o->gch.gct) != (uint8_t)~LJ_TSTR)
    return 1;
  if (marked == 0 &&
      (lj_obj_gcflags(o) & (LJ_GC_FIXED|LJ_GC_SFIXED)) == 0)
    return 0;
  str_sweep_cancel_candidate(o);
  return 1;
}

static void str_body_retired_push(global_State *g, StrBodyRetire *ret)
{
  StrBodyRetire *head = lj_str_body_retired_head_acq(g);
  do {
    lj_str_body_retired_next_rel(ret, head);
  } while (!lj_str_body_retired_head_cas(g, &head, ret));
}

/* -- b1.2 explicit sole-mutator body retirement ------------------------ */

void lj_str_gc2_reclaim_request(global_State *g)
{
  if (g)
    lj_str_reclaim_requested_rel(g, 1);
}

void lj_str_gc2_reclaim_cancel(global_State *g)
{
  if (!g)
    return;
  /* A synchronous full collection cannot return while its admitted string
  ** sweep is still active.  Keep this assertion close to the API cancellation
  ** edge so a future asynchronous caller cannot silently shorten exclusion. */
  lj_assertG(lj_str_reclaim_exclusive_acq(g) == 0,
	     "explicit string reclaim returned while exclusion was active");
  lj_str_reclaim_requested_rel(g, 0);
}

static void str_reclaim_exclusive_leave(global_State *g)
{
  if (g && lj_str_reclaim_exclusive_acq(g) != 0)
    lj_str_reclaim_exclusive_rel(g, 0);
}

static int str_reclaim_exclusive_try(global_State *g)
{
  TGState *tg;
  uint32_t expect = 0;
  if (!g || lj_str_reclaim_requested_acq(g) == 0 ||
      g->allocf != lj_arena_allocf ||
      la_load32_acq(&g->allocf_arena) == 0 ||
      lj_str_qcount_acq(g) != 0 || lj_gc2_finalizer_phase_pending(g))
    return 0;
  tg = lj_thr_get_tg();
  if (!tg || tg != g->main_tg || tg->gl != g ||
      !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return 0;
  /* A C or FFI caller can still hold a borrowed byte address. Callback
  ** entry temporarily clears in_native and can suspend jit_base, so those
  ** two fields alone do not certify that its outer native borrow ended. */
  if (lj_tg_in_native_acq(tg) != 0)
    return 0;
#if LJ_HASFFI
  if (lj_tg_ffi_call_func_acq(tg) || ccallback_depth_acq(&tg->cb) != 0)
    return 0;
#if LJ_HASJIT
  if (lj_ffi_native_frame_depth_acq(tg) != 0)
    return 0;
#endif
#endif
#if LJ_HASJIT
  /* MARK's EXIT_TRACES boundary normally establishes this already. Keep it an
  ** admission predicate so a future transition reorder cannot free a string
  ** body while the main TG still executes code that borrowed its payload. */
  if (lj_tg_any_jit_active(g))
    return 0;
#endif
  if (!lj_str_reclaim_exclusive_cas(g, &expect, 1))
    return 0;
  str_test_reclaim(g, LJ_STR_TEST_RECLAIM_EXCLUSIVE_CLAIMED);
  /* Entrants publish mt_entering and pool control publishes n_workers before
  ** the matching fence/recheck.  This fence is the other half of the
  ** store-buffering exclusion: either this owner sees their publication, or
  ** they see reclaim_exclusive and back out before touching shared VM state. */
  la_fence_seq();
  if (mt_shutdown_acq(g) != 0 || mt_live_acq(g) != 0 ||
      mt_entering_acq(g) != 0 || gc2_n_workers_acq(g) != 0 ||
      lj_str_sweep_batch_acq(g) != NULL ||
      lj_str_retired_batch_head_acq(g) != NULL) {
    str_reclaim_exclusive_leave(g);
    return 0;
  }
  /* No interner can consume a new count credit until exclusion is released.
  ** Make the count exact before physical destructors start subtracting it. */
  lj_str_flush_num_credit(g, tg);
  lj_str_reclaim_requested_rel(g, 0);
  return 1;
}

static StrRetireBatch *str_retire_batch_new(global_State *g, StrTabHdr *hdr)
{
  TGState *tg = lj_thr_get_tg();
  StrRetireBatch *batch;
  if (!g || !hdr || !tg || tg != g->main_tg || tg->gl != g ||
      !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) ||
      g->allocf != lj_arena_allocf)
    return NULL;
  batch = (StrRetireBatch *)lj_arena_allocd_alloc(&tg->allocd,
						   sizeof(*batch), 0);
  if (!batch)
    return NULL;
  memset(batch, 0, sizeof(*batch));
  batch->hdr = hdr;
  return batch;
}

static void str_retire_batch_push(global_State *g, StrRetireBatch *batch)
{
  StrRetireBatch *head = lj_str_retired_batch_head_acq(g);
  do {
    la_storeptr_rel((void **)&batch->next, head);
  } while (!lj_str_retired_batch_head_cas(g, &head, batch));
}

static void str_retire_batch_seal_current(global_State *g)
{
  StrRetireBatch *batch = lj_str_sweep_batch_acq(g);
  if (!batch)
    return;
  lj_assertG(lj_str_sweep_batch_pending_acq(g) == 0,
	     "string batch sealed across unlink handoff");
  if (batch->count == 0) {
    lj_str_sweep_batch_rel(g, NULL);
    lj_mem_free(g, batch, sizeof(*batch));
    return;
  }
  batch->retire_epoch = lj_gc2_retire_epoch(g);
  la_store32_rel(&batch->sealed, 1);
  /* Publish the durable list owner before clearing the current owner.  Both
  ** pointers can name the batch only inside this no-safepoint handoff. */
  str_retire_batch_push(g, batch);
  lj_str_sweep_batch_rel(g, NULL);
}

static StrRetireBatch *str_retire_batch_current(global_State *g,
						 StrTabHdr *hdr)
{
  StrRetireBatch *batch = lj_str_sweep_batch_acq(g);
  if (batch && batch->count == LJ_STR_RETIRE_BATCH_CAP) {
    str_retire_batch_seal_current(g);
    batch = NULL;
  }
  if (!batch) {
    batch = str_retire_batch_new(g, hdr);
    if (batch) {
      TGState *tg = lj_thr_get_tg();
      /* Make the initialized allocation globally discoverable before the
      ** accounting checkpoint can trigger a hard-limit assist. */
      lj_gc_total_add(g, sizeof(*batch));
      lj_str_sweep_batch_rel(g, batch);
      lj_gc2_account_alloc(g, tg, sizeof(*batch));
    }
  }
  return batch;
}

static uint32_t str_retire_batches_reclaim_all(global_State *g,
						int reclaim_held)
{
  StrRetireBatch *batch;
  uint32_t reclaimed = 0;
  str_retire_batch_seal_current(g);
  batch = lj_str_retired_batch_head_xchg_acqrel(g, NULL);
  while (batch) {
    StrRetireBatch *next;
    uint32_t i, count;
    lj_assertG(reclaim_held ?
	       lj_gc2_mem_registered_known_reclaim_held(g, batch) :
	       lj_gc2_mem_registered_known(g, batch),
	       "invalid sole-mutator string retirement batch");
    next = (StrRetireBatch *)la_loadptr_acq((void *const *)&batch->next);
    count = la_load32_acq(&batch->count);
    lj_assertG(la_load32_acq(&batch->sealed) != 0 &&
	       count != 0 && count <= LJ_STR_RETIRE_BATCH_CAP,
	       "invalid sealed string retirement batch");
    for (i = 0; i < count; i++) {
      GCstr *s = (GCstr *)la_loadptr_acq((void *const *)&batch->body[i]);
      uint8_t gct;
      int freed;
      lj_assertG(s != NULL && (reclaim_held ?
		 lj_gc2_mem_registered_known_reclaim_held(g, s) :
		 lj_gc2_mem_registered_known(g, s)),
		 "invalid sole-mutator retired string body");
      gct = la_load8_acq(&s->gct);
      if (gct == (uint8_t)~LJ_TSTR) {
	freed = str_free_try_impl(g, s, reclaim_held);
	if (LJ_UNLIKELY(freed == LJ_GC_DESTRUCT_LOST)) {
	  /* Releasing exclusion after losing an unlinked canonical body would
	  ** permit a duplicate identity. This narrow path therefore fails closed
	  ** instead of pretending the concurrent quarantine protocol exists. */
	  lj_assertG(0, "sole-mutator string destructor ownership lost");
	  abort();
	}
	if (freed == LJ_GC_DESTRUCT_ACQUIRED) {
	  lj_str_sweep_reclaimed_add(g, 1);
	  reclaimed++;
	}
      } else if (LJ_UNLIKELY(gct != 0)) {
	lj_assertG(0, "sole-mutator retired string type identity changed");
	abort();
      }
      la_storeptr_rel((void **)&batch->body[i], NULL);
    }
    lj_mem_free(g, batch, sizeof(*batch));
    batch = next;
  }
  return reclaimed;
}

#ifdef LJ_STR_TEST_HELPERS
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

static void strq_preserve_body(global_State *g, GCstr *s)
{
  uint32_t phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK)
    (void)lj_gc2_markobj_nogrey(g, obj2gco(s));
  else if (phase == LJ_GC2_SWEEP)
    (void)lj_gc2_preserve_sweep_root(g, obj2gco(s));
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
    la_storeptr_rel((void **)&rec->str, NULL);
    la_store32_rel(&rec->status,
	LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_CANCELLED);
    return 0;
  }
  la_storeptr_rel((void **)&rec->hdr, hdr);
  link = &hdr->bucket[s->hash & hdr->mask];
  while ((o = lj_str_link_target(u = lj_str_link_load_acq(link))) != NULL &&
	 o != obj2gco(s))
    link = lj_str_nodelink(hdr, o);
  if (!o) {
    la_storeptr_rel((void **)&rec->str, NULL);
    la_store32_rel(&rec->status,
	LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_CANCELLED);
    lj_str_sweep_release(hdr);
    return 0;
  }
  canon = LJ_STR_CANON_LIVE;
  want = lj_str_canon_pack(rec, LJ_STR_CANON_QACTIVE);
  if (!lj_str_canon_cas(s, &canon, want)) {
    la_storeptr_rel((void **)&rec->str, NULL);
    la_store32_rel(&rec->status,
	LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_CANCELLED);
    lj_str_sweep_release(hdr);
    return 0;
  }
  la_store32_rel(&rec->status,
	LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_BODY_OWNED);
  (void)lj_str_qcount_inc_sat_acqrel(g);
  epoch = lj_gc2_retire_epoch(g);
  lj_str_body_retired_epoch_rel(rec, epoch);
  /* Durable list ownership precedes this phase-aware retention barrier. If a
  ** new cycle starts after the barrier, its metadata scan sees the list; if it
  ** started before list publication, this barrier observes the active phase. */
  strq_preserve_body(g, s);
  next = lj_str_link_load_acq(lj_str_nodelink(hdr, obj2gco(s)));
  want = (next & ~(uintptr_t)LJ_STRHASH_SECONDARY) |
	 (u & LJ_STRHASH_SECONDARY);
  if (!lj_str_link_cas_acqrel(link, &u, want)) {
    canon = lj_str_canon_pack(rec, LJ_STR_CANON_QACTIVE);
    want = lj_str_canon_pack(rec, LJ_STR_CANON_QRESCUED);
    (void)lj_str_canon_cas(s, &canon, want);
    /* The main table still owns the body. Quarantine reserves identity but
    ** must not run the terminal body destructor a second time. */
    la_store32_rel(&rec->status, LJ_STR_CANONREC_Q_LINKED);
    strq_preserve_body(g, s);
    lj_str_sweep_release(hdr);
    return 0;
  }
  /* Remote handshakes can advance while this actor still owns its exact
  ** splice. Sample the boundary after removal, before settling the transfer. */
  la_store64_rel(&rec->main_unlink_epoch, lj_gc2_retire_epoch(g));
  la_store32_rel(&rec->main_linked, 0);
  strq_preserve_body(g, s);
  lj_str_sweep_release(hdr);
  /* This hook is deliberately after releasing the main topology claim: a
  ** concurrent interner can now observe a true main miss and must acquire the
  ** already-published non-zero Q count before consulting the directory. */
  str_test_canon(L, s, rec, LJ_STR_TEST_CANON_MAIN_UNLINKED);
  return 1;
}

/* The fixture certifies that the detached body has no unchecked native or
** managed references. Real sweep must establish that proof before admission. */
int lj_str_test_quarantine_close(global_State *g, GCstr *s)
{
  uintptr_t canon = lj_str_canon_acq(s);
  StrCanonRec *rec = lj_str_canon_record(canon);
  uint32_t status;
  if (!rec || lj_str_canon_state(canon) != LJ_STR_CANON_QACTIVE ||
      lj_str_body_retired_main_linked_acq(rec))
    return 0;
  status = la_load32_acq(&rec->status);
  if (status != (LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_BODY_OWNED) ||
      !lj_str_canon_cas(s, &canon,
                        lj_str_canon_pack(rec, LJ_STR_CANON_QCLOSING)))
    return 0;
  la_store64_rel(&rec->close_epoch, lj_gc2_retire_epoch(g));
  la_store32_rel(&rec->status, status|LJ_STR_CANONREC_CLOSE_PROVEN);
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
  str_retire_batch_seal_current(g);
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
  uintptr_t u, want, current, canon;
  GCobj *o;
  if (!link)
    return str_sweep_finish_tag(g);
  u = lj_str_link_load_acq(link);
  o = lj_str_link_target(u);
  if (!o)
    return str_sweep_advance_bucket(g, hdr, LJ_STR_SWEEP_TAG);

  if (str_sweep_target_live(g, o) ||
      ((canon = lj_str_canon_acq(gco2str(o))) != LJ_STR_CANON_LIVE &&
       canon != LJ_STR_CANON_CANDIDATE)) {
    if (u & LJ_STRHASH_DEAD) {
      current = u;
      want = u & ~(uintptr_t)LJ_STRHASH_DEAD;
      if (!lj_str_link_cas_acqrel(link, &current, want))
	return 1;
    }
    lj_str_sweep_link_rel(g, lj_str_nodelink(hdr, o));
    return 1;
  }

  /* CANDIDATE is the lifecycle authority. DEAD describes the incoming
  ** topology edge. A strong late root may rescue on the lifecycle word. */
  if (canon == LJ_STR_CANON_LIVE &&
      !lj_str_canon_cas(gco2str(o), &canon, LJ_STR_CANON_CANDIDATE))
    return 1;
  str_test_reclaim(g, LJ_STR_TEST_RECLAIM_CANDIDATE);
  if (!(u & LJ_STRHASH_DEAD)) {
    current = u;
    want = u | LJ_STRHASH_DEAD;
    if (!lj_str_link_cas_acqrel(link, &current, want)) {
      str_sweep_cancel_candidate(o);
      return 1;  /* A prepend moved this incoming edge: retry it. */
    }
    lj_str_sweep_tagged_add(g, 1);
    u = want;
  }
  /* A late root can race either publication. Cancel its candidate before
  ** clearing the hint, even if a subsequent prepend moved the incoming link. */
  if (str_sweep_target_live(g, o) ||
      lj_str_canon_acq(gco2str(o)) != LJ_STR_CANON_CANDIDATE) {
    current = u;
    want = u & ~(uintptr_t)LJ_STRHASH_DEAD;
    (void)lj_str_link_cas_acqrel(link, &current, want);
  }
  /* UNLINK restarts at every bucket head, so a later prepend cannot be missed. */
  lj_str_sweep_link_rel(g, lj_str_nodelink(hdr, o));
  return 1;
}

static uint32_t str_sweep_unlink_one(global_State *g, StrTabHdr *hdr)
{
  GCRef *link = lj_str_sweep_link_acq(g);
  uintptr_t u, current, want, next;
  StrRetireBatch *batch;
  GCstr *s;
  uint32_t slot;
  GCobj *o;
  if (!link)
    return str_sweep_finish_unlink(g, hdr);
  u = lj_str_link_load_acq(link);
  o = lj_str_link_target(u);
  if (!o)
    return str_sweep_advance_bucket(g, hdr, LJ_STR_SWEEP_UNLINK);
  if (!(u & LJ_STRHASH_DEAD)) {
    lj_str_sweep_link_rel(g, lj_str_nodelink(hdr, o));
    return 1;
  }

  /* A rescued/fixed/invalid target wins over an earlier tag observation. */
  if (str_sweep_target_live(g, o) ||
      lj_str_canon_acq(gco2str(o)) != LJ_STR_CANON_CANDIDATE) {
    current = u;
    want = u & ~(uintptr_t)LJ_STRHASH_DEAD;
    if (!lj_str_link_cas_acqrel(link, &current, want))
      return 1;
    lj_str_sweep_link_rel(g, lj_str_nodelink(hdr, o));
    return 1;
  }

  batch = str_retire_batch_current(g, hdr);
  if (!batch) {
    /* Metadata OOM retains canonical identity and retries in a later cycle. */
    str_sweep_cancel_candidate(o);
    current = u;
    want = u & ~(uintptr_t)LJ_STRHASH_DEAD;
    if (!lj_str_link_cas_acqrel(link, &current, want))
      return 1;
    lj_str_sweep_link_rel(g, lj_str_nodelink(hdr, o));
    return 1;
  }

  s = gco2str(o);
  slot = batch->count;
  lj_assertG(slot < LJ_STR_RETIRE_BATCH_CAP,
	     "full string retirement batch reached unlink");
  /* Publish the body slot and inclusive count before the unlink LP.  The
  ** current-batch pointer is already globally discoverable; pending prevents
  ** abort/terminal code from consuming the provisional last slot. */
  la_storeptr_rel((void **)&batch->body[slot], s);
  la_store32_rel(&batch->count, slot + 1u);
  lj_str_sweep_batch_pending_rel(g, 1);
  next = lj_str_link_load_acq(lj_str_nodelink(hdr, o));
  want = (next & ~(uintptr_t)LJ_STRHASH_SECONDARY) |
	 (u & LJ_STRHASH_SECONDARY);
  current = u;
  if (!lj_str_link_cas_acqrel(link, &current, want)) {
    la_store32_rel(&batch->count, slot);
    la_storeptr_rel((void **)&batch->body[slot], NULL);
    lj_str_sweep_batch_pending_rel(g, 0);
    return 1;
  }

  /* The inclusive count now describes a durable unlinked body. */
  lj_str_sweep_batch_pending_rel(g, 0);
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
  if (LJ_UNLIKELY(lj_str_sweep_pending_acq(g) != NULL)) {
    /* A pending record exists only inside unlink's no-safepoint handoff. A new
    ** sweep owner cannot clear or adopt it without racing the exact bucket CAS. */
    lj_assertG(0, "string sweep began during pending unlink handoff");
    abort();
  }
  if (lj_str_sweep_phase_acq(g) != LJ_STR_SWEEP_IDLE)
    lj_str_gc2_sweep_abort(g);
  lj_str_sweep_hdr_rel(g, NULL);
  lj_str_sweep_link_rel(g, NULL);
  lj_str_sweep_bucket_rel(g, 0);
  lj_str_sweep_grace_epoch_rel(g, 0);
  lj_str_sweep_batch_pending_rel(g, 0);
  la_store32_rel(&g->str.sweep_cycle, gc2_cycle_acq(g));
  /* The fully concurrent quarantine/commit protocol remains hard-disabled.
  ** b1.2 instead admits only an explicit, internal-allocator, sole-main-TG
  ** collection and holds that nonwaiting exclusion through physical free. */
  major = major && str_reclaim_exclusive_try(g);
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
	str_reclaim_exclusive_leave(g);
	lj_str_sweep_phase_rel(g, LJ_STR_SWEEP_DONE);
	n++;
	continue;
      }
      if (!strtab_gc2_claim(g, hdr)) {
	/* Failed claims are retry work, not a reason to park forever. */
	n++;
	continue;
      }
      /* Admission has excluded every new interner. A pre-admission native
      ** table pin would mean the sole-mutator proof was incomplete, so skip
      ** this pass before tagging or unlinking anything. */
      if (LJ_UNLIKELY(strtab_active_on_hdr(g, hdr))) {
	strtab_release(hdr);
	str_reclaim_exclusive_leave(g);
	lj_str_sweep_phase_rel(g, LJ_STR_SWEEP_DONE);
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
  StrTabHdr *hdr;
  if (!g)
    return;
  /* worker_active/phase-gate ownership makes this slot NULL at every legal
  ** abort boundary. Never xchg-and-free it: before the bucket CAS the worker
  ** still owns the record, and after that CAS the record owns an unlinked body.
  ** Either interpretation makes concurrent consumption a UAF or lost body. */
  if (LJ_UNLIKELY(lj_str_sweep_pending_acq(g) != NULL)) {
    lj_assertG(0, "string sweep abort crossed pending unlink handoff");
    abort();
  }
  if (LJ_UNLIKELY(lj_str_sweep_batch_pending_acq(g) != 0)) {
    lj_assertG(0, "string sweep abort crossed batched unlink handoff");
    abort();
  }
  hdr = lj_str_sweep_hdr_acq(g);
  if (hdr && (strtab_resize_acq(hdr) & LJ_STRTAB_SWEEP))
    strtab_release(hdr);
  lj_str_sweep_hdr_rel(g, NULL);
  lj_str_sweep_link_rel(g, NULL);
  lj_str_sweep_bucket_rel(g, 0);
  lj_str_sweep_grace_epoch_rel(g, 0);
  if (LJ_UNLIKELY(lj_str_sweep_batch_acq(g) != NULL ||
      lj_str_retired_batch_head_acq(g) != NULL)) {
    /* Once a body is unlinked, only the post-SWEEP exact reclaimer can consume
    ** its allocator generation. Every legal preserve/forced abort is gated
    ** before that point; reaching this path would otherwise strand identity. */
    lj_assertG(0, "string sweep abort crossed irreversible batch retirement");
    abort();
  }
  str_reclaim_exclusive_leave(g);
  lj_str_sweep_phase_rel(g, LJ_STR_SWEEP_IDLE);
}

void lj_str_gc2_sweep_finish(global_State *g)
{
  if (!g)
    return;
  lj_assertG(lj_str_sweep_phase_acq(g) == LJ_STR_SWEEP_DONE,
	     "unfinished string sweep reached GC2 close");
  lj_assertG(lj_str_sweep_batch_pending_acq(g) == 0 &&
	     lj_str_sweep_batch_acq(g) == NULL,
	     "unfinished batched string ownership reached GC2 close");
  lj_str_sweep_hdr_rel(g, NULL);
  lj_str_sweep_link_rel(g, NULL);
  lj_str_sweep_bucket_rel(g, 0);
  lj_str_sweep_grace_epoch_rel(g, 0);
  /* Keep reclaim_exclusive and the sealed batch list through the IDLE grace
  ** drain. Arena PREPSWEEP/PENDING state still vetoes a semantic destructor
  ** while SWEEP is active. */
  lj_str_sweep_phase_rel(g, LJ_STR_SWEEP_IDLE);
}

int lj_str_gc2_reclaim_complete(global_State *g)
{
  if (!g || lj_str_reclaim_exclusive_acq(g) == 0)
    return 1;
  if (lj_str_sweep_batch_acq(g) != NULL ||
      lj_str_retired_batch_head_acq(g) != NULL)
    return 0;
  str_reclaim_exclusive_leave(g);
  return 1;
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

static LJ_AINLINE int str_retire_node_known(global_State *g, const void *p,
					     int reclaim_held)
{
  return reclaim_held ?
    lj_gc2_mem_registered_known_reclaim_held(g, p) :
    lj_gc2_mem_registered_known(g, p);
}

/* Validate a detached intrusive chain before mutating or freeing any node.
** A duplicate node in a singly linked list necessarily forms a cycle, so
** Floyd detection is also duplicate detection. Every successor dereference is
** preceded by the appropriate exact-reclaimer or joined-world membership
** proof. Returning false never truncates or partially consumes the chain. */
#define STR_RETIRE_CHAIN_VALIDATOR(name, type, nextfn) \
static int name(global_State *g, type *head, int reclaim_held) \
{ \
  type *slow = head, *fast = head; \
  while (fast) { \
    if (slow) { \
      if (!str_retire_node_known(g, slow, reclaim_held)) return 0; \
      slow = nextfn(slow); \
    } \
    if (!str_retire_node_known(g, fast, reclaim_held)) return 0; \
    fast = nextfn(fast); \
    if (fast) { \
      if (!str_retire_node_known(g, fast, reclaim_held)) return 0; \
      fast = nextfn(fast); \
    } \
    if (fast && slow == fast) return 0; \
  } \
  return 1; \
}

STR_RETIRE_CHAIN_VALIDATOR(str_body_retired_chain_valid, StrBodyRetire,
			   lj_str_body_retired_next_acq)
STR_RETIRE_CHAIN_VALIDATOR(str_tab_retired_chain_valid, StrTabHdr,
			   lj_str_retired_next_acq)
STR_RETIRE_CHAIN_VALIDATOR(str_qretired_chain_valid, StrCanonHdr,
			   lj_str_qretired_next_acq)

#undef STR_RETIRE_CHAIN_VALIDATOR

static int str_retire_grace(uint64_t completed, uint64_t boundary)
{
  return completed >= boundary &&
    completed - boundary >= LJ_STR_SWEEP_GRACE_EPOCHS;
}

static void strq_count_remove(global_State *g)
{
  uint32_t old = lj_str_qcount_acq(g);
  while (old != UINT32_MAX) {
    if (LJ_UNLIKELY(old == 0)) {
      lj_assertG(0, "canonical quarantine count underflow");
      abort();
    }
    if (la_cas32(&g->str.qcount, &old, old - 1u, LA_ACQ_REL, LA_ACQ))
      break;
  }
}

/* All qnext rewrites share the resize claim. The caller retains record
** ownership until the post-unlink grace, including a cancelled publication. */
static int strq_unlink(global_State *g, StrCanonRec *rec)
{
  StrCanonHdr *hdr = lj_str_qtabh_acq(g);
  StrCanonRec **link, *cur;
  if (!strq_claim(g, hdr))
    return 0;
  link = &hdr->bucket[rec->hash & hdr->mask];
  while ((cur = lj_str_qbucket_acq(link)) != NULL && cur != rec)
    link = &cur->qnext;
  if (LJ_UNLIKELY(cur != rec)) {
    lj_assertG(0, "canonical quarantine lost a linked record");
    abort();
  }
  lj_str_qbucket_rel(link, lj_str_qnext_acq(rec));
  la_store32_rel(&hdr->resize, 0);
  return 1;
}

/* Publish the rescued body to the current main generation. Older chain
** readers must drain before its intrusive next link can be written again. */
static int strq_relink(global_State *g, StrCanonRec *rec, GCstr *s,
		      StrTabHdr *hdr, uint64_t completed)
{
  uint64_t unlink_epoch = la_load64_acq(&rec->main_unlink_epoch);
  GCRef *head;
  StrHash hash = rec->hash;
  uintptr_t edge, want;
  int hashalg = 0;
  if (lj_str_body_retired_main_linked_acq(rec))
    return 1;
  if (!str_retire_grace(completed, unlink_epoch) || !strtab_gc2_claim(g, hdr))
    return 0;
  if (strtab_active_on_hdr_before(g, hdr, unlink_epoch)) {
    strtab_release(hdr);
    return 0;
  }
  head = &hdr->bucket[hash & hdr->mask];
  edge = lj_str_link_load_acq(head);
#if LUAJIT_SECURITY_STRHASH
  if (edge & LJ_STRHASH_SECONDARY) {
    hashalg = 1;
    hash = hash_dense(g->str.seed, hash, strdata(s), s->len);
    head = &hdr->bucket[hash & hdr->mask];
    edge = lj_str_link_load_acq(head);
  }
#endif
  s->hash = hash;
  s->hashalg = (uint8_t)hashalg;
  lj_str_ref_store_rel(lj_str_nodelink(hdr, obj2gco(s)),
		       edge & ~LJ_STRHASH_SECONDARY);
  want = (uintptr_t)s | (edge & LJ_STRHASH_SECONDARY);
  if (!lj_str_link_cas_acqrel(head, &edge, want)) {
    strtab_release(hdr);
    return 0;
  }
  /* The body was already counted. Durable ownership transfers to main at
  ** publication, while the record still reserves the same identity. */
  la_store32_rel(&rec->main_linked, 1);
  la_store32_rel(&rec->status,
		la_load32_acq(&rec->status) & ~LJ_STR_CANONREC_BODY_OWNED);
  strtab_release(hdr);
  return 1;
}

/* The IDLE reclaimer serializes ownership changes. Semantic rescue always
** races logical death on the same lifecycle word. */
static int strq_retire_record(global_State *g, StrCanonRec *rec,
			      uint64_t completed, uint32_t status)
{
  GCstr *s = NULL;
  StrTabHdr *mainhdr = lj_str_tabh_acq(g);
  int owns_body = (status & LJ_STR_CANONREC_BODY_OWNED) != 0;
  int counted = owns_body;
  uintptr_t canon = 0, committed = 0;
  if (status & LJ_STR_CANONREC_LIST_ONLY)
    return 0;
  /* A body owner with the main edge still linked is inside its exact
  ** detach handoff. Do not consume its quarantine reservation. */
  if (owns_body && lj_str_body_retired_main_linked_acq(rec))
    return 0;
  if (owns_body || (status & LJ_STR_CANONREC_Q_LINKED &&
		    !(status & LJ_STR_CANONREC_CANCELLED))) {
    s = lj_str_body_retired_str_acq(rec);
    if (!s || !lj_gc2_mem_registered_known_reclaim_held(g, s))
      return 0;
    canon = lj_str_canon_acq(s);
    if (canon == lj_str_canon_pack(rec, LJ_STR_CANON_QRESCUED)) {
      if (!strq_relink(g, rec, s, mainhdr, completed))
        return 0;
      owns_body = 0;
      counted = 1;
    } else {
      if (!owns_body || !(status & LJ_STR_CANONREC_CLOSE_PROVEN))
        return 0;
      committed = lj_str_canon_pack(rec, LJ_STR_CANON_QCOMMIT);
      if (canon != committed) {
        uint64_t close = la_load64_acq(&rec->close_epoch);
        if (canon != lj_str_canon_pack(rec, LJ_STR_CANON_QCLOSING) ||
            !str_retire_grace(completed, close) ||
            str_source_pinned(g, s) ||
            (mainhdr && strtab_active_on_hdr_before(g, mainhdr, close)) ||
            !lj_str_canon_cas(s, &canon, committed))
          return 0;
      }
    }
  }

  if (status & LJ_STR_CANONREC_Q_LINKED) {
    if (!strq_unlink(g, rec))
      return 0;
    if (!owns_body && s) {
      /* Main publication precedes removal from Q. No later lookup can
      ** confuse the retiring reservation with the live main identity. */
      if (LJ_UNLIKELY(!lj_str_canon_cas(s, &canon, LJ_STR_CANON_LIVE))) {
        lj_assertG(0, "rescued string lost canonical relink ownership");
        abort();
      }
      la_storeptr_rel((void **)&rec->str, NULL);
      status &= ~(LJ_STR_CANONREC_BODY_OWNED|LJ_STR_CANONREC_CLOSE_PROVEN);
    }
    la_store64_rel(&rec->q_unlink_epoch, lj_gc2_retire_epoch(g));
    la_store32_rel(&rec->status, (status & ~LJ_STR_CANONREC_Q_LINKED) |
                   LJ_STR_CANONREC_Q_UNLINKED);
    if (counted)
      strq_count_remove(g);
    return 0;
  }
  if (!str_retire_grace(completed, la_load64_acq(&rec->q_unlink_epoch)))
    return 0;
  if (owns_body) {
    LJGCDestructCtx dctx;
    if (str_source_pinned(g, s) ||
        (mainhdr && strtab_active_on_hdr_before(g, mainhdr,
                         la_load64_acq(&rec->q_unlink_epoch))) ||
        str_free_prepare_impl(g, s, &dctx, 1) != LJ_GC_DESTRUCT_ACQUIRED)
      return 0;
    /* Claim arena destruction first so a lease refusal leaves QCOMMIT
    ** retryable. FREEING has no path back to an acquirable identity. */
    if (LJ_UNLIKELY(!lj_str_canon_cas(s, &committed,
                            lj_str_canon_pack(rec, LJ_STR_CANON_FREEING)))) {
      lj_assertG(0, "committed string lost canonical retirement ownership");
      abort();
    }
    lj_str_free_commit(g, s, &dctx);
    lj_str_sweep_reclaimed_add(g, 1);
  }
  lj_mem_free(g, rec, sizeof(*rec));
  return 1;
}

uint32_t lj_str_reclaim_retired(global_State *g, uint64_t completed_epoch)
{
  StrBodyRetire *ret;
  StrTabHdr *hdr;
  StrCanonHdr *qhdr;
  uint32_t reclaimed = 0;
  if (!g || completed_epoch == 0)
    return 0;
  /* The generic GC2 owner has closed SMR admission and installed its exact
  ** current-thread capability across every detach and nested validation. */
  /* Body records are processed before header records. A deferred current-header
  ** body still needs that generation name for the active-pin check below. */
  reclaimed += str_retire_batches_reclaim_all(g, 1);
  ret = lj_str_body_retired_head_xchg_acqrel(g, NULL);
  if (LJ_UNLIKELY(!str_body_retired_chain_valid(g, ret, 1))) {
    lj_assertG(0, "invalid/cyclic detached string-body retire chain");
    abort();
  }
  while (ret) {
    if (LJ_UNLIKELY(
	!lj_gc2_mem_registered_known_reclaim_held(g, ret))) {
      /* The exclusive owner already detached this list. Losing the remaining
      ** chain here is an allocator/accounting corruption, not a retry result. */
      lj_assertG(0, "invalid detached string-body retire record");
      abort();
    }
    StrBodyRetire *next = lj_str_body_retired_next_acq(ret);
    uint64_t retire_epoch = lj_str_body_retired_epoch_acq(ret);
    StrTabHdr *rethdr = lj_str_body_retired_hdr_acq(ret);
    uint32_t status = la_load32_acq(&ret->status);
    uint32_t main_linked = lj_str_body_retired_main_linked_acq(ret);
    int old_enough = completed_epoch >= retire_epoch &&
	completed_epoch - retire_epoch >= LJ_STR_SWEEP_GRACE_EPOCHS;
    lj_str_body_retired_next_rel(ret, NULL);
    if (status & (LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_LIST_ONLY|
                  LJ_STR_CANONREC_Q_UNLINKED)) {
      if (strq_retire_record(g, ret, completed_epoch, status))
        reclaimed++;
      else
        str_body_retired_push(g, ret);
      ret = next;
      continue;
    }
    if (LJ_UNLIKELY(main_linked != 0)) {
      /* Normal sweep records enter this list only after their exact bucket
      ** unlink. A still-linked record never owns the canonical body. */
      lj_assertG(0, "linked string body reached retired ownership list");
      abort();
    }
    if (old_enough &&
	(lj_str_tabh_acq(g) != rethdr || !strtab_active_on_hdr(g, rethdr))) {
      GCstr *s = lj_str_body_retired_str_acq(ret);
      uint8_t gct;
      int freed;
      /* This record is the exact detached body owner. Validate the nested
      ** allocation with that same non-transferable reclaimer capability before
      ** reading its type/size, then arbitrate destruction through the matching
      ** reclaim-held entry point. A transient failed validation retains both
      ** the record and its body for the next grace pass. */
      if (LJ_UNLIKELY(!s ||
	  !lj_gc2_mem_registered_known_reclaim_held(g, s))) {
	str_body_retired_push(g, ret);
	ret = next;
	continue;
      }
      gct = la_load8_acq(&s->gct);
      if (gct == (uint8_t)~LJ_TSTR) {
	freed = str_free_try_impl(g, s, 1);
	if (freed == LJ_GC_DESTRUCT_LOST) {
	  str_body_retired_push(g, ret);
	  ret = next;
	  continue;
	}
	if (freed == LJ_GC_DESTRUCT_ACQUIRED)
	  lj_str_sweep_reclaimed_add(g, 1);
      } else if (LJ_UNLIKELY(gct != 0)) {
	/* A completed destructor publishes type zero. Any other type means the
	** allocation was reused or the exact retire record names the wrong body;
	** neither condition permits silently dropping its ownership record. */
	lj_assertG(0, "retired string body type identity changed");
	abort();
      }
      lj_mem_free(g, ret, sizeof(*ret));
      reclaimed++;
    } else {
      str_body_retired_push(g, ret);
    }
    ret = next;
  }
  hdr = lj_str_retired_head_xchg_acqrel(g, NULL);
  if (LJ_UNLIKELY(!str_tab_retired_chain_valid(g, hdr, 1))) {
    lj_assertG(0, "invalid/cyclic detached string-table retire chain");
    abort();
  }
  while (hdr) {
    if (LJ_UNLIKELY(
	!lj_gc2_mem_registered_known_reclaim_held(g, hdr))) {
      lj_assertG(0, "invalid detached string-table retire header");
      abort();
    }
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
  if (LJ_UNLIKELY(!str_qretired_chain_valid(g, qhdr, 1))) {
    lj_assertG(0, "invalid/cyclic detached string-canonical retire chain");
    abort();
  }
  while (qhdr) {
    if (LJ_UNLIKELY(
	!lj_gc2_mem_registered_known_reclaim_held(g, qhdr))) {
      lj_assertG(0, "invalid detached string-canonical retire header");
      abort();
    }
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

static int str_terminal_body_reconcile(global_State *g, GCstr *s)
{
  GCArena *a;
  if (g->allocf != lj_arena_allocf ||
      la_load32_acq(&g->allocf_arena) == 0)
    return 1;
  if (!s || !checkptrGC(s))
    return 0;
  a = lj_arena_of(s);
  return lj_arena_ishuge(a) || lj_arena_terminal_reconcile(a);
}

/* Joined-world only. Validate every list node before its successor read so a
** stale duplicate pending pointer can never turn into a second free. */
static int str_terminal_retired_contains(global_State *g,
					 StrBodyRetire *needle)
{
  StrBodyRetire *ret = lj_str_body_retired_head_acq(g);
  if (LJ_UNLIKELY(!str_body_retired_chain_valid(g, ret, 0))) {
    lj_assertG(0, "invalid/cyclic terminal string-body ownership list");
    abort();
  }
  while (ret) {
    StrBodyRetire *next;
    if (LJ_UNLIKELY(!lj_gc2_mem_registered_known(g, ret))) {
      lj_assertG(0, "invalid terminal string-body ownership list");
      abort();
    }
    if (ret == needle)
      return 1;
    next = lj_str_body_retired_next_acq(ret);
    ret = next;
  }
  return 0;
}

/* Run only while `ret` remains discoverable from either sweep_pending or the
** detached retired list. The caller clears/frees the record after this exact
** body transaction succeeds. */
static void str_terminal_free_retired_body(global_State *g,
					   StrBodyRetire *ret)
{
  GCstr *s = lj_str_body_retired_str_acq(ret);
  uint32_t status = la_load32_acq(&ret->status);
  uint32_t main_linked = lj_str_body_retired_main_linked_acq(ret);
  int owns_body = status == 0 || (status & LJ_STR_CANONREC_BODY_OWNED);
  if (!owns_body)
    return;
  if (LJ_UNLIKELY(main_linked != 0)) {
    lj_assertG(0, "linked string record claimed terminal body ownership");
    abort();
  }
  if (LJ_UNLIKELY(!s || !lj_gc2_mem_registered_known(g, s) ||
      !str_terminal_body_reconcile(g, s))) {
    lj_assertG(0, "invalid terminal retired string body");
    abort();
  }
  {
    uint8_t gct = la_load8_acq(&s->gct);
    if (gct == (uint8_t)~LJ_TSTR) {
      if (lj_str_free_try(g, s) == LJ_GC_DESTRUCT_LOST) {
	/* Keep the record discoverable on failure. Reconcile is exact for
	** count-zero CLOSED|PENDING, so this is terminal ownership corruption. */
	lj_assertG(0, "terminal string destructor ownership failed");
	abort();
      }
    } else if (LJ_UNLIKELY(gct != 0)) {
      lj_assertG(0, "terminal retired string body type identity changed");
      abort();
    }
  }
}

void lj_str_free_retired_bodies(global_State *g)
{
  StrBodyRetire *ret, *pending;
  if (!g)
    return;
  /* Joined-world shutdown is also the exact abort sink for a partially
  ** completed sole-mutator batch. No worker can remain inside the pending
  ** unlink handoff after lj_gc2_fini() has joined the pool. */
  if (LJ_UNLIKELY(lj_str_sweep_batch_pending_acq(g) != 0)) {
    lj_assertG(0, "terminal string cleanup crossed batched unlink handoff");
    abort();
  }
  if (lj_str_sweep_batch_acq(g) != NULL ||
      lj_str_retired_batch_head_acq(g) != NULL)
    (void)str_retire_batches_reclaim_all(g, 0);
  str_reclaim_exclusive_leave(g);
  lj_str_reclaim_requested_rel(g, 0);
  /* Workers are stopped before the terminal GC2 drain, so an unlink CAS cannot
  ** still be between its discoverable pending slot and retired-list publish. */
  pending = lj_str_sweep_pending_acq(g);
  if (pending) {
    if (LJ_UNLIKELY(!lj_gc2_mem_registered_known(g, pending))) {
      lj_assertG(0, "invalid terminal pending string retire record");
      abort();
    }
    if (str_terminal_retired_contains(g, pending)) {
      /* The list is now the durable owner. Remove only the redundant slot;
      ** the list loop below performs the one record/body destruction. */
      lj_str_sweep_pending_rel(g, NULL);
    } else {
      if (lj_str_body_retired_main_linked_acq(pending) == 0)
	str_terminal_free_retired_body(g, pending);
      /* A main-linked pending record lost its bucket CAS and owns metadata
      ** only. In either case keep the slot published until all body work has
      ** succeeded, then retire the record exactly once. */
      lj_str_sweep_pending_rel(g, NULL);
      lj_mem_free(g, pending, sizeof(*pending));
    }
  }
  ret = lj_str_body_retired_head_xchg_acqrel(g, NULL);
  if (LJ_UNLIKELY(!str_body_retired_chain_valid(g, ret, 0))) {
    lj_assertG(0, "invalid/cyclic terminal string-body retire chain");
    abort();
  }
  while (ret) {
    if (LJ_UNLIKELY(!lj_gc2_mem_registered_known(g, ret))) {
      lj_assertG(0, "invalid terminal string-body retire record");
      abort();
    }
    StrBodyRetire *next = lj_str_body_retired_next_acq(ret);
    str_terminal_free_retired_body(g, ret);
    lj_mem_free(g, ret, sizeof(*ret));
    ret = next;
  }
}

#ifdef LJ_STR_TEST_HELPERS
int lj_str_test_body_retired_chain_valid(global_State *g,
					  StrBodyRetire *head)
{
  return str_body_retired_chain_valid(g, head, 0);
}

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
  if (LJ_UNLIKELY(!str_tab_retired_chain_valid(g, hdr, 0))) {
    lj_assertG(0, "invalid/cyclic terminal string-table retire chain");
    abort();
  }
  while (hdr) {
    if (LJ_UNLIKELY(!lj_gc2_mem_registered_known(g, hdr))) {
      lj_assertG(0, "invalid terminal string-table retire header");
      abort();
    }
    StrTabHdr *next = lj_str_retired_next_acq(hdr);
    lj_mem_free(g, hdr, lj_str_tabbytes(hdr));
    hdr = next;
  }
  qhdr = lj_str_qtabh_xchg_acqrel(g, NULL);
  if (qhdr)
    lj_mem_free(g, qhdr, lj_str_qtabbytes(qhdr));
  qhdr = lj_str_qretired_head_xchg_acqrel(g, NULL);
  if (LJ_UNLIKELY(!str_qretired_chain_valid(g, qhdr, 0))) {
    lj_assertG(0, "invalid/cyclic terminal string-canonical retire chain");
    abort();
  }
  while (qhdr) {
    if (LJ_UNLIKELY(!lj_gc2_mem_registered_known(g, qhdr))) {
      lj_assertG(0, "invalid terminal string-canonical retire header");
      abort();
    }
    StrCanonHdr *next = lj_str_qretired_next_acq(qhdr);
    lj_mem_free(g, qhdr, lj_str_qtabbytes(qhdr));
    qhdr = next;
  }
}
