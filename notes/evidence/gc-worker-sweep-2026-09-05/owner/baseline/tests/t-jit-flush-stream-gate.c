/*
** Focused universe-global, token-free TRACE FLUSH stream-gate regression.
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
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_state.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"
#include "lj_tgregistry.h"
#include "lj_vmevent.h"

#include "lib/test_sleep.h"

#ifndef LJ_GC2_TEST_HELPERS
#error "t-jit-flush-stream-gate requires LJ_GC2_TEST_HELPERS"
#endif

#define TEST_CONTENDERS 3u
#define TEST_WAIT_ATTEMPTS 5000u
#define TEST_WAIT_NS 1000000L

typedef struct FlushContenderCtx {
  lua_State *L;
  jit_State *J;
  uint64_t attachment_generation;
  uint32_t ready;
  uint32_t go;
  uint32_t done;
  uint32_t token_claimed;
  uint32_t admitted;
} FlushContenderCtx;

typedef struct FlushDetachCtx {
  lua_State *L;
  jit_State *J;
  TGState *tg;
  LJJitTraceStreamHandle handle;
  uint32_t active;
  uint32_t detach_refused;
  uint32_t close_now;
  uint32_t done;
} FlushDetachCtx;

static int flush_callback_handler(lua_State *L)
{
  assert(lua_gettop(L) == 1);
  assert(lua_tonumber(L, 1) == 17);
  return 0;
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

static void claim_for_flush(lua_State *L, jit_State *J)
{
  assert(lj_jit_token_try_l(L, J));
  jit_owner_l_rel(J, L);
}

static void release_failed_claim(lua_State *L, jit_State *J)
{
  lj_jit_token_release_l(L, J);
  assert(jit_owner_word_acq(G(L)) == jit_owner_pack(0, 0));
  assert(jit_owner_l_acq(J) == NULL);
}

static void inject_callback_owner(TGState *tg, lua_State *L,
                                  const LJJitTraceStreamHandle *stream)
{
  LJJitEventCallbackOwner *owner = &tg->jit_event_callback_owner;
  uint64_t sequence = la_load64_acq(&owner->sequence);
  uint64_t generation = la_load64_acq(&owner->next_generation) + 1u;
  assert((sequence & 1u) == 0 && generation != 0);
  la_store64_rel(&owner->sequence, sequence + 1u);
  assert(lj_tg_hookmask_callback_enter_try(tg));
  la_store64_rel(&owner->next_generation, generation);
  la_store64_rel(&owner->generation, generation);
  la_store64_rel(&owner->stream_generation, stream->generation);
  la_store64_rel(&owner->session_generation,
                 stream->terminal_session.generation);
  la_store32_rel(&owner->state, LJ_JIT_EVENT_CALLBACK_CALLING);
  la_store32_rel(&owner->owner_actor, lj_tg_actor_acq(tg));
  la_store32_rel(&owner->event, LJ_JIT_EVENT_TRACE_FLUSH);
  la_store32_rel(&owner->session_slot, stream->terminal_session.slot);
  la_storeptr_rel((void **)&owner->owner_L, L);
  la_store64_rel(&owner->sequence, sequence + 2u);
  assert(!lj_jit_event_callback_idle(tg));
}

static void clear_injected_callback_owner(TGState *tg)
{
  LJJitEventCallbackOwner *owner = &tg->jit_event_callback_owner;
  uint64_t sequence = la_load64_acq(&owner->sequence);
  assert((sequence & 1u) == 0);
  la_store64_rel(&owner->sequence, sequence + 1u);
  assert(lj_tg_hookmask_callback_leave_exact(tg));
  la_store64_rel(&owner->generation, 0);
  la_store64_rel(&owner->stream_generation, 0);
  la_store64_rel(&owner->session_generation, 0);
  la_store32_rel(&owner->state, LJ_JIT_EVENT_CALLBACK_IDLE);
  la_store32_rel(&owner->owner_actor, 0);
  la_store32_rel(&owner->event, 0);
  la_store32_rel(&owner->session_slot, 0);
  la_storeptr_rel((void **)&owner->owner_L, NULL);
  la_store64_rel(&owner->sequence, sequence + 2u);
  assert(lj_jit_event_callback_idle(tg));
}

static int stream_snapshot_wait(global_State *g,
                                LJJitTraceStreamSnapshot *snapshot)
{
  uint32_t attempts;
  int result = LJ_JIT_STREAM_SNAPSHOT_RETRY;
  for (attempts = 0; attempts < TEST_WAIT_ATTEMPTS; attempts++) {
    result = lj_jit_trace_stream_snapshot(g, snapshot);
    if (result != LJ_JIT_STREAM_SNAPSHOT_RETRY)
      break;
    sleep_ns(TEST_WAIT_NS);
  }
  assert(result != LJ_JIT_STREAM_SNAPSHOT_RETRY);
  return result;
}

static int session_snapshot_wait(global_State *g, TGState *tg,
                                 LJJitEventSessionSnapshot *snapshot)
{
  uint32_t attempts;
  int result = LJ_JIT_EVENT_SNAPSHOT_RETRY;
  for (attempts = 0; attempts < TEST_WAIT_ATTEMPTS; attempts++) {
    result = lj_jit_event_session_snapshot_acquire(g, tg, snapshot);
    if (result != LJ_JIT_EVENT_SNAPSHOT_RETRY)
      break;
    sleep_ns(TEST_WAIT_NS);
  }
  assert(result != LJ_JIT_EVENT_SNAPSHOT_RETRY);
  return result;
}

static void expect_stream_idle(global_State *g)
{
  LJJitTraceStreamSnapshot snapshot;
  memset(&snapshot, 0xa5, sizeof(snapshot));
  assert(stream_snapshot_wait(g, &snapshot) == LJ_JIT_STREAM_SNAPSHOT_IDLE);
  assert(snapshot.phase == LJ_JIT_STREAM_IDLE);
  assert(snapshot.generation == 0);
  assert(lj_jit_trace_stream_idle(g));
  assert(g->main_tg != NULL);
  assert(!lj_jit_trace_stream_names_tg(g, g->main_tg));
}

static void expect_active_flush(global_State *g, TGState *owner,
                                const LJJitTraceStreamHandle *handle,
                                LJJitTraceStreamSnapshot *snapshot)
{
  memset(snapshot, 0, sizeof(*snapshot));
  assert(stream_snapshot_wait(g, snapshot) == LJ_JIT_STREAM_SNAPSHOT_ACTIVE);
  assert((snapshot->sequence & 1u) == 0);
  assert(snapshot->generation == handle->generation);
  assert(snapshot->generation != 0);
  assert(snapshot->next_generation == snapshot->generation);
  assert(snapshot->event_ordinal == 1u);
  assert(lj_tgregistry_key_equal(&snapshot->owner_key,
                                 &handle->owner_key));
  assert(lj_tgregistry_key_equal(&snapshot->owner_key,
                                 &owner->registry_key));
  assert(snapshot->owner_tid == handle->owner_tid);
  assert(snapshot->owner_tid == lj_tg_tid_acq(owner));
  assert(snapshot->owner_actor == handle->owner_actor);
  assert(snapshot->owner_actor == lj_tg_actor_acq(owner));
  assert(snapshot->phase == LJ_JIT_STREAM_DETACHED_PENDING);
  assert(snapshot->traceno == 0);
  assert(snapshot->callback_event == 0);
  assert(snapshot->callback_slot == LJ_JIT_EVENT_SESSION_SLOTS);
  assert(snapshot->callback_session_generation == 0);
  assert(snapshot->terminal_event == LJ_JIT_EVENT_TRACE_FLUSH);
  assert(snapshot->terminal_slot == handle->terminal_session.slot);
  assert(snapshot->terminal_session_generation ==
         handle->terminal_session.generation);
  assert(snapshot->terminal_reason == 0);
  assert(snapshot->flags == 0);
  assert(!lj_jit_trace_stream_idle(g));
  assert(lj_jit_trace_stream_names_tg(g, owner));
}

static void expect_empty_flush_session(global_State *g, TGState *owner,
                                       uint64_t attachment_generation,
                                       LJJitEventSessionSnapshot *snapshot)
{
  const LJJitEventSessionSlot *slot;
  memset(snapshot, 0, sizeof(*snapshot));
  assert(session_snapshot_wait(g, owner, snapshot) ==
         LJ_JIT_EVENT_SNAPSHOT_ACTIVE);
  assert(snapshot->event == LJ_JIT_EVENT_TRACE_FLUSH);
  assert(snapshot->owner_mode == LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE);
  assert(snapshot->edge_proof == LJ_JIT_EVENT_EDGE_NONE);
  assert(snapshot->attachment_generation == attachment_generation);
  assert(snapshot->attachment_state == LJ_VMEVENT_ATTACHMENT_PUBLISHED);
  assert(snapshot->callback_root_count == 0);
  assert(snapshot->callback_handler == NULL);
  slot = snapshot->slot;
  assert(slot != NULL);
  assert(la_load32_acq(&slot->state) == LJ_JIT_EVENT_SLOT_ACTIVE);
  assert(la_load32_acq(&slot->readers) != 0);
  assert(la_load32_acq(&slot->flags) == 0);
  assert(la_load32_acq(&slot->root_count) == 0);
  assert(la_load32_acq(&slot->callback_root_count) == 0);
  assert(la_load32_acq(&slot->attachment_state) ==
         LJ_VMEVENT_ATTACHMENT_PUBLISHED);
  {
    GCRef *roots = (GCRef *)la_loadptr_acq(
      (void *const *)&slot->root_data);
    assert(roots != NULL && gcref_acq(roots[0]) == NULL);
  }
  assert(la_loadptr_acq((void *const *)&slot->source) == NULL);
  assert(slot->view.size == 0);
  assert(slot->view.format == LJ_JIT_EVENT_VIEW_FORMAT_NONE);
}

static void expect_callback_flush(
  global_State *g, TGState *owner,
  const LJJitTraceStreamHandle *stream_handle,
  const LJJitEventCallbackHandle *callback_handle)
{
  LJJitTraceStreamSnapshot stream;
  LJJitEventCallbackSnapshot callback;
  memset(&stream, 0, sizeof(stream));
  assert(stream_snapshot_wait(g, &stream) == LJ_JIT_STREAM_SNAPSHOT_ACTIVE);
  assert(stream.generation == stream_handle->generation);
  assert(stream.phase == LJ_JIT_STREAM_DETACHED_CALLBACK);
  assert(stream.callback_event == LJ_JIT_EVENT_TRACE_FLUSH);
  assert(stream.callback_slot == stream_handle->terminal_session.slot);
  assert(stream.callback_session_generation ==
         stream_handle->terminal_session.generation);
  assert(stream.terminal_event == LJ_JIT_EVENT_TRACE_FLUSH);
  assert(stream.terminal_slot == stream.callback_slot);
  assert(stream.terminal_session_generation ==
         stream.callback_session_generation);
  assert(lj_tgregistry_key_equal(&stream.owner_key,
                                 &stream_handle->owner_key));
  assert(lj_tgregistry_key_equal(&stream.owner_key,
                                 &owner->registry_key));

  assert(lj_jit_event_callback_snapshot(owner, &callback) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE);
  assert(callback.state == LJ_JIT_EVENT_CALLBACK_CALLING);
  assert(callback.generation == callback_handle->generation);
  assert(callback.stream_generation == stream.generation);
  assert(callback.stream_generation == callback_handle->stream_generation);
  assert(callback.session_generation ==
         stream.callback_session_generation);
  assert(callback.session_generation ==
         callback_handle->session_generation);
  assert(callback.session_slot == stream.callback_slot);
  assert(callback.event == stream.callback_event);
  assert(callback.owner_actor == stream.owner_actor);
  assert(callback.owner_L == callback_handle->owner_L);
}

static void expect_callback_flush_session(
  global_State *g, TGState *owner,
  const LJJitTraceStreamHandle *stream_handle,
  LJJitEventSessionSnapshot *snapshot)
{
  const LJJitEventSessionSlot *slot;
  GCRef *roots;
  memset(snapshot, 0, sizeof(*snapshot));
  assert(session_snapshot_wait(g, owner, snapshot) ==
         LJ_JIT_EVENT_SNAPSHOT_ACTIVE);
  assert(snapshot->generation ==
         stream_handle->terminal_session.generation);
  assert(snapshot->slot_index == stream_handle->terminal_session.slot);
  assert(snapshot->event == LJ_JIT_EVENT_TRACE_FLUSH);
  assert(snapshot->owner_mode == LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE);
  assert(snapshot->edge_proof == LJ_JIT_EVENT_EDGE_NONE);
  assert(snapshot->attachment_state == stream_handle->attachment_state);
  assert(snapshot->attachment_generation ==
         stream_handle->attachment_generation);
  assert(snapshot->callback_root_count == 1u);
  assert(snapshot->callback_handler == stream_handle->callback_handler);
  slot = snapshot->slot;
  assert(slot != NULL);
  assert(la_load32_acq(&slot->flags) ==
         LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT);
  assert(la_load32_acq(&slot->root_count) == 0);
  assert(la_load32_acq(&slot->callback_root_count) == 1u);
  roots = (GCRef *)la_loadptr_acq((void *const *)&slot->root_data);
  assert(roots != NULL &&
         gcref_acq(roots[0]) == obj2gco(stream_handle->callback_handler));
}

static void expect_same_active_generation(global_State *g,
                                          uint64_t generation)
{
  LJJitTraceStreamSnapshot snapshot;
  assert(stream_snapshot_wait(g, &snapshot) == LJ_JIT_STREAM_SNAPSHOT_ACTIVE);
  assert(snapshot.generation == generation);
  assert(snapshot.phase == LJ_JIT_STREAM_DETACHED_PENDING);
}

static void expect_stale_closes_refused(lua_State *L, jit_State *J,
                                        const LJJitTraceStreamHandle *live)
{
  LJJitTraceStreamHandle stale = *live;

  stale.generation++;
  assert(stale.generation != 0);
  assert(!lj_jit_trace_flush_close_l(L, J, &stale));
  expect_same_active_generation(G(L), live->generation);

  stale = *live;
  stale.attachment_generation++;
  assert(stale.attachment_generation != 0);
  assert(!lj_jit_trace_flush_close_l(L, J, &stale));
  expect_same_active_generation(G(L), live->generation);

  stale = *live;
  stale.owner_key.slot = NULL;
  assert(!lj_jit_trace_flush_close_l(L, J, &stale));
  expect_same_active_generation(G(L), live->generation);

  stale = *live;
  stale.owner_tid = stale.owner_tid == UINT32_MAX ? stale.owner_tid - 1u :
    stale.owner_tid + 1u;
  assert(!lj_jit_trace_flush_close_l(L, J, &stale));
  expect_same_active_generation(G(L), live->generation);

  stale = *live;
  stale.owner_actor = stale.owner_actor == UINT32_MAX - 1u ?
    stale.owner_actor - 1u : stale.owner_actor + 1u;
  assert(stale.owner_actor != 0 && stale.owner_actor != LJ_THR_ACTOR_RETIRED);
  assert(!lj_jit_trace_flush_close_l(L, J, &stale));
  expect_same_active_generation(G(L), live->generation);

  stale = *live;
  stale.terminal_session.generation++;
  assert(stale.terminal_session.generation != 0);
  assert(!lj_jit_trace_flush_close_l(L, J, &stale));
  expect_same_active_generation(G(L), live->generation);

  stale = *live;
  stale.terminal_session.slot =
    (stale.terminal_session.slot + 1u) % LJ_JIT_EVENT_SESSION_SLOTS;
  assert(stale.terminal_session.slot != live->terminal_session.slot);
  assert(!lj_jit_trace_flush_close_l(L, J, &stale));
  expect_same_active_generation(G(L), live->generation);

  stale = *live;
  stale.terminal_session.owner_mode =
    LJ_JIT_EVENT_OWNER_CONTINUATION_LIFECYCLE;
  assert(!lj_jit_trace_flush_close_l(L, J, &stale));
  expect_same_active_generation(G(L), live->generation);
}

static void test_reader_retry_and_generation_saturation(lua_State *L,
                                                        jit_State *J)
{
  global_State *g = G(L);
  LJJitTraceStream *stream = &g->main_tg->jit_trace_stream;
  LJJitEventSessions *sessions = &g->main_tg->jit_event_sessions;
  LJJitTraceStreamSnapshot snapshot;
  LJJitTraceStreamHandle rejected;
  LJJitEventSessionSpec spec;
  LJJitEventSessionHandle event_handle;
  LJJitEventSessionSnapshot event_snapshot;
  uint64_t sequence, next_generation, session_sequence;
  uint64_t session_next_generation;

  assert(stream_snapshot_wait(g, &snapshot) == LJ_JIT_STREAM_SNAPSHOT_IDLE);
  sequence = snapshot.sequence;
  next_generation = snapshot.next_generation;
  assert((sequence & 1u) == 0 && sequence <= UINT64_MAX - 2u);

  /* The helper build may expose a publication interval directly. Readers and
  ** the admission predicate must refuse the odd snapshot without spinning. */
  la_store64_rel(&stream->sequence, sequence + 1u);
  assert(lj_jit_trace_stream_snapshot(g, &snapshot) ==
         LJ_JIT_STREAM_SNAPSHOT_RETRY);
  assert(!lj_jit_trace_stream_idle(g));
  assert(lj_jit_trace_stream_names_tg(g, g->main_tg));
  la_store64_rel(&stream->sequence, sequence);
  expect_stream_idle(g);

  /* Even sequence alone is insufficient for IDLE: all current identity and
  ** phase metadata must be the canonical empty tuple. A torn/stale-looking
  ** stable value is conservative retry, never false admission authority. */
  la_store64_rel(&stream->generation, 1u);
  assert(lj_jit_trace_stream_snapshot(g, &snapshot) ==
         LJ_JIT_STREAM_SNAPSHOT_RETRY);
  assert(!lj_jit_trace_stream_idle(g));
  la_store64_rel(&stream->generation, 0);
  expect_stream_idle(g);

  /* Registry identity is an exact two-field tuple, not merely key_valid().
  ** Either half by itself is malformed stable IDLE metadata and must fail
  ** closed for both stream admission and TG detach. */
  assert(lj_tgregistry_key_valid(&g->main_tg->registry_key));
  la_storeptr_rel((void **)&stream->owner_key.slot,
                  g->main_tg->registry_key.slot);
  assert(lj_jit_trace_stream_snapshot(g, &snapshot) ==
         LJ_JIT_STREAM_SNAPSHOT_RETRY);
  assert(!lj_jit_trace_stream_idle(g));
  assert(lj_jit_trace_stream_names_tg(g, g->main_tg));
  la_storeptr_rel((void **)&stream->owner_key.slot, NULL);

  la_store64_rel(&stream->owner_key.incarnation,
                 g->main_tg->registry_key.incarnation);
  assert(lj_jit_trace_stream_snapshot(g, &snapshot) ==
         LJ_JIT_STREAM_SNAPSHOT_RETRY);
  assert(!lj_jit_trace_stream_idle(g));
  assert(lj_jit_trace_stream_names_tg(g, g->main_tg));
  la_store64_rel(&stream->owner_key.incarnation,
                 LJ_TGSLOT_INCARNATION_NONE);
  expect_stream_idle(g);

  /* The event-session selector has one canonical stable IDLE tuple too. */
  la_store32_rel(&sessions->active_slot, 0);
  assert(lj_jit_event_session_snapshot_acquire(
           g, g->main_tg, &event_snapshot) == LJ_JIT_EVENT_SNAPSHOT_RETRY);
  la_store32_rel(&sessions->active_slot, LJ_JIT_EVENT_SESSION_SLOTS);
  la_store64_rel(&sessions->active_generation, 1u);
  assert(lj_jit_event_session_snapshot_acquire(
           g, g->main_tg, &event_snapshot) == LJ_JIT_EVENT_SNAPSHOT_RETRY);
  la_store64_rel(&sessions->active_generation, 0);
  assert(lj_jit_event_sessions_quiescent(g->main_tg));

  memset(&spec, 0, sizeof(spec));
  spec.event = LJ_JIT_EVENT_TRACE_FLUSH;
  spec.owner_mode = LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE;
  spec.edge_proof = LJ_JIT_EVENT_EDGE_NONE;
  spec.attachment_generation = 54u;
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_PUBLISHED;
  session_sequence = la_load64_acq(&sessions->sequence);
  session_next_generation = la_load64_acq(&sessions->next_generation);
  assert((session_sequence & 1u) == 0);

  /* Event-session admission needs two complete +2 transitions: publish and
  ** eventual close. UINT64_MAX-3 is even, but cannot reserve that +4 lifetime
  ** and must be rejected before the detached begin releases the low token. */
  claim_for_flush(L, J);
  la_store64_rel(&sessions->sequence, UINT64_MAX - 3u);
  memset(&event_handle, 0xa5, sizeof(event_handle));
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &event_handle));
  assert(event_handle.generation == 0);
  assert(jit_owner_word_acq(g) ==
         jit_owner_pack(lj_tg_tid_acq(g->main_tg), 0));
  assert(jit_owner_l_acq(J) == L);
  la_store64_rel(&sessions->sequence, session_sequence);

  la_store64_rel(&sessions->next_generation, UINT64_MAX);
  memset(&event_handle, 0xa5, sizeof(event_handle));
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &event_handle));
  assert(event_handle.generation == 0);
  assert(jit_owner_word_acq(g) ==
         jit_owner_pack(lj_tg_tid_acq(g->main_tg), 0));
  assert(jit_owner_l_acq(J) == L);
  la_store64_rel(&sessions->next_generation, session_next_generation);
  release_failed_claim(L, J);
  assert(lj_jit_event_sessions_quiescent(g->main_tg));

  /* Close independently reserves its final +2 interval. An injected active
  ** UINT64_MAX-1 sequence is refused without losing the exact session; after
  ** restoring its real stable sequence, ordinary detached close succeeds. */
  claim_for_flush(L, J);
  assert(lj_jit_event_session_begin_l(L, J, &spec, &event_handle));
  session_sequence = la_load64_acq(&sessions->sequence);
  assert((session_sequence & 1u) == 0);
  session_next_generation = la_load64_acq(&sessions->next_generation);
  assert(session_next_generation == event_handle.generation);
  la_store64_rel(&sessions->next_generation,
                 event_handle.generation + 1u);
  assert(lj_jit_event_session_snapshot_acquire(
           g, g->main_tg, &event_snapshot) == LJ_JIT_EVENT_SNAPSHOT_RETRY);
  assert(!lj_jit_event_session_end_l(L, J, &event_handle));
  la_store64_rel(&sessions->next_generation, session_next_generation);
  la_store64_rel(&sessions->sequence, UINT64_MAX - 1u);
  assert(!lj_jit_event_session_end_l(L, J, &event_handle));
  la_store64_rel(&sessions->sequence, session_sequence);
  assert(lj_jit_event_session_end_l(L, J, &event_handle));
  assert(lj_jit_event_sessions_quiescent(g->main_tg));

  /* The universe stream has the same paired-transition headroom contract.
  ** Refusal happens before an empty session is published or the low token is
  ** released. */
  claim_for_flush(L, J);
  la_store64_rel(&stream->sequence, UINT64_MAX - 3u);
  memset(&rejected, 0xa5, sizeof(rejected));
  assert(!lj_jit_trace_flush_admit_l(L, J, 55u, &rejected));
  assert(rejected.generation == 0);
  assert(jit_owner_word_acq(g) ==
         jit_owner_pack(lj_tg_tid_acq(g->main_tg), 0));
  assert(jit_owner_l_acq(J) == L);
  assert(lj_jit_event_sessions_quiescent(g->main_tg));
  la_store64_rel(&stream->sequence, sequence);
  release_failed_claim(L, J);
  expect_stream_idle(g);

  /* Saturation is terminal refusal, never wrap-to-zero ABA. It is detected
  ** before the empty session is published and preserves the caller's exact
  ** low token so ordinary failure cleanup remains possible. */
  la_store64_rel(&stream->next_generation, UINT64_MAX);
  claim_for_flush(L, J);
  memset(&rejected, 0xa5, sizeof(rejected));
  assert(!lj_jit_trace_flush_admit_l(L, J, 55u, &rejected));
  assert(rejected.generation == 0);
  assert(jit_owner_word_acq(g) ==
         jit_owner_pack(lj_tg_tid_acq(g->main_tg), 0));
  assert(jit_owner_l_acq(J) == L);
  assert(lj_jit_event_sessions_quiescent(g->main_tg));
  la_store64_rel(&stream->next_generation, next_generation);
  release_failed_claim(L, J);
  expect_stream_idle(g);
}

static void expect_malformed_active_retry(global_State *g, TGState *owner)
{
  LJJitTraceStreamSnapshot snapshot;
  assert(lj_jit_trace_stream_snapshot(g, &snapshot) ==
         LJ_JIT_STREAM_SNAPSHOT_RETRY);
  assert(!lj_jit_trace_stream_idle(g));
  assert(lj_jit_trace_stream_names_tg(g, owner));
}

static void test_active_shape_fail_closed(
  lua_State *L, jit_State *J, TGState *owner,
  const LJJitTraceStreamHandle *live)
{
  global_State *g = G(L);
  LJJitTraceStream *stream = &g->main_tg->jit_trace_stream;
  LJJitEventSessionSlot *slot =
    &owner->jit_event_sessions.slot[live->terminal_session.slot];
  GCRef *roots = (GCRef *)la_loadptr_acq(
    (void *const *)&slot->root_data);
  uint32_t tid = la_load32_acq(&stream->owner_tid);
  uint32_t actor = la_load32_acq(&stream->owner_actor);
  uint32_t slot_flags = la_load32_acq(&slot->flags);
  uint32_t peer_tid = tid == UINT32_MAX ? tid - 1u : tid + 1u;
  uint32_t peer_actor = actor == LJ_THR_ACTOR_RETIRED - 1u ? actor - 1u :
    actor + 1u;

  assert(peer_tid != 0 && peer_tid != tid);
  assert(peer_actor != 0 && peer_actor != actor &&
         peer_actor != LJ_THR_ACTOR_RETIRED);
  assert(roots != NULL && la_load32_acq(&slot->root_count) == 0);

  /* The stream descriptor is insufficient close authority: its exact
  ** terminal session must retain canonical attachment identity and an empty
  ** structural callback lane throughout this pre-delivery slice. */
  la_store32_rel(&slot->attachment_state, LJ_VMEVENT_ATTACHMENT_INITIAL);
  assert(!lj_jit_trace_flush_close_l(L, J, live));
  expect_same_active_generation(g, live->generation);
  la_store32_rel(&slot->attachment_state, LJ_VMEVENT_ATTACHMENT_PUBLISHED);

  la_store64_rel(&slot->attachment_generation, 0);
  assert(!lj_jit_trace_flush_close_l(L, J, live));
  expect_same_active_generation(g, live->generation);
  la_store64_rel(&slot->attachment_generation, live->attachment_generation);

  la_store32_rel(&slot->callback_root_count, 1u);
  assert(!lj_jit_trace_flush_close_l(L, J, live));
  expect_same_active_generation(g, live->generation);
  la_store32_rel(&slot->callback_root_count, 0);

  la_store32_rel(&slot->flags,
		 slot_flags | LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT);
  assert(!lj_jit_trace_flush_close_l(L, J, live));
  expect_same_active_generation(g, live->generation);
  la_store32_rel(&slot->flags, slot_flags);

  setgcrefrel(roots[0], obj2gco(L));
  assert(!lj_jit_trace_flush_close_l(L, J, live));
  expect_same_active_generation(g, live->generation);
  setgcrefrel(roots[0], NULL);

  /* Exact registry identity still names the owner, so a torn tid/actor must
  ** never turn names_tg(owner) into false detach authority. A snapshot may
  ** conservatively retry or expose a non-IDLE value, but it cannot be IDLE. */
  la_store32_rel(&stream->owner_tid, peer_tid);
  assert(!lj_jit_trace_stream_idle(g));
  assert(lj_jit_trace_stream_names_tg(g, owner));
  la_store32_rel(&stream->owner_tid, tid);
  expect_same_active_generation(g, live->generation);

  la_store32_rel(&stream->owner_actor, peer_actor);
  assert(!lj_jit_trace_stream_idle(g));
  assert(lj_jit_trace_stream_names_tg(g, owner));
  la_store32_rel(&stream->owner_actor, actor);
  expect_same_active_generation(g, live->generation);

  la_store64_rel(&stream->next_generation, live->generation + 1u);
  expect_malformed_active_retry(g, owner);
  assert(!lj_jit_trace_flush_close_l(L, J, live));
  la_store64_rel(&stream->next_generation, live->generation);

  /* DETACHED_PENDING has an exact phase shape in this structural slice. */
  la_store32_rel(&stream->terminal_event, 0);
  expect_malformed_active_retry(g, owner);
  la_store32_rel(&stream->terminal_event, LJ_JIT_EVENT_TRACE_FLUSH);

  la_store64_rel(&stream->terminal_session_generation, 0);
  expect_malformed_active_retry(g, owner);
  la_store64_rel(&stream->terminal_session_generation,
                 live->terminal_session.generation);

  la_store32_rel(&stream->callback_event, LJ_JIT_EVENT_TRACE_FLUSH);
  expect_malformed_active_retry(g, owner);
  la_store32_rel(&stream->callback_event, 0);

  la_store32_rel(&stream->phase, LJ_JIT_STREAM_OPEN);
  expect_malformed_active_retry(g, owner);
  la_store32_rel(&stream->phase, LJ_JIT_STREAM_DETACHED_PENDING);

  la_store64_rel(&stream->owner_key.incarnation,
                 LJ_TGSLOT_INCARNATION_NONE);
  expect_malformed_active_retry(g, owner);
  la_store64_rel(&stream->owner_key.incarnation,
                 live->owner_key.incarnation);
  expect_same_active_generation(g, live->generation);
}

static void expect_stream_close_headroom(lua_State *L, jit_State *J,
                                         const LJJitTraceStreamHandle *live)
{
  global_State *g = G(L);
  LJJitTraceStream *stream = &g->main_tg->jit_trace_stream;
  uint64_t sequence = la_load64_acq(&stream->sequence);
  assert((sequence & 1u) == 0);
  la_store64_rel(&stream->sequence, UINT64_MAX - 1u);
  assert(!lj_jit_trace_flush_close_l(L, J, live));
  la_store64_rel(&stream->sequence, sequence);
  expect_same_active_generation(g, live->generation);
}

static void *flush_contender_worker(void *arg)
{
  FlushContenderCtx *ctx = (FlushContenderCtx *)arg;
  LJJitTraceStreamHandle handle;
  TGState *tg;

  assert(lj_threading_attach(ctx->L));
  tg = lj_thr_get_tg();
  assert(tg != NULL && tg != G(ctx->L)->main_tg);
  la_store32_rel(&ctx->ready, 1);
  wait_flag(&ctx->go);

  /* These hot loops must make bounded interpreted progress while the global
  ** stream gate is non-IDLE. They must not wait for its owner or open a trace.
  */
  assert(luaL_dostring(ctx->L,
    "local s=0; for i=1,4000 do s=s+i end; assert(s==8002000)") == LUA_OK);

  /* The generic token may still be used by readers/control operations. If it
  ** is available, the second exact stream admission is the refusal LP and
  ** must return without disturbing the already-published FLUSH generation. */
  ctx->token_claimed = (uint32_t)lj_jit_token_try_l(ctx->L, ctx->J);
  if (ctx->token_claimed) {
    jit_owner_l_rel(ctx->J, ctx->L);
    memset(&handle, 0, sizeof(handle));
    ctx->admitted = (uint32_t)lj_jit_trace_flush_admit_l(
      ctx->L, ctx->J, ctx->attachment_generation, &handle);
    if (ctx->admitted)
      assert(lj_jit_trace_flush_close_l(ctx->L, ctx->J, &handle));
    else
      release_failed_claim(ctx->L, ctx->J);
  }
  lj_threading_detach(ctx->L, 1);
  assert(lj_thr_get_tg() == NULL);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void test_pending_contention(lua_State *L, jit_State *J,
                                    const LJJitTraceStreamHandle *live)
{
  global_State *g = G(L);
  FlushContenderCtx ctx[TEST_CONTENDERS];
  pthread_t thread[TEST_CONTENDERS];
  uint32_t i, token_claims = 0;
  uint32_t live_before = gc2_n_threads_acq(g);

  assert(live_before == 1u && mt_live_acq(g) == 0);
  assert(lj_tg_reclaim_dead(g) == 0u);
  memset(ctx, 0, sizeof(ctx));
  for (i = 0; i < TEST_CONTENDERS; i++) {
    ctx[i].L = lua_newthread(L);  /* Keep every initiating state rooted. */
    assert(ctx[i].L != NULL);
    ctx[i].J = J;
    ctx[i].attachment_generation = 100u + i;
    assert(pthread_create(&thread[i], NULL, flush_contender_worker,
                          &ctx[i]) == 0);
  }
  for (i = 0; i < TEST_CONTENDERS; i++)
    wait_flag(&ctx[i].ready);
  assert(gc2_n_threads_acq(g) == live_before + TEST_CONTENDERS);
  assert(mt_live_acq(g) == TEST_CONTENDERS);
  for (i = 0; i < TEST_CONTENDERS; i++)
    la_store32_rel(&ctx[i].go, 1);
  for (i = 0; i < TEST_CONTENDERS; i++)
    wait_flag(&ctx[i].done);

  /* The owner deliberately does not close until every peer has returned.
  ** Completion itself therefore proves there is no hidden stream wait. */
  expect_same_active_generation(g, live->generation);
  assert(!lj_trace_hasany(g));
  for (i = 0; i < TEST_CONTENDERS; i++) {
    assert(ctx[i].admitted == 0);
    token_claims += ctx[i].token_claimed;
    assert(pthread_join(thread[i], NULL) == 0);
  }
  assert(token_claims != 0);  /* Exercise admission, not only token collision. */
  assert(gc2_n_threads_acq(g) == live_before);
  assert(mt_live_acq(g) == 0);
  while (lj_tg_reclaim_dead(g) != 0)
    ;
  lua_pop(L, TEST_CONTENDERS);
}

static void *flush_detach_worker(void *arg)
{
  FlushDetachCtx *ctx = (FlushDetachCtx *)arg;
  TGState *tg;
  uint32_t actor;

  assert(lj_threading_attach(ctx->L));
  tg = lj_thr_get_tg();
  assert(tg != NULL && tg != G(ctx->L)->main_tg);
  actor = lj_tg_actor_acq(tg);
  assert(actor != 0 && actor != LJ_THR_ACTOR_RETIRED);
  claim_for_flush(ctx->L, ctx->J);
  assert(lj_jit_trace_flush_admit_l(ctx->L, ctx->J, 33u, &ctx->handle));
  assert(jit_owner_word_acq(G(ctx->L)) == jit_owner_pack(0, 0));
  ctx->tg = tg;
  la_store32_rel(&ctx->active, 1);

  /* Neither physical actor handoff nor public detach may orphan a stream
  ** which still names this exact key/tid/actor/session generation. */
  assert(!lj_thr_tg_handoff_current(tg));
  assert(lj_thr_get_tg() == tg && lj_tg_actor_acq(tg) == actor);
  lj_threading_detach(ctx->L, 1);
  assert(lj_thr_get_tg() == tg && !lj_tg_flags_test_acq(tg, TGF_DEAD));
  la_store32_rel(&ctx->detach_refused, 1);
  wait_flag(&ctx->close_now);

  assert(lj_jit_trace_flush_close_l(ctx->L, ctx->J, &ctx->handle));
  lj_threading_detach(ctx->L, 1);
  assert(lj_thr_get_tg() == NULL);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void test_owner_detach_gate(lua_State *L, jit_State *J)
{
  global_State *g = G(L);
  FlushDetachCtx ctx;
  LJJitTraceStreamSnapshot stream;
  LJJitEventSessionSnapshot held;
  pthread_t worker;

  memset(&ctx, 0, sizeof(ctx));
  ctx.L = lua_newthread(L);
  assert(ctx.L != NULL);
  ctx.J = J;
  assert(pthread_create(&worker, NULL, flush_detach_worker, &ctx) == 0);
  wait_flag(&ctx.active);
  wait_flag(&ctx.detach_refused);
  assert(ctx.tg != NULL);
  expect_active_flush(g, ctx.tg, &ctx.handle, &stream);
  expect_empty_flush_session(g, ctx.tg, 33u, &held);

  /* Retain a reader across close and logical detach. It keeps the exact TG
  ** body in SMR while the owner publishes DEAD; physical cleanup waits for
  ** the last reader, but the universe stream itself may return to IDLE. */
  la_store32_rel(&ctx.close_now, 1);
  wait_flag(&ctx.done);
  assert(pthread_join(worker, NULL) == 0);
  expect_stream_idle(g);
  assert(lj_tg_flags_test_acq(ctx.tg, TGF_DEAD));
  assert(lj_tg_actor_acq(ctx.tg) == LJ_THR_ACTOR_RETIRED);
  assert(!lj_jit_trace_flush_close_l(L, J, &ctx.handle));
  assert(lj_tg_reclaim_dead(g) == 0u);  /* Held session SMR pins this TG. */
  {
    const LJJitEventSessionSlot *slot = held.slot;
    GCRef *roots = (GCRef *)la_loadptr_acq(
      (void *const *)&slot->root_data);
    assert(la_load32_acq(&slot->state) == LJ_JIT_EVENT_SLOT_CLOSED);
    assert(la_load32_acq(&slot->attachment_state) ==
	   LJ_VMEVENT_ATTACHMENT_PUBLISHED);
    assert(la_load32_acq(&slot->callback_root_count) == 0);
    assert((la_load32_acq(&slot->flags) &
	    LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT) == 0);
    assert(roots != NULL && gcref_acq(roots[0]) == NULL);
  }
  assert(!lj_jit_event_session_snapshot_release(&held));
  /* Public detach performed the terminal allocation-free SSB flush. With the
  ** final reader gone, exactly this one heap TG is now reclaimable. */
  assert(lj_tg_reclaim_dead(g) == 1u);
  lua_pop(L, 1);
}

static void test_callback_admission(lua_State *L, jit_State *J, TGState *tg)
{
  global_State *g = G(L);
  LJJitTraceStream *descriptor = &g->main_tg->jit_trace_stream;
  LJJitTraceStreamHandle stream;
  LJJitEventCallbackHandle callback, stale_callback;
  LJJitEventSessionSnapshot session;
  LJJitTraceStreamSnapshot stream_snapshot;
  LJJitEventCallbackSnapshot owner_snapshot;
  LJJitVMEVENTCallResult call_result;
  GCfunc *handler;
  TValue *top;
  ptrdiff_t argbase, oldtop;
  uint64_t sequence;

  lua_pushcfunction(L, flush_callback_handler);
  handler = funcV(L->top-1);
  assert(handler != NULL);

  /* Production admission reserves publish, callback-phase and close
  ** transitions before it roots a session. UINT64_MAX-5 is even but cannot
  ** provide all three +2 publications without wrapping. */
  sequence = la_load64_acq(&descriptor->sequence);
  assert((sequence & 1u) == 0);
  claim_for_flush(L, J);
  la_store64_rel(&descriptor->sequence, UINT64_MAX - 5u);
  memset(&stream, 0xa5, sizeof(stream));
  memset(&callback, 0xa5, sizeof(callback));
  assert(!lj_jit_trace_flush_callback_admit_l(
    L, J, LJ_VMEVENT_ATTACHMENT_PUBLISHED, 70u, handler,
    &stream, &callback));
  assert(stream.generation == 0 && callback.generation == 0);
  assert(jit_owner_word_acq(g) == jit_owner_pack(lj_tg_tid_acq(tg), 0));
  assert(jit_owner_l_acq(J) == L);
  assert(lj_jit_event_sessions_quiescent(tg));
  la_store64_rel(&descriptor->sequence, sequence);
  release_failed_claim(L, J);
  expect_stream_idle(g);

  /* A same-TG local exclusion collision occurs only after the rooted pending
  ** session/stream exist. It must release its temporary snapshot and roll
  ** both publications back while retaining the original low token. */
  claim_for_flush(L, J);
  (void)lj_tg_hookmask_update(tg, 0, HOOK_PROFILE);
  memset(&stream, 0xa5, sizeof(stream));
  memset(&callback, 0xa5, sizeof(callback));
  assert(!lj_jit_trace_flush_callback_admit_l(
    L, J, LJ_VMEVENT_ATTACHMENT_PUBLISHED, 71u, handler,
    &stream, &callback));
  assert(stream.generation == 0 && callback.generation == 0);
  assert((lj_tg_hookmask_load(tg) & HOOK_PROFILE) != 0);
  assert((lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT)) == 0);
  assert(jit_owner_word_acq(g) == jit_owner_pack(lj_tg_tid_acq(tg), 0));
  assert(jit_owner_l_acq(J) == L);
  assert(lj_jit_event_sessions_quiescent(tg));
  (void)lj_tg_hookmask_update(tg, HOOK_PROFILE, 0);
  release_failed_claim(L, J);
  expect_stream_idle(g);

  /* Handoff failure occurs after exact callback claim and stream phase
  ** publication. Rollback must unwind the callback owner first, then clear
  ** the stream/session and preserve the still-exact low token. */
  claim_for_flush(L, J);
  lj_trace_test_force_event_handoff_failure(1);
  memset(&stream, 0xa5, sizeof(stream));
  memset(&callback, 0xa5, sizeof(callback));
  assert(!lj_jit_trace_flush_callback_admit_l(
    L, J, LJ_VMEVENT_ATTACHMENT_PUBLISHED, 72u, handler,
    &stream, &callback));
  assert(stream.generation == 0 && callback.generation == 0);
  assert(jit_owner_word_acq(g) == jit_owner_pack(lj_tg_tid_acq(tg), 0));
  assert(jit_owner_l_acq(J) == L);
  assert(lj_jit_event_callback_snapshot(tg, &owner_snapshot) ==
         LJ_JIT_EVENT_CALLBACK_SNAPSHOT_IDLE);
  assert((lj_tg_hookmask_load(tg) & (HOOK_ACTIVE|HOOK_VMEVENT)) == 0);
  assert(lj_jit_event_sessions_quiescent(tg));
  release_failed_claim(L, J);
  expect_stream_idle(g);

  /* Success crosses the low-to-zero edge only after the exact rooted session,
  ** callback owner and DETACHED_CALLBACK stream generation agree. */
  claim_for_flush(L, J);
  memset(&stream, 0, sizeof(stream));
  memset(&callback, 0, sizeof(callback));
  assert(lj_jit_trace_flush_callback_admit_l(
    L, J, LJ_VMEVENT_ATTACHMENT_PUBLISHED, 73u, handler,
    &stream, &callback));
  assert(jit_owner_word_acq(g) == jit_owner_pack(0, 0));
  assert(jit_owner_l_acq(J) == NULL);
  assert(stream.attachment_state == LJ_VMEVENT_ATTACHMENT_PUBLISHED);
  assert(stream.attachment_generation == 73u);
  assert(stream.callback_root_count == 1u);
  assert(stream.callback_handler == handler);
  expect_callback_flush(g, tg, &stream, &callback);
  expect_callback_flush_session(g, tg, &stream, &session);
  assert(lj_jit_event_session_snapshot_release(&session));

  /* The prepared callback phase is independently canonical while its exact
  ** owner remains the close exclusion. The production close cannot silently
  ** consume a callback which has not completed owner unwind. */
  assert(!lj_jit_trace_flush_close_l(L, J, &stream));
  assert(stream_snapshot_wait(g, &stream_snapshot) ==
         LJ_JIT_STREAM_SNAPSHOT_ACTIVE);
  assert(stream_snapshot.phase == LJ_JIT_STREAM_DETACHED_CALLBACK);
  stale_callback = callback;
  stale_callback.stream_generation++;
  assert(stale_callback.stream_generation != 0);
  assert(!lj_jit_event_callback_unwind_l(L, &stale_callback));
  expect_callback_flush(g, tg, &stream, &callback);

  /* Exercise the complete admitted substrate transaction before production
  ** callsites adopt it: exact prepared function -> protected Lua -> owner
  ** release -> token-free stream/session close. */
  lj_state_checkstack(L, LUA_MINSTACK);
  oldtop = savestack(L, L->top);
  top = L->top;
  setfuncV(L, top, handler);
  lj_state_stack_pubtv(L, L, top++);
  if (LJ_FR2)
    setnilV(top++);
  argbase = savestack(L, top);
  setnumV(top, 17);
  lj_state_stack_pubtv(L, L, top++);
  L->top = top;
  memset(&call_result, 0xa5, sizeof(call_result));
  assert(lj_jit_vmevent_call_l(
    L, argbase, oldtop, &callback, &call_result));
  assert(call_result.status == LUA_OK);
  assert(savestack(L, L->top) == oldtop);
  assert(lj_jit_event_callback_idle(tg));
  assert(lj_jit_trace_flush_close_l(L, J, &stream));
  expect_stream_idle(g);

  /* INITIAL/zero is a real exact attachment identity for direct registry
  ** manipulation. The rooted production path must not retain the structural
  ** API's provisional nonzero-only restriction. */
  claim_for_flush(L, J);
  assert(lj_jit_trace_flush_callback_admit_l(
    L, J, LJ_VMEVENT_ATTACHMENT_INITIAL, 0, handler,
    &stream, &callback));
  assert(stream.attachment_state == LJ_VMEVENT_ATTACHMENT_INITIAL);
  assert(stream.attachment_generation == 0);
  expect_callback_flush(g, tg, &stream, &callback);
  assert(lj_jit_event_callback_unwind_l(L, &callback));
  assert(lj_jit_event_callback_release_l(L, &callback));
  assert(lj_jit_trace_flush_close_l(L, J, &stream));
  expect_stream_idle(g);

  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  jit_State *J;
  TGState *tg;
  LJJitTraceStreamHandle first, second, pending;
  LJJitTraceStreamSnapshot stream;
  LJJitEventSessionSnapshot held;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  J = G2J(g);
  tg = L2TG(L);
  assert(tg != NULL && tg == g->main_tg && tg->gl == g);
  assert(luaL_dostring(L,
    "jit.flush(); jit.opt.start('hotloop=1', 'hotexit=1')") == LUA_OK);
  assert(!lj_trace_hasany(g));
  expect_stream_idle(g);

  /* Zero is never a provisional attachment identity. Rejection is wholly
  ** pre-publication and leaves the exact low-token transaction unchanged. */
  claim_for_flush(L, J);
  memset(&pending, 0xa5, sizeof(pending));
  assert(!lj_jit_trace_flush_admit_l(L, J, 0, &pending));
  assert(pending.generation == 0);
  assert(jit_owner_word_acq(g) ==
         jit_owner_pack(lj_tg_tid_acq(tg), 0));
  assert(jit_owner_l_acq(J) == L);
  release_failed_claim(L, J);
  expect_stream_idle(g);

  /* A failure after publishing both immutable layers but before the exact
  ** low-to-zero handoff must roll them both back and preserve the caller's
  ** original token/J->L transaction for ordinary cleanup. */
  claim_for_flush(L, J);
  lj_trace_test_force_event_handoff_failure(1);
  memset(&pending, 0xa5, sizeof(pending));
  assert(!lj_jit_trace_flush_admit_l(L, J, 10u, &pending));
  assert(pending.generation == 0);
  assert(jit_owner_word_acq(g) ==
         jit_owner_pack(lj_tg_tid_acq(tg), 0));
  assert(jit_owner_l_acq(J) == L);
  assert(lj_jit_event_sessions_quiescent(tg));
  expect_stream_idle(g);
  release_failed_claim(L, J);

  test_reader_retry_and_generation_saturation(L, J);
  test_callback_admission(L, J, tg);

  claim_for_flush(L, J);
  memset(&first, 0, sizeof(first));
  assert(lj_jit_trace_flush_admit_l(L, J, 11u, &first));
  assert(jit_owner_word_acq(g) == jit_owner_pack(0, 0));
  assert(jit_owner_l_acq(J) == NULL);
  assert(first.attachment_generation == 11u);
  expect_active_flush(g, tg, &first, &stream);
  expect_empty_flush_session(g, tg, 11u, &held);
  expect_stale_closes_refused(L, J, &first);
  expect_stream_close_headroom(L, J, &first);
  test_active_shape_fail_closed(L, J, tg, &first);

  /* Same-owner close/handoff shortcuts must honor the stream independently
  ** of the now-zero recorder word. */
  {
    uint32_t actor = lj_tg_actor_acq(tg);
    assert(!lj_thr_main_close_claim(L));
    assert(!lj_thr_tg_handoff_current(tg));
    assert(lj_thr_get_tg() == tg && lj_tg_actor_acq(tg) == actor);
  }

  test_pending_contention(L, J, &first);
  inject_callback_owner(tg, L, &first);
  assert(!lj_jit_trace_flush_close_l(L, J, &first));
  expect_active_flush(g, tg, &first, &stream);
  clear_injected_callback_owner(tg);
  assert(lj_jit_trace_flush_close_l(L, J, &first));
  expect_stream_idle(g);

  /* Keep the first session reader paused so the successor must use the other
  ** retained slot. Its distinct attachment and stream generations make the
  ** old handle incapable of clearing the successor (TraceNo is always zero). */
  claim_for_flush(L, J);
  memset(&second, 0, sizeof(second));
  assert(lj_jit_trace_flush_admit_l(L, J, 22u, &second));
  assert(second.generation > first.generation);
  assert(second.terminal_session.generation >
         first.terminal_session.generation);
  assert(second.terminal_session.slot != first.terminal_session.slot);
  assert(!lj_jit_trace_flush_close_l(L, J, &first));
  expect_active_flush(g, tg, &second, &stream);
  {
    LJJitEventSessionSnapshot current;
    expect_empty_flush_session(g, tg, 22u, &current);
    assert(lj_jit_event_session_snapshot_release(&current));
  }
  assert(!lj_jit_event_session_snapshot_release(&held));
  assert(lj_jit_trace_flush_close_l(L, J, &second));
  expect_stream_idle(g);

  /* A final pending generation isolates exact stale-close behavior after all
  ** retained storage has become reusable. */
  claim_for_flush(L, J);
  memset(&pending, 0, sizeof(pending));
  assert(lj_jit_trace_flush_admit_l(L, J, 44u, &pending));
  assert(!lj_jit_trace_flush_close_l(L, J, &second));
  expect_same_active_generation(g, pending.generation);
  assert(lj_jit_trace_flush_close_l(L, J, &pending));
  expect_stream_idle(g);

  test_owner_detach_gate(L, J);
  assert(lj_jit_event_sessions_quiescent(tg));
  expect_stream_idle(g);
  lua_close(L);
  puts("t-jit-flush-stream-gate OK: contention, stale handles, empty payload and detach gates verified");
  return 0;
}
