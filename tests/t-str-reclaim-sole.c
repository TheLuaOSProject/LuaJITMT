/*
** b1.2 explicit sole-mutator string-body reclamation regressions.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_str.h"
#include "lj_tg.h"
#include "lj_thr.h"

typedef struct AttachRaceCtx {
  lua_State *L;
  global_State *g;
  uint32_t go;
  uint32_t hook_seen;
  uint32_t attached;
  uint32_t done;
} AttachRaceCtx;

static AttachRaceCtx *attach_race;

static void *attach_race_worker(void *ud)
{
  AttachRaceCtx *ctx = (AttachRaceCtx *)ud;
  while (la_load32_acq(&ctx->go) == 0)
    (void)lj_thr_retry_yield(NULL);
  if (lj_threading_attach(ctx->L)) {
    la_store32_rel(&ctx->attached, 1);
    lj_threading_detach(ctx->L, 0);
  }
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void attach_race_hook(global_State *g, uint32_t stage)
{
  AttachRaceCtx *ctx = attach_race;
  uint32_t i;
  assert(ctx != NULL && ctx->g == g);
  assert(stage == LJ_STR_TEST_RECLAIM_EXCLUSIVE_CLAIMED);
  la_store32_rel(&ctx->hook_seen, 1);
  la_store32_rel(&ctx->go, 1);
  /* The entrant publishes mt_entering before its reciprocal gate recheck.
  ** Return only after that publication so the collector must lose admission. */
  for (i = 0; i < 1000000u && mt_entering_acq(g) == 0; i++)
    (void)lj_thr_retry_yield(NULL);
  assert(mt_entering_acq(g) != 0);
}

static GCstr *candidate_target;
static uint32_t candidate_rescues;

static void candidate_rescue_hook(global_State *g, uint32_t stage)
{
  uintptr_t link;
  if (stage != LJ_STR_TEST_RECLAIM_CANDIDATE)
    return;
  link = lj_str_link_load_acq(lj_str_sweep_link_acq(g));
  if (lj_str_link_target(link) != obj2gco(candidate_target))
    return;
  assert((link & LJ_STRHASH_DEAD) == 0);
  assert(lj_str_canon_acq(candidate_target) == LJ_STR_CANON_CANDIDATE);
  (void)lj_gc2_trace_sweep_root(g, obj2gco(candidate_target));
  assert(lj_str_canon_acq(candidate_target) == LJ_STR_CANON_LIVE);
  candidate_rescues++;
}

static void check_candidate_rescue(lua_State *L)
{
  static const char bytes[] = "candidate-rescued-before-dead";
  candidate_target = lj_str_new(L, bytes, sizeof(bytes)-1u);
  lj_str_test_set_reclaim_hook(candidate_rescue_hook);
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  lj_str_test_set_reclaim_hook(NULL);
  assert(candidate_rescues == 1u);
  assert(lj_str_new(L, bytes, sizeof(bytes)-1u) == candidate_target);
  assert(lj_str_canon_acq(candidate_target) == LJ_STR_CANON_LIVE);
  candidate_target = NULL;
}

static void intern_churn(lua_State *L, const char *prefix, uint32_t n)
{
  uint32_t i;
  char buf[96];
  for (i = 0; i < n; i++) {
    int len = snprintf(buf, sizeof(buf), "%s-%u-%08x", prefix, i,
		       i * UINT32_C(2654435761));
    assert(len > 0 && (size_t)len < sizeof(buf));
    assert(lj_str_new(L, buf, (size_t)len) != NULL);
  }
}

#if LJ_HASFFI
static const char native_bytes[] = "borrowed while a callback collects";
static uint32_t native_borrow_calls, native_reclaim_claims;

static void native_borrow_callback(const char *str, void (*cb)(void))
{
  assert(memcmp(str, native_bytes, sizeof(native_bytes)) == 0);
  cb();
  assert(memcmp(str, native_bytes, sizeof(native_bytes)) == 0);
  native_borrow_calls++;
}

static void count_native_reclaim_claims(global_State *g, uint32_t stage)
{
  UNUSED(g);
  if (stage == LJ_STR_TEST_RECLAIM_EXCLUSIVE_CLAIMED)
    native_reclaim_claims++;
}

static void check_native_callback_borrow(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  uint64_t completed0;
  MSize count0;
  assert(L != NULL);
  g = G(L);
  luaL_openlibs(L);
  lua_pushlightuserdata(L, (void *)native_borrow_callback);
  lua_setglobal(L, "native_borrow_callback");
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "jit.off()\n"
    "local cb = ffi.cast('void (*)(void)', function()\n"
    "  collectgarbage('collect')\n"
    "end)\n"
    "local native = ffi.cast('void (*)(const char *, void (*)(void))', "
    "native_borrow_callback)\n"
    "return function(bytes) native(bytes, cb) end\n") == 0);
  lua_pushlstring(L, native_bytes, sizeof(native_bytes)-1u);
  intern_churn(L, "native-borrow-unreachable", 512u);
  lj_str_flush_num_credit(g, L2TG(L));
  completed0 = gc2_sweep_to_idle_acq(g);
  count0 = lj_str_num_acq(g);
  lj_str_test_set_reclaim_hook(count_native_reclaim_claims);
  assert(lua_pcall(L, 1, 0, 0) == 0);
  assert(native_borrow_calls == 1u);
  assert(gc2_sweep_to_idle_acq(g) > completed0);
  assert(native_reclaim_claims == 0u);
  assert(lj_str_num_acq(g) >= count0);
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  assert(native_reclaim_claims == 1u);
  assert(lj_str_num_acq(g) + 512u <= count0);
  lj_str_test_set_reclaim_hook(NULL);
  lua_close(L);
}
#endif

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  LJStrTestSweepSnapshot snap;
  GCSize bytes0, bytes_peak, bytes_after;
  MSize num0, num_peak, num_after, race_peak, race_after;
  AttachRaceCtx ctx;
  LJThr worker;

  assert(L != NULL);
  g = G(L);
  tg = L2TG(L);
  assert(tg == g->main_tg);
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  lj_str_flush_num_credit(g, tg);
  num0 = lj_str_num_acq(g);
  bytes0 = lj_gc_total_load(g);

  lj_str_test_reset_sweep_counters(g);
  intern_churn(L, "sole-string", 10000u);
  lj_str_flush_num_credit(g, tg);
  num_peak = lj_str_num_acq(g);
  bytes_peak = lj_gc_total_load(g);
  assert(num_peak >= num0 + 10000u);
  assert(bytes_peak > bytes0 + 300000u);

  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  lj_str_flush_num_credit(g, tg);
  num_after = lj_str_num_acq(g);
  bytes_after = lj_gc_total_load(g);
  lj_str_test_sweep_snapshot(g, &snap);
  assert(num_after <= num0 + 32u);
  assert(num_peak - num_after >= 10000u);
  assert(bytes_peak > bytes_after + 250000u);
  assert(snap.unlinked >= 10000u);
  assert(snap.reclaimed == snap.unlinked);
  assert(snap.retired == 0);
  assert(lj_str_sweep_batch_acq(g) == NULL);
  assert(lj_str_retired_batch_head_acq(g) == NULL);
  assert(lj_str_reclaim_exclusive_acq(g) == 0);

  check_candidate_rescue(L);

  /* Force the no-both-win attach race at the collector's post-CAS hook. */
  memset(&ctx, 0, sizeof(ctx));
  ctx.L = lua_newthread(L);  /* Keep the foreign-attach target rooted. */
  ctx.g = g;
  memset(&worker, 0, sizeof(worker));
  intern_churn(L, "attach-race-string", 512u);
  lj_str_flush_num_credit(g, tg);
  race_peak = lj_str_num_acq(g);
  attach_race = &ctx;
  lj_str_test_set_reclaim_hook(attach_race_hook);
  assert(lj_thr_create(&worker, attach_race_worker, &ctx) == 0);
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lj_thr_join(&worker, NULL) == 0);
  lj_str_test_set_reclaim_hook(NULL);
  attach_race = NULL;
  assert(la_load32_acq(&ctx.hook_seen) != 0);
  assert(la_load32_acq(&ctx.attached) != 0);
  assert(la_load32_acq(&ctx.done) != 0);
  assert(lj_str_reclaim_exclusive_acq(g) == 0);
  assert(lj_str_sweep_batch_acq(g) == NULL);
  assert(lj_str_retired_batch_head_acq(g) == NULL);
  lj_str_flush_num_credit(g, tg);
  race_after = lj_str_num_acq(g);
  assert(race_after >= race_peak);  /* Losing admission did not unlink. */

  lua_settop(L, 0);  /* Drop the coroutine before the successful retry. */
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  lj_str_flush_num_credit(g, tg);
  assert(lj_str_num_acq(g) + 512u <= race_after);
  assert(lj_str_reclaim_exclusive_acq(g) == 0);

  /* Terminal cancellation must tolerate a requested but never consumed pass. */
  lj_str_gc2_reclaim_request(g);
  lua_close(L);
#if LJ_HASFFI
  check_native_callback_borrow();
#endif
  printf("t-str-reclaim-sole OK: exact count/memory shrink, candidate rescue, batch drain, attach-race backout, and terminal request cleanup\n");
  return 0;
}
