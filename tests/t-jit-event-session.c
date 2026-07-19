/*
** Focused immutable JIT event-session publication/GC regression.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"
#include "lj_vmevent.h"

#include "lib/test_sleep.h"

#ifndef LJ_GC2_TEST_HELPERS
#error "t-jit-event-session requires LJ_GC2_TEST_HELPERS"
#endif

#define TEST_ROOTS 17u
#define TEST_WAIT_ATTEMPTS 5000u
#define TEST_WAIT_NS 1000000L

static int test_callback_handler(lua_State *L)
{
  (void)L;
  return 0;
}

typedef struct JitEventDetachCtx {
  lua_State *L;
  jit_State *J;
  TGState *tg;
  uint32_t active;
  uint32_t reader_held;
  uint32_t done;
} JitEventDetachCtx;

static void wait_flag(uint32_t *flag)
{
  uint32_t attempts;
  for (attempts = 0;
       la_load32_acq(flag) == 0 && attempts < TEST_WAIT_ATTEMPTS;
       attempts++)
    sleep_ns(TEST_WAIT_NS);
  assert(la_load32_acq(flag) != 0);
}

static uint32_t align_up_u32(uint32_t value, uint32_t alignment)
{
  uint32_t mask = alignment - 1u;
  assert(alignment != 0 && (alignment & mask) == 0);
  assert(value <= ~(uint32_t)0 - mask);
  return (value + mask) & ~mask;
}

static void settle_automatic_cycle(global_State *g)
{
  uint32_t attempts;
  for (attempts = 0;
       gc2_phase_acq(g) != LJ_GC2_IDLE && attempts < 4096u;
       attempts++) {
    (void)lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH);
    lj_gc2_cycle_to_idle(g);
  }
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
}

static void unmark_small(GCobj *o)
{
  GCArena *a = lj_arena_of(o);
  uint32_t cell = lj_arena_cellof(o);
  assert(!lj_arena_ishuge(a));
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  lj_arena_bm_clear(a->mark, cell);
}

static GCtrace *published_trace_find(jit_State *J)
{
  TraceVec *tv = tracevec_acq(J);
  MSize i;
  for (i = 1; tv && i < tv->sizetrace; i++) {
    GCtrace *T = traceref_safe(J, (TraceNo)i);
    if (T && trace_runnable_acq(T, (TraceNo)i))
      return T;
  }
  return NULL;
}

static uint32_t trace_startpc_pos(GCtrace *T)
{
  GCproto *pt = trace_startpt_acq(T);
  const BCIns *pc = trace_startpc_acq(T);
  uintptr_t bc, p;
  assert(pt != NULL && pc != NULL);
  bc = (uintptr_t)proto_bc(pt);
  p = (uintptr_t)pc;
  assert(p >= bc && p < bc + (uintptr_t)pt->sizebc * sizeof(BCIns));
  assert((p - bc) % sizeof(BCIns) == 0);
  return (uint32_t)((p - bc) / sizeof(BCIns));
}

static void frozen_view_build(GCtrace *T, LJJitEventFrozenViewSpec *view,
			      void **storagep)
{
  IRRef nins = trace_nins_acq(T), nk = trace_nk_acq(T);
  uint32_t nir = (uint32_t)(nins - nk);
  uint32_t nsnap = (uint32_t)trace_nsnap_acq(T);
  uint32_t nsnapmap = (uint32_t)trace_nsnapmap_acq(T);
  uint32_t irbytes, snapbytes, snapmapbytes, end, i;
  unsigned char *storage;
  assert(nins >= nk);
  assert((uint64_t)nir * sizeof(IRIns) <= ~(uint32_t)0);
  assert((uint64_t)nsnap * sizeof(SnapShot) <= ~(uint32_t)0);
  assert((uint64_t)nsnapmap * sizeof(SnapEntry) <= ~(uint32_t)0);
  irbytes = nir * (uint32_t)sizeof(IRIns);
  snapbytes = nsnap * (uint32_t)sizeof(SnapShot);
  snapmapbytes = nsnapmap * (uint32_t)sizeof(SnapEntry);
  memset(view, 0, sizeof(*view));
  view->format = LJ_JIT_EVENT_VIEW_FORMAT_TRACE_V1;
  view->ir.offset = 0;
  view->ir.count = nir;
  view->ir.stride = sizeof(IRIns);
  view->snap.offset = align_up_u32(irbytes, __alignof__(SnapShot));
  view->snap.count = nsnap;
  view->snap.stride = sizeof(SnapShot);
  assert(view->snap.offset <= ~(uint32_t)0 - snapbytes);
  view->snapmap.offset = align_up_u32(view->snap.offset + snapbytes,
				      __alignof__(SnapEntry));
  view->snapmap.count = nsnapmap;
  view->snapmap.stride = sizeof(SnapEntry);
  assert(view->snapmap.offset <= ~(uint32_t)0 - snapmapbytes);
  end = view->snapmap.offset + snapmapbytes;
  view->size = end ? end : 1u;
  storage = (unsigned char *)calloc(1, view->size);
  assert(storage != NULL);
  view->data = storage;
  if (nir)
    memcpy(storage + view->ir.offset, trace_ir_acq(T) + nk, irbytes);
  for (i = 0; i < nsnap; i++) {
    SnapShot *frozen =
      (SnapShot *)(void *)(storage + view->snap.offset) + i;
    lj_jit_event_snapshot_copy_canonical(frozen, trace_snap_acq(T) + i);
  }
  if (nsnapmap)
    memcpy(storage + view->snapmap.offset, trace_snapmap_acq(T),
	   snapmapbytes);
  view->trace.version = LJ_JIT_EVENT_FROZEN_TRACE_VERSION;
  view->trace.traceno = trace_traceno_acq(T);
  view->trace.root = trace_root_acq(T);
  view->trace.link = trace_link_acq(T);
  view->trace.linktype = (uint32_t)trace_linktype_acq(T);
  view->trace.nins = nins;
  view->trace.nk = nk;
  view->trace.nsnap = nsnap;
  view->trace.nsnapmap = nsnapmap;
  view->trace.ir_ref_first = nk;
  view->trace.ir_ref_count = nir;
  view->trace.startpc_pos = trace_startpc_pos(T);
  view->trace.startins = trace_startins_acq(T);
  view->trace.mcode_addr = (uint64_t)(uintptr_t)trace_mcode_acq(T);
  view->trace.szmcode = trace_szmcode_acq(T);
  view->trace.mcloop = trace_mcloop_acq(T);
  view->trace.exitstub_addr = (uint64_t)(uintptr_t)trace_exitstub_acq(T);
  view->trace.nexits = nsnap;
  *storagep = storage;
}

static void claim_for_session(lua_State *L, jit_State *J)
{
  assert(lj_jit_token_try_l(L, J));
  jit_owner_l_rel(J, L);
}

static void release_after_session(lua_State *L, jit_State *J)
{
  lj_jit_token_release_l(L, J);
  assert(jit_owner_word_acq(G(L)) == jit_owner_pack(0, 0));
}

static void *secondary_closed_reader_worker(void *arg)
{
  JitEventDetachCtx *ctx = (JitEventDetachCtx *)arg;
  LJJitEventSessionSpec spec;
  LJJitEventSessionHandle handle;
  TGState *tg;

  assert(lj_threading_attach(ctx->L));
  tg = lj_thr_get_tg();
  assert(tg != NULL && tg != G(ctx->L)->main_tg && tg->gl == G(ctx->L));
  assert(lj_tg_flags_test_acq(tg, TGF_HEAP));
  assert(!lj_tg_flags_test_acq(tg, TGF_DEAD));
  assert(lj_trace_state_load(ctx->J) == LJ_TRACE_IDLE);

  claim_for_session(ctx->L, ctx->J);
  memset(&spec, 0, sizeof(spec));
  spec.event = LJ_JIT_EVENT_TRACE_FLUSH;
  spec.owner_mode = LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE;
  spec.edge_proof = LJ_JIT_EVENT_EDGE_NONE;
  spec.attachment_generation = 6;
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_PUBLISHED;
  assert(lj_jit_event_session_begin_l(ctx->L, ctx->J, &spec, &handle));
  assert(handle.slot == 0 && handle.generation == 1u);
  assert(jit_owner_word_acq(tg->gl) == jit_owner_pack(0, 0));
  assert(jit_owner_l_acq(ctx->J) == NULL);

  /* The release flag publishes the raw TG pointer only while mt_live still
  ** prevents physical reclamation. The worker cannot logically detach until
  ** the main thread has converted that handoff into a snapshot SMR lease. */
  ctx->tg = tg;
  la_store32_rel(&ctx->active, 1);
  wait_flag(&ctx->reader_held);

  assert(lj_jit_event_session_end_l(ctx->L, ctx->J, &handle));
  assert(la_load32_acq(&tg->jit_event_sessions.slot[handle.slot].state) ==
         LJ_JIT_EVENT_SLOT_CLOSED);
  assert(la_load32_acq(&tg->jit_event_sessions.slot[handle.slot].readers) ==
         1u);
  assert(lj_jit_event_sessions_logical_detach_ready(tg));
  assert(!lj_jit_event_sessions_quiescent(tg));
  assert(!lj_jit_event_sessions_detach_ready(tg));

  /* CLOSED+reader is logically detachable, but actor handoff is stricter: it
  ** must not strand the retained slot under actor zero. Rejection is wholly
  ** pre-mutation, including the raw TLS binding. */
  {
    uint32_t actor = lj_tg_actor_acq(tg);
    assert(actor != 0 && actor != LJ_THR_ACTOR_RETIRED);
    assert(!lj_thr_tg_handoff_current(tg));
    assert(lj_thr_get_tg() == tg && lj_tg_actor_acq(tg) == actor);
  }

  /* Public detach must accept CLOSED+reader as a logical terminal state. It
  ** may not wait for the foreign reader or reclaim its own TG inline. */
  lj_threading_detach(ctx->L, 1);
  assert(lj_thr_get_tg() == NULL);
  assert(lj_tg_flags_test_acq(tg, TGF_DEAD));
  assert(lj_tg_actor_acq(tg) == LJ_THR_ACTOR_RETIRED);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void test_secondary_closed_reader_detach(lua_State *L, jit_State *J)
{
  global_State *g = G(L);
  JitEventDetachCtx ctx;
  LJJitEventSessionSnapshot held;
  const LJJitEventSessionSlot *closed_slot;
  TGState *secondary_tg;
  lua_State *secondary_L;
  pthread_t worker;
  uint32_t live_tgs = gc2_n_threads_acq(g);
  uint32_t attempts;
  int snapshot_result = LJ_JIT_EVENT_SNAPSHOT_RETRY;

  assert(live_tgs == 1u && mt_live_acq(g) == 0);
  assert(!lj_trace_hasany(g));  /* First MT activation needs no handshake. */
  secondary_L = lua_newthread(L);
  assert(secondary_L != NULL);
  memset(&ctx, 0, sizeof(ctx));
  ctx.L = secondary_L;
  ctx.J = J;
  assert(pthread_create(&worker, NULL, secondary_closed_reader_worker,
                        &ctx) == 0);
  wait_flag(&ctx.active);

  /* active is a release/acquire handoff while the worker remains attached, so
  ** this raw pointer is stable until snapshot acquisition takes its own SMR
  ** lease. RETRY is a legal transient nonwaiting result; IDLE is not. */
  secondary_tg = ctx.tg;
  assert(secondary_tg != NULL);
  assert(gc2_n_threads_acq(g) == live_tgs + 1u);
  assert(mt_live_acq(g) == 1u);
  for (attempts = 0; attempts < TEST_WAIT_ATTEMPTS; attempts++) {
    snapshot_result =
      lj_jit_event_session_snapshot_acquire(g, secondary_tg, &held);
    if (snapshot_result == LJ_JIT_EVENT_SNAPSHOT_ACTIVE)
      break;
    assert(snapshot_result == LJ_JIT_EVENT_SNAPSHOT_RETRY);
    sleep_ns(TEST_WAIT_NS);
  }
  assert(snapshot_result == LJ_JIT_EVENT_SNAPSHOT_ACTIVE);
  assert(held.tg == secondary_tg);
  assert(held.event == LJ_JIT_EVENT_TRACE_FLUSH);
  assert(held.owner_mode == LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE);
  assert(held.edge_proof == LJ_JIT_EVENT_EDGE_NONE);
  assert(held.attachment_generation == 6u);
  assert(held.attachment_state == LJ_VMEVENT_ATTACHMENT_PUBLISHED);
  assert(held.callback_root_count == 0 && held.callback_handler == NULL);
  closed_slot = held.slot;
  assert(closed_slot != NULL);
  assert(la_load32_acq(&closed_slot->state) == LJ_JIT_EVENT_SLOT_ACTIVE);
  assert(la_load32_acq(&closed_slot->readers) == 1u);
  la_store32_rel(&ctx.reader_held, 1);

  /* Wait for the whole public detach before joining, so a missing relaxed gate
  ** fails within the fixture timeout instead of becoming an ambiguous join. */
  wait_flag(&ctx.done);
  assert(pthread_join(worker, NULL) == 0);
  assert(lj_tg_flags_test_acq(secondary_tg, TGF_HEAP));
  assert(lj_tg_flags_test_acq(secondary_tg, TGF_DEAD));
  assert(lj_tg_actor_acq(secondary_tg) == LJ_THR_ACTOR_RETIRED);
  assert(gc2_n_threads_acq(g) == live_tgs);
  assert(mt_live_acq(g) == 0);
  assert(secondary_L->tg_hint == NULL);
  assert(lj_state_owner_acq(secondary_L) == 0);
  assert(la_load32_acq(&closed_slot->state) == LJ_JIT_EVENT_SLOT_CLOSED);
  assert(la_load32_acq(&closed_slot->readers) == 1u);
  assert(lj_jit_event_sessions_logical_detach_ready(secondary_tg));
  assert(!lj_jit_event_sessions_quiescent(secondary_tg));
  assert(!lj_jit_event_sessions_detach_ready(secondary_tg));

  /* A logical DEAD publication does not authorize physical teardown. The
  ** strict subsystem finalizer refuses the reader; the full TG finalizer
  ** serializes that refusal through BUSY and publishes retryable RETRY. The
  ** snapshot's SMR lease independently vetoes physical list reclaim. */
  assert(!lj_jit_event_sessions_fini(g, secondary_tg));
  assert(!lj_tg_fini_thread(g, secondary_tg));
  assert(lj_tg_fini_state_acq(secondary_tg) == TG_FINI_RETRY);
  assert(lj_tg_reclaim_dead(g) == 0u);
  assert(lj_tg_flags_test_acq(secondary_tg, TGF_DEAD));
  assert(lj_tg_fini_state_acq(secondary_tg) == TG_FINI_RETRY);

  /* Closing changed the generation/selector, so release reports stale while
  ** performing last-reader cleanup. Only then can strict fini and physical
  ** heap-TG reclaim complete. */
  assert(!lj_jit_event_session_snapshot_release(&held));
  assert(lj_jit_event_sessions_quiescent(secondary_tg));
  assert(lj_jit_event_sessions_detach_ready(secondary_tg));
  assert(lj_tg_fini_state_acq(secondary_tg) == TG_FINI_RETRY);
  assert(lj_tg_reclaim_dead(g) == 1u);  /* Frees the TGF_HEAP body. */
  lua_pop(L, 1);  /* secondary_L root */
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  jit_State *J;
  TGState *tg;
  GCtrace *T;
  GCproto *pt;
  GCobj *roots[TEST_ROOTS];
  LJJitEventFrozenViewSpec view, malformed_view, mismatch_view, alias_view;
  LJJitEventSessionSpec spec, stop_spec, abort_spec;
  LJJitEventSessionHandle h1, h2, h3, h4, rejected;
  LJJitEventSessionSnapshot held, probe;
  const LJJitEventSessionSlot *held_slot;
  GCfunc *callback_handler;
  GCRef *retained_roots;
  GCobj *const *saved_roots;
  void *view_storage;
  uint32_t i;
  int callback_weak_ref;
  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  J = G2J(g);
  tg = L2TG(L);
  assert(tg != NULL && tg->gl == g);
  assert(luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function f(n) local s=0; for i=1,n do s=s+i end; return s end\n"
    "for i=1,40 do assert(f(80)==3240) end\n"
    "return f\n") == LUA_OK);
  settle_automatic_cycle(g);
  T = published_trace_find(J);
  assert(T != NULL);
  pt = trace_startpt_acq(T);
  assert(pt != NULL);

  lua_pushcfunction(L, test_callback_handler);
  callback_handler = funcV(L->top - 1);
  assert(callback_handler != NULL && callback_handler->c.f ==
	 test_callback_handler);
  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_rawset(L, -3);
  assert(lua_setmetatable(L, -2));
  lua_pushinteger(L, 1);
  lua_pushvalue(L, -3);  /* Exact callback closure below the weak table. */
  lua_rawset(L, -3);
  callback_weak_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  for (i = 0; i < TEST_ROOTS; i++) {
    lua_newtable(L);
    roots[i] = obj2gco(tabV(L->top - 1));
  }
  settle_automatic_cycle(g);
  claim_for_session(L, J);
  /* A target-owned low recorder token alone vetoes actor handoff even while
  ** all event slots are strict FREE. Zero comparisons are intentionally not
  ** used as ownership for synthetic tid-zero fixtures. */
  {
    uint32_t actor = lj_tg_actor_acq(tg);
    assert(jit_owner_word_acq(g) ==
	   jit_owner_pack(lj_tg_tid_acq(tg), 0));
    assert(!lj_thr_tg_handoff_current(tg));
    assert(lj_thr_get_tg() == tg && lj_tg_actor_acq(tg) == actor);
  }
  frozen_view_build(T, &view, &view_storage);

  /* A pinned-source proof must match every immutable source scalar. */
  mismatch_view = view;
  mismatch_view.trace.root ^= 1u;
  memset(&stop_spec, 0, sizeof(stop_spec));
  stop_spec.event = LJ_JIT_EVENT_TRACE_STOP;
  stop_spec.owner_mode = LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE;
  stop_spec.edge_proof = LJ_JIT_EVENT_EDGE_PINNED_SOURCE;
  stop_spec.attachment_generation = 1;
  stop_spec.attachment_state = LJ_VMEVENT_ATTACHMENT_PUBLISHED;
  stop_spec.view = &mismatch_view;
  stop_spec.source = T;
  stop_spec.source_traceno = trace_traceno_acq(T);
  assert(!lj_jit_event_session_begin_l(L, J, &stop_spec, &rejected));
  assert(trace_native_pins_acq(T) == 0);
  assert(jit_owner_word_acq(g) ==
	 jit_owner_pack(lj_tg_tid_acq(tg), 0));

  /* Exact-source admission compares immutable bytes and canonical snapshot
  ** fields, not just the frozen scalar header. */
  stop_spec.view = &view;
  assert(view.ir.count != 0);
  ((unsigned char *)view_storage)[view.ir.offset] ^= 1u;
  assert(!lj_jit_event_session_begin_l(L, J, &stop_spec, &rejected));
  ((unsigned char *)view_storage)[view.ir.offset] ^= 1u;
  if (view.snapmap.count != 0) {
    ((unsigned char *)view_storage)[view.snapmap.offset] ^= 1u;
    assert(!lj_jit_event_session_begin_l(L, J, &stop_spec, &rejected));
    ((unsigned char *)view_storage)[view.snapmap.offset] ^= 1u;
  }
  if (view.snap.count != 0) {
    SnapShot *frozen =
      (SnapShot *)(void *)((unsigned char *)view_storage + view.snap.offset);
    MSize count = snap_count_acq(frozen);
    uint32_t mapofs = (uint32_t)snap_mapofs_acq(frozen);
    snap_count_rel(frozen, count == SNAPCOUNT_DONE ? count - 1u : count + 1u);
    assert(lj_jit_event_snapshot_matches_live(frozen, trace_snap_acq(T)));
    snap_count_rel(frozen, count);
    la_store32_rel(&frozen->mapofs, mapofs ^ 1u);
    assert(!lj_jit_event_session_begin_l(L, J, &stop_spec, &rejected));
    la_store32_rel(&frozen->mapofs, mapofs);
  }
  assert(trace_native_pins_acq(T) == 0);

  memset(&abort_spec, 0, sizeof(abort_spec));
  abort_spec.event = LJ_JIT_EVENT_TRACE_ABORT;
  abort_spec.owner_mode = LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE;
  abort_spec.edge_proof = LJ_JIT_EVENT_EDGE_EXACT_ROOTS;
  abort_spec.attachment_generation = 2;
  abort_spec.attachment_state = LJ_VMEVENT_ATTACHMENT_PUBLISHED;
  abort_spec.view = &view;
  abort_spec.roots = roots;
  abort_spec.root_count = TEST_ROOTS;
  assert(view.trace.mcode_addr != 0 && view.trace.szmcode != 0);
  assert(!lj_jit_event_session_begin_l(L, J, &abort_spec, &rejected));

  /* The fixture models a recorder-owned RECORD callback cut. Its V1 bytes are
  ** copied and every future raw-view GC edge is represented by this >8 vector.
  ** Decoder equality/range enforcement is intentionally a later wiring step. */
  lj_trace_state_store(J, LJ_TRACE_RECORD);
  memset(&spec, 0, sizeof(spec));
  spec.event = LJ_JIT_EVENT_RECORD;
  spec.owner_mode = LJ_JIT_EVENT_OWNER_CONTINUATION_LIFECYCLE;
  spec.edge_proof = LJ_JIT_EVENT_EDGE_EXACT_ROOTS;
  spec.attachment_generation = 2;
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_PUBLISHED;
  spec.view = &view;
  spec.roots = roots;
  spec.root_count = TEST_ROOTS;
  spec.callback_root_count = 1;
  spec.callback_handler = callback_handler;

  /* Attachment classification and generation are one canonical identity.
  ** INVALID/unknown never publish; INITIAL and UNCLOCKED require zero, while
  ** PUBLISHED requires nonzero (including the final uint64_t value). */
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_INVALID;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_INITIAL;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_PUBLISHED;
  spec.attachment_generation = 0;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_UNCLOCKED;
  spec.attachment_generation = 2;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.attachment_state = UINT32_MAX;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));

  /* Callback cardinality is explicit and the sentinel may only name FUNC. */
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_PUBLISHED;
  spec.callback_root_count = 0;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.callback_handler = NULL;
  spec.callback_root_count = 1;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.callback_handler = callback_handler;
  spec.callback_root_count = 2;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.callback_root_count = 1;
  spec.callback_handler = (GCfunc *)(void *)roots[0];
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.callback_handler = callback_handler;

  /* Seven proof roots plus the callback sentinel fit the eight inline lanes.
  ** Eight proof roots require a ninth lane and therefore the grow path. */
  spec.root_count = LJ_JIT_EVENT_SESSION_ROOTS - 1u;
  spec.attachment_generation = 0;
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_INITIAL;
  assert(lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  {
    LJJitEventSessionSlot *slot =
      &tg->jit_event_sessions.slot[rejected.slot];
    GCRef *slot_roots = (GCRef *)la_loadptr_acq(
      (void *const *)&slot->root_data);
    assert(slot_roots == slot->root_inline);
    assert(la_load32_acq(&slot->root_capacity) ==
	   LJ_JIT_EVENT_SESSION_ROOTS);
    assert(gcref_acq(slot_roots[spec.root_count]) ==
	   obj2gco(callback_handler));
  }
  assert(lj_jit_event_session_end_l(L, J, &rejected));

  spec.root_count = LJ_JIT_EVENT_SESSION_ROOTS;
  spec.attachment_generation = UINT64_MAX;
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_PUBLISHED;
  assert(lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  {
    LJJitEventSessionSlot *slot =
      &tg->jit_event_sessions.slot[rejected.slot];
    GCRef *slot_roots = (GCRef *)la_loadptr_acq(
      (void *const *)&slot->root_data);
    assert(slot_roots != slot->root_inline);
    assert(la_load32_acq(&slot->root_capacity) > spec.root_count);
    assert(gcref_acq(slot_roots[spec.root_count]) ==
	   obj2gco(callback_handler));
  }
  assert(lj_jit_event_session_end_l(L, J, &rejected));

  spec.root_count = TEST_ROOTS;
  spec.attachment_generation = 0;
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_INITIAL;

  malformed_view = view;
  malformed_view.ir.stride++;
  spec.view = &malformed_view;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));

  /* Offsets aligned relative to zero are insufficient: the actual byte base
  ** participates in every typed span address. Validate both unpublished
  ** specs and retained reader views before a SnapShot atomic load can occur. */
  {
    LJJitEventFrozenView malformed_retained;
    unsigned char *unaligned_storage;
    uintptr_t alignment = __alignof__(IRIns);
    uintptr_t overflow_base;
    if (alignment < __alignof__(SnapShot))
      alignment = __alignof__(SnapShot);
    if (alignment < __alignof__(SnapEntry))
      alignment = __alignof__(SnapEntry);
    assert(alignment > 1u && (alignment & (alignment - 1u)) == 0);
    assert(view.size != ~(uint32_t)0);
    unaligned_storage = (unsigned char *)malloc((size_t)view.size + 1u);
    assert(unaligned_storage != NULL);
    memcpy(unaligned_storage + 1u, view.data, view.size);
    malformed_view = view;
    malformed_view.data = unaligned_storage + 1u;
    spec.view = &malformed_view;
    assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));

    memset(&malformed_retained, 0, sizeof(malformed_retained));
    malformed_retained.data = unaligned_storage + 1u;
    malformed_retained.capacity = view.size;
    malformed_retained.size = view.size;
    malformed_retained.format = view.format;
    malformed_retained.flags = view.flags;
    malformed_retained.trace = view.trace;
    malformed_retained.ir = view.ir;
    malformed_retained.snap = view.snap;
    malformed_retained.snapmap = view.snapmap;
    assert(!lj_jit_event_frozen_view_valid(&malformed_retained));

    /* An aligned synthetic base near UINTPTR_MAX isolates address overflow
    ** from the alignment checks. No byte at this address is dereferenced. */
    overflow_base = ~(uintptr_t)0 & ~(alignment - 1u);
    assert((uintptr_t)view.size > ~(uintptr_t)0 - overflow_base);
    malformed_view.data = (const void *)overflow_base;
    assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
    malformed_retained.data = (void *)overflow_base;
    assert(!lj_jit_event_frozen_view_valid(&malformed_retained));
    free(unaligned_storage);
  }
  spec.view = &view;
  assert(lj_jit_event_sessions_quiescent(tg));

  /* Exercise the only ordinary post-publication handoff failure. It must roll
  ** back the slot while the exact low token and J->L are still intact. */
  lj_trace_test_force_event_handoff_failure(1);
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  assert(rejected.generation == 0 && rejected.slot == 0);
  assert(jit_owner_word_acq(g) ==
	 jit_owner_pack(lj_tg_tid_acq(tg), 0));
  assert(jit_owner_l_acq(J) == L);
  assert(lj_jit_event_sessions_quiescent(tg));

  /* Publication inputs may not alias either retained allocation before its
  ** reserve/realloc. The rollback above populated both retained buffers. */
  retained_roots = (GCRef *)la_loadptr_acq(
    (void *const *)&tg->jit_event_sessions.slot[0].root_data);
  assert(retained_roots != tg->jit_event_sessions.slot[0].root_inline);
  for (i = 0; i < TEST_ROOTS; i++)
    setgcrefrel(retained_roots[i], roots[i]);
  saved_roots = spec.roots;
  spec.roots = (GCobj *const *)(void *)retained_roots;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  /* The reserved callback lane is covered by retained-allocation alias
  ** rejection even though it lies outside the proof-only root_count. */
  setgcrefrel(retained_roots[TEST_ROOTS], obj2gco(callback_handler));
  spec.roots = (GCobj *const *)(void *)&retained_roots[TEST_ROOTS];
  spec.root_count = 1;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.root_count = TEST_ROOTS;
  setgcrefrel(retained_roots[TEST_ROOTS], NULL);
  spec.roots = saved_roots;
  alias_view = view;
  alias_view.data = tg->jit_event_sessions.slot[0].view.data;
  spec.view = &alias_view;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.view = &view;
  assert(lj_jit_event_sessions_quiescent(tg));

  assert(lj_jit_event_session_begin_l(L, J, &spec, &h1));
  assert(h1.slot == 0 && h1.generation != 0);
  assert(h1.owner_mode == LJ_JIT_EVENT_OWNER_CONTINUATION_LIFECYCLE);
  assert(jit_owner_word_acq(g) ==
	 jit_owner_pack(0, lj_tg_tid_acq(tg)));
  lua_pop(L, TEST_ROOTS);  /* Keep handler rooted until detached GC oracle. */
  assert(trace_native_pins_acq(T) == 0);
  assert(lj_jit_event_session_snapshot_acquire(g, tg, &held) ==
	 LJ_JIT_EVENT_SNAPSHOT_ACTIVE);
  held_slot = held.slot;
  assert(held.event == LJ_JIT_EVENT_RECORD);
  assert(held.owner_mode == LJ_JIT_EVENT_OWNER_CONTINUATION_LIFECYCLE);
  assert(held.edge_proof == LJ_JIT_EVENT_EDGE_EXACT_ROOTS);
  assert(held.attachment_state == LJ_VMEVENT_ATTACHMENT_INITIAL);
  assert(held.attachment_generation == 0);
  assert(held.callback_root_count == 1);
  assert(held.callback_handler == callback_handler);
  assert(held_slot->root_data != held_slot->root_inline);
  assert(la_load32_acq(&held_slot->root_capacity) > TEST_ROOTS);
  assert(la_load32_acq(&held_slot->root_count) == TEST_ROOTS);
  assert(la_load32_acq(&held_slot->callback_root_count) == 1u);
  assert((la_load32_acq(&held_slot->flags) &
	  LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT) != 0);
  retained_roots = (GCRef *)la_loadptr_acq(
    (void *const *)&held_slot->root_data);
  assert(gcref_acq(retained_roots[TEST_ROOTS]) ==
	 obj2gco(callback_handler));
  assert(held_slot->view.data != view.data);
  assert(memcmp(held_slot->view.data, view.data, view.size) == 0);
  assert(lj_jit_event_frozen_view_valid(&held_slot->view));

  /* CONTINUATION bytes are rooted only under the exact low/high owner word,
  ** J->L carrier and TG actor captured by publication. Each corruption must
  ** produce a retry; detached sessions below remain owner-word independent. */
  {
    uint32_t owner_tid = lj_tg_tid_acq(tg);
    uint32_t actor = lj_tg_actor_acq(tg);
    uint32_t peer_actor = actor == LJ_THR_ACTOR_RETIRED - 1u ? actor - 1u :
      actor + 1u;
    assert(peer_actor != 0 && peer_actor != actor &&
	   peer_actor != LJ_THR_ACTOR_RETIRED);
    assert(!lj_thr_tg_handoff_current(tg));
    assert(lj_thr_get_tg() == tg && lj_tg_actor_acq(tg) == actor);
    jit_owner_test_rel(g, 0, 0);
    assert(!lj_gc2_test_scan_jit_event_sessions(g, tg));
    jit_owner_test_rel(g, 0, owner_tid);
    jit_owner_l_rel(J, NULL);
    assert(!lj_gc2_test_scan_jit_event_sessions(g, tg));
    jit_owner_l_rel(J, L);
    la_store32_rel(&tg->jit_event_sessions.slot[h1.slot].owner_actor,
		   peer_actor);
    assert(!lj_gc2_test_scan_jit_event_sessions(g, tg));
    la_store32_rel(&tg->jit_event_sessions.slot[h1.slot].owner_actor, actor);
  }

  /* Snapshot release compares the new immutable identity and callback lane,
  ** not merely the outer session generation. A held peer reader keeps the
  ** backing stable while each exactness failure is injected and restored. */
  assert(lj_jit_event_session_snapshot_acquire(g, tg, &probe) ==
	 LJ_JIT_EVENT_SNAPSHOT_ACTIVE);
  la_store32_rel(&tg->jit_event_sessions.slot[h1.slot].attachment_state,
		 LJ_VMEVENT_ATTACHMENT_PUBLISHED);
  assert(!lj_jit_event_session_snapshot_release(&probe));
  la_store32_rel(&tg->jit_event_sessions.slot[h1.slot].attachment_state,
		 LJ_VMEVENT_ATTACHMENT_INITIAL);

  assert(lj_jit_event_session_snapshot_acquire(g, tg, &probe) ==
	 LJ_JIT_EVENT_SNAPSHOT_ACTIVE);
  setgcrefrel(retained_roots[TEST_ROOTS], NULL);
  assert(!lj_jit_event_session_snapshot_release(&probe));
  setgcrefrel(retained_roots[TEST_ROOTS], obj2gco(callback_handler));

  {
    LJJitEventSessionSlot *slot =
      &tg->jit_event_sessions.slot[h1.slot];
    uint32_t flags = la_load32_acq(&slot->flags);
    uint32_t capacity = la_load32_acq(&slot->root_capacity);

    la_store32_rel(&slot->attachment_state, LJ_VMEVENT_ATTACHMENT_INVALID);
    assert(!lj_gc2_test_scan_jit_event_sessions(g, tg));
    la_store32_rel(&slot->attachment_state, LJ_VMEVENT_ATTACHMENT_INITIAL);

    la_store64_rel(&slot->attachment_generation, 1u);
    assert(!lj_gc2_test_scan_jit_event_sessions(g, tg));
    la_store64_rel(&slot->attachment_generation, 0);

    la_store32_rel(&slot->callback_root_count, 2u);
    assert(!lj_gc2_test_scan_jit_event_sessions(g, tg));
    la_store32_rel(&slot->callback_root_count, 1u);

    la_store32_rel(&slot->flags,
		   flags & ~LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT);
    assert(!lj_gc2_test_scan_jit_event_sessions(g, tg));
    la_store32_rel(&slot->flags, flags);

    setgcrefrel(retained_roots[TEST_ROOTS], NULL);
    assert(!lj_gc2_test_scan_jit_event_sessions(g, tg));
    setgcrefrel(retained_roots[TEST_ROOTS], roots[0]);
    assert(lj_jit_event_session_snapshot_acquire(g, tg, &probe) ==
	   LJ_JIT_EVENT_SNAPSHOT_RETRY);
    assert(!lj_gc2_test_scan_jit_event_sessions(g, tg));
    setgcrefrel(retained_roots[TEST_ROOTS], obj2gco(callback_handler));

    la_store32_rel(&slot->root_count, capacity);
    assert(!lj_gc2_test_scan_jit_event_sessions(g, tg));
    la_store32_rel(&slot->root_count, TEST_ROOTS);
  }

  for (i = 0; i < TEST_ROOTS; i++) {
    unmark_small(roots[i]);
    assert(lj_gc2_ismarked(g, roots[i]) == 0);
  }
  unmark_small(obj2gco(callback_handler));
  assert(lj_gc2_ismarked(g, obj2gco(callback_handler)) == 0);
  assert(lj_gc2_test_scan_jit_event_sessions(g, tg));
  for (i = 0; i < TEST_ROOTS; i++)
    assert(lj_gc2_ismarked(g, roots[i]) > 0);
  assert(lj_gc2_ismarked(g, obj2gco(callback_handler)) > 0);

  /* END resumes high->low while roots remain ACTIVE, then closes without
  ** waiting for this retained reader. Slot 1 remains available. */
  assert(!lj_jit_event_sessions_fini(g, tg));
  assert(lj_jit_event_session_end_l(L, J, &h1));
  assert(jit_owner_word_acq(g) ==
	 jit_owner_pack(lj_tg_tid_acq(tg), 0));
  assert(!lj_jit_event_sessions_quiescent(tg));
  assert(lj_jit_event_sessions_logical_detach_ready(tg));
  assert(!lj_jit_event_sessions_fini(g, tg));
  assert(la_load32_acq(&held_slot->state) == LJ_JIT_EVENT_SLOT_CLOSED);
  assert(la_load32_acq(&held_slot->callback_root_count) == 1u);
  retained_roots = (GCRef *)la_loadptr_acq(
    (void *const *)&held_slot->root_data);
  assert(gcref_acq(retained_roots[TEST_ROOTS]) ==
	 obj2gco(callback_handler));

  /* Slot 1 would be selected while the held reader keeps slot 0 CLOSED. Input
  ** aliases against that other retained slot must be rejected before even
  ** dereferencing a root element which last-reader cleanup could zero. */
  retained_roots = (GCRef *)la_loadptr_acq(
    (void *const *)&held_slot->root_data);
  saved_roots = spec.roots;
  spec.roots = (GCobj *const *)(void *)retained_roots;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.roots = (GCobj *const *)(void *)&retained_roots[TEST_ROOTS];
  spec.root_count = 1;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.root_count = TEST_ROOTS;
  spec.roots = saved_roots;
  alias_view = view;
  alias_view.data = held_slot->view.data;
  spec.view = &alias_view;
  assert(!lj_jit_event_session_begin_l(L, J, &spec, &rejected));
  spec.view = &view;

  spec.attachment_generation = 3;
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_PUBLISHED;
  assert(lj_jit_event_session_begin_l(L, J, &spec, &h2));
  assert(h2.slot == 1 && h2.generation > h1.generation);
  assert(!lj_jit_event_session_end_l(L, J, &h1));
  assert(jit_owner_word_acq(g) ==
	 jit_owner_pack(0, lj_tg_tid_acq(tg)));
  assert(lj_jit_event_session_end_l(L, J, &h2));
  assert(!lj_jit_event_session_snapshot_release(&held));
  assert(la_load32_acq(&held_slot->state) == LJ_JIT_EVENT_SLOT_FREE);
  assert(la_load32_acq(&held_slot->callback_root_count) == 0);
  assert(la_load32_acq(&held_slot->attachment_state) ==
	 LJ_VMEVENT_ATTACHMENT_INVALID);
  assert((la_load32_acq(&held_slot->flags) &
	  LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT) == 0);
  assert(gcref_acq(retained_roots[TEST_ROOTS]) == NULL);
  assert(lj_jit_event_sessions_quiescent(tg));

  /* A completed STOP owns only immutable bytes plus an exact published source.
  ** BEGIN releases low->zero, so an unrelated recorder may run concurrently. */
  lj_trace_state_store(J, LJ_TRACE_IDLE);
  stop_spec.attachment_generation = 4;
  stop_spec.attachment_state = LJ_VMEVENT_ATTACHMENT_PUBLISHED;
  stop_spec.view = &view;
  stop_spec.callback_root_count = 1;
  stop_spec.callback_handler = callback_handler;
  assert(lj_jit_event_session_begin_l(L, J, &stop_spec, &h3));
  assert(h3.slot == 0 && h3.generation > h2.generation);
  assert(h3.owner_mode == LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE);
  assert(jit_owner_word_acq(g) == jit_owner_pack(0, 0));
  assert(jit_owner_l_acq(J) == NULL);
  assert(trace_native_pins_acq(T) == 1u);

  /* The weak registry table is now the only non-session reference. Unlike a
  ** yielded continuation, this detached owner-word-zero contract legitimately
  ** permits a complete GC without the future same-owner control borrow. */
  lua_pop(L, 1);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  lua_rawgeti(L, LUA_REGISTRYINDEX, callback_weak_ref);
  lua_rawgeti(L, -1, 1);
  assert(lua_isfunction(L, -1));
  assert(funcV(L->top - 1) == callback_handler);
  lua_pop(L, 2);

  /* A leaked same-TG reentrant token cannot authorize detached close. */
  claim_for_session(L, J);
  assert(!lj_jit_event_session_end_l(L, J, &h3));
  assert(lj_jit_event_session_snapshot_acquire(g, tg, &held) ==
	 LJ_JIT_EVENT_SNAPSHOT_ACTIVE);
  assert(held.owner_mode == LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE);
  assert(lj_jit_event_session_snapshot_release(&held));
  release_after_session(L, J);

  assert(lj_trace_flushall_gc(L) == 0);
  assert(trace_native_pin_closed_acq(T));
  assert(trace_native_pins_acq(T) == 1u);
  unmark_small(obj2gco(T));
  unmark_small(obj2gco(pt));
  {
    uint32_t owner_tid = lj_tg_tid_acq(tg);
    uint32_t peer_tid = owner_tid == ~(uint32_t)0 ? owner_tid - 1u :
      owner_tid + 1u;
    assert(peer_tid != 0 && peer_tid != owner_tid);
    jit_owner_test_rel(g, peer_tid, 0);
    assert(lj_gc2_test_scan_jit_event_sessions(g, tg));
    jit_owner_test_rel(g, 0, 0);
  }
  assert(lj_gc2_ismarked(g, obj2gco(T)) > 0);
  assert(lj_gc2_ismarked(g, obj2gco(pt)) > 0);
  {
    uint32_t owner_tid = lj_tg_tid_acq(tg);
    uint32_t peer_tid = owner_tid == ~(uint32_t)0 ? owner_tid - 1u :
      owner_tid + 1u;
    assert(peer_tid != 0 && peer_tid != owner_tid);
    jit_owner_test_rel(g, peer_tid, 0);
    assert(lj_jit_event_session_end_l(L, J, &h3));
    assert(jit_owner_word_acq(g) == jit_owner_pack(peer_tid, 0));
    jit_owner_test_rel(g, 0, 0);
  }
  assert(trace_native_pins_acq(T) == 0);
  assert(!lj_jit_event_session_end_l(L, J, &h3));

  /* The direct scanner probe above deliberately set the handler mark outside
  ** a collector cycle. Remove that test-only mark after closing the last
  ** semantic root, then prove no retained lane keeps the weak value alive. */
  unmark_small(obj2gco(callback_handler));
  assert(lj_gc2_ismarked(g, obj2gco(callback_handler)) == 0);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  lua_rawgeti(L, LUA_REGISTRYINDEX, callback_weak_ref);
  lua_rawgeti(L, -1, 1);
  assert(lua_isnil(L, -1));
  lua_pop(L, 2);
  luaL_unref(L, LUA_REGISTRYINDEX, callback_weak_ref);

  /* Payload-free FLUSH is the other detached storage contract. Isolate the
  ** same-owner lua_close fast-path gate from the owner word. */
  claim_for_session(L, J);
  memset(&spec, 0, sizeof(spec));
  spec.event = LJ_JIT_EVENT_TRACE_FLUSH;
  spec.owner_mode = LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE;
  spec.edge_proof = LJ_JIT_EVENT_EDGE_NONE;
  spec.attachment_generation = 0;
  spec.attachment_state = LJ_VMEVENT_ATTACHMENT_UNCLOCKED;
  assert(lj_jit_event_session_begin_l(L, J, &spec, &h4));
  assert(jit_owner_word_acq(g) == jit_owner_pack(0, 0));
  assert(lj_jit_event_session_snapshot_acquire(g, tg, &held) ==
	 LJ_JIT_EVENT_SNAPSHOT_ACTIVE);
  assert(held.attachment_state == LJ_VMEVENT_ATTACHMENT_UNCLOCKED);
  assert(held.attachment_generation == 0);
  assert(held.callback_root_count == 0 && held.callback_handler == NULL);
  assert(lj_jit_event_session_snapshot_release(&held));
  {
    uint32_t actor = lj_tg_actor_acq(tg);
    assert(!lj_thr_tg_handoff_current(tg));
    assert(lj_thr_get_tg() == tg && lj_tg_actor_acq(tg) == actor);
  }
  assert(!lj_thr_main_close_claim(L));
  assert(lj_jit_event_session_end_l(L, J, &h4));
  assert(lj_jit_event_sessions_detach_ready(tg));

  test_secondary_closed_reader_detach(L, J);
  free(view_storage);
  lua_close(L);
  puts("t-jit-event-session OK: composite modes, roots, pins and reuse verified");
  return 0;
}
