/*
** Deterministic terminal lua_State preparation and open-upvalue regression.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_func.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_state.h"
#include "lj_thr.h"
#include "lj_tg.h"
#if LJ_HASFFI
#include "lj_ccallback.h"
#include "lj_ctype.h"
#endif

#include "lib/lua_fixture_helpers.h"

typedef struct ReclaimCtx {
  global_State *g;
  lua_State *target;
  uint32_t done;
  uint32_t actor;
  int result;
} ReclaimCtx;

typedef struct PublishCtx {
  global_State *g;
  GCobj *o;
  int result;
} PublishCtx;

static void *reclaim_main(void *arg)
{
  ReclaimCtx *ctx = (ReclaimCtx *)arg;
  while (!lj_gc2_test_idle_reclaim_enter(ctx->g))
    (void)lj_thr_retry_yield(NULL);
  ctx->result = lj_gc_test_reclaim_thread(ctx->g, ctx->target);
  lj_gc2_test_idle_reclaim_leave(ctx->g);
  ctx->actor = lj_thr_actor_current();
  (void)lj_gc2_test_recovery_drain(ctx->g, LJ_GC2_ROOT_SCAN_LIMIT);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void *publish_main(void *arg)
{
  PublishCtx *ctx = (PublishCtx *)arg;
  ctx->result = lj_gc2_test_recovery_publish(ctx->g, ctx->o);
  return NULL;
}

static int reclaim_thread_once(global_State *g, lua_State *target)
{
  int result;
  while (!lj_gc2_test_idle_reclaim_enter(g))
    (void)lj_thr_retry_yield(NULL);
  result = lj_gc_test_reclaim_thread(g, target);
  lj_gc2_test_idle_reclaim_leave(g);
  return result;
}

#if LJ_HASFFI
static lua_State *callback_owner_first(global_State *g)
{
  CTState *cts = ctype_ctsG(g);
  lua_State **owner;
  MSize i, n;
  if (!cts)
    return NULL;
  owner = ctype_cb_owner_acq(cts);
  n = ctype_cb_sizeid_acq(cts);
  if (!owner || n > lj_ccallback_maxslot())
    return NULL;
  for (i = 0; i < n; i++) {
    lua_State *L = ctype_cb_owner_slot_acq(owner, i);
    if (L)
      return L;
  }
  return NULL;
}

static int callback_owner_has(global_State *g, lua_State *L)
{
  CTState *cts = ctype_ctsG(g);
  lua_State **owner;
  MSize i, n;
  if (!cts)
    return 0;
  owner = ctype_cb_owner_acq(cts);
  n = ctype_cb_sizeid_acq(cts);
  if (!owner || n > lj_ccallback_maxslot())
    return 0;
  for (i = 0; i < n; i++)
    if (ctype_cb_owner_slot_acq(owner, i) == L)
      return 1;
  return 0;
}
#endif

static void wait_for_pre_lp(ReclaimCtx *ctx)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_state_test_gcprep_pre_lp_paused())
      return;
    assert(la_load32_acq(&ctx->done) == 0);
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"terminal THREAD pre-LP hook was not reached");
}

static void wait_for_lifetime_claim(ReclaimCtx *ctx)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_arena_test_lifetime_paused())
      return;
    assert(la_load32_acq(&ctx->done) == 0);
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"terminal THREAD lifetime claim hook was not reached");
}

static void wait_for_gcprep(ReclaimCtx *ctx)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_state_test_gcprep_paused())
      return;
    assert(la_load32_acq(&ctx->done) == 0);
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"terminal THREAD preparation hook was not reached");
}

static void test_late_rescue_cancel(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  lua_State *co;
  GCArena *a;
  uint32_t cell;
  ReclaimCtx ctx;
  PublishCtx pub;
  pthread_t collector;
  pthread_t publisher;

  ljt_lua_dostring(L,
    "collectgarbage('stop')\n"
    "hold_co = coroutine.create(function()\n"
    "  coroutine.yield()\n"
    "end)\n"
    "assert(coroutine.resume(hold_co))\n");
  lua_getglobal(L, "hold_co");
  co = lua_tothread(L, -1);
  assert(co != NULL && co != L);
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_setglobal(L, "hold_co");

  a = lj_arena_of(co);
  cell = lj_arena_cellof(co);
  assert(!lj_arena_ishuge(a));
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_gc_unlink_root_obj(g, obj2gco(co)) == LJ_GC_ROOT_UNLINKED);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);

  ctx.g = g;
  ctx.target = co;
  ctx.done = 0;
  ctx.actor = 0;
  ctx.result = -1;

  /* Pause the target after its owner/root preflight, then arm the allocator's
  ** next lifetime-claim hook. Recovery publication wins DESTRUCT->RESCUE, so
  ** the terminal split must cancel both incomplete pins and leave no queue. */
  lj_state_test_gcprep_pre_lp_pause(1);
  assert(pthread_create(&collector, NULL, reclaim_main, &ctx) == 0);
  wait_for_pre_lp(&ctx);
  assert(lj_state_owner_acq(co) == LJ_THREAD_GCPREP);
  assert(lj_state_gcprep_state_acq(co) == LJ_STATE_GCPREP_NONE);
  assert(lj_arena_gcprep_pending_acq(a) != 0);
  assert(lj_state_gcprep_pending_acq(g) != 0);
  lj_arena_test_lifetime_pause(1);
  lj_state_test_gcprep_pre_lp_pause(0);
  wait_for_lifetime_claim(&ctx);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_DESTRUCT);
  pub.g = g;
  pub.o = obj2gco(co);
  pub.result = 0;
  lj_gc2_test_recovery_pause(LJ_GC2_RECOVERY_TEST_PRE_LIFETIME_RESTORE);
  assert(pthread_create(&publisher, NULL, publish_main, &pub) == 0);
  while (lj_gc2_test_recovery_paused() !=
	 LJ_GC2_RECOVERY_TEST_PRE_LIFETIME_RESTORE)
    (void)lj_thr_retry_yield(NULL);
  assert(lj_arena_lifetime_state_acq(a, cell) ==
	 LJ_ARENA_LIFETIME_RESCUE);
  lj_gc2_test_recovery_release();
  assert(pthread_join(publisher, NULL) == 0);
  assert(pub.result == 1);
  lj_arena_test_lifetime_pause(0);
  assert(pthread_join(collector, NULL) == 0);
  assert(ctx.result == LJ_GC_DESTRUCT_LOST);
  assert(lj_state_gcprep_state_acq(co) == LJ_STATE_GCPREP_NONE);
  assert(lj_state_owner_acq(co) != LJ_THREAD_GCPREP);
  assert(lj_state_gcprep_pending_acq(g) == 0);
  assert(lj_arena_gcprep_pending_acq(a) == 0);

  lua_close(L);
}

static void test_terminal_queue_open_upvalue(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  lua_State *co;
  GCfunc *fn;
  GCupval *uv;
  TValue *slot;
  GCArena *a;
  uint32_t cell;
  ReclaimCtx ctx;
  pthread_t collector;

  ljt_lua_dostring(L,
    "collectgarbage('stop')\n"
    "hold_co = coroutine.create(function()\n"
    "  coroutine.yield()\n"
    "end)\n"
    "assert(coroutine.resume(hold_co))\n"
    "hold_fn = (function(x) return function() return x + 1 end end)(0)\n");
  lua_getglobal(L, "hold_co");
  co = lua_tothread(L, -1);
  assert(co != NULL && co != L);
  lua_pop(L, 1);
  lua_getglobal(L, "hold_fn");
  assert(tvisfunc(L->top-1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn) && lj_funcL_nupvalues(&fn->l) == 1);
  lua_pop(L, 1);

  /* New bytecode promotes ordinary captures to closed cells. Build one exact
  ** legacy open-upvalue edge explicitly so this fixture covers the destructor
  ** obligation retained for API/old-bytecode compatibility. */
  assert(co->top < tvref(co->maxstack));
  slot = co->top++;
  setintV(slot, 41);
  uv = lj_func_test_openuv(co, slot);
  assert(uv != NULL && !uv->closed && uvval(uv) == slot);
  setgcrefrel(fn->l.uvptr[0], obj2gco(uv));
  lj_gc_pubobjobj(L, fn, uv);

  lua_pushnil(L);
  lua_setglobal(L, "hold_co");

  a = lj_arena_of(co);
  cell = lj_arena_cellof(co);
  assert(!lj_arena_ishuge(a));
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_state_openupval_acq(co) != NULL);
  assert(lj_gc_unlink_root_obj(g, obj2gco(co)) == LJ_GC_ROOT_UNLINKED);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);

  ctx.g = g;
  ctx.target = co;
  ctx.done = 0;
  ctx.actor = 0;
  ctx.result = -1;
  lj_state_test_gcprep_pause(1);
  assert(pthread_create(&collector, NULL, reclaim_main, &ctx) == 0);
  wait_for_gcprep(&ctx);

  /* The exact reclaimer crossed semantic death, but the queue and per-arena
  ** incomplete pin still own every byte. Claims reject GCPREP without waiting
  ** and quarantine cannot clear/reuse this allocation before pause release. */
  assert(lj_state_gcprep_state_acq(co) == LJ_STATE_GCPREP_PENDING);
  assert(lj_state_owner_acq(co) == LJ_THREAD_GCPREP);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_FREE);
  assert(lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_FREEING);
  assert(lj_arena_gcprep_pending_acq(a) != 0);
  assert(lj_state_gcprep_pending_acq(g) != 0);
  assert(lj_arena_bm_get(a->block, cell));
  assert(lj_arena_quarantine_owns_body(co, sizeof(lua_State)));
  assert(!lj_state_claim(co, lj_thr_current_id(g)));

  lj_state_test_gcprep_pause(0);
  assert(pthread_join(collector, NULL) == 0);
  assert(ctx.result == LJ_GC_DESTRUCT_ACQUIRED);
  assert(ctx.actor != 0 && ctx.actor != lj_thr_actor_current());
  assert(lj_state_owner_word_acq(co) ==
	 lj_state_owner_pack(LJ_THREAD_GCPREP, ctx.actor));
  /* The publishing reclaimer has left its exclusive scope. Main now owns the
  ** unique pop and must retag GCPREP to its actor before destructing co. */
  assert(lj_state_gcprep_drain(g, LJ_GC2_ROOT_SCAN_LIMIT) == 1);
  assert(lj_state_gcprep_pending_acq(g) == 0);

  /* Preparation copied the stack cell into the escaped upvalue before the
  ** stack vector was released. Later collections may reuse the state body. */
  ljt_lua_dostring(L,
    "collectgarbage('restart')\n"
    "for _ = 1, 3 do collectgarbage('collect') end\n"
    "assert(hold_fn() == 42)\n");

  lua_close(L);
}

static void test_close_drains_terminal_queue(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  lua_State *co;
  GCArena *a;
  uint32_t cell;
  int result;

  ljt_lua_dostring(L,
    "collectgarbage('stop')\n"
    "hold_co = coroutine.create(function() end)\n");
  assert(lj_gc2_workers_set(g, 0));
  lua_getglobal(L, "hold_co");
  co = lua_tothread(L, -1);
  assert(co != NULL && co != L);
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_setglobal(L, "hold_co");

  a = lj_arena_of(co);
  cell = lj_arena_cellof(co);
  assert(lj_gc_unlink_root_obj(g, obj2gco(co)) == LJ_GC_ROOT_UNLINKED);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);
  result = reclaim_thread_once(g, co);
  assert(result == LJ_GC_DESTRUCT_ACQUIRED);
  assert(lj_state_gcprep_state_acq(co) == LJ_STATE_GCPREP_PENDING);
  assert(lj_state_gcprep_pending_acq(g) == 1);
  assert(lj_arena_gcprep_pending_acq(a) == 1);

  lj_state_test_gcprep_terminal_drain_reset();
  lua_close(L);
  assert(lj_state_test_gcprep_terminal_drain_count() == 1);
}

static void test_registry_membership_veto(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  lua_State *co;
  GCArena *a;
  uint32_t cell;

  ljt_lua_dostring(L,
    "collectgarbage('stop')\n"
    "hold_co = coroutine.create(function() end)\n");
  assert(lj_gc2_workers_set(g, 0));
  lua_getglobal(L, "hold_co");
  co = lua_tothread(L, -1);
  assert(co != NULL && co != L);
  lua_pop(L, 1);
  lj_state_thread_registry_publish(g, co);
  assert(lj_state_thread_registry_head_acq(g) == co);
  lua_pushnil(L);
  lua_setglobal(L, "hold_co");

  a = lj_arena_of(co);
  cell = lj_arena_cellof(co);
  assert(lj_gc_unlink_root_obj(g, obj2gco(co)) == LJ_GC_ROOT_UNLINKED);
  assert(reclaim_thread_once(g, co) == LJ_GC_DESTRUCT_LOST);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_state_owner_acq(co) == 0);
  assert(lj_state_gcprep_pending_acq(g) == 0);
  assert(lj_arena_gcprep_pending_acq(a) == 0);
  lua_close(L);
}

static void test_stale_global_cur_mirror_is_not_root(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  lua_State *co;
  GCArena *a;
  uint32_t cell;

  ljt_lua_dostring(L,
    "collectgarbage('stop')\n"
    "hold_co = coroutine.create(function() end)\n");
  assert(lj_gc2_workers_set(g, 0));
  lua_getglobal(L, "hold_co");
  co = lua_tothread(L, -1);
  assert(co != NULL && co != L);
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_setglobal(L, "hold_co");

  a = lj_arena_of(co);
  cell = lj_arena_cellof(co);
  assert(lj_gc_unlink_root_obj(g, obj2gco(co)) == LJ_GC_ROOT_UNLINKED);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE);

  /* x64 resumes publish cur_L in the current TG. The process-global field is a
  ** compatibility mirror and may still name the last resumed coroutine. */
  lj_tg_store_cur_L(g->main_tg, L);
  setgcrefrel(g->cur_L, obj2gco(co));
  assert(lj_tg_cur_L(g) == L);
  assert(gcref_acq(g->cur_L) == obj2gco(co));
  assert(reclaim_thread_once(g, co) == LJ_GC_DESTRUCT_ACQUIRED);

  /* Repair the deliberately stale test mirror before releasing co's final
  ** preparation pin and permitting its arena body to be reused. */
  setgcrefrel(g->cur_L, obj2gco(L));
  assert(lj_state_gcprep_drain(g, LJ_GC2_ROOT_SCAN_LIMIT) == 1);
  assert(lj_state_gcprep_pending_acq(g) == 0);
  lua_close(L);
}

#if LJ_HASFFI
static void test_callback_owner_veto(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  lua_State *co;
  GCArena *a;
  uint32_t cell;

  ljt_lua_dostring(L,
    "collectgarbage('stop')\n"
    "local ffi = require('ffi')\n"
    "hold_cb = ffi.cast('int(*)(int)', function(x) return x + 1 end)\n");
  assert(lj_gc2_workers_set(g, 0));
  /* Callback slots deliberately name a hidden carrier lua_State rather than
  ** the allocating coroutine. That carrier is the exact semantic root whose
  ** terminal preflight must veto. */
  co = callback_owner_first(g);
  assert(co != NULL && co != L);
  assert(callback_owner_has(g, co));

  a = lj_arena_of(co);
  cell = lj_arena_cellof(co);
  assert(lj_gc_unlink_root_obj(g, obj2gco(co)) == LJ_GC_ROOT_UNLINKED);
  assert(reclaim_thread_once(g, co) == LJ_GC_DESTRUCT_LOST);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_state_owner_acq(co) == 0);
  assert(lj_state_gcprep_pending_acq(g) == 0);
  assert(lj_arena_gcprep_pending_acq(a) == 0);

  lj_ccallback_disown_state(co);
  assert(!callback_owner_has(g, co));
  lua_pushnil(L);
  lua_setglobal(L, "hold_cb");
  assert(reclaim_thread_once(g, co) == LJ_GC_DESTRUCT_ACQUIRED);
  assert(lj_state_gcprep_drain(g, LJ_GC2_ROOT_SCAN_LIMIT) == 1);
  assert(lj_state_gcprep_pending_acq(g) == 0);
  lua_close(L);
}
#endif

int main(void)
{
  test_late_rescue_cancel();
  test_terminal_queue_open_upvalue();
  test_close_drains_terminal_queue();
  test_registry_membership_veto();
  test_stale_global_cur_mirror_is_not_root();
#if LJ_HASFFI
  test_callback_owner_veto();
#endif
  puts("t-thread-gcprep OK: terminal queue pins and closes open upvalues");
  return 0;
}
