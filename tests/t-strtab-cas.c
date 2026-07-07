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
#include "lj_str.h"
#include "lj_tg.h"
#include "lj_thr.h"

#define TEST_STRTAB_RESIZE	((MSize)0x80000000u)
#define TEST_STRTAB_RESIZE_LOBITS	(TEST_STRTAB_RESIZE - 1u)

typedef struct ActiveReleaseCtx {
  TGState *tg;
  StrTabHdr *hdr;
  int64_t delay_ns;
  uint32_t claimed;
  uint32_t cleared;
} ActiveReleaseCtx;

typedef struct FailAllocCtx {
  int fail_next;
  size_t fail_min;
} FailAllocCtx;

typedef struct ResizeOOMCtx {
  MSize newmask;
} ResizeOOMCtx;

typedef struct ReclaimReaderCtx {
  global_State *g;
  uint64_t epoch;
  uint32_t done;
  uint32_t reclaimed;
} ReclaimReaderCtx;

static MSize assert_resize_state(StrTabHdr *hdr)
{
  MSize state = la_load32_acq(&hdr->resize);
  assert((state & TEST_STRTAB_RESIZE_LOBITS) == 0);
  assert(state == 0 || state == TEST_STRTAB_RESIZE);
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

static void *release_active_after_claim(void *arg)
{
  ActiveReleaseCtx *ctx = (ActiveReleaseCtx *)arg;
  while (assert_resize_state(ctx->hdr) == 0)
    (void)lj_thr_sleep_ns(NULL, 100000);
  assert_resize_claimed(ctx->hdr);
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

static int protected_resize(lua_State *L)
{
  ResizeOOMCtx *ctx = (ResizeOOMCtx *)lua_touserdata(L, 1);
  lj_str_resize(L, ctx->newmask);
  return 0;
}

static void *reclaim_retired_worker(void *arg)
{
  ReclaimReaderCtx *ctx = (ReclaimReaderCtx *)arg;
  ctx->reclaimed = lj_gc2_reclaim_retired(ctx->g, ctx->epoch);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

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

  exercise_resize_oom_does_not_claim();

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
  assert_resize_claimed(hdr);
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
  printf("t-strtab-cas OK: resize OOM, active-drain claim, TLS-only active drain, GC2 epoch retire, duplicate intern guard, and string ID/count block reservation verified\n");
  return 0;
}
