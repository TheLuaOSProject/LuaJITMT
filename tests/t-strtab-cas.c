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

typedef struct ActiveReleaseCtx {
  TGState *tg;
  StrTabHdr *hdr;
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
  while ((la_load32_acq(&ctx->hdr->resize) & TEST_STRTAB_RESIZE) == 0)
    (void)lj_thr_sleep_ns(NULL, 100000);
  assert(lj_tg_strtab_active_hdr_acq(ctx->tg) == ctx->hdr);
  assert(lj_tg_strtab_active_depth_acq(ctx->tg) == 1u);
  lj_tg_strtab_active_depth_rel(ctx->tg, 0);
  lj_tg_strtab_active_hdr_rel(ctx->tg, NULL);
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
  assert(hdr->resize == 0);

  resizectx.newmask = (hdr->mask << 1) + 1u;
  lua_pushcfunction(L, protected_resize);
  lua_pushlightuserdata(L, &resizectx);
  allocctx.fail_min = lj_str_tabsize(resizectx.newmask);
  allocctx.fail_next = 1;
  rc = lua_pcall(L, 1, 0, 0);
  assert(rc == LUA_ERRMEM);
  assert(allocctx.fail_next == 0);
  assert(lj_str_tabh_acq(g) == hdr);
  assert(hdr->resize == 0);
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
  TGState *tg;
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
  assert(hdr->resize == 0);
  assert(lj_tg_strtab_active_hdr_acq(tg) == NULL);
  assert(lj_tg_strtab_active_depth_acq(tg) == 0);

  s1 = lj_str_new(L, "m5-strtab-cas-same", strlen("m5-strtab-cas-same"));
  s2 = lj_str_new(L, "m5-strtab-cas-same", strlen("m5-strtab-cas-same"));
  assert(s1 == s2);

  oldmask = g->str.mask;
  wantmask = (oldmask << 1) + 1u;

  /* Simulate the last active interner leaving after resize claims the header. */
  lj_tg_strtab_active_hdr_rel(tg, hdr);
  lj_tg_strtab_active_depth_rel(tg, 1);
  ctx.tg = tg;
  ctx.hdr = hdr;
  assert(lj_thr_create(&release_thr, release_active_after_claim, &ctx) == 0);
  lj_str_resize(L, wantmask);
  assert(lj_thr_join(&release_thr, NULL) == 0);
  assert(lj_tg_strtab_active_hdr_acq(tg) == NULL);
  assert(lj_tg_strtab_active_depth_acq(tg) == 0);
  assert(lj_str_tabh_acq(g) != hdr);
  assert(g->str.mask == wantmask);
  assert(lj_str_tabh_acq(g)->resize == 0);
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
  for (i = 0; i < 1000 &&
	      gc2_smr_reclaiming_acq(g) == 0 &&
	      la_load32_acq(&reclaim_ctx.done) == 0; i++)
    (void)lj_thr_sleep_ns(NULL, 100000);
  assert(gc2_smr_reclaiming_acq(g) != 0);
  assert(la_load32_acq(&reclaim_ctx.done) == 0);
  assert(lj_str_retired_head_acq(g) == hdr);
  lj_gc2_smr_read_leave(g);
  assert(lj_thr_join(&reclaim_thr, NULL) == 0);
  assert(la_load32_acq(&reclaim_ctx.done) != 0);
  assert(reclaim_ctx.reclaimed >= 1u);
  assert(gc2_smr_reclaiming_acq(g) == 0);
  assert(lj_str_retired_head_acq(g) == NULL);
  assert(lj_str_new(L, "m5-strtab-cas-same",
		    strlen("m5-strtab-cas-same")) == s1);

  for (i = 0; i < 8192; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "m5-strtab-cas-%d-%d", i, i * 31);
    assert(lj_str_new(L, buf, strlen(buf)) != NULL);
  }
  assert(lj_str_tabh_acq(g)->resize == 0);
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  assert(gc2_hs_epoch_acq(g) > retire_epoch);
  assert(lj_str_retired_head_acq(g) == NULL);
  assert(gc2_smr_reclaim_runs_acq(g) > smr_runs0);
  assert(gc2_smr_reclaimed_acq(g) >= smr_reclaimed0 + 1u);

  lua_close(L);
  printf("t-strtab-cas OK: resize OOM, active-drain claim, GC2 epoch retire, and duplicate intern guard verified\n");
  return 0;
}
