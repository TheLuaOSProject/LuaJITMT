/*
** Focused per-TG JIT VM-event callback-owner regression.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_jit.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"
#include "lj_vmevent.h"

#include "lib/test_sleep.h"

#ifndef LJ_GC2_TEST_HELPERS
#error "t-jit-event-callback-owner requires LJ_GC2_TEST_HELPERS"
#endif

#define TEST_WAIT_ATTEMPTS 5000u
#define TEST_WAIT_NS 1000000L

typedef struct CallbackWorkerCtx {
  lua_State *L;
  jit_State *J;
  GCfunc *handler;
  TGState *tg;
  uint8_t global_hookmask;
  uint32_t active;
  uint32_t release;
  uint32_t done;
} CallbackWorkerCtx;

static uint32_t callback_debug_hits;

static int callback_handler(lua_State *L)
{
  UNUSED(L);
  return 0;
}

static void callback_debug_hook(lua_State *L, lua_Debug *ar)
{
  UNUSED(L);
  UNUSED(ar);
  (void)la_add32_rlx(&callback_debug_hits, 1u);
}

static void run_callback_lua(lua_State *L)
{
  static const char code[] =
    "local ok, v = pcall(function() "
    "  local s = 0; for i = 1, 100 do s = s + i end; return s "
    "end); assert(ok and v == 5050); return v";
  assert(luaL_loadbuffer(L, code, sizeof(code)-1, "callback-owner") == 0);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  assert(lua_tonumber(L, -1) == 5050);
  lua_pop(L, 1);
}

static void wait_flag(uint32_t *flag)
{
  uint32_t attempts;
  for (attempts = 0;
       la_load32_acq(flag) == 0 && attempts < TEST_WAIT_ATTEMPTS;
       attempts++)
    sleep_ns(TEST_WAIT_NS);
  assert(la_load32_acq(flag) != 0);
}

static void publish_callback_session(lua_State *L, jit_State *J,
                                     GCfunc *handler,
                                     uint64_t attachment_generation,
                                     LJJitEventSessionHandle *handle)
{
  LJJitEventSessionSpec spec;
  assert(lj_jit_token_try_l(L, J));
  jit_owner_l_rel(J, L);
  memset(&spec, 0, sizeof(spec));
  spec.event = LJ_JIT_EVENT_TRACE_FLUSH;
  spec.owner_mode = LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE;
  spec.edge_proof = LJ_JIT_EVENT_EDGE_NONE;
  spec.attachment_generation = attachment_generation;
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_PUBLISHED;
  spec.callback_root_count = 1u;
  spec.callback_handler = handler;
  assert(lj_jit_event_session_begin_l(L, J, &spec, handle));
  assert(jit_owner_word_acq(G(L)) == jit_owner_pack(0, 0));
  assert(jit_owner_l_acq(J) == NULL);
}

static void acquire_callback_session(lua_State *L, TGState *tg,
                                     LJJitEventSessionSnapshot *snapshot)
{
  uint32_t attempts;
  int result = LJ_JIT_EVENT_SNAPSHOT_RETRY;
  for (attempts = 0; attempts < TEST_WAIT_ATTEMPTS; attempts++) {
    result = lj_jit_event_session_snapshot_acquire(G(L), tg, snapshot);
    if (result != LJ_JIT_EVENT_SNAPSHOT_RETRY)
      break;
    sleep_ns(TEST_WAIT_NS);
  }
  assert(result == LJ_JIT_EVENT_SNAPSHOT_ACTIVE);
  assert(snapshot->callback_root_count == 1u);
  assert(snapshot->callback_handler != NULL);
}

static void expect_owner_idle(TGState *tg, uint64_t next_generation)
{
  LJJitEventCallbackSnapshot snapshot;
  memset(&snapshot, 0xa5, sizeof(snapshot));
  assert(lj_jit_event_callback_snapshot(tg, &snapshot) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_IDLE);
  assert(snapshot.tg == tg);
  assert((snapshot.sequence & 1u) == 0);
  assert(snapshot.next_generation == next_generation);
  assert(snapshot.generation == 0);
  assert(snapshot.stream_generation == 0);
  assert(snapshot.session_generation == 0);
  assert(snapshot.state == LJ_JIT_EVENT_CALLBACK_IDLE);
  assert(snapshot.owner_actor == 0);
  assert(snapshot.event == 0);
  assert(snapshot.session_slot == 0);
  assert(snapshot.owner_L == NULL);
  assert(lj_jit_event_callback_idle(tg));
}

static void expect_owner_active(TGState *tg,
                                const LJJitEventCallbackHandle *handle,
                                uint32_t state)
{
  LJJitEventCallbackSnapshot snapshot;
  assert(lj_jit_event_callback_snapshot(tg, &snapshot) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE);
  assert(snapshot.tg == tg);
  assert(snapshot.generation == handle->generation);
  assert(snapshot.next_generation == handle->generation);
  assert(snapshot.stream_generation == handle->stream_generation);
  assert(snapshot.session_generation == handle->session_generation);
  assert(snapshot.state == state);
  assert(snapshot.owner_actor == handle->owner_actor);
  assert(snapshot.event == handle->event);
  assert(snapshot.session_slot == handle->session_slot);
  assert(snapshot.owner_L == handle->owner_L);
  assert(!lj_jit_event_callback_idle(tg));
}

static void complete_callback(lua_State *L,
                              const LJJitEventCallbackHandle *handle)
{
  assert(lj_jit_event_callback_unwind_l(L, handle));
  expect_owner_active(handle->tg, handle,
                      LJ_JIT_EVENT_CALLBACK_UNWINDING);
  assert(lj_jit_event_callback_release_l(L, handle));
  assert((lj_tg_hookmask_load(handle->tg) &
          (HOOK_ACTIVE|HOOK_VMEVENT)) == 0);
}

static void *callback_worker(void *arg)
{
  CallbackWorkerCtx *ctx = (CallbackWorkerCtx *)arg;
  LJJitEventSessionHandle session_handle;
  LJJitEventSessionSnapshot session;
  LJJitEventCallbackHandle callback;
  TGState *tg;

  assert(lj_threading_attach(ctx->L));
  tg = lj_thr_get_tg();
  assert(tg && tg != G(ctx->L)->main_tg && tg->gl == G(ctx->L));
  ctx->tg = tg;
  publish_callback_session(ctx->L, ctx->J, ctx->handler, 202u,
                           &session_handle);
  acquire_callback_session(ctx->L, tg, &session);
  assert(lj_jit_event_callback_claim_l(ctx->L, 2002u, &session, &callback));
  expect_owner_active(tg, &callback, LJ_JIT_EVENT_CALLBACK_CALLING);
  assert((lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT)) ==
         (HOOK_ACTIVE|HOOK_VMEVENT));
  assert(hookmask_load(G(ctx->L)) == ctx->global_hookmask);
  assert(vmevent_owner_acq(G(ctx->L)) == 0);
  la_store32_rel(&ctx->active, 1);
  wait_flag(&ctx->release);

  complete_callback(ctx->L, &callback);
  expect_owner_idle(tg, callback.generation);
  assert(hookmask_load(G(ctx->L)) == ctx->global_hookmask);
  assert(vmevent_owner_acq(G(ctx->L)) == 0);
  assert(lj_jit_event_session_snapshot_release(&session));
  assert(lj_jit_event_session_end_l(ctx->L, ctx->J, &session_handle));
  lj_threading_detach(ctx->L, 1);
  assert(lj_thr_get_tg() == NULL);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void test_snapshot_retry_and_saturation(
  lua_State *L, TGState *tg, const LJJitEventSessionSnapshot *session)
{
  LJJitEventCallbackOwner *owner = &tg->jit_event_callback_owner;
  LJJitEventCallbackSnapshot snapshot;
  LJJitEventCallbackHandle rejected;

  la_store64_rel(&owner->sequence, 1u);
  assert(lj_jit_event_callback_snapshot(tg, &snapshot) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_RETRY);
  assert(!lj_jit_event_callback_idle(tg));
  la_store64_rel(&owner->sequence, 0);

  (void)lj_tg_hookmask_update(tg, 0, HOOK_ACTIVE);
  assert(lj_jit_event_callback_snapshot(tg, &snapshot) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_RETRY);
  assert(!lj_jit_event_callback_idle(tg));
  (void)lj_tg_hookmask_update(tg, HOOK_ACTIVE, 0);
  (void)lj_tg_hookmask_update(tg, 0, HOOK_ACTIVE|HOOK_VMEVENT);
  assert(lj_jit_event_callback_snapshot(tg, &snapshot) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_RETRY);
  assert(lj_tg_hookmask_callback_leave_exact(tg));
  expect_owner_idle(tg, 0);

  la_store64_rel(&owner->next_generation, UINT64_MAX);
  assert(!lj_jit_event_callback_claim_l(L, 1u, session, &rejected));
  assert((lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT)) == 0);
  la_store64_rel(&owner->next_generation, 0);

  la_store64_rel(&owner->sequence, UINT64_MAX - 5u);
  assert(!lj_jit_event_callback_claim_l(L, 1u, session, &rejected));
  assert((lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT)) == 0);
  la_store64_rel(&owner->sequence, 0);
  expect_owner_idle(tg, 0);
}

static void test_owner_transitions(
  lua_State *L, jit_State *J, TGState *tg,
  const LJJitEventSessionHandle *session_handle,
  const LJJitEventSessionSnapshot *session)
{
  LJJitEventCallbackHandle first, second, rejected, stale;
  uint8_t global = hookmask_load(G(L));
  uint32_t legacy_expect = 0;

  assert(vmevent_owner_cas(G(L), &legacy_expect, lj_tg_tid_acq(tg)));
  assert(!lj_jit_event_callback_claim_l(L, 8u, session, &rejected));
  assert((lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT)) == 0);
  vmevent_owner_rel(G(L), lj_tg_tid_acq(tg));

  (void)lj_tg_hookmask_update(tg, 0, HOOK_PROFILE);
  assert(!lj_jit_event_callback_claim_l(L, 10u, session, &rejected));
  assert((lj_tg_hookmask_load(tg) & HOOK_PROFILE) != 0);
  assert((lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT)) == 0);
  (void)lj_tg_hookmask_update(tg, HOOK_PROFILE, 0);

  assert(lj_jit_event_callback_claim_l(L, 11u, session, &first));
  assert(first.generation != 0);
  expect_owner_active(tg, &first, LJ_JIT_EVENT_CALLBACK_CALLING);
  assert((lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT)) ==
         (HOOK_ACTIVE|HOOK_VMEVENT));
  assert(!lj_tg_hookmask_set_if_clear(
    tg, HOOK_PROFILE|HOOK_ACTIVE|HOOK_VMEVENT, HOOK_PROFILE));
  assert((lj_tg_hookmask_load(tg) & HOOK_PROFILE) == 0);
  assert(!lj_jit_event_session_end_l(L, J, session_handle));
  assert(!lj_jit_event_sessions_logical_detach_ready(tg));
  assert(!lj_jit_event_sessions_quiescent(tg));
  (void)lj_tg_hookmask_update(tg, HOOK_VMEVENT, 0);
  {
    LJJitEventCallbackSnapshot malformed;
    assert(lj_jit_event_callback_snapshot(tg, &malformed) ==
           LJ_JIT_EVENT_CALLBACK_SNAPSHOT_RETRY);
  }
  (void)lj_tg_hookmask_update(tg, 0, HOOK_VMEVENT);
  expect_owner_active(tg, &first, LJ_JIT_EVENT_CALLBACK_CALLING);
  assert(hookmask_load(G(L)) == global);
  assert(vmevent_owner_acq(G(L)) == 0);
  assert(!lj_jit_event_callback_claim_l(L, 12u, session, &rejected));
  complete_callback(L, &first);
  expect_owner_idle(tg, first.generation);

  assert(lj_jit_event_callback_claim_l(L, 13u, session, &second));
  assert(second.generation == first.generation + 1u);
  stale = first;
  assert(!lj_jit_event_callback_unwind_l(L, &stale));
  assert(!lj_jit_event_callback_release_l(L, &stale));
  expect_owner_active(tg, &second, LJ_JIT_EVENT_CALLBACK_CALLING);

  stale = second;
  stale.owner_actor++;
  assert(stale.owner_actor != 0 && stale.owner_actor != LJ_THR_ACTOR_RETIRED);
  assert(!lj_jit_event_callback_unwind_l(L, &stale));
  stale = second;
  stale.event = LJ_JIT_EVENT_TRACE_STOP;
  assert(!lj_jit_event_callback_unwind_l(L, &stale));
  stale = second;
  stale.session_slot = (stale.session_slot + 1u) %
    LJ_JIT_EVENT_SESSION_SLOTS;
  assert(!lj_jit_event_callback_unwind_l(L, &stale));
  stale = second;
  stale.session_generation++;
  assert(!lj_jit_event_callback_unwind_l(L, &stale));
  stale = second;
  stale.stream_generation++;
  assert(!lj_jit_event_callback_unwind_l(L, &stale));
  stale = second;
  stale.owner_L = NULL;
  assert(!lj_jit_event_callback_unwind_l(L, &stale));
  expect_owner_active(tg, &second, LJ_JIT_EVENT_CALLBACK_CALLING);

  complete_callback(L, &second);
  expect_owner_idle(tg, second.generation);
  assert(hookmask_load(G(L)) == global);
  assert(vmevent_owner_acq(G(L)) == 0);
}

static void test_gc_while_owner_active(
  lua_State *L, TGState *tg, LJJitEventSessionSnapshot *session)
{
  LJJitEventCallbackHandle callback;
  assert(lj_jit_event_callback_claim_l(L, 9u, session, &callback));
  /* Claim binds the immutable publication. Drop the temporary SMR reader
  ** before executing callback-like Lua/GC work; the active owner prevents the
  ** session root authority from closing until exact release. */
  assert(lj_jit_event_session_snapshot_release(session));
  lua_sethook(L, callback_debug_hook, LUA_MASKCALL|LUA_MASKRET|LUA_MASKCOUNT,
              1);
  la_store32_rel(&callback_debug_hits, 0);
  run_callback_lua(L);
  assert(la_load32_acq(&callback_debug_hits) == 0);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  expect_owner_active(tg, &callback, LJ_JIT_EVENT_CALLBACK_CALLING);
  complete_callback(L, &callback);
  la_store32_rel(&callback_debug_hits, 0);
  run_callback_lua(L);
  assert(la_load32_acq(&callback_debug_hits) != 0);
  lua_sethook(L, NULL, 0, 0);
  acquire_callback_session(L, tg, session);
}

static void test_two_tg_overlap(lua_State *L, jit_State *J, GCfunc *handler,
                                const LJJitEventSessionSnapshot *session)
{
  CallbackWorkerCtx ctx;
  LJJitEventCallbackHandle callback;
  LJJitEventCallbackSnapshot peer;
  lua_State *secondary_L;
  pthread_t worker;
  uint8_t global = hookmask_load(G(L));

  secondary_L = lua_newthread(L);
  assert(secondary_L != NULL);
  memset(&ctx, 0, sizeof(ctx));
  ctx.L = secondary_L;
  ctx.J = J;
  ctx.handler = handler;
  ctx.global_hookmask = global;
  assert(pthread_create(&worker, NULL, callback_worker, &ctx) == 0);
  wait_flag(&ctx.active);
  assert(ctx.tg != NULL && ctx.tg != L2TG(L));
  assert(lj_jit_event_callback_snapshot(ctx.tg, &peer) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE);
  assert(peer.state == LJ_JIT_EVENT_CALLBACK_CALLING);

  assert(lj_jit_event_callback_claim_l(L, 1001u, session, &callback));
  expect_owner_active(L2TG(L), &callback, LJ_JIT_EVENT_CALLBACK_CALLING);
  assert(lj_jit_event_callback_snapshot(ctx.tg, &peer) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE);
  assert(peer.generation != 0 && peer.tg == ctx.tg);
  assert((lj_tg_hookmask_load(L2TG(L)) &
          (HOOK_ACTIVE|HOOK_VMEVENT)) == (HOOK_ACTIVE|HOOK_VMEVENT));
  assert((lj_tg_hookmask_load(ctx.tg) &
          (HOOK_ACTIVE|HOOK_VMEVENT)) == (HOOK_ACTIVE|HOOK_VMEVENT));
  assert(hookmask_load(G(L)) == global);
  assert(vmevent_owner_acq(G(L)) == 0);

  complete_callback(L, &callback);
  la_store32_rel(&ctx.release, 1);
  wait_flag(&ctx.done);
  assert(pthread_join(worker, NULL) == 0);
  assert(hookmask_load(G(L)) == global);
  assert(vmevent_owner_acq(G(L)) == 0);
  lua_pop(L, 1);  /* secondary_L root */
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  jit_State *J;
  TGState *tg;
  GCfunc *handler;
  LJJitEventSessionHandle session_handle;
  LJJitEventSessionSnapshot session;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  J = G2J(g);
  tg = L2TG(L);
  assert(tg && tg->gl == g);
  assert(vmevent_owner_acq(g) == 0);
  expect_owner_idle(tg, 0);

  lua_pushcfunction(L, callback_handler);
  handler = funcV(L->top - 1);
  assert(handler && handler->c.f == callback_handler);
  publish_callback_session(L, J, handler, 101u, &session_handle);
  acquire_callback_session(L, tg, &session);

  test_snapshot_retry_and_saturation(L, tg, &session);
  test_gc_while_owner_active(L, tg, &session);
  test_owner_transitions(L, J, tg, &session_handle, &session);
  test_two_tg_overlap(L, J, handler, &session);

  assert(lj_jit_event_session_snapshot_release(&session));
  assert(lj_jit_event_session_end_l(L, J, &session_handle));
  assert(lj_jit_event_sessions_quiescent(tg));
  lua_pop(L, 1);  /* callback handler */
  lua_close(L);
  puts("t-jit-event-callback-owner OK: per-TG ownership and ABA verified");
  return 0;
}
