/*
** Trace management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_trace_c
#define LUA_CORE

#include <stdlib.h>
#include <string.h>

#include "lj_obj.h"

#if LJ_HASJIT

#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_buf.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_oserr.h"
#include "lj_debug.h"
#include "lj_str.h"
#include "lj_frame.h"
#include "lj_state.h"
#include "lj_bc.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_iropt.h"
#include "lj_mcode.h"
#include "lj_trace.h"
#include "lj_snap.h"
#include "lj_gdbjit.h"
#include "lj_record.h"
#include "lj_asm.h"
#include "lj_safepoint.h"
#include "lj_dispatch.h"
#include "lj_thr.h"
#include "lj_tg.h"
#if LJ_HASFFI && LJ_HASJIT
#include "lj_ccall.h"
#endif
#if LJ_HASPROFILE
#include "lj_profile.h"
#endif
#include "lj_vm.h"
#include "lj_vmevent.h"
#include "lj_target.h"
#include "lj_prng.h"

/* Trace bodies/prototypes are semantically ordinary GC2 objects, but the
** compatibility total includes the much larger fixed GC2 runtime state. A
** short burst can therefore fill public trace slots without reaching the
** allocation trigger that stock reaches. Periodically publish ordinary GC
** pressure; the recorder still never performs collection or liveness work. */
#define TRACE_GC_PRESSURE_BATCH	64u

#ifdef LJ_TRACE_TEST_HELPERS
static uint32_t trace_test_call_unroll_aborts;
static uint32_t trace_test_call_unroll_linked;
static uint32_t trace_test_flush_unlink_calls;
static uint32_t trace_test_flush_unlink_returns;
static uint32_t trace_test_abort_selflinks;
static uint32_t trace_test_slot_release_calls;
static uint32_t trace_test_slot_release_clears;
static uint32_t trace_test_findfree_calls;
static uint32_t trace_test_findfree_reuses;
static uint32_t trace_test_findfree_grows;
static uint32_t trace_test_last_unlinked;
static uint32_t trace_test_last_findfree;
static uint32_t trace_test_last_released;
static uint32_t trace_test_admission_stage;
static uint32_t trace_test_admission_request;
static uint32_t trace_test_admission_actions;
static uint32_t trace_test_admission_hit_count;
static uint32_t trace_test_admission_clean_release_count;
static uint32_t trace_test_admission_protected_poll_count;
static uint32_t trace_test_admission_side_gate_block_count;
static uint32_t trace_test_admission_side_clean_release_count;
static uint32_t trace_test_admission_observer_waiting_flag;
static uint32_t trace_test_admission_hotcount_slot;
static uint32_t trace_test_admission_hotcount_value;
static uint32_t trace_test_admission_side_parent;
static uint32_t trace_test_admission_side_exitno;
static uint32_t trace_test_admission_side_snapshot_value;
static uint32_t trace_test_admission_cleanup_errno_clobber;

void lj_trace_test_admission_reset(void)
{
  la_store32_rel(&trace_test_admission_stage, 0);
  la_store32_rel(&trace_test_admission_request, 0);
  la_store32_rel(&trace_test_admission_actions, 0);
  la_store32_rel(&trace_test_admission_hit_count, 0);
  la_store32_rel(&trace_test_admission_clean_release_count, 0);
  la_store32_rel(&trace_test_admission_protected_poll_count, 0);
  la_store32_rel(&trace_test_admission_side_gate_block_count, 0);
  la_store32_rel(&trace_test_admission_side_clean_release_count, 0);
  la_store32_rel(&trace_test_admission_observer_waiting_flag, 0);
  la_store32_rel(&trace_test_admission_hotcount_slot, 0);
  la_store32_rel(&trace_test_admission_hotcount_value, 0);
  la_store32_rel(&trace_test_admission_side_parent, 0);
  la_store32_rel(&trace_test_admission_side_exitno, 0);
  la_store32_rel(&trace_test_admission_side_snapshot_value, 0);
  la_store32_rel(&trace_test_admission_cleanup_errno_clobber, 0);
}

void lj_trace_test_admission_arm(uint32_t stage, uint32_t request,
				 uint32_t actions)
{
  if (stage < LJ_TRACE_TEST_ADMISSION_ENTRY ||
      stage > LJ_TRACE_TEST_ADMISSION_SIDE_AFTER_TOKEN ||
      (request != LJ_TRACE_TEST_REQUEST_COUNTED &&
       request != LJ_TRACE_TEST_REQUEST_PROFILE &&
       request != LJ_TRACE_TEST_REQUEST_OBSERVE) ||
      (request == LJ_TRACE_TEST_REQUEST_COUNTED && actions == 0) ||
      (request != LJ_TRACE_TEST_REQUEST_COUNTED && actions != 0))
    abort();
  la_store32_rel(&trace_test_admission_request, request);
  la_store32_rel(&trace_test_admission_actions, actions);
  la_store32_rel(&trace_test_admission_stage, stage);
}

void lj_trace_test_admission_clobber_cleanup_errno(uint32_t errnum)
{
  if (errnum == 0)
    abort();
  la_store32_rel(&trace_test_admission_cleanup_errno_clobber, errnum);
}

uint32_t lj_trace_test_admission_hits(void)
{
  return la_load32_acq(&trace_test_admission_hit_count);
}

uint32_t lj_trace_test_admission_clean_releases(void)
{
  return la_load32_acq(&trace_test_admission_clean_release_count);
}

uint32_t lj_trace_test_admission_protected_polls(void)
{
  return la_load32_acq(&trace_test_admission_protected_poll_count);
}

uint32_t lj_trace_test_admission_side_gate_blocks(void)
{
  return la_load32_acq(&trace_test_admission_side_gate_block_count);
}

uint32_t lj_trace_test_admission_side_clean_releases(void)
{
  return la_load32_acq(&trace_test_admission_side_clean_release_count);
}

uint32_t lj_trace_test_admission_observer_waiting(void)
{
  return la_load32_acq(&trace_test_admission_observer_waiting_flag);
}

uint32_t lj_trace_test_admission_armed(void)
{
  return la_load32_acq(&trace_test_admission_stage);
}

uint32_t lj_trace_test_admission_hotcount_index(void)
{
  return la_load32_acq(&trace_test_admission_hotcount_slot);
}

uint32_t lj_trace_test_admission_hotcount_before(void)
{
  return la_load32_acq(&trace_test_admission_hotcount_value);
}

TraceNo lj_trace_test_admission_side_parent(void)
{
  return (TraceNo)la_load32_acq(&trace_test_admission_side_parent);
}

ExitNo lj_trace_test_admission_side_exitno(void)
{
  return (ExitNo)la_load32_acq(&trace_test_admission_side_exitno);
}

uint32_t lj_trace_test_admission_side_snapshot_before(void)
{
  return la_load32_acq(&trace_test_admission_side_snapshot_value);
}

static void trace_test_admission_publish(lua_State *L, jit_State *J,
					 const BCIns *pc, uint32_t stage,
					 TraceNo parent, ExitNo exitno,
					 SnapShot *snap)
{
  global_State *g;
  TGState *tg;
  uint32_t armed = stage;
  uint32_t request, actions;
  if (!la_cas32(&trace_test_admission_stage, &armed, 0,
		LA_ACQ_REL, LA_ACQ))
    return;
  if (!L || !J || G(L) != J2G(J) || !(tg = L2TG(L)))
    abort();
  g = G(L);
  request = la_load32_acq(&trace_test_admission_request);
  actions = la_load32_acq(&trace_test_admission_actions);
  if (pc) {
    uint32_t slot = (u32ptr(pc) >> 2) & (HOTCOUNT_SIZE-1u);
    la_store32_rel(&trace_test_admission_hotcount_slot, slot);
    la_store32_rel(&trace_test_admission_hotcount_value,
			   (uint32_t)tg->hotcount[slot]);
  }
  if (snap) {
    if (parent == 0)
      abort();
    la_store32_rel(&trace_test_admission_side_parent, (uint32_t)parent);
    la_store32_rel(&trace_test_admission_side_exitno, (uint32_t)exitno);
    la_store32_rel(&trace_test_admission_side_snapshot_value,
		   (uint32_t)snap_count_acq(snap));
  }
  if (request == LJ_TRACE_TEST_REQUEST_COUNTED) {
    uint64_t epoch;
    if (actions == 0 || gc2_hs_pending_acq(g) != 0 ||
	lj_tg_reqmask_acq(tg) != 0 || lj_tg_poll_acq(tg) != 0)
      abort();
    epoch = gc2_hs_epoch_rlx(g) + 1u;
    gc2_hs_actions_rel(g, actions);
    gc2_hs_pending_rel(g, 1);
    gc2_hs_epoch_rel(g, epoch);
    lj_tg_reqmask_rel(tg, actions);
    lj_tg_poll_rel(tg, 1);
  } else if (request == LJ_TRACE_TEST_REQUEST_PROFILE) {
    if (actions != 0 || lj_tg_profile_request_acq(tg) != 0)
      abort();
    lj_tg_profile_request_rel(tg, 1);
  } else if (request == LJ_TRACE_TEST_REQUEST_OBSERVE) {
    /* Expose the exact real-metadata observation point to a publisher thread,
    ** then wait until its serialized leader is paused after reqmask. This hook
    ** does not manufacture or acknowledge any request itself. */
    la_store32_rel(&trace_test_admission_observer_waiting_flag, 1);
    while (!lj_safepoint_test_signal_paused())
      la_cpu_pause();
    la_store32_rel(&trace_test_admission_observer_waiting_flag, 0);
  } else {
    abort();
  }
  (void)la_add32_acqrel(&trace_test_admission_hit_count, 1);
}

static void trace_test_admission_inject(lua_State *L, jit_State *J,
					const BCIns *pc, uint32_t stage)
{
  trace_test_admission_publish(L, J, pc, stage, 0, 0, NULL);
}

static void trace_test_side_admission_inject_held(lua_State *L, jit_State *J,
						   TraceNo parent,
						   ExitNo exitno,
						   SnapShot *snap,
						   uint32_t stage)
{
  trace_test_admission_publish(L, J, NULL, stage, parent, exitno, snap);
}

static void trace_test_side_admission_inject(lua_State *L, jit_State *J,
					      TraceNo parent, ExitNo exitno,
					      uint32_t stage)
{
  global_State *g;
  GCtrace *T;
  SnapShot *snap;
  if (la_load32_acq(&trace_test_admission_stage) != stage)
    return;
  if (!L || !J || G(L) != J2G(J))
    abort();
  g = G(L);
  if (!lj_gc2_smr_read_try(g))
    abort();
  T = traceref_safe(J, parent);
  if (!trace_runnable_acq(T, parent) || exitno >= trace_nsnap_acq(T)) {
    lj_gc2_smr_read_leave(g);
    abort();
  }
  snap = &trace_snap_acq(T)[exitno];
  trace_test_side_admission_inject_held(L, J, parent, exitno, snap, stage);
  lj_gc2_smr_read_leave(g);
}

static void trace_test_admission_note_clean_release(lua_State *L,
					     jit_State *J)
{
  if (L && J && lj_trace_state_load(J) == LJ_TRACE_IDLE &&
      jit_token_acq(J2G(J)) == 0 && jit_owner_l_acq(J) == NULL)
    (void)la_add32_acqrel(&trace_test_admission_clean_release_count, 1);
}

static void trace_test_admission_note_protected_poll(void)
{
  (void)la_add32_acqrel(&trace_test_admission_protected_poll_count, 1);
}

static void trace_test_admission_note_side_gate_block(void)
{
  (void)la_add32_acqrel(&trace_test_admission_side_gate_block_count, 1);
}

static void trace_test_admission_note_side_clean_release(lua_State *L,
						  jit_State *J)
{
  global_State *g = J ? J2G(J) : NULL;
  if (L && J && lj_trace_state_load(J) == LJ_TRACE_IDLE &&
      jit_token_acq(g) == 0 && jit_owner_l_acq(J) == NULL &&
      gc2_smr_readers_acq(g) == 0)
    (void)la_add32_acqrel(&trace_test_admission_side_clean_release_count, 1);
}

static void trace_test_admission_maybe_clobber_cleanup_oserr(void)
{
  uint32_t errnum = la_load32_acq(&trace_test_admission_cleanup_errno_clobber);
  if (errnum != 0) {
    LJOSerrState oserr;
    la_store32_rel(&trace_test_admission_cleanup_errno_clobber, 0);
    oserr.errnum = (int)errnum;
    oserr.winerr = errnum;
    lj_oserr_restore(&oserr);
  }
}
#if LJ_TARGET_ARM64 && LJ_HASJIT
static uint32_t trace_test_root_entry_pause_stage;
static uint32_t trace_test_root_entry_pause_waiting;
static uint32_t trace_test_root_entry_pause_release;
static uint32_t trace_test_root_entry_publish_count;
static uint32_t trace_test_root_entry_cleanup_count;

void lj_trace_test_root_entry_reset(void)
{
  la_store32_rel(&trace_test_root_entry_pause_stage, 0);
  la_store32_rel(&trace_test_root_entry_pause_waiting, 0);
  la_store32_rel(&trace_test_root_entry_pause_release, 0);
  la_store32_rel(&trace_test_root_entry_publish_count, 0);
  la_store32_rel(&trace_test_root_entry_cleanup_count, 0);
}

void lj_trace_test_root_entry_pause(uint32_t stage)
{
  la_store32_rel(&trace_test_root_entry_pause_waiting, 0);
  la_store32_rel(&trace_test_root_entry_pause_release, 0);
  la_store32_rel(&trace_test_root_entry_pause_stage, stage);
}

uint32_t lj_trace_test_root_entry_paused(void)
{
  return la_load32_acq(&trace_test_root_entry_pause_waiting);
}

void lj_trace_test_root_entry_release(void)
{
  la_store32_rel(&trace_test_root_entry_pause_release, 1);
}

uint32_t lj_trace_test_root_entry_publishes(void)
{
  return la_load32_acq(&trace_test_root_entry_publish_count);
}

uint32_t lj_trace_test_root_entry_cleanups(void)
{
  return la_load32_acq(&trace_test_root_entry_cleanup_count);
}

static void trace_test_root_entry_maybe_pause(uint32_t stage)
{
  if (la_load32_acq(&trace_test_root_entry_pause_stage) != stage)
    return;
  la_store32_rel(&trace_test_root_entry_pause_waiting, stage);
  while (la_load32_acq(&trace_test_root_entry_pause_release) == 0)
    la_cpu_pause();
  la_store32_rel(&trace_test_root_entry_pause_stage, 0);
  la_store32_rel(&trace_test_root_entry_pause_waiting, 0);
}

#define trace_test_root_entry_pause_at(stage) \
  trace_test_root_entry_maybe_pause((stage))
#define trace_test_root_entry_published() \
  ((void)la_add32_acqrel(&trace_test_root_entry_publish_count, 1))
#define trace_test_root_entry_cleaned() \
  ((void)la_add32_acqrel(&trace_test_root_entry_cleanup_count, 1))
#endif

#define TRACE_TEST_COUNTER(name) \
uint32_t lj_trace_test_##name(void) \
{ \
  return la_load32_acq(&trace_test_##name); \
}

void lj_trace_test_reset_retention_stats(void)
{
  la_store32_rel(&trace_test_call_unroll_aborts, 0);
  la_store32_rel(&trace_test_call_unroll_linked, 0);
  la_store32_rel(&trace_test_flush_unlink_calls, 0);
  la_store32_rel(&trace_test_flush_unlink_returns, 0);
  la_store32_rel(&trace_test_abort_selflinks, 0);
  la_store32_rel(&trace_test_slot_release_calls, 0);
  la_store32_rel(&trace_test_slot_release_clears, 0);
  la_store32_rel(&trace_test_findfree_calls, 0);
  la_store32_rel(&trace_test_findfree_reuses, 0);
  la_store32_rel(&trace_test_findfree_grows, 0);
  la_store32_rel(&trace_test_last_unlinked, 0);
  la_store32_rel(&trace_test_last_findfree, 0);
  la_store32_rel(&trace_test_last_released, 0);
}

void lj_trace_test_note_call_unroll_abort(TraceNo lnk)
{
  (void)la_add32_acqrel(&trace_test_call_unroll_aborts, 1);
  if (lnk)
    (void)la_add32_acqrel(&trace_test_call_unroll_linked, 1);
}

TRACE_TEST_COUNTER(call_unroll_aborts)
TRACE_TEST_COUNTER(call_unroll_linked)
TRACE_TEST_COUNTER(flush_unlink_calls)
TRACE_TEST_COUNTER(flush_unlink_returns)
TRACE_TEST_COUNTER(abort_selflinks)
TRACE_TEST_COUNTER(slot_release_calls)
TRACE_TEST_COUNTER(slot_release_clears)
TRACE_TEST_COUNTER(findfree_calls)
TRACE_TEST_COUNTER(findfree_reuses)
TRACE_TEST_COUNTER(findfree_grows)
TRACE_TEST_COUNTER(last_unlinked)
TRACE_TEST_COUNTER(last_findfree)
TRACE_TEST_COUNTER(last_released)

static LJ_AINLINE void trace_test_note_flush_unlink(GCtrace *T,
						    TraceNo traceno)
{
  (void)la_add32_acqrel(&trace_test_flush_unlink_calls, 1);
  if (trace_linktype_acq(T) == LJ_TRLINK_RETURN)
    (void)la_add32_acqrel(&trace_test_flush_unlink_returns, 1);
  la_store32_rel(&trace_test_last_unlinked, (uint32_t)traceno);
}

static LJ_AINLINE void trace_test_note_abort_selflink(TraceNo traceno)
{
  (void)la_add32_acqrel(&trace_test_abort_selflinks, 1);
  la_store32_rel(&trace_test_last_released, (uint32_t)traceno);
}

static LJ_AINLINE void trace_test_note_slot_release(TraceNo traceno,
						   int cleared)
{
  (void)la_add32_acqrel(&trace_test_slot_release_calls, 1);
  if (cleared)
    (void)la_add32_acqrel(&trace_test_slot_release_clears, 1);
  la_store32_rel(&trace_test_last_released, (uint32_t)traceno);
}

static LJ_AINLINE void trace_test_note_findfree_reuse(TraceNo traceno)
{
  (void)la_add32_acqrel(&trace_test_findfree_reuses, 1);
  la_store32_rel(&trace_test_last_findfree, (uint32_t)traceno);
}

static LJ_AINLINE void trace_test_note_findfree_grow(TraceNo traceno)
{
  (void)la_add32_acqrel(&trace_test_findfree_grows, 1);
  la_store32_rel(&trace_test_last_findfree, (uint32_t)traceno);
}
#else
#define trace_test_admission_inject(L, J, pc, stage) \
  ((void)(L), (void)(J), (void)(pc), (void)(stage))
#define trace_test_side_admission_inject(L, J, parent, exitno, stage) \
  ((void)(L), (void)(J), (void)(parent), (void)(exitno), (void)(stage))
#define trace_test_side_admission_inject_held(L, J, parent, exitno, snap, stage) \
  ((void)(L), (void)(J), (void)(parent), (void)(exitno), (void)(snap), \
   (void)(stage))
#define trace_test_admission_note_clean_release(L, J) \
  ((void)(L), (void)(J))
#define trace_test_admission_note_protected_poll() ((void)0)
#define trace_test_admission_note_side_gate_block() ((void)0)
#define trace_test_admission_note_side_clean_release(L, J) \
  ((void)(L), (void)(J))
#define trace_test_admission_maybe_clobber_cleanup_oserr() ((void)0)
#define trace_test_note_flush_unlink(T, traceno) \
  ((void)(T), (void)(traceno))
#define trace_test_note_abort_selflink(traceno)		((void)(traceno))
#define trace_test_note_slot_release(traceno, cleared) \
  ((void)(traceno), (void)(cleared))
#define trace_test_note_findfree_reuse(traceno)		((void)(traceno))
#define trace_test_note_findfree_grow(traceno)		((void)(traceno))
#endif

#if defined(LJ_TRACE_TEST_HELPERS) || defined(LJ_GC2_TEST_HELPERS)
static uint32_t trace_test_retire_publish_calls;
static uint32_t trace_test_force_startins_retries;
static uint32_t trace_test_exit_calls;
static uint32_t trace_test_last_exit_parent;
static uint32_t trace_test_last_exitno;
static uint32_t trace_test_force_event_handoff_failures;

void lj_trace_test_force_startins_retry(uint32_t count)
{
  la_store32_rel(&trace_test_force_startins_retries, count);
}

void lj_trace_test_force_event_handoff_failure(uint32_t count)
{
  la_store32_rel(&trace_test_force_event_handoff_failures, count);
}

static int trace_test_take_event_handoff_failure(void)
{
  uint32_t old = la_load32_acq(&trace_test_force_event_handoff_failures);
  while (old != 0) {
    uint32_t expect = old;
    if (la_cas32(&trace_test_force_event_handoff_failures, &expect, old - 1u,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
    old = expect;
  }
  return 0;
}

static int trace_test_take_startins_retry(void)
{
  uint32_t old = la_load32_acq(&trace_test_force_startins_retries);
  while (old != 0) {
    uint32_t expect = old;
    if (la_cas32(&trace_test_force_startins_retries, &expect, old - 1u,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
    old = expect;
  }
  return 0;
}

void lj_trace_test_reset_retire_publish_calls(void)
{
  la_store32_rel(&trace_test_retire_publish_calls, 0);
}

uint32_t lj_trace_test_retire_publish_calls(void)
{
  return la_load32_acq(&trace_test_retire_publish_calls);
}

void lj_trace_test_reset_exit_stats(void)
{
  la_store32_rel(&trace_test_exit_calls, 0);
  la_store32_rel(&trace_test_last_exit_parent, 0);
  la_store32_rel(&trace_test_last_exitno, 0);
}

uint32_t lj_trace_test_exit_calls(void)
{
  return la_load32_acq(&trace_test_exit_calls);
}

TraceNo lj_trace_test_last_exit_parent(void)
{
  return (TraceNo)la_load32_acq(&trace_test_last_exit_parent);
}

ExitNo lj_trace_test_last_exitno(void)
{
  return (ExitNo)la_load32_acq(&trace_test_last_exitno);
}

static void trace_test_note_exit(TraceNo parent, ExitNo exitno)
{
  la_store32_rel(&trace_test_last_exit_parent, (uint32_t)parent);
  la_store32_rel(&trace_test_last_exitno, (uint32_t)exitno);
  (void)la_add32_acqrel(&trace_test_exit_calls, 1);
}
#else
#define trace_test_take_startins_retry() 0
#define trace_test_take_event_handoff_failure() 0
#define trace_test_note_exit(parent, exitno) \
  ((void)(parent), (void)(exitno))
#endif

static int trace_scope_mark_pending(GCtrace *T)
{
  uint8_t flags = la_load8_acq(&T->unused1);
  while ((flags & TRACE_SCOPE_FLUSH_PENDING) == 0) {
    uint8_t next = (uint8_t)(flags | TRACE_SCOPE_FLUSH_PENDING);
    if (la_cas8(&T->unused1, &flags, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
  return 0;
}

/* Gate an optimized-next root without claiming a scoped-flush transaction.
** The VM may set this bit without the recorder token. A later token owner must
** still set TRACE_SCOPE_FLUSH_PENDING, close dependencies, and cross the
** EXIT_TRACES boundary before graph teardown. */
static int trace_entry_mark_invalidated(GCtrace *T)
{
  uint8_t flags = la_load8_acq(&T->unused1);
  while ((flags & TRACE_ENTRY_INVALIDATED) == 0) {
    uint8_t next = (uint8_t)(flags | TRACE_ENTRY_INVALIDATED);
    if (la_cas8(&T->unused1, &flags, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
  return 0;
}

/* Defined with the trace-graph mutation helpers below. Retirement publishes
** the body first, then disconnects every semantic entry edge while the same
** recorder-token owner still excludes assembler/link publication. */
static void trace_retire_disconnect(jit_State *J, GCtrace *T);

/* -- Error handling ------------------------------------------------------ */

TGState *lj_jit_owner_tg_l(lua_State *L, jit_State *J)
{
  global_State *g = J2G(J);
  TGState *tg;
  uint32_t owner;
  if (!L || G(L) != g)
    return NULL;
  /*
  ** Recorder ownership is tied to the lua_State that started recording. Do not
  ** fall back through ambient TLS here: a different thread can observe J->L and
  ** otherwise mistake its own TG for the active recorder owner.
  */
  owner = lj_state_owner_acq(L);
  if (!lj_thr_id_is_owner(owner))
    return NULL;
  tg = L->tg_hint;
  if (tg && tg->gl == g && lj_tg_tid_acq(tg) == owner &&
      lj_tg_owns_state_acq(tg, L))
    return tg;
  tg = lj_tg_find_owner(g, owner);
  return tg && lj_tg_owns_state_acq(tg, L) ? tg : NULL;
}

static LJ_AINLINE uint32_t jit_token_tid_l(lua_State *L, jit_State *J)
{
  TGState *tg = lj_jit_owner_tg_l(L, J);
  return tg ? lj_tg_tid_acq(tg) : 0;
}

int lj_jit_token_try(jit_State *J)
{
  global_State *g = J2G(J);
  TGState *tg = J2TG(J);
  uint32_t tid = tg ? lj_tg_tid_acq(tg) : 0;
  uint32_t expect = 0;
  if (tid == 0)
    return 0;
  return jit_token_cas(g, &expect, tid);
}

int lj_jit_token_try_l(lua_State *L, jit_State *J)
{
  global_State *g = J2G(J);
  uint32_t tid = jit_token_tid_l(L, J);
  uint32_t expect = 0;
  return tid != 0 && jit_token_cas(g, &expect, tid);
}

int lj_jit_token_held(jit_State *J)
{
  global_State *g = J2G(J);
  TGState *tg = J2TG(J);
  uint32_t tid = tg ? lj_tg_tid_acq(tg) : 0;
  return tid != 0 && jit_token_acq(g) == tid;
}

int lj_jit_token_held_l(lua_State *L, jit_State *J)
{
  uint32_t tid = jit_token_tid_l(L, J);
  return tid != 0 && jit_token_acq(J2G(J)) == tid;
}

void lj_jit_token_release(jit_State *J)
{
  global_State *g = J2G(J);
  TGState *tg = J2TG(J);
  uint32_t tid = tg ? lj_tg_tid_acq(tg) : 0;
  if (tid != 0 && jit_token_acq(g) == tid) {
    if (lj_trace_state_load(J) == LJ_TRACE_IDLE)
      jit_owner_l_rel(J, NULL);  /* Never publish an idle detachable state. */
    if (!jit_token_release_exact(g, tid)) {
      lj_assertJ(0, "recorder-token release lost exact owner word");
      abort();
    }
  }
}

void lj_jit_token_release_l(lua_State *L, jit_State *J)
{
  global_State *g = J2G(J);
  uint32_t tid = jit_token_tid_l(L, J);
  if (tid != 0 && jit_token_acq(g) == tid) {
    if (lj_trace_state_load(J) == LJ_TRACE_IDLE)
      jit_owner_l_rel(J, NULL);  /* The explicit L is valid for this release. */
    if (!jit_token_release_exact(g, tid)) {
      lj_assertJ(0, "recorder-token release lost exact owner word");
      abort();
    }
  }
}

/* Atomically make recorder scratch immutable across a future token-free VM
** event. IDLE is intentionally valid here for token-free flush callbacks.
** These primitives are deliberately not wired into callbacks until every
** waiting control caller recognizes lifecycle ownership and defers. */
int lj_jit_lifecycle_yield_l(lua_State *L, jit_State *J)
{
  global_State *g = J2G(J);
  uint32_t tid = jit_token_tid_l(L, J);
  LJJitOwnerWord old;
  if (tid == 0 || jit_owner_l_acq(J) != L)
    return 0;
  old = jit_owner_pack(tid, 0);
  return jit_owner_word_cas(g, &old, jit_owner_pack(0, tid));
}

int lj_jit_lifecycle_resume_l(lua_State *L, jit_State *J)
{
  global_State *g = J2G(J);
  uint32_t tid = jit_token_tid_l(L, J);
  LJJitOwnerWord old;
  if (tid == 0 || jit_owner_l_acq(J) != L)
    return 0;
  old = jit_owner_pack(0, tid);
  return jit_owner_word_cas(g, &old, jit_owner_pack(tid, 0));
}

int lj_jit_lifecycle_held_l(lua_State *L, jit_State *J)
{
  uint32_t tid = jit_token_tid_l(L, J);
  return tid != 0 && jit_owner_l_acq(J) == L &&
    jit_owner_word_acq(J2G(J)) == jit_owner_pack(0, tid);
}

/* -- Rooted immutable JIT event sessions ------------------------------- */

static int jit_event_span_valid(uintptr_t base, uint32_t size,
				const LJJitEventFrozenSpan *span,
				uint32_t stride, uint32_t alignment)
{
  uintptr_t address, bytes;
  uint32_t available;
  if (span->stride != stride || span->offset > size ||
      (alignment != 0 && span->offset % alignment) != 0)
    return 0;
  if ((uintptr_t)span->offset > ~(uintptr_t)0 - base)
    return 0;
  address = base + (uintptr_t)span->offset;
  if (alignment != 0 && address % alignment != 0)
    return 0;
  if (span->count == 0)
    return 1;
  available = size - span->offset;
  if (span->count > available / span->stride)
    return 0;
  bytes = (uintptr_t)span->count * (uintptr_t)span->stride;
  return bytes <= ~(uintptr_t)0 - address;
}

static int jit_event_view_geometry_valid(
  const void *data, uint32_t size, uint32_t format, uint32_t flags,
  const LJJitEventFrozenTraceHeader *trace,
  const LJJitEventFrozenSpan *ir, const LJJitEventFrozenSpan *snap,
  const LJJitEventFrozenSpan *snapmap)
{
  uintptr_t base = (uintptr_t)data;
  uint64_t irend, snapend, snapmapend;
  if (!data || size == 0 || (uintptr_t)size > ~(uintptr_t)0 - base ||
      format != LJ_JIT_EVENT_VIEW_FORMAT_TRACE_V1 || flags != 0 ||
      trace->version != LJ_JIT_EVENT_FROZEN_TRACE_VERSION ||
      trace->flags != 0 || trace->traceno == 0 || trace->nins < trace->nk ||
      trace->nk > REF_BASE || trace->nins < REF_BASE ||
      trace->linktype > LJ_TRLINK_STITCH ||
      trace->ir_ref_first != trace->nk ||
      trace->ir_ref_count != trace->nins - trace->nk ||
      trace->ir_ref_count != ir->count || trace->nsnap != snap->count ||
      trace->nsnapmap != snapmap->count || trace->nexits != trace->nsnap ||
      trace->mcloop > trace->szmcode ||
      ((trace->mcode_addr == 0) != (trace->szmcode == 0)) ||
      (trace->mcode_addr == 0 &&
       (trace->mcloop != 0 || trace->exitstub_addr != 0)) ||
      !jit_event_span_valid(base, size, ir, sizeof(IRIns),
			    __alignof__(IRIns)) ||
      !jit_event_span_valid(base, size, snap, sizeof(SnapShot),
			    __alignof__(SnapShot)) ||
      !jit_event_span_valid(base, size, snapmap, sizeof(SnapEntry),
			    __alignof__(SnapEntry)))
    return 0;
  irend = (uint64_t)ir->offset + (uint64_t)ir->count * ir->stride;
  snapend = (uint64_t)snap->offset + (uint64_t)snap->count * snap->stride;
  snapmapend = (uint64_t)snapmap->offset +
    (uint64_t)snapmap->count * snapmap->stride;
  return irend <= snap->offset && snapend <= snapmap->offset &&
    snapmapend <= size;
}

void lj_jit_event_snapshot_copy_canonical(SnapShot *dst,
					  const SnapShot *src)
{
  uint32_t mapofs;
  uint16_t ref, mcofs;
  uint8_t nslots, topslot, nent, count;
  if (LJ_UNLIKELY(!dst || !src || dst == src))
    abort();
  /* Read every live field independently. In particular, count is changed by
  ** native exits and must never participate in a plain structure load. */
  mapofs = (uint32_t)snap_mapofs_acq(src);
  ref = (uint16_t)snap_ref_acq(src);
  mcofs = (uint16_t)snap_mcofs_acq(src);
  nslots = (uint8_t)snap_nslots_acq(src);
  topslot = (uint8_t)snap_topslot_acq(src);
  nent = (uint8_t)snap_nent_acq(src);
  count = (uint8_t)snap_count_acq(src);
  /* The destination is private frozen backing. Zeroing first canonicalizes any
  ** present or future padding; release stores then prevent a compiler from
  ** coalescing the live-count access with adjacent fields. */
  memset(dst, 0, sizeof(*dst));
  la_store32_rel(&dst->mapofs, mapofs);
  la_store16_rel(&dst->ref, ref);
  la_store16_rel(&dst->mcofs, mcofs);
  la_store8_rel(&dst->nslots, nslots);
  la_store8_rel(&dst->topslot, topslot);
  la_store8_rel(&dst->nent, nent);
  snap_count_rel(dst, count);
}

int lj_jit_event_snapshot_matches_live(const SnapShot *frozen,
					const SnapShot *live)
{
  return frozen && live &&
    snap_mapofs_acq(frozen) == snap_mapofs_acq(live) &&
    snap_ref_acq(frozen) == snap_ref_acq(live) &&
    snap_mcofs_acq(frozen) == snap_mcofs_acq(live) &&
    snap_nslots_acq(frozen) == snap_nslots_acq(live) &&
    snap_topslot_acq(frozen) == snap_topslot_acq(live) &&
    snap_nent_acq(frozen) == snap_nent_acq(live);
}

int lj_jit_event_frozen_view_valid(const LJJitEventFrozenView *view)
{
  return view && view->data && view->size <= view->capacity &&
    jit_event_view_geometry_valid(view->data, view->size, view->format,
				  view->flags,
				  &view->trace, &view->ir, &view->snap,
				  &view->snapmap);
}

int lj_jit_event_session_contract_valid(uint32_t event, uint32_t owner_mode,
					uint32_t edge_proof, int has_view,
					int has_source,
					uint32_t root_count,
					uint32_t attachment_state,
					uint64_t attachment_generation,
					uint32_t callback_root_count)
{
  if (!lj_vmevent_attachment_identity_valid(attachment_state,
						    attachment_generation) ||
      callback_root_count > 1u)
    return 0;
  if (owner_mode == LJ_JIT_EVENT_OWNER_CONTINUATION_LIFECYCLE) {
    return (event == LJ_JIT_EVENT_TRACE_START ||
	    event == LJ_JIT_EVENT_RECORD) &&
      edge_proof == LJ_JIT_EVENT_EDGE_EXACT_ROOTS && has_view &&
      !has_source && root_count != 0;
  }
  if (owner_mode != LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE)
    return 0;
  switch (event) {
  case LJ_JIT_EVENT_TRACE_STOP:
    return edge_proof == LJ_JIT_EVENT_EDGE_PINNED_SOURCE && has_view &&
      has_source && root_count == 0;
  case LJ_JIT_EVENT_TRACE_ABORT:
    return has_view &&
      ((edge_proof == LJ_JIT_EVENT_EDGE_PINNED_SOURCE && has_source &&
	root_count == 0) ||
       (edge_proof == LJ_JIT_EVENT_EDGE_EXACT_ROOTS && !has_source &&
	root_count != 0));
  case LJ_JIT_EVENT_TRACE_FLUSH:
    return edge_proof == LJ_JIT_EVENT_EDGE_NONE && !has_view &&
      !has_source && root_count == 0;
  default:
    return 0;
  }
}

static int jit_event_view_spec_valid(const LJJitEventFrozenViewSpec *view)
{
  return !view || (view->data &&
    jit_event_view_geometry_valid(view->data, view->size, view->format,
				  view->flags,
				  &view->trace, &view->ir, &view->snap,
				  &view->snapmap));
}

static TGState *jit_event_owner_tg(lua_State *L)
{
  TGState *tg;
  global_State *g;
  uint32_t actor;
  if (!L || !(g = G(L)) || !(tg = L->tg_hint) || tg->gl != g ||
      lj_tg_flags_test_acq(tg, TGF_DEAD) ||
      lj_tg_load_cur_L(tg) != L || !lj_tg_owns_state_acq(tg, L))
    return NULL;
  actor = lj_thr_actor_current();
  return actor != 0 && lj_tg_actor_acq(tg) == actor ? tg : NULL;
}

static int jit_event_root_need(uint32_t proof_count, uint32_t *need)
{
  if (!need || proof_count > LJ_ROOT_SCAN_LIMIT ||
      proof_count == ~(uint32_t)0)
    return 0;
  *need = proof_count + 1u;  /* Dedicated callback-root sentinel lane. */
  return *need > proof_count;
}

static int jit_event_size_mul(uint32_t count, size_t width, size_t *bytes)
{
  if (!bytes || width == 0 || (size_t)count > ~(size_t)0 / width)
    return 0;
  *bytes = (size_t)count * width;
  return 1;
}

static int jit_event_slot_root_geometry(const LJJitEventSessionSlot *slot,
					GCRef **rootsp, uint32_t *nrootsp,
					uint32_t *capacityp)
{
  GCRef *roots;
  uint32_t nroots, capacity;
  if (!slot)
    return 0;
  roots = (GCRef *)la_loadptr_acq((void *const *)&slot->root_data);
  nroots = la_load32_acq(&slot->root_count);
  capacity = la_load32_acq(&slot->root_capacity);
  if (!roots || ((uintptr_t)roots & (__alignof__(GCRef) - 1u)) != 0 ||
      capacity < LJ_JIT_EVENT_SESSION_ROOTS || nroots >= capacity ||
      nroots > LJ_ROOT_SCAN_LIMIT ||
      ((roots == slot->root_inline) !=
	(capacity == LJ_JIT_EVENT_SESSION_ROOTS)))
    return 0;
  if (rootsp) *rootsp = roots;
  if (nrootsp) *nrootsp = nroots;
  if (capacityp) *capacityp = capacity;
  return 1;
}

static GCfunc *jit_event_callback_handler_acq(GCRef *roots, uint32_t nroots)
{
  return (GCfunc *)(void *)gcref_acq(roots[nroots]);
}

static int jit_event_callback_handler_type(global_State *g, GCfunc *handler)
{
  LJGC2Lease lease;
  int status;
  if (!handler)
    return 1;
  status = lj_gc2_obj_lease_acquire(g, obj2gco(handler),
				    (uint32_t)~LJ_TFUNC, NULL, &lease);
  if (status < 0)
    return 0;
  lj_gc2_lease_release(&lease);
  return 1;
}

static void jit_event_slot_reset_fields(LJJitEventSessionSlot *slot)
{
  GCRef *roots;
  uint32_t nroots;
  uint32_t i;
  /* CLEANING owns the slot, but corrupt geometry still must fail-stop before
  ** the first sentinel/proof-vector write. */
  if (LJ_UNLIKELY(!jit_event_slot_root_geometry(
	  slot, &roots, &nroots, NULL)))
    abort();
  la_store64_rel(&slot->generation, 0);
  la_store32_rel(&slot->flags, 0);
  la_store32_rel(&slot->event, 0);
  la_store32_rel(&slot->owner_mode, 0);
  la_store32_rel(&slot->edge_proof, 0);
  la_store64_rel(&slot->attachment_generation, 0);
  la_store32_rel(&slot->attachment_state, LJ_VMEVENT_ATTACHMENT_INVALID);
  la_store32_rel(&slot->callback_root_count, 0);
  la_store32_rel(&slot->control_borrow_state, 0);
  la_store64_rel(&slot->control_borrow_generation, 0);
  la_storeptr_rel((void **)&slot->saved_jit_owner_L, NULL);
  la_store32_rel(&slot->owner_tid, 0);
  la_store32_rel(&slot->owner_actor, 0);
  la_storeptr_rel((void **)&slot->owner_L, NULL);
  setgcrefrel(slot->owner_root, NULL);
  la_storeptr_rel((void **)&slot->source, NULL);
  la_store32_rel(&slot->source_traceno, 0);
  /* Clear the callback lane independently: it is not one of nroots frozen
  ** proof edges and must not survive CLOSED-to-FREE reuse. */
  setgcrefrel(roots[nroots], NULL);
  for (i = 0; i < nroots; i++)
    setgcrefrel(roots[i], NULL);
  la_store32_rel(&slot->root_count, 0);
  slot->view.size = 0;
  slot->view.format = LJ_JIT_EVENT_VIEW_FORMAT_NONE;
  slot->view.flags = 0;
  memset(&slot->view.trace, 0, sizeof(slot->view.trace));
  memset(&slot->view.ir, 0, sizeof(slot->view.ir));
  memset(&slot->view.snap, 0, sizeof(slot->view.snap));
  memset(&slot->view.snapmap, 0, sizeof(slot->view.snapmap));
}

static int jit_event_slot_cleanup(global_State *g,
				  LJJitEventSessionSlot *slot,
				  uint32_t expected_state)
{
  uint32_t state = expected_state;
  uint32_t flags;
  GCtrace *source;
  if (la_load32_acq(&slot->readers) != 0 ||
      !la_cas32(&slot->state, &state, LJ_JIT_EVENT_SLOT_CLEANING,
		LA_ACQ_REL, LA_ACQ))
    return 0;
  /* No admitted reader can name the immutable fields after CLEANING wins.
  ** A late pre-close reader may transiently increment readers, but its global
  ** sequence recheck fails before the first field dereference. */
  flags = la_load32_acq(&slot->flags);
  source = (GCtrace *)la_loadptr_acq((void *const *)&slot->source);
  if ((flags & LJ_JIT_EVENT_SLOT_F_SOURCE_PIN) != 0) {
    if (LJ_UNLIKELY(!source))
      abort();
    lj_trace_native_unpin(g, source);
  }
  jit_event_slot_reset_fields(slot);
  la_store32_rel(&slot->state, LJ_JIT_EVENT_SLOT_FREE);
  return 1;
}

static void jit_event_slot_reader_drop(TGState *tg,
				       LJJitEventSessionSlot *slot)
{
  uint32_t old = la_sub32_acqrel(&slot->readers, 1);
  if (LJ_UNLIKELY(old == 0))
    abort();
  if (old == 1 && la_load32_acq(&slot->state) == LJ_JIT_EVENT_SLOT_CLOSED)
    (void)jit_event_slot_cleanup(tg->gl, slot, LJ_JIT_EVENT_SLOT_CLOSED);
}

static int jit_event_slot_claim(global_State *g, LJJitEventSessions *sessions,
				uint32_t *slot_index)
{
  uint32_t i;
  for (i = 0; i < LJ_JIT_EVENT_SESSION_SLOTS; i++) {
    LJJitEventSessionSlot *slot = &sessions->slot[i];
    uint32_t state = la_load32_acq(&slot->state);
    if (state == LJ_JIT_EVENT_SLOT_CLOSED &&
	la_load32_acq(&slot->readers) == 0) {
      (void)jit_event_slot_cleanup(g, slot, LJ_JIT_EVENT_SLOT_CLOSED);
      state = la_load32_acq(&slot->state);
    }
    if (state == LJ_JIT_EVENT_SLOT_FREE) {
      uint32_t expect = LJ_JIT_EVENT_SLOT_FREE;
      if (la_cas32(&slot->state, &expect, LJ_JIT_EVENT_SLOT_BUILDING,
		   LA_ACQ_REL, LA_ACQ)) {
	/* A stale reader is allowed to arrive after cleanup, but cannot pass its
	** publication recheck.  Do not overwrite its slot until it leaves. */
	if (la_load32_acq(&slot->readers) == 0) {
	  *slot_index = i;
	  return 1;
	}
	la_store32_rel(&slot->state, LJ_JIT_EVENT_SLOT_FREE);
      }
    }
  }
  return 0;
}

static int jit_event_view_matches_source(const LJJitEventFrozenViewSpec *view,
					 GCtrace *source, TraceNo traceno)
{
  const LJJitEventFrozenTraceHeader *h;
  const unsigned char *data;
  const IRIns *source_ir;
  const SnapShot *source_snap;
  const SnapEntry *source_snapmap;
  GCproto *pt;
  const BCIns *pc, *bc;
  uintptr_t pca, bca, bce;
  uint32_t pcpos, i;
  if (!view)
    return 1;
  h = &view->trace;
  data = (const unsigned char *)view->data;
  pt = trace_startpt_acq(source);
  pc = trace_startpc_acq(source);
  if (!pt || !pc)
    return 0;
  bc = proto_bc(pt);
  pca = (uintptr_t)pc;
  bca = (uintptr_t)bc;
  bce = bca + (uintptr_t)pt->sizebc * sizeof(BCIns);
  if (bce < bca || pca < bca || pca >= bce ||
      (pca - bca) % sizeof(BCIns) != 0)
    return 0;
  pcpos = (uint32_t)((pca - bca) / sizeof(BCIns));
  /* Prove every live allocation bound before using frozen counts to index the
  ** source arrays. Geometry alone constrains only the copied byte buffer. */
  if (h->traceno != traceno || h->root != trace_root_acq(source) ||
      h->link != trace_link_acq(source) ||
      h->linktype != (uint32_t)trace_linktype_acq(source) ||
      h->nins != (uint32_t)trace_nins_acq(source) ||
      h->nk != (uint32_t)trace_nk_acq(source) ||
      h->nsnap != (uint32_t)trace_nsnap_acq(source) ||
      h->nsnapmap != (uint32_t)trace_nsnapmap_acq(source) ||
      h->startpc_pos != pcpos || h->startins != trace_startins_acq(source) ||
      h->mcode_addr != (uint64_t)(uintptr_t)trace_mcode_acq(source) ||
      h->szmcode != (uint32_t)trace_szmcode_acq(source) ||
      h->mcloop != (uint32_t)trace_mcloop_acq(source) ||
      h->exitstub_addr !=
	(uint64_t)(uintptr_t)trace_exitstub_acq(source) ||
      h->nexits != (uint32_t)trace_nsnap_acq(source))
    return 0;
  source_ir = trace_ir_acq(source);
  source_snap = trace_snap_acq(source);
  source_snapmap = trace_snapmap_acq(source);
  if ((view->ir.count != 0 && !source_ir) ||
      (view->snap.count != 0 && !source_snap) ||
      (view->snapmap.count != 0 && !source_snapmap))
    return 0;
  for (i = 0; i < view->snap.count; i++) {
    const SnapShot *frozen =
      (const SnapShot *)(const void *)(data + view->snap.offset) + i;
    if (!lj_jit_event_snapshot_matches_live(frozen, source_snap + i))
      return 0;
  }
  return (view->ir.count == 0 ||
     memcmp(data + view->ir.offset, source_ir + h->nk,
	    (size_t)view->ir.count * sizeof(IRIns)) == 0) &&
    (view->snapmap.count == 0 ||
     memcmp(data + view->snapmap.offset, source_snapmap,
	    (size_t)view->snapmap.count * sizeof(SnapEntry)) == 0);
}

static int jit_event_source_pin(global_State *g, GCtrace *source,
				TraceNo traceno,
				const LJJitEventFrozenViewSpec *view)
{
  jit_State *J = G2J(g);
  GCtrace *published;
  int pinned = 0;
  if (!source || traceno == 0 || !lj_gc2_smr_read_try(g))
    return 0;
  published = traceref_safe(J, traceno);
  if (published == source && lj_trace_native_pin(source)) {
    published = traceref_safe(J, traceno);
	if (published == source && jit_event_view_matches_source(view, source,
							traceno) &&
	lj_trace_native_mark_pinned(g, source, traceno))
      pinned = 1;
    if (!pinned)
      lj_trace_native_unpin(g, source);
  }
  lj_gc2_smr_read_leave(g);
  return pinned;
}

void lj_jit_event_sessions_init(TGState *tg)
{
  LJJitEventSessions *sessions;
  uint32_t i, j;
  if (LJ_UNLIKELY(!tg))
    abort();
  sessions = &tg->jit_event_sessions;
  memset(sessions, 0, sizeof(*sessions));
  /* Only g->main_tg's copy is the universe descriptor. Initializing every
  ** append-only TG copy here keeps secondary allocation/bootstrap symmetric
  ** without ever publishing those unused copies. */
  memset(&tg->jit_trace_stream, 0, sizeof(tg->jit_trace_stream));
  /* As with the stream, only the main-TG attachment-clock array is an
  ** authority.  Production jit.attach() does not publish it yet. */
  memset(tg->jit_event_attachment, 0, sizeof(tg->jit_event_attachment));
  la_store32_rlx(&sessions->active_slot, LJ_JIT_EVENT_SESSION_SLOTS);
  for (i = 0; i < LJ_JIT_EVENT_SESSION_SLOTS; i++) {
    LJJitEventSessionSlot *slot = &sessions->slot[i];
    for (j = 0; j < LJ_JIT_EVENT_SESSION_ROOTS; j++)
      setgcrefrel(slot->root_inline[j], NULL);
    la_storeptr_rlx((void **)&slot->root_data, slot->root_inline);
    la_store32_rlx(&slot->root_capacity, LJ_JIT_EVENT_SESSION_ROOTS);
    la_store32_rlx(&slot->callback_root_count, 0);
    la_store32_rlx(&slot->attachment_state,
		   LJ_VMEVENT_ATTACHMENT_INVALID);
    setgcrefrel(slot->owner_root, NULL);
    la_store32_rlx(&slot->state, LJ_JIT_EVENT_SLOT_FREE);
  }
}

int lj_jit_event_sessions_quiescent(TGState *tg)
{
  LJJitEventSessions *sessions;
  uint64_t sequence;
  uint32_t i;
  if (!tg ||
      !lj_jit_event_callback_idle(tg) ||
      (tg->gl && tg == tg->gl->main_tg &&
       !lj_jit_trace_stream_idle(tg->gl)) ||
      lj_jit_trace_stream_names_tg(tg->gl, tg))
    return 0;
  sessions = &tg->jit_event_sessions;
  sequence = la_load64_acq(&sessions->sequence);
  if ((sequence & 1u) != 0 ||
      la_load32_acq(&sessions->state) != LJ_JIT_EVENT_PUBLICATION_IDLE ||
      la_load32_acq(&sessions->active_slot) != LJ_JIT_EVENT_SESSION_SLOTS ||
      la_load64_acq(&sessions->active_generation) != 0)
    return 0;
  for (i = 0; i < LJ_JIT_EVENT_SESSION_SLOTS; i++) {
    LJJitEventSessionSlot *slot = &sessions->slot[i];
    uint32_t state = la_load32_acq(&slot->state);
    if (state == LJ_JIT_EVENT_SLOT_CLOSED &&
	la_load32_acq(&slot->readers) == 0) {
      (void)jit_event_slot_cleanup(tg->gl, slot, LJ_JIT_EVENT_SLOT_CLOSED);
      state = la_load32_acq(&slot->state);
    }
    if (state != LJ_JIT_EVENT_SLOT_FREE ||
	la_load32_acq(&slot->readers) != 0)
      return 0;
  }
  return !(tg->gl && tg == tg->gl->main_tg &&
	   !lj_jit_trace_stream_idle(tg->gl)) &&
    !lj_jit_trace_stream_names_tg(tg->gl, tg) &&
    la_load64_acq(&sessions->sequence) == sequence &&
    la_load32_acq(&sessions->state) == LJ_JIT_EVENT_PUBLICATION_IDLE &&
    la_load32_acq(&sessions->active_slot) == LJ_JIT_EVENT_SESSION_SLOTS &&
    la_load64_acq(&sessions->active_generation) == 0;
}

int lj_jit_event_sessions_logical_detach_ready(TGState *tg)
{
  LJJitEventSessions *sessions;
  uint64_t sequence;
  uint32_t i;
  if (!tg ||
      !lj_jit_event_callback_idle(tg) ||
      (tg->gl && tg == tg->gl->main_tg &&
       !lj_jit_trace_stream_idle(tg->gl)) ||
      lj_jit_trace_stream_names_tg(tg->gl, tg))
    return 0;
  sessions = &tg->jit_event_sessions;
  sequence = la_load64_acq(&sessions->sequence);
  if ((sequence & 1u) != 0 ||
      la_load32_acq(&sessions->state) != LJ_JIT_EVENT_PUBLICATION_IDLE ||
      la_load32_acq(&sessions->active_slot) != LJ_JIT_EVENT_SESSION_SLOTS ||
      la_load64_acq(&sessions->active_generation) != 0)
    return 0;
  for (i = 0; i < LJ_JIT_EVENT_SESSION_SLOTS; i++) {
    uint32_t state = la_load32_acq(&sessions->slot[i].state);
    /* CLEANING is either this single actor's completed-before-detach path or a
    ** last snapshot reader which still owns GC2 SMR. In the latter case TG
    ** physical reclaim cannot pass until cleanup and read-leave both finish. */
    if (state != LJ_JIT_EVENT_SLOT_FREE &&
	state != LJ_JIT_EVENT_SLOT_CLOSED &&
	state != LJ_JIT_EVENT_SLOT_CLEANING)
      return 0;
  }
  return !(tg->gl && tg == tg->gl->main_tg &&
	   !lj_jit_trace_stream_idle(tg->gl)) &&
    !lj_jit_trace_stream_names_tg(tg->gl, tg) &&
    la_load64_acq(&sessions->sequence) == sequence &&
    la_load32_acq(&sessions->state) == LJ_JIT_EVENT_PUBLICATION_IDLE &&
    la_load32_acq(&sessions->active_slot) == LJ_JIT_EVENT_SESSION_SLOTS &&
    la_load64_acq(&sessions->active_generation) == 0;
}

int lj_jit_event_sessions_detach_ready(TGState *tg)
{
  LJJitOwnerWord owner_word;
  uint32_t tid;
  if (!tg || !(tid = lj_tg_tid_acq(tg)))
    return 0;
  owner_word = jit_owner_word_acq(tg->gl);
  return jit_owner_token(owner_word) != tid &&
    jit_owner_lifecycle(owner_word) != tid &&
    lj_jit_event_sessions_quiescent(tg);
}

int lj_jit_event_sessions_fini(global_State *g, TGState *tg)
{
  LJJitEventSessions *sessions;
  uint64_t sequence;
  uint32_t i;
  if (!g || !tg || tg->gl != g || !lj_jit_event_callback_idle(tg) ||
      (tg == g->main_tg && !lj_jit_trace_stream_idle(g)) ||
      lj_jit_trace_stream_names_tg(g, tg))
    return 0;
  sessions = &tg->jit_event_sessions;
  sequence = la_load64_acq(&sessions->sequence);
  /* Physical finalization may follow a DEAD TG whose owner word was already
  ** cleared. Secondary logical detach may leave CLOSED readers under their SMR
  ** leases; physical finalization repeats strict slot quiescence so post-DEAD
  ** cleanup remains retryable and idempotent. */
  if ((sequence & 1u) != 0 ||
      la_load32_acq(&sessions->state) != LJ_JIT_EVENT_PUBLICATION_IDLE ||
      la_load32_acq(&sessions->active_slot) != LJ_JIT_EVENT_SESSION_SLOTS ||
      la_load64_acq(&sessions->active_generation) != 0)
    return 0;
  for (i = 0; i < LJ_JIT_EVENT_SESSION_SLOTS; i++) {
    LJJitEventSessionSlot *slot = &sessions->slot[i];
    uint32_t state = la_load32_acq(&slot->state);
    if (state == LJ_JIT_EVENT_SLOT_CLOSED &&
	la_load32_acq(&slot->readers) == 0) {
      (void)jit_event_slot_cleanup(g, slot, LJ_JIT_EVENT_SLOT_CLOSED);
      state = la_load32_acq(&slot->state);
    }
    if (state != LJ_JIT_EVENT_SLOT_FREE ||
	la_load32_acq(&slot->readers) != 0)
      return 0;
  }
  if (la_load64_acq(&sessions->sequence) != sequence ||
      la_load32_acq(&sessions->state) != LJ_JIT_EVENT_PUBLICATION_IDLE ||
      la_load32_acq(&sessions->active_slot) != LJ_JIT_EVENT_SESSION_SLOTS ||
      la_load64_acq(&sessions->active_generation) != 0 ||
      (tg == g->main_tg && !lj_jit_trace_stream_idle(g)) ||
      lj_jit_trace_stream_names_tg(g, tg))
    return 0;
  for (i = 0; i < LJ_JIT_EVENT_SESSION_SLOTS; i++) {
    LJJitEventFrozenView *view = &sessions->slot[i].view;
    LJJitEventSessionSlot *slot = &sessions->slot[i];
    GCRef *roots = (GCRef *)
      la_loadptr_acq((void *const *)&slot->root_data);
    free(view->data);
    view->data = NULL;
    view->capacity = 0;
    if (roots != slot->root_inline)
      free(roots);
    la_storeptr_rel((void **)&slot->root_data, slot->root_inline);
    la_store32_rel(&slot->root_capacity, LJ_JIT_EVENT_SESSION_ROOTS);
  }
  return 1;
}

static int jit_event_slot_root_reserve(LJJitEventSessionSlot *slot,
				       uint32_t need)
{
  GCRef *roots;
  uint32_t capacity;
  uint32_t next, i;
  size_t bytes;
  void *data;
  if (need == 0 ||
      !jit_event_slot_root_geometry(slot, &roots, NULL, &capacity))
    return 0;
  if (need <= capacity)
    return 1;
  next = capacity ? capacity : LJ_JIT_EVENT_SESSION_ROOTS;
  while (next < need) {
    if (next > ~(uint32_t)0 / 2u) {
      next = need;
      break;
    }
    next *= 2u;
  }
  if (!jit_event_size_mul(next, sizeof(GCRef), &bytes))
    return 0;
  if (roots == slot->root_inline) {
    data = malloc(bytes);
    if (data)
      for (i = 0; i < next; i++)
        setgcrefrel(((GCRef *)data)[i], NULL);
  } else {
    data = realloc(roots, bytes);
    if (data)
      for (i = capacity; i < next; i++)
        setgcrefrel(((GCRef *)data)[i], NULL);
  }
  if (!data)
    return 0;
  la_storeptr_rel((void **)&slot->root_data, data);
  la_store32_rel(&slot->root_capacity, next);
  return 1;
}

static int jit_event_ranges_overlap(const void *a, size_t asize,
				    const void *b, size_t bsize)
{
  uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
  uintptr_t ae, be;
  if (!a || !b || asize == 0 || bsize == 0)
    return 0;
  if (asize > ~(uintptr_t)0 - ap || bsize > ~(uintptr_t)0 - bp)
    return 1;
  ae = ap + asize;
  be = bp + bsize;
  return ap < be && bp < ae;
}

static int jit_event_inputs_alias_retained(
  const LJJitEventSessions *sessions, const LJJitEventSessionSpec *spec)
{
  size_t input_root_bytes;
  uint32_t i;
  if (!jit_event_size_mul(spec->root_count, sizeof(*spec->roots),
			  &input_root_bytes))
    return 1;
  for (i = 0; i < LJ_JIT_EVENT_SESSION_SLOTS; i++) {
    const LJJitEventSessionSlot *slot = &sessions->slot[i];
    GCRef *roots;
    uint32_t capacity;
    size_t root_bytes;
    if (!jit_event_slot_root_geometry(slot, &roots, NULL, &capacity) ||
	!jit_event_size_mul(capacity, sizeof(*roots), &root_bytes))
      return 1;
    if (spec->root_count != 0 &&
	(jit_event_ranges_overlap(spec->roots, input_root_bytes,
				  roots, root_bytes) ||
	 jit_event_ranges_overlap(spec->roots, input_root_bytes,
				  slot->view.data, slot->view.capacity)))
      return 1;
    if (spec->view &&
	(jit_event_ranges_overlap(spec->view->data, spec->view->size,
				  roots, root_bytes) ||
	 jit_event_ranges_overlap(spec->view->data, spec->view->size,
				  slot->view.data, slot->view.capacity)))
      return 1;
  }
  return 0;
}

static int jit_event_session_publish_l(lua_State *L, jit_State *J,
				       const LJJitEventSessionSpec *spec,
				       LJJitEventSessionHandle *handle)
{
  TGState *tg = jit_event_owner_tg(L);
  global_State *g;
  LJJitEventSessions *sessions;
  LJJitEventSessionSlot *slot;
  GCRef *slot_roots;
  LJGC2Lease callback_lease;
  uint64_t sequence, generation;
  uint32_t slot_index, i, tid, flags = 0, root_need;
  int callback_lease_held = 0;
  if (!J || !tg || G(L) != J2G(J) || !spec || !handle ||
      !jit_event_root_need(spec->root_count, &root_need) ||
      (spec->root_count != 0 && !spec->roots) ||
      spec->callback_root_count > 1u ||
      ((spec->callback_handler != NULL) !=
	(spec->callback_root_count == 1u)) ||
      (!!spec->source != (spec->source_traceno != 0)) ||
      !lj_jit_event_session_contract_valid(spec->event, spec->owner_mode,
					  spec->edge_proof,
					  spec->view != NULL,
					  spec->source != NULL,
					  spec->root_count,
					  spec->attachment_state,
					  spec->attachment_generation,
					  spec->callback_root_count) ||
      !jit_event_view_spec_valid(spec->view))
    return 0;
  if (spec->event == LJ_JIT_EVENT_TRACE_ABORT &&
      spec->owner_mode == LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE &&
      spec->edge_proof == LJ_JIT_EVENT_EDGE_EXACT_ROOTS &&
      (spec->view->trace.mcode_addr != 0 ||
       spec->view->trace.szmcode != 0 || spec->view->trace.mcloop != 0 ||
       spec->view->trace.exitstub_addr != 0))
    return 0;  /* No native/source lease survives detached low->zero. */
  g = tg->gl;
  /* Admission precedes the first handler-body read and remains held until the
  ** sentinel, even publication and second active-cycle barrier are durable.
  ** Besides lifetime, the exact lease rejects stale, foreign and non-FUNC
  ** candidates without peeking through an unauthorised raw pointer. */
  if (spec->callback_handler) {
    if (lj_gc2_obj_lease_acquire(g, obj2gco(spec->callback_handler),
				 (uint32_t)~LJ_TFUNC, NULL,
				 &callback_lease) < 0)
      return 0;
    callback_lease_held = 1;
  }
  tid = lj_tg_tid_acq(tg);
  /* Building the immutable continuation is a recorder/control operation. The
  ** future callback cutover publishes it completely before yielding this exact
  ** low-half token into the high-half lifecycle reservation. */
  if (tid == 0 || jit_owner_word_acq(g) != jit_owner_pack(tid, 0) ||
      jit_owner_l_acq(J) != L)
    goto fail_callback_lease;
  sessions = &tg->jit_event_sessions;
  /* This must precede even the first root-element load: a CLOSED slot's last
  ** reader may concurrently zero its retained vector. */
  if (jit_event_inputs_alias_retained(sessions, spec))
    goto fail_callback_lease;
  for (i = 0; i < spec->root_count; i++)
    if (!spec->roots[i])
      goto fail_callback_lease;
  sequence = la_load64_acq(&sessions->sequence);
  /* Reserve sequence headroom for both this even->even publication and the
  ** matching close. An ACTIVE session at MAX-1 could never be unpublished. */
  if ((sequence & 1u) != 0 || sequence > ~(uint64_t)4 ||
      la_load32_acq(&sessions->state) != LJ_JIT_EVENT_PUBLICATION_IDLE ||
      la_load32_acq(&sessions->active_slot) != LJ_JIT_EVENT_SESSION_SLOTS ||
      la_load64_acq(&sessions->active_generation) != 0 ||
      !jit_event_slot_claim(g, sessions, &slot_index))
    goto fail_callback_lease;
  slot = &sessions->slot[slot_index];
  generation = la_load64_acq(&sessions->next_generation);
  if (generation == ~(uint64_t)0) {
    la_store32_rel(&slot->state, LJ_JIT_EVENT_SLOT_FREE);
    goto fail_callback_lease;
  }
  generation++;
  if (!jit_event_slot_root_reserve(slot, root_need)) {
    la_store32_rel(&slot->state, LJ_JIT_EVENT_SLOT_FREE);
    goto fail_callback_lease;
  }
  if (spec->source &&
      spec->view->trace.traceno != spec->source_traceno) {
    la_store32_rel(&slot->state, LJ_JIT_EVENT_SLOT_FREE);
    goto fail_callback_lease;
  }
  if (spec->view && spec->view->size > slot->view.capacity) {
    void *data = realloc(slot->view.data, spec->view->size);
    if (!data) {
      la_store32_rel(&slot->state, LJ_JIT_EVENT_SLOT_FREE);
      goto fail_callback_lease;
    }
    slot->view.data = data;
    slot->view.capacity = spec->view->size;
  }
  if (spec->source &&
      !jit_event_source_pin(g, spec->source, spec->source_traceno,
			    spec->view)) {
    la_store32_rel(&slot->state, LJ_JIT_EVENT_SLOT_FREE);
    goto fail_callback_lease;
  }
  if (spec->source)
    flags |= LJ_JIT_EVENT_SLOT_F_SOURCE_PIN;
  if (spec->callback_root_count != 0)
    flags |= LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT;
  if (spec->view) {
    memcpy(slot->view.data, spec->view->data, spec->view->size);
    slot->view.size = spec->view->size;
    slot->view.format = spec->view->format;
    slot->view.flags = spec->view->flags;
    slot->view.trace = spec->view->trace;
    slot->view.ir = spec->view->ir;
    slot->view.snap = spec->view->snap;
    slot->view.snapmap = spec->view->snapmap;
    flags |= LJ_JIT_EVENT_SLOT_F_VIEW;
  }
  la_store64_rel(&slot->generation, generation);
  la_store32_rel(&slot->flags, flags);
  la_store32_rel(&slot->event, spec->event);
  la_store32_rel(&slot->owner_mode, spec->owner_mode);
  la_store32_rel(&slot->edge_proof, spec->edge_proof);
  la_store64_rel(&slot->attachment_generation,
		 spec->attachment_generation);
  la_store32_rel(&slot->attachment_state, spec->attachment_state);
  la_store32_rel(&slot->control_borrow_state, 0);
  la_store64_rel(&slot->control_borrow_generation, 0);
  la_storeptr_rel((void **)&slot->saved_jit_owner_L, NULL);
  la_store32_rel(&slot->root_count, spec->root_count);
  la_store32_rel(&slot->owner_tid, tid);
  la_store32_rel(&slot->owner_actor, lj_tg_actor_acq(tg));
  la_storeptr_rel((void **)&slot->owner_L, L);
  setgcrefrel(slot->owner_root, obj2gco(L));
  la_storeptr_rel((void **)&slot->source, spec->source);
  la_store32_rel(&slot->source_traceno, spec->source_traceno);
  slot_roots = (GCRef *)la_loadptr_acq((void *const *)&slot->root_data);
  for (i = 0; i < spec->root_count; i++) {
    GCobj *root = spec->roots[i];
    setgcrefrel(slot_roots[i], root);
    if (root)
      lj_gc_pubobjroot(L, root);  /* Pre-publication active-cycle barrier. */
  }
  setgcrefrel(slot_roots[spec->root_count],
	      spec->callback_handler ? obj2gco(spec->callback_handler) : NULL);
  la_store32_rel(&slot->callback_root_count, spec->callback_root_count);
  if (spec->callback_handler)
    lj_gc_pubobjroot(L, obj2gco(spec->callback_handler));
  lj_gc_pubobjroot(L, obj2gco(L));
  if (spec->source)
    lj_gc_pubobjroot(L, obj2gco(spec->source));
  if (!la_cas64(&sessions->sequence, &sequence, sequence + 1u,
		LA_ACQ_REL, LA_ACQ)) {
    la_store32_rel(&slot->state, LJ_JIT_EVENT_SLOT_CLOSED);
    (void)jit_event_slot_cleanup(g, slot, LJ_JIT_EVENT_SLOT_CLOSED);
    goto fail_callback_lease;
  }
  if (LJ_UNLIKELY(la_load32_acq(&sessions->state) !=
		   LJ_JIT_EVENT_PUBLICATION_IDLE))
    abort();
  la_store64_rel(&sessions->next_generation, generation);
  la_store32_rel(&slot->state, LJ_JIT_EVENT_SLOT_ACTIVE);
  la_store64_rel(&sessions->active_generation, generation);
  la_store32_rel(&sessions->active_slot, slot_index);
  la_store32_rel(&sessions->state, LJ_JIT_EVENT_PUBLICATION_ACTIVE);
  la_store64_rel(&sessions->sequence, sequence + 2u);
  /* Close a MARK/WEAK/SWEEP activation edge which crossed the even publish. */
  for (i = 0; i < spec->root_count; i++)
    if (spec->roots[i])
      lj_gc_pubobjroot(L, spec->roots[i]);
  if (spec->callback_handler)
    lj_gc_pubobjroot(L, obj2gco(spec->callback_handler));
  lj_gc_pubobjroot(L, obj2gco(L));
  if (spec->source)
    lj_gc_pubobjroot(L, obj2gco(spec->source));
  handle->generation = generation;
  handle->slot = slot_index;
  handle->owner_mode = spec->owner_mode;
  if (callback_lease_held)
    lj_gc2_lease_release(&callback_lease);
  return 1;

fail_callback_lease:
  if (callback_lease_held)
    lj_gc2_lease_release(&callback_lease);
  return 0;
}

static int jit_event_session_unpublish_l(
  lua_State *L, jit_State *J, const LJJitEventSessionHandle *handle)
{
  TGState *tg = jit_event_owner_tg(L);
  LJJitEventSessions *sessions;
  LJJitEventSessionSlot *slot;
  uint64_t sequence;
  uint32_t tid;
  if (!J || !tg || G(L) != J2G(J) || !handle ||
      handle->generation == 0 || handle->slot >= LJ_JIT_EVENT_SESSION_SLOTS ||
      (handle->owner_mode != LJ_JIT_EVENT_OWNER_CONTINUATION_LIFECYCLE &&
       handle->owner_mode != LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE))
    return 0;
  /* The callback descriptor's owner_L is comparison-only; the ACTIVE session
  ** is its sole root authority.  Exact owner teardown must therefore precede
  ** every normal, rollback, stream-close, or terminal session unpublication. */
  if (!lj_jit_event_callback_idle(tg))
    return 0;
  sessions = &tg->jit_event_sessions;
  tid = lj_tg_tid_acq(tg);
  if (tid == 0 ||
      (handle->owner_mode == LJ_JIT_EVENT_OWNER_CONTINUATION_LIFECYCLE &&
       (jit_owner_word_acq(tg->gl) != jit_owner_pack(tid, 0) ||
	jit_owner_l_acq(J) != L)))
    return 0;
  slot = &sessions->slot[handle->slot];
  sequence = la_load64_acq(&sessions->sequence);
  if ((sequence & 1u) != 0 || sequence > ~(uint64_t)2 ||
      la_load32_acq(&sessions->state) != LJ_JIT_EVENT_PUBLICATION_ACTIVE ||
      la_load32_acq(&sessions->active_slot) != handle->slot ||
      la_load64_acq(&sessions->active_generation) != handle->generation ||
      la_load64_acq(&sessions->next_generation) != handle->generation ||
      la_load32_acq(&slot->state) != LJ_JIT_EVENT_SLOT_ACTIVE ||
      la_load64_acq(&slot->generation) != handle->generation ||
      la_load32_acq(&slot->owner_mode) != handle->owner_mode ||
      !la_cas64(&sessions->sequence, &sequence, sequence + 1u,
		LA_ACQ_REL, LA_ACQ))
    return 0;
  la_store32_rel(&slot->state, LJ_JIT_EVENT_SLOT_CLOSED);
  la_store64_rel(&sessions->active_generation, 0);
  la_store32_rel(&sessions->active_slot, LJ_JIT_EVENT_SESSION_SLOTS);
  la_store32_rel(&sessions->state, LJ_JIT_EVENT_PUBLICATION_IDLE);
  la_store64_rel(&sessions->sequence, sequence + 2u);
  if (la_load32_acq(&slot->readers) == 0)
    (void)jit_event_slot_cleanup(tg->gl, slot, LJ_JIT_EVENT_SLOT_CLOSED);
  return 1;
}

static int jit_event_session_active_handle(
  TGState *tg, const LJJitEventSessionHandle *handle)
{
  LJJitEventSessions *sessions;
  LJJitEventSessionSlot *slot;
  uint64_t sequence;
  if (!tg || !handle || handle->generation == 0 ||
      handle->slot >= LJ_JIT_EVENT_SESSION_SLOTS)
    return 0;
  sessions = &tg->jit_event_sessions;
  sequence = la_load64_acq(&sessions->sequence);
  if ((sequence & 1u) != 0 ||
      la_load32_acq(&sessions->state) != LJ_JIT_EVENT_PUBLICATION_ACTIVE ||
      la_load32_acq(&sessions->active_slot) != handle->slot ||
      la_load64_acq(&sessions->active_generation) != handle->generation ||
      la_load64_acq(&sessions->next_generation) != handle->generation)
    return 0;
  slot = &sessions->slot[handle->slot];
  return la_load32_acq(&slot->state) == LJ_JIT_EVENT_SLOT_ACTIVE &&
    la_load64_acq(&slot->generation) == handle->generation &&
    la_load32_acq(&slot->owner_mode) == handle->owner_mode &&
    la_load64_acq(&sessions->sequence) == sequence;
}

int lj_jit_event_session_begin_l(lua_State *L, jit_State *J,
				 const LJJitEventSessionSpec *spec,
				 LJJitEventSessionHandle *handle)
{
  TGState *tg;
  global_State *g;
  TraceState trace_state;
  uint32_t tid;
  if (!handle)
    return 0;
  memset(handle, 0, sizeof(*handle));
  if (!L || !J || G(L) != J2G(J) || !(tg = jit_event_owner_tg(L)) ||
      tg != lj_jit_owner_tg_l(L, J) || !spec)
    return 0;
  g = tg->gl;
  tid = lj_tg_tid_acq(tg);
  trace_state = lj_trace_state_load(J);
  if (tid == 0 || jit_owner_word_acq(g) != jit_owner_pack(tid, 0) ||
      jit_owner_l_acq(J) != L ||
      (spec->owner_mode == LJ_JIT_EVENT_OWNER_CONTINUATION_LIFECYCLE &&
       ((spec->event == LJ_JIT_EVENT_TRACE_START &&
	 trace_state != LJ_TRACE_START) ||
	(spec->event == LJ_JIT_EVENT_RECORD &&
	 trace_state != LJ_TRACE_RECORD &&
	 trace_state != LJ_TRACE_RECORD_1ST))) ||
      (spec->owner_mode == LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE &&
       trace_state != LJ_TRACE_IDLE) ||
      !jit_event_session_publish_l(L, J, spec, handle))
    return 0;

  /* Deterministic coverage of the post-publication rollback edge. Production
  ** CAS failure has the same low-owner cleanup, unless ownership corruption
  ** makes a safe rollback impossible. */
  if (trace_test_take_event_handoff_failure()) {
    if (LJ_UNLIKELY(jit_owner_word_acq(g) != jit_owner_pack(tid, 0) ||
		    jit_owner_l_acq(J) != L ||
		    !jit_event_session_unpublish_l(L, J, handle)))
      abort();
    memset(handle, 0, sizeof(*handle));
    return 0;
  }

  if (spec->owner_mode == LJ_JIT_EVENT_OWNER_CONTINUATION_LIFECYCLE) {
    if (lj_jit_lifecycle_yield_l(L, J))
      return 1;
    if (LJ_UNLIKELY(jit_owner_word_acq(g) != jit_owner_pack(tid, 0) ||
		    jit_owner_l_acq(J) != L ||
		    !jit_event_session_unpublish_l(L, J, handle)))
      abort();
    memset(handle, 0, sizeof(*handle));
    return 0;
  }

  if (LJ_UNLIKELY(spec->owner_mode !=
		  LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE))
    abort();  /* The publication contract rejected every other mode. */
  jit_owner_l_rel(J, NULL);
  if (jit_token_release_exact(g, tid))
    return 1;
  if (jit_owner_word_acq(g) == jit_owner_pack(tid, 0)) {
    jit_owner_l_rel(J, L);
    if (LJ_UNLIKELY(!jit_event_session_unpublish_l(L, J, handle)))
      abort();
    memset(handle, 0, sizeof(*handle));
    return 0;
  }
  abort();  /* No legal peer may steal an exact low-half token. */
}

int lj_jit_event_session_end_l(lua_State *L, jit_State *J,
			       const LJJitEventSessionHandle *handle)
{
  TGState *tg;
  global_State *g;
  LJJitOwnerWord owner_word;
  uint32_t tid;
  if (!L || !J || G(L) != J2G(J) || !(tg = jit_event_owner_tg(L)) ||
      !handle || !jit_event_session_active_handle(tg, handle))
    return 0;
  g = tg->gl;
  tid = lj_tg_tid_acq(tg);
  if (handle->owner_mode ==
      LJ_JIT_EVENT_OWNER_CONTINUATION_LIFECYCLE) {
    if (tid == 0 || jit_owner_word_acq(g) != jit_owner_pack(0, tid) ||
	jit_owner_l_acq(J) != L || !lj_jit_lifecycle_resume_l(L, J))
      return 0;
    if (LJ_UNLIKELY(!jit_event_session_unpublish_l(L, J, handle)))
      abort();
    return 1;
  }
  if (handle->owner_mode != LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE)
    return 0;
  owner_word = jit_owner_word_acq(g);
  if (jit_owner_token(owner_word) == tid ||
      jit_owner_lifecycle(owner_word) == tid)
    return 0;
  return jit_event_session_unpublish_l(L, J, handle);
}

int lj_jit_event_session_snapshot_acquire(
  global_State *g, TGState *tg, LJJitEventSessionSnapshot *snapshot)
{
  LJJitEventSessions *sessions;
  LJJitEventSessionSlot *slot;
  GCRef *roots;
  GCfunc *callback_handler;
  uint64_t sequence, generation;
  uint32_t slot_index, readers, nroots, callback_root_count, flags;
  if (!snapshot)
    return LJ_JIT_EVENT_SNAPSHOT_RETRY;
  memset(snapshot, 0, sizeof(*snapshot));
  if (!g || !tg || !lj_gc2_smr_read_try(g))
    return LJ_JIT_EVENT_SNAPSHOT_RETRY;
  if (tg->gl != g) {
    lj_gc2_smr_read_leave(g);
    return LJ_JIT_EVENT_SNAPSHOT_RETRY;
  }
  sessions = &tg->jit_event_sessions;
  sequence = la_load64_acq(&sessions->sequence);
  if ((sequence & 1u) != 0) {
    lj_gc2_smr_read_leave(g);
    return LJ_JIT_EVENT_SNAPSHOT_RETRY;
  }
  if (la_load32_acq(&sessions->state) == LJ_JIT_EVENT_PUBLICATION_IDLE) {
    int idle = la_load64_acq(&sessions->sequence) == sequence &&
      la_load32_acq(&sessions->state) == LJ_JIT_EVENT_PUBLICATION_IDLE &&
      la_load32_acq(&sessions->active_slot) == LJ_JIT_EVENT_SESSION_SLOTS &&
      la_load64_acq(&sessions->active_generation) == 0 ?
      LJ_JIT_EVENT_SNAPSHOT_IDLE : LJ_JIT_EVENT_SNAPSHOT_RETRY;
    lj_gc2_smr_read_leave(g);
    return idle;
  }
  slot_index = la_load32_acq(&sessions->active_slot);
  generation = la_load64_acq(&sessions->active_generation);
  if (slot_index >= LJ_JIT_EVENT_SESSION_SLOTS || generation == 0 ||
      la_load64_acq(&sessions->next_generation) != generation)
    goto retry_leave;
  slot = &sessions->slot[slot_index];
  readers = la_load32_acq(&slot->readers);
  for (;;) {
    if (readers == ~(uint32_t)0)
      goto retry_leave;
    if (la_cas32(&slot->readers, &readers, readers + 1u,
		 LA_ACQ_REL, LA_ACQ))
      break;
  }
  if (la_load64_acq(&sessions->sequence) != sequence ||
      la_load32_acq(&sessions->state) != LJ_JIT_EVENT_PUBLICATION_ACTIVE ||
      la_load32_acq(&sessions->active_slot) != slot_index ||
      la_load64_acq(&sessions->active_generation) != generation ||
      la_load64_acq(&sessions->next_generation) != generation ||
      la_load32_acq(&slot->state) != LJ_JIT_EVENT_SLOT_ACTIVE ||
      la_load64_acq(&slot->generation) != generation) {
    jit_event_slot_reader_drop(tg, slot);
    goto retry_leave;
  }
  snapshot->g = g;
  snapshot->tg = tg;
  snapshot->slot = slot;
  snapshot->sequence = sequence;
  snapshot->generation = generation;
  snapshot->slot_index = slot_index;
  snapshot->event = la_load32_acq(&slot->event);
  snapshot->owner_mode = la_load32_acq(&slot->owner_mode);
  snapshot->edge_proof = la_load32_acq(&slot->edge_proof);
  snapshot->attachment_generation =
    la_load64_acq(&slot->attachment_generation);
  snapshot->attachment_state = la_load32_acq(&slot->attachment_state);
  if (!lj_vmevent_attachment_identity_valid(snapshot->attachment_state,
						    snapshot->attachment_generation)) {
    jit_event_slot_reader_drop(tg, slot);
    memset(snapshot, 0, sizeof(*snapshot));
    goto retry_leave;
  }
  if (!jit_event_slot_root_geometry(slot, &roots, &nroots, NULL)) {
    jit_event_slot_reader_drop(tg, slot);
    memset(snapshot, 0, sizeof(*snapshot));
    goto retry_leave;
  }
  callback_handler = jit_event_callback_handler_acq(roots, nroots);
  callback_root_count = la_load32_acq(&slot->callback_root_count);
  flags = la_load32_acq(&slot->flags);
  if (callback_root_count > 1u ||
      ((callback_handler != NULL) != (callback_root_count == 1u)) ||
      (((flags & LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT) != 0) !=
	(callback_root_count == 1u)) ||
      !jit_event_callback_handler_type(g, callback_handler)) {
    jit_event_slot_reader_drop(tg, slot);
    memset(snapshot, 0, sizeof(*snapshot));
    goto retry_leave;
  }
  snapshot->callback_root_count = callback_root_count;
  snapshot->callback_handler = callback_handler;
  if (la_load64_acq(&sessions->sequence) != sequence ||
      la_load64_acq(&sessions->next_generation) != generation ||
      la_load32_acq(&slot->state) != LJ_JIT_EVENT_SLOT_ACTIVE ||
      la_load32_acq(&slot->attachment_state) != snapshot->attachment_state ||
      la_load32_acq(&slot->callback_root_count) != callback_root_count ||
      jit_event_callback_handler_acq(roots, nroots) != callback_handler) {
    jit_event_slot_reader_drop(tg, slot);
    memset(snapshot, 0, sizeof(*snapshot));
    lj_gc2_smr_read_leave(g);
    return LJ_JIT_EVENT_SNAPSHOT_RETRY;
  }
  return LJ_JIT_EVENT_SNAPSHOT_ACTIVE;

retry_leave:
  lj_gc2_smr_read_leave(g);
  return LJ_JIT_EVENT_SNAPSHOT_RETRY;
}

int lj_jit_event_session_snapshot_release(
  LJJitEventSessionSnapshot *snapshot)
{
  TGState *tg;
  global_State *g;
  LJJitEventSessions *sessions;
  LJJitEventSessionSlot *slot;
  GCRef *roots = NULL;
  GCfunc *callback_handler = NULL;
  uint32_t nroots = 0, callback_root_count = 0, flags = 0;
  int stable;
  if (!snapshot || !(g = snapshot->g) || !(tg = snapshot->tg) ||
      !(slot = (LJJitEventSessionSlot *)snapshot->slot))
    return 0;
  sessions = &tg->jit_event_sessions;
  if (jit_event_slot_root_geometry(slot, &roots, &nroots, NULL)) {
    callback_handler = jit_event_callback_handler_acq(roots, nroots);
    callback_root_count = la_load32_acq(&slot->callback_root_count);
    flags = la_load32_acq(&slot->flags);
  }
  stable = roots != NULL &&
    lj_vmevent_attachment_identity_valid(snapshot->attachment_state,
					 snapshot->attachment_generation) &&
    callback_root_count <= 1u &&
    ((callback_handler != NULL) == (callback_root_count == 1u)) &&
    (((flags & LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT) != 0) ==
      (callback_root_count == 1u)) &&
    la_load64_acq(&sessions->sequence) == snapshot->sequence &&
    la_load32_acq(&sessions->state) == LJ_JIT_EVENT_PUBLICATION_ACTIVE &&
    la_load32_acq(&sessions->active_slot) == snapshot->slot_index &&
    la_load64_acq(&sessions->active_generation) == snapshot->generation &&
    la_load64_acq(&sessions->next_generation) == snapshot->generation &&
    la_load32_acq(&slot->state) == LJ_JIT_EVENT_SLOT_ACTIVE &&
    la_load64_acq(&slot->generation) == snapshot->generation &&
    la_load32_acq(&slot->event) == snapshot->event &&
    la_load32_acq(&slot->owner_mode) == snapshot->owner_mode &&
    la_load32_acq(&slot->edge_proof) == snapshot->edge_proof &&
    la_load64_acq(&slot->attachment_generation) ==
      snapshot->attachment_generation &&
    la_load32_acq(&slot->attachment_state) == snapshot->attachment_state &&
    callback_root_count == snapshot->callback_root_count &&
    callback_handler == snapshot->callback_handler;
  jit_event_slot_reader_drop(tg, slot);
  memset(snapshot, 0, sizeof(*snapshot));
  lj_gc2_smr_read_leave(g);
  return stable;
}

/* -- Universe-global JIT TRACE stream ---------------------------------- */

static LJJitTraceStream *jit_trace_stream(global_State *g)
{
  return g && g->main_tg ? &g->main_tg->jit_trace_stream : NULL;
}

static LJTGRegistryKey jit_trace_stream_owner_key_acq(
  const LJJitTraceStream *stream)
{
  LJTGRegistryKey key;
  key.slot = (LJTGRegistrySlot *)
    la_loadptr_acq((void *const *)&stream->owner_key.slot);
  key.incarnation = la_load64_acq(&stream->owner_key.incarnation);
  return key;
}

static void jit_trace_stream_owner_key_rel(LJJitTraceStream *stream,
					   const LJTGRegistryKey *key)
{
  la_storeptr_rel((void **)&stream->owner_key.slot,
		  key ? key->slot : NULL);
  la_store64_rel(&stream->owner_key.incarnation,
		 key ? key->incarnation : LJ_TGSLOT_INCARNATION_NONE);
}

static int jit_trace_stream_idle_snapshot(
  const LJJitTraceStreamSnapshot *snapshot)
{
  return snapshot->phase == LJ_JIT_STREAM_IDLE &&
    snapshot->generation == 0 && snapshot->event_ordinal == 0 &&
    snapshot->owner_key.slot == NULL &&
    snapshot->owner_key.incarnation == LJ_TGSLOT_INCARNATION_NONE &&
    snapshot->owner_tid == 0 && snapshot->owner_actor == 0 &&
    snapshot->traceno == 0 && snapshot->callback_event == 0 &&
    snapshot->callback_slot == 0 &&
    snapshot->callback_session_generation == 0 &&
    snapshot->terminal_event == 0 && snapshot->terminal_slot == 0 &&
    snapshot->terminal_session_generation == 0 &&
    snapshot->terminal_reason == 0 && snapshot->flags == 0;
}

int lj_jit_trace_stream_snapshot(global_State *g,
				 LJJitTraceStreamSnapshot *snapshot)
{
  LJJitTraceStream *stream;
  LJTGSlotSnap owner_slot;
  LJTGRegistryBodySnap owner_body;
  uint64_t sequence;
  if (!snapshot)
    return LJ_JIT_STREAM_SNAPSHOT_RETRY;
  memset(snapshot, 0, sizeof(*snapshot));
  stream = jit_trace_stream(g);
  if (!stream)
    return LJ_JIT_STREAM_SNAPSHOT_RETRY;
  sequence = la_load64_acq(&stream->sequence);
  if ((sequence & 1u) != 0)
    return LJ_JIT_STREAM_SNAPSHOT_RETRY;
  snapshot->sequence = sequence;
  snapshot->next_generation = la_load64_acq(&stream->next_generation);
  snapshot->generation = la_load64_acq(&stream->generation);
  snapshot->event_ordinal = la_load64_acq(&stream->event_ordinal);
  snapshot->owner_key = jit_trace_stream_owner_key_acq(stream);
  snapshot->owner_tid = la_load32_acq(&stream->owner_tid);
  snapshot->owner_actor = la_load32_acq(&stream->owner_actor);
  snapshot->phase = la_load32_acq(&stream->phase);
  snapshot->traceno = la_load32_acq(&stream->traceno);
  snapshot->callback_event = la_load32_acq(&stream->callback_event);
  snapshot->callback_slot = la_load32_acq(&stream->callback_slot);
  snapshot->callback_session_generation =
    la_load64_acq(&stream->callback_session_generation);
  snapshot->terminal_event = la_load32_acq(&stream->terminal_event);
  snapshot->terminal_slot = la_load32_acq(&stream->terminal_slot);
  snapshot->terminal_session_generation =
    la_load64_acq(&stream->terminal_session_generation);
  snapshot->terminal_reason = la_load32_acq(&stream->terminal_reason);
  snapshot->flags = la_load32_acq(&stream->flags);
  if (la_load64_acq(&stream->sequence) != sequence)
    goto retry;
  if (snapshot->phase == LJ_JIT_STREAM_IDLE)
    return jit_trace_stream_idle_snapshot(snapshot) ?
      LJ_JIT_STREAM_SNAPSHOT_IDLE : LJ_JIT_STREAM_SNAPSHOT_RETRY;
  /* Standalone FLUSH may be structurally pending without a handler, pending
  ** with one exact rooted callback session, or already claimed for callback
  ** execution.  Every callback identity is the same detached terminal
  ** session; no half-populated or cross-session shape is authoritative. */
  if ((snapshot->phase != LJ_JIT_STREAM_DETACHED_PENDING &&
       snapshot->phase != LJ_JIT_STREAM_DETACHED_CALLBACK) ||
      snapshot->generation == 0 || snapshot->event_ordinal != 1u ||
      snapshot->next_generation != snapshot->generation ||
      !lj_tgregistry_key_valid(&snapshot->owner_key) ||
      snapshot->owner_tid == 0 || snapshot->owner_actor == 0 ||
      snapshot->owner_actor == LJ_THR_ACTOR_RETIRED ||
      snapshot->traceno != 0 ||
      snapshot->terminal_event != LJ_JIT_EVENT_TRACE_FLUSH ||
      snapshot->terminal_slot >= LJ_JIT_EVENT_SESSION_SLOTS ||
      snapshot->terminal_session_generation == 0 ||
      snapshot->terminal_reason != 0 || snapshot->flags != 0 ||
      lj_tgregistry_key_snapshot(&snapshot->owner_key, &owner_slot) !=
	LJ_TGSLOT_OK || owner_slot.state != LJ_TGSLOT_LIVE)
    goto retry;
  if (snapshot->callback_event == 0) {
    if (snapshot->phase != LJ_JIT_STREAM_DETACHED_PENDING ||
	snapshot->callback_slot != LJ_JIT_EVENT_SESSION_SLOTS ||
	snapshot->callback_session_generation != 0)
      goto retry;
  } else if (snapshot->callback_event != LJ_JIT_EVENT_TRACE_FLUSH ||
	     snapshot->callback_slot != snapshot->terminal_slot ||
	     snapshot->callback_session_generation !=
	       snapshot->terminal_session_generation) {
    goto retry;
  }
  owner_body = lj_tgregistry_slot_body_snapshot(snapshot->owner_key.slot);
  /* The stable registry node may be inspected without a body lease, but its
  ** published TG body may be reclaimed immediately after our scalar sequence
  ** recheck. Never dereference it here. Exact owner/body field validation is
  ** performed by admission/close while their session keeps the TG rooted. */
  if (!owner_body.body ||
      owner_body.incarnation != snapshot->owner_key.incarnation)
    goto retry;
  return LJ_JIT_STREAM_SNAPSHOT_ACTIVE;
retry:
  memset(snapshot, 0, sizeof(*snapshot));
  return LJ_JIT_STREAM_SNAPSHOT_RETRY;
}

int lj_jit_trace_stream_idle(global_State *g)
{
  LJJitTraceStreamSnapshot snapshot;
  return lj_jit_trace_stream_snapshot(g, &snapshot) ==
    LJ_JIT_STREAM_SNAPSHOT_IDLE;
}

int lj_jit_trace_stream_names_tg(global_State *g, TGState *tg)
{
  LJJitTraceStreamSnapshot snapshot;
  int state;
  if (!g || !tg || tg->gl != g)
    return 1;  /* An invalid query cannot authorize lifecycle progress. */
  state = lj_jit_trace_stream_snapshot(g, &snapshot);
  if (state == LJ_JIT_STREAM_SNAPSHOT_RETRY)
    return 1;  /* Odd or corrupt publication fails closed without waiting. */
  if (state == LJ_JIT_STREAM_SNAPSHOT_IDLE)
    return 0;
  /* The immutable registry incarnation alone is lifecycle naming authority.
  ** Tid/actor are additional ABA fields for exact close, but a mismatch in
  ** either can never authorize detaching the TG named by this exact key. */
  return lj_tgregistry_key_equal(&snapshot.owner_key, &tg->registry_key);
}

static int jit_trace_stream_writer_claim(LJJitTraceStream *stream,
					 uint64_t *sequence, uint64_t reserve)
{
  uint64_t value;
  if (!stream || !sequence || reserve < 2u || (reserve & 1u) != 0)
    return 0;
  value = la_load64_acq(&stream->sequence);
  /* The first publication reserves every remaining even-to-even transition
  ** in its linear lifetime.  Refuse rather than wrap a stable sequence. */
  if ((value & 1u) != 0 || value > ~(uint64_t)0 - reserve)
    return 0;
  if (!la_cas64(&stream->sequence, &value, value + 1u,
		LA_ACQ_REL, LA_ACQ))
    return 0;
  *sequence = value;
  return 1;
}

static void jit_trace_stream_writer_release(LJJitTraceStream *stream,
					    uint64_t sequence)
{
  la_store64_rel(&stream->sequence, sequence + 2u);
}

static int jit_trace_stream_live_key(TGState *tg, LJTGRegistryKey *key)
{
  LJTGSlotSnap slotsnap;
  LJTGRegistryBodySnap bodysnap;
  if (!tg || !key)
    return 0;
  key->slot = (LJTGRegistrySlot *)
    la_loadptr_acq((void *const *)&tg->registry_key.slot);
  key->incarnation = la_load64_acq(&tg->registry_key.incarnation);
  if (!lj_tgregistry_key_valid(key) ||
      lj_tgregistry_key_snapshot(key, &slotsnap) != LJ_TGSLOT_OK ||
      slotsnap.state != LJ_TGSLOT_LIVE)
    return 0;
  bodysnap = lj_tgregistry_slot_body_snapshot(key->slot);
  return bodysnap.body == tg && bodysnap.incarnation == key->incarnation;
}

static int jit_trace_bytes_zero(const void *data, size_t size)
{
  const unsigned char *p = (const unsigned char *)data;
  size_t i;
  for (i = 0; i < size; i++)
    if (p[i] != 0)
      return 0;
  return 1;
}

static int jit_trace_flush_session_exact(
  TGState *tg, lua_State *L, const LJJitTraceStreamHandle *handle)
{
  LJJitEventSessions *sessions;
  LJJitEventSessionSlot *slot;
  GCRef *roots;
  uint32_t nroots;
  uint64_t sequence;
  if (!tg || !L || !handle || handle->terminal_session.generation == 0 ||
      handle->terminal_session.slot >= LJ_JIT_EVENT_SESSION_SLOTS ||
      handle->terminal_session.owner_mode !=
	LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE ||
      !lj_vmevent_attachment_identity_valid(handle->attachment_state,
					     handle->attachment_generation) ||
      handle->callback_root_count > 1u ||
      ((handle->callback_handler != NULL) !=
	(handle->callback_root_count == 1u)))
    return 0;
  sessions = &tg->jit_event_sessions;
  slot = &sessions->slot[handle->terminal_session.slot];
  if (!jit_event_slot_root_geometry(slot, &roots, &nroots, NULL))
    return 0;
  sequence = la_load64_acq(&sessions->sequence);
  if ((sequence & 1u) != 0 ||
      la_load32_acq(&sessions->state) != LJ_JIT_EVENT_PUBLICATION_ACTIVE ||
      la_load32_acq(&sessions->active_slot) !=
	handle->terminal_session.slot ||
      la_load64_acq(&sessions->active_generation) !=
	handle->terminal_session.generation ||
      la_load64_acq(&sessions->next_generation) !=
	handle->terminal_session.generation ||
      la_load32_acq(&slot->state) != LJ_JIT_EVENT_SLOT_ACTIVE ||
      la_load64_acq(&slot->generation) !=
	handle->terminal_session.generation ||
      la_load32_acq(&slot->event) != LJ_JIT_EVENT_TRACE_FLUSH ||
      la_load32_acq(&slot->owner_mode) !=
	LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE ||
      la_load32_acq(&slot->edge_proof) != LJ_JIT_EVENT_EDGE_NONE ||
      la_load64_acq(&slot->attachment_generation) !=
	handle->attachment_generation ||
      la_load32_acq(&slot->attachment_state) !=
	handle->attachment_state ||
      la_load32_acq(&slot->flags) !=
	(handle->callback_root_count != 0 ?
	 LJ_JIT_EVENT_SLOT_F_CALLBACK_ROOT : 0) ||
      la_load32_acq(&slot->control_borrow_state) != 0 ||
      la_load64_acq(&slot->control_borrow_generation) != 0 ||
      la_loadptr_acq((void *const *)&slot->saved_jit_owner_L) != NULL ||
      nroots != 0 ||
      la_load32_acq(&slot->callback_root_count) !=
	handle->callback_root_count ||
      jit_event_callback_handler_acq(roots, nroots) !=
	handle->callback_handler ||
      la_load32_acq(&slot->owner_tid) != handle->owner_tid ||
      la_load32_acq(&slot->owner_actor) != handle->owner_actor ||
      la_loadptr_acq((void *const *)&slot->owner_L) != L ||
      gcref_acq(slot->owner_root) != obj2gco(L) ||
      la_loadptr_acq((void *const *)&slot->source) != NULL ||
      la_load32_acq(&slot->source_traceno) != 0 ||
      la_load32_acq(&slot->view.size) != 0 ||
      la_load32_acq(&slot->view.format) != LJ_JIT_EVENT_VIEW_FORMAT_NONE ||
      la_load32_acq(&slot->view.flags) != 0 ||
      !jit_trace_bytes_zero(&slot->view.trace, sizeof(slot->view.trace)) ||
      !jit_trace_bytes_zero(&slot->view.ir, sizeof(slot->view.ir)) ||
      !jit_trace_bytes_zero(&slot->view.snap, sizeof(slot->view.snap)) ||
      !jit_trace_bytes_zero(&slot->view.snapmap,
			    sizeof(slot->view.snapmap)))
    return 0;
  return la_load64_acq(&sessions->sequence) == sequence;
}

static int jit_trace_flush_descriptor_exact_phase(
  const LJJitTraceStream *stream, const LJJitTraceStreamHandle *handle,
  uint32_t phase)
{
  LJTGRegistryKey key = jit_trace_stream_owner_key_acq(stream);
  uint32_t callback = handle->callback_root_count;
  return la_load64_acq(&stream->next_generation) == handle->generation &&
    la_load64_acq(&stream->generation) == handle->generation &&
    la_load64_acq(&stream->event_ordinal) == 1u &&
    lj_tgregistry_key_equal(&key, &handle->owner_key) &&
    la_load32_acq(&stream->owner_tid) == handle->owner_tid &&
    la_load32_acq(&stream->owner_actor) == handle->owner_actor &&
    la_load32_acq(&stream->phase) == phase &&
    la_load32_acq(&stream->traceno) == 0 &&
    la_load32_acq(&stream->callback_event) ==
      (callback ? LJ_JIT_EVENT_TRACE_FLUSH : 0) &&
    la_load32_acq(&stream->callback_slot) ==
      (callback ? handle->terminal_session.slot :
	LJ_JIT_EVENT_SESSION_SLOTS) &&
    la_load64_acq(&stream->callback_session_generation) ==
      (callback ? handle->terminal_session.generation : 0) &&
    la_load32_acq(&stream->terminal_event) == LJ_JIT_EVENT_TRACE_FLUSH &&
    la_load32_acq(&stream->terminal_slot) ==
      handle->terminal_session.slot &&
    la_load64_acq(&stream->terminal_session_generation) ==
      handle->terminal_session.generation &&
    la_load32_acq(&stream->terminal_reason) == 0 &&
    la_load32_acq(&stream->flags) == 0;
}

static void jit_trace_stream_clear_locked(LJJitTraceStream *stream)
{
  la_store64_rel(&stream->generation, 0);
  la_store64_rel(&stream->event_ordinal, 0);
  jit_trace_stream_owner_key_rel(stream, NULL);
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
}

static int jit_trace_flush_clear_exact(
  TGState *tg, lua_State *L, const LJJitTraceStreamHandle *handle,
  uint32_t phase)
{
  LJJitTraceStream *stream = jit_trace_stream(tg ? tg->gl : NULL);
  uint64_t sequence;
  if (!stream || !jit_trace_flush_session_exact(tg, L, handle) ||
      !jit_trace_flush_descriptor_exact_phase(stream, handle, phase) ||
      !jit_trace_stream_writer_claim(stream, &sequence, 2u))
    return 0;
  if (!jit_trace_flush_session_exact(tg, L, handle) ||
      !jit_trace_flush_descriptor_exact_phase(stream, handle, phase)) {
    jit_trace_stream_writer_release(stream, sequence);
    return 0;
  }
  jit_trace_stream_clear_locked(stream);
  jit_trace_stream_writer_release(stream, sequence);
  return 1;
}

/* Publish the rooted detached session and pending universe stream while the
** exact low token remains held.  The caller either binds a callback and then
** hands off, or performs the structural handoff directly. */
static int jit_trace_flush_publish_l(lua_State *L, jit_State *J,
				     uint32_t attachment_state,
				     uint64_t attachment_generation,
				     GCfunc *callback_handler,
				     uint64_t stream_reserve,
				     LJJitTraceStreamHandle *handle)
{
  LJJitEventSessionSpec spec;
  LJJitTraceStreamSnapshot before;
  LJJitTraceStream *stream;
  TGState *tg;
  global_State *g;
  LJTGRegistryKey key;
  LJTGSlotSnap slotsnap;
  uint64_t sequence, generation;
  uint32_t tid, actor;
  if (!handle)
    return 0;
  memset(handle, 0, sizeof(*handle));
  if (!L || !J ||
      !lj_vmevent_attachment_identity_valid(attachment_state,
					     attachment_generation) ||
      stream_reserve < 4u || (stream_reserve & 1u) != 0 ||
      G(L) != J2G(J) ||
      !(tg = jit_event_owner_tg(L)) || tg != lj_jit_owner_tg_l(L, J))
    return 0;
  g = tg->gl;
  stream = jit_trace_stream(g);
  tid = lj_tg_tid_acq(tg);
  actor = lj_tg_actor_acq(tg);
  if (!stream || tid == 0 || actor == 0 || actor == LJ_THR_ACTOR_RETIRED ||
      actor != lj_thr_actor_current() || !jit_trace_stream_live_key(tg, &key) ||
      !lj_jit_event_callback_idle(tg) ||
      lj_tgregistry_key_snapshot(&key, &slotsnap) != LJ_TGSLOT_OK ||
      slotsnap.state != LJ_TGSLOT_LIVE ||
      lj_trace_state_load(J) != LJ_TRACE_IDLE ||
      jit_owner_word_acq(g) != jit_owner_pack(tid, 0) ||
      jit_owner_l_acq(J) != L ||
      lj_jit_trace_stream_snapshot(g, &before) !=
	LJ_JIT_STREAM_SNAPSHOT_IDLE ||
      before.sequence > ~(uint64_t)0 - stream_reserve ||
      before.next_generation == ~(uint64_t)0 ||
      la_load64_acq(&tg->jit_event_sessions.sequence) > ~(uint64_t)4)
    return 0;

  memset(&spec, 0, sizeof(spec));
  spec.event = LJ_JIT_EVENT_TRACE_FLUSH;
  spec.owner_mode = LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE;
  spec.edge_proof = LJ_JIT_EVENT_EDGE_NONE;
  spec.attachment_generation = attachment_generation;
  spec.attachment_state = attachment_state;
  spec.callback_root_count = callback_handler ? 1u : 0u;
  spec.callback_handler = callback_handler;
  if (!jit_event_session_publish_l(L, J, &spec, &handle->terminal_session))
    return 0;

  /* No allocator, GC action, handler lookup or Lua execution is permitted
  ** between this odd claim and its paired even release. */
  if (!jit_trace_stream_writer_claim(stream, &sequence, stream_reserve))
    goto rollback_session;
  if (la_load32_acq(&stream->phase) != LJ_JIT_STREAM_IDLE ||
      la_load64_acq(&stream->generation) != 0 ||
      la_load64_acq(&stream->next_generation) != before.next_generation ||
      jit_owner_word_acq(g) != jit_owner_pack(tid, 0) ||
      jit_owner_l_acq(J) != L || lj_tg_tid_acq(tg) != tid ||
      lj_tg_actor_acq(tg) != actor || actor != lj_thr_actor_current() ||
      !lj_tgregistry_key_equal(&key, &tg->registry_key)) {
    jit_trace_stream_writer_release(stream, sequence);
    goto rollback_session;
  }
  handle->generation = generation = before.next_generation + 1u;
  handle->attachment_generation = attachment_generation;
  handle->owner_key = key;
  handle->owner_tid = tid;
  handle->owner_actor = actor;
  handle->attachment_state = attachment_state;
  handle->callback_root_count = callback_handler ? 1u : 0u;
  handle->callback_handler = callback_handler;
  if (!jit_trace_flush_session_exact(tg, L, handle)) {
    jit_trace_stream_writer_release(stream, sequence);
    goto rollback_handle;
  }
  la_store64_rel(&stream->next_generation, generation);
  la_store64_rel(&stream->generation, generation);
  la_store64_rel(&stream->event_ordinal, 1u);
  jit_trace_stream_owner_key_rel(stream, &key);
  la_store32_rel(&stream->owner_tid, tid);
  la_store32_rel(&stream->owner_actor, actor);
  la_store32_rel(&stream->phase, LJ_JIT_STREAM_DETACHED_PENDING);
  la_store32_rel(&stream->traceno, 0);
  la_store32_rel(&stream->callback_event,
		 callback_handler ? LJ_JIT_EVENT_TRACE_FLUSH : 0);
  la_store32_rel(&stream->callback_slot,
		 callback_handler ? handle->terminal_session.slot :
		 LJ_JIT_EVENT_SESSION_SLOTS);
  la_store64_rel(&stream->callback_session_generation,
		 callback_handler ? handle->terminal_session.generation : 0);
  la_store32_rel(&stream->terminal_event, LJ_JIT_EVENT_TRACE_FLUSH);
  la_store32_rel(&stream->terminal_slot, handle->terminal_session.slot);
  la_store64_rel(&stream->terminal_session_generation,
		handle->terminal_session.generation);
  la_store32_rel(&stream->terminal_reason, 0);
  la_store32_rel(&stream->flags, 0);
  jit_trace_stream_writer_release(stream, sequence);
  return 1;

rollback_handle:
rollback_session:
  if (LJ_UNLIKELY(!jit_event_session_unpublish_l(
	L, J, &handle->terminal_session)))
    abort();
  memset(handle, 0, sizeof(*handle));
  return 0;
}

static int jit_trace_flush_handoff_l(lua_State *L, jit_State *J,
				     const LJJitTraceStreamHandle *handle)
{
  global_State *g = G(L);
  uint32_t tid = handle->owner_tid;
  if (trace_test_take_event_handoff_failure())
    return 0;
  jit_owner_l_rel(J, NULL);
  if (jit_token_release_exact(g, tid))
    return 1;
  if (jit_owner_word_acq(g) != jit_owner_pack(tid, 0))
    abort();  /* No legal peer can steal this exact low-half token. */
  jit_owner_l_rel(J, L);
  return 0;
}

static int jit_trace_flush_callback_owner_exact(
  TGState *tg, lua_State *L, const LJJitTraceStreamHandle *stream_handle,
  const LJJitEventCallbackHandle *callback_handle)
{
  LJJitEventCallbackSnapshot snapshot;
  return callback_handle &&
    lj_jit_event_callback_snapshot(tg, &snapshot) ==
      LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE &&
    snapshot.tg == tg && snapshot.owner_L == L &&
    snapshot.generation == callback_handle->generation &&
    snapshot.next_generation == callback_handle->generation &&
    snapshot.stream_generation == stream_handle->generation &&
    snapshot.stream_generation == callback_handle->stream_generation &&
    snapshot.session_generation ==
      stream_handle->terminal_session.generation &&
    snapshot.session_generation == callback_handle->session_generation &&
    snapshot.state == LJ_JIT_EVENT_CALLBACK_CALLING &&
    snapshot.owner_actor == stream_handle->owner_actor &&
    snapshot.owner_actor == callback_handle->owner_actor &&
    snapshot.event == LJ_JIT_EVENT_TRACE_FLUSH &&
    snapshot.event == callback_handle->event &&
    snapshot.session_slot == stream_handle->terminal_session.slot &&
    snapshot.session_slot == callback_handle->session_slot;
}

static int jit_trace_flush_callback_phase_l(
  TGState *tg, lua_State *L, const LJJitTraceStreamHandle *stream_handle,
  const LJJitEventCallbackHandle *callback_handle)
{
  LJJitTraceStream *stream = jit_trace_stream(tg ? tg->gl : NULL);
  uint64_t sequence;
  if (!stream || stream_handle->callback_root_count != 1u ||
      !jit_trace_flush_session_exact(tg, L, stream_handle) ||
      !jit_trace_flush_descriptor_exact_phase(
	stream, stream_handle, LJ_JIT_STREAM_DETACHED_PENDING) ||
      !jit_trace_flush_callback_owner_exact(
	tg, L, stream_handle, callback_handle) ||
      !jit_trace_stream_writer_claim(stream, &sequence, 4u))
    return 0;
  if (!jit_trace_flush_session_exact(tg, L, stream_handle) ||
      !jit_trace_flush_descriptor_exact_phase(
	stream, stream_handle, LJ_JIT_STREAM_DETACHED_PENDING) ||
      !jit_trace_flush_callback_owner_exact(
	tg, L, stream_handle, callback_handle)) {
    jit_trace_stream_writer_release(stream, sequence);
    return 0;
  }
  la_store32_rel(&stream->phase, LJ_JIT_STREAM_DETACHED_CALLBACK);
  jit_trace_stream_writer_release(stream, sequence);
  return 1;
}

static void jit_trace_flush_rollback_l(
  lua_State *L, jit_State *J, TGState *tg,
  LJJitTraceStreamHandle *stream_handle,
  LJJitEventCallbackHandle *callback_handle, uint32_t phase)
{
  if (callback_handle && callback_handle->generation != 0) {
    if (LJ_UNLIKELY(!lj_jit_event_callback_unwind_l(
	    L, callback_handle) ||
	  !lj_jit_event_callback_release_l(L, callback_handle)))
      abort();
  }
  if (LJ_UNLIKELY(!jit_trace_flush_clear_exact(
	  tg, L, stream_handle, phase) ||
	!jit_event_session_unpublish_l(
	  L, J, &stream_handle->terminal_session)))
    abort();
  memset(stream_handle, 0, sizeof(*stream_handle));
  if (callback_handle)
    memset(callback_handle, 0, sizeof(*callback_handle));
}

int lj_jit_trace_flush_admit_l(lua_State *L, jit_State *J,
			       uint64_t attachment_generation,
			       LJJitTraceStreamHandle *handle)
{
  TGState *tg;
  if (!handle)
    return 0;
  memset(handle, 0, sizeof(*handle));
  if (!L || !J || attachment_generation == 0 || G(L) != J2G(J) ||
      !(tg = jit_event_owner_tg(L)) ||
      !jit_trace_flush_publish_l(
	L, J, LJ_VMEVENT_ATTACHMENT_PUBLISHED, attachment_generation,
	NULL, 4u, handle))
    return 0;
  if (jit_trace_flush_handoff_l(L, J, handle))
    return 1;
  jit_trace_flush_rollback_l(
    L, J, tg, handle, NULL, LJ_JIT_STREAM_DETACHED_PENDING);
  return 0;
}

int lj_jit_trace_flush_callback_admit_l(
  lua_State *L, jit_State *J, uint32_t attachment_state,
  uint64_t attachment_generation, GCfunc *callback_handler,
  LJJitTraceStreamHandle *stream_handle,
  LJJitEventCallbackHandle *callback_handle)
{
  LJJitEventSessionSnapshot session;
  TGState *tg;
  int snapshot_state;
  if (!stream_handle || !callback_handle)
    return 0;
  memset(stream_handle, 0, sizeof(*stream_handle));
  memset(callback_handle, 0, sizeof(*callback_handle));
  if (!L || !J || !callback_handler ||
      (attachment_state != LJ_VMEVENT_ATTACHMENT_INITIAL &&
	attachment_state != LJ_VMEVENT_ATTACHMENT_PUBLISHED) ||
      !lj_vmevent_attachment_identity_valid(attachment_state,
					     attachment_generation) ||
      G(L) != J2G(J) || !(tg = jit_event_owner_tg(L)) ||
      !jit_trace_flush_publish_l(
	L, J, attachment_state, attachment_generation, callback_handler,
	6u, stream_handle))
    return 0;

  memset(&session, 0, sizeof(session));
  snapshot_state = lj_jit_event_session_snapshot_acquire(
    G(L), tg, &session);
  if (snapshot_state != LJ_JIT_EVENT_SNAPSHOT_ACTIVE ||
      session.generation != stream_handle->terminal_session.generation ||
      session.slot_index != stream_handle->terminal_session.slot ||
      session.event != LJ_JIT_EVENT_TRACE_FLUSH ||
      session.owner_mode != LJ_JIT_EVENT_OWNER_DETACHED_IMMUTABLE ||
      session.edge_proof != LJ_JIT_EVENT_EDGE_NONE ||
      session.attachment_state != attachment_state ||
      session.attachment_generation != attachment_generation ||
      session.callback_root_count != 1u ||
      session.callback_handler != callback_handler) {
    if (snapshot_state == LJ_JIT_EVENT_SNAPSHOT_ACTIVE &&
	LJ_UNLIKELY(!lj_jit_event_session_snapshot_release(&session)))
      abort();
    jit_trace_flush_rollback_l(
      L, J, tg, stream_handle, NULL, LJ_JIT_STREAM_DETACHED_PENDING);
    return 0;
  }
  if (!lj_jit_event_callback_claim_l(
	L, stream_handle->generation, &session, callback_handle)) {
    if (LJ_UNLIKELY(!lj_jit_event_session_snapshot_release(&session)))
      abort();
    jit_trace_flush_rollback_l(
      L, J, tg, stream_handle, NULL, LJ_JIT_STREAM_DETACHED_PENDING);
    return 0;
  }
  /* Claim makes the session immutable-close exclusion durable.  Never carry
  ** its temporary GC2 reader across token release or arbitrary Lua. */
  if (LJ_UNLIKELY(!lj_jit_event_session_snapshot_release(&session))) {
    jit_trace_flush_rollback_l(
      L, J, tg, stream_handle, callback_handle,
      LJ_JIT_STREAM_DETACHED_PENDING);
    return 0;
  }
  if (!jit_trace_flush_callback_phase_l(
	tg, L, stream_handle, callback_handle)) {
    jit_trace_flush_rollback_l(
      L, J, tg, stream_handle, callback_handle,
      LJ_JIT_STREAM_DETACHED_PENDING);
    return 0;
  }
  if (jit_trace_flush_handoff_l(L, J, stream_handle))
    return 1;
  jit_trace_flush_rollback_l(
    L, J, tg, stream_handle, callback_handle,
    LJ_JIT_STREAM_DETACHED_CALLBACK);
  return 0;
}

int lj_jit_trace_flush_close_l(lua_State *L, jit_State *J,
			       const LJJitTraceStreamHandle *handle)
{
  LJJitTraceStreamSnapshot snapshot;
  TGState *tg;
  global_State *g;
  LJJitOwnerWord owner_word;
  uint32_t phase;
  if (!L || !J || !handle || handle->generation == 0 ||
      !lj_vmevent_attachment_identity_valid(handle->attachment_state,
					     handle->attachment_generation) ||
      handle->callback_root_count > 1u ||
      ((handle->callback_handler != NULL) !=
	(handle->callback_root_count == 1u)) || G(L) != J2G(J) ||
      !(tg = jit_event_owner_tg(L)) || tg != lj_jit_owner_tg_l(L, J))
    return 0;
  g = tg->gl;
  phase = handle->callback_root_count != 0 ?
    LJ_JIT_STREAM_DETACHED_CALLBACK : LJ_JIT_STREAM_DETACHED_PENDING;
  if (!lj_tgregistry_key_equal(&handle->owner_key, &tg->registry_key) ||
      handle->owner_tid != lj_tg_tid_acq(tg) ||
      handle->owner_actor != lj_tg_actor_acq(tg) ||
      handle->owner_actor != lj_thr_actor_current() ||
      !lj_jit_event_callback_idle(tg) ||
      lj_jit_trace_stream_snapshot(g, &snapshot) !=
	LJ_JIT_STREAM_SNAPSHOT_ACTIVE ||
      snapshot.generation != handle->generation ||
      !jit_trace_flush_descriptor_exact_phase(
	jit_trace_stream(g), handle, phase) ||
      !jit_trace_flush_session_exact(tg, L, handle))
    return 0;
  owner_word = jit_owner_word_acq(g);
  if (jit_owner_token(owner_word) == handle->owner_tid ||
      jit_owner_lifecycle(owner_word) == handle->owner_tid)
    return 0;

  /* Storage-level detached safety permits a peer recorder. The global grammar
  ** becomes IDLE first; then the old immutable session is closed exactly and
  ** cannot invalidate that peer's owner word. */
  if (!jit_trace_flush_clear_exact(tg, L, handle, phase))
    return 0;
  if (LJ_UNLIKELY(!lj_jit_event_session_end_l(
	L, J, &handle->terminal_session)))
    abort();
  return 1;
}

int lj_jit_token_acquire_wait(jit_State *J)
{
  TGState *tg = J2TG(J);
  lua_State *L = tg ? lj_tg_load_cur_L(tg) : NULL;
  int had_stopreq = lj_safepoint_had_stopreq(L);
  if (lj_jit_token_held(J))
    return 0;
  for (;;) {
    if (lj_jit_token_try(J))
      return 1;
    lj_trace_state_abort(J);
    lj_safepoint_checkstop_fresh(L, lj_thr_retry_yield(L), had_stopreq);
  }
}

void lj_trace_abort(global_State *g)
{
  jit_State *J = G2J(g);
  lj_trace_state_abort(J);
}

/* Synchronous abort with error message. */
void lj_trace_err(jit_State *J, TraceError e)
{
  setnilV(&J->errinfo);  /* No error info. */
  setintV(J->L->top++, (int32_t)e);
  lj_err_throw(J->L, LUA_ERRRUN);
}

/* Synchronous abort with error message and error info. */
void lj_trace_err_info(jit_State *J, TraceError e)
{
  setintV(J->L->top++, (int32_t)e);
  lj_err_throw(J->L, LUA_ERRRUN);
}

/* -- Trace management ---------------------------------------------------- */

/* The current trace is first assembled in J->cur. The variable length
** arrays point to shared, growable buffers (J->irbuf etc.). When trace
** recording ends successfully, the current trace and its data structures
** are copied to a new (compact) GCtrace object.
*/

static TraceVec *tracevec_new(lua_State *L, MSize sizetrace)
{
  TraceVec *tv = (TraceVec *)lj_mem_new(L, tracevec_size(sizetrace));
  tv->sizetrace = sizetrace;
  tv->retire_epoch = 0;
  tracevec_retired_next_rel(tv, NULL);
  memset(tv->slot, 0, sizetrace*sizeof(GCRef));
  return tv;
}

static void tracevec_free(global_State *g, TraceVec *tv)
{
  lj_mem_free(g, tv, tracevec_size(tv->sizetrace));
}

static void tracevec_publish(jit_State *J, TraceVec *tv)
{
  trace_sizetrace_rel(J, tv->sizetrace);
  tracevec_rel(J, tv);
}

static void tracevec_retired_push(jit_State *J, TraceVec *tv)
{
  TraceVec *head = tracevec_retired_head_acq(J);
  do {
    tracevec_retired_next_rel(tv, head);
  } while (!tracevec_retired_head_cas(J, &head, tv));
  /* 08 section 8.3 RCU retire. */
}

static void tracevec_preserve_retired(global_State *g, TraceVec *tv)
{
  /* The recorder token owns tv before the push and excludes its list reclaimer
  ** afterwards. The two publication-side marks are tactical root-certificate
  ** barriers, not the body-lifetime authority for this exact vector. */
  if (tv)
    (void)lj_gc2_markmem_registered_publish_try(g, tv);
}

static void tracevec_retire(jit_State *J, TraceVec *tv)
{
  if (tv) {
    global_State *g = J2G(J);
    la_store64_rel(&tv->retire_epoch, lj_gc2_retire_epoch(J2G(J)));
    /*
    ** Retired trace vectors are raw arena records, not ordinary Lua roots.
    ** Preserve both sides of publication: the first mark protects against an
    ** in-progress sweep, the second covers a GC2 root scan that starts between
    ** the mark and the CAS push. A later idle cycle clears stale marks before
    ** it starts tracing.
    */
    tracevec_preserve_retired(g, tv);
    tracevec_retired_push(J, tv);
    tracevec_preserve_retired(g, tv);
  }
}

static void trace_exittab_free(global_State *g, GCtrace *T, SnapNo nsnap);
static void trace_preservebody(global_State *g, GCtrace *T);

static LJArenaAllocD *trace_arena_allocd_for_tg(global_State *g, TGState *tg)
{
  if (tg && lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return &tg->allocd;
  return (LJArenaAllocD *)g->allocd;
}

static LJArenaAllocD *trace_arena_allocd_for_ptr(global_State *g, const void *p)
{
  if (p) {
    uint32_t owner_tid = lj_arena_owner_acq(lj_arena_of(p));
    TGState *tg = lj_tg_find_owner(g, owner_tid);
    if (tg)
      return trace_arena_allocd_for_tg(g, tg);
  }
  return (LJArenaAllocD *)g->allocd;
}

static int trace_body_fits_alloc(global_State *g, GCtrace *T, GCSize size)
{
  GCArena *a;
  uint32_t cell, maxcells;
  if (g->allocf != lj_arena_allocf)
    return 1;
  if (!T || size > LJ_MAX_MEM32)
    return 0;
  a = lj_arena_of(T);
  if (lj_arena_ishuge(a)) {
    LJArenaAllocD *ad = trace_arena_allocd_for_ptr(g, T);
    LJHugeInfo hi;
    return ad && ad->huge &&
	   lj_arena_hugetab_lookup(ad->huge, T, &hi) == 1 &&
	   hi.size == size;
  }
  cell = lj_arena_cellof(T);
  if (cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS)
    return 0;
  maxcells = LJ_ARENA_CELLS - cell;
  return lj_arena_ncells(size) <= maxcells;
}

static int trace_size_checked(global_State *g, GCtrace *T, GCSize *sizep,
			      SnapNo *nsnapp)
{
  IRRef nins, nk;
  SnapNo nsnap;
  MSize nsnapmap;
  GCSize size = (GCSize)((sizeof(GCtrace)+7)&~7);
  GCSize nref;
  const MSize snapmap_per_snap =
    (MSize)LJ_MAX_JSLOTS + (MSize)LJ_STACK_EXTRA + 32u;
  if (!T || !checkptrGC(T) || T->gct != (uint32_t)~LJ_TTRACE)
    return 0;
  nins = trace_nins_acq(T);
  nk = trace_nk_acq(T);
  nsnap = trace_nsnap_acq(T);
  nsnapmap = trace_nsnapmap_acq(T);
  /*
  ** A trace body's allocation size is reconstructed from its geometry at free
  ** time. Retired traces are still visible to stale exits and bytecode readers
  ** until their SMR epoch expires, so close/reclaim can observe a stale body if a
  ** duplicate retire entry slipped through. Validate the format invariants before
  ** using the header as an allocator contract.
  */
  if (nins < REF_BASE || nins > 0xffffu || nk > REF_BIAS || nk > nins)
    return 0;
  if ((nsnap == 0 && nsnapmap != 0) ||
      (nsnap != 0 && nsnapmap > (MSize)nsnap * snapmap_per_snap))
    return 0;
  nref = (GCSize)(nins - nk);
  if (nref > ((GCSize)LJ_MAX_MEM32 - size) / (GCSize)sizeof(IRIns))
    return 0;
  size += nref * (GCSize)sizeof(IRIns);
  if ((GCSize)nsnap > ((GCSize)LJ_MAX_MEM32 - size) /
			(GCSize)sizeof(SnapShot))
    return 0;
  size += (GCSize)nsnap * (GCSize)sizeof(SnapShot);
  if ((GCSize)nsnapmap > ((GCSize)LJ_MAX_MEM32 - size) /
			   (GCSize)sizeof(SnapEntry))
    return 0;
  size += (GCSize)nsnapmap * (GCSize)sizeof(SnapEntry);
  if (!trace_body_fits_alloc(g, T, size))
    return 0;
  *sizep = size;
  *nsnapp = nsnap;
  return 1;
}

static int trace_body_refs_valid(global_State *g, GCtrace *T, SnapNo *nsnapp)
{
  GCSize size;
  SnapNo nsnap;
  IRRef nins, nk;
  MSize nsnapmap;
  char *p;
  IRIns *irbase;
  SnapShot *snap;
  SnapEntry *snapmap;
  if (LJ_UNLIKELY(!trace_size_checked(g, T, &size, &nsnap)))
    return 0;
  nins = trace_nins_acq(T);
  nk = trace_nk_acq(T);
  nsnapmap = trace_nsnapmap_acq(T);
  p = (char *)T + ((sizeof(GCtrace)+7)&~7);
  irbase = trace_ir_acq(T);
  /*
  ** Published trace bodies are compact: IR, snapshots and snapmap live inside
  ** the same allocation immediately after GCtrace. Retired bodies can be seen
  ** through stale list entries until SMR completes, so do not trust derived
  ** payload pointers unless they still match the compact layout.
  */
  if (!irbase || &irbase[nk] != (IRIns *)p)
    return 0;
  p += (MSize)(nins - nk) * sizeof(IRIns);
  snap = trace_snap_acq(T);
  if (snap != (SnapShot *)p)
    return 0;
  p += (MSize)nsnap * sizeof(SnapShot);
  snapmap = trace_snapmap_acq(T);
  if (snapmap != (SnapEntry *)p)
    return 0;
  p += nsnapmap * sizeof(SnapEntry);
  if (p != (char *)T + size)
    return 0;
  if (nsnapp)
    *nsnapp = nsnap;
  return 1;
}

/* Validate only the immutable fields which distinguish an assembler scratch
** allocation from a semantic trace body. Retire-list consumers use this before
** exact destruction; they must never infer snapshot, prototype, exit-table or
** native-code ownership from a scratch body's reserved compact storage. */
static int trace_unpublished_scratch_valid(GCtrace *T)
{
  uint8_t flags;
  if (!T || !trace_retired_unpublished_acq(T))
    return 0;
  flags = la_load8_acq(&T->unused1);
  if (flags != TRACE_RETIRED_UNPUBLISHED ||
      trace_traceno_acq(T) != 0 || trace_link_acq(T) != 0 ||
      trace_root_acq(T) != 0 || trace_nextroot_acq(T) != 0 ||
      trace_nextside_acq(T) != 0 || trace_startptgco_acq(T) != NULL ||
      trace_startpc_acq(T) != NULL || trace_snap_acq(T) != NULL ||
      trace_snapmap_acq(T) != NULL || trace_szmcode_acq(T) != 0 ||
      trace_mcode_acq(T) != NULL || trace_exittab_acq(T) != NULL ||
      trace_exitstub_acq(T) != NULL || trace_native_pins_acq(T) != 0 ||
      !trace_native_pin_closed_acq(T) ||
      la_load64_acq(&T->retire_epoch) == 0)
    return 0;
#ifdef LUAJIT_USE_GDBJIT
  if (trace_gdbjit_entry_acq(T) != NULL)
    return 0;
#endif
  return 1;
}

/*
** Insert an embedded trace body while owning the sole recorder token. GC uses
** the same one-shot, nonwaiting token protocol as the recorder, so reclaimer
** detach/free and list insertion are serialized and no containment scan or
** same-node help protocol can create a duplicate/cycle.
*/
static void trace_retired_publish_token(jit_State *J, GCtrace *T)
{
  uintptr_t link = trace_retired_link_acq(T);
  lj_assertJ(lj_jit_token_held(J),
	     "trace retire-list insertion without recorder token");
  if (link & TRACE_RETIRED_LINK_LISTED)
    return;
  lj_assertJ(link == TRACE_RETIRED_LINK_UNLINKED,
	     "trace retire-list node has incomplete link state");
  for (;;) {
    GCtrace *head = trace_retired_head_acq(J);
    trace_retired_link_rel(T, (uintptr_t)head | TRACE_RETIRED_LINK_LISTED);
    if (trace_retired_head_cas(J, &head, T))
      break;
    /* Only a token protocol violation can change this head. Rebuild the
    ** unpublished descriptor defensively in release builds and retry.
    */
    lj_assertJ(0, "concurrent trace retire-list writer");
  }
}

static LJ_AINLINE uint64_t trace_retire_stamp(uint64_t epoch)
{
  /* Reserve zero for a live trace. Saturation is conservative at epoch wrap. */
  return epoch >= UINT64_MAX-1u ? UINT64_MAX : epoch + 1u;
}

static LJ_AINLINE uint64_t trace_retire_epoch_decode(uint64_t stamp)
{
  return stamp == UINT64_MAX ? UINT64_MAX : stamp - 1u;
}

static int trace_retired_payload_grace_active(global_State *g, GCtrace *T)
{
  uint64_t completed_epoch, retire_epoch;
  if (!g || !T)
    return 0;
  retire_epoch = la_load64_acq(&T->retire_epoch);
  if (retire_epoch == 0)
    return 0;
  retire_epoch = trace_retire_epoch_decode(retire_epoch);
  completed_epoch = lj_gc2_retire_epoch(g);
  return completed_epoch < retire_epoch ||
	 completed_epoch - retire_epoch < LJ_FLUSH_EPOCHS;
}

static int trace_retired_needs_payload_preserve(global_State *g, GCtrace *T)
{
  if (trace_retired_unpublished_acq(T))
    return 0;
  /*
  ** A public slot reservation remains an exit-restore name through its SMR grace
  ** generations. The raw body alone is not enough: snapshot restore and stale
  ** exits may still need the compact IR/snapshot/KGC payload while the slot names
  ** this retired body. Non-public retired bodies only need that payload during
  ** the same grace window.
  */
  if (trace_traceno_acq(T) != 0 || trace_nextroot_acq(T) != 0)
    return 1;
  return trace_retired_payload_grace_active(g, T);
}

static int trace_preservebody_raw_(global_State *g, GCtrace *T,
				   int reclaim_held)
{
  if (reclaim_held) {
    if (lj_gc2_markmem_reclaim_held_status(g, T) < 0)
      return 0;
  } else {
    (void)lj_gc2_markmem(g, T);
  }
  /* An unpublished assembler copy has only an immutable compact IR payload.
  ** It never owns snapshot, exit-table or native-code fields, and its KGC
  ** operands cease to be semantic roots when the failed recording is aborted.
  ** Retain the exact allocation without decoding it as a published trace. */
  if (trace_retired_unpublished_acq(T) ||
      LJ_UNLIKELY(!trace_body_refs_valid(g, T, NULL)))
    return 1;  /* The exact body is preserved; malformed children stay opaque. */
  {
    MCode **exittab = trace_exittab_acq(T);
    if (exittab && !trace_exittab_ismcode(T)) {
	if (reclaim_held) {
	  if (lj_gc2_markmem_reclaim_held_status(g, exittab) < 0)
	    return 0;
	} else {
	(void)lj_gc2_markmem(g, exittab);
	}
    }
  }
  return 1;
}

static void trace_preservebody_raw(global_State *g, GCtrace *T)
{
  (void)trace_preservebody_raw_(g, T, 0);
}

/* Tactical publication for a recorder-owned assembler scratch allocation.
** The construction owner retains T until its retire-list CAS, and list
** membership owns it afterwards. A failed one-shot admission requests a root
** retry without reading T; no semantic child graph exists for this body kind. */
static int trace_preserve_unpublished_publish(global_State *g, GCtrace *T)
{
  if (!g || !T)
    return 0;
  return lj_gc2_markmem_registered_publish_try(g, T);
}

static void trace_preserve_retired_body(global_State *g, GCtrace *T)
{
  if (trace_retired_unpublished_acq(T))
    trace_preservebody_raw(g, T);
  else if (trace_retired_needs_payload_preserve(g, T))
    trace_preservebody(g, T);
  else
    trace_preservebody_raw(g, T);
}

static void trace_preserve_retired_publish(global_State *g, GCtrace *T)
{
  trace_preserve_retired_body(g, T);
}

static void trace_retired_push_preserved(jit_State *J, GCtrace *T)
{
  global_State *g = J2G(J);
  /*
  ** Retired trace bodies can be named by stale patched bytecode, exit restore,
  ** or mcode until their SMR epoch completes. Preserve before and after list
  ** publication for the same race covered by tracevec_retire(): active sweep
  ** may already be running, or a root scan may begin between the local mark and
  ** the CAS push.
  */
  lj_assertJ(lj_jit_token_held(J),
	     "retired trace preservation without recorder token");
  trace_preserve_retired_publish(g, T);
  trace_retired_publish_token(J, T);
  trace_preserve_retired_body(g, T);
}

/* Requeue from inside the exclusive SMR writer. Entering an ordinary reader
** here would wait for this same writer to finish and deadlock the safepoint
** leader. The writer gate and recorder token already protect every detached
** trace/list body consumed by this path. */
static void trace_retired_push_preserved_reclaim(jit_State *J, GCtrace *T)
{
  global_State *g = J2G(J);
  lj_assertJ(lj_gc2_jit_reclaim_context_acq(g) && lj_jit_token_held(J),
	     "retired trace requeue without exclusive reclaim ownership");
  /* Requeue does not create or extend a semantic edge: retirement publication
  ** or this cycle's retired-root scan already preserved every proto/KGC child.
  ** The exclusive SMR writer excludes a concurrent retired-root scan while the
  ** node is detached. Retain the raw body on both sides of republishing so an
  ** arena owner cannot consume the detached allocation, but do not repeat an
  ** unchanged snapshot-PC ownership-spine traversal for every 64-cell sweep
  ** slice. The next cycle's root scan will close the graph again before sweep. */
  if (LJ_UNLIKELY(!trace_preservebody_raw_(g, T, 1))) {
    /* A detached-list ticket authorizes the current body access, but arena
    ** quarantine only observes its durable mark (and the exittab needs its own
    ** mark). Never publish a list node whose physical allocations were not
    ** preserved: that would defer a deterministic UAF into a later epoch. */
    lj_assertJ(0, "retired trace requeue without durable allocation marks");
    abort();
  }
  trace_retired_publish_token(J, T);
  if (LJ_UNLIKELY(!trace_preservebody_raw_(g, T, 1))) {
    lj_assertJ(0, "retired trace publication lost allocation marks");
    abort();
  }
}

/*
** The retirement LP is the encoded epoch claim while holding the sole recorder
** token. The same owner then closes native-pin admission in the count word and
** publishes the intrusive retire node before releasing ownership; slot or
** semantic unlink can only follow both publications. Thus a native pin either
** increments before CLOSED and is retained by slot disposition, or observes
** CLOSED and fails. GC never waits for the token and simply retries its bounded
** root/quarantine item after requesting an asynchronous recorder abort.
*/
static void trace_native_pin_close(GCtrace *T)
{
  uint32_t word = trace_native_pinword_acq(T);
  while ((word & TRACE_NATIVE_PIN_CLOSED) == 0) {
    if (la_cas32(&T->native_pins, &word, word | TRACE_NATIVE_PIN_CLOSED,
		 LA_ACQ_REL, LA_ACQ))
      return;
  }
}

static void trace_retire_claim_at_epoch(global_State *g, GCtrace *T,
					uint64_t epoch, int unpublished)
{
  uint64_t expect = 0;
  uint64_t stamp = trace_retire_stamp(epoch);
#if defined(LJ_TRACE_TEST_HELPERS) || defined(LJ_GC2_TEST_HELPERS)
  (void)la_add32_acqrel(&trace_test_retire_publish_calls, 1);
#endif
  /* Preserve in GC2 before the unique entry gate; a root-prune caller can then
  ** unlink only after this helper returns without opening an unpreserved gap.
  ** Only the recorder's exact unpublished construction may use the tactical
  ** raw marker. It is an explicitly nonsemantic retire-list kind. */
  if (unpublished) {
    lj_assertG(trace_traceno_acq(T) == 0 && trace_nextroot_acq(T) == 0 &&
	       la_load64_acq(&T->retire_epoch) == 0 &&
	       !trace_retired_link_listed_acq(T),
	       "invalid unpublished trace pre-claim publication");
    trace_retired_unpublished_set_rel(T);
    (void)trace_preserve_unpublished_publish(g, T);
  } else {
    lj_assertG(!trace_retired_unpublished_acq(T),
	       "published trace carries unpublished retire kind");
    trace_preserve_retired_publish(g, T);
  }
  (void)la_cas64(&T->retire_epoch, &expect, stamp, LA_ACQ_REL, LA_ACQ);
  trace_native_pin_close(T);
}

static int trace_retire_discoverable_acq(jit_State *J, GCtrace *T)
{
  TraceVec *tv;
  TraceNo traceno;
  GCobj *slot;
  /* Once listed, the token-owned retire stack is authoritative even if its
  ** public trace slot has already been disconnected.
  */
  if (trace_retired_link_listed_acq(T))
    return 1;
  traceno = trace_traceno_acq(T);
  if (traceno == 0 && la_load64_acq(&T->retire_epoch) != 0)
    traceno = trace_nextroot_acq(T);  /* Retired public-slot reservation. */
  tv = tracevec_acq(J);
  if (traceno != 0 && tv && (MSize)traceno < tv->sizetrace) {
    slot = gcref_acq(tv->slot[traceno]);
    if (slot == obj2gco(T))
      return 1;
  }
  /* A token owner always lists before clearing its exact slot. Recheck the
  ** list publication after a failed slot snapshot to close that handoff race.
  */
  return trace_retired_link_listed_acq(T);
}

static int trace_has_runnable_inbound_link(jit_State *J, GCtrace *target)
{
  TraceVec *tv = tracevec_acq(J);
  TraceNo targetno = trace_traceno_acq(target);
  TraceNo i;
  if (targetno == 0 && la_load64_acq(&target->retire_epoch) != 0)
    targetno = trace_nextroot_acq(target);
  if (targetno == 0 || tv == NULL)
    return 0;
  /*
  ** The recorder token serializes the assembler's validate->publish window and
  ** every semantic trace-link rewrite. Thus a single reverse scan is the
  ** retirement admission check: once it reports no runnable terminal source,
  ** no new persistent mcode edge can appear before the target's epoch claim and
  ** disconnect transaction complete.
  **
  ** A runnable but otherwise unreachable source may conservatively rescue its
  ** target for one collection. The source is retired later in the same root
  ** pass and the target becomes collectible in the next cycle. This bounded
  ** retention is required because root-spine order does not tell admission
  ** whether the source is semantically live; retiring the target first would
  ** leave an unretargetable native edge and an immortal deferred arena cell.
  */
  for (i = 1; (MSize)i < tv->sizetrace; i++) {
    GCtrace *T = traceref_fromgco(gcref_acq(tv->slot[i]));
    if (T && T != target && trace_runnable_acq(T, i) &&
	trace_link_acq(T) == targetno)
      return 1;
  }
  return 0;
}

static void trace_rescue_runnable_target(global_State *g, GCtrace *T)
{
  GCobj *o = obj2gco(T);
  /*
  ** The reverse edge is semantic reachability, not stale-reader body
  ** retention. Reopen the GC2 traversal frontier before declining retirement.
  ** During sweep this publishes through the current TG's SSB; during an earlier
  ** phase the same helper performs an ordinary GC2 semantic mark. The root-prune
  ** cursor then retries the same body and observes its new mark instead of
  ** transferring it to RETIRED.
  */
  (void)lj_gc2_trace_sweep_root(g, o);
}

static void trace_retire_at_epoch(global_State *g, GCtrace *T,
				  uint64_t epoch);

int LJ_FASTCALL lj_trace_retire_gc_claim(global_State *g, GCtrace *T)
{
  jit_State *J;
  int token = 0;
  int discoverable;
  if (!g || !T)
    return 0;
  J = G2J(g);
  if (la_load64_acq(&T->retire_epoch) != 0 &&
      trace_retire_discoverable_acq(J, T))
    return 1;
  /* Recorder/assembler target validation and final direct-link publication are
  ** token-private, but an epoch-only GC claim would otherwise race between
  ** those two operations and retire a target newly published into machine code.
  ** Take the token once without waiting, publish the retire node before release,
  ** and let the bounded root/quarantine pass retry if a recorder won. Abort that
  ** recorder asynchronously so continuous compilation cannot starve sweep.
  */
  if (!lj_jit_token_held(J)) {
    if (!lj_jit_token_try(J)) {
      lj_trace_state_abort(J);
      return 0;
    }
    token = 1;
  }
  /* A terminal link is a persistent native inbound edge. It cannot be
  ** retargeted by disconnecting the target, so admit retirement only after the
  ** token-protected reverse scan proves that no runnable source still names it.
  */
  if (trace_has_runnable_inbound_link(J, T)) {
    trace_rescue_runnable_target(g, T);
    if (token)
      lj_jit_token_release(J);
    return 0;
  }
  trace_retire_at_epoch(g, T, lj_gc2_retire_epoch(g));
  trace_retire_disconnect(J, T);
  discoverable = trace_retire_discoverable_acq(J, T);
  if (token)
    lj_jit_token_release(J);
  return discoverable;
}

static void trace_retire_at_epoch(global_State *g, GCtrace *T, uint64_t epoch)
{
  jit_State *J = G2J(g);
  lj_assertJ(lj_jit_token_held(J),
	     "trace retire-list publication without recorder token");
  trace_retire_claim_at_epoch(g, T, epoch, 0);
  trace_retired_push_preserved(J, T);
}

static void trace_retire_unpublished(global_State *g, GCtrace *T)
{
  jit_State *J = G2J(g);
  lj_assertJ(lj_jit_token_held(J),
	     "unpublished trace retire-list publication without recorder token");
  trace_retire_claim_at_epoch(g, T, lj_gc2_retire_epoch(g), 1);
  /* The token/local-construction owner covers the pre-CAS side; the tagged
  ** retire-list node is the exact lifetime descriptor afterwards. The second
  ** tactical mark closes a root snapshot between the first mark and list CAS.
  ** Both operations are one-shot and neither decodes semantic trace fields. */
  trace_retired_publish_token(J, T);
  (void)trace_preserve_unpublished_publish(g, T);
}

static void trace_retire(global_State *g, GCtrace *T)
{
  trace_retire_at_epoch(g, T, lj_gc2_retire_epoch(g));
}

/*
** Acquire an exact trace-body lease for native execution. The caller must
** already own an independent lifetime proof for T (currently jit_base/JIT
** activity; later the generic FFI entry handoff). This prerequisite closes the
** zero-to-one race with a reclaimer which has already observed no pins. A raw
** pointer obtained without such a lease is not safe input to this API.
**
** Retirement may start before or after this increment. Once published, the
** pin keeps the retired body, its public slot reservation and every referenced
** mcode area alive until the matching release.
*/
int LJ_FASTCALL lj_trace_native_pin(GCtrace *T)
{
  uint32_t word;
  if (LJ_UNLIKELY(T == NULL))
    return 0;
  word = trace_native_pinword_acq(T);
  while ((word & TRACE_NATIVE_PIN_CLOSED) == 0 &&
	 (word & TRACE_NATIVE_PIN_COUNT_MASK) != TRACE_NATIVE_PIN_COUNT_MASK) {
    if (la_cas32(&T->native_pins, &word, word + 1u,
		 LA_ACQ_REL, LA_ACQ))
      return 1;
  }
  return 0;
}

static void trace_native_pin_release_notify(global_State *g)
{
  jit_State *J = G2J(g);
  uint64_t seq = la_load64_acq(&J->trace_pin_release_seq);
  while (!la_cas64(&J->trace_pin_release_seq, &seq, seq + 1u,
		   LA_ACQ_REL, LA_ACQ))
    ;
}

void LJ_FASTCALL lj_trace_native_unpin(global_State *g, GCtrace *T)
{
  uint32_t word;
  if (LJ_UNLIKELY(g == NULL || T == NULL))
    abort();
  word = trace_native_pinword_acq(T);
  for (;;) {
    uint32_t count = word & TRACE_NATIVE_PIN_COUNT_MASK;
    uint32_t next;
    if (LJ_UNLIKELY(count == 0)) {
      lj_assertX(0, "trace native pin underflow");
      abort();
    }
    next = (word & TRACE_NATIVE_PIN_CLOSED) | (count - 1u);
    if (la_cas32(&T->native_pins, &word, next, LA_ACQ_REL, LA_ACQ)) {
      /* Only a final release of a retired body can unblock a memoized mature
      ** trace/mcode scan. Publish one shared notification after the count. */
      if (count == 1u && (word & TRACE_NATIVE_PIN_CLOSED) != 0)
	trace_native_pin_release_notify(g);
      return;
    }
  }
}

static int trace_freebody_(global_State *g, GCtrace *T, int reclaim_held,
			   int terminal)
{
  GCSize size;
  SnapNo nsnap;
  LJGCDestructCtx dctx;
  int acquired;
  int unpublished = trace_retired_unpublished_acq(T);
  if (LJ_UNLIKELY(unpublished &&
		  !trace_unpublished_scratch_valid(T))) {
    /* The immutable kind bit is exact destruction authority only together with
    ** the scratch shape. Runtime reclaim retries fail-closed; terminal close
    ** cannot safely guess which side allocation a corrupt body might own. */
    if (terminal) {
      lj_assertG(0, "invalid unpublished trace scratch at VM close");
      abort();
    }
    return 0;
  }
  if (LJ_UNLIKELY(trace_native_pins_acq(T) != 0)) {
    /* Runtime retirement leaves the exact body list-owned and retries. VM close
    ** has no later retry point: an outstanding native frame is a violated
    ** teardown precondition and must never become a silent executable UAF. */
    if (terminal) {
      lj_assertG(0, "terminal trace destruction with native execution pin");
      abort();
    }
    return 0;
  }
  if (LJ_UNLIKELY(!trace_size_checked(g, T, &size, &nsnap)))
    return 0;
  if (terminal && g->allocf == lj_arena_allocf &&
      la_load32_acq(&g->allocf_arena) != 0) {
    GCArena *a = lj_arena_of(T);
    if (!lj_arena_ishuge(a) && !lj_arena_terminal_reconcile(a))
      abort();
  }
  acquired = reclaim_held ?
    lj_gc_destructor_enter_reclaim_held(g, T, size, &dctx) :
    lj_gc_destructor_enter(g, T, size, &dctx);
  if (acquired == LJ_GC_DESTRUCT_OWNED)
    return 1;
  if (acquired != LJ_GC_DESTRUCT_ACQUIRED)
    return 0;
  /* Runtime reclaim only reaches here after the nonwaiting debugger unregister
  ** succeeded. At VM close, lj_trace_freeretired() performs the exceptional
  ** teardown unregister before entering this helper.
  */
#ifdef LUAJIT_USE_GDBJIT
  lj_assertG(trace_gdbjit_entry_acq(T) == NULL,
	     "trace freed with live GDB JIT descriptor");
#endif
  if (!unpublished)
    trace_exittab_free(g, T, nsnap);
  /* This release is the exact physical-destructor completion signal consumed
  ** by bounded arena quarantine. The trace retire list is the sole owner of
  ** payload/exittab destruction; a quarantined arena retains the allocation
  ** bitmap until it observes this byte with acquire ordering.
  */
  la_store8_rel(&T->gct, 0);
  lj_mem_free(g, T, size);
  lj_gc_destructor_leave(g, &dctx);
  return 1;
}

int LJ_FASTCALL lj_trace_body_destroyed_acq(const GCtrace *T)
{
  return T && la_load8_acq(&T->gct) == 0;
}

static void trace_free_immediate(global_State *g, GCtrace *T)
{
  GCSize size;
  SnapNo nsnap;
  LJGCDestructCtx dctx;
  int acquired;
  if (LJ_UNLIKELY(trace_native_pins_acq(T) != 0)) {
    lj_assertG(0, "immediate trace destruction with native execution pin");
    abort();
  }
  if (LJ_UNLIKELY(!trace_size_checked(g, T, &size, &nsnap)))
    return;
  acquired = lj_gc_destructor_enter(g, T, size, &dctx);
  if (acquired != LJ_GC_DESTRUCT_ACQUIRED)
    return;
  trace_exittab_free(g, T, nsnap);
  T->gct = 0;  /* Unpublished aborts may race with preserved retired scans. */
  lj_mem_free(g, T, size);
  lj_gc_destructor_leave(g, &dctx);
}

void LJ_FASTCALL lj_trace_free_unpublished(global_State *g, GCtrace *T)
{
  jit_State *J = G2J(g);
  int abandoned;
  /* GC2 may have acquired J->curfinal immediately before the owner replaces or
  ** aborts it. Use the ordinary epoch/SMR retire path even though no trace slot
  ** was published; this keeps the compact body immutable until that reader exits.
  ** Unlike a public GC claim, this body has no discovery slot, so its recorder
  ** owner must insert it while retaining the token.
  */
  lj_assertJ(lj_jit_token_held(J),
	     "unpublished trace retirement without recorder token");
  abandoned = lj_mem_abandon_gco_unpublished(g, T);
  if (LJ_UNLIKELY(abandoned != LJ_ARENA_HUGE_ROOT_COMPLETE_LIVE)) {
    /* A second logical owner for an unpublished recorder body is corruption;
    ** do not enqueue a pointer whose mapping may already be sweep-owned. */
    lj_assertJ(0, "unpublished trace construction ownership lost");
    abort();
  }
  trace_retire_unpublished(g, T);
}

static LJ_AINLINE int trace_body_retire_ready(GCtrace *T,
					       uint64_t completed_epoch)
{
  uint64_t stamp = la_load64_acq(&T->retire_epoch);
  uint64_t retire_epoch;
  if (stamp == 0)
    return 0;
  retire_epoch = trace_retire_epoch_decode(stamp);
  return completed_epoch >= retire_epoch &&
	 completed_epoch - retire_epoch >= LJ_FLUSH_EPOCHS;
}

static int trace_preserve_body_candidate(global_State *g, GCobj *o,
					 uint32_t *gctp,
					 LJGC2Lease *lease)
{
  uint32_t gct;
  if (!lease)
    return 0;
  memset(lease, 0, sizeof(*lease));
  /* Every successful caller is about to preserve or inspect an object reached
  ** through a trace-owned edge. Keep the counted admission in the caller's
  ** lease until its final gct/proto/trace/gcw read; a status-only validator
  ** would drop the body lease before the type-specific dereference. */
  if (lj_gc2_obj_lease_acquire(g, o, 0, &gct, lease) < 0)
    return 0;
  if (LJ_UNLIKELY(gct == 0 || gct < (uint32_t)~LJ_TSTR ||
		  gct > (uint32_t)~LJ_TUDATA)) {
    lj_gc2_lease_release(lease);
    return 0;
  }
  if (gctp)
    *gctp = gct;
  return 1;
}

static void trace_preserve_body_obj(global_State *g, GCobj *o)
{
  if (gc2_phase_acq(g) == LJ_GC2_IDLE)
    return;  /* No reclaim can race; active root scans republish this edge. */
  /* Retired KGC operands share the semantic arena mark domain. Queue their
  ** graph so this body pin cannot poison later ordinary marking. Retain first:
  ** the retired trace lease protects T/IR, not the child allocation. FINREG is
  ** allowed to observe the resulting physical cdata pin independently. */
  (void)lj_gc2_markobj(g, o);
}

static void trace_preserve_proto_obj(global_State *g, GCobj *o)
{
  uint32_t gct;
  if (gc2_phase_acq(g) == LJ_GC2_IDLE)
    return;  /* The retired body/list remains discoverable at activation. */
  /* Retired traces need prototype bodies for stale PC ownership checks. */
  if (lj_gc2_markobj_status(g, o, &gct) < 0 ||
      gct != (uint32_t)~LJ_TPROTO)
    return;
  /* The semantic mark plane cannot encode "body only". Queue traversal when
  ** active so a proto body pin never hides its graph from a later root. Trace
  ** unlink still owns the lifetime of the retired proto->trace edge. */
}

static GCproto *trace_proto_pc_candidate_valid(global_State *g, GCobj *o,
					       uint32_t gct,
					       const BCIns **bcp,
					       const BCIns **endp,
					       const LJGC2Lease *lease)
{
  GCproto *pt;
  const BCIns *bc;
  if (gct != (uint32_t)~LJ_TPROTO)
    return NULL;
  pt = gco2pt(o);
  if (!lj_gc2_valid_proto_for_traverse_held(g, pt, lease))
    return NULL;
  bc = proto_bc(pt);
  if (bcp)
    *bcp = bc;
  if (endp)
    *endp = bc + pt->sizebc;
  return pt;
}

static GCproto *trace_proto_pc_candidate(global_State *g, GCobj *o,
					 const BCIns **bcp,
					 const BCIns **endp,
					 LJGC2Lease *lease)
{
  GCproto *pt;
  uint32_t gct;
  if (!trace_preserve_body_candidate(g, o, &gct, lease))
    return NULL;
  pt = trace_proto_pc_candidate_valid(g, o, gct, bcp, endp, lease);
  if (!pt)
    lj_gc2_lease_release(lease);
  return pt;
}

static int trace_pc_in_proto_range(const BCIns *pc, const BCIns *bc,
				    MSize sizebc)
{
  uintptr_t p, b, bytes;
  if (!pc || !bc || sizebc == 0)
    return 0;
  p = (uintptr_t)pc;
  b = (uintptr_t)bc;
  bytes = (uintptr_t)sizebc * sizeof(BCIns);
  if (bytes / sizeof(BCIns) != (uintptr_t)sizebc)
    return 0;
  return p >= b && p - b < bytes &&
	 ((p - b) % sizeof(BCIns)) == 0;
}

#ifdef LJ_TRACE_TEST_HELPERS
int lj_trace_test_preserve_body_candidate(global_State *g, GCobj *o)
{
  LJGC2Lease lease;
  int valid = trace_preserve_body_candidate(g, o, NULL, &lease);
  lj_gc2_lease_release(&lease);
  return valid;
}

int lj_trace_test_proto_pc_candidate(global_State *g, GCobj *o,
				     const BCIns *pc)
{
  LJGC2Lease lease;
  const BCIns *bc, *end;
  GCproto *pt = pc ? trace_proto_pc_candidate(g, o, &bc, &end, &lease) :
		    NULL;
  int valid = pt && trace_pc_in_proto_range(pc, bc, (MSize)(end - bc));
  if (pc)
    lj_gc2_lease_release(&lease);
  return valid;
}
#else
#if LJ_TARGET_ARM64 && LJ_HASJIT
#define trace_test_root_entry_pause_at(stage) ((void)0)
#define trace_test_root_entry_published() ((void)0)
#define trace_test_root_entry_cleaned() ((void)0)
#endif
#endif

#if LJ_TARGET_ARM64 && LJ_HASJIT
static LJ_AINLINE int trace_root_entry_source_valid(uint32_t sourceop)
{
  return sourceop == (uint32_t)BC_JLOOP ||
	 sourceop == (uint32_t)BC_JFUNCF;
}

static LJ_AINLINE int trace_root_entry_start_valid(uint32_t sourceop,
					    BCIns startins)
{
  BCOp op = bc_op(startins);
  if (sourceop == (uint32_t)BC_JFUNCF)
    return op == BC_FUNCF;
  return op == BC_ITERL || op == BC_ITERN || op == BC_LOOP || bc_isret(op);
}

static LJ_AINLINE GCtrace *trace_root_entry_slot_acq(jit_State *J,
					      TraceNo traceno,
					      TraceVec **tvp)
{
  TraceVec *tv = tracevec_acq(J);
  GCobj *o;
  *tvp = tv;
  if (traceno == 0 || tv == NULL || (MSize)traceno >= tv->sizetrace)
    return NULL;
  o = gcref_acq(tv->slot[traceno]);
  return traceref_fromgco_safe(o);
}

static LJ_AINLINE int trace_root_entry_bytecode_valid(const BCIns *pc,
					       TraceNo traceno,
					       uint32_t sourceop)
{
  BCIns ins = (BCIns)la_load32_acq((const uint32_t *)pc);
  return (uint32_t)bc_op(ins) == sourceop && (TraceNo)bc_d(ins) == traceno;
}

/* Validate and publish one root-trace entry intent without allocating,
** waiting, entering SMR, invoking callbacks or raising an error. The gate
** close/recheck handshake makes the published TG-local jit_base the lifetime
** lease for every metadata and mcode load below. Rejection never repairs stale
** bytecode: the VM caller must reload it and redispatch after this returns. */
LJTraceRootEntry LJ_FASTCALL
lj_trace_enter_root(jit_State *J, const BCIns *pc, TraceNo traceno,
		    lua_State *L, TValue *base, uint32_t sourceop)
{
  LJTraceRootEntry result = { NULL, NULL };
  global_State *g;
  TGState *tg;
  TValue *stack, *maxstack, *top;
  uintptr_t basep, stackp, maxstackp, topp, mcodep;
  TraceVec *tv, *tv2;
  GCtrace *T, *T2;
  BCIns startins;
  MCode *mcode;
  MSize szmcode;
  ASMFunction target;

  if (J == NULL || pc == NULL || L == NULL || base == NULL || traceno == 0 ||
      !trace_root_entry_source_valid(sourceop) ||
      ((uintptr_t)(const void *)pc & (sizeof(BCIns)-1u)) != 0)
    return result;
  g = G(L);
  if (g == NULL || J != G2J(g) || (tg = L->tg_hint) == NULL || tg->gl != g ||
      G2TG(g) != tg || lj_tg_flags_test_acq(tg, TGF_DEAD) ||
      lj_tg_load_cur_L(tg) != L || !lj_tg_owns_state_acq(tg, L) ||
      lj_tg_actor_acq(tg) != lj_thr_actor_current() || L->base != base)
    return result;
  stack = mref_acq(L->stack, TValue);
  maxstack = mref_acq(L->maxstack, TValue);
  top = L->top;
  basep = (uintptr_t)(void *)base;
  stackp = (uintptr_t)(void *)stack;
  maxstackp = (uintptr_t)(void *)maxstack;
  topp = (uintptr_t)(void *)top;
  if (stack == NULL || maxstack == NULL || maxstackp <= stackp ||
      basep < stackp || basep >= maxstackp ||
      top == NULL || topp < basep || topp > maxstackp ||
      (basep-stackp) % sizeof(TValue) != 0 ||
      (topp-stackp) % sizeof(TValue) != 0 ||
      lj_tg_load_jit_base(tg) != NULL || lj_tg_vmstate_load_acq(tg) > 0 ||
      lj_tg_in_native_acq(tg) != 0)
    return result;

  if (!lj_gc2_jit_entry_open(g)) {
    gc2_jit_sweep_displaced_rel(g, 1);
    return result;
  }
  if (gc2_jit_mark_auto_yield_acq(g) != 0)
    gc2_jit_mark_auto_yield_rel(g, 0);
  trace_test_root_entry_pause_at(LJ_TRACE_ROOT_ENTRY_PAUSE_PREPUBLISH);
  lj_tg_store_jit_base(tg, base);
  trace_test_root_entry_published();
  la_fence_seq();
  trace_test_root_entry_pause_at(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTPUBLISH);
  if (!lj_gc2_jit_entry_open(g)) {
    gc2_jit_sweep_displaced_rel(g, 1);
    goto reject_published;
  }

  T = trace_root_entry_slot_acq(J, traceno, &tv);
  if (!trace_runnable_acq(T, traceno) || trace_root_acq(T) != 0 ||
      trace_startpc_acq(T) != pc ||
      !trace_root_entry_start_valid(sourceop, trace_startins_acq(T)) ||
      !trace_root_entry_bytecode_valid(pc, traceno, sourceop))
    goto reject_published;
  startins = trace_startins_acq(T);
  mcode = trace_mcode_acq(T);
  szmcode = trace_szmcode_acq(T);
  mcodep = (uintptr_t)(void *)mcode;
  if (mcode == NULL || szmcode == 0 ||
      ((uintptr_t)szmcode & (sizeof(MCode)-1u)) != 0 ||
      (mcodep & (sizeof(MCode)-1u)) != 0 ||
      mcodep > ~(uintptr_t)0-(uintptr_t)szmcode)
    goto reject_published;
#if LJ_ABI_PAUTH
  target = trace_mcauth_acq(T);
  if (target == NULL || (uintptr_t)lj_ptr_strip(target) != mcodep)
    goto reject_published;
#else
  target = (ASMFunction)(void *)mcode;
#endif

  /* Reacquire the publication and every field that grants entry after the
  ** target loads. This catches slot replacement, retirement, repatching and a
  ** torn body view without extending authority beyond the TG lifetime lease. */
  T2 = trace_root_entry_slot_acq(J, traceno, &tv2);
  if (tv2 != tv || T2 != T || !trace_runnable_acq(T2, traceno) ||
      trace_root_acq(T2) != 0 || trace_startpc_acq(T2) != pc ||
      trace_startins_acq(T2) != startins ||
      !trace_root_entry_start_valid(sourceop, startins) ||
      trace_mcode_acq(T2) != mcode || trace_szmcode_acq(T2) != szmcode ||
      !trace_root_entry_bytecode_valid(pc, traceno, sourceop))
    goto reject_published;
#if LJ_ABI_PAUTH
  if (trace_mcauth_acq(T2) != target ||
      (uintptr_t)lj_ptr_strip(target) != mcodep)
    goto reject_published;
#endif
  setsbufL(&tg->tmpbuf, L);
  result.trace = T;
  result.target = target;
  return result;

reject_published:
  lj_tg_store_jit_base(tg, NULL);
  trace_test_root_entry_cleaned();
  return result;
}
#endif

static void trace_preserve_proto_for_pc(global_State *g, const BCIns *pc)
{
  if (!pc || gc2_phase_acq(g) == LJ_GC2_IDLE)
    return;
  /* proto_bc() starts immediately after GCproto, so allocator coverage can
  ** resolve this interior pointer to its exact allocation in bounded time.
  ** This avoids an O(root-spine) search per PC and, critically, has no shared
  ** walk budget that can silently drop an owner edge. */
  (void)lj_gc2_mark_proto_for_pc(g, pc);
}

static void trace_preserve_snapshot_pcs(global_State *g, GCtrace *T)
{
  LJGC2Lease startlease;
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  GCobj *startpt = trace_startptgco_acq(T);
  const BCIns *startbc = NULL, *startend = NULL;
  MSize nsnapmap = trace_nsnapmap_acq(T);
  SnapNo i, nsnap = trace_nsnap_acq(T);
  if (!snap || !snapmap)
    return;
  /* Every trace directly owns its starting prototype, which
  ** trace_preservebody_inner() has already preserved. Most snapshot PCs name
  ** that same bytecode body. Resolve its interval once and avoid restarting a
  ** potentially long ownership-spine walk for every such snapshot/requeue. */
  (void)trace_proto_pc_candidate(g, startpt, &startbc, &startend,
				 &startlease);
  (void)lj_gc_flush_root_pending(g);
  for (i = 0; i < nsnap; i++) {
    SnapShot *s = &snap[i];
    MSize ofs = snap_mapofs_acq(s);
    MSize nent = snap_nent_acq(s);
    SnapEntry *map;
    if (ofs >= nsnapmap || nent >= nsnapmap - ofs) {
      lj_gc2_lease_release(&startlease);
      return;
    }
    map = &snapmap[ofs];
    {
      const BCIns *pc = snap_pc_acq(&map[nent]);
      if (startbc && trace_pc_in_proto_range(
	    pc, startbc, (MSize)(startend - startbc)))
	continue;
      trace_preserve_proto_for_pc(g, pc);
    }
  }
  lj_gc2_lease_release(&startlease);
}

static void trace_preserve_kgc(global_State *g, GCtrace *T)
{
  IRIns *irbase = trace_ir_acq(T);
  IRRef ref;
  if (!irbase)
    return;
  for (ref = trace_nk_acq(T); ref < REF_TRUE; ref++) {
    IRIns *ir = &irbase[ref];
    IRIns irs = ir_load_acq(ir);
    if (irs.o == IR_KGC) {
      GCobj *o = ir_kgc_load_acq(ir);
      trace_preserve_body_obj(g, o);
    }
    if (irt_is64(irs.t) && irs.o != IR_KNULL)
      ref++;
  }
}

/* Checked preservation is reserved for a certified parked native frame. The
** ordinary retired-body walk above remains best-effort and preserves every
** edge it can; this variant instead rejects the frame snapshot when any exact
** child cannot be admitted, so GC2 never counts a partial graph as stable. */
static int trace_preserve_kgc_checked(global_State *g, GCtrace *T)
{
  IRIns *irbase = trace_ir_acq(T);
  IRRef ref;
  if (!irbase)
    return 0;
  for (ref = trace_nk_acq(T); ref < REF_TRUE; ref++) {
    IRIns *ir = &irbase[ref];
    IRIns irs = ir_load_acq(ir);
    if (irs.o == IR_KGC) {
      GCobj *o = ir_kgc_load_acq(ir);
      if (!o || lj_gc2_markobj_status(g, o, NULL) < 0)
	return 0;
    }
    if (irt_is64(irs.t) && irs.o != IR_KNULL)
      ref++;
  }
  return 1;
}

static int trace_preserve_snapshot_pcs_checked(global_State *g, GCtrace *T)
{
  LJGC2Lease startlease;
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  GCobj *startpt = trace_startptgco_acq(T);
  const BCIns *startbc, *startend;
  MSize nsnapmap = trace_nsnapmap_acq(T);
  SnapNo i, nsnap = trace_nsnap_acq(T);
  GCproto *pt;
  int status;

  memset(&startlease, 0, sizeof(startlease));
  if (!snap || !snapmap || !startpt)
    return 0;
  status = lj_gc2_obj_lease_acquire(g, startpt, (uint32_t)~LJ_TPROTO,
				     NULL, &startlease);
  if (status < 0)
    return 0;
  pt = gco2pt(startpt);
  if (!lj_gc2_valid_proto_for_traverse_held(g, pt, &startlease)) {
    lj_gc2_lease_release(&startlease);
    return 0;
  }
  startbc = proto_bc(pt);
  startend = startbc + pt->sizebc;
  (void)lj_gc_flush_root_pending(g);
  for (i = 0; i < nsnap; i++) {
    SnapShot *s = &snap[i];
    MSize ofs = snap_mapofs_acq(s);
    MSize nent = snap_nent_acq(s);
    MSize nextofs = snap_nextofs_acq(T, s);
    MSize remaining;
    SnapEntry *map;
    const BCIns *pc;
    if (ofs >= nsnapmap || nextofs < ofs || nextofs > nsnapmap) {
      lj_gc2_lease_release(&startlease);
      return 0;
    }
    remaining = nextofs - ofs;
    if (nent > remaining || remaining - nent < (MSize)(1 + LJ_FR2)) {
      lj_gc2_lease_release(&startlease);
      return 0;
    }
    map = &snapmap[ofs];
    pc = snap_pc_acq(&map[nent]);
    if (!trace_pc_in_proto_range(pc, startbc,
				 (MSize)(startend - startbc)) &&
	!lj_gc2_mark_proto_for_pc(g, pc)) {
      lj_gc2_lease_release(&startlease);
      return 0;
    }
  }
  lj_gc2_lease_release(&startlease);
  return 1;
}

static int trace_preservebody_inner_checked(global_State *g, GCtrace *T)
{
  LJGC2Lease bodylease, exitlease;
  GCobj *startpt;
  MCode **exittab;
  int status;

  memset(&bodylease, 0, sizeof(bodylease));
  memset(&exitlease, 0, sizeof(exitlease));
  status = lj_gc2_mem_lease_acquire(g, T, &bodylease);
  if (status < 0)
    return 0;
  if (LJ_UNLIKELY(!trace_body_refs_valid(g, T, NULL)) ||
      !trace_preserve_kgc_checked(g, T))
    goto fail;
  startpt = trace_startptgco_acq(T);
  if (!startpt || lj_gc2_markobj_expected_status(
	      g, startpt, (uint32_t)~LJ_TPROTO, NULL) < 0)
    goto fail;
  if (!trace_preserve_snapshot_pcs_checked(g, T))
    goto fail;
  if (!lj_gc2_mark_trace_slot_status(g, trace_link_acq(T)) ||
      !lj_gc2_mark_trace_slot_status(g, trace_nextroot_acq(T)) ||
      !lj_gc2_mark_trace_slot_status(g, trace_nextside_acq(T)))
    goto fail;
  exittab = trace_exittab_acq(T);
  if (exittab && !trace_exittab_ismcode(T)) {
    status = lj_gc2_mem_lease_acquire(g, exittab, &exitlease);
    if (status < 0)
      goto fail;
  }
  lj_gc2_lease_release(&exitlease);
  lj_gc2_lease_release(&bodylease);
  return 1;

fail:
  lj_gc2_lease_release(&exitlease);
  lj_gc2_lease_release(&bodylease);
  return 0;
}

static void trace_preservebody_inner(global_State *g, GCtrace *T)
{
  /* Retired traces are SMR-protected bodies, but stale bytecode readers can
  ** still redispatch through their start/snapshot PCs until the grace period
  ** completes. Preserve those prototype owners along with the body and
  ** auxiliary exit table. The compact body can also contain GC operands that
  ** stale machine-code readers/snapshot restorers may still load, so preserve
  ** those object bodies without treating the retired trace as a live trace slot.
  ** Published live traces mark the same graph through traversal.
  */
  (void)lj_gc2_markmem(g, T);
  if (LJ_UNLIKELY(!trace_body_refs_valid(g, T, NULL)))
    return;
  trace_preserve_kgc(g, T);
  trace_preserve_proto_obj(g, trace_startptgco_acq(T));
  trace_preserve_snapshot_pcs(g, T);
  /* A retire publisher can overlap the last semantic traversal that would have
  ** followed these direct trace links. Raw-marking T must not make that later
  ** traversal see ALREADY while silently dropping the targets. */
  lj_gc2_mark_trace_slot(g, trace_link_acq(T));
  lj_gc2_mark_trace_slot(g, trace_nextroot_acq(T));
  lj_gc2_mark_trace_slot(g, trace_nextside_acq(T));
  {
    MCode **exittab = trace_exittab_acq(T);
    if (exittab && !trace_exittab_ismcode(T))
      (void)lj_gc2_markmem(g, exittab);
  }
}

static void trace_preservebody(global_State *g, GCtrace *T)
{
  lj_gc2_smr_read_enter(g);
  trace_preservebody_inner(g, T);
  lj_gc2_smr_read_leave(g);
}

/* Preserve the complete semantic graph of an exact native-pinned body. A
** normal GC2 trace traversal deliberately ignores traceno==0, but retirement
** may clear that field while a generated native frame still owns the body.
** Validate through the reserved public slot before the first T dereference;
** the caller's frame pin keeps its IR/snapshot payload and allocation resident.
** Outgoing routing fields remain retirement-owned until post-call activation
** supplies the separate FLUSHJ/JIT-base exclusion documented for that path. */
int lj_trace_native_mark_pinned(global_State *g, GCtrace *T, TraceNo traceno)
{
  jit_State *J;
  GCtrace *slot;
  int valid = 0;
  if (!g || !T || traceno == 0)
    return 0;
  J = G2J(g);
  if (!lj_gc2_smr_read_try(g))
    return 0;
  slot = traceref_safe(J, traceno);
  if (slot != T || trace_native_pins_acq(slot) == 0 ||
      !trace_body_refs_valid(g, slot, NULL))
    goto out;
  if (!trace_preservebody_inner_checked(g, slot))
    goto out;
  slot = traceref_safe(J, traceno);
  valid = slot == T && trace_native_pins_acq(slot) != 0;
out:
  lj_gc2_smr_read_leave(g);
  return valid;
}

static int trace_retired_slot_clear(jit_State *J, TraceVec *tv, TraceNo traceno,
				    GCtrace *T)
{
  GCobj *slot;
  if (traceno == 0 || tv == NULL || (MSize)traceno >= tv->sizetrace)
    return 0;
  slot = gcref_acq(tv->slot[traceno]);
  if (slot == obj2gco(T)) {
    setgcrefrel(tv->slot[traceno], NULL);
    if (J->freetrace == 0 || traceno < J->freetrace)
      J->freetrace = traceno;
    return 1;
  }
  return 0;
}

static void trace_retired_slot_release(jit_State *J, GCtrace *T)
{
  TraceNo traceno = trace_traceno_acq(T);
  int cleared = 0;
  TraceVec *tv;
  lj_assertJ(lj_jit_token_held(J),
	     "trace slot release without recorder-token ownership");
  if (LJ_UNLIKELY(trace_native_pins_acq(T) != 0)) {
    lj_assertJ(0, "trace slot released with native execution pin");
    abort();
  }
  if (traceno == 0 && la_load64_acq(&T->retire_epoch) != 0)
    traceno = trace_nextroot_acq(T);
  tv = tracevec_acq(J);
  cleared = trace_retired_slot_clear(J, tv, traceno, T);
  if (!cleared && tv) {
    TraceNo i;
    /*
    ** Retired traces normally reserve their public slot number in nextroot.
    ** Reclaim runs outside the recorder token, while trace-vector growth and
    ** root-chain unlinking are independent publications. If that compact
    ** metadata no longer names the current slot, release by exact body identity
    ** before the body is freed; this is a cold SMR cleanup path, not a semantic
    ** trace root walk.
    */
    for (i = 1; i < tv->sizetrace; i++) {
      if (i != traceno && trace_retired_slot_clear(J, tv, i, T)) {
	traceno = i;
	cleared = 1;
	break;
      }
    }
  }
  trace_test_note_slot_release(traceno, cleared);
  trace_nextroot_rel(T, 0);
  trace_traceno_rel(T, 0);
}

static void trace_slot_retire(jit_State *J, GCtrace *T, TraceNo traceno)
{
  global_State *g = J2G(J);
  lj_assertJ(lj_jit_token_held(J),
	     "trace slot retirement without recorder-token ownership");
  if (mt_active_or_entering_acq(g) || gc2_n_threads_acq(g) > 1 ||
      trace_native_pins_acq(T) != 0) {
    /*
    ** Keep the public trace slot reserved with the retired body until stale
    ** bytecode readers and in-flight exits have aged out. T->traceno and
    ** retire_epoch remain the runnable gate, so VM/recorder/assembler entry
    ** paths reject this body, but snapshot restore can still resolve the
    ** exiting trace after its live root/side links have been unlinked. Sticky MT
    ** mode keeps the same reservation between worker generations, but only until
    ** LJ_FLUSH_EPOCHS completed safepoint generations prove that every stale
    ** reader has quiesced. While retire_epoch is non-zero, nextroot is private
    ** slot-reservation metadata; normal root chains were unlinked before retiring
    ** the trace.
    */
    trace_nextroot_rel(T, traceno);
    trace_traceno_rel(T, 0);
    traceslot_publish(J, traceno, T);
  } else {
    trace_retired_slot_release(J, T);
  }
}

/* Complete the JIT-state half of a previously published retirement claim.
** GC2 may publish the claim while another TG records, but only the token
** owner may disconnect the public slot or update J->freetrace. */
static int trace_finish_slot_retire(jit_State *J, GCtrace *T)
{
  TraceNo traceno;
  int debug_done;
  lj_assertJ(lj_jit_token_held(J),
	     "trace destructor without recorder-token ownership");
  /* Optional GDB descriptor mutation is process-global. It is deliberately
  ** nonwaiting; the body stays on the retire list and a later grace pass retries
  ** before physical free if another universe currently owns that descriptor.
  */
  debug_done = lj_gdbjit_deltrace(J, T);
  traceno = trace_traceno_acq(T);
  if (traceno != 0) {
    trace_slot_retire(J, T, traceno);
  }
  return debug_done;
}

#if defined(EXITSTATE_PCREG) || (LJ_UNWIND_JIT && !EXITTRACE_VMSTATE) || \
    defined(LUA_USE_ASSERT)
static LJ_AINLINE int trace_exit_body_match(const GCtrace *T, TraceNo traceno)
{
  if (T == NULL || traceno == 0)
    return 0;
  if (trace_traceno_acq(T) == traceno)
    return 1;
  /*
  ** Retired traces stay published in their public slot until the SMR epoch
  ** expires. They are not runnable because traceno is cleared, but in-flight
  ** machine-code exits may still carry the public trace number and need the
  ** preserved snapshot body to restore interpreter state.
  */
  return trace_traceno_acq(T) == 0 &&
	 la_load64_acq(&T->retire_epoch) != 0 &&
	 trace_nextroot_acq(T) == traceno;
}
#endif

uint32_t lj_trace_reclaim_retired(global_State *g, uint64_t completed_epoch)
{
  jit_State *J;
  TraceVec *tv;
  GCtrace *rt;
  uint64_t pin_release_seq;
  uint32_t reclaimed = 0;
  int token = 0, retry_same_epoch = 0;
  if (!g || completed_epoch == 0)
    return 0;
  J = G2J(g);
  /*
  ** Slot release updates J->freetrace and must serialize with the sole recorder.
  ** Reclamation is opportunistic: never wait for a peer that owns the token,
  ** because that recorder may itself be waiting for the outer SMR reclaim pass to
  ** finish. The caller will retry at a later completed epoch.
  */
  if (!lj_jit_token_held(J)) {
    if (!lj_jit_token_try(J))
      return 0;
    token = 1;
  }
  if (!lj_gc2_jit_reclaim_context_acq(g) || !lj_jit_token_held(J)) {
    if (token)
      lj_jit_token_release(J);
    return 0;
  }
  pin_release_seq = la_load64_acq(&J->trace_pin_release_seq);
  /* The previous scan records this epoch only when every requeue is stable
  ** until either the epoch advances or a final native unpin changes the paired
  ** sequence. Sweep otherwise calls this routine once per 64-cell quarantine
  ** slice and repeatedly detaches the same list, turning bounded arena progress
  ** into quadratic work. A trace retired after that scan cannot be ready in
  ** this same epoch and its publisher preserves both sides of list publication. */
  if (J->trace_reclaim_epoch == completed_epoch &&
      J->trace_reclaim_pin_seq == pin_release_seq) {
    if (token)
      lj_jit_token_release(J);
    return 0;
  }
  lj_assertG(lj_gc2_jit_reclaim_context_acq(g) && lj_jit_token_held(J),
	     "trace retire-list detach without exclusive reclaim gate");
  tv = tracevec_retired_head_xchg_acqrel(J, NULL);
  while (tv) {
    if (LJ_UNLIKELY(
	!lj_gc2_mem_registered_known_reclaim_held(g, tv))) {
      /* xchg transferred the complete list to this exclusive owner. Losing an
      ** unvalidated head would also lose every successor, while reading next
      ** without a certificate would be a UAF. Fail before either action. */
      lj_assertG(0, "invalid detached retired TraceVec identity");
      abort();
    }
    TraceVec *next = tracevec_retired_next_acq(tv);
    tracevec_retired_next_rel(tv, NULL);
    if (la_load64_acq(&tv->retire_epoch) < completed_epoch) {
      tracevec_free(g, tv);
      reclaimed++;
    } else {
      tracevec_retired_push(J, tv);
    }
    tv = next;
  }
  rt = trace_retired_head_xchg_acqrel(J, NULL);
  while (rt) {
    if (LJ_UNLIKELY(
	!lj_gc2_mem_registered_known_reclaim_held(g, rt))) {
      lj_assertG(0, "invalid detached retired trace identity");
      abort();
    }
    GCtrace *next = trace_retired_next_acq(rt);
    trace_retired_link_unlinked_rel(rt);
    if (trace_body_retire_ready(rt, completed_epoch)) {
      if (trace_native_pins_acq(rt) != 0) {
	/* A final unpin publishes trace_pin_release_seq. Memoize this stable
	** blocked scan instead of detaching the same mature list on every bounded
	** reclaim quantum during a long foreign call. */
	trace_retired_push_preserved_reclaim(J, rt);
	rt = next;
	continue;
      }
      if (trace_retired_unpublished_acq(rt)) {
	/* Scratch bodies never had a trace slot, prototype/root-spine edge,
	** debugger registration or executable mcode publication. The completed
	** epoch plus this exclusive owner is sufficient for exact destruction;
	** running public-trace unlink logic would manufacture semantic meaning
	** from deliberately NULL compact fields. */
	if (!trace_freebody_(g, rt, 1, 0)) {
	  retry_same_epoch = 1;
	  trace_retired_push_preserved_reclaim(J, rt);
	  rt = next;
	  continue;
	}
	reclaimed++;
	rt = next;
	continue;
      }
      if (trace_has_runnable_inbound_link(J, rt)) {
	retry_same_epoch = 1;
	trace_retired_push_preserved_reclaim(J, rt);
	rt = next;
	continue;
      }
      /*
      ** The completed generation and the outer zero-reader SMR gate jointly prove
      ** that no stale bytecode entry or native exit can still require this public
      ** trace number. Clear the exact body identity before making the number free;
      ** a replacement trace may publish the same number immediately afterward.
      */
      if (!trace_finish_slot_retire(J, rt)) {
	retry_same_epoch = 1;
	trace_retired_push_preserved_reclaim(J, rt);
	rt = next;
	continue;
      }
      if (trace_traceno_acq(rt) != 0 || trace_nextroot_acq(rt) != 0)
	trace_retired_slot_release(J, rt);
      /* The sweep-owner reclaim gate runs only after its bounded bridge/root
      ** prune detached every old object. Avoid a redundant unbounded spine walk
      ** while the world is otherwise ready for bounded quarantine progress.
      ** IDLE reclaim still needs the compatibility unlink for traces retired by
      ** an ordinary jit.flush() before another sweep has detached their root.
      */
      if (gc2_phase_acq(g) != LJ_GC2_SWEEP &&
	  lj_gc_unlink_root_obj(g, obj2gco(rt)) ==
	    LJ_GC_ROOT_UNLINK_UNPROVEN) {
	/* An invalid ownership entry was severed without reading its foreign
	** successor, or the bounded scan could not prove target absence. Keep the
	** exact retired body list-owned: freeing it while a hidden root edge may
	** still name it would turn fail-closed spine damage into an arena UAF. */
	retry_same_epoch = 1;
	trace_retired_push_preserved_reclaim(J, rt);
	rt = next;
	continue;
      }
      if (!trace_freebody_(g, rt, 1, 0)) {
	/* A malformed/stale compact header is not an allocator contract. Keep the
	** exact body and its mcode references discoverable instead of turning a
	** validation failure into an unbounded-size free or executable UAF.
	*/
	retry_same_epoch = 1;
	trace_retired_push_preserved_reclaim(J, rt);
	rt = next;
	continue;
      }
      reclaimed++;
    } else {
      trace_retired_push_preserved_reclaim(J, rt);
    }
    rt = next;
  }
  /* Epoch-young and native-pin-blocked bodies are stable until the paired epoch
  ** or release sequence changes. Ready bodies with transient inbound, debugger,
  ** or validation failures remain eligible for a same-epoch retry: another body
  ** later in this batch may remove their last inbound link, and quarantine
  ** progress can depend on that retry. */
  if (!retry_same_epoch) {
    J->trace_reclaim_pin_seq = pin_release_seq;
    J->trace_reclaim_epoch = completed_epoch;
  }
  if (token)
    lj_jit_token_release(J);
  return reclaimed;
}

static int trace_ptr_in_mcode_span(const void *ptr, uintptr_t lo, uintptr_t hi)
{
  uintptr_t p = (uintptr_t)ptr;
  return p >= lo && p < hi;
}

static int trace_ptr_in_mcode_area(const void *ptr, uintptr_t rxlo,
				   uintptr_t rxhi, uintptr_t rwlo,
				   uintptr_t rwhi)
{
  return trace_ptr_in_mcode_span(ptr, rxlo, rxhi) ||
	 trace_ptr_in_mcode_span(ptr, rwlo, rwhi);
}

static int trace_mcode_area_refs(global_State *g, GCtrace *T, uintptr_t rxlo,
				 uintptr_t rxhi, uintptr_t rwlo,
				 uintptr_t rwhi)
{
  MCode *mcode;
  MCode *exitstub;
  MCode **exittab;
  SnapNo i, nsnap;
  if (!T)
    return 0;
  /* Partial assembler output is never committed, linked or registered with a
  ** debugger. Its active mcode area has independent list ownership, so the
  ** raw scratch retire node contributes no executable-area reference. */
  if (trace_retired_unpublished_acq(T))
    return 0;
  if (LJ_UNLIKELY(!trace_body_refs_valid(g, T, &nsnap)))
    return 1;  /* Keep mcode if a stale retired body cannot be decoded safely. */
  mcode = trace_mcode_acq(T);
  exitstub = trace_exitstub_acq(T);
  exittab = trace_exittab_acq(T);
  if (trace_ptr_in_mcode_area(mcode, rxlo, rxhi, rwlo, rwhi) ||
      trace_ptr_in_mcode_area(exitstub, rxlo, rxhi, rwlo, rwhi))
    return 1;
  if (!exittab)
    return 0;
  if (trace_exittab_ismcode(T))
    return trace_ptr_in_mcode_area(exittab, rxlo, rxhi, rwlo, rwhi);
  for (i = 0; i < nsnap; i++)
    if (trace_ptr_in_mcode_area(la_loadptr_acq((void *const *)&exittab[i]),
				rxlo, rxhi, rwlo, rwhi))
      return 1;
  return 0;
}

int lj_trace_retired_mcode_refs(global_State *g, MCode *area, size_t size)
{
  jit_State *J;
  GCtrace *T;
  MCode *rwarea;
  uintptr_t rxlo, rxhi, rwlo, rwhi;
  int pinned_ref = 0;
  if (!g || !area || size == 0)
    return 0;
  J = G2J(g);
  rwarea = lj_mcode_area_rw(area);
  rxlo = (uintptr_t)area;
  rxhi = rxlo + size;
  rwlo = (uintptr_t)rwarea;
  rwhi = rwlo + size;
  {
    TraceNo i, sizetrace = trace_sizetrace_acq(J);
    for (i = 1; i < sizetrace; i++) {
      GCtrace *slot = traceref_safe(J, i);
      if (trace_mcode_area_refs(g, slot, rxlo, rxhi, rwlo, rwhi)) {
	if (trace_native_pins_acq(slot) == 0)
	  return LJ_TRACE_MCODE_REF_ACTIVE;
	pinned_ref = 1;
      }
    }
  }
  for (T = trace_retired_head_acq(J); T != NULL;) {
    GCtrace *next;
    if (LJ_UNLIKELY(
	!lj_gc2_mem_registered_known_reclaim_held(g, T))) {
      /* The recorder token and exact-thread SMR writer pin this live retire
      ** list. Treat an invalid head as structural corruption; stopping early
      ** would falsely prove the candidate mcode area unreferenced. */
      lj_assertG(0, "invalid retired trace during mcode reference scan");
      abort();
    }
    next = trace_retired_next_acq(T);
    if (trace_mcode_area_refs(g, T, rxlo, rxhi, rwlo, rwhi)) {
      if (trace_native_pins_acq(T) == 0)
	return LJ_TRACE_MCODE_REF_ACTIVE;
      pinned_ref = 1;
    }
    T = next;
  }
  return pinned_ref ? LJ_TRACE_MCODE_REF_PINNED_ONLY :
	 LJ_TRACE_MCODE_REF_NONE;
}

void lj_trace_freeretired(global_State *g)
{
  jit_State *J = G2J(g);
  TraceVec *tv = tracevec_retired_head_xchg_acqrel(J, NULL);
  GCtrace *rt;
  while (tv) {
    if (LJ_UNLIKELY(!lj_gc2_mem_registered(g, tv))) {
      lj_assertG(0, "invalid terminal detached retired TraceVec");
      abort();
    }
    TraceVec *next = tracevec_retired_next_acq(tv);
    tracevec_free(g, tv);
    tv = next;
  }
  rt = trace_retired_head_xchg_acqrel(J, NULL);
  while (rt) {
    if (LJ_UNLIKELY(!lj_gc2_mem_registered(g, rt))) {
      lj_assertG(0, "invalid terminal detached retired trace");
      abort();
    }
    GCtrace *next = trace_retired_next_acq(rt);
    int freed;
    /* lj_trace_freestate() has already verified and freed the active trace
    ** vector. Do not route close-time body draining through the runtime slot
    ** release helper: there is no slot left to clear and no token-free write to
    ** J->freetrace is permitted. */
    if (!trace_retired_unpublished_acq(rt))
      lj_gdbjit_deltrace_close(g, rt);
    freed = trace_freebody_(g, rt, 0, 1);
    lj_assertG(freed, "invalid retired trace body at VM close");
    UNUSED(freed);
    rt = next;
  }
}

typedef struct TraceRootCycleGuard {
  const void *anchor;
  uint64_t power;
  uint64_t length;
} TraceRootCycleGuard;

static LJ_AINLINE void trace_root_cycle_init(TraceRootCycleGuard *guard,
					     const void *head)
{
  guard->anchor = head;
  guard->power = 1;
  guard->length = 0;
}

static LJ_AINLINE int trace_root_cycle_step(TraceRootCycleGuard *guard,
					    const void *next)
{
  if (next && next == guard->anchor)
    return 0;
  guard->length++;
  if (guard->length == guard->power) {
    guard->anchor = next;
    guard->length = 0;
    if (guard->power <= ~(uint64_t)0 / 2u)
      guard->power *= 2u;
  }
  return 1;
}

int lj_trace_markvecs(global_State *g, int gc2)
{
  jit_State *J = G2J(g);
  TraceVec *tv = tracevec_acq(J);
  GCtrace *rt, *nextrt;
  TraceVec *nexttv;
  TraceRootCycleGuard guard;
  UNUSED(gc2);  /* Compatibility argument: GC2 is the sole runtime collector. */
  if (tv) {
    (void)lj_gc2_markmem(g, tv);
    /*
    ** The trace vector is raw VM metadata and must survive concurrent readers,
    ** but its slots are not semantic roots. Stock LuaJIT lets a dead prototype
    ** and its root traces die together: live prototypes, active TG vmstates and
    ** live trace links mark the needed trace bodies. Marking every published
    ** slot here creates a trace->prototype cycle and keeps throwaway compiled
    ** chunks alive indefinitely.
    */
    {
      TraceNo i, sizetrace = trace_sizetrace_acq(J);
      /*
      ** A nonzero encoded retirement claim gates new entry before semantic links
      ** are removed. Preserve any such slot body; ordinary unretired live traces
      ** still derive reachability from prototypes, links and TG vmstates.
      */
      for (i = 1; i < sizetrace; i++) {
	GCtrace *T = traceref_safe(J, i);
	if (T && la_load64_acq(&T->retire_epoch) != 0)
	  trace_preserve_retired_body(g, T);
      }
    }
  }
  tv = tracevec_retired_head_acq(J);
  trace_root_cycle_init(&guard, tv);
  while (tv != NULL && lj_gc2_mem_registered(g, tv)) {
    nexttv = tracevec_retired_next_acq(tv);
    (void)lj_gc2_markmem_registered(g, tv);
    tv = nexttv;
    if (LJ_UNLIKELY(!trace_root_cycle_step(&guard, tv)))
      return 0;
  }
  if (LJ_UNLIKELY(tv != NULL))
    return 0;
  rt = trace_retired_head_acq(J);
  trace_root_cycle_init(&guard, rt);
  while (rt != NULL && lj_gc2_mem_registered(g, rt)) {
    nextrt = trace_retired_next_acq(rt);
    trace_preserve_retired_body(g, rt);
    rt = nextrt;
    if (LJ_UNLIKELY(!trace_root_cycle_step(&guard, rt)))
      return 0;
  }
  return rt == NULL;
}

/* Find a free trace number. */
static TraceNo trace_findfree(jit_State *J)
{
  MSize osz, lim;
  TraceVec *oldtv, *newtv;
#ifdef LJ_TRACE_TEST_HELPERS
  (void)la_add32_acqrel(&trace_test_findfree_calls, 1);
#endif
  if (J->freetrace == 0)
    J->freetrace = 1;
  for (; J->freetrace < trace_sizetrace_acq(J); J->freetrace++)
    if (gcref_acq(*traceslot_ref_acq(J, J->freetrace)) == NULL) {
      trace_test_note_findfree_reuse(J->freetrace);
      return J->freetrace++;
    }
  /* Need to grow trace array. */
  lim = (MSize)jit_param_acq(J, JIT_P_maxtrace) + 1;
  if (lim < 2) lim = 2; else if (lim > 65535) lim = 65535;
  osz = trace_sizetrace_acq(J);
  if (osz >= lim)
    return 0;  /* Too many traces. */
  newtv = tracevec_new(J->L, lim);
  /*
  ** Trace-body reclaim clears public slots and frees retired bodies after the
  ** SMR grace period. Hold an SMR read section while copying and publishing the
  ** replacement vector so reclaim cannot clear/free a copied retired slot before
  ** the new vector becomes the current publication.
  */
  lj_gc2_smr_read_enter(J2G(J));
  oldtv = tracevec_acq(J);
  osz = trace_sizetrace_acq(J);
  if (oldtv)
    memcpy(newtv->slot, oldtv->slot, osz*sizeof(GCRef));
  tracevec_publish(J, newtv);
  tracevec_retire(J, oldtv);
  lj_gc2_smr_read_leave(J2G(J));
  trace_test_note_findfree_grow(J->freetrace);
  return J->freetrace;
}

#define TRACE_APPENDVEC(field, szfield, tp) \
  T->field = (tp *)p; \
  memcpy(p, J->cur.field, J->cur.szfield*sizeof(tp)); \
  p += J->cur.szfield*sizeof(tp);

#ifdef LUAJIT_USE_PERFTOOLS
/*
** Create symbol table of JIT-compiled code. For use with Linux perf tools.
** Example usage:
**   perf record -f -e cycles luajit test.lua
**   perf report -s symbol
**   rm perf.data /tmp/perf-*.map
*/
#include <stdio.h>
#include <unistd.h>

static FILE *perftools_native_fopen(lua_State *L, const char *fname)
{
  FILE *fp;
  lj_native_enter(L2TG(L));
  fp = fopen(fname, "w");
  (void)lj_native_leave(L);
  return fp;
}

static void perftools_native_fprintf(lua_State *L, FILE *fp, uintptr_t mcode,
				     MSize szmcode, TraceNo traceno,
				     const char *name, BCLine lineno)
{
  lj_native_enter(L2TG(L));
  fprintf(fp, "%lx %x TRACE_%d::%s:%u\n",
	  (long)mcode, szmcode, traceno, name, lineno);
  (void)lj_native_leave(L);
}

static void perftools_addtrace(jit_State *J, GCtrace *T)
{
  static FILE *fp;
  lua_State *L = J->L;
  GCproto *pt = trace_startpt_acq(T);
  const BCIns *startpc = trace_startpc_acq(T);
  MCode *mcode = trace_mcode_acq(T);
  MSize szmcode = trace_szmcode_acq(T);
  TraceNo traceno = trace_traceno_acq(T);
  const char *name = proto_chunknamestr_acq(pt);
  BCLine lineno;
  if (name[0] == '@' || name[0] == '=')
    name++;
  else
    name = "(string)";
  lj_assertX(startpc >= proto_bc(pt) && startpc < proto_bc(pt) + pt->sizebc,
	     "trace PC out of range");
  lineno = lj_debug_line(pt, proto_bcpos(pt, startpc));
  if (!fp) {
    char fname[40];
    sprintf(fname, "/tmp/perf-%d.map", getpid());
    if (!(fp = perftools_native_fopen(L, fname))) return;
    setlinebuf(fp);
  }
  perftools_native_fprintf(L, fp, (uintptr_t)mcode, szmcode, traceno,
			   name, lineno);
}
#endif

/* Allocate space for copy of T. */
GCtrace * LJ_FASTCALL lj_trace_alloc(lua_State *L, GCtrace *T)
{
  size_t sztr = ((sizeof(GCtrace)+7)&~7);
  size_t szins = (T->nins-T->nk)*sizeof(IRIns);
  size_t sz = sztr + szins +
	      T->nsnap*sizeof(SnapShot) +
	      T->nsnapmap*sizeof(SnapEntry);
  GCtrace *T2 = (GCtrace *)lj_mem_newgco_raw(
    L, (MSize)sz, LJ_AF_TRAVERSABLE|LJ_AF_ROOT_CONSTRUCT);
  char *p = (char *)T2 + sztr;
  setgcrefnull(T2->nextgc);
  T2->gct = ~LJ_TTRACE;
  lj_obj_setgcflags(obj2gco(T2), 0);
#if LJ_GC64
  T2->unused_gc64 = 0;
#endif
  setgcrefnull(T2->gclist);
  T2->traceno = 0;
  T2->ir = (IRIns *)p - T->nk;
  T2->nins = T->nins;
  T2->nk = T->nk;
  T2->nsnap = T->nsnap;
  T2->nsnapmap = T->nsnapmap;
  T2->snap = NULL;
  T2->snapmap = NULL;
  trace_startpt_clear(T2);
  setmref(T2->startpc, NULL);
  T2->startins = 0;
  T2->szmcode = 0;
  T2->mcode = NULL;
  T2->exittab = NULL;
  T2->exitstub = NULL;
#if LJ_ABI_PAUTH
  T2->mcauth = NULL;
#endif
  T2->mcloop = 0;
  T2->nchild = 0;
  T2->spadjust = 0;
  trace_link_rel(T2, 0);
  T2->root = 0;
  trace_nextroot_rel(T2, 0);
  trace_nextside_rel(T2, 0);
  T2->sinktags = 0;
  T2->topslot = 0;
  T2->linktype = 0;
  T2->unused1 = 0;
  T2->native_pins = 0;
  T2->retire_epoch = 0;
  trace_retired_link_unlinked_rel(T2);
#ifdef LUAJIT_USE_GDBJIT
  T2->gdbjit_entry = NULL;
#endif
  memcpy(p, T->ir + T->nk, szins);
  return T2;
}

static void trace_exittab_free(global_State *g, GCtrace *T, SnapNo nsnap)
{
  MCode **exittab = trace_exittab_acq(T);
  if (exittab) {
    trace_exittab_rel(T, NULL);
    if (!trace_exittab_ismcode(T))
      lj_mem_freevec(g, exittab, nsnap, MCode *);
  }
  trace_exittab_mcode_clear(T);
  trace_exitstub_rel(T, NULL);
}

static void trace_exittab_reset(jit_State *J, GCtrace *T)
{
#if LJ_64 && defined(EXITSTUBS_PER_GROUP)
  ExitNo i;
  if (trace_exittab_acq(T) == NULL)
    return;
  for (i = 0; i < trace_nsnap_acq(T); i++)
    trace_exittarget_rel(T, i, exitstub_addr(J, i));
#elif LJ_TARGET_ARM64
  ExitNo i;
  if (trace_exittab_acq(T) == NULL)
    return;
  for (i = 0; i < trace_nsnap_acq(T); i++)
    trace_exittarget_rel(T, i, exitstub_trace_addr(T, i));
#else
  UNUSED(J); UNUSED(T);
#endif
}

static void trace_exittab_resetroot(jit_State *J, TraceNo rootno)
{
  global_State *g = J2G(J);
  TraceNo i;
  MSize sizetrace = trace_sizetrace_acq(J);
  lj_gc2_smr_read_enter(g);
  for (i = 1; i < sizetrace; i++) {
    GCtrace *T = traceref_safe(J, i);
    if (T && (trace_traceno_acq(T) == rootno || trace_root_acq(T) == rootno))
      trace_exittab_reset(J, T);
  }
  lj_gc2_smr_read_leave(g);
}

/* Save current trace by copying and compacting it. */
static void trace_save(jit_State *J, GCtrace *T)
{
  size_t sztr = ((sizeof(GCtrace)+7)&~7);
  size_t szins = (J->cur.nins-J->cur.nk)*sizeof(IRIns);
  char *p = (char *)T + sztr;
  global_State *g = J2G(J);
  memcpy(T, &J->cur, sizeof(GCtrace));
  newwhite(g, T);
  T->gct = ~LJ_TTRACE;
  T->native_pins = 0;
  T->retire_epoch = 0;
  trace_retired_link_unlinked_rel(T);
  T->ir = (IRIns *)p - J->cur.nk;  /* The IR has already been copied above. */
#if LJ_ABI_PAUTH
  T->mcauth = lj_ptr_sign((ASMFunction)T->mcode, T);
#endif
  p += szins;
  TRACE_APPENDVEC(snap, nsnap, SnapShot)
  TRACE_APPENDVEC(snapmap, nsnapmap, SnapEntry)
  J->cur.traceno = 0;
  J->cur.exittab = NULL;
  J->cur.exitstub = NULL;
  J->curfinal = NULL;
  lj_gc_linkobj_new(g, obj2gco(T));  /* Publish root after body init. */
  traceslot_publish(J, T->traceno, T);
  lj_gc_pubtrace(g, T->traceno);
  lj_gdbjit_addtrace(J, T);
#ifdef LUAJIT_USE_PERFTOOLS
  perftools_addtrace(J, T);
#endif
}

int LJ_FASTCALL lj_trace_free_gc(global_State *g, GCtrace *T)
{
  jit_State *J = G2J(g);
  TraceNo traceno = trace_traceno_acq(T);
  int sfixed = (g->gc.currentwhite & LJ_GC_SFIXED) != 0;
  int needs_publish;
  int needs_slot;
  int debug_done = 1;
  int token = 0;
  lj_assertG(traceno != 0 || trace_startptgco_acq(T) != NULL ||
	     la_load64_acq(&T->retire_epoch) != 0,
	     "unpublished trace body retired");
  UNUSED(traceno);
  needs_publish = !sfixed && !trace_retired_link_listed_acq(T);
  needs_slot = trace_traceno_acq(T) != 0;
  /* First retirement must run outside the exclusive SMR writer so its full
  ** proto/KGC/snapshot-PC preservation can acquire ordinary body leases. A
  ** quarantine pass may finish an already-listed trace, but cannot manufacture
  ** that initial semantic publication while its own reader gate is closed. */
  if (needs_publish &&
      lj_gc2_mem_registered_known_reclaim_held(g, T)) {
    /* A small/huge body reaches the sweep reclaimer only after root pruning
    ** transferred its exact membership ticket. Trace pruning calls
    ** lj_trace_retire_gc_claim() before that transfer, and that function lists
    ** the body before returning success. A LIVE trace found from the retired
    ** root is already listed by definition. Thus first retirement here is an
    ** ownership invariant violation, not retryable work: returning would leave
    ** an off-spine body with no outside-writer publication point forever. */
    lj_assertG(0, "unlisted trace reached exclusive sweep reclaim");
    abort();
  }
  /* GC workers never wait for an active recorder. Claim/list publication and
  ** slot teardown are one token-serialized transaction; a loser requests an
  ** asynchronous recorder abort and leaves the root/quarantine body unchanged
  ** for the next bounded sweep pass.
  */
  if (needs_publish || needs_slot) {
    if (!lj_jit_token_held(J)) {
      if (sfixed) {
	token = lj_jit_token_acquire_wait(J);
      } else if (!lj_jit_token_try(J)) {
	lj_trace_state_abort(J);
	return 0;
      } else {
	token = 1;
      }
    }
    if (!sfixed) {
      /* Publish before semantic entry/root teardown. Keep both operations in
      ** this token transaction so no assembler can install a new direct edge
      ** after the retirement epoch starts. */
      if (needs_publish)
	trace_retire_at_epoch(g, T, lj_gc2_retire_epoch(g));
      else
	lj_assertG(la_load64_acq(&T->retire_epoch) != 0,
		   "listed trace without retirement epoch");
      trace_retire_disconnect(J, T);
    }
    if (needs_slot)
      debug_done = trace_finish_slot_retire(J, T);
  }

  if (sfixed) {
    if (!debug_done)
      lj_gdbjit_deltrace_close(g, T);
    if (la_load64_acq(&T->retire_epoch) != 0) {
      /* A listed body is owned by lj_trace_freeretired(). Keep the intrusive
      ** link valid until that single close-time drain reaches it.
      */
      if ((!trace_retired_link_listed_acq(T) ||
	   trace_traceno_acq(T) != 0 || trace_nextroot_acq(T) != 0) &&
	  !lj_jit_token_held(J))
	token = lj_jit_token_acquire_wait(J);
      if (!trace_retired_link_listed_acq(T))
	trace_retired_push_preserved(J, T);
      if (trace_traceno_acq(T) != 0 || trace_nextroot_acq(T) != 0)
	trace_retired_slot_release(J, T);
      if (token)
	lj_jit_token_release(J);
      return 1;
    }
    if ((trace_traceno_acq(T) != 0 || trace_nextroot_acq(T) != 0) &&
	!lj_jit_token_held(J))
      token = lj_jit_token_acquire_wait(J);
    if (trace_traceno_acq(T) != 0 || trace_nextroot_acq(T) != 0)
      trace_retired_slot_release(J, T);
    trace_free_immediate(g, T);
    if (token)
      lj_jit_token_release(J);
    return 1;
  }
  if (token)
    lj_jit_token_release(J);
  return 1;
}

void LJ_FASTCALL lj_trace_free(global_State *g, GCtrace *T)
{
  (void)lj_trace_free_gc(g, T);
}

/* Re-enable compiling a prototype by unpatching any modified bytecode. */
void lj_trace_reenableproto(GCproto *pt)
{
  if ((pt->flags & PROTO_ILOOP)) {
    BCIns *bc = proto_bc(pt);
    BCPos i, sizebc = pt->sizebc;
    pt->flags &= ~PROTO_ILOOP;
    if (bc_op(bc[0]) == BC_IFUNCF)
      bc_publish_op(&bc[0], BC_FUNCF);
    for (i = 1; i < sizebc; i++) {
      BCOp op = bc_op(bc[i]);
      if (op == BC_IFORL || op == BC_IITERL || op == BC_ILOOP)
	bc_publish_op(&bc[i], (int)op+(int)BC_LOOP-(int)BC_ILOOP);
    }
  }
}

/* Unpatch the bytecode modified by a root trace. */
static void trace_unpatch(jit_State *J, GCtrace *T)
{
  BCIns startins = trace_startins_acq(T);
  BCOp op = bc_op(startins);
  BCIns *pc = (BCIns *)trace_startpc_acq(T);
  TraceNo traceno = trace_traceno_acq(T);
  BCIns cur;
  if (op == BC_JMP)
    return;  /* No need to unpatch branches in parent traces (yet). */
  cur = (BCIns)la_load32_acq((uint32_t *)pc);
  switch (bc_op(cur)) {
  case BC_JFORL:
    if (bc_d(cur) != traceno)
      break;
    lj_assertJ(traceref_safe(J, bc_d(cur)) == T,
	       "JFORL references other trace");
    {
      BCIns *foripc = pc + bc_j(startins);
      lj_assertJ(bc_op(*foripc) == BC_JFORI || bc_op(*foripc) == BC_FORI,
		 "FORL does not point to JFORI");
      /* Restore the branch target before exposing the original FORL. */
      bc_publish_op(foripc, BC_FORI);
      (void)bc_publish_cas(pc, (uint32_t *)&cur, startins);
    }
    break;
  case BC_JITERL:
  case BC_JLOOP:
    if (bc_d(cur) != traceno)
      break;
    lj_assertJ(op == BC_ITERL || op == BC_ITERN || op == BC_LOOP ||
	       bc_isret(op), "bad original bytecode %d", op);
    (void)bc_publish_cas(pc, (uint32_t *)&cur, startins);
    break;
  case BC_JFUNCF:
    if (bc_d(cur) != traceno)
      break;
    lj_assertJ(op == BC_FUNCF, "bad original bytecode %d", op);
    (void)bc_publish_cas(pc, (uint32_t *)&cur, startins);
    break;
  default:  /* Already unpatched. */
    break;
  }
}

/* Flush a root trace. Returns 1 iff trace-exit publication changed. */
static uint32_t trace_flushroot(jit_State *J, GCtrace *T, int scoped)
{
  GCproto *pt = trace_startpt_acq(T);
  TraceNo traceno = trace_traceno_acq(T);
  TraceNo head;
  TraceNo nextroot = trace_nextroot_acq(T);
  uint32_t retargeted = 1;
  lj_assertJ(trace_root_acq(T) == 0, "not a root trace");
  lj_assertJ(pt != NULL, "trace has no prototype");
  if (LJ_UNLIKELY(pt == NULL))
    return 0;
  if (scoped) {
    uint32_t marked = trace_scope_mark_pending(T);
    /* A scoped root flush keeps the trace body and trace slot alive until the
    ** safepoint boundary, but no new interpreter dispatch should enter the
    ** pending body. The caller holds the JIT token until the boundary, so
    ** restoring the original bytecode here cannot race a replacement recorder;
    ** peers that already fetched JLOOP still take the pending-trace fallback.
    */
    if (marked)
      trace_unpatch(J, T);
    return marked;
  }
  head = proto_trace_acq(pt);
  trace_exittab_resetroot(J, traceno);
  /* Unlink root trace from chain anchored in prototype. */
  if (head == traceno) {  /* Trace is first in chain. Easy. */
    proto_trace_rel(pt, nextroot);
unpatch:
    /* Unpatch modified bytecode only if the trace has not been flushed. */
    trace_unpatch(J, T);
    return 1;
  } else if (head) {  /* Otherwise search in chain of root traces. */
    GCtrace *T2 = traceref_safe(J, head);
    if (T2) {
      TraceNo next;
      for (next = trace_nextroot_acq(T2); next;
	   next = T2 ? trace_nextroot_acq(T2) : 0) {
	if (next == traceno) {
	  trace_nextroot_rel(T2, nextroot);  /* Unlink from chain. */
	  goto unpatch;
	}
	T2 = traceref_safe(J, next);
      }
    }
  }
  /* Another scoped flush may have unlinked this root first. The guarded
  ** unpatch above is idempotent and only rewrites bytecode still naming T.
  */
  trace_unpatch(J, T);
  return retargeted;
}

static int trace_scope_flushing(jit_State *J, TraceNo traceno)
{
  global_State *g = J2G(J);
  int flushing = 0;
  if (traceno > 0 && traceno < trace_sizetrace_acq(J)) {
    GCtrace *T;
    lj_gc2_smr_read_enter(g);
    T = traceref_safe(J, traceno);
    flushing = T && trace_traceno_acq(T) == traceno &&
	       trace_scope_pending_acq(T);
    lj_gc2_smr_read_leave(g);
  }
  return flushing;
}

static uint32_t trace_flushside(jit_State *J, GCtrace *T, int scoped)
{
  IRIns base = ir_load_acq(&trace_ir_acq(T)[REF_BASE]);
  TraceNo parentno = (TraceNo)base.op1;
  GCtrace *parent = traceref_safe(J, parentno);
  ExitNo exitno = (ExitNo)base.op2;
  lj_assertJ(trace_root_acq(T) != 0, "not a side trace");
  if (scoped) {
    uint32_t marked = trace_scope_mark_pending(T);
    /* Once a scoped side trace is pending, no parent exit may enter it as a
    ** runnable body. The body and slot are still retired only after the
    ** safepoint boundary, but the inbound exit table edge must be disconnected
    ** at mark time to close the pending-but-enterable window.
    */
    if (marked && parent && trace_traceno_acq(parent) == parentno &&
	trace_exittab_acq(parent) && exitno < trace_nsnap_acq(parent))
      trace_exittarget_rel(parent, exitno,
#if LJ_TARGET_ARM64
			   exitstub_trace_addr(parent, exitno)
#else
			   exitstub_addr(J, exitno)
#endif
			   );
    return marked;
  }
  trace_exittab_reset(J, T);
  if (parent && trace_traceno_acq(parent) == parentno &&
      trace_exittab_acq(parent) && exitno < trace_nsnap_acq(parent))
    trace_exittarget_rel(parent, exitno,
#if LJ_TARGET_ARM64
			 exitstub_trace_addr(parent, exitno)
#else
			 exitstub_addr(J, exitno)
#endif
			 );
  return 1;
}

static void trace_unlink_side_chain(jit_State *J, GCtrace *T,
				    TraceNo traceno, TraceNo rootno)
{
  GCtrace *root = traceref_safe(J, rootno);
  if (root && trace_traceno_acq(root) == rootno) {
    TraceNo next = trace_nextside_acq(T);
    TraceNo head = trace_nextside_acq(root);
    if (head == traceno) {
      trace_nextside_rel(root, next);
      trace_nchild_dec_acqrel(root);
    } else if (head != 0) {
      GCtrace *prev = traceref_safe(J, head);
      while (prev) {
	TraceNo prevnext = trace_nextside_acq(prev);
	if (prevnext == traceno) {
	  trace_nextside_rel(prev, next);
	  trace_nchild_dec_acqrel(root);
	  break;
	}
	prev = prevnext ? traceref_safe(J, prevnext) : NULL;
      }
    }
  }
}

static void trace_retire_disconnect(jit_State *J, GCtrace *T)
{
  TraceNo traceno = trace_traceno_acq(T);
  TraceNo rootno = trace_root_acq(T);
  lj_assertJ(lj_jit_token_held(J),
	     "trace entry disconnection without recorder-token ownership");
  if (traceno == 0)
    return;
  /* retire_epoch is already non-zero here. Interpreter entry therefore fails
  ** closed while the token owner removes persistent bytecode and direct-mcode
  ** edges. The later epoch margin only has to cover readers which fetched an
  ** edge before this transaction; it is not asked to age out a permanent
  ** parent exit-table pointer.
  */
  lj_assertJ(la_load64_acq(&T->retire_epoch) != 0,
	     "trace edges disconnected before retirement publication");
  if (rootno == 0) {
    (void)trace_flushroot(J, T, 0);
  } else {
    (void)trace_flushside(J, T, 0);
    trace_unlink_side_chain(J, T, traceno, rootno);
  }
  trace_link_rel(T, 0);
  trace_nextroot_rel(T, 0);
  trace_nextside_rel(T, 0);
}

static int trace_scope_flush_dependency(jit_State *J, GCtrace *T)
{
  TraceNo link = trace_link_acq(T);
  TraceNo root = trace_root_acq(T);
  if (trace_scope_flushing(J, link))
    return 1;
  if (root != 0) {
    IRIns base = ir_load_acq(&trace_ir_acq(T)[REF_BASE]);
    TraceNo parent = (TraceNo)base.op1;
    if (trace_scope_flushing(J, root) ||
	trace_scope_flushing(J, parent))
      return 1;
  }
  return 0;
}

static uint32_t trace_flushscope_mark_deps(jit_State *J)
{
  global_State *g = J2G(J);
  uint32_t marked = 0, changed;
  do {
    TraceNo i;
    MSize sizetrace = trace_sizetrace_acq(J);
    changed = 0;
    lj_gc2_smr_read_enter(g);
    for (i = 1; i < sizetrace; i++) {
      GCtrace *T = traceref_safe(J, i);
      if (T && trace_traceno_acq(T) == i &&
			  !trace_scope_pending_acq(T) &&
			  trace_scope_flush_dependency(J, T)) {
	if (trace_root_acq(T) == 0) {
	  if (!trace_flushroot(J, T, 1))
	    continue;
	} else {
	  (void)trace_flushside(J, T, 1);
	}
	marked++;
	changed = 1;
      }
    }
    lj_gc2_smr_read_leave(g);
  } while (changed);
  return marked;
}

/* Flush a root or side trace. Returns non-zero iff scoped work was marked. */
uint32_t lj_trace_flush(jit_State *J, TraceNo traceno)
{
  global_State *g = J2G(J);
  uint32_t flushed = 0;
  if (traceno > 0 && traceno < trace_sizetrace_acq(J)) {
    GCtrace *T;
    lj_gc2_smr_read_enter(g);
    T = traceref_safe(J, traceno);
    if (T && trace_traceno_acq(T) == traceno) {
      if (trace_root_acq(T) == 0)
	flushed = trace_flushroot(J, T, 1);
      else
	flushed = trace_flushside(J, T, 1);
    }
    lj_gc2_smr_read_leave(g);
  }
  return flushed;
}

/* Unlink a trace without retiring its slot. Recorder aborts need this for the
** stock recursive-call path: trace_abort() must still see the return trace and
** self-link it as a blacklist entry.
*/
uint32_t lj_trace_flush_unlink(jit_State *J, TraceNo traceno)
{
  global_State *g = J2G(J);
  uint32_t unlinked = 0;
  if (traceno > 0 && traceno < trace_sizetrace_acq(J)) {
    GCtrace *T;
    lj_gc2_smr_read_enter(g);
    T = traceref_safe(J, traceno);
    if (T && trace_traceno_acq(T) == traceno) {
      trace_test_note_flush_unlink(T, traceno);
      if (trace_root_acq(T) == 0)
	unlinked = trace_flushroot(J, T, 0);
      else
	unlinked = trace_flushside(J, T, 0);
    }
    lj_gc2_smr_read_leave(g);
  }
  return unlinked;
}

static int trace_stale_startins_valid(GCproto *pt, const BCIns *pc,
				      BCIns startins)
{
  BCOp op = bc_op(startins);
  const BCIns *bc;
  uintptr_t index;
  if (!pt)
    return 1;
  bc = proto_bc(pt);
  if (!trace_pc_in_proto_range(pc, bc, pt->sizebc))
    return 0;
  index = ((uintptr_t)pc - (uintptr_t)bc) / sizeof(BCIns);
  if (op == BC_FORL || op == BC_ITERL) {
    int64_t target = (int64_t)index + 1 +
	((int32_t)bc_d(startins) - BCBIAS_J);
    if (target < 0 || (uint64_t)target >= (uint64_t)pt->sizebc)
      return 0;
  }
  return 1;
}

static BCIns trace_stale_startins_match(GCtrace *T, const BCIns *pc,
					GCproto *owner)
{
  if (T && trace_startpc_acq(T) == pc) {
    BCIns startins = trace_startins_acq(T);
    BCOp op = bc_op(startins);
    if (op == BC_FORL || op == BC_ITERL || op == BC_LOOP ||
	op == BC_FUNCF || op == BC_ITERN || bc_isret(op)) {
      if (trace_stale_startins_valid(owner, pc, startins))
	return startins;
    }
  }
  return 0;
}

static BCIns trace_stale_startins_match_valid(global_State *g, GCtrace *T,
					      const BCIns *pc, GCproto *owner)
{
  if (!T || LJ_UNLIKELY(!trace_body_refs_valid(g, T, NULL)))
    return 0;
  return trace_stale_startins_match(T, pc, owner);
}

static GCtrace *trace_stale_startins_root_candidate_valid(global_State *g,
						   GCobj *o, uint32_t gct)
{
  GCtrace *T;
  if (gct != (uint32_t)~LJ_TTRACE)
    return NULL;
  T = gco2trace(o);
  if (LJ_UNLIKELY(!trace_body_refs_valid(g, T, NULL)))
    return NULL;
  return T;
}

#ifdef LJ_TRACE_TEST_HELPERS
static GCtrace *trace_stale_startins_root_candidate(global_State *g, GCobj *o,
					     LJGC2Lease *lease)
{
  GCtrace *T;
  uint32_t gct;
  if (!trace_preserve_body_candidate(g, o, &gct, lease))
    return NULL;
  T = trace_stale_startins_root_candidate_valid(g, o, gct);
  if (!T)
    lj_gc2_lease_release(lease);
  return T;
}

int lj_trace_test_stale_startins_candidate(global_State *g, GCobj *o)
{
  LJGC2Lease lease;
  int valid = trace_stale_startins_root_candidate(g, o, &lease) != NULL;
  lj_gc2_lease_release(&lease);
  return valid;
}
#endif

static BCIns trace_stale_startins_root(global_State *g, const BCIns *pc,
				       GCproto *owner)
{
  LJGC2Lease leases[2];
  uint32_t gct[2];
  uint32_t leaseidx = 0;
  GCobj *o;
  uint32_t n = 0;
  memset(leases, 0, sizeof(leases));
  (void)lj_gc_flush_root_pending(g);
  o = lj_gc_root_acq(g);
  if (o && !trace_preserve_body_candidate(
	     g, o, &gct[leaseidx], &leases[leaseidx]))
    return 0;
  while (o != NULL) {
    GCobj *next;
    int next_valid = 1;
    if (LJ_UNLIKELY(gct[leaseidx] == (uint32_t)~LJ_TSTR)) {
      lj_gc2_lease_release(&leases[leaseidx]);
      break;
    }
    next = lj_obj_gcw_acq(o);
    /* Admit the successor before releasing current. Root-spine membership is
    ** an ownership relation, but a concurrent prune can remove/destroy next in
    ** the load-to-next-iteration gap and reuse the same address. */
    if (next && !trace_preserve_body_candidate(
		  g, next, &gct[leaseidx ^ 1u],
		  &leases[leaseidx ^ 1u]))
      next_valid = 0;
    GCtrace *T = trace_stale_startins_root_candidate_valid(
	  g, o, gct[leaseidx]);
    if (T) {
      BCIns startins = trace_stale_startins_match(T, pc, owner);
	if (startins != 0) {
	  lj_gc2_lease_release(&leases[leaseidx]);
	  lj_gc2_lease_release(&leases[leaseidx ^ 1u]);
	return startins;
	}
    }
    lj_gc2_lease_release(&leases[leaseidx]);
    if (!next_valid)
      break;
    if (++n >= 1000000u) {
      lj_gc2_lease_release(&leases[leaseidx ^ 1u]);
      break;
    }
    o = next;
    leaseidx ^= 1u;
  }
  return 0;
}

static BCIns trace_stale_startins_shadow(lua_State *L, const BCIns *pc,
					 GCproto **ownerp)
{
  cTValue *frame, *bot;
  if (ownerp)
    *ownerp = NULL;
  if (!L)
    return 0;
  if (curr_funcisL(L)) {
    GCproto *pt = curr_proto(L);
    BCIns ins = proto_jit_startins_acq(pt, pc);
    if (ins != 0) {
      if (ownerp) *ownerp = pt;
      return ins;
    }
  }
  /* Trace-exit RET/ITERN and a gate-denied callee header can observe a C or
  ** already-shifted current base. Their prototype is nevertheless rooted by
  ** the live Lua frame. Walk only this state-owned frame chain and select the
  ** unique prototype whose fixed sidecar contains pc; no global SMR metadata
  ** or peer-owned structure is touched. */
  bot = tvref(L->stack) + LJ_FR2;
  for (frame = L->base - 1; frame > bot; ) {
    cTValue *prev;
    GCfunc *fn;
    GCproto *pt;
    BCIns ins;
    fn = frame_func(frame);
    if (isluafunc(fn)) {
      pt = funcproto(fn);
      ins = proto_jit_startins_acq(pt, pc);
      if (ins != 0) {
	if (ownerp) *ownerp = pt;
	return ins;
      }
    }
    prev = frame_islua(frame) ? frame_prevl(frame) : frame_prevd(frame);
    if (LJ_UNLIKELY(prev >= frame))
      break;
    frame = prev;
  }
  return 0;
}

BCIns LJ_FASTCALL lj_trace_stale_startins(jit_State *J, const BCIns *pc,
					  TraceNo traceno, lua_State *L)
{
  global_State *g = J2G(J);
  BCIns startins = 0;
  GCproto *owner = NULL;
  if (trace_test_take_startins_retry())
    return LJ_TRACE_STARTINS_RETRY;
  startins = trace_stale_startins_shadow(L, pc, &owner);
  if (startins != 0)
    return startins;
  /* A zero result means there is no recoverable original opcode. Temporary
  ** exclusive-writer contention is different: yield once and ask the VM to
  ** redispatch this exact PC. The next bounded attempt observes either restored
  ** bytecode or an admissible trace/retired-body generation, without blocking
  ** a mutator inside the metadata lookup. */
  if (!lj_gc2_smr_read_try(g)) {
    (void)lj_thr_retry_yield(NULL);
    return LJ_TRACE_STARTINS_RETRY;
  }
  if (traceno > 0 && traceno < trace_sizetrace_acq(J))
    startins = trace_stale_startins_match_valid(g, traceref_safe(J, traceno),
						pc, owner);
  if (startins == 0) {
    TraceNo i, sizetrace = trace_sizetrace_acq(J);
    for (i = 1; i < sizetrace; i++) {
      startins = trace_stale_startins_match_valid(g, traceref_safe(J, i), pc,
						  owner);
      if (startins != 0)
	break;
    }
  }
  if (startins == 0) {
    GCtrace *T;
    for (T = trace_retired_head_acq(J);
	 T != NULL && lj_gc2_mem_registered(g, T);
	 T = trace_retired_next_acq(T)) {
      if (trace_retired_unpublished_acq(T))
	continue;
      startins = trace_stale_startins_match_valid(g, T, pc, owner);
      if (startins != 0)
	break;
    }
  }
  if (startins == 0)
    startins = trace_stale_startins_root(g, pc, owner);
  lj_gc2_smr_read_leave(g);
  return startins;
}

/* Recover an ITERN trace's original word and close its VM/C validation gate.
** Failed ISNEXT executes without the recorder token, so it cannot unlink or
** retire a root trace. TRACE_ENTRY_INVALIDATED is a one-word nonwaiting gate:
** VM entry checks it before loading mcode, and a later scoped/full flush or GC
** retirement performs the token-owned graph teardown after the usual grace.
**
** The bytecode publisher calls this before its exact JLOOP -> ITERC CAS. If
** that CAS loses, gating the superseded trace is conservative and safe; if it
** wins, no new VM-dispatched entry or recorder/assembler link can validate the
** optimized-next trace after ITERC becomes visible. Pre-existing direct native
** links remain safe because the body stays allocated until a later scoped flush
** closes dependencies and crosses the EXIT_TRACES boundary.
*/
BCIns LJ_FASTCALL lj_trace_invalidate_itern(jit_State *J, const BCIns *pc,
					     TraceNo traceno, lua_State *L)
{
  global_State *g = J2G(J);
  GCproto *owner = NULL;
  BCIns shadow, startins = 0;

  if (trace_test_take_startins_retry())
    return LJ_TRACE_STARTINS_RETRY;
  shadow = trace_stale_startins_shadow(L, pc, &owner);

  /* Body validation and the invalidation-bit CAS need one allocation lease. A
  ** temporary exclusive registry writer is progress by a peer, so redispatch
  ** and retry instead of waiting inside the VM. */
  if (!lj_gc2_smr_read_try(g)) {
    (void)lj_thr_retry_yield(NULL);
    return LJ_TRACE_STARTINS_RETRY;
  }
  if (traceno > 0 && traceno < trace_sizetrace_acq(J)) {
    GCtrace *T = traceref_safe(J, traceno);
    startins = trace_stale_startins_match_valid(g, T, pc, owner);
    if (startins != 0 && bc_op(startins) == BC_ITERN &&
	T && trace_traceno_acq(T) == traceno &&
	la_load64_acq(&T->retire_epoch) == 0)
      (void)trace_entry_mark_invalidated(T);
  }
  lj_gc2_smr_read_leave(g);

  if (startins != 0)
    return startins;
  /* A missing exact live body means the JLOOP generation is already stale.
  ** Its prototype sidecar is sufficient for static redispatch and no runnable
  ** trace remains to gate. Keep the general retired/root search as the rare
  ** fallback for shifted frame layouts where the sidecar owner was not found.
  */
  return shadow != 0 ? shadow :
    lj_trace_stale_startins(J, pc, traceno, L);
}

/* Flush all traces associated with a prototype. */
uint32_t lj_trace_flushproto(global_State *g, GCproto *pt)
{
  jit_State *J = G2J(g);
  TraceNo trace;
  uint32_t flushed = 0;
  lj_gc2_smr_read_enter(g);
  for (trace = proto_trace_acq(pt); trace != 0; ) {
    GCtrace *T = traceref_safe(J, trace);
    if (!T || trace_traceno_acq(T) != trace)
      break;
    trace = trace_nextroot_acq(T);
    if (!trace_flushroot(J, T, 1))
      break;
    flushed++;
  }
  lj_gc2_smr_read_leave(g);
  return flushed;
}

static void trace_scope_clear_slot(jit_State *J, TraceNo traceno, GCtrace *T,
				   uint64_t epoch)
{
  global_State *g = J2G(J);
  lj_assertJ(lj_jit_token_held(J),
	     "scoped trace retirement without recorder-token ownership");
  /* Gate new entry and publish GC2 preservation before unlinking the root
  ** graph or inbound side-trace edge below.
  */
  trace_retire_at_epoch(g, T, epoch);
  trace_retire_disconnect(J, T);
  (void)lj_gdbjit_deltrace(J, T);
  if (trace_root_acq(T) == 0)
    trace_unpatch(J, T);
  /* Keep the trace number reserved until the retired body is reclaimable. */
  trace_slot_retire(J, T, traceno);
}

uint32_t lj_trace_flushscope_retire_hs(global_State *g, uint64_t epoch)
{
  jit_State *J = G2J(g);
  TraceNo i;
  uint32_t retired = 0;
  MSize sizetrace = trace_sizetrace_acq(J);
  /* This is the post-boundary token-owner half of scoped retirement. Generic
  ** EXIT_TRACES leaders only quiesce native users and abort recording: they may
  ** be unrelated GC workers and must never mutate trace slots on behalf of the
  ** parked token owner. Pending traces remain entry-gated until this call. */
  lj_assertJ(lj_jit_token_held(J),
	     "scoped retirement helper called without recorder token");
  if (!lj_jit_token_held(J))
    return 0;
  lj_gc2_smr_read_enter(g);
  for (i = 1; i < sizetrace; i++) {
    GCtrace *T = traceref_safe(J, i);
    if (T && trace_root_acq(T) != 0 && trace_traceno_acq(T) == i) {
      TraceNo rootno = trace_root_acq(T);
      GCtrace *root = traceref_safe(J, rootno);
      if (trace_scope_pending_acq(T) ||
	  (root && trace_traceno_acq(root) == rootno &&
	   trace_scope_pending_acq(root))) {
	trace_scope_clear_slot(J, i, T, epoch);
	retired++;
      }
    }
  }
  for (i = 1; i < sizetrace; i++) {
    GCtrace *T = traceref_safe(J, i);
    if (T && trace_root_acq(T) == 0 && trace_traceno_acq(T) == i &&
	trace_scope_pending_acq(T)) {
      trace_scope_clear_slot(J, i, T, epoch);
      retired++;
    }
  }
  lj_gc2_smr_read_leave(g);
  if (retired)
    gc2_jit_scoped_slots_retired_add(g, retired);
  return retired;
}

/* Flush all traces. */
int lj_trace_hasany(global_State *g)
{
  jit_State *J = G2J(g);
  MSize i, sizetrace = trace_sizetrace_acq(J);
  if (lj_trace_state_load(J) != LJ_TRACE_IDLE)
    return 1;  /* Active recorder must still be aborted by the boundary. */
  lj_gc2_smr_read_enter(g);
  for (i = 1; i < sizetrace; i++) {
    GCtrace *T = traceref_safe(J, i);
    if (T && trace_traceno_acq(T) == i) {
      lj_gc2_smr_read_leave(g);
      return 1;
    }
  }
  lj_gc2_smr_read_leave(g);
  return 0;
}

typedef struct TraceFlushVMEVENTCtx {
  jit_State *J;
  ptrdiff_t oldtop;
  LJJitVMEVENTCallResult result;
  int handed_off;
  int called;
  int closed;
} TraceFlushVMEVENTCtx;

/* Prepare and deliver one standalone TRACE "flush" event. All allocation is
** protected by the surrounding cpcall. Before handoff, every refusal leaves
** the exact low token with the caller; after handoff, only nonthrowing exact
** callback/stream cleanup is legal. */
static TValue *trace_flush_callback_cp(lua_State *L, lua_CFunction dummy,
				       void *ud)
{
  TraceFlushVMEVENTCtx *ctx = (TraceFlushVMEVENTCtx *)ud;
  LJVMEVENTPrepareResult prepared;
  LJJitTraceStreamHandle stream;
  LJJitEventCallbackHandle callback;
  TValue *handler_slot, *arg;
  GCstr *reason;
  GCfunc *handler;
  ptrdiff_t argbase;
  uint32_t trace_slot;
  int prepare_status;
  UNUSED(dummy);

  prepare_status = lj_vmevent_prepare_try(
    L, LJ_VMEVENT_TRACE, &prepared);
  if (prepare_status != LJ_VMEVENT_PREPARE_READY) {
    /* ABSENT and RETRY are bounded dropped instrumentation events. The
    ** preparation contract already restores the entry top; repeat the exact
    ** restoration defensively before the token owner resumes. */
    L->top = restorestack(L, ctx->oldtop);
    return NULL;
  }

  argbase = prepared.argbase;
  if (argbase < ctx->oldtop || argbase != savestack(L, L->top) ||
      argbase - ctx->oldtop !=
	(ptrdiff_t)((1u+LJ_FR2) * sizeof(TValue)) ||
      !lj_jit_event_attachment_clock_slot(
	VMEVENT_HASH(LJ_VMEVENT_TRACE), &trace_slot) ||
      prepared.slot != trace_slot ||
      !lj_vmevent_attachment_identity_valid(
	prepared.attachment_state, prepared.attachment.generation))
    goto drop;

  /* The argument was interned and fixed at bootstrap, so this load cannot
  ** enter the concurrent string-table writer/wait protocol. The prepared
  ** function is rooted at oldtop and recovered after possible relocation. */
  reason = lj_vmevent_trace_flush_reason_acq(G(L));
  if (LJ_UNLIKELY(!reason || reason->gct != (uint8_t)~LJ_TSTR))
    goto drop;
  handler_slot = restorestack(L, argbase) - (1+LJ_FR2);
  if (handler_slot != restorestack(L, ctx->oldtop) || !tvisfunc(handler_slot))
    goto drop;
  handler = funcV(handler_slot);
  arg = restorestack(L, argbase);
  setstrV(L, arg, reason);
  lj_state_stack_pubtv(L, L, arg);
  L->top = arg + 1;

  memset(&stream, 0, sizeof(stream));
  memset(&callback, 0, sizeof(callback));
  if (!lj_jit_trace_flush_callback_admit_l(
	L, ctx->J, prepared.attachment_state,
	prepared.attachment.generation, handler, &stream, &callback))
    goto drop;

  /* Admission has already published the exact rooted session/callback/stream,
  ** cleared J->L and released {tid,0}. Publish this fact before any operation
  ** whose failure path is observed by the outer cpcall. */
  ctx->handed_off = 1;
  if (LJ_UNLIKELY(!lj_jit_vmevent_call_l(
	L, argbase, ctx->oldtop, &callback, &ctx->result)))
    abort();
  ctx->called = 1;
  if (LJ_UNLIKELY(!lj_jit_trace_flush_close_l(L, ctx->J, &stream)))
    abort();
  ctx->closed = 1;
  return NULL;

drop:
  L->top = restorestack(L, ctx->oldtop);
  return NULL;
}

/* Consume responsibility for one newly-acquired disposable low JIT token.
** The wrapper releases it exactly once on absence, retry, admission refusal or
** setup error. Successful admission consumes it itself; callback errors are
** protected and STOPREQ is checked only after owner, stream and session close. */
static void trace_flush_callback_newtoken(lua_State *L, jit_State *J)
{
  TraceFlushVMEVENTCtx ctx;
  int errcode = 0;
  memset(&ctx, 0, sizeof(ctx));
  ctx.J = J;
  ctx.oldtop = savestack(L, L->top);

  lj_assertJ(lj_jit_token_held_l(L, J) && jit_owner_l_acq(J) == L,
	     "FLUSH callback wrapper requires exact disposable token");
  if (vmevmask_load_acq(G(L)) & VMEVENT_MASK(LJ_VMEVENT_TRACE))
    errcode = lj_vm_cpcall(L, NULL, &ctx, trace_flush_callback_cp);

  if (!ctx.handed_off) {
    /* A successful bounded refusal owns no error object. On cpcall failure the
    ** protected frame has left that object on the Lua stack for rethrow. */
    if (!errcode)
      L->top = restorestack(L, ctx.oldtop);
    lj_jit_token_release_l(L, J);
  } else if (LJ_UNLIKELY(errcode || !ctx.called || !ctx.closed)) {
    /* No production operation between handoff and close may escape the
    ** protected callback. Continuing would strand linear callback authority. */
    abort();
  }
  if (LJ_UNLIKELY(errcode))
    lj_err_throw(L, errcode);
  if (ctx.called)
    lj_safepoint_checkstop_fresh(
      L, ctx.result.actions, ctx.result.had_stopreq);
}

static int trace_flushall_direct(lua_State *L, int allow_gc_hook,
				 int send_event, int smr_held)
{
  jit_State *J = L2J(L);
  global_State *g = J2G(J);
  ptrdiff_t i;
  int token;
  /* This raw path has no remote EXIT_TRACES handshake. Keep its conservative
  ** process-wide GC-hook veto; peer callers use trace_flushall_hs_impl(). */
  if (!allow_gc_hook && (hookmask_load(g) & HOOK_GC))
    return 1;
  token = lj_jit_token_acquire_wait(J);
  /* A newly acquired token belongs to this flush, so publish L for retirement
  ** helpers and eventual callback admission. A pre-owned token retains the
  ** enclosing recorder/control transaction's existing J->L identity. */
  if (token)
    jit_owner_l_rel(J, L);
  if (!smr_held)
    lj_gc2_smr_read_enter(g);
  for (i = (ptrdiff_t)trace_sizetrace_acq(J)-1; i > 0; i--) {
    GCtrace *T = traceref_safe(J, i);
    if (T && trace_traceno_acq(T) == (TraceNo)i) {
      /* The body must be on the preserved retire list before any prototype,
      ** side, bytecode, or trace-vector edge is removed.
      */
      trace_retire(J2G(J), T);
      trace_exittab_reset(J, T);
      if (trace_root_acq(T) == 0) {
	trace_flushroot(J, T, 0);
	trace_unpatch(J, T);
      }
      (void)lj_gdbjit_deltrace(J, T);
      /* Keep the trace number reserved until the retired body is reclaimable. */
      trace_link_rel(T, 0);
      trace_nextroot_rel(T, 0);
      trace_nextside_rel(T, 0);
      trace_slot_retire(J, T, (TraceNo)i);
      /*
      ** A peer may have fetched a patched JFORL/JITERL before this flush
      ** unpatched bytecode. Keep the body reachable through the retired list
      ** so the interpreter fallback can recover the original loop offset.
      */
    }
  }
  if (!smr_held)
    lj_gc2_smr_read_leave(g);
  J->cur.traceno = 0;
  J->freetrace = 0;
  J->gc_pressure_traces = 0;
  /* Clear penalty cache. */
  memset(J->penalty, 0, sizeof(J->penalty));
  /* Free the whole machine code and invalidate all exit stub groups. */
  lj_mcode_free(J);
  memset(J->exitstubgroup, 0, sizeof(J->exitstubgroup));
  if (token && send_event)
    trace_flush_callback_newtoken(L, J);  /* Consumes the disposable token. */
  else if (token)
    lj_jit_token_release(J);
  /* token==0 belongs to an enclosing recorder/control transaction. Never
  ** detach it merely to deliver nested FLUSH instrumentation. */
  return 0;
}

int lj_trace_flushall(lua_State *L)
{
  return trace_flushall_direct(L, 0, 1, 0);
}

int lj_trace_flushall_gc(lua_State *L)
{
  return trace_flushall_direct(L, 1, 0, 0);
}

/* Request a leader-owned full trace flush through the safepoint protocol.
** Internal policy transitions use the eventless form: invoking a user TRACE
** callback while their lifecycle state is transitional permits a same-thread
** reentrant operation to self-wait forever. Public flushes retain the event. */
static int trace_flushall_hs_impl(lua_State *L, int send_event)
{
  global_State *g = G(L);
  jit_State *J = L2J(L);
  int token;
  if ((hookmask_load(g) & HOOK_GC)) {
    if (lj_gc2_finalizer_owned_by_current(g))
      return 1;
    /* HOOK_GC is process-wide, but the stock no-JIT-action rule belongs only
    ** to the TG executing the finalizer. A peer may safely request the normal
    ** trace-exit handshake. Suppress its TRACE callback while the finalizer's
    ** global hook exclusion is active, just as a busy VM event is skipped.
    */
    send_event = 0;
  }
  if (gc2_n_threads_acq(g) <= 1 && mt_active_acq(g) == 0) {
    /* Before the first MT generation there is no remote trace user to quiesce.
    ** Use the direct path so the stock single-mutator jit.flush() keeps its TRACE
    ** "flush" vmevent. Once mt_active is sticky, even a one-TG gap must advance
    ** the safepoint epoch: otherwise every flush reserves another trace number at
    ** the same generation and the namespace can never reach its reuse grace.
    */
    return trace_flushall_direct(L, 0, send_event, 0);
  }
  token = lj_jit_token_acquire_wait(J);
  /* FLUSHJ's nested eventless direct pass sees this token as pre-owned. Give
  ** its retirement/mcode helpers the initiating state before the handshake;
  ** the direct path must preserve, not replace, that exact outer identity. */
  if (token)
    jit_owner_l_rel(J, L);
  (void)lj_gc2_handshake(g, LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ);
  /* The arbitrary safepoint leader uses the eventless GC flush path. A caller
  ** which acquired this disposable token now publishes L, roots the exact
  ** clocked handler in a detached session/stream, and atomically hands the low
  ** token to zero before protected delivery. Exact close is token-free and
  ** never restores J->L over a peer recorder.
  */
  if (token && send_event) {
    trace_flush_callback_newtoken(L, J);  /* Consumes the disposable token. */
  } else if (token) {
    lj_jit_token_release(J);
  }
  /* A pre-owned token is part of an enclosing recorder/control transaction.
  ** The handshake may retire its traces, but nested FLUSH instrumentation is a
  ** bounded drop and must not release or overwrite that outer ownership. */
  return 0;
}

int lj_trace_flushall_hs(lua_State *L)
{
  return trace_flushall_hs_impl(L, 1);
}

int lj_trace_flushall_hs_noevent(lua_State *L)
{
  return trace_flushall_hs_impl(L, 0);
}

/*
** A resize descriptor has already made the raw mt_active word nonzero before
** entering here, preventing new pre-MT direct-store traces. Retire all traces
** assembled before that publication without a wait, callback, STOPREQ check,
** or error path: the caller already owns an install transaction which must be
** explicitly unwound on contention.
*/
int lj_trace_flushall_try_noevent(lua_State *L)
{
  global_State *g = G(L);
  jit_State *J = L2J(L);
  int acquired = 0;
  int retry;

  if (mt_active_acq(g) != 0)
    return 0;  /* First MT activation already retired pre-MT traces. */
  if (mt_entering_acq(g) != 0)
    return 1;  /* Let that activation own the trace transition. */
  if (lj_trace_state_load(J) != LJ_TRACE_IDLE)
    return 1;  /* Never dismantle an in-progress pre-guard recording. */
  if (!lj_jit_token_held_l(L, J)) {
    if (!lj_jit_token_try_l(L, J))
      return 1;
    acquired = 1;
    jit_owner_l_rel(J, L);
  }

  /*
  ** Close races with a first entrant after token acquisition. A published
  ** latch proves its mandatory flush completed; a still-entering actor has not
  ** made that proof yet, so this descriptor retries without waiting.
  */
  if (mt_active_acq(g) != 0)
    retry = 0;
  else if (mt_entering_acq(g) != 0)
    retry = 1;
  else if (lj_trace_state_load(J) != LJ_TRACE_IDLE)
    retry = 1;
  else
    retry = trace_flushall_direct(L, 0, 0, 1);

  if (acquired)
    lj_jit_token_release_l(L, J);
  return retry;
}

void lj_trace_flushscope_hs(global_State *g, uint32_t work)
{
  if (work != 0) {
    jit_State *J = G2J(g);
    int token = lj_jit_token_acquire_wait(J);
    (void)trace_flushscope_mark_deps(G2J(g));
    (void)lj_gc2_handshake(g, LJ_GC2_HS_EXIT_TRACES);  /* 08 section 8.7 scoped boundary. */
    (void)lj_trace_flushscope_retire_hs(g, lj_gc2_retire_epoch(g));
    if (token)
      lj_jit_token_release(J);
  }
}

uint32_t lj_trace_flushscope(jit_State *J, TraceNo traceno)
{
  int token = lj_jit_token_acquire_wait(J);
  uint32_t work = lj_trace_flush(J, traceno);
  lj_trace_flushscope_hs(J2G(J), work);
  if (token)
    lj_jit_token_release(J);
  return work;
}

/* Initialize JIT compiler state. */
void lj_trace_initstate(global_State *g)
{
  jit_State *J = G2J(g);
  TValue *tv;

  J->trace_reclaim_epoch = 0;
  J->mcode_reclaim_epoch = 0;
  J->trace_pin_release_seq = 0;
  J->trace_reclaim_pin_seq = 0;
  J->mcode_reclaim_pin_seq = 0;

  /* Initialize aligned SIMD constants. */
  tv = LJ_KSIMD(J, LJ_KSIMD_ABS);
  tv[0].u64 = U64x(7fffffff,ffffffff);
  tv[1].u64 = U64x(7fffffff,ffffffff);
  tv = LJ_KSIMD(J, LJ_KSIMD_NEG);
  tv[0].u64 = U64x(80000000,00000000);
  tv[1].u64 = U64x(80000000,00000000);

  /* Initialize 32/64 bit constants. */
#if LJ_TARGET_X64 || LJ_TARGET_MIPS64
  J->k64[LJ_K64_M2P64].u64 = U64x(c3f00000,00000000);
#endif
#if LJ_TARGET_X86ORX64
  J->k64[LJ_K64_TOBIT].u64 = U64x(43380000,00000000);
  J->k64[LJ_K64_2P64].u64 = U64x(43f00000,00000000);
#endif
#if LJ_TARGET_MIPS64
  J->k64[LJ_K64_2P63].u64 = U64x(43e00000,00000000);
#endif
#if LJ_TARGET_MIPS
  J->k64[LJ_K64_2P31].u64 = U64x(41e00000,00000000);
#endif

#if LJ_TARGET_X86ORX64 || LJ_TARGET_MIPS64
  J->k32[LJ_K32_M2P64] = 0xdf800000;
#endif
#if LJ_TARGET_MIPS64
  J->k32[LJ_K32_2P63] = 0x5f000000;
#endif
#if LJ_TARGET_PPC
  J->k32[LJ_K32_2P52_2P31] = 0x59800004;
  J->k32[LJ_K32_2P52] = 0x59800000;
#endif
#if LJ_TARGET_PPC
  J->k32[LJ_K32_2P31] = 0x4f000000;
#endif

#if LJ_TARGET_PPC || LJ_TARGET_MIPS32
  J->k32[LJ_K32_VM_EXIT_HANDLER] = (uintptr_t)(void *)lj_vm_exit_handler;
  J->k32[LJ_K32_VM_EXIT_INTERP] = (uintptr_t)(void *)lj_vm_exit_interp;
#endif
#if LJ_TARGET_ARM64 || LJ_TARGET_MIPS64
  J->k64[LJ_K64_VM_EXIT_HANDLER].u64 = (uintptr_t)lj_ptr_sign((void *)lj_vm_exit_handler, 0);
  J->k64[LJ_K64_VM_EXIT_INTERP].u64 = (uintptr_t)lj_ptr_sign((void *)lj_vm_exit_interp, 0);
#endif
}

static void trace_terminal_pin_preflight(global_State *g)
{
  jit_State *J = G2J(g);
  TraceVec *tv = tracevec_acq(J);
  GCtrace *T;
  TraceNo i, sizetrace = trace_sizetrace_acq(J);
  if (J->curfinal && trace_native_pins_acq(J->curfinal) != 0) {
    lj_assertG(0, "unfinished recorder body pinned at VM close");
    abort();
  }
  if (tv) {
    for (i = 1; i < sizetrace; i++) {
      T = traceref_safe(J, i);
      if (T && trace_native_pins_acq(T) != 0) {
	lj_assertG(0, "trace slot retains native execution pin at VM close");
	abort();
      }
    }
  }
  for (T = trace_retired_head_acq(J); T != NULL;
       T = trace_retired_next_acq(T)) {
    if (LJ_UNLIKELY(!lj_gc2_mem_registered(g, T))) {
      lj_assertG(0, "invalid retired trace during terminal pin preflight");
      abort();
    }
    if (trace_retired_unpublished_acq(T) &&
	LJ_UNLIKELY(!trace_unpublished_scratch_valid(T))) {
      lj_assertG(0, "invalid unpublished trace during terminal preflight");
      abort();
    }
    if (trace_native_pins_acq(T) != 0) {
      lj_assertG(0, "retired trace retains native execution pin at VM close");
      abort();
    }
  }
}

/* Free everything associated with the JIT compiler state. */
void lj_trace_freestate(global_State *g)
{
  jit_State *J = G2J(g);
  if (LJ_UNLIKELY(jit_owner_word_acq(g) != jit_owner_pack(0, 0))) {
    lj_assertG(0, "JIT owner word remained reserved at VM close");
    abort();
  }
  if (LJ_UNLIKELY(!lj_jit_trace_stream_idle(g))) {
    lj_assertG(0, "JIT TRACE stream remained live at VM close");
    abort();
  }
  /* Main-TG teardown may bypass lj_tg_fini() for the embedded arena path.
  ** Close and free retained raw views while trace slots are still available,
  ** and fail before trace retirement could invalidate a leaked source pin. */
  if (g->main_tg && !lj_jit_event_sessions_fini(g, g->main_tg)) {
    lj_assertG(0, "JIT event session remained live at VM close");
    abort();
  }
  /* Fail before buffers or the active vector disappear. A live native frame is
  ** a universe-lifetime violation, not terminal garbage to repair by force. */
  trace_terminal_pin_preflight(g);
#ifdef LUA_USE_ASSERT
  {  /* Live slots are gone. Reserved retired slots may still name bodies until
      ** the active vector is freed below and the retire list drains. */
    ptrdiff_t i;
    ptrdiff_t sizetrace = (ptrdiff_t)trace_sizetrace_acq(J);
    for (i = 1; i < sizetrace; i++) {
      GCtrace *T = traceref_safe(J, (TraceNo)i);
      lj_assertG(i == (ptrdiff_t)J->cur.traceno || T == NULL ||
		 (trace_traceno_acq(T) == 0 &&
		  la_load64_acq(&T->retire_epoch) != 0 &&
		  trace_retired_link_listed_acq(T)),
		 "live trace remained at VM close");
    }
  }
#endif
  lj_mem_freevec(g, J->snapmapbuf, J->sizesnapmap, SnapEntry);
  lj_mem_freevec(g, J->snapbuf, J->sizesnap, SnapShot);
  lj_mem_freevec(g, J->irbuf + J->irbotlim, J->irtoplim - J->irbotlim, IRIns);
  {
    TraceVec *tv = tracevec_acq(J);
    if (tv) {
      trace_sizetrace_rel(J, 0);
      tracevec_rel(J, NULL);
      tracevec_free(g, tv);
    }
  }
  lj_trace_freeretired(g);
  lj_mcode_freeall(g);
}

/* -- Penalties and blacklisting ------------------------------------------ */

/* Blacklist a bytecode instruction. */
static void blacklist_pc(GCproto *pt, BCIns *pc)
{
  BCIns ins = (BCIns)la_load32_acq((const uint32_t *)pc);
  BCOp op = bc_op(ins);
  if (op == BC_ITERN || op == BC_ITERC) {
    BCIns *bc = proto_bc(pt);
    int target_generic = op == BC_ITERC;
    if (pc < bc || pc >= bc + pt->sizebc - 1)
      return;
    if (!target_generic) {
      BCIns desired = ins;
      setbc_op(&desired, BC_ITERC);
      target_generic = bc_publish_cas(pc, (uint32_t *)&ins, desired) ||
		       bc_op(ins) == BC_ITERC;
    }
    if (target_generic) {
      BCIns iterl = (BCIns)la_load32_acq((const uint32_t *)(pc + 1));
      BCOp iop = bc_op(iterl);
      BCPos pcpos = proto_bcpos(pt, pc);
      int32_t guardpos;
      if (iop == BC_JITERL)
	iterl = proto_jit_startins_acq(pt, pc + 1);
      iop = bc_op(iterl);
      /* ITERL branches to the first loop-body instruction. ISNEXT is the
      ** immediately preceding guard, so step back one word from that target. */
      guardpos = (int32_t)pcpos + 1 + (int32_t)bc_j(iterl);
      if ((iop == BC_ITERL || iop == BC_IITERL) &&
	  bc_a(iterl) == bc_a(ins) && guardpos >= 0 &&
	  (uint32_t)guardpos < pt->sizebc) {
	BCIns *guardpc = &bc[guardpos];
	BCIns guard =
	  (BCIns)la_load32_acq((const uint32_t *)guardpc);
	if (bc_op(guard) == BC_ISNEXT && bc_a(guard) == bc_a(ins) &&
	    (int64_t)guardpos + 1 + (int64_t)bc_j(guard) ==
	      (int64_t)pcpos)
	  (void)bc_publish_op_cas(guardpc, (uint32_t *)&guard, BC_JMP);
      }
    }
  } else if (op == BC_FORL || op == BC_ITERL || op == BC_LOOP ||
	     op == BC_FUNCF) {
    BCOp disabled = (BCOp)((int)op + (int)BC_ILOOP - (int)BC_LOOP);
    if (bc_publish_op_cas(pc, (uint32_t *)&ins, disabled))
      pt->flags |= PROTO_ILOOP;
  }
}

/* Penalize a bytecode instruction. */
static void penalty_pc(jit_State *J, GCproto *pt, BCIns *pc, TraceError e)
{
  lua_State *owner_L = jit_owner_l_acq(J);
  TGState *owner_tg = hotcount_ownertg(J2G(J), owner_L);
  uint32_t i, val = PENALTY_MIN;
  for (i = 0; i < PENALTY_SLOTS; i++)
    if (mref(J->penalty[i].pc, const BCIns) == pc) {  /* Cache slot found? */
      /* First try to bump its hotcount several times. */
      val = ((uint32_t)J->penalty[i].val << 1) +
	    (owner_tg ?
	     (lj_prng_u64(&owner_tg->prng) & ((1u<<PENALTY_RNDBITS)-1)) : 0);
      if (val > PENALTY_MAX) {
	blacklist_pc(pt, pc);  /* Blacklist it, if that didn't help. */
	return;
      }
      goto setpenalty;
    }
  /* Assign a new penalty cache slot. */
  i = J->penaltyslot;
  J->penaltyslot = (J->penaltyslot + 1) & (PENALTY_SLOTS-1);
  setmref(J->penalty[i].pc, pc);
setpenalty:
  J->penalty[i].val = (uint16_t)val;
  J->penalty[i].reason = e;
  (void)hotcount_setl(J2G(J), owner_L, pc+1, val);
}

static void trace_mark_active_startpt(jit_State *J)
{
  global_State *g;
  GCobj *o;
  if (!J || !J->L || !J->pt || !checkptrGC(J->pt) ||
      J->pt->gct != ~LJ_TPROTO)
    return;
  g = J2G(J);
  o = obj2gco(J->pt);
  /*
  ** The recorder starts with only J->pt; J->cur.startpt is published later after
  ** trace-number allocation. The start prototype is already a semantic root in
  ** that setup window, and remains one until recording publishes or aborts.
  */
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    (void)lj_gc2_trace_sweep_root(g, o);
  lj_gc2_barrier_marked_proto(J->L, J->pt);
}

/* -- Trace compiler state machine ---------------------------------------- */

/* Validate one complete root-start generation captured under the JIT token.
** Stitched CALL/ITERC roots carry their parent trace in exitno; an ordinary
** hot root or down-recursion root must use one of the unpatched start opcodes.
*/
static int trace_root_startins_valid(BCIns ins, ExitNo exitno)
{
  if (exitno != 0) {
    BCOp op = bc_op(ins);
    return op == BC_CALLM || op == BC_CALL || op == BC_ITERC;
  }
  switch (bc_op(ins)) {
  case BC_FORL:
  case BC_ITERL:
  case BC_ITERN:
  case BC_LOOP:
  case BC_FUNCF:
  case BC_RET:
  case BC_RET0:
  case BC_RET1:
    return 1;
  default:
    return 0;
  }
}

/* Capture the fixed ISNEXT ... ITERN ITERL tuple before callbacks. A peer may
** independently despecialize ITERN, so recheck the two words which define the
** root after validating the immutable branch geometry. */
static int trace_root_itern_tuple(GCproto *pt, const BCIns *pc,
				  BCIns startins, BCIns *iterlp)
{
  const BCIns *bc;
  BCIns iterl, guard;
  BCPos pcpos;
  int64_t bodypos, guardpos, targetpos;
  if (!pt || !pc || !iterlp || pt->sizebc < 2)
    return 0;
  bc = proto_bc(pt);
  if (pc < bc || pc >= bc + pt->sizebc - 1)
    return 0;
  pcpos = proto_bcpos(pt, pc);
  iterl = (BCIns)la_load32_acq((const uint32_t *)&pc[1]);
  if (bc_op(iterl) != BC_ITERL || bc_a(iterl) != bc_a(startins))
    return 0;
  bodypos = (int64_t)pcpos + 2 + (int64_t)bc_j(iterl);
  guardpos = bodypos - 1;
  if (guardpos < 0 || bodypos < 0 || bodypos > (int64_t)pcpos ||
      bodypos >= (int64_t)pt->sizebc)
    return 0;
  guard = (BCIns)la_load32_acq(
    (const uint32_t *)&bc[(BCPos)guardpos]);
  targetpos = guardpos + 1 + (int64_t)bc_j(guard);
  if (bc_op(guard) != BC_ISNEXT || bc_a(guard) != bc_a(startins) ||
      targetpos != (int64_t)pcpos)
    return 0;
  if ((BCIns)la_load32_acq((const uint32_t *)pc) != startins ||
      (BCIns)la_load32_acq((const uint32_t *)&pc[1]) != iterl)
    return 0;
  *iterlp = iterl;
  return 1;
}

typedef enum TraceStartResult {
  TRACE_START_RESULT_ACTIVE = 0,
  TRACE_START_RESULT_IDLE,
  TRACE_START_RESULT_FLUSH_ALL
} TraceStartResult;

/* Start tracing. Terminal work is returned to trace_state(), which first
** publishes IDLE and releases the recorder token before dispatch repair or a
** full flush. */
static TraceStartResult trace_start(jit_State *J)
{
  TraceNo traceno;
  uint32_t gc2phase = gc2_phase_acq(J2G(J));
  BCIns root_startins = 0;
  BCIns root_iterl = 0;

  lj_assertJ(lj_jit_token_held(J),
	     "trace start without recorder-token ownership");
  J->root_startins_pending = 0;

  /* The TRACE-start callback may execute this prototype and mutate shared
  ** bytecode before recorder setup. Capture one whole generation now and use
  ** it through setup; a later final patch CAS either publishes this trace or
  ** retires it. A transition which already won simply cancels this attempt. */
  if (J->parent == 0) {
    root_startins =
      (BCIns)la_load32_acq((const uint32_t *)J->pc);
    if (!trace_root_startins_valid(root_startins, J->exitno))
      return TRACE_START_RESULT_IDLE;
    if (bc_op(root_startins) == BC_ITERN &&
	!trace_root_itern_tuple(J->pt, J->pc, root_startins, &root_iterl))
      return TRACE_START_RESULT_IDLE;
  }

  /* Cooperative MARK admits recording only after activation installed black
  ** allocation and mutation barriers on every TG. A root-snapshot close
  ** asynchronously aborts any still-active recorder and reopens the mark round;
  ** token-private J->cur construction edges are never a persistent certificate.
  ** SWEEP still rejects recording: J->cur KGC/snapshot construction edges are
  ** intentionally NOBARRIER, so a post-bridge recorder would need its own
  ** sweep-rescue publication before physical reclaim could remain concurrent.
  ** Published native traces still execute during READY SWEEP; the short
  ** hotcount retry below lets compilation resume naturally in IDLE.
  */
  if (gc2phase != LJ_GC2_IDLE &&
      !(gc2phase == LJ_GC2_MARK && lj_gc2_jit_entry_open(J2G(J)))) {
    /* This is a transient collector gate, not a recorder penalty. Leaving the
    ** ordinary full hotloop reset in place can consume every hot event while a
    ** peer-owned MARK/WEAK cycle is active, so a finite loop never records even
    ** after GC2 returns to IDLE. Keep a root edge on a short retry backoff; the
    ** token acquisition remains a bounded try and trace_start() still refuses
    ** to publish anything until the phase is safe. */
    if (J->parent == 0 && J->pc != NULL)
      (void)hotcount_setl(J2G(J), jit_owner_l_acq(J), J->pc+1, 16);
    return TRACE_START_RESULT_IDLE;
  }
  trace_mark_active_startpt(J);
  if ((J->pt->flags & PROTO_NOJIT)) {  /* JIT disabled for this proto? */
    if (J->parent == 0 && J->exitno == 0) {
      /* Lazy bytecode patching to disable hotcount events. */
      BCOp op = bc_op(root_startins);
      if (op == BC_FORL || op == BC_ITERL || op == BC_LOOP ||
	  op == BC_FUNCF) {
	BCIns expected = root_startins;
	BCOp disabled =
	  (BCOp)((int)op + (int)BC_ILOOP - (int)BC_LOOP);
	if (bc_publish_op_cas(J->pc, (uint32_t *)&expected, disabled))
	  J->pt->flags |= PROTO_ILOOP;
      }
    }
    return TRACE_START_RESULT_IDLE;  /* Silently ignored. */
  }

  /* Get a new trace number. */
  traceno = trace_findfree(J);
  if (LJ_UNLIKELY(traceno == 0)) {  /* No free trace? */
    lj_assertJ((hookmask_load(J2G(J)) & HOOK_GC) == 0,
	       "recorder called from GC hook");
    return TRACE_START_RESULT_FLUSH_ALL;  /* Silently ignored. */
  }
  traceslot_pending(J, traceno);

  /* Setup enough of the current trace to be able to send the vmevent. */
  memset(&J->cur, 0, sizeof(GCtrace));
  J->cur.traceno = traceno;
  J->cur.nins = J->cur.nk = REF_BASE;
  J->cur.ir = J->irbuf;
  J->cur.snap = J->snapbuf;
  J->cur.snapmap = J->snapmapbuf;
  if (J->parent == 0)
    J->cur.startins = root_startins;
  J->mergesnap = 0;
  J->needsnap = 0;
  J->bcskip = 0;
  J->guardemit.irt = 0;
  J->postproc = LJ_POST_NONE;
  lj_resetsplit(J);
  J->retryrec = 0;
  J->ktrace = 0;
  trace_startpt_rel(&J->cur, J->pt);
  trace_mark_active_startpt(J);

  lj_vmevent_send_l_(J->L, TRACE,
    TValue savetv = J2TG(J)->tmptv;
    TValue savetv2 = J2TG(J)->tmptv2;
    TraceNo parent = J->parent;
    ExitNo exitno = J->exitno;
    setstrV(V, V->top++, lj_str_newlit(V, "start"));
    setintV(V->top++, traceno);
    setfuncV(V, V->top++, J->fn);
    setintV(V->top++, proto_bcpos(J->pt, J->pc));
    if (J->parent) {
      setintV(V->top++, J->parent);
      setintV(V->top++, J->exitno);
    } else {
      BCOp op = bc_op(J->cur.startins);
      if (op == BC_CALLM || op == BC_CALL || op == BC_ITERC) {
	setintV(V->top++, J->exitno);  /* Parent of stitched trace. */
	setintV(V->top++, -1);
      }
    }
  ,
    J2TG(J)->tmptv = savetv;
    J2TG(J)->tmptv2 = savetv2;
    J->parent = parent;
    J->exitno = exitno;
  );
  lj_record_setup(J, root_iterl);
  return TRACE_START_RESULT_ACTIVE;
}

/* Stop tracing. */
static int trace_stop(jit_State *J)
{
  global_State *g = J2G(J);
  BCIns *pc = mref(J->cur.startpc, BCIns);
  BCOp op = bc_op(J->cur.startins);
  GCproto *pt = trace_startpt_acq(&J->cur);
  TraceNo traceno = J->cur.traceno;
  TraceNo parentno = 0, rootno = 0;
  ExitNo exitno = J->exitno;
  GCtrace *T = J->curfinal;
  BCIns *patchpc = NULL;
  BCIns patchins = 0;
  GCtrace *parent = NULL;
  GCtrace *root = NULL;
  SnapShot *snap = NULL;
  MSize topslot;
  int addroot = 0;
  int root_patch_lost = 0;

  switch (op) {
  case BC_FORL:
    /* The matching FORI is patched after trace publication. */
    /* fallthrough */
  case BC_LOOP:
  case BC_ITERL:
  case BC_FUNCF:
    patchpc = pc;
    patchins = BCINS_AD((int)op+(int)BC_JLOOP-(int)BC_LOOP,
			bc_a(J->cur.startins), traceno);
  addroot:
    J->cur.nextroot = (TraceNo1)proto_trace_acq(pt);
    addroot = 1;
    break;
  case BC_ITERN:
  case BC_RET:
  case BC_RET0:
  case BC_RET1:
    patchpc = pc;
    patchins = BCINS_AD(BC_JLOOP, J->cur.snap[0].nslots, traceno);
    goto addroot;
  case BC_JMP:
    parentno = J->parent;
    rootno = J->cur.root;
    lj_assertJ(parentno != 0 && rootno != 0, "not a side trace");
    lj_gc2_smr_read_enter(g);
    parent = traceref_safe(J, parentno);
    root = traceref_safe(J, rootno);
    lj_assertJ(parent != NULL && root != NULL, "missing parent/root trace");
    /* Avoid compiling a side trace twice (stack resizing uses parent exit). */
    J->cur.nextside = (TraceNo1)trace_nextside_acq(root);
    lj_gc2_smr_read_leave(g);
    break;
  case BC_CALLM:
  case BC_CALL:
  case BC_ITERC:
    parentno = (TraceNo)J->exitno;
    lj_gc2_smr_read_enter(g);
    parent = traceref_safe(J, parentno);
    lj_assertJ(parent != NULL, "missing stitched trace");
    lj_gc2_smr_read_leave(g);
    break;
  default:
    lj_assertJ(0, "bad stop bytecode %d", op);
    break;
  }

  /* Commit and publish the final trace before enabling bytecode/exits. */
  lj_mcode_commit(J, J->cur.mcode);
  lj_mcode_sync_core(J);
  J->postproc = LJ_POST_NONE;
  trace_save(J, T);

  switch (op) {
  case BC_FORL:
    /* Leave FORI unpatched. JFORL carries the trace number by itself; avoiding
    ** the paired JFORI/JFORL publication race preserves numeric-for semantics
    ** when another TG is currently executing the first-iteration opcode.
    */
    /* fallthrough */
  case BC_LOOP:
  case BC_ITERL:
  case BC_FUNCF:
  case BC_ITERN:
  case BC_RET:
  case BC_RET0:
  case BC_RET1:
    if (addroot)
      proto_trace_rel(pt, traceno);
    if (patchpc) {
      /* Publish a prototype-owned immutable recovery copy before replacing the
      ** opcode. It survives trace-slot/mcode retirement, so a gate-denied VM
      ** can interpret RET/ITERN/FORL/ITERL without touching exclusive SMR state
      ** or waiting for the IDLE retire owner. */
      proto_jit_startins_rel(pt, patchpc, J->cur.startins);
      /* Root publication and VM/blacklist despecialization are competing
      ** terminal transitions from this exact original generation. Never
      ** overwrite ILOOP/IITERL/IFUNCF, or ISNEXT's terminal ITERC, with a
      ** trace compiled for the superseded instruction. The VM wrapper is also
      ** the deterministic collision point used by the regression fixture. */
      {
	BCIns observed = lj_bc_publish_cas_vm(patchpc, J->cur.startins,
					    patchins);
	root_patch_lost = observed != patchins;
      }
    }
    break;
  case BC_JMP:
    lj_gc2_smr_read_enter(g);
    parent = traceref_safe(J, parentno);
    root = traceref_safe(J, rootno);
    lj_assertJ(parent != NULL && root != NULL, "missing parent/root trace");
    lj_assertJ(trace_exittab_acq(parent) != NULL, "missing parent exit table");
    snap = &trace_snap_acq(parent)[exitno];
    topslot = trace_topslot_acq(T);
    if (topslot > snap_topslot_acq(snap)) snap_topslot_rel(snap, topslot);
    trace_nchild_inc_acqrel(root);
    trace_nextside_rel(root, traceno);
    snap_count_rel(snap, SNAPCOUNT_DONE);
    /*
    ** The parent exit target is the runnable side-trace gate. Publish it only
    ** after the parent/root metadata above can be observed by other threads.
    */
    trace_exittarget_rel(parent, exitno, trace_mcode_acq(T));
    lj_gc2_smr_read_leave(g);
    break;
  case BC_CALLM:
  case BC_CALL:
  case BC_ITERC:
    lj_gc2_smr_read_enter(g);
    parent = traceref_safe(J, parentno);
    lj_assertJ(parent != NULL, "missing stitched trace");
    trace_link_rel(parent, traceno);
    lj_gc2_smr_read_leave(g);
    break;
  default:
    break;
  }

  if (LJ_UNLIKELY(root_patch_lost)) {
    /* trace_save() already published the body and prototype root. Preserve it
    ** for abort-event inspection, but gate native entry before invoking user
    ** code. Ordinary abort consumers (notably jit.dump's aborted-IR mode) expect
    ** jit.util.trace* to resolve the trace during the callback. The callback may
    ** reentrantly flush it, so resolve the exact slot again before cleanup. */
    (void)trace_entry_mark_invalidated(T);
    lj_vmevent_send_l(J->L, TRACE,
      setstrV(V, V->top++, lj_str_newlit(V, "abort"));
      setintV(V->top++, traceno);
      setfuncV(V, V->top++, J->fn);
      setintV(V->top++, proto_bcpos(pt, pc));
      setintV(V->top++, LJ_TRERR_RETRY);
      setnilV(V->top++);
    );
    lj_gc2_smr_read_enter(g);
    {
      GCtrace *live = traceref_safe(J, traceno);
      if (live == T && trace_traceno_acq(live) == traceno)
	trace_scope_clear_slot(J, traceno, live, lj_gc2_retire_epoch(g));
    }
    lj_gc2_smr_read_leave(g);
    return 0;
  }

  lj_vmevent_send_l(J->L, TRACE,
    setstrV(V, V->top++, lj_str_newlit(V, "stop"));
    setintV(V->top++, traceno);
    setfuncV(V, V->top++, J->fn);
  );
  /* Count only completely published traces. The caller releases the recorder
  ** token before converting this hint into an ordinary GC2 cycle request. */
  if (++J->gc_pressure_traces >= TRACE_GC_PRESSURE_BATCH) {
    J->gc_pressure_traces = 0;
    /* Publication churn which continually reuses a small namespace is not
    ** heap pressure. Request only when the token-owned free cursor proves the
    ** live/reserved high-water has crossed this batch. */
    return J->freetrace > TRACE_GC_PRESSURE_BATCH;
  }
  return 0;
}

/* Start a new root trace for down-recursion. */
static TraceStartResult trace_downrec(jit_State *J)
{
  BCIns ins;
  /* Restart recording at the return instruction. */
  lj_assertJ(J->pt != NULL, "no active prototype");
  ins = (BCIns)la_load32_acq((const uint32_t *)J->pc);
  lj_assertJ(bc_isret(bc_op(ins)), "not at a return bytecode");
  if (bc_op(ins) == BC_RETM)
    return TRACE_START_RESULT_IDLE;  /* NYI: down-recursion with RETM. */
  J->parent = 0;
  J->exitno = 0;
  if (lj_trace_state_aborted(lj_trace_state_store_active(J, LJ_TRACE_RECORD)))
    return TRACE_START_RESULT_IDLE;
  return trace_start(J);
}

typedef enum TraceAbortResult {
  TRACE_ABORT_DONE = 0,
  TRACE_ABORT_RETRY,
  TRACE_ABORT_FLUSH_ALL
} TraceAbortResult;

/* Abort tracing. Return any work which must run only after terminal recorder
** state has been published and the JIT token has been released. */
static TraceAbortResult trace_abort(jit_State *J)
{
  lua_State *L = J->L;
  TraceError e = LJ_TRERR_RECERR;
  TraceNo traceno;

  J->postproc = LJ_POST_NONE;
  J->root_startins_pending = 0;
  lj_mcode_abort(J);
  if (J->curfinal) {
    GCtrace *scratch = J->curfinal;
    /* Close the token-private observation before publishing the raw retire
    ** descriptor. A pre-clear observer is covered by the retirement epoch. */
    J->curfinal = NULL;
    lj_trace_free_unpublished(J2G(J), scratch);
  }
  if (tvisnumber(L->top-1))
    e = (TraceError)numberVint(L->top-1);
  /* MCODELM retries rebuild per-trace exit stubs in a fresh mcode area. */
  trace_exittab_free(J2G(J), &J->cur, J->cur.nsnap);
  if (e == LJ_TRERR_MCODELM) {
    if (!lj_trace_state_aborted(
	  lj_trace_state_store_active(J, LJ_TRACE_ASM))) {
      L->top--;  /* Remove error object. */
      return TRACE_ABORT_RETRY;  /* Retry ASM with new MCode area. */
    }
    /* An asynchronous abort won before the restart. Continue through ordinary
    ** slot/error cleanup; publishing IDLE here would leak J->cur's reservation. */
    e = LJ_TRERR_RECERR;
    setintV(L->top-1, (int32_t)e);
  }
  /* Penalize or blacklist starting bytecode instruction. */
  if (J->parent == 0 && !bc_isret(bc_op(J->cur.startins))) {
    if (J->exitno == 0) {
      BCIns *startpc = mref(J->cur.startpc, BCIns);
      if (e == LJ_TRERR_RETRY || e == LJ_TRERR_SMRRETRY)
	(void)hotcount_setl(J2G(J), jit_owner_l_acq(J), startpc+1, 1);
      else
	penalty_pc(J, trace_startpt_acq(&J->cur), startpc, e);
    } else {
      TraceNo selflink = (TraceNo)J->exitno;
      GCtrace *T;
      /* Blacklisting is a retry-suppression optimization. Error cleanup still
      ** owns the recorder token, so it must not wait behind an exclusive body
      ** reclaimer which may need that token to finish. */
      if (lj_gc2_smr_read_try(J2G(J))) {
	T = traceref_safe(J, selflink);
	if (T && trace_traceno_acq(T) == selflink) {
	  trace_test_note_abort_selflink(selflink);
	  trace_link_rel(T, selflink);  /* Self-link is blacklisted. */
	}
	lj_gc2_smr_read_leave(J2G(J));
      }
    }
  }

  /* Is there anything to abort? */
  traceno = J->cur.traceno;
  if (traceno) {
    J->cur.link = 0;
    J->cur.linktype = LJ_TRLINK_NONE;
    /* SMRRETRY is the exact fail-closed transition raised when trace-body SMR
    ** is already exclusively closed. The abort event is observational and
    ** arbitrary handler code can enter another reader while this callback
    ** still owns the recorder token, so only this collision path suppresses
    ** instrumentation. Ordinary RETRY remains API-visible. */
    if (e != LJ_TRERR_SMRRETRY) {
      lj_vmevent_send_l(L, TRACE,
	cTValue *bot = tvref(L->stack)+LJ_FR2;
	cTValue *frame;
	const BCIns *pc;
	BCPos pos = 0;
	setstrV(V, V->top++, lj_str_newlit(V, "abort"));
	setintV(V->top++, traceno);
	/* Find original Lua function call to generate a better error message. */
	for (frame = L->base-1, pc = J->pc; ; frame = frame_prev(frame)) {
	  if (isluafunc(frame_func(frame))) {
	    pos = proto_bcpos(funcproto(frame_func(frame)), pc);
	    break;
	  } else if (frame_prev(frame) <= bot) {
	    break;
	  } else if (frame_iscont(frame)) {
	    pc = frame_contpc(frame) - 1;
	  } else {
	    pc = frame_pc(frame) - 1;
	  }
	}
	setfuncV(V, V->top++, frame_func(frame));
	setintV(V->top++, pos);
	copyTV(V, V->top++, restorestack(L, vmevtop)-1);
	copyTV(V, V->top++, &J->errinfo);
      );
    }
    /* Drop aborted trace after the optional vmevent (which may still access it). */
    traceslot_clear(J, traceno);
    if (traceno < J->freetrace)
      J->freetrace = traceno;
    J->cur.traceno = 0;
  }
  L->top--;  /* Remove error object */
  if (e == LJ_TRERR_DOWNREC) {
    TraceStartResult start_result = trace_downrec(J);
    if (start_result == TRACE_START_RESULT_FLUSH_ALL)
      return TRACE_ABORT_FLUSH_ALL;
    if (start_result == TRACE_START_RESULT_ACTIVE &&
	lj_trace_state_load(J) != LJ_TRACE_IDLE)
      return TRACE_ABORT_RETRY;
    return TRACE_ABORT_DONE;
  } else if (e == LJ_TRERR_MCODEAL) {
    if (!J->mcarea) {  /* Disable JIT compiler if first mcode alloc fails. */
      jit_flags_setmask(J, JIT_F_ON, 0);
    }
    /* Full flush may cross a safepoint boundary and reacquire the recorder
    ** token. Defer it to trace_state() after terminal token release. */
    return TRACE_ABORT_FLUSH_ALL;
  }
  return TRACE_ABORT_DONE;
}

/* Perform pending re-patch of a bytecode instruction. */
static LJ_AINLINE void trace_pendpatch(jit_State *J, int force)
{
  if (LJ_UNLIKELY(J->patchpc)) {
    if (force || J->bcskip == 0) {
      BCIns patchins = J->patchins;
      BCOp op = bc_op(patchins);
      if (op == BC_JFORL || op == BC_JITERL || op == BC_JLOOP ||
	  op == BC_JFUNCF) {
	TraceNo traceno = bc_d(patchins);
	GCtrace *T;
	/* Re-patching is optional for correctness: on contention, retaining the
	** immutable original bytecode merely leaves this trace detached from its
	** root entry. Never loop on SMR here—the synchronous recorder-error path
	** still owns the JIT token and a peer reclaimer may be waiting for it. */
	if (lj_gc2_smr_read_try(J2G(J))) {
	  T = traceref_safe(J, traceno);
	  if (trace_runnable_acq(T, traceno)) {
	    BCIns expected = trace_startins_acq(T);
	    /* A failed ISNEXT may have terminally despecialized ITERN while the
	    ** recorder used a temporary original instruction. Never resurrect the
	    ** saved JLOOP over that newer ITERC generation. */
	    (void)bc_publish_cas(J->patchpc, (uint32_t *)&expected, patchins);
	  }
	  lj_gc2_smr_read_leave(J2G(J));
	}
      } else {
	bc_publish(J->patchpc, patchins);
      }
      J->patchpc = NULL;
    } else {
      J->bcskip = 0;
    }
  }
}

/* Publish terminal recorder state before any operation which may itself need
** the JIT token or enter a safepoint/dispatch refresh. The release helper also
** clears jit_owner_l while IDLE, so ordinary dispatch rebuilding cannot expose
** a stale detachable owner. */
static void trace_terminal_release(lua_State *L, jit_State *J)
{
  global_State *g = J2G(J);
  setvmstate(g, INTERP);
  lj_trace_state_store(J, LJ_TRACE_IDLE);
  lj_jit_token_release_l(L, J);
  lj_dispatch_update(g, 0);
}

/* State machine for the trace compiler. Protected callback. */
static TValue *trace_state(lua_State *L, lua_CFunction dummy, void *ud)
{
  jit_State *J = (jit_State *)ud;
  UNUSED(dummy);
  trace_test_admission_inject(L, J, J->pc,
			      LJ_TRACE_TEST_ADMISSION_TRACE_STATE);
  if (lj_safepoint_owner_poll_pending(L)) {
    /* This callback is the first protected boundary after publishing START.
    ** A fresh STOPREQ must unwind through lj_trace_ins(), whose external-error
    ** cleanup retires the unpublished recorder and releases its exact owner. */
    trace_test_admission_note_protected_poll();
    (void)lj_safepoint_ack_check(L);
  }
  do {
  retry:
    switch ((uint32_t)lj_trace_state_load(J)) {
    case LJ_TRACE_START:
      {
	TraceStartResult start_result;
	if (lj_trace_state_aborted(
	      lj_trace_state_store_active(J, LJ_TRACE_RECORD)))
	  goto retry;
	start_result = trace_start(J);
	if (start_result != TRACE_START_RESULT_ACTIVE) {
	  trace_terminal_release(J->L, J);
	  if (start_result == TRACE_START_RESULT_FLUSH_ALL)
	    (void)lj_trace_flushall_hs(L);
	  return NULL;
	}
	if (!lj_dispatch_record_start(J->L, J)) {
	  lj_trace_state_abort(J);
	  goto retry;
	}
	if (lj_trace_state_aborted(lj_trace_state_load(J)))
	  goto retry;
	if (lj_trace_state_load(J) != LJ_TRACE_RECORD_1ST)
	  break;
      }
      /* fallthrough */

    case LJ_TRACE_RECORD_1ST:
      if (lj_trace_state_aborted(
	    lj_trace_state_store_active(J, LJ_TRACE_RECORD)))
	goto retry;
      /* fallthrough */
    case LJ_TRACE_RECORD:
      trace_pendpatch(J, 0);
      setvmstate(J2G(J), RECORD);
      lj_vmevent_send_l_(J->L, RECORD,
	/* Save/restore state for trace recorder. */
	TValue savetv = J2TG(J)->tmptv;
	TValue savetv2 = J2TG(J)->tmptv2;
	TraceNo parent = J->parent;
	ExitNo exitno = J->exitno;
	setintV(V->top++, J->cur.traceno);
	setfuncV(V, V->top++, J->fn);
	setintV(V->top++, J->pt ? (int32_t)proto_bcpos(J->pt, J->pc) : -1);
	setintV(V->top++, J->framedepth);
      ,
	J2TG(J)->tmptv = savetv;
	J2TG(J)->tmptv2 = savetv2;
	J->parent = parent;
	J->exitno = exitno;
      );
      lj_record_ins(J);
      break;

    case LJ_TRACE_END:
      trace_pendpatch(J, 1);
      J->loopref = 0;
      if ((jit_flags_acq(J) & JIT_F_OPT_LOOP) &&
	  J->cur.link == J->cur.traceno && J->framedepth + J->retdepth == 0) {
	setvmstate(J2G(J), OPT);
	lj_opt_dce(J);
	if (lj_opt_loop(J)) {  /* Loop optimization failed? */
	  J->cur.link = 0;
	  J->cur.linktype = LJ_TRLINK_NONE;
	  J->loopref = J->cur.nins;
	  if (lj_trace_state_aborted(
		lj_trace_state_store_active(J, LJ_TRACE_RECORD)))
	    goto retry;  /* Try to continue recording. */
	  break;
	}
	J->loopref = J->chain[IR_LOOP];  /* Needed by assembler. */
      }
      lj_opt_split(J);
      lj_opt_sink(J);
      if (!J->loopref) J->cur.snap[J->cur.nsnap-1].count = SNAPCOUNT_DONE;
      if (lj_trace_state_aborted(
	    lj_trace_state_store_active(J, LJ_TRACE_ASM)))
	goto retry;
      break;

    case LJ_TRACE_ASM:
      {
        int gc_pressure;
        setvmstate(J2G(J), ASM);
        lj_asm_trace(J, &J->cur);
        if (lj_trace_state_aborted(lj_trace_state_load(J)))
	  goto retry;
        gc_pressure = trace_stop(J);
        trace_terminal_release(J->L, J);
        if (gc_pressure)
	  (void)lj_gc2_request_cycle_pressure(G(L), L2TG(L));
        if (gc2_phase_acq(G(L)) != LJ_GC2_IDLE || lj_gc_should_step(G(L)))
	  lj_gc_step(L);
        return NULL;
      }

    default:  /* Trace aborted asynchronously. */
      setintV(L->top++, (int32_t)LJ_TRERR_RECERR);
      /* fallthrough */
    /* lj_err_throw() clears ACTIVE for synchronous recorder errors, too. */
    case (LJ_TRACE_ERR & ~LJ_TRACE_ACTIVE):
    case LJ_TRACE_ERR:
      {
      TraceAbortResult abort_result;
      trace_pendpatch(J, 1);
      abort_result = trace_abort(J);
      if (abort_result == TRACE_ABORT_RETRY)
	goto retry;
      trace_terminal_release(J->L, J);
      if (abort_result == TRACE_ABORT_FLUSH_ALL)
	(void)lj_trace_flushall_hs(L);
      return NULL;
      }
    }
  } while (lj_trace_state_load(J) > LJ_TRACE_RECORD);
  if (lj_trace_state_aborted(lj_trace_state_load(J)))
    goto retry;
  if (lj_trace_state_load(J) == LJ_TRACE_IDLE)
    lj_jit_token_release_l(J->L, J);
  return NULL;
}

/* Cancel unpublished recorder state before its owning TG detaches. */
void lj_trace_abort_owner(lua_State *L)
{
  jit_State *J;
  global_State *g;
  TraceNo traceno;
  if (!L)
    return;
  J = L2J(L);
  /* Prove ownership before reading or mutating any token-private recorder
  ** field. J->L may name a coroutine on this TG, not necessarily L itself.
  */
  if (!lj_jit_token_held_l(L, J))
    return;
  g = J2G(J);
  lj_trace_state_abort(J);
  trace_pendpatch(J, 1);
  J->postproc = LJ_POST_NONE;
  lj_mcode_abort(J);
  if (J->curfinal) {
    GCtrace *scratch = J->curfinal;
    /* Detach teardown may release the recorder token immediately below. Make
    ** the raw retire node, not J->curfinal, the sole lifetime descriptor. */
    J->curfinal = NULL;
    lj_trace_free_unpublished(g, scratch);
  }
  trace_exittab_free(g, &J->cur, J->cur.nsnap);
  traceno = J->cur.traceno;
  if (traceno) {
    J->cur.link = 0;
    J->cur.linktype = LJ_TRLINK_NONE;
    traceslot_clear(J, traceno);
    if (traceno < J->freetrace)
      J->freetrace = traceno;
  }
  memset(&J->cur, 0, sizeof(J->cur));
  J->patchpc = NULL;
  J->patchins = 0;
  J->bcskip = 0;
  J->root_startins_pending = 0;
  J->mergesnap = 0;
  J->needsnap = 0;
  J->guardemit.irt = 0;
  J->retryrec = 0;
  J->loopref = 0;
  J->ktrace = 0;
  J->parent = 0;
  J->exitno = 0;
  /* Do not run trace_abort(): teardown occurs after lua_pcall has unwound the
  ** recorded frame, so penalty, down-recursion and TRACE-abort event paths must
  ** not inspect J->pc or walk L's now-different frame chain.
  */
  trace_terminal_release(L, J);
  trace_test_admission_maybe_clobber_cleanup_oserr();
}

/* Retire an interrupted recorder before its owner parks outside the recorder
** callback stack. A TRACE/RECORD VM-event handler still has trace_state() live
** beneath it; destructively clearing J->cur there would make that state machine
** resume into freed or zeroed recorder state. Leave the exact callback owner
** intact so trace_state() consumes the asynchronous abort after the handler
** returns. */
void lj_trace_abort_owner_before_park(lua_State *L)
{
  global_State *g;
  if (!L)
    return;
  g = G(L);
  if (vmevent_owner_acq(g) == lj_thr_current_id(g))
    return;
  lj_trace_abort_owner(L);
}

/* -- Event handling ------------------------------------------------------ */

/* A bytecode instruction is about to be executed. Record it. */
void lj_trace_ins(jit_State *J, const BCIns *pc)
{
#if LJ_ARM64_JIT_FAIL_CLOSED
  /* Defensive recorder ingress: direct/stale dispatch must not be able to
  ** bypass lj_trace_hot() and leave an unpublished token-owned trace behind. */
  UNUSED(pc);
  UNUSED(trace_state);  /* Keep the complete recorder compiled for this gate. */
  lj_trace_abort_owner(J->L);
  return;
#else
  lua_State *L = J->L;
  int errcode;
  /* Note: J->L must already be set. pc is the true bytecode PC here. */
  J->pc = pc;
  J->fn = curr_func(L);
  J->pt = isluafunc(J->fn) ? funcproto(J->fn) : NULL;
  while ((errcode = lj_vm_cpcall(L, NULL, (void *)J,
				 trace_state)) != 0) {
    /* Recorder failures use an integer LJ_TRERR_* object and are consumed by
    ** trace_state() on the next protected entry. An external VM error (notably
    ** STOPREQ from a native publication boundary) carries its ordinary Lua
    ** error object. Re-entering cpcall with that pending unwind can only throw
    ** again, stranding the recorder token in an infinite ERR loop. We are now
    ** between trace_state() invocations, so discard unpublished owner state
    ** before propagating the original error to the surrounding Lua pcall. */
    if (errcode != LUA_ERRRUN || !tvisnumber(L->top - 1)) {
      LJOSerrState oserr;
      /* cpcall has restored the error-edge errno/LastError pair. Recorder
      ** cleanup may call allocators, dispatch rebuilds or platform teardown;
      ** carry the authoritative pair across it before throwing outward. */
      lj_oserr_save(&oserr);
      lj_trace_abort_owner(L);
      lj_oserr_restore(&oserr);
      lj_err_throw(L, errcode);
    }
    lj_trace_state_store_active(J, LJ_TRACE_ERR);
  }
#endif
}

static int trace_hot_root_start_valid(const BCIns *pc)
{
  /* Hotcount events are edge-triggered against mutable bytecode. Another
  ** thread may publish a JIT or disabled variant while this TG is reaching
  ** the recorder. Only unpatched root-start bytecodes may enter
  ** rec_setup_root(); patched bytecode should redispatch normally.
  */
  BCOp op = bc_op((BCIns)la_load32_acq((uint32_t *)pc));
  return op == BC_FORL || op == BC_ITERL || op == BC_ITERN ||
	 op == BC_LOOP || op == BC_FUNCF;
}

/* A hotcount triggered. Start recording a root trace. */
void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc, lua_State *L)
{
  /* Note: pc is the interpreter bytecode PC here. It's offset by 1. */
  ERRNO_SAVE
  trace_test_admission_inject(L, J, pc, LJ_TRACE_TEST_ADMISSION_ENTRY);
  /* vm_hotloop reaches C only after its counter subtraction and deliberately
  ** does not skip or route the underflowing bytecode through vm_safepoint.
  ** Service both counted handshakes and profile-only signals here before the
  ** counter reset or any recorder-token acquisition. */
  if (lj_safepoint_owner_poll_pending(L))
    (void)lj_safepoint_ack_check(L);
  /* Reset hotcount. */
  (void)hotcount_setl(J2G(J), L, pc,
	jit_param_acq(J, JIT_P_hotloop)*HOTCOUNT_LOOP);
#if LJ_ARM64_JIT_FAIL_CLOSED
  /* Compile the complete JIT surface without permitting recorder ownership,
  ** trace publication, or native entry during the ARM64 scaffolding phase. */
  UNUSED(L);
  ERRNO_RESTORE
  return;
#endif
  /* Only start a new trace if not recording or inside __gc call or vmevent. */
  if ((jit_flags_acq(J) & JIT_F_ON) &&
      lj_trace_state_load(J) == LJ_TRACE_IDLE &&
      !(lj_tg_hookmask_combined_load(J2G(J), L2TG(L)) &
	(HOOK_GC|HOOK_VMEVENT)) &&
      lj_jit_trace_stream_idle(J2G(J)) &&
      lj_jit_token_try_l(L, J)) {
    jit_owner_l_rel(J, L);
    trace_test_admission_inject(L, J, pc,
				LJ_TRACE_TEST_ADMISSION_AFTER_TOKEN);
    /* Close the pre-admission check/token-CAS race without acknowledging while
    ** an otherwise disposable IDLE token is published. A late STOPREQ may
    ** throw, so clear both the low token and J->L before entering the checked
    ** owner path. A non-throwing request abandons this hot edge and lets normal
    ** dispatch retry later. */
    if (lj_safepoint_owner_poll_pending(L)) {
      lj_jit_token_release_l(L, J);
      trace_test_admission_note_clean_release(L, J);
      (void)lj_safepoint_ack_check(L);
      ERRNO_RESTORE
      return;
    }
    /* Close the idle-snapshot/token-CAS race. A standalone terminal may have
    ** published before releasing the low token we just acquired. */
    if (!lj_jit_trace_stream_idle(J2G(J))) {
      lj_jit_token_release_l(L, J);
      ERRNO_RESTORE
      return;
    }
    if (!trace_hot_root_start_valid(pc-1)) {
      lj_jit_token_release_l(L, J);
      ERRNO_RESTORE
      return;
    }
    J->parent = 0;  /* Root trace. */
    J->exitno = 0;
    if (!lj_trace_state_aborted(
		  lj_trace_state_store_active(J, LJ_TRACE_START)))
      lj_trace_ins(J, pc-1);
    else
      lj_jit_token_release_l(L, J);
  }
  ERRNO_RESTORE
}

/* Check for a hot side exit. If yes, start recording a side trace. */
static void trace_hotside(jit_State *J, const BCIns *pc, lua_State *L,
			  TraceNo parent, ExitNo exitno)
{
  global_State *g = J2G(J);
  GCtrace *parentT;
  SnapShot *snap;
  uint32_t hotexit = (uint32_t)jit_param_acq(J, JIT_P_hotexit);
  uint8_t count;
#if LJ_ARM64_JIT_FAIL_CLOSED
  UNUSED(pc); UNUSED(L); UNUSED(parent); UNUSED(exitno);
  return;
#endif
  /* Side recording is speculative. If an exclusive trace reclaimer won the
  ** body gate, stay in the interpreter instead of waiting at a hot exit. */
  if (LJ_UNLIKELY(!lj_gc2_smr_read_try(g)))
    return;
  parentT = traceref_safe(J, parent);
  if (!trace_runnable_acq(parentT, parent) || exitno >= trace_nsnap_acq(parentT))
    goto out;
  {
    TraceNo rootno = trace_root_acq(parentT);
    GCtrace *root = rootno ? traceref_safe(J, rootno) : parentT;
    GCproto *pt = root ? trace_startpt_acq(root) : NULL;
    /*
    ** Root and first-level side traces have the complete original local-cell
    ** snapshot shape. Only an active-MT side-of-side would replay a snapshot
    ** that has already replayed CGET/CSET cells; leave that chain interpreted
    ** until generated code carries the nested-cell proof.
    */
    if (rootno != 0 && pt && proto_cellops(pt) &&
	lj_record_mt_runtime_shared(g, L))
      goto out;
  }
  snap = &trace_snap_acq(parentT)[exitno];
  /* The outer trace-exit check precedes this SMR/metadata acquisition. Close
  ** that window before the first shared snapshot-count CAS. Service remains
  ** deferred until vm_exit_interp has cleared the jit_base lifetime lease. */
  if (lj_safepoint_owner_poll_pending(L)) {
    lj_gc2_smr_read_leave(g);
    lj_safepoint_owner_rearm_counted_poll(L);
    trace_test_admission_note_side_gate_block();
    return;
  }
  if (!(lj_tg_hookmask_combined_load(g, L2TG(L)) &
	(HOOK_GC|HOOK_VMEVENT)) &&
      lj_jit_trace_stream_idle(g) &&
      isluafunc(curr_func(L))) {
    for (;;) {
      count = (uint8_t)snap_count_acq(snap);
      if (count == SNAPCOUNT_DONE)
	goto out;
      if ((uint32_t)count + 1u >= hotexit)
	break;
      if (snap_count_cas_acqrel(snap, &count, count + 1u))
	goto out;
    }
    if (lj_trace_state_load(J) != LJ_TRACE_IDLE)
      goto out;
    if (!lj_jit_token_try_l(L, J))
      goto out;
    trace_test_side_admission_inject_held(
	L, J, parent, exitno, snap,
	LJ_TRACE_TEST_ADMISSION_SIDE_AFTER_TOKEN);
    /* The pre-exit gate and token CAS are separate publications. Close that
    ** race before revalidating the stream or performing the post-token
    ** side-claim snapshot mutation. Pre-threshold count increments above are
    ** independent atomic CAS operations. Service may throw or park, so first
    ** drop both the disposable IDLE token and this trace-body SMR read lease.
    ** The VM exit landing still owns the published jit_base lifetime lease and
    ** will clear it before servicing TGPOLL; do not acknowledge here. */
    if (lj_safepoint_owner_poll_pending(L)) {
      lj_jit_token_release_l(L, J);
      lj_gc2_smr_read_leave(g);
      lj_safepoint_owner_rearm_counted_poll(L);
      trace_test_admission_note_side_clean_release(L, J);
      return;
    }
    if (!lj_jit_trace_stream_idle(g)) {
      lj_jit_token_release_l(L, J);
      goto out;
    }
    parentT = traceref_safe(J, parent);
    if (!trace_runnable_acq(parentT, parent) ||
	exitno >= trace_nsnap_acq(parentT)) {
      lj_jit_token_release_l(L, J);
      goto out;
    }
    snap = &trace_snap_acq(parentT)[exitno];
    for (;;) {
      count = (uint8_t)snap_count_acq(snap);
      if (count == SNAPCOUNT_DONE) {
	lj_jit_token_release_l(L, J);
	goto out;
      }
      if (count >= SNAPCOUNT_DONE-1 ||
	  snap_count_cas_acqrel(snap, &count, count + 1u))
	break;
    }
    jit_owner_l_rel(J, L);
    J->parent = parent;
    J->exitno = exitno;
    /* J->parent is non-zero for a side trace. */
    lj_gc2_smr_read_leave(g);
    if (!lj_trace_state_aborted(
	      lj_trace_state_store_active(J, LJ_TRACE_START)))
      lj_trace_ins(J, pc);
    else
      lj_jit_token_release_l(L, J);
    return;
  }
out:
  lj_gc2_smr_read_leave(g);
}

/* Stitch a new trace to the previous trace. */
uint32_t LJ_FASTCALL lj_trace_stitch_probe(jit_State *J, GCtrace *T)
{
  /*
  ** Stitched fast-function return traces restore across C/VM boundaries where
  ** GC2 stack publication and snapshot reconstruction are still conservative.
  ** Keep these edges interpreted until the stitched-exit state is proven as
  ** complete as ordinary hot-loop exits.
  */
  UNUSED(J); UNUSED(T);
  return 0;
}

void LJ_FASTCALL lj_trace_stitch(jit_State *J, const BCIns *pc, lua_State *L,
				 TraceNo traceno)
{
#if LJ_ARM64_JIT_FAIL_CLOSED
  /* This ingress currently has no producer, but keep it independently closed
  ** so a stale continuation cannot start or retain recorder state. */
  UNUSED(pc); UNUSED(traceno);
  lj_trace_abort_owner(L);
  UNUSED(J);
  return;
#else
  UNUSED(L); UNUSED(traceno);
  UNUSED(J); UNUSED(pc);
  return;
#endif
}


/* Tiny struct to pass data to protected call. */
typedef struct ExitDataCP {
  jit_State *J;
  lua_State *L;
  void *exptr;		/* Pointer to exit state. */
  GCtrace *T;		/* Exited trace body resolved before flush races. */
  TraceNo parent;	/* Exited trace. */
  ExitNo exitno;	/* Exited snapshot. */
  const BCIns *pc;	/* Restart interpreter at this PC. */
} ExitDataCP;

/* Need to protect lj_snap_restore because it may throw. */
static TValue *trace_exit_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  ExitDataCP *exd = (ExitDataCP *)ud;
  /* Always catch error here and don't call error function. */
  cframe_errfunc(L->cframe) = 0;
  cframe_nres(L->cframe) = -2*LUAI_MAXSTACK*(int)sizeof(TValue);
#if LJ_TARGET_X64 || LJ_TARGET_ARM64
  exd->pc = lj_snap_restore_exit(exd->J, exd->exptr, exd->L,
				 exd->T, exd->parent, exd->exitno);
#else
  exd->pc = lj_snap_restore(exd->J, exd->exptr);
#endif
  UNUSED(dummy);
  return NULL;
}

#ifndef LUAJIT_DISABLE_VMEVENT
/* Push all registers from exit state. */
static void trace_exit_regs(lua_State *V, ExitState *ex)
{
  int32_t i;
  setintV(V->top++, RID_NUM_GPR);
  setintV(V->top++, RID_NUM_FPR);
  for (i = 0; i < RID_NUM_GPR; i++) {
    if (sizeof(ex->gpr[i]) == sizeof(int32_t))
      setintV(V->top++, (int32_t)ex->gpr[i]);
    else
      setnumV(V->top++, (lua_Number)ex->gpr[i]);
  }
#if !LJ_SOFTFP
  for (i = 0; i < RID_NUM_FPR; i++) {
    setnumV(V->top, ex->fpr[i]);
    if (LJ_UNLIKELY(tvisnan(V->top)))
      setnanV(V->top);
    V->top++;
  }
#endif
}
#endif

#if defined(EXITSTATE_PCREG) || (LJ_UNWIND_JIT && !EXITTRACE_VMSTATE)
/* Determine trace number from pc of exit instruction. */
static TraceNo trace_exit_find(jit_State *J, MCode *pc, GCtrace **Tp)
{
  TraceNo traceno;
  MSize sizetrace = trace_sizetrace_acq(J);
  for (traceno = 1; traceno < sizetrace; traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (trace_exit_body_match(T, traceno)) {
      MCode *mcode = trace_mcode_acq(T);
      MSize szmcode = trace_szmcode_acq(T);
      if (mcode && pc >= mcode && pc < (MCode *)((char *)mcode + szmcode)) {
	if (Tp) *Tp = T;
	return traceno;
      }
    }
  }
  if (Tp) *Tp = NULL;
  lj_assertJ(0, "bad exit pc");
  return 0;
}
#endif

/* A trace exited. Restore interpreter state. */
#if LJ_TARGET_X64 && LJ_ABI_WIN
int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr, lua_State *L,
			      uint32_t exitpair)
#elif LJ_TARGET_X64 || LJ_TARGET_ARM64
int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr, lua_State *L,
			      TraceNo parent, ExitNo exitno)
#else
int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr)
#endif
{
  ERRNO_SAVE
#if LJ_TARGET_X64 && LJ_ABI_WIN
  /* The x64 exit stub encodes both values in 16 bits and GCtrace stores the
  ** corresponding public trace/snapshot counts in 16-bit fields. Keep the
  ** Win64 four-register ABI without publishing either ID through jit_State.
  */
  LJ_STATIC_ASSERT(sizeof(((GCtrace *)0)->traceno) == sizeof(uint16_t));
  LJ_STATIC_ASSERT(sizeof(((GCtrace *)0)->nsnap) == sizeof(uint16_t));
  TraceNo parent = (TraceNo)(exitpair >> 16);
  ExitNo exitno = (ExitNo)(exitpair & 0xffffu);
#elif !(LJ_TARGET_X64 || LJ_TARGET_ARM64)
  lua_State *L = J->L;
  TraceNo parent = J->parent;
  ExitNo exitno = J->exitno;
#endif
  trace_test_note_exit(parent, exitno);
#if LJ_TARGET_X64 || LJ_TARGET_ARM64
  /* Error unwind publishes exitcode in the currently executing TG, not in the
  ** lua_State's migratable owner hint. A coroutine can move between TGs after
  ** that hint was last sampled; use the same TLS/fallback route as unwind.
  */
  TGState *tg = G2TG(G(L));
#endif
  ExitState *ex = (ExitState *)exptr;
  ExitDataCP exd;
  int errcode;
#if LJ_TARGET_X64 || LJ_TARGET_ARM64
  int exitcode = lj_tg_jit_exitcode_acq(tg);
#else
  int exitcode = J->exitcode;
#endif
  TValue exiterr;
  const BCIns *pc;
  void *cf;
  GCtrace *T;
  global_State *g = G(L);

  setnilV(&exiterr);
  if (exitcode) {  /* Trace unwound with error code. */
#if LJ_TARGET_X64 || LJ_TARGET_ARM64
    lj_tg_jit_exitcode_rel(tg, 0);
#else
    J->exitcode = 0;
#endif
    copyTV(L, &exiterr, L->top-1);
  }

#ifdef EXITSTATE_PCREG
  lj_gc2_smr_read_enter(g);
  parent = trace_exit_find(J, (MCode *)(intptr_t)ex->gpr[EXITSTATE_PCREG],
			   &T);
#else
  lj_gc2_smr_read_enter(g);
  UNUSED(ex);
  T = traceref_safe(J, parent);
#endif
  if (!T)
    T = traceref_safe(J, parent);
  lj_assertJ(trace_exit_body_match(T, parent), "bad trace number");
#ifdef EXITSTATE_CHECKEXIT
  if (exitno == trace_nsnap_acq(T)) {  /* Stack check. */
    IRIns base = ir_load_acq(&trace_ir_acq(T)[REF_BASE]);
    lj_assertJ(trace_root_acq(T) != 0, "stack check in root trace");
    exitno = base.op2;
    parent = base.op1;
    T = traceref_safe(J, parent);
    lj_assertJ(trace_exit_body_match(T, parent), "bad stack-check trace");
  }
#endif
  lj_assertJ(trace_exit_body_match(T, parent) && exitno < trace_nsnap_acq(T),
	     "bad trace or exit number");
  exd.J = J;
  exd.L = L;
  exd.exptr = exptr;
  exd.T = T;
  exd.parent = parent;
  exd.exitno = exitno;
  /*
  ** Trace flush/retire keeps stale slots visible until SMR reclamation. Restore
  ** holds a reader while it walks the trace body and snapshot map, otherwise a
  ** concurrent grace pass can free metadata still needed to compute exd.pc.
  */
  errcode = lj_vm_cpcall(L, NULL, &exd, trace_exit_cp);
#if LJ_HASFFI && LJ_HASJIT
  /* A forced non-replayable foreign call transfers its exact body pin to this
  ** exit. Snapshot restore (or its protected error) has finished, while the
  ** SMR reader still keeps T valid for exact-match cleanup. */
  (void)lj_ffi_native_trace_exit_cleanup(L, T, (uint32_t)parent);
#endif
  lj_gc2_smr_read_leave(g);
  if (errcode) {
    ERRNO_RESTORE
    return -errcode;  /* Return negated error code. */
  }

  if (exitcode) copyTV(L, L->top++, &exiterr);  /* Anchor the error object. */

#if LJ_HASPROFILE
  if (!lj_profile_pending(L))
#endif
    lj_vmevent_send_l(L, TEXIT,
      lj_state_checkstack(V, 4+RID_NUM_GPR+RID_NUM_FPR+LUA_MINSTACK);
      setintV(V->top++, parent);
      setintV(V->top++, exitno);
      trace_exit_regs(V, ex);
    );

  pc = exd.pc;
  cf = cframe_raw(L->cframe);
  setcframe_pc(cf, pc);
  if (exitcode) {
    ERRNO_RESTORE
    return -exitcode;
#if LJ_HASPROFILE
  } else if (lj_profile_pending(L)) {
    /* Just exit to interpreter. */
#endif
  } else {
    int gcdefer = lj_gc2_jit_needs_exit(g);
    if (gcdefer) {
      /* GC-step exits must resume in the interpreter instead of recording a
      ** hot side trace that can stitch back to the same still-due GC check.
      ** Snapshot restore, the TEXIT event and its SMR read section are complete
      ** at this point, so this TG no longer depends on the exiting trace body or
      ** mcode. Publish that quiescence before GC2 starts a handshake: a peer
      ** trace-exit leader may otherwise wait for this jit_base while this TG is
      ** itself waiting to enter the serialized handshake. vm_exit_interp clears
      ** the same field again after this function returns.
      */
#if LJ_TARGET_X64 || LJ_TARGET_ARM64
      lj_tg_store_jit_base(tg, NULL);
#else
      lj_tg_store_jit_base(L2TG(L), NULL);
#endif
      if (gc2_phase_acq(g) == LJ_GC2_MARK)
	(void)lj_gc2_fixpoint_round(g, L, LJ_GC2_WORKER_DRAIN_BATCH);
      if (!(hookmask_load(g) & HOOK_GC))
	lj_gc_step(L);  /* Exited because of GC: drive GC forward. */
    } else if (jit_flags_acq(J) & JIT_F_ON) {
      trace_test_side_admission_inject(
	L, J, parent, exitno, LJ_TRACE_TEST_ADMISSION_SIDE_ENTRY);
      /* Snapshot hotcounts are shared parent metadata. A counted request can
      ** be visible in reqmask before its poll signal, while SIGPROF uses only
      ** profile_request; all three publications must block side mutation. */
      if (lj_safepoint_owner_poll_pending(L)) {
	lj_safepoint_owner_rearm_counted_poll(L);
	trace_test_admission_note_side_gate_block();
      } else {
	trace_hotside(J, pc, L, parent, exitno);
      }
    }
  }
  /* Return MULTRES or 0 or -17. */
  ERRNO_RESTORE
  {
    BCIns exitins = (BCIns)la_load32_acq((const uint32_t *)pc);
    switch (bc_op(exitins)) {
    case BC_CALLM: case BC_CALLMT:
      return (int)((BCReg)(L->top - L->base) -
		   bc_a(exitins) - bc_c(exitins) - LJ_FR2);
    case BC_RETM:
      return (int)((BCReg)(L->top - L->base) + 1 -
		   bc_a(exitins) - bc_d(exitins));
    case BC_TSETM:
      return (int)((BCReg)(L->top - L->base) + 1 - bc_a(exitins));
    case BC_JLOOP: {
      TraceNo targetno = bc_d(exitins);
      GCtrace *target;
      BCIns startins;
      /* This is only a stale-patch optimization after snapshot restoration.
      ** A closed trace-body gate means redispatch, not a peer-dependent wait. */
      if (LJ_UNLIKELY(!lj_gc2_smr_read_try(g)))
	return 0;
      target = traceref_safe(J, targetno);
      if (!trace_runnable_acq(target, targetno) ||
	  trace_startpc_acq(target) != pc) {
	lj_gc2_smr_read_leave(g);
	return 0;  /* Stale JLOOP after a concurrent flush: redispatch it. */
      }
      startins = trace_startins_acq(target);
      lj_gc2_smr_read_leave(g);
      if (bc_isret(bc_op(startins)) || bc_op(startins) == BC_ITERN) {
	/* Dispatch to original ins to ensure forward progress. */
	/* Only the exact recorder owner may use token-private temporary patch
	** state. A peer TG, another coroutine on the owner's TG, or nested GC/VM
	** event execution redispatches the immutable original statically instead;
	** none may overwrite the active recorder's patchpc/patchins/bcskip. */
	if (lj_trace_state_load(J) != LJ_TRACE_RECORD ||
	    !lj_jit_token_held_l(L, J) || jit_owner_l_acq(J) != L ||
	    (lj_tg_hookmask_combined_load(g, L2TG(L)) &
	     (HOOK_GC|HOOK_VMEVENT)) != 0)
	  return -17;
	/* Unpatch only the trace generation validated above. ISNEXT may have
	** terminally installed ITERC while this exit recovered metadata. */
	{
	  BCIns expected = exitins;
	  if (bc_publish_cas(pc, (uint32_t *)&expected, startins)) {
	    J->patchins = exitins;
	    J->patchpc = (BCIns *)pc;
	    J->bcskip = 1;
	  }
	}
      }
      return 0;
    }
    default:
      if (bc_isfunc_or_ff(bc_op(exitins)))
	return (int)((BCReg)(L->top - L->base) + 1);
      return 0;
    }
  }
}

#if LJ_UNWIND_JIT
#ifdef exitstub_trace_addr
static LJ_AINLINE uintptr_t trace_unwind_exitstub_addr_acq(GCtrace *T,
							   ExitNo exitno)
{
  TraceMCodeView tv;
  tv.mcode = trace_mcode_acq(T);
  tv.szmcode = trace_szmcode_acq(T);
  tv.exitstub = trace_exitstub_acq(T);
  if (tv.mcode == NULL)
    return 0;
#if LJ_TARGET_X86ORX64 && LJ_64
  if (tv.exitstub == NULL)
    return 0;
#endif
  return (uintptr_t)exitstub_trace_addr(&tv, exitno);
}
#endif

/* Given an mcode address determine trace exit address for unwinding. */
uintptr_t LJ_FASTCALL lj_trace_unwind(jit_State *J, uintptr_t addr, ExitNo *ep)
{
  global_State *g = J2G(J);
  GCtrace *T = NULL;
  uintptr_t target = 0;
#if EXITTRACE_VMSTATE
  TGState *tg = J2TG(J);
  TraceNo traceno = tg ?
    (TraceNo)lj_tg_vmstate_load_acq(tg) :
    (TraceNo)vmstate_load_acq(J2G(J));
#else
  TraceNo traceno;
#endif
  MCode *mcode;
  MSize szmcode;
  lj_gc2_smr_read_enter(g);
#if EXITTRACE_VMSTATE
  T = traceref_safe(J, traceno);
#else
  traceno = trace_exit_find(J, (MCode *)addr, &T);
#endif
  if (!T)
    T = traceref_safe(J, traceno);
  mcode = T ? trace_mcode_acq(T) : NULL;
  szmcode = T ? trace_szmcode_acq(T) : 0;
  if (T && mcode
#if EXITTRACE_VMSTATE
      && addr >= (uintptr_t)mcode &&
      addr < (uintptr_t)mcode + szmcode
#endif
     ) {
    SnapShot *snap = trace_snap_acq(T);
    SnapNo lo = 0, exitno = trace_nsnap_acq(T);
    uintptr_t ofs = (uintptr_t)((MCode *)addr - mcode);  /* MCode units! */
    /* Rightmost binary search for mcode offset to determine exit number. */
    do {
      SnapNo mid = (lo+exitno) >> 1;
      if (ofs < snap_mcofs_acq(&snap[mid])) exitno = mid; else lo = mid + 1;
    } while (lo < exitno);
    exitno--;
    *ep = exitno;
#ifdef exitstub_trace_addr
    target = trace_unwind_exitstub_addr_acq(T, exitno);
#elif defined(EXITSTUBS_PER_GROUP)
    target = (uintptr_t)exitstub_addr(J, exitno);
#endif
  }
  lj_gc2_smr_read_leave(g);
  if (target)
    return target;
  /* Cannot correlate addr with trace/exit. This will be fatal. */
  lj_assertJ(0, "bad exit pc");
  return 0;
}
#endif

#endif
