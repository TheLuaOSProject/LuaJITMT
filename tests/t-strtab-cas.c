/*
** Focused regression test for M5 string table CAS publication scaffolding.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_str.h"
#include "lj_tg.h"
#include "lj_thr.h"

#define TEST_STRTAB_RESIZE	((MSize)0x80000000u)
#define TEST_STRTAB_SWEEP	((MSize)0x40000000u)
#define TEST_STRTAB_OWNER_LOBITS	(TEST_STRTAB_SWEEP - 1u)

typedef struct ActiveReleaseCtx {
  TGState *tg;
  StrTabHdr *hdr;
  int64_t delay_ns;
  uint32_t claimed;
  uint32_t cleared;
} ActiveReleaseCtx;

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
typedef struct FailAllocCtx {
  int fail_next;
  size_t fail_min;
} FailAllocCtx;

typedef struct ResizeOOMCtx {
  MSize newmask;
} ResizeOOMCtx;
#endif

typedef struct ReclaimReaderCtx {
  global_State *g;
  uint64_t epoch;
  uint32_t done;
  uint32_t reclaimed;
} ReclaimReaderCtx;

enum {
  TAGGED_HOOK_NONE,
  TAGGED_HOOK_TAG_AFTER_COMPARE,
  TAGGED_HOOK_CLEAR_BEFORE_CAS
};

typedef struct TaggedMatchCtx {
  GCobj *target;
  uint32_t action;
  uint32_t after_compare;
  uint32_t before_cas;
  uint32_t mutations;
} TaggedMatchCtx;

static TaggedMatchCtx tagged_match_ctx;

typedef struct CanonRaceCtx {
  lua_State *L;
  GCstr *s;
  StrCanonRec *rec;
  StrCanonHdr *qhdr;
  uint32_t pause_stage;
  uint32_t reached;
  uint32_t release;
  uint32_t done;
  int result;
} CanonRaceCtx;

static CanonRaceCtx *canon_race_ctx;

static void test_sleep_without_tg(int64_t ns)
{
  TGState *saved = lj_thr_get_tg();
  /* These deterministic test barriers hand one TG between OS threads. Never
  ** let the polling sleep mutate that TG's non-RMW in_native depth. */
  lj_thr_set_tg(NULL);
  (void)lj_thr_sleep_ns(NULL, ns);
  lj_thr_set_tg(saved);
}

static StrHash test_hash_sparse(uint64_t seed, const char *str, MSize len)
{
  StrHash a, b, h = len ^ (StrHash)seed;
  if (len >= 4) {
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
  UNUSED(a);
  return h;
}

static void tagged_match_hook(lua_State *L, GCRef *link, GCobj *target,
			      uintptr_t observed, uint32_t stage)
{
  uintptr_t expect;
  if (target != tagged_match_ctx.target)
    return;
  assert(lj_str_link_target(observed) == target);
  if (stage == LJ_STR_TEST_MATCH_AFTER_COMPARE) {
    tagged_match_ctx.after_compare++;
    if (tagged_match_ctx.action == TAGGED_HOOK_TAG_AFTER_COMPARE) {
      assert((observed & LJ_STRHASH_DEAD) == 0);
      expect = observed;
      assert(lj_str_link_cas_acqrel(link, &expect,
				    observed | LJ_STRHASH_DEAD));
      tagged_match_ctx.mutations++;
      tagged_match_ctx.action = TAGGED_HOOK_NONE;
    }
  } else {
    assert(stage == LJ_STR_TEST_MATCH_BEFORE_RESCUE_CAS);
    tagged_match_ctx.before_cas++;
    if (tagged_match_ctx.action == TAGGED_HOOK_CLEAR_BEFORE_CAS) {
      assert((observed & LJ_STRHASH_DEAD) != 0);
      expect = observed;
      assert(lj_str_link_cas_acqrel(link, &expect,
				    observed & ~(uintptr_t)LJ_STRHASH_DEAD));
      (void)lj_gc2_preserve_sweep_root(G(L), target);
      tagged_match_ctx.mutations++;
      tagged_match_ctx.action = TAGGED_HOOK_NONE;
    }
  }
}

static void reset_tagged_match_hook(GCobj *target, uint32_t action)
{
  memset(&tagged_match_ctx, 0, sizeof(tagged_match_ctx));
  tagged_match_ctx.target = target;
  tagged_match_ctx.action = action;
  lj_str_test_set_match_hook(tagged_match_hook);
}

static void exercise_tagged_link_protocol(lua_State *L)
{
  enum { TEST_MASK = 4095u };
  global_State *g = G(L);
  StrTabHdr *hdr;
  GCRef synthetic;
  GCRef *head;
  GCstr *target, *prepended;
  uintptr_t expect, raw;
  StrHash target_hash;
  MSize mask, bucket;
  char target_buf[64], prepend_buf[64];
  uint32_t n;

  /* Raw link CAS preserves unrelated tags, and next links strip SECONDARY only. */
  synthetic.gcptr64 = (uint64_t)((uintptr_t)obj2gco(&g->strempty) |
				 LJ_STRHASH_DEAD | LJ_STRHASH_SECONDARY);
  expect = lj_str_link_load_acq(&synthetic);
  assert(lj_str_link_target(expect) == obj2gco(&g->strempty));
  assert(lj_str_link_cas_acqrel(&synthetic, &expect,
				expect & ~(uintptr_t)LJ_STRHASH_DEAD));
  raw = lj_str_link_load_acq(&synthetic);
  assert((raw & LJ_STRHASH_DEAD) == 0);
  assert((raw & LJ_STRHASH_SECONDARY) != 0);

  lj_str_resize(L, TEST_MASK);
  hdr = lj_str_tabh_acq(g);
  assert(hdr != NULL);
  mask = hdr->mask;
  assert(mask >= TEST_MASK);

  /* Pick an empty primary bucket, then calculate a distinct colliding name. */
  for (n = 0;; n++) {
    MSize len;
    snprintf(target_buf, sizeof(target_buf),
	     "m5-strtab-tag-target-%08x", n);
    len = (MSize)strlen(target_buf);
    target_hash = test_hash_sparse(g->str.seed, target_buf, len);
    bucket = target_hash & mask;
    raw = lj_str_link_load_acq(&hdr->bucket[bucket]);
    if (raw == 0)
      break;
  }
  for (n = 0;; n++) {
    MSize len;
    snprintf(prepend_buf, sizeof(prepend_buf),
	     "m5-strtab-tag-prepend-%08x", n);
    len = (MSize)strlen(prepend_buf);
    if ((test_hash_sparse(g->str.seed, prepend_buf, len) & mask) == bucket)
      break;
  }

  target = lj_str_new(L, target_buf, strlen(target_buf));
  assert(target != NULL && target->hashalg == 0);
  head = &hdr->bucket[bucket];
  raw = lj_str_link_load_acq(head);
  assert(lj_str_link_target(raw) == obj2gco(target));
  assert((raw & LJ_STRHASH_LINKMASK) == 0);

  /* A prepend moves the old head tag to the new object's next link. */
  expect = raw;
  assert(lj_str_link_cas_acqrel(head, &expect, raw | LJ_STRHASH_DEAD));
  prepended = lj_str_new(L, prepend_buf, strlen(prepend_buf));
  assert(prepended != NULL && prepended != target);
  raw = lj_str_link_load_acq(head);
  assert(lj_str_link_target(raw) == obj2gco(prepended));
  assert((raw & LJ_STRHASH_DEAD) == 0);
  assert((raw & LJ_STRHASH_SECONDARY) == 0);
  raw = lj_str_next_link_acq(obj2gco(prepended));
  assert(lj_str_link_target(raw) == obj2gco(target));
  assert((raw & LJ_STRHASH_DEAD) != 0);
  assert((raw & LJ_STRHASH_SECONDARY) == 0);

  /* A competing rescue wins the CAS; the loser restarts and keeps identity. */
  reset_tagged_match_hook(obj2gco(target), TAGGED_HOOK_CLEAR_BEFORE_CAS);
  assert(lj_str_new(L, target_buf, strlen(target_buf)) == target);
  lj_str_test_set_match_hook(NULL);
  assert(tagged_match_ctx.mutations == 1);
  assert(tagged_match_ctx.before_cas == 1);
  assert(tagged_match_ctx.after_compare == 2);  /* Retry reached the match. */
  raw = lj_str_next_link_acq(obj2gco(prepended));
  assert((raw & LJ_STRHASH_DEAD) == 0);

  /* Tagging after bytes match is observed, cleared and rescued before return. */
  reset_tagged_match_hook(obj2gco(prepended),
			  TAGGED_HOOK_TAG_AFTER_COMPARE);
  assert(lj_str_new(L, prepend_buf, strlen(prepend_buf)) == prepended);
  lj_str_test_set_match_hook(NULL);
  assert(tagged_match_ctx.mutations == 1);
  assert(tagged_match_ctx.after_compare == 1);
  assert(tagged_match_ctx.before_cas == 1);
  raw = lj_str_link_load_acq(head);
  assert(lj_str_link_target(raw) == obj2gco(prepended));
  assert((raw & LJ_STRHASH_LINKMASK) == 0);
}

static void exercise_rehash_ignores_legacy_colors(lua_State *L)
{
  enum { TEST_MASK = 4095u, TEST_CHAIN = 34u, TEST_TOTAL = TEST_CHAIN + 1u };
  global_State *g = G(L);
  StrTabHdr *hdr;
  GCstr *members[TEST_CHAIN];
  GCstr *trigger;
  char names[TEST_TOTAL][64];
  size_t lens[TEST_TOTAL];
  MSize bucket = 0, found = 0;
  uint8_t deadwhite;
  uint32_t n;
  int oldstate;

  lj_str_resize(L, TEST_MASK);
  hdr = lj_str_tabh_acq(g);
  assert(hdr != NULL && hdr->mask == TEST_MASK);

  /* Prepare an overlong primary chain while the sweep topology claim asks
  ** ordinary interners to defer secondary rehash. The final name is retained
  ** as the post-claim insertion that must run lj_str_rehash_chain(). */
  for (n = 0; found < TEST_TOTAL; n++) {
    char candidate[64];
    MSize len, b;
    int written = snprintf(candidate, sizeof(candidate),
			   "m5-strtab-rehash-color-%08x", n);
    assert(written > 0 && (size_t)written < sizeof(candidate));
    len = (MSize)written;
    b = test_hash_sparse(g->str.seed, candidate, len) & hdr->mask;
    if (found == 0) {
      if (lj_str_link_load_acq(&hdr->bucket[b]) != 0)
	continue;
      bucket = b;
    } else if (b != bucket) {
      continue;
    }
    memcpy(names[found], candidate, len + 1u);
    lens[found] = len;
    found++;
  }

  assert(lj_str_sweep_claim(L, hdr) == 1);
  assert(la_load32_acq(&hdr->resize) == TEST_STRTAB_SWEEP);
  for (n = 0; n < TEST_CHAIN; n++) {
    members[n] = lj_str_new(L, names[n], lens[n]);
    assert(members[n] != NULL && members[n]->hashalg == 0);
  }
  lj_str_sweep_release(hdr);
  assert(la_load32_acq(&hdr->resize) == 0);

  /* The retired implementation swept this chain as a side effect whenever
  ** the compatibility state and header whites happened to look old. Those
  ** fields are not GC2 liveness input and rehash must only change topology. */
  deadwhite = (uint8_t)(otherwhite(g) & LJ_GC_WHITES);
  for (n = 0; n < TEST_CHAIN; n++) {
    lj_obj_masksetgcflags(obj2gco(members[n]), LJ_GC_COLORS, deadwhite);
    assert(isdead(g, obj2gco(members[n])));
  }
  oldstate = g->gc.state;
  g->gc.state = GCSsweepstring;
  trigger = lj_str_new(L, names[TEST_CHAIN], lens[TEST_CHAIN]);
  g->gc.state = (uint8_t)oldstate;
  assert(trigger != NULL && trigger->hashalg == 1);

  for (n = 0; n < TEST_CHAIN; n++) {
    assert(members[n]->gct == (uint8_t)~LJ_TSTR);
    assert(members[n]->len == lens[n]);
    assert(memcmp(strdata(members[n]), names[n], lens[n] + 1u) == 0);
    assert(members[n]->hashalg == 1);
    assert(lj_str_new(L, names[n], lens[n]) == members[n]);
    lj_obj_masksetgcflags(obj2gco(members[n]), LJ_GC_COLORS,
			 (uint8_t)curwhite(g));
  }
}

static void exercise_canonical_layout(lua_State *L)
{
  global_State *g = G(L);
  static const MSize lens[] = { 1u, 3u, 4u, 7u, 8u, 15u, 16u, 31u };
  char buf[64];
  size_t i;
  assert(strdata(&g->strempty) == (const char *)&g->stremptyz);
  assert(strdata(&g->strempty)[0] == '\0');
  assert(lj_str_canon_acq(&g->strempty) == LJ_STR_CANON_LIVE);
  for (i = 0; i < sizeof(lens)/sizeof(lens[0]); i++) {
    MSize len = lens[i];
    GCstr *s;
    MSize j;
    for (j = 0; j < len; j++)
      buf[j] = (char)('a' + (j + (MSize)i) % 26u);
    s = lj_str_new(L, buf, len);
    assert(s != NULL);
    assert(s->len == len);
    assert(lj_str_canon_acq(s) == LJ_STR_CANON_LIVE);
    assert(memcmp(strdata(s), buf, len) == 0);
    assert(strdata(s)[len] == '\0');
    assert(lj_str_new(L, buf, len) == s);
  }
}

static void canonical_race_hook(lua_State *L, GCstr *s, StrCanonRec *rec,
				uint32_t stage)
{
  CanonRaceCtx *ctx = canon_race_ctx;
  if (!ctx || ctx->pause_stage != stage || ctx->s != s)
    return;
  assert(ctx->L == L);
  assert(rec != NULL);
  ctx->rec = rec;
  if (stage == LJ_STR_TEST_CANON_Q_PINNED_BEFORE_PUBLISH) {
    ctx->qhdr = lj_tg_strq_active_hdr_acq(L2TG(L));
    assert(ctx->qhdr != NULL);
    assert(lj_tg_strq_active_depth_acq(L2TG(L)) != 0);
  }
  la_store32_rel(&ctx->reached, 1);
  while (la_load32_acq(&ctx->release) == 0)
    test_sleep_without_tg(100000);
}

static void *canonical_detach_worker(void *arg)
{
  CanonRaceCtx *ctx = (CanonRaceCtx *)arg;
  TGState *oldtg = lj_thr_get_tg();
  /* The hook's release/acquire handoff serializes every main-TG mutation with
  ** the driver thread. The driver polls without a TLS TG; the worker stops
  ** touching the TG before handing ownership back at either pause. */
  lj_thr_set_tg(L2TG(ctx->L));
  ctx->result = lj_str_test_quarantine_detach(ctx->L, ctx->s);
  lj_thr_set_tg(oldtg);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void canonical_race_start(CanonRaceCtx *ctx, lua_State *L, GCstr *s,
				 uint32_t stage, LJThr *thr)
{
  int i;
  memset(ctx, 0, sizeof(*ctx));
  ctx->L = L;
  ctx->s = s;
  ctx->pause_stage = stage;
  canon_race_ctx = ctx;
  lj_str_test_set_canon_hook(canonical_race_hook);
  assert(lj_thr_create(thr, canonical_detach_worker, ctx) == 0);
  for (i = 0; i < 50000 && la_load32_acq(&ctx->reached) == 0 &&
	 la_load32_acq(&ctx->done) == 0; i++)
    test_sleep_without_tg(100000);
  assert(la_load32_acq(&ctx->reached) != 0);
  assert(la_load32_acq(&ctx->done) == 0);
}

static void canonical_race_finish(CanonRaceCtx *ctx, LJThr *thr)
{
  la_store32_rel(&ctx->release, 1);
  assert(lj_thr_join(thr, NULL) == 0);
  assert(la_load32_acq(&ctx->done) != 0);
  assert(ctx->result == 1);
  lj_str_test_set_canon_hook(NULL);
  canon_race_ctx = NULL;
}

static int canonical_qbucket_contains(StrCanonHdr *hdr, StrCanonRec *want)
{
  StrCanonRec *rec;
  for (rec = lj_str_qbucket_acq(&hdr->bucket[want->hash & hdr->mask]);
       rec != NULL; rec = lj_str_qnext_acq(rec)) {
    if (rec == want)
      return 1;
  }
  return 0;
}

static void exercise_canonical_quarantine(lua_State *L)
{
  static const char publish_name[] =
    "m5-strtab-canonical-q-publish-resize-race";
  static const char unlink_name[] =
    "m5-strtab-canonical-unlink-qcount-race";
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  CanonRaceCtx ctx;
  LJThr worker;
  MSize before = lj_str_qcount_acq(g);
  MSize oldmask;
  StrCanonHdr *oldhdr;
  GCstr *spublish, *sunlink;
  uintptr_t canon;

  /* Exercise the zero -> non-zero correctness gate first in a fresh state.
  ** The test barrier makes this a program-order regression, not a hardware
  ** memory-order litmus: once the exact main edge is absent, a new interner
  ** must take the now-enabled Q lookup and return the identical body. */
  assert(before == 0);
  sunlink = lj_str_new(L, unlink_name, sizeof(unlink_name)-1u);
  assert(sunlink != NULL);
  assert(lj_str_canon_acq(sunlink) == LJ_STR_CANON_LIVE);
  canonical_race_start(&ctx, L, sunlink, LJ_STR_TEST_CANON_MAIN_UNLINKED,
		       &worker);
  assert(ctx.rec != NULL);
  assert(lj_str_qcount_acq(g) == 1u);
  assert(lj_str_new(L, unlink_name, sizeof(unlink_name)-1u) == sunlink);
  canon = lj_str_canon_acq(sunlink);
  assert(lj_str_canon_state(canon) == LJ_STR_CANON_QRESCUED);
  canonical_race_finish(&ctx, &worker);

  /* Pause an actual publisher while it pins the old Q header. A concurrent
  ** resize must abort without moving the bucket topology out from under it. */
  oldmask = lj_str_qmask_acq(g);
  oldhdr = lj_str_qtabh_acq(g);
  spublish = lj_str_new(L, publish_name, sizeof(publish_name)-1u);
  assert(spublish != NULL);
  assert(lj_str_canon_acq(spublish) == LJ_STR_CANON_LIVE);
  canonical_race_start(&ctx, L, spublish,
	LJ_STR_TEST_CANON_Q_PINNED_BEFORE_PUBLISH, &worker);
  assert(ctx.rec != NULL);
  assert(ctx.qhdr == oldhdr);
  assert(!canonical_qbucket_contains(ctx.qhdr, ctx.rec));
  assert(la_load32_acq(&ctx.rec->status) == LJ_STR_CANONREC_LIST_ONLY);
  assert(lj_str_canon_acq(spublish) == LJ_STR_CANON_LIVE);
  assert(lj_tg_strq_active_hdr_acq(tg) == oldhdr);
  assert(lj_tg_strq_active_depth_acq(tg) == 1u);
  assert(lj_str_quarantine_resize(L, (oldmask << 1) + 1u) == 0);
  assert(lj_str_qtabh_acq(g) == oldhdr);
  canonical_race_finish(&ctx, &worker);
  assert(lj_tg_strq_active_hdr_acq(tg) == NULL);
  assert(lj_tg_strq_active_depth_acq(tg) == 0);
  assert(lj_str_qcount_acq(g) == 2u);
  canon = lj_str_canon_acq(spublish);
  assert(lj_str_canon_state(canon) == LJ_STR_CANON_QACTIVE);
  assert(lj_str_canon_record(canon) != NULL);
  assert(lj_str_quarantine_resize(L, (oldmask << 1) + 1u) == 1);
  assert(lj_str_qtabh_acq(g) != oldhdr);
  assert(lj_str_qmask_acq(g) == (oldmask << 1) + 1u);
  assert(lj_str_qretired_head_acq(g) == oldhdr);
  assert(lj_str_new(L, publish_name, sizeof(publish_name)-1u) == spublish);
  canon = lj_str_canon_acq(spublish);
  assert(lj_str_canon_state(canon) == LJ_STR_CANON_QRESCUED);

  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lj_str_qcount_acq(g) == before + 2u);
  assert(lj_str_canon_state(lj_str_canon_acq(spublish)) ==
	 LJ_STR_CANON_QRESCUED);
  assert(lj_str_canon_state(lj_str_canon_acq(sunlink)) ==
	 LJ_STR_CANON_QRESCUED);
  assert(lj_str_new(L, publish_name, sizeof(publish_name)-1u) == spublish);
  assert(lj_str_new(L, unlink_name, sizeof(unlink_name)-1u) == sunlink);

  /* The zero/non-zero bypass is a correctness boundary. Prove that the Stage
  ** A monotonic count saturates instead of wrapping back to the false zero. */
  lj_str_qcount_store_rlx(g, UINT32_MAX - 1u);
  assert(lj_str_qcount_inc_sat_acqrel(g) == UINT32_MAX - 1u);
  assert(lj_str_qcount_acq(g) == UINT32_MAX);
  assert(lj_str_qcount_inc_sat_acqrel(g) == UINT32_MAX);
  assert(lj_str_qcount_acq(g) == UINT32_MAX);
}

static MSize assert_resize_state(StrTabHdr *hdr)
{
  MSize state = la_load32_acq(&hdr->resize);
  assert((state & TEST_STRTAB_OWNER_LOBITS) == 0);
  assert(state == 0 || state == TEST_STRTAB_RESIZE ||
	 state == TEST_STRTAB_SWEEP);
  return state;
}

static void assert_resize_idle(StrTabHdr *hdr)
{
  assert(assert_resize_state(hdr) == 0);
}

static void assert_resize_claimed(StrTabHdr *hdr)
{
  assert(assert_resize_state(hdr) == TEST_STRTAB_RESIZE);
}

static void assert_sweep_claimed(StrTabHdr *hdr)
{
  assert(assert_resize_state(hdr) == TEST_STRTAB_SWEEP);
}

static int cmp_strid(const void *a, const void *b)
{
  StrID x = *(const StrID *)a;
  StrID y = *(const StrID *)b;
  return (x > y) - (x < y);
}

static void exercise_string_id_blocks(lua_State *L)
{
  enum { N = 192 };
  GCstr *s[N];
  StrID ids[N];
  uint32_t refills0, refills1;
  int i;

  lj_str_test_reset_id_refills();
  refills0 = lj_str_test_id_refills();

  for (i = 0; i < N; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-id-block-%03d", i);
    s[i] = lj_str_new(L, buf, strlen(buf));
    assert(s[i] != NULL);
    ids[i] = s[i]->sid;
  }

  refills1 = lj_str_test_id_refills();
  assert(refills1 > refills0);
  assert(refills1 - refills0 < N / 4);

  qsort(ids, N, sizeof(ids[0]), cmp_strid);
  for (i = 1; i < N; i++)
    assert(ids[i] != ids[i - 1]);

  for (i = 0; i < N; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-id-block-%03d", i);
    assert(lj_str_new(L, buf, strlen(buf)) == s[i]);
  }
}

static void exercise_string_count_blocks(lua_State *L)
{
  enum { N = 96 };
  global_State *g = G(L);
  TGState *tg = L2TG(L);
  GCstr *s[N];
  MSize before, published, exact;
  uint32_t refills0, refills1;
  int i;

  lj_str_flush_num_credit(g, tg);
  assert(tg->strnum_credit == 0);
  before = lj_str_num_acq(g);
  lj_str_test_reset_num_refills();
  refills0 = lj_str_test_num_refills();

  for (i = 0; i < N; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-num-block-%03d", i);
    s[i] = lj_str_new(L, buf, strlen(buf));
    assert(s[i] != NULL);
  }

  refills1 = lj_str_test_num_refills();
  assert(refills1 > refills0);
  assert(refills1 - refills0 < N / 4);
  published = lj_str_num_acq(g);
  assert(published >= before + N);
  assert(published == before + N + tg->strnum_credit);

  lj_str_flush_num_credit(g, tg);
  exact = lj_str_num_acq(g);
  assert(tg->strnum_credit == 0);
  assert(exact == before + N);

  for (i = 0; i < N; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-num-block-%03d", i);
    assert(lj_str_new(L, buf, strlen(buf)) == s[i]);
  }
  assert(lj_str_test_num_refills() == refills1);
  assert(lj_str_num_acq(g) == exact);
}

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
static void *fail_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  FailAllocCtx *ctx = (FailAllocCtx *)ud;
  UNUSED(osize);
  if (nsize == 0) {
    free(ptr);
    return NULL;
  }
  if (ctx->fail_next && nsize >= ctx->fail_min) {
    ctx->fail_next = 0;
    return NULL;
  }
  return realloc(ptr, nsize);
}
#endif

static void *release_active_after_claim(void *arg)
{
  ActiveReleaseCtx *ctx = (ActiveReleaseCtx *)arg;
  while (assert_resize_state(ctx->hdr) == 0)
    (void)lj_thr_sleep_ns(NULL, 100000);
  assert(assert_resize_state(ctx->hdr) != 0);
  la_store32_rel(&ctx->claimed, 1);
  if (ctx->delay_ns > 0)
    (void)lj_thr_sleep_ns(NULL, ctx->delay_ns);
  assert(lj_tg_strtab_active_hdr_acq(ctx->tg) == ctx->hdr);
  assert(lj_tg_strtab_active_depth_acq(ctx->tg) == 1u);
  lj_tg_strtab_active_depth_rel(ctx->tg, 0);
  lj_tg_strtab_active_hdr_rel(ctx->tg, NULL);
  la_store32_rel(&ctx->cleared, 1);
  return NULL;
}

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
static int protected_resize(lua_State *L)
{
  ResizeOOMCtx *ctx = (ResizeOOMCtx *)lua_touserdata(L, 1);
  lj_str_resize(L, ctx->newmask);
  return 0;
}
#endif

static void *reclaim_retired_worker(void *arg)
{
  ReclaimReaderCtx *ctx = (ReclaimReaderCtx *)arg;
  ctx->reclaimed = lj_gc2_reclaim_retired(ctx->g, ctx->epoch);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
static void exercise_resize_oom_does_not_claim(void)
{
  FailAllocCtx allocctx = { 0, 0 };
  lua_State *L = lua_newstate(fail_alloc, &allocctx);
  global_State *g;
  StrTabHdr *hdr;
  ResizeOOMCtx resizectx;
  int rc;

  assert(L != NULL);
  g = G(L);
  hdr = lj_str_tabh_acq(g);
  assert(hdr != NULL);
  assert_resize_idle(hdr);

  resizectx.newmask = (hdr->mask << 1) + 1u;
  lua_pushcfunction(L, protected_resize);
  lua_pushlightuserdata(L, &resizectx);
  allocctx.fail_min = lj_str_tabsize(resizectx.newmask);
  allocctx.fail_next = 1;
  rc = lua_pcall(L, 1, 0, 0);
  assert(rc == LUA_ERRMEM);
  assert(allocctx.fail_next == 0);
  assert(lj_str_tabh_acq(g) == hdr);
  assert_resize_idle(hdr);
  lua_settop(L, 0);
  assert(lj_str_new(L, "m5-strtab-resize-oom",
		    strlen("m5-strtab-resize-oom")) != NULL);

  lua_close(L);
}
#endif

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  StrTabHdr *hdr;
  TGState *tg, *oldtls;
  TGState extra;
  MSize oldmask, wantmask;
  uint64_t retire_epoch;
  uint64_t smr_runs0, smr_reclaimed0;
  GCstr *s1, *s2;
  ActiveReleaseCtx ctx;
  ReclaimReaderCtx reclaim_ctx;
  LJThr release_thr;
  LJThr reclaim_thr;
  int i;

  assert(L != NULL);
  g = G(L);
  tg = L2TG(L);
  assert(tg != NULL);

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  exercise_resize_oom_does_not_claim();
#endif

  hdr = lj_str_tabh_acq(g);
  assert(hdr != NULL);
  assert_resize_idle(hdr);
  assert(lj_tg_strtab_active_hdr_acq(tg) == NULL);
  assert(lj_tg_strtab_active_depth_acq(tg) == 0);

  s1 = lj_str_new(L, "m5-strtab-cas-same", strlen("m5-strtab-cas-same"));
  s2 = lj_str_new(L, "m5-strtab-cas-same", strlen("m5-strtab-cas-same"));
  assert(s1 == s2);
  exercise_string_id_blocks(L);
  exercise_string_count_blocks(L);
  exercise_tagged_link_protocol(L);
  exercise_rehash_ignores_legacy_colors(L);
  exercise_canonical_layout(L);
  retire_epoch = gc2_hs_epoch_acq(g);
  (void)lj_gc2_reclaim_retired(g, retire_epoch + 1u);
  assert(lj_str_retired_head_acq(g) == NULL);
  hdr = lj_str_tabh_acq(g);
  assert(hdr != NULL);
  assert(hdr->resize == 0);

  oldmask = lj_str_mask_acq(g);
  wantmask = (oldmask << 1) + 1u;

  /* Simulate the last active interner leaving after resize claims the header. */
  lj_tg_strtab_active_hdr_rel(tg, hdr);
  lj_tg_strtab_active_depth_rel(tg, 1);
  ctx.tg = tg;
  ctx.hdr = hdr;
  ctx.delay_ns = 0;
  ctx.claimed = 0;
  ctx.cleared = 0;
  assert(lj_thr_create(&release_thr, release_active_after_claim, &ctx) == 0);
  lj_str_resize(L, wantmask);
  assert(lj_thr_join(&release_thr, NULL) == 0);
  assert(la_load32_acq(&ctx.claimed) != 0);
  assert(la_load32_acq(&ctx.cleared) != 0);
  assert(lj_tg_strtab_active_hdr_acq(tg) == NULL);
  assert(lj_tg_strtab_active_depth_acq(tg) == 0);
  assert(lj_str_tabh_acq(g) != hdr);
  assert(lj_str_mask_acq(g) == wantmask);
  assert_resize_idle(lj_str_tabh_acq(g));
  assert_resize_claimed(hdr);
  assert(lj_str_retired_head_acq(g) == hdr);
  assert(lj_str_retired_next_acq(hdr) == NULL);
  retire_epoch = gc2_hs_epoch_acq(g);
  assert(lj_str_retire_epoch_acq(hdr) == retire_epoch);
  smr_runs0 = gc2_smr_reclaim_runs_acq(g);
  smr_reclaimed0 = gc2_smr_reclaimed_acq(g);
  assert(lj_gc2_reclaim_retired(g, retire_epoch) == 0);
  assert(gc2_smr_reclaim_runs_acq(g) == smr_runs0);
  assert(gc2_smr_reclaimed_acq(g) == smr_reclaimed0);
  assert(lj_str_retired_head_acq(g) == hdr);
  gc2_phase_rel(g, LJ_GC2_MARK);
  assert(lj_gc2_reclaim_retired(g, retire_epoch + 1u) == 0);
  gc2_phase_rel(g, LJ_GC2_IDLE);
  g->gc.state = GCSpropagate;
  assert(lj_gc2_reclaim_retired(g, retire_epoch + 1u) == 0);
  g->gc.state = GCSpause;
  gc2_worker_active_rel(g, 1);
  assert(lj_gc2_reclaim_retired(g, retire_epoch + 1u) == 0);
  gc2_worker_active_rel(g, 0);
  assert(gc2_smr_reclaim_runs_acq(g) == smr_runs0);
  assert(gc2_smr_reclaimed_acq(g) == smr_reclaimed0);
  assert(lj_str_retired_head_acq(g) == hdr);
  reclaim_ctx.g = g;
  reclaim_ctx.epoch = retire_epoch + 1u;
  reclaim_ctx.done = 0;
  reclaim_ctx.reclaimed = 0;
  lj_gc2_smr_read_enter(g);
  assert(lj_thr_create(&reclaim_thr, reclaim_retired_worker,
		       &reclaim_ctx) == 0);
  for (i = 0; i < 1000 && la_load32_acq(&reclaim_ctx.done) == 0; i++)
    (void)lj_thr_sleep_ns(NULL, 100000);
  assert(la_load32_acq(&reclaim_ctx.done) != 0);
  assert(reclaim_ctx.reclaimed == 0);
  assert(lj_str_retired_head_acq(g) == hdr);
  lj_gc2_smr_read_leave(g);
  assert(lj_thr_join(&reclaim_thr, NULL) == 0);
  assert(gc2_smr_reclaiming_acq(g) == 0);
  assert(lj_gc2_reclaim_retired(g, retire_epoch + 1u) >= 1u);
  assert(lj_str_retired_head_acq(g) == NULL);
  assert(lj_str_new(L, "m5-strtab-cas-same",
		    strlen("m5-strtab-cas-same")) == s1);

  hdr = lj_str_tabh_acq(g);
  lj_tg_strtab_active_hdr_rel(tg, hdr);
  lj_tg_strtab_active_depth_rel(tg, 1);
  ctx.tg = tg;
  ctx.hdr = hdr;
  ctx.delay_ns = 0;
  ctx.claimed = 0;
  ctx.cleared = 0;
  assert(lj_thr_create(&release_thr, release_active_after_claim, &ctx) == 0);
  assert(lj_str_sweep_claim(L, hdr) == 1);
  assert(lj_thr_join(&release_thr, NULL) == 0);
  assert(la_load32_acq(&ctx.claimed) != 0);
  assert(la_load32_acq(&ctx.cleared) != 0);
  assert(lj_tg_strtab_active_hdr_acq(tg) == NULL);
  assert(lj_tg_strtab_active_depth_acq(tg) == 0);
  assert_sweep_claimed(hdr);
  lj_str_sweep_release(hdr);
  assert_resize_idle(hdr);

  hdr = lj_str_tabh_acq(g);
  oldmask = lj_str_mask_acq(g);
  wantmask = (oldmask << 1) + 1u;
  lj_tg_init_thread(g, &extra, NULL, 0);
  lj_tg_tid_rel(&extra, lj_thr_newid());
  oldtls = lj_thr_get_tg();
  lj_thr_set_tg(&extra);
  lj_tg_strtab_active_hdr_rel(&extra, hdr);
  lj_tg_strtab_active_depth_rel(&extra, 1);
  ctx.tg = &extra;
  ctx.hdr = hdr;
  ctx.delay_ns = 20000000;
  ctx.claimed = 0;
  ctx.cleared = 0;
  assert(lj_thr_create(&release_thr, release_active_after_claim, &ctx) == 0);
  lj_str_resize(L, wantmask);
  assert(lj_thr_join(&release_thr, NULL) == 0);
  assert(la_load32_acq(&ctx.claimed) != 0);
  assert(la_load32_acq(&ctx.cleared) != 0);
  assert(lj_tg_strtab_active_hdr_acq(&extra) == NULL);
  assert(lj_tg_strtab_active_depth_acq(&extra) == 0);
  assert(lj_str_tabh_acq(g) != hdr);
  assert(lj_str_mask_acq(g) == wantmask);
  assert_resize_idle(lj_str_tabh_acq(g));
  assert_resize_claimed(hdr);
  assert(lj_str_retired_head_acq(g) == hdr);
  lj_thr_set_tg(oldtls);
  lj_tg_fini_thread(g, &extra);
  retire_epoch = gc2_hs_epoch_acq(g);
  assert(lj_gc2_reclaim_retired(g, retire_epoch + 1u) >= 1u);
  assert(lj_str_retired_head_acq(g) == NULL);

  for (i = 0; i < 8192; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-cas-%d-%d", i, i * 31);
    assert(lj_str_new(L, buf, strlen(buf)) != NULL);
  }
  assert_resize_idle(lj_str_tabh_acq(g));
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  assert(gc2_hs_epoch_acq(g) > retire_epoch);
  assert(lj_str_retired_head_acq(g) == NULL);
  assert(gc2_smr_reclaim_runs_acq(g) > smr_runs0);
  assert(gc2_smr_reclaimed_acq(g) >= smr_reclaimed0 + 1u);

  exercise_canonical_quarantine(L);

  lua_close(L);
#if LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  printf("t-strtab-cas OK: tagged-link rescue/prepend races, color-independent secondary rehash, canonical layout, Q-publish/resize and unlink/qcount identity races, saturating Q presence, active-drain claim, TLS-only active drain, GC2 epoch retire, duplicate intern guard, and string ID/count block reservation verified (custom-allocator resize OOM injection skipped by the temporary internal-allocator-only policy)\n");
#else
  printf("t-strtab-cas OK: tagged-link rescue/prepend races, color-independent secondary rehash, canonical layout, Q-publish/resize and unlink/qcount identity races, saturating Q presence, resize OOM, active-drain claim, TLS-only active drain, GC2 epoch retire, duplicate intern guard, and string ID/count block reservation verified\n");
#endif
  return 0;
}
