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

  lua_close(L);
#if LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  printf("t-strtab-cas OK: tagged-link rescue/prepend races, active-drain claim, TLS-only active drain, GC2 epoch retire, duplicate intern guard, and string ID/count block reservation verified (custom-allocator resize OOM injection skipped by the temporary internal-allocator-only policy)\n");
#else
  printf("t-strtab-cas OK: tagged-link rescue/prepend races, resize OOM, active-drain claim, TLS-only active drain, GC2 epoch retire, duplicate intern guard, and string ID/count block reservation verified\n");
#endif
  return 0;
}
