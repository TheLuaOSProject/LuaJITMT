/*
** Focused per-TG JIT VM-event callback-owner regression.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_jit.h"
#include "lj_profile.h"
#include "lj_safepoint.h"
#include "lj_state.h"
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

enum {
  PROTECTED_CALL_SUCCESS = 1,
  PROTECTED_CALL_ERROR = 2
};

typedef struct ProtectedCallCtx {
  TGState *tg;
  uint32_t mode;
  uint32_t calls;
  uint32_t restore_hooks;
} ProtectedCallCtx;

#if LJ_PROFILE_TGLOCAL
typedef struct ProfileCallbackCtx {
  TGState *tg;
  TGState *other_tg;
  uint64_t next_generation;
  uint32_t calls;
} ProfileCallbackCtx;
#endif

static uint32_t callback_debug_hits;
static ProtectedCallCtx protected_call_ctx;

static void run_callback_lua(lua_State *L);

static int callback_handler(lua_State *L)
{
  UNUSED(L);
  return 0;
}

static int protected_call_handler(lua_State *L)
{
  ProtectedCallCtx *ctx = &protected_call_ctx;
  assert(L2TG(L) == ctx->tg);
  assert(lua_gettop(L) == 1);
  assert(lua_tonumber(L, 1) == 42);
  assert((lj_tg_hookmask_load(ctx->tg) &
          (HOOK_ACTIVE|HOOK_VMEVENT)) ==
         (HOOK_ACTIVE|HOOK_VMEVENT));
  run_callback_lua(L);
  ctx->calls++;
  if (ctx->mode == PROTECTED_CALL_ERROR)
    return luaL_error(L, "injected protected JIT VM-event failure");
  assert(ctx->mode == PROTECTED_CALL_SUCCESS);
  return 0;
}

static void protected_call_restore_hook(lua_State *L, void *ud)
{
  ProtectedCallCtx *ctx = (ProtectedCallCtx *)ud;
  LJJitEventCallbackSnapshot owner;
  assert(ctx == &protected_call_ctx);
  assert(lj_jit_event_callback_snapshot(ctx->tg, &owner) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE);
  assert(owner.state == LJ_JIT_EVENT_CALLBACK_UNWINDING);
  assert((lj_tg_hookmask_load(ctx->tg) &
          (HOOK_ACTIVE|HOOK_VMEVENT)) ==
         (HOOK_ACTIVE|HOOK_VMEVENT));
  /* The VM has finished unwinding. Disturb both owner-local caches now, with
  ** no callout or GC before the helper's exact restoration stores. */
  L->tg_hint = NULL;
  lj_tg_clearcur_L(G(L));
  ctx->restore_hooks++;
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

static void publish_gc_callback_stream(
  lua_State *L, const LJJitEventSessionSnapshot *session,
  uint64_t generation)
{
  TGState *tg = session->tg;
  LJJitTraceStream *stream = &G(L)->main_tg->jit_trace_stream;
  uint64_t sequence = la_load64_acq(&stream->sequence);
  assert(tg == L2TG(L));
  assert(generation != 0 && (sequence & 1u) == 0);
  assert(lj_jit_trace_stream_idle(G(L)));
  la_store64_rel(&stream->sequence, sequence + 1u);
  la_store64_rel(&stream->next_generation, generation);
  la_store64_rel(&stream->generation, generation);
  la_store64_rel(&stream->event_ordinal, 1u);
  la_storeptr_rel((void **)&stream->owner_key.slot, tg->registry_key.slot);
  la_store64_rel(&stream->owner_key.incarnation,
                 tg->registry_key.incarnation);
  la_store32_rel(&stream->owner_tid, lj_tg_tid_acq(tg));
  la_store32_rel(&stream->owner_actor, lj_tg_actor_acq(tg));
  la_store32_rel(&stream->phase, LJ_JIT_STREAM_DETACHED_CALLBACK);
  la_store32_rel(&stream->traceno, 0);
  la_store32_rel(&stream->callback_event, session->event);
  la_store32_rel(&stream->callback_slot, session->slot_index);
  la_store64_rel(&stream->callback_session_generation,
                 session->generation);
  la_store32_rel(&stream->terminal_event, session->event);
  la_store32_rel(&stream->terminal_slot, session->slot_index);
  la_store64_rel(&stream->terminal_session_generation,
                 session->generation);
  la_store32_rel(&stream->terminal_reason, 0);
  la_store32_rel(&stream->flags, 0);
  la_store64_rel(&stream->sequence, sequence + 2u);
  assert(!lj_jit_trace_stream_idle(G(L)));
}

static void clear_gc_callback_stream(lua_State *L)
{
  LJJitTraceStream *stream = &G(L)->main_tg->jit_trace_stream;
  uint64_t sequence = la_load64_acq(&stream->sequence);
  assert((sequence & 1u) == 0);
  la_store64_rel(&stream->sequence, sequence + 1u);
  la_store64_rel(&stream->generation, 0);
  la_store64_rel(&stream->event_ordinal, 0);
  la_storeptr_rel((void **)&stream->owner_key.slot, NULL);
  la_store64_rel(&stream->owner_key.incarnation,
                 LJ_TGSLOT_INCARNATION_NONE);
  la_store32_rel(&stream->owner_tid, 0);
  la_store32_rel(&stream->owner_actor, 0);
  la_store32_rel(&stream->phase, LJ_JIT_STREAM_IDLE);
  la_store32_rel(&stream->traceno, 0);
  la_store32_rel(&stream->callback_event, 0);
  la_store32_rel(&stream->callback_slot, 0);
  la_store64_rel(&stream->callback_session_generation, 0);
  la_store32_rel(&stream->terminal_event, 0);
  la_store32_rel(&stream->terminal_slot, 0);
  la_store64_rel(&stream->terminal_session_generation, 0);
  la_store32_rel(&stream->terminal_reason, 0);
  la_store32_rel(&stream->flags, 0);
  la_store64_rel(&stream->sequence, sequence + 2u);
  assert(lj_jit_trace_stream_idle(G(L)));
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
  publish_gc_callback_stream(L, session, 9u);
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
  clear_gc_callback_stream(L);
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

static void prepare_protected_call(lua_State *L, GCfunc *handler,
                                   ptrdiff_t *oldtop, ptrdiff_t *argbase)
{
  TValue *top;
  lj_state_checkstack(L, LUA_MINSTACK);
  *oldtop = savestack(L, L->top);
  top = L->top;
  setfuncV(L, top, handler);
  lj_state_stack_pubtv(L, L, top++);
  if (LJ_FR2)
    setnilV(top++);
  *argbase = savestack(L, top);
  setnumV(top, 42);
  lj_state_stack_pubtv(L, L, top++);
  L->top = top;
}

static void test_protected_call_case(lua_State *L, jit_State *J, TGState *tg,
                                     GCfunc *handler, uint32_t mode)
{
  global_State *g = G(L);
  LJJitEventSessionHandle session_handle;
  LJJitEventSessionSnapshot session;
  LJJitEventCallbackHandle callback, stale;
  LJJitEventCallbackSnapshot owner_before, owner_after;
  LJJitVMEVENTCallResult bad, result;
  GCfunc *wrong_handler;
  lua_State *oldcur;
  lua_State *oldJL;
  TGState *oldhint;
  ptrdiff_t oldbase, oldtop, prepared_top, argbase;
  uint64_t old_jit_owner;
  uint32_t old_vmevent_owner;
  uint8_t old_global_hookmask;
  int old_had_stopreq;

  assert(mode == PROTECTED_CALL_SUCCESS || mode == PROTECTED_CALL_ERROR);
  memset(&protected_call_ctx, 0, sizeof(protected_call_ctx));
  protected_call_ctx.tg = tg;
  protected_call_ctx.mode = mode;
  lua_pushcfunction(L, callback_handler);
  wrong_handler = funcV(L->top - 1);
  assert(wrong_handler != handler);
  publish_callback_session(L, J, handler, 400u + mode, &session_handle);
  prepare_protected_call(L, handler, &oldtop, &argbase);
  prepared_top = savestack(L, L->top);
  oldbase = savestack(L, L->base);
  oldcur = lj_tg_load_cur_L(tg);
  oldhint = L->tg_hint;
  old_jit_owner = jit_owner_word_acq(g);
  oldJL = jit_owner_l_acq(J);
  old_vmevent_owner = vmevent_owner_acq(g);
  old_global_hookmask = hookmask_load(g);
  old_had_stopreq = lj_safepoint_had_stopreq(L);

  acquire_callback_session(L, tg, &session);
  publish_gc_callback_stream(L, &session, 4000u + mode);
  assert(lj_jit_event_callback_claim_l(
    L, 4000u + mode, &session, &callback));
  assert(lj_jit_event_session_snapshot_release(&session));
  assert(lj_jit_event_callback_snapshot(tg, &owner_before) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE);

  /* Bad geometry and stale authority are bounded refusals. They must not
  ** consume or advance the already-CALLING owner. */
  memset(&bad, 0xa5, sizeof(bad));
  assert(!lj_jit_vmevent_call_l(
    L, argbase + (ptrdiff_t)sizeof(TValue), oldtop,
    &callback, &bad));
  assert(bad.status == 0 && bad.actions == 0 && bad.had_stopreq == 0);
  assert(lj_jit_event_callback_snapshot(tg, &owner_after) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE);
  assert(owner_after.sequence == owner_before.sequence);
  assert(owner_after.generation == owner_before.generation);
  assert(owner_after.state == LJ_JIT_EVENT_CALLBACK_CALLING);
  assert(savestack(L, L->top) == prepared_top);

  /* Equal-distance but byte-misaligned offsets must be rejected before any
  ** TValue load. */
  memset(&bad, 0xa5, sizeof(bad));
  assert(!lj_jit_vmevent_call_l(
    L, argbase + 1, oldtop + 1, &callback, &bad));
  assert(bad.status == 0 && bad.actions == 0 && bad.had_stopreq == 0);
  assert(lj_jit_event_callback_snapshot(tg, &owner_after) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE);
  assert(owner_after.sequence == owner_before.sequence);

  /* Stack shape alone is not callback authority. Even another valid function
  ** must be refused unless it is the exact function rooted by this session. */
  setfuncV(L, restorestack(L, oldtop), wrong_handler);
  memset(&bad, 0xa5, sizeof(bad));
  assert(!lj_jit_vmevent_call_l(L, argbase, oldtop, &callback, &bad));
  assert(bad.status == 0 && bad.actions == 0 && bad.had_stopreq == 0);
  assert(lj_jit_event_callback_snapshot(tg, &owner_after) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE);
  assert(owner_after.sequence == owner_before.sequence);
  setfuncV(L, restorestack(L, oldtop), handler);

  stale = callback;
  stale.generation--;
  memset(&bad, 0xa5, sizeof(bad));
  assert(!lj_jit_vmevent_call_l(L, argbase, oldtop, &stale, &bad));
  assert(bad.status == 0 && bad.actions == 0 && bad.had_stopreq == 0);
  assert(lj_jit_event_callback_snapshot(tg, &owner_after) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE);
  assert(owner_after.sequence == owner_before.sequence);
  assert(owner_after.generation == owner_before.generation);
  assert(owner_after.state == LJ_JIT_EVENT_CALLBACK_CALLING);

  lj_jit_vmevent_call_test_set_hook(
    protected_call_restore_hook, &protected_call_ctx);
  memset(&result, 0xa5, sizeof(result));
  assert(lj_jit_vmevent_call_l(L, argbase, oldtop, &callback, &result));
  assert(protected_call_ctx.calls == 1u);
  assert(protected_call_ctx.restore_hooks == 1u);
  if (mode == PROTECTED_CALL_SUCCESS)
    assert(result.status == LUA_OK);
  else
    assert(result.status != LUA_OK);
  assert(result.had_stopreq == old_had_stopreq);
  assert(savestack(L, L->base) == oldbase);
  assert(savestack(L, L->top) == oldtop);
  assert(lj_tg_load_cur_L(tg) == oldcur);
  assert(L->tg_hint == oldhint);
  assert(jit_owner_word_acq(g) == old_jit_owner);
  assert(jit_owner_l_acq(J) == oldJL);
  assert(vmevent_owner_acq(g) == old_vmevent_owner);
  assert(hookmask_load(g) == old_global_hookmask);
  assert((lj_tg_hookmask_load(tg) &
          (HOOK_ACTIVE|HOOK_VMEVENT)) == 0);
  expect_owner_idle(tg, callback.generation);
  clear_gc_callback_stream(L);
  assert(lj_jit_event_session_end_l(L, J, &session_handle));
  lua_pop(L, 1);  /* wrong_handler root */
}

static void test_protected_callback_calls(lua_State *L, jit_State *J,
                                          TGState *tg)
{
  GCfunc *handler;
  lua_pushcfunction(L, protected_call_handler);
  handler = funcV(L->top - 1);
  assert(handler && handler->c.f == protected_call_handler);
  test_protected_call_case(L, J, tg, handler, PROTECTED_CALL_SUCCESS);
  test_protected_call_case(L, J, tg, handler, PROTECTED_CALL_ERROR);
  lua_pop(L, 1);
}

#if LJ_PROFILE_TGLOCAL
static void profile_callback_claim(void *data, lua_State *L, int samples,
                                   int vmstate)
{
  ProfileCallbackCtx *ctx = (ProfileCallbackCtx *)data;
  LJJitEventSessionSnapshot session;
  LJJitEventCallbackSnapshot owner;
  LJJitEventCallbackHandle rejected;

  UNUSED(vmstate);
  assert(samples > 0);
  assert(L2TG(L) == ctx->tg);
  assert(lj_profile_callback_active_tg(ctx->tg));
  assert(!lj_profile_callback_active_tg(ctx->other_tg));
  /* PROFILE has already handed exclusion to callback_tg. Neither the local
  ** overlay nor the legacy VM-event owner word blocks this claim. */
  assert((lj_tg_hookmask_load(ctx->tg) &
          (HOOK_PROFILE|HOOK_ACTIVE|HOOK_VMEVENT)) == 0);
  assert((hookmask_load(G(L)) & HOOK_VMEVENT) != 0);
  assert(vmevent_owner_acq(G(L)) == 0);
  acquire_callback_session(L, ctx->tg, &session);
  assert(!lj_jit_event_callback_claim_l(L, 3003u, &session, &rejected));
  assert(rejected.tg == NULL && rejected.owner_L == NULL);
  assert(lj_jit_event_callback_snapshot(ctx->tg, &owner) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_IDLE);
  assert(owner.next_generation == ctx->next_generation);
  assert(lj_jit_event_session_snapshot_release(&session));
  ctx->calls++;
}

static void test_profile_callback_exclusion(lua_State *L, jit_State *J,
                                            TGState *tg, GCfunc *handler)
{
  ProfileCallbackCtx ctx;
  static TGState other_tg;
  LJJitEventSessionHandle session_handle;
  LJJitEventSessionSnapshot session;
  LJJitEventCallbackSnapshot before;
  LJJitEventCallbackHandle callback;

  memset(&ctx, 0, sizeof(ctx));
  assert(lj_jit_event_callback_snapshot(tg, &before) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_IDLE);
  ctx.tg = tg;
  ctx.other_tg = &other_tg;
  ctx.next_generation = before.next_generation;

  luaJIT_profile_start(L, "i1000000", profile_callback_claim, &ctx);
  assert(lj_profile_active(L));
  assert(!lj_profile_callback_active_tg(tg));
  publish_callback_session(L, J, handler, 303u, &session_handle);

  assert(raise(SIGPROF) == 0);
  assert(lj_tg_profile_request_acq(tg));
  lj_profile_owner_poll(L);
  assert(!lj_tg_profile_request_acq(tg));
  assert((lj_tg_hookmask_load(tg) & HOOK_PROFILE) != 0);
  lj_profile_interpreter(L);
  assert(ctx.calls == 1u);
  assert(!lj_profile_callback_active_tg(tg));
  assert((lj_tg_hookmask_load(tg) &
          (HOOK_PROFILE|HOOK_ACTIVE|HOOK_VMEVENT)) == 0);

  /* The exact same structural claim is immediately admissible once the
  ** profiler callback has returned. */
  acquire_callback_session(L, tg, &session);
  assert(lj_jit_event_callback_claim_l(L, 3004u, &session, &callback));
  assert(callback.generation == ctx.next_generation + 1u);
  assert(lj_jit_event_session_snapshot_release(&session));
  complete_callback(L, &callback);
  assert(lj_jit_event_session_end_l(L, J, &session_handle));
  luaJIT_profile_stop(L);
  assert(!lj_profile_active(L));
  assert(!lj_profile_callback_active_tg(tg));
}
#endif

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
  test_protected_callback_calls(L, J, tg);
#if LJ_PROFILE_TGLOCAL
  test_profile_callback_exclusion(L, J, tg, handler);
#endif
  lua_pop(L, 1);  /* callback handler */
  lua_close(L);
  puts("t-jit-event-callback-owner OK: per-TG ownership and ABA verified");
  return 0;
}
