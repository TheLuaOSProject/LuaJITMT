/*
** Concurrent GC scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_gc2_c
#define LUA_CORE

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if LJ_GC2_PARANOIA
#include <stdio.h>
#endif

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_chan.h"
#include "lj_gc2.h"
#ifndef LJ_GC2_HAS_FINALIZER_DISPATCH_TYPE
/* Amalgamation may include lj_gc2.h before lj_gc2_c exposes internals. */
typedef int (*GC2FinalizerDispatchFunc)(lua_State *L, global_State *g,
					GCobj *o);
#define LJ_GC2_HAS_FINALIZER_DISPATCH_TYPE 1
#endif
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_thr.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_debug.h"
#include "lj_safepoint.h"
#include "lj_arena.h"
#include "lj_tg.h"
#include "lj_frame.h"
#include "lj_state.h"
#include "lj_lex.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_ccall.h"
#include "lj_cdata.h"
#include "lj_clib.h"
#endif
#include "lj_trace.h"
#include "lj_mcode.h"
#include "lj_dispatch.h"
#include "lj_vmevent.h"
#include "lj_vm.h"

#define GC2_GREY_INIT	LJ_GC2_GREY_EMBEDDED
#define GC2_GREY_LIMIT	((MSize)(LJ_MAX_MEM32 / sizeof(GCRef)))
#define GC2_WEAK_INIT	128u
#define GC2_WEAK_LIMIT	((MSize)(LJ_MAX_MEM32 / sizeof(GCRef)))
#define GC2_FINCLAIM_FIXED	4096u
#define GC2_ACTIVE_FINALIZE_COST	100
#define GC2_RECOVERY_LANES	3u
#define GC2_RECOVERY_LANE_BITS	2u
#define GC2_RECOVERY_LANE_MASK	((1u << GC2_RECOVERY_LANE_BITS) - 1u)
#define GC2_RECOVERY_LANE_QUANTUM	64u
#define GC2_JIT_MARK_AUTO_YIELDS	64u
#define GC2_JIT_MARK_YIELD_NS	50000u
#define GC2_JIT_SWEEP_YIELD_NS	50000u

static void lj_gc2_worker_wake(global_State *g);

static LJ_AINLINE int gc2_jit_recorder_active(global_State *g)
{
#if LJ_HASJIT
  return g && lj_trace_state_load(G2J(g)) != LJ_TRACE_IDLE;
#else
  UNUSED(g);
  return 0;
#endif
}

/*
** GC2 trace entry is a two-sided publication protocol. The VM first observes
** jit_phase_gate, publishes TG-local jit_base, executes an SC fence, and then
** revalidates the gate immediately before using trace metadata/mcode. A GC
** owner closes the gate and executes the matching SC fence before sampling
** jit_base/vmstate. Thus either the entry rejects the close or the owner sees
** an active intent and defers without waiting. XPOLL/allocation exits publish
** quiescence before a later owner performs physical reclaim.
*/
static void gc2_jit_phase_gate_close(global_State *g)
{
  gc2_jit_mark_yield_until_ns_rel(g, 0);
  gc2_jit_sweep_yield_until_ns_rel(g, 0);
  gc2_jit_phase_gate_rel(g, 0);
  la_fence_seq();
}

static void gc2_jit_phase_gate_open_idle(global_State *g)
{
  lj_assertG(gc2_phase_acq(g) == LJ_GC2_IDLE,
	     "IDLE JIT gate opened outside IDLE");
  gc2_jit_mark_resume_rel(g, 0);
  gc2_jit_mark_auto_yield_rel(g, 0);
  gc2_jit_mark_yield_until_ns_rel(g, 0);
  gc2_jit_sweep_displaced_rel(g, 0);
  gc2_jit_sweep_yield_until_ns_rel(g, 0);
  gc2_jit_phase_gate_rel(g, 1);
}

static int gc2_jit_mark_resume_authorized(global_State *g)
{
  uint32_t cycle;
  if (!g || gc2_phase_acq(g) != LJ_GC2_MARK)
    return 0;
  cycle = gc2_cycle_acq(g);
  return gc2_jit_mark_resume_acq(g) == cycle;
}

static void gc2_jit_phase_gate_open_mark(global_State *g, int mutator_turn)
{
  uint64_t deadline = 0;
  if (!gc2_jit_mark_resume_authorized(g))
    return;
  lj_assertG(gc2_phase_acq(g) == LJ_GC2_MARK &&
	     gc2_jit_mark_resume_acq(g) == gc2_cycle_acq(g),
	     "MARK JIT gate opened before activation acknowledgement");
  if (mutator_turn) {
    uint64_t now = lj_thr_now_ns();
    deadline = now > ~(uint64_t)0 - GC2_JIT_MARK_YIELD_NS ?
	       ~(uint64_t)0 : now + GC2_JIT_MARK_YIELD_NS;
  }
  gc2_jit_sweep_yield_until_ns_rel(g, 0);
  gc2_jit_mark_yield_until_ns_rel(g, deadline);
  gc2_jit_phase_gate_rel(g, 1);
}

static int gc2_jit_mark_turn_deferred(global_State *g)
{
  uint64_t deadline;
  if (!gc2_jit_mark_resume_authorized(g) ||
      gc2_jit_phase_gate_acq(g) == 0)
    return 0;
  /* Once a real entry consumed the pre-dispatch allowance and every TG has
  ** returned from native code, there is no lease holder to protect. A racing
  ** entrant is still covered by the gate-close publication/recheck protocol. */
  if (gc2_jit_mark_auto_yield_acq(g) == 0 &&
      !lj_tg_any_jit_active(g))
    return 0;
  deadline = gc2_jit_mark_yield_until_ns_acq(g);
  return deadline != 0 && lj_thr_now_ns() < deadline;
}

static void gc2_jit_phase_gate_open_sweep(global_State *g, int mutator_turn)
{
  uint64_t deadline = 0;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP ||
      !gc2_sweep_bridge_ready_acq(g))
    return;
  lj_assertG(gc2_phase_acq(g) == LJ_GC2_SWEEP &&
	     gc2_sweep_bridge_ready_acq(g) != 0,
	     "SWEEP JIT gate opened before READY");
  if (gc2_jit_sweep_displaced_xchg_acqrel(g, 0) != 0)
    mutator_turn = 1;
  if (mutator_turn) {
    uint64_t now = lj_thr_now_ns();
    deadline = now > ~(uint64_t)0 - GC2_JIT_SWEEP_YIELD_NS ?
	       ~(uint64_t)0 : now + GC2_JIT_SWEEP_YIELD_NS;
  }
  /* Publish the scheduling lease before native entry is admitted. */
  gc2_jit_mark_resume_rel(g, 0);
  gc2_jit_mark_auto_yield_rel(g, 0);
  gc2_jit_mark_yield_until_ns_rel(g, 0);
  gc2_jit_sweep_yield_until_ns_rel(g, deadline);
  gc2_jit_phase_gate_rel(g, 1);
}

static int gc2_jit_sweep_turn_deferred(global_State *g)
{
  uint64_t deadline;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP ||
      gc2_jit_phase_gate_acq(g) == 0)
    return 0;
  deadline = gc2_jit_sweep_yield_until_ns_acq(g);
  if (deadline == 0)
    return 0;
  if (lj_thr_now_ns() < deadline)
    return 1;
  /* Expiry is observational until an owned close. A pre-claim observer must
  ** not erase a fresh lease concurrently published by the current worker. */
  return 0;
}

int lj_gc2_jit_entry_open(global_State *g)
{
  uint32_t phase;
  if (!g || gc2_jit_phase_gate_acq(g) == 0)
    return 0;
  phase = gc2_phase_acq(g);
  return phase == LJ_GC2_IDLE ||
    (phase == LJ_GC2_MARK && gc2_jit_mark_resume_authorized(g)) ||
    (phase == LJ_GC2_SWEEP && gc2_sweep_bridge_ready_acq(g) != 0);
}

void lj_gc2_jit_mark_request_exit(global_State *g)
{
  if (!gc2_jit_mark_resume_authorized(g) ||
      gc2_jit_phase_gate_acq(g) == 0)
    return;
  /* The caller can still own trace mcode. Publish only the asynchronous entry
  ** close; a later collector observes jit_base==NULL before MARK fixpoint or
  ** root-snapshot work and may then grant the next bounded native turn. */
  gc2_jit_phase_gate_close(g);
  lj_gc2_worker_wake(g);
}

void lj_gc2_jit_sweep_request_exit(global_State *g)
{
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP ||
      !gc2_sweep_bridge_ready_acq(g) || gc2_jit_phase_gate_acq(g) == 0)
    return;
  /* This can run inside lj_gc_step_jit while the caller still owns its mcode.
  ** Close only the entry/XPOLL gate; the helper returns to the normal trace
  ** exit before any collector is allowed to observe zero active users. */
  gc2_jit_phase_gate_close(g);
  gc2_jit_sweep_displaced_rel(g, 1);
  lj_gc2_sweep_publish_wake(g);
}

#ifdef LJ_FUNC_TEST_HELPERS
/* Test-only words read by an x64 no-call spin probe after exact certificate
** validation and before any typed claim.  Keep these naturally aligned and
** use the ordinary atomic helpers for the C-side arm/observe/release edges. */
static uint32_t gc2_test_fnew_env_pause_armed;
static uint32_t gc2_test_fnew_env_pause_waiting;
static uint32_t gc2_test_fnew_env_pause_release;

uintptr_t lj_gc2_test_fnew_env_pause_armed_addr(void)
{
  return (uintptr_t)(void *)&gc2_test_fnew_env_pause_armed;
}

uintptr_t lj_gc2_test_fnew_env_pause_waiting_addr(void)
{
  return (uintptr_t)(void *)&gc2_test_fnew_env_pause_waiting;
}

uintptr_t lj_gc2_test_fnew_env_pause_release_addr(void)
{
  return (uintptr_t)(void *)&gc2_test_fnew_env_pause_release;
}

void lj_gc2_test_fnew_env_pause_arm(void)
{
  la_store32_rel(&gc2_test_fnew_env_pause_release, 0);
  la_store32_rel(&gc2_test_fnew_env_pause_waiting, 0);
  la_store32_rel(&gc2_test_fnew_env_pause_armed, 1);
}

uint32_t lj_gc2_test_fnew_env_pause_waiting(void)
{
  return la_load32_acq(&gc2_test_fnew_env_pause_waiting);
}

void lj_gc2_test_fnew_env_pause_continue(void)
{
  /* Disarm later loop iterations before releasing the one already waiting. */
  la_store32_rel(&gc2_test_fnew_env_pause_armed, 0);
  la_store32_rel(&gc2_test_fnew_env_pause_release, 1);
}
#endif

#if defined(LJ_GC2_TEST_HELPERS)
static uint32_t gc2_recovery_test_fail_grey_grow;
static uint32_t gc2_recovery_test_pause_stage;
static uint32_t gc2_recovery_test_paused_stage;
static uint32_t gc2_recovery_test_pause_release;
static uint32_t gc2_thread_needscan_test_pause_stage;
static uint32_t gc2_thread_needscan_test_paused_stage;
static uint32_t gc2_thread_needscan_test_pause_release;
static uint32_t gc2_table_rescan_test_pause_stage;
static uint32_t gc2_table_rescan_test_paused_stage;
static uint32_t gc2_table_rescan_test_pause_release;
static uintptr_t gc2_queue_post_admit_test_target;
static uint32_t gc2_queue_post_admit_test_armed;
static uint32_t gc2_queue_post_admit_test_paused;
static uint32_t gc2_queue_post_admit_test_release;
static uintptr_t gc2_queue_retry_witness_test_target;
static uint32_t gc2_queue_retry_witness_test_armed;
static uint32_t gc2_queue_retry_witness_test_paused;
static uint32_t gc2_queue_retry_witness_test_release;
static uint64_t gc2_test_jit_mark_checkpoint_closes;
static uint64_t gc2_test_jit_sweep_checkpoint_closes;
static uintptr_t gc2_test_stack_admission_retry_target;
static uint32_t gc2_test_stack_admission_retry_armed;
static uint32_t gc2_test_stack_admission_retry_hits;
static uintptr_t gc2_test_root_semantic_retry_target;
static uint32_t gc2_test_root_semantic_retry_armed;
static uint32_t gc2_test_root_semantic_retry_hits;
static uint32_t gc2_test_weak_frontier_fault_kind;
static uint32_t gc2_test_weak_frontier_fault_skip;
static uint32_t gc2_test_weak_frontier_fault_armed;
static uint32_t gc2_test_weak_frontier_fault_hits;

void lj_gc2_test_jit_mark_checkpoint_reset(void)
{
  la_store64_rel(&gc2_test_jit_mark_checkpoint_closes, 0);
}

uint64_t lj_gc2_test_jit_mark_checkpoint_closes(void)
{
  return la_load64_acq(&gc2_test_jit_mark_checkpoint_closes);
}

static void gc2_test_jit_mark_checkpoint_closed(void)
{
  (void)la_add64_rlx(&gc2_test_jit_mark_checkpoint_closes, 1);
}

void lj_gc2_test_jit_sweep_checkpoint_reset(void)
{
  la_store64_rel(&gc2_test_jit_sweep_checkpoint_closes, 0);
}

uint64_t lj_gc2_test_jit_sweep_checkpoint_closes(void)
{
  return la_load64_acq(&gc2_test_jit_sweep_checkpoint_closes);
}

static void gc2_test_jit_sweep_checkpoint_closed(void)
{
  (void)la_add64_rlx(&gc2_test_jit_sweep_checkpoint_closes, 1);
}
static uint32_t gc2_recovery_test_huge_scans;
static uint32_t gc2_recovery_test_worker_table_skips;

static int gc2_recovery_test_take_fail(uint32_t *counter)
{
  uint32_t old = la_load32_acq(counter);
  while (old != 0) {
    uint32_t expect = old;
    if (la_cas32(counter, &expect, old - 1u, LA_ACQ_REL, LA_ACQ))
      return 1;
    old = expect;
  }
  return 0;
}

void lj_gc2_test_stack_admission_retry_once(GCobj *target)
{
  la_store32_rel(&gc2_test_stack_admission_retry_hits, 0);
  la_storeuptr_rel(&gc2_test_stack_admission_retry_target,
		   (uintptr_t)(void *)target);
  la_store32_rel(&gc2_test_stack_admission_retry_armed, target != NULL);
}

uint32_t lj_gc2_test_stack_admission_retry_hits(void)
{
  return la_load32_acq(&gc2_test_stack_admission_retry_hits);
}

static int gc2_tv_test_take_admission_retry(GCobj *o)
{
  uint32_t expect = 1;
  if ((GCobj *)(void *)la_loaduptr_acq(
	&gc2_test_stack_admission_retry_target) != o)
    return 0;
  if (!la_cas32(&gc2_test_stack_admission_retry_armed, &expect, 0,
		LA_ACQ_REL, LA_ACQ))
    return 0;
  (void)la_add32_rlx(&gc2_test_stack_admission_retry_hits, 1);
  return 1;
}

void lj_gc2_test_root_semantic_retry_once(GCobj *target)
{
  la_store32_rel(&gc2_test_root_semantic_retry_hits, 0);
  la_storeuptr_rel(&gc2_test_root_semantic_retry_target,
		   (uintptr_t)(void *)target);
  la_store32_rel(&gc2_test_root_semantic_retry_armed, target != NULL);
}

uint32_t lj_gc2_test_root_semantic_retry_hits(void)
{
  return la_load32_acq(&gc2_test_root_semantic_retry_hits);
}

static int gc2_root_test_take_semantic_retry(GCobj *o)
{
  uint32_t expect = 1;
  if ((GCobj *)(void *)la_loaduptr_acq(
	&gc2_test_root_semantic_retry_target) != o)
    return 0;
  if (!la_cas32(&gc2_test_root_semantic_retry_armed, &expect, 0,
		LA_ACQ_REL, LA_ACQ))
    return 0;
  (void)la_add32_rlx(&gc2_test_root_semantic_retry_hits, 1);
  return 1;
}

void lj_gc2_test_weak_frontier_fault_once(uint32_t kind, uint32_t skip)
{
  la_store32_rel(&gc2_test_weak_frontier_fault_hits, 0);
  la_store32_rel(&gc2_test_weak_frontier_fault_skip, skip);
  la_store32_rel(&gc2_test_weak_frontier_fault_kind, kind);
  la_store32_rel(&gc2_test_weak_frontier_fault_armed, kind != 0);
}

uint32_t lj_gc2_test_weak_frontier_fault_hits(void)
{
  return la_load32_acq(&gc2_test_weak_frontier_fault_hits);
}

static int gc2_test_weak_frontier_take_fault(uint32_t kind)
{
  uint32_t skip;
  if (la_load32_acq(&gc2_test_weak_frontier_fault_kind) != kind ||
      la_load32_acq(&gc2_test_weak_frontier_fault_armed) == 0)
    return 0;
  skip = la_load32_acq(&gc2_test_weak_frontier_fault_skip);
  while (skip != 0) {
    uint32_t expect = skip;
    if (la_cas32(&gc2_test_weak_frontier_fault_skip, &expect, skip - 1u,
		 LA_ACQ_REL, LA_ACQ))
      return 0;
    skip = expect;
  }
  {
    uint32_t expect = 1;
    if (!la_cas32(&gc2_test_weak_frontier_fault_armed, &expect, 0,
		  LA_ACQ_REL, LA_ACQ))
      return 0;
  }
  (void)la_add32_rlx(&gc2_test_weak_frontier_fault_hits, 1);
  return 1;
}

static void gc2_recovery_test_pause_at(uint32_t stage)
{
  if (la_load32_acq(&gc2_recovery_test_pause_stage) != stage)
    return;
  la_store32_rel(&gc2_recovery_test_paused_stage, stage);
  while (la_load32_acq(&gc2_recovery_test_pause_release) == 0)
    la_cpu_pause();
  la_store32_rel(&gc2_recovery_test_paused_stage, 0);
  la_store32_rel(&gc2_recovery_test_pause_stage, 0);
}

static void gc2_thread_needscan_test_pause_at(uint32_t stage)
{
  if (la_load32_acq(&gc2_thread_needscan_test_pause_stage) != stage)
    return;
  la_store32_rel(&gc2_thread_needscan_test_paused_stage, stage);
  while (la_load32_acq(&gc2_thread_needscan_test_pause_release) == 0)
    la_cpu_pause();
  la_store32_rel(&gc2_thread_needscan_test_paused_stage, 0);
  la_store32_rel(&gc2_thread_needscan_test_pause_stage, 0);
}

static void gc2_table_rescan_test_pause_at(uint32_t stage)
{
  uint32_t expect = stage;
  if (!la_cas32(&gc2_table_rescan_test_pause_stage, &expect, 0,
		LA_ACQ_REL, LA_ACQ))
    return;
  la_store32_rel(&gc2_table_rescan_test_paused_stage, stage);
  while (la_load32_acq(&gc2_table_rescan_test_pause_release) == 0)
    la_cpu_pause();
  la_store32_rel(&gc2_table_rescan_test_paused_stage, 0);
}

static void gc2_queue_post_admit_test_pause_at(GCobj *o)
{
  uint32_t expect = 1;
  if ((GCobj *)(void *)la_loaduptr_acq(
	&gc2_queue_post_admit_test_target) != o ||
      !la_cas32(&gc2_queue_post_admit_test_armed, &expect, 0,
		LA_ACQ_REL, LA_ACQ))
    return;
  la_store32_rel(&gc2_queue_post_admit_test_paused, 1);
  while (la_load32_acq(&gc2_queue_post_admit_test_release) == 0)
    la_cpu_pause();
  la_store32_rel(&gc2_queue_post_admit_test_paused, 0);
  la_storeuptr_rel(&gc2_queue_post_admit_test_target, 0);
}

static void gc2_queue_retry_witness_test_pause_at(GCobj *o)
{
  uint32_t expect = 1;
  if ((GCobj *)(void *)la_loaduptr_acq(
	&gc2_queue_retry_witness_test_target) != o ||
      !la_cas32(&gc2_queue_retry_witness_test_armed, &expect, 0,
		LA_ACQ_REL, LA_ACQ))
    return;
  la_store32_rel(&gc2_queue_retry_witness_test_paused, 1);
  while (la_load32_acq(&gc2_queue_retry_witness_test_release) == 0)
    la_cpu_pause();
  la_store32_rel(&gc2_queue_retry_witness_test_paused, 0);
  la_storeuptr_rel(&gc2_queue_retry_witness_test_target, 0);
}

void lj_gc2_test_thread_needscan_pause(uint32_t stage)
{
  la_store32_rel(&gc2_thread_needscan_test_pause_release, 0);
  la_store32_rel(&gc2_thread_needscan_test_paused_stage, 0);
  la_store32_rel(&gc2_thread_needscan_test_pause_stage, stage);
}

uint32_t lj_gc2_test_thread_needscan_paused(void)
{
  return la_load32_acq(&gc2_thread_needscan_test_paused_stage);
}

void lj_gc2_test_thread_needscan_release(void)
{
  la_store32_rel(&gc2_thread_needscan_test_pause_release, 1);
}

void lj_gc2_test_table_rescan_pause(uint32_t stage)
{
  la_store32_rel(&gc2_table_rescan_test_pause_release, 0);
  la_store32_rel(&gc2_table_rescan_test_paused_stage, 0);
  la_store32_rel(&gc2_table_rescan_test_pause_stage, stage);
}

uint32_t lj_gc2_test_table_rescan_paused(void)
{
  return la_load32_acq(&gc2_table_rescan_test_paused_stage);
}

void lj_gc2_test_table_rescan_release(void)
{
  la_store32_rel(&gc2_table_rescan_test_pause_release, 1);
}

void lj_gc2_test_queue_post_admit_pause(GCobj *target)
{
  la_store32_rel(&gc2_queue_post_admit_test_release, 0);
  la_store32_rel(&gc2_queue_post_admit_test_paused, 0);
  la_storeuptr_rel(&gc2_queue_post_admit_test_target,
		   (uintptr_t)(void *)target);
  la_store32_rel(&gc2_queue_post_admit_test_armed, target != NULL);
}

uint32_t lj_gc2_test_queue_post_admit_paused(void)
{
  return la_load32_acq(&gc2_queue_post_admit_test_paused);
}

void lj_gc2_test_queue_post_admit_release(void)
{
  la_store32_rel(&gc2_queue_post_admit_test_release, 1);
}

void lj_gc2_test_queue_retry_witness_pause(GCobj *target)
{
  la_store32_rel(&gc2_queue_retry_witness_test_release, 0);
  la_store32_rel(&gc2_queue_retry_witness_test_paused, 0);
  la_storeuptr_rel(&gc2_queue_retry_witness_test_target,
		   (uintptr_t)(void *)target);
  la_store32_rel(&gc2_queue_retry_witness_test_armed, target != NULL);
}

uint32_t lj_gc2_test_queue_retry_witness_paused(void)
{
  return la_load32_acq(&gc2_queue_retry_witness_test_paused);
}

void lj_gc2_test_queue_retry_witness_release(void)
{
  la_store32_rel(&gc2_queue_retry_witness_test_release, 1);
}

void lj_gc2_test_recovery_fail_grey_grow(uint32_t count)
{
  la_store32_rel(&gc2_recovery_test_fail_grey_grow, count);
}

void lj_gc2_test_recovery_pause(uint32_t stage)
{
  la_store32_rel(&gc2_recovery_test_pause_release, 0);
  la_store32_rel(&gc2_recovery_test_paused_stage, 0);
  la_store32_rel(&gc2_recovery_test_pause_stage, stage);
}

uint32_t lj_gc2_test_recovery_paused(void)
{
  return la_load32_acq(&gc2_recovery_test_paused_stage);
}

void lj_gc2_test_recovery_pause_disarm(void)
{
  /* Prevent another caller from entering the selected hook without releasing
  ** the caller which has already published paused_stage. */
  la_store32_rel(&gc2_recovery_test_pause_stage, 0);
}

void lj_gc2_test_recovery_release(void)
{
  la_store32_rel(&gc2_recovery_test_pause_release, 1);
}

void lj_gc2_test_recovery_huge_scans_reset(void)
{
  la_store32_rel(&gc2_recovery_test_huge_scans, 0);
}

uint32_t lj_gc2_test_recovery_huge_scans(void)
{
  return la_load32_acq(&gc2_recovery_test_huge_scans);
}

void lj_gc2_test_worker_table_skips_reset(void)
{
  la_store32_rel(&gc2_recovery_test_worker_table_skips, 0);
}

uint32_t lj_gc2_test_worker_table_skips(void)
{
  return la_load32_acq(&gc2_recovery_test_worker_table_skips);
}

#else
#define LJ_GC2_RECOVERY_TEST_RESERVED 1u
#define LJ_GC2_RECOVERY_TEST_PRE_COMPLETE 2u
#define LJ_GC2_RECOVERY_TEST_SSB_COMMITTED 3u
#define LJ_GC2_RECOVERY_TEST_PRE_LIFETIME_RESTORE 4u
#define LJ_GC2_RECOVERY_TEST_POST_CLAIM 5u
#define LJ_GC2_RECOVERY_TEST_SMALL_IDLE_SAMPLED 6u
#define LJ_GC2_THREAD_NEEDSCAN_TEST_BEFORE_SET 1u
#define LJ_GC2_THREAD_NEEDSCAN_TEST_AFTER_SET 2u
#define LJ_GC2_THREAD_NEEDSCAN_TEST_INSTALLING 3u
#define LJ_GC2_TABLE_RESCAN_TEST_INSTALLING 1u
#define LJ_GC2_TABLE_RESCAN_TEST_HINT_CLEARED 2u
#define gc2_recovery_test_pause_at(stage) ((void)(stage))
#define gc2_thread_needscan_test_pause_at(stage) ((void)(stage))
#define gc2_table_rescan_test_pause_at(stage) ((void)(stage))
#define gc2_queue_post_admit_test_pause_at(o) ((void)(o))
#define gc2_queue_retry_witness_test_pause_at(o) ((void)(o))
#define gc2_test_jit_mark_checkpoint_closed() ((void)0)
#define gc2_test_jit_sweep_checkpoint_closed() ((void)0)
#define gc2_root_test_take_semantic_retry(o) (0)
#endif

#if defined(LJ_GC2_TEST_HELPERS) || defined(LJ_TRACE_TEST_HELPERS)
static uint32_t gc2_idle_reclaim_test_pause;
static uint32_t gc2_idle_reclaim_test_paused;
static uint32_t gc2_idle_reclaim_test_release;

static void gc2_idle_reclaim_test_pause_after_jit_quiescence(void)
{
  if (la_load32_acq(&gc2_idle_reclaim_test_pause) == 0)
    return;
  la_store32_rel(&gc2_idle_reclaim_test_paused, 1);
  while (la_load32_acq(&gc2_idle_reclaim_test_release) == 0)
    la_cpu_pause();
  la_store32_rel(&gc2_idle_reclaim_test_paused, 0);
  la_store32_rel(&gc2_idle_reclaim_test_pause, 0);
}

void lj_gc2_test_idle_reclaim_pause_after_jit_quiescence(void)
{
  la_store32_rel(&gc2_idle_reclaim_test_release, 0);
  la_store32_rel(&gc2_idle_reclaim_test_paused, 0);
  la_store32_rel(&gc2_idle_reclaim_test_pause, 1);
}

uint32_t lj_gc2_test_idle_reclaim_paused(void)
{
  return la_load32_acq(&gc2_idle_reclaim_test_paused);
}

void lj_gc2_test_idle_reclaim_release(void)
{
  la_store32_rel(&gc2_idle_reclaim_test_release, 1);
}
#else
#define gc2_idle_reclaim_test_pause_after_jit_quiescence() ((void)0)
#endif

typedef struct GC2FinalizerNode {
  struct GC2FinalizerNode *next;
  GCobj *obj;
  TValue fin;
  uint32_t has_fin;
} GC2FinalizerNode;

struct GC2WeakOverflow {
  GC2WeakOverflow *next;
  GCtab *tab;
};

typedef struct GC2FinalizerDispatchCtx {
  GC2FinalizerDispatchFunc dispatch;
} GC2FinalizerDispatchCtx;

/*
** The typed activation word is a migration-time veto, never a reclaim grant.
** Legacy phase/worker/SMR predicates remain the sole positive authority until
** every root writer participates in the exact root gate.
*/
static int gc2_activation_state_for_phase(uint32_t phase, uint8_t *statep)
{
  uint8_t state;
  switch (phase) {
  case LJ_GC2_IDLE: state = LJ_GC2_ACT_IDLE; break;
  case LJ_GC2_MARK: state = LJ_GC2_ACT_MARK; break;
  case LJ_GC2_WEAK: state = LJ_GC2_ACT_WEAK; break;
  case LJ_GC2_SWEEP: state = LJ_GC2_ACT_SWEEP_OPEN; break;
  default: return 0;
  }
  if (statep)
    *statep = state;
  return 1;
}

/* Fault-only sticky pin. A failing actor never waits for a peer, but retries
** its exact CX16 snapshot while racing phase actors make system-wide progress. */
static void gc2_activation_pin_no_reclaim(global_State *g)
{
  LJGC2Activation *token = &g->gc2.activation;
  for (;;) {
    LJGC2ActivationSnap snap = lj_gc2_activation_snapshot(token);
    LJGC2ActivationSnap observed;
    LJGC2TransitionResult result;
    if (snap.state == LJ_GC2_ACT_NO_RECLAIM &&
        snap.gate == LJ_GC2_ROOT_GATE_OPEN)
      return;
    if (lj_gc2_activation_value_valid(snap.mark_epoch, snap.generation,
                                       snap.state, snap.gate)) {
      result = lj_gc2_activation_try_transition(token, &snap,
        snap.mark_epoch, LJ_GC2_ACT_NO_RECLAIM, &observed);
      if (result == LJ_GC2_TRANSITION_OK ||
          result == LJ_GC2_TRANSITION_PINNED)
        return;
      if (result == LJ_GC2_TRANSITION_LOST)
        continue;
    }
    /* Normalize a malformed but stably decoded authority. The exact CAS cannot
    ** erase a racing valid transition, and NO_RECLAIM accepts epoch zero. */
    {
      la_u128 expected, desired;
      uint64_t generation = snap.generation == LJ_GC2_ACT_MAX_GENERATION ?
                            snap.generation : snap.generation + 1u;
      expected.lo = snap.mark_epoch;
      expected.hi = lj_gc2_activation_pack_hi(snap.generation,
                                               snap.state, snap.gate);
      desired.lo = snap.mark_epoch;
      desired.hi = lj_gc2_activation_pack_hi(generation,
        LJ_GC2_ACT_NO_RECLAIM, LJ_GC2_ROOT_GATE_OPEN);
      if (la_cas128(&token->value, &expected, desired))
        return;
    }
  }
}

static int gc2_activation_matches_legacy(const LJGC2ActivationSnap *snap,
                                          uint32_t phase)
{
  uint8_t state;
  return gc2_activation_state_for_phase(phase, &state) &&
         lj_gc2_activation_value_valid(snap->mark_epoch, snap->generation,
                                        snap->state, snap->gate) &&
         snap->state == state && snap->gate == LJ_GC2_ROOT_GATE_OPEN;
}

/* A delayed forward mirror may observe a preserve abort after the legacy
** phase reached IDLE.  Typed IDLE proves the abort completed.  Otherwise only
** the exact GCSCAN owner proves that an active typed source is still waiting
** for its reset.  Active/IDLE with no owner is an orphan and must fail closed.
*/
static int gc2_activation_forward_abort_defer(global_State *g)
{
  LJGC2ActivationSnap current;
  uint32_t before, leader, after;
  before = gc2_phase_acq(g);
  leader = gc2_cycle_leader_acq(g);
  after = gc2_phase_acq(g);
  if (before != LJ_GC2_IDLE || after != LJ_GC2_IDLE)
    return 0;
  if (leader == LJ_THREAD_GCSCAN)
    return 1;
  /* The caller's source snapshot may predate a completed abort and sentinel
  ** release. Resample the typed word only after seeing stable legacy IDLE. */
  current = lj_gc2_activation_snapshot(&g->gc2.activation);
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    return 0;
  return lj_gc2_activation_value_valid(current.mark_epoch,
                                        current.generation, current.state,
                                        current.gate) &&
         current.state == LJ_GC2_ACT_IDLE &&
         current.gate == LJ_GC2_ROOT_GATE_OPEN;
}

static int gc2_activation_stage_mark(global_State *g,
                                      LJGC2ActivationSnap *staged)
{
  LJGC2Activation *token = &g->gc2.activation;
  LJGC2ActivationSnap snap = lj_gc2_activation_snapshot(token);
  LJGC2ActivationSnap observed;
  LJGC2TransitionResult result;
  uint64_t epoch;
  if (!gc2_activation_matches_legacy(&snap, LJ_GC2_IDLE)) {
    gc2_activation_pin_no_reclaim(g);
    return 0;
  }
  epoch = snap.mark_epoch == UINT64_MAX ? snap.mark_epoch :
                                           snap.mark_epoch + 1u;
  result = lj_gc2_activation_try_transition(token, &snap, epoch,
                                             LJ_GC2_ACT_MARK, &observed);
  if (result == LJ_GC2_TRANSITION_OK) {
    if (staged)
      *staged = observed;
    return 1;
  }
  if (result != LJ_GC2_TRANSITION_PINNED)
    gc2_activation_pin_no_reclaim(g);
  return 0;
}

static void gc2_activation_mirror_edge(global_State *g, uint32_t from_phase,
                                        uint32_t to_phase)
{
  LJGC2Activation *token = &g->gc2.activation;
  LJGC2ActivationSnap snap = lj_gc2_activation_snapshot(token);
  LJGC2ActivationSnap observed;
  LJGC2TransitionResult result = LJ_GC2_TRANSITION_INVALID;
  uint8_t to_state;
  uint64_t epoch;
  int forward;
  if (!gc2_activation_state_for_phase(to_phase, &to_state)) {
    gc2_activation_pin_no_reclaim(g);
    return;
  }
  /* An xchg which returned its target did not own a semantic phase edge: a
  ** prior closer can still be between its legacy xchg and typed reset. Such an
  ** observer leaves the mismatch as a temporary veto instead of spuriously
  ** making ordinary close contention permanently NO_RECLAIM. An actor which
  ** did change the legacy phase must still prove its exact typed source; an
  ** already-target token there could hide a genuinely missing mirror. */
  if (from_phase == to_phase)
    return;
  if (from_phase == LJ_GC2_IDLE && to_phase == LJ_GC2_MARK) {
    (void)gc2_activation_stage_mark(g, NULL);
    return;
  }
  forward = from_phase != LJ_GC2_IDLE && to_phase != LJ_GC2_IDLE;
  if (forward) {
    uint32_t legacy = gc2_phase_acq(g);
    uint32_t expected_legacy =
      from_phase == LJ_GC2_MARK && to_phase == LJ_GC2_WEAK ?
      to_phase : from_phase;
    if (legacy == LJ_GC2_IDLE) {
      if (gc2_activation_forward_abort_defer(g))
        return;
      gc2_activation_pin_no_reclaim(g);
      return;
    }
    if (legacy != expected_legacy) {
      gc2_activation_pin_no_reclaim(g);
      return;
    }
  }
  if (!gc2_activation_matches_legacy(&snap, from_phase)) {
    if (forward && gc2_activation_forward_abort_defer(g))
      return;
    gc2_activation_pin_no_reclaim(g);
    return;
  }
  epoch = snap.mark_epoch;
  if (from_phase == LJ_GC2_SWEEP && to_phase == LJ_GC2_IDLE) {
    result = lj_gc2_activation_try_abandon_sweep_open(token, &snap,
                                                       &observed);
  } else {
    result = lj_gc2_activation_try_transition(token, &snap, epoch, to_state,
                                               &observed);
  }
  if (result == LJ_GC2_TRANSITION_LOST && forward &&
      gc2_activation_forward_abort_defer(g))
    return;
  if (result != LJ_GC2_TRANSITION_OK &&
      result != LJ_GC2_TRANSITION_PINNED)
    gc2_activation_pin_no_reclaim(g);
}

/* Collapse a defensive preserve/forced-close skew while the caller owns the
** exact GCSCAN sentinel.  The runtime does not publish PREP/CLOSING/COMMIT yet;
** seeing one (or a non-OPEN gate) is therefore a real authority mismatch.
*/
static void gc2_activation_abort_reset(global_State *g,
                                        uint32_t legacy_source)
{
  LJGC2Activation *token = &g->gc2.activation;
  for (;;) {
    LJGC2ActivationSnap snap = lj_gc2_activation_snapshot(token);
    LJGC2ActivationSnap observed;
    LJGC2TransitionResult result;
    if (!lj_gc2_activation_value_valid(snap.mark_epoch, snap.generation,
                                        snap.state, snap.gate) ||
        snap.gate != LJ_GC2_ROOT_GATE_OPEN) {
      gc2_activation_pin_no_reclaim(g);
      return;
    }
    if (snap.state == LJ_GC2_ACT_NO_RECLAIM)
      return;
    if (snap.state == LJ_GC2_ACT_IDLE) {
      if (legacy_source != LJ_GC2_IDLE)
        gc2_activation_pin_no_reclaim(g);
      return;
    }
    /* Exact phase-gate ownership rules out an already-completed competing
    ** reset. Accept only the two explicitly modeled WEAK transition skews;
    ** every other active-source disagreement is a missing mirror. */
    if (!((legacy_source == LJ_GC2_MARK &&
           snap.state == LJ_GC2_ACT_MARK) ||
          (legacy_source == LJ_GC2_WEAK &&
           (snap.state == LJ_GC2_ACT_MARK ||
            snap.state == LJ_GC2_ACT_WEAK ||
            snap.state == LJ_GC2_ACT_SWEEP_OPEN)) ||
          (legacy_source == LJ_GC2_SWEEP &&
           snap.state == LJ_GC2_ACT_SWEEP_OPEN))) {
      gc2_activation_pin_no_reclaim(g);
      return;
    }
    if (snap.state == LJ_GC2_ACT_SWEEP_OPEN) {
      result = lj_gc2_activation_try_abandon_sweep_open(token, &snap,
                                                         &observed);
    } else if (snap.state == LJ_GC2_ACT_MARK ||
               snap.state == LJ_GC2_ACT_WEAK) {
      result = lj_gc2_activation_try_transition(token, &snap, snap.mark_epoch,
                                                 LJ_GC2_ACT_IDLE, &observed);
    } else {
      gc2_activation_pin_no_reclaim(g);
      return;
    }
    if (result == LJ_GC2_TRANSITION_OK ||
        result == LJ_GC2_TRANSITION_PINNED)
      return;
    if (result != LJ_GC2_TRANSITION_LOST) {
      gc2_activation_pin_no_reclaim(g);
      return;
    }
  }
}

/* Revalidate the staged IDLE->MARK edge before any irreversible cycle-start
** side effect.  An exact rollback never touches a replacement cycle leader.
*/
static int gc2_activation_mark_recheck(
  global_State *g, uint32_t expected_leader,
  const LJGC2ActivationSnap *staged)
{
  LJGC2ActivationSnap current;
  LJGC2ActivationSnap observed;
  LJGC2TransitionResult result;
  uint32_t phase = gc2_phase_acq(g);
  uint32_t leader = gc2_cycle_leader_acq(g);
  if (phase == LJ_GC2_IDLE && leader == expected_leader) {
    if (!staged)
      return 1;  /* Sticky NO_RECLAIM remains a veto-only migration state. */
    current = lj_gc2_activation_snapshot(&g->gc2.activation);
    if (lj_gc2_activation_equal(&current, staged) &&
        gc2_phase_acq(g) == LJ_GC2_IDLE &&
        gc2_cycle_leader_acq(g) == expected_leader)
      return 1;
    phase = gc2_phase_acq(g);
  }
  if (!staged)
    return 0;  /* The sticky activation veto already records the mismatch. */
  if (phase != LJ_GC2_IDLE) {
    gc2_activation_pin_no_reclaim(g);
    return 0;
  }
  result = lj_gc2_activation_try_transition(&g->gc2.activation, staged,
                                             staged->mark_epoch,
                                             LJ_GC2_ACT_IDLE, &observed);
  if (result == LJ_GC2_TRANSITION_OK ||
      result == LJ_GC2_TRANSITION_PINNED)
    return 0;
  if (result == LJ_GC2_TRANSITION_LOST &&
      gc2_activation_forward_abort_defer(g))
    return 0;
  gc2_activation_pin_no_reclaim(g);
  return 0;
}

/* GCSCAN is an exact phase-edge gate. Forward actors hold it through all
** post-CAS initialization; close actors hold it through typed reset and idle
** publication. A cycle request is never overwritten in either direction. */
static int gc2_phase_gate_try(global_State *g)
{
  uint32_t expect = 0;
  return gc2_cycle_leader_cas(g, &expect, LJ_THREAD_GCSCAN);
}

static void gc2_phase_gate_release(global_State *g)
{
  uint32_t expect = LJ_THREAD_GCSCAN;
  if (LJ_UNLIKELY(!gc2_cycle_leader_cas(g, &expect, 0))) {
    /* Never overwrite a replacement request.  A stolen sentinel is a protocol
    ** fault, so retain storage through the typed sticky veto. */
    gc2_activation_pin_no_reclaim(g);
  }
}

int lj_gc2_activation_reclaim_veto(global_State *g)
{
  LJGC2ActivationSnap snap;
  uint32_t before, after;
  if (!g)
    return 1;
  /* A disagreement can be the short interval between two conservative mirror
  ** stores. Veto it, but let the exact phase actor decide whether it is a real
  ** mismatch that must become sticky NO_RECLAIM. */
  before = gc2_phase_acq(g);
  snap = lj_gc2_activation_snapshot(&g->gc2.activation);
  after = gc2_phase_acq(g);
  return before != after || !gc2_activation_matches_legacy(&snap, after);
}

#if defined(lj_gc2_c) || defined(LJ_GC2_TEST_HELPERS) || defined(LUA_USE_ASSERT)
void lj_gc2_test_activation_mirror_edge(global_State *g, uint32_t from_phase,
                                         uint32_t to_phase)
{
  if (g)
    gc2_activation_mirror_edge(g, from_phase, to_phase);
}

int lj_gc2_test_activation_mark_recheck(global_State *g,
                                         uint32_t expected_leader)
{
  LJGC2ActivationSnap staged;
  if (!g)
    return 0;
  staged = lj_gc2_activation_snapshot(&g->gc2.activation);
  return gc2_activation_mark_recheck(g, expected_leader, &staged);
}
#endif

static GC2FinalizerNode *gc2_finalizer_node_next_acq(GC2FinalizerNode *fn)
{
  return (GC2FinalizerNode *)la_loadptr_acq((void *const *)&fn->next);
}

static void gc2_finalizer_node_next_rel(GC2FinalizerNode *fn,
					GC2FinalizerNode *next)
{
  la_storeptr_rel((void **)&fn->next, next);
}

static GCobj *gc2_finalizer_node_obj_acq(GC2FinalizerNode *fn)
{
  return (GCobj *)la_loadptr_acq((void *const *)&fn->obj);
}

static void gc2_finalizer_node_obj_rel(GC2FinalizerNode *fn, GCobj *o)
{
  la_storeptr_rel((void **)&fn->obj, o);
}

static int gc2_finalizer_node_fin_acq(GC2FinalizerNode *fn, TValue *fin)
{
  if (!fn || !fin || la_load32_acq(&fn->has_fin) == 0)
    return 0;
  lj_tv_load_acq(fin, &fn->fin);
  return 1;
}

static void gc2_finalizer_node_fin_rel(GC2FinalizerNode *fn, cTValue *fin)
{
  if (!fn)
    return;
  if (fin) {
    fn->fin = *fin;
    la_store32_rel(&fn->has_fin, 1);
  } else {
    setnilV(&fn->fin);
    la_store32_rel(&fn->has_fin, 0);
  }
}

static GC2WeakOverflow *gc2_weak_overflow_next_acq(GC2WeakOverflow *node)
{
  return (GC2WeakOverflow *)la_loadptr_acq((void *const *)&node->next);
}

static void gc2_weak_overflow_next_rel(GC2WeakOverflow *node,
				       GC2WeakOverflow *next)
{
  la_storeptr_rel((void **)&node->next, next);
}

static GCtab *gc2_weak_overflow_tab_acq(GC2WeakOverflow *node)
{
  return (GCtab *)la_loadptr_acq((void *const *)&node->tab);
}

static void gc2_weak_overflow_tab_rel(GC2WeakOverflow *node, GCtab *tab)
{
  la_storeptr_rel((void **)&node->tab, tab);
}

static int gc2_grey_grow(global_State *g);
static int gc2_grey_empty(global_State *g);
static int gc2_weak_resize(global_State *g, MSize cap);
static void gc2_weak_overflow_free(global_State *g, GC2WeakOverflow *node);
static void gc2_weak_reset(global_State *g);
static int gc2_finclaim_ensure(global_State *g);
static void gc2_finclaim_reset(global_State *g);
static void gc2_finalizer_free_stack(global_State *g, GC2FinalizerNode *node);
static void gc2_finalizer_free_ring(global_State *g, GC2FinalizerNode *tail);
static void lj_gc2_finalizer_enqueue(global_State *g, GCobj *o);
static void lj_gc2_finalizer_drain_owned(global_State *g);
static void lj_gc2_finalizer_drain_l(lua_State *L, global_State *g);
static void lj_gc2_finalizer_drain(global_State *g);
static GC2FinalizerNode *gc2_finalizer_dequeue_node_owned(global_State *g,
						   TValue *fin,
						   int *has_fin);
static GCobj *lj_gc2_finalizer_dequeue_owned(global_State *g,
					     TValue *fin, int *has_fin);
static GCobj *lj_gc2_finalizer_dequeue(global_State *g);
static int lj_gc2_finalizer_try_enter(global_State *g);
int lj_gc2_finalizer_owned_by_current(global_State *g);
static int gc2_finalizer_queue_pending(global_State *g);
static int gc2_finalizer_sweep_pending(global_State *g);
static void gc2_peer_wait_no_l(void);
static void gc2_peer_wait_l(lua_State *L);
static void gc2_peer_wait_owned_l(lua_State *L);
static void gc2_root_scan_retry(global_State *g);
static int gc2_mark_begin(global_State *g);
static void lj_gc2_finalizer_enter_l(lua_State *L, global_State *g);
static void lj_gc2_finalizer_enter(global_State *g);
static void lj_gc2_finalizer_leave(global_State *g);
static int lj_gc2_finalizer_pending(global_State *g);
static int gc2_tab_weak_mode(global_State *g, GCtab *t, GCtab *mt,
			     int mark_mode, int smr_held,
			     int semantic_admission, int *retryp);
static void *gc2_worker_main(void *arg);
static uint32_t gc2_worker_drain_logical(global_State *g, TGState *tg,
					 uint32_t limit, int hold_mark_gate);
typedef struct GC2MarkScope GC2MarkScope;
static int gc2_mark_thread_root_obj_status(global_State *g, GCobj *o);
static void gc2_mark_thread_root_obj(global_State *g, GCobj *o);
static void gc2_mark_thread_root_obj_worker(global_State *g, GCobj *o);
static void gc2_mark_tv_worker(global_State *g, cTValue *tv);
static void gc2_mark_thread_root_tv_worker(global_State *g, cTValue *tv);
static int gc2_mark_payload_obj_worker(global_State *g, GCobj *o);
static int gc2_valid_proto_for_traverse_held(GCproto *pt);
static void gc2_traverse_proto(global_State *g, GCproto *pt, int force);
static int lj_gc2_ssb_push(global_State *g, GCobj *o);
static uint32_t gc2_flush_ssb(global_State *g, TGState *tg, int allow_drain);
static uint32_t gc2_drain_ssb_owned(global_State *g);
static uint32_t lj_gc2_drain_ssb(global_State *g);
static int lj_gc2_ssb_empty(global_State *g);
static int gc2_ssb_published_empty(global_State *g);
static int gc2_ssb_detached_empty(global_State *g);
static int gc2_weak_owned_peer_active(global_State *g);
static LJ_NOINLINE uint32_t gc2_drain_published_ssb_to_grey(global_State *g,
							    uint32_t limit);
static uint32_t gc2_drain_active_ssb_to_grey(global_State *g, TGState *tg,
					     uint32_t limit);
static uint32_t gc2_drain_grey(global_State *g, uint32_t limit);
static uint32_t gc2_mark_drain_owned_bounded(global_State *g,
					      uint32_t limit);
static void gc2_worker_reclaim_retired_tgs(global_State *g);
static LJ_AINLINE void gc2_preserve_direct_bodies(global_State *g, GCobj *o);
static void gc2_traverse_func(global_State *g, GCfunc *fn);
static int gc2_valid_thread_for_traverse_held(global_State *g,
					       lua_State *th,
					       const GC2MarkScope *held);
static void gc2_traverse_thread(global_State *g, lua_State *th,
				const GC2MarkScope *held);
void lj_gc2_trace_sweep_roots(global_State *g);
uint32_t lj_gc2_trace_sweep_root(global_State *g, GCobj *o);
static uint32_t gc2_trace_sweep_worker_edge(global_State *g, GCobj *o);
static uint32_t gc2_trace_sweep_tv_edge(global_State *g, cTValue *tv,
					 int worker_edge);
static int lj_gc2_finreg_cdata_preclaim(lua_State *L, global_State *g,
					GCobj *o, cTValue *fin);
static int lj_gc2_finreg_cdata_preclaim_take(lua_State *L, global_State *g,
					     GCobj *o, TValue *fin);
static int gc2_call_finalizer(global_State *g, lua_State *L,
			      cTValue *mo, GCobj *o);
static int lj_gc2_finreg_cdata_dispatch(lua_State *L, global_State *g,
					GCobj *o, cTValue *fin);
static int lj_gc2_finreg_udata_dispatch(lua_State *L, global_State *g,
					GCobj *o, cTValue *fin);
static LJ_AINLINE int gc2_finreg_udata_obj_valid(global_State *g, GCobj *o);
static uint32_t gc2_clear_container_needscan_all(global_State *g);
static uint32_t gc2_clear_uncounted_needscan_all(global_State *g);
typedef struct GC2FrameScope GC2FrameScope;
static int gc2_frame_func_valid(global_State *g, TValue *frame,
				GCfunc **fnp, GCproto **ptp,
				GC2FrameScope *scope);
static int gc2_traverse_obj(global_State *g, GCobj *o);
static int lj_gc2_ssb_empty(global_State *g);
static int gc2_traverse_tab(global_State *g, GCtab *t);
static int gc2_traverse_tab_admitted(global_State *g, GCtab *t,
				      int record_weak);
static LJ_AINLINE int gc2_table_scan_current(global_State *g, GCtab *t);
static int gc2_table_rescan_later(global_State *g, GCtab *t);
static int gc2_table_rescan_later_force(global_State *g, GCtab *t);
static void gc2_table_rescan_requeue(global_State *g, GCtab *t);
static void gc2_table_rescan_requeue_held(global_State *g, GCtab *t);
static LJ_AINLINE int gc2_gct_may_traverse(uint32_t gct);
static LJ_AINLINE int gc2_obj_may_traverse(GCobj *o);
enum {
  GC2_MARK_DEAD = -1,
  GC2_MARK_LIVE_ALREADY = 0,
  GC2_MARK_NEW = 1
};
/* Distinguish a counted HugeTab reader from arena rescue admission values.
** Existing small-scope initialization remains a two-field fast path; stale
** huge bytes are ignored unless this exact tag is present. */
#define GC2_SCOPE_HUGE_READER	(-0x474332)
struct GC2MarkScope {
  GCArena *a;
  int admission;
  LJHugeReader huge;
};
static LJ_AINLINE void gc2_mark_scope_init(GC2MarkScope *scope);
static LJ_AINLINE int gc2_mark_admission_counted(int admission);
static int gc2_markobj_base_valid_scoped(global_State *g, GCobj *o,
					 void **basep, uint32_t *gctp,
					 GC2MarkScope *scope);
struct GC2FrameScope {
  GC2MarkScope fn;
  GC2MarkScope pt;
  GC2MarkScope pc;
};
static void gc2_mark_scope_leave(GC2MarkScope *scope);
static void gc2_frame_scope_leave(GC2FrameScope *scope);
static int gc2_markmem_registered_scoped_status(global_State *g, void *p,
							 GC2MarkScope *holdp);
static int gc2_markobj_preserve_scoped_status(global_State *g, GCobj *o,
					       uint32_t *gctp,
					       GC2MarkScope *scope,
					       int *retryp);
static int gc2_markobj_expected_scoped_status(global_State *g, GCobj *o,
					       uint32_t expected_gct,
					       uint32_t *gctp,
					       GC2MarkScope *scope);
static int gc2_markobj_expected_scoped_status_mode(global_State *g,
						    GCobj *o,
						    uint32_t expected_gct,
						    uint32_t *gctp,
						    GC2MarkScope *scope,
						    int semantic_publish);
static int gc2_markobj_preserve_status(global_State *g, GCobj *o,
				       void **basep, uint32_t *gctp,
				       int *traversablep);
static int gc2_markobj_preserve_queue_status(global_State *g, GCobj *o,
					       uint32_t *gctp,
					       int *traversablep,
					       GC2MarkScope *scope,
					       int *retryp);
static int gc2_markobj_preserve_expected_status(global_State *g, GCobj *o,
						 uint32_t expected_gct);
static int gc2_markobj_expected_status(global_State *g, GCobj *o,
					uint32_t expected_gct,
					uint32_t *gctp);
static int gc2_retain_candidate_status(global_State *g, GCobj *o,
					void **basep, uint32_t *gctp,
					int *traversablep,
					uint32_t expected_gct,
					GC2MarkScope *holdp,
					int *retryp);
static int gc2_small_arena_known(global_State *g, GCArena *a);
static int gc2_small_lifetime_nearest(GCArena *a, uint32_t cell,
					      uint32_t *startp,
					      uint32_t *lifep);
static int gc2_small_lifetime_containing_start(GCArena *a, uint32_t cell,
						uint32_t *startp);
static int gc2_small_containing_admit(global_State *g, GCArena *a,
				      uint32_t cell,
				      void **basep, uint32_t *startp,
				      GC2MarkScope *scope);
static int gc2_small_containing_start(GCArena *a, uint32_t cell,
				      uint32_t *startp);
static int gc2_small_candidate_admit(global_State *g, GCobj *o, GCArena *a,
				     uint32_t expected_gct,
				     void **basep, uint32_t *startp,
				     uint32_t *gctp, GC2MarkScope *scope);
static int gc2_small_candidate_transition_retry_held(GCArena *a,
						      uint32_t start,
						      uint32_t life);
static int gc2_mark_small_cell_admitted(global_State *g, GCArena *a,
					uint32_t cell, int admission,
					int *retryp);
static int gc2_frame_pc_valid_scoped(global_State *g, const BCIns *pc,
				      GC2MarkScope *scope);
static LJ_AINLINE void gc2_preserve_direct_bodies(global_State *g, GCobj *o);
static int gc2_retained_candidate_valid(global_State *g, GCobj *o,
					 void *base, GCArena *a,
					 uint32_t start, size_t alloc_size,
					 int interior_tag, uint32_t *gctp);
static int gc2_markobj_worker_status(global_State *g, GCobj *o,
				     uint32_t *gctp);
static int gc2_markobj_direct_status(global_State *g, GCobj *o);
static LJ_AINLINE int gc2_reclaim_tls_active(global_State *g);
static void gc2_traverse_udata(global_State *g, GCudata *ud);
static LJ_AINLINE int gc2_rescan_pending_set(GCobj *o);
static LJ_AINLINE uint8_t gc2_rescan_pending_clear(GCobj *o);
static int gc2_grey_push(global_State *g, GCobj *o);
static int gc2_recovery_publish_scoped(global_State *g, GCobj *o,
				       const GC2MarkScope *scope);
static int gc2_recovery_publish(global_State *g, GCobj *o);
static LJ_AINLINE int gc2_recovery_work_pending(global_State *g);
static LJ_AINLINE int gc2_recovery_failed_veto(global_State *g);
static LJ_AINLINE int gc2_recovery_stalled_failed(global_State *g);
static LJ_AINLINE int gc2_recovery_empty(global_State *g);
static void gc2_recovery_fail_closed(global_State *g);
static int gc2_publish_mutator_scoped(global_State *g, GCobj *o,
				      const GC2MarkScope *scope);
static int gc2_publish_mutator(global_State *g, GCobj *o);
static int gc2_publish_mutator_nodrain(global_State *g, GCobj *o);
static int gc2_publish_worker(global_State *g, GCobj *o);
static uint32_t gc2_recovery_drain_owned(global_State *g, uint32_t limit);
static uint32_t gc2_recovery_discard_terminal(global_State *g);
static int gc2_observed_obj_valid_scoped(global_State *g, GCobj *o,
						 uint32_t *gctp,
						 GC2MarkScope *scope);
static int gc2_observed_obj_status_scoped(global_State *g, GCobj *o,
						  uint32_t *gctp,
						  GC2MarkScope *scope);
static int gc2_observed_obj_valid(global_State *g, GCobj *o);
static int gc2_queue_obj_info(global_State *g, GCobj *o,
			      GCArena *known_small,
			      LJGC2QueuedInfo *info, int full);
static int gc2_tv_gcref_type_match_known(global_State *g, cTValue *tv);
static int gc2_thread_needscan(lua_State *L);
static int gc2_thread_scan_current(global_State *g, lua_State *L);
static int gc2_thread_has_live_owner(global_State *g, lua_State *th);
static int gc2_admit_thread_identity(global_State *g, lua_State *L,
				      GC2MarkScope *scope);
static void gc2_mark_marked_table_payload_worker(global_State *g, GCtab *t);
static uint32_t gc2_mark_close_help(global_State *g, lua_State *L,
				    uint32_t max_rounds, uint32_t limit);
#if LJ_HASFFI
static void gc2_traverse_clib_retired_cache(global_State *g);
#endif

static LJ_AINLINE int gc2_mark_close_intent_blocks_worker(global_State *g)
{
  /* mark_close_intent arbitrates only the MARK fixpoint/transition. A helper
  ** may publish WEAK and then be descheduled before clearing the advisory
  ** intent. Ordinary WEAK/SWEEP work must remain claimable in that window.
  ** Recheck this predicate after the token CAS so a new MARK intent cannot be
  ** crossed by a claimant which sampled an older phase. */
  return gc2_phase_acq(g) == LJ_GC2_MARK &&
	 gc2_mark_close_intent_acq(g) != 0;
}

static int gc2_worker_claim(global_State *g)
{
  uint32_t expect = 0;
  /*
  ** Worker ownership and zero-reader SMR reclamation are mutually exclusive.
  ** The reclaimer publishes smr_reclaiming first and then rechecks that no
  ** worker won; take the inverse half here. A worker which wins the CAS while
  ** the reclaimer is publishing its gate immediately rolls back, so neither a
  ** GC destructor nor a retire-list publisher can overlap detached metadata.
  */
  if (gc2_mark_close_intent_blocks_worker(g) ||
      gc2_cycle_leader_acq(g) == LJ_THREAD_GCSCAN ||
      gc2_smr_reclaiming_acq(g) != 0 ||
      !gc2_worker_active_cas(g, &expect, 1))
    return 0;
  if (LJ_UNLIKELY(gc2_mark_close_intent_blocks_worker(g) ||
		  gc2_cycle_leader_acq(g) == LJ_THREAD_GCSCAN ||
		  gc2_smr_reclaiming_acq(g) != 0)) {
    gc2_worker_active_rel(g, 0);
    return 0;
  }
  return 1;
}

static int gc2_worker_claim_mark_close(global_State *g)
{
  uint32_t expect = 0;
  if (gc2_cycle_leader_acq(g) == LJ_THREAD_GCSCAN ||
      gc2_smr_reclaiming_acq(g) != 0 ||
      !gc2_worker_active_cas(g, &expect, 1))
    return 0;
  if (LJ_UNLIKELY(gc2_cycle_leader_acq(g) == LJ_THREAD_GCSCAN ||
                  gc2_smr_reclaiming_acq(g) != 0)) {
    gc2_worker_active_rel(g, 0);
    return 0;
  }
  return 1;
}

static int gc2_worker_claim_count_busy(global_State *g)
{
  if (gc2_worker_claim(g))
    return 1;
  gc2_worker_busy_retries_add(g, 1);
  return 0;
}

static int gc2_small_cdata_span_exact(GCArena *a, uint32_t start,
				       GCcdata *cd, GCSize size)
{
  uint32_t cells, i, end;
  if (!a || !cd || size > LJ_HUGE_THRESHOLD ||
      !cdata_size_tail_matches(cd, (size_t)size))
    return 0;
  cells = lj_arena_ncells((size_t)size);
  if (cells == 0 || start + cells > LJ_ARENA_CELLS)
    return 0;
  for (i = 0; i < cells; i++) {
    uint32_t cell = start + i;
    if (!lj_arena_cdata_get(a, cell) ||
	(i != 0 && (lj_arena_bm_get(a->block, cell) ||
		    lj_arena_bm_get(a->mark, cell))))
      return 0;
  }
  end = start + cells;
  /* At the exact end either the next allocation/free-run boundary starts, or
  ** cdata coverage stops. Adjacent cdata allocations are distinguished by
  ** their block boundary even though both coverage ranges contain one bits. */
  if (end < LJ_ARENA_CELLS &&
      !lj_arena_bm_get(a->block, end) && !lj_arena_bm_get(a->mark, end) &&
      lj_arena_cdata_get(a, end))
    return 0;
  return 1;
}

static void gc2_worker_release(global_State *g)
{
  gc2_worker_active_rel(g, 0);
  la_futex_wake(&g->gc2.worker_active, 0x7fffffff);
}

static HugeTab *gc2_small_arena_registry_new(void)
{
  HugeTab *tab = (HugeTab *)malloc(sizeof(HugeTab));
  if (!tab)
    return NULL;
  tab->h = NULL;
  if (!lj_arena_hugetab_init(tab, LJ_ARENA_REGISTRY_BITS)) {
    free(tab);
    return NULL;
  }
  return tab;
}

static void gc2_clear_tg_arena_registry(TGState *tg)
{
  if (tg)
    lj_arena_alloc_set_registry(&tg->alloc, NULL);
}

static void gc2_small_arena_registry_clear_refs(global_State *g)
{
  TGState *tg;
  uint32_t i;
  if (!g)
    return;
  gc2_clear_tg_arena_registry(g->main_tg);
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg))
    gc2_clear_tg_arena_registry(tg);
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++)
    gc2_clear_tg_arena_registry(gc2_worker_tg_acq(g, i));
}

static int gc2_small_arena_registry_init(global_State *g)
{
  HugeTab *tab;
  if (!g || !g->main_tg)
    return 0;
  tab = gc2_small_arena_registry_new();
  if (!tab)
    return 0;
  gc2_small_arena_tab_rel(g, tab);
  lj_arena_alloc_set_registry(&g->main_tg->alloc, tab);
  if (!lj_arena_alloc_register_existing(&g->main_tg->alloc)) {
    /* Fixed-capacity boot registration cannot be rolled back by merely
    ** clearing aliases: an inserted prefix remains the authoritative locator.
    ** The configured table has vastly more slots than boot can consume, so a
    ** failure here is corruption/capacity misconfiguration, not recoverable
    ** allocation failure. Keep every owner published and fail closed. */
    abort();
  }
  return 1;
}

int lj_gc2_small_arena_registry_fini_try(global_State *g)
{
  HugeTab *tab;
  if (!g)
    return 1;
  tab = (HugeTab *)gc2_small_arena_tab_acq(g);
  if (!tab)
    return 1;
  /* Keep every TGAlloc.smalltab and the global wrapper owner published while
  ** a live registry entry refuses teardown. Once the backing table is gone,
  ** joined-world shutdown can clear the now-non-owning aliases and wrapper. */
  if (!lj_arena_hugetab_fini_try(tab))
    return 0;
  gc2_small_arena_registry_clear_refs(g);
  gc2_small_arena_tab_store_rlx(g, NULL);
  free(tab);
  return 1;
}

void lj_gc2_init(global_State *g)
{
  uint32_t i;
  gc2_gcpause_pct_store_rlx(g, (uint32_t)lj_gc_pause_load(g));
  gc2_assist_shift_store_rlx(g,
    lj_gc2_assist_shift_from_stepmul(lj_gc_stepmul_load(g)));
  if (!lj_gc2_activation_init_unpublished(&g->gc2.activation, 0, 0,
                                           LJ_GC2_ACT_IDLE))
    abort();  /* Constant unpublished IDLE/OPEN authority must be representable. */
  lj_gc2_tabledesc_init_unpublished(&g->gc2.table_rescan_desc, 0);
  gc2_phase_store_rlx(g, LJ_GC2_IDLE);
  gc2_cycle_store_rlx(g, 0);
  gc2_thread_scan_cycle_store_rlx(g, 0);
  gc2_cycle_leader_store_rlx(g, 0);
  gc2_hs_epoch_store_rlx(g, 0);
  gc2_hs_pending_store_rlx(g, 0);
  gc2_hs_actions_store_rlx(g, 0);
  gc2_hs_leader_store_rlx(g, 0);
  gc2_hs_signal_ns_store_rlx(g, 0);
  gc2_hs_ack_latency_samples_store_rlx(g, 0);
  gc2_hs_ack_latency_sum_store_rlx(g, 0);
  gc2_hs_ack_latency_max_store_rlx(g, 0);
  for (i = 0; i < LJ_GC2_HS_LATENCY_BUCKETS; i++)
    gc2_hs_ack_latency_bucket_store_rlx(g, i, 0);
  gc2_smr_reclaim_runs_store_rlx(g, 0);
  gc2_smr_reclaimed_store_rlx(g, 0);
  gc2_smr_readers_store_rlx(g, 0);
  gc2_smr_reclaiming_store_rlx(g, LJ_GC2_SMR_OPEN);
  gc2_cycle_requests_store_rlx(g, 0);
  gc2_cycle_starts_store_rlx(g, 0);
  gc2_major_cycle_starts_store_rlx(g, 0);
  gc2_minor_cycle_requests_store_rlx(g, 0);
  gc2_minor_cycle_starts_store_rlx(g, 0);
  gc2_cycle_minor_requested_store_rlx(g, 0);
  gc2_cycle_sweep_minor_store_rlx(g, 0);
  gc2_minor_sweep_enabled_store_rlx(g, 0);
  gc2_cycle_roots_minor_store_rlx(g, 0);
  gc2_minor_roots_enabled_store_rlx(g, 0);
  gc2_minor_sweep_deferred_store_rlx(g, 0);
  gc2_minor_sweep_arenas_store_rlx(g, 0);
  gc2_minor_roots_deferred_store_rlx(g, 0);
  gc2_major_root_scans_store_rlx(g, 0);
  gc2_minor_root_scans_store_rlx(g, 0);
  gc2_pending_root_flushes_store_rlx(g, 0);
  gc2_pending_root_flushed_store_rlx(g, 0);
  gc2_pending_root_flush_max_store_rlx(g, 0);
  gc2_minor_survival_base_live_store_rlx(g, 0);
  gc2_minor_survival_bytes_store_rlx(g, 0);
  gc2_minor_survival_pct_store_rlx(g, 0);
  gc2_minor_survival_threshold_pct_store_rlx(
    g, LJ_GC2_MINOR_SURVIVAL_MAJOR_PCT);
  gc2_minor_survival_major_requests_store_rlx(g, 0);
  gc2_force_major_store_rlx(g, 0);
  gc2_remembered_barriers_store_rlx(g, 0);
  gc2_remembered_pushed_store_rlx(g, 0);
  gc2_remembered_overflows_store_rlx(g, 0);
  gc2_remembered_filtered_store_rlx(g, 0);
  gc2_remembered_drained_store_rlx(g, 0);
  gc2_marks_this_round_store_rlx(g, 0);
  gc2_mark_root_scanned_store_rlx(g, 0);
  gc2_jit_phase_gate_store_rlx(g, 1);
  gc2_jit_mark_resume_store_rlx(g, 0);
  gc2_jit_mark_auto_yield_store_rlx(g, 0);
  gc2_jit_mark_yield_until_ns_store_rlx(g, 0);
  gc2_jit_sweep_displaced_store_rlx(g, 0);
  gc2_jit_sweep_yield_until_ns_store_rlx(g, 0);
  gc2_ssb_head_store_rlx(g, NULL);
  gc2_ssb_drain_rel(g, NULL);
  gc2_ssb_consumer_active_store_rlx(g, 0);
  gc2_ssb_published_store_rlx(g, 0);
  gc2_ssb_drained_store_rlx(g, 0);
  gc2_ssb_items_published_store_rlx(g, 0);
  gc2_ssb_items_drained_store_rlx(g, 0);
  gc2_recovery_items_store_rlx(g, 0);
  gc2_recovery_huge_items_store_rlx(g, 0);
  gc2_recovery_published_store_rlx(g, 0);
  gc2_recovery_redirtied_store_rlx(g, 0);
  gc2_recovery_drained_store_rlx(g, 0);
  gc2_recovery_main_state_store_rlx(g, 0);
  gc2_recovery_failed_store_rlx(g, 0);
  gc2_recovery_scan_lane_store_rlx(g, 0);
  gc2_recovery_small_slot_store_rlx(g, 0);
  gc2_recovery_small_cell_store_rlx(g, LJ_AFIRST_CELL);
  gc2_recovery_huge_slot_store_rlx(g, 0);
  gc2_fixpoint_rounds_store_rlx(g, 0);
  gc2_fixpoint_hits_store_rlx(g, 0);
  gc2_mark_complete_runs_store_rlx(g, 0);
  gc2_mark_complete_hits_store_rlx(g, 0);
  gc2_mark_complete_peer_waits_store_rlx(g, 0);
  gc2_mark_to_weak_store_rlx(g, 0);
  gc2_weak_complete_runs_store_rlx(g, 0);
  gc2_weak_complete_progress_store_rlx(g, 0);
  gc2_weak_to_sweep_store_rlx(g, 0);
  gc2_sweep_bridge_ready_store_rlx(g, 0);
  gc2_sweep_root_scanned_store_rlx(g, 0);
  gc2_sweep_root_cursor_rel(g, NULL);
  gc2_sweep_root_done_rel(g, 0);
  gc2_sweep_grace_needed_rel(g, 0);
  gc2_sweep_to_idle_store_rlx(g, 0);
  gc2_preserve_abort_to_idle_store_rlx(g, 0);
  gc2_alloc_total_bytes_store_rlx(g, 0);
  lj_gc2_alloc_since_store(g, 0);
  lj_gc2_cycle_alloc_store(g, 0);
  lj_gc2_trigger_store(g, 0);
  lj_gc2_hard_store(g, 0);
  lj_gc2_helper_soft_limit_store(g, LJ_GC2_HELPER_IDLE_STEP);
  gc2_assist_runs_store_rlx(g, 0);
  gc2_assist_grey_drained_store_rlx(g, 0);
  gc2_assist_ssb_converted_store_rlx(g, 0);
  gc2_assist_weak_drained_store_rlx(g, 0);
  gc2_jit_hard_checks_store_rlx(g, 0);
  gc2_interp_hard_checks_store_rlx(g, 0);
  gc2_jit_scoped_slots_retired_store_rlx(g, 0);
  gc2_clib_cache_retired_store_rlx(g, NULL);
  gc2_assist_active_store_rlx(g, 0);
  gc2_generational_store_rlx(g, 0);
  for (i = 0; i < LJ_GC2_GREY_EMBEDDED; i++)
    setgcrefnull(g->gc2.grey_embedded[i]);
  gc2_grey_stack_store_rlx(g, g->gc2.grey_embedded);
  gc2_grey_capacity_store_rlx(g, LJ_GC2_GREY_EMBEDDED);
  gc2_grey_top_store_rlx(g, 0);
  gc2_grey_bottom_store_rlx(g, 0);
  gc2_grey_pushed_store_rlx(g, 0);
  gc2_grey_drained_store_rlx(g, 0);
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++)
    gc2_worker_thread_store_rlx(g, i, NULL);
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++)
    gc2_worker_tg_store_rlx(g, i, NULL);
  gc2_worker_tg_retired_store_rlx(g, NULL);
  gc2_n_workers_store_rlx(g, 0);
  gc2_worker_control_store_rlx(g, 0);
  gc2_worker_stop_store_rlx(g, 0);
  gc2_worker_wake_store_rlx(g, 0);
  gc2_worker_started_store_rlx(g, 0);
  gc2_worker_exited_store_rlx(g, 0);
  gc2_worker_active_store_rlx(g, 0);
  gc2_mark_close_intent_store_rlx(g, 0);
  gc2_worker_runs_store_rlx(g, 0);
  gc2_worker_grey_drained_store_rlx(g, 0);
  gc2_worker_ssb_converted_store_rlx(g, 0);
  gc2_worker_weak_drained_store_rlx(g, 0);
  gc2_worker_idle_declares_store_rlx(g, 0);
  gc2_worker_busy_retries_store_rlx(g, 0);
  gc2_worker_wakes_store_rlx(g, 0);
  gc2_worker_parks_store_rlx(g, 0);
  gc2_worker_async_progress_store_rlx(g, 0);
  gc2_tg_thread_roots_store_rlx(g, 0);
  gc2_tg_cur_roots_store_rlx(g, 0);
  gc2_tg_trace_roots_store_rlx(g, 0);
  gc2_thread_scan_claims_store_rlx(g, 0);
  gc2_thread_scan_busy_store_rlx(g, 0);
  gc2_thread_scan_requeues_store_rlx(g, 0);
  gc2_thread_scan_owner_scans_store_rlx(g, 0);
  gc2_thread_scan_needscan_store_rlx(g, 0);
  gc2_thread_scan_owner_needscans_store_rlx(g, 0);
  gc2_thread_scan_needscan_pending_store_rlx(g, 0);
  gc2_table_rescan_pending_store_rlx(g, 0);
  gc2_thread_scan_dirty_misses_store_rlx(g, 0);
  gc2_thread_scan_frame_fallbacks_store_rlx(g, 0);
  gc2_ffi_native_scan_attempts_store_rlx(g, 0);
  gc2_ffi_native_scan_stable_frames_store_rlx(g, 0);
  gc2_ffi_native_scan_retries_store_rlx(g, 0);
  gc2_ffi_native_scan_invalid_store_rlx(g, 0);
  gc2_sweep_owner_runs_store_rlx(g, 0);
  gc2_sweep_owner_arenas_store_rlx(g, 0);
  gc2_sweep_owner_live_cells_store_rlx(g, 0);
  gc2_sweep_live_updates_store_rlx(g, 0);
  gc2_sweep_live_huge_bytes_store_rlx(g, 0);
  gc2_live_estimate_store_rlx(g, 0);
  gc2_weak_stack_store_rlx(g, NULL);
  gc2_weak_ready_store_rlx(g, NULL);
  gc2_weak_overflow_store_rlx(g, NULL);
  gc2_weak_capacity_store_rlx(g, 0);
  gc2_weak_drain_active_store_rlx(g, 0);
  gc2_weak_write_active_store_rlx(g, 0);
  gc2_weak_mark_closed_store_rlx(g, 0);
  gc2_weak_root_scanned_store_rlx(g, 0);
  gc2_weak_count_store_rlx(g, 0);
  gc2_weak_tables_seen_store_rlx(g, 0);
  gc2_weak_tables_weakkey_store_rlx(g, 0);
  gc2_weak_tables_weakval_store_rlx(g, 0);
  gc2_weak_tables_allweak_store_rlx(g, 0);
  gc2_weak_tables_queued_store_rlx(g, 0);
  gc2_weak_tables_overflow_store_rlx(g, 0);
  gc2_weak_scan_cursor_store_rlx(g, 0);
  gc2_weak_scan_runs_store_rlx(g, 0);
  gc2_weak_scan_tables_store_rlx(g, 0);
  gc2_weak_scan_slots_store_rlx(g, 0);
  gc2_weak_scan_clearable_store_rlx(g, 0);
  gc2_weak_clear_cursor_store_rlx(g, 0);
  gc2_weak_clear_runs_store_rlx(g, 0);
  gc2_weak_clear_tables_store_rlx(g, 0);
  gc2_weak_clear_slots_store_rlx(g, 0);
  gc2_weak_clear_cleared_store_rlx(g, 0);
  gc2_weak_bridge_skipped_store_rlx(g, 0);
  gc2_weak_bridge_fallbacks_store_rlx(g, 0);
  gc2_weak_bridge_backfills_store_rlx(g, 0);
  gc2_weak_bridge_backfill_tables_store_rlx(g, 0);
  gc2_weak_bridge_backfill_slots_store_rlx(g, 0);
  gc2_weak_bridge_backfill_cleared_store_rlx(g, 0);
  gc2_finreg_cdata_sets_store_rlx(g, 0);
  gc2_finreg_cdata_clears_store_rlx(g, 0);
  gc2_finreg_cdata_queued_store_rlx(g, 0);
  gc2_finreg_cdata_sweep_queued_store_rlx(g, 0);
  gc2_finreg_cdata_pweak_queued_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_objvec_store_rlx(g, NULL);
  gc2_finreg_cdata_preclaim_finvec_store_rlx(g, NULL);
  gc2_finreg_cdata_preclaim_capacity_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_head_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_count_store_rlx(g, 0);
  gc2_finreg_cdata_pweak_claimed_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_overflow_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_dispatched_store_rlx(g, 0);
  gc2_finreg_cdata_order_seen_store_rlx(g, 0);
  gc2_finreg_cdata_order_claimed_store_rlx(g, 0);
  gc2_finreg_cdata_order_unlinked_store_rlx(g, 0);
  gc2_finreg_cdata_order_queued_store_rlx(g, 0);
  gc2_finreg_cdata_order_retired_store_rlx(g, 0);
  gc2_finreg_cdata_order_tombstones_store_rlx(g, 0);
  gc2_finreg_cdata_order_fallbacks_store_rlx(g, 0);
  gc2_finreg_cdata_pending_order_hits_store_rlx(g, 0);
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  gc2_finreg_cdata_preclaim_test_fail_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_publish_pause_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_publish_paused_store_rlx(g, 0);
  gc2_finreg_cdata_preclaim_publish_release_store_rlx(g, 0);
#endif
  gc2_finreg_udata_sets_store_rlx(g, 0);
  gc2_finreg_udata_clears_store_rlx(g, 0);
  gc2_finreg_udata_queued_store_rlx(g, 0);
  gc2_finreg_udata_head_store_rlx(g, NULL);
  gc2_finreg_udata_retired_store_rlx(g, NULL);
  gc2_finreg_udata_registered_store_rlx(g, 0);
  gc2_finreg_udata_retired_nodes_store_rlx(g, 0);
  gc2_finreg_udata_discovered_store_rlx(g, 0);
  gc2_finreg_udata_forgets_store_rlx(g, 0);
  gc2_finalizer_mpsc_store_rlx(g, NULL);
  gc2_finalizer_tail_store_rlx(g, NULL);
  gc2_finalizer_active_store_rlx(g, 0);
  gc2_finalizer_owner_store_rlx(g, 0);
  gc2_finalizer_spawn_latch_store_rlx(g, 0);
  gc2_finalizer_queued_store_rlx(g, 0);
  gc2_finalizer_dequeued_store_rlx(g, 0);
  gc2_finalizer_mpsc_drained_store_rlx(g, 0);
  gc2_finalizer_enters_store_rlx(g, 0);
  gc2_finalizer_leaves_store_rlx(g, 0);
  gc2_finalizer_sweep_blocks_store_rlx(g, 0);
  gc2_finalizer_spawn_deferrals_store_rlx(g, 0);
  gc2_finalizer_spawn_release_wakes_store_rlx(g, 0);
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  gc2_finalizer_drain_test_pause_store_rlx(g, 0);
  gc2_finalizer_drain_test_paused_store_rlx(g, 0);
  gc2_finalizer_drain_test_release_store_rlx(g, 0);
#endif
  gc2_weak_keys_marked_store_rlx(g, 0);
  gc2_weak_values_marked_store_rlx(g, 0);
  gc2_small_arena_tab_store_rlx(g, NULL);
  gc2_tg_registry_head_store_rlx(g, NULL);
  gc2_tg_registry_nodes_store_rlx(g, 0);
  gc2_tg_registry_incomplete_store_rlx(g, 0);
  gc2_tg_registry_alloc_failures_store_rlx(g, 0);
#if defined(LJ_GC2_TEST_HELPERS)
  gc2_tg_registry_test_fail_alloc_rel(g, 0);
#endif
  gc2_tg_list_store_rlx(g, NULL);
  gc2_n_threads_store_rlx(g, 0);
  gc2_tg_reclaiming_store_rlx(g, 0);
  (void)gc2_small_arena_registry_init(g);
  lj_gc2_update_pacing(g);
  lj_tg_attach(g, G2TG(g));  /* 05 section 5.4.1 main TG registration. */
}

void lj_gc2_fini(global_State *g)
{
  if (LJ_UNLIKELY(!lj_gc2_worker_stop(g)))
    abort();  /* Terminal teardown cannot continue past an unjoined worker. */
  if (g)
    (void)gc2_recovery_discard_terminal(g);
  lj_assertG(!g || (gc2_ssb_head_acq(g) == NULL &&
		    gc2_ssb_drain_acq(g) == NULL &&
		    gc2_ssb_consumer_active_acq(g) == 0),
	     "published SSB survived terminal pre-free drain");
  if (g && mt_shutdown_acq(g) != 0)
    (void)lj_tg_reclaim_dead_terminal(g);
  else
    (void)lj_tg_reclaim_dead(g);
  gc2_worker_reclaim_retired_tgs(g);
  if (g) {
    GCRef *grey_stack = gc2_grey_stack_acq(g);
    if (grey_stack && grey_stack != g->gc2.grey_embedded) {
      lj_mem_freevec(g, grey_stack, gc2_grey_capacity_acq(g), GCRef);
    }
    gc2_grey_stack_store_rlx(g, NULL);
    gc2_grey_capacity_store_rlx(g, 0);
    gc2_grey_top_store_rlx(g, 0);
    gc2_grey_bottom_store_rlx(g, 0);
  }
  if (g) {
    GCRef *weak_stack = gc2_weak_stack_acq(g);
    if (weak_stack) {
      lj_mem_freevec(g, weak_stack, gc2_weak_capacity_acq(g), GCRef);
      gc2_weak_stack_store_rlx(g, NULL);
    }
  }
  if (g) {
    uint8_t *weak_ready = gc2_weak_ready_acq(g);
    if (weak_ready) {
      lj_mem_freevec(g, weak_ready, gc2_weak_capacity_acq(g), uint8_t);
      gc2_weak_ready_store_rlx(g, NULL);
    }
  }
  if (g) {
    GC2WeakOverflow *weak_overflow =
      gc2_weak_overflow_xchg_acqrel(g, NULL);
    if (weak_overflow)
      gc2_weak_overflow_free(g, weak_overflow);
  }
  if (g) {
    gc2_weak_capacity_store_rlx(g, 0);
    gc2_weak_count_store_rlx(g, 0);
  }
  if (g) {
    GCRef *obj = gc2_finreg_cdata_preclaim_objvec_acq(g);
    if (obj) {
      lj_mem_freevec(g, obj, gc2_finreg_cdata_preclaim_capacity_acq(g), GCRef);
      gc2_finreg_cdata_preclaim_objvec_store_rlx(g, NULL);
    }
  }
  if (g) {
    TValue *fin = gc2_finreg_cdata_preclaim_finvec_acq(g);
    if (fin) {
      lj_mem_freevec(g, fin, gc2_finreg_cdata_preclaim_capacity_acq(g), TValue);
      gc2_finreg_cdata_preclaim_finvec_store_rlx(g, NULL);
    }
  }
  if (g) {
    gc2_finreg_cdata_preclaim_capacity_store_rlx(g, 0);
    gc2_finreg_cdata_preclaim_head_store_rlx(g, 0);
    gc2_finreg_cdata_preclaim_count_store_rlx(g, 0);
  }
  if (g) {
    GC2FinRegUDataNode *node =
      gc2_finreg_udata_head_xchg_acqrel(g, NULL);
    while (node) {
      if (LJ_UNLIKELY(!lj_gc2_mem_registered(g, node))) {
	lj_assertG(0, "invalid detached active FINREG userdata node");
	abort();
      }
      GC2FinRegUDataNode *next = gc2_finreg_udata_next_acq(node);
      if (gc2_finreg_udata_active_acq(node))
	lj_mem_freet(g, node);
      node = next;
    }
    node = gc2_finreg_udata_retired_xchg_acqrel(g, NULL);
    while (node) {
      if (LJ_UNLIKELY(!lj_gc2_mem_registered(g, node))) {
	lj_assertG(0, "invalid detached retired FINREG userdata node");
	abort();
      }
      GC2FinRegUDataNode *next = gc2_finreg_udata_retired_next_acq(node);
      lj_mem_freet(g, node);
      node = next;
    }
  }
  if (g) {
    GC2FinalizerNode *stack =
      (GC2FinalizerNode *)gc2_finalizer_mpsc_xchg_acqrel(g, NULL);
    GC2FinalizerNode *tail =
      (GC2FinalizerNode *)gc2_finalizer_tail_acq(g);
    gc2_finalizer_tail_store_rlx(g, NULL);
    gc2_finalizer_free_stack(g, stack);
    gc2_finalizer_free_ring(g, tail);
  }
}

static void lj_gc2_worker_wake_n(global_State *g, int wake_n)
{
  uint32_t n;
  if (!g)
    return;
  n = gc2_n_workers_acq(g);
  if (n == 0)
    return;
  gc2_worker_wakes_add(g, 1);
  (void)gc2_worker_wake_add(g, 1);
  gc2_worker_wake_futex_wake(g, wake_n < (int)n ? wake_n : (int)n);
}

static void lj_gc2_worker_wake(global_State *g)
{
  lj_gc2_worker_wake_n(g, 1);
}

void lj_gc2_sweep_publish_wake(global_State *g)
{
  /* Arena terminal progress is actionable only while SWEEP owns the lifecycle
  ** lists. MARK/WEAK already wake through SSB/grey publication; waking here for
  ** every rescue of a CLOSED survivor turns ordinary marking into a futex storm. */
  if (g && gc2_phase_acq(g) == LJ_GC2_SWEEP)
    lj_gc2_worker_wake(g);
}

static void lj_gc2_worker_wake_all(global_State *g)
{
  lj_gc2_worker_wake_n(g, LJ_GC2_WORKER_MAX);
}

void lj_gc2_test_worker_wake(global_State *g)
{
  lj_gc2_worker_wake_all(g);
}

static int gc2_worker_arena_internal(global_State *g)
{
  return g && g->allocf == lj_arena_allocf && g->main_tg &&
	 lj_tg_flags_test_acq(g->main_tg, TGF_ARENA_INTERNAL);
}

static int gc2_worker_tg_registered(global_State *g, TGState *target)
{
  TGState *tg;
  if (!g || !target)
    return 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg))
    if (tg == target)
      return 1;
  return 0;
}

static int gc2_worker_retire_tg(global_State *g, TGState *tg)
{
  if (!g || !tg)
    return 0;
  /* A worker controller may be a foreign Lua thread. Allocating a retirement
  ** record through mainthread(g) would then mutate the main TG allocator from
  ** the wrong owner while the main thread/JIT is allocating. Keep the link in
  ** the already-quiescent worker TG instead; its storage remains live until
  ** registry unlink makes physical reclamation safe. */
  lj_tg_worker_retire_next_rel(
    tg, (TGState *)gc2_worker_tg_retired_acq(g));
  gc2_worker_tg_retired_rel(g, tg);
  return 1;
}

static void gc2_worker_reclaim_retired_tgs(global_State *g)
{
  TGState *prev = NULL;
  TGState *tg = (TGState *)gc2_worker_tg_retired_acq(g);
  while (tg) {
    TGState *next = lj_tg_worker_retire_next_acq(tg);
    if (!gc2_worker_tg_registered(g, tg)) {
      /* Do not unlink the last raw owner until checked inner finalization has
      ** published DONE. A retained HugeTab/arena remains retryable in place. */
      if (!lj_tg_fini_thread(g, tg)) {
	prev = tg;
	tg = next;
	continue;
      }
      if (prev)
	lj_tg_worker_retire_next_rel(prev, next);
      else
	gc2_worker_tg_retired_rel(g, next);
      lj_tg_worker_retire_next_rel(tg, NULL);
      free(tg);
      tg = next;
      continue;
    }
    prev = tg;
    tg = next;
  }
}

uint32_t lj_gc2_terminal_reclaim_tgs(global_State *g)
{
  TGState *tg;
  uint32_t n = 0, reclaimed;
  if (!g || mt_shutdown_acq(g) == 0)
    return 0;
  /* close_state calls this only after lj_gc2_fini and every final allocator-
  ** routed raw free (including main-TG/global tmpbufs, main stack and lightud
  ** segments), but before GG/main-TG/registry/retire metadata is released. */
  lj_assertG(gc2_n_workers_acq(g) == 0 &&
	     gc2_worker_active_acq(g) == 0,
	     "terminal TG drain overlaps GC worker");
  /* The small-arena directory deliberately survives this raw GC2 teardown:
  ** terminal TG allocators still need it to delete their exact mapping slots.
  ** It is released after the main allocator drains at the close-state tail. */
  lj_assertG(gc2_grey_stack_acq(g) == NULL &&
	     gc2_weak_stack_acq(g) == NULL,
	     "terminal TG drain precedes GC2 raw teardown");
  reclaimed = lj_tg_reclaim_dead_terminal_orphans(g);
  gc2_worker_reclaim_retired_tgs(g);
  /* No later owner lookup or retry point remains. Require the exact registry
  ** shape, not merely the absence of DEAD nodes: a denied orphan try with a
  ** still-LIVE secondary TG must fail before main allocator destruction. */
  if (gc2_n_threads_acq(g) != 1)
    abort();
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (tg != g->main_tg || lj_tg_flags_test_acq(tg, TGF_DEAD) ||
	tg == lj_tg_next_acq(tg) || ++n > 1u)
      abort();
  }
  if (n != 1u)
    abort();
  if (gc2_worker_tg_retired_acq(g) != NULL)
    abort();  /* Unflagged worker storage must have observed FINI_DONE. */
  return reclaimed;
}

static int gc2_worker_release_tg_slot(global_State *g, uint32_t i)
{
  TGState *tg;
  if (!g || i >= LJ_GC2_WORKER_MAX)
    return 0;
  tg = gc2_worker_tg_acq(g, i);
  if (!tg)
    return 1;
  if (gc2_worker_tg_registered(g, tg)) {
    if (!lj_tg_flags_test_acq(tg, TGF_DEAD))
      return 0;
    if (!gc2_worker_retire_tg(g, tg))
      return 0;
    gc2_worker_tg_store_rlx(g, i, NULL);
    return 1;
  }
  if (!lj_tg_fini_thread(g, tg))
    return 0;
  /* Keep the slot as the raw owner through checked finalization, then close
  ** the publication before releasing the body. */
  gc2_worker_tg_store_rlx(g, i, NULL);
  free(tg);
  return 1;
}

static int gc2_worker_release_tg_slots(global_State *g)
{
  uint32_t i;
  int ok = 1;
  if (!g)
    return 0;
  (void)lj_tg_reclaim_dead(g);
  gc2_worker_reclaim_retired_tgs(g);
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++)
    ok &= gc2_worker_release_tg_slot(g, i);
  return ok;
}

static int gc2_worker_prepare_tg_slots(global_State *g)
{
  return gc2_worker_release_tg_slots(g);
}

#if LJ_HASJIT
static int gc2_worker_prepare_traces_l(lua_State *L, int *tokenp)
{
  global_State *g;
  jit_State *J;
  int token;
  if (tokenp)
    *tokenp = 0;
  if (!L)
    return 1;
  g = G(L);
  J = L2J(L);
  token = lj_jit_token_acquire_wait(J);
  if (tokenp)
    *tokenp = token;
  if (!lj_trace_hasany(g))
    return 1;
  if ((hookmask_load(g) & HOOK_GC)) {
    lj_trace_abort(g);
    (void)lj_trace_flushall_gc(L);
    return 1;
  }
  if (lj_trace_flushall_hs(L)) {
    if (token)
      lj_jit_token_release(J);
    if (tokenp)
      *tokenp = 0;
    return 0;
  }
  return 1;
}

static void gc2_worker_finish_traces_l(lua_State *L, int token)
{
  if (token && L)
    lj_jit_token_release(L2J(L));
}
#else
static LJ_AINLINE int gc2_worker_prepare_traces_l(lua_State *L, int *tokenp)
{
  UNUSED(L);
  if (tokenp)
    *tokenp = 0;
  return 1;
}

static LJ_AINLINE void gc2_worker_finish_traces_l(lua_State *L, int token)
{
  UNUSED(L); UNUSED(token);
}
#endif

static int gc2_worker_had_stopreq_l(lua_State *L)
{
  TGState *tg = L ? L2TG(L) : NULL;
  return tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ);
}

static int gc2_worker_fresh_stopreq_l(lua_State *L, uint32_t actions,
				      int had_stopreq)
{
  return lj_safepoint_fresh_stopreq(L, actions, had_stopreq);
}

static int gc2_worker_control_lock_l(global_State *g, lua_State *L,
				     uint32_t *actionsp)
{
  TGState *self = NULL;
  uint32_t was_native = 0;
  int had_stopreq = gc2_worker_had_stopreq_l(L);
  for (;;) {
    uint32_t expect = 0;
    if (gc2_worker_control_cas(g, &expect, 1)) {
      if (self && was_native != ~(uint32_t)0)
	(void)lj_native_leave_tg(self);
      return 1;
    }
    if (!L && !self) {
      self = lj_thr_get_tg_fallback(g);
      if (self) {
	was_native = lj_tg_in_native_acq(self);
	lj_tg_in_native_rel(self, was_native == ~(uint32_t)0 ?
			    was_native : was_native + 1);
      }
    }
    if (expect == 0)
      expect = gc2_worker_control_acq(g);
    if (L) {
      uint32_t actions;
      lj_native_enter(L2TG(L));
      if (expect != 0)
	gc2_worker_control_futex_wait(g, expect, 1000000);
      else
	gc2_peer_wait_no_l();
      actions = lj_native_leave(L);
      if (actionsp)
	*actionsp |= actions;
      if (gc2_worker_fresh_stopreq_l(L, actions, had_stopreq))
	return 0;
    } else {
      if (expect != 0)
	gc2_worker_control_futex_wait(g, expect, 1000000);
      else
	gc2_peer_wait_no_l();
    }
  }
}

static void gc2_worker_control_lock(global_State *g)
{
  (void)gc2_worker_control_lock_l(g, NULL, NULL);
}

static void gc2_worker_control_unlock(global_State *g)
{
  gc2_worker_control_rel(g, 0);
  gc2_worker_control_futex_wake(g, 0x7fffffff);
}

static int gc2_worker_stop_locked_l(global_State *g, lua_State *L,
				    uint32_t *actionsp);

static int gc2_worker_thr_create_l(global_State *g, lua_State *L,
				   LJThr *thr, LJThrFunc func, void *arg,
				   int had_stopreq, uint32_t *actionsp,
				   int *fresh_stopreqp)
{
  TGState *self = NULL;
  uint32_t was_native = 0;
  uint32_t actions = 0;
  int rc;
  if (L) {
    lj_native_enter(L2TG(L));
  } else {
    self = lj_thr_get_tg_fallback(g);
    if (self) {
      was_native = lj_tg_in_native_acq(self);
      lj_tg_in_native_rel(self, was_native == ~(uint32_t)0 ?
			  was_native : was_native + 1);
    }
  }
  rc = lj_thr_create(thr, func, arg);
  if (L) {
    actions = lj_native_leave(L);
    if (actionsp)
      *actionsp |= actions;
    if (fresh_stopreqp && gc2_worker_fresh_stopreq_l(L, actions, had_stopreq))
      *fresh_stopreqp = 1;
  } else if (self) {
    if (was_native != ~(uint32_t)0)
      (void)lj_native_leave_tg(self);
  }
  return rc;
}

static int gc2_worker_start_count_locked_l(global_State *g, uint32_t n,
					   lua_State *waitL,
					   uint32_t *actionsp)
{
  lua_State *L;
  uint32_t i;
  int rc, wait;
  int had_stopreq = gc2_worker_had_stopreq_l(waitL);
  int trace_token = 0;
  if (!g || n == 0)
    return 1;
  if (n > LJ_GC2_WORKER_MAX)
    n = LJ_GC2_WORKER_MAX;
  if (gc2_n_workers_acq(g) != 0)
    return 1;
  /* The explicit sole-mutator string pass never waits for pool control.  A
  ** competing enable either observes this gate here or loses the reciprocal
  ** publication/recheck below before it starts an OS worker. */
  if (lj_str_reclaim_exclusive_acq(g) != 0)
    return 0;
  L = mainthread_acq(g);
  if (!L)
    return 0;
  if (!gc2_worker_prepare_tg_slots(g))
    return 0;
  if (!gc2_worker_prepare_traces_l(waitL, &trace_token))
    return 0;
  gc2_worker_stop_rel(g, 0);
  gc2_worker_started_rel(g, 0);
  gc2_worker_exited_rel(g, 0);
  gc2_n_workers_rel(g, n);  /* 05 section 5.6.3 parked pool. */
  la_fence_seq();
  if (lj_str_reclaim_exclusive_acq(g) != 0) {
    /* No pthread exists yet. Roll the advertised pool back while the control
    ** lock still excludes another pool transition. */
    gc2_n_workers_rel(g, 0);
    gc2_worker_finish_traces_l(waitL, trace_token);
    (void)gc2_worker_release_tg_slots(g);
    return 0;
  }
  gc2_worker_finish_traces_l(waitL, trace_token);
  for (i = 0; i < n; i++) {
    /* Pool control may run on an attached foreign Lua thread. These records
    ** are runtime-only worker metadata, so allocating them through L (the
    ** shared mainthread) would remotely race the main TG bump/bin allocator.
    ** Explicit pool creation already permits OS allocation and pthread calls;
    ** keep this control storage outside the Lua heap for its whole lifetime. */
    LJThr *thr = (LJThr *)malloc(sizeof(LJThr));
    TGState *tg = (TGState *)malloc(sizeof(TGState));
    if (!thr || !tg) {
      free(tg);
      free(thr);
      (void)gc2_worker_stop_locked_l(g, waitL, actionsp);
      return 0;
    }
    thr->tid = lj_thr_newid();
    if (thr->tid == 0) {
      free(tg);
      free(thr);
      (void)gc2_worker_stop_locked_l(g, waitL, actionsp);
      return 0;
    }
    lj_tg_init_thread(g, tg, NULL, gc2_worker_arena_internal(g));
    lj_tg_tid_rel(tg, thr->tid);
    lj_tg_derive_prng(g, tg, thr->tid);
    gc2_worker_thread_store_rlx(g, i, thr);
    gc2_worker_tg_store_rlx(g, i, tg);
    {
      int fresh_stopreq = 0;
      rc = gc2_worker_thr_create_l(g, waitL, thr, gc2_worker_main, tg,
				   had_stopreq, actionsp, &fresh_stopreq);
      if (rc != 0) {
	gc2_worker_thread_store_rlx(g, i, NULL);
	gc2_worker_tg_store_rlx(g, i, NULL);
	if (!lj_tg_fini_thread(g, tg))
	  abort();  /* Never release a wrapper around retained allocator state. */
	free(tg);
	free(thr);
	(void)gc2_worker_stop_locked_l(g, waitL, actionsp);
	return 0;
      }
      if (fresh_stopreq) {
	(void)gc2_worker_stop_locked_l(g, waitL, actionsp);
	return 0;
      }
    }
  }
  for (wait = 0; wait < 1000; wait++) {
    uint32_t started = gc2_worker_started_acq(g);
    if (started >= n)
      return 1;
    if (waitL) {
      uint32_t actions;
      lj_native_enter(L2TG(waitL));
      gc2_worker_started_futex_wait(g, started, 1000000);
      actions = lj_native_leave(waitL);
      if (actionsp)
	*actionsp |= actions;
      if (gc2_worker_fresh_stopreq_l(waitL, actions, had_stopreq)) {
	(void)gc2_worker_stop_locked_l(g, waitL, actionsp);
	return 0;
      }
    } else {
      gc2_worker_started_futex_wait(g, started, 1000000);
    }
  }
  if (gc2_worker_started_acq(g) >= n)
    return 1;
  (void)gc2_worker_stop_locked_l(g, waitL, actionsp);
  return 0;
}

static int gc2_worker_start_count_locked(global_State *g, uint32_t n)
{
  return gc2_worker_start_count_locked_l(g, n, NULL, NULL);
}

uint32_t lj_gc2_workers_count(global_State *g)
{
  return g ? gc2_n_workers_acq(g) : 0;
}

int lj_gc2_workers_set_l(lua_State *L, uint32_t n, uint32_t *actionsp)
{
  global_State *g = L ? G(L) : NULL;
  uint32_t old;
  uint32_t actions = 0;
  int ok;
  int had_stopreq = gc2_worker_had_stopreq_l(L);
  if (actionsp)
    *actionsp = 0;
  if (!g)
    return 0;
  if (n > LJ_GC2_WORKER_MAX)
    n = LJ_GC2_WORKER_MAX;
  if (!gc2_worker_control_lock_l(g, L, &actions)) {
    if (actionsp)
      *actionsp = actions;
    return 0;
  }
  old = gc2_n_workers_acq(g);
  if (old == n && gc2_worker_stop_acq(g) == 0) {
    gc2_worker_control_unlock(g);
    if (actionsp)
      *actionsp = actions;
    return 1;
  }
  if (old != 0 && !gc2_worker_stop_locked_l(g, L, &actions)) {
    gc2_worker_control_unlock(g);
    if (actionsp)
      *actionsp = actions;
    return 0;
  }
  if (n == 0) {
    gc2_worker_control_unlock(g);
    if (actionsp)
      *actionsp = actions;
    return 1;
  }
  if (gc2_worker_fresh_stopreq_l(L, actions, had_stopreq)) {
    gc2_worker_control_unlock(g);
    if (actionsp)
      *actionsp = actions;
    return 0;
  }
  ok = gc2_worker_start_count_locked_l(g, n, L, &actions);
  gc2_worker_control_unlock(g);
  if (actionsp)
    *actionsp = actions;
  return ok;
}

int lj_gc2_workers_set(global_State *g, uint32_t n)
{
  uint32_t old;
  int ok;
  if (!g)
    return 0;
  if (n > LJ_GC2_WORKER_MAX)
    n = LJ_GC2_WORKER_MAX;
  gc2_worker_control_lock(g);
  old = gc2_n_workers_acq(g);
  if (old == n && gc2_worker_stop_acq(g) == 0) {
    gc2_worker_control_unlock(g);
    return 1;
  }
  if (old != 0 && !gc2_worker_stop_locked_l(g, NULL, NULL)) {
    gc2_worker_control_unlock(g);
    return 0;
  }
  if (n == 0) {
    gc2_worker_control_unlock(g);
    return 1;
  }
  ok = gc2_worker_start_count_locked(g, n);
  gc2_worker_control_unlock(g);
  return ok;
}

static int gc2_worker_stop_locked_l(global_State *g, lua_State *L,
				    uint32_t *actionsp)
{
  uint32_t i, any = 0, unjoined = 0;
  TGState *self;
  uint32_t was_native = 0;
  if (!g)
    return 1;
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++)
    any |= gc2_worker_thread_acq(g, i) != NULL;
  if (!any) {
    gc2_n_workers_rel(g, 0);
    return gc2_worker_release_tg_slots(g);
  }
  gc2_worker_stop_rel(g, 1);
  lj_gc2_worker_wake_all(g);
  self = NULL;
  if (L) {
    lj_native_enter(L2TG(L));  /* Join wait can remote-ack workers. */
  } else {
    self = lj_thr_get_tg_fallback(g);
    if (self) {
      was_native = lj_tg_in_native_acq(self);
      lj_tg_in_native_rel(self, was_native == ~(uint32_t)0 ?
			  was_native : was_native + 1);
      /* Join wait can remote-ack workers. */
    }
  }
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++) {
    LJThr *thr = (LJThr *)gc2_worker_thread_acq(g, i);
    if (!thr)
      continue;
    if (lj_thr_join(thr, NULL) == 0) {
      gc2_worker_thread_store_rlx(g, i, NULL);
      free(thr);
    } else {
      /* A failed join is not quiescence. Retain both the OS-thread record and
      ** its TG slot, keep STOP published, and let a later controller retry.
      ** Reclaiming either record here would turn an OS error into a UAF. */
      unjoined++;
    }
  }
  if (L) {
    uint32_t actions = lj_native_leave(L);
    if (actionsp)
      *actionsp |= actions;
  } else if (self) {
    if (was_native != ~(uint32_t)0)
      (void)lj_native_leave_tg(self);
  }
  if (unjoined != 0) {
    gc2_n_workers_rel(g, unjoined);
    return 0;
  }
  /* All worker pthreads are joined. Publish zero before the dead-TG registry
  ** pass so its conservative no-worker predicate can reclaim their detached
  ** ownership records without weakening attach-time safety. */
  gc2_n_workers_rel(g, 0);
  return gc2_worker_release_tg_slots(g);
}

static int gc2_worker_stop_locked(global_State *g)
{
  return gc2_worker_stop_locked_l(g, NULL, NULL);
}

int lj_gc2_worker_stop(global_State *g)
{
  int stopped;
  if (!g)
    return 1;
  gc2_worker_control_lock(g);
  stopped = gc2_worker_stop_locked(g);
  gc2_worker_control_unlock(g);
  return stopped;
}

static int gc2_worker_park_timeout_ns(global_State *g)
{
  enum { GC2_ACTIVE_PARK_TIMEOUT_NS = 10000000 };
  uint32_t phase = gc2_phase_acq(g);
  if (gc2_recovery_stalled_failed(g))
    return -1;
  if (phase == LJ_GC2_IDLE &&
      !(gc2_cycle_leader_acq(g) != 0 &&
	gc2_jit_phase_gate_acq(g) == 0))
    return -1;
  if (phase == LJ_GC2_MARK && gc2_jit_phase_gate_acq(g) != 0) {
    uint64_t deadline = gc2_jit_mark_yield_until_ns_acq(g);
    if (deadline != 0) {
      uint64_t now = lj_thr_now_ns();
      uint64_t left = deadline > now ? deadline - now : 1u;
      if (left < GC2_ACTIVE_PARK_TIMEOUT_NS)
	return (int)left;
    }
  }
  if (phase == LJ_GC2_SWEEP && gc2_jit_phase_gate_acq(g) != 0) {
    uint64_t deadline = gc2_jit_sweep_yield_until_ns_acq(g);
    if (deadline != 0) {
      uint64_t now = lj_thr_now_ns();
      uint64_t left = deadline > now ? deadline - now : 1u;
      if (left < GC2_ACTIVE_PARK_TIMEOUT_NS)
	return (int)left;
    }
  }
  return GC2_ACTIVE_PARK_TIMEOUT_NS;
}

static void *gc2_worker_main(void *arg)
{
  TGState *tg = (TGState *)arg;
  global_State *g = tg ? tg->gl : NULL;
  if (!g)
    return NULL;
  lj_thr_set_tg(tg);
  lj_native_enter(tg);  /* No Lua stack: remote handshakes may ack this TG. */
  lj_tg_attach(g, tg);
  (void)gc2_worker_started_add(g, 1);
  gc2_worker_started_futex_wake(g, LJ_GC2_WORKER_MAX);
  while (gc2_worker_stop_acq(g) == 0) {
    uint32_t wake = gc2_worker_wake_acq(g);
    uint32_t total = 0;
    for (;;) {
      uint32_t step;
      if (gc2_worker_stop_acq(g) != 0)
	break;
      step = lj_gc2_worker_drain(g, LJ_GC2_WORKER_DRAIN_BATCH);
      if (step == 0)
	break;
      total = step > ~(uint32_t)0 - total ? ~(uint32_t)0 : total + step;
    }
    if (total == 0 && gc2_phase_acq(g) == LJ_GC2_SWEEP &&
	lj_gc2_sweep_to_idle(g))
      total = 1;  /* 05 section 5.8: scheduler-owned idle close. */
    if (total)
      gc2_worker_async_progress_add(g, total);
    if (gc2_worker_stop_acq(g) != 0)
      break;
    /* Snapshot-before-check closes the wake-before-wait window. A publisher
    ** which made work visible after this iteration began either changes the
    ** sequence here or makes the futex wait fail its expected-value check. */
    if (gc2_worker_wake_acq(g) != wake)
      continue;
    gc2_worker_parks_add(g, 1);
    if (gc2_worker_stop_acq(g) != 0)
      break;
    /* worker_active release wakes its own futex, not worker_wake. A worker
    ** which observed that token busy can otherwise consume the last wake,
    ** park here, and miss the release while real mark/sweep work remains.
    ** Keep the overwhelmingly common IDLE park indefinite (zero polling), but
    ** use a low-frequency active-cycle retry as the notification backstop.
    ** This is scheduler repair, not ownership transfer: a preempted token
    ** holder remains a separate helpability problem. */
    gc2_worker_wake_futex_wait(g, wake, gc2_worker_park_timeout_ns(g));
  }
  lj_tg_detach(g, tg);
  lj_thr_set_tg(NULL);
  (void)gc2_worker_exited_add(g, 1);
  gc2_worker_exited_futex_wake(g, LJ_GC2_WORKER_MAX);
  return NULL;
}

uint64_t lj_gc2_flush_alloc(global_State *g, TGState *tg)
{
  uint64_t bytes;
  if (!g || !tg)
    return 0;
  bytes = lj_tg_local_total_xchg_acqrel(tg, 0);
  if (bytes != 0) {
    gc2_alloc_total_bytes_add(g, bytes);
    lj_gc2_alloc_since_add(g, bytes);  /* 05 section 5.11. */
  }
  return bytes;
}

static int gc2_logical_stopped(global_State *g)
{
  if (mt_live_acq(g) != 0) {
    GCSize threshold = lj_gc_mt_threshold_load(g);
    if (mt_live_acq(g) == 0)
      threshold = lj_gc_threshold_load(g);
    return threshold == LJ_MAX_MEM;
  }
  return lj_gc_threshold_load(g) == LJ_MAX_MEM;
}

static void gc2_request_threshold(global_State *g)
{
  GCSize total = lj_gc_total_load(g);
  if (mt_live_acq(g) != 0) {
    lj_gc_mt_threshold_store(g, total);
    if (mt_live_acq(g) == 0)
      lj_gc_threshold_store(g, total);
  } else {
    lj_gc_threshold_store(g, total);
  }
}

static int gc2_request_cycle_start(global_State *g, TGState *tg,
				   int honor_stop)
{
  uint32_t expect = 0;
  uint32_t tid = tg ? lj_tg_tid_acq(tg) : 0;
  if (!lj_thr_id_is_owner(tid))
    return 0;
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    return 0;
  if (honor_stop && gc2_logical_stopped(g))
    return 0;  /* Honor collectgarbage("stop"). */
  if (!gc2_cycle_leader_cas(g, &expect, tid))
    return 0;  /* 05 section 5.11 nonblocking cycle-request token. */
  if (LJ_UNLIKELY(gc2_phase_acq(g) != LJ_GC2_IDLE)) {
    /* A requester may have paused after its initial IDLE sample while another
    ** actor published MARK and consumed the preceding request. Do not strand a
    ** tid token in an active phase: relinquish only this exact request. */
    expect = tid;
    (void)gc2_cycle_leader_cas(g, &expect, 0);
    return 0;
  }
  gc2_cycle_requests_add(g, 1);  /* 05 section 5.11 telemetry. */
  gc2_request_threshold(g);  /* Color cycle-driver bridge. */
  return 1;
}

static int lj_gc2_request_cycle(global_State *g, TGState *tg)
{
  int requested;
  if (!g)
    return 0;
  requested = gc2_request_cycle_start(g, tg, 1);
  if (!requested && gc2_phase_acq(g) != LJ_GC2_IDLE)
    lj_gc2_worker_wake(g);  /* Explicit step/overflow may expose active work. */
  return requested;
}

int lj_gc2_request_cycle_pressure(global_State *g, TGState *tg)
{
  return lj_gc2_request_cycle(g, tg);
}

int lj_gc2_request_cycle_explicit(global_State *g, TGState *tg)
{
  int requested;
  if (!g)
    return 0;
  requested = gc2_request_cycle_start(g, tg, 0);
  if (!requested && gc2_phase_acq(g) != LJ_GC2_IDLE)
    lj_gc2_worker_wake(g);
  return requested;  /* Explicit step ignores the automatic-GC stop gate. */
}

int lj_gc2_request_major(global_State *g, TGState *tg)
{
  if (!g)
    return 0;
  lj_gc2_force_major(g);
  return lj_gc2_request_cycle(g, tg);
}

static int gc2_request_major_explicit(global_State *g, TGState *tg)
{
  if (!g)
    return 0;
  lj_gc2_force_major(g);
  return gc2_request_cycle_start(g, tg, 0);
}

int lj_gc2_collect_active(lua_State *L)
{
  global_State *g;
  TGState *tg;
  int need_major = 1;
  int extra_finreg = 0;
  if (!L)
    return 0;
  g = G(L);
  tg = L2TG(L);
  if (!g || !tg)
    return 0;
#if LJ_HASJIT
  /* A full collection invoked from a recorder-owned callback/allocation cannot
  ** wait for that same recorder to reach IDLE: the recorder cannot unwind
  ** until this nested API call returns. Publish the ordinary asynchronous
  ** abort and defer the collection. A peer-owned recorder follows the normal
  ** nonwaiting root-retry path instead. */
  if (gc2_jit_recorder_active(g) && lj_jit_token_held_l(L, G2J(g))) {
    lj_trace_abort(g);
    return 0;
  }
#endif
  /* gc2_call_finalizer retains the finalized object's body scope across the
  ** Lua callback, so no full collector can close SWEEP until that callback
  ** returns. This is not only a same-owner recursion hazard: a finalizer may
  ** join a pre-existing peer which concurrently calls collectgarbage(). If
  ** that peer waits for finalizer_active, each TG waits for the other forever.
  ** Treat every active finalizer as an asynchronous full-collection deferral;
  ** the outer collector or a later request resumes the already-open cycle.
  */
  if (gc2_finalizer_active_acq(g) != 0)
    return 0;
  /* A sticky classification failure deliberately has no synthetic queue
  ** identity. Once all real identities drain, full collection must return
  ** fail-closed instead of self-waiting forever for impossible work. */
  if (gc2_recovery_stalled_failed(g))
    return 0;
  if (gc2_request_major_explicit(g, tg) && gc2_mark_begin(g)) {
    need_major = 0;
  }
  for (;;) {
    uint32_t phase = gc2_phase_acq(g);
    /* A peer callback can enter after the admission check above. Never turn
    ** that race into a SWEEP-close wait; bounded steps and GC workers retain
    ** responsibility for progress after the callback releases its scope. */
    if (gc2_finalizer_active_acq(g) != 0)
      return 0;
    if (gc2_recovery_stalled_failed(g))
      return 0;
    /* Gate closure is an asynchronous request, not permission for explicit
    ** collection to wait on a peer's jit_base. This is expected in pre-MARK
    ** IDLE and in SWEEP when ordinary FFI remains blocked; MARK/WEAK normally
    ** cannot observe it because their transition proofs require quiescence. */
    if (gc2_jit_phase_gate_acq(g) == 0 && lj_tg_any_jit_active(g))
      return 0;
    if (phase == LJ_GC2_IDLE) {
      if (need_major) {
	if (gc2_request_major_explicit(g, tg) ||
	    gc2_cycle_leader_acq(g) != 0) {
	  if (gc2_mark_begin(g)) {
	    need_major = 0;
	    continue;
	  }
	  gc2_peer_wait_l(L);
	  continue;
	}
      }
      if (!need_major && !extra_finreg && lj_gc2_finreg_cdata_pending(g)) {
	extra_finreg = 1;
	if (gc2_request_major_explicit(g, tg) ||
	    gc2_cycle_leader_acq(g) != 0) {
	  if (gc2_mark_begin(g))
	    continue;
	  gc2_peer_wait_l(L);
	  continue;
	}
      }
      lj_gc2_publish_idle_threshold(g);
      return 1;
    }
    if (phase == LJ_GC2_MARK) {
      if (gc2_worker_drain_logical(g, tg,
					   LJ_GC2_WORKER_DRAIN_BATCH, 0) != 0) {
	(void)lj_safepoint_ack(L);
	continue;
      }
      if (lj_gc2_mark_complete(g, L, 64, ~(uint32_t)0)) {
	lj_gc2_mark_to_weak(g);
	continue;
      }
      gc2_peer_wait_l(L);
      continue;
    }
    if (phase == LJ_GC2_WEAK) {
      if (lj_gc2_weak_complete(g, L, NULL, LJ_GC2_WEAK_DRAIN_BATCH)) {
	lj_gc2_weak_to_sweep(g, L);
	continue;
      }
      gc2_peer_wait_l(L);
      continue;
    }
    if (phase == LJ_GC2_SWEEP) {
      GCSize cost;
      int finstep;
      lj_gc2_sweep_prepare_bridge_boundary(
	g, lj_gc_preserve_root_chain_for_gc2_sweep);
      if (gc2_worker_drain_logical(g, tg,
					   LJ_GC2_SWEEP_BATCH, 0) != 0) {
	(void)lj_safepoint_ack(L);
	continue;
      }
      finstep = lj_gc2_finalizer_step(L, GC2_ACTIVE_FINALIZE_COST, &cost);
      UNUSED(cost);
      if (finstep != 0) {
	if (finstep < 0) {
	  if (lj_gc2_finalizer_deferred(g))
	    return 0;
	  gc2_peer_wait_l(L);
	}
	continue;
      }
      if (lj_gc2_sweep_to_idle(g)) {
	lj_gc2_publish_idle_threshold(g);
	continue;
      }
      gc2_peer_wait_l(L);
      continue;
    }
    return 0;
  }
}

int lj_gc2_step_explicit(lua_State *L, uint32_t budget)
{
  global_State *g;
  TGState *tg;
  int drove_cycle = 0;
  int extra_finreg = 0;
  if (!L)
    return 0;
  g = G(L);
  tg = L2TG(L);
  if (!g || !tg)
    return 0;
  if (budget == 0)
    budget = 1;
  if (budget > (1u << 20))
    budget = (1u << 20);
  while (budget-- != 0) {
    uint32_t phase = gc2_phase_acq(g);
    if (gc2_recovery_stalled_failed(g))
      break;
    if (phase != LJ_GC2_IDLE)
      drove_cycle = 1;
    if (phase == LJ_GC2_IDLE) {
      if (!drove_cycle) {
	if (gc2_request_major_explicit(g, tg) ||
	    gc2_cycle_leader_acq(g) != 0) {
	  if (gc2_mark_begin(g)) {
	    drove_cycle = 1;
	    continue;
	  }
	  if (budget != 0) {
	    gc2_peer_wait_l(L);
	    continue;
	  }
	  break;
	}
      } else if (!extra_finreg && lj_gc2_finreg_cdata_pending(g)) {
	extra_finreg = 1;
	if (gc2_request_major_explicit(g, tg) ||
	    gc2_cycle_leader_acq(g) != 0) {
	  if (gc2_mark_begin(g))
	    continue;
	  if (budget != 0) {
	    gc2_peer_wait_l(L);
	    continue;
	  }
	  break;
	}
      }
      lj_gc2_publish_idle_threshold(g);
      return 1;
    }
    if (phase == LJ_GC2_MARK) {
      if (gc2_worker_drain_logical(g, tg,
					   LJ_GC2_WORKER_DRAIN_BATCH, 0) != 0) {
	(void)lj_safepoint_ack(L);
	continue;
      }
      if (lj_gc2_mark_complete(g, L, 1, LJ_GC2_WORKER_DRAIN_BATCH)) {
	lj_gc2_mark_to_weak(g);
	continue;
      }
      if (budget != 0) {
	gc2_peer_wait_l(L);
	continue;
      }
      break;
    }
    if (phase == LJ_GC2_WEAK) {
      if (lj_gc2_weak_complete(g, L, NULL, LJ_GC2_WEAK_DRAIN_BATCH)) {
	lj_gc2_weak_to_sweep(g, L);
	continue;
      }
      if (budget != 0) {
	gc2_peer_wait_l(L);
	continue;
      }
      break;
    }
    if (phase == LJ_GC2_SWEEP) {
      GCSize cost;
      int finstep;
      lj_gc2_sweep_prepare_bridge_boundary(
	g, lj_gc_preserve_root_chain_for_gc2_sweep);
      if (gc2_worker_drain_logical(g, tg,
					   LJ_GC2_SWEEP_BATCH, 0) != 0) {
	(void)lj_safepoint_ack(L);
	continue;
      }
      finstep = lj_gc2_finalizer_step(L, GC2_ACTIVE_FINALIZE_COST, &cost);
      UNUSED(cost);
      if (finstep < 0 || (finstep != 0 && budget == 0))
	break;
      if (finstep != 0)
	continue;
      if (lj_gc2_sweep_to_idle(g)) {
	lj_gc2_publish_idle_threshold(g);
	if (budget != 0)
	  continue;
	return 1;
      }
      if (budget != 0) {
	gc2_peer_wait_l(L);
	continue;
      }
      break;
    }
    break;
  }
  return 0;
}

void lj_gc2_check_trigger(global_State *g, TGState *tg)
{
  if (gc2_phase_acq(g) != LJ_GC2_IDLE)
    return;
  if (lj_gc2_alloc_since_load(g) <=
      lj_gc2_trigger_load(g))  /* 05 section 5.11 trigger. */
    return;
  (void)lj_gc2_request_cycle(g, tg);
}

void lj_gc2_account_alloc(global_State *g, TGState *tg, GCSize bytes)
{
  uint64_t old, total;
  if (!g || !tg || bytes == 0)
    return;
  old = lj_tg_local_total_add_rlx(tg, (uint64_t)bytes);
  total = old + (uint64_t)bytes;
  if (total >= old && total < LJ_GC2_ACCT_FLUSH)
    return;
  (void)lj_gc2_flush_alloc_checkpoint(g, tg);
}

uint64_t lj_gc2_flush_alloc_checkpoint(global_State *g, TGState *tg)
{
  uint64_t flushed;
  if (!g || !tg)
    return 0;
  flushed = lj_gc2_flush_alloc(g, tg);
  if (flushed == 0)
    return 0;
  lj_gc2_check_trigger(g, tg);
  if (lj_gc2_hard_limit_reached(g)) {  /* 05 section 5.11 hard limit. */
    uint64_t since = lj_gc2_alloc_since_load(g);
    if (lj_tg_jit_base(g) == NULL ||
	lj_gc2_hard_load(g) < LJ_GC2_ACCT_FLUSH ||
	since >= lj_gc2_hard_check_load(g)) {
      if (lj_tg_jit_base(g) != NULL)
	gc2_jit_hard_checks_add(g, 1);
      (void)lj_gc2_assist(g, tg);
      /* A traced allocation helper can consume this hard checkpoint before
      ** asm_gc_check reaches lj_gc_step_jit(). In cooperative SWEEP, publish
      ** the same asynchronous gate close after bounded assist has released any
      ** worker ownership and before advancing the cadence. The current helper
      ** returns normally; XPOLL or finite trace return publishes quiescence. */
      if (lj_tg_jit_base(g) != NULL &&
	  gc2_phase_acq(g) == LJ_GC2_MARK &&
	  lj_gc2_jit_entry_open(g)) {
	lj_gc2_jit_mark_request_exit(g);
	lj_assertG(gc2_jit_phase_gate_acq(g) == 0,
		   "allocation checkpoint failed to close JIT MARK gate");
	gc2_test_jit_mark_checkpoint_closed();
      } else if (lj_tg_jit_base(g) != NULL &&
	  gc2_phase_acq(g) == LJ_GC2_SWEEP &&
	  lj_gc2_jit_entry_open(g)) {
	lj_gc2_jit_sweep_request_exit(g);
	lj_assertG(gc2_jit_phase_gate_acq(g) == 0,
		   "allocation checkpoint failed to close JIT SWEEP gate");
	gc2_test_jit_sweep_checkpoint_closed();
      }
      lj_gc2_hard_check_advance(g, since);
    }
  }
  return flushed;
}

uint32_t lj_gc2_assist_shift_from_stepmul(uint32_t stepmul)
{
  uint32_t shift = 0;
  uint32_t work = stepmul < 100 ? 1u : stepmul / 100u;
  while (work > 1u && shift < 8u) {
    work = (work + 1u) >> 1;
    shift++;
  }
  return shift;
}

static uint64_t gc2_helper_soft_next(GCSize total)
{
  uint64_t max = ~(uint64_t)0;
  return (uint64_t)total > max - LJ_GC2_HELPER_IDLE_STEP ?
	 max : (uint64_t)total + LJ_GC2_HELPER_IDLE_STEP;
}

static uint64_t gc2_hard_check_next(global_State *g, uint64_t since)
{
  uint64_t hard, max = ~(uint64_t)0;
  if (!g)
    return 0;
  hard = lj_gc2_hard_load(g);
  if (hard < LJ_GC2_ACCT_FLUSH || since <= hard)
    return hard;
  return since > max - LJ_GC2_TRACE_HARD_CHECK_BATCH ?
	 max : since + LJ_GC2_TRACE_HARD_CHECK_BATCH;
}

void lj_gc2_hard_check_advance(global_State *g, uint64_t since)
{
  uint64_t old, next;
  if (!g)
    return;
  next = gc2_hard_check_next(g, since);
  old = lj_gc2_hard_check_load(g);
  while (old < next) {
    uint64_t expect = old;
    if (lj_gc2_hard_check_cas(g, &expect, next))
      return;
    old = expect;
  }
}

void lj_gc2_update_pacing(global_State *g)
{
  uint64_t live, gc_live, gc2_live, trigger, hard;
  uint32_t pct;
  if (!g)
    return;
  gc_live = g->gc.estimate ? g->gc.estimate : lj_gc_total_load(g);
  gc2_live = gc2_live_estimate_acq(g);
  live = gc2_live > gc_live ? gc2_live : gc_live;
  if (live < LJ_GC2_TRIGGER_MIN)
    live = LJ_GC2_TRIGGER_MIN;
  pct = gc2_gcpause_pct_acq(g);
  if (pct == 0)
    pct = 100;
  trigger = (live / 100u) * (uint64_t)pct +
	    ((live % 100u) * (uint64_t)pct) / 100u;
  if (trigger < LJ_GC2_TRIGGER_MIN)
    trigger = LJ_GC2_TRIGGER_MIN;
  /*
  ** Fresh GC objects are published through per-TG pending-root chains until a
  ** GC2 root scan folds them into the root spine. The root scan is bounded
  ** only by allocation since the previous cycle, not by live heap size, so cap
  ** the automatic trigger while this bridge exists. This keeps cycle-start
  ** root work non-blocking even after a larger live heap raises the ordinary
  ** pause-derived trigger.
  */
  if (trigger > LJ_GC2_PENDING_ROOT_TRIGGER_MAX)
    trigger = LJ_GC2_PENDING_ROOT_TRIGGER_MAX;
  hard = trigger > ~(uint64_t)0 / 2u ? ~(uint64_t)0 : trigger * 2u;
  lj_gc2_trigger_store(g, trigger);  /* 05 section 5.11. */
  lj_gc2_hard_store(g, hard);  /* 05 section 5.11. */
}

void lj_gc2_publish_idle_threshold(global_State *g)
{
  uint64_t trigger, since, remain, total, limit;
  if (!g)
    return;
  if (g->gc.state != GCSpause || gc2_phase_acq(g) != LJ_GC2_IDLE)
    return;
  if (lj_gc_threshold_load(g) == LJ_MAX_MEM)
    return;  /* Honor collectgarbage("stop") and MT stop-the-world gates. */
  trigger = lj_gc2_trigger_load(g);
  since = lj_gc2_alloc_since_load(g);
  remain = since >= trigger ? 0 : trigger - since;
  total = (uint64_t)lj_gc_total_load(g);
  lj_gc2_helper_soft_limit_store(g, gc2_helper_soft_next((GCSize)total));
  limit = (uint64_t)LJ_MAX_MEM - 1u;
  if (remain > limit || total > limit - remain)
    lj_gc_threshold_store(g, (GCSize)limit);
  else
    lj_gc_threshold_store(g, (GCSize)(total + remain));
}

static void gc2_reset_alloc_trigger(global_State *g)
{
  TGState *tg;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg))
    (void)lj_gc2_flush_alloc(g, tg);
  lj_gc2_cycle_alloc_store(g, lj_gc2_alloc_since_xchg(g, 0));
  lj_gc2_hard_check_store(g, lj_gc2_hard_load(g));
  lj_gc2_helper_soft_limit_store(g, ~(uint64_t)0);
}

static int gc2_arena_list_contains(GCArena *a, GCArena *want)
{
  uint32_t n = 0;
  for (; a != NULL; a = lj_arena_next_acq(a)) {
    if (a == want)
      return 1;
    if (LJ_UNLIKELY(++n >= LJ_GC2_ROOT_SCAN_LIMIT))
      break;
  }
  return 0;
}

static int gc2_tg_owns_small_arena(TGState *tg, GCArena *want)
{
  uint32_t k;
  if (!tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return 0;
  /*
  ** The shared arena registry is authoritative when present. This list walk is
  ** kept for states that could not allocate the registry during bootstrap.
  */
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    if (tg->alloc.bump[k].a == want ||
	gc2_arena_list_contains(tg->alloc.owned[k], want) ||
	gc2_arena_list_contains(tg->alloc.needsweep[k], want))
      return 1;
  }
  return 0;
}

static int gc2_small_arena_registered(global_State *g, const GCArena *want,
				      LJHugeInfo *hi)
{
  uint32_t flags;
  HugeTab *tab = g ? (HugeTab *)gc2_small_arena_tab_acq(g) : NULL;
  if (!want)
    return 0;
  if (tab)
    return lj_arena_hugetab_lookup(tab, want, hi) == 1;
  /*
  ** During bootstrap the shared small-arena registry may not exist yet, but the
  ** only raw-memory candidates come from the internal arena allocator. Once the
  ** registry is published, membership must be decided by address lookup before
  ** reading the candidate header: retired metadata can contain stale words whose
  ** arena-aligned base is unmapped or otherwise not ours.
  */
  flags = lj_arena_flags_acq(want);
  if ((flags & LJ_AF_HUGE_MAGIC) != LJ_AF_HUGE_MAGIC &&
      (flags & LJ_AF_REGISTERED)) {
    if (hi) {
      hi->size = LJ_ARENA_SIZE;
      hi->flags = (flags & LJ_AF_TRAVERSABLE) ? LJ_HUGEF_TRAVERSABLE : 0;
    }
    return 1;
  }
  return -1;
}

/* Close the registry-lookup -> remote_active gap. A published registry is a
** HugeTab of exact arena mappings; its counted reader prevents delete/unmap
** until rescue_enter has installed the arena-local lifetime admission. During
** bootstrap, tactical SMR protects the trusted owner-list fallback through
** the same handoff. No arena header byte is read before one of those pins. */
static int gc2_small_registered_rescue_enter(global_State *g, GCArena *a)
{
  HugeTab *tab;
  int admission;
  if (!g || !a)
    return LJ_ARENA_RESCUE_RETRY;
  tab = (HugeTab *)gc2_small_arena_tab_acq(g);
  if (tab) {
    LJHugeInfo hi;
    admission = lj_arena_hugetab_rescue_enter(tab, a, &hi);
    if (admission == LJ_ARENA_RESCUE_RETRY)
      return admission;
    if (hi.size != LJ_ARENA_SIZE) {
      if (gc2_mark_admission_counted(admission))
	lj_arena_rescue_leave(a);
      return LJ_ARENA_RESCUE_RETRY;
    }
    return admission;
  }
  if (!lj_gc2_smr_read_try(g))
    return LJ_ARENA_RESCUE_RETRY;
  if (!gc2_small_arena_known(g, a)) {
    lj_gc2_smr_read_leave(g);
    return LJ_ARENA_RESCUE_RETRY;
  }
  admission = lj_arena_rescue_enter(a);
  lj_gc2_smr_read_leave(g);
  return admission;
}

/* Exact-reclaimer counterpart for an allocation already owned by a detached
** list/body ticket. The exclusive writer pins both the shared small registry
** and its fallback TG spine, so no ordinary SMR admission is needed. */
static int gc2_small_registered_rescue_enter_reclaim_held(global_State *g,
						    GCArena *a)
{
  HugeTab *tab;
  int admission;
  if (!g || !a || !gc2_reclaim_tls_active(g))
    return LJ_ARENA_RESCUE_RETRY;
  tab = (HugeTab *)gc2_small_arena_tab_acq(g);
  if (tab) {
    LJHugeInfo hi;
    admission = lj_arena_hugetab_rescue_enter(tab, a, &hi);
    if (admission == LJ_ARENA_RESCUE_RETRY)
      return admission;
    if (hi.size != LJ_ARENA_SIZE) {
      if (gc2_mark_admission_counted(admission))
	lj_arena_rescue_leave(a);
      return LJ_ARENA_RESCUE_RETRY;
    }
    return admission;
  }
  if ((lj_arena_flags_acq(a) & LJ_AF_REGISTERED) == 0 ||
      lj_arena_owner_acq(a) == 0)
    return LJ_ARENA_RESCUE_RETRY;
  return lj_arena_rescue_enter(a);
}

/* The global reclaimer bit excludes new SMR readers, but only this per-thread
** record identifies its owner. It also holds the same-thread borrowed IDLE
** gate and nested-reader elision state. These are capabilities, not hints:
** sharing the record would authorize reclamation behind another OS thread.
**
** Windows embeds the record in the process-lifetime thread cell admitted by
** the TG lifecycle. A foreign thread without a cell gets NULL: ordinary SMR
** remains safe through the fully-counted fallback, while exclusive ownership
** fails closed and no GC hot path lazily allocates. POSIX uses native TLS. */
#if LJ_TARGET_WINDOWS
static LJ_AINLINE LJThrGC2TLS *gc2_tls_current(void)
{
  return lj_thr_gc2_tls_current();
}
#else
static LJ_TLS LJThrGC2TLS gc2_tls_state;

static LJ_AINLINE LJThrGC2TLS *gc2_tls_current(void)
{
  return &gc2_tls_state;
}
#endif

static LJ_AINLINE int gc2_reclaim_tls_active_state(const LJThrGC2TLS *tls,
						    global_State *g)
{
  return g && tls && tls->reclaim_g == g &&
	 lj_gc2_jit_reclaim_context_acq(g);
}

static LJ_AINLINE int gc2_smr_reader_tls_active_state(const LJThrGC2TLS *tls,
						       global_State *g)
{
  return g && tls && tls->smr_reader_g == g && tls->smr_reader_depth != 0;
}

static LJ_AINLINE int gc2_reclaim_tls_active(global_State *g)
{
  return gc2_reclaim_tls_active_state(gc2_tls_current(), g);
}

static LJ_AINLINE int gc2_smr_reader_tls_active(global_State *g)
{
  return gc2_smr_reader_tls_active_state(gc2_tls_current(), g);
}

static LJ_AINLINE int gc2_reclaim_tls_enter_state(LJThrGC2TLS *tls,
						   global_State *g)
{
  if (!g || !tls || tls->reclaim_g != NULL)
    return 0;
  tls->reclaim_g = g;
  return 1;
}

static LJ_AINLINE void gc2_reclaim_tls_leave_state(LJThrGC2TLS *tls,
						    global_State *g)
{
  if (LJ_UNLIKELY(!tls || tls->reclaim_g != g)) {
    lj_assertG(0, "GC2 reclaimer TLS ownership mismatch");
    abort();
  }
  tls->reclaim_g = NULL;
}

static void gc2_reclaim_tls_leave(global_State *g)
{
  gc2_reclaim_tls_leave_state(gc2_tls_current(), g);
}

static int gc2_tg_owns_huge_ptr(TGState *tg, const void *p)
{
  LJHugeInfo hi;
  return tg && lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
	 lj_arena_hugetab_lookup(&tg->huge, p, &hi) == 1;
}

static TGState *gc2_tg_for_registered_mem(global_State *g, const void *p)
{
  TGState *tg;
  GCArena *want;
  uint32_t cell;
  want = lj_arena_of(p);
  cell = lj_arena_cellof(p);
  if (cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS) {
    int registered = gc2_small_arena_registered(g, want, NULL);
    if (registered > 0) {
      uint32_t owner_tid = lj_arena_owner_acq(want);
      TGState *mtg = G2TG(g);
      TGState *owner = mtg && lj_tg_tid_acq(mtg) == owner_tid ?
	mtg : lj_tg_find_owner(g, owner_tid);
      return owner ? owner : g->main_tg;
    }
    if (registered == 0)
      return NULL;
    /*
    ** Most registered memory is ordinary arena storage. Check trusted arena-list
    ** roots first without dereferencing the candidate arena base; huge-table
    ** lookup remains the fallback for large mappings and arbitrary stale words.
    */
    for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg))
      if (gc2_tg_owns_small_arena(tg, want))
	return tg;
    if (g->main_tg && gc2_tg_owns_small_arena(g->main_tg, want))
      return g->main_tg;
  }
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg))
    if (gc2_tg_owns_huge_ptr(tg, p))
      return tg;
  return g->main_tg && gc2_tg_owns_huge_ptr(g->main_tg, p) ?
	 g->main_tg : NULL;
}

int lj_gc2_mem_registered(global_State *g, const void *p)
{
  int registered;
  if (!g || !p)
    return 0;
  if (la_load32_acq(&g->allocf_arena) == 0)
    return 1;
  if (!lj_gc2_smr_read_try(g))
    return 0;
  registered = gc2_tg_for_registered_mem(g, p) != NULL;
  lj_gc2_smr_read_leave(g);
  return registered;
}

int lj_gc2_mem_registered_known(global_State *g, const void *p)
{
  GCArena *a;
  uint32_t cell;
  int known = 0;
  if (!g || !p)
    return 0;
  if (la_load32_acq(&g->allocf_arena) == 0)
    return 1;
  if (!lj_gc2_smr_read_try(g))
    return 0;
  if (!gc2_tg_for_registered_mem(g, p))
    goto out;
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    known = 1;
    goto out;
  }
  cell = lj_arena_cellof(p);
  known = cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS &&
	  lj_arena_owner_acq(a) != 0 && lj_arena_bm_get(a->block, cell);
out:
  lj_gc2_smr_read_leave(g);
  return known;
}

int lj_gc2_mem_registered_known_reclaim_held(global_State *g, const void *p)
{
  GCArena *a;
  uint32_t cell, flags;
  if (!g || !p)
    return 0;
  if (la_load32_acq(&g->allocf_arena) == 0)
    return 1;
  /* The typed context proves smr_reclaiming plus either IDLE/worker0 or
  ** SWEEP/worker-owned authority. The TLS marker is the non-transferable proof
  ** that this caller won that exact CAS rather than merely observing it. */
  if (!gc2_reclaim_tls_active(g))
    return 0;
  /* The caller's retained list/body ticket makes this header dereference legal.
  ** Avoid the TG/registry walk entirely: the exclusive reclaimer pins retired
  ** registry metadata, while exact object/side-storage ownership pins p. */
  a = lj_arena_of(p);
  flags = lj_arena_flags_acq(a);
  if ((flags & LJ_AF_HUGE_MAGIC) == LJ_AF_HUGE_MAGIC)
    return lj_arena_owner_acq(a) != 0;
  cell = lj_arena_cellof(p);
  return (flags & LJ_AF_REGISTERED) != 0 &&
	 lj_arena_owner_acq(a) != 0 &&
	 cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS &&
	 lj_arena_bm_get(a->block, cell);
}

static void gc2_clear_marks(TGState *tg)
{
  if (tg && lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL)) {
    lj_arena_alloc_clear_marks(&tg->alloc);
    if (lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      lj_arena_hugetab_clear_marks(&tg->huge);
  }
}

static void gc2_clear_marks_all(global_State *g)
{
  TGState *tg;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg))
    gc2_clear_marks(tg);
}

typedef struct GC2RootCycleGuard {
  const void *anchor;
  uint64_t power;
  uint64_t length;
} GC2RootCycleGuard;

static LJ_AINLINE void gc2_root_cycle_guard_init(GC2RootCycleGuard *guard,
						  const void *head)
{
  guard->anchor = head;
  guard->power = 1;
  guard->length = 0;
}

/* Brent cycle detection piggybacks on an admitted forward walk. This removes
** arbitrary count caps without allowing a corrupt publication cycle to trap a
** root-handshake participant forever. */
static LJ_AINLINE int gc2_root_cycle_guard_step(GC2RootCycleGuard *guard,
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

static LJ_NOINLINE void gc2_mark_strtab_mem(global_State *g)
{
  StrTabHdr *hdr, *nexthdr;
  StrCanonHdr *qhdr, *nextqhdr;
  StrBodyRetire *ret, *nextret;
  GC2RootCycleGuard guard;
  if (!g)
    return;
  hdr = lj_str_tabh_acq(g);
  if (hdr && !lj_gc2_mem_registered(g, hdr))
    gc2_root_scan_retry(g);
  else if (hdr)
    lj_gc2_markmem(g, hdr);
  hdr = lj_str_retired_head_acq(g);
  gc2_root_cycle_guard_init(&guard, hdr);
  while (hdr != NULL && lj_gc2_mem_registered(g, hdr)) {
    nexthdr = lj_str_retired_next_acq(hdr);
    lj_gc2_markmem(g, hdr);
    hdr = nexthdr;
    if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, hdr))) {
      gc2_root_scan_retry(g);
      break;
    }
  }
  if (LJ_UNLIKELY(hdr != NULL && !lj_gc2_mem_registered(g, hdr)))
    gc2_root_scan_retry(g);
  qhdr = lj_str_qtabh_acq(g);
  if (qhdr && !lj_gc2_mem_registered(g, qhdr))
    gc2_root_scan_retry(g);
  else if (qhdr)
    lj_gc2_markmem(g, qhdr);
  qhdr = lj_str_qretired_head_acq(g);
  gc2_root_cycle_guard_init(&guard, qhdr);
  while (qhdr != NULL && lj_gc2_mem_registered(g, qhdr)) {
    nextqhdr = lj_str_qretired_next_acq(qhdr);
    lj_gc2_markmem(g, qhdr);
    qhdr = nextqhdr;
    if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, qhdr))) {
      gc2_root_scan_retry(g);
      break;
    }
  }
  if (LJ_UNLIKELY(qhdr != NULL && !lj_gc2_mem_registered(g, qhdr)))
    gc2_root_scan_retry(g);
  ret = lj_str_sweep_pending_acq(g);
  if (ret && !lj_gc2_mem_registered(g, ret)) {
    gc2_root_scan_retry(g);
  } else if (ret) {
    lj_gc2_markmem(g, ret);
    if ((la_load32_acq(&ret->status) &
	 (LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_BODY_OWNED)) ==
	(LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_BODY_OWNED)) {
      GCstr *s = lj_str_body_retired_str_acq(ret);
      if (s && !lj_gc2_mem_registered(g, s)) {
	gc2_root_scan_retry(g);
      } else if (s) {
	if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
	  (void)lj_gc2_preserve_sweep_root(g, obj2gco(s));
	else
	  (void)lj_gc2_markobj(g, obj2gco(s));
      }
    }
  }
  /* Side records are ownership metadata. Stage A/B deliberately retain every
  ** authoritative quarantine body until QCOMMIT is implemented; this is not a
  ** liveness sample authorizing unlink/free. Later close stages replace this
  ** conservative mark with their separate physical-quarantine proof. */
  ret = lj_str_body_retired_head_acq(g);
  gc2_root_cycle_guard_init(&guard, ret);
  while (ret != NULL && lj_gc2_mem_registered(g, ret)) {
    uint32_t status = la_load32_acq(&ret->status);
    nextret = lj_str_body_retired_next_acq(ret);
    lj_gc2_markmem(g, ret);
    if ((status & (LJ_STR_CANONREC_Q_LINKED|
		   LJ_STR_CANONREC_BODY_OWNED)) ==
	(LJ_STR_CANONREC_Q_LINKED|LJ_STR_CANONREC_BODY_OWNED)) {
      GCstr *s = lj_str_body_retired_str_acq(ret);
      if (s && !lj_gc2_mem_registered(g, s)) {
	gc2_root_scan_retry(g);
      } else if (s) {
	if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
	  (void)lj_gc2_preserve_sweep_root(g, obj2gco(s));
	else
	  (void)lj_gc2_markobj(g, obj2gco(s));
      }
    }
    ret = nextret;
    if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, ret))) {
      gc2_root_scan_retry(g);
      break;
    }
  }
  if (LJ_UNLIKELY(ret != NULL && !lj_gc2_mem_registered(g, ret)))
    gc2_root_scan_retry(g);
}

static LJ_NOINLINE void gc2_mark_tab_retired_mem(global_State *g)
{
  TabNodeRetire *ret, *nextret;
  TabArrayRetire *aret, *nextaret;
  GC2RootCycleGuard guard;
  /*
  ** The SMR reader held by callers keeps registered retire records stable while
  ** their payload pointer and next link are read. Every published node must
  ** therefore remain registered; an invalid node rejects this root snapshot
  ** instead of becoming a silent end-of-list sentinel.
  */
  ret = (TabNodeRetire *)la_loadptr_acq(
    (void *const *)&g->tab.retired_nodes);
  gc2_root_cycle_guard_init(&guard, ret);
  while (ret != NULL && lj_gc2_mem_registered(g, ret)) {
    nextret = lj_tab_node_retired_next_acq(ret);
    (void)lj_gc2_markmem(g, ret);
    /*
    ** The armed bit controls when retired storage can be freed. It must not
    ** control marking: resize links records before the new generation is fully
    ** published, and transient snapshots can still point at that old storage.
    */
    lj_gc2_markmem(g, lj_tab_node_hdrw(lj_tab_node_retired_node_acq(ret)));
    ret = nextret;
    if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, ret))) {
      gc2_root_scan_retry(g);
      break;
    }
  }
  if (LJ_UNLIKELY(ret != NULL && !lj_gc2_mem_registered(g, ret)))
    gc2_root_scan_retry(g);
  aret = lj_tab_array_retired_head_acq(g);
  gc2_root_cycle_guard_init(&guard, aret);
  while (aret != NULL && lj_gc2_mem_registered(g, aret)) {
    nextaret = lj_tab_array_retired_next_acq(aret);
    (void)lj_gc2_markmem(g, aret);
    /*
    ** Keep the array payload marked even before the retire record is armed;
    ** arming only makes the record eligible for epoch-based reclamation.
    */
    lj_gc2_markmem(g, lj_tab_array_hdrw(lj_tab_array_retired_array_acq(aret)));
    aret = nextaret;
    if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, aret))) {
      gc2_root_scan_retry(g);
      break;
    }
  }
  if (LJ_UNLIKELY(aret != NULL && !lj_gc2_mem_registered(g, aret)))
    gc2_root_scan_retry(g);
}

static int gc2_root_spine_admit(global_State *g, GCobj *o,
				 GC2MarkScope *scope)
{
  GCobj *th;
  uint32_t gct;
  gc2_mark_scope_init(scope);
  if (LJ_UNLIKELY(!g || !o || !scope))
    return 0;
  th = gcref_acq(*mainthread_ref(g));
  if (!th || th->gch.gct != ~LJ_TTHREAD || o != th) {
    th = gcref_acq(*vmthread_ref(g));
    if ((!th || th->gch.gct != ~LJ_TTHREAD || o != th) &&
	(LJ_UNLIKELY(!gc2_observed_obj_valid_scoped(
	   g, o, &gct, scope)) || gct == (uint32_t)~LJ_TSTR)) {
	gc2_mark_scope_leave(scope);
      return 0;
    }
  }
  return 1;
}

/* Load the intrusive successor while current is admitted, acquire successor's
** own exact scope, and only then release current. Embedded main/vm threads use
** their state-lifetime exemption; huge root MEMBER is the mapping ticket. */
static int gc2_root_spine_handoff(global_State *g, GCobj *o,
				   GC2MarkScope *scope, GCobj **nextp,
				   GC2MarkScope *nextscope)
{
  GCobj *next = lj_obj_gcw_acq(o);
  gc2_mark_scope_init(nextscope);
  if (next && next != o && !gc2_root_spine_admit(g, next, nextscope)) {
    gc2_mark_scope_leave(scope);
    return 0;
  }
  gc2_mark_scope_leave(scope);
  *nextp = next;
  return 1;
}

#if LJ_HASJIT
static GCtrace *gc2_traceref_safe(global_State *g, TraceNo traceno)
{
  return traceref_safe(G2J(g), traceno);
}

static int gc2_trace_geometry_valid(GCtrace *T)
{
  IRIns *irbase;
  SnapShot *snap;
  SnapEntry *snapmap;
  IRRef nins, nk;
  SnapNo nsnap;
  MSize nsnapmap;
  const MSize snapmap_per_snap =
    (MSize)LJ_MAX_JSLOTS + (MSize)LJ_STACK_EXTRA + 32u;
  if (!T || !checkptrGC(T) || T->gct != (uint32_t)~LJ_TTRACE)
    return 0;
  nins = trace_nins_acq(T);
  nk = trace_nk_acq(T);
  nsnap = trace_nsnap_acq(T);
  nsnapmap = trace_nsnapmap_acq(T);
  if (nins < REF_BASE || nins > 0xffffu || nk > REF_BIAS || nk > nins)
    return 0;
  if ((nsnap == 0 && nsnapmap != 0) ||
      (nsnap != 0 && nsnapmap > (MSize)nsnap * snapmap_per_snap))
    return 0;
  irbase = trace_ir_acq(T);
  if (nk < REF_TRUE && (!irbase || !checkptrGC(&irbase[nk])))
    return 0;
  snap = trace_snap_acq(T);
  snapmap = trace_snapmap_acq(T);
  if (nsnap == 0)
    return 1;
  return snap && snapmap && checkptrGC(snap) && checkptrGC(snapmap);
}

static void gc2_traverse_trace(global_State *g, GCtrace *T);
static int gc2_mark_trace_root_status(global_State *g, TraceNo traceno)
{
  GCtrace *T;
  if (traceno == 0)
    return 1;
  T = gc2_traceref_safe(g, traceno);
  if (!T)
    return 0;
  return gc2_mark_thread_root_obj_status(g, obj2gco(T));
}

static int gc2_mark_trace_root(global_State *g, TraceNo traceno)
{
  return traceno != 0 && gc2_mark_trace_root_status(g, traceno);
}

int lj_gc2_mark_trace_slot_status(global_State *g, uint32_t traceno)
{
  return gc2_mark_trace_root_status(g, (TraceNo)traceno);
}

void lj_gc2_mark_trace_slot(global_State *g, uint32_t traceno)
{
  (void)lj_gc2_mark_trace_slot_status(g, traceno);
}

static GCproto *gc2_trace_pc_proto_candidate(global_State *g, GCobj *o,
					     const BCIns *pc)
{
  GC2MarkScope scope;
  GCproto *pt;
  const BCIns *bc;
  uint32_t gct;
  /* This is a root-spine type probe, not a semantic root hit. Marking before
  ** checking the type kept every preceding FINREG cdata/table alive while
  ** searching for the one prototype containing pc. The spine supplies the
  ** lifetime observation; only the caller marks the matching prototype. */
  gc2_mark_scope_init(&scope);
  if (!pc || !gc2_observed_obj_valid_scoped(g, o, &gct, &scope) ||
      gct != (uint32_t)~LJ_TPROTO) {
    gc2_mark_scope_leave(&scope);
    return NULL;
  }
  pt = gco2pt(o);
  if (!gc2_valid_proto_for_traverse_held(pt)) {
    gc2_mark_scope_leave(&scope);
    return NULL;
  }
  bc = proto_bc(pt);
  pt = (pc >= bc && pc < bc + pt->sizebc) ? pt : NULL;
  gc2_mark_scope_leave(&scope);
  return pt;
}

int lj_gc2_test_trace_pc_proto_candidate(global_State *g, GCobj *o,
					 const BCIns *pc)
{
  return gc2_trace_pc_proto_candidate(g, o, pc) != NULL;
}

static void gc2_mark_active_cframe_proto_root(global_State *g, lua_State *L)
{
  if (!L || L->cframe == NULL)
    return;
  if (tvref(L->stack) != NULL) {
    TValue *bot = tvref(L->stack);
    TValue *max = tvref(L->maxstack);
    TValue *frame = L->base ? L->base - 1 : NULL;
    GCfunc *fn = NULL;
    GC2FrameScope scope;
    if (frame && frame > bot + LJ_FR2 && frame < max &&
	gc2_frame_func_valid(g, frame, &fn, NULL, &scope)) {
      if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
	(void)lj_gc2_trace_sweep_root(g, obj2gco(fn));
      else
	gc2_mark_thread_root_obj(g, obj2gco(fn));
      gc2_frame_scope_leave(&scope);
    }
  }
}

#endif

static int gc2_tg_list_contains(global_State *g, TGState *needle)
{
  TGState *tg;
  if (!g || !needle)
    return 0;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg))
    if (tg == needle)
      return 1;
  return 0;
}

static int gc2_mark_begin(global_State *g)
{
  TGState *tg;
  LJGC2ActivationSnap staged;
  uint32_t leader, mark_cycle;
  uint32_t forced_major, minor_requested, sweep_minor, roots_minor;
  int staged_mark;
  if (!g)
    return 0;
  if (!gc2_worker_claim(g))
    return 0;
  leader = gc2_cycle_leader_acq(g);
  if (gc2_phase_acq(g) != LJ_GC2_IDLE || leader == 0 ||
      leader == LJ_THREAD_GCSCAN) {
    /* GCSCAN is the close/abort sentinel. In particular, do not consume it in
    ** the short legacy-IDLE/typed-active reset window and misdiagnose that
    ** conservative mismatch as a corrupt next-cycle source. */
    gc2_worker_release(g);
    return 0;
  }
  /* Close native entry before staging typed MARK or consuming any request,
  ** force-major bit, counter, mark or queue state. The SC fence pairs with the
  ** x64 entry publication/recheck. If a trace is still active (including a
  ** peer blocked in FFI), leave the exact request intact and return: XPOLL or
  ** the trace allocation check exits asynchronously, and a later owner retries
  ** from unchanged IDLE state. */
  gc2_jit_phase_gate_close(g);
  if (lj_tg_any_jit_active(g)) {
    gc2_worker_release(g);
    return 0;
  }
  /* An IDLE retire owner can publish smr_reclaiming after this worker's first
  ** claim check but before the worker closes native entry. It owns that close
  ** only after winning its 1->0 CAS. Do not stage typed MARK or reopen its
  ** gate: leave the exact request intact, release the worker token, and let
  ** the reclaimer either finish or roll its own closure back. */
  if (gc2_smr_reclaiming_acq(g) != 0) {
    gc2_worker_release(g);
    return 0;
  }
  if (gc2_phase_acq(g) != LJ_GC2_IDLE ||
      gc2_cycle_leader_acq(g) != leader) {
    if (gc2_phase_acq(g) == LJ_GC2_IDLE && gc2_cycle_leader_acq(g) == 0)
      gc2_jit_phase_gate_open_idle(g);
    gc2_worker_release(g);
    return 0;
  }
  /* Stage the veto-only typed mirror while the nonzero request token excludes
  ** every CAS-owned close sentinel. Recheck it before consuming requests,
  ** changing counters, or touching queue state so a failed admission can
  ** exact-rollback without leaving partial cycle-start effects. */
  staged_mark = gc2_activation_stage_mark(g, &staged);
  if (!gc2_activation_mark_recheck(g, leader,
                                    staged_mark ? &staged : NULL)) {
    if (gc2_phase_acq(g) == LJ_GC2_IDLE && gc2_cycle_leader_acq(g) == 0)
      gc2_jit_phase_gate_open_idle(g);
    gc2_worker_release(g);
    lj_gc2_worker_wake(g);
    return 0;
  }
  tg = G2TG(g);
  forced_major = gc2_force_major_xchg_acqrel(g, 0);
  /*
  ** Cycle zero is reserved as the prototype-scan reset value. Make the
  ** uint32_t wrap cycle major so all survivor marks are rebuilt while
  ** per-prototype deduplication is disabled for that one cycle.
  */
  if (gc2_cycle_acq(g) == ~(uint32_t)0)
    forced_major = 1;
  minor_requested = !forced_major && gc2_generational_acq(g) != 0;
  sweep_minor = minor_requested &&
    gc2_minor_sweep_enabled_acq(g) != 0;
  roots_minor = sweep_minor &&
    gc2_minor_roots_enabled_acq(g) != 0;
  gc2_cycle_minor_requested_rel(g, minor_requested);
  gc2_cycle_sweep_minor_rel(g, sweep_minor);
  gc2_cycle_roots_minor_rel(g, roots_minor);
  if (minor_requested)
    gc2_minor_cycle_requests_add(g, 1);
  if (minor_requested && !sweep_minor)
    gc2_minor_sweep_deferred_add(g, 1);
  if (minor_requested && !roots_minor)
    gc2_minor_roots_deferred_add(g, 1);
  if (roots_minor)
    gc2_minor_cycle_starts_add(g, 1);
  else
    gc2_major_cycle_starts_add(g, 1);
  /*
  ** A forced/late-preserve abort deliberately carries exact SSB and grey
  ** references into the next cycle.  The LJ_GC_NEEDSCAN bit on an uncounted
  ** payload container is only queue deduplication, however: it has no cycle
  ** tag and cannot prove that the carried queue still contains this exact
  ** object.  Clear stale FUNC/PROTO/UPVAL/UDATA/TRACE dedupe while the worker
  ** token excludes queue consumers and phase is still IDLE.  Current-cycle
  ** root discovery can then publish a fresh traversal after mark reset.
  **
  ** Do not clear TABLE or THREAD here.  Their NEEDSCAN state has an associated
  ** pending counter/owner handoff that must be reconciled by its own protocol.
  */
  (void)gc2_clear_uncounted_needscan_all(g);
  /*
  ** Live published traces are reached from prototypes, trace links and active
  ** TG vmstates; gc2_scan_jit_roots() separately preserves the token-owned
  ** in-flight trace and retired metadata. Never flush traces or wait for the
  ** recorder token at cycle start: trace compilation allocates, so doing so can
  ** abort every recording attempt, and a stalled recorder would block GC
  ** progress. The EXIT_TRACES handshake below quiesces executable trace users
  ** before the root snapshot; VM JLOOP entry stays closed until the cycle is
  ** IDLE again.
  */
  /* Publish MARK while retaining the exact request as a phase gate through all
  ** mark resets and barrier initialization. A close actor can claim only zero,
  ** so it cannot publish IDLE underneath this still-running initializer. */
  gc2_phase_rel(g, LJ_GC2_MARK);
  if (tg != NULL && !gc2_tg_list_contains(g, tg))
    lj_tg_attach(g, tg);
  mark_cycle = gc2_cycle_inc_acqrel(g);
  (void)gc2_thread_scan_cycle_inc_acqrel(g);
  if (leader)
    gc2_cycle_starts_add(g, 1);
  gc2_marks_this_round_store_rlx(g, 0);
  gc2_mark_root_scanned_rel(g, 0);
  /* Idle remembered entries and abort-preserved work are conservative roots
  ** for this cycle. Never walk/reset live SSB or grey storage at start: a
  ** FLUSH acknowledgement can resume its mutator immediately, while the
  ** worker token below keeps consumers from detaching the old chain during
  ** mark reset. The ordinary bounded workers consume it after publication. */
  (void)lj_tg_reclaim_dead(g);
  if (gc2_grey_capacity_acq(g) == 0)
    (void)gc2_grey_grow(g);
  gc2_weak_reset(g);
  gc2_finclaim_reset(g);
  gc2_reset_alloc_trigger(g);
  /* Minor cycles keep old black marks; remembered SSB entries still traverse. */
  if (!sweep_minor)
    gc2_clear_marks_all(g);
  lj_gc2_handshake(g, LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK|
		   LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_REDISPATCH|
		   LJ_GC2_HS_FLUSH_SSB);
  {
    uint32_t expect = leader;
    if (LJ_UNLIKELY(!gc2_cycle_leader_cas(g, &expect, 0))) {
      gc2_activation_pin_no_reclaim(g);
    } else {
      /* The cycle generation is the C-owned proof that every activation action
      ** was acknowledged before native MARK entry became possible. The entry
      ** gate stayed closed while the request token was nonzero. */
      gc2_jit_mark_resume_rel(g, mark_cycle);
      gc2_jit_mark_auto_yield_rel(g, GC2_JIT_MARK_AUTO_YIELDS);
      gc2_jit_phase_gate_open_mark(g, 1);
    }
  }
  gc2_worker_release(g);
  lj_gc2_worker_wake(g);  /* 05 section 5.6.3 parked worker scheduler. */
  return 1;
}

void lj_gc2_mark_begin(global_State *g)
{
  /* This exported entry is the explicit/test cycle-start hook. Ordinary
  ** pacing installs the request token before calling the internal driver, but
  ** callers must not depend on incidental allocation debt having done so. */
  if (g && gc2_phase_acq(g) == LJ_GC2_IDLE &&
      gc2_cycle_leader_acq(g) == 0)
    (void)gc2_request_cycle_start(g, G2TG(g), 0);
  (void)gc2_mark_begin(g);
}

void lj_gc2_force_major(global_State *g)
{
  if (g)
    gc2_force_major_rel(g, 1);
}

static uint32_t gc2_idle_barrier_actions(global_State *g, int flush_ssb)
{
  uint32_t actions = LJ_GC2_HS_ALLOC_WHITE;
  if (flush_ssb)
    actions |= LJ_GC2_HS_FLUSH_SSB;
  if (gc2_generational_acq(g))
    actions |= LJ_GC2_HS_ENABLE_BARRIER;
  else
    actions |= LJ_GC2_HS_DISABLE_BARRIER;
  return actions;
}

static uint32_t gc2_idle_transition_handshake(global_State *g,
					       uint32_t actions)
{
  LJThrGC2TLS *tls = gc2_tls_current();
  uint32_t result;
  if (!g)
    return 0;
  /* Callers publish the IDLE phase before invoking this mandatory close. A
  ** missing per-thread capability therefore cannot be reported as an
  ** optional zero result without leaving the phase half-transitioned. Every
  ** valid controller/worker path is admitted before it can get here. */
  if (LJ_UNLIKELY(!tls)) {
    lj_assertG(0, "GC2 IDLE transition lacks an admitted thread cell");
    abort();
  }
  if (LJ_UNLIKELY(gc2_phase_acq(g) != LJ_GC2_IDLE ||
		  gc2_cycle_leader_acq(g) != LJ_THREAD_GCSCAN ||
		  gc2_jit_phase_gate_acq(g) != 0 ||
		  tls->idle_transition_gate_g != NULL)) {
    lj_assertG(0, "invalid borrowed IDLE transition gate");
    abort();
  }
  tls->idle_transition_gate_g = g;
  result = lj_gc2_handshake(g, actions);
  tls->idle_transition_gate_g = NULL;
  return result;
}

static void gc2_update_public_minor_gates(global_State *g)
{
  if (!g)
    return;
  /* b1.2 keeps generational remembered barriers active but deliberately
  ** defers physical minor sweep/root elision. Several g-only non-table
  ** mutation paths do not yet prove an IDLE parent-qualified remembered
  ** publication, so retaining old marks while trusting an old payload token
  ** would be unsafe. Every requested minor therefore runs as a major until
  ** that audit is complete. */
  gc2_minor_sweep_enabled_rel(g, 0);
  gc2_minor_roots_enabled_rel(g, 0);
}

static uint32_t gc2_ratio_pct(uint64_t num, uint64_t den)
{
  if (num == 0 || den == 0)
    return 0;
  if (num >= den)
    return 100;
  while (num > ~(uint64_t)0 / 100u) {
    num >>= 1;
    den = (den + 1u) >> 1;
    if (den == 0 || num >= den)
      return 100;
  }
  return (uint32_t)((num * 100u) / den);
}

static void lj_gc2_update_minor_survival_policy(global_State *g, uint64_t live)
{
  uint64_t base, alloc, survived = 0;
  uint32_t pct = 0, threshold;
  int minor;
  if (!g)
    return;
  base = gc2_minor_survival_base_live_acq(g);
  alloc = lj_gc2_cycle_alloc_load(g);
  minor = gc2_cycle_sweep_minor_acq(g) != 0;
  if (minor && live > base && alloc != 0) {
    survived = live - base;
    pct = gc2_ratio_pct(survived, alloc);
  }
  gc2_minor_survival_bytes_rel(g, survived);
  gc2_minor_survival_pct_rel(g, pct);
  gc2_minor_survival_base_live_rel(g, live);
  threshold = gc2_minor_survival_threshold_pct_acq(g);
  if (threshold == 0)
    threshold = LJ_GC2_MINOR_SURVIVAL_MAJOR_PCT;
  if (minor && pct >= threshold &&
      gc2_generational_acq(g) != 0) {
    gc2_minor_survival_major_requests_add(g, 1);
    lj_gc2_force_major(g);
  }
}

#if defined(lj_gc2_c) || defined(LJ_GC2_TEST_HELPERS) || defined(LUA_USE_ASSERT)
void lj_gc2_test_update_minor_survival_policy(global_State *g, uint64_t live)
{
  lj_gc2_update_minor_survival_policy(g, live);
}
#endif

void lj_gc2_set_generational(global_State *g, int enabled)
{
  uint32_t want = enabled ? 1u : 0u;
  if (!g)
    return;
  if (gc2_generational_acq(g) == want)
    return;
  gc2_generational_rel(g, want);
  if (want)
    lj_gc2_force_major(g);  /* First generational cycle establishes old marks. */
  else {
    gc2_force_major_rel(g, 0);
    gc2_minor_survival_pct_rel(g, 0);
    gc2_minor_survival_bytes_rel(g, 0);
    gc2_update_public_minor_gates(g);
  }
  if (gc2_phase_acq(g) == LJ_GC2_IDLE)
    lj_gc2_handshake(g, gc2_idle_barrier_actions(g, 0));
}

int lj_gc2_mark_phase_active(global_State *g)
{
  return g && gc2_phase_acq(g) == LJ_GC2_MARK;
}

/*
** The phase/minor-root bits are a GC2-owned protocol. Non-GC2 code asks these
** helpers so the bridge policy stays with the scanner that defines the cycle.
*/
int lj_gc2_minor_roots_active(global_State *g)
{
  return g && gc2_cycle_roots_minor_acq(g) != 0;
}

int lj_gc2_minor_roots_skip_bridge_mark(global_State *g)
{
  return lj_gc2_mark_phase_active(g) && lj_gc2_minor_roots_active(g);
}

void lj_gc2_mark_to_weak(global_State *g)
{
  uint32_t expect = LJ_GC2_MARK;
  if (!g || gc2_phase_acq(g) != LJ_GC2_MARK)
    return;
  if (gc2_jit_phase_gate_acq(g) != 0)
    gc2_jit_phase_gate_close(g);
  if (gc2_jit_recorder_active(g)) {
    lj_trace_abort(g);
    gc2_root_scan_retry(g);
    return;
  }
  if (lj_tg_any_jit_active(g) || !gc2_recovery_empty(g) ||
      !gc2_phase_gate_try(g))
    return;
  if (gc2_jit_phase_gate_acq(g) != 0 || lj_tg_any_jit_active(g) ||
      gc2_jit_recorder_active(g) ||
      !gc2_recovery_empty(g) ||
      !gc2_phase_cas(g, &expect, LJ_GC2_WEAK)) {
    gc2_phase_gate_release(g);
    return;
  }
  /* WEAK cannot reclaim, so the exact legacy CAS may safely lead its mirror. */
  gc2_activation_mirror_edge(g, LJ_GC2_MARK, LJ_GC2_WEAK);
  gc2_jit_mark_resume_rel(g, 0);
  gc2_jit_mark_auto_yield_rel(g, 0);
  gc2_jit_mark_yield_until_ns_rel(g, 0);
  gc2_mark_root_scanned_rel(g, 0);
  gc2_mark_to_weak_add(g, 1);
  gc2_phase_gate_release(g);
  lj_gc2_worker_wake(g);  /* 05 section 5.6.3 parked worker scheduler. */
}

static void gc2_weak_to_sweep_drain_boundary(global_State *g, lua_State *L)
{
  TGState *tg;
  uint32_t round;
  if (!g)
    return;
  tg = L ? L2TG(L) : G2TG(g);
  for (round = 0; round < LJ_GC2_ROOT_RETRY_ROUNDS; round++) {
    if (tg)
      (void)lj_gc2_flush_ssb(g, tg);
    (void)gc2_drain_ssb_owned(g);
    if (gc2_thread_scan_needscan_pending_acq(g) != 0 ||
	gc2_table_rescan_pending_acq(g) != 0) {
      /*
      ** A busy owned lua_State removed from the grey queue is represented by
      ** NEEDSCAN until its owner acknowledges a root scan. The weak-to-sweep
      ** boundary is the last point before arenas can reclaim unmarked cells, so
      ** close those handoffs here and then drain the stack marks they publish.
      */
      (void)lj_gc2_handshake(g,
	LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_FLUSH_SSB);
      continue;
    }
    if (lj_gc2_ssb_empty(g) && gc2_recovery_empty(g) &&
	!lj_tg_any_jit_active(g) &&
	!gc2_weak_owned_peer_active(g))
      return;
    /* Post-transition mutations are protected by the SWEEP rescue barrier.
    ** Rotate their owner-local SSBs without manufacturing another semantic
    ** root snapshot merely because a resumed stack changed its dirty epoch. */
    (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  }
  /* A bounded miss leaves SWEEP's semantic bridge closed. The worker wake at
  ** the caller retries after the foreign owner or registry writer advances. */
}

void lj_gc2_weak_to_sweep(global_State *g, lua_State *L)
{
  uint32_t expect = LJ_GC2_WEAK;
  if (!g || lj_tg_any_jit_active(g) || !gc2_recovery_empty(g))
    return;
  if (!gc2_worker_claim(g))
    return;  /* Serialize transition handshakes with sweep prepare/close. */
  if (lj_tg_any_jit_active(g) || gc2_phase_acq(g) != LJ_GC2_WEAK ||
      !gc2_weak_mark_closed_acq(g) ||
      gc2_weak_root_scanned_acq(g) != 1 ||
      gc2_weak_owned_peer_active(g) || !lj_gc2_ssb_empty(g) ||
      !gc2_recovery_empty(g) ||
      gc2_thread_scan_needscan_pending_acq(g) != 0 ||
      gc2_table_rescan_pending_acq(g) != 0 ||
      gc2_marks_this_round_acq(g) != 0) {
    /* A writer/root publication can land between weak_complete() releasing its
    ** owner token and this transition claim. Reopen closure rather than letting
    ** a stale success cross the WEAK->SWEEP LP. */
    if (gc2_phase_acq(g) == LJ_GC2_WEAK)
      gc2_weak_mark_closed_rel(g, 0);
    gc2_worker_release(g);
    return;
  }
  /* Exclude every preserve/forced close through the complete post-CAS SWEEP
  ** initialization. A preserve publisher which loses this gate rescues its
  ** exact object into the still-active phase below. */
  if (!gc2_phase_gate_try(g)) {
    gc2_worker_release(g);
    return;
  }
  if (lj_tg_any_jit_active(g) || gc2_phase_acq(g) != LJ_GC2_WEAK ||
      !gc2_weak_mark_closed_acq(g) ||
      gc2_weak_root_scanned_acq(g) != 1 ||
      gc2_weak_owned_peer_active(g) || !lj_gc2_ssb_empty(g) ||
      !gc2_recovery_empty(g) ||
      gc2_thread_scan_needscan_pending_acq(g) != 0 ||
      gc2_table_rescan_pending_acq(g) != 0 ||
      gc2_marks_this_round_acq(g) != 0) {
    if (gc2_phase_acq(g) == LJ_GC2_WEAK)
      gc2_weak_mark_closed_rel(g, 0);
    gc2_phase_gate_release(g);
    gc2_worker_release(g);
    return;
  }
  /* Typed SWEEP_OPEN must be visible before legacy SWEEP can enable any of its
  ** existing reclaim paths. It remains a veto, not positive permission. */
  gc2_activation_mirror_edge(g, LJ_GC2_WEAK, LJ_GC2_SWEEP);
  if (!gc2_phase_cas(g, &expect, LJ_GC2_SWEEP)) {
    /* A CAS-owned preserve abort may have reset legacy phase first and either
    ** still own the typed reset or have completed it. Its exact reset consumes
    ** any staged SWEEP_OPEN state; other failures are genuine mismatches. */
    if (expect != LJ_GC2_IDLE ||
        !gc2_activation_forward_abort_defer(g))
      gc2_activation_pin_no_reclaim(g);
    gc2_phase_gate_release(g);
    gc2_worker_release(g);
    return;
  }
  gc2_sweep_bridge_ready_rel(g, 0);
  gc2_sweep_root_scanned_rel(g, 0);
  gc2_sweep_root_cursor_rel(g, lj_gc_root_ref(g));
  gc2_sweep_root_done_rel(g, 0);
  gc2_sweep_grace_needed_rel(g, 0);
  /* String identity reclamation is major-only until it has its own birth
  ** generation cutoff.  Publish pending work now, but the string subsystem
  ** does not claim or tag the header until the semantic bridge is ready. */
  lj_str_gc2_sweep_begin(g, gc2_cycle_sweep_minor_acq(g) == 0);
  gc2_weak_to_sweep_add(g, 1);
  lj_gc2_handshake(g, LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_FLUSH_SSB|
		   (gc2_cycle_sweep_minor_acq(g) ?
		    LJ_GC2_HS_ALLOC_WHITE : LJ_GC2_HS_ALLOC_BLACK));
  gc2_weak_to_sweep_drain_boundary(g, L);
  (void)lj_tg_reclaim_dead(g);
  gc2_phase_gate_release(g);
  gc2_worker_release(g);
  lj_gc2_worker_wake(g);  /* 05 section 5.6.3 parked worker scheduler. */
}

static TGState *gc2_finalizer_current_tg(global_State *g)
{
  TGState *tg = lj_thr_get_tg();
  if (!g || !tg || tg->gl == g)
    return tg;
  /* Missing TLS can be an unattached helper; only stale TLS can fallback. */
  return g->main_tg;
}

static uint32_t gc2_finalizer_current_owner(global_State *g)
{
  TGState *tg = gc2_finalizer_current_tg(g);
  uint32_t tid = tg ? lj_tg_tid_acq(tg) : 0;
  return tid != 0 ? tid : ~(uint32_t)0;
}

int lj_gc2_finalizer_owned_by_current(global_State *g)
{
  TGState *tg;
  uint32_t owner;
  if (!g)
    return 0;
  owner = gc2_finalizer_owner_acq(g);
  if (owner == 0)
    return 0;
  tg = gc2_finalizer_current_tg(g);
  return owner == (tg ? lj_tg_tid_acq(tg) : ~(uint32_t)0);
}

static GC2FinalizerNode *gc2_finalizer_node_new(global_State *g, GCobj *o,
						cTValue *fin)
{
  GC2FinalizerNode *node;
  if (!g || !o)
    return NULL;
  /* Producers may enqueue without owning the Lua allocator/TG. */
  node = (GC2FinalizerNode *)malloc(sizeof(GC2FinalizerNode));
  if (!node) {
    lj_assertG_(g, 0, "out of memory allocating finalizer queue node");
    abort();
  }
  gc2_finalizer_node_next_rel(node, NULL);
  gc2_finalizer_node_obj_rel(node, o);
  gc2_finalizer_node_fin_rel(node, fin);
  return node;
}

static void gc2_finalizer_node_free(global_State *g, GC2FinalizerNode *node)
{
  UNUSED(g);
  if (node)
    free(node);
}

static void gc2_finalizer_free_stack(global_State *g, GC2FinalizerNode *node)
{
  while (node) {
    GC2FinalizerNode *next = gc2_finalizer_node_next_acq(node);
    gc2_finalizer_node_free(g, node);
    node = next;
  }
}

static void gc2_finalizer_free_ring(global_State *g, GC2FinalizerNode *tail)
{
  GC2FinalizerNode *node;
  if (!tail)
    return;
  node = gc2_finalizer_node_next_acq(tail);
  gc2_finalizer_node_next_rel(tail, NULL);
  gc2_finalizer_free_stack(g, node);
}

static void lj_gc2_finalizer_enqueue_fin(global_State *g, GCobj *o,
					 cTValue *fin)
{
  GC2FinalizerNode *node;
  void *head;
  if (!g || !o)
    return;
  node = gc2_finalizer_node_new(g, o, fin);
  if (!node)
    return;
  do {
    head = gc2_finalizer_mpsc_acq(g);
    gc2_finalizer_node_next_rel(node, (GC2FinalizerNode *)head);
  } while (!gc2_finalizer_mpsc_cas(g, &head, node));
  gc2_finalizer_queued_add(g, 1);
  if (head == NULL)
    lj_gc2_worker_wake(g);  /* 05 section 5.8: finalizer work became visible. */
}

static void lj_gc2_finalizer_enqueue(global_State *g, GCobj *o)
{
  lj_gc2_finalizer_enqueue_fin(g, o, NULL);
}

static void lj_gc2_finalizer_drain_owned(global_State *g)
{
  GC2FinalizerNode *stack, *rev = NULL, *newtail = NULL, *oldtail;
  size_t n = 0;
  if (!g)
    return;
  lj_assertG(lj_gc2_finalizer_owned_by_current(g),
	     "gc2 finalizer drain requires owner");
  stack = (GC2FinalizerNode *)gc2_finalizer_mpsc_xchg_acqrel(g, NULL);
  while (stack) {
    GC2FinalizerNode *next = gc2_finalizer_node_next_acq(stack);
    if (newtail == NULL)
      newtail = stack;
    gc2_finalizer_node_next_rel(stack, rev);
    rev = stack;
    stack = next;
    n++;
  }
  if (!rev)
    return;
  oldtail = (GC2FinalizerNode *)gc2_finalizer_tail_acq(g);
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  if (gc2_finalizer_drain_test_pause_xchg_acqrel(g, 0) != 0) {
    gc2_finalizer_drain_test_paused_rel(g, 1);
    while (gc2_finalizer_drain_test_release_acq(g) == 0)
      gc2_peer_wait_no_l();
    gc2_finalizer_drain_test_paused_rel(g, 0);
  }
#endif
  if (oldtail) {
    GC2FinalizerNode *head = gc2_finalizer_node_next_acq(oldtail);
    gc2_finalizer_node_next_rel(newtail, head);
    gc2_finalizer_node_next_rel(oldtail, rev);
    gc2_finalizer_tail_rel(g, newtail);
  } else {
    gc2_finalizer_node_next_rel(newtail, rev);
    gc2_finalizer_tail_rel(g, newtail);
  }
  gc2_finalizer_mpsc_drained_add(g, n);
}

static void lj_gc2_finalizer_drain_l(lua_State *L, global_State *g)
{
  if (!g)
    return;
  lj_gc2_finalizer_enter_l(L, g);
  lj_gc2_finalizer_drain_owned(g);
  lj_gc2_finalizer_leave(g);
}

static void lj_gc2_finalizer_drain(global_State *g)
{
  lj_gc2_finalizer_drain_l(NULL, g);
}

static GC2FinalizerNode *gc2_finalizer_dequeue_node_owned(global_State *g,
						   TValue *fin,
						   int *has_fin)
{
  GC2FinalizerNode *tail, *node;
  GCobj *o;
  if (!g)
    return NULL;
  if (has_fin)
    *has_fin = 0;
  lj_assertG(lj_gc2_finalizer_owned_by_current(g),
	     "gc2 finalizer dequeue requires owner");
  tail = (GC2FinalizerNode *)gc2_finalizer_tail_acq(g);
  if (!tail) {
    lj_gc2_finalizer_drain_owned(g);
    tail = (GC2FinalizerNode *)gc2_finalizer_tail_acq(g);
    if (!tail)
      return NULL;
  }
  node = gc2_finalizer_node_next_acq(tail);
  o = node ? gc2_finalizer_node_obj_acq(node) : NULL;
  lj_assertG(o != NULL, "broken gc2 finalizer queue");
  if (!o)
    return NULL;
  if (has_fin)
    *has_fin = gc2_finalizer_node_fin_acq(node, fin);
  if (node == tail) {
    gc2_finalizer_tail_rel(g, NULL);
  } else {
    gc2_finalizer_node_next_rel(tail, gc2_finalizer_node_next_acq(node));
  }
  gc2_finalizer_node_next_rel(node, NULL);
  return node;
}

static void gc2_finalizer_dequeue_commit(global_State *g,
					 GC2FinalizerNode *node)
{
  gc2_finalizer_node_free(g, node);
  gc2_finalizer_dequeued_add(g, 1);
}

static GCobj *lj_gc2_finalizer_dequeue_owned(global_State *g,
					     TValue *fin, int *has_fin)
{
  GC2FinalizerNode *node =
    gc2_finalizer_dequeue_node_owned(g, fin, has_fin);
  GCobj *o = node ? gc2_finalizer_node_obj_acq(node) : NULL;
  if (node)
    gc2_finalizer_dequeue_commit(g, node);
  return o;  /* 05 section 5.8: GC2-owned finalizer queue bridge. */
}

static GCobj *lj_gc2_finalizer_dequeue(global_State *g)
{
  GCobj *o;
  if (!g)
    return NULL;
  lj_gc2_finalizer_enter(g);
  o = lj_gc2_finalizer_dequeue_owned(g, NULL, NULL);
  lj_gc2_finalizer_leave(g);
  return o;
}

/* Built-in dispatch could not acquire the object's intrusive-root claim. The
** callback and registration state are still untouched. */
#define GC2_FINALIZER_DISPATCH_DEFER	(-2)

static void gc2_finreg_queue_mark(global_State *g, GCobj *o);

/* Restore a tentatively detached callback at the owned FIFO head. The exact
** node/TValue and queue accounting remain unchanged; producers may continue
** publishing independently through the MPSC stack. */
static void gc2_finalizer_requeue_front_owned(global_State *g,
					      GC2FinalizerNode *node)
{
  GC2FinalizerNode *tail;
  lj_assertG(lj_gc2_finalizer_owned_by_current(g),
	     "gc2 finalizer retry requires owner");
  if (!node)
    return;
  tail = (GC2FinalizerNode *)gc2_finalizer_tail_acq(g);
  if (tail) {
    gc2_finalizer_node_next_rel(node, gc2_finalizer_node_next_acq(tail));
    gc2_finalizer_node_next_rel(tail, node);
  } else {
    gc2_finalizer_node_next_rel(node, node);
    gc2_finalizer_tail_rel(g, node);
  }
}

static int gc2_finalizer_dispatch_obj(lua_State *L, global_State *g, GCobj *o,
				      GC2FinalizerDispatchCtx *ctx,
				      cTValue *fin)
{
  GC2MarkScope scope;
  uint32_t gct;
  int rc;
  if (!ctx || !o)
    return 0;
  if (ctx->dispatch)
    return ctx->dispatch(L, g, o);
  if (LJ_UNLIKELY(!gc2_observed_obj_valid_scoped(g, o, &gct, &scope)))
    return 0;
#if LJ_HASFFI
  if (gct == (uint32_t)~LJ_TCDATA) {
    rc = lj_gc2_finreg_cdata_dispatch(L, g, o, fin);
    gc2_mark_scope_leave(&scope);
    return rc;
  }
#endif
  if (gct == (uint32_t)~LJ_TUDATA) {
    rc = lj_gc2_finreg_udata_dispatch(L, g, o, fin);
    gc2_mark_scope_leave(&scope);
    return rc;
  }
  gc2_mark_scope_leave(&scope);
  lj_assertG(0, "bad GC2 finalizer dispatch object");
  return 0;
}

static int lj_gc2_finalizer_dispatch_one(lua_State *L,
					 GC2FinalizerDispatchCtx *ctx)
{
  global_State *g;
  LJStateClaim claim;
  GC2FinalizerNode *node;
  GCobj *o;
  TValue fin;
  int has_fin;
  int rc;
  if (!L || !ctx)
    return 0;
  g = G(L);
  lj_assertG(lj_tg_jit_base(g) == NULL, "finalizer called on trace");
  if (!lj_state_resumeclaim(L, lj_thr_current_id(g), &claim))
    return 0;  /* 05 section 5.8: callback stack busy; leave queue intact. */
  if (!lj_gc2_finalizer_try_enter(g)) {
    lj_state_dropresumeclaim(&claim);
    return 0;
  }
  lj_gc2_finalizer_drain_owned(g);
  node = gc2_finalizer_dequeue_node_owned(g, &fin, &has_fin);
  o = node ? gc2_finalizer_node_obj_acq(node) : NULL;
  if (node == NULL || o == NULL) {
    lj_gc2_finalizer_leave(g);
    lj_state_dropresumeclaim(&claim);
    return 0;
  }
  rc = gc2_finalizer_dispatch_obj(L, g, o, ctx, has_fin ? &fin : NULL);
  if (rc == GC2_FINALIZER_DISPATCH_DEFER) {
    /* No registration bit, slot, preclaim, or callback was consumed. Put the
    ** exact callback back at the FIFO head before publishing semantic liveness
    ** for the retry. */
    gc2_finalizer_requeue_front_owned(g, node);
    gc2_finreg_queue_mark(g, o);
    lj_gc2_finalizer_leave(g);
    lj_state_dropresumeclaim(&claim);
    return 0;
  }
  gc2_finalizer_dequeue_commit(g, node);
  lj_gc2_finalizer_leave(g);
  lj_state_dropresumeclaim(&claim);
  return rc < 0 ? -1 : 1;
}

void lj_gc2_finalizer_dispatch_all(lua_State *L)
{
  GC2FinalizerDispatchCtx ctx;
  global_State *g;
  if (!L)
    return;
  ctx.dispatch = NULL;
  g = G(L);
  for (;;) {
    lj_gc2_finalizer_drain_l(L, g);
    if (!gc2_finalizer_queue_pending(g))
      break;
    if (!lj_gc2_finalizer_dispatch_one(L, &ctx))
      gc2_peer_wait_owned_l(L);
  }
}

static int gc2_finalizer_spawn_deferred(global_State *g);

static int lj_gc2_finalizer_step_ctx(lua_State *L,
				     GC2FinalizerDispatchCtx *ctx,
				     GCSize finalize_cost, GCSize *cost)
{
  global_State *g;
  if (cost)
    *cost = 0;
  if (!L || !ctx)
    return 0;
  g = G(L);
  if (gc2_finalizer_queue_pending(g)) {
    GCSize old, total;
    int finrc;
    if (lj_tg_any_jit_active(g)) {
      if (cost)
	*cost = LJ_MAX_MEM;
      return -1;  /* 05 section 5.8: do not run finalizers on trace. */
    }
    old = lj_gc_total_load(g);
    finrc = lj_gc2_finalizer_dispatch_one(L, ctx);
    if (finrc <= 0) {
      if (cost)
	*cost = LJ_MAX_MEM;
      return -1;  /* Busy owner or finalizer-spawn deferred GC. */
    }
    total = lj_gc_total_load(g);
    if (old >= total && g->gc.estimate > old - total)
      g->gc.estimate -= old - total;
    if (g->gc.estimate > finalize_cost)
      g->gc.estimate -= finalize_cost;
    if (cost)
      *cost = finalize_cost;
    return 1;
  }
  if (gc2_finalizer_spawn_deferred(g)) {
    if (cost)
      *cost = LJ_MAX_MEM;
    return -1;  /* Keep GC2 SWEEP open until the spawned TG exits. */
  }
  return 0;
}

int lj_gc2_finalizer_step(lua_State *L, GCSize finalize_cost, GCSize *cost)
{
  GC2FinalizerDispatchCtx ctx;
  ctx.dispatch = NULL;
  return lj_gc2_finalizer_step_ctx(L, &ctx, finalize_cost, cost);
}

int lj_gc2_finalizer_deferred(global_State *g)
{
  return gc2_finalizer_spawn_deferred(g);
}

static int lj_gc2_finalizer_try_enter(global_State *g)
{
  uint32_t owner, old;
  if (!g)
    return 0;
  owner = gc2_finalizer_current_owner(g);
  for (;;) {
    old = gc2_finalizer_active_acq(g);
    if (old != 0) {
      if (gc2_finalizer_owner_acq(g) != owner)
	return 0;  /* 05 section 5.8: peer finalizer dispatch backs off. */
      if (old == ~(uint32_t)0)
	return 0;
      if (gc2_finalizer_active_cas(g, &old, old + 1)) {
	gc2_finalizer_enters_add(g, 1);
	return 1;
      }
      continue;
    }
    if (gc2_finalizer_active_cas(g, &old, 1)) {
      gc2_finalizer_owner_rel(g, owner);
      gc2_finalizer_enters_add(g, 1);
      return 1;
    }
  }
}

static void gc2_peer_wait_no_l(void)
{
  (void)lj_thr_retry_yield(NULL);
}

static void gc2_peer_wait_l(lua_State *L)
{
  global_State *g;
  uint32_t active;
  if (!L) {
    gc2_peer_wait_no_l();
    return;
  }
  g = G(L);
  active = g ? gc2_worker_active_acq(g) : 0;
  if (active != 0) {
    /* The owner releases after a bounded batch. Sleep on that exact token
    ** instead of burning millions of retry-yields while GC workers and an
    ** explicit collector compete for the zero window. */
    lj_native_enter(L2TG(L));
    la_futex_wait(&g->gc2.worker_active, active, 1000000);
    (void)lj_native_leave(L);
    return;
  }
  (void)lj_thr_retry_yield(L);
}

static void gc2_peer_wait_owned_l(lua_State *L)
{
  TGState *tg;
  uint32_t tid;
  if (!L)
    goto no_l;
  tg = lj_thr_get_tg();
  if (!tg || tg->gl != G(L) || L2TG(L) != tg)
    goto no_l;
  tid = lj_tg_tid_acq(tg);
  if (lj_thr_id_is_owner(tid) &&
      lj_state_owner_acq(L) == tid) {
    gc2_peer_wait_l(L);
    return;
  }
no_l:
  gc2_peer_wait_no_l();
}

static void lj_gc2_finalizer_enter_l(lua_State *L, global_State *g)
{
  if (!g)
    return;
  while (!lj_gc2_finalizer_try_enter(g))
    gc2_peer_wait_owned_l(L);
}

static void lj_gc2_finalizer_enter(global_State *g)
{
  if (!g)
    return;
  while (!lj_gc2_finalizer_try_enter(g))
    gc2_peer_wait_no_l();
}

static void lj_gc2_finalizer_leave(global_State *g)
{
  uint32_t old;
  int wake_worker = 0;
  if (!g)
    return;
  for (;;) {
    old = gc2_finalizer_active_acq(g);
    lj_assertG(old != 0, "gc2 finalizer active underflow");
    if (old == 0)
      return;
    if (old == 1) {
      uint32_t expect = 1;
      if (gc2_finalizer_active_cas(g, &expect, ~(uint32_t)0)) {
	gc2_finalizer_owner_rel(g, 0);
	gc2_finalizer_active_rel(g, 0);
	wake_worker = gc2_finalizer_mpsc_acq(g) != NULL;
	break;  /* 05 section 5.8: close finalizer owner after last leave. */
      }
      continue;
    }
    if (old == ~(uint32_t)0) {
      gc2_peer_wait_no_l();
      continue;
    }
    if (gc2_finalizer_active_cas(g, &old, old - 1))
      break;  /* 05 section 5.8: nested owner leave. */
  }
  gc2_finalizer_leaves_add(g, 1);
  if (wake_worker)
    lj_gc2_worker_wake(g);  /* 05 section 5.8: owner release exposes work. */
}

int lj_gc2_test_finalizer_try_enter(global_State *g)
{
  return lj_gc2_finalizer_try_enter(g);
}

void lj_gc2_test_finalizer_enter(global_State *g)
{
  lj_gc2_finalizer_enter(g);
}

void lj_gc2_test_finalizer_leave(global_State *g)
{
  lj_gc2_finalizer_leave(g);
}

void lj_gc2_test_finalizer_drain_owned(global_State *g)
{
  lj_gc2_finalizer_drain_owned(g);
}

void lj_gc2_test_finalizer_drain(global_State *g)
{
  lj_gc2_finalizer_drain(g);
}

GCobj *lj_gc2_test_finalizer_dequeue(global_State *g)
{
  return lj_gc2_finalizer_dequeue(g);
}

void lj_gc2_test_finalizer_enqueue(global_State *g, GCobj *o)
{
  lj_gc2_finalizer_enqueue(g, o);
}

int lj_gc2_test_finalizer_queue_pending(global_State *g)
{
  return gc2_finalizer_queue_pending(g);
}

int lj_gc2_test_finalizer_pending(global_State *g)
{
  return lj_gc2_finalizer_pending(g);
}

int lj_gc2_test_finalizer_sweep_pending(global_State *g)
{
  return gc2_finalizer_sweep_pending(g);
}

int lj_gc2_test_finalizer_step_dispatch(lua_State *L,
					GC2FinalizerDispatchFunc dispatch,
					GCSize finalize_cost, GCSize *cost)
{
  GC2FinalizerDispatchCtx ctx;
  ctx.dispatch = dispatch;
  return lj_gc2_finalizer_step_ctx(L, &ctx, finalize_cost, cost);
}

static int gc2_finalizer_pending_for_sweep(global_State *g, int owner_ok)
{
  if (!g)
    return 0;
  if (gc2_finalizer_queue_pending(g))
    return 1;
  if (gc2_finalizer_active_acq(g) == 0)
    return 0;
  return !(owner_ok && lj_gc2_finalizer_owned_by_current(g));
}

static int gc2_finalizer_queue_pending(global_State *g)
{
  if (!g)
    return 0;
  return gc2_finalizer_tail_acq(g) != NULL ||
	 gc2_finalizer_mpsc_acq(g) != NULL;
}

int lj_gc2_finalizer_phase_pending(global_State *g)
{
  return gc2_finalizer_queue_pending(g);
}

static int gc2_finreg_udata_close_pending(global_State *g)
{
  GC2FinRegUDataNode *node;
  GC2RootCycleGuard guard;
  if (!g)
    return 0;
  node = gc2_finreg_udata_head_acq(g);
  gc2_root_cycle_guard_init(&guard, node);
  while (node) {
    GC2FinRegUDataNode *next;
    if (!lj_gc2_mem_registered(g, node))
      return 1;  /* Invalid metadata cannot prove terminal quiescence. */
    if (gc2_finreg_udata_active_acq(node))
      return 1;
    next = gc2_finreg_udata_next_acq(node);
    node = next;
    if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, node)))
      return 1;
  }
  return 0;
}

int lj_gc2_finalizer_close_pending(global_State *g)
{
  return gc2_finalizer_queue_pending(g) || lj_gc2_finreg_cdata_pending(g) ||
	 gc2_finreg_udata_close_pending(g);
}

static int lj_gc2_finalizer_pending(global_State *g)
{
  return gc2_finalizer_pending_for_sweep(g, 0);
}

static int gc2_finalizer_sweep_pending(global_State *g)
{
  return gc2_finalizer_pending_for_sweep(g, 1);
}

static int gc2_finalizer_mt_release_exclusive(global_State *g);
static int gc2_finalizer_mt_reclaim_exclusive(global_State *g);

static GCSize gc2_finalizer_pause_threshold(global_State *g)
{
  GCSize oldt;
  if (!g)
    return 0;
  oldt = mt_live_acq(g) != 0 ? lj_gc_mt_threshold_load(g) :
	 lj_gc_threshold_load(g);
  lj_gc_mt_threshold_store(g, oldt);
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  return oldt;
}

static void gc2_finalizer_restore_threshold(global_State *g, GCSize oldt)
{
  if (!g)
    return;
  lj_gc_threshold_store(g, oldt);
  if (mt_live_acq(g) != 0) {
    lj_gc_mt_threshold_store(g, oldt);
    lj_gc_threshold_store(g, LJ_MAX_MEM);
    if (mt_live_acq(g) == 0)
      lj_gc_threshold_store(g, oldt);
  }
}

static int gc2_finalizer_mt_release_exclusive(global_State *g)
{
  if (!g || mt_gc_exclusive_acq(g) == 0)
    return 0;
  mt_gc_exclusive_rel(g, 0);
#if defined(LA_HAS_FUTEX)
  mt_gc_exclusive_futex_wake(g, INT_MAX);
#endif
  return 1;  /* 09 section 9.6: finalizer may spawn while GC is paused. */
}

static int gc2_finalizer_mt_reclaim_exclusive(global_State *g)
{
  if (!g)
    return 0;
  for (;;) {
    uint32_t expect = 0;
    if (mt_live_acq(g) != 0)
      return 0;  /* 09 section 9.6: finalizer-spawn outlived callback. */
    if (mt_gc_exclusive_cas(g, &expect, 1)) {
      if (mt_live_acq(g) == 0)
	return 1;
      mt_gc_exclusive_rel(g, 0);
#if defined(LA_HAS_FUTEX)
      mt_gc_exclusive_futex_wake(g, INT_MAX);
#endif
      return 0;
    }
    if (mt_gc_exclusive_acq(g) != 0)
      return 0;
  }
}

static int gc2_finalizer_pcall(global_State *g, lua_State *L,
			       TValue *top, int *continue_gc)
{
  uint32_t oldphase = gc2_phase_acq(g);
  uint32_t live_before = mt_live_acq(g);
  uint32_t oldlatch = gc2_finalizer_spawn_latch_update(
    g, LJ_GC2_FINSPAWN_CALLBACK_ACTIVE, 0);
  int owns_callback_latch =
    (oldlatch & LJ_GC2_FINSPAWN_CALLBACK_ACTIVE) == 0;
  int had_mt_exclusive = gc2_finalizer_mt_release_exclusive(g);
  int errcode = lj_vm_pcall_unwind(L, top, 1+0, -1);  /* |mo|o| -> | */
  int keep_gc;
  if (had_mt_exclusive) {
    /* Publish before the reclaim attempt so the last exiting secondary cannot
    ** miss the scheduler wake in the live-to-zero race. A successful reclaim
    ** proves that no spawned TG outlived this callback and consumes the latch.
    */
    (void)gc2_finalizer_spawn_latch_update(
      g, LJ_GC2_FINSPAWN_DEFERRED, 0);
    keep_gc = gc2_finalizer_mt_reclaim_exclusive(g);
    if (keep_gc)
      (void)gc2_finalizer_spawn_latch_update(
	g, 0, LJ_GC2_FINSPAWN_DEFERRED);
  } else {
    keep_gc = 1;
  }
  if (!had_mt_exclusive && oldphase == LJ_GC2_SWEEP &&
      mt_live_acq(g) > live_before) {
    keep_gc = 0;  /* GC2 finalizer spawned a secondary thread. */
    (void)gc2_finalizer_spawn_latch_update(
      g, LJ_GC2_FINSPAWN_DEFERRED, 0);
  }
  if (owns_callback_latch)
    (void)gc2_finalizer_spawn_latch_update(
      g, 0, LJ_GC2_FINSPAWN_CALLBACK_ACTIVE);
  if (continue_gc)
    *continue_gc = keep_gc;
  return errcode;
}

typedef struct GC2ErrFinEvent {
  ptrdiff_t errslot;
} GC2ErrFinEvent;

static TValue *gc2_errfin_vmevent_cp(lua_State *L, lua_CFunction dummy,
				     void *ud)
{
  GC2ErrFinEvent *ctx = (GC2ErrFinEvent *)ud;
  UNUSED(dummy);
  lj_vmevent_send_l(L, ERRFIN,
    copyTV(V, V->top++, restorestack(L, ctx->errslot));
  );
  return NULL;
}

/* Deliver ERRFIN while the initiating state remains exclusively claimed. The
** original error stays rooted in its Lua stack slot throughout handler lookup
** and allocation. Protect the event because its final STOPREQ check may throw;
** release the claim before propagating that error so shutdown cannot strand a
** permanently busy state.
*/
static void gc2_errfin_vmevent_claimed(lua_State *L, LJStateClaim *claim,
				       ptrdiff_t errslot,
				       ptrdiff_t restoretop)
{
  GC2ErrFinEvent ctx;
  int errcode;
  ctx.errslot = errslot;
  errcode = lj_vm_cpcall(L, NULL, &ctx, gc2_errfin_vmevent_cp);
  if (errcode == 0)
    L->top = restorestack(L, restoretop);
  lj_state_dropclaim(claim);
  if (LJ_UNLIKELY(errcode))
    lj_err_throw(L, errcode);
}

static int gc2_finalizer_checkstack_claimed(global_State *g, lua_State *L,
					    MSize need, LJStateClaim *claim)
{
  ptrdiff_t oldtop = savestack(L, L->top);
  int errcode = 0;
  UNUSED(g);
  if ((mref(L->maxstack, char) - (char *)L->top) <=
      (ptrdiff_t)need*(ptrdiff_t)sizeof(TValue))
    errcode = lj_state_cpgrowstack(L, need);
  if (LJ_UNLIKELY(errcode)) {
    ptrdiff_t errslot = savestack(L, L->top-1);
    gc2_errfin_vmevent_claimed(L, claim, errslot, oldtop);
    return 0;
  }
  return 1;
}

static int gc2_call_finalizer(global_State *g, lua_State *L,
			      cTValue *mo, GCobj *o)
{
  /* Save and restore lots of state around the __gc callback. */
  LJStateClaim claim;
  lua_State *cbL = L;
  lua_State *oldL;
  uint8_t oldh;
  GCSize oldt;
  int continue_gc = 1;
  int errcode;
  ptrdiff_t oldbase;
  ptrdiff_t oldtop;
  TValue *top;
  if (!g || !cbL || !mo || !o)
    return 0;
  if (!lj_state_tryclaim(cbL, lj_thr_current_id(g), &claim))
    return 0;  /* Caller must preclaim before clearing FINREG state. */
  lj_assertG(cbL != vmthread_acq(g),
	     "gc2 finalizer must not use shared vmthread callback stack");
  if (!gc2_finalizer_checkstack_claimed(g, cbL, 2+LJ_FR2+LUA_MINSTACK,
					&claim))
    return 1;
  oldL = lj_tg_cur_L(g);
  oldh = hook_save(g);
  oldt = gc2_finalizer_pause_threshold(g);
  lj_trace_abort(g);
  hook_entergc(g);  /* Disable hooks and new traces during __gc. */
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  oldbase = savestack(cbL, cbL->base);
  oldtop = savestack(cbL, cbL->top);
  top = cbL->top;
  copyTV(cbL, top++, mo);
  if (LJ_FR2) setnilV(top++);
  setgcV(cbL, top, o, ~o->gch.gct);
  cbL->top = top+1;
  errcode = gc2_finalizer_pcall(g, cbL, top, &continue_gc);
  if (oldL)
    lj_tg_setcur_L(g, oldL);
  else
    lj_tg_clearcur_L(g);
  hook_restore(g, oldh);
  if (LJ_HASPROFILE && (oldh & HOOK_PROFILE)) lj_dispatch_update(g, 0);
  gc2_finalizer_restore_threshold(g, oldt);
  if (errcode) {
    ptrdiff_t errslot = savestack(cbL, cbL->top-1);
    cbL->base = restorestack(cbL, oldbase);
    gc2_errfin_vmevent_claimed(cbL, &claim, errslot, oldtop);
  } else {
    cbL->base = restorestack(cbL, oldbase);
    cbL->top = restorestack(cbL, oldtop);
    lj_state_dropclaim(&claim);
  }
  return continue_gc;
}

static int gc2_finalizer_spawn_deferred(global_State *g)
{
  if (!g)
    return 0;
  if ((gc2_finalizer_spawn_latch_acq(g) & LJ_GC2_FINSPAWN_DEFERRED) != 0 &&
      mt_live_acq(g) != 0 &&
      mt_gc_exclusive_acq(g) == 0) {
    gc2_finalizer_spawn_deferrals_add(g, 1);
    return 1;
  }
  /* The GC driver which observes the released last secondary consumes the
  ** latch. The leaving TG only wakes the driver, keeping latch ownership on
  ** the collector side and avoiding an exit-vs-predicate lost-clear race. */
  if ((gc2_finalizer_spawn_latch_acq(g) & LJ_GC2_FINSPAWN_DEFERRED) != 0 &&
      mt_live_acq(g) == 0)
    (void)gc2_finalizer_spawn_latch_update(
      g, 0, LJ_GC2_FINSPAWN_DEFERRED);
  return 0;
}

void lj_gc2_finalizer_spawn_release(global_State *g)
{
  if (!g)
    return;
  if ((gc2_finalizer_spawn_latch_acq(g) & LJ_GC2_FINSPAWN_DEFERRED) != 0 &&
      mt_live_acq(g) == 0 &&
      mt_gc_exclusive_acq(g) == 0) {
    gc2_finalizer_spawn_release_wakes_add(g, 1);
    lj_gc2_worker_wake(g);
  }
}

static int gc2_sweep_blocked_by_finalizer(global_State *g)
{
  if (!gc2_finalizer_sweep_pending(g))
    return 0;
  gc2_finalizer_sweep_blocks_add(g, 1);
  return 1;  /* 05 section 5.8 finalizer drain before traversable sweep. */
}

int lj_gc2_sweep_tg_ready(TGState *tg)
{
  uint8_t flags;
  if (!tg)
    return 0;
  flags = lj_tg_flags_acq(tg);
  return !(flags & TGF_DEAD) && (flags & TGF_ARENA_INTERNAL);
}

int lj_gc2_sweep_bridge_can_progress(global_State *g)
{
  return g && gc2_phase_acq(g) == LJ_GC2_SWEEP &&
	 gc2_recovery_empty(g) &&
	 !gc2_finalizer_pending_for_sweep(g, 1);
}

int lj_gc2_sweep_minor_active(global_State *g)
{
  return g && gc2_phase_acq(g) == LJ_GC2_SWEEP &&
	 gc2_cycle_sweep_minor_acq(g) != 0;
}

int lj_gc2_sweep_needs_prepare(global_State *g)
{
  TGState *tg;
  uint32_t cycle;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP)
    return 0;
  cycle = gc2_cycle_acq(g);
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg))
    if (lj_gc2_sweep_tg_ready(tg) && tg->alloc.prepare_epoch != cycle)
      return 1;
  return 0;
}

int lj_gc2_sweep_needs_restore(global_State *g)
{
  TGState *tg;
  uint32_t k;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP)
    return 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    if (!lj_gc2_sweep_tg_ready(tg))
      continue;
    if (tg->alloc.prepare_epoch != 0)
      return 1;
    for (k = 0; k < LJ_ARENA_NKINDS; k++)
      if (tg->alloc.needsweep[k] != NULL)
	return 1;
  }
  return 0;
}

static int gc2_sweep_bridge_owner_roots_pending(global_State *g)
{
  return gc2_thread_scan_needscan_pending_acq(g) != 0;
}

static void gc2_sweep_root_snapshot_abort(global_State *g)
{
  uint32_t expect = 2;
  (void)gc2_sweep_root_scanned_cas(g, &expect, 0);
}

/* The mandatory pre-READY SWEEP root scan uses the same provisional
** certificate discipline as MARK and WEAK. Root participants may reset 2->0
** through gc2_root_scan_retry(); only an unchanged closed generation may
** publish 2->1 after the complete handshake. */
static int gc2_sweep_root_snapshot(global_State *g)
{
  uint32_t state, expect, cycle;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP)
    return 0;
  state = gc2_sweep_root_scanned_acq(g);
  if (state == 1)
    return 1;
  if (state != 0)
    return 0;
  expect = 0;
  if (!gc2_sweep_root_scanned_cas(g, &expect, 2))
    return 0;
  cycle = gc2_cycle_acq(g);
  if (gc2_jit_phase_gate_acq(g) != 0)
    gc2_jit_phase_gate_close(g);
  if (lj_tg_any_jit_active(g) || gc2_jit_recorder_active(g)) {
    if (gc2_jit_recorder_active(g))
      lj_trace_abort(g);
    gc2_sweep_root_snapshot_abort(g);
    return 0;
  }
  lj_gc2_trace_sweep_roots(g);
  if (gc2_phase_acq(g) != LJ_GC2_SWEEP ||
      gc2_cycle_acq(g) != cycle || gc2_jit_phase_gate_acq(g) != 0 ||
      lj_tg_any_jit_active(g) || gc2_jit_recorder_active(g)) {
    if (gc2_jit_recorder_active(g))
      lj_trace_abort(g);
    gc2_sweep_root_snapshot_abort(g);
    return 0;
  }
  expect = 2;
  return gc2_sweep_root_scanned_cas(g, &expect, 1);
}

void lj_gc2_sweep_prepare_bridge_boundary(global_State *g,
					  GC2SweepBridgePreserveFunc preserve)
{
  UNUSED(preserve);  /* Per-entry preservation is folded into bounded pruning. */
  if (!lj_gc2_sweep_bridge_can_progress(g))
    return;
  if (!gc2_worker_claim(g))
    return;  /* Serialize list mutation with sweep batches and close. */
  if (!lj_gc2_sweep_bridge_can_progress(g)) {
    gc2_worker_release(g);
    return;
  }
  /*
  ** Allocation cursors are owner-local. Request the boundary reset through a
  ** safepoint so each TG moves its own traversable bump/free state to sweep
  ** while it is not concurrently allocating from that state.
  */
  if (lj_gc2_sweep_needs_prepare(g))
    lj_gc2_handshake(g, LJ_GC2_HS_RESET_ALLOC);
  if (!gc2_sweep_bridge_ready_acq(g)) {
    TGState *self = G2TG(g);
    /* The mandatory SWEEP semantic snapshot is distinct from bounded pruning
    ** of the ownership spine. Record it once so a long root list cannot force
    ** another all-TG scan on every batch. A real thread NEEDSCAN handoff is the
    ** only condition which requests an additional owner snapshot. */
    if (gc2_sweep_root_scanned_acq(g) != 1) {
      if (!gc2_sweep_root_snapshot(g)) {
	gc2_worker_release(g);
	lj_gc2_worker_wake(g);
	return;
      }
    }
    if (gc2_sweep_bridge_owner_roots_pending(g)) {
      lj_gc2_trace_sweep_roots(g);
      gc2_worker_release(g);
      lj_gc2_worker_wake(g);
      return;
    }
    /* Rotate post-snapshot rescue entries, including the initiating TG which
    ** is not necessarily a remote handshake target. The ordinary SWEEP worker
    ** drains the resulting published SSB/grey work before this routine can
    ** publish the bridge. */
    if (self && self->gl == g && !lj_tg_flags_test_acq(self, TGF_DEAD))
      (void)gc2_flush_ssb(g, self, 0);
    (void)gc2_mark_drain_owned_bounded(g, LJ_GC2_SWEEP_BATCH);
    if (!lj_gc2_ssb_empty(g)) {
      (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
      gc2_worker_release(g);
      lj_gc2_worker_wake(g);
      return;
    }
    if (gc2_table_rescan_pending_acq(g) != 0) {
      /* Exact table tokens also cover publishers between reservation and queue
      ** visibility. Never erase their aggregate credit from a close observer. */
      gc2_worker_release(g);
      lj_gc2_worker_wake(g);
      return;
    }
    if (lj_tg_any_jit_active(g) || gc2_jit_recorder_active(g) ||
	gc2_weak_owned_peer_active(g)) {
      gc2_worker_release(g);
      lj_gc2_worker_wake(g);
      return;
    }
    /* Retire the marks produced by the completed snapshot/drain, then sample
    ** the real work publications again. A racing producer either increments
    ** the counter after this LP or leaves SSB/grey/NEEDSCAN work visible. */
    (void)gc2_marks_this_round_xchg_acqrel(g, 0);
    la_fence_seq();
    if (gc2_phase_acq(g) != LJ_GC2_SWEEP ||
	lj_tg_any_jit_active(g) || gc2_jit_recorder_active(g) ||
	gc2_weak_owned_peer_active(g) ||
	!lj_gc2_ssb_empty(g) || !gc2_recovery_empty(g) ||
	gc2_sweep_bridge_owner_roots_pending(g) ||
	gc2_table_rescan_pending_acq(g) != 0 ||
	gc2_marks_this_round_acq(g) != 0) {
      gc2_worker_release(g);
      lj_gc2_worker_wake(g);
      return;
    }
    /* One bounded ownership-spine batch. It only unlinks old mark-zero entries
    ** and preserves destructor side bodies; no destructor runs here. */
    (void)lj_gc_sweep_gc2_unmarked(g);
    if (!gc2_sweep_root_done_acq(g)) {
      gc2_worker_release(g);
      lj_gc2_worker_wake(g);
      return;
    }
    /* Revalidate every concrete producer at the READY LP. Stack dirty epochs
    ** are deliberately absent: after the mandatory snapshot, phase-aware
    ** barriers publish actual rescue work and freshness is not a lease. */
    (void)gc2_marks_this_round_xchg_acqrel(g, 0);
    la_fence_seq();
    if (gc2_phase_acq(g) != LJ_GC2_SWEEP ||
	gc2_sweep_root_scanned_acq(g) != 1 ||
	lj_tg_any_jit_active(g) || gc2_jit_recorder_active(g) ||
	gc2_weak_owned_peer_active(g) || !lj_gc2_ssb_empty(g) ||
	!gc2_recovery_empty(g) ||
	gc2_sweep_bridge_owner_roots_pending(g) ||
	gc2_table_rescan_pending_acq(g) != 0 ||
	gc2_marks_this_round_acq(g) != 0) {
      gc2_worker_release(g);
      lj_gc2_worker_wake(g);
      return;
    }
    lj_gc2_sweep_bridge_ready(g);
  }
  gc2_worker_release(g);
}

int lj_gc2_sweep_pending(global_State *g)
{
  TGState *tg;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP)
    return 0;
  if (lj_state_gcprep_pending_acq(g) != 0)
    return 1;
  if (lj_str_gc2_sweep_pending(g))
    return 1;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg))
    if (lj_gc2_sweep_tg_ready(tg)) {
      if (tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL ||
	  tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] != NULL)
	return 1;
      if (lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
	  lj_arena_hugetab_has_sweep_old(&tg->huge))
	return 1;
    }
  return 0;
}

static void gc2_sweep_reclaim_leave(global_State *g);

static int gc2_sweep_reclaim_enter(global_State *g)
{
  LJThrGC2TLS *tls;
  uint32_t expect = LJ_GC2_SMR_OPEN;
  if (!g)
    return 0;
  tls = gc2_tls_current();
  if (!tls || gc2_phase_acq(g) != LJ_GC2_SWEEP ||
      gc2_worker_active_acq(g) == 0 ||
      gc2_jit_phase_gate_acq(g) != 0 || lj_tg_any_jit_active(g) ||
      gc2_jit_recorder_active(g) || gc2_sweep_root_scanned_acq(g) != 1 ||
      !gc2_recovery_empty(g) || lj_gc2_activation_reclaim_veto(g) ||
      !gc2_smr_reclaiming_cas(g, &expect, LJ_GC2_SMR_SWEEP_STABLE))
    return 0;
  if (gc2_phase_acq(g) != LJ_GC2_SWEEP ||
      gc2_worker_active_acq(g) == 0 ||
      gc2_jit_phase_gate_acq(g) != 0 || lj_tg_any_jit_active(g) ||
      gc2_jit_recorder_active(g) || gc2_sweep_root_scanned_acq(g) != 1 ||
      !gc2_recovery_empty(g) || lj_gc2_activation_reclaim_veto(g) ||
      gc2_smr_readers_acq(g) != 0) {
    gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
    return 0;
  }
  if (!gc2_reclaim_tls_enter_state(tls, g)) {
    gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
    return 0;
  }
  return 1;
}

#if defined(lj_gc2_c) || defined(LJ_GC2_TEST_HELPERS) || defined(LUA_USE_ASSERT)
int lj_gc2_test_sweep_reclaim_enter(global_State *g)
{
  int entered = gc2_sweep_reclaim_enter(g);
  /* This probe tests only the process-visible activation/SMR gate and its
  ** fixtures release that word directly. Do not strand the private
  ** current-thread capability outside the real enter/leave call pair. */
  if (entered)
    gc2_reclaim_tls_leave(g);
  return entered;
}

#if defined(LJ_GC2_TEST_HELPERS) || defined(LJ_TRACE_TEST_HELPERS)
int lj_gc2_test_sweep_reclaim_scope_enter(global_State *g)
{
  return gc2_sweep_reclaim_enter(g);
}

void lj_gc2_test_sweep_reclaim_scope_leave(global_State *g)
{
  gc2_sweep_reclaim_leave(g);
}
#endif
#endif

static void gc2_sweep_reclaim_leave(global_State *g)
{
  lj_assertG(gc2_jit_phase_gate_acq(g) == 0,
	     "SWEEP reclaim scope outlived closed JIT gate");
  lj_assertG(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_SWEEP_STABLE,
	     "SWEEP reclaim scope lost stable registry mode");
  gc2_reclaim_tls_leave(g);
  gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
}

static uint32_t gc2_sweep_reclaim_jit(global_State *g, uint64_t epoch)
{
  uint32_t n = 0;
#if LJ_HASJIT
  jit_State *J = G2J(g);
  int token = 0;
  /* SWEEP gates new compiled entry, but an already published entry/exit intent
  ** still owns its trace slot and mcode. Reclaim stays opportunistic and lets a
  ** later owner pass retry after that TG publishes quiescence. */
  if (lj_tg_any_jit_active(g))
    return 0;
  /* SWEEP_STABLE deliberately remains open to exact object readers. Trace
  ** disconnect publishes only atomic linkage changes after preserving the
  ** graph; the per-allocation destructor claim arbitrates every body/side
  ** free against small rescue counts or Huge readers. */
  if (lj_jit_token_held(J) || (token = lj_jit_token_try(J))) {
    if (!lj_tg_any_jit_active(g)) {
      n += lj_trace_reclaim_retired(g, epoch);
      n += lj_mcode_reclaim_retired(g, epoch);
    }
    if (token)
      lj_jit_token_release(J);
  }
#else
  UNUSED(g);
  UNUSED(epoch);
#endif
  return n;
}

static uint32_t gc2_sweep_huge_progress(global_State *g, TGState *tg,
					 int preserve_marks)
{
  TGAlloc *alloc = &tg->alloc;
  uint32_t cursor = alloc->huge_reclaim_cursor;
  LJHugeInfo hi;
  void *p = NULL;
  int pending = 0;
  if (!lj_tg_flags_test_acq(tg, TGF_HUGETAB))
    return 0;
  if (lj_arena_hugetab_sweep_next(&tg->huge, &cursor, &p, &hi)) {
    /* DEFER_FREE means the logical free is complete but a descriptor/token or
    ** another exact lifetime owner still names the mapping. Never interpret
    ** payload bytes in this state. Bounded passes retry the certificate until
    ** one owner atomically publishes the ordinary FREEING sweep handoff. */
    if ((hi.flags & LJ_HUGEF_DEFER_FREE) &&
	!lj_arena_hugetab_retry_deferred(&tg->huge, p, &hi)) {
      alloc->huge_reclaim_cursor = cursor;
      alloc->huge_retire_done = 0;
      return 1;
    }
    uint32_t n = lj_gc_reclaim_gc2_huge(g, tg, p, &hi, &pending);
    alloc->huge_reclaim_cursor = cursor;
    if (pending)
      alloc->huge_retire_done = 0;
    return n ? n : 1u;  /* Advancing the bounded table cursor is real work. */
  }
  alloc->huge_reclaim_cursor = 0;
  if (gc2_sweep_grace_needed_acq(g))
    return 1;  /* A newly retired entry needs another completed handshake. */
  lj_arena_hugetab_finish_sweep(&tg->huge, preserve_marks);
  alloc->huge_retire_done =
    (uint8_t)!lj_arena_hugetab_has_sweep_old(&tg->huge);
  return alloc->huge_retire_done ? 1u : 0u;
}

static uint32_t lj_gc2_sweep_owner_progress(global_State *g, TGState *tg,
					     uint32_t limit,
					     int *finishedp)
{
  uint32_t n = 0, arenas = 0, epoch;
  uint64_t live = 0;
  int minor, preserve_marks;
  if (finishedp)
    *finishedp = 0;
  if (!g || !tg || limit == 0 ||
      !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL))
    return 0;
  if (gc2_phase_acq(g) != LJ_GC2_SWEEP || !gc2_recovery_empty(g) ||
      gc2_jit_recorder_active(g) || gc2_sweep_root_scanned_acq(g) != 1)
    return 0;
  if (!gc2_sweep_bridge_ready_acq(g))
    return 0;
  if (gc2_sweep_blocked_by_finalizer(g))
    return 0;
  if (lj_state_gcprep_pending_acq(g) != 0) {
    /* Semantic THREAD teardown may publish closed-upvalue/root/SSB work. Run
    ** it outside the exclusive writer, then return so the ordinary closure
    ** predicates consume that work before another physical reclaim batch. */
    n = lj_state_gcprep_drain(g, limit);
    return n;
  }
  /* Once the bridge is ready, ordinary dirty-epoch changes are protected by
  ** phase-aware root barriers and SSB rescue; freshness was a closure check,
  ** never a lease valid after peers resumed. A queued thread identity is
  ** different: NEEDSCAN promises stack-only reachability from its owner. Close
  ** that handoff once, then return so root-generated SSB/grey work drains before
  ** this batch touches another arena. */
  if (lj_tg_any_jit_active(g) ||
      gc2_thread_scan_needscan_pending_acq(g) != 0) {
    (void)lj_gc2_handshake(g,
	LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_FLUSH_SSB);
    return 0;
  }
  if (gc2_weak_owned_peer_active(g))
    return 0;
  if (!lj_gc2_ssb_empty(g)) {
    (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
    return 0;
  }
  if (gc2_table_rescan_pending_acq(g) != 0) {
    /* An exact reservation can precede queue visibility. The publisher or a
    ** duplicate requeue owns progress; a sweep worker must not forge zero. */
    return 0;
  }
  if (gc2_marks_this_round_xchg_acqrel(g, 0) != 0)
    return 0;
  epoch = gc2_cycle_acq(g);
  minor = gc2_cycle_sweep_minor_acq(g) != 0;
  /*
  ** Minor arena sweeps use mark bits as the old-generation set. A forced major
  ** cycle while generational mode is enabled must therefore leave survivor marks
  ** set for the first following minor; otherwise old global-root objects that
  ** are not part of the minor root snapshot can look collectable.
  */
  preserve_marks = minor || gc2_generational_acq(g) != 0;
  if (minor)
    (void)lj_gc_sweep_gc2_unmarked(g);
  tg->alloc.sweep_epoch = epoch;
  while (n < limit) {
    GCArena *next = tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
    GCArena *qa = tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE];
    if (next) {
      (void)lj_arena_remote_free_drain_sweep(&tg->alloc, next);
      if (tg == g->main_tg)
	(void)lj_gc_sweep_gc2_arena_unmarked_exclusive(g, next);
      else
	(void)lj_gc_sweep_gc2_arena_unmarked(g, next);
      if (!lj_arena_alloc_quarantine_one(&tg->alloc,
		LJ_ARENAK_TRAVERSABLE, gc2_hs_epoch_acq(g)))
	break;
      gc2_sweep_grace_needed_rel(g, 1);
      n++;
      continue;
    }
    if (qa) {
      int done = 0;
      int finished_arena = 0;
      uint32_t step;
      uint64_t retire_epoch = la_load64_acq(&qa->hdr.retire_epoch);
      if (retire_epoch == ~(uint64_t)0) {
	la_store64_rel(&qa->hdr.retire_epoch, gc2_hs_epoch_acq(g));
	gc2_sweep_grace_needed_rel(g, 1);
	n++;  /* Fresh-grace publication is durable owner progress. */
	break;
      }
      if (gc2_sweep_grace_needed_acq(g) ||
	  gc2_hs_epoch_acq(g) <= retire_epoch)
	break;
      if (!gc2_sweep_reclaim_enter(g))
	break;
      (void)gc2_sweep_reclaim_jit(g, gc2_hs_epoch_acq(g));
      la_store32_rel(&qa->hdr.flags,
		     lj_arena_flags_acq(qa) | LJ_AF_PREPSWEEP);
      if (!lj_arena_reclaim_seal(qa)) {
	la_store32_rel(&qa->hdr.flags,
		       lj_arena_flags_acq(qa) & ~LJ_AF_PREPSWEEP);
	gc2_sweep_reclaim_leave(g);
	break;
      }
      if (la_load64_acq(&qa->hdr.retire_epoch) == ~(uint64_t)0) {
	la_store64_rel(&qa->hdr.retire_epoch, gc2_hs_epoch_acq(g));
	gc2_sweep_grace_needed_rel(g, 1);
	la_store32_rel(&qa->hdr.flags,
		       lj_arena_flags_acq(qa) & ~LJ_AF_PREPSWEEP);
	lj_arena_reclaim_unseal(qa, 1);
	gc2_sweep_reclaim_leave(g);
	n++;
	break;
      }
      step = lj_arena_remote_free_drain_sweep(&tg->alloc, qa);
      {
	uint32_t reclaimed_step = lj_gc_reclaim_gc2_arena(g, qa, 64u, &done);
	step = reclaimed_step > ~(uint32_t)0 - step ?
	       ~(uint32_t)0 : step + reclaimed_step;
      }
      if (done) {
	uint32_t reason = LJ_ARENA_FINISH_NONE;
	if (lj_arena_alloc_quarantine_finish(&tg->alloc,
	      LJ_ARENAK_TRAVERSABLE, qa, epoch, preserve_marks, &reason)) {
	  live += qa->hdr.live_cells;
	  arenas++;
	  finished_arena = 1;
	  step++;
	} else {
	  uint64_t gate = lj_arena_remote_active_acq(qa);
	  if (reason == LJ_ARENA_FINISH_ACTIONABLE &&
	      qa->hdr.reclaim_cell < LJ_ARENA_CELLS && !step)
	    step = 1;
	  else if (reason == LJ_ARENA_FINISH_EPOCH && !step)
	    step = 1;
	  else if (reason == LJ_ARENA_FINISH_PUBLISHER &&
		   (gate & LJ_ARENA_REMOTE_COUNT_MASK) == 0 && !step)
	    step = 1;
	  lj_arena_reclaim_unseal(qa, 1);
	}
	/* Finish found an actionable LIVE/RETIRED state behind EOF and
	** rearmed the exact cell. Discovery of that lost backedge is bounded
	** real work; active publishers instead wake on their release edge. */
	if (!step && qa->hdr.reclaim_cell < LJ_ARENA_CELLS) {
	  /* Finish found an actionable LIVE/RETIRED state behind EOF and
	  ** rearmed the exact cell. Discovery of that lost backedge is bounded
	  ** real work; keep this owner scheduled to consume it without spinning
	  ** on an active publisher or unclassified-WHITE blocker. Durable queue
	  ** and epoch reasons are accounted by their terminal protocol. */
	  if (!step)
	    step = 1;
	}
      } else {
	/* Let a pending trace's physical free publish RETIRED/LIVE->FREEING
	** between bounded passes. Each pass freezes again before gcw rewrite. */
	la_store32_rel(&qa->hdr.flags,
		       lj_arena_flags_acq(qa) & ~LJ_AF_PREPSWEEP);
	(void)lj_arena_reclaim_clear_pending(qa);
	lj_arena_reclaim_unseal(qa, 1);
      }
      gc2_sweep_reclaim_leave(g);
      if (lj_state_gcprep_pending_acq(g) != 0)
	break;
      if (!step)
	break;
      if (finished_arena) {
	/* Bitmap-word summaries can now finish an otherwise inert arena in one
	** reclaim visit. End the physical-commit quantum explicitly instead of
	** inflating the returned work count: the caller reopens the JIT gate and
	** automatic pacing retains its LJ_GC2_SWEEP_BATCH completion bound. */
	n++;
	if (finishedp)
	  *finishedp = 1;
	break;
      }
      n++;
      continue;
    }
    if (lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
	lj_arena_hugetab_has_sweep_old(&tg->huge)) {
      uint32_t step;
      if (gc2_sweep_grace_needed_acq(g))
	break;
      if (!gc2_sweep_reclaim_enter(g))
	break;
      (void)gc2_sweep_reclaim_jit(g, gc2_hs_epoch_acq(g));
      step = gc2_sweep_huge_progress(g, tg, preserve_marks);
      gc2_sweep_reclaim_leave(g);
      if (lj_state_gcprep_pending_acq(g) != 0)
	break;
      if (!step)
	break;
      n++;
      continue;
    }
    break;
  }
  if (n) {
    gc2_sweep_owner_runs_add(g, 1);
    if (arenas)
      gc2_sweep_owner_arenas_add(g, arenas);
    gc2_sweep_owner_live_cells_add(g, live);
    if (minor && arenas)
      gc2_minor_sweep_arenas_add(g, arenas);
  }
  return n;
}

uint32_t lj_gc2_test_sweep_owner_progress(global_State *g, TGState *tg,
					  uint32_t limit)
{
  uint32_t n;
  int claimed = 0;
  int gate_owned = 0;
  if (!g || !tg || limit == 0)
    return 0;
  if (gc2_worker_active_acq(g) == 0) {
    if (!gc2_worker_claim(g))
      return 0;
    claimed = 1;
  }
  if (gc2_sweep_grace_needed_acq(g)) {
    uint32_t grace;
    for (grace = 0; grace < LJ_GC2_GRACE_EPOCHS; grace++)
      /* Grace needs acknowledgement and SSB closure, not a duplicate semantic
      ** root traversal. The next bounded sweep batch establishes its one root
      ** boundary before touching another arena. */
      (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
      gc2_sweep_grace_needed_rel(g, 0);
    n = 1;
  } else {
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP &&
	gc2_sweep_bridge_ready_acq(g) != 0 &&
	gc2_jit_phase_gate_acq(g) != 0) {
      gc2_jit_phase_gate_close(g);
      if (lj_tg_any_jit_active(g)) {
	gc2_jit_sweep_displaced_rel(g, 1);
	if (claimed)
	  gc2_worker_release(g);
	return 0;
      }
      gate_owned = 1;
    }
    n = lj_gc2_sweep_owner_progress(g, tg, limit, NULL);
  }
  if (gate_owned && gc2_phase_acq(g) == LJ_GC2_SWEEP &&
      gc2_sweep_bridge_ready_acq(g) != 0)
    gc2_jit_phase_gate_open_sweep(g, 0);
  if (claimed)
    gc2_worker_release(g);
  return n;
}

static uint64_t gc2_sweep_live_cells(GCArena *a, uint32_t epoch)
{
  uint64_t cells = 0;
  GCArena *slow = a, *fast = a;
  for (; a != NULL; a = lj_arena_next_acq(a)) {
    GCArena *next = lj_arena_next_acq(a);
    if (a->hdr.sweep_epoch == epoch)
      cells += a->hdr.live_cells;
    if (next == a)
      break;
    if (slow)
      slow = lj_arena_next_acq(slow);
    if (fast)
      fast = lj_arena_next_acq(fast);
    if (fast)
      fast = lj_arena_next_acq(fast);
    if (slow && fast && slow == fast)
      break;
  }
  return cells;
}

static uint64_t gc2_saturating_add64(uint64_t a, uint64_t b)
{
  return a > ~(uint64_t)0 - b ? ~(uint64_t)0 : a + b;
}

static uint64_t lj_gc2_sweep_live_aggregate(global_State *g)
{
  TGState *tg;
  uint64_t cells = 0, huge_bytes = 0, bytes;
  uint32_t epoch;
  if (!g)
    return 0;
  epoch = gc2_cycle_acq(g);
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    uint8_t flags = lj_tg_flags_acq(tg);
    if ((flags & (TGF_DEAD|TGF_ARENA_INTERNAL)) != TGF_ARENA_INTERNAL)
      continue;
    cells += gc2_sweep_live_cells(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE],
				  epoch);
    cells += gc2_sweep_live_cells(
	lj_arena_alloc_reclaimed_head(&tg->alloc, LJ_ARENAK_TRAVERSABLE),
	epoch);
    if (flags & TGF_HUGETAB)
      huge_bytes = gc2_saturating_add64(huge_bytes,
	lj_arena_hugetab_live_bytes(&tg->huge,
	  LJ_HUGEF_TRAVERSABLE));
  }
  bytes = cells > (~(uint64_t)0 >> LJ_CELL_SHIFT) ?
	  ~(uint64_t)0 : cells << LJ_CELL_SHIFT;
  bytes = gc2_saturating_add64(bytes, huge_bytes);
  gc2_sweep_live_huge_bytes_rel(g, huge_bytes);
  gc2_live_estimate_rel(g, bytes);
  gc2_sweep_live_updates_add(g, 1);
  return bytes;
}

static void gc2_root_spine_counts(global_State *g, uint64_t *objectsp,
				  uint64_t *tombstonesp, uint32_t *cappedp)
{
  GCobj *o;
  GC2MarkScope scope;
  uint64_t objects = 0, tombstones = 0;
  uint32_t capped = 0;
  if (!g) {
    *objectsp = 0;
    *tombstonesp = 0;
    *cappedp = 0;
    return;
  }
  (void)lj_gc_repair_root_spine(g);
  o = lj_gc_root_acq(g);
  if (o && !gc2_root_spine_admit(g, o, &scope))
    o = NULL;
  while (o != NULL) {
    GC2MarkScope nextscope;
    GCobj *next;
    objects++;
    if (o->gch.gct == 0)
      tombstones++;
    if (objects == LJ_GC2_ROOT_SCAN_LIMIT) {
      next = lj_obj_gcw_acq(o);
      capped = next != NULL;
      gc2_mark_scope_leave(&scope);
      break;
    }
    if (!gc2_root_spine_handoff(g, o, &scope, &next, &nextscope))
      break;
    if (next == o) {
      gc2_mark_scope_leave(&nextscope);
      break;
    }
    o = next;
    scope = nextscope;
  }
  *objectsp = objects;
  *tombstonesp = tombstones;
  *cappedp = capped;
}

static uint64_t gc2_arena_list_count(GCArena *a)
{
  uint64_t n = 0;
  for (; a != NULL; a = lj_arena_next_acq(a)) {
    n++;
    if (n == LJ_GC2_ROOT_SCAN_LIMIT)
      break;
  }
  return n;
}

void lj_gc2_sweep_bridge_ready(global_State *g)
{
  uint32_t expect = 0;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP)
    return;
  /* READY is the irreversible admission edge for physical reclaim. Keep the
  ** mandatory root-snapshot certificate inside the latch precondition, rather
  ** than trusting every internal/test caller to have used the bridge driver.
  ** Otherwise READY=1/root_scanned=0 is permanent: post-READY retries quite
  ** deliberately cannot revoke the latch, while every destructor and normal
  ** SWEEP close correctly refuses the missing certificate. */
  if (gc2_sweep_root_scanned_acq(g) != 1 ||
      gc2_sweep_bridge_owner_roots_pending(g) || !gc2_recovery_empty(g))
    return;
  /* READY publication and the initial entry opening are one-shot. A redundant
  ** compatibility/test call must never reopen the gate while a reclaim owner
  ** has READY=1 and the gate closed around physical destruction. */
  if (!gc2_sweep_bridge_ready_cas(g, &expect, 1))
    return;
  /* Every semantic root and rescue publication is closed before READY. Native
  ** execution may now run between bounded physical-reclaim quanta. */
  gc2_jit_phase_gate_open_sweep(g, gc2_n_workers_acq(g) != 0);
  lj_gc2_worker_wake(g);  /* 05 section 5.8: roots reached close. */
}

void lj_gc2_sweep_bridge_boundary_reached(global_State *g)
{
  lj_gc2_sweep_bridge_ready(g);
}

void lj_gc2_preserve_abort_to_idle(global_State *g)
{
  uint32_t phase;
  if (!g)
    return;
  /* NO_RECLAIM is absorbing for this universe. Preserve-abort may carry real
  ** queue identities into a replacement cycle, but it must never turn a
  ** sticky unclassified edge into an IDLE/reclaim-capable generation. */
  if (gc2_recovery_failed_veto(g)) {
    lj_gc2_worker_wake(g);
    return;
  }
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_IDLE)
    return;
  if ((phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK ||
       (phase == LJ_GC2_SWEEP && !gc2_sweep_bridge_ready_acq(g))) &&
      gc2_jit_phase_gate_acq(g) != 0)
    gc2_jit_phase_gate_close(g);
  if ((phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK ||
       (phase == LJ_GC2_SWEEP && !gc2_sweep_bridge_ready_acq(g))) &&
      lj_tg_any_jit_active(g)) {
    /* Preserve-abort is opportunistic. Closing entry requests an ordinary
    ** trace/FFI exit; this caller never waits for the active publication. */
    lj_gc2_worker_wake(g);
    return;
  }
  /* A running GC owner may still mutate mark/sweep work after an IDLE store.
  ** Preserve-root callers rescue their exact object into the active phase, so
  ** abort remains opportunistic until the worker token is quiescent. */
  if (gc2_worker_active_acq(g) != 0) {
    lj_gc2_worker_wake(g);
    return;
  }
  /* A close actor owns exactly 0->GCSCAN. It never overwrites an admitted MARK
  ** request and competing closers cannot clear this actor's sentinel. */
  if (!gc2_phase_gate_try(g)) {
    lj_gc2_worker_wake(g);
    return;
  }
  if (gc2_worker_active_acq(g) != 0) {
    gc2_phase_gate_release(g);
    lj_gc2_worker_wake(g);
    return;
  }
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_IDLE) {
    gc2_phase_gate_release(g);
    return;
  }
  if ((phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK ||
       (phase == LJ_GC2_SWEEP && !gc2_sweep_bridge_ready_acq(g))) &&
      (gc2_jit_phase_gate_acq(g) != 0 || lj_tg_any_jit_active(g))) {
    gc2_phase_gate_release(g);
    lj_gc2_worker_wake(g);
    return;
  }
  if (phase == LJ_GC2_SWEEP) {
    if (gc2_sweep_bridge_ready_acq(g) || gc2_sweep_root_done_acq(g) ||
	gc2_sweep_grace_needed_acq(g)) {
      gc2_phase_gate_release(g);
      lj_gc2_worker_wake(g);
      return;
    }
    (void)lj_gc2_handshake(g, LJ_GC2_HS_RESTORE_ALLOC);
    if (lj_gc2_sweep_needs_restore(g)) {
      /* Restore can lose its exact SEALED->OPEN arbitration to a counted
      ** publisher. Keep SWEEP and every frozen generation intact; the last
      ** publisher release wakes a later bounded abort/collector attempt. */
      gc2_phase_gate_release(g);
      lj_gc2_worker_wake(g);
      return;
    }
  }
  if (gc2_recovery_failed_veto(g)) {
    gc2_phase_gate_release(g);
    lj_gc2_worker_wake(g);
    return;
  }
  lj_str_gc2_sweep_abort(g);
  gc2_sweep_bridge_ready_rel(g, 0);
  gc2_sweep_root_scanned_rel(g, 0);
  /*
  ** A mutator can publish a late preserve root while another thread is closing
  ** WEAK. Do not start with an SSB-flush handshake: the collector may be inside
  ** a synchronous weak-table walk and cannot acknowledge until it observes the
  ** abort. Publish IDLE first; stale SSB entries are conservative and are flushed
  ** by the next cycle's normal start/transition handshakes.
  */
  phase = gc2_phase_xchg_acqrel(g, LJ_GC2_IDLE);
  /* The reset accepts the two defensive forward-transition skews: legacy WEAK
  ** with typed MARK, and legacy WEAK with typed SWEEP_OPEN. It still pins every
  ** invalid/gated authority and keeps NO_RECLAIM absorbing. */
  gc2_activation_abort_reset(g, phase);
  if (phase == LJ_GC2_IDLE) {
    gc2_jit_phase_gate_open_idle(g);
    gc2_phase_gate_release(g);
    lj_gc2_worker_wake(g);
    return;
  }
  gc2_mark_root_scanned_rel(g, 0);
  gc2_mark_close_intent_rel(g, 0);
  lj_gc2_worker_wake(g);
  if (phase != LJ_GC2_IDLE)
    gc2_preserve_abort_to_idle_add(g, 1);
  lj_gc2_worker_wake(g);
  (void)gc2_idle_transition_handshake(
    g, gc2_idle_barrier_actions(g, 0));
  /* Queue references and NEEDSCAN membership remain a conservative next-cycle
  ** root set. Walking them here would race a worker-owned detached suffix or a
  ** resumed idle-generational producer. The next MARK start preserves and
  ** drains the exact work instead. */
  (void)lj_tg_reclaim_dead(g);
  gc2_jit_phase_gate_open_idle(g);
  gc2_phase_gate_release(g);
}

void lj_gc2_preserve_root(global_State *g, GCobj *o)
{
  uint32_t phase;
  if (!g || !o)
    return;
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_SWEEP) {
    /* Quarantine makes SWEEP resurrection a normal state transition. Aborting
    ** to IDLE here would strand detached gcw links and NEEDSWEEP arenas. */
    (void)lj_gc2_trace_sweep_root(g, o);
    return;
  }
  if (phase != LJ_GC2_IDLE)
    lj_gc2_preserve_abort_to_idle(g);
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_IDLE) {
    lj_gc2_remember_root(g, o);
  } else if (phase == LJ_GC2_SWEEP) {
    /* A WEAK->SWEEP actor can own the phase gate after the abort attempt. Its
    ** rescue barrier is already authoritative for this exact late root. */
    (void)lj_gc2_trace_sweep_root(g, o);
  } else if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK) {
    /* Losing the phase gate is not permission to drop an ephemeral native
    ** root. Mark/enqueue it in the active cycle; the transition predicates and
    ** post-SWEEP drain account for the resulting SSB/mark publication. */
    gc2_mark_thread_root_obj(g, o);
  } else {
    gc2_recovery_fail_closed(g);
  }
}

int lj_gc2_sweep_to_idle(global_State *g)
{
  uint32_t phase;
  uint64_t live;
  if (!g)
    return 0;
  if (gc2_jit_sweep_turn_deferred(g))
    return 0;
  if (!gc2_worker_claim(g))
    return 0;  /* 05 section 5.8 scheduler close waits for worker owner. */
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_SWEEP) {
    gc2_worker_release(g);
    return 0;
  }
  gc2_jit_phase_gate_close(g);
  if (lj_tg_any_jit_active(g) || gc2_jit_recorder_active(g)) {
    /* Never wait for a peer in blocking FFI, and never self-wait from a trace
    ** helper. Allocation checks observe the closed gate; x64 loop XPOLL does
    ** the same. A later owner retries after every TG has published quiescence. */
    if (gc2_jit_recorder_active(g))
      lj_trace_abort(g);
    gc2_jit_sweep_displaced_rel(g, 1);
    gc2_worker_release(g);
    return 0;
  }
  if (gc2_phase_acq(g) != LJ_GC2_SWEEP) {
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
      gc2_jit_phase_gate_open_sweep(g, 0);
    else if (gc2_phase_acq(g) == LJ_GC2_IDLE)
      gc2_jit_phase_gate_open_idle(g);
    gc2_worker_release(g);
    return 0;
  }
  if (!gc2_phase_gate_try(g)) {
    gc2_jit_phase_gate_open_sweep(g, 0);
    gc2_worker_release(g);
    return 0;
  }
  if (gc2_phase_acq(g) != LJ_GC2_SWEEP ||
      gc2_sweep_blocked_by_finalizer(g) ||
      !gc2_sweep_bridge_ready_acq(g) ||
      gc2_sweep_root_scanned_acq(g) != 1 ||
      lj_tg_any_jit_active(g) || gc2_jit_recorder_active(g) ||
      gc2_weak_owned_peer_active(g) ||
      gc2_sweep_bridge_owner_roots_pending(g) ||
      gc2_table_rescan_pending_acq(g) != 0 ||
      gc2_sweep_grace_needed_acq(g) || !lj_gc2_ssb_empty(g) ||
      !gc2_recovery_empty(g) ||
      !gc2_grey_empty(g) || lj_gc2_sweep_needs_prepare(g) ||
      lj_gc2_sweep_pending(g) ||
      gc2_marks_this_round_xchg_acqrel(g, 0) != 0) {
    gc2_jit_phase_gate_open_sweep(g, 0);
    gc2_phase_gate_release(g);
    gc2_worker_release(g);
    return 0;
  }
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  (void)gc2_flush_ssb(g, G2TG(g), 0);
  (void)gc2_drain_ssb_owned(g);
  /* The final handshake can publish sweep-rescue edges after the first close
  ** predicate. Revalidate while retaining the worker token; using the generic
  ** drain here would fail by trying to acquire our own token. */
  if (gc2_phase_acq(g) != LJ_GC2_SWEEP ||
      gc2_sweep_blocked_by_finalizer(g) ||
      gc2_sweep_root_scanned_acq(g) != 1 ||
      lj_tg_any_jit_active(g) || gc2_jit_recorder_active(g) ||
      gc2_weak_owned_peer_active(g) ||
      gc2_sweep_bridge_owner_roots_pending(g) ||
      gc2_table_rescan_pending_acq(g) != 0 ||
      gc2_sweep_grace_needed_acq(g) || !lj_gc2_ssb_empty(g) ||
      !gc2_recovery_empty(g) ||
      !gc2_grey_empty(g) || lj_gc2_sweep_needs_prepare(g) ||
      lj_gc2_sweep_pending(g) ||
      gc2_marks_this_round_xchg_acqrel(g, 0) != 0) {
    gc2_jit_phase_gate_open_sweep(g, 0);
    gc2_phase_gate_release(g);
    gc2_worker_release(g);
    lj_gc2_worker_wake(g);
    return 0;
  }
  lj_str_gc2_sweep_finish(g);
  live = lj_gc2_sweep_live_aggregate(g);
  lj_gc2_update_minor_survival_policy(g, live);
  gc2_update_public_minor_gates(g);
  phase = gc2_phase_xchg_acqrel(g, LJ_GC2_IDLE);
  /* Legacy remains the close authority. This exact reset only abandons the
  ** veto-only SWEEP_OPEN generation and never publishes CLOSING or COMMIT. */
  if (phase == LJ_GC2_SWEEP)
    gc2_activation_mirror_edge(g, phase, LJ_GC2_IDLE);
  else
    gc2_activation_pin_no_reclaim(g);
  gc2_mark_root_scanned_rel(g, 0);
  gc2_mark_close_intent_rel(g, 0);
  lj_gc2_worker_wake(g);
  if (phase != LJ_GC2_SWEEP) {
    if (gc2_phase_acq(g) == LJ_GC2_IDLE)
      gc2_jit_phase_gate_open_idle(g);
    gc2_phase_gate_release(g);
    gc2_worker_release(g);
    return 0;
  }
  /* GC2 phase is the sole collector authority. Once its SWEEP closes, keep the
  ** legacy/color compatibility state coherent regardless of which bounded
  ** bridge substate last touched it; public GC APIs still observe GCSpause. */
  g->gc.state = GCSpause;
  g->gc.debt = 0;
  gc2_sweep_to_idle_add(g, 1);
  gc2_sweep_grace_needed_rel(g, 0);
  gc2_sweep_root_scanned_rel(g, 0);
  gc2_sweep_root_done_rel(g, 0);
  gc2_sweep_root_cursor_rel(g, NULL);
  /*
  ** The close owner has finished the SWEEP->IDLE transition, and cycle_leader
  ** remains set until pacing is republished below. Release worker_active before
  ** the grace handshake so lj_gc2_reclaim_retired() can drain retired strtab,
  ** table, trace, mcode, and ctype/clib state at the completed epoch.
  */
  gc2_worker_release(g);
  (void)gc2_idle_transition_handshake(
    g, gc2_idle_barrier_actions(g, 0));
  /* An admitted b1.2 string batch is consumed by the exact IDLE reclaimer
  ** inside the handshake above.  Never reopen attach/pool admission while an
  ** unlinked canonical body remains undiscoverable from the main table. */
  if (LJ_UNLIKELY(!lj_str_gc2_reclaim_complete(g))) {
    lj_assertG(0, "explicit string retirement survived IDLE grace drain");
    abort();
  }
  lj_assertG(gc2_thread_scan_needscan_pending_acq(g) == 0,
	     "thread NEEDSCAN leaked to normal sweep close");
  lj_assertG(gc2_table_rescan_pending_acq(g) == 0,
	     "table NEEDSCAN leaked to normal sweep close");
  (void)gc2_clear_container_needscan_all(g);
  (void)lj_tg_reclaim_dead(g);
  lj_gc2_update_pacing(g);
  gc2_jit_phase_gate_open_idle(g);
  gc2_phase_gate_release(g);
  return 1;
}

void lj_gc2_cycle_to_idle(global_State *g)
{
  uint32_t phase;
  uint64_t live = 0;
  if (!g)
    return;
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_IDLE)
    return;
  if (phase == LJ_GC2_SWEEP &&
      (gc2_sweep_bridge_ready_acq(g) || gc2_sweep_root_done_acq(g) ||
       gc2_sweep_grace_needed_acq(g))) {
    /* Once gcw detachment/quarantine starts, only the normal bounded close may
    ** publish IDLE. A forced close would orphan exact variable cdata headers. */
    gc2_jit_sweep_yield_until_ns_rel(g, 0);
    if (!lj_gc2_sweep_to_idle(g))
      lj_gc2_worker_wake(g);
    return;
  }
  if (!gc2_worker_claim(g)) {
    lj_gc2_worker_wake(g);
    return;
  }
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_IDLE) {
    gc2_worker_release(g);
    return;
  }
  if ((phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK ||
       (phase == LJ_GC2_SWEEP && !gc2_sweep_bridge_ready_acq(g))) &&
      gc2_jit_phase_gate_acq(g) != 0)
    gc2_jit_phase_gate_close(g);
  if ((phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK ||
       (phase == LJ_GC2_SWEEP && !gc2_sweep_bridge_ready_acq(g))) &&
      lj_tg_any_jit_active(g)) {
    gc2_worker_release(g);
    lj_gc2_worker_wake(g);
    return;
  }
  if (phase == LJ_GC2_SWEEP &&
      (gc2_sweep_bridge_ready_acq(g) || gc2_sweep_root_done_acq(g) ||
       gc2_sweep_grace_needed_acq(g))) {
    gc2_worker_release(g);
    gc2_jit_sweep_yield_until_ns_rel(g, 0);
    if (!lj_gc2_sweep_to_idle(g))
      lj_gc2_worker_wake(g);
    return;
  }
  if (!gc2_phase_gate_try(g)) {
    gc2_worker_release(g);
    lj_gc2_worker_wake(g);
    return;
  }
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_IDLE) {
    gc2_phase_gate_release(g);
    gc2_worker_release(g);
    return;
  }
  if ((phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK ||
       (phase == LJ_GC2_SWEEP && !gc2_sweep_bridge_ready_acq(g))) &&
      (gc2_jit_phase_gate_acq(g) != 0 || lj_tg_any_jit_active(g))) {
    gc2_phase_gate_release(g);
    gc2_worker_release(g);
    lj_gc2_worker_wake(g);
    return;
  }
  if (phase == LJ_GC2_SWEEP &&
      (gc2_sweep_bridge_ready_acq(g) || gc2_sweep_root_done_acq(g) ||
       gc2_sweep_grace_needed_acq(g))) {
    gc2_phase_gate_release(g);
    gc2_worker_release(g);
    gc2_jit_sweep_yield_until_ns_rel(g, 0);
    if (!lj_gc2_sweep_to_idle(g))
      lj_gc2_worker_wake(g);
    return;
  }
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  (void)gc2_flush_ssb(g, G2TG(g), 0);
  (void)gc2_drain_ssb_owned(g);
  if (!gc2_recovery_empty(g)) {
    /* Drainable identities or a sticky failure both retain the active phase.
    ** In particular, abort-reset must never erase NO_RECLAIM. */
    gc2_phase_gate_release(g);
    gc2_worker_release(g);
    lj_gc2_worker_wake(g);
    return;
  }
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    /* No root was detached yet. Restore frozen generations on their owning TG
    ** before publishing IDLE; direct cross-owner bin mutation is forbidden. */
    (void)lj_gc2_handshake(g, LJ_GC2_HS_RESTORE_ALLOC);
    if (lj_gc2_sweep_needs_restore(g)) {
      /* A counted publisher defeated one owner's exact restore LP. Keep the
      ** cycle and all sweep-old registries intact; its release edge wakes the
      ** next bounded abort attempt. */
      gc2_phase_gate_release(g);
      gc2_worker_release(g);
      lj_gc2_worker_wake(g);
      return;
    }
    if (!gc2_recovery_empty(g)) {
      gc2_phase_gate_release(g);
      gc2_worker_release(g);
      lj_gc2_worker_wake(g);
      return;
    }
    live = lj_gc2_sweep_live_aggregate(g);
    lj_gc2_update_minor_survival_policy(g, live);
  }
  lj_str_gc2_sweep_abort(g);
  phase = gc2_phase_xchg_acqrel(g, LJ_GC2_IDLE);
  /* Forced and pre-bridge closes accept only the same defensive forward skews
  ** as preserve abort, while the exact GCSCAN sentinel remains owned. */
  gc2_activation_abort_reset(g, phase);
  if (phase == LJ_GC2_IDLE) {
    gc2_jit_phase_gate_open_idle(g);
    gc2_phase_gate_release(g);
    gc2_worker_release(g);
    lj_gc2_worker_wake(g);
    return;
  }
  gc2_mark_root_scanned_rel(g, 0);
  gc2_mark_close_intent_rel(g, 0);
  lj_gc2_worker_wake(g);
  if (phase == LJ_GC2_SWEEP) {
    gc2_sweep_to_idle_add(g, 1);
  }
  gc2_sweep_bridge_ready_rel(g, 0);
  gc2_sweep_root_scanned_rel(g, 0);
  gc2_sweep_grace_needed_rel(g, 0);
  gc2_sweep_root_done_rel(g, 0);
  gc2_sweep_root_cursor_rel(g, NULL);
  gc2_update_public_minor_gates(g);
  gc2_worker_release(g);
  (void)gc2_idle_transition_handshake(
    g, gc2_idle_barrier_actions(g, 0));
  /* Forced close preserves queue membership for the next MARK cycle. This is
  ** conservative and avoids cross-owner queue/cursor mutation after IDLE. */
  (void)lj_tg_reclaim_dead(g);
  lj_gc2_update_pacing(g);
  gc2_jit_phase_gate_open_idle(g);
  gc2_phase_gate_release(g);
}

int lj_gc2_sweep_bridge_close(global_State *g)
{
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
    return lj_gc2_sweep_to_idle(g);
  lj_gc2_cycle_to_idle(g);  /* Preserving full-GC fast-forward sweep. */
  return 1;
}

uint32_t lj_gc2_handshake(global_State *g, uint32_t actions)
{
  return lj_safepoint_handshake(g, actions);
}

uint64_t lj_gc2_retire_epoch(global_State *g)
{
  return g ? gc2_hs_epoch_acq(g) : 0;
}

void lj_gc2_stats_snapshot(global_State *g, GC2StatsSnapshot *s)
{
  uint32_t i;
  if (!s)
    return;
  if (!g) {
    memset(s, 0, sizeof(*s));
    return;
  }
  s->total_bytes = lj_gc_total_load(g);
  s->phase = gc2_phase_acq(g);
  s->gc_state = la_load8_acq(&g->gc.state);
  s->generational = gc2_generational_acq(g);
  s->cycle_minor_requested = gc2_cycle_minor_requested_acq(g);
  s->cycle_sweep_minor = gc2_cycle_sweep_minor_acq(g);
  s->minor_sweep_enabled = gc2_minor_sweep_enabled_acq(g);
  s->cycle_roots_minor = lj_gc2_minor_roots_active(g);
  s->minor_roots_enabled = gc2_minor_roots_enabled_acq(g);
  s->cycle_requests = gc2_cycle_requests_acq(g);
  s->cycle_starts = gc2_cycle_starts_acq(g);
  s->major_cycle_starts = gc2_major_cycle_starts_acq(g);
  s->minor_cycle_requests = gc2_minor_cycle_requests_acq(g);
  s->minor_cycle_starts = gc2_minor_cycle_starts_acq(g);
  s->minor_sweep_deferred = gc2_minor_sweep_deferred_acq(g);
  s->minor_sweep_arenas = gc2_minor_sweep_arenas_acq(g);
  s->minor_roots_deferred = gc2_minor_roots_deferred_acq(g);
  s->major_root_scans = gc2_major_root_scans_acq(g);
  s->minor_root_scans = gc2_minor_root_scans_acq(g);
  s->pending_root_flushes = gc2_pending_root_flushes_acq(g);
  s->pending_root_flushed = gc2_pending_root_flushed_acq(g);
  s->pending_root_flush_max = gc2_pending_root_flush_max_acq(g);
  s->minor_survival_base_live = gc2_minor_survival_base_live_acq(g);
  s->minor_survival_bytes = gc2_minor_survival_bytes_acq(g);
  s->minor_survival_pct = gc2_minor_survival_pct_acq(g);
  s->minor_survival_threshold_pct = gc2_minor_survival_threshold_pct_acq(g);
  s->minor_survival_major_requests = gc2_minor_survival_major_requests_acq(g);
  s->remembered_barriers = gc2_remembered_barriers_acq(g);
  s->remembered_pushed = gc2_remembered_pushed_acq(g);
  s->remembered_overflows = gc2_remembered_overflows_acq(g);
  s->remembered_filtered = gc2_remembered_filtered_acq(g);
  s->remembered_drained = gc2_remembered_drained_acq(g);
  s->recovery_items = gc2_recovery_items_acq(g);
  s->recovery_huge_items = gc2_recovery_huge_items_acq(g);
  s->recovery_published = gc2_recovery_published_acq(g);
  s->recovery_redirtied = gc2_recovery_redirtied_acq(g);
  s->recovery_drained = gc2_recovery_drained_acq(g);
  s->recovery_failed = gc2_recovery_failed_acq(g);
  s->poll_ack_samples = gc2_hs_ack_latency_samples_acq(g);
  s->poll_ack_latency_sum_ns = gc2_hs_ack_latency_sum_acq(g);
  s->poll_ack_latency_max_ns = gc2_hs_ack_latency_max_acq(g);
  for (i = 0; i < LJ_GC2_HS_LATENCY_BUCKETS; i++)
    s->poll_ack_latency_buckets[i] = gc2_hs_ack_latency_bucket_acq(g, i);
  s->alloc_total_bytes = gc2_alloc_total_bytes_acq(g);
  s->alloc_since_trigger = lj_gc2_alloc_since_load(g);
  s->cycle_alloc_bytes = lj_gc2_cycle_alloc_load(g);
  s->trigger_bytes = lj_gc2_trigger_load(g);
  s->hard_bytes = lj_gc2_hard_load(g);
  s->assist_runs = gc2_assist_runs_acq(g);
  s->assist_grey_drained = gc2_assist_grey_drained_acq(g);
  s->assist_ssb_converted = gc2_assist_ssb_converted_acq(g);
  s->assist_weak_drained = gc2_assist_weak_drained_acq(g);
  s->jit_hard_checks = gc2_jit_hard_checks_acq(g);
  s->interp_hard_checks = gc2_interp_hard_checks_acq(g);
  s->worker_runs = gc2_worker_runs_acq(g);
  s->worker_grey_drained = gc2_worker_grey_drained_acq(g);
  s->worker_ssb_converted = gc2_worker_ssb_converted_acq(g);
  s->worker_weak_drained = gc2_worker_weak_drained_acq(g);
  s->worker_idle_declares = gc2_worker_idle_declares_acq(g);
  s->worker_busy_retries = gc2_worker_busy_retries_acq(g);
  s->worker_wakes = gc2_worker_wakes_acq(g);
  s->worker_parks = gc2_worker_parks_acq(g);
  s->worker_async_progress = gc2_worker_async_progress_acq(g);
  s->thread_scan_frame_fallbacks =
    gc2_thread_scan_frame_fallbacks_acq(g);
  s->ffi_native_scan_attempts = gc2_ffi_native_scan_attempts_acq(g);
  s->ffi_native_scan_stable_frames =
    gc2_ffi_native_scan_stable_frames_acq(g);
  s->ffi_native_scan_retries = gc2_ffi_native_scan_retries_acq(g);
  s->ffi_native_scan_invalid = gc2_ffi_native_scan_invalid_acq(g);
  s->sweep_owner_runs = gc2_sweep_owner_runs_acq(g);
  s->sweep_owner_arenas = gc2_sweep_owner_arenas_acq(g);
  s->sweep_owner_live_cells = gc2_sweep_owner_live_cells_acq(g);
  s->sweep_live_updates = gc2_sweep_live_updates_acq(g);
  s->sweep_live_huge_bytes = gc2_sweep_live_huge_bytes_acq(g);
  s->live_estimate = gc2_live_estimate_acq(g);
  s->smr_reclaim_runs = gc2_smr_reclaim_runs_acq(g);
  s->smr_reclaimed = gc2_smr_reclaimed_acq(g);
  gc2_root_spine_counts(g, &s->root_spine_objects,
				&s->root_spine_tombstones,
				&s->root_spine_count_capped);
  s->root_spine_count_cap = LJ_GC2_ROOT_SCAN_LIMIT;
  if (g && g->main_tg) {
    TGAlloc *alloc = &g->main_tg->alloc;
    s->arena_traversable_owned =
      gc2_arena_list_count(alloc->owned[LJ_ARENAK_TRAVERSABLE]);
    s->arena_traversable_needsweep =
      gc2_arena_list_count(alloc->needsweep[LJ_ARENAK_TRAVERSABLE]);
    s->arena_traversable_binmask = alloc->binmask[LJ_ARENAK_TRAVERSABLE];
  } else {
    s->arena_traversable_owned = 0;
    s->arena_traversable_needsweep = 0;
    s->arena_traversable_binmask = 0;
  }
  s->weak_clear_tables = gc2_weak_clear_tables_acq(g);
  s->weak_clear_cleared = gc2_weak_clear_cleared_acq(g);
  s->weak_bridge_skipped = gc2_weak_bridge_skipped_acq(g);
  s->weak_bridge_fallbacks = gc2_weak_bridge_fallbacks_acq(g);
  s->weak_bridge_backfills = gc2_weak_bridge_backfills_acq(g);
  s->weak_bridge_backfill_tables = gc2_weak_bridge_backfill_tables_acq(g);
  s->weak_bridge_backfill_slots = gc2_weak_bridge_backfill_slots_acq(g);
  s->weak_bridge_backfill_cleared = gc2_weak_bridge_backfill_cleared_acq(g);
  s->weak_keys_marked = gc2_weak_keys_marked_acq(g);
  s->weak_values_marked = gc2_weak_values_marked_acq(g);
  s->finreg_cdata_sets = gc2_finreg_cdata_sets_acq(g);
  s->finreg_cdata_clears = gc2_finreg_cdata_clears_acq(g);
  s->finreg_cdata_queued = gc2_finreg_cdata_queued_acq(g);
  s->finreg_cdata_sweep_queued = gc2_finreg_cdata_sweep_queued_acq(g);
  s->finreg_cdata_pweak_queued = gc2_finreg_cdata_pweak_queued_acq(g);
  s->finreg_cdata_pweak_claimed = gc2_finreg_cdata_pweak_claimed_acq(g);
  s->finreg_cdata_preclaim_overflow =
    gc2_finreg_cdata_preclaim_overflow_acq(g);
  s->finreg_cdata_preclaim_dispatched =
    gc2_finreg_cdata_preclaim_dispatched_acq(g);
  s->finreg_cdata_order_seen = gc2_finreg_cdata_order_seen_acq(g);
  s->finreg_cdata_order_claimed = gc2_finreg_cdata_order_claimed_acq(g);
  s->finreg_cdata_order_unlinked = gc2_finreg_cdata_order_unlinked_acq(g);
  s->finreg_cdata_order_queued = gc2_finreg_cdata_order_queued_acq(g);
  s->finreg_cdata_order_retired = gc2_finreg_cdata_order_retired_acq(g);
  s->finreg_cdata_order_tombstones = gc2_finreg_cdata_order_tombstones_acq(g);
  s->finreg_cdata_order_fallbacks = gc2_finreg_cdata_order_fallbacks_acq(g);
  s->finreg_cdata_pending_order_hits =
    gc2_finreg_cdata_pending_order_hits_acq(g);
  s->finreg_udata_sets = gc2_finreg_udata_sets_acq(g);
  s->finreg_udata_clears = gc2_finreg_udata_clears_acq(g);
  s->finreg_udata_queued = gc2_finreg_udata_queued_acq(g);
  s->finreg_udata_registered = gc2_finreg_udata_registered_acq(g);
  s->finreg_udata_retired_nodes = gc2_finreg_udata_retired_nodes_acq(g);
  s->finreg_udata_discovered = gc2_finreg_udata_discovered_acq(g);
  s->finreg_udata_forgets = gc2_finreg_udata_forgets_acq(g);
  s->finalizer_queued = gc2_finalizer_queued_acq(g);
  s->finalizer_dequeued = gc2_finalizer_dequeued_acq(g);
  s->finalizer_mpsc_drained = gc2_finalizer_mpsc_drained_acq(g);
  s->finalizer_enters = gc2_finalizer_enters_acq(g);
  s->finalizer_leaves = gc2_finalizer_leaves_acq(g);
  s->finalizer_sweep_blocks = gc2_finalizer_sweep_blocks_acq(g);
  s->finalizer_spawn_deferrals = gc2_finalizer_spawn_deferrals_acq(g);
  s->finalizer_spawn_release_wakes =
    gc2_finalizer_spawn_release_wakes_acq(g);
}

static LJ_AINLINE int gc2_reclaim_retired_ready(global_State *g)
{
  return g->gc.state == GCSpause &&
	 gc2_phase_acq(g) == LJ_GC2_IDLE &&
	 gc2_worker_active_acq(g) == 0 &&
	 gc2_assist_active_acq(g) == 0 &&
	 gc2_weak_drain_active_acq(g) == 0 &&
	 gc2_weak_write_active_acq(g) == 0 &&
	 !lj_gc2_activation_reclaim_veto(g);
}

int lj_gc2_smr_read_try(global_State *g)
{
  LJThrGC2TLS *tls;
  if (!g)
    return 1;
  tls = gc2_tls_current();
  if (gc2_smr_reader_tls_active_state(tls, g)) {
    if (LJ_UNLIKELY(tls->smr_reader_depth == ~(uint32_t)0)) {
      lj_assertG(0, "gc2 nested SMR reader overflow");
      abort();
    }
    tls->smr_reader_depth++;
    return 1;
  }
  if (gc2_smr_reclaiming_acq(g) != LJ_GC2_SMR_OPEN)
    return 0;
  (void)gc2_smr_readers_add(g, 1);
  if (gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_OPEN) {
    /* A nested read of a different independent Lua universe remains a normal
    ** counted reader. Only the outer tracked universe gets reentrant elision. */
    if (tls && tls->smr_reader_g == NULL) {
      tls->smr_reader_g = g;
      tls->smr_reader_depth = 1;
    }
    return 1;
  }
  (void)gc2_smr_readers_sub(g, 1);
  return 0;
}

typedef enum GC2HugeRegistryLease {
  GC2_HUGE_REGISTRY_NONE = 0,
  GC2_HUGE_REGISTRY_FULL,
  GC2_HUGE_REGISTRY_SWEEP
} GC2HugeRegistryLease;

/* A SWEEP owner keeps TG/HugeTab topology stable, but may reclaim bodies. A
** positive scan under this narrower reader is therefore legal only when it
** ends in the same-slot HugeReader/mark CAS before any body byte is inspected;
** a negative scan touches registry slots only. It must not install ordinary
** SMR TLS: nested generic readers need the full OPEN mode. */
static int gc2_huge_registry_read_try(global_State *g,
				       GC2HugeRegistryLease *lease)
{
  uint32_t mode, old;
  if (lease)
    *lease = GC2_HUGE_REGISTRY_NONE;
  if (!g || !lease)
    return 0;
  mode = gc2_smr_reclaiming_acq(g);
  if (mode == LJ_GC2_SMR_OPEN) {
    if (!lj_gc2_smr_read_try(g))
      return 0;
    *lease = GC2_HUGE_REGISTRY_FULL;
    return 1;
  }
  if (mode != LJ_GC2_SMR_SWEEP_STABLE)
    return 0;
  old = gc2_smr_readers_add(g, 1);
  if (LJ_UNLIKELY(old == ~(uint32_t)0)) {
    (void)gc2_smr_readers_sub(g, 1);
    lj_assertG(0, "GC2 Huge registry reader overflow");
    abort();
  }
  la_fence_seq();
  if (gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_SWEEP_STABLE) {
    *lease = GC2_HUGE_REGISTRY_SWEEP;
    return 1;
  }
  old = gc2_smr_readers_sub(g, 1);
  lj_assertG(old != 0, "GC2 Huge registry reader rollback underflow");
  UNUSED(old);
  return 0;
}

static void gc2_huge_registry_read_leave(global_State *g,
					 GC2HugeRegistryLease *lease)
{
  uint32_t old;
  if (!g || !lease || *lease == GC2_HUGE_REGISTRY_NONE)
    return;
  if (*lease == GC2_HUGE_REGISTRY_FULL) {
    lj_gc2_smr_read_leave(g);
  } else {
    lj_assertG(*lease == GC2_HUGE_REGISTRY_SWEEP,
	       "bad GC2 Huge registry lease");
    old = gc2_smr_readers_sub(g, 1);
    lj_assertG(old != 0, "GC2 Huge registry reader underflow");
    UNUSED(old);
  }
  *lease = GC2_HUGE_REGISTRY_NONE;
}

void lj_gc2_smr_read_enter(global_State *g)
{
  while (!lj_gc2_smr_read_try(g))
    (void)lj_thr_retry_yield(NULL);
}

void lj_gc2_smr_read_leave(global_State *g)
{
  LJThrGC2TLS *tls;
  uint32_t old;
  if (!g)
    return;
  tls = gc2_tls_current();
  if (tls && tls->smr_reader_g == g) {
    if (LJ_UNLIKELY(tls->smr_reader_depth == 0)) {
      lj_assertG(0, "gc2 TLS SMR reader underflow");
      abort();
    }
    if (--tls->smr_reader_depth != 0)
      return;
    tls->smr_reader_g = NULL;
  }
  old = gc2_smr_readers_sub(g, 1);
  lj_assertG(old != 0, "gc2 SMR reader underflow");
  UNUSED(old);
}

int lj_gc2_reclaim_context_held(global_State *g)
{
  return gc2_reclaim_tls_active(g);
}

static int gc2_idle_reclaim_enter(global_State *g)
{
  LJThrGC2TLS *tls;
  uint32_t expect = LJ_GC2_SMR_OPEN;
  int gate_owned = 0;
  if (!g)
    return 0;
  tls = gc2_tls_current();
  if (!tls)
    return 0;
  lj_assertG(tls->idle_reclaim_gate_owned == 0,
	     "nested IDLE reclaim gate ownership");
  if (gc2_cycle_leader_acq(g) == LJ_THREAD_GCSCAN &&
      tls->idle_transition_gate_g != g)
    return 0;
  if (!gc2_reclaim_retired_ready(g) ||
      !gc2_smr_reclaiming_cas(g, &expect, LJ_GC2_SMR_META_EXCLUSIVE))
    return 0;
  /* smr_reclaiming excludes a new worker claim. Own the IDLE native-entry
  ** closure with an exact 1->0 CAS: a zero gate may instead belong to a MARK
  ** admission which raced our initial readiness sample, and this reclaimer
  ** must neither destroy behind nor reopen that actor's closure. */
  if (gc2_jit_phase_gate_acq(g) == 0 &&
      tls->idle_transition_gate_g == g &&
      gc2_cycle_leader_acq(g) == LJ_THREAD_GCSCAN) {
    /* The transition owner retains gate0 and GCSCAN around this same-thread
    ** callback. Repeat the SC fence before the nested zero-active proof, but
    ** do not claim permission to reopen its gate. */
  } else {
    expect = 1;
    if (!gc2_jit_phase_gate_cas(g, &expect, 0))
      goto fail_unowned;
    gate_owned = 1;
    gc2_jit_sweep_yield_until_ns_rel(g, 0);
  }
  la_fence_seq();
  /* The SC fence pairs with every x64 intent publication/recheck. Therefore a
  ** late entry either rejected the owned close or is visible here before any
  ** retired string/table/ctype/trace/mcode body can be released. */
  if (!gc2_reclaim_retired_ready(g) ||
      gc2_smr_readers_acq(g) != 0 || lj_tg_any_jit_active(g) ||
      !gc2_reclaim_tls_enter_state(tls, g))
    goto fail_gate;
  tls->idle_reclaim_gate_owned = (uint32_t)gate_owned;
  gc2_idle_reclaim_test_pause_after_jit_quiescence();
  return 1;

fail_gate:
  /* Reopen only our own still-IDLE closure while smr_reclaiming continues to
  ** exclude MARK worker admission. A newly published request deliberately
  ** inherits the closed gate and will be retried after this word is released. */
  if (gate_owned && gc2_reclaim_retired_ready(g) &&
      gc2_cycle_leader_acq(g) == 0) {
    uint32_t closed = 0;
    gc2_jit_sweep_displaced_rel(g, 0);
    gc2_jit_sweep_yield_until_ns_rel(g, 0);
    (void)gc2_jit_phase_gate_cas(g, &closed, 1);
  }
fail_unowned:
  gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
  if (gc2_cycle_leader_acq(g) != 0)
    lj_gc2_worker_wake(g);
  return 0;
}

static void gc2_idle_reclaim_leave(global_State *g)
{
  LJThrGC2TLS *tls = gc2_tls_current();
  uint32_t request;
  uint32_t gate_owned;
  if (LJ_UNLIKELY(!tls)) {
    lj_assertG(0, "GC2 IDLE reclaimer lost its thread cell");
    abort();
  }
  gate_owned = tls->idle_reclaim_gate_owned;
  lj_assertG(gc2_jit_phase_gate_acq(g) == 0,
	     "IDLE reclaim scope outlived owned closed JIT gate");
  lj_assertG(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_META_EXCLUSIVE,
	     "IDLE reclaim scope lost exclusive registry mode");
  /* Reopen before dropping smr_reclaiming, so no MARK worker can close the
  ** same binary gate between our eligibility check and owned 0->1 CAS. A
  ** pending request retains closure; its later worker performs MARK admission. */
  request = gc2_cycle_leader_acq(g);
  if (gate_owned && gc2_reclaim_retired_ready(g) && request == 0) {
    uint32_t closed = 0;
    gc2_jit_sweep_displaced_rel(g, 0);
    gc2_jit_sweep_yield_until_ns_rel(g, 0);
    (void)gc2_jit_phase_gate_cas(g, &closed, 1);
  }
  if (!gate_owned)
    lj_assertG(tls->idle_transition_gate_g == g &&
	       request == LJ_THREAD_GCSCAN,
	       "borrowed IDLE reclaim escaped transition owner");
  tls->idle_reclaim_gate_owned = 0;
  gc2_reclaim_tls_leave_state(tls, g);
  gc2_smr_reclaiming_rel(g, LJ_GC2_SMR_OPEN);
  if (request != 0 || gc2_cycle_leader_acq(g) != 0)
    lj_gc2_worker_wake(g);
}

#if defined(LJ_GC2_TEST_HELPERS) || defined(LJ_TRACE_TEST_HELPERS)
int lj_gc2_test_idle_reclaim_enter(global_State *g)
{
  return gc2_idle_reclaim_enter(g);
}

void lj_gc2_test_idle_reclaim_leave(global_State *g)
{
  gc2_idle_reclaim_leave(g);
}
#endif

uint32_t lj_gc2_reclaim_retired(global_State *g, uint64_t epoch)
{
  uint32_t n = 0;
  if (!g || epoch == 0)
    return 0;
  if (!gc2_idle_reclaim_enter(g)) {
    /*
    ** Reclaim is opportunistic. SMR readers may cover trace-exit restoration or
    ** table slot validation, and those sections can allocate while rebuilding
    ** interpreter state. Do not wait: an existing reader or competing owner lets
    ** a later retire pass observe a clean grace period.
    */
    return 0;
  }
  n += lj_str_reclaim_retired(g, epoch);  /* 05 section 5.9 SMR drain. */
  n += lj_tab_reclaim_retired(g, epoch);  /* 06 section 6.3.5 SMR drain. */
#if LJ_HASFFI
  n += lj_ctype_reclaim_retired(g, epoch);  /* 11.2 CTState table SMR drain. */
  n += lj_clib_cache_reclaim_retired(g, epoch);  /* 11.7 CLibrary cache SMR. */
#endif
#if LJ_HASJIT
  if (!lj_tg_any_jit_active(g)) {
    jit_State *J = G2J(g);
    int token = 0;
    /*
    ** Mcode and trace retirement share recorder-owned accounting, trace slots,
    ** and J->freetrace. Reclaim them as one opportunistic token transaction;
    ** never wait for a recorder which may itself be inside an SMR read section.
    */
    if (lj_jit_token_held(J) || (token = lj_jit_token_try(J))) {
      /* Trace bodies own the last mcode/exit-table references. Drain them first
      ** so an area which becomes unreferenced can be unmapped in this same
      ** token transaction instead of waiting for another handshake.
      */
      if (!lj_tg_any_jit_active(g)) {
	n += lj_trace_reclaim_retired(g, epoch);  /* 08 section 8.3/8.7. */
	n += lj_mcode_reclaim_retired(g, epoch);  /* 08 section 8.7. */
      }
      if (token)
	lj_jit_token_release(J);
    }
  }
#endif
  if (n) {
    gc2_smr_reclaim_runs_add(g, 1);
    gc2_smr_reclaimed_add(g, n);
  }
  gc2_idle_reclaim_leave(g);
  return n;
}

enum {
  GC2_TV_SCOPE_RETRY = -1,
  GC2_TV_SCOPE_STALE = 0,
  GC2_TV_SCOPE_ADMITTED = 1
};

/* Conservative TValue snapshots can contain ordinary popped, spill, or stale
** table cells. Distinguish a terminal stale word from a transient inability to
** establish lifetime, and return ADMITTED with the exact observation scope
** still held. The caller must retain that scope through every semantic check:
** otherwise address reuse between validation and use can change incarnation. */
static int gc2_tv_admit_scoped(global_State *g, cTValue *tv,
			       GC2MarkScope *scope)
{
  GCobj *o;
  uint32_t gct;
  int status;
  gc2_mark_scope_init(scope);
  if (!tvisgcv(tv))
    return GC2_TV_SCOPE_ADMITTED;
  o = gcval(tv);
#if defined(LJ_GC2_TEST_HELPERS)
  if (gc2_tv_test_take_admission_retry(o))
    return GC2_TV_SCOPE_RETRY;
#endif
  if (itype(tv) == LJ_TSTR && o == obj2gco(&g->strempty))
    return o->gch.gct == ~LJ_TSTR ?
	   GC2_TV_SCOPE_ADMITTED : GC2_TV_SCOPE_STALE;
  status = gc2_observed_obj_status_scoped(g, o, &gct, scope);
  if (status < 0)
    return GC2_TV_SCOPE_RETRY;
  if (status == 0)
    return GC2_TV_SCOPE_STALE;
#if LJ_HASFFI
  if (itype(tv) == LJ_TCDATA) {
    if (gct != (uint32_t)~LJ_TCDATA ||
	(ctype_ctsG(g) == NULL &&
	 (cdataisv(gco2cd(o)) ||
	  ((uintptr_t)o & (uintptr_t)(LJ_CELL_SIZE - 1u)) != 0)))
      goto ignore;
    return GC2_TV_SCOPE_ADMITTED;
  }
#endif
  if ((uint32_t)~itype(tv) == gct)
    return GC2_TV_SCOPE_ADMITTED;
#if LJ_HASFFI
ignore:
#endif
  gc2_mark_scope_leave(scope);
  return GC2_TV_SCOPE_STALE;
}

static int gc2_tv_gcref_type_match_known_status(global_State *g,
						 cTValue *tv)
{
  GC2MarkScope scope;
  int status = gc2_tv_admit_scoped(g, tv, &scope);
  gc2_mark_scope_leave(&scope);
  return status;
}

static int gc2_tv_gcref_type_match_known(global_State *g, cTValue *tv)
{
  return gc2_tv_gcref_type_match_known_status(g, tv) > 0;
}

int lj_gc2_tv_gcref_status_edge(global_State *g, cTValue *tv)
{
  if (!g || !tv)
    return LJ_GC2_TV_EDGE_STALE;
  return gc2_tv_gcref_type_match_known_status(g, tv);
}

int lj_gc2_tv_gcref_valid_edge(global_State *g, cTValue *tv)
{
  return g && tv && gc2_tv_gcref_type_match_known(g, tv);
}

static void gc2_mark_tv(global_State *g, cTValue *tv)
{
  if (tvisgcv(tv) && gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    (void)gc2_trace_sweep_tv_edge(g, tv, 0);
    return;
  }
  if (tvisgcv(tv))
    lj_gc2_markobj(g, gcV(tv));
}

static void gc2_root_rescan_later(global_State *g, GCobj *o)
{
  int pushed;
  /* The sole caller has already sampled gc2.phase through g. Keep that
  ** non-null precondition explicit: a redundant g == NULL arm makes GCC 14
  ** partially inline this path and misdiagnose &g->gc2.cycle as address zero. */
  if (!o)
    return;
  if (o->gch.gct == ~LJ_TTAB) {
    GCtab *t = gco2tab(o);
    if (!gc2_table_rescan_later(g, t)) {
      (void)gc2_traverse_tab(g, t);
      /* Fresh lifetime admission can be transient. A failed immediate fallback
      ** must leave concrete queue work (or the fail-closed pending veto), not
      ** only an undrainable NEEDSCAN bit. */
      if (!gc2_table_scan_current(g, t))
	(void)gc2_table_rescan_later_force(g, t);
    }
    return;
  }
  if (!gc2_rescan_pending_set(o)) {
    /*
    ** Same-cycle NEEDSCAN is a traversal/deduplication token: it proves this
    ** container has already been scheduled or scanned and bounds cyclic graphs.
    ** A preserved abort can retain an uncounted bit after its old queue suffix
    ** has drained, so keep the compatibility repair only at an otherwise empty
    ** frontier. Clearing and republishing FUNC/PROTO roots while unrelated work
    ** is visible manufactures fresh work on every repeated root handshake and
    ** prevents mark closure.
    */
    if (!gc2_grey_empty(g) || !lj_gc2_ssb_empty(g))
      return;
    (void)gc2_rescan_pending_clear(o);
    if (!gc2_rescan_pending_set(o))
      return;
  }
  pushed = gc2_publish_mutator(g, o);
  if (!pushed) {
    (void)gc2_rescan_pending_clear(o);
    lj_gc2_worker_wake(g);
  }
}

static void gc2_thread_root_rescan_marked_obj_forced(global_State *g,
						      GCobj *o)
{
  if (!o)
    return;
  if (o->gch.gct == ~LJ_TFUNC)
    gc2_preserve_direct_bodies(g, o);
  switch (o->gch.gct) {
  case ~LJ_TTAB:
    /*
    ** Root containers are mutable roots. Active allocation and root preservation
    ** can leave a root container already marked before all of its child edges
    ** are published. A root hit therefore proves more than liveness of the
    ** container: it is also a fresh root snapshot that must rescan the container
    ** edges for this cycle, matching the root-TV path.
    */
    gc2_root_rescan_later(g, o);
    break;
  case ~LJ_TTHREAD:
    /*
    ** A grey traversal of an owned stack consumes its concrete queue item by
    ** replacing it with NEEDSCAN. The owner can release the lua_State before
    ** its next root acknowledgement (thread exit and coroutine yield both do
    ** this). In that state no live TG can consume the counted handoff, and an
    ** already-marked registry/root hit used to leave MARK/WEAK permanently
    ** open. Transfer an ownerless/release-sentinel token back to concrete queue
    ** work; an ownerless traversal can then take GCSCAN and clear the exact
    ** token/count once installation has completed.
    **
    ** Keep an existing NEEDSCAN deduplicated for every ordinary owner. A live
    ** mutator is the only authority for its changing stack, while stale owner
    ** ids need takeover under the traversal SMR lease and GCPREP owns terminal
    ** destruction. The per-state counted token is exact
    ** pending-count state and survives a deliberately preserved abort without
    ** relying on a cycle value or the ambiguous global aggregate. INSTALLING is
    ** also concrete work: root discovery may queue it before its pending-count
    ** publication completes, and a premature worker clear will requeue it.
    */
    if (!gc2_thread_scan_current(g, gco2th(o))) {
      uint32_t owner = lj_state_owner_acq(gco2th(o));
      if (!gc2_thread_needscan(gco2th(o)) || owner == 0 ||
	  owner == LJ_THREAD_GCSCAN)
	(void)gc2_publish_mutator(g, o);
    }
    break;
  case ~LJ_TFUNC:
  case ~LJ_TPROTO:
    gc2_root_rescan_later(g, o);
    break;
  case ~LJ_TUDATA:
    gc2_root_rescan_later(g, o);
    break;
#if LJ_HASJIT
  case ~LJ_TTRACE:
    gc2_root_rescan_later(g, o);
    break;
#endif
  case ~LJ_TUPVAL:
    /*
    ** Local-cell roots are mutable containers. If the cell body was already
    ** marked by an allocation or preservation path, a root hit still has to
    ** resample the current payload so userdata metatables, tables and closures
    ** remain live under ordinary Lua stack semantics.
    */
    gc2_root_rescan_later(g, o);
    break;
  default:
    break;
  }
}

static void gc2_thread_root_rescan_marked_obj(global_State *g, GCobj *o)
{
  uint32_t phase;
  if (!o)
    return;
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK &&
      phase != LJ_GC2_SWEEP)
    return;
  gc2_thread_root_rescan_marked_obj_forced(g, o);
}

#if defined(LJ_GC2_TEST_HELPERS)
void lj_gc2_test_thread_root_rescan_marked_obj(global_State *g, GCobj *o)
{
  gc2_thread_root_rescan_marked_obj(g, o);
}
#endif

static int gc2_mark_thread_root_obj_status(global_State *g, GCobj *o)
{
  uint32_t gct;
  int status, traversable;
  if (!o)
    return 1;
  if (mainthread_acq(g) && o == obj2gco(mainthread_acq(g))) {
    (void)gc2_publish_mutator(g, o);
    return 1;
  }
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    (void)lj_gc2_trace_sweep_root(g, o);
    /* SWEEP turns a failed nested admission into durable recovery work (or a
    ** sticky no-reclaim veto), so the stack identity is not dropped. */
    return 1;
  }
  if (gc2_root_test_take_semantic_retry(o))
    return 0;
  status = gc2_markobj_preserve_status(g, o, NULL, &gct, &traversable);
  if (status == GC2_MARK_DEAD)
    return 0;
  if (status == GC2_MARK_NEW) {
    if (gc2_gct_may_traverse(gct) &&
	(traversable || gct == (uint32_t)~LJ_TUDATA)) {
      (void)gc2_publish_mutator(g, o);
    }
    return 1;
  }
  if (status == GC2_MARK_LIVE_ALREADY)
    gc2_thread_root_rescan_marked_obj(g, o);
  return 1;
}

static void gc2_mark_thread_root_obj(global_State *g, GCobj *o)
{
  (void)gc2_mark_thread_root_obj_status(g, o);
}

static void gc2_mark_thread_root_tv(global_State *g, cTValue *tv)
{
  if (tvisgcv(tv) && gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    (void)gc2_trace_sweep_tv_edge(g, tv, 0);
    return;
  }
  if (tvisgcv(tv))
    gc2_mark_thread_root_obj(g, gcV(tv));
}

static int gc2_mark_thread_root_tv_status(global_State *g, cTValue *tv)
{
  if (tvisgcv(tv) && gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    (void)gc2_trace_sweep_tv_edge(g, tv, 0);
    return 1;
  }
  if (tvisgcv(tv))
    return gc2_mark_thread_root_obj_status(g, gcV(tv));
  return 1;
}

static int gc2_mark_upval_payload_tv_status(global_State *g, cTValue *tv)
{
  if (tvisgcv(tv) && gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    (void)gc2_trace_sweep_tv_edge(g, tv, 0);
    return 1;
  }
  if (!tvisgcv(tv))
    return 1;
  if (tvisfunc(tv))
    return lj_gc2_markobj_status(g, gcV(tv), NULL) != GC2_MARK_DEAD;
  return gc2_mark_thread_root_tv_status(g, tv);
}

static BCReg gc2_live_local_topslot(GCproto *pt, const BCIns *ip)
{
  const char *p = (const char *)proto_varinfo(pt);
  BCPos pc, lastpc = 0;
  BCReg nactive = 0;
  if (!p)
    return pt->framesize;
  pc = proto_bcpos(pt, ip);
  for (;;) {
    uint32_t vn = *(const uint8_t *)p;
    BCPos startpc, endpc;
    if (vn < VARNAME__MAX) {
      if (vn == VARNAME_END)
	break;
    } else {
      do { p++; } while (*(const uint8_t *)p);
    }
    p++;
    lastpc = startpc = lastpc + lj_buf_ruleb128(&p);
    if (startpc > pc)
      break;
    endpc = startpc + lj_buf_ruleb128(&p);
    if (pc < endpc)
      nactive++;
  }
  return nactive < pt->framesize ? nactive : pt->framesize;
}

static BCReg gc2_cur_topslot(GCproto *pt, const BCIns *pc, uint32_t nres,
			      int *precisep)
{
  BCIns ins = pc[-1];
  if (precisep)
    *precisep = 0;
  if (bc_op(ins) == BC_UCLO)
    ins = pc[bc_j(ins)];
  switch (bc_op(ins)) {
  case BC_CALLM: case BC_CALLMT:
    /*
    ** Multi-result calls depend on the VM MULTRES register, which is not always
    ** a reliable cframe source for asynchronous root snapshots. Keep the whole
    ** frame for these variable-result windows.
    */
    return pt->framesize;
  case BC_CALL:
  case BC_ITERC:
    /*
    ** Fixed-argument calls have a stable bytecode call window, but true locals
    ** can live above that window and later be captured by FNEW. Merge the call
    ** slots with debug local ranges so stale temporaries remain collectible
    ** without letting live locals go dead. Stripped prototypes fall back to the
    ** full frame because there is no precise local map.
    */
    if (bc_c(ins) != 0) {
      BCReg top = bc_a(ins) + bc_c(ins) + LJ_FR2;
      BCReg ltop = gc2_live_local_topslot(pt, pc-1);
      if (ltop > top)
	top = ltop;
      if (precisep)
	*precisep = top < pt->framesize;
      return top;
    }
    break;
  case BC_RETM:
    return bc_a(ins) + bc_d(ins) + nres-1;
  case BC_TSETM:
    /*
    ** TSETM consumes the same variable-result window; keep the frame live until
    ** the VM table-store helper publishes the payload range.
    */
    return pt->framesize;
  default:
    return pt->framesize;
  }
  return pt->framesize;
}

static GCproto *gc2_func_proto_if_lua(GCfunc *fn)
{
  return lj_func_ffid_acq(fn) == FF_LUA ?
			 (GCproto *)(mref(fn->l.pc, char) - sizeof(GCproto)) : NULL;
}

static LJ_AINLINE void gc2_frame_scope_init(GC2FrameScope *scope)
{
  if (scope) {
    gc2_mark_scope_init(&scope->fn);
    gc2_mark_scope_init(&scope->pt);
    gc2_mark_scope_init(&scope->pc);
  }
}

static void gc2_frame_scope_leave(GC2FrameScope *scope)
{
  if (scope) {
    gc2_mark_scope_leave(&scope->pc);
    gc2_mark_scope_leave(&scope->pt);
    gc2_mark_scope_leave(&scope->fn);
  }
}

static int gc2_frame_func_valid(global_State *g, TValue *frame,
				GCfunc **fnp, GCproto **ptp,
				GC2FrameScope *scope)
{
  cTValue *ftv = frame - 1;
  GCobj *fo;
  GCfunc *fn;
  uint32_t gct;
  if (fnp) *fnp = NULL;
  if (ptp) *ptp = NULL;
  if (!scope)
    return 0;
  gc2_frame_scope_init(scope);
  if (!tvisfunc(ftv))
    return 0;
  fo = frame_gc(frame);
  if (!gc2_observed_obj_valid_scoped(g, fo, &gct, &scope->fn) ||
      gct != (uint32_t)~LJ_TFUNC)
    goto fail;
  fn = &fo->fn;
  if (isluafunc(fn)) {
    const char *pc = mref(fn->l.pc, const char);
    GCproto *pt;
    uint32_t ptgct;
    if (!pc || !checkptrGC(pc) || lj_funcL_nupvalues(&fn->l) > LJ_MAX_UPVAL)
	goto fail;
    pt = (GCproto *)(void *)(pc - sizeof(GCproto));
    if (!gc2_observed_obj_valid_scoped(g, obj2gco(pt), &ptgct,
					   &scope->pt) ||
	ptgct != (uint32_t)~LJ_TPROTO ||
	!gc2_valid_proto_for_traverse_held(pt) ||
	lj_funcL_nupvalues(&fn->l) > pt->sizeuv)
      goto fail;
    if (ptp) *ptp = pt;
  }
  if (fnp) *fnp = fn;
  return 1;
fail:
  gc2_frame_scope_leave(scope);
  return 0;
}

static int gc2_frame_prev_safe(global_State *g, TValue *bot, TValue *max,
			       TValue *frame, TValue **prevp, GCfunc **fnp,
			       GC2FrameScope *scope)
{
  GCfunc *fn = NULL;
  GCproto *pt = NULL;
  TValue *prev;
  if (prevp) *prevp = NULL;
  if (fnp) *fnp = NULL;
  if (!scope)
    return 0;
  gc2_frame_scope_init(scope);
  if (frame <= bot + LJ_FR2 || frame >= max)
    return 0;
  if (!gc2_frame_func_valid(g, frame, &fn, &pt, scope))
    goto fail;
  if (frame_islua(frame)) {
    const BCIns *pc;
    pc = frame_pc(frame);
    if (!pc || !gc2_frame_pc_valid_scoped(g, pc, &scope->pc))
	goto fail;
    /* frame_pc belongs to the caller, not necessarily to the callee function
    ** in frame-1. This is particularly visible for Lua-to-C/fast calls, whose
    ** Lua-style frame has no callee prototype. Resolve and retain the caller
    ** prototype through the return PC before decoding pc[-1]. The observation
    ** lease deliberately does not change MARK: caller-function traversal owns
    ** the semantic proto edge, while owner authority keeps the frame word
    ** stable through this body read. */
    prev = frame - (1 + LJ_FR2 + bc_a(pc[-1]));
  } else {
    ptrdiff_t sz = frame_sized(frame);
    if (sz <= 0 || (uintptr_t)sz > (uintptr_t)((char *)frame -
						(char *)(bot + LJ_FR2)))
	goto fail;
    prev = (TValue *)((char *)frame - sz);
  }
  if (prevp) *prevp = prev;
  if (fnp) *fnp = fn;
  return 1;
fail:
  gc2_frame_scope_leave(scope);
  return 0;
}

#if LJ_HASJIT
static TValue *gc2_thread_jit_base(global_State *g, lua_State *L)
{
  TGState *tg = lj_tg_thread_active(g, L);
  return tg ? lj_tg_load_jit_base(tg) : NULL;
}
#endif

#if LJ_HASJIT
static void gc2_mark_jit_frame_funcs(global_State *g, lua_State *L)
{
  TValue *base = gc2_thread_jit_base(g, L);
  TValue *bot, *max, *frame;
  uint32_t n = 0;
  if (!base || !L || tvref(L->stack) == NULL)
    return;
  bot = tvref(L->stack);
  max = tvref(L->maxstack);
  if (base <= bot + 1 + LJ_FR2 || base > max)
    return;
  /*
  ** JIT C helpers keep jit_base published while vmstate is C, so there is no
  ** positive trace-number root for the scanner to follow. The value slots are
  ** still conservatively scanned below, but frame headers are not ordinary
  ** tagged values. Preserve the Lua function/prototype chain explicitly so
  ** caller constants remain live across full collections entered from traces.
  */
  for (frame = base - 1; frame > bot + LJ_FR2 && frame < max; ) {
    GCfunc *fn;
    TValue *prev;
    GC2FrameScope scope;
    if (!gc2_frame_prev_safe(g, bot, max, frame, &prev, &fn, &scope))
      break;
    if (fn)
      gc2_mark_thread_root_obj(g, obj2gco(fn));
    gc2_frame_scope_leave(&scope);
    if (prev >= frame || prev <= bot + LJ_FR2 || prev >= max)
      break;
    frame = prev;
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT)
      break;
  }
}
#endif

static int gc2_thread_is_current(global_State *g, lua_State *L)
{
  uint32_t owner;
  TGState *tg;
  if (!g || !L)
    return 0;
  owner = lj_state_owner_acq(L);
  if (lj_thr_id_is_owner(owner)) {
    tg = lj_tg_find_owner(g, owner);
    if (tg && !lj_tg_flags_test_acq(tg, TGF_DEAD) &&
	lj_tg_load_cur_L(tg) == L)
      return 1;
  }
  return L == lj_tg_cur_L(g);
}

static int gc2_thread_is_jit_current(global_State *g, lua_State *L)
{
#if LJ_HASJIT
  /*
  ** A trace/native helper owns the live frame layout while jit_base or a
  ** positive trace vmstate is published. In that window L->base is not a stable
  ** interpreter frame chain, so root scans preserve the whole stack storage.
  ** When jit_base is the only published edge, gc2_mark_jit_frame_funcs() marks
  ** frame-header function roots before the raw slot scan.
  */
  {
    TGState *tg = lj_tg_thread_active(g, L);
    return tg && lj_tg_jit_active_acq(tg);
  }
#else
  UNUSED(g); UNUSED(L);
  return 0;
#endif
}

static void gc2_mark_frame_chain_funcs(global_State *g, lua_State *L)
{
  TValue *bot, *max, *frame;
  uint32_t n = 0;
  if (!L || tvref(L->stack) == NULL)
    return;
  bot = tvref(L->stack);
  max = tvref(L->maxstack);
  frame = L->base - 1;
  /*
  ** Raw stack scans preserve slot storage while a remote/native/JIT owner still
  ** controls the frame layout, but frame headers are not guaranteed to decode as
  ** normal TValue roots. Walk the bounded frame chain and validate each function
  ** before marking its payload graph.
  */
  while (frame > bot + LJ_FR2 && frame < max) {
    GCfunc *fn;
    TValue *prev;
    GC2FrameScope scope;
    if (!gc2_frame_prev_safe(g, bot, max, frame, &prev, &fn, &scope))
      break;
    if (fn)
      gc2_mark_thread_root_obj(g, obj2gco(fn));
    gc2_frame_scope_leave(&scope);
    if (prev >= frame || prev <= bot + LJ_FR2 || prev >= max)
      break;
    frame = prev;
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT)
      break;
  }
}

static int gc2_thread_is_native_current(global_State *g, lua_State *L)
{
  TGState *tg = lj_tg_thread_active(g, L);
  /*
  ** A native section may be scanned from a safepoint before control has
  ** returned through the C frame. Scan the complete stack storage in that
  ** window instead of interpreting frame headers as a stable Lua frame chain.
  */
  return tg && lj_tg_in_native_acq(tg) != 0;
}

static int gc2_thread_is_remote_current(global_State *g, lua_State *L)
{
  uint32_t owner;
  TGState *tg;
  if (!g || !L)
    return 0;
  owner = lj_state_owner_acq(L);
  if (!lj_thr_id_is_owner(owner))
    return 0;
  tg = lj_tg_find_owner(g, owner);
  return tg && !lj_tg_flags_test_acq(tg, TGF_DEAD) &&
	 tg != lj_thr_get_tg() &&
	 lj_tg_load_cur_L(tg) == L && L != lj_tg_cur_L(g);
}

static int gc2_thread_stack_scan_authoritative(global_State *g, lua_State *L)
{
  /*
  ** Remote/JIT current stacks can be sampled as bounded raw slots, but their
  ** owner-private frame chain is not closed by that sample. Keep NEEDSCAN as the
  ** explicit promise that the owning TG will publish an authoritative scan before
  ** GC2 reaches a root-closed state. Native-parked TGs are handled separately:
  ** once at a safepoint they are not mutating the Lua stack.
  */
  return !gc2_thread_is_remote_current(g, L) &&
	 !gc2_thread_is_native_current(g, L) &&
	 !gc2_thread_is_jit_current(g, L);
}

static TValue *gc2_active_thread_top(global_State *g, lua_State *L, TValue *top,
				     int *precisep)
{
  TValue *bot = tvref(L->stack);
  TValue *max = tvref(L->maxstack);
  TValue *frame;
  if (precisep)
    *precisep = 0;
  if (top > max)
    top = max;
  if (L->base <= bot + 1 + LJ_FR2)
    return top;
  frame = L->base - 1;
  if (frame > bot + LJ_FR2 && frame < max && frame_islua(frame)) {
    GCfunc *fn;
    GCproto *pt;
    GC2FrameScope scope;
    (void)gc2_frame_func_valid(g, frame, &fn, &pt, &scope);
    if (pt) {
      TValue *ltop = L->base + pt->framesize;
      if (ltop > top)
	top = ltop;
    }
    gc2_frame_scope_leave(&scope);
  } else if (frame > bot + LJ_FR2 && frame < max && frame_isc(frame)) {
    TValue *prev;
    GCfunc *fn;
    GC2FrameScope cscope;
    if (gc2_frame_prev_safe(g, bot, max, frame, &prev, &fn, &cscope)) {
      gc2_frame_scope_leave(&cscope);
      if (prev > bot + LJ_FR2 && prev < max && frame_islua(prev)) {
	GCproto *pt;
	GC2FrameScope lscope;
	(void)gc2_frame_func_valid(g, prev, &fn, &pt, &lscope);
	if (pt) {
	  void *cf = cframe_raw(L->cframe);
		const BCIns *pc = cf ? cframe_pc(cf) : NULL;
		const BCIns *bc = proto_bc(pt);
		if (pc && pc > bc && pc <= bc + pt->sizebc) {
		  int precise = 0;
		  /*
		  ** The bytecode call window is the precise same-thread C-call root set.
		  ** Open local cells are scanned through the thread's open-upvalue list;
		  ** broadening every cell-op frame here keeps stale temporaries alive and
		  ** changes stock weak/trace collection semantics. Remote/native scanners
		  ** still use this bounded active top instead of maxstack.
		  */
		  TValue *ctop = prev + 1 + gc2_cur_topslot(pt, pc,
							    cframe_multres_n(cf),
							    &precise);
		  if (precisep)
		    *precisep = precise;
		  if (ctop > top)
		    top = ctop;
		}
	  }
	  gc2_frame_scope_leave(&lscope);
      }
    }
  }
  return top > max ? max : top;
}

static TValue *gc2_stack_scan_top(global_State *g, lua_State *L,
				  int *conservativep)
{
  TValue *frame, *bot = tvref(L->stack);
  TValue *top = L->top, *used, *max = tvref(L->maxstack);
  uint32_t n = 0;
  int vm_current = gc2_thread_is_current(g, L);
  int remote_current = gc2_thread_is_remote_current(g, L);
  int native_current = gc2_thread_is_native_current(g, L);
  int jit_current = gc2_thread_is_jit_current(g, L);
  if (conservativep)
    *conservativep = 0;
  if (remote_current || native_current || jit_current) {
    gc2_mark_frame_chain_funcs(g, L);
#if LJ_HASJIT
    if (jit_current)
      gc2_mark_jit_frame_funcs(g, L);
#endif
    /*
    ** Remote, native and JIT-owned frame chains can have owner-private layout
    ** until their boundary publishes a stable base/top pair. Preserve raw stack
    ** storage in that window; ordinary same-thread VM collections use precise
    ** frame tops so dead call results do not affect weak/FINREG decisions.
    */
    if (conservativep)
      *conservativep = 1;
    return max;
  }
  used = L->top - 1;
  for (frame = L->base - 1; frame > bot + LJ_FR2; ) {
    GCfunc *fn;
    GCproto *pt;
    TValue *prev;
    GC2FrameScope scope;
    TValue *ftop = frame;
    if (!gc2_frame_prev_safe(g, bot, max, frame, &prev, &fn, &scope)) {
      /* A same-thread C transition may expose a temporary frame header. */
      gc2_thread_scan_frame_fallbacks_add(g, 1);
      if (conservativep)
	*conservativep = 1;
      return max;
    }
    pt = fn ? gc2_func_proto_if_lua(fn) : NULL;
    if (pt)
      ftop += pt->framesize;
    if (ftop > used)
      used = ftop;
    /*
    ** A live frame function is a root for its prototype and upvalue graph.
    ** Allocation/SMR marking can make the closure body non-white before this
    ** GC2 cycle has traversed its payload, so use the thread-root path rather
    ** than a body-only mark.
    */
    if (fn)
      gc2_mark_thread_root_obj(g, obj2gco(fn));
    gc2_frame_scope_leave(&scope);
    if (prev >= frame || prev <= bot + LJ_FR2 || prev >= max)
      break;
    frame = prev;
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT) {
      gc2_thread_scan_frame_fallbacks_add(g, 1);
      if (conservativep)
	*conservativep = 1;
      return max;
    }
  }
  used++;
  if (used > max)
    used = max;
  if (vm_current && L->base > bot + 1 + LJ_FR2) {
    top = gc2_active_thread_top(g, L, top, NULL);
    /*
    ** The active C-call PC gives the current call window. Frame functions were
    ** already marked above, and open local cells are scanned through the open-upval
    ** list; widening back to the declared frame size revives dead lexical slots.
    */
  } else {
    TValue *ctop = curr_top(L);
    if (ctop > top)
      top = ctop;
    if (used > top)
      top = used;
  }
  return top > max ? max : top;
}

static LJ_AINLINE uint8_t *gc2_thread_flagp(lua_State *L)
{
  return lj_obj_gcflags_ref(obj2gco(L));
}

#define GC2_THREAD_NEEDSCAN_NONE	0u
#define GC2_THREAD_NEEDSCAN_COUNTED	1u
#define GC2_THREAD_NEEDSCAN_INSTALLING	2u

static void gc2_thread_set_needscan(global_State *g, lua_State *L)
{
  uint32_t expect = GC2_THREAD_NEEDSCAN_NONE;
  int installed = lj_state_scan_needscan_counted_cas(
    L, &expect, GC2_THREAD_NEEDSCAN_INSTALLING);
  /* The per-state token is authoritative. Publish it before the header hint so
  ** owner release cannot observe a hint-only pre-token window. */
  (void)la_or8_rlx(gc2_thread_flagp(L), LJ_GC_NEEDSCAN);
  if (installed) {
    /* INSTALLING is concrete work even though it has not joined the aggregate
    ** yet. Release and root paths may conservatively queue the state while this
    ** publisher finishes; a premature clear returns false and requeues it. */
    gc2_thread_needscan_test_pause_at(
      LJ_GC2_THREAD_NEEDSCAN_TEST_INSTALLING);
    lj_state_scan_handoff_epoch_rel(L, gc2_thread_scan_cycle_acq(g));
    gc2_thread_scan_needscan_add(g, 1);
    gc2_thread_scan_requeues_add(g, 1);
    gc2_thread_scan_needscan_pending_inc(g);
    lj_state_scan_needscan_counted_rel(
      L, GC2_THREAD_NEEDSCAN_COUNTED);
  }
  /* Race closure with tid->GCSCAN->0 release. The same-value owner CAS is an
  ** RMW ordering edge after COUNTED publication. If it wins, a later release
  ** CAS acquires that edge before inspecting this token. If it loses, release
  ** has already reached its sentinel (or the state is ownerless), so retain the
  ** exact identity on the MPMC recovery plane without touching a foreign SSB or
  ** the single-owner grey deque. Duplicate publication is harmless. */
  {
    uint32_t owner = lj_state_owner_acq(L);
    uint32_t owner_expect = owner;
    if (!lj_thr_id_is_owner(owner) ||
	!lj_state_owner_cas(L, &owner_expect, owner)) {
      if (!gc2_recovery_publish(g, obj2gco(L)))
	gc2_recovery_fail_closed(g);
      lj_gc2_worker_wake(g);
    }
  }
}

static int gc2_thread_clear_needscan(global_State *g, lua_State *L)
{
  uint32_t state = lj_state_scan_needscan_counted_acq(L);
  if (state == GC2_THREAD_NEEDSCAN_INSTALLING)
    return 0;
  if (state == GC2_THREAD_NEEDSCAN_COUNTED) {
    uint32_t expect = GC2_THREAD_NEEDSCAN_COUNTED;
    if (!lj_state_scan_needscan_counted_cas(
	  L, &expect, GC2_THREAD_NEEDSCAN_NONE))
      return 0;
    lj_assertG(g != NULL, "counted thread NEEDSCAN clear without global state");
    gc2_thread_scan_needscan_pending_dec(g);
  }
  (void)la_and8_rlx(gc2_thread_flagp(L), (uint8_t)~LJ_GC_NEEDSCAN);
  /* A racing new installer owns the authoritative token. Restore the hint if
  ** this clear overlapped its header-bit publication. */
  if (lj_state_scan_needscan_counted_acq(L) !=
      GC2_THREAD_NEEDSCAN_NONE)
    (void)la_or8_rlx(gc2_thread_flagp(L), LJ_GC_NEEDSCAN);
  return 1;
}

#if defined(LJ_GC2_TEST_HELPERS)
int lj_gc2_test_thread_needscan_clear(global_State *g, lua_State *L)
{
  return gc2_thread_clear_needscan(g, L);
}
#endif

static uint32_t gc2_clear_container_needscan_chain(global_State *g, GCobj *head)
{
  GCobj *o = head;
  GC2MarkScope scope;
  uint32_t cleared = 0, n = 0;
  if (o && !gc2_root_spine_admit(g, o, &scope))
    return 0;
  while (o != NULL) {
    GC2MarkScope nextscope;
    GCobj *next;
    /*
    ** Non-table NEEDSCAN is same-cycle queue membership for already-marked
    ** container payload rescans. Keep it during MARK/WEAK to deduplicate cyclic
    ** proto/function/upvalue graphs, then clear it at IDLE so the next cycle can
    ** publish a fresh rescan if a root reaches the same container.
    */
    if (o->gch.gct != ~LJ_TTAB && gc2_obj_may_traverse(o) &&
	(gc2_rescan_pending_clear(o) & LJ_GC_NEEDSCAN))
      cleared++;
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT) {
      gc2_mark_scope_leave(&scope);
      break;
    }
    if (!gc2_root_spine_handoff(g, o, &scope, &next, &nextscope))
      break;
    if (next == o) {
      gc2_mark_scope_leave(&nextscope);
      break;
    }
    o = next;
    scope = nextscope;
  }
  return cleared;
}

static int gc2_uncounted_needscan_type(uint32_t gct)
{
  switch (gct) {
  case ~LJ_TFUNC:
  case ~LJ_TPROTO:
  case ~LJ_TUPVAL:
  case ~LJ_TUDATA:
#if LJ_HASJIT
  case ~LJ_TTRACE:
#endif
    return 1;
  default:
    return 0;
  }
}

static uint32_t gc2_clear_uncounted_needscan_chain(global_State *g,
						    GCobj *head)
{
  GCobj *o = head;
  GCobj *maino, *vmo;
  lua_State *mainL, *vmL;
  GCArena *known_small = NULL;
  uint32_t cleared = 0, n = 0;
  if (!o || !lj_gc2_smr_read_try(g))
    return 0;
  mainL = mainthread_acq(g);
  vmL = vmthread_acq(g);
  maino = mainL ? obj2gco(mainL) : NULL;
  vmo = vmL ? obj2gco(vmL) : NULL;
  while (o != NULL) {
    LJGC2QueuedInfo info;
    GCobj *next;
    uint32_t gct;
    /* The intrusive root membership is already the lifetime lease. Keep one
    ** bounded registry reader across the whole walk and validate each exact
    ** allocation non-semantically, instead of opening and closing a rescue
    ** admission for every root. The permanent threads are GG_State-owned and
    ** retain their state-lifetime exemption. */
    if (o == maino || o == vmo) {
      gct = (uint32_t)~LJ_TTHREAD;
      known_small = NULL;
    } else {
      if (LJ_UNLIKELY(!gc2_queue_obj_info(g, o, known_small, &info, 0) ||
		      info.gct == (uint32_t)~LJ_TSTR))
	break;
      gct = info.gct;
      known_small = info.start != 0 ? (GCArena *)info.arena : NULL;
    }
    next = lj_obj_gcw_acq(o);
    if (gc2_uncounted_needscan_type(gct) &&
	(gc2_rescan_pending_clear(o) & LJ_GC_NEEDSCAN))
      cleared++;
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT)
      break;
    if (next == o)
      break;
    o = next;
  }
  lj_gc2_smr_read_leave(g);
  return cleared;
}

static uint32_t gc2_clear_uncounted_needscan_all(global_State *g)
{
  lua_State *mainL;
  uint32_t cleared = 0;
  if (!g)
    return 0;
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  cleared += gc2_clear_uncounted_needscan_chain(g, lj_gc_root_acq(g));
  mainL = mainthread_acq(g);
  if (mainL)
    cleared += gc2_clear_uncounted_needscan_chain(
		g, lj_obj_gcw_acq(obj2gco(mainL)));
  return cleared;
}

static uint32_t gc2_clear_container_needscan_all(global_State *g)
{
  lua_State *mainL;
  uint32_t cleared = 0;
  if (!g)
    return 0;
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  cleared += gc2_clear_container_needscan_chain(g, lj_gc_root_acq(g));
  mainL = mainthread_acq(g);
  if (mainL)
    cleared += gc2_clear_container_needscan_chain(g,
						 lj_obj_gcw_acq(obj2gco(mainL)));
  return cleared;
}

static int gc2_thread_needscan(lua_State *L)
{
  return lj_state_scan_needscan_counted_acq(L) !=
    GC2_THREAD_NEEDSCAN_NONE;
}

static uint64_t gc2_thread_owner_dirty(global_State *g, lua_State *L,
				       TGState **ptg)
{
  uint32_t owner;
  TGState *tg;
  if (ptg)
    *ptg = NULL;
  if (!g || !L)
    return 0;
  owner = lj_state_owner_acq(L);
  if (!lj_thr_id_is_owner(owner))
    return 0;
  tg = lj_tg_find_owner(g, owner);
  if (!tg || lj_tg_flags_test_acq(tg, TGF_DEAD))
    return 0;
  if (ptg)
    *ptg = tg;
  return lj_tg_stack_dirty_epoch_acq(tg);
}

static int gc2_thread_scan_current(global_State *g, lua_State *L)
{
  uint64_t owner_dirty;
  if (!g || !L || gc2_thread_needscan(L))
    return 0;
  if (lj_state_scan_epoch_acq(L) != gc2_thread_scan_cycle_acq(g))
    return 0;
  owner_dirty = gc2_thread_owner_dirty(g, L, NULL);
  return lj_state_scan_dirty_epoch_acq(L) == owner_dirty;
}

static int gc2_scan_thread_stack(global_State *g, lua_State *L,
				 const GC2MarkScope *held)
{
  GCobj *mt, *uv;
  TValue *o, *top;
  TValue tv;
  uint64_t dirty_epoch, cycle;
  int conservative = 0;
  int stack_retry = 0;
  if (!gc2_valid_thread_for_traverse_held(g, L, held))
    return 0;
  cycle = gc2_thread_scan_cycle_acq(g);
  /* This owner snapshot traverses the complete thread payload synchronously.
  ** Mark the identity without queueing a duplicate gc2_traverse_thread pass. */
  (void)lj_gc2_markobj_nogrey(g, obj2gco(L));
  lj_gc2_markmem(g, tvref(L->stack));
  top = gc2_stack_scan_top(g, L, &conservative);
  for (o = tvref(L->stack) + 1 + LJ_FR2; o < top; o++) {
    lj_tv_load_acq(&tv, o);
    if (conservative) {
      GC2MarkScope scope;
      int admitted = gc2_tv_admit_scoped(g, &tv, &scope);
      if (LJ_UNLIKELY(admitted == GC2_TV_SCOPE_RETRY)) {
	stack_retry = 1;
	gc2_mark_scope_leave(&scope);
	continue;
      }
      if (admitted == GC2_TV_SCOPE_STALE) {
	gc2_mark_scope_leave(&scope);
	continue;
      }
      /* The observation lease remains live across the nested semantic
      ** admission. A nested DEAD result is therefore transient, not stale. */
      if (LJ_UNLIKELY(!gc2_mark_thread_root_tv_status(g, &tv)))
	stack_retry = 1;
      gc2_mark_scope_leave(&scope);
    } else {
      /* Authoritative slots need no stale-word classifier, but a transient
      ** semantic admission still prevents this snapshot from completing. */
      if (LJ_UNLIKELY(!gc2_mark_thread_root_tv_status(g, &tv)))
	stack_retry = 1;
    }
  }
  if (L == lj_tg_cur_L(g) && L->cframe != NULL) {
#if LJ_HASJIT
    gc2_mark_active_cframe_proto_root(g, L);
#endif
  }
  {
    GCtab *env = lj_state_env_acq(L);
    if (env && LJ_UNLIKELY(
		 !gc2_mark_thread_root_obj_status(g, obj2gco(env))))
      stack_retry = 1;
  }
  mt = lj_state_mt_thread_acq(L);
  if (mt != NULL && LJ_UNLIKELY(!gc2_mark_thread_root_obj_status(g, mt)))
    stack_retry = 1;
  for (uv = lj_state_openupval_acq(L); uv != NULL;
       uv = lj_obj_gcw_acq(uv)) {
    int uv_admitted = gc2_mark_thread_root_obj_status(g, uv);
    if (LJ_UNLIKELY(!uv_admitted)) {
      stack_retry = 1;
    } else if (uv->gch.gct == ~LJ_TUPVAL) {
      TValue tv;
      lj_tv_load_acq(&tv, uvval(gco2uv(uv)));
      if (LJ_UNLIKELY(!gc2_mark_upval_payload_tv_status(g, &tv)))
	stack_retry = 1;
    }
  }
  if (LJ_UNLIKELY(stack_retry)) {
    /* Partial marking is conservative, but it is not a completed snapshot.
    ** Publish concrete owner work without changing the scan/dirty completion
    ** stamps. A previously absent handoff stamp is part of that publication. */
    gc2_thread_set_needscan(g, L);
    return 0;
  }
  /*
  ** A native-parked TG is not mutating the Lua stack; after the bounded safe
  ** active-top scan above, the remote safepoint can publish a fresh stack stamp.
  ** Running remote/JIT-owned stacks still need an owner NEEDSCAN handoff.
  */
  if (!gc2_thread_stack_scan_authoritative(g, L) &&
      !gc2_thread_is_native_current(g, L)) {
    if (!gc2_thread_scan_current(g, L) && gc2_thread_has_live_owner(g, L))
      gc2_thread_set_needscan(g, L);
    return 0;
  }
  dirty_epoch = gc2_thread_owner_dirty(g, L, NULL);
  lj_state_scan_dirty_epoch_rel(L, dirty_epoch);
  lj_state_scan_epoch_rel(L, cycle);
  /* A direct owner-root acknowledgement closes the same grey-thread work item
  ** as an explicit NEEDSCAN handoff. Publishing the cycle here lets later
  ** identity traversal consume the owner snapshot without scanning it twice. */
  lj_state_scan_handoff_epoch_rel(L, cycle);
  return gc2_thread_clear_needscan(g, L);
}

static void gc2_scan_owned_needscan_chain(global_State *g, uint32_t tid,
					  GCobj *head)
{
  GCobj *o = head;
  GC2MarkScope scope;
  uint32_t n = 0;
  if (o && !gc2_root_spine_admit(g, o, &scope))
    return;
  while (o != NULL) {
    GC2MarkScope nextscope;
    GCobj *next;
    lua_State *th;
    if (o->gch.gct != ~LJ_TTHREAD)
      goto next_root;
    th = gco2th(o);
    if (!gc2_thread_needscan(th))
      goto next_root;
    if (lj_state_owner_acq(th) != tid)
      goto next_root;
    (void)gc2_scan_thread_stack(g, th, &scope);
    gc2_thread_scan_owner_needscans_add(g, 1);
next_root:
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT) {
      gc2_mark_scope_leave(&scope);
      break;
    }
    if (!gc2_root_spine_handoff(g, o, &scope, &next, &nextscope))
      break;
    if (next == o) {
      gc2_mark_scope_leave(&nextscope);
      break;
    }
    o = next;
    scope = nextscope;
  }
}

static void gc2_scan_owned_needscan_registry(global_State *g, uint32_t tid)
{
  lua_State *th = lj_state_thread_registry_head_acq(g);
  GC2MarkScope scope;
  uint32_t n = 0;
  int status;
  if (!th)
    return;
  status = gc2_admit_thread_identity(g, th, &scope);
  if (status == GC2_MARK_DEAD) {
    gc2_mark_scope_leave(&scope);
    return;
  }
  while (th != NULL && n++ < LJ_GC2_ROOT_SCAN_LIMIT) {
    GC2MarkScope nextscope;
    lua_State *next;
    if (!lj_state_thread_registry_valid(g, th)) {
      gc2_mark_scope_leave(&scope);
      break;
    }
    next = lj_state_thread_registry_next_acq(th);
    if (gc2_thread_needscan(th) && lj_state_owner_acq(th) == tid) {
      (void)gc2_scan_thread_stack(g, th, &scope);
      gc2_thread_scan_owner_needscans_add(g, 1);
    }
    if (next) {
      status = gc2_admit_thread_identity(g, next, &nextscope);
      if (status == GC2_MARK_DEAD) {
	gc2_mark_scope_leave(&scope);
	break;
      }
    } else {
      gc2_mark_scope_init(&nextscope);
    }
    gc2_mark_scope_leave(&scope);
    th = next;
    scope = nextscope;
  }
  gc2_mark_scope_leave(&scope);
}

static void gc2_scan_owned_needscan(global_State *g, TGState *tg)
{
  uint32_t tid;
  if (!tg || lj_tg_flags_test_acq(tg, TGF_DEAD))
    return;
  tid = lj_tg_tid_acq(tg);
  if (!lj_thr_id_is_owner(tid))
    return;
  if (gc2_thread_scan_needscan_pending_acq(g) == 0)
    return;
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  gc2_scan_owned_needscan_chain(g, tid, lj_gc_root_acq(g));
  if (gc2_thread_scan_needscan_pending_acq(g) != 0) {
    lua_State *mainL = mainthread_acq(g);
    if (mainL)
      gc2_scan_owned_needscan_chain(g, tid,
				    lj_obj_gcw_acq(obj2gco(mainL)));
  }
  if (gc2_thread_scan_needscan_pending_acq(g) != 0)
    gc2_scan_owned_needscan_registry(g, tid);
}

#if LJ_HASFFI
static void gc2_finreg_markobj(global_State *g, GCobj *o)
{
  /* Root snapshots are producer-side operations. Publish semantic work through
  ** the leader TG's SSB; never mutate the single-owner grey deque from a
  ** safepoint leader while a parked worker may own it. */
  gc2_mark_thread_root_obj(g, o);
}

static void gc2_finreg_markmem(global_State *g, void *p)
{
  (void)lj_gc2_markmem(g, p);
}

static void gc2_finreg_marktv(global_State *g, cTValue *tv)
{
  gc2_mark_thread_root_tv(g, tv);
}

static void gc2_mark_finreg_cdata_preclaims(global_State *g,
					    GC2FinRegMarkObjFunc markobj,
					    GC2FinRegMarkTVFunc marktv)
{
  GCRef *objv;
  TValue *finv;
  GC2MarkScope objscope, finscope;
  MSize i, head, count, cap;
  int objstatus, finstatus;
  if (!g || !markobj || !marktv)
    return;
  objv = gc2_finreg_cdata_preclaim_objvec_acq(g);
  finv = gc2_finreg_cdata_preclaim_finvec_acq(g);
  cap = gc2_finreg_cdata_preclaim_capacity_acq(g);
  head = gc2_finreg_cdata_preclaim_head_acq(g);
  count = gc2_finreg_cdata_preclaim_count_acq(g);
  if (!objv || !finv) {
    /* A completely absent zero-capacity side vector has no roots. Any partial
    ** publication is not a certificate that the claimed prefix was empty. */
    if (objv || finv || cap != 0 || head != 0 || count != 0)
      gc2_root_scan_retry(g);
    return;
  }
  objstatus = gc2_markmem_registered_scoped_status(g, objv, &objscope);
  finstatus = gc2_markmem_registered_scoped_status(g, finv, &finscope);
  if (objstatus == GC2_MARK_DEAD || finstatus == GC2_MARK_DEAD) {
    gc2_root_scan_retry(g);
    goto out;
  }
  /* Both fixed vectors are now retained. Snapshot bounds only afterwards and
  ** use the admitted local pointers throughout the entry walk. */
  cap = gc2_finreg_cdata_preclaim_capacity_acq(g);
  head = gc2_finreg_cdata_preclaim_head_acq(g);
  count = gc2_finreg_cdata_preclaim_count_acq(g);
  if (head > count || count > cap) {
    gc2_root_scan_retry(g);
    goto out;
  }
  for (i = head; i < count; i++) {
    GCobj *o = gcref_acq(objv[i]);
    if (o) {
      TValue fin;
      markobj(g, o);
      lj_tv_load_acq(&fin, &finv[i]);
      marktv(g, &fin);
    }
  }
out:
  if (finstatus != GC2_MARK_DEAD)
    gc2_mark_scope_leave(&finscope);
  if (objstatus != GC2_MARK_DEAD)
    gc2_mark_scope_leave(&objscope);
}

static int gc2_mark_finreg_cdata_generations(global_State *g,
					     GC2FinRegMarkObjFunc markobj,
					     GC2FinRegMarkMemFunc markmem)
{
  if (!g || !markobj || !markmem)
    return 1;
  if (lj_ctype_fin_mark(g, markobj, markmem))
    return 1;
  /* Root snapshot state 2 is owned by the current handshake. Clearing it is a
  ** durable retry request: the owner's final 2->1 CAS fails, while an already
  ** completed state 1 is explicitly reopened. marks_this_round prevents a
  ** same-pass fixpoint even for direct/non-snapshot root scans. */
  gc2_root_scan_retry(g);
  return 0;
}

void lj_gc2_finreg_cdata_mark_roots(global_State *g,
				    GC2FinRegMarkObjFunc markobj,
				    GC2FinRegMarkMemFunc markmem,
				    GC2FinRegMarkTVFunc marktv)
{
  if (!g || !markobj || !markmem || !marktv)
    return;
  (void)gc2_mark_finreg_cdata_generations(g, markobj, markmem);
  gc2_mark_finreg_cdata_preclaims(g, markobj, marktv);
}
#endif

static void gc2_mark_finalizer_obj(global_State *g, GCobj *o)
{
  gc2_mark_thread_root_obj(g, o);
}

static void gc2_finalizer_mark_queued_owned(global_State *g,
					    GC2FinalizerMarkFunc mark)
{
  GC2FinalizerNode *tail, *node;
  if (!g || !mark)
    return;
  lj_assertG(lj_gc2_finalizer_owned_by_current(g),
	     "gc2 finalizer mark requires owner");
  tail = (GC2FinalizerNode *)gc2_finalizer_tail_acq(g);
  if (!tail)
    return;
  node = tail;
  do {
    node = gc2_finalizer_node_next_acq(node);
    {
      GCobj *o = gc2_finalizer_node_obj_acq(node);
      TValue fin;
      if (o)
	mark(g, o);
      if (gc2_finalizer_node_fin_acq(node, &fin))
	gc2_mark_thread_root_tv_worker(g, &fin);
    }
  } while (node != tail);
}

void lj_gc2_finalizer_mark_all(global_State *g, GC2FinalizerMarkFunc mark)
{
  if (!g || !mark)
    return;
  /* A root snapshot must not wait behind an arbitrary user __gc/FFI
  ** callback owned by another TG. Keep the completed/provisional phase
  ** certificate open and let the worker retry after that owner leaves.
  ** Same-owner recursive scans still enter through try_enter's nesting path. */
  if (!lj_gc2_finalizer_try_enter(g)) {
    gc2_root_scan_retry(g);
    return;
  }
  lj_gc2_finalizer_drain_owned(g);
  gc2_finalizer_mark_queued_owned(g, mark);
  lj_gc2_finalizer_leave(g);
}

static void gc2_scan_threading_live_roots(global_State *g)
{
  LJThreadLive *node, *next;
  GC2RootCycleGuard guard;
  node = lj_thread_live_head_acq(g);
  gc2_root_cycle_guard_init(&guard, node);
  while (node != NULL) {
    GC2MarkScope scope;
    GCobj *o;
    int status = gc2_markmem_registered_scoped_status(g, node, &scope);
    if (status == GC2_MARK_DEAD) {
      gc2_mark_scope_leave(&scope);
      gc2_root_scan_retry(g);
      break;
    }
    o = lj_thread_live_udata_ref_acq(node);
    next = lj_thread_live_next_acq(node);
    /* The node is the root publication. Removal may concurrently clear it and
    ** a terminal arena generation may recycle the old userdata body, so this
    ** scanner must not inspect LJThread payload fields before lifetime
    ** admission. Mark/queue the raw reference; worker-owned userdata traversal
    ** performs the admitted payload scan. */
    if (o)
      gc2_mark_thread_root_obj(g, o);
    gc2_mark_scope_leave(&scope);
    node = next;
    if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, node))) {
      gc2_root_scan_retry(g);
      break;
    }
  }

  /* Tombstones are retained until terminal shutdown so an in-flight scanner
  ** may finish following its stable active-next link. They are raw arena
  ** allocations too, and therefore remain explicit native roots even after
  ** their userdata reference is cleared. */
  node = lj_thread_live_retired_head_acq(g);
  gc2_root_cycle_guard_init(&guard, node);
  while (node != NULL) {
    GC2MarkScope scope;
    int status = gc2_markmem_registered_scoped_status(g, node, &scope);
    if (status == GC2_MARK_DEAD) {
      gc2_mark_scope_leave(&scope);
      gc2_root_scan_retry(g);
      break;
    }
    next = lj_thread_live_retired_next_acq(node);
    gc2_mark_scope_leave(&scope);
    node = next;
    if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, node))) {
      gc2_root_scan_retry(g);
      break;
    }
  }
}

static void gc2_scan_finreg_udata_nodes(global_State *g)
{
  GC2FinRegUDataNode *node, *next;
  GC2RootCycleGuard guard;
  /* These records are native ownership metadata, not semantic userdata roots:
  ** retaining node->obj here would prevent ordinary finalizer discovery. Hold
  ** the raw allocation admission only long enough to load its stable link. */
  node = gc2_finreg_udata_head_acq(g);
  gc2_root_cycle_guard_init(&guard, node);
  while (node != NULL) {
    GC2MarkScope scope;
    int status = gc2_markmem_registered_scoped_status(g, node, &scope);
    if (status == GC2_MARK_DEAD) {
      gc2_mark_scope_leave(&scope);
      gc2_root_scan_retry(g);
      break;
    }
    next = gc2_finreg_udata_next_acq(node);
    gc2_mark_scope_leave(&scope);
    node = next;
    if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, node))) {
      gc2_root_scan_retry(g);
      break;
    }
  }

  /* Logical retirement leaves the active link stable for in-flight readers;
  ** the separate retired list owns the same raw record until final shutdown. */
  node = gc2_finreg_udata_retired_acq(g);
  gc2_root_cycle_guard_init(&guard, node);
  while (node != NULL) {
    GC2MarkScope scope;
    int status = gc2_markmem_registered_scoped_status(g, node, &scope);
    if (status == GC2_MARK_DEAD) {
      gc2_mark_scope_leave(&scope);
      gc2_root_scan_retry(g);
      break;
    }
    next = gc2_finreg_udata_retired_next_acq(node);
    gc2_mark_scope_leave(&scope);
    node = next;
    if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, node))) {
      gc2_root_scan_retry(g);
      break;
    }
  }
}

static void gc2_scan_pending_roots(global_State *g)
{
  if (!g)
    return;
  lj_gc2_finalizer_mark_all(g, gc2_mark_finalizer_obj);
  gc2_scan_threading_live_roots(g);
  gc2_scan_finreg_udata_nodes(g);
#if LJ_HASFFI
  gc2_mark_finreg_cdata_preclaims(g, gc2_finreg_markobj,
				   gc2_finreg_marktv);
#endif
}

static int gc2_admit_thread_identity(global_State *g, lua_State *L,
				      GC2MarkScope *scope)
{
  uint32_t gct;
  gc2_mark_scope_init(scope);
  if (!g || !L)
    return GC2_MARK_DEAD;
  /* The main lua_State is embedded in GG_State and shares the global state's
  ** externally pinned lifetime, rather than owning an exact arena allocation.
  ** All other states require counted object admission. */
  if (L == mainthread_acq(g))
    return GC2_MARK_LIVE_ALREADY;
  if (la_load32_acq(&g->allocf_arena) != 0 &&
      gc2_small_arena_known(g, lj_arena_of(L))) {
    if (!gc2_observed_obj_valid_scoped(g, obj2gco(L), &gct, scope) ||
	gct != (uint32_t)~LJ_TTHREAD) {
      gc2_mark_scope_leave(scope);
      return GC2_MARK_DEAD;
    }
    /* Registry membership is already the semantic identity root. Admission
    ** must not manufacture duplicate SSB graph work while scanners hand off
    ** from current to successor. */
    return GC2_MARK_LIVE_ALREADY;
  }
  /* A hypothetical huge lua_State needs the durable HugeTab MARK lifetime LP;
  ** normal states are fixed-size small-arena allocations above. */
  return gc2_markobj_expected_scoped_status(
    g, obj2gco(L), (uint32_t)~LJ_TTHREAD, NULL, scope);
}

static void gc2_scan_threading_states(global_State *g)
{
  lua_State *th;
  GC2MarkScope scope;
  GC2RootCycleGuard guard;
  int status;
  if (!g)
    return;
  th = lj_state_thread_registry_head_acq(g);
  if (!th)
    return;
  gc2_root_cycle_guard_init(&guard, th);
  status = gc2_admit_thread_identity(g, th, &scope);
  if (status == GC2_MARK_DEAD) {
    gc2_mark_scope_leave(&scope);
    gc2_root_scan_retry(g);
    return;
  }
  /* Publication prepends a finite head snapshot and removal only shortens its
  ** stable successor path. Walk that admitted suffix to completion: retrying
  ** forever at an arbitrary count cap would make a large valid registry pin
  ** MARK permanently. */
  while (th != NULL) {
    GC2MarkScope nextscope;
    lua_State *next;
    if (!lj_state_thread_registry_valid(g, th)) {
      gc2_root_scan_retry(g);
      break;
    }
    next = lj_state_thread_registry_next_acq(th);
    /* Registry membership is a process-global identity root, never permission
    ** to read stack storage. An ownerless state can race a 0->tid claim, and a
    ** threading child is normally foreign-owned. Marking the identity queues
    ** ordinary traversal, which either claims a genuinely suspended state or
    ** publishes a tid-addressed NEEDSCAN handoff for its owner. */
    gc2_mark_thread_root_obj(g, obj2gco(th));
    if (next) {
      status = gc2_admit_thread_identity(g, next, &nextscope);
      if (status == GC2_MARK_DEAD) {
	gc2_mark_scope_leave(&nextscope);
	gc2_root_scan_retry(g);
	break;
      }
    } else {
      gc2_mark_scope_init(&nextscope);
    }
    gc2_mark_scope_leave(&scope);
    th = next;
    scope = nextscope;
    if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, th))) {
      gc2_root_scan_retry(g);
      break;
    }
  }
  gc2_mark_scope_leave(&scope);
}

static void gc2_scan_tg_anchor_roots(global_State *g, TGState *tg)
{
  TGRootAnchorBlock *block = &tg->root_anchor;
  uint32_t i = 0, n = lj_tg_root_anchor_top_acq(tg);
  if (LJ_UNLIKELY(n > LJ_GC2_ROOT_SCAN_LIMIT)) {
    gc2_root_scan_retry(g);
    return;
  }
  while (block && i < n) {
    GC2MarkScope scope;
    TGRootAnchorBlock *next;
    uint32_t j;
    int status = GC2_MARK_LIVE_ALREADY;
    gc2_mark_scope_init(&scope);
    if (block != &tg->root_anchor) {
      status = gc2_markmem_registered_scoped_status(g, block, &scope);
      if (status == GC2_MARK_DEAD) {
	gc2_mark_scope_leave(&scope);
	gc2_root_scan_retry(g);
	return;
      }
    }
    for (j = 0; j < TG_ROOT_ANCHOR_SLOTS && i < n; j++, i++) {
      TValue tv;
      lj_tv_load_acq(&tv, &block->slot[j]);
      gc2_mark_thread_root_tv(g, &tv);
    }
    next = lj_tg_root_anchor_next_acq(block);
    gc2_mark_scope_leave(&scope);
    block = next;
  }
  if (LJ_UNLIKELY(i != n))
    gc2_root_scan_retry(g);
}

static void gc2_scan_tg_thread_root(global_State *g, TGState *tg,
				     lua_State *L)
{
  GC2MarkScope scope;
  int status;
  uint32_t tid;
  if (!L)
    return;
  status = gc2_admit_thread_identity(g, L, &scope);
  if (status == GC2_MARK_DEAD) {
    gc2_root_scan_retry(g);
    return;
  }
  /* Scoped admission marks the TG publication identity even if ownership has
  ** just moved. Stack contents/frame geometry remain strictly tid-owned. */
  tid = lj_tg_tid_acq(tg);
  if (!lj_thr_id_is_owner(tid) ||
      lj_state_owner_acq(L) != tid || tvref(L->stack) == NULL) {
    /* A stale live alias still needs ordinary identity traversal so its actual
    ** owner can satisfy a NEEDSCAN handoff; admission only pins the body. */
    gc2_mark_thread_root_obj(g, obj2gco(L));
    gc2_mark_scope_leave(&scope);
    return;
  }
  if (!gc2_scan_thread_stack(g, L, &scope)) {
    /* If validation/authority was transiently unavailable, republish identity
    ** traversal so env/mt/upvalues and the stack cannot disappear behind an
    ** already-marked thread body. */
    gc2_mark_thread_root_obj(g, obj2gco(L));
  }
  if (LJ_UNLIKELY(lj_state_owner_acq(L) != tid) &&
      gc2_thread_has_live_owner(g, L))
    gc2_thread_set_needscan(g, L);
  gc2_mark_scope_leave(&scope);
}

#if LJ_HASFFI && LJ_HASJIT
typedef enum GC2FFINativeScanStatus {
  GC2_FFI_NATIVE_SCAN_INVALID = -1,
  GC2_FFI_NATIVE_SCAN_RETRY = 0,
  GC2_FFI_NATIVE_SCAN_OK = 1
} GC2FFINativeScanStatus;

typedef struct GC2FFINativeGeometry {
  lua_State *L;
  TValue *stack;
  TValue *maxstack;
  TValue *root;
  TValue *base;
  TValue *top;
  TValue *jitbase;
  LJGC2Lease stacklease;
} GC2FFINativeGeometry;

static int gc2_ffi_native_offset_ptr(uintptr_t stack, uintptr_t maxstack,
				     uint64_t offset, TValue **result)
{
  uint64_t extent;
  if (maxstack < stack)
    return 0;
  extent = (uint64_t)(maxstack - stack);
  if (offset > extent || (offset % sizeof(TValue)) != 0)
    return 0;
  *result = (TValue *)(void *)(stack + (uintptr_t)offset);
  return 1;
}

/* Convert saved byte offsets only after the frame's raw L pointer has matched
** the TG's trusted current-state publication and that identity has acquired a
** GC2 observation scope. The native-park certificate, not this scope or the
** sequence word, is what prevents stack storage relocation during the reads. */
static GC2FFINativeScanStatus gc2_ffi_native_frame_geometry(
  global_State *g, TGState *tg, const LJFFINativeFrame *frame,
  GC2FFINativeGeometry *geo, GC2MarkScope *scope)
{
  lua_State *L = lj_ffi_native_frame_L_acq(frame);
  TValue *stack, *maxstack;
  uintptr_t stack_addr, maxstack_addr;
  uint64_t root_offset, base_offset, top_offset, jitbase_offset;
  uint64_t prefix = (uint64_t)(1 + LJ_FR2) * sizeof(TValue);
  uint32_t tid;
  int status;

  gc2_mark_scope_init(scope);
  memset(geo, 0, sizeof(*geo));
  if (!L || L != lj_tg_load_cur_L(tg))
    return GC2_FFI_NATIVE_SCAN_INVALID;
  status = gc2_admit_thread_identity(g, L, scope);
  if (status == GC2_MARK_DEAD)
    return GC2_FFI_NATIVE_SCAN_RETRY;
  tid = lj_tg_tid_acq(tg);
  if (!lj_thr_id_is_owner(tid) || lj_state_owner_acq(L) != tid ||
      mref(L->glref, global_State) != g ||
      !gc2_valid_thread_for_traverse_held(g, L, scope))
    goto invalid;
  stack = tvref(L->stack);
  maxstack = tvref(L->maxstack);
  if (!stack || !maxstack)
    goto invalid;
  stack_addr = (uintptr_t)(void *)stack;
  maxstack_addr = (uintptr_t)(void *)maxstack;
  if (maxstack_addr < stack_addr ||
      ((maxstack_addr - stack_addr) % sizeof(TValue)) != 0)
    goto invalid;
  status = lj_gc2_mem_lease_acquire(g, stack, &geo->stacklease);
  if (status < 0)
    goto retry;

  root_offset = lj_ffi_native_frame_root_offset_acq(frame);
  base_offset = lj_ffi_native_frame_base_offset_acq(frame);
  top_offset = lj_ffi_native_frame_top_offset_acq(frame);
  jitbase_offset = lj_ffi_native_frame_jit_base_offset_acq(frame);
  if (root_offset < prefix ||
      root_offset >= (uint64_t)(maxstack_addr-stack_addr) ||
      base_offset >= (uint64_t)(maxstack_addr-stack_addr) ||
      jitbase_offset < prefix ||
      jitbase_offset >= (uint64_t)(maxstack_addr-stack_addr) ||
      root_offset > base_offset || base_offset > top_offset ||
      jitbase_offset > top_offset ||
      !gc2_ffi_native_offset_ptr(stack_addr, maxstack_addr, root_offset,
					&geo->root) ||
      !gc2_ffi_native_offset_ptr(stack_addr, maxstack_addr, base_offset,
					&geo->base) ||
      !gc2_ffi_native_offset_ptr(stack_addr, maxstack_addr, top_offset,
					&geo->top) ||
      !gc2_ffi_native_offset_ptr(stack_addr, maxstack_addr, jitbase_offset,
					&geo->jitbase))
    goto invalid;
  geo->L = L;
  geo->stack = stack;
  geo->maxstack = maxstack;
  return GC2_FFI_NATIVE_SCAN_OK;

invalid:
  lj_gc2_lease_release(&geo->stacklease);
  gc2_mark_scope_leave(scope);
  return GC2_FFI_NATIVE_SCAN_INVALID;

retry:
  lj_gc2_lease_release(&geo->stacklease);
  gc2_mark_scope_leave(scope);
  return GC2_FFI_NATIVE_SCAN_RETRY;
}

/* The slot lookup is the lifetime-safe first dereference. A frame pointer is
** never inspected until the current TraceVec proves that it is the exact body
** reserved by trace_no, and the body's count word proves the native lease. */
static GC2FFINativeScanStatus gc2_ffi_native_mark_trace(
  global_State *g, const LJFFINativeFrame *frame)
{
  GCtrace *trace = lj_ffi_native_frame_trace_acq(frame);
  TraceNo traceno = (TraceNo)lj_ffi_native_frame_trace_no_acq(frame);
  GCtrace *slot = gc2_traceref_safe(g, traceno);
  if (!slot || slot != trace || trace_native_pins_acq(slot) == 0)
    return GC2_FFI_NATIVE_SCAN_INVALID;
  /* Retirement may already have cleared T->traceno. Queueing T alone would
  ** then make gc2_traverse_trace() intentionally skip its graph, so preserve
  ** every KGC/link/start-proto/snapshot-PC/exittab edge synchronously. */
  if (!lj_trace_native_mark_pinned(g, slot, traceno))
    return GC2_FFI_NATIVE_SCAN_RETRY;
  if (!gc2_mark_thread_root_obj_status(g, obj2gco(slot)))
    return GC2_FFI_NATIVE_SCAN_RETRY;
  /* The frame owner brackets unpin/slot handoff with an odd sequence. This
  ** second exact check catches a violated or transient publication before the
  ** result can be counted as a completed exact scan. */
  slot = gc2_traceref_safe(g, traceno);
  if (!slot || slot != trace || trace_native_pins_acq(slot) == 0)
    return GC2_FFI_NATIVE_SCAN_RETRY;
  return GC2_FFI_NATIVE_SCAN_OK;
}

/* The generated-call result box is deliberately outside the materialized Lua
** stack until CALLXS has returned and the caller snapshot is constructed.
** Its native frame is therefore the stable direct root across ACTIVE foreign
** execution, callback SUSPENDED continuations and the POSTCALL exit handoff. */
static GC2FFINativeScanStatus gc2_ffi_native_mark_result_root(
  global_State *g, const LJFFINativeFrame *frame)
{
  GCcdata *root = lj_ffi_native_frame_result_root_acq(frame);
  LJGC2Lease lease;
  int status;
  if (!root)
    return GC2_FFI_NATIVE_SCAN_OK;
  if (!gc2_mark_thread_root_obj_status(g, obj2gco(root)))
    return GC2_FFI_NATIVE_SCAN_RETRY;
  /* Re-admit with an exact type lease instead of dereferencing the raw frame
  ** word after a SWEEP recovery. A stale or corrupted publication which names
  ** another live allocation is stable invalidity, never an exact cdata root. */
  status = lj_gc2_obj_lease_acquire(g, obj2gco(root),
	(uint32_t)~LJ_TCDATA, NULL, &lease);
  if (status < 0)
    return GC2_FFI_NATIVE_SCAN_INVALID;
  lj_gc2_lease_release(&lease);
  return GC2_FFI_NATIVE_SCAN_OK;
}

static GC2FFINativeScanStatus gc2_ffi_native_mark_frame_funcs(
  global_State *g, const GC2FFINativeGeometry *geo)
{
  TValue *frame = geo->base - 1;
  uint32_t n = 0;
  while (frame > geo->stack + LJ_FR2 && frame < geo->maxstack) {
    GCfunc *fn;
    TValue *prev;
    GC2FrameScope scope;
    if (!gc2_frame_prev_safe(g, geo->stack, geo->maxstack, frame,
			     &prev, &fn, &scope))
      return GC2_FFI_NATIVE_SCAN_RETRY;
    if (fn && !gc2_mark_thread_root_obj_status(g, obj2gco(fn))) {
      gc2_frame_scope_leave(&scope);
      return GC2_FFI_NATIVE_SCAN_RETRY;
    }
    gc2_frame_scope_leave(&scope);
    if (prev >= frame || prev >= geo->maxstack)
      return GC2_FFI_NATIVE_SCAN_INVALID;
    if (prev < geo->stack + LJ_FR2)
      return GC2_FFI_NATIVE_SCAN_INVALID;
    if (prev == geo->stack + LJ_FR2)
      break;
    frame = prev;
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT)
      return GC2_FFI_NATIVE_SCAN_RETRY;
  }
  return GC2_FFI_NATIVE_SCAN_OK;
}

static GC2FFINativeScanStatus gc2_ffi_native_scan_one(
  global_State *g, TGState *tg, const LJFFINativeFrame *frame, int top_frame)
{
  GC2FFINativeGeometry geo;
  GC2MarkScope scope;
  GC2FFINativeScanStatus status;
  uint32_t flags = lj_ffi_native_frame_flags_acq(frame);
  TValue *o;

  /* Only the top ACTIVE frame can certify a materialized parked stack. Lower
  ** callback continuations are SUSPENDED and are handled as trace-graph roots
  ** by the enclosing whole-stack scanner. POSTCALL is never scan authority. */
  if ((flags & (LJ_FFI_NATIVE_FRAME_F_ACTIVE |
		LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
		LJ_FFI_NATIVE_FRAME_F_POSTCALL)) !=
      LJ_FFI_NATIVE_FRAME_F_ACTIVE || !top_frame)
    return GC2_FFI_NATIVE_SCAN_INVALID;
  status = gc2_ffi_native_frame_geometry(g, tg, frame, &geo, &scope);
  if (status != GC2_FFI_NATIVE_SCAN_OK)
    return status;
  if (top_frame && geo.jitbase != lj_tg_load_jit_base(tg)) {
    status = GC2_FFI_NATIVE_SCAN_INVALID;
    goto out;
  }
  status = gc2_ffi_native_mark_trace(g, frame);
  if (status != GC2_FFI_NATIVE_SCAN_OK)
    goto out;
  status = gc2_ffi_native_mark_result_root(g, frame);
  if (status != GC2_FFI_NATIVE_SCAN_OK)
    goto out;
  if (!gc2_mark_thread_root_obj_status(g, obj2gco(geo.L))) {
    status = GC2_FFI_NATIVE_SCAN_RETRY;
    goto out;
  }
  for (o = geo.stack + 1 + LJ_FR2; o < geo.top; o++) {
    TValue tv;
    lj_tv_load_acq(&tv, o);
    if (!gc2_mark_thread_root_tv_status(g, &tv)) {
      status = GC2_FFI_NATIVE_SCAN_RETRY;
      goto out;
    }
  }
  status = gc2_ffi_native_mark_frame_funcs(g, &geo);
out:
  if (status == GC2_FFI_NATIVE_SCAN_OK &&
      (lj_tg_load_cur_L(tg) != geo.L ||
       lj_state_owner_acq(geo.L) != lj_tg_tid_acq(tg) ||
       mref(geo.L->glref, global_State) != g ||
       !gc2_valid_thread_for_traverse_held(g, geo.L, &scope) ||
       tvref(geo.L->stack) != geo.stack ||
       tvref(geo.L->maxstack) != geo.maxstack ||
       (top_frame && lj_tg_load_jit_base(tg) != geo.jitbase)))
    status = GC2_FFI_NATIVE_SCAN_RETRY;
  lj_gc2_lease_release(&geo.stacklease);
  gc2_mark_scope_leave(&scope);
  return status;
}

static int gc2_scan_ffi_native_frames_parked(global_State *g, TGState *tg)
{
  LJFFINativeFrameSnapshot snapshot;
  LJFFINativeFrameSnapshotResult result;
  GC2FFINativeScanStatus status;
  lua_State *L;
  uint32_t i, top_state;

  gc2_ffi_native_scan_attempts_add(g, 1);
  result = lj_ffi_native_frame_snapshot(tg, &snapshot);
  if (result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY)
    return 1;
  if (result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_RETRY) {
    gc2_ffi_native_scan_retries_add(g, 1);
    gc2_root_scan_retry(g);
    return 0;
  }
  if (result != LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE) {
    gc2_ffi_native_scan_invalid_add(g, 1);
    gc2_root_scan_retry(g);
    return 0;
  }
  L = lj_tg_load_cur_L(tg);
  if (!L) {
    gc2_ffi_native_scan_invalid_add(g, 1);
    gc2_root_scan_retry(g);
    return 0;
  }
  top_state = lj_ffi_native_frame_flags_acq(
    &snapshot.frame[snapshot.depth - 1u]) &
    (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
     LJ_FFI_NATIVE_FRAME_F_POSTCALL);
  if (top_state != LJ_FFI_NATIVE_FRAME_F_ACTIVE &&
      top_state != LJ_FFI_NATIVE_FRAME_F_SUSPENDED) {
    gc2_ffi_native_scan_invalid_add(g, 1);
    gc2_root_scan_retry(g);
    return 0;
  }
  if (top_state == LJ_FFI_NATIVE_FRAME_F_SUSPENDED &&
      lj_tg_load_jit_base(tg) != NULL) {
    gc2_ffi_native_scan_invalid_add(g, 1);
    gc2_root_scan_retry(g);
    return 0;
  }
  for (i = 0; i < snapshot.depth; i++) {
    const LJFFINativeFrame *frame = &snapshot.frame[i];
    uint32_t state = lj_ffi_native_frame_flags_acq(frame) &
      (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
       LJ_FFI_NATIVE_FRAME_F_POSTCALL);
    uint32_t expected = i + 1u == snapshot.depth ? top_state :
      LJ_FFI_NATIVE_FRAME_F_SUSPENDED;
    if (state != expected || lj_ffi_native_frame_L_acq(frame) != L) {
      gc2_ffi_native_scan_invalid_add(g, 1);
      gc2_root_scan_retry(g);
      return 0;
    }
    /* A lower continuation contributes its exact pinned trace graph, but its
    ** pre-callback top/base geometry is not authority for the current stack.
    ** A suspended-only stack means an unrelated interpreted native helper is
    ** parked; the conservative owner scan below remains stack authority. */
    if (state == LJ_FFI_NATIVE_FRAME_F_ACTIVE) {
      status = gc2_ffi_native_scan_one(g, tg, frame, 1);
    } else {
      status = gc2_ffi_native_mark_trace(g, frame);
      if (status == GC2_FFI_NATIVE_SCAN_OK)
	status = gc2_ffi_native_mark_result_root(g, frame);
    }
    if (status != GC2_FFI_NATIVE_SCAN_OK) {
      if (status == GC2_FFI_NATIVE_SCAN_INVALID)
	gc2_ffi_native_scan_invalid_add(g, 1);
      else
	gc2_ffi_native_scan_retries_add(g, 1);
      gc2_root_scan_retry(g);
      return 0;
    }
  }
  /* Validate after the final stack/body read. This is diagnostic under the
  ** consumed-poll certificate, but fail-closed if a future caller misuses it. */
  if (lj_ffi_native_frame_sequence_acq(tg) != snapshot.sequence) {
    gc2_ffi_native_scan_retries_add(g, 1);
    gc2_root_scan_retry(g);
    return 0;
  }
  gc2_ffi_native_scan_stable_frames_add(g, snapshot.depth);
  return 1;
}

/* Owner execution covers the current Lua stack conservatively, but a generated
** result box is not stack-visible until the protected post-call snapshot has
** restored it. Preserve the exact trace and result root of every published
** ACTIVE, SUSPENDED or POSTCALL frame. This also covers older callback
** continuations after retirement has cleared T->traceno. */
static int gc2_mark_ffi_native_owner_frames(global_State *g, TGState *tg)
{
  LJFFINativeFrameSnapshot snapshot;
  LJFFINativeFrameSnapshotResult result;
  lua_State *L;
  uint32_t i;
  result = lj_ffi_native_frame_snapshot(tg, &snapshot);
  if (result == LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY)
    return 1;
  if (result != LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE)
    goto retry;
  L = lj_tg_load_cur_L(tg);
  if (!L)
    goto retry;
  for (i = 0; i < snapshot.depth; i++) {
    const LJFFINativeFrame *frame = &snapshot.frame[i];
    uint32_t state = lj_ffi_native_frame_flags_acq(frame) &
      (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
       LJ_FFI_NATIVE_FRAME_F_POSTCALL);
    if ((i + 1u < snapshot.depth &&
	 state != LJ_FFI_NATIVE_FRAME_F_SUSPENDED) ||
	(i + 1u == snapshot.depth &&
	 state != LJ_FFI_NATIVE_FRAME_F_ACTIVE &&
	 state != LJ_FFI_NATIVE_FRAME_F_SUSPENDED &&
	 state != LJ_FFI_NATIVE_FRAME_F_POSTCALL))
      goto retry;
    if (lj_ffi_native_frame_L_acq(frame) != L ||
	gc2_ffi_native_mark_trace(g, frame) != GC2_FFI_NATIVE_SCAN_OK ||
	gc2_ffi_native_mark_result_root(g, frame) !=
	  GC2_FFI_NATIVE_SCAN_OK)
      goto retry;
  }
  if (lj_ffi_native_frame_sequence_acq(tg) != snapshot.sequence)
    goto retry;
  return 1;
retry:
  gc2_root_scan_retry(g);
  return 0;
}
#endif

static int gc2_scan_owner_tg_roots(global_State *g, TGState *tg)
{
  lua_State *thread_L, *cur_L;
  TValue tv;
  int complete = 1;
  if (!tg || lj_tg_flags_test_acq(tg, TGF_DEAD))
    return 1;
#if LJ_HASFFI && LJ_HASJIT
  complete = gc2_mark_ffi_native_owner_frames(g, tg);
#endif
  /* Detach tears down tmpbuf before publishing DEAD. Every remaining live TG
  ** scans this owner-private backing only at its acknowledgement boundary. */
  lj_gc2_markmem(g, lj_buf_bptr_acq(&tg->tmpbuf));
  /* Active source/bytecode loads keep parser vectors in native LexState frames.
  ** Their owner-published TG chain is stable at this acknowledgement boundary. */
  lj_lex_gc2_markroots(g, tg);
  if (lj_tg_jit_active_acq(tg)) {
    lj_tv_load_acq(&tv, &tg->tmptv);
    gc2_mark_thread_root_tv(g, &tv);
    lj_tv_load_acq(&tv, &tg->tmptv2);
    gc2_mark_thread_root_tv(g, &tv);
  }
  /* Table resize/retry helpers publish transient keys here instead of L->top.
  ** The block walk also roots dynamically extended anchor backing storage. */
  gc2_scan_tg_anchor_roots(g, tg);
  thread_L = lj_tg_load_thread_L(tg);
  cur_L = lj_tg_load_cur_L(tg);
  if (thread_L) {
    gc2_scan_tg_thread_root(g, tg, thread_L);  /* 05 section 5.7.4. */
    gc2_tg_thread_roots_add(g, 1);
  }
  if (cur_L && cur_L != thread_L) {
    gc2_scan_tg_thread_root(g, tg, cur_L);  /* 05 section 5.7.4. */
    gc2_tg_cur_roots_add(g, 1);
  }
#if LJ_HASJIT
  {
    int32_t vmstate = lj_tg_vmstate_load_acq(tg);
    if (vmstate > 0 && gc2_mark_trace_root(g, (TraceNo)vmstate))
      gc2_tg_trace_roots_add(g, 1);
  }
#endif
  gc2_scan_owned_needscan(g, tg);
  return complete;
}

#if LJ_HASJIT
static void gc2_scan_current_trace_root(global_State *g);

static void gc2_current_trace_root_retry(global_State *g)
{
  lj_trace_abort(g);
  gc2_root_scan_retry(g);
}

static void gc2_scan_jit_roots(global_State *g)
{
  jit_State *J = G2J(g);
  /*
  ** Published and retired JIT state is outside the ordinary Lua root graph, but
  ** stale patched bytecode, exit restore, and mcode retirement may still name it
  ** until the SMR grace period completes. Both major and minor root scans must
  ** retain the same JIT-side roots; otherwise a minor cycle after jit.flush()
  ** can recycle retired trace/mcode metadata while stale readers still hold it.
  */
  if (!lj_trace_markvecs(g, 1))
    gc2_root_scan_retry(g);
  gc2_scan_current_trace_root(g);
  if (!lj_mcode_markretired(g, 1))
    gc2_root_scan_retry(g);
  lj_gc2_markmem(g, J->irbuf ? J->irbuf + J->irbotlim : NULL);
  lj_gc2_markmem(g, J->snapbuf);
  lj_gc2_markmem(g, J->snapmapbuf);
}

static void gc2_scan_current_trace_root(global_State *g)
{
  jit_State *J = G2J(g);
  if (lj_trace_state_load(J) == LJ_TRACE_IDLE)
    return;
  /*
  ** J->cur/J->curfinal are NOBARRIER token-private construction state. Even a
  ** same-TG nested allocation can enter this root scan while it owns the JIT
  ** token, and recording may append a KGC or snapshot edge after that nested
  ** call returns. Consequently no observation of active recorder geometry can
  ** become the persistent root certificate. Request an asynchronous abort and
  ** keep MARK open until the recorder state machine has unwound to IDLE; the
  ** published/retired trace roots then cover everything which survived.
  **
  ** This path never waits. A descheduled or foreign recorder merely defers
  ** root closure, and no arena sweep can begin while its retry is outstanding.
  */
  gc2_current_trace_root_retry(g);
}
#endif

static void gc2_root_scan_retry(global_State *g)
{
  uint32_t phase;
  if (!g)
    return;
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_MARK) {
    /* MARK's state 1 is a reusable certificate, so any failed later scan must
    ** invalidate completed as well as provisional state. */
    gc2_mark_root_scanned_rel(g, 0);
  } else if (phase == LJ_GC2_WEAK) {
    /* WEAK owns its own 0->2->1 snapshot. Clearing state 2 makes the owner's
    ** final CAS fail; clearing state 1/mark_closed also reopens a completed
    ** frontier if a later required scan reports an incomplete publication. */
    gc2_weak_root_scanned_rel(g, 0);
    gc2_weak_mark_closed_rel(g, 0);
  } else if (phase == LJ_GC2_SWEEP) {
    uint32_t expect = 2;
    /* Only the initial mandatory SWEEP snapshot is provisional. A later
    ** barrier/NEEDSCAN handshake must not revoke an already-published READY
    ** bridge; its concrete pending work and marks still block reclaim. */
    (void)gc2_sweep_root_scanned_cas(g, &expect, 0);
  } else {
    return;
  }
  gc2_marks_this_round_add(g, 1);
  lj_gc2_worker_wake(g);
}

static void gc2_scan_global_roots(global_State *g)
{
  lua_State *mainL, *vmL;
  ptrdiff_t i;
  if (!g)
    return;
  mainL = mainthread_acq(g);
  vmL = vmthread_acq(g);
  /* The root scan cannot make progress without the required main thread. */
  lj_assertG(mainL != NULL, "missing main thread root");
  if (LJ_UNLIKELY(mainL == NULL))
    return;
  /*
  ** Object allocation publishes freshly initialized bodies through per-TG
  ** pending root chains. A major GC2 root scan owns the same global root
  ** frontier as the root pass, so flush those chains before walking it.
  */
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  if (!lj_gc2_smr_read_try(g)) {
    gc2_root_scan_retry(g);
    return;
  }
  gc2_mark_thread_root_obj(g, obj2gco(mainL));
  if (vmL)
    gc2_mark_thread_root_obj(g, obj2gco(vmL));
  gc2_mark_thread_root_tv(g, lj_registry_ref(g));
  for (i = 0; i < GCROOT_MAX; i++) {
    GCobj *o = lj_gcroot_acq(g, (GCRootID)i);
    if (o != NULL)
      gc2_mark_thread_root_obj(g, o);
  }
  gc2_scan_pending_roots(g);
  gc2_scan_threading_states(g);
  gc2_mark_strtab_mem(g);
  gc2_mark_tab_retired_mem(g);
#if LJ_64
  lj_gc2_markmem(g, mref(g->gc.lightudseg, uint32_t));
#endif
  lj_gc2_markmem(g, lj_buf_bptr_acq(&g->tmpbuf));
#if LJ_HASFFI
  {
    CTState *cts = ctype_ctsG(g);
    if (cts) {
      CTypeTab *ret, *nextret;
      GC2RootCycleGuard guard;
      GCtab *miscmap;
      GCRef *meta;
      uint64_t *cbblack;
      TValue *func;
      lua_State **owner;
      /* Retain the CTState before reading any concurrently published field. */
      lj_gc2_markmem(g, cts);
      miscmap = ctype_miscmap_acq(cts);
      if (miscmap)
	(void)lj_gc2_markobj(g, obj2gco(miscmap));
      meta = ctype_metamap_acq(cts);
      cbblack = ctype_cbblack_acq(cts);
      lj_gc2_markmem(g, ctype_tabh_acq(cts));
      ret = ctype_retiredtab_acq(cts);
      gc2_root_cycle_guard_init(&guard, ret);
      while (ret != NULL && lj_gc2_mem_registered(g, ret)) {
	nextret = ctype_tab_retired_next_acq(ret);
	lj_gc2_markmem(g, ret);
	ret = nextret;
	if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, ret))) {
	  gc2_root_scan_retry(g);
	  break;
	}
      }
      if (LJ_UNLIKELY(ret != NULL && !lj_gc2_mem_registered(g, ret)))
	gc2_root_scan_retry(g);
      lj_gc2_markmem(g, meta);
      lj_gc2_markmem(g, cbblack);
      if (meta) {
	MSize i, n = ctype_metamap_size_acq(cts);
	for (i = 0; i < n; i++) {
	  GCobj *o = ctype_metamap_obj_acq(meta, i);
	  if (o)
	    lj_gc2_markobj(g, o);
	}
      }
      gc2_traverse_clib_retired_cache(g);
      (void)gc2_mark_finreg_cdata_generations(g, gc2_finreg_markobj,
					       gc2_finreg_markmem);
      lj_gc2_markmem(g, ctype_cb_cbid_acq(cts));
      owner = ctype_cb_owner_acq(cts);
      lj_gc2_markmem(g, owner);
      if (owner) {
	MSize i, n = ctype_cb_sizeid_acq(cts);
	for (i = 0; i < n; i++) {
	  lua_State *th = ctype_cb_owner_slot_acq(owner, i);
	  if (th)
	    lj_gc2_markobj(g, obj2gco(th));
	}
      }
      func = ctype_cb_func_acq(cts);
      lj_gc2_markmem(g, func);
      if (func) {
	MSize i, n = ctype_cb_sizeid_acq(cts);
	for (i = 0; i < n; i++) {
	  TValue tv;
	  lj_tv_load_acq(&tv, &func[i]);
	  gc2_mark_tv(g, &tv);
	}
      }
    }
  }
#endif
#if LJ_HASJIT
  gc2_scan_jit_roots(g);
#endif
  lj_gc2_smr_read_leave(g);
}

static void lj_gc2_scan_roots(global_State *g, lua_State *L)
{
  if (!g)
    return;
  gc2_major_root_scans_add(g, 1);
  gc2_scan_global_roots(g);
  if (L)
    lj_gc2_scan_cycle_owner_tg_roots(g, L2TG(L));
}

static void lj_gc2_scan_minor_roots(global_State *g, lua_State *L)
{
  if (!g || gc2_cycle_roots_minor_acq(g) == 0)
    return;
  gc2_minor_root_scans_add(g, 1);
  /*
  ** Minor sweep identity does not shrink the root set. Process-global roots can
  ** point at young objects just as stacks can: library environments, the
  ** registry, fixed metatables and FFI/JIT side roots all publish ordinary GC
  ** edges. Keep the root frontier identical to a major scan, then let the minor
  ** mark/sweep filters decide which generations to reclaim.
  */
  gc2_scan_global_roots(g);
  if (L)
    lj_gc2_scan_cycle_owner_tg_roots(g, L2TG(L));
}

void lj_gc2_scan_cycle_roots(global_State *g, lua_State *L)
{
  if (!g)
    return;
  if (gc2_cycle_roots_minor_acq(g))
    lj_gc2_scan_minor_roots(g, L);
  else
    lj_gc2_scan_roots(g, L);
}

void lj_gc2_scan_cycle_global_roots(global_State *g)
{
  if (!g)
    return;
  if (gc2_cycle_roots_minor_acq(g))
    gc2_minor_root_scans_add(g, 1);
  else
    gc2_major_root_scans_add(g, 1);
  gc2_scan_global_roots(g);
}

void lj_gc2_scan_cycle_owner_roots(global_State *g, lua_State *L)
{
  if (g && L)
    lj_gc2_scan_cycle_owner_tg_roots(g, L2TG(L));
}

static int gc2_scan_cycle_owner_tg_roots_mode(global_State *g, TGState *tg,
					      int native_parked)
{
  int exact = 1;
  if (!g || !tg || tg->gl != g)
    return 0;
  /* TG storage, allocator membership validation and owner lookup all share the
  ** same tactical SMR lease as the global pass. Reclaim is opportunistic and
  ** abandons its writer attempt instead of waiting for owner acknowledgements. */
  if (!lj_gc2_smr_read_try(g)) {
    gc2_root_scan_retry(g);
    return 0;
  }
#if LJ_HASFFI && LJ_HASJIT
  if (native_parked)
    exact = gc2_scan_ffi_native_frames_parked(g, tg);
#else
  UNUSED(native_parked);
#endif
  /* Exact native-frame marking is deliberately additive. The conservative
  ** owner scan, including native/JIT maxstack widening and every NEEDSCAN
  ** handoff, remains authoritative until the later narrowing proof. */
  if (exact)
    exact = gc2_scan_owner_tg_roots(g, tg);
  lj_gc2_smr_read_leave(g);
  return exact;
}

void lj_gc2_scan_cycle_owner_tg_roots(global_State *g, TGState *tg)
{
  (void)gc2_scan_cycle_owner_tg_roots_mode(g, tg, 0);
}

int lj_gc2_scan_cycle_owner_tg_roots_native_parked(global_State *g,
						    TGState *tg)
{
  return gc2_scan_cycle_owner_tg_roots_mode(g, tg, 1);
}

void lj_gc2_test_scan_roots(global_State *g, lua_State *L)
{
  lj_gc2_scan_roots(g, L);
}

#if defined(LJ_GC2_TEST_HELPERS)
#if LJ_HASFFI && LJ_HASJIT
int lj_gc2_test_scan_ffi_native_frames(global_State *g, TGState *tg)
{
  int result;
  if (!g || !tg || tg->gl != g || !lj_gc2_smr_read_try(g))
    return 0;
  result = gc2_scan_ffi_native_frames_parked(g, tg);
  lj_gc2_smr_read_leave(g);
  return result;
}
#endif

void lj_gc2_test_scan_tg_thread_root(global_State *g, TGState *tg,
				      lua_State *L)
{
  if (g && tg && L && lj_gc2_smr_read_try(g)) {
    gc2_scan_tg_thread_root(g, tg, L);
    lj_gc2_smr_read_leave(g);
  }
}
#endif

void lj_gc2_test_scan_owned_needscan(global_State *g, lua_State *owner_L)
{
  if (g && owner_L) {
    if (!lj_gc2_smr_read_try(g))
      return;
    gc2_scan_owned_needscan(g, L2TG(owner_L));
    lj_gc2_smr_read_leave(g);
  }
}

void lj_gc2_test_scan_minor_roots(global_State *g, lua_State *L)
{
  lj_gc2_scan_minor_roots(g, L);
}

static LJ_AINLINE void *gc2_mark_base(global_State *g, GCobj *o);
static LJ_AINLINE int gc2_obj_may_traverse(GCobj *o);
static LJ_AINLINE int gc2_rescan_pending_set(GCobj *o);
static LJ_AINLINE uint8_t gc2_rescan_pending_clear(GCobj *o);
static LJ_AINLINE void gc2_rescan_pending_clear_if_table(global_State *g,
							 GCobj *o);
static LJ_AINLINE void gc2_rescan_pending_clear_cycle(global_State *g,
						      GCobj *o);
static int gc2_grey_push(global_State *g, GCobj *o);
static uint32_t gc2_drain_grey(global_State *g, uint32_t limit);
static int gc2_traverse_tab(global_State *g, GCtab *t);
static void gc2_traverse_udata(global_State *g, GCudata *ud);

static LJ_AINLINE void gc2_queue_slot_store_rel(GCRef *slot, GCobj *o)
{
  setgcrefrel(*slot, o);
}

static LJ_AINLINE GCobj *gc2_queue_slot_load_acq(const GCRef *slot)
{
  return gcref_acq(*slot);
}

static LJ_AINLINE void gc2_queue_slot_clear_rel(GCRef *slot)
{
  setgcrefnullrel(*slot);
}

/* Raw registry lookup is legal only while the caller retains either its own
** ordinary SMR reader or the exact current-thread exclusive-reclaimer
** certificate. The queue/root edge separately owns the candidate body. */
static int gc2_huge_observed_registry_held(global_State *g, GCobj *o,
					    void **basep, LJHugeInfo *hip)
{
  TGState *tg;
  int found = 0;
  if (!g || !o ||
      (!gc2_smr_reader_tls_active(g) && !gc2_reclaim_tls_active(g)))
    return 0;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg))
    if (lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
	lj_arena_hugetab_cdata_range_lookup(&tg->huge, o, basep, hip) == 1) {
      found = 1;
      break;
    }
  if (!found) {
    tg = g->main_tg;
    found = tg && lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
      lj_arena_hugetab_cdata_range_lookup(&tg->huge, o, basep, hip) == 1;
  }
  return found;
}

static int gc2_huge_observed(global_State *g, GCobj *o, void **basep,
			     LJHugeInfo *hip)
{
  int found;
  if (!g || !o || !lj_gc2_smr_read_try(g))
    return 0;
  found = gc2_huge_observed_registry_held(g, o, basep, hip);
  lj_gc2_smr_read_leave(g);
  return found;
}

/* Acquire an observational (non-marking) huge body reader. MISSING permits a
** search in the next registry; rejection/overflow identifies this allocation
** but supplies no dereferenceable body and must be returned immediately. */
static int gc2_huge_observed_scoped(global_State *g, GCobj *o, void **basep,
					    LJHugeInfo *hip,
					    GC2MarkScope *scope)
{
  GC2HugeRegistryLease lease = GC2_HUGE_REGISTRY_NONE;
  TGState *tg;
  int rc;
  if (basep)
    *basep = NULL;
  gc2_mark_scope_init(scope);
  if (!g || !o || !scope)
    return LJ_ARENA_HUGE_READER_MISSING;
  if (!gc2_huge_registry_read_try(g, &lease)) {
    gc2_activation_pin_no_reclaim(g);
    gc2_marks_this_round_add(g, 1);
    lj_gc2_worker_wake(g);
    return LJ_ARENA_HUGE_READER_OVERFLOW;
  }
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (!lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      continue;
    rc = lj_arena_hugetab_reader_cdata_range_acquire(
      &tg->huge, o, basep, &scope->huge, hip);
    if (rc != LJ_ARENA_HUGE_READER_MISSING)
      goto found;
  }
  tg = g->main_tg;
  if (!tg || !lj_tg_flags_test_acq(tg, TGF_HUGETAB)) {
    gc2_huge_registry_read_leave(g, &lease);
    return LJ_ARENA_HUGE_READER_MISSING;
  }
  rc = lj_arena_hugetab_reader_cdata_range_acquire(
    &tg->huge, o, basep, &scope->huge, hip);
found:
  gc2_huge_registry_read_leave(g, &lease);
  /* A successful slot reader pins the direct header beyond either registry
  ** admission mode. */
  if (rc == LJ_ARENA_HUGE_READER_ACQUIRED) {
    scope->admission = GC2_SCOPE_HUGE_READER;
  }
  return rc;
}

static HugeTab *gc2_huge_recovery_table(global_State *g, GCobj *o,
						 void **basep,
						 LJHugeInfo *hip)
{
  TGState *tg;
  if (basep)
    *basep = NULL;
  if (!g || !o)
    return NULL;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg))
    if (lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
	lj_arena_hugetab_range_lookup(&tg->huge, o, basep, hip) == 1)
      return &tg->huge;
  tg = g->main_tg;
  if (tg && lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
      lj_arena_hugetab_range_lookup(&tg->huge, o, basep, hip) == 1)
    return &tg->huge;
  return NULL;
}

static int gc2_mark_huge_range_scoped(global_State *g, const void *p,
				       void **basep, LJHugeInfo *hip,
				       GC2MarkScope *scope)
{
  GC2HugeRegistryLease lease = GC2_HUGE_REGISTRY_NONE;
  TGState *tg;
  int marked;
  if (basep)
    *basep = NULL;
  gc2_mark_scope_init(scope);
  if (!g || !p || !scope || !gc2_huge_registry_read_try(g, &lease))
    return LJ_ARENA_HUGE_READER_OVERFLOW;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (!lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      continue;
    marked = lj_arena_hugetab_mark_range_reader_acquire(
      &tg->huge, p, basep, &scope->huge, hip);
    if (marked >= 0 || marked == LJ_ARENA_HUGE_READER_OVERFLOW)
      goto found;
  }
  tg = g->main_tg;
  if (!tg || !lj_tg_flags_test_acq(tg, TGF_HUGETAB)) {
    gc2_huge_registry_read_leave(g, &lease);
    return GC2_MARK_DEAD;
  }
  marked = lj_arena_hugetab_mark_range_reader_acquire(
    &tg->huge, p, basep, &scope->huge, hip);
found:
  gc2_huge_registry_read_leave(g, &lease);
  if (marked >= 0 && marked != LJ_ARENA_HUGE_MARK_INTENT &&
      marked != LJ_ARENA_HUGE_MARK_SATURATED)
    scope->admission = GC2_SCOPE_HUGE_READER;
  return marked;
}

/* Resolve a return PC to its containing prototype without changing MARK.
** Frame-chain validation only needs a body-read lease for pc[-1]; the caller
** function discovered by the next frame owns the semantic prototype edge. */
static int gc2_frame_pc_valid_scoped(global_State *g, const BCIns *pc,
				      GC2MarkScope *scope)
{
  GCArena *a;
  GCproto *pt;
  GCobj *o;
  void *base = NULL;
  uint32_t gct = 0, start = 0;
  uintptr_t p, bc, end;
  size_t alloc_size = 0;
  gc2_mark_scope_init(scope);
  if (!g || !pc || !scope || !checkptrGC(pc))
    return 0;
  /* Custom lua_Alloc bodies are outside GC2 reclamation for the documented
  ** temporary b1.2 boundary. The owner-stable VM frame is sufficient there. */
  if (la_load32_acq(&g->allocf_arena) == 0)
    return 1;
  a = lj_arena_of(pc);
  if (gc2_small_arena_known(g, a)) {
    if (!gc2_small_containing_admit(g, a, lj_arena_cellof(pc),
				    &base, &start, scope))
      return 0;
    o = (GCobj *)base;
    if (!gc2_retained_candidate_valid(g, o, base, a, start, 0, 0,
				       &gct) ||
	gct != (uint32_t)~LJ_TPROTO)
      goto fail;
  } else {
    GC2HugeRegistryLease lease = GC2_HUGE_REGISTRY_NONE;
    TGState *tg;
    LJHugeInfo hi;
    int rc = LJ_ARENA_HUGE_READER_MISSING;
    if (!gc2_huge_registry_read_try(g, &lease))
      return 0;
    for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
      if (!lj_tg_flags_test_acq(tg, TGF_HUGETAB))
	continue;
      rc = lj_arena_hugetab_reader_range_acquire(
	&tg->huge, pc, &base, &scope->huge, &hi);
      if (rc != LJ_ARENA_HUGE_READER_MISSING)
	break;
    }
    if (rc == LJ_ARENA_HUGE_READER_MISSING) {
      tg = g->main_tg;
      if (tg && lj_tg_flags_test_acq(tg, TGF_HUGETAB))
	rc = lj_arena_hugetab_reader_range_acquire(
	  &tg->huge, pc, &base, &scope->huge, &hi);
    }
    gc2_huge_registry_read_leave(g, &lease);
    if (rc != LJ_ARENA_HUGE_READER_ACQUIRED)
      return 0;
    scope->admission = GC2_SCOPE_HUGE_READER;
    alloc_size = hi.size;
    o = (GCobj *)base;
    if (!base ||
	(hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) !=
	  (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY) ||
	(hi.flags & LJ_HUGEF_FREEING) ||
	!gc2_retained_candidate_valid(g, o, base, NULL, 0, alloc_size, 0,
				       &gct) ||
	gct != (uint32_t)~LJ_TPROTO)
      goto fail;
  }
  pt = gco2pt(o);
  if (!gc2_valid_proto_for_traverse_held(pt))
    goto fail;
  p = (uintptr_t)pc;
  bc = (uintptr_t)proto_bc(pt);
  end = bc + (uintptr_t)pt->sizebc * sizeof(BCIns);
  if (end < bc || p <= bc || p > end ||
      ((p - bc) % sizeof(BCIns)) != 0)
    goto fail;
  if (scope->admission == GC2_SCOPE_HUGE_READER &&
      !lj_arena_hugetab_reader_covers_range(&scope->huge, pc-1,
					     sizeof(BCIns)))
    goto fail;
  return 1;
fail:
  gc2_mark_scope_leave(scope);
  return 0;
}

int lj_gc2_mark_proto_for_pc(global_State *g, const BCIns *pc)
{
  GC2MarkScope scope, locscope;
  GCArena *a;
  GCobj *o;
  GCproto *pt;
  void *base = NULL;
  uint32_t gct = 0;
  int status, traversable = 0, match = 0;
  uintptr_t p, bc, end;
  if (!g || !pc || !checkptrGC(pc))
    return 0;
  /* C and fast-function snapshots encode synthetic bytecode PCs in permanent
  ** global storage. Their owner is the global allocation itself, not a
  ** GCproto discoverable through the arena registries. */
  p = (uintptr_t)(const void *)pc;
  if (p == (uintptr_t)(const void *)&g->bc_cfunc_int ||
      p == (uintptr_t)(const void *)&g->bc_cfunc_ext)
    return 1;
  bc = (uintptr_t)(const void *)&G2GG(g)->bcff[0];
  end = (uintptr_t)(const void *)&G2GG(g)->bcff[GG_NUM_ASMFF];
  if (p >= bc && p < end && ((p - bc) % sizeof(BCIns)) == 0)
    return 1;
  /* IDLE has no concurrent reclaiming cycle to close. Do not seed a future
  ** minor/major mark plane merely because a diagnostic caller resolves a PC. */
  if (gc2_phase_acq(g) == LJ_GC2_IDLE)
    return 1;
  /* Custom lua_Alloc storage is temporarily outside GC2 reclamation. Treat a
  ** well-formed pointer as retained without probing arena-aligned metadata;
  ** notes/lua-alloc-temporarily-disabled-2026-07-10.md documents this boundary.
  */
  if (la_load32_acq(&g->allocf_arena) == 0)
    return 1;
  a = lj_arena_of(pc);
  gc2_mark_scope_init(&locscope);
  if (gc2_small_arena_known(g, a)) {
    uint32_t cell = lj_arena_cellof(pc);
    if (!gc2_small_containing_admit(g, a, cell, &base, NULL, &locscope))
      return 0;
  } else {
    LJHugeInfo hi;
    int marked = gc2_mark_huge_range_scoped(g, pc, &base, &hi, &locscope);
    if (marked == LJ_ARENA_HUGE_READER_OVERFLOW ||
	marked == LJ_ARENA_HUGE_MARK_SATURATED) {
      /* No body token exists, so the interior PC cannot yet be resolved to an
      ** object descriptor. Reopen the current phase's exact root certificate;
      ** the retaining root retries without any speculative header read. */
      gc2_root_scan_retry(g);
      return 0;
    }
    if (marked == LJ_ARENA_HUGE_MARK_INTENT || marked < 0 || !base ||
	(hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) !=
	  (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY) ||
	(hi.flags & LJ_HUGEF_FREEING)) {
      gc2_mark_scope_leave(&locscope);
      return 0;
    }
    o = (GCobj *)base;
    if (!gc2_retained_candidate_valid(g, o, base, NULL, 0, hi.size, 0,
				       &gct) ||
	gct != (uint32_t)~LJ_TPROTO) {
      gc2_mark_scope_leave(&locscope);
      return 0;
    }
    status = marked > 0 ? GC2_MARK_NEW : GC2_MARK_LIVE_ALREADY;
    if (marked > 0)
      gc2_marks_this_round_add(g, 1);
    traversable = 1;
    scope = locscope;
    gc2_mark_scope_init(&locscope);
    goto validate_pc;
  }
  o = (GCobj *)base;
  gc2_mark_scope_init(&scope);
  status = gc2_retain_candidate_status(g, o, &base, &gct, &traversable,
					(uint32_t)~LJ_TPROTO, &scope, NULL);
  if (status == GC2_MARK_DEAD) {
    gc2_mark_scope_leave(&locscope);
    return 0;
  }
validate_pc:
  /* The counted small-arena lease (or huge mark ticket) precedes every proto
  ** byte read. Validate both the compact layout and the exact half-open
  ** bytecode interval before accepting the raw snapshot PC as an owner edge. */
  pt = gco2pt(o);
  if (base == (void *)o && gct == (uint32_t)~LJ_TPROTO &&
      gc2_valid_proto_for_traverse_held(pt)) {
    p = (uintptr_t)pc;
    bc = (uintptr_t)proto_bc(pt);
    end = bc + (uintptr_t)pt->sizebc * sizeof(BCIns);
    match = p >= bc && p < end && ((p - bc) % sizeof(BCIns)) == 0;
  }
  /* Admission may have conservatively marked the containing prototype before
  ** range validation. Discharge the graph mark even on a malformed PC so a
  ** later real edge never observes ALREADY and skips the prototype children. */
  if (gc2_gct_may_traverse(gct)) {
    uint32_t phase = gc2_phase_acq(g);
    if (((status == GC2_MARK_NEW &&
	  (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK)) ||
	 phase == LJ_GC2_SWEEP) && traversable)
      (void)gc2_publish_mutator_scoped(g, o, &scope);
  }
  gc2_mark_scope_leave(&scope);
  gc2_mark_scope_leave(&locscope);
  return match;
}

static int gc2_recovery_count_reserve(global_State *g)
{
  uint64_t old = gc2_recovery_items_add(g, 1);
  if (LJ_UNLIKELY(old == ~(uint64_t)0)) {
    /* More durable identities than addressable allocations is impossible.
    ** Keep reclamation vetoed if memory corruption violates that bound. */
    gc2_recovery_fail_closed(g);
    return 0;
  }
  return 1;
}

static void gc2_recovery_count_rollback(global_State *g)
{
  if (LJ_UNLIKELY(!gc2_recovery_items_dec(g))) {
    /* A losing publisher may roll back only its still-owned reservation.
    ** Refuse zero atomically in release builds: wrapping the aggregate would
    ** make every later close predicate report permanent work with no locator. */
    gc2_recovery_fail_closed(g);
    abort();
  }
}

static int gc2_recovery_huge_count_reserve(global_State *g)
{
  uint64_t old = gc2_recovery_huge_items_add(g, 1);
  if (LJ_UNLIKELY(old == ~(uint64_t)0)) {
    gc2_recovery_fail_closed(g);
    return 0;
  }
  return 1;
}

static void gc2_recovery_huge_count_rollback(global_State *g)
{
  if (LJ_UNLIKELY(!gc2_recovery_huge_items_dec(g))) {
    gc2_recovery_fail_closed(g);
    abort();
  }
}

#if defined(LJ_GC2_TEST_HELPERS)
void lj_gc2_test_recovery_count_rollback(global_State *g)
{
  gc2_recovery_count_rollback(g);
}

void lj_gc2_test_recovery_huge_count_rollback(global_State *g)
{
  gc2_recovery_huge_count_rollback(g);
}
#endif

static void gc2_recovery_publish_wake(global_State *g)
{
  gc2_recovery_published_add(g, 1);
  lj_gc2_worker_wake(g);
}

static int gc2_recovery_publish_main(global_State *g)
{
  for (;;) {
    uint32_t state = gc2_recovery_main_state_acq(g);
    if (state == LJ_ARENA_RECOVERY_PENDING ||
	state == LJ_ARENA_RECOVERY_REDIRTY) {
      lj_gc2_worker_wake(g);
      return 1;
    }
    if (state == LJ_ARENA_RECOVERY_CLAIMED) {
      uint32_t expect = state;
      if (gc2_recovery_main_state_cas(
	    g, &expect, LJ_ARENA_RECOVERY_REDIRTY)) {
	gc2_recovery_redirtied_add(g, 1);
	lj_gc2_worker_wake(g);
	return 1;
      }
      continue;
    }
    if (state != LJ_ARENA_RECOVERY_IDLE)
      return 0;
    if (!gc2_recovery_count_reserve(g))
      return 0;
    gc2_recovery_test_pause_at(LJ_GC2_RECOVERY_TEST_RESERVED);
    {
      uint32_t expect = LJ_ARENA_RECOVERY_IDLE;
      if (gc2_recovery_main_state_cas(
	    g, &expect, LJ_ARENA_RECOVERY_PENDING)) {
	gc2_recovery_publish_wake(g);
	return 1;
      }
    }
    gc2_recovery_count_rollback(g);
  }
}

static int gc2_recovery_small_lifetime_release(global_State *g, GCArena *a,
						uint32_t start,
						uint32_t origin,
						uint32_t held)
{
  uint32_t target = origin;
  if (origin == LJ_ARENA_LIFETIME_DESTRUCT) {
    target = LJ_ARENA_LIFETIME_LIVE;
  } else if (origin == LJ_ARENA_LIFETIME_CONSTRUCT &&
      lj_arena_root_state_acq(a, start) != LJ_ARENA_ROOT_LINKING)
    target = LJ_ARENA_LIFETIME_LIVE;
  if (LJ_LIKELY(lj_arena_lifetime_state_cas(a, start,
	held, target))) {
    if (target == LJ_ARENA_LIFETIME_CONSTRUCT &&
	lj_arena_root_state_acq(a, start) != LJ_ARENA_ROOT_LINKING) {
      /* Constructor commit/abandon may have won its LINKING transition after
      ** our first root sample. Repair the transient CONSTRUCT publication;
      ** the constructor performs the symmetric recheck if it wins later. */
      if (!lj_arena_lifetime_state_cas(a, start,
	    LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_LIVE) &&
	  lj_arena_lifetime_state_acq(a, start) !=
	    LJ_ARENA_LIFETIME_LIVE) {
	gc2_recovery_fail_closed(g);
	return 0;
      }
    }
    return 1;
  }
  /* A successful claim has one unique owner. Losing it is descriptor
  ** corruption (or an invalid concurrent terminal teardown), so retain every
  ** recovery identity instead of pretending that its body is still safe. */
  gc2_recovery_fail_closed(g);
  return 0;
}

static int gc2_recovery_small_exact_ready(GCArena *a, GCobj *o,
						   uint32_t start,
						   uint32_t held)
{
  uint32_t check;
  return a && o && (void *)o == lj_arena_cellptr(a, start) &&
    lj_arena_lifetime_state_acq(a, start) == held &&
    gc2_small_containing_start(a, lj_arena_cellof(o), &check) &&
    check == start && lj_arena_bm_get(a->block, start) &&
    lj_arena_ready_get(a, start);
}

/* The first recovery-side load may have sampled IDLE before a worker acquired
** an already-PENDING identity and published lifetime MUTATING. That lifetime
** acquire is ordered after the worker's PENDING observation, so a second side
** acquire sees durable work or its later completion. Completion restores the
** lifetime before publishing IDLE; the final lifetime acquire distinguishes
** that crossover from a stable, unrelated MUTATING owner with no identity. */
static int gc2_recovery_small_mutating_recheck(GCArena *a, uint32_t start)
{
  if (lj_arena_recovery_state_acq(a, start) != LJ_ARENA_RECOVERY_IDLE)
    return 1;
  return lj_arena_lifetime_state_acq(a, start) !=
	 LJ_ARENA_LIFETIME_MUTATING;
}

static int gc2_recovery_publish_small(global_State *g, GCArena *a,
					       GCobj *o, uint32_t start)
{
  for (;;) {
    uint32_t state = lj_arena_recovery_state_acq(a, start);
    if (state == LJ_ARENA_RECOVERY_PENDING ||
	state == LJ_ARENA_RECOVERY_REDIRTY) {
      lj_gc2_worker_wake(g);
      return 1;
    }
    if (state == LJ_ARENA_RECOVERY_CLAIMED) {
      if (lj_arena_recovery_state_cas(a, start, state,
					     LJ_ARENA_RECOVERY_REDIRTY)) {
	gc2_recovery_redirtied_add(g, 1);
	lj_gc2_worker_wake(g);
	return 1;
      }
      continue;
    }
    if (state != LJ_ARENA_RECOVERY_IDLE)
      return 0;
    gc2_recovery_test_pause_at(
	LJ_GC2_RECOVERY_TEST_SMALL_IDLE_SAMPLED);
    {
      uint32_t origin = lj_arena_lifetime_state_acq(a, start);
      uint32_t held;
      if (origin == LJ_ARENA_LIFETIME_RESCUE ||
	  origin == LJ_ARENA_LIFETIME_RECOVERY) {
	/* Another counted publisher already owns this exact allocation. RESCUE
	** cancelled DESTRUCT, while RECOVERY claimed LIVE/CONSTRUCT; in both cases
	** the reservation precedes the lifetime lane, so work is durable before
	** its recovery side state appears. MUTATING alone is not coalescible. */
	lj_gc2_worker_wake(g);
	return 1;
      }
      if (origin == LJ_ARENA_LIFETIME_LIVE ||
	  origin == LJ_ARENA_LIFETIME_CONSTRUCT)
	held = LJ_ARENA_LIFETIME_RECOVERY;
      else if (origin == LJ_ARENA_LIFETIME_DESTRUCT) {
	/* Irrevocable external/remote free publishes late before DESTRUCT.
	** Its intent supersedes traversal and must never be resurrected. A
	** late-clear DESTRUCT belongs to cancellable GC sweep. */
	if (lj_arena_late_get(a, start))
	  return 1;
	held = LJ_ARENA_LIFETIME_RESCUE;
      } else if (origin == LJ_ARENA_LIFETIME_MUTATING) {
	if (gc2_recovery_small_mutating_recheck(a, start))
	  continue;
	return 0;
      } else
	return 0;
      /* Reserve before claiming either lane. This closes MARK/WEAK/SWEEP
	** transition predicates across the descriptor-before-side-plane gap. */
      if (!gc2_recovery_count_reserve(g))
	return 0;
      if (!lj_arena_lifetime_state_cas(a, start, origin, held)) {
	uint32_t now;
	gc2_recovery_count_rollback(g);
	if (lj_arena_recovery_state_acq(a, start) !=
	    LJ_ARENA_RECOVERY_IDLE)
	  continue;
	now = lj_arena_lifetime_state_acq(a, start);
	if (now == LJ_ARENA_LIFETIME_RESCUE ||
	    now == LJ_ARENA_LIFETIME_RECOVERY ||
	    (now == LJ_ARENA_LIFETIME_DESTRUCT &&
	     lj_arena_late_get(a, start)))
	  return 1;
	if (now == LJ_ARENA_LIFETIME_LIVE ||
	    now == LJ_ARENA_LIFETIME_CONSTRUCT ||
	    now == LJ_ARENA_LIFETIME_DESTRUCT)
	  continue;
	return 0;
      }
      if (held == LJ_ARENA_LIFETIME_RESCUE &&
	  lj_arena_late_get(a, start)) {
	/* An irrevocable free published intent just before RESCUE won. Restore
	** ordinary lifetime, preserve late, and consume only our unpublished
	** reservation. No object or allocation-layout byte has been read. */
	(void)gc2_recovery_small_lifetime_release(
	  g, a, start, origin, held);
	gc2_recovery_count_rollback(g);
	return 1;
      }
      if (!gc2_recovery_small_exact_ready(a, o, start, held)) {
	(void)gc2_recovery_small_lifetime_release(
	  g, a, start, origin, held);
	gc2_recovery_count_rollback(g);
	return 0;
      }
      if (held == LJ_ARENA_LIFETIME_RESCUE &&
	  lj_arena_late_get(a, start)) {
	(void)gc2_recovery_small_lifetime_release(
	  g, a, start, origin, held);
	gc2_recovery_count_rollback(g);
	return 1;
      }
      gc2_recovery_test_pause_at(LJ_GC2_RECOVERY_TEST_RESERVED);
      if (lj_arena_recovery_state_cas(a, start,
	    LJ_ARENA_RECOVERY_IDLE, LJ_ARENA_RECOVERY_PENDING)) {
	gc2_recovery_test_pause_at(
	  LJ_GC2_RECOVERY_TEST_PRE_LIFETIME_RESTORE);
	(void)gc2_recovery_small_lifetime_release(
	  g, a, start, origin, held);
	/* PENDING is already durable even if descriptor corruption prevented
	** release. Account and wake it; fail-closed state prevents reclamation. */
	gc2_recovery_publish_wake(g);
	return 1;
      }
      gc2_recovery_count_rollback(g);
      if (!gc2_recovery_small_lifetime_release(
	    g, a, start, origin, held))
	return 0;
    }
  }
}

static int gc2_recovery_publish_huge(global_State *g, HugeTab *ht,
					      void *base)
{
  for (;;) {
    LJHugeInfo hi;
    int observed = lj_arena_hugetab_recovery_state_acq(ht, base, &hi);
    uint32_t state;
    if (observed < 0)
      return 0;  /* The mapping may have transferred; caller locates again. */
    state = (uint32_t)observed;
    if (state == LJ_ARENA_RECOVERY_PENDING ||
	state == LJ_ARENA_RECOVERY_REDIRTY) {
      lj_gc2_worker_wake(g);
      return 1;
    }
    if (state == LJ_ARENA_RECOVERY_CLAIMED) {
      if (lj_arena_hugetab_recovery_state_cas(
	    ht, base, state, LJ_ARENA_RECOVERY_REDIRTY, NULL)) {
	gc2_recovery_redirtied_add(g, 1);
	lj_gc2_worker_wake(g);
	return 1;
      }
      continue;
    }
    if (state != LJ_ARENA_RECOVERY_IDLE)
      return -1;
    if (hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_DEFER_FREE))
      return 1;  /* Terminal free already consumed this object's graph role. */
    if ((hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) !=
	(LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY) ||
	((hi.flags & LJ_HUGEF_BUSY) && !(hi.flags & LJ_HUGEF_SWEEP_OLD)))
      return -1;  /* Persistent ownership rejection: never spin on its owner. */
    if (!gc2_recovery_count_reserve(g))
      return -1;
    if (!gc2_recovery_huge_count_reserve(g)) {
      gc2_recovery_count_rollback(g);
      return -1;
    }
    gc2_recovery_test_pause_at(LJ_GC2_RECOVERY_TEST_RESERVED);
    if (lj_arena_hugetab_recovery_state_cas(
	  ht, base, state, LJ_ARENA_RECOVERY_PENDING, NULL)) {
      gc2_recovery_publish_wake(g);
      return 1;
    }
    gc2_recovery_huge_count_rollback(g);
    gc2_recovery_count_rollback(g);
  }
}

static int gc2_recovery_publish_scoped(global_State *g, GCobj *o,
				       const GC2MarkScope *scope)
{
  GCArena *a;
  uint32_t cell, start, attempt;
  lua_State *mainL;
  if (!g || !o)
    return 0;
  mainL = mainthread_acq(g);
  if (mainL && o == obj2gco(mainL))
    return gc2_recovery_publish_main(g);
  a = lj_arena_of(o);
  if (gc2_small_arena_known(g, a)) {
    cell = lj_arena_cellof(o);
    /* Lifetime is the sole initial locator. A DESTRUCT candidate remains
    ** byte-stable until its DESTRUCT->FREE commit and can be cancelled by the
    ** recovery publisher's exact DESTRUCT->RESCUE CAS. */
    if (!gc2_small_lifetime_nearest(a, cell, &start, NULL))
      return 0;
    return gc2_recovery_publish_small(g, a, o, start);
  }
  if (scope && scope->admission == GC2_SCOPE_HUGE_READER &&
      scope->huge.h && scope->huge.base == (void *)o &&
      lj_arena_hugetab_reader_covers(&scope->huge, o)) {
    HugeTab held = { scope->huge.h };
    /* The counted entry reader is stronger than TG-list SMR for this exact
    ** mapping: transfer, delete and table-header teardown all refuse it. Use
    ** that already-paid lifetime admission directly so an unrelated registry
    ** writer cannot turn a full-SSB fallback into sticky NO_RECLAIM. */
    return gc2_recovery_publish_huge(g, &held, scope->huge.base) > 0;
  }
  /* A marked huge mapping is its own lifetime admission. TG SMR keeps each
  ** candidate table alive while a concurrent dead-owner transfer advances. */
  for (attempt = 0; attempt < 4u; attempt++) {
    HugeTab *ht;
    LJHugeInfo hi;
    int published;
    void *base = NULL;
    if (!lj_gc2_smr_read_try(g))
      return 0;  /* Never wait behind a registry reclaimer on a barrier path. */
    ht = gc2_huge_recovery_table(g, o, &base, &hi);
    if (!ht || !base) {
      lj_gc2_smr_read_leave(g);
      return 0;
    }
    if (base != (void *)o ||
	(hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) !=
	  (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) {
      lj_gc2_smr_read_leave(g);
      return 0;
    }
    published = gc2_recovery_publish_huge(g, ht, base);
    if (published > 0) {
      lj_gc2_smr_read_leave(g);
      return 1;
    }
    lj_gc2_smr_read_leave(g);
    if (published < 0)
      return 0;
    /* IDLE transfer can move the exact mapping between the lookup and CAS.
    ** Transfer is lock-free and recovery-bearing entries no longer move, so a
    ** fresh registry pass eventually owns the publication. */
    la_cpu_pause();
  }
  return 0;
}

static int gc2_recovery_publish(global_State *g, GCobj *o)
{
  return gc2_recovery_publish_scoped(g, o, NULL);
}

static LJ_AINLINE int gc2_recovery_work_pending(global_State *g)
{
  return g && gc2_recovery_items_acq(g) != 0;
}

static LJ_AINLINE int gc2_recovery_failed_veto(global_State *g)
{
  return g && gc2_recovery_failed_acq(g) != 0;
}

static LJ_AINLINE int gc2_recovery_stalled_failed(global_State *g)
{
  return !gc2_recovery_work_pending(g) && gc2_recovery_failed_veto(g);
}

static LJ_AINLINE int gc2_recovery_empty(global_State *g)
{
  return !gc2_recovery_work_pending(g) && !gc2_recovery_failed_veto(g);
}

static void gc2_recovery_fail_closed(global_State *g)
{
  if (!g)
    return;
  gc2_recovery_failed_rel(g, 1);
  gc2_activation_pin_no_reclaim(g);
  lj_gc2_worker_wake(g);
}

/* Validate a candidate whose containing queue, root-spine, table snapshot or
** SMR scope already supplies the lifetime lease. Unlike the conservative
** validator above, this must never turn structural observation into semantic
** reachability. In particular, weak clearing and FINREG liveness tests depend
** on a white interior cdata remaining white after validation. */
static int gc2_observed_obj_status_scoped(global_State *g, GCobj *o,
					  uint32_t *gctp,
					  GC2MarkScope *scope)
{
  GCArena *a;
  uint32_t gct;
  void *base;
  gc2_mark_scope_init(scope);
  if (!g || !o || !checkptrGC(o) ||
      ((uintptr_t)o & (uintptr_t)(sizeof(void *) - 1u)) != 0)
    return 0;
  if (la_load32_acq(&g->allocf_arena) == 0) {
    if (gctp)
      *gctp = (uint32_t)la_load8_acq(&o->gch.gct);
    return 1;  /* Temporary custom-lua_Alloc compatibility boundary. */
  }
  a = lj_arena_of(o);
  if (gc2_small_arena_known(g, a)) {
    uint32_t start;
    GC2MarkScope local;
    GC2MarkScope *hold = scope ? scope : &local;
    int admitted = gc2_small_candidate_admit(g, o, a, 0, &base, &start,
						      &gct, hold);
    UNUSED(base); UNUSED(start);
    if (admitted <= 0)
      return admitted;
    if (!scope)
      gc2_mark_scope_leave(&local);
  } else {
    GC2MarkScope local;
    GC2MarkScope *hold = scope ? scope : &local;
    LJHugeInfo hi;
    int admitted;
    int interior_tag;
    base = NULL;
    admitted = gc2_huge_observed_scoped(g, o, &base, &hi, hold);
    if (admitted != LJ_ARENA_HUGE_READER_ACQUIRED)
      return admitted == LJ_ARENA_HUGE_READER_OVERFLOW ? -1 : 0;
    if ((hi.flags & LJ_HUGEF_FREEING) ||
	((hi.flags & LJ_HUGEF_BUSY) &&
	 (hi.flags & (LJ_HUGEF_TICKET|LJ_HUGEF_MARK)) !=
	   (LJ_HUGEF_TICKET|LJ_HUGEF_MARK)) ||
	(hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) !=
	  (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY))
      goto huge_invalid;
    interior_tag = o != (GCobj *)base;
    if ((interior_tag &&
	 (hi.flags & (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA)) !=
	   (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA)) ||
	(!interior_tag && (hi.flags & LJ_HUGEF_INTERIOR_CDATA)))
      goto huge_invalid;
    if (!gc2_retained_candidate_valid(g, o, base, NULL, 0, hi.size,
				       interior_tag, &gct))
      goto huge_invalid;
    if ((gct == (uint32_t)~LJ_TCDATA) !=
	((hi.flags & LJ_HUGEF_CDATA) != 0))
      goto huge_invalid;
    if (!scope)
      gc2_mark_scope_leave(&local);
    goto observed_valid;
huge_invalid:
    gc2_mark_scope_leave(hold);
    return 0;
  }
observed_valid:
  if (gctp)
    *gctp = gct;
  return 1;
}

static int gc2_observed_obj_valid_scoped(global_State *g, GCobj *o,
						 uint32_t *gctp,
						 GC2MarkScope *scope)
{
  return gc2_observed_obj_status_scoped(g, o, gctp, scope) > 0;
}

static int gc2_observed_obj_valid(global_State *g, GCobj *o)
{
  GC2MarkScope scope;
  int valid = gc2_observed_obj_valid_scoped(g, o, NULL, &scope);
  gc2_mark_scope_leave(&scope);
  return valid;
}

static int gc2_observed_obj_type(global_State *g, GCobj *o,
					  uint32_t expected_gct)
{
  GC2MarkScope scope;
  uint32_t gct = 0;
  int valid = gc2_observed_obj_valid_scoped(g, o, &gct, &scope) &&
    gct == expected_gct;
  gc2_mark_scope_leave(&scope);
  return valid;
}

int lj_gc2_obj_valid(global_State *g, GCobj *o)
{
  return gc2_observed_obj_valid(g, o);
}

static int gc2_queue_obj_info(global_State *g, GCobj *o,
			      GCArena *known_small,
			      LJGC2QueuedInfo *info, int full)
{
  GCArena *a;
  uint32_t gct;
  void *base;
  if (info)
    memset(info, 0, sizeof(*info));
  if (!g || !o || !checkptrGC(o) ||
      ((uintptr_t)o & (uintptr_t)(sizeof(void *) - 1u)) != 0)
    return 0;
  if (la_load32_acq(&g->allocf_arena) == 0)
    goto custom_valid;
  a = lj_arena_of(o);
  if ((known_small && a == known_small) || gc2_small_arena_known(g, a)) {
    uint32_t cell = lj_arena_cellof(o), start = cell;
    int interior_tag;
    /* Queue-valid callers already retain root MEMBER/UNLINKING, a post-grace
    ** RETIRED sweep ticket, or terminal ownership. Opening a rescue admission
    ** here would set PENDING on the sealed generation and defeat the very
    ** destructor whose ticket protects these reads. */
    if (!(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) ||
	(!lj_arena_bm_get(a->block, cell) &&
	 !gc2_small_containing_start(a, cell, &start)) ||
	!lj_arena_ready_get(a, start))
      return 0;
    base = lj_arena_cellptr(a, start);
    interior_tag = o != (GCobj *)base;
    if ((interior_tag && !lj_arena_cdata_get(a, start)) ||
	!gc2_retained_candidate_valid(g, o, base, a, start, 0,
				       interior_tag, &gct))
      return 0;
    if (info) {
      info->arena = a;
      info->base = base;
      info->start = start;
      info->gct = gct;
      info->marked = lj_arena_bm_get(a->mark, start);
      if (full) {
	uint32_t end;
	for (end = start + 1u; end < LJ_ARENA_CELLS; end++)
	  if (lj_arena_bm_get(a->block, end) ||
	      lj_arena_bm_get(a->mark, end))
	    break;
	info->alloc_size = (size_t)(end - start) << LJ_CELL_SHIFT;
	info->end = end;
      }
    }
  } else {
    LJHugeInfo hi;
    int interior_tag;
    base = NULL;
    if ((gc2_smr_reader_tls_active(g) || gc2_reclaim_tls_active(g)) ?
	!gc2_huge_observed_registry_held(g, o, &base, &hi) :
	!gc2_huge_observed(g, o, &base, &hi))
      return 0;
    if ((hi.flags & LJ_HUGEF_FREEING) ||
	((hi.flags & LJ_HUGEF_BUSY) &&
	 (hi.flags & (LJ_HUGEF_TICKET|LJ_HUGEF_MARK)) !=
	   (LJ_HUGEF_TICKET|LJ_HUGEF_MARK)) ||
	(hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) !=
	  (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY))
      return 0;
    interior_tag = o != (GCobj *)base;
    if ((interior_tag &&
	 (hi.flags & (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA)) !=
	   (LJ_HUGEF_CDATA|LJ_HUGEF_INTERIOR_CDATA)) ||
	(!interior_tag && (hi.flags & LJ_HUGEF_INTERIOR_CDATA)) ||
	!gc2_retained_candidate_valid(g, o, base, NULL, 0, hi.size,
				       interior_tag, &gct) ||
	((gct == (uint32_t)~LJ_TCDATA) !=
	 ((hi.flags & LJ_HUGEF_CDATA) != 0)))
      return 0;
    if (info) {
      info->arena = a;
      info->base = base;
      info->alloc_size = hi.size;
      info->gct = gct;
      info->marked = (hi.flags & LJ_HUGEF_MARK) != 0;
    }
  }
  return 1;
custom_valid:
  if (info) {
    info->base = o;
    info->gct = (uint32_t)la_load8_acq(&o->gch.gct);
    info->marked = 1;
  }
  return 1;
}

int lj_gc2_obj_valid_queued(global_State *g, GCobj *o)
{
  int valid;
  /* Queue/root ownership pins the object body, but not the TG and HugeTab
  ** registry used to prove its allocation identity. Exact reclaim owns the
  ** registry exclusively; every ordinary caller takes one tactical reader.
  ** A caller such as pending-root flush which already holds this universe's
  ** reader pays only the TLS nesting increment. */
  if (gc2_reclaim_tls_active(g))
    return gc2_queue_obj_info(g, o, NULL, NULL, 0);
  if (!lj_gc2_smr_read_try(g))
    return 0;
  valid = gc2_queue_obj_info(g, o, NULL, NULL, 0);
  lj_gc2_smr_read_leave(g);
  return valid;
}

int lj_gc2_obj_queued_info_held(global_State *g, GCobj *o,
					 void *known_arena,
					 LJGC2QueuedInfo *info)
{
  if (!g || !o || !info ||
      (!gc2_smr_reader_tls_active(g) && !gc2_reclaim_tls_active(g))) {
    if (info)
      memset(info, 0, sizeof(*info));
    return 0;
  }
  return gc2_queue_obj_info(g, o, (GCArena *)known_arena, info, 1);
}

int lj_gc2_obj_queued_brief_held(global_State *g, GCobj *o,
					  void *known_arena,
					  LJGC2QueuedInfo *info)
{
  if (!g || !o || !info ||
      (!gc2_smr_reader_tls_active(g) && !gc2_reclaim_tls_active(g))) {
    if (info)
      memset(info, 0, sizeof(*info));
    return 0;
  }
  return gc2_queue_obj_info(g, o, (GCArena *)known_arena, info, 0);
}

static uint64_t gc2_grey_repair_span(global_State *g, MSize cap,
				     uint64_t top, uint64_t bottom)
{
  if (cap != 0 && bottom > top && bottom - top > (uint64_t)cap) {
    /*
    ** Advancing top here used to discard the oldest semantic identities after
    ** cursor corruption. Recovery cannot reconstruct work which was already
    ** overwritten, so fail closed instead: NO_RECLAIM makes the generation a
    ** permanent veto and the unchanged span keeps the invariant violation
    ** visible to every owner. Correct push/grow arbitration never reaches this
    ** branch.
    */
    gc2_recovery_fail_closed(g);
  }
  return top;
}

static int gc2_grey_grow(global_State *g)
{
  GCRef *oldstack = gc2_grey_stack_acq(g);
  GCRef *newstack;
  MSize oldcap = gc2_grey_capacity_acq(g);
  MSize newcap = oldcap ? oldcap << 1 : GC2_GREY_INIT;
  uint64_t top = gc2_grey_top_acq(g);
  uint64_t bottom = gc2_grey_bottom_rlx(g);
  top = gc2_grey_repair_span(g, oldcap, top, bottom);
  MSize count = bottom > top ? (MSize)(bottom - top) : 0;
  lua_State *L = lj_tg_cur_L(g);
  if (!L) {
    L = mainthread_acq(g);
  }
  if (!L || oldcap >= GC2_GREY_LIMIT)
    return 0;
  if (newcap < oldcap || newcap > GC2_GREY_LIMIT)
    newcap = GC2_GREY_LIMIT;
  if (newcap <= oldcap || count > newcap)
    return 0;
#if defined(LJ_GC2_TEST_HELPERS)
  if (gc2_recovery_test_take_fail(&gc2_recovery_test_fail_grey_grow))
    return 0;
#endif
  /* Queue capacity is an optimization, never a correctness precondition.
  ** Growth must not throw through a worker/SSB consumer ownership scope; a
  ** failed grow is routed to the allocation-free recovery plane by callers. */
  newstack = (GCRef *)lj_mem_new_nothrow(
    L, (GCSize)((size_t)newcap * sizeof(GCRef)));
  if (!newstack)
    return 0;
  if (oldstack && oldcap) {
    MSize i;
    for (i = 0; i < count; i++) {
      GCobj *o = gc2_queue_slot_load_acq(&oldstack[(MSize)((top + i) % oldcap)]);
      if (o)
	gc2_queue_slot_store_rel(&newstack[i], o);
      else
	gc2_queue_slot_clear_rel(&newstack[i]);
    }
  }
  gc2_grey_stack_rel(g, newstack);
  gc2_grey_capacity_rel(g, newcap);
  gc2_grey_top_store_rlx(g, 0);
  gc2_grey_bottom_store_rlx(g, count);
  if (oldstack && oldstack != g->gc2.grey_embedded)
    lj_mem_freevec(g, oldstack, oldcap, GCRef);
  return 1;
}

static int gc2_grey_push(global_State *g, GCobj *o)
{
  GCRef *stack;
  uint64_t top, bottom;
  MSize cap;
  if (!g || !o)
    return 0;
  bottom = gc2_grey_bottom_rlx(g);
  top = gc2_grey_top_acq(g);
  cap = gc2_grey_capacity_acq(g);
  top = gc2_grey_repair_span(g, cap, top, bottom);
  if (top > bottom) {
    /*
    ** Chase-Lev indexes describe an empty deque when the steal side catches or
    ** passes the owner side. Normalize before owner push so unsigned capacity
    ** math cannot interpret the empty state as a full ring.
    */
    gc2_grey_bottom_rel(g, top);
    bottom = top;
  }
  if ((cap == 0 || bottom - top >= cap) && !gc2_grey_grow(g))
    return 0;
  bottom = gc2_grey_bottom_rlx(g);
  cap = gc2_grey_capacity_acq(g);
  stack = gc2_grey_stack_acq(g);
  if (!stack || cap == 0)
    return 0;
  gc2_queue_slot_store_rel(&stack[(MSize)(bottom % cap)], o);
  /* 05 section 5.6.3: slot release-published before bottom. */
  gc2_grey_bottom_rel(g, bottom + 1);
  gc2_grey_pushed_add(g, 1);
  return 1;
}

static int gc2_grey_empty(global_State *g)
{
  uint64_t top, bottom;
  MSize cap;
  if (!g)
    return 1;
  cap = gc2_grey_capacity_acq(g);
  top = gc2_grey_top_acq(g);
  bottom = gc2_grey_bottom_acq(g);
  top = gc2_grey_repair_span(g, cap, top, bottom);
  return top >= bottom;
}

static int gc2_weak_ensure(global_State *g)
{
  MSize cap;
  GCRef *stack;
  uint8_t *ready;
  if (!g)
    return 0;
  cap = gc2_weak_capacity_acq(g);
  stack = gc2_weak_stack_acq(g);
  ready = gc2_weak_ready_acq(g);
  if (stack && ready && cap > 0)
    return 1;
  return gc2_weak_resize(g, GC2_WEAK_INIT);
}

static MSize gc2_weak_next_capacity(MSize cap, uint64_t need)
{
  MSize n = cap ? cap : GC2_WEAK_INIT;
  if (n < GC2_WEAK_INIT)
    n = GC2_WEAK_INIT;
  if (need > (uint64_t)GC2_WEAK_LIMIT)
    need = (uint64_t)GC2_WEAK_LIMIT;
  while ((uint64_t)n < need && n < GC2_WEAK_LIMIT) {
    if (n > (GC2_WEAK_LIMIT >> 1)) {
      n = GC2_WEAK_LIMIT;
      break;
    }
    n <<= 1;
  }
  return n;
}

static int gc2_weak_resize(global_State *g, MSize cap)
{
  lua_State *L;
  GCRef *oldstack, *newstack;
  uint8_t *oldready, *newready;
  MSize oldcap;
  if (!g || cap == 0)
    return 0;
  L = mainthread_acq(g);
  if (!L)
    return 0;
  oldcap = gc2_weak_capacity_acq(g);
  oldstack = gc2_weak_stack_acq(g);
  oldready = gc2_weak_ready_acq(g);
  newstack = lj_mem_newvec(L, cap, GCRef);
  newready = lj_mem_newvec(L, cap, uint8_t);
  gc2_weak_stack_rel(g, newstack);
  gc2_weak_ready_rel(g, newready);
  gc2_weak_capacity_rel(g, cap);
  if (oldstack)
    lj_mem_freevec(g, oldstack, oldcap, GCRef);
  if (oldready)
    lj_mem_freevec(g, oldready, oldcap, uint8_t);
  return 1;
}

static void gc2_weak_overflow_free(global_State *g, GC2WeakOverflow *node)
{
  while (node && lj_gc2_mem_registered(g, node)) {
    GC2WeakOverflow *next = gc2_weak_overflow_next_acq(node);
    lj_mem_free(g, node, sizeof(*node));
    node = next;
  }
}

static int gc2_weak_overflow_push(global_State *g, GCtab *t)
{
  GC2WeakOverflow *node, *head;
  if (!g || !t)
    return 0;
  if (g->allocf == lj_arena_allocf) {
    node = (GC2WeakOverflow *)lj_arena_allocd_alloc(
      (LJArenaAllocD *)g->allocd, sizeof(*node), 0);
  } else {
    node = (GC2WeakOverflow *)g->allocf(g->allocd, NULL, 0, sizeof(*node));
  }
  if (!node)
    return 0;
  lj_assertG(checkptrGC(node),
	     "allocated memory address %p outside required range", node);
  lj_gc_total_add(g, sizeof(*node));
  gc2_weak_overflow_tab_rel(node, t);
  head = gc2_weak_overflow_acq(g);
  do {
    gc2_weak_overflow_next_rel(node, head);
    /*
    ** Weak snapshot overflow is a semantic slow path, not a warm mutator path.
    ** Allocate with the raw allocator so a parked GC2 worker never longjmps
    ** through a borrowed Lua stack. If allocation fails, the weak bridge
    ** still participates in completion and the overflow counter records the
    ** gap.
    */
  } while (!gc2_weak_overflow_cas(g, &head, node));
  return 1;
}

static void gc2_weak_reset(global_State *g)
{
  MSize i;
  MSize cap;
  uint8_t *ready;
  uint64_t prior_count;
  if (!g)
    return;
  prior_count = gc2_weak_count_acq(g);
  gc2_weak_overflow_free(g, gc2_weak_overflow_xchg_acqrel(g, NULL));
  (void)gc2_weak_ensure(g);
  cap = gc2_weak_capacity_acq(g);
  if (prior_count > (uint64_t)cap) {
    MSize ncap = gc2_weak_next_capacity(cap, prior_count);
    if (ncap > cap)
      (void)gc2_weak_resize(g, ncap);  /* 05 section 5.8 adaptive weak snapshot. */
  }
  cap = gc2_weak_capacity_acq(g);
  ready = gc2_weak_ready_acq(g);
  if (ready)
    for (i = 0; i < cap; i++)
      la_store8_rlx(&ready[i], 0);
  gc2_weak_count_store_rlx(g, 0);  /* 05 section 5.8 side vector. */
  gc2_weak_drain_active_store_rlx(g, 0);
  gc2_weak_mark_closed_store_rlx(g, 0);
  gc2_weak_root_scanned_store_rlx(g, 0);
  /*
  ** weak_write_active is a cross-thread in-flight counter, not phase-local
  ** scratch. Mutators may span a weak-vector reset while publishing protected
  ** weak entries, and the matching end must observe its begin increment.
  */
  gc2_weak_scan_cursor_store_rlx(g, 0);
  gc2_weak_clear_cursor_store_rlx(g, 0);
}

static void gc2_finclaim_publish(lua_State *L, global_State *g, MSize idx,
				 GCobj *o, cTValue *fin)
{
  GCRef *objv = gc2_finreg_cdata_preclaim_objvec_acq(g);
  TValue *finv = gc2_finreg_cdata_preclaim_finvec_acq(g);
  copyTVrel(L, &finv[idx], fin);
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  if (gc2_finreg_cdata_preclaim_publish_pause_xchg_acqrel(g, 0) != 0) {
    gc2_finreg_cdata_preclaim_publish_paused_rel(g, 1);
    while (gc2_finreg_cdata_preclaim_publish_release_acq(g) == 0)
      gc2_peer_wait_no_l();
    gc2_finreg_cdata_preclaim_publish_paused_rel(g, 0);
  }
#endif
  gc2_queue_slot_store_rel(&objv[idx], o);
  /* 05 section 5.8: finalizer value is visible before object ready marker. */
}

static void gc2_finclaim_clear(lua_State *L, global_State *g, MSize idx)
{
  GCRef *objv = gc2_finreg_cdata_preclaim_objvec_acq(g);
  TValue *finv = gc2_finreg_cdata_preclaim_finvec_acq(g);
  TValue nilv;
  gc2_queue_slot_clear_rel(&objv[idx]);
  setnilV(&nilv);
  copyTVrel(L, &finv[idx], &nilv);
}

static void gc2_finclaim_copy_slot(lua_State *L, GCRef *newobj, TValue *newfin,
				   MSize dst, GCRef *oldobj,
				   TValue *oldfin, MSize src)
{
  GCobj *o = gc2_queue_slot_load_acq(&oldobj[src]);
  if (o) {
    TValue fin;
    lj_tv_load_acq(&fin, &oldfin[src]);
    copyTVrel(L, &newfin[dst], &fin);
    gc2_queue_slot_store_rel(&newobj[dst], o);
  } else {
    TValue nilv;
    setnilV(&nilv);
    copyTVrel(L, &newfin[dst], &nilv);
    gc2_queue_slot_clear_rel(&newobj[dst]);
  }
}

static int gc2_finclaim_ensure(global_State *g)
{
  lua_State *L;
  GCRef *obj;
  TValue *fin;
  MSize cap;
  if (!g)
    return 0;
  cap = gc2_finreg_cdata_preclaim_capacity_acq(g);
  if (gc2_finreg_cdata_preclaim_ready(g))
    return cap == GC2_FINCLAIM_FIXED;
  if (gc2_finreg_cdata_preclaim_objvec_acq(g) ||
      gc2_finreg_cdata_preclaim_finvec_acq(g) || cap != 0)
    return 0;
  L = mainthread_acq(g);
  if (!L)
    return 0;
  obj = lj_mem_newvec(L, GC2_FINCLAIM_FIXED, GCRef);
  /* Keep the first raw allocation live across construction of its peer. */
  (void)lj_gc2_markmem(g, obj);
  fin = lj_mem_newvec(L, GC2_FINCLAIM_FIXED, TValue);
  (void)lj_gc2_markmem(g, obj);
  (void)lj_gc2_markmem(g, fin);
  gc2_finreg_cdata_preclaim_objvec_rel(g, obj);
  gc2_finreg_cdata_preclaim_finvec_rel(g, fin);
  gc2_finreg_cdata_preclaim_capacity_rel(g, GC2_FINCLAIM_FIXED);
  gc2_finreg_cdata_preclaim_head_rel(g, 0);
  gc2_finreg_cdata_preclaim_count_rel(g, 0);
  /* Close construction marks versus a cycle that clears them immediately
  ** before the global pointer/capacity publication above. */
  (void)lj_gc2_markmem(g, obj);
  (void)lj_gc2_markmem(g, fin);
  return 1;  /* Fixed preclaim vector: no active-GC migration/free. */
}

static int gc2_finclaim_prepare(global_State *g)
{
  return gc2_finreg_cdata_preclaim_ready(g) || gc2_finclaim_ensure(g);
}

static void gc2_finclaim_reset(global_State *g)
{
  lua_State *L;
  GCRef *obj;
  TValue *fin;
  MSize head, count, pending, cap, i;
  if (!g)
    return;
  L = mainthread_acq(g);
  if (!L)
    return;
  obj = gc2_finreg_cdata_preclaim_objvec_acq(g);
  fin = gc2_finreg_cdata_preclaim_finvec_acq(g);
  cap = gc2_finreg_cdata_preclaim_capacity_acq(g);
  head = gc2_finreg_cdata_preclaim_head_acq(g);
  count = gc2_finreg_cdata_preclaim_count_acq(g);
  pending = count > head ? count - head : 0;
  if (!obj || !fin || cap == 0) {
    (void)gc2_finclaim_ensure(g);
    return;
  }
  if (head != 0 && pending != 0) {
    for (i = 0; i < pending; i++)
      gc2_finclaim_copy_slot(L, obj, fin, i, obj, fin, head + i);
  }
  gc2_finreg_cdata_preclaim_head_rel(g, 0);
  gc2_finreg_cdata_preclaim_count_rel(g, pending);
}

static void gc2_weak_record(global_State *g, GCtab *t)
{
  MSize cap;
  GCRef *stack;
  uint8_t *ready;
  uint64_t idx;
  if (!g || !t) {
    if (g)
      gc2_weak_tables_overflow_add(g, 1);
    return;
  }
  cap = gc2_weak_capacity_acq(g);
  stack = gc2_weak_stack_acq(g);
  ready = gc2_weak_ready_acq(g);
  if (!stack || !ready || cap == 0) {
    (void)gc2_weak_overflow_push(g, t);
    gc2_weak_tables_overflow_add(g, 1);
    return;
  }
  idx = gc2_weak_count_add(g, 1);  /* 05 section 5.8 MPSC slot. */
  if (idx < (uint64_t)cap) {
    gc2_queue_slot_store_rel(&stack[(MSize)idx], obj2gco(t));
    /* 05 section 5.8: publish weak snapshot slot before ready byte. */
    la_store8_rel(&ready[(MSize)idx], 1);
    gc2_weak_tables_queued_add(g, 1);
  } else {
    (void)gc2_weak_overflow_push(g, t);
    gc2_weak_tables_overflow_add(g, 1);
  }
}

static uint32_t lj_gc2_weak_snapshot_count(global_State *g)
{
  uint64_t reserved, count;
  MSize cap;
  GCRef *stack;
  uint8_t *ready;
  if (!g)
    return 0;
  cap = gc2_weak_capacity_acq(g);
  stack = gc2_weak_stack_acq(g);
  ready = gc2_weak_ready_acq(g);
  if (!stack || !ready)
    return 0;
  reserved = gc2_weak_count_acq(g);
  if (reserved > (uint64_t)cap)
    reserved = (uint64_t)cap;
  for (count = 0; count < reserved; count++)
    if (la_load8_acq(&ready[(MSize)count]) == 0)
      break;
  return count > ~(uint32_t)0 ? ~(uint32_t)0 : (uint32_t)count;
}

enum {
  GC2_TAB_SCOPE_STALE = 0,
  GC2_TAB_SCOPE_VALID = 1,
  GC2_TAB_SCOPE_RETRY = 2
};

/* Preserve semantic expected-type marking while retaining the one useful
** distinction hidden by generic GC2_MARK_DEAD: a small-arena reader can lose
** admission transiently before inspecting any body bytes. This is used for
** both a weak table and its __mode string, so neither identity needs a wait or
** a body read after its lease has ended. */
static int gc2_expected_type_scoped_status(global_State *g, GCobj *o,
					    uint32_t expected_gct,
					    GCobj **admittedp,
					    GC2MarkScope *scope,
					    int semantic_publish)
{
  GCArena *a;
  void *base;
  uint32_t start, gct;
  int admitted, status, markretry = 0;
  if (admittedp)
    *admittedp = NULL;
  gc2_mark_scope_init(scope);
  if (!g || !o || !scope || !checkptrGC(o) ||
      ((uintptr_t)o & (uintptr_t)(sizeof(void *) - 1u)) != 0)
    return GC2_TAB_SCOPE_STALE;
  if (la_load32_acq(&g->allocf_arena) != 0) {
    a = lj_arena_of(o);
    if (gc2_small_arena_known(g, a)) {
      admitted = gc2_small_candidate_admit(
	g, o, a, expected_gct, &base, &start, &gct, scope);
      UNUSED(base); UNUSED(gct);
      if (admitted < 0)
	return GC2_TAB_SCOPE_RETRY;
      if (admitted == 0)
	return GC2_TAB_SCOPE_STALE;
      status = gc2_mark_small_cell_admitted(
	g, a, start, scope->admission, &markretry);
      if (status == GC2_MARK_DEAD) {
	if (markretry) {
	  uint32_t phase;
	  gc2_queue_retry_witness_test_pause_at(o);
	  phase = gc2_phase_acq(g);
	  /* The mark CAS may already have won before a transient lifetime owner
	  ** made final validation return RETRY. Publish exact graph work while the
	  ** counted scope still pins this identity; otherwise a later retry can see
	  ** LIVE_ALREADY and incorrectly treat the undispatched mark as complete. */
	  if (semantic_publish && (phase == LJ_GC2_MARK ||
		phase == LJ_GC2_WEAK || phase == LJ_GC2_SWEEP))
	    (void)gc2_publish_mutator_scoped(g, o, scope);
	}
	gc2_mark_scope_leave(scope);
	return markretry ? GC2_TAB_SCOPE_RETRY : GC2_TAB_SCOPE_STALE;
      }
      gc2_preserve_direct_bodies(g, o);
      {
	uint32_t phase = gc2_phase_acq(g);
	if (semantic_publish &&
	    ((status == GC2_MARK_NEW &&
	     (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK)) ||
	    phase == LJ_GC2_SWEEP))
	  (void)gc2_publish_mutator_scoped(g, o, scope);
      }
      if (admittedp)
	*admittedp = o;
      return GC2_TAB_SCOPE_VALID;
    }
  }
  /* Huge expected-type marking currently supplies the durable MARK lifetime
  ** LP and the generic helper publishes NEW graph work. Custom lua_Alloc
  ** follows its documented temporary compatibility boundary. */
  status = gc2_markobj_expected_scoped_status_mode(
    g, o, expected_gct, NULL, scope, semantic_publish);
  if (status == GC2_MARK_DEAD) {
    gc2_mark_scope_leave(scope);
    return GC2_TAB_SCOPE_STALE;
  }
  if (admittedp)
    *admittedp = o;
  return GC2_TAB_SCOPE_VALID;
}

static int gc2_expected_tab_scoped_status(global_State *g, GCobj *o,
					   GCtab **tabp,
					   GC2MarkScope *scope)
{
  GCobj *admitted = NULL;
  int status = gc2_expected_type_scoped_status(
    g, o, (uint32_t)~LJ_TTAB, &admitted, scope, 1);
  if (tabp)
    *tabp = admitted ? gco2tab(admitted) : NULL;
  return status;
}

static int gc2_expected_tab_worker_scoped_status(global_State *g, GCobj *o,
						   GCtab **tabp,
						   GC2MarkScope *scope)
{
  GCobj *admitted = NULL;
  int status = gc2_expected_type_scoped_status(
    g, o, (uint32_t)~LJ_TTAB, &admitted, scope, 0);
  if (tabp)
    *tabp = admitted ? gco2tab(admitted) : NULL;
  return status;
}

static int gc2_expected_string_scoped_status(global_State *g, GCobj *o,
					      GCstr **strp,
					      GC2MarkScope *scope)
{
  GCobj *admitted = NULL;
  int status = gc2_expected_type_scoped_status(
    g, o, (uint32_t)~LJ_TSTR, &admitted, scope, 1);
  if (strp)
    *strp = admitted ? gco2str(admitted) : NULL;
  return status;
}

static int gc2_expected_string_worker_scoped_status(global_State *g,
						      GCobj *o,
						      GCstr **strp,
						      GC2MarkScope *scope)
{
  GCobj *admitted = NULL;
  int status = gc2_expected_type_scoped_status(
    g, o, (uint32_t)~LJ_TSTR, &admitted, scope, 0);
  if (strp)
    *strp = admitted ? gco2str(admitted) : NULL;
  return status;
}

/* Weak vectors and the legacy weak bridge contain tables already discovered
** through semantic graph traversal. Re-establish the exact expected-TAB mark
** here: small arenas transfer a counted body scope, while huge MARK is the
** durable lifetime LP. A newly rescued stale-queue table is published by the
** expected-status helper before any caller scans its graph. */
static int gc2_weak_candidate_tab_scoped_status(global_State *g, GCobj *o,
						 GCtab **tabp,
						 GC2MarkScope *scope)
{
  return gc2_expected_tab_scoped_status(g, o, tabp, scope);
}

static GCtab *gc2_weak_candidate_tab_scoped(global_State *g, GCobj *o,
					     GC2MarkScope *scope)
{
  GCtab *t = NULL;
  (void)gc2_weak_candidate_tab_scoped_status(g, o, &t, scope);
  return t;
}

static int lj_gc2_weak_snapshot_tab_scoped_status(global_State *g,
						   uint32_t idx, GCtab **tabp,
						   GC2MarkScope *scope)
{
  GCobj *o;
  GCRef *stack;
  if (tabp)
    *tabp = NULL;
  gc2_mark_scope_init(scope);
  if (!g || !scope || idx >= lj_gc2_weak_snapshot_count(g))
    return 0;
  stack = gc2_weak_stack_acq(g);
  if (!stack)
    return 0;
  o = gc2_queue_slot_load_acq(&stack[idx]);
  return gc2_weak_candidate_tab_scoped_status(g, o, tabp, scope);
}

static GCtab *lj_gc2_weak_snapshot_tab_scoped(global_State *g, uint32_t idx,
					       GC2MarkScope *scope)
{
  GCtab *t = NULL;
  (void)lj_gc2_weak_snapshot_tab_scoped_status(g, idx, &t, scope);
  return t;
}

/* Test-only identity snapshots never dereference the returned pointer. */
static GCtab *lj_gc2_weak_snapshot_tab(global_State *g, uint32_t idx)
{
  GC2MarkScope scope;
  GCtab *t = lj_gc2_weak_snapshot_tab_scoped(g, idx, &scope);
  gc2_mark_scope_leave(&scope);
  return t;
}

/* Keep the current table admitted until its acquired gclist successor has its
** own exact admission. This closes the load/release/reuse gap in weak-bridge
** walks without turning either structural link into a semantic mark. */
static int gc2_weak_bridge_handoff(global_State *g, GCtab *t,
				    GC2MarkScope *scope, GCtab **nextp,
				    GC2MarkScope *nextscope)
{
  GCobj *o = lj_tab_gclist_acq(t);
  GCtab *next = NULL;
  gc2_mark_scope_init(nextscope);
  if (o && !(next = gc2_weak_candidate_tab_scoped(g, o, nextscope))) {
    gc2_mark_scope_leave(scope);
    return 0;
  }
  gc2_mark_scope_leave(scope);
  *nextp = next;
  return 1;
}

enum {
  GC2_WEAK_RETRY = -1,
  GC2_WEAK_KEEP = 0,
  GC2_WEAK_CLEAR = 1
};

static int gc2_weak_classify(global_State *g, cTValue *o, int val,
			     int markstr)
{
  GC2MarkScope scope;
  int admitted, marked;
  int result = GC2_WEAK_KEEP;

  /*
  ** Weak processing scans racy table snapshots. A terminal tag/header mismatch
  ** is a stale slot and therefore clearable. An admission failure which may be
  ** transient must replay the table instead of becoming an irreversible clear.
  */
  admitted = gc2_tv_admit_scoped(g, o, &scope);
  if (LJ_UNLIKELY(admitted == GC2_TV_SCOPE_RETRY)) {
    gc2_mark_scope_leave(&scope);
    return GC2_WEAK_RETRY;
  }
  if (LJ_UNLIKELY(admitted == GC2_TV_SCOPE_STALE)) {
    gc2_mark_scope_leave(&scope);
    return GC2_WEAK_CLEAR;
  }
  if (tvisgcv(o)) {
    if (tvisstr(o)) {
      if (gcV(o) != obj2gco(&g->strempty) && markstr &&
	  lj_gc2_markobj_status(g, gcV(o), NULL) == GC2_MARK_DEAD)
	result = GC2_WEAK_RETRY;
      goto out;  /* 05 section 5.8: strings are not weak-cleared. */
    }
    marked = lj_gc2_ismarked(g, gcV(o));
    if (LJ_UNLIKELY(marked < 0)) {
	result = GC2_WEAK_RETRY;
	goto out;
    }
    if (marked == 0) {
	  /*
	  ** GC2-owned weak completion closes roots, SSBs, grey work and weak-write
	  ** windows before clearing. At that point the GC2 mark bitmap is the completed
	  ** liveness oracle for this pass. Color color can be stale in this fork
	  ** because GC2/SMR may preserve bodies without making them semantic roots;
	  ** the color fallback path is the only code that should use colors.
	  */
	  result = GC2_WEAK_CLEAR;
	  goto out;
    }
    /* GC2 late weak write mark wins over color white during GCSatomic. */
    if (tvisudata(o) && val &&
	(lj_obj_gcflags(obj2gco(udataV(o))) & LJ_GC_FINALIZED))
      result = GC2_WEAK_CLEAR;
  }
out:
  /* Keep the exact incarnation pinned through type, string mark, liveness and
  ** finalized-state observations. */
  gc2_mark_scope_leave(&scope);
  return result;
}

static int gc2_tab_is_ffi_fin(global_State *g, GCtab *t)
{
#if LJ_HASFFI
  return lj_ctype_fin_istab(g, t);
#else
  UNUSED(g); UNUSED(t);
  return 0;
#endif
}

/* Caller retains an exact TAB scope for the full call. The SMR interval keeps
** separated array/node generations stable through their final slot read. */
static int gc2_weak_process_tab(global_State *g, GCtab *t,
				GC2MarkScope *tabscope, int clear,
				uint64_t *slots, uint64_t *clearable)
{
  int weak;
  int result = 0;
  UNUSED(tabscope);  /* The retained scope is a lifetime contract. */
  if (!g || !t || !tabscope || !lj_gc2_smr_read_try(g))
    return 0;
  /* Individual key/value scopes may end after classification because the
  ** keyed CAS consumes only captured TValue bits. This is safe only while the
  ** exact table scope and table SMR retain the slot generation and WEAK stays
  ** stable, before SWEEP can reclaim or reuse any captured object identity. */
  weak = lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK;
  if (!weak)
    goto success;
  if (weak & LJ_GC_WEAKVAL) {
    TValue *array;
    MSize i, asize, acap;
    if (lj_tab_array_snapshot_gc_held(g, t, &array, &asize, &acap) !=
	LJ_TAB_GC_SNAPSHOT_OK)
      goto out;
    UNUSED(acap);
    for (i = 0; i < asize; i++) {
      TValue val;
      TValue *slot = &array[i];
      lj_tv_load_acq(&val, &array[i]);
      /* FORWARD is an in-progress generation handoff, not an absent weak
      ** entry. Retry from a fresh table snapshot on a later bounded turn. */
      if (LJ_UNLIKELY(tvisforward(&val)))
	goto out;
      if (!tvisnil(&val)) {
	int valclass;
	(*slots)++;
	valclass = gc2_weak_classify(g, &val, 1, clear);
	if (LJ_UNLIKELY(valclass == GC2_WEAK_RETRY))
	  goto out;
	if (valclass == GC2_WEAK_CLEAR) {
	  (*clearable)++;
	  if (clear) {
	    TValue key;
	    setintV(&key, (int32_t)i);
	    (void)lj_tab_clear_weak_slot_keyed(t, slot, &key, &val);
	  }
	}
      }
    }
  }
  {
    MSize i, hmask;
    Node *node;
    if (lj_tab_node_snapshot_gc_held(g, t, &node, &hmask) !=
	LJ_TAB_GC_SNAPSHOT_OK)
      goto out;
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      TValue key, val;
      TValue *slot = &n->val;
      int key_loaded = 0;
      int keyclass, valclass;
      lj_tv_load_acq(&val, &n->val);
      if (tvisforward(&val)) {
	lj_tv_load_acq(&key, &n->key);
	key_loaded = 1;
	/* A descheduled structural writer may retain KEYLOCK indefinitely. Leave
	** this item/cursor unclaimed so a later bounded worker turn retries it. */
	if (LJ_UNLIKELY(tviskeylock(&key)))
	  goto out;
	if (tvisnil(&key))
	  continue;
	/* The successor may not be published yet. Never enter the generic mutator
	** resolver, which is allowed to wait for its structural writer. */
	goto out;
      }
      if (!tvisnil(&val)) {
	if (!key_loaded)
	  lj_tv_load_acq(&key, &n->key);
	if (LJ_UNLIKELY(tviskeylock(&key)))
	  goto out;
	(*slots)++;
	/* Observe both halves explicitly. In particular, a clearable key must not
	** short-circuit a transient value admission into an irreversible clear. */
	keyclass = gc2_weak_classify(g, &key, 0, clear);
	if (LJ_UNLIKELY(keyclass == GC2_WEAK_RETRY))
	  goto out;
	/* A clearable key makes value classification a RETRY veto only. Do not
	** mark a string value for an entry which this pass will remove. */
	valclass = gc2_weak_classify(
	  g, &val, 1, clear && keyclass == GC2_WEAK_KEEP);
	if (LJ_UNLIKELY(valclass == GC2_WEAK_RETRY))
	  goto out;
	if (keyclass == GC2_WEAK_CLEAR || valclass == GC2_WEAK_CLEAR) {
	  (*clearable)++;
	  if (clear)
	    (void)lj_tab_clear_weak_slot_keyed(t, slot, &key, &val);
	}
      }
    }
  }
success:
  result = 1;
out:
  lj_gc2_smr_read_leave(g);
  return result;
}

#if LJ_GC2_PARANOIA
static void gc2_weak_paranoia_zero_diff(global_State *g, GCobj *bridge_head)
{
  uint64_t tables = 0, slots = 0, clearable = 0;
  GC2MarkScope scope;
  GCtab *t = bridge_head ?
    gc2_weak_candidate_tab_scoped(g, bridge_head, &scope) : NULL;
  while (bridge_head) {
    GC2MarkScope nextscope;
    GCtab *next;
    if (!t) {
      fprintf(stderr, "GC2 weak paranoia: non-table bridge weak node %p\n",
	      (void *)bridge_head);
      abort();
    }
    if (!gc2_weak_process_tab(g, t, &scope, 0, &slots, &clearable)) {
      gc2_mark_scope_leave(&scope);
      fprintf(stderr, "GC2 weak paranoia: unstable bridge weak table %p\n",
	      (void *)t);
      abort();
    }
    tables++;
    if (!gc2_weak_bridge_handoff(g, t, &scope, &next, &nextscope)) {
      fprintf(stderr, "GC2 weak paranoia: invalid bridge successor %p\n",
	      (void *)t);
      abort();
    }
    t = next;
    scope = nextscope;
    bridge_head = next ? obj2gco(next) : NULL;
  }
  if (clearable != 0) {
    fprintf(stderr, "GC2 weak paranoia: %llu/%llu clearable weak slots "
	    "after GC2 skip across %llu tables\n",
	    (unsigned long long)clearable, (unsigned long long)slots,
	    (unsigned long long)tables);
    abort();
  }
}
#endif

static uint32_t lj_gc2_weak_snapshot_scan(global_State *g, uint32_t limit)
{
  uint32_t claimed = 0, n, scanned = 0;
  uint64_t slots = 0, clearable = 0;
  if (!g || limit == 0)
    return 0;
  n = lj_gc2_weak_snapshot_count(g);
  /* Inspect one slot before publishing its cursor advance. A transient table
  ** or SMR admission failure therefore remains retryable instead of turning a
  ** reserved-but-unscanned slot into apparent progress. Concurrent scanners
  ** may duplicate read-only work; only the cursor-CAS winner accounts it. */
  while (claimed < limit) {
    GC2MarkScope scope;
    uint64_t start = gc2_weak_scan_cursor_acq(g);
    uint64_t end = start + 1u;
    uint64_t item_slots = 0, item_clearable = 0;
    int status;
    GCtab *t;
    if (start >= (uint64_t)n)
      break;
    status = lj_gc2_weak_snapshot_tab_scoped_status(
      g, (uint32_t)start, &t, &scope);
    if (!t) {
      if (status == GC2_TAB_SCOPE_RETRY)
	break;
      if (gc2_weak_scan_cursor_cas(g, &start, end))
	claimed++;
      continue;
    }
    if (!gc2_weak_process_tab(g, t, &scope, 0, &item_slots,
			      &item_clearable)) {
      gc2_mark_scope_leave(&scope);
      break;
    }
    gc2_mark_scope_leave(&scope);
    if (gc2_weak_scan_cursor_cas(g, &start, end)) {
      claimed++;
      scanned++;
      slots += item_slots;
      clearable += item_clearable;
    }
  }
  if (scanned) {
    gc2_weak_scan_runs_add(g, 1);
    gc2_weak_scan_tables_add(g, scanned);
    gc2_weak_scan_slots_add(g, slots);
    gc2_weak_scan_clearable_add(g, clearable);
  }
  return scanned;
}

static uint32_t lj_gc2_weak_snapshot_clear(global_State *g, uint32_t limit)
{
  uint32_t claimed = 0, n, scanned = 0;
  uint64_t slots = 0, cleared = 0;
  if (!g || limit == 0 || !gc2_recovery_empty(g))
    return 0;
  if (gc2_weak_write_active_acq(g) != 0)
    return 0;
  gc2_weak_drain_active_add(g, 1);
  /* Recovery reserves its global count before publishing the exact arena
  ** identity. Recheck after advertising the clear owner so a reservation gap
  ** or sticky fail-closed state can never advance this irreversible cursor. */
  if (gc2_weak_write_active_acq(g) != 0 || !gc2_recovery_empty(g)) {
    uint32_t old = gc2_weak_drain_active_sub(g, 1);
    lj_assertG(old != 0, "gc2 weak drain active underflow");
    UNUSED(old);
    return 0;
  }
  n = lj_gc2_weak_snapshot_count(g);
  /* Clear before advancing the cursor. Weak-slot CAS makes duplicate work by
  ** competing drainers harmless, while a transient vector snapshot leaves the
  ** same index visible for a later retry. */
  while (claimed < limit) {
    GC2MarkScope scope;
    uint64_t start = gc2_weak_clear_cursor_acq(g);
    uint64_t end = start + 1u;
    uint64_t item_slots = 0, item_cleared = 0;
    int status;
    GCtab *t;
    if (start >= (uint64_t)n)
      break;
    status = lj_gc2_weak_snapshot_tab_scoped_status(
      g, (uint32_t)start, &t, &scope);
    if (!t) {
      if (status == GC2_TAB_SCOPE_RETRY)
	break;
      if (gc2_weak_clear_cursor_cas(g, &start, end))
	claimed++;
      continue;
    }
    if (!gc2_weak_process_tab(g, t, &scope, 1, &item_slots,
			      &item_cleared)) {
      gc2_mark_scope_leave(&scope);
      break;
    }
    gc2_mark_scope_leave(&scope);
    if (gc2_weak_clear_cursor_cas(g, &start, end)) {
      claimed++;
      scanned++;
      slots += item_slots;
      cleared += item_cleared;
    }
  }
  if (scanned) {
    gc2_weak_clear_runs_add(g, 1);
    gc2_weak_clear_tables_add(g, scanned);
    gc2_weak_clear_slots_add(g, slots);
    gc2_weak_clear_cleared_add(g, cleared);
  }
  {
    uint32_t old = gc2_weak_drain_active_sub(g, 1);
    lj_assertG(old != 0, "gc2 weak drain active underflow");
    UNUSED(old);
  }
  return scanned;
}

static uint32_t lj_gc2_weak_drain(global_State *g, uint32_t limit)
{
  if (!g || limit == 0 || gc2_phase_acq(g) != LJ_GC2_WEAK ||
      !gc2_weak_mark_closed_acq(g))
    return 0;
  return lj_gc2_weak_snapshot_clear(g, limit);
}

/* WEAK closure owns worker_active, so exclude that self-token while checking
** for producers which can still invalidate its snapshot. */
static int gc2_weak_owned_peer_active(global_State *g)
{
  return g && (gc2_assist_active_acq(g) != 0 ||
	       gc2_weak_drain_active_acq(g) != 0 ||
	       gc2_weak_write_active_acq(g) != 0 ||
	       gc2_ssb_consumer_active_acq(g) != 0);
}

static uint32_t gc2_mark_drain_owned_bounded(global_State *g, uint32_t limit)
{
  uint32_t total = 0;
  while (g && total < limit) {
    TGState *tg = G2TG(g);
    uint32_t moved, active = 0, grey, recovery, step;
    moved = gc2_drain_published_ssb_to_grey(g, limit - total);
    total += moved;
    if (total < limit && tg && !lj_tg_flags_test_acq(tg, TGF_DEAD)) {
      active = gc2_drain_active_ssb_to_grey(g, tg, limit - total);
      total += active;
    }
    grey = total < limit ? gc2_drain_grey(g, limit - total) : 0;
    total += grey;
    recovery = total < limit ?
      gc2_recovery_drain_owned(g, limit - total) : 0;
    total += recovery;
    step = moved + active + grey + recovery;
    if (step == 0 || gc2_thread_scan_needscan_pending_acq(g) != 0)
      break;
  }
  return total;
}

static void gc2_weak_root_reopen(global_State *g)
{
  uint32_t expect = 1;
  (void)gc2_weak_root_scanned_cas(g, &expect, 0);
}

static void gc2_weak_root_snapshot_abort(global_State *g)
{
  uint32_t expect = 2;
  (void)gc2_weak_root_scanned_cas(g, &expect, 0);
}

static int gc2_weak_snapshot_complete(global_State *g, uint32_t *pn)
{
  uint64_t reserved, cleared;
  MSize cap;
  uint32_t n;
  GCRef *stack;
  uint8_t *ready;
  if (!g)
    return 0;
  if (gc2_phase_acq(g) != LJ_GC2_WEAK)
    return 0;
  cap = gc2_weak_capacity_acq(g);
  stack = gc2_weak_stack_acq(g);
  ready = gc2_weak_ready_acq(g);
  if (!stack || !ready) {
    if (pn)
      *pn = 0;
    return gc2_weak_count_acq(g) == 0;
  }
  reserved = gc2_weak_count_acq(g);
  if (reserved > (uint64_t)cap)
    return 0;  /* Overflowed snapshots are handled by the owner-clear bridge. */
  n = lj_gc2_weak_snapshot_count(g);
  if ((uint64_t)n != reserved)
    return 0;  /* Reserved slots must all be published in the ready prefix. */
  cleared = gc2_weak_clear_cursor_acq(g);
  if (cleared < (uint64_t)n)
    return 0;  /* The bounded GC2 clear cursor has not drained the prefix. */
  if (gc2_weak_drain_active_acq(g) != 0)
    return 0;  /* Cursor reservation alone is not completed weak clearing. */
  if (gc2_weak_write_active_acq(g) != 0)
    return 0;  /* Mutator stores can still publish a protected weak entry. */
  if (pn)
    *pn = n;
  return 1;
}

static int gc2_weak_snapshot_has_tab(global_State *g, GCtab *t, uint32_t n)
{
  GCRef *stack = gc2_weak_stack_acq(g);
  uint32_t i;
  if (!stack)
    return 0;
  for (i = 0; i < n; i++) {
    GCobj *o = gc2_queue_slot_load_acq(&stack[i]);
    if (o == obj2gco(t))
      return 1;
  }
  return 0;
}

static int lj_gc2_weak_snapshot_covers_bridge(global_State *g,
					      GCobj *bridge_head)
{
  GC2MarkScope scope;
  GCtab *t;
  uint32_t n;
  uint64_t bridge_count = 0;
  if (!gc2_weak_snapshot_complete(g, &n))
    return 0;
  t = bridge_head ?
    gc2_weak_candidate_tab_scoped(g, bridge_head, &scope) : NULL;
  while (bridge_head) {
    GC2MarkScope nextscope;
    GCtab *next;
    int found = 0;
    uint8_t flags;
    if (!t)
      return 0;
    flags = lj_obj_gcflags(obj2gco(t));
    if ((flags & LJ_GC_WEAK) == 0) {
      gc2_mark_scope_leave(&scope);
      return 0;
    }
    if (bridge_count >= (uint64_t)n) {
      gc2_mark_scope_leave(&scope);
      return 0;  /* Conservative guard against duplicates/corruption. */
    }
    bridge_count++;
    found = gc2_weak_snapshot_has_tab(g, t, n);
    if (!found) {
      gc2_mark_scope_leave(&scope);
      return 0;
    }
    if (!gc2_weak_bridge_handoff(g, t, &scope, &next, &nextscope))
      return 0;
    t = next;
    scope = nextscope;
    bridge_head = next ? obj2gco(next) : NULL;
  }
  return 1;  /* 05 section 5.8: GC2-cleared snapshot covers bridge weak list. */
}

static int gc2_weak_bridge_trace_tab(global_State *g, GCtab *t)
{
  if (!g || !t)
    return 0;
  /*
  ** A table can reach the weak list through the color marker before it
  ** is recorded in GC2's weak snapshot. Bridge clearing still uses GC2 mark bits
  ** as its oracle, so first trace the table's strong edges into GC2. This keeps
  ** weak-table metatables and __index chains live without forcing the color
  ** fallback for ordinary bridge gaps.
  */
  /* The bridge walker retains the exact table scope across this call. Avoid a
  ** redundant fresh admission which can transient-fail despite that proof. */
  if (gc2_traverse_tab_admitted(g, t, 0) == 0)
    return 0;
  (void)gc2_mark_drain_owned_bounded(g, ~(uint32_t)0);
  return gc2_thread_scan_needscan_pending_acq(g) == 0 &&
	 gc2_table_rescan_pending_acq(g) == 0 && lj_gc2_ssb_empty(g);
}

static int gc2_weak_backfill_bridge(global_State *g, GCobj *bridge_head)
{
  GC2MarkScope scope;
  GCtab *t;
  uint32_t n;
  uint64_t tables = 0, slots = 0, cleared = 0;
  if (!gc2_weak_snapshot_complete(g, &n))
    return 0;
  t = bridge_head ?
    gc2_weak_candidate_tab_scoped(g, bridge_head, &scope) : NULL;
  while (bridge_head) {
    GC2MarkScope nextscope;
    GCtab *next;
    uint8_t flags;
    if (!t)
      return 0;
    flags = lj_obj_gcflags(obj2gco(t));
    if ((flags & LJ_GC_WEAK) == 0) {
      gc2_mark_scope_leave(&scope);
      return 0;
    }
    if (!gc2_weak_snapshot_has_tab(g, t, n)) {
      if (!gc2_weak_bridge_trace_tab(g, t) ||
	  !gc2_weak_process_tab(g, t, &scope, 1, &slots, &cleared)) {
	gc2_mark_scope_leave(&scope);
	return 0;
      }
      tables++;
    }
    if (!gc2_weak_bridge_handoff(g, t, &scope, &next, &nextscope))
      return 0;
    t = next;
    scope = nextscope;
    bridge_head = next ? obj2gco(next) : NULL;
  }
  if (tables) {
    gc2_weak_bridge_backfills_add(g, 1);
    gc2_weak_bridge_backfill_tables_add(g, tables);
    gc2_weak_bridge_backfill_slots_add(g, slots);
    gc2_weak_bridge_backfill_cleared_add(g, cleared);
  }
  return 1;  /* 05 section 5.8: owner-cleared bridge weak snapshot gaps. */
}

static int gc2_weak_clear_overflow(global_State *g, uint64_t *tablesp,
				   uint64_t *slotsp, uint64_t *clearedp)
{
  GC2WeakOverflow *node;
  uint64_t tables = 0, slots = 0, cleared = 0;
  for (node = gc2_weak_overflow_acq(g); node != NULL; ) {
    GC2MarkScope nodescope, tabscope;
    GC2WeakOverflow *next;
    GCtab *t, *admitted = NULL;
    int tabstatus;
    if (gc2_markmem_registered_scoped_status(g, node, &nodescope) ==
	GC2_MARK_DEAD) {
      gc2_mark_scope_leave(&nodescope);
      return 0;
    }
    next = gc2_weak_overflow_next_acq(node);
    t = gc2_weak_overflow_tab_acq(node);
    gc2_mark_scope_init(&tabscope);
    tabstatus = t ? gc2_weak_candidate_tab_scoped_status(
	g, obj2gco(t), &admitted, &tabscope) : GC2_TAB_SCOPE_STALE;
    if (tabstatus != GC2_TAB_SCOPE_VALID || admitted != t) {
      gc2_mark_scope_leave(&tabscope);
      gc2_mark_scope_leave(&nodescope);
      return 0;
    }
    if (!gc2_weak_process_tab(g, t, &tabscope, 1, &slots, &cleared)) {
      gc2_mark_scope_leave(&tabscope);
      gc2_mark_scope_leave(&nodescope);
      return 0;
    }
    gc2_mark_scope_leave(&tabscope);
    gc2_mark_scope_leave(&nodescope);
    tables++;
    node = next;
  }
  if (tablesp)
    *tablesp += tables;
  if (slotsp)
    *slotsp += slots;
  if (clearedp)
    *clearedp += cleared;
  return 1;
}

static int gc2_weak_overflow_clear_bridge(global_State *g, GCobj *bridge_head)
{
  GC2MarkScope scope;
  GCtab *t;
  uint64_t reserved, tables = 0, slots = 0, cleared = 0;
  MSize cap;
  GCRef *stack;
  uint8_t *ready;
  if (!g || gc2_phase_acq(g) != LJ_GC2_WEAK)
    return 0;
  cap = gc2_weak_capacity_acq(g);
  stack = gc2_weak_stack_acq(g);
  ready = gc2_weak_ready_acq(g);
  if ((!stack || !ready) && gc2_weak_overflow_acq(g) == NULL)
    return 0;
  reserved = gc2_weak_count_acq(g);
  if (stack && ready && reserved <= (uint64_t)cap &&
      gc2_weak_overflow_acq(g) == NULL)
    return 0;
  /*
  ** The bounded vector remains the common case. Overflow nodes are the GC2
  ** ownership record for tables that did not fit it; the weak list is
  ** only an additional bridge source because stale colors can keep
  ** reachable weak tables out of that list.
  */
  if (!gc2_weak_clear_overflow(g, &tables, &slots, &cleared))
    return 0;
  t = bridge_head ?
    gc2_weak_candidate_tab_scoped(g, bridge_head, &scope) : NULL;
  while (bridge_head) {
    GC2MarkScope nextscope;
    GCtab *next;
    uint8_t flags;
    if (!t)
      return 0;
    flags = lj_obj_gcflags(obj2gco(t));
    if ((flags & LJ_GC_WEAK) == 0) {
      gc2_mark_scope_leave(&scope);
      return 0;
    }
    if (!gc2_weak_bridge_trace_tab(g, t) ||
	!gc2_weak_process_tab(g, t, &scope, 1, &slots, &cleared)) {
      gc2_mark_scope_leave(&scope);
      return 0;
    }
    tables++;
    if (!gc2_weak_bridge_handoff(g, t, &scope, &next, &nextscope))
      return 0;
    t = next;
    scope = nextscope;
    bridge_head = next ? obj2gco(next) : NULL;
  }
  if (tables) {
    gc2_weak_bridge_backfills_add(g, 1);
    gc2_weak_bridge_backfill_tables_add(g, tables);
    gc2_weak_bridge_backfill_slots_add(g, slots);
    gc2_weak_bridge_backfill_cleared_add(g, cleared);
  }
  return 1;  /* 05 section 5.8: overflowed weak snapshots stay GC2-owned. */
}

static void lj_gc2_weak_bridge_result(global_State *g, int skipped)
{
  if (!g)
    return;
  if (skipped)
    gc2_weak_bridge_skipped_add(g, 1);
  else
    gc2_weak_bridge_fallbacks_add(g, 1);
}

uint32_t lj_gc2_test_weak_snapshot_count(global_State *g)
{
  return lj_gc2_weak_snapshot_count(g);
}

GCtab *lj_gc2_test_weak_snapshot_tab(global_State *g, uint32_t idx)
{
  return lj_gc2_weak_snapshot_tab(g, idx);
}

uint32_t lj_gc2_test_weak_snapshot_scan(global_State *g, uint32_t limit)
{
  return lj_gc2_weak_snapshot_scan(g, limit);
}

uint32_t lj_gc2_test_weak_snapshot_clear(global_State *g, uint32_t limit)
{
  return lj_gc2_weak_snapshot_clear(g, limit);
}

uint32_t lj_gc2_test_weak_drain(global_State *g, uint32_t limit)
{
  return lj_gc2_weak_drain(g, limit);
}

int lj_gc2_test_weak_snapshot_covers_bridge(global_State *g,
					    GCobj *bridge_head)
{
  return lj_gc2_weak_snapshot_covers_bridge(g, bridge_head);
}

#if defined(LJ_GC2_TEST_HELPERS)
int lj_gc2_test_weak_overflow_clear_bridge(global_State *g,
					    GCobj *bridge_head)
{
  return gc2_weak_overflow_clear_bridge(g, bridge_head);
}
#endif

static int gc2_weak_trace_table_strong(global_State *g, GCtab *t)
{
  /* The caller's exact table scope bridges into traverse_tab_rec(), which
  ** takes an SMR interval for all payload reads without redundant admission. */
  if (!g || gc2_phase_acq(g) != LJ_GC2_WEAK || !t)
    return 0;
  return gc2_traverse_tab_admitted(g, t, 0) != 0;
}

/* Overflow records are raw allocations, so each link must acquire its own
** exact body scope while the predecessor is still admitted. A failed raw
** identity is never an end-of-list sentinel: closure must replay instead. */
static int gc2_weak_frontier_overflow_admit(global_State *g,
					     GC2WeakOverflow *node,
					     GC2MarkScope *scope)
{
  int status;
  gc2_mark_scope_init(scope);
  if (!node)
    return 1;
#if defined(LJ_GC2_TEST_HELPERS)
  if (gc2_test_weak_frontier_take_fault(
	LJ_GC2_WEAK_FRONTIER_FAULT_OVERFLOW_NODE))
    return 0;
#endif
  status = gc2_markmem_registered_scoped_status(g, node, scope);
  if (status == GC2_MARK_DEAD) {
    gc2_mark_scope_leave(scope);
    return 0;
  }
  return 1;
}

static int gc2_weak_trace_close_frontier(global_State *g, GCobj *bridge_head)
{
  GC2WeakOverflow *node;
  GC2MarkScope scope, nodescope;
  GCtab *t;
  uint32_t i, n;
  if (!g || gc2_phase_acq(g) != LJ_GC2_WEAK)
    return 0;
  n = lj_gc2_weak_snapshot_count(g);
  for (i = 0; i < n; i++) {
    int status;
#if defined(LJ_GC2_TEST_HELPERS)
    if (gc2_test_weak_frontier_take_fault(
	  LJ_GC2_WEAK_FRONTIER_FAULT_VECTOR_TAB)) {
      gc2_mark_scope_init(&scope);
      t = NULL;
      status = GC2_TAB_SCOPE_RETRY;
    } else
#endif
    status = lj_gc2_weak_snapshot_tab_scoped_status(
      g, i, &t, &scope);
    if (status == GC2_TAB_SCOPE_RETRY) {
      gc2_mark_scope_leave(&scope);
      return 0;
    }
    if (status == GC2_TAB_SCOPE_STALE) {
      /* A terminal stale vector slot has no live table incarnation left to
      ** contribute a strong edge; explicitly discharge it and continue. */
      gc2_mark_scope_leave(&scope);
      continue;
    }
    if (status != GC2_TAB_SCOPE_VALID || !t) {
      gc2_mark_scope_leave(&scope);
      return 0;
    }
    {
      int traced = gc2_weak_trace_table_strong(g, t);
      gc2_mark_scope_leave(&scope);
      if (!traced)
	return 0;
    }
  }
  node = gc2_weak_overflow_acq(g);
  if (!gc2_weak_frontier_overflow_admit(g, node, &nodescope))
    return 0;
  while (node != NULL) {
    GC2MarkScope nextscope;
    GC2WeakOverflow *next;
    GCtab *admitted = NULL;
    int status;
    t = gc2_weak_overflow_tab_acq(node);
    if (!t) {
      gc2_mark_scope_leave(&nodescope);
      return 0;
    }
#if defined(LJ_GC2_TEST_HELPERS)
    if (gc2_test_weak_frontier_take_fault(
	  LJ_GC2_WEAK_FRONTIER_FAULT_OVERFLOW_TAB)) {
      gc2_mark_scope_init(&scope);
      status = GC2_TAB_SCOPE_RETRY;
    } else
#endif
    status = gc2_weak_candidate_tab_scoped_status(
      g, obj2gco(t), &admitted, &scope);
    if (status != GC2_TAB_SCOPE_VALID || admitted != t) {
      gc2_mark_scope_leave(&scope);
      gc2_mark_scope_leave(&nodescope);
      return 0;
    }
    {
      int traced = gc2_weak_trace_table_strong(g, t);
      gc2_mark_scope_leave(&scope);
      if (!traced) {
	gc2_mark_scope_leave(&nodescope);
	return 0;
      }
    }
    next = gc2_weak_overflow_next_acq(node);
    if (!gc2_weak_frontier_overflow_admit(g, next, &nextscope)) {
      gc2_mark_scope_leave(&nodescope);
      return 0;
    }
    gc2_mark_scope_leave(&nodescope);
    node = next;
    nodescope = nextscope;
  }
  t = bridge_head ?
    gc2_weak_candidate_tab_scoped(g, bridge_head, &scope) : NULL;
  while (bridge_head) {
    GC2MarkScope nextscope;
    GCtab *next;
    if (!t)
      return 0;
    if ((lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK) == 0) {
      gc2_mark_scope_leave(&scope);
      return 0;
    }
    if (!gc2_weak_trace_table_strong(g, t)) {
      gc2_mark_scope_leave(&scope);
      return 0;
    }
    if (!gc2_weak_bridge_handoff(g, t, &scope, &next, &nextscope))
      return 0;
    t = next;
    scope = nextscope;
    bridge_head = next ? obj2gco(next) : NULL;
  }
  return 1;
}

#if defined(LJ_GC2_TEST_HELPERS)
int lj_gc2_test_weak_trace_close_frontier(global_State *g,
					   GCobj *bridge_head)
{
  return gc2_weak_trace_close_frontier(g, bridge_head);
}
#endif

static int gc2_weak_mark_close_round(global_State *g, lua_State *L,
				     GCobj *bridge_head,
				     uint32_t drain_limit)
{
  int finqueued;
  uint32_t cycle;
  if (!g || drain_limit == 0 || gc2_phase_acq(g) != LJ_GC2_WEAK)
    return 0;
  lj_assertG(gc2_worker_active_acq(g) != 0,
	     "weak close without worker ownership");
  if (gc2_weak_mark_closed_acq(g))
    return 1;
  if (lj_tg_any_jit_active(g))
    return 0;
  /* Take exactly one root snapshot, then finish its bounded work across later
  ** calls. Repeating the handshake after every partial drain manufactures a
  ** fresh executable-container rescan and can never reach closure. */
  if (gc2_weak_root_scanned_acq(g) != 1) {
    uint32_t expect = 0;
    if (!gc2_weak_root_scanned_cas(g, &expect, 2))
      return 0;  /* One peer owns the snapshot handshake/pre-drain. */
    cycle = gc2_cycle_acq(g);
    (void)gc2_marks_this_round_xchg_acqrel(g, 0);
    if (gc2_mark_drain_owned_bounded(g, drain_limit) != 0) {
      gc2_weak_root_snapshot_abort(g);
      return 0;
    }
    if (!gc2_ssb_published_empty(g) || !gc2_grey_empty(g) ||
	!gc2_recovery_empty(g)) {
      gc2_weak_root_snapshot_abort(g);
      return 0;
    }
    (void)lj_gc2_handshake(
      g, LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_FLUSH_SSB);
    if (gc2_phase_acq(g) != LJ_GC2_WEAK ||
	gc2_cycle_acq(g) != cycle) {
      gc2_weak_root_snapshot_abort(g);
      return 0;
    }
    expect = 2;
    if (!gc2_weak_root_scanned_cas(g, &expect, 1))
      return 0;
    return 0;
  }

  /* Weak clearing uses GC2 mark bits as the liveness oracle, so finish every
  ** edge created by that snapshot before any slot is nilled. */
  if (gc2_mark_drain_owned_bounded(g, drain_limit) != 0)
    return 0;
  if (!lj_gc2_ssb_empty(g)) {
    gc2_weak_root_reopen(g);
    return 0;
  }
  if (gc2_weak_owned_peer_active(g))
    return 0;
  if (gc2_thread_scan_needscan_pending_acq(g) != 0) {
    gc2_weak_root_reopen(g);
    return 0;
  }
  if (gc2_table_rescan_pending_acq(g) != 0)
    return 0;
  if (gc2_marks_this_round_xchg_acqrel(g, 0) != 0)
    return 0;
  /*
  ** Weak tables found earlier in the cycle may be reprocessed here without
  ** adding duplicate weak-vector entries. This closes strong header edges such
  ** as metatable and __index chains before weak-value observers consult marks.
  */
  if (!gc2_weak_trace_close_frontier(g, bridge_head))
    return 0;
  (void)gc2_mark_drain_owned_bounded(g, drain_limit);
  if (!lj_gc2_ssb_empty(g)) {
    gc2_weak_root_reopen(g);
    return 0;
  }
  if (gc2_weak_owned_peer_active(g) ||
      gc2_thread_scan_needscan_pending_acq(g) != 0 ||
      gc2_table_rescan_pending_acq(g) != 0 ||
      gc2_marks_this_round_acq(g) != 0)
    return 0;
  finqueued = lj_gc2_finreg_udata_finalize(g, 0) != 0;
#if LJ_HASFFI
  if (lj_gc2_finreg_cdata_finalize_pweak(L, g, gc2_finreg_marktv))
    finqueued = 1;
#endif
  if (finqueued) {
    (void)gc2_flush_ssb(g, G2TG(g), 0);
    (void)gc2_mark_drain_owned_bounded(g, drain_limit);
    if (gc2_weak_owned_peer_active(g) ||
	gc2_thread_scan_needscan_pending_acq(g) != 0 ||
	gc2_table_rescan_pending_acq(g) != 0 ||
	!lj_gc2_ssb_empty(g) ||
	gc2_marks_this_round_acq(g) != 0)
      return 0;
  }
  if (gc2_phase_acq(g) != LJ_GC2_WEAK ||
      gc2_weak_root_scanned_acq(g) != 1)
    return 0;
  gc2_weak_mark_closed_rel(g, 1);
  return 1;
}

static int gc2_weak_frontier_still_closed(global_State *g, lua_State *L,
					  uint32_t drain_limit)
{
  UNUSED(L);
  if (!g || drain_limit == 0 || gc2_phase_acq(g) != LJ_GC2_WEAK)
    return 0;
  lj_assertG(gc2_worker_active_acq(g) != 0,
	     "weak frontier validation without worker ownership");
  if (lj_tg_any_jit_active(g))
    return 0;
  if (gc2_mark_drain_owned_bounded(g, drain_limit) != 0)
    return 0;
  if (!lj_gc2_ssb_empty(g)) {
    gc2_weak_root_reopen(g);
    return 0;
  }
  if (gc2_weak_owned_peer_active(g) ||
      gc2_thread_scan_needscan_pending_acq(g) != 0 ||
      gc2_table_rescan_pending_acq(g) != 0 ||
      gc2_marks_this_round_acq(g) != 0)
    return 0;
  return gc2_weak_root_scanned_acq(g) == 1;
}

int lj_gc2_weak_complete(global_State *g, lua_State *L, GCobj *bridge_head,
			 uint32_t drain_limit)
{
  uint32_t weakdrain;
  uint64_t progress = 0;
  int complete = 0;
  if (!g || drain_limit == 0 || gc2_phase_acq(g) != LJ_GC2_WEAK)
    return 0;
  if (lj_tg_any_jit_active(g))
    return 0;
  if (!gc2_worker_claim_count_busy(g))
    return 0;
  if (gc2_phase_acq(g) != LJ_GC2_WEAK || lj_tg_any_jit_active(g))
    goto out;
  gc2_weak_complete_runs_add(g, 1);
  if (!gc2_weak_mark_closed_acq(g)) {
    if (lj_tg_any_jit_active(g))
      goto out;
    if (gc2_weak_mark_close_round(g, L, bridge_head, drain_limit)) {
      /* Continue below with one bounded weak drain. */
    } else {
      goto out;
    }
  }
  if (lj_tg_any_jit_active(g))
    goto out;
  weakdrain = lj_gc2_weak_snapshot_clear(g, drain_limit);
  if (weakdrain) {
    progress += (uint64_t)weakdrain;
    gc2_weak_complete_progress_add(g, progress);
    goto out;
  }
  if (gc2_weak_owned_peer_active(g))
    goto out;
  if (progress)
    gc2_weak_complete_progress_add(g, progress);
  if (lj_tg_any_jit_active(g))
    goto out;
  if (!gc2_weak_frontier_still_closed(g, L, drain_limit)) {
    gc2_weak_mark_closed_rel(g, 0);
    goto out;
  }
  if (lj_gc2_weak_snapshot_covers_bridge(g, bridge_head)) {
#if LJ_GC2_PARANOIA
    gc2_weak_paranoia_zero_diff(g, bridge_head);
#endif
    lj_gc2_weak_bridge_result(g, 1);
    complete = 1;  /* 05 section 5.8 scheduler-owned weak completion bridge. */
    goto out;
  }
  if (gc2_weak_overflow_clear_bridge(g, bridge_head)) {
#if LJ_GC2_PARANOIA
    gc2_weak_paranoia_zero_diff(g, bridge_head);
#endif
    lj_gc2_weak_bridge_result(g, 1);
    complete = 1;  /* 05 section 5.8 owner-cleared overflow bridge. */
    goto out;
  }
  if (gc2_weak_backfill_bridge(g, bridge_head)) {
#if LJ_GC2_PARANOIA
    gc2_weak_paranoia_zero_diff(g, bridge_head);
#endif
    lj_gc2_weak_bridge_result(g, 1);
    complete = 1;  /* 05 section 5.8 owner-cleared bridge weak gaps. */
    goto out;
  }
  lj_gc2_weak_bridge_result(g, 0);
out:
  if (complete &&
      (gc2_phase_acq(g) != LJ_GC2_WEAK ||
       lj_tg_any_jit_active(g) ||
       gc2_weak_root_scanned_acq(g) != 1 ||
       !gc2_weak_mark_closed_acq(g) ||
       gc2_weak_owned_peer_active(g) || !lj_gc2_ssb_empty(g) ||
       gc2_thread_scan_needscan_pending_acq(g) != 0 ||
       gc2_table_rescan_pending_acq(g) != 0 ||
       gc2_marks_this_round_acq(g) != 0))
    complete = 0;
  gc2_worker_release(g);
  return complete;  /* 05 section 5.8 conditional bridge result. */
}

#if LJ_HASFFI
static LJ_AINLINE int gc2_finreg_cdata_obj_valid(global_State *g, GCobj *o)
{
  return gc2_observed_obj_type(g, o, (uint32_t)~LJ_TCDATA);
}
#endif

static LJ_AINLINE int gc2_finreg_udata_obj_valid(global_State *g, GCobj *o)
{
  return g && o && gc2_observed_obj_type(g, o, (uint32_t)~LJ_TUDATA);
}

void lj_gc2_finreg_cdata_set(global_State *g, GCobj *o, int enabled)
{
#if LJ_HASFFI
  if (!gc2_finreg_cdata_obj_valid(g, o))
    return;
  if (enabled) {
    gc2_finreg_cdata_sets_add(g, 1);
  } else {
    gc2_finreg_cdata_clears_add(g, 1);
  }
#else
  UNUSED(g); UNUSED(o); UNUSED(enabled);
#endif
}

void lj_gc2_finreg_cdata_note_sweep_queued(global_State *g)
{
#if LJ_HASFFI
  if (g)
    gc2_finreg_cdata_sweep_queued_add(g, 1);
#else
  UNUSED(g);
#endif
}

void lj_gc2_finreg_cdata_note_order_retired(global_State *g)
{
#if LJ_HASFFI
  if (g)
    gc2_finreg_cdata_order_retired_add(g, 1);
#else
  UNUSED(g);
#endif
}

static void gc2_finreg_queue_mark(global_State *g, GCobj *o)
{
  uint32_t phase;
  if (!g || !o)
    return;
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK)
    (void)lj_gc2_markobj(g, o);  /* 05 section 5.8 FINREG resurrection. */
  else if (phase == LJ_GC2_SWEEP)
    (void)lj_gc2_preserve_sweep_root(g, o);
}

static void gc2_finreg_cdata_finalizer_enqueue_admitted(global_State *g,
							 GCobj *o,
							 cTValue *fin)
{
#if LJ_HASFFI
  /* The FINREG walker retains this exact cdata incarnation through slot
  ** consumption and queue publication. Do not re-enter observational
  ** admission after unlinking the root: a transient rescue/huge-reader miss
  ** there would otherwise consume the finalizer and silently drop its
  ** callback. */
  markfinalized(o);
  gc2_finreg_queue_mark(g, o);
  gc2_finreg_cdata_queued_add(g, 1);
  lj_gc2_finalizer_enqueue_fin(g, o, fin);
#else
  UNUSED(g); UNUSED(o); UNUSED(fin);
#endif
}

static void lj_gc2_finreg_cdata_finalizer_enqueue(global_State *g, GCobj *o,
						  cTValue *fin)
{
#if LJ_HASFFI
  if (!gc2_finreg_cdata_obj_valid(g, o))
    return;
  gc2_finreg_cdata_finalizer_enqueue_admitted(g, o, fin);
#else
  UNUSED(g); UNUSED(o); UNUSED(fin);
#endif
}

#if LJ_HASFFI
enum {
  GC2_FINREG_CANDIDATE_RETRY = -1,
  GC2_FINREG_CANDIDATE_NO = 0,
  GC2_FINREG_CANDIDATE_YES = 1
};

static int gc2_finreg_cdata_candidate_pweak(global_State *g, GCobj *o)
{
  int marked;
  if (o->gch.gct != ~LJ_TCDATA ||
      !(lj_obj_gcflags(o) & LJ_GC_CDATA_FIN) ||
      (lj_obj_gcflags(o) & LJ_GC_FINALIZED))
    return GC2_FINREG_CANDIDATE_NO;
  marked = lj_gc2_ismarked(g, o);
  if (marked < 0)
    return GC2_FINREG_CANDIDATE_RETRY;
  return marked == 0 ? GC2_FINREG_CANDIDATE_YES :
		       GC2_FINREG_CANDIDATE_NO;
}

static int gc2_finreg_cdata_candidate_close(GCobj *o)
{
  return o->gch.gct == ~LJ_TCDATA &&
	 (lj_obj_gcflags(o) & LJ_GC_CDATA_FIN) &&
	 !(lj_obj_gcflags(o) & LJ_GC_FINALIZED);
}

static int gc2_finreg_cdata_order_resolve(lua_State *L, CTState *cts,
					  FinRegOrderNode *ord, GCobj **op,
					  GC2MarkScope *objscope,
					  CTypeFinLease *finlease)
{
  TValue key;
  GCobj *o;
  int rc;
  uint32_t gct;
  if (op)
    *op = NULL;
  gc2_mark_scope_init(objscope);
  if (finlease)
    memset(finlease, 0, sizeof(*finlease));
  if (!L || !cts || !ord || !op || !objscope || !finlease)
    return LJ_CTYPE_FIN_MISS;
  o = fin_order_obj_acq(ord);
  rc = gc2_observed_obj_status_scoped(cts->g, o, &gct, objscope);
  if (rc <= 0 || gct != (uint32_t)~LJ_TCDATA) {
    gc2_mark_scope_leave(objscope);
    return rc < 0 ? LJ_CTYPE_FIN_RETRY : LJ_CTYPE_FIN_MISS;
  }
  setcdataV(L, &key, gco2cd(o));
  rc = lj_ctype_fin_get(L, cts, &key, finlease);
  if (rc != LJ_CTYPE_FIN_FOUND || !finlease->tab || !finlease->slot ||
      !fin_gen_tab_enabled_acq(finlease->tab)) {
    lj_ctype_fin_lease_release(finlease);
    gc2_mark_scope_leave(objscope);
    return rc == LJ_CTYPE_FIN_RETRY ? LJ_CTYPE_FIN_RETRY :
	   LJ_CTYPE_FIN_MISS;
  }
  /* 05 section 5.8: ordered FINREG node owns cdata identity, while the two
  ** returned move-only scopes own its body and exact FINREG slot bytes. */
  *op = o;
  return LJ_CTYPE_FIN_FOUND;
}

static void gc2_finreg_cdata_order_scopes_leave(GC2MarkScope *objscope,
						 CTypeFinLease *finlease)
{
  lj_ctype_fin_lease_release(finlease);
  gc2_mark_scope_leave(objscope);
}

static int gc2_finreg_cdata_store_claim(global_State *g,
					 CTypeFinLease *finlease,
					 cTValue *key, cTValue *src)
{
  if (lj_cdata_fin_store_claim_held(finlease, key, src))
    return 1;
  /* A successful keyed claim pins its vector generation until replacement.
  ** Failure here is therefore an invariant break, not an ordinary miss. Keep
  ** physical reclamation vetoed and leave the order/finalizer work retryable. */
  gc2_activation_pin_no_reclaim(g);
  gc2_marks_this_round_add(g, 1);
  lj_gc2_worker_wake(g);
  return 0;
}

typedef struct GC2FinOrderWalk {
  FinRegOrderNode *prev;
  FinRegOrderNode *cur;
  FinRegOrderNode *next;
  LJGC2Lease prevlease;
  LJGC2Lease curlease;
  LJGC2Lease nextlease;
} GC2FinOrderWalk;

static void gc2_fin_order_walk_init(GC2FinOrderWalk *walk)
{
  memset(walk, 0, sizeof(*walk));
}

static void gc2_fin_order_walk_fini(GC2FinOrderWalk *walk)
{
  if (!walk)
    return;
  lj_gc2_lease_release(&walk->nextlease);
  lj_gc2_lease_release(&walk->curlease);
  lj_gc2_lease_release(&walk->prevlease);
  gc2_fin_order_walk_init(walk);
}

static int gc2_fin_order_walk_start(global_State *g, CTState *cts,
				     GC2FinOrderWalk *walk)
{
  gc2_fin_order_walk_init(walk);
  walk->cur = fin_order_head_acq(cts);
  if (!walk->cur)
    return 1;
  if (lj_gc2_mem_lease_acquire(g, walk->cur, &walk->curlease) < 0) {
    lj_gc2_lease_release(&walk->curlease);
    walk->cur = NULL;
    return 0;
  }
  return 1;
}

/* Load and admit successor before any branch can release current. The active
** predecessor lease is retained independently so retire/splice never receives
** a pointer whose allocation was already allowed to disappear. */
static int gc2_fin_order_walk_prepare_next(global_State *g,
					    GC2FinOrderWalk *walk)
{
  walk->next = fin_order_next_acq(walk->cur);
  memset(&walk->nextlease, 0, sizeof(walk->nextlease));
  if (!walk->next)
    return 1;
  if (walk->next == walk->cur || walk->next == walk->prev) {
    gc2_activation_pin_no_reclaim(g);
    return 0;
  }
  if (lj_gc2_mem_lease_acquire(g, walk->next, &walk->nextlease) < 0) {
    lj_gc2_lease_release(&walk->nextlease);
    return 0;
  }
  return 1;
}

static void gc2_fin_order_walk_advance(GC2FinOrderWalk *walk,
					int current_is_prev)
{
  if (current_is_prev) {
    lj_gc2_lease_release(&walk->prevlease);
    walk->prev = walk->cur;
    walk->prevlease = walk->curlease;
    memset(&walk->curlease, 0, sizeof(walk->curlease));
  } else {
    lj_gc2_lease_release(&walk->curlease);
  }
  walk->cur = walk->next;
  walk->curlease = walk->nextlease;
  walk->next = NULL;
  memset(&walk->nextlease, 0, sizeof(walk->nextlease));
}

static void gc2_fin_order_walk_retry(global_State *g)
{
  gc2_root_scan_retry(g);
}

static int gc2_finreg_cdata_unlink_root(global_State *g, GCobj *target)
{
  /* Keep ABSENT distinct from a bounded/invalid-spine failure. Dispatch may
  ** reinsert this intrusive object only after discovery proves that it is no
  ** longer a member; treating UNPROVEN as absence overwrites an in-list gcw and
  ** forms a cycle. The shared unlinker also rejects string-hash successors. */
  return lj_gc_unlink_root_obj(g, target);
}
#endif

size_t lj_gc2_finreg_cdata_finalize_pweak(lua_State *L, global_State *g,
					  GC2FinRegMarkFunc mark)
{
#if LJ_HASFFI
  CTState *cts;
  GC2FinOrderWalk walk;
  size_t queued = 0;
  if (!L || !g || !mark)
    return 0;
  cts = ctype_ctsG(g);
  if (cts == NULL)
    return 0;
  if (!lj_gc2_smr_read_try(g)) {
    gc2_fin_order_walk_retry(g);
    return 0;
  }
  if (!gc2_fin_order_walk_start(g, cts, &walk)) {
    lj_gc2_smr_read_leave(g);
    gc2_fin_order_walk_retry(g);
    return 0;
  }
  while (walk.cur != NULL) {
    CTypeFinLease finlease = CTYPE_FIN_LEASE_INIT;
    GC2MarkScope objscope;
    TValue *slot;
    TValue fin, key;
    GCobj *o;
    int candidate_status, claim_status, preclaim_ready, resolve_status;
    int slot_status;
    int unlink_status;
    if (!gc2_fin_order_walk_prepare_next(g, &walk)) {
      gc2_fin_order_walk_retry(g);
      break;
    }
    if (fin_order_active_acq(walk.cur) != 1) {
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    gc2_finreg_cdata_order_seen_add(g, 1);
    resolve_status = gc2_finreg_cdata_order_resolve(
      L, cts, walk.cur, &o, &objscope, &finlease);
    if (resolve_status == LJ_CTYPE_FIN_RETRY) {
      gc2_fin_order_walk_retry(g);
      break;
    }
    if (resolve_status != LJ_CTYPE_FIN_FOUND) {
      gc2_finreg_cdata_order_tombstones_add(g, 1);
      if (!lj_ctype_fin_order_retire(
	    cts, walk.prev, walk.cur, walk.next)) {
	gc2_fin_order_walk_retry(g);
	break;
      }
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    slot = finlease.slot;
    setcdataV(L, &key, gco2cd(o));
    slot_status = lj_tab_read_current_keyed(finlease.tab, slot, &key, &fin);
    if (slot_status != LJ_TAB_STORE_CAS_OK || tvisforward(&fin) ||
	lj_cdata_fin_isclaim(&fin)) {
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      gc2_fin_order_walk_retry(g);
      break;
    }
    if (tvisnil(&fin)) {
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      gc2_finreg_cdata_order_tombstones_add(g, 1);
      if (!lj_ctype_fin_order_retire(
	    cts, walk.prev, walk.cur, walk.next)) {
	gc2_fin_order_walk_retry(g);
	break;
      }
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    candidate_status = gc2_finreg_cdata_candidate_pweak(g, o);
    if (candidate_status == GC2_FINREG_CANDIDATE_RETRY) {
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      gc2_fin_order_walk_retry(g);
      break;
    }
    if (candidate_status != GC2_FINREG_CANDIDATE_YES) {
      if (!(lj_obj_gcflags(o) & LJ_GC_CDATA_FIN) ||
	  (lj_obj_gcflags(o) & LJ_GC_FINALIZED)) {
	gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
	gc2_finreg_cdata_order_tombstones_add(g, 1);
	if (!lj_ctype_fin_order_retire(
	      cts, walk.prev, walk.cur, walk.next)) {
	  gc2_fin_order_walk_retry(g);
	  break;
	}
	gc2_fin_order_walk_advance(&walk, 0);
	continue;
      }
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      gc2_fin_order_walk_advance(&walk, 1);
      continue;
    }
    preclaim_ready = gc2_finclaim_prepare(g);
    claim_status = lj_cdata_fin_claim_held(&finlease, &key, &fin, 1);
    if (claim_status != LJ_CTYPE_FIN_FOUND) {
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      if (claim_status == LJ_CTYPE_FIN_RETRY) {
	gc2_fin_order_walk_retry(g);
	break;
      }
      gc2_finreg_cdata_order_tombstones_add(g, 1);
      if (!lj_ctype_fin_order_retire(
	    cts, walk.prev, walk.cur, walk.next)) {
	gc2_fin_order_walk_retry(g);
	break;
      }
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    /*
    ** 05 section 5.8: ordered FINREG identity is enough for P_WEAK
    ** discovery without root membership.
    */
    unlink_status = gc2_finreg_cdata_unlink_root(g, o);
    if (unlink_status == LJ_GC_ROOT_UNLINK_UNPROVEN) {
      /* The finalizer slot is the retry lease. Restore the value release-wise
      ** and leave this order node active; neither the finalizer queue nor a
      ** later root requeue may own o until membership is proved. A weak/root
      ** scanner may have observed the claim sentinel, so retain the restored
      ** finalizer before making that weak value visible again. Structural table
      ** publication is unchanged; claim-aware scanners wait for this release. */
      gc2_finreg_queue_mark(g, o);
      mark(g, &fin);
      (void)gc2_finreg_cdata_store_claim(g, &finlease, &key, &fin);
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      gc2_fin_order_walk_advance(&walk, 1);
      continue;
    }
    gc2_finreg_cdata_order_claimed_add(g, 1);
    if (unlink_status == LJ_GC_ROOT_UNLINKED)
      gc2_finreg_cdata_order_unlinked_add(g, 1);
    if (!preclaim_ready) {
      gc2_finreg_cdata_preclaim_overflow_add(g, 1);
    }
    if (!preclaim_ready ||
	!lj_gc2_finreg_cdata_preclaim(L, g, o, &fin)) {
      TValue nilv;
      setnilV(&nilv);
      if (!gc2_finreg_cdata_store_claim(g, &finlease, &key, &nilv)) {
	gc2_finreg_queue_mark(g, o);
	gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
	gc2_fin_order_walk_advance(&walk, 1);
	continue;
      }
      lj_ctype_fin_lease_release(&finlease);
      mark(g, &fin);
      gc2_finreg_cdata_finalizer_enqueue_admitted(g, o, &fin);
      gc2_mark_scope_leave(&objscope);
      gc2_finreg_cdata_order_fallbacks_add(g, 1);
      gc2_finreg_cdata_order_queued_add(g, 1);
      if (!lj_ctype_fin_order_retire(
	    cts, walk.prev, walk.cur, walk.next)) {
	gc2_fin_order_walk_retry(g);
	break;
      }
      queued++;
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    {
      TValue nilv;
      setnilV(&nilv);
      if (!gc2_finreg_cdata_store_claim(g, &finlease, &key, &nilv)) {
	TValue unused;
	(void)lj_gc2_finreg_cdata_preclaim_take(L, g, o, &unused);
	gc2_finreg_queue_mark(g, o);
	gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
	gc2_fin_order_walk_advance(&walk, 1);
	continue;
      }
    }
    lj_ctype_fin_lease_release(&finlease);
    mark(g, &fin);
    gc2_finreg_cdata_finalizer_enqueue_admitted(g, o, &fin);
    gc2_mark_scope_leave(&objscope);
    gc2_finreg_cdata_order_queued_add(g, 1);
    if (!lj_ctype_fin_order_retire(
	  cts, walk.prev, walk.cur, walk.next)) {
      gc2_fin_order_walk_retry(g);
      break;
    }
    queued++;
    gc2_fin_order_walk_advance(&walk, 0);
  }
  gc2_fin_order_walk_fini(&walk);
  lj_gc2_smr_read_leave(g);
  if (queued)
    gc2_finreg_cdata_pweak_queued_add(g, queued);
  return queued;  /* 05 section 5.8: GC2-owned P_WEAK FINREG cdata scan. */
#else
  UNUSED(L); UNUSED(g); UNUSED(mark);
  return 0;
#endif
}

size_t lj_gc2_finreg_cdata_finalize_close(global_State *g)
{
#if LJ_HASFFI
  CTState *cts;
  GC2FinOrderWalk walk;
  lua_State *L;
  size_t queued = 0;
  if (!g)
    return 0;
  cts = ctype_ctsG(g);
  if (cts == NULL)
    return 0;
  L = mainthread_acq(g);
  if (!lj_gc2_smr_read_try(g)) {
    gc2_fin_order_walk_retry(g);
    return 0;
  }
  if (!gc2_fin_order_walk_start(g, cts, &walk)) {
    lj_gc2_smr_read_leave(g);
    gc2_fin_order_walk_retry(g);
    return 0;
  }
  while (walk.cur != NULL) {
    CTypeFinLease finlease = CTYPE_FIN_LEASE_INIT;
    GC2MarkScope objscope;
    TValue *slot;
    TValue fin, key;
    GCobj *o;
    int claim_status, resolve_status, slot_status, unlink_status;
    if (!gc2_fin_order_walk_prepare_next(g, &walk)) {
      gc2_fin_order_walk_retry(g);
      break;
    }
    if (fin_order_active_acq(walk.cur) != 1) {
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    resolve_status = gc2_finreg_cdata_order_resolve(
      L, cts, walk.cur, &o, &objscope, &finlease);
    if (resolve_status == LJ_CTYPE_FIN_RETRY) {
      gc2_fin_order_walk_retry(g);
      break;
    }
    if (resolve_status != LJ_CTYPE_FIN_FOUND) {
      if (!lj_ctype_fin_order_retire(
	    cts, walk.prev, walk.cur, walk.next)) {
	gc2_fin_order_walk_retry(g);
	break;
      }
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    slot = finlease.slot;
    setcdataV(L, &key, gco2cd(o));
    slot_status = lj_tab_read_current_keyed(finlease.tab, slot, &key, &fin);
    if (slot_status != LJ_TAB_STORE_CAS_OK || tvisforward(&fin) ||
	lj_cdata_fin_isclaim(&fin)) {
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      gc2_fin_order_walk_retry(g);
      break;
    }
    if (tvisnil(&fin)) {
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      if (!lj_ctype_fin_order_retire(
	    cts, walk.prev, walk.cur, walk.next)) {
	gc2_fin_order_walk_retry(g);
	break;
      }
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    if (!gc2_finreg_cdata_candidate_close(o)) {
      if (!(lj_obj_gcflags(o) & LJ_GC_CDATA_FIN) ||
	  (lj_obj_gcflags(o) & LJ_GC_FINALIZED)) {
	gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
	if (!lj_ctype_fin_order_retire(
	      cts, walk.prev, walk.cur, walk.next)) {
	  gc2_fin_order_walk_retry(g);
	  break;
	}
	gc2_fin_order_walk_advance(&walk, 0);
	continue;
      }
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      gc2_fin_order_walk_advance(&walk, 1);
      continue;
    }
    claim_status = lj_cdata_fin_claim_held(&finlease, &key, &fin, 1);
    if (claim_status != LJ_CTYPE_FIN_FOUND) {
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      if (claim_status == LJ_CTYPE_FIN_RETRY) {
	gc2_fin_order_walk_retry(g);
	break;
      }
      if (!lj_ctype_fin_order_retire(
	    cts, walk.prev, walk.cur, walk.next)) {
	gc2_fin_order_walk_retry(g);
	break;
      }
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    /*
    ** 05 section 5.8: ordered FINREG identity is enough for close-time
    ** discovery without root membership.
    */
    unlink_status = gc2_finreg_cdata_unlink_root(g, o);
    if (unlink_status == LJ_GC_ROOT_UNLINK_UNPROVEN) {
      /* Keep the slot/order node intact for a later bounded retry. An active
      ** close entered from a non-idle collector still needs the object and
      ** callback retained after an invalid prefix was severed. */
      gc2_finreg_queue_mark(g, o);
      gc2_finreg_marktv(g, &fin);
      (void)gc2_finreg_cdata_store_claim(g, &finlease, &key, &fin);
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      gc2_fin_order_walk_advance(&walk, 1);
      continue;
    }
    {
      TValue nilv;
      setnilV(&nilv);
      if (!gc2_finreg_cdata_store_claim(g, &finlease, &key, &nilv)) {
	gc2_finreg_queue_mark(g, o);
	gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
	gc2_fin_order_walk_advance(&walk, 1);
	continue;
      }
    }
    lj_ctype_fin_lease_release(&finlease);
    gc2_finreg_cdata_finalizer_enqueue_admitted(g, o, &fin);
    gc2_mark_scope_leave(&objscope);
    gc2_finreg_cdata_order_queued_add(g, 1);
    queued++;
    if (!lj_ctype_fin_order_retire(
	  cts, walk.prev, walk.cur, walk.next)) {
      gc2_fin_order_walk_retry(g);
      break;
    }
    gc2_fin_order_walk_advance(&walk, 0);
  }
  gc2_fin_order_walk_fini(&walk);
  lj_gc2_smr_read_leave(g);
  return queued;  /* 05 section 5.8: GC2-owned close-time FINREG scan. */
#else
  UNUSED(g);
  return 0;
#endif
}

int lj_gc2_finreg_cdata_pending(global_State *g)
{
#if LJ_HASFFI
  CTState *cts;
  GC2FinOrderWalk walk;
  lua_State *L;
  int pending = 0;
  if (!g)
    return 0;
  cts = ctype_ctsG(g);
  if (cts == NULL)
    return 0;
  L = mainthread_acq(g);
  if (!lj_gc2_smr_read_try(g)) {
    gc2_fin_order_walk_retry(g);
    return 1;
  }
  if (!gc2_fin_order_walk_start(g, cts, &walk)) {
    lj_gc2_smr_read_leave(g);
    gc2_fin_order_walk_retry(g);
    return 1;
  }
  while (walk.cur != NULL) {
    CTypeFinLease finlease = CTYPE_FIN_LEASE_INIT;
    GC2MarkScope objscope;
    TValue *slot;
    TValue fin, key;
    GCobj *o;
    int resolve_status, slot_status;
    if (!gc2_fin_order_walk_prepare_next(g, &walk)) {
      gc2_fin_order_walk_retry(g);
      pending = 1;
      break;
    }
    if (fin_order_active_acq(walk.cur) != 1) {
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    resolve_status = gc2_finreg_cdata_order_resolve(
      L, cts, walk.cur, &o, &objscope, &finlease);
    if (resolve_status == LJ_CTYPE_FIN_RETRY) {
      pending = 1;  /* Transient slot identity is conservatively pending. */
      break;
    }
    if (resolve_status != LJ_CTYPE_FIN_FOUND) {
      if (!lj_ctype_fin_order_retire(
	    cts, walk.prev, walk.cur, walk.next)) {
	gc2_fin_order_walk_retry(g);
	pending = 1;
	break;
      }
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    slot = finlease.slot;
    setcdataV(L, &key, gco2cd(o));
    slot_status = lj_tab_read_current_keyed(finlease.tab, slot, &key, &fin);
    if (slot_status != LJ_TAB_STORE_CAS_OK || tvisforward(&fin) ||
	lj_cdata_fin_isclaim(&fin)) {
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      pending = 1;
      break;
    }
    if (tvisnil(&fin)) {
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      if (!lj_ctype_fin_order_retire(
	    cts, walk.prev, walk.cur, walk.next)) {
	gc2_fin_order_walk_retry(g);
	pending = 1;
	break;
      }
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    if (gc2_finreg_cdata_candidate_close(o)) {
      gc2_finreg_cdata_pending_order_hits_add(g, 1);
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      pending = 1;
      break;
    }
    if (!(lj_obj_gcflags(o) & LJ_GC_CDATA_FIN) ||
	(lj_obj_gcflags(o) & LJ_GC_FINALIZED)) {
      gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
      if (!lj_ctype_fin_order_retire(
	    cts, walk.prev, walk.cur, walk.next)) {
	gc2_fin_order_walk_retry(g);
	pending = 1;
	break;
      }
      gc2_fin_order_walk_advance(&walk, 0);
      continue;
    }
    gc2_finreg_cdata_order_scopes_leave(&objscope, &finlease);
    gc2_fin_order_walk_advance(&walk, 1);
  }
  gc2_fin_order_walk_fini(&walk);
  lj_gc2_smr_read_leave(g);
  return pending;  /* 05 section 5.8: close-time pending FINREG scan. */
#else
  UNUSED(g);
  return 0;
#endif
}

void lj_gc2_finreg_cdata_disable(global_State *g)
{
#if LJ_HASFFI
  CTState *cts = ctype_ctsG(g);
  FinRegGen *gen;
  if (!g || cts == NULL)
    return;
  if (!lj_gc2_smr_read_try(g))
    return;
  for (gen = fin_gen_head_acq(cts); gen != NULL;) {
    CTypeFinLease finlease = CTYPE_FIN_LEASE_INIT;
    LJGC2Lease genlease;
    FinRegGen *next;
    int rc;
    memset(&genlease, 0, sizeof(genlease));
    if (lj_gc2_mem_lease_acquire(g, gen, &genlease) < 0) {
      lj_gc2_lease_release(&genlease);
      break;
    }
    next = fin_gen_next_acq(gen);
    rc = lj_ctype_fin_gen_tab_acquire(cts, gen, &finlease);
    if (rc == LJ_CTYPE_FIN_FOUND)
      fin_gen_tab_disable_rel(finlease.tab);  /* Disable while table-held. */
    lj_ctype_fin_lease_release(&finlease);
    lj_gc2_lease_release(&genlease);
    if (rc == LJ_CTYPE_FIN_RETRY)
      break;
    gen = next;
  }
  lj_gc2_smr_read_leave(g);
#else
  UNUSED(g);
#endif
}

#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
void lj_gc2_test_finreg_cdata_preclaim_fail(global_State *g, uint32_t n)
{
  if (g)
    gc2_finreg_cdata_preclaim_test_fail_rel(g, n);
}

void lj_gc2_test_finreg_cdata_preclaim_publish_pause(global_State *g)
{
  if (!g)
    return;
  gc2_finreg_cdata_preclaim_publish_release_rel(g, 0);
  gc2_finreg_cdata_preclaim_publish_paused_rel(g, 0);
  gc2_finreg_cdata_preclaim_publish_pause_rel(g, 1);
}

void lj_gc2_test_finalizer_drain_pause(global_State *g)
{
  if (!g)
    return;
  gc2_finalizer_drain_test_release_rel(g, 0);
  gc2_finalizer_drain_test_paused_rel(g, 0);
  gc2_finalizer_drain_test_pause_rel(g, 1);
}
#endif

static int lj_gc2_finreg_cdata_preclaim(lua_State *L, global_State *g,
					GCobj *o, cTValue *fin)
{
#if LJ_HASFFI
  MSize count, cap;
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  uint32_t test_fail;
#endif
  if (!L || !fin || !gc2_finreg_cdata_obj_valid(g, o))
    return 0;
#if defined(LUA_USE_ASSERT) || LJ_GC2_PARANOIA
  test_fail = gc2_finreg_cdata_preclaim_test_fail_acq(g);
  if (test_fail) {
    gc2_finreg_cdata_preclaim_test_fail_rel(g, test_fail - 1u);
    gc2_finreg_cdata_preclaim_overflow_add(g, 1);
    return 0;  /* Test-only side-vector failure injection. */
  }
#endif
  count = gc2_finreg_cdata_preclaim_count_acq(g);
  cap = gc2_finreg_cdata_preclaim_capacity_acq(g);
  if (!gc2_finreg_cdata_preclaim_ready(g)) {
    if (!gc2_finclaim_ensure(g)) {
      gc2_finreg_cdata_preclaim_overflow_add(g, 1);
      return 0;
    }
    count = gc2_finreg_cdata_preclaim_count_acq(g);
    cap = gc2_finreg_cdata_preclaim_capacity_acq(g);
  }
  if (count >= cap) {
    gc2_finreg_cdata_preclaim_overflow_add(g, 1);
    return 0;
  }
  gc2_finclaim_publish(L, g, count, o, fin);
  gc2_finreg_cdata_preclaim_count_rel(g, count + 1u);
  gc2_finreg_cdata_pweak_claimed_add(g, 1);
  return 1;  /* 05 section 5.8: P_WEAK owns claimed cdata finalizer. */
#else
  UNUSED(L); UNUSED(g); UNUSED(o); UNUSED(fin);
  return 0;
#endif
}

static int lj_gc2_finreg_cdata_preclaim_take(lua_State *L, global_State *g,
					     GCobj *o, TValue *fin)
{
#if LJ_HASFFI
  MSize head, count, i;
  GCobj *claimed;
  if (!L || !fin || !gc2_finreg_cdata_obj_valid(g, o) ||
      !gc2_finreg_cdata_preclaim_ready(g))
    return 0;
  head = gc2_finreg_cdata_preclaim_head_acq(g);
  count = gc2_finreg_cdata_preclaim_count_acq(g);
  if (head >= count)
    return 0;
  for (i = head; i < count; i++) {
    claimed = gc2_finreg_cdata_preclaim_obj_acq(g, i);
    if (claimed != o)
      continue;
    gc2_finreg_cdata_preclaim_fin_acq(g, i, fin);
    gc2_finclaim_clear(L, g, i);
    while (head < count &&
	   gc2_finreg_cdata_preclaim_obj_acq(g, head) == NULL)
      head++;
    if (head == count) {
      gc2_finreg_cdata_preclaim_head_rel(g, 0);
      gc2_finreg_cdata_preclaim_count_rel(g, 0);
    } else {
      gc2_finreg_cdata_preclaim_head_rel(g, head);
    }
    gc2_finreg_cdata_preclaim_dispatched_add(g, 1);
    return 1;  /* 05 section 5.8: dispatch order may differ from FINREG scan. */
  }
  if (head == count) {
    gc2_finreg_cdata_preclaim_head_rel(g, 0);
    gc2_finreg_cdata_preclaim_count_rel(g, 0);
  }
  return 0;
#else
  UNUSED(L); UNUSED(g); UNUSED(o); UNUSED(fin);
  return 0;
#endif
}

void lj_gc2_test_finreg_cdata_finalizer_enqueue(global_State *g, GCobj *o)
{
  lj_gc2_finreg_cdata_finalizer_enqueue(g, o, NULL);
}

int lj_gc2_test_finreg_cdata_preclaim(lua_State *L, global_State *g,
				      GCobj *o, cTValue *fin)
{
  return lj_gc2_finreg_cdata_preclaim(L, g, o, fin);
}

int lj_gc2_test_finreg_cdata_preclaim_take(lua_State *L, global_State *g,
					   GCobj *o, TValue *fin)
{
  return lj_gc2_finreg_cdata_preclaim_take(L, g, o, fin);
}

static int gc2_finreg_dispatch_requeue(global_State *g, GCobj *o)
{
  GC2MarkScope scope;
  uint32_t gct;
  int link;
  if (!gc2_observed_obj_valid_scoped(g, o, &gct, &scope))
    return LJ_GC_ROOT_LINK_INVALID;
#if LJ_HASFFI
  if (gct == (uint32_t)~LJ_TCDATA) {
    void *base = NULL;
    /* FINREG owns this incarnation, but variable/aligned cdata roots are
    ** represented by their allocation base. Never let a poisoned or stale
    ** header make the generic exact-object linker claim the wrong lane. */
    if (!lj_cdata_validate(g, gco2cd(o), &base, NULL) || !base) {
      gc2_mark_scope_leave(&scope);
      return LJ_GC_ROOT_LINK_INVALID;
    }
    link = lj_gc_linkobj_at(g, o, base);
  } else
#endif
  if (gct == (uint32_t)~LJ_TUDATA) {
    link = lj_gc_linkobj_after(g, obj2gco(mainthread_acq(g)), o);
  } else {
    gc2_mark_scope_leave(&scope);
    return LJ_GC_ROOT_LINK_INVALID;
  }
  if (link != LJ_GC_ROOT_LINKED && link != LJ_GC_ROOT_LINK_ALREADY) {
    gc2_mark_scope_leave(&scope);
    return link;
  }
  gc2_mark_scope_leave(&scope);
  /* The reinserted object is a GC2 root. Keep it live in an in-flight cycle
  ** without reviving or traversing any legacy color-collector state. */
  gc2_finreg_queue_mark(g, o);
  return link;
}

#if LJ_HASFFI
static void gc2_finreg_cdata_dispatch_clear(global_State *g, GCobj *o)
{
  lj_obj_cleargcflags_atomic(o, LJ_GC_CDATA_FIN);
  lj_gc2_finreg_cdata_set(g, o, 0);
}

static int gc2_finreg_cdata_dispatch_claim_preclaimed(global_State *g,
						      GCobj *o)
{
  uint8_t old = la_load8_acq(lj_obj_gcflags_ref(o));
  for (;;) {
    uint8_t next;
    if (!(old & LJ_GC_CDATA_FIN))
      return 0;
    next = (uint8_t)(old & (uint8_t)~LJ_GC_CDATA_FIN);
    if (la_cas8(lj_obj_gcflags_ref(o), &old, next, LA_ACQ_REL, LA_ACQ)) {
      lj_gc2_finreg_cdata_set(g, o, 0);
      return 1;
    }
  }
}

static int gc2_finreg_cdata_dispatch_preclaimed(lua_State *L, global_State *g,
						GCobj *o, cTValue *fin)
{
  TValue tmp;
  if (!gc2_finreg_cdata_dispatch_claim_preclaimed(g, o))
    return 0;  /* P_WEAK preclaim suppressed by later ffi.gc(cd, nil). */
  copyTV(L, &tmp, fin);
  return gc2_call_finalizer(g, L, &tmp, o) ? 1 : -1;
}

static int gc2_finreg_cdata_dispatch_slot(lua_State *L, global_State *g,
					  GCobj *o, cTValue *key)
{
  CTState *cts = ctype_ctsG(g);
  CTypeFinLease finlease = CTYPE_FIN_LEASE_INIT;
  TValue fin;
  int rc = lj_ctype_fin_get(L, cts, key, &finlease);
  if (rc == LJ_CTYPE_FIN_RETRY)
    return GC2_FINALIZER_DISPATCH_DEFER;
  if (rc == LJ_CTYPE_FIN_FOUND) {
    rc = lj_cdata_fin_claim_held(&finlease, key, &fin, 1);
    if (rc == LJ_CTYPE_FIN_RETRY) {
      lj_ctype_fin_lease_release(&finlease);
      return GC2_FINALIZER_DISPATCH_DEFER;
    }
    if (rc == LJ_CTYPE_FIN_FOUND) {
    TValue tmp;
    copyTV(L, &tmp, &fin);
    {
      TValue nilv;
      setnilV(&nilv);
      if (!gc2_finreg_cdata_store_claim(g, &finlease, key, &nilv)) {
	(void)gc2_finreg_cdata_store_claim(g, &finlease, key, &fin);
	lj_ctype_fin_lease_release(&finlease);
	return GC2_FINALIZER_DISPATCH_DEFER;
      }
    }
    lj_ctype_fin_lease_release(&finlease);
    gc2_finreg_cdata_dispatch_clear(g, o);
    return gc2_call_finalizer(g, L, &tmp, o) ? 1 : -1;
    }
  }
  lj_ctype_fin_lease_release(&finlease);
  gc2_finreg_cdata_dispatch_clear(g, o);
  return 0;
}

static int gc2_finreg_cdata_dispatch_ffi(lua_State *L, global_State *g,
					 GCobj *o)
{
  TValue key;
  TValue fin;
  if (!L || !gc2_finreg_cdata_obj_valid(g, o))
    return 0;
  if (lj_gc2_finreg_cdata_preclaim_take(L, g, o, &fin))
    return gc2_finreg_cdata_dispatch_preclaimed(L, g, o, &fin);
  setcdataV(L, &key, gco2cd(o));
  return gc2_finreg_cdata_dispatch_slot(L, g, o, &key);
}
#endif

static int lj_gc2_finreg_cdata_dispatch(lua_State *L, global_State *g,
					GCobj *o, cTValue *fin)
{
#if LJ_HASFFI
  TValue tmp;
  if (!L || !gc2_finreg_cdata_obj_valid(g, o))
    return 0;
  if (fin) {
    TValue unused;
    if (gc2_finreg_dispatch_requeue(g, o) <= LJ_GC_ROOT_LINK_DEFER)
      return GC2_FINALIZER_DISPATCH_DEFER;
    (void)lj_gc2_finreg_cdata_preclaim_take(L, g, o, &unused);
    if (!gc2_finreg_cdata_dispatch_claim_preclaimed(g, o))
      return 0;  /* Queued snapshot suppressed by later ffi.gc(cd, nil). */
    copyTV(L, &tmp, fin);
    return gc2_call_finalizer(g, L, &tmp, o) ? 1 : -1;
  }
  if (gc2_finreg_dispatch_requeue(g, o) <= LJ_GC_ROOT_LINK_DEFER)
    return GC2_FINALIZER_DISPATCH_DEFER;
  return gc2_finreg_cdata_dispatch_ffi(L, g, o);
#else
  UNUSED(L); UNUSED(g); UNUSED(o); UNUSED(fin);
  return 0;
#endif
}

static int gc2_finreg_udata_set_admitted(global_State *g, GCobj *o,
					  int enabled)
{
  uint8_t old;
  old = la_load8_acq(lj_obj_gcflags_ref(o));
  for (;;) {
    uint8_t next = enabled ? (uint8_t)(old | LJ_GC_UDATA_FINREG) :
			     (uint8_t)(old & (uint8_t)~LJ_GC_UDATA_FINREG);
    if (next == old)
      return 0;
    if (la_cas8(lj_obj_gcflags_ref(o), &old, next, LA_ACQ_REL, LA_ACQ))
      break;
  }
  if (enabled) {
    gc2_finreg_udata_sets_add(g, 1);
    return 1;
  } else {
    gc2_finreg_udata_clears_add(g, 1);
    return -1;
  }
}

int lj_gc2_finreg_udata_set(global_State *g, GCobj *o, int enabled)
{
  if (!gc2_finreg_udata_obj_valid(g, o))
    return 0;
  return gc2_finreg_udata_set_admitted(g, o, enabled);
}

static void gc2_finreg_udata_node_publish(global_State *g,
					  GC2FinRegUDataNode *node)
{
  GC2FinRegUDataNode *head;
  /* The allocating mutator owns node until the active-list CAS. Active-list
  ** membership, then the unlink/retired-list protocol, owns it afterwards and
  ** defers physical free until terminal teardown. These marks only close the
  ** IDLE->MARK clear/root-snapshot race; an ordinary metadata writer must not
  ** turn that tactical miss into an absorbing reclaim veto. */
  (void)lj_gc2_markmem_registered_publish_try(g, node);
  do {
    head = gc2_finreg_udata_head_acq(g);
    gc2_finreg_udata_next_rel(node, head);
  } while (!gc2_finreg_udata_head_cas(g, &head, node));
  /* Close construction-mark versus IDLE->MARK clear/root-snapshot. */
  (void)lj_gc2_markmem_registered_publish_try(g, node);
}

static int gc2_finreg_udata_register_nothrow(lua_State *L, global_State *g,
					      GCobj *o)
{
  GC2FinRegUDataNode *node;
  if (!L || !gc2_finreg_udata_obj_valid(g, o))
    return 1;
  for (node = gc2_finreg_udata_head_acq(g);
       node != NULL && lj_gc2_mem_registered(g, node);
       node = gc2_finreg_udata_next_acq(node)) {
    if (gc2_finreg_udata_active_acq(node) &&
	gc2_finreg_udata_obj_acq(node) == o)
      return 1;
  }
  node = (GC2FinRegUDataNode *)
    lj_mem_new_nothrow(L, sizeof(GC2FinRegUDataNode));
  if (LJ_UNLIKELY(node == NULL))
    return 0;  /* No registry/FINREG mutation precedes the OOM result. */
  gc2_finreg_udata_obj_rel(node, o);
  gc2_finreg_udata_retired_next_rel(node, NULL);
  gc2_finreg_udata_active_rel(node, 1);
  gc2_finreg_udata_node_publish(g, node);
  gc2_finreg_udata_registered_add(g, 1);
  return 1;
}

#if defined(LJ_GC2_TEST_HELPERS)
void lj_gc2_test_finreg_udata_node_publish(global_State *g,
					   GC2FinRegUDataNode *node)
{
  if (!g || !node)
    return;
  gc2_finreg_udata_node_publish(g, node);
}
#endif

void lj_gc2_finreg_udata_register(lua_State *L, global_State *g, GCobj *o)
{
  if (LJ_UNLIKELY(!gc2_finreg_udata_register_nothrow(L, g, o)))
    lj_err_mem(L);
}

int lj_gc2_finreg_udata_register_mt_nothrow(lua_State *L, global_State *g,
					     GCudata *ud, GCtab *mt)
{
  TValue mmv;
  if (!L || !g || !ud || !mt)
    return 1;
  if (LJ_UNLIKELY(!gc2_finreg_udata_register_nothrow(L, g, obj2gco(ud))))
    return 0;
  if (lj_meta_fasttv(g, mt, MM_gc, &mmv))
    (void)lj_gc2_finreg_udata_set(g, obj2gco(ud), 1);
  return 1;
}

void lj_gc2_finreg_udata_register_mt(lua_State *L, global_State *g,
				     GCudata *ud, GCtab *mt)
{
  if (LJ_UNLIKELY(!lj_gc2_finreg_udata_register_mt_nothrow(L, g, ud, mt)))
    lj_err_mem(L);
}

static int gc2_finreg_udata_retire(global_State *g,
				   GC2FinRegUDataNode *node)
{
  GC2FinRegUDataNode *head;
  if (!g || !node || !lj_gc2_mem_registered(g, node))
    return 0;
  /* Retain the raw record before inspecting object or active state. */
  (void)lj_gc2_markmem(g, node);
  lj_assertG(gc2_finreg_udata_obj_acq(node) == NULL,
	     "retiring live userdata FINREG node");
  if (!gc2_finreg_udata_active_retire(node))
    return 0;
  do {
    head = gc2_finreg_udata_retired_acq(g);
    gc2_finreg_udata_retired_next_rel(node, head);
  } while (!gc2_finreg_udata_retired_cas(g, &head, node));
  (void)lj_gc2_markmem(g, node);
  gc2_finreg_udata_retired_nodes_add(g, 1);
  return 1;
}

static int lj_gc2_finreg_udata_unlink(global_State *g,
				      GC2FinRegUDataNode *prev,
				      GC2FinRegUDataNode *node,
				      GC2FinRegUDataNode *next)
{
  GC2FinRegUDataNode *expect;
  if (!g || !node || !lj_gc2_mem_registered(g, node))
    return 0;
  if (!gc2_finreg_udata_retire(g, node))
    return 1;
  if (prev) {
    if (lj_gc2_mem_registered(g, prev)) {
      expect = node;
      if (gc2_finreg_udata_active_acq(prev))
	(void)gc2_finreg_udata_next_cas(prev, &expect, next);
    }
  } else {
    expect = node;
    (void)gc2_finreg_udata_head_cas(g, &expect, next);
  }
  return 1;  /* 05 section 5.8: logical retire plus best-effort CAS unlink. */
}

void lj_gc2_finreg_udata_forget(global_State *g, GCobj *o)
{
  GC2FinRegUDataNode *prev, *node;
  int cleared = 0;
  if (!gc2_finreg_udata_obj_valid(g, o))
    return;
  prev = NULL;
  node = gc2_finreg_udata_head_acq(g);
  while (node && lj_gc2_mem_registered(g, node)) {
    GC2FinRegUDataNode *next = gc2_finreg_udata_next_acq(node);
    if (!gc2_finreg_udata_active_acq(node)) {
      node = next;
      continue;
    }
    if (gc2_finreg_udata_obj_acq(node) == o) {
      gc2_finreg_udata_obj_clear(node);
      cleared = 1;
      if (lj_gc2_finreg_udata_unlink(g, prev, node, next)) {
	node = next;
	continue;
      }
      prev = NULL;
      node = gc2_finreg_udata_head_acq(g);
      continue;
    }
    prev = node;
    node = next;
  }
  if (cleared)
    gc2_finreg_udata_forgets_add(g, 1);
}

static int gc2_finreg_udata_unlink_root(global_State *g, GCobj *target,
					int terminal)
{
  /* As for cdata, finalizer dispatch may reinsert the intrusive object only
  ** after a complete scan proves absence or removes its exact membership. */
  return terminal ? lj_gc_unlink_root_obj_terminal(g, target) :
		    lj_gc_unlink_root_obj(g, target);
}

/* Resolve __gc without entering the ordinary table getter's retry/yield loop.
** FINREG and the intrusive root still retain the userdata while this runs;
** retain the sampled metatable incarnation explicitly and cover its separated
** hash generation with SMR until the copied TValue has been classified and
** marked. Any uncertain lifetime or structural snapshot is a retry, never
** semantic absence. */
static int gc2_finreg_udata_finalizer_lookup(global_State *g, GCtab *mt,
					      TValue *mmv)
{
  GC2MarkScope mtscope;
  uint32_t gct = 0;
  int lookup_status;
  if (!g || !mmv)
    return LJ_TAB_GC_LOOKUP_RETRY;
  setnilV(mmv);
  if (!mt)
    return LJ_TAB_GC_LOOKUP_ABSENT;
  gc2_mark_scope_init(&mtscope);
  if (!gc2_observed_obj_valid_scoped(g, obj2gco(mt), &gct, &mtscope) ||
      gct != (uint32_t)~LJ_TTAB) {
    gc2_mark_scope_leave(&mtscope);
    return LJ_TAB_GC_LOOKUP_RETRY;
  }
  if (!lj_gc2_smr_read_try(g)) {
    gc2_mark_scope_leave(&mtscope);
    return LJ_TAB_GC_LOOKUP_RETRY;
  }
  lookup_status = lj_tab_getstr_held_try(
    g, mt, mmname_str(g, MM_gc), mmv);
  if (lookup_status == LJ_TAB_GC_LOOKUP_FOUND &&
      LJ_UNLIKELY(tvistabinternal(mmv) || !lj_tv_gcref_type_match(mmv)))
    lookup_status = LJ_TAB_GC_LOOKUP_RETRY;
  if (lookup_status == LJ_TAB_GC_LOOKUP_FOUND) {
    gc2_finreg_queue_mark(g, obj2gco(mt));
    gc2_mark_tv(g, mmv);
  }
  lj_gc2_smr_read_leave(g);
  gc2_mark_scope_leave(&mtscope);
  return lookup_status;
}

static void gc2_finreg_udata_finalizer_lookup_retry(
  global_State *g, GC2FinRegUDataNode *node, GCobj *o)
{
  /* The node is the authoritative retry locator. Retain both it and its
  ** userdata while reopening the current phase's root certificate; WEAK close
  ** must observe this as incomplete even when no finalizer was queued. */
  (void)lj_gc2_markmem(g, node);
  gc2_finreg_queue_mark(g, o);
  gc2_root_scan_retry(g);
}

static void gc2_finreg_udata_finalizer_enqueue_admitted(
  global_State *g, GCobj *o, cTValue *fin)
{
  /* The caller retains the exact userdata incarnation through publication;
  ** avoid a second observational admission turning a successful discovery
  ** into a silently dropped callback. */
  markfinalized(o);
  gc2_finreg_queue_mark(g, o);
  gc2_finreg_udata_queued_add(g, 1);
  lj_gc2_finalizer_enqueue_fin(g, o, fin);
}

size_t lj_gc2_finreg_udata_finalize(global_State *g, int all)
{
  GC2FinRegUDataNode *prev, *node;
  TValue mmv;
  size_t m = 0;
  if (!g)
    return 0;
  /*
  ** Fresh userdata may still be in the after-main pending-root queue. FINREG
  ** discovery unlinks finalized userdata from the mainthread chain and later
  ** requeues it; flush first so the pending queue cannot publish the same
  ** object again after discovery.
  */
  (void)lj_gc_flush_root_pending(g);
  prev = NULL;
  node = gc2_finreg_udata_head_acq(g);
  while (node && lj_gc2_mem_registered(g, node)) {
    GC2FinRegUDataNode *next = gc2_finreg_udata_next_acq(node);
    GCobj *o = gc2_finreg_udata_obj_acq(node);
    GC2MarkScope oscope;
    uint32_t gct = 0;
    uint8_t flags;
    size_t osize = 0;
    int finreg, marked, objstatus, unlink_status;
    if (!gc2_finreg_udata_active_acq(node)) {
      node = next;
      continue;
    }
    if (!o) {
      if (lj_gc2_finreg_udata_unlink(g, prev, node, next)) {
	node = next;
	continue;
      }
      prev = NULL;
      node = gc2_finreg_udata_head_acq(g);
      continue;
    }
    /* The side-list reference is semantic ownership, but not permission to
    ** dereference a body between its lifetime LPs. Keep an exact admission
    ** through every userdata header/metatable read. Overflow/transient
    ** admission is retryable; only a terminally invalid or wrong-type
    ** incarnation may retire the node. */
    gc2_mark_scope_init(&oscope);
    objstatus = gc2_observed_obj_status_scoped(g, o, &gct, &oscope);
    if (LJ_UNLIKELY(objstatus < 0)) {
      gc2_finreg_udata_finalizer_lookup_retry(g, node, o);
      gc2_mark_scope_leave(&oscope);
      prev = node;
      node = next;
      continue;
    }
    if (objstatus == 0 || gct != (uint32_t)~LJ_TUDATA) {
      gc2_mark_scope_leave(&oscope);
      gc2_finreg_udata_obj_clear(node);
      if (lj_gc2_finreg_udata_unlink(g, prev, node, next)) {
	node = next;
	continue;
      }
      prev = NULL;
      node = gc2_finreg_udata_head_acq(g);
      continue;
    }
    if (lj_obj_gcflags(o) & LJ_GC_FINALIZED) {
      gc2_finreg_udata_obj_clear(node);
      if (lj_gc2_finreg_udata_unlink(g, prev, node, next)) {
	gc2_mark_scope_leave(&oscope);
	node = next;
	continue;
      }
      gc2_mark_scope_leave(&oscope);
      prev = NULL;
      node = gc2_finreg_udata_head_acq(g);
      continue;
    }
    if (!all) {
      marked = lj_gc2_ismarked(g, o);
      if (LJ_UNLIKELY(marked < 0)) {
	gc2_finreg_udata_finalizer_lookup_retry(g, node, o);
	gc2_mark_scope_leave(&oscope);
	prev = node;
	node = next;
	continue;
      }
      if (marked > 0) {
	gc2_mark_scope_leave(&oscope);
	prev = node;
	node = next;
	continue;
      }
    }
    {
      GCtab *mt = lj_udata_metatable_acq(gco2ud(o));
      int lookup_status = gc2_finreg_udata_finalizer_lookup(g, mt, &mmv);
      flags = lj_obj_gcflags(o);
      finreg = (flags & LJ_GC_UDATA_FINREG) != 0;
      if (LJ_UNLIKELY(lookup_status == LJ_TAB_GC_LOOKUP_RETRY)) {
	gc2_finreg_udata_finalizer_lookup_retry(g, node, o);
	gc2_mark_scope_leave(&oscope);
	prev = node;
	node = next;
	continue;
      }
      if (lookup_status == LJ_TAB_GC_LOOKUP_ABSENT) {
	if (finreg)
	  gc2_finreg_udata_set_admitted(g, o, 0);
	markfinalized(o);  /* Side-list no-finalizer userdata is done. */
	gc2_finreg_udata_obj_clear(node);
	if (lj_gc2_finreg_udata_unlink(g, prev, node, next)) {
	  gc2_mark_scope_leave(&oscope);
	  node = next;
	  continue;
	}
	gc2_mark_scope_leave(&oscope);
	prev = NULL;
	node = gc2_finreg_udata_head_acq(g);
	continue;
      }
      if (!finreg)
	(void)gc2_finreg_udata_set_admitted(g, o, 1);
      lj_assertG(lookup_status == LJ_TAB_GC_LOOKUP_FOUND,
		 "bad userdata finalizer lookup status");
      osize = sizeudata(gco2ud(o));
    }
    /*
    ** 05 section 5.8: GC2 userdata FINREG identity is enough for
    ** discovery without userdata-chain membership.
    */
    unlink_status = gc2_finreg_udata_unlink_root(g, o, all);
    if (unlink_status == LJ_GC_ROOT_UNLINK_UNPROVEN) {
      /* Keep FINREG identity and the side-list node active for a later bounded
      ** retry. The shared unlinker may have severed an invalid prefix, so make
      ** the side-list identity a sweep root before leaving o unqueued. */
      gc2_finreg_queue_mark(g, o);
      gc2_mark_scope_leave(&oscope);
      prev = node;
      node = next;
      continue;
    }
    gc2_finreg_udata_obj_clear(node);
    gc2_finreg_udata_discovered_add(g, 1);
    gc2_finreg_udata_finalizer_enqueue_admitted(g, o, &mmv);
    m += osize;
    gc2_mark_scope_leave(&oscope);
    if (lj_gc2_finreg_udata_unlink(g, prev, node, next)) {
      node = next;
      continue;
    }
    prev = NULL;
    node = gc2_finreg_udata_head_acq(g);
  }
  return m;  /* 05 section 5.8: GC2-owned userdata FINREG discovery. */
}

static int lj_gc2_finreg_udata_dispatch(lua_State *L, global_State *g,
					GCobj *o, cTValue *fin)
{
  cTValue *mo;
  TValue motv;
  if (!L || !gc2_finreg_udata_obj_valid(g, o))
    return 0;
  if (gc2_finreg_dispatch_requeue(g, o) <= LJ_GC_ROOT_LINK_DEFER)
    return GC2_FINALIZER_DISPATCH_DEFER;
  if (lj_gc2_finreg_udata_set(g, o, 0) < 0)
    lj_gc2_finreg_udata_forget(g, o);
  if (fin) {
    copyTV(L, &motv, fin);
    mo = &motv;
  } else {
    mo = lj_meta_fasttv(g, lj_udata_metatable_acq(gco2ud(o)), MM_gc, &motv);
  }
  if (mo && !gc2_call_finalizer(g, L, mo, o))
    return -1;
  return 1;  /* 05 section 5.8: GC2-owned userdata dispatch resolution. */
}

static void lj_gc2_finreg_udata_queue(global_State *g, GCobj *o)
{
  if (!gc2_finreg_udata_obj_valid(g, o))
    return;
  gc2_finreg_queue_mark(g, o);
  gc2_finreg_udata_queued_add(g, 1);
}

void lj_gc2_test_finreg_udata_queue(global_State *g, GCobj *o)
{
  lj_gc2_finreg_udata_queue(g, o);
}

static GCobj *gc2_grey_pop(global_State *g)
{
  GCRef *stack;
  uint64_t top, bottom;
  GCRef *slot;
  GCobj *o;
  MSize cap;
  if (!g)
    return NULL;
  stack = gc2_grey_stack_acq(g);
  cap = gc2_grey_capacity_acq(g);
  if (!stack || cap == 0)
    return NULL;
  bottom = gc2_grey_bottom_rlx(g);
  top = gc2_grey_top_acq(g);
  top = gc2_grey_repair_span(g, cap, top, bottom);
  if (top >= bottom)
    return NULL;
  bottom--;
  gc2_grey_bottom_store_rlx(g, bottom);
  la_fence_seq();  /* Chase-Lev owner pop: order bottom before top load. */
  top = gc2_grey_top_acq(g);
  if (top <= bottom) {
    slot = &stack[(MSize)(bottom % cap)];
    o = gc2_queue_slot_load_acq(slot);
    if (top == bottom) {
      uint64_t expect = top;
      /* 05 section 5.6.3: single item is claimed through top. */
      if (!gc2_grey_top_cas(g, &expect, top + 1)) {
	o = NULL;
	top = expect;
	gc2_grey_bottom_rel(g, top);
      } else {
	gc2_grey_bottom_rel(g, top + 1);
      }
    }
    if (o)
      gc2_queue_slot_clear_rel(slot);
    return o;
  }
  gc2_grey_bottom_rel(g, top);
  return NULL;
}

#if defined(LJ_GC2_TEST_HELPERS)
static GCobj *lj_gc2_grey_steal(global_State *g)
{
  GCRef *stack;
  uint64_t top, bottom, expect;
  GCRef *slot;
  GCobj *o;
  MSize cap;
  if (!g)
    return NULL;
  stack = gc2_grey_stack_acq(g);
  cap = gc2_grey_capacity_acq(g);
  if (!stack || cap == 0)
    return NULL;
  /* 05 section 5.6.3: non-owner steal; deque growth is owner-quiesced. */
  top = gc2_grey_top_acq(g);
  la_fence_seq();  /* 05 section 5.6.3: order top before bottom snapshot. */
  bottom = gc2_grey_bottom_acq(g);
  top = gc2_grey_repair_span(g, cap, top, bottom);
  if (top >= bottom)
    return NULL;
  slot = &stack[(MSize)(top % cap)];
  o = gc2_queue_slot_load_acq(slot);
  expect = top;
  /* 05 section 5.6.3: steal claim linearizes through top. */
  if (!gc2_grey_top_cas(g, &expect, top + 1))
    return NULL;
  if (o)
    gc2_queue_slot_clear_rel(slot);
  return o;
}

GCobj *lj_gc2_test_grey_steal(global_State *g)
{
  return lj_gc2_grey_steal(g);
}

int lj_gc2_test_grey_push(global_State *g, GCobj *o)
{
  return g && o ? gc2_grey_push(g, o) : 0;
}
#endif

static void gc2_ssb_activate(TGState *tg, GC2SSBNode *node)
{
  lj_gc2_ssb_next_rel(node, NULL);
  lj_gc2_ssb_count_rel(node, 0);
  lj_gc2_ssb_remembered_rel(node, 0);
  lj_tg_ssb_active_rel(tg, node);
  /* 05 section 5.6.2: publish active SSB cursor reset. */
  lj_tg_ssb_base_rel(tg, node->slot);
  lj_tg_ssb_end_rel(tg, node->slot + TG_GC2_SSB_SLOTS);
  lj_tg_ssb_next_rel(tg, node->slot);
}

static void gc2_ssb_publish(global_State *g, GC2SSBNode *node)
{
  TGState *owner = lj_gc2_ssb_owner_acq(node);
  GC2SSBNode *head = gc2_ssb_head_acq(g);
  if (owner && !(node->pad & TG_GC2_SSB_DYNAMIC))
    (void)lj_tg_ssb_refs_add(owner, 1);
  do {
    lj_gc2_ssb_next_rel(node, head);
  } while (!gc2_ssb_head_cas(g, &head, node));
  lj_gc2_worker_wake(g);  /* 05 section 5.6.3 parked worker scheduler. */
}

static uint32_t gc2_recycle_published_ssb_for_flush(global_State *g,
						    TGState *tg)
{
  uint32_t phase, moved;
  if (!g || !tg || lj_tg_ssb_free_acq(tg) != NULL)
    return 0;
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK)
    return 0;
  if (!gc2_worker_claim_count_busy(g))
    return 0;
  /*
  ** SSB flush needs a fresh node before it can publish the active node. Convert
  ** one published SSB node while holding the grey-deque owner gate; do not drain
  ** arbitrary grey work from this writer-side overflow path.
  */
  moved = gc2_drain_published_ssb_to_grey(g, TG_GC2_SSB_SLOTS);
  if (moved) {
    gc2_worker_runs_add(g, 1);
    gc2_worker_ssb_converted_add(g, moved);
  }
  gc2_worker_release(g);
  return moved;
}

static uint32_t gc2_flush_ssb(global_State *g, TGState *tg, int allow_drain)
{
  GC2SSBNode *node, *fresh;
  GCRef *base, *next;
  uint32_t n;
  if (!g || !tg)
    return 0;
  node = lj_tg_ssb_active_acq(tg);
  if (!node)
    return 0;
  base = lj_tg_ssb_base_acq(tg);
  next = lj_tg_ssb_next_acq(tg);
  if (!base || !next)
    return 0;
  n = (uint32_t)(next - base);
  if (n == 0)
    return 0;
  fresh = lj_tg_ssb_free_pop(tg);
  while (!fresh && allow_drain &&
	 gc2_recycle_published_ssb_for_flush(g, tg) != 0) {
    fresh = lj_tg_ssb_free_pop(tg);
  }
  if (!fresh)
    return 0;
  lj_assertG(lj_gc2_ssb_remembered_acq(node) <= n,
	     "SSB remembered suffix exceeds published count");
  lj_gc2_ssb_count_rel(node, n);
  gc2_ssb_publish(g, node);
  gc2_ssb_published_add(g, 1);
  gc2_ssb_items_published_add(g, n);
  gc2_ssb_activate(tg, fresh);
  return n;
}

uint32_t lj_gc2_flush_ssb(global_State *g, TGState *tg)
{
  return gc2_flush_ssb(g, tg, 1);
}

uint32_t lj_gc2_flush_ssb_detach(global_State *g, TGState *tg)
{
  GC2SSBNode *node;
  GCRef *base, *next, *end;
  uint32_t n;
  if (!g || !tg)
    return 0;
  node = lj_tg_ssb_active_acq(tg);
  base = lj_tg_ssb_base_acq(tg);
  next = lj_tg_ssb_next_acq(tg);
  end = lj_tg_ssb_end_acq(tg);
  if (!node || !base || !next || !end || next < base || next > end)
    return 0;
  n = (uint32_t)(next - base);
  if (n == 0)
    return 0;
  /* Detach is a terminal rotation: publication must not depend on malloc or
  ** on obtaining a replacement active buffer. The embedded-node pin keeps TG
  ** storage alive until this exact node is consumed. */
  lj_assertG(lj_gc2_ssb_remembered_acq(node) <= n,
	     "detached SSB remembered suffix exceeds published count");
  lj_gc2_ssb_count_rel(node, n);
  lj_tg_ssb_active_rel(tg, NULL);
  lj_tg_ssb_base_rel(tg, NULL);
  lj_tg_ssb_next_rel(tg, NULL);
  lj_tg_ssb_end_rel(tg, NULL);
  gc2_ssb_publish(g, node);
  gc2_ssb_published_add(g, 1);
  gc2_ssb_items_published_add(g, n);
  return n;
}

static int gc2_ssb_push_tg(global_State *g, TGState *tg, GCobj *o,
			    int allow_drain)
{
  GCRef *next, *end;
  int remembered;
  if (!g || !tg || !o)
    return 0;
  next = lj_tg_ssb_next_acq(tg);
  end = lj_tg_ssb_end_acq(tg);
  if (!next || !end)
    return 0;
  /* Every legal IDLE SSB publication in generational mode is remembered-set
  ** work. Classify at the slot LP so the test hook and production barriers
  ** share the same exact queue representation. */
  remembered = gc2_phase_acq(g) == LJ_GC2_IDLE &&
	       gc2_generational_acq(g) != 0;
  if (next == end) {
    if (gc2_flush_ssb(g, tg, allow_drain) == 0)
      return 0;
    next = lj_tg_ssb_next_acq(tg);
    end = lj_tg_ssb_end_acq(tg);
    if (!next || !end || next == end)
      return 0;
  }
  gc2_queue_slot_store_rel(next, o);
  if (remembered)
    (void)lj_gc2_ssb_remembered_add(lj_tg_ssb_active_acq(tg));
  /* 05 section 5.6.2: slot release-published before cursor advance. */
  lj_tg_ssb_next_rel(tg, next + 1);
  return 1;
}

static int gc2_ssb_push(global_State *g, GCobj *o, int allow_drain)
{
  return gc2_ssb_push_tg(g, G2TG(g), o, allow_drain);
}

static int lj_gc2_ssb_push(global_State *g, GCobj *o)
{
  return gc2_ssb_push(g, o, 1);
}

int lj_gc2_fnew_certify_pair_nodrain(global_State *g, TGState *tg,
				      GCproto *pt, GCtab *env)
{
  GC2SSBNode *node;
  GCRef *base;
  GCRef *next, *end;
  uint32_t cycle;

  /* Only the current OS-thread owner may append its active SSB cursor. Caller
  ** provenance is part of certificate authority, not merely an optimization
  ** precondition. */
  if (!g || !tg || !pt || !env || tg->gl != g || tg != G2TG(g))
    return 0;
  /* This is certificate authority, not a best-effort barrier. Require the
  ** exact cooperative-MARK native-entry predicate also checked by emitted
  ** code. The C constructor's ordinary barriers remain authoritative on every
  ** miss or failed seed. */
  if (gc2_phase_acq(g) != LJ_GC2_MARK ||
      !lj_tg_mark_active_acq(tg) || !lj_tg_alloc_black_acq(tg) ||
      gc2_jit_phase_gate_acq(g) == 0)
    return 0;
  cycle = gc2_cycle_acq(g);
  if (cycle == 0 || gc2_jit_mark_resume_acq(g) != cycle)
    return 0;

  node = lj_tg_ssb_active_acq(tg);
  base = lj_tg_ssb_base_acq(tg);
  next = lj_tg_ssb_next_acq(tg);
  end = lj_tg_ssb_end_acq(tg);
  if (!node || lj_gc2_ssb_owner_acq(node) != tg ||
      lj_gc2_ssb_remembered_acq(node) != 0 ||
      base != node->slot || end != node->slot + TG_GC2_SSB_SLOTS || !next ||
      next < base || next > end ||
      (size_t)(end - next) < 2u)
    return 0;

  /* Raw SSB entries are explicit traversal requests even for already-marked
  ** graph-bearing objects. Store both slots before one release cursor advance;
  ** there is no allocation, rotation, drain, recovery fallback, or partial
  ** publication on the full-buffer path. */
  gc2_queue_slot_store_rel(next, obj2gco(pt));
  gc2_queue_slot_store_rel(next + 1, obj2gco(env));
  lj_tg_ssb_next_rel(tg, next + 2);

  /* Cursor publication retains the pair. Cache pointers only compare exact
  ** identity and become usable through the final release cycle store. */
  lj_tg_fnew_cert_publish_rel(tg, pt, env, cycle);
  return 1;
}

static int gc2_publish_mutator_(global_State *g, GCobj *o,
				const GC2MarkScope *scope, int allow_drain)
{
  if (gc2_ssb_push(g, o, allow_drain) ||
      gc2_recovery_publish_scoped(g, o, scope))
    return 1;
  /* A supported arena/main-thread object always has one allocation-free
  ** recovery identity. An unexpected classification failure must veto all
  ** physical reclamation rather than degrade into an assertion-only drop. */
  gc2_recovery_fail_closed(g);
  return 0;
}

static int gc2_publish_mutator_scoped(global_State *g, GCobj *o,
				      const GC2MarkScope *scope)
{
  return gc2_publish_mutator_(g, o, scope, 1);
}

static int gc2_publish_mutator(global_State *g, GCobj *o)
{
  return gc2_publish_mutator_(g, o, NULL, 1);
}

static int gc2_publish_mutator_nodrain(global_State *g, GCobj *o)
{
  return gc2_publish_mutator_(g, o, NULL, 0);
}

void lj_gc2_thread_owner_releasing(global_State *g, lua_State *L,
				     uint32_t tid)
{
  TGState *tg;
  int pushed = 0;
  if (!g || !L || !lj_thr_id_is_owner(tid) ||
      lj_state_owner_acq(L) != LJ_THREAD_GCSCAN ||
      lj_state_gcprep_state_acq(L) != LJ_STATE_GCPREP_NONE ||
      !gc2_thread_needscan(L))
    return;

  /*
  ** The releasing thread still owns the state through the temporary GCSCAN
  ** sentinel. Prefer its exact TLS TG without using G2TG's foreign-thread
  ** main-TG fallback: only the OS owner may advance an active SSB cursor.
  ** A synthetic/API release without that exact TLS identity transfers the
  ** object directly to the MPMC recovery plane instead. Both representations
  ** pin the state before the caller publishes owner zero.
  */
  tg = lj_thr_get_tg();
  if (tg && tg->gl == g && lj_tg_tid_acq(tg) == tid &&
      !lj_tg_flags_test_acq(tg, TGF_DEAD))
    pushed = gc2_ssb_push_tg(g, tg, obj2gco(L), 0);
  if (!pushed)
    pushed = gc2_recovery_publish(g, obj2gco(L));
  if (!pushed)
    gc2_recovery_fail_closed(g);
  lj_gc2_worker_wake(g);
}

static int gc2_publish_worker(global_State *g, GCobj *o)
{
  if (gc2_grey_push(g, o) || gc2_recovery_publish(g, o))
    return 1;
  gc2_recovery_fail_closed(g);
  return 0;
}

static int gc2_ssb_mark_one(global_State *g, GCobj *o)
{
  if (o) {
    GC2MarkScope scope;
    uint32_t gct;
    int result, status, traversable, retry = 0;
    gc2_mark_scope_init(&scope);
    if (mainthread_acq(g) && o == obj2gco(mainthread_acq(g)))
      return gc2_publish_worker(g, o);
    status = gc2_markobj_preserve_queue_status(g, o, &gct, &traversable,
					       &scope, &retry);
    if (status == GC2_MARK_DEAD && retry) {
      gc2_mark_scope_leave(&scope);
      return 0;  /* Retain the owned SSB slot and its exact table token. */
    }
    if (status == GC2_MARK_DEAD) {
      gc2_mark_scope_leave(&scope);
      return 1;
    }
    if (gct == (uint32_t)~LJ_TTAB &&
	!(lj_obj_gcflags(o) & LJ_GC_NEEDSCAN) &&
	gc2_table_scan_current(g, gco2tab(o))) {
      /*
      ** A table can be reached twice while a parent table is being traversed.
      ** The first edge marks and grays the child; a later edge may enqueue a
      ** rescan entry through the owner SSB. If the grey child is scanned before
      ** that rescan entry is consumed, NEEDSCAN has been cleared and the table
      ** stamp already covers this cycle. The exact per-table token must also be
      ** absent: a delayed old hint-clear can temporarily erase NEEDSCAN after a
      ** new COUNTED token and this very queue locator were installed. All three
      ** observations stay inside the retained body lease.
      */
      if (lj_tab_gc2_rescan_state_acq(gco2tab(o)) == LJ_TAB_RESCAN_NONE) {
	gc2_mark_scope_leave(&scope);
	return 1;
      }
    }
    /* An SSB slot is already an explicit traversal request. Consume it for
    ** both live statuses; only ordinary edge discovery is NEW-only. */
    if (gc2_gct_may_traverse(gct) &&
	(traversable || gct == (uint32_t)~LJ_TUDATA)) {
      /* Userdata remains graphless for allocator classification, but semantic
      ** traversal is worker-owned just like every other container. */
      result = gc2_publish_worker(g, o);
      gc2_mark_scope_leave(&scope);
      return result;
    }
    gc2_mark_scope_leave(&scope);
  }
  return 1;
}

static void gc2_ssb_recycle_node(GC2SSBNode *node)
{
  TGState *owner = lj_gc2_ssb_owner_acq(node);
  lj_gc2_ssb_count_rel(node, 0);
  lj_gc2_ssb_remembered_rel(node, 0);
  if (node->pad & TG_GC2_SSB_DYNAMIC) {
    free(node);
    return;
  }
  if (owner) {
    uint32_t old;
    /* The published reference pins owner through this final access. If detach
    ** won, do not republish into its free list; otherwise publish the node
    ** before dropping the pin so a racing detach/fini observes it. */
    if (lj_tg_flags_test_acq(owner, TGF_DEAD))
      lj_gc2_ssb_next_rel(node, NULL);
    else
      lj_tg_ssb_free_push(owner, node);
    old = lj_tg_ssb_refs_sub(owner, 1);
    lj_assertX(old != 0, "TG SSB publication reference underflow");
    UNUSED(old);
  } else {
    lj_gc2_ssb_next_rel(node, NULL);
  }
}

static uint32_t gc2_discard_published_ssb(global_State *g)
{
  GC2SSBNode *node;
  uint32_t nitems = 0, nnodes = 0;
  if (!g)
    return 0;
  (void)gc2_ssb_consumer_enter(g);
  node = gc2_ssb_drain_xchg_acqrel(g, NULL);
  for (;;) {
    while (node) {
      GC2SSBNode *next = lj_gc2_ssb_next_acq(node);
      uint32_t count = lj_gc2_ssb_count_acq(node);
      while (count > 0) {
	GCRef *slot = &node->slot[count - 1u];
	GCobj *o = gc2_queue_slot_load_acq(slot);
	gc2_rescan_pending_clear_cycle(g, o);
	gc2_queue_slot_clear_rel(slot);
	count--;
	nitems++;
      }
      lj_gc2_ssb_count_rel(node, 0);
      nnodes++;
      gc2_ssb_recycle_node(node);
      node = next;
    }
    node = gc2_ssb_head_xchg_acqrel(g, NULL);
    if (!node)
      break;
  }
  if (nnodes)
    gc2_ssb_drained_add(g, nnodes);
  if (nitems)
    gc2_ssb_items_drained_add(g, nitems);
  (void)gc2_ssb_consumer_leave(g);
  return nitems;
}

static uint32_t gc2_discard_active_ssb_(global_State *g, int include_dead)
{
  TGState *tg;
  uint32_t nitems = 0;
  if (!g)
    return 0;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    GC2SSBNode *node;
    GCRef *base, *next;
    if (!include_dead && lj_tg_flags_test_acq(tg, TGF_DEAD))
      continue;
    base = lj_tg_ssb_base_acq(tg);
    next = lj_tg_ssb_next_acq(tg);
    node = lj_tg_ssb_active_acq(tg);
    if (!node || !base || !next)
      continue;
    while (next > base) {
      GCRef *slot = next - 1;
      GCobj *o = gc2_queue_slot_load_acq(slot);
      gc2_rescan_pending_clear_cycle(g, o);
      gc2_queue_slot_clear_rel(slot);
      next = slot;
      nitems++;
    }
    lj_tg_ssb_next_rel(tg, base);
    /* Terminal discard consumes the tagged suffix without tracing it. A later
    ** freeall destructor may reuse this active node before the post-freeall
    ** discard, so its remembered count must follow the cleared cursor. */
    lj_gc2_ssb_remembered_rel(node, 0);
  }
  if (nitems)
    gc2_ssb_items_drained_add(g, nitems);
  return nitems;
}

uint32_t lj_gc2_shutdown_discard_ssb(global_State *g)
{
  uint32_t n;
  if (!g)
    return 0;
  /* Terminal single-thread drain: threading children and collector workers
  ** are already joined. Do not rotate/allocate a fresh active node. Clear all
  ** published and owner-local references while their objects are still valid,
  ** then let the TG registry release nodes whose embedded publications were
  ** the last lifetime pin. */
  lj_assertG(gc2_n_threads_acq(g) <= 1,
	     "terminal SSB drain still has live publishers");
  lj_assertG(gc2_worker_active_acq(g) == 0,
	     "terminal SSB drain overlaps a worker owner");
  lj_assertG(gc2_ssb_consumer_active_acq(g) == 0,
	     "terminal SSB drain overlaps a queue consumer");
  n = gc2_discard_published_ssb(g);
  n += gc2_discard_active_ssb_(g, 1);
  (void)lj_tg_reclaim_dead(g);
  return n;
}

static LJ_NOINLINE uint32_t gc2_drain_published_ssb_to_grey(global_State *g,
							    uint32_t limit)
{
  GC2SSBNode *node;
  uint32_t nitems = 0, nnodes = 0;
  if (!g || limit == 0)
    return 0;
  (void)gc2_ssb_consumer_enter(g);
  node = gc2_ssb_drain_xchg_acqrel(g, NULL);
  if (!node)
    node = gc2_ssb_head_xchg_acqrel(g, NULL);
  while (node && nitems < limit) {
    GC2SSBNode *next = lj_gc2_ssb_next_acq(node);
    uint32_t count = lj_gc2_ssb_count_acq(node);
    uint32_t remembered = lj_gc2_ssb_remembered_acq(node);
    lj_assertG(remembered <= count,
	       "published SSB remembered suffix exceeds count");
    while (count > 0 && nitems < limit) {
      GCRef *slot = &node->slot[count - 1u];
      GCobj *o = gc2_queue_slot_load_acq(slot);
      /* Slot ownership transfers only after the semantic object is either on
      ** grey or durably represented by its allocation-free recovery state. */
      if (!gc2_ssb_mark_one(g, o))
	break;
      gc2_recovery_test_pause_at(LJ_GC2_RECOVERY_TEST_SSB_COMMITTED);
      gc2_queue_slot_clear_rel(slot);
      count--;
      lj_gc2_ssb_count_rel(node, count);
      if (remembered > 0) {
	remembered--;
	if (gc2_phase_acq(g) == LJ_GC2_MARK &&
	    gc2_cycle_minor_requested_acq(g))
	  gc2_remembered_drained_add(g, 1);
      }
      nitems++;
    }
    lj_gc2_ssb_remembered_rel(node, remembered);
    if (count == 0) {
      nnodes++;
      gc2_ssb_recycle_node(node);
      node = next;
    } else {
      lj_gc2_ssb_next_rel(node, next);
      break;
    }
  }
  if (node)
    gc2_ssb_drain_rel(g, node);
  if (nnodes) {
    gc2_ssb_drained_add(g, nnodes);
  }
  if (nitems)
    gc2_ssb_items_drained_add(g, nitems);
  (void)gc2_ssb_consumer_leave(g);
  return nitems;
}

static uint32_t gc2_drain_active_ssb_to_grey(global_State *g, TGState *tg,
					     uint32_t limit)
{
  GC2SSBNode *node;
  GCRef *base, *next;
  uint32_t n = 0, remembered;
  if (!g || !tg || limit == 0)
    return 0;
  node = lj_tg_ssb_active_acq(tg);
  base = lj_tg_ssb_base_acq(tg);
  next = lj_tg_ssb_next_acq(tg);
  if (!node || !base || !next)
    return 0;
  remembered = lj_gc2_ssb_remembered_acq(node);
  lj_assertG(remembered <= (uint32_t)(next - base),
	     "active SSB remembered suffix exceeds cursor");
  while (n < limit && next > base) {
    GCRef *slot = next - 1;
    GCobj *o = gc2_queue_slot_load_acq(slot);
    if (!gc2_ssb_mark_one(g, o))
      break;
    gc2_recovery_test_pause_at(LJ_GC2_RECOVERY_TEST_SSB_COMMITTED);
    gc2_queue_slot_clear_rel(slot);
    if (remembered > 0) {
      remembered--;
      if (gc2_phase_acq(g) == LJ_GC2_MARK &&
	  gc2_cycle_minor_requested_acq(g))
	gc2_remembered_drained_add(g, 1);
    }
    next = slot;
    /* 05 section 5.7.1: publish slot processed before cursor retreat. */
    lj_tg_ssb_next_rel(tg, next);
    n++;
  }
  lj_gc2_ssb_remembered_rel(node, remembered);
  return n;
}

static uint32_t gc2_drain_ssb_owned(global_State *g)
{
  uint32_t total = 0, converted = 0;
  if (!g)
    return 0;
  for (;;) {
    TGState *tg = G2TG(g);
    uint32_t published, active = 0, grey1, grey2, recovered, step;
    published = gc2_drain_published_ssb_to_grey(g, ~(uint32_t)0);
    grey1 = gc2_drain_grey(g, ~(uint32_t)0);
    if (tg && !lj_tg_flags_test_acq(tg, TGF_DEAD))
      active = gc2_drain_active_ssb_to_grey(g, tg, ~(uint32_t)0);
    grey2 = gc2_drain_grey(g, ~(uint32_t)0);
    recovered = gc2_recovery_drain_owned(g, ~(uint32_t)0);
    step = published + active + grey1 + grey2 + recovered;
    if (step == 0)
      break;
    total += step;
    converted += published + active;
    if (gc2_thread_scan_needscan_pending_acq(g) != 0)
      break;
  }
  if (total)
    gc2_worker_runs_add(g, 1);  /* Temporary single-worker scaffold. */
  return converted;
}

static uint32_t lj_gc2_drain_ssb(global_State *g)
{
  uint32_t nitems;
  if (!g)
    return 0;
  if (!gc2_worker_claim_count_busy(g))
    return 0;
  nitems = gc2_drain_ssb_owned(g);
  gc2_worker_release(g);
  return nitems;
}

uint32_t lj_gc2_assist(global_State *g, TGState *tg)
{
  uint32_t phase, shift, limit, assist_expect = 0;
  uint32_t n = 0, converted = 0, recovered = 0, weak = 0;
  if (!g || !tg || lj_tg_gc_assist_acq(tg))
    return 0;
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK)
    return 0;
  if (!lj_gc2_hard_limit_reached(g))
    return 0;
  /*
  ** Assist-owned SSB conversion and mark tracing still mutate the same global
  ** grey deque as the worker/recycle paths. Claim the worker token first, then
  ** nest assist_active entirely inside it. This removes flag-only preemption
  ** windows which could strand a fair MARK-close intent behind no actual owner.
  */
  if (!gc2_worker_claim_count_busy(g))
    return 0;
  if (!gc2_assist_active_cas(g, &assist_expect, 1)) {
    gc2_worker_release(g);
    return 0;  /* Current global grey deque has one owner side. */
  }
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK) {
    gc2_assist_active_rel(g, 0);
    gc2_worker_release(g);
    return 0;
  }
  lj_tg_gc_assist_store_rlx(tg, 1);
  gc2_assist_runs_add(g, 1);  /* 05 section 5.11 telemetry. */
  shift = gc2_assist_shift_acq(g);
  if (shift > 8u)
    shift = 8u;
  limit = 1u << shift;
  (void)lj_gc2_flush_alloc(g, tg);
  while (n + converted + recovered < limit) {
    uint32_t left = limit - (n + converted + recovered);
    uint32_t drained = gc2_drain_grey(g, left);
    if (drained) {
      n += drained;
      continue;
    }
    if (gc2_drain_active_ssb_to_grey(g, tg, 1) ||
	gc2_drain_published_ssb_to_grey(g, 1)) {
      converted++;
      continue;
    }
    if (!gc2_recovery_drain_owned(g, 1))
      break;
    recovered++;
  }
  if (phase == LJ_GC2_WEAK) {
    uint32_t work = n + converted + recovered;
    if (work < limit)
      weak = lj_gc2_weak_drain(g, limit - work);  /* 05 section 5.11. */
  }
  if (n)
    gc2_assist_grey_drained_add(g, n);
  if (converted)
    gc2_assist_ssb_converted_add(g, converted);
  if (weak)
    gc2_assist_weak_drained_add(g, weak);
  lj_tg_gc_assist_store_rlx(tg, 0);
  gc2_assist_active_rel(g, 0);
  gc2_worker_release(g);
  return n + recovered + weak;
}

static int gc2_ssb_published_empty(global_State *g)
{
  if (!g)
    return 1;
  if (gc2_ssb_head_acq(g) != NULL || gc2_ssb_drain_acq(g) != NULL ||
      gc2_ssb_consumer_active_acq(g) != 0)
    return 0;
  /* A consumer publishes its count before detaching either list and drops it
  ** only after publishing the remainder. The second snapshot rejects both
  ** sides of that handoff without making a worker-owned chain globally walkable. */
  la_fence_seq();
  return gc2_ssb_head_acq(g) == NULL &&
	 gc2_ssb_drain_acq(g) == NULL &&
	 gc2_ssb_consumer_active_acq(g) == 0;
}

static int lj_gc2_ssb_empty(global_State *g)
{
  TGState *tg;
  TGState *curtg;
  if (!g)
    return 1;
  if (!gc2_recovery_empty(g))
    return 0;
  if (!gc2_ssb_published_empty(g))
    return 0;  /* 05 section 5.7.1 SSB-empty fixpoint predicate. */
  if (!gc2_grey_empty(g))
    return 0;
  /*
  ** The main TG is not a secondary-thread list node, but root scans and API
  ** calls can publish table rescans to its active SSB. Include it in the
  ** fixpoint predicate so table NEEDSCAN is always backed by visible work.
  */
  tg = g->main_tg;
  if (tg && !lj_tg_flags_test_acq(tg, TGF_DEAD)) {
    GCRef *next = lj_tg_ssb_next_acq(tg);
    GCRef *base = lj_tg_ssb_base_acq(tg);
    if (next != base)
      return 0;
  }
  curtg = G2TG(g);
  if (curtg && curtg != g->main_tg &&
      !lj_tg_flags_test_acq(curtg, TGF_DEAD)) {
    GCRef *next = lj_tg_ssb_next_acq(curtg);
    GCRef *base = lj_tg_ssb_base_acq(curtg);
    if (next != base)
      return 0;
  }
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    GCRef *next, *base;
    if (lj_tg_flags_test_acq(tg, TGF_DEAD))
      continue;
    next = lj_tg_ssb_next_acq(tg);
    base = lj_tg_ssb_base_acq(tg);
    if (next != base)
      return 0;
  }
  return 1;
}

static int gc2_ssb_detached_empty(global_State *g)
{
  return !g || (gc2_worker_active_acq(g) == 0 &&
		gc2_ssb_published_empty(g) && gc2_recovery_empty(g));
}

int lj_gc2_test_ssb_push(global_State *g, GCobj *o)
{
  return lj_gc2_ssb_push(g, o);
}

uint32_t lj_gc2_test_ssb_drain(global_State *g)
{
  return lj_gc2_drain_ssb(g);
}

int lj_gc2_test_ssb_empty(global_State *g)
{
  return lj_gc2_ssb_empty(g);
}

#if defined(LJ_GC2_TEST_HELPERS)
void lj_gc2_test_recovery_fail_closed(global_State *g)
{
  gc2_recovery_fail_closed(g);
}

int lj_gc2_test_recovery_publish(global_State *g, GCobj *o)
{
  return gc2_recovery_publish(g, o);
}

int lj_gc2_test_publish_mutator_reader(global_State *g, GCobj *o,
				       const LJHugeReader *reader)
{
  GC2MarkScope scope;
  gc2_mark_scope_init(&scope);
  if (!reader)
    return 0;
  scope.admission = GC2_SCOPE_HUGE_READER;
  scope.huge = *reader;
  /* The fixture retains ownership of reader; this borrowed scope must never
  ** release or otherwise mutate the counted token. */
  return gc2_publish_mutator_scoped(g, o, &scope);
}

int lj_gc2_test_recovery_mutating_recheck(GCArena *a, uint32_t start)
{
  return gc2_recovery_small_mutating_recheck(a, start);
}

uint32_t lj_gc2_test_recovery_drain(global_State *g, uint32_t limit)
{
  uint32_t n;
  if (!g || limit == 0 || !gc2_worker_claim_count_busy(g))
    return 0;
  n = gc2_recovery_drain_owned(g, limit);
  gc2_worker_release(g);
  return n;
}

int lj_gc2_test_recovery_state(global_State *g, GCobj *o)
{
  GCArena *a;
  uint32_t start;
  lua_State *mainL;
  if (!g || !o)
    return -1;
  mainL = mainthread_acq(g);
  if (mainL && o == obj2gco(mainL))
    return (int)gc2_recovery_main_state_acq(g);
  a = lj_arena_of(o);
  if (gc2_small_arena_known(g, a)) {
    if (!gc2_small_lifetime_nearest(
	  a, lj_arena_cellof(o), &start, NULL))
      return -1;
    return (int)lj_arena_recovery_state_acq(a, start);
  }
  if (lj_gc2_smr_read_try(g)) {
    HugeTab *ht;
    void *base = NULL;
    int state = -1;
    ht = gc2_huge_recovery_table(g, o, &base, NULL);
    if (ht && base == (void *)o)
      state = lj_arena_hugetab_recovery_state_acq(ht, base, NULL);
    lj_gc2_smr_read_leave(g);
    return state;
  }
  return -1;
}

uint32_t lj_gc2_test_recovery_discard_terminal(global_State *g)
{
  return gc2_recovery_discard_terminal(g);
}
#endif

static int gc2_barrier_active_g(global_State *g)
{
  TGState *tg;
  uint32_t phase;
  if (!g)
    return 0;
  tg = G2TG(g);
  if (!tg || !lj_tg_mark_active_acq(tg))
    return 0;
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK)
    return 0;
  return 1;
}

static int gc2_remember_active_g(global_State *g)
{
  TGState *tg;
  if (!g || gc2_phase_acq(g) != LJ_GC2_IDLE ||
      gc2_generational_acq(g) == 0)
    return 0;
  tg = G2TG(g);
  return tg && lj_tg_mark_active_acq(tg);
}

static void gc2_remember_obj(global_State *g, GCobj *o)
{
  if (!g || !o)
    return;
  if (gc2_publish_mutator_nodrain(g, o)) {
    gc2_remembered_pushed_add(g, 1);
  } else {
    gc2_remembered_overflows_add(g, 1);
    lj_gc2_force_major(g);
    (void)lj_gc2_request_cycle(g, G2TG(g));
  }
}

static int gc2_remember_pair_match(global_State *g, GCobj *parent,
				   GCobj *child)
{
  if (gc2_minor_sweep_enabled_acq(g) == 0 || child == NULL)
    return 1;
  if (parent && lj_gc2_ismarked(g, parent) <= 0)
    return 0;
  return lj_gc2_ismarked(g, child) == 0;
}

static void gc2_remember_pair(global_State *g, GCobj *parent, GCobj *child)
{
  GCobj *root = parent ? parent : child;
  if (!g || !root || !gc2_remember_active_g(g))
    return;
  gc2_remembered_barriers_add(g, 1);
  if (!gc2_remember_pair_match(g, parent, child)) {
    gc2_remembered_filtered_add(g, 1);
    return;
  }
  gc2_remember_obj(g, root);
}

static void gc2_sweep_barrier_obj(global_State *g, GCobj *o)
{
  if (g && o && gc2_phase_acq(g) == LJ_GC2_SWEEP)
    (void)lj_gc2_trace_sweep_root(g, o);
}

static int gc2_sweep_barrier_tv(global_State *g, cTValue *tv)
{
  TValue snap;
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP || !tv)
    return 0;
  lj_tv_load_acq(&snap, tv);
  if (tvisgcv(&snap))
    (void)gc2_trace_sweep_tv_edge(g, &snap, 0);
  return 1;
}

void lj_gc2_remember_root(global_State *g, GCobj *o)
{
  if (!g || !o || gc2_phase_acq(g) != LJ_GC2_IDLE ||
      gc2_generational_acq(g) == 0)
    return;
  gc2_remember_obj(g, o);
}

static int gc2_tab_weak_mode(global_State *g, GCtab *t, GCtab *mt,
			     int mark_mode, int smr_held,
			     int semantic_admission, int *retryp)
{
  GC2MarkScope mtscope, strscope;
  GCtab *admitted_mt;
  GCstr *modes = NULL;
  int weak = 0;
  TValue modev;
  int own_smr = 0, status, strstatus, lookup_status;
  UNUSED(mark_mode);  /* __mode is a strong metadata edge whenever observed. */
  if (retryp)
    *retryp = 0;
  if (!g || !mt)
    return 0;
  status = semantic_admission ? gc2_expected_tab_scoped_status(
    g, obj2gco(mt), &admitted_mt, &mtscope) :
    gc2_expected_tab_worker_scoped_status(
      g, obj2gco(mt), &admitted_mt, &mtscope);
  if (status == GC2_TAB_SCOPE_RETRY) {
    if (retryp)
      *retryp = 1;
    return 0;
  }
  if (status != GC2_TAB_SCOPE_VALID)
    return 0;
  mt = admitted_mt;
  if (!smr_held && !lj_gc2_smr_read_try(g)) {
    gc2_mark_scope_leave(&mtscope);
    if (retryp)
      *retryp = 1;
    return 0;
  }
  own_smr = !smr_held;
  setnilV(&modev);
  lookup_status = LJ_TAB_GC_LOOKUP_ABSENT;
  if ((lj_tab_nomm_acq(mt) & (1u << MM_mode)) == 0)
    lookup_status = lj_tab_getstr_held_try(
      g, mt, mmname_str(g, MM_mode), &modev);
  if (LJ_UNLIKELY(lookup_status == LJ_TAB_GC_LOOKUP_RETRY)) {
    if (own_smr)
      lj_gc2_smr_read_leave(g);
    gc2_mark_scope_leave(&mtscope);
    if (retryp)
      *retryp = 1;
    return 0;
  }
  strstatus = tvisstr(&modev) ?
    (semantic_admission ? gc2_expected_string_scoped_status(
      g, gcV(&modev), &modes, &strscope) :
      gc2_expected_string_worker_scoped_status(
        g, gcV(&modev), &modes, &strscope)) : GC2_TAB_SCOPE_STALE;
  if (tvisstr(&modev) && strstatus != GC2_TAB_SCOPE_VALID) {
    /* An acquired table slot that names a string cannot be classified as
    ** strong merely because its exact string identity is between lifetime LPs.
    ** Replay the parent table instead; a malformed terminal edge is also
    ** fail-closed as retry and therefore cannot close weak processing early. */
    if (own_smr)
      lj_gc2_smr_read_leave(g);
    gc2_mark_scope_leave(&mtscope);
    if (retryp)
      *retryp = 1;
    return 0;
  }
  if (modes) {
    const char *modestr = strdata(modes);
    MSize i, len = modes->len;
    for (i = 0; i < len; i++) {
      int c = (unsigned char)modestr[i];
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
    }
    gc2_mark_scope_leave(&strscope);
  #if LJ_HASFFI
    if (weak && gc2_tab_is_ffi_fin(g, t))
      weak = (int)(~0u & ~LJ_GC_WEAKVAL);
  #endif
  }
  if (own_smr)
    lj_gc2_smr_read_leave(g);
  gc2_mark_scope_leave(&mtscope);
  return weak;
}

static int gc2_tab_weak_barrier_mode(global_State *g, GCtab *t)
{
  int retry = 0;
  int weak = lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK;
  if (weak)
    return weak;  /* 05 section 5.8: use captured P_WEAK mode. */
  weak = gc2_tab_weak_mode(g, t, lj_tab_metatable_acq(t), 1, 0, 1,
			   &retry);
  if (retry) {
    gc2_table_rescan_requeue(g, t);
    /* Unknown mode is conservatively all-weak for this point barrier. The
    ** barrier marks both operands, while the queued parent traversal restores
    ** its exact strong/weak policy before phase closure. */
    return LJ_GC_WEAK;
  }
  return weak;
}

static int gc2_tab_weak_write_candidate(global_State *g, GCtab *t)
{
  int weak, retry = 0;
  if (!g || !t)
    return 0;
  weak = lj_obj_gcflags(obj2gco(t)) & LJ_GC_WEAK;
  if (weak)
    return weak;
  weak = gc2_tab_weak_mode(g, t, lj_tab_metatable_acq(t), 0, 0, 1,
			   &retry);
  if (retry) {
    gc2_table_rescan_requeue(g, t);
    return LJ_GC_WEAK;
  }
  return weak;
}

int lj_gc2_weak_write_candidate(lua_State *L, GCtab *t)
{
  if (!L || !t)
    return 0;
  return gc2_tab_weak_write_candidate(G(L), t);
}

int lj_gc2_weak_write_begin(lua_State *L, GCtab *t)
{
  global_State *g;
  if (!lj_gc2_weak_write_candidate(L, t))
    return 0;
  g = G(L);
  (void)gc2_weak_write_active_add(g, 1);
  return 1;
}

void lj_gc2_weak_write_end(lua_State *L, int active)
{
  global_State *g;
  uint32_t old;
  if (!active || !L)
    return;
  g = G(L);
  old = gc2_weak_write_active_sub(g, 1);
  lj_assertG(old != 0, "gc2 weak write active underflow");
  UNUSED(old);
}

void lj_gc2_barrier_tv_g(global_State *g, cTValue *tv)
{
  TValue snap;
  if (tv) {
    lj_tv_load_acq(&snap, tv);
    if (tvisgcv(&snap)) {
      uint32_t phase = gc2_phase_acq(g);
      if (phase != LJ_GC2_IDLE) {
	GCobj *o = gcV(&snap);
	if (phase == LJ_GC2_SWEEP) {
	  /* A semantic root published while a destructive lifetime owner is
	  ** active must leave an exact recovery identity (or the sticky reclaim
	  ** veto). Ordinary mark admission can report DEAD for that bounded
	  ** arbitration window without doing either, which lets the caller's
	  ** subsequent validation manufacture a false nil. */
	  (void)gc2_trace_sweep_tv_edge(g, &snap, 0);
	} else {
	  int status = gc2_markobj_expected_status(
	    g, o, (uint32_t)~itype(&snap), NULL);
	  if (status == GC2_MARK_DEAD &&
	      gc2_phase_acq(g) == LJ_GC2_SWEEP) {
	    /* Close the MARK/WEAK -> SWEEP sampling edge as well. */
	    (void)gc2_trace_sweep_tv_edge(g, &snap, 0);
	  } else if (status == GC2_MARK_LIVE_ALREADY) {
	    gc2_thread_root_rescan_marked_obj(g, o);
	  }
	}
      } else {
	gc2_remember_pair(g, NULL, gcV(&snap));
      }
    }
  }
}

static LJ_AINLINE uint64_t gc2_tabstamp_pack(uint32_t cycle, uint32_t dirty)
{
  return ((uint64_t)cycle << 32) | (uint64_t)dirty;
}

static LJ_AINLINE uint32_t gc2_tabstamp_dirty(uint64_t state)
{
  return (uint32_t)state;
}

static LJ_AINLINE uint32_t gc2_tabstamp_cycle(uint64_t state)
{
  return (uint32_t)(state >> 32);
}

static LJGC2TabStamp *gc2_table_stamp(global_State *g, GCtab *t)
{
  if (!g || !t || g->allocf != lj_arena_allocf)
    return NULL;
  return lj_arena_gc2_stamp_acq(t);
}

static LJ_AINLINE uint32_t gc2_table_dirty_epoch(global_State *g, GCtab *t,
						 int *stamped)
{
  LJGC2TabStamp *s = gc2_table_stamp(g, t);
  if (stamped)
    *stamped = s != NULL;
  return s ? gc2_tabstamp_dirty(la_load64_acq(&s->state)) : 0;
}

static LJ_AINLINE int gc2_table_scan_publish(global_State *g, GCtab *t,
					     uint32_t cycle, uint32_t dirty)
{
  LJGC2TabStamp *s = gc2_table_stamp(g, t);
  uint64_t old;
  if (!s)
    return 0;
  old = la_load64_acq(&s->state);
  for (;;) {
    uint64_t next;
    if (gc2_tabstamp_dirty(old) != dirty)
      return 0;
    next = gc2_tabstamp_pack(cycle, dirty);
    if (la_cas64(&s->state, &old, next, LA_ACQ_REL, LA_ACQ))
      return 1;
  }
}

static LJ_AINLINE void gc2_table_dirty_bump(global_State *g, GCtab *t)
{
  LJGC2TabStamp *s = gc2_table_stamp(g, t);
  uint64_t old;
  if (!s)
    return;
  old = la_load64_acq(&s->state);
  for (;;) {
    uint32_t dirty = gc2_tabstamp_dirty(old) + 1u;
    uint64_t next;
    if (dirty == 0)
      dirty = 1;
    /*
    ** Dirtying a table invalidates its same-cycle scan proof in the same atomic
    ** word used by the scanner. The scanner's post-clear check repairs the
    ** remaining NEEDSCAN lost-clear race.
    */
    next = gc2_tabstamp_pack(0, dirty);
    if (la_cas64(&s->state, &old, next, LA_ACQ_REL, LA_ACQ))
      return;
  }
}

static LJ_AINLINE void gc2_table_dirty_bump_parent(global_State *g,
						   GCobj *parent)
{
  GC2MarkScope scope;
  uint32_t gct;
  if (parent && gc2_observed_obj_valid_scoped(
	 g, parent, &gct, &scope) && gct == (uint32_t)~LJ_TTAB) {
    gc2_table_dirty_bump(g, gco2tab(parent));
    gc2_mark_scope_leave(&scope);
  } else if (parent) {
    gc2_mark_scope_leave(&scope);
  }
}

static LJ_AINLINE void gc2_barrier_mark_or_rescan(global_State *g, GCobj *o)
{
  if (gc2_markobj_direct_status(g, o) == GC2_MARK_LIVE_ALREADY)
    gc2_thread_root_rescan_marked_obj(g, o);
}

void lj_gc2_barrier_tvn_pair_g(global_State *g, GCobj *parent,
				       cTValue *tv, uint32_t n)
{
  uint32_t i;
  int dirtied = 0;
  int active;
  if (!tv)
    return;
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    for (i = 0; i < n; i++)
      (void)gc2_sweep_barrier_tv(g, &tv[i]);
    return;
  }
  /* g-only publication cannot rely on the local TG's delayed mark_active ACK.
  ** The global phase store precedes root scanning, so non-IDLE is the exact
  ** startup-safe barrier gate; an IDLE observation is covered by the later
  ** activation scan of the already-published values. */
  active = gc2_phase_acq(g) != LJ_GC2_IDLE;
  if (!active && !gc2_remember_active_g(g) &&
      parent && gc2_phase_acq(g) == LJ_GC2_IDLE &&
      lj_gc2_ismarked(g, parent) > 0) {
    /*
    ** A VM helper can run just after an active cycle closed, while it is still
    ** publishing values into a table born/marked during that cycle. Preserve that
    ** range from the marked parent instead of relying on an earlier broad stack
    ** snapshot.
    */
    active = 1;
  }
  if (active) {
    for (i = 0; i < n; i++) {
      TValue snap;
      lj_tv_load_acq(&snap, &tv[i]);
      if (tvisgcv(&snap)) {
	if (gc2_phase_acq(g) == LJ_GC2_SWEEP) {
	  (void)gc2_trace_sweep_tv_edge(g, &snap, 0);
	  continue;
	}
	{
	  int status = gc2_markobj_expected_status(g, gcV(&snap),
						  (uint32_t)~itype(&snap), NULL);
	  if (status == GC2_MARK_DEAD)
	    continue;
	  if (!dirtied) {
	    gc2_table_dirty_bump_parent(g, parent);
	    dirtied = 1;
	  }
	  if (status == GC2_MARK_LIVE_ALREADY)
	    gc2_thread_root_rescan_marked_obj(g, gcV(&snap));
	}
      }
    }
  } else if (gc2_remember_active_g(g)) {
    for (i = 0; i < n; i++) {
      TValue snap;
      lj_tv_load_acq(&snap, &tv[i]);
      if (tvisgcv(&snap)) {
	if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
	  (void)gc2_trace_sweep_tv_edge(g, &snap, 0);
	else
	  gc2_remember_pair(g, parent, gcV(&snap));
      }
    }
  }
}

void lj_gc2_barrier_obj_pair_g(global_State *g, GCobj *parent,
				GCobj *child)
{
  uint32_t phase;
  if (!g || !child)
    return;
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_SWEEP) {
    gc2_sweep_barrier_obj(g, child);
    return;
  }
  /* This is the g-only ABI barrier. It cannot borrow a TG from raw TLS and
  ** must retain the historic guarantee that every active phase preserves the
  ** child, including MARK/WEAK startup before mark_active is acknowledged. */
  if (phase != LJ_GC2_IDLE) {
    gc2_table_dirty_bump_parent(g, parent);
    gc2_barrier_mark_or_rescan(g, child);
  }
}

void lj_gc2_barrier_obj_pair(lua_State *L, GCobj *parent, GCobj *child)
{
  global_State *g;
  if (!child || !L)
    return;
  g = G(L);
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    gc2_sweep_barrier_obj(g, child);
    return;
  }
  if (gc2_barrier_active_g(g)) {
    gc2_table_dirty_bump_parent(g, parent);
    gc2_barrier_mark_or_rescan(g, child);
  } else {
    gc2_remember_pair(g, parent, child);
  }
}

void lj_gc2_barrier_tv_pair_g(global_State *g, GCobj *parent, cTValue *tv)
{
  TValue snap;
  if (tv) {
    lj_tv_load_acq(&snap, tv);
    if (tvisgcv(&snap)) {
      uint32_t phase = gc2_phase_acq(g);
      if (phase != LJ_GC2_IDLE) {
	int status = gc2_markobj_expected_status(g, gcV(&snap),
						(uint32_t)~itype(&snap), NULL);
	if (status != GC2_MARK_DEAD) {
	  gc2_table_dirty_bump_parent(g, parent);
	  if (status == GC2_MARK_LIVE_ALREADY && phase != LJ_GC2_SWEEP)
	    gc2_thread_root_rescan_marked_obj(g, gcV(&snap));
	}
      } else {
	gc2_remember_pair(g, parent, gcV(&snap));
      }
    }
  }
}

void lj_gc2_barrier_tv_pair(lua_State *L, GCobj *parent, cTValue *tv)
{
  if (L)
    lj_gc2_barrier_tv_pair_g(G(L), parent, tv);
}

static void gc2_barrier_tab_mark(global_State *g, GCtab *t)
{
  GCobj *o;
  int marked;
  o = obj2gco(t);
  gc2_table_dirty_bump(g, t);
  marked = lj_gc2_ismarked(g, o);
  if (marked > 0) {
    (void)gc2_table_rescan_later(g, t);
  } else if (marked == 0) {
    (void)lj_gc2_markobj_direct(g, o);
  }
}

void lj_gc2_barrier_tab_g(global_State *g, GCtab *t)
{
  if (!t)
    return;
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    gc2_sweep_barrier_obj(g, obj2gco(t));
    return;
  }
  if (gc2_barrier_active_g(g))
    gc2_barrier_tab_mark(g, t);
  else
    gc2_remember_pair(g, obj2gco(t), NULL);
}

void lj_gc2_barrier_key_g(global_State *g, GCtab *t, cTValue *key)
{
  TGState *tg;
  GCobj *child;
  if (!t || !key || !tvisgcv(key))
    return;
  if (g && gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    (void)gc2_trace_sweep_tv_edge(g, key, 0);
    return;
  }
  if (LJ_UNLIKELY(!gc2_tv_gcref_type_match_known(g, key)))
    return;
  /*
  ** Key publication always passes through this helper so weak-table and
  ** color-color boundaries stay in one place. GC2 only has work when a thread
  ** group is actively marking: both the mark barrier and the idle remembered
  ** pair path are gated by mark_active. Keep the common inactive path out of
  ** phase/generational checks without moving the publication callsite.
  */
  if (!g)
    return;
  child = gcV(key);
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    gc2_sweep_barrier_obj(g, child);
    return;
  }
  tg = G2TG(g);
  if (!tg || !lj_tg_mark_active_acq(tg))
    return;
  if (gc2_tab_weak_write_candidate(g, t) & LJ_GC_WEAKKEY)
    return;
  gc2_table_dirty_bump(g, t);
  if (gc2_barrier_active_g(g))
    gc2_barrier_mark_or_rescan(g, child);
  else
    gc2_remember_pair(g, obj2gco(t), child);
}

void lj_gc2_barrier_tab(lua_State *L, GCtab *t)
{
  global_State *g;
  if (!t || !L)
    return;
  g = G(L);
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    gc2_sweep_barrier_obj(g, obj2gco(t));
    return;
  }
  if (gc2_barrier_active_g(g))
    gc2_barrier_tab_mark(g, t);
  else
    gc2_remember_pair(g, obj2gco(t), NULL);
}

void lj_gc2_barrier_weak_key(lua_State *L, GCtab *t, cTValue *key)
{
  global_State *g;
  int weak;
  if (!L || !t || !key || !tvisgcv(key))
    return;
  g = G(L);
  if (gc2_phase_acq(g) != LJ_GC2_WEAK)
    return;
  if (LJ_UNLIKELY(!gc2_tv_gcref_type_match_known(g, key)))
    return;
  if (gc2_phase_acq(g) != LJ_GC2_WEAK)
    return;
  weak = gc2_tab_weak_barrier_mode(g, t);
  /* 05 section 5.8 weak-table key write. */
  if (weak && lj_gc2_markobj(g, gcV(key)))
    gc2_weak_keys_marked_add(g, 1);
}

void lj_gc2_barrier_weak_value(lua_State *L, GCtab *t, cTValue *val)
{
  global_State *g;
  int weak;
  if (!L || !t || !val || !tvisgcv(val))
    return;
  g = G(L);
  if (gc2_phase_acq(g) != LJ_GC2_WEAK)
    return;
  if (LJ_UNLIKELY(!gc2_tv_gcref_type_match_known(g, val)))
    return;
  if (gc2_phase_acq(g) != LJ_GC2_WEAK)
    return;
  weak = gc2_tab_weak_barrier_mode(g, t);
  /*
  ** All-weak tables still need key-side reachability to preserve a hash
  ** entry. Marking only the new value through point-value store paths can
  ** keep entries one cycle longer than stock weak-kv clearing.
  */
  if (weak == LJ_GC_WEAKVAL && lj_gc2_markobj(g, gcV(val)))
    gc2_weak_values_marked_add(g, 1);
}

void lj_gc2_barrier_weak_write(lua_State *L, GCtab *t, cTValue *key,
				       cTValue *val)
{
  global_State *g;
  if (!L || !t)
    return;
  g = G(L);
  if (gc2_phase_acq(g) != LJ_GC2_WEAK)
    return;
  if (gc2_tab_weak_barrier_mode(g, t) == 0)
    return;
  if (key && tvisgcv(key) && gc2_tv_gcref_type_match_known(g, key) &&
      lj_gc2_markobj(g, gcV(key)))
    gc2_weak_keys_marked_add(g, 1);
  if (val && tvisgcv(val) && gc2_tv_gcref_type_match_known(g, val) &&
      lj_gc2_markobj(g, gcV(val)))
    gc2_weak_values_marked_add(g, 1);
}

static LJ_AINLINE void *gc2_mark_base(global_State *g, GCobj *o)
{
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    GCcdata *cd = gco2cd(o);
    void *base;
    if (cdataisv(cd) && lj_cdata_validate(g, cd, &base, NULL))
      return base;
  }
#else
  UNUSED(g);
#endif
  return o;
}

static LJ_AINLINE int gc2_gct_may_traverse(uint32_t gct)
{
  return gct == ~LJ_TTAB || gct == ~LJ_TFUNC || gct == ~LJ_TPROTO ||
	 gct == ~LJ_TTHREAD || gct == ~LJ_TUPVAL || gct == ~LJ_TUDATA
#if LJ_HASJIT
	 || gct == ~LJ_TTRACE
#endif
	 ;
}

static LJ_AINLINE int gc2_obj_may_traverse(GCobj *o)
{
  return gc2_gct_may_traverse(o->gch.gct);
}

static int gc2_markobj_base_valid_scoped(global_State *g, GCobj *o,
					 void **basep, uint32_t *gctp,
					 GC2MarkScope *scope)
{
  void *base;
  uint32_t gct;
  int status, traversable;
  if (!scope)
    return 0;
  status = gc2_retain_candidate_status(g, o, &base, &gct, &traversable,
					0, scope, NULL);
  if (status == GC2_MARK_DEAD)
    return 0;
  gc2_preserve_direct_bodies(g, o);
  if (status == GC2_MARK_NEW && gc2_gct_may_traverse(gct)) {
    uint32_t phase = gc2_phase_acq(g);
    if ((phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK ||
	 phase == LJ_GC2_SWEEP) &&
	(traversable || gct == (uint32_t)~LJ_TUDATA)) {
      (void)gc2_publish_mutator_scoped(g, o, scope);
    }
  }
  if (basep)
    *basep = base;
  if (gctp)
    *gctp = gct;
  return 1;
}

static void gc2_preserve_lfunc_direct_bodies(global_State *g, GCfunc *fn)
{
  const char *pc;
  uint32_t i, nup;
  if (!fn || fn->c.gct != ~LJ_TFUNC || !isluafunc(fn))
    return;
  nup = lj_funcL_nupvalues(&fn->l);
  if (nup > LJ_MAX_UPVAL)
    return;
  pc = mref(fn->l.pc, const char);
  if (pc && checkptrGC(pc)) {
    GCproto *pt = (GCproto *)(void *)(pc - sizeof(GCproto));
    (void)gc2_markobj_preserve_expected_status(g, obj2gco(pt),
						(uint32_t)~LJ_TPROTO);
  }
  for (i = 0; i < nup; i++) {
    GCobj *uv = func_uvptr_acq(&fn->l, i);
    if (uv && checkptrGC(uv))
      (void)gc2_markobj_preserve_expected_status(g, uv,
						(uint32_t)~LJ_TUPVAL);
  }
}

static void gc2_preserve_tab_direct_bodies(global_State *g, GCtab *t)
{
  TValue *array;
  MSize asize, acap, hmask;
  Node *node;
  if (!g || !t)
    return;
  /* This is an early representation-preservation optimization. The retained
  ** table identity is still published for semantic traversal, and ordinary
  ** table access owns its separate generation-read epoch. Do not turn a
  ** normal collision with an opportunistic retired-body reclaimer into sticky
  ** NO_RECLAIM: acquire one tactical SMR section, snapshot only validated
  ** current generations, and skip the early marks if that try loses. */
  if (!lj_gc2_smr_read_try(g))
    return;
  if (lj_tab_array_snapshot_gc_held(g, t, &array, &asize, &acap) ==
      LJ_TAB_GC_SNAPSHOT_OK && array && !lj_tab_array_is_colocated(t, array))
    (void)lj_gc2_markmem(g, acap ? (void *)lj_tab_array_hdrw(array) :
				 (void *)array);
  UNUSED(asize);
  if (lj_tab_node_snapshot_gc_held(g, t, &node, &hmask) ==
      LJ_TAB_GC_SNAPSHOT_OK && node != &g->nilnode && hmask > 0)
    (void)lj_gc2_markmem(g, lj_tab_node_hdrw(node));
  lj_gc2_smr_read_leave(g);
}

static LJ_AINLINE void gc2_preserve_direct_bodies(global_State *g, GCobj *o)
{
  /*
  ** Some object headers contain raw representation pointers used before queued
  ** semantic traversal may run. Preserve only those allocation bodies here; the
  ** normal grey/SSB traversal still owns the Lua object graph so weak edges and
  ** collectible cycles keep stock semantics.
  */
  if (o && o->gch.gct == ~LJ_TFUNC)
    gc2_preserve_lfunc_direct_bodies(g, gco2func(o));
  else if (o && o->gch.gct == ~LJ_TTAB)
    gc2_preserve_tab_direct_bodies(g, gco2tab(o));
}

/*
** Mark/rescue one exact small-arena allocation start. WHITE remains the state
** for marked raw/fixed allocations; LIVE specifically records a detached GC
** header which the post-grace pass must reanchor. A publisher only changes
** RETIRED->LIVE. FREEING is past the destructor LP and cannot be republished.
*/
static LJ_AINLINE int gc2_mark_admission_counted(int admission)
{
  return admission == LJ_ARENA_RESCUE_FULL ||
	 admission == LJ_ARENA_RESCUE_BIT_ONLY ||
	 admission == LJ_ARENA_RESCUE_COMMITTED;
}

static LJ_AINLINE void gc2_mark_scope_init(GC2MarkScope *scope)
{
  if (scope) {
    scope->a = NULL;
    scope->admission = LJ_ARENA_RESCUE_RETRY;
    memset(&scope->huge, 0, sizeof(scope->huge));
  }
}

static void gc2_mark_scope_leave(GC2MarkScope *scope)
{
  if (scope && scope->admission == GC2_SCOPE_HUGE_READER)
    (void)lj_arena_hugetab_reader_release(&scope->huge, NULL);
  else if (scope && scope->a &&
	   gc2_mark_admission_counted(scope->admission))
    lj_arena_rescue_leave(scope->a);
  gc2_mark_scope_init(scope);
}

static LJ_AINLINE void gc2_public_lease_init(LJGC2Lease *lease)
{
  if (lease)
    memset(lease, 0, sizeof(*lease));
}

static void gc2_public_lease_take(LJGC2Lease *lease, GC2MarkScope *scope)
{
  lj_assertX(lease != NULL && scope != NULL,
	     "GC2 lease transfer requires storage");
  lease->arena = (void *)scope->a;
  lease->admission = (intptr_t)scope->admission;
  if (scope->admission == GC2_SCOPE_HUGE_READER)
    lease->huge = scope->huge;
  lj_assertX((int)lease->admission == scope->admission,
	     "GC2 lease admission does not round-trip through intptr_t");
  scope->a = NULL;
  scope->admission = LJ_ARENA_RESCUE_RETRY;
  memset(&scope->huge, 0, sizeof(scope->huge));
}

int lj_gc2_tv_lease_acquire(global_State *g, cTValue *tv,
				     LJGC2Lease *lease)
{
  GC2MarkScope scope;
  int status;
  if (!lease)
    return LJ_GC2_TV_EDGE_STALE;
  gc2_public_lease_init(lease);
  if (!g || !tv)
    return LJ_GC2_TV_EDGE_STALE;
  status = gc2_tv_admit_scoped(g, tv, &scope);
  if (status != GC2_TV_SCOPE_ADMITTED) {
    gc2_mark_scope_leave(&scope);
    return status == GC2_TV_SCOPE_RETRY ? LJ_GC2_TV_EDGE_RETRY :
						 LJ_GC2_TV_EDGE_STALE;
  }
  gc2_public_lease_take(lease, &scope);
  return LJ_GC2_TV_EDGE_VALID;
}

void lj_gc2_lease_release(LJGC2Lease *lease)
{
  GC2MarkScope scope;
  if (!lease)
    return;
  scope.a = (GCArena *)lease->arena;
  scope.admission = (int)lease->admission;
  scope.huge = lease->huge;
  lj_assertX((intptr_t)scope.admission == lease->admission,
	     "GC2 lease admission does not round-trip to int");
  /* Clear first so repeated release and any diagnostic re-entry are no-ops. */
  memset(lease, 0, sizeof(*lease));
  gc2_mark_scope_leave(&scope);
}

/* Conservative TValue roots may be popped stack cells, stale weak slots, or
** racy spill words. During SWEEP the tag alone is not recovery authority:
** distinguish stable stale incarnations from transient lifetime ownership,
** and retain an admitted allocation through all body/type accesses made by
** the ordinary sweep tracer. */
static uint32_t gc2_trace_sweep_tv_edge(global_State *g, cTValue *tv,
					 int worker_edge)
{
  LJGC2Lease lease;
  GCobj *o;
  uint32_t traced;
  int edge;
  if (!g || !tv || !tvisgcv(tv))
    return 0;
  edge = lj_gc2_tv_lease_acquire(g, tv, &lease);
  if (edge == LJ_GC2_TV_EDGE_STALE)
    return 0;
  o = gcV(tv);
  if (edge == LJ_GC2_TV_EDGE_RETRY) {
    /* A semantic edge colliding with DESTRUCT/MUTATING/RECOVERY must leave an
    ** exact allocation-side locator. Stable stale edges never reach this arm
    ** and therefore cannot poison the cycle's reclaim authority. */
    if (!gc2_recovery_publish(g, o))
      gc2_recovery_fail_closed(g);
    return 1;
  }
  traced = worker_edge ? gc2_trace_sweep_worker_edge(g, o) :
			 lj_gc2_trace_sweep_root(g, o);
  lj_gc2_lease_release(&lease);
  return traced;
}

#define GC2_CDATA_BACK_CELLS \
  ((uint32_t)(LJ_HUGE_THRESHOLD / LJ_CELL_SIZE))

static LJ_AINLINE int gc2_small_lifetime_state_readable(uint32_t life)
{
  return life == LJ_ARENA_LIFETIME_LIVE ||
    life == LJ_ARENA_LIFETIME_CONSTRUCT ||
    life == LJ_ARENA_LIFETIME_RESCUE;
}

static LJ_AINLINE int gc2_small_lifetime_readable(GCArena *a,
						   uint32_t start)
{
  uint32_t life = lj_arena_lifetime_state_acq(a, start);
  return gc2_small_lifetime_state_readable(life);
}

/* Find the nearest non-FREE lifetime start without consulting block[] or body
** bytes. Callers decide whether its acquired state is readable, counted but
** opaque RECOVERY, generic/body-owned MUTATING, or cancellable DESTRUCT. */
static int gc2_small_lifetime_nearest(GCArena *a, uint32_t cell,
					      uint32_t *startp,
					      uint32_t *lifep)
{
  uint32_t mincell, i;
  if (!a || !startp || cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS)
    return 0;
  mincell = cell >= GC2_CDATA_BACK_CELLS - 1u ?
    cell - (GC2_CDATA_BACK_CELLS - 1u) : LJ_AFIRST_CELL;
  if (mincell < LJ_AFIRST_CELL)
    mincell = LJ_AFIRST_CELL;
  for (i = cell;; i--) {
    uint32_t life = lj_arena_lifetime_state_acq(a, i);
    if (life != LJ_ARENA_LIFETIME_FREE) {
      *startp = i;
      if (lifep)
	*lifep = life;
      return 1;
    }
    if (i == mincell)
      break;
  }
  return 0;
}

/* Body readers call this only after admission RMW -> SC fence. The writer
** pairs lifetime CAS -> SC fence -> admission proof, so a reader either sees a
** readable lane or rejects before any allocation bitmap/header/body access. */
static int gc2_small_lifetime_containing_start(GCArena *a, uint32_t cell,
						uint32_t *startp)
{
  uint32_t life;
  return gc2_small_lifetime_nearest(a, cell, startp, &life) &&
    (life == LJ_ARENA_LIFETIME_LIVE ||
     life == LJ_ARENA_LIFETIME_CONSTRUCT ||
     life == LJ_ARENA_LIFETIME_RESCUE);
}

/* Admit an arbitrary address inside a traversable small allocation without
** reading block[], READY, or body bytes before the lifetime handshake. */
static int gc2_small_containing_admit(global_State *g, GCArena *a,
				      uint32_t cell,
				      void **basep, uint32_t *startp,
				      GC2MarkScope *scope)
{
  uint32_t start, checkstart;
  int admission;
  gc2_mark_scope_init(scope);
  if (!g || !a || !scope)
    return 0;
  admission = gc2_small_registered_rescue_enter(g, a);
  if (admission == LJ_ARENA_RESCUE_RETRY)
    return 0;
  scope->a = a;
  scope->admission = admission;
  la_fence_seq();
  if (!(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) ||
      !gc2_small_lifetime_containing_start(a, cell, &start) ||
      !gc2_small_containing_start(a, cell, &checkstart) ||
      checkstart != start || !lj_arena_bm_get(a->block, start) ||
      !lj_arena_ready_get(a, start) || lj_arena_late_get(a, start) ||
      lj_arena_sweep_state_acq(a, start) == LJ_ARENA_SWEEP_FREEING ||
      !gc2_small_lifetime_readable(a, start)) {
    gc2_mark_scope_leave(scope);
    return 0;
  }
  if (basep)
    *basep = lj_arena_cellptr(a, start);
  if (startp)
    *startp = start;
  return 1;
}

static int gc2_mark_small_committed_status(global_State *g, GCArena *a,
					    uint32_t cell)
{
  uint32_t state = lj_arena_sweep_state_acq(a, cell);
  uint64_t block;
  UNUSED(g);
  lj_assertX(state == LJ_ARENA_SWEEP_WHITE ||
	     state == LJ_ARENA_SWEEP_FREEING,
	     "terminal arena LP exposed actionable sweep state");
  if (state != LJ_ARENA_SWEEP_WHITE)
    return GC2_MARK_DEAD;
  /* Terminal apply publishes block before resetting sweep[] to WHITE. This
  ** state->block acquire order rejects both pre-reset FREEING and post-reset
  ** dead block0; it cannot synthesize a live cell across generations. */
  block = la_load64_acq(&a->block[cell >> 6]);
  if (!((block >> (cell & 63)) & 1u) || lj_arena_late_get(a, cell))
    return GC2_MARK_DEAD;
  /* mark[] is being overwritten by terminal apply in this generation. The
  ** committed representation is read-only; sweep-root code separately traces
  ** a preserved live root without treating it as a fresh mark discovery. */
  return GC2_MARK_LIVE_ALREADY;
}

/* PREPSWEEP/restore/adoption commit excludes owner allocation and never
** rewrites an allocated live body. It may normalize sweep state, so publish a
** durable mark and validate the terminal indicators on both sides of that OR.
** FREEING remains terminal and late[] remains a physical-destructor pin. */
static int gc2_mark_small_prep_committed_status(global_State *g, GCArena *a,
						 uint32_t cell)
{
  uint32_t state;
  uint64_t block;
  int marked;
  state = lj_arena_sweep_state_acq(a, cell);
  block = la_load64_acq(&a->block[cell >> 6]);
  if (!((block >> (cell & 63)) & 1u) ||
      state == LJ_ARENA_SWEEP_FREEING || lj_arena_late_get(a, cell))
    return GC2_MARK_DEAD;
  marked = !la_bit_test_and_set64(&a->mark[cell >> 6], cell & 63);
  /* PREP consumes a late pin as state->FREEING then bit-clear, and preserves
  ** intrusive FREEING across sidecar normalization. This final ordered check
  ** therefore cannot accept the transient old body after either publication. */
  if (lj_arena_late_get(a, cell) ||
      lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_FREEING)
    return GC2_MARK_DEAD;
  if (marked)
    gc2_marks_this_round_add(g, 1);
  return marked ? GC2_MARK_NEW : GC2_MARK_LIVE_ALREADY;
}

static LJ_AINLINE int gc2_committed_generation_marks(uint32_t flags)
{
  /* QUARANTINE is terminal bitmap apply. RECLAIMED without PREPSWEEP is its
  ** stable read-only result. Prepare, restore, and adoption carry PREPSWEEP;
  ** their live bodies must accept a durable mark. The second clause covers a
  ** delayed committed reader after PREPSWEEP was cleared/opened. */
  return !(flags & LJ_AF_QUARANTINE) &&
    ((flags & LJ_AF_PREPSWEEP) || !(flags & LJ_AF_RECLAIMED));
}

/* Preserve the lifetime value which actually rejected an admitted mark. A
** transition owner may restore LIVE immediately after that acquire; callers
** must not attempt to reconstruct the reason from a later lifetime reload. */
static LJ_AINLINE int gc2_mark_small_transition_dead(GCArena *a,
						      uint32_t cell,
						      uint32_t life,
						      int *retryp)
{
  if (retryp && gc2_small_candidate_transition_retry_held(a, cell, life))
    *retryp = 1;
  return GC2_MARK_DEAD;
}

/* Mark one already-admitted allocation. The result separates lifetime from
** discovery: callers may inspect direct bodies for either live result, but
** may enqueue semantic traversal only for GC2_MARK_NEW. */
static int gc2_mark_small_cell_admitted(global_State *g, GCArena *a,
					uint32_t cell, int admission,
					int *retryp)
{
  uint32_t state, life;
  uint64_t block;
  int marked, rescued = 0, result;
  if (retryp)
    *retryp = 0;
  if (!a || cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS)
    return GC2_MARK_DEAD;
  life = lj_arena_lifetime_state_acq(a, cell);
  if ((lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) &&
      !gc2_small_lifetime_state_readable(life))
    return gc2_mark_small_transition_dead(a, cell, life, retryp);
  block = la_load64_acq(&a->block[cell >> 6]);
  if (!((block >> (cell & 63)) & 1u))
    return GC2_MARK_DEAD;
  if (admission == LJ_ARENA_RESCUE_COMMITTED) {
    uint32_t flags = lj_arena_flags_acq(a);
    if (gc2_committed_generation_marks(flags))
      result = gc2_mark_small_prep_committed_status(g, a, cell);
    else
      result = gc2_mark_small_committed_status(g, a, cell);
    goto out;
  }
  if (lj_arena_late_get(a, cell))
    return GC2_MARK_DEAD;
  state = lj_arena_sweep_state_acq(a, cell);
  if (state == LJ_ARENA_SWEEP_FREEING) {
    life = lj_arena_lifetime_state_acq(a, cell);
    return gc2_mark_small_transition_dead(a, cell, life, retryp);
  }
  marked = !la_bit_test_and_set64(&a->mark[cell >> 6], cell & 63);
  if (retryp)
    gc2_queue_post_admit_test_pause_at(
      (GCobj *)lj_arena_cellptr(a, cell));
  for (;;) {
    state = lj_arena_sweep_state_acq(a, cell);
    if (state == LJ_ARENA_SWEEP_FREEING) {
      life = lj_arena_lifetime_state_acq(a, cell);
      return gc2_mark_small_transition_dead(a, cell, life, retryp);
    }
    if (state != LJ_ARENA_SWEEP_RETIRED)
      break;
    if (lj_arena_sweep_state_cas(a, cell, LJ_ARENA_SWEEP_RETIRED,
					 LJ_ARENA_SWEEP_LIVE)) {
      uint32_t old = lj_arena_reclaim_deferred_sub(a, 1);
      lj_assertG(old != 0, "arena mark rescue deferred underflow");
      UNUSED(old);
      rescued = 1;
      break;
    }
  }
  if (lj_arena_late_get(a, cell))
    return GC2_MARK_DEAD;
  if (lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_FREEING) {
    life = lj_arena_lifetime_state_acq(a, cell);
    return gc2_mark_small_transition_dead(a, cell, life, retryp);
  }
  if (marked || rescued)
    gc2_marks_this_round_add(g, 1);
  result = (marked || rescued) ? GC2_MARK_NEW : GC2_MARK_LIVE_ALREADY;
out:
  if (result == GC2_MARK_DEAD) {
    life = lj_arena_lifetime_state_acq(a, cell);
    return gc2_mark_small_transition_dead(a, cell, life, retryp);
  }
  life = lj_arena_lifetime_state_acq(a, cell);
  if ((lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) &&
      !gc2_small_lifetime_state_readable(life))
    return gc2_mark_small_transition_dead(a, cell, life, retryp);
  return result;
}

static int gc2_mark_small_cell_begin(global_State *g, GCArena *a,
				     uint32_t cell, GC2MarkScope *scope)
{
  int admission, status;
  gc2_mark_scope_init(scope);
  admission = gc2_small_registered_rescue_enter(g, a);
  if (admission == LJ_ARENA_RESCUE_RETRY)
    return GC2_MARK_DEAD;
  /* Pair the admission RMW with the destructive-owner SC fence before the
  ** first lifetime, bitmap, or body observation. */
  la_fence_seq();
  if (scope) {
    scope->a = a;
    scope->admission = admission;
  }
  status = gc2_mark_small_cell_admitted(g, a, cell, admission, NULL);
  if (status == GC2_MARK_DEAD)
    gc2_mark_scope_leave(scope);
  return status;
}

static int gc2_mark_small_cell_begin_reclaim_held(global_State *g,
						   GCArena *a,
						   uint32_t cell,
						   GC2MarkScope *scope)
{
  int admission, status;
  gc2_mark_scope_init(scope);
  admission = gc2_small_registered_rescue_enter_reclaim_held(g, a);
  if (admission == LJ_ARENA_RESCUE_RETRY)
    return GC2_MARK_DEAD;
  la_fence_seq();
  if (scope) {
    scope->a = a;
    scope->admission = admission;
  }
  status = gc2_mark_small_cell_admitted(g, a, cell, admission, NULL);
  if (status == GC2_MARK_DEAD)
    gc2_mark_scope_leave(scope);
  return status;
}

static int gc2_small_arena_known(global_State *g, GCArena *a)
{
  TGState *tg;
  int registered = gc2_small_arena_registered(g, a, NULL);
  if (registered >= 0)
    return registered;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg))
    if (gc2_tg_owns_small_arena(tg, a))
      return 1;
  return g->main_tg && gc2_tg_owns_small_arena(g->main_tg, a);
}

/* Find the nearest allocation start at or before an interior cdata header.
** The small-allocation size cap bounds this to at most 17 acquired bitmap
** words even at the worst word alignment. No candidate bytes are read here. */
static int gc2_small_containing_start(GCArena *a, uint32_t cell,
				      uint32_t *startp)
{
  uint32_t mincell, wi, minwi;
  if (!a || cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS)
    return 0;
  mincell = cell >= GC2_CDATA_BACK_CELLS - 1u ?
    cell - (GC2_CDATA_BACK_CELLS - 1u) : LJ_AFIRST_CELL;
  if (mincell < LJ_AFIRST_CELL)
    mincell = LJ_AFIRST_CELL;
  wi = cell >> 6;
  minwi = mincell >> 6;
  for (;;) {
    uint64_t bits = la_load64_acq(&a->block[wi]) |
		    la_load64_acq(&a->mark[wi]);
    uint32_t hi = wi == (cell >> 6) ? (cell & 63u) : 63u;
    uint32_t lo = wi == minwi ? (mincell & 63u) : 0u;
    uint64_t himask = hi == 63u ? ~(uint64_t)0 :
	(((uint64_t)1 << (hi + 1u)) - 1u);
    uint64_t lomask = lo == 0 ? ~(uint64_t)0 :
	(~(uint64_t)0 << lo);
    bits &= himask & lomask;
    if (bits) {
      uint32_t start = (wi << 6) + lj_fls64(bits);
      if (!lj_arena_bm_get(a->block, start))
	return 0;  /* The nearest boundary is a reusable free run. */
      *startp = start;
      return 1;
    }
    if (wi == minwi)
      break;
    wi--;
  }
  return 0;
}

static int gc2_retained_cdata_layout(global_State *g, GCobj *o, void *base,
				      GCArena *a, uint32_t start,
				      size_t authoritative_size,
				      int interior_tag)
{
#if LJ_HASFFI
  void *realbase;
  GCSize size;
  uintptr_t b = (uintptr_t)base, p = (uintptr_t)o;
  GCcdata *cd = gco2cd(o);
  if (!interior_tag || p <= b || p - b > UINT16_MAX ||
      la_load16_acq((const uint16_t *)base) != (uint16_t)(p - b) ||
      !cdataisv(cd) || !lj_cdata_validate(g, cd, &realbase, &size) ||
      realbase != base)
    return 0;
  if (a) {
    if (!gc2_small_cdata_span_exact(a, start, cd, size))
      return 0;
  } else if ((size_t)size != authoritative_size) {
    return 0;
  }
  return 1;
#else
  UNUSED(g); UNUSED(o); UNUSED(base); UNUSED(a); UNUSED(start);
  UNUSED(authoritative_size); UNUSED(interior_tag);
  return 0;
#endif
}

#if LJ_HASFFI
static int gc2_predefined_ctype_payload_size(CTypeID id, CTSize *sizep)
{
  switch (id) {
#define GC2_PREDEF_SIZE(name, sz, ct, info) \
  case CTID_##name: \
    if ((int)(sz) < 0 || (ct) == CT_ATTRIB || (ct) == CT_VOID) return 0; \
    *sizep = (CTSize)(sz); \
    return 1;
    CTTYDEF(GC2_PREDEF_SIZE)
#undef GC2_PREDEF_SIZE
  default:
    return 0;
  }
}
#endif

/* Validate only after the containing allocation has been marked/admitted.
** A non-base candidate is accepted solely through allocation-owned cdata
** identity metadata published after the complete interior header. */
static int gc2_retained_candidate_valid(global_State *g, GCobj *o,
					 void *base, GCArena *a,
					 uint32_t start, size_t alloc_size,
					 int interior_tag, uint32_t *gctp)
{
  uint32_t gct = (uint32_t)la_load8_acq(&o->gch.gct);
  if (gct == 0 || gct < (uint32_t)~LJ_TSTR ||
      gct > (uint32_t)~LJ_TUDATA)
    return 0;
  if (a && ((gct == (uint32_t)~LJ_TCDATA) !=
	    (lj_arena_cdata_get(a, start) != 0)))
    return 0;
  if (o != (GCobj *)base) {
    if (gct != (uint32_t)~LJ_TCDATA ||
	!gc2_retained_cdata_layout(g, o, base, a, start, alloc_size,
				   interior_tag))
      return 0;
  } else {
    if (interior_tag)
      return 0;  /* An interior-cdata allocation never places cd at base. */
#if LJ_HASFFI
    if (gct == (uint32_t)~LJ_TCDATA) {
      void *realbase;
      GCSize size;
      if (ctype_ctsG(g) == NULL) {
	CTSize payload;
	/* Lexer/bytecode bootstrap uses only immutable predefined fixed types.
	** Coverage identifies the cdata family, but exact bytes still come from
	** this authoritative table plus the header tail encoding. */
	if (cdataisv(gco2cd(o)) ||
	    ((uintptr_t)o & (uintptr_t)(LJ_CELL_SIZE - 1u)) != 0 ||
	    !gc2_predefined_ctype_payload_size(gco2cd(o)->ctypeid, &payload) ||
	    payload > LJ_MAX_MEM32 - sizeof(GCcdata))
	  return 0;
	size = (GCSize)(sizeof(GCcdata) + payload);
	if (!cdata_size_tail_matches(gco2cd(o), (size_t)size) ||
	    (a ? !gc2_small_cdata_span_exact(a, start, gco2cd(o), size) :
		  (size_t)size != alloc_size))
	  return 0;
      } else if (cdataisv(gco2cd(o)) ||
		 !lj_cdata_validate(g, gco2cd(o), &realbase, &size) ||
		 realbase != base || (a ? size > LJ_HUGE_THRESHOLD :
					(size_t)size != alloc_size)) {
	return 0;
      }
      if (a && ctype_ctsG(g) != NULL &&
	  !gc2_small_cdata_span_exact(a, start, gco2cd(o), size))
	return 0;
    }
#endif
  }
  if (gctp)
    *gctp = gct;
  return 1;
}

/* Classify a structural rejection while the exact arena admission remains
** counted. A late bit is irrevocable external-free provenance. Without that
** LP, a lifetime owner or exact recovery/root transition can still restore
** this generation and must be replayed instead of becoming semantic nil. */
static int gc2_small_candidate_transition_retry_held(GCArena *a,
						      uint32_t start,
						      uint32_t life)
{
  uint32_t recovery, root;
  if (!a || start < LJ_AFIRST_CELL || start >= LJ_ARENA_CELLS ||
      lj_arena_late_get(a, start))
    return 0;
  if (life == LJ_ARENA_LIFETIME_CONSTRUCT ||
      life == LJ_ARENA_LIFETIME_RECOVERY ||
      life == LJ_ARENA_LIFETIME_DESTRUCT ||
      life == LJ_ARENA_LIFETIME_RESCUE ||
      life == LJ_ARENA_LIFETIME_MUTATING)
    return 1;
  recovery = lj_arena_recovery_state_acq(a, start);
  root = lj_arena_root_state_acq(a, start);
  return recovery != LJ_ARENA_RECOVERY_IDLE ||
    root == LJ_ARENA_ROOT_LINKING || root == LJ_ARENA_ROOT_UNLINKING;
}

/* Establish a counted small-arena body lease without changing semantic mark
** state. The structural planes are allocation-owned and are sampled on both
** sides of header/layout validation. FREEING/late is the destructor LP; READY
** is the constructor LP. A successful caller may inspect the admitted object
** only until gc2_mark_scope_leave(). */
static int gc2_small_candidate_admit(global_State *g, GCobj *o, GCArena *a,
				     uint32_t expected_gct,
				     void **basep, uint32_t *startp,
				     uint32_t *gctp, GC2MarkScope *scope)
{
  uint32_t cell, start, checkstart, state, gct, life;
  void *base;
  int admission, interior_tag;
  gc2_mark_scope_init(scope);
  if (!g || !o || !a || !scope)
    return 0;
  admission = gc2_small_registered_rescue_enter(g, a);
  if (admission == LJ_ARENA_RESCUE_RETRY)
    return -1;
  scope->a = a;
  scope->admission = admission;
  la_fence_seq();
  cell = lj_arena_cellof(o);
  start = checkstart = ~(uint32_t)0;
  /* Lifetime is the first per-allocation observation after reader admission.
  ** It also locates interior cdata without racing a block-boundary rewrite. */
  if (!(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE)) {
    life = lj_arena_lifetime_state_acq(a, cell);
    start = cell;
    goto structural_invalid;
  }
  if (!gc2_small_lifetime_nearest(a, cell, &start, &life)) {
    life = lj_arena_lifetime_state_acq(a, cell);
    start = cell;
    goto structural_invalid;
  }
  if (life != LJ_ARENA_LIFETIME_LIVE &&
      life != LJ_ARENA_LIFETIME_CONSTRUCT &&
      life != LJ_ARENA_LIFETIME_RESCUE)
    goto structural_invalid;
  if (!gc2_small_containing_start(a, cell, &checkstart) ||
      checkstart != start || !lj_arena_bm_get(a->block, start) ||
      !lj_arena_ready_get(a, start) || lj_arena_late_get(a, start) ||
      lj_arena_sweep_state_acq(a, start) == LJ_ARENA_SWEEP_FREEING) {
    life = lj_arena_lifetime_state_acq(a, start);
    goto structural_invalid;
  }
  base = lj_arena_cellptr(a, start);
  interior_tag = o != (GCobj *)base;
  if ((interior_tag && !lj_arena_cdata_get(a, start)) ||
      !gc2_retained_candidate_valid(g, o, base, a, start, 0,
				    interior_tag, &gct) ||
      (expected_gct != 0 && gct != expected_gct)) {
    life = lj_arena_lifetime_state_acq(a, start);
    if (gc2_small_candidate_transition_retry_held(a, start, life))
      goto retry;
    goto invalid;
  }
  /* A remote destructor publishes FREEING before touching body bytes. Keep a
  ** final structural check inside the counted lease so a validator which met
  ** that LP never returns a dereferenceable identity. */
  state = lj_arena_sweep_state_acq(a, start);
  if (!lj_arena_bm_get(a->block, start) ||
      !lj_arena_ready_get(a, start) || lj_arena_late_get(a, start) ||
      state == LJ_ARENA_SWEEP_FREEING ||
      !gc2_small_lifetime_readable(a, start)) {
    life = lj_arena_lifetime_state_acq(a, start);
    goto structural_invalid;
  }
  if (basep)
    *basep = base;
  if (startp)
    *startp = start;
  if (gctp)
    *gctp = gct;
  return 1;
structural_invalid:
  if (gc2_small_candidate_transition_retry_held(a, start, life))
    goto retry;
invalid:
  gc2_mark_scope_leave(scope);
  return 0;
retry:
  gc2_mark_scope_leave(scope);
  return -1;
}

static int gc2_mark_huge_candidate(global_State *g, GCobj *o, void **basep,
				    LJHugeInfo *hip,
				    GC2MarkScope *scope)
{
  GC2HugeRegistryLease lease = GC2_HUGE_REGISTRY_NONE;
  TGState *tg;
  int marked;
  if (basep)
    *basep = NULL;
  gc2_mark_scope_init(scope);
  if (!g || !o || !scope)
    return GC2_MARK_DEAD;
  if (!gc2_huge_registry_read_try(g, &lease))
    goto registry_retry;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (!lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      continue;
    marked = lj_arena_hugetab_mark_cdata_range_reader_acquire(
      &tg->huge, o, basep, &scope->huge, hip);
    if (marked >= 0)
      goto found;
    if (marked == LJ_ARENA_HUGE_READER_OVERFLOW)
      goto overflow;
  }
  tg = g->main_tg;
  if (!tg || !lj_tg_flags_test_acq(tg, TGF_HUGETAB)) {
    gc2_huge_registry_read_leave(g, &lease);
    return GC2_MARK_DEAD;
  }
  marked = lj_arena_hugetab_mark_cdata_range_reader_acquire(
    &tg->huge, o, basep, &scope->huge, hip);
  if (marked == LJ_ARENA_HUGE_READER_OVERFLOW)
    goto overflow;
  if (marked < 0) {
    gc2_huge_registry_read_leave(g, &lease);
    return GC2_MARK_DEAD;
  }
found:
  gc2_huge_registry_read_leave(g, &lease);
  /* The slot-local reader now pins the stable header after either registry
  ** admission mode has ended. */
  if (marked == LJ_ARENA_HUGE_MARK_INTENT)
    return GC2_MARK_DEAD;  /* Unique retire owner discharges after TICKET. */
  if (marked == LJ_ARENA_HUGE_MARK_SATURATED)
    goto overflow;
  scope->admission = GC2_SCOPE_HUGE_READER;
  if (marked > 0)
    gc2_marks_this_round_add(g, 1);
  return marked > 0 ? GC2_MARK_NEW : GC2_MARK_LIVE_ALREADY;
overflow:
  gc2_huge_registry_read_leave(g, &lease);
  /* Saturation publishes MARK but no body token. Queue this identity
  ** unconditionally: an old MARK may come from raw-memory preservation and is
  ** not proof that the object's graph was ever scheduled. The future drain
  ** re-enters this reader protocol before touching the header. */
  gc2_marks_this_round_add(g, 1);
  if (!gc2_publish_mutator(g, o))
    gc2_activation_pin_no_reclaim(g);
  return GC2_MARK_DEAD;
registry_retry:
  /* This is an authoritative semantic candidate, not a negative lookup. Keep
  ** its exact identity durable and veto closure until a later drain can enter
  ** the TG registry and acquire the real body reader. */
  gc2_marks_this_round_add(g, 1);
  if (!gc2_publish_mutator(g, o))
    gc2_activation_pin_no_reclaim(g);
  return GC2_MARK_DEAD;
}

static int gc2_retain_candidate_status(global_State *g, GCobj *o,
					void **basep, uint32_t *gctp,
					int *traversablep,
					uint32_t expected_gct,
					GC2MarkScope *holdp,
					int *retryp)
{
  GC2MarkScope scope;
  GCArena *a;
  void *base = NULL;
  uint32_t gct = 0;
  int status, traversable = 0;
  gc2_mark_scope_init(&scope);
  gc2_mark_scope_init(holdp);
  if (retryp)
    *retryp = 0;
  if (!g || !o || !checkptrGC(o) ||
      ((uintptr_t)o & (uintptr_t)(sizeof(void *) - 1u)) != 0)
    return GC2_MARK_DEAD;
  if (o == obj2gco(&g->strempty)) {
    /* The canonical empty string is an immortal leaf embedded in
    ** global_State, not an arena allocation. It can be a normal stack/table/
    ** JIT root (notably from getenv("")-shaped results), so central retention
    ** must recognize it before allocator membership. Keep expected-type
    ** validation exact and manufacture neither a mark nor grey work. */
    gct = (uint32_t)la_load8_acq(&o->gch.gct);
    if (gct != (uint32_t)~LJ_TSTR ||
	(expected_gct != 0 && expected_gct != gct))
      return GC2_MARK_DEAD;
    base = o;
    status = GC2_MARK_LIVE_ALREADY;
    goto done;
  }
  if (la_load32_acq(&g->allocf_arena) == 0) {
    /* Custom lua_Alloc is intentionally outside the current GC2 retention
    ** tranche. Preserve the temporary compatibility behavior documented in
    ** notes/lua-alloc-temporarily-disabled-2026-07-10.md. */
    base = o;
    gct = (uint32_t)la_load8_acq(&o->gch.gct);
#if LJ_HASFFI
    if (gct == (uint32_t)~LJ_TCDATA && cdataisv(gco2cd(o)) &&
	!lj_cdata_validate(g, gco2cd(o), &base, NULL))
      return GC2_MARK_DEAD;
#endif
    if (gct == 0 || gct < (uint32_t)~LJ_TSTR ||
	gct > (uint32_t)~LJ_TUDATA ||
	(expected_gct != 0 && gct != expected_gct))
      return GC2_MARK_DEAD;
    status = GC2_MARK_LIVE_ALREADY;
    traversable = gct != (uint32_t)~LJ_TUDATA;
    goto done;
  }
  a = lj_arena_of(o);
  if (gc2_small_arena_known(g, a)) {
    uint32_t start;
    int admitted = gc2_small_candidate_admit(g, o, a, expected_gct,
					      &base, &start, &gct, &scope);
    if (admitted <= 0) {
      if (admitted < 0 && retryp)
	*retryp = 1;
      return GC2_MARK_DEAD;
    }
    /* Header identity, type and layout are now exact under a counted body
    ** lease. Only this point may change the semantic mark/rescue state. */
    status = gc2_mark_small_cell_admitted(
      g, a, start, scope.admission, retryp);
    if (status == GC2_MARK_DEAD) {
      if (retryp && *retryp)
	gc2_queue_retry_witness_test_pause_at(o);
      gc2_mark_scope_leave(&scope);
      return status;
    }
    traversable = gct == (uint32_t)~LJ_TUDATA ? 0 :
	((lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) != 0);
    if (holdp) {
      *holdp = scope;
      gc2_mark_scope_init(&scope);
    } else {
      gc2_mark_scope_leave(&scope);
    }
  } else {
    LJHugeInfo hi;
    GCobj *actual;
    uint32_t actual_gct = 0;
    uint32_t phase;
    uint16_t offset = 0;
    int interior_tag;
    /* The mark CAS is the huge-mapping lifetime LP. No candidate/header byte
    ** may be read before it succeeds. MARK_INTENT deliberately returns DEAD
    ** with no base; the unique retire owner performs the traversal discharge. */
    status = gc2_mark_huge_candidate(g, o, &base, &hi, &scope);
    if (status == GC2_MARK_DEAD || !base) {
      gc2_mark_scope_leave(&scope);
      return GC2_MARK_DEAD;
    }
    interior_tag = (hi.flags & LJ_HUGEF_INTERIOR_CDATA) != 0;
    if (interior_tag) {
      if ((hi.flags & LJ_HUGEF_CDATA) == 0 ||
	  hi.size < sizeof(GCcdataVar) + sizeof(GCcdata))
	goto huge_invalid;
      offset = la_load16_acq((const uint16_t *)base);
      if (offset < sizeof(GCcdataVar) ||
	  (size_t)offset + sizeof(GCcdata) > hi.size)
	goto huge_invalid;
      actual = (GCobj *)(void *)((char *)base + offset);
    } else {
      actual = (GCobj *)base;
    }
    if (!gc2_retained_candidate_valid(g, actual, base, NULL, 0, hi.size,
				      interior_tag, &actual_gct) ||
	((actual_gct == (uint32_t)~LJ_TCDATA) !=
	 ((hi.flags & LJ_HUGEF_CDATA) != 0)))
      goto huge_invalid;
    phase = gc2_phase_acq(g);
    if (o != actual || (expected_gct != 0 && actual_gct != expected_gct)) {
      /* A false candidate/type may conservatively retain this allocation for
      ** one cycle, but it must discharge any graph mark it just consumed so a
      ** later real edge cannot observe ALREADY and skip the children. */
      if (gc2_gct_may_traverse(actual_gct) &&
	  (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK ||
	   phase == LJ_GC2_SWEEP)) {
	(void)gc2_publish_mutator_scoped(g, actual, &scope);
      }
      goto huge_invalid;
    }
    gct = actual_gct;
    traversable = gct == (uint32_t)~LJ_TUDATA ? 0 :
	((hi.flags & LJ_HUGEF_TRAVERSABLE) != 0);
    if (holdp) {
      *holdp = scope;
      gc2_mark_scope_init(&scope);
    } else {
      gc2_mark_scope_leave(&scope);
    }
    goto done;
huge_invalid:
    gc2_mark_scope_leave(&scope);
    return GC2_MARK_DEAD;
  }
done:
  if (basep)
    *basep = base;
  if (gctp)
    *gctp = gct;
  if (traversablep)
    *traversablep = traversable;
  return status;
}

int lj_gc2_tab_generation_current(global_State *g, GCtab *t,
					   const void *generation,
					   int array_generation)
{
  GC2MarkScope scope;
  GCArena *a;
  void *base;
  uint32_t start, gct;
  int admitted, same;
  if (!g || !t || !generation || !checkptrGC(t) ||
      ((uintptr_t)t & (uintptr_t)(sizeof(void *) - 1u)) != 0)
    return 0;
  if (la_load32_acq(&g->allocf_arena) == 0)
    return -1;  /* Temporary custom-lua_Alloc boundary: retain, never free. */
  a = lj_arena_of(t);
  if (!gc2_small_arena_known(g, a))
    return -1;  /* GCtab is small today; fail closed if that ever changes. */
  admitted = gc2_small_candidate_admit(g, obj2gco(t), a,
					(uint32_t)~LJ_TTAB,
					&base, &start, &gct, &scope);
  UNUSED(base); UNUSED(start); UNUSED(gct);
  if (admitted < 0)
    return -1;
  if (admitted == 0)
    return 0;
  same = array_generation ?
    (const void *)lj_tab_array_acq(t) == generation :
    (const void *)lj_tab_node_acq(t) == generation;
  gc2_mark_scope_leave(&scope);
  return same;
}

static int gc2_markmem_registered_scoped_status(global_State *g, void *p,
						 GC2MarkScope *holdp);
static int gc2_markmem_registered_scoped_status_impl(global_State *g, void *p,
						      GC2MarkScope *holdp,
						      int reclaim_held);

static int gc2_markmem_registered_status(global_State *g, void *p)
{
  GC2MarkScope scope;
  int status = gc2_markmem_registered_scoped_status(g, p, &scope);
  gc2_mark_scope_leave(&scope);
  return status;
}

static int gc2_markmem_status(global_State *g, void *p)
{
  if (!g || !p)
    return GC2_MARK_DEAD;
  if (la_load32_acq(&g->allocf_arena) == 0)
    return GC2_MARK_LIVE_ALREADY;  /* Temporary custom-allocator no-op pin. */
  /* The registry/hugetab lookup proves that the arena-aligned address is one
  ** of ours before any header word is read. A stale raw edge may point into an
  ** unmapped span, so deriving `a` is harmless but lj_arena_ishuge(a) is not. */
  return gc2_markmem_registered_status(g, p);
}

int lj_gc2_markmem(global_State *g, void *p)
{
  return gc2_markmem_status(g, p) == GC2_MARK_NEW;
}

static int gc2_markmem_registered_scoped_status(global_State *g, void *p,
						 GC2MarkScope *holdp)
{
  return gc2_markmem_registered_scoped_status_impl(g, p, holdp, 0);
}

static int gc2_mark_huge_exact_scoped_status(global_State *g, void *p,
					      GC2MarkScope *holdp)
{
  GC2HugeRegistryLease lease = GC2_HUGE_REGISTRY_NONE;
  GC2MarkScope scope;
  TGState *tg;
  LJHugeInfo hi;
  int marked = -1;
  gc2_mark_scope_init(&scope);
  if (!g || !p || !gc2_huge_registry_read_try(g, &lease))
    return GC2_MARK_DEAD;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (!lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      continue;
    marked = lj_arena_hugetab_mark_reader_acquire(
      &tg->huge, p, &scope.huge, &hi);
    if (marked >= 0 || marked == LJ_ARENA_HUGE_READER_OVERFLOW)
      break;
  }
  if (marked < 0 && marked != LJ_ARENA_HUGE_READER_OVERFLOW) {
    tg = g->main_tg;
    if (tg && lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      marked = lj_arena_hugetab_mark_reader_acquire(
	&tg->huge, p, &scope.huge, &hi);
  }
  gc2_huge_registry_read_leave(g, &lease);
  if (marked == LJ_ARENA_HUGE_READER_OVERFLOW ||
      marked == LJ_ARENA_HUGE_MARK_SATURATED) {
    gc2_marks_this_round_add(g, 1);
    gc2_activation_pin_no_reclaim(g);
    return GC2_MARK_DEAD;
  }
  if (marked == LJ_ARENA_HUGE_MARK_INTENT)
    return GC2_MARK_DEAD;
  if (marked < 0)
    return GC2_MARK_DEAD;
  scope.admission = GC2_SCOPE_HUGE_READER;
  if (marked > 0)
    gc2_marks_this_round_add(g, 1);
  if (holdp) {
    *holdp = scope;
    gc2_mark_scope_init(&scope);
  } else {
    gc2_mark_scope_leave(&scope);
  }
  return marked > 0 ? GC2_MARK_NEW : GC2_MARK_LIVE_ALREADY;
}

static int gc2_markmem_registered_scoped_status_impl(global_State *g, void *p,
						      GC2MarkScope *holdp,
						      int reclaim_held)
{
  TGState *tg;
  GCArena *a;
  uint32_t cell;
  int marked, status, smr_held = 0;
  GC2MarkScope scope;
  gc2_mark_scope_init(&scope);
  gc2_mark_scope_init(holdp);
  if (!g || !p)
    return GC2_MARK_DEAD;
  if (la_load32_acq(&g->allocf_arena) == 0)
    return GC2_MARK_LIVE_ALREADY;  /* holdp stays an idempotent no-op token. */
  if (reclaim_held) {
    if (!lj_gc2_mem_registered_known_reclaim_held(g, p))
      return GC2_MARK_DEAD;
  } else {
    if (!lj_gc2_smr_read_try(g)) {
      /* SWEEP keeps registry topology stable. Try the shared small-arena
      ** registry first (its exact rescue CAS precedes any header read), then
      ** use the narrower Huge registry lease and same-slot mark+reader CAS. */
      a = lj_arena_of(p);
      cell = lj_arena_cellof(p);
      status = gc2_mark_small_cell_begin(g, a, cell, &scope);
      if (status != GC2_MARK_DEAD) {
	if (holdp) {
	  *holdp = scope;
	  gc2_mark_scope_init(&scope);
	} else {
	  gc2_mark_scope_leave(&scope);
	}
	return status;
      }
      status = gc2_mark_huge_exact_scoped_status(g, p, holdp);
      if (status != GC2_MARK_DEAD)
	return status;
      gc2_activation_pin_no_reclaim(g);
      return GC2_MARK_DEAD;
    }
    smr_held = 1;
  }
  tg = gc2_tg_for_registered_mem(g, p);
  if (!tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL)) {
    if (smr_held)
      lj_gc2_smr_read_leave(g);
    return GC2_MARK_DEAD;
  }
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    LJHugeInfo hi;
    if (!lj_tg_flags_test_acq(tg, TGF_HUGETAB)) {
      if (smr_held)
	lj_gc2_smr_read_leave(g);
      return GC2_MARK_DEAD;
    }
    marked = lj_arena_hugetab_mark_reader_acquire(
      &tg->huge, p, &scope.huge, &hi);
    if (smr_held)
      lj_gc2_smr_read_leave(g);  /* Huge reader pins header on success. */
    if (marked == LJ_ARENA_HUGE_READER_OVERFLOW ||
	marked == LJ_ARENA_HUGE_MARK_SATURATED) {
      /* MARK may have been published, but raw-body callers need a real token.
      ** Fail closed without dereferencing and keep this cycle from reclaiming
      ** the allocation while a later admission retries. */
      gc2_marks_this_round_add(g, 1);
      if (reclaim_held) {
	/* The marking reader converts a full reader count to MARK_SATURATED,
	** atomically publishing MARK without a body token. READER_OVERFLOW is
	** consequently unreachable for this API; fail-stop if that contract is
	** ever weakened instead of poisoning the current activation from inside
	** its own exclusive writer. The detached-list ticket remains the body
	** authority, while the durable MARK is the arena-quarantine authority. */
	if (LJ_UNLIKELY(marked == LJ_ARENA_HUGE_READER_OVERFLOW)) {
	  lj_assertG(0, "reclaim-held huge mark returned reader overflow");
	  abort();
	}
	return GC2_MARK_LIVE_ALREADY;
      }
      gc2_activation_pin_no_reclaim(g);
      return GC2_MARK_DEAD;
    }
    if (marked == LJ_ARENA_HUGE_MARK_INTENT) {
      /* MARK_INTENT likewise publishes durable liveness but supplies no body
      ** token. An exact detached owner already has independent body authority;
      ** ordinary callers must wait for the allocator retire owner to discharge
      ** the intent and therefore retain the historical DEAD result. */
      if (reclaim_held) {
	gc2_marks_this_round_add(g, 1);
	return GC2_MARK_LIVE_ALREADY;
      }
      return GC2_MARK_DEAD;
    }
    if (marked < 0)
      return GC2_MARK_DEAD;
    scope.admission = GC2_SCOPE_HUGE_READER;
    if (marked > 0)
      gc2_marks_this_round_add(g, 1);
    status = marked > 0 ? GC2_MARK_NEW : GC2_MARK_LIVE_ALREADY;
    if (holdp) {
      *holdp = scope;
      gc2_mark_scope_init(&scope);
    } else {
      gc2_mark_scope_leave(&scope);
    }
    return status;
  }
  cell = lj_arena_cellof(p);
  status = reclaim_held ?
    gc2_mark_small_cell_begin_reclaim_held(g, a, cell, &scope) :
    gc2_mark_small_cell_begin(g, a, cell, &scope);
  if (smr_held)
    lj_gc2_smr_read_leave(g);  /* remote_active now pins a on success. */
  if (status != GC2_MARK_DEAD && holdp) {
    *holdp = scope;
    gc2_mark_scope_init(&scope);
  } else {
    gc2_mark_scope_leave(&scope);
  }
  return status;
}

int lj_gc2_markmem_reclaim_held_status(global_State *g, void *p)
{
  GC2MarkScope scope;
  int status = gc2_markmem_registered_scoped_status_impl(g, p, &scope, 1);
  gc2_mark_scope_leave(&scope);
  return status;
}

int lj_gc2_markmem_registered(global_State *g, void *p)
{
  return gc2_markmem_registered_status(g, p) == GC2_MARK_NEW;
}

int lj_gc2_markmem_registered_publish_try(global_State *g, void *p)
{
  int status;
  if (!g || !p)
    return 0;
  /* Hold one outer registry admission so the existing scoped marker's nested
  ** read is reentrant and cannot mistake an ordinary reclaimer collision for
  ** a missing mandatory edge. The caller's independent list/local ownership
  ** supplies identity lifetime when this tactical admission loses. */
  if (!lj_gc2_smr_read_try(g)) {
    gc2_root_scan_retry(g);
    return 0;
  }
  status = gc2_markmem_registered_status(g, p);
  lj_gc2_smr_read_leave(g);
  return status == GC2_MARK_NEW;
}

int lj_gc2_mem_lease_acquire(global_State *g, void *p, LJGC2Lease *lease)
{
  GC2MarkScope scope;
  int status;
  if (!lease)
    return GC2_MARK_DEAD;
  gc2_public_lease_init(lease);
  status = gc2_markmem_registered_scoped_status(g, p, &scope);
  if (status == GC2_MARK_DEAD) {
    gc2_mark_scope_leave(&scope);
    return status;
  }
  gc2_public_lease_take(lease, &scope);
  return status;
}

/* Establish small-arena lifetime before reading gct/base/direct-body fields,
** and keep the counted admission until all such reads are complete. */
static int gc2_markobj_preserve_status_impl(global_State *g, GCobj *o,
					    void **basep, uint32_t *gctp,
					    int *traversablep,
					    uint32_t expected_gct,
					    GC2MarkScope *holdp,
					    int *retryp)
{
  GC2MarkScope scope;
  void *base;
  uint32_t gct;
  int status, traversable;
  gc2_mark_scope_init(&scope);
  gc2_mark_scope_init(holdp);
  status = gc2_retain_candidate_status(g, o, &base, &gct, &traversable,
				expected_gct, &scope, retryp);
  if (status == GC2_MARK_DEAD) {
    /* Keep the scoped-helper contract explicit even though today's DEAD
    ** retain paths release before returning. */
    gc2_mark_scope_leave(&scope);
    return status;
  }
  if (status == GC2_MARK_NEW && gc2_uncounted_needscan_type(gct)) {
    /* A unique mark transition starts this allocation's participation in the
    ** new cycle. Discard any uncounted queue token left by an aborted/limited
    ** prior-cycle cleanup before current-cycle scheduling observes it. Public
    ** minor sweeping remains disabled, so every reclaiming cycle supplies this
    ** transition for its surviving graph-bearing bodies. */
    (void)gc2_rescan_pending_clear(o);
  }
  gc2_preserve_direct_bodies(g, o);
  if (expected_gct != 0 && gct != expected_gct) {
    if (status == GC2_MARK_NEW && gc2_gct_may_traverse(gct) &&
	(traversable || gct == (uint32_t)~LJ_TUDATA)) {
      uint32_t phase = gc2_phase_acq(g);
      if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK ||
	  phase == LJ_GC2_SWEEP) {
	(void)gc2_publish_mutator_scoped(g, o, &scope);
      }
    }
    gc2_mark_scope_leave(&scope);
    return GC2_MARK_DEAD;
  }
  if (holdp) {
    *holdp = scope;
    gc2_mark_scope_init(&scope);
  } else {
    gc2_mark_scope_leave(&scope);
  }
  if (basep)
    *basep = base;
  if (gctp)
    *gctp = gct;
  if (traversablep)
    *traversablep = traversable;
  return status;
}

static int gc2_markobj_preserve_scoped_status(global_State *g, GCobj *o,
					       uint32_t *gctp,
					       GC2MarkScope *scope,
					       int *retryp)
{
  return gc2_markobj_preserve_status_impl(g, o, NULL, gctp, NULL, 0, scope,
						  retryp);
}

static int gc2_markobj_preserve_status(global_State *g, GCobj *o,
				       void **basep, uint32_t *gctp,
				       int *traversablep)
{
  return gc2_markobj_preserve_status_impl(g, o, basep, gctp, traversablep,
						  0, NULL, NULL);
}

/* Queue ownership needs the transient-admission distinction which ordinary
** mark callers intentionally collapse into DEAD. A RETRY result means no body
** byte was consumed and the exact queue identity must remain durable. */
static int gc2_markobj_preserve_queue_status(global_State *g, GCobj *o,
					       uint32_t *gctp,
					       int *traversablep,
					       GC2MarkScope *scope,
					       int *retryp)
{
  return gc2_markobj_preserve_status_impl(g, o, NULL, gctp, traversablep,
						  0, scope, retryp);
}

static int gc2_markobj_preserve_expected_status(global_State *g, GCobj *o,
						 uint32_t expected_gct)
{
  return gc2_markobj_preserve_status_impl(g, o, NULL, NULL, NULL,
						  expected_gct, NULL, NULL);
}

static int gc2_markobj_expected_scoped_status_mode(global_State *g,
						    GCobj *o,
						    uint32_t expected_gct,
						    uint32_t *gctp,
						    GC2MarkScope *scope,
						    int semantic_publish)
{
  int status;
  int traversable;
  uint32_t gct;
  gc2_mark_scope_init(scope);
  status = gc2_markobj_preserve_status_impl(g, o, NULL, &gct, &traversable,
						     expected_gct, scope, NULL);
  if (status == GC2_MARK_DEAD)
    return GC2_MARK_DEAD;
  if (gctp)
    *gctp = gct;
  if (semantic_publish && gc2_gct_may_traverse(gct)) {
    uint32_t phase = gc2_phase_acq(g);
    if (((status == GC2_MARK_NEW &&
	  (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK)) ||
	 phase == LJ_GC2_SWEEP) &&
	(traversable || gct == (uint32_t)~LJ_TUDATA)) {
      (void)gc2_publish_mutator_scoped(g, o, scope);  /* 05 section 5.6.1. */
    }
  }
  return status;
}

static int gc2_markobj_expected_scoped_status(global_State *g, GCobj *o,
					       uint32_t expected_gct,
					       uint32_t *gctp,
					       GC2MarkScope *scope)
{
  return gc2_markobj_expected_scoped_status_mode(
    g, o, expected_gct, gctp, scope, 1);
}

static int gc2_markobj_expected_status(global_State *g, GCobj *o,
					uint32_t expected_gct,
					uint32_t *gctp)
{
  GC2MarkScope scope;
  int status = gc2_markobj_expected_scoped_status(
    g, o, expected_gct, gctp, &scope);
  gc2_mark_scope_leave(&scope);
  return status;
}

int lj_gc2_obj_lease_acquire(global_State *g, GCobj *o,
				      uint32_t expected_gct, uint32_t *gctp,
				      LJGC2Lease *lease)
{
  GC2MarkScope scope;
  int status;
  if (!lease)
    return GC2_MARK_DEAD;
  gc2_public_lease_init(lease);
  status = gc2_markobj_expected_scoped_status(
    g, o, expected_gct, gctp, &scope);
  if (status == GC2_MARK_DEAD) {
    gc2_mark_scope_leave(&scope);
    return status;
  }
  gc2_public_lease_take(lease, &scope);
  return status;
}

int lj_gc2_markobj_status(global_State *g, GCobj *o, uint32_t *gctp)
{
  return gc2_markobj_expected_status(g, o, 0, gctp);
}

int lj_gc2_markobj_expected_status(global_State *g, GCobj *o,
					    uint32_t expected_gct,
					    uint32_t *gctp)
{
  if (expected_gct == 0)
    return GC2_MARK_DEAD;
  return gc2_markobj_expected_status(g, o, expected_gct, gctp);
}

int lj_gc2_markobj(global_State *g, GCobj *o)
{
  return lj_gc2_markobj_status(g, o, NULL) == GC2_MARK_NEW;
}

static int gc2_markobj_direct_status(global_State *g, GCobj *o)
{
  return lj_gc2_markobj_status(g, o, NULL);
}

int lj_gc2_markobj_direct(global_State *g, GCobj *o)
{
  return gc2_markobj_direct_status(g, o) == GC2_MARK_NEW;
}

int lj_gc2_markobj_nogrey(global_State *g, GCobj *o)
{
  int status;
  uint32_t gct;
  status = gc2_markobj_preserve_status(g, o, NULL, &gct, NULL);
  if (status == GC2_MARK_DEAD)
    return 0;
  /* Only leaf strings and a lua_State whose owner immediately performs the
  ** complete synchronous stack traversal may share the semantic mark domain
  ** without publishing grey work. Graph-bearing body-retention users must use
  ** lj_gc2_markobj(), otherwise a later semantic edge can see ALREADY and skip
  ** the graph. */
  if (gct != (uint32_t)~LJ_TSTR && gct != (uint32_t)~LJ_TTHREAD) {
    lj_assertG(0, "graph-bearing no-grey mark without synchronous traversal");
    abort();
  }
  return status == GC2_MARK_NEW;
}

static int gc2_markobj_worker_status(global_State *g, GCobj *o,
				     uint32_t *gctp)
{
  int status;
  int traversable;
  uint32_t gct;
  status = gc2_markobj_preserve_status(g, o, NULL, &gct, &traversable);
  if (status == GC2_MARK_DEAD)
    return status;
  if (gctp)
    *gctp = gct;
  if (gct == (uint32_t)~LJ_TSTR)
    return status;
  if (status == GC2_MARK_NEW && gc2_gct_may_traverse(gct)) {
    if (!(traversable || gct == (uint32_t)~LJ_TUDATA))
      return status;
    {
      (void)gc2_publish_worker(g, o);  /* 05 section 5.6.3. */
    }
  }
  return status;
}

static int gc2_markobj_worker(global_State *g, GCobj *o)
{
  return gc2_markobj_worker_status(g, o, NULL) == GC2_MARK_NEW;
}

static void gc2_mark_tv_worker(global_State *g, cTValue *tv)
{
  if (tvisgcv(tv) && gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    (void)gc2_trace_sweep_tv_edge(g, tv, 1);
    return;
  }
  if (tvisgcv(tv))
    gc2_markobj_worker(g, gcV(tv));
}

static int gc2_mark_thread_root_obj_worker_status(global_State *g, GCobj *o)
{
  int status;
  if (!o)
    return 1;
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    (void)gc2_trace_sweep_worker_edge(g, o);
    return 1;  /* DEAD is represented by SWEEP recovery work. */
  }
  if (gc2_root_test_take_semantic_retry(o))
    return 0;
  status = gc2_markobj_worker_status(g, o, NULL);
  if (status == GC2_MARK_DEAD)
    return 0;
  if (status == GC2_MARK_NEW)
    return 1;
  if (status == GC2_MARK_LIVE_ALREADY)
    gc2_thread_root_rescan_marked_obj(g, o);
  return 1;
}

static void gc2_mark_thread_root_obj_worker(global_State *g, GCobj *o)
{
  (void)gc2_mark_thread_root_obj_worker_status(g, o);
}

static void gc2_mark_thread_root_tv_worker(global_State *g, cTValue *tv)
{
  if (tvisgcv(tv) && gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    (void)gc2_trace_sweep_tv_edge(g, tv, 1);
    return;
  }
  if (tvisgcv(tv))
    gc2_mark_thread_root_obj_worker(g, gcV(tv));
}

static int gc2_mark_thread_root_tv_worker_status(global_State *g, cTValue *tv)
{
  if (tvisgcv(tv) && gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    (void)gc2_trace_sweep_tv_edge(g, tv, 1);
    return 1;
  }
  if (tvisgcv(tv))
    return gc2_mark_thread_root_obj_worker_status(g, gcV(tv));
  return 1;
}

static int gc2_mark_upval_payload_tv_worker_status(global_State *g,
					    cTValue *tv)
{
  if (tvisgcv(tv) && gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    (void)gc2_trace_sweep_tv_edge(g, tv, 1);
    return 1;
  }
  if (!tvisgcv(tv))
    return 1;
  if (tvistab(tv))
    return gc2_mark_thread_root_tv_worker_status(g, tv);
  return gc2_mark_payload_obj_worker(g, gcV(tv)) != GC2_MARK_DEAD;
}

static void gc2_mark_upval_payload_tv_worker(global_State *g, cTValue *tv)
{
  (void)gc2_mark_upval_payload_tv_worker_status(g, tv);
}

static LJ_AINLINE int gc2_rescan_pending_set(GCobj *o)
{
  uint8_t old = la_or8_rlx(&o->gch.marked, LJ_GC_NEEDSCAN);
  return (old & LJ_GC_NEEDSCAN) == 0;
}

static LJ_AINLINE uint8_t gc2_rescan_pending_clear(GCobj *o)
{
  return la_and8_rlx(&o->gch.marked, (uint8_t)~LJ_GC_NEEDSCAN);
}

/* Reserve one exact aggregate credit for a table rescan publication. The
** caller must unconditionally finish the reservation after its queue attempt. */
static LJ_AINLINE int gc2_table_rescan_pending_begin(global_State *g,
						       GCtab *t)
{
  GCobj *o = obj2gco(t);
  uint8_t expect = LJ_TAB_RESCAN_NONE;

  /* Reserve the aggregate credit before exposing INSTALLING. A concurrent
  ** clearer can consume only a COUNTED token, so it can neither steal this
  ** provisional credit nor clear the header hint between the old OR/inc pair.
  ** A failed installer immediately returns its provisional credit. */
  gc2_table_rescan_pending_inc(g);
  if (!lj_tab_gc2_rescan_state_cas(
	 t, &expect, LJ_TAB_RESCAN_INSTALLING)) {
    gc2_table_rescan_pending_dec(g);
    (void)gc2_rescan_pending_set(o);
    return 0;
  }
  (void)gc2_rescan_pending_set(o);
  gc2_table_rescan_test_pause_at(LJ_GC2_TABLE_RESCAN_TEST_INSTALLING);
  return 1;
}

static void gc2_table_rescan_hint_clear(global_State *g, GCtab *t)
{
  GCobj *o = obj2gco(t);
  UNUSED(g);
  (void)gc2_rescan_pending_clear(o);
  gc2_table_rescan_test_pause_at(LJ_GC2_TABLE_RESCAN_TEST_HINT_CLEARED);
  /* A new installer can win after a prior token reaches NONE but before this
  ** header clear. Restore the hint; its own aggregate credit remains exact. */
  if (lj_tab_gc2_rescan_state_acq(t) != LJ_TAB_RESCAN_NONE)
    (void)gc2_rescan_pending_set(o);
}

/* Settle the reservation after the queue publication attempt. A consumer may
** have changed INSTALLING to CANCELLED after consuming an older/just-published
** queue item. In that case it deliberately leaves the aggregate credit for
** this publisher to discharge; successful publication remains concrete but
** no longer needs a counted token. */
static void gc2_table_rescan_pending_finish(global_State *g, GCtab *t,
						      int published)
{
  uint8_t state = lj_tab_gc2_rescan_state_acq(t);
  for (;;) {
    uint8_t expect = state;
    if (state == LJ_TAB_RESCAN_INSTALLING) {
      uint8_t next = published ? LJ_TAB_RESCAN_COUNTED : LJ_TAB_RESCAN_NONE;
      if (!lj_tab_gc2_rescan_state_cas(t, &expect, next)) {
	state = expect;
	continue;
      }
      if (!published) {
	gc2_table_rescan_pending_dec(g);
	gc2_table_rescan_hint_clear(g, t);
      }
      return;
    }
    if (state == LJ_TAB_RESCAN_CANCELLED) {
      if (!lj_tab_gc2_rescan_state_cas(
	    t, &expect, LJ_TAB_RESCAN_NONE)) {
	state = expect;
	continue;
      }
      gc2_table_rescan_pending_dec(g);
      gc2_table_rescan_hint_clear(g, t);
      return;
    }
    /* Only this unique installer can publish COUNTED or settle CANCELLED to
    ** NONE, and each successful CAS returns above. Observing either state on a
    ** fresh loop is therefore a duplicate finisher/ABA symptom, not a racing
    ** clearer outcome. Preserve safety in release builds as well as assertions. */
    lj_assertG(0, "duplicate table NEEDSCAN publish settlement");
    gc2_recovery_fail_closed(g);
    return;
  }
}

/* Clear an admitted table's exact aggregate membership. INSTALLING is changed
** to CANCELLED, not decremented: the publisher still owns the reservation and
** may be concurrently making its queue entry visible. */
static LJ_AINLINE int gc2_table_rescan_pending_clear_held(global_State *g,
							   GCtab *t)
{
  uint8_t state = lj_tab_gc2_rescan_state_acq(t);
  for (;;) {
    if (state == LJ_TAB_RESCAN_INSTALLING) {
      uint8_t expect = LJ_TAB_RESCAN_INSTALLING;
      if (!lj_tab_gc2_rescan_state_cas(
	    t, &expect, LJ_TAB_RESCAN_CANCELLED)) {
	state = expect;
	continue;
      }
      /* CANCELLED remains a live aggregate reservation until its publisher
      ** observes it and settles. Keep the header hint paired with that credit. */
      return 0;
    }
    if (state == LJ_TAB_RESCAN_CANCELLED)
      return 0;
    if (state == LJ_TAB_RESCAN_COUNTED) {
      uint8_t expect = LJ_TAB_RESCAN_COUNTED;
      if (!lj_tab_gc2_rescan_state_cas(t, &expect, LJ_TAB_RESCAN_NONE)) {
	state = expect;
	continue;
      }
      gc2_table_rescan_pending_dec(g);
    } else {
      lj_assertG(state == LJ_TAB_RESCAN_NONE,
		 "bad table NEEDSCAN membership state");
    }
    gc2_table_rescan_hint_clear(g, t);
    return 1;
  }
}

static LJ_AINLINE int gc2_table_rescan_pending_clear_gct(global_State *g,
							 GCobj *o,
							 uint32_t gct)
{
  return gct == (uint32_t)~LJ_TTAB ?
    gc2_table_rescan_pending_clear_held(g, gco2tab(o)) : 0;
}

/* A caller without an existing allocation scope must distinguish a transient
** admission veto from a terminal stale identity. Traversal callers use the
** held variant above so a RECOVERY/MUTATING arbitration window cannot leak the
** token after consuming its queue item. */
static LJ_AINLINE int gc2_table_rescan_pending_clear(global_State *g,
							     GCobj *o)
{
  GC2MarkScope scope;
  GCtab *t = NULL;
  int status = gc2_expected_tab_scoped_status(g, o, &t, &scope);
  int cleared;
  if (status != GC2_TAB_SCOPE_VALID) {
    gc2_mark_scope_leave(&scope);
    return status == GC2_TAB_SCOPE_RETRY ? -1 : -2;
  }
  cleared = gc2_table_rescan_pending_clear_held(g, t);
  gc2_mark_scope_leave(&scope);
  return cleared;
}

static int gc2_table_rescan_publish_reserved(global_State *g, GCtab *t)
{
  int pushed;
  if (LJ_UNLIKELY(!g || !t))
    return 0;  /* Internal contract; no reservation exists without both. */
  pushed = gc2_publish_mutator(g, obj2gco(t));
  gc2_table_rescan_pending_finish(g, t, pushed);
  if (!pushed) {
    /* Recovery publication already pinned reclamation fail-closed. Drop the
    ** exact queue-membership credit so explicit collection returns that
    ** failure instead of waiting forever for work which cannot materialize. */
    gc2_marks_this_round_add(g, 1);
    lj_gc2_worker_wake(g);
  }
  return pushed;
}

static int gc2_table_rescan_publish_duplicate(global_State *g, GCtab *t)
{
  int pushed;
  if (LJ_UNLIKELY(!g || !t))
    return 0;
  pushed = gc2_publish_mutator(g, obj2gco(t));
  if (!pushed) {
    gc2_marks_this_round_add(g, 1);
    lj_gc2_worker_wake(g);
  }
  return pushed;
}

#if defined(LJ_GC2_TEST_HELPERS)
int lj_gc2_test_table_rescan_set(global_State *g, GCtab *t)
{
  int installed;
  if (!g || !t)
    return 0;
  installed = gc2_table_rescan_pending_begin(g, t);
  if (installed)
    gc2_table_rescan_pending_finish(g, t, 1);
  return installed;
}

int lj_gc2_test_table_rescan_clear(global_State *g, GCtab *t)
{
  return g && t ? gc2_table_rescan_pending_clear_held(g, t) : 0;
}

int lj_gc2_test_table_expected_status(global_State *g, GCtab *t)
{
  GC2MarkScope scope;
  GCtab *admitted = NULL;
  int status;
  if (!g || !t)
    return GC2_TAB_SCOPE_STALE;
  status = gc2_expected_tab_scoped_status(
    g, obj2gco(t), &admitted, &scope);
  gc2_mark_scope_leave(&scope);
  return status == GC2_TAB_SCOPE_VALID ? LJ_GC2_TV_EDGE_VALID :
    status == GC2_TAB_SCOPE_RETRY ? LJ_GC2_TV_EDGE_RETRY :
				    LJ_GC2_TV_EDGE_STALE;
}

/* Replay a delayed prior-generation hint-clear tail under an exact body lease.
** The pause inside the real helper exposes its clear/recheck repair window. */
void lj_gc2_test_table_rescan_stale_hint_clear(global_State *g, GCtab *t)
{
  GC2MarkScope scope;
  GCtab *admitted = NULL;
  int status;
  if (!g || !t)
    return;
  status = gc2_expected_tab_scoped_status(g, obj2gco(t), &admitted, &scope);
  if (status == GC2_TAB_SCOPE_VALID)
    gc2_table_rescan_hint_clear(g, admitted);
  gc2_mark_scope_leave(&scope);
}
#endif

static LJ_AINLINE int gc2_table_scan_current(global_State *g, GCtab *t)
{
  LJGC2TabStamp *s;
  uint32_t cycle;
  if (!g || !t)
    return 0;
  cycle = gc2_cycle_acq(g);
  if (cycle == 0)
    return 0;
  s = gc2_table_stamp(g, t);
  return s && gc2_tabstamp_cycle(la_load64_acq(&s->state)) == cycle;
}

#if defined(LJ_GC2_TEST_HELPERS)
int lj_gc2_test_table_scan_current(global_State *g, GCtab *t)
{
  return gc2_table_scan_current(g, t);
}
#endif

static int gc2_table_rescan_later_(global_State *g, GCtab *t, int force)
{
  if (!g || !t)
    return 0;
  if (!force && gc2_table_scan_current(g, t))
    return 1;
  if (!gc2_table_rescan_pending_begin(g, t)) {
    /* The per-table state, not the header hint, proves an existing exact
    ** reservation. If its queue is no longer visible, publish a duplicate
    ** identity instead of clearing through a fresh transient admission. */
    if (gc2_table_scan_current(g, t) ||
	!gc2_grey_empty(g) || !lj_gc2_ssb_empty(g))
      return 1;
    return gc2_table_rescan_publish_duplicate(g, t);
  }
  /*
  ** Table traversal can run from root-scan contexts as well as worker-owned
  ** drains. Publish deferred table rescans through the multi-producer SSB; the
  ** worker token converts SSB entries to grey work later. Keep NEEDSCAN set if
  ** publication cannot complete so fixpoint cannot close over skipped storage.
  */
  return gc2_table_rescan_publish_reserved(g, t);
}

static int gc2_table_rescan_later(global_State *g, GCtab *t)
{
  return gc2_table_rescan_later_(g, t, 0);
}

static int gc2_table_rescan_later_force(global_State *g, GCtab *t)
{
  return gc2_table_rescan_later_(g, t, 1);
}

static void gc2_table_rescan_requeue(global_State *g, GCtab *t)
{
  GC2MarkScope scope;
  GCtab *admitted = NULL;
  int cleared, status;
  if (!g || !t)
    return;
  status = gc2_expected_tab_scoped_status(g, obj2gco(t), &admitted, &scope);
  if (status != GC2_TAB_SCOPE_VALID) {
    gc2_mark_scope_leave(&scope);
    if (status == GC2_TAB_SCOPE_RETRY) {
      /* The raw identity is authoritative but its body is temporarily owned.
      ** Transfer the requeue request to the allocation-side recovery lane;
      ** never reuse t after a failed admission. */
      if (!gc2_recovery_publish(g, obj2gco(t)))
	gc2_recovery_fail_closed(g);
      return;
    }
    gc2_recovery_fail_closed(g);
    return;
  }
  t = admitted;
  cleared = gc2_table_rescan_pending_clear_held(g, t);
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    gc2_mark_marked_table_payload_worker(g, t);
  } else if (cleared > 0) {
    (void)gc2_table_rescan_later_force(g, t);
  } else {
    (void)gc2_table_rescan_publish_duplicate(g, t);
  }
  gc2_mark_scope_leave(&scope);
}

static void gc2_table_rescan_requeue_held(global_State *g, GCtab *t)
{
  int cleared;
  if (!g || !t)
    return;
  cleared = gc2_table_rescan_pending_clear_held(g, t);
  if (gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    gc2_mark_marked_table_payload_worker(g, t);
    return;
  }
  if (cleared)
    (void)gc2_table_rescan_later_force(g, t);
  else
    (void)gc2_table_rescan_publish_duplicate(g, t);
}

static void gc2_mark_marked_table_payload_worker(global_State *g, GCtab *t)
{
  GCobj *o;
  uint32_t phase;
  int pushed;
  if (!g || !t || gc2_table_scan_current(g, t))
    return;
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK) {
    (void)gc2_table_rescan_later(g, t);
    return;
  }
  if (phase != LJ_GC2_SWEEP)
    return;
  /*
  ** SWEEP root tracing must close table side-vector reachability before arena
  ** reuse starts. MARK/WEAK table rescans may still be represented only by SSB
  ** NEEDSCAN bits, but SWEEP drains the grey deque directly, so publish a direct
  ** grey item even when NEEDSCAN was already set by an earlier handoff.
  */
  o = obj2gco(t);
  (void)gc2_rescan_pending_set(o);
  pushed = gc2_publish_worker(g, o);
  if (!pushed)
    gc2_activation_pin_no_reclaim(g);
}

static int gc2_mark_payload_obj_worker(global_State *g, GCobj *o)
{
  uint32_t gct;
  int status = gc2_markobj_worker_status(g, o, &gct);
  if (status == GC2_MARK_DEAD)
    return status;
  if (status == GC2_MARK_NEW)
    return status;
  switch (gct) {
  case ~LJ_TTAB:
    gc2_mark_marked_table_payload_worker(g, gco2tab(o));
    break;
  case ~LJ_TFUNC:
  case ~LJ_TPROTO:
  case ~LJ_TTHREAD:
  case ~LJ_TUDATA:
  case ~LJ_TUPVAL:
#if LJ_HASJIT
  case ~LJ_TTRACE:
#endif
    /*
    ** A marked arena bit is not proof that a mutable or graph-bearing payload was
    ** traversed in this cycle. Queue already-marked non-table containers for
    ** ordinary GC2 traversal; table payloads use the table-child helper with its
    ** local seen set, and strings/cdata have no GC2 child graph to rescan.
    */
    if (gc2_rescan_pending_set(o)) {
      int pushed = gc2_publish_worker(g, o);
      if (!pushed)
	gc2_activation_pin_no_reclaim(g);
      UNUSED(pushed);
    }
    break;
  default:
    break;
  }
  return status;
}

static void gc2_mark_table_child_obj_worker(global_State *g, GCtab *t)
{
  GCobj *o;
  uint32_t gct;
  uint32_t phase;
  if (!t)
    return;
  o = obj2gco(t);
  {
    int status = gc2_markobj_worker_status(g, o, &gct);
    if (status == GC2_MARK_DEAD)
      return;
    if (status == GC2_MARK_NEW)
      return;
  }
  if (gct != (uint32_t)~LJ_TTAB)
    return;
  /*
  ** A marked table body is not proof that its payload was scanned for this
  ** cycle. Requeue already-marked child tables unless their scan stamp proves
  ** the current dirty epoch is covered. This keeps table graph traversal bounded:
  ** child tables become ordinary grey work instead of recursive work hidden
  ** inside one worker budget item.
  */
  phase = gc2_phase_acq(g);
  if (phase == LJ_GC2_MARK || phase == LJ_GC2_WEAK ||
      phase == LJ_GC2_SWEEP)
    gc2_mark_marked_table_payload_worker(g, t);
}

static void gc2_mark_table_child_tv_worker(global_State *g, cTValue *tv)
{
  if (tvisgcv(tv) && gc2_phase_acq(g) == LJ_GC2_SWEEP) {
    (void)gc2_trace_sweep_tv_edge(g, tv, 1);
    return;
  }
  if (!tvisgcv(tv))
    return;
  if (tvistab(tv))
    gc2_mark_table_child_obj_worker(g, tabV(tv));
  else
    gc2_mark_payload_obj_worker(g, gcV(tv));
}

void lj_gc2_barrier_marked_proto(lua_State *L, GCproto *pt)
{
  global_State *g;
  GCobj *o;
  uint32_t phase;
  int pushed;
  if (!L || !pt)
    return;
  g = G(L);
  o = obj2gco(pt);
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK)
    return;
  if (!mt_active_or_entering_acq(g) && gc2_n_workers_acq(g) == 0 &&
      g->allocf == lj_arena_allocf && la_load32_acq(&g->allocf_arena) != 0 &&
      !lj_arena_ishuge(lj_arena_of(pt)) &&
      (lj_arena_flags_acq(lj_arena_of(pt)) & LJ_AF_TRAVERSABLE) &&
      lj_arena_cellof(pt) >= LJ_AFIRST_CELL &&
      lj_arena_cellof(pt) < LJ_ARENA_CELLS &&
      lj_arena_cellptr(lj_arena_of(pt), lj_arena_cellof(pt)) == (void *)pt &&
      pt->gct == ~LJ_TPROTO &&
      lj_arena_bm_get(lj_arena_of(pt)->block, lj_arena_cellof(pt)) &&
      lj_arena_ready_get(lj_arena_of(pt), lj_arena_cellof(pt)) &&
      lj_arena_bm_get(lj_arena_of(pt)->mark, lj_arena_cellof(pt))) {
    /* Authoritative constructor/recorder prototype under the sole-runtime
    ** gate: the exact arena cannot transfer or disappear during this call. */
  } else if (lj_gc2_ismarked(g, o) <= 0) {
    (void)lj_gc2_markobj_direct(g, o);
    return;
  }
  /*
  ** Active black allocation sets a proto's arena mark bit before constructor
  ** edges publish. A normal barrier then sees "already marked" and would not
  ** queue traversal, so immutable proto children can be swept while the fresh
  ** closure survives by its birth mark. Queue each such proto once; the bit can
  ** remain set after traversal because protos are immutable and future cycles
  ** with cleared arena marks take the ordinary mark-and-queue path.
  */
  if (!gc2_rescan_pending_set(o))
    return;
  pushed = gc2_publish_mutator(g, o);
  if (!pushed)
    gc2_activation_pin_no_reclaim(g);
}

static LJ_AINLINE void gc2_rescan_pending_clear_if_table(global_State *g,
							 GCobj *o)
{
  gc2_table_rescan_pending_clear(g, o);
}

static LJ_AINLINE void gc2_rescan_pending_clear_cycle(global_State *g,
						      GCobj *o)
{
  GC2MarkScope scope;
  uint32_t gct;
  if (!gc2_markobj_base_valid_scoped(g, o, NULL, &gct, &scope))
    return;
  if (gct == (uint32_t)~LJ_TTAB) {
    gc2_table_rescan_pending_clear_gct(g, o, gct);
  } else if (gc2_gct_may_traverse(gct)) {
    /*
    ** LJ_GC_NEEDSCAN is queue membership for already-marked mutable containers.
    ** Tables pair it with a pending counter; other containers only need the bit
    ** cleared once their queued traversal has run or the cycle is abandoned.
    */
    (void)gc2_rescan_pending_clear(o);
  }
  gc2_mark_scope_leave(&scope);
}

void lj_gc2_test_rescan_pending_clear_if_table(global_State *g, GCobj *o)
{
  gc2_rescan_pending_clear_if_table(g, o);
}

void lj_gc2_test_rescan_pending_clear_cycle(global_State *g, GCobj *o)
{
  gc2_rescan_pending_clear_cycle(g, o);
}

static void gc2_note_weak_table(global_State *g, GCtab *t, int weak)
{
  if (!weak)
    return;
  if (gc2_tab_is_ffi_fin(g, t))
    return;  /* FFI finalizer registry is owned by FINREG, not weak clear. */
  /* 05 section 5.8: capture traversal-time weak mode. */
  lj_obj_masksetgcflags(obj2gco(t), LJ_GC_WEAK, weak);
  gc2_weak_tables_seen_add(g, 1);
  if (weak & LJ_GC_WEAKKEY)
    gc2_weak_tables_weakkey_add(g, 1);
  if (weak & LJ_GC_WEAKVAL)
    gc2_weak_tables_weakval_add(g, 1);
  if (weak == LJ_GC_WEAK)
    gc2_weak_tables_allweak_add(g, 1);
  gc2_weak_record(g, t);
}

#if LJ_HASJIT
static void gc2_marktrace_worker(global_State *g, TraceNo traceno)
{
  GCtrace *T = gc2_traceref_safe(g, traceno);
  if (T)
    gc2_markobj_worker(g, obj2gco(T));
}

static void gc2_mark_proto_for_trace_pc_worker(global_State *g,
					       const BCIns *pc)
{
  if (!pc)
    return;
  (void)lj_gc2_mark_proto_for_pc(g, pc);
}

static void gc2_mark_trace_snapshot_pcs_worker(global_State *g, GCtrace *T)
{
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  MSize nsnapmap = trace_nsnapmap_acq(T);
  SnapNo i, nsnap = trace_nsnap_acq(T);
  for (i = 0; i < nsnap; i++) {
    SnapShot *s = &snap[i];
    MSize ofs = snap_mapofs_acq(s);
    MSize nent = snap_nent_acq(s);
    SnapEntry *map;
    if (ofs >= nsnapmap || nent >= nsnapmap - ofs)
      return;
    map = &snapmap[ofs];
    gc2_mark_proto_for_trace_pc_worker(g,
				       snap_pc_acq(&map[nent]));
  }
}
#endif

static int gc2_traverse_tab_rec(global_State *g, GCtab *t, int record_weak,
				 int already_admitted)
{
  GC2MarkScope scope;
  GCtab *mt;
  int smr = 0;
  int weak = 0, ffi_fin = 0;
  TValue *array = NULL;
  MSize asize = 0, acap = 0, hmask = 0;
  Node *node = NULL;
  int array_status, node_status;
  uint32_t cycle, dirty0;
  int stamped, result = 0, tabstatus, weak_retry = 0;
  gc2_mark_scope_init(&scope);
  if (!already_admitted) {
    tabstatus = gc2_expected_tab_scoped_status(
      g, obj2gco(t), &t, &scope);
    /* Transient rescue ownership is a retry result, not permission to wait
    ** behind a descheduled allocator/sweeper while monopolizing GC work. */
    if (LJ_UNLIKELY(tabstatus == GC2_TAB_SCOPE_RETRY))
      return 0;
    if (LJ_UNLIKELY(tabstatus != GC2_TAB_SCOPE_VALID)) {
      gc2_mark_scope_leave(&scope);
      return 0;
    }
  }
  if (LJ_UNLIKELY(!lj_gc2_smr_read_try(g))) {
    gc2_table_rescan_requeue_held(g, t);
    goto out;
  }
  smr = 1;
  cycle = gc2_cycle_acq(g);
  dirty0 = gc2_table_dirty_epoch(g, t, &stamped);
  mt = lj_tab_metatable_acq(t);
  /*
  ** NEEDSCAN is queue membership, not a proof that this traversal owns all
  ** future table writes. Keep it set while scanning and clear it only after the
  ** dirty epoch proves the scan covered a stable payload.
  */
  array_status = lj_tab_array_snapshot_gc_held(g, t, &array, &asize, &acap);
  node_status = lj_tab_node_snapshot_gc_held(g, t, &node, &hmask);
  if (LJ_UNLIKELY(array_status == LJ_TAB_GC_SNAPSHOT_TRANSIENT ||
			  node_status == LJ_TAB_GC_SNAPSHOT_TRANSIENT)) {
    gc2_table_rescan_requeue_held(g, t);
    goto out;
  }
  if (LJ_UNLIKELY(array_status != LJ_TAB_GC_SNAPSHOT_OK ||
			  node_status != LJ_TAB_GC_SNAPSHOT_OK)) {
    (void)gc2_table_rescan_pending_clear_held(g, t);
    goto out;
  }
  /* Metadata reached from an admitted worker traversal is not a new semantic
  ** SWEEP root. Retain and mark the metatable/__mode bodies for this read,
  ** while the child-edge helper below owns any required graph scheduling. */
  weak = gc2_tab_weak_mode(g, t, mt, 1, 1, 0, &weak_retry);
  if (LJ_UNLIKELY(weak_retry)) {
    gc2_table_rescan_requeue_held(g, t);
    goto out;
  }
  ffi_fin = gc2_tab_is_ffi_fin(g, t);
  if (record_weak)
    gc2_note_weak_table(g, t, weak);  /* 05 section 5.8 discovery scaffold. */
  if (array)
    lj_gc2_markmem(g, acap ? (void *)lj_tab_array_hdrw(array) :
				      (void *)array);
  if (hmask > 0)
    lj_gc2_markmem(g, lj_tab_node_hdrw(node));
  gc2_mark_table_child_obj_worker(g, mt);
  if (weak == LJ_GC_WEAK)
    goto finish_scan;
  if (!(weak & LJ_GC_WEAKVAL)) {
    MSize i;
    for (i = 0; i < asize; i++) {
      TValue val;
      lj_tv_load_acq(&val, &array[i]);
      if (LJ_UNLIKELY(tvisforward(&val)))
	goto retry_scan;
      gc2_mark_table_child_tv_worker(g, &val);
    }
  }
  {
    MSize i;
    if (hmask > 0) {
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	TValue key, val;
	int key_loaded = 0;
	lj_tv_load_acq(&val, &n->val);
#if LJ_HASFFI
	if (ffi_fin) {
	  lj_tv_load_acq(&key, &n->key);
	  key_loaded = 1;
	  if (LJ_UNLIKELY(lj_cdata_fin_isclaim(&val) ||
			  tviskeylock(&key)))
	    goto retry_scan;
	}
#endif
	if (tvisforward(&val)) {
	  if (!key_loaded) {
	    lj_tv_load_acq(&key, &n->key);
	    key_loaded = 1;
	  }
	  if (LJ_UNLIKELY(tviskeylock(&key)))
	    goto retry_scan;
	  if (tvisnil(&key))
	    continue;
	  goto retry_scan;
	}
	if (!tvisnil(&val)) {
	  if (!key_loaded)
	    lj_tv_load_acq(&key, &n->key);
	  if (LJ_UNLIKELY(tviskeylock(&key)))
	    goto retry_scan;
	  lj_assertG(!tvisnil(&key), "mark of nil key in non-empty slot");
	  lj_assertG(!tviskeylock(&key),
			     "mark of key lock in non-empty slot");
	  if (!(weak & LJ_GC_WEAKKEY))
	    gc2_mark_table_child_tv_worker(g, &key);
	  if (!(weak & LJ_GC_WEAKVAL))
	    gc2_mark_table_child_tv_worker(g, &val);
	}
      }
    }
  }
finish_scan:
  if (!stamped) {
    (void)gc2_table_rescan_pending_clear_held(g, t);
  } else if (gc2_table_scan_publish(g, t, cycle, dirty0)) {
    /*
    ** Publish the scan stamp before clearing NEEDSCAN. A racing writer may still
    ** dirty the table and set NEEDSCAN before the clear; the post-clear dirty
    ** check below repairs that lost-clear race by publishing a fresh rescan.
    */
    (void)gc2_table_rescan_pending_clear_held(g, t);
    if (!gc2_table_scan_current(g, t))
      (void)gc2_table_rescan_later_force(g, t);
  } else {
    gc2_table_rescan_requeue_held(g, t);
  }
  result = 1;
  goto out;
retry_scan:
  /* Retain NEEDSCAN and republish bounded work. Partial marking/weak clearing
  ** is monotonic and idempotent; the next turn restarts from a fresh snapshot. */
  gc2_table_rescan_requeue_held(g, t);
out:
  if (smr)
    lj_gc2_smr_read_leave(g);
  gc2_mark_scope_leave(&scope);
  return result;
}

static int gc2_traverse_tab(global_State *g, GCtab *t)
{
  return gc2_traverse_tab_rec(g, t, 1, 0);
}

static int gc2_traverse_tab_admitted(global_State *g, GCtab *t,
				      int record_weak)
{
  /* gc2_traverse_obj retains the exact body scope across this call. Reusing
  ** expected-type admission here would treat worker-owned graph traversal as
  ** a fresh semantic SWEEP root and REDIRTY its own recovery identity. */
  return gc2_traverse_tab_rec(g, t, record_weak, 1);
}

#if LJ_HASFFI
static void gc2_traverse_clib_retired_cache(global_State *g)
{
  CLibCacheEntry *e, *next;
  GC2RootCycleGuard guard;
  /*
  ** Retired CLibrary cache entries follow the raw SMR-list convention used by
  ** retired table and mcode metadata: validate the node before reading its
  ** payload or next link, then let the surrounding SMR reader keep it stable.
  */
  e = lj_clib_cache_retired_head_acq(g);
  gc2_root_cycle_guard_init(&guard, e);
  while (e != NULL && lj_gc2_mem_registered(g, e)) {
    GCstr *name = lj_clib_cache_name_acq(e);
    TValue tv;
    next = lj_clib_cache_retired_next_acq(e);
    lj_gc2_markmem(g, e);
    if (name)
      lj_gc2_markobj(g, obj2gco(name));
    lj_clib_cache_val_acq(&tv, e);
    gc2_mark_tv(g, &tv);
    e = next;
    if (LJ_UNLIKELY(!gc2_root_cycle_guard_step(&guard, e))) {
      gc2_root_scan_retry(g);
      break;
    }
  }
  if (LJ_UNLIKELY(e != NULL && !lj_gc2_mem_registered(g, e)))
    gc2_root_scan_retry(g);
}

static void gc2_traverse_clib_cache(global_State *g, CLibrary *cl)
{
  GCtab *cache_env = lj_clib_cache_env_acq(cl);
  CLibCacheEntry *e;
  if (cache_env)
    gc2_markobj_worker(g, obj2gco(cache_env));
  for (e = lj_clib_cache_head_acq(cl);
       e != NULL;
       e = lj_clib_cache_next_acq(e)) {
    GCstr *name = lj_clib_cache_name_acq(e);
    TValue tv;
    lj_gc2_markmem(g, e);
    if (name)
      gc2_markobj_worker(g, obj2gco(name));
    lj_clib_cache_val_acq(&tv, e);
    gc2_mark_tv_worker(g, &tv);
  }
}
#endif

static void gc2_traverse_udata(global_State *g, GCudata *ud)
{
  GCSize payload_size;
  GCtab *mt;
  GCtab *env;
  uint8_t udtype;
  if (!lj_gc_udata_payload_valid(ud, &payload_size))
    return;
  UNUSED(payload_size);
  mt = lj_udata_metatable_acq(ud);
  env = lj_udata_env_acq(ud);
  udtype = lj_udata_udtype_acq(ud);
  if (mt)
    gc2_markobj_worker(g, obj2gco(mt));
  if (env)
    gc2_markobj_worker(g, obj2gco(env));
#if LJ_HASFFI
  if (udtype == UDTYPE_FFI_CLIB)
    gc2_traverse_clib_cache(g, (CLibrary *)uddata(ud));
#endif
  if (LJ_HASBUFFER && udtype == UDTYPE_BUFFER) {
    SBufExt *sbx = (SBufExt *)uddata(ud);
    GCobj *ref;
    if (!sbufiscoworborrow(sbx))
      lj_gc2_markmem(g, lj_buf_bptr_acq((SBuf *)sbx));
    ref = lj_bufx_cowref_acq(sbx);
    if (sbufiscow(sbx) && ref)
      gc2_markobj_worker(g, ref);
    ref = obj2gco(lj_bufx_dict_str_acq(sbx));
    if (ref)
      gc2_markobj_worker(g, ref);
    ref = obj2gco(lj_bufx_dict_mt_acq(sbx));
    if (ref)
      gc2_markobj_worker(g, ref);
  }
  if (udtype == UDTYPE_CHANNEL) {
    LJChan *ch = (LJChan *)uddata(ud);
    uint32_t i;
    for (i = 0; i < ch->cap; i++) {
      TValue tv;
      lj_tv_load_acq(&tv, &ch->slot[i].tv);
      gc2_mark_tv_worker(g, &tv);  /* 09 section 9.5. */
    }
  }
  if (udtype == UDTYPE_THREAD) {
    LJThread *th = (LJThread *)uddata(ud);
    TValue *roots = lj_thread_start_roots_acq(th);
    uint32_t i, n = lj_thread_start_root_count_acq(th);
    lua_State *child = lj_thread_state_load_acq(th);
    GC2MarkScope rootscope;
    int rootstatus = roots && n <= LUAI_MAXSTACK ?
      gc2_markmem_registered_scoped_status(g, roots, &rootscope) :
      GC2_MARK_DEAD;
    if (rootstatus != GC2_MARK_DEAD) {
	for (i = 0; i < n; i++) {
	  TValue tv;
	  lj_tv_load_acq(&tv, &roots[i]);
	  /* Captured channel/threading roots are userdata payload edges, not a
	  ** mutable lua_State stack snapshot. Use the private payload path so an
	  ** exact self-channel cycle consumes its current-cycle visit stamp. */
	  gc2_mark_upval_payload_tv_worker(g, &tv);
	}
      gc2_mark_scope_leave(&rootscope);
    }
    if (child)
      gc2_markobj_worker(g, obj2gco(child));  /* 09 section 9.2. */
  }
}

static void gc2_traverse_upval(global_State *g, GCupval *uv)
{
  TValue *slot = uvval(uv);
  TValue tv;
  /*
  ** Live closed and open upvalues publish uv->v before any edge can name the
  ** GCupval. A NULL payload pointer can only come from an acquired stale edge
  ** to an arena body that is no longer a valid upvalue payload owner.
  */
  if (LJ_UNLIKELY(slot == NULL))
    return;
  lj_tv_load_acq(&tv, slot);
  gc2_mark_upval_payload_tv_worker(g, &tv);
}

/* Geometry-only validation. Every caller holds the exact prototype body lease
** from before entry through its final proto field or bytecode read. */
static int gc2_valid_proto_for_traverse_held(GCproto *pt)
{
  MSize minpt;
  if (!pt || !checkptrGC(pt) || pt->gct != ~LJ_TPROTO)
    return 0;
  if (pt->sizept < sizeof(GCproto) || pt->sizept > LJ_MAX_MEM32)
    return 0;
  if (pt->sizebc == 0 || pt->sizebc > LJ_MAX_BCINS)
    return 0;
  if (pt->framesize > LJ_MAX_SLOTS || pt->sizeuv > LJ_MAX_UPVAL)
    return 0;
  if ((uintptr_t)mref(pt->jit_startins, void) !=
      (uintptr_t)(void *)(proto_bc(pt) + pt->sizebc))
    return 0;
  minpt = (MSize)sizeof(GCproto) + pt->sizebc*2u*(MSize)sizeof(BCIns);
  minpt = (minpt + (MSize)sizeof(TValue)-1) & ~((MSize)sizeof(TValue)-1);
  return minpt <= pt->sizept;
}

int lj_gc2_valid_proto_for_traverse_held(global_State *g, GCproto *pt,
					 const LJGC2Lease *lease)
{
  GCArena *a;
  intptr_t admission;
  if (!g || !pt || !lease)
    return 0;
  /* Custom lua_Alloc is intentionally unreclaimed during this temporary
  ** compatibility tranche, so its documented no-op lease covers the body. */
  if (la_load32_acq(&g->allocf_arena) == 0) {
    LJGC2Lease zero;
    memset(&zero, 0, sizeof(zero));
    return memcmp(lease, &zero, sizeof(zero)) == 0 &&
	   gc2_valid_proto_for_traverse_held(pt);
  }
  if (lease->admission == (intptr_t)GC2_SCOPE_HUGE_READER) {
    /* Verify the fixed header before reading sizept, then prove the complete
    ** compact body interval against the same opaque HugeReader token. */
    if (lease->arena != NULL ||
	!lj_arena_hugetab_reader_covers_range(
	  &lease->huge, pt, sizeof(GCproto)) ||
	!gc2_valid_proto_for_traverse_held(pt))
      return 0;
    return lj_arena_hugetab_reader_covers_range(
      &lease->huge, pt, (size_t)pt->sizept);
  }
  a = lj_arena_of(pt);
  admission = lease->admission;
  if (lease->arena != (void *)a ||
      (intptr_t)(int)admission != admission ||
      !gc2_mark_admission_counted((int)admission))
    return 0;
  return gc2_valid_proto_for_traverse_held(pt);
}

static GCproto *gc2_func_proto_for_traverse(global_State *g, GCfunc *fn,
					     GC2MarkScope *ptscope)
{
  const char *pc;
  GCproto *pt;
  uint32_t gct;
  gc2_mark_scope_init(ptscope);
  if (!ptscope)
    return NULL;
  if (!fn || !isluafunc(fn) || lj_funcL_nupvalues(&fn->l) > LJ_MAX_UPVAL)
    return NULL;
  pc = mref(fn->l.pc, const char);
  if (!pc || !checkptrGC(pc))
    return NULL;
  pt = (GCproto *)(void *)(pc - sizeof(GCproto));
  if (!gc2_observed_obj_valid_scoped(g, obj2gco(pt), &gct, ptscope) ||
      gct != (uint32_t)~LJ_TPROTO ||
      !gc2_valid_proto_for_traverse_held(pt) ||
      lj_funcL_nupvalues(&fn->l) > pt->sizeuv) {
    gc2_mark_scope_leave(ptscope);
    return NULL;
  }
  return pt;
}

static void gc2_traverse_func(global_State *g, GCfunc *fn)
{
  GCtab *env = lj_func_env_acq(fn);
  if (env)
    gc2_mark_thread_root_obj_worker(g, obj2gco(env));
  if (isluafunc(fn)) {
    GC2MarkScope ptscope;
    GCproto *pt = gc2_func_proto_for_traverse(g, fn, &ptscope);
    uint32_t i, nup = lj_funcL_nupvalues(&fn->l);
    if (LJ_UNLIKELY(!pt))
      return;
    {
      int ptstatus = gc2_mark_payload_obj_worker(g, obj2gco(pt));
      if (ptstatus != GC2_MARK_DEAD)
	gc2_traverse_proto(g, pt, ptstatus == GC2_MARK_NEW);
    }
    for (i = 0; i < nup; i++) {
      GCobj *uv = obj2gco(func_uv_acq(&fn->l, i));
      if (uv && uv->gch.gct == ~LJ_TUPVAL)
	gc2_mark_payload_obj_worker(g, uv);
    }
    gc2_mark_scope_leave(&ptscope);
  } else {
    uint32_t i, nup = lj_funcC_nupvalues(&fn->c);
    for (i = 0; i < nup; i++) {
      TValue tv;
      lj_tv_load_acq(&tv, &fn->c.upvalue[i]);
      /*
      ** Match the color collector: C closure upvalues are strong payload edges,
      ** but function-valued upvalues can form closure cycles. Rescan mutable
      ** containers reached through already-marked values and use the ordinary
      ** function mark path for function values.
      */
      gc2_mark_upval_payload_tv_worker(g, &tv);
    }
  }
}

#if LJ_HASJIT
static void gc2_traverse_trace(global_State *g, GCtrace *T)
{
  IRIns *irbase;
  IRRef ref;
  if (trace_traceno_acq(T) == 0)
    return;
  if (!gc2_trace_geometry_valid(T))
    return;
  irbase = trace_ir_acq(T);
  for (ref = trace_nk_acq(T); ref < REF_TRUE; ref++) {
	IRIns *ir = &irbase[ref];
	IRIns irs = ir_load_acq(ir);
	if (irs.o == IR_KGC)
	  gc2_mark_payload_obj_worker(g, ir_kgc_load_acq(ir));
	if (irt_is64(irs.t) && irs.o != IR_KNULL)
	  ref++;
      }
  gc2_marktrace_worker(g, trace_link_acq(T));
  gc2_marktrace_worker(g, trace_nextroot_acq(T));
  gc2_marktrace_worker(g, trace_nextside_acq(T));
  gc2_mark_payload_obj_worker(g, trace_startptgco_acq(T));
  gc2_mark_trace_snapshot_pcs_worker(g, T);
}
#endif

static void gc2_traverse_proto(global_State *g, GCproto *pt, int force)
{
  uint32_t cycle = gc2_cycle_acq(g);
  ptrdiff_t i, sizekgc = (ptrdiff_t)proto_sizekgc_acq(pt);
  /*
  ** KGC and chunkname edges are immutable after READY. The trace anchor is the
  ** sole mutable GC edge and its publisher marks the new trace independently
  ** through lj_gc_pubtrace(). Therefore a completed same-cycle scan covers the
  ** prototype payload for every closure sharing it.
  **
  ** Store the stamp only after every child mark/publication returns. The
  ** worker token excludes phase close during this traversal. A first mark is
  ** forced even if a 32-bit stamp happens to match after wrap; cycle zero
  ** disables deduplication and the wrap cycle is forced major at mark start.
  */
  if (!force && cycle != 0 && proto_gc2_scan_cycle_acq(pt) == cycle)
    return;
  gc2_markobj_worker(g, obj2gco(proto_chunkname_acq(pt)));
  for (i = -sizekgc; i < 0; i++)
    gc2_mark_payload_obj_worker(g, proto_kgc_acq(pt, i));
#if LJ_HASJIT
  gc2_marktrace_worker(g, proto_trace_acq(pt));
#endif
  proto_gc2_scan_cycle_rel(pt, cycle);
}

static void gc2_mark_frame_chain_funcs_worker(global_State *g, lua_State *L)
{
  TValue *bot, *max, *frame;
  uint32_t n = 0;
  if (!L || tvref(L->stack) == NULL)
    return;
  bot = tvref(L->stack);
  max = tvref(L->maxstack);
  frame = L->base - 1;
  while (frame > bot + LJ_FR2 && frame < max) {
    GCfunc *fn;
    TValue *prev;
    GC2FrameScope scope;
    if (!gc2_frame_prev_safe(g, bot, max, frame, &prev, &fn, &scope))
      break;
    if (fn) {
      if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
	(void)lj_gc2_trace_sweep_root(g, obj2gco(fn));
      else
	gc2_mark_thread_root_obj_worker(g, obj2gco(fn));
    }
    gc2_frame_scope_leave(&scope);
    if (prev >= frame || prev <= bot + LJ_FR2 || prev >= max)
      break;
    frame = prev;
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT)
      break;
  }
}

static TValue *gc2_stack_scan_top_worker(global_State *g, lua_State *L,
					 int *conservativep)
{
  TValue *frame, *bot = tvref(L->stack);
  TValue *top = L->top, *used = L->top - 1, *max = tvref(L->maxstack);
  uint32_t n = 0;
  int vm_current = gc2_thread_is_current(g, L);
  int remote_current = gc2_thread_is_remote_current(g, L);
  int native_current = gc2_thread_is_native_current(g, L);
  int jit_current = gc2_thread_is_jit_current(g, L);
  if (conservativep)
    *conservativep = 0;
  if (remote_current || native_current || jit_current) {
    gc2_mark_frame_chain_funcs_worker(g, L);
#if LJ_HASJIT
    if (jit_current)
      gc2_mark_jit_frame_funcs(g, L);
#endif
    /* The widened tail includes ordinary popped/spill cells above L->top.
    ** Validate their tagged pointers before treating them as semantic roots. */
    if (conservativep)
      *conservativep = 1;
    return max;
  }
  for (frame = L->base - 1; frame > bot + LJ_FR2; ) {
    GCfunc *fn;
    GCproto *pt;
    TValue *prev;
    GC2FrameScope scope;
    TValue *ftop = frame;
    if (!gc2_frame_prev_safe(g, bot, max, frame, &prev, &fn, &scope)) {
      gc2_thread_scan_frame_fallbacks_add(g, 1);
      if (conservativep)
	*conservativep = 1;
      return max;
    }
    pt = fn ? gc2_func_proto_if_lua(fn) : NULL;
    if (pt)
      ftop += pt->framesize;
    if (ftop > used)
      used = ftop;
    /*
    ** Match the same-thread scanner: frame functions are payload roots, not
    ** just live closure bodies. Already-marked closures still need their proto
    ** and upvalue edges sampled for this worker cycle.
    */
    if (fn) {
      if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
	(void)lj_gc2_trace_sweep_root(g, obj2gco(fn));
      else
	gc2_mark_thread_root_obj_worker(g, obj2gco(fn));
    }
    gc2_frame_scope_leave(&scope);
    if (prev >= frame || prev <= bot + LJ_FR2 || prev >= max)
      break;
    frame = prev;
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT) {
      gc2_thread_scan_frame_fallbacks_add(g, 1);
      if (conservativep)
	*conservativep = 1;
      return max;
    }
  }
  used++;
  if (used > max)
    used = max;
  if (vm_current && L->base > bot + 1 + LJ_FR2) {
    top = gc2_active_thread_top(g, L, top, NULL);
  } else {
    TValue *ctop = curr_top(L);
    if (ctop > top)
      top = ctop;
    if (used > top)
      top = used;
  }
  return top > max ? max : top;
}

static int gc2_thread_owner_scans(global_State *g, lua_State *th)
{
  TGState *tg;
  uint64_t scan_epoch, scanned_dirty, handoff_epoch, owner_dirty, cycle;
  if (!g || !th)
    return 0;
  owner_dirty = gc2_thread_owner_dirty(g, th, &tg);
  if (!tg)
    return 0;
  cycle = gc2_thread_scan_cycle_acq(g);
  /* A same-cycle scan only covers this grey item after its NEEDSCAN handoff. */
  handoff_epoch = lj_state_scan_handoff_epoch_acq(th);
  if (handoff_epoch != cycle || gc2_thread_needscan(th))
    return 0;
  scan_epoch = lj_state_scan_epoch_acq(th);
  if (scan_epoch != cycle)
    return 0;
  scanned_dirty = lj_state_scan_dirty_epoch_acq(th);
  if (scanned_dirty != owner_dirty) {
    gc2_thread_scan_dirty_misses_add(g, 1);
    return 0;
  }
  return 1;
}

static int gc2_thread_has_live_owner(global_State *g, lua_State *th)
{
  uint32_t owner;
  TGState *tg;
  if (!g || !th)
    return 0;
  owner = lj_state_owner_acq(th);
  if (!lj_thr_id_is_owner(owner))
    return 0;
  tg = lj_tg_find_owner(g, owner);
  return tg && !lj_tg_flags_test_acq(tg, TGF_DEAD);
}

static int gc2_stack_in_arena(GCArena *a, GCArena *want, uint32_t cell,
			      uint32_t ncells)
{
  if (a != want)
    return 0;
  return cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS &&
	 ncells <= LJ_ARENA_CELLS - cell &&
	 lj_arena_bm_get(a->block, cell);
}

static int gc2_stack_in_arena_list(GCArena *head, GCArena *want,
				   uint32_t cell, uint32_t ncells)
{
  GCArena *a;
  for (a = head; a != NULL; a = lj_arena_next_acq(a)) {
    if (gc2_stack_in_arena(a, want, cell, ncells))
      return 1;
  }
  return 0;
}

static int gc2_stack_mem_valid_tg(TGState *tg, GCArena *a, uint32_t cell,
				  uint32_t ncells)
{
  uint32_t k;
  if (!tg)
    return 0;
  for (k = 0; k < LJ_ARENA_NKINDS; k++) {
    if (gc2_stack_in_arena(tg->alloc.bump[k].a, a, cell, ncells) ||
	gc2_stack_in_arena_list(tg->alloc.owned[k], a, cell, ncells) ||
	gc2_stack_in_arena_list(tg->alloc.needsweep[k], a, cell, ncells))
      return 1;
  }
  return 0;
}

static int gc2_stack_mem_valid(global_State *g, TValue *st, MSize stacksize)
{
  TGState *tg, *curtg;
  GCArena *a;
  uint32_t cell, ncells;
  int registered;
  if (!st || stacksize == 0 || !checkptrGC(st))
    return 0;
  if (g->allocf != lj_arena_allocf)
    return 1;
  a = lj_arena_of(st);
  cell = lj_arena_cellof(st);
  ncells = lj_arena_ncells((size_t)stacksize * sizeof(TValue));
  registered = gc2_small_arena_registered(g, a, NULL);
  if (registered > 0)
    return cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS &&
	   ncells <= LJ_ARENA_CELLS - cell &&
	   lj_arena_bm_get(a->block, cell);
  if (registered < 0) {
    for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
      if (gc2_stack_mem_valid_tg(tg, a, cell, ncells))
	return 1;
    }
    if (gc2_stack_mem_valid_tg(g->main_tg, a, cell, ncells))
      return 1;
    curtg = G2TG(g);
    if (curtg != g->main_tg && curtg && curtg->gl == g &&
	gc2_stack_mem_valid_tg(curtg, a, cell, ncells))
      return 1;
  }
  /* Large Lua stacks are exact HugeTab allocations, not small arenas. Acquire
  ** a counted reader before validating the complete stack byte interval. */
  {
    GC2MarkScope scope;
    size_t bytes = (size_t)stacksize * sizeof(TValue);
    int status, valid = 0;
    gc2_mark_scope_init(&scope);
    if (bytes / sizeof(TValue) != (size_t)stacksize)
      return 0;
    status = gc2_mark_huge_exact_scoped_status(g, st, &scope);
    if (status != GC2_MARK_DEAD &&
	scope.admission == GC2_SCOPE_HUGE_READER)
      valid = lj_arena_hugetab_reader_covers_range(&scope.huge, st, bytes);
    gc2_mark_scope_leave(&scope);
    return valid;
  }
}

/* The body scope is separate from stack-owner authority. Callers retain both:
** `held` prevents physical state destruction, while owner/GCSCAN authority
** prevents concurrent stack replacement during geometry and payload reads. */
static int gc2_valid_thread_for_traverse_held(global_State *g,
					       lua_State *th,
					       const GC2MarkScope *held)
{
  TValue *st, *end, *maxstack;
  MSize stacksize;
  if (!g || !th || !checkptrGC(th) || th->gct != ~LJ_TTHREAD ||
      mref(th->glref, global_State) != g)
    return 0;
  if (th != mainthread_acq(g) &&
      la_load32_acq(&g->allocf_arena) != 0 &&
      (!held || held->a != lj_arena_of(th) ||
       !gc2_mark_admission_counted(held->admission)))
    return 0;
  stacksize = th->stacksize;
  if (stacksize < 1 + LJ_STACK_EXTRA ||
      stacksize > LUAI_MAXSTACK + 1 + LJ_STACK_EXTRA)
    return 0;
  st = tvref(th->stack);
  maxstack = tvref(th->maxstack);
  if (!gc2_stack_mem_valid(g, st, stacksize) || !checkptrGC(maxstack) ||
      !checkptrGC(th->base) || !checkptrGC(th->top))
    return 0;
  end = st + stacksize;
  if (maxstack != end - LJ_STACK_EXTRA - 1)
    return 0;
  if (th->base < st || th->base > end || th->top < st || th->top > end)
    return 0;
  return 1;
}

static void gc2_traverse_thread(global_State *g, lua_State *th,
				const GC2MarkScope *held)
{
  LJStateClaim claim;
  GCobj *mt, *uv;
  TValue *o, *top;
  TValue tv;
  uint64_t dirty_epoch, cycle;
  uint32_t owner, current_tid;
  int sweep, registry_lease = 0;
  int conservative = 0;
  int stack_retry = 0;
  claim.L = NULL;
  claim.tg_hint = NULL;
  claim.tid = 0;
  claim.release = 0;
  /* Identity traversal may originate in the process-global registry scan. Do
  ** not validate stack geometry or allocator membership until authority is
  ** proved below. The nonwaiting lease also keeps a dead owner's allocator/TG
  ** storage stable through a successful terminal GCSCAN takeover. */
  if (!lj_gc2_smr_read_try(g))
    goto requeue_thread;
  registry_lease = 1;
  owner = lj_state_owner_acq(th);
  current_tid = lj_thr_current_id(g);
  if (owner != current_tid || !lj_thr_id_is_owner(current_tid)) {
    if (lj_state_gcscan_claim(th, &claim))
      goto scan_thread;
    gc2_thread_scan_busy_add(g, 1);
    if (gc2_thread_owner_scans(g, th)) {
      lj_state_scan_handoff_epoch_rel(th, 0);
      gc2_thread_scan_owner_scans_add(g, 1);
    } else {
      if (!gc2_thread_has_live_owner(g, th)) {
	uint32_t expect;
	/*
	** A state left owned by a dead TG has no mutator to acknowledge NEEDSCAN,
	** but dead-owner detection alone is not stack authority. Convert the exact
	** stale owner id to GCSCAN only after the registry contains no TG with that
	** tid. A registered DEAD TG is still inside detach-before-state-release and
	** must be retried. The lease makes physical absence stable; a concurrent
	** state release/claim loses the CAS without exposing stack fields.
	*/
	owner = lj_state_owner_acq(th);
	if (owner == 0 && lj_state_gcscan_claim(th, &claim))
	  goto scan_thread;
	expect = owner;
	if (lj_thr_id_is_owner(owner) &&
	    lj_tg_find_owner(g, owner) == NULL &&
	    lj_state_owner_cas(th, &expect, LJ_THREAD_GCSCAN)) {
	  claim.L = th;
	  claim.tid = LJ_THREAD_GCSCAN;
	  claim.release = 1;
	  goto scan_thread;
	}
	goto requeue_thread;
      } else {
	/*
	** Once NEEDSCAN is set, the pending counter is the work item. Keeping a
	** duplicate copy on the grey deque lets the collector repeatedly pop the
	** same busy state while the owner is the only thread that can scan it.
	*/
	gc2_thread_needscan_test_pause_at(
	  LJ_GC2_THREAD_NEEDSCAN_TEST_BEFORE_SET);
	gc2_thread_set_needscan(g, th);
	gc2_thread_needscan_test_pause_at(
	  LJ_GC2_THREAD_NEEDSCAN_TEST_AFTER_SET);
	/* The owner release path passes through GCSCAN before publishing zero.
	** Recheck after the NEEDSCAN LP so either that release hook observes the
	** bit or this traversal retains its own concrete retry. */
	if (!gc2_thread_has_live_owner(g, th))
	  goto requeue_thread;
      }
    }
    goto out_thread;  /* 05 section 5.7.2: owner scan/handoff preserves work. */
  }
  /*
  ** collectgarbage("collect") can traverse the running lua_State while the C API
  ** holds its ordinary owner claim. Requeueing that state waits for the same OS
  ** thread to perform an owner scan, but the owner is already inside the collector.
  ** Same-owner traversal is stable and preserves the ordinary stack claim.
  */
scan_thread:
  if (!gc2_valid_thread_for_traverse_held(g, th, held))
    goto out_thread;
  cycle = gc2_thread_scan_cycle_acq(g);
  sweep = gc2_phase_acq(g) == LJ_GC2_SWEEP;
  gc2_thread_scan_claims_add(g, 1);
  lj_gc2_markmem(g, tvref(th->stack));
  top = gc2_stack_scan_top_worker(g, th, &conservative);
  for (o = tvref(th->stack) + 1 + LJ_FR2; o < top; o++) {
    lj_tv_load_acq(&tv, o);
    if (conservative) {
      GC2MarkScope scope;
      int admitted = gc2_tv_admit_scoped(g, &tv, &scope);
      if (LJ_UNLIKELY(admitted == GC2_TV_SCOPE_RETRY)) {
	stack_retry = 1;
	gc2_mark_scope_leave(&scope);
	continue;
      }
      if (admitted == GC2_TV_SCOPE_STALE) {
	gc2_mark_scope_leave(&scope);
	continue;
      }
      if (sweep && tvisthread(&tv) && threadV(&tv) == th) {
	gc2_mark_scope_leave(&scope);
	continue;
      }
      if (sweep) {
	/* A stack TValue is a public semantic root. Do not apply the worker-edge
	** current-table/NEEDSCAN filters used for private graph descendants. */
	if (LJ_UNLIKELY(!gc2_mark_thread_root_tv_status(g, &tv)))
	  stack_retry = 1;
      } else if (LJ_UNLIKELY(
		   !gc2_mark_thread_root_tv_worker_status(g, &tv))) {
	stack_retry = 1;
      }
      gc2_mark_scope_leave(&scope);
    } else if (sweep) {
      if (tvisthread(&tv) && threadV(&tv) == th)
	continue;
      if (LJ_UNLIKELY(!gc2_mark_thread_root_tv_status(g, &tv)))
	stack_retry = 1;
    } else {
      if (LJ_UNLIKELY(!gc2_mark_thread_root_tv_worker_status(g, &tv)))
	stack_retry = 1;
    }
  }
  if (th == lj_tg_cur_L(g) && th->cframe != NULL) {
#if LJ_HASJIT
    gc2_mark_active_cframe_proto_root(g, th);
#endif
  }
  {
    GCtab *env = lj_state_env_acq(th);
    if (env) {
      int admitted = sweep ?
	gc2_mark_thread_root_obj_status(g, obj2gco(env)) :
	gc2_mark_thread_root_obj_worker_status(g, obj2gco(env));
      if (LJ_UNLIKELY(!admitted))
	stack_retry = 1;
    }
  }
  mt = lj_state_mt_thread_acq(th);
  if (mt != NULL) {
    int admitted = sweep ? gc2_mark_thread_root_obj_status(g, mt) :
	gc2_markobj_worker_status(g, mt, NULL) != GC2_MARK_DEAD;
    if (LJ_UNLIKELY(!admitted))
      stack_retry = 1;
  }
  for (uv = lj_state_openupval_acq(th); uv != NULL;
       uv = lj_obj_gcw_acq(uv)) {
    int uv_admitted = sweep ? gc2_mark_thread_root_obj_status(g, uv) :
	gc2_markobj_worker_status(g, uv, NULL) != GC2_MARK_DEAD;
    if (LJ_UNLIKELY(!uv_admitted)) {
      stack_retry = 1;
    } else if (uv->gch.gct == ~LJ_TUPVAL) {
      TValue tv;
      lj_tv_load_acq(&tv, uvval(gco2uv(uv)));
      int admitted = sweep ? gc2_mark_upval_payload_tv_status(g, &tv) :
	gc2_mark_upval_payload_tv_worker_status(g, &tv);
      if (LJ_UNLIKELY(!admitted))
	stack_retry = 1;
    }
  }
  if (LJ_UNLIKELY(stack_retry))
    goto requeue_thread;
  dirty_epoch = gc2_thread_owner_dirty(g, th, NULL);
  lj_state_scan_dirty_epoch_rel(th, dirty_epoch);
  lj_state_scan_epoch_rel(th, cycle);
  lj_state_scan_handoff_epoch_rel(th, 0);
  if (!gc2_thread_clear_needscan(g, th))
    goto requeue_thread;
out_thread:
  lj_state_dropclaim(&claim);
  if (registry_lease)
    lj_gc2_smr_read_leave(g);
  return;

requeue_thread:
  /* This label is also reached after an acquired GCSCAN claim. Release stack
  ** authority before publishing the concrete thread retry, then end the SMR
  ** registry lease only after that publication is durable. */
  lj_state_dropclaim(&claim);
  {
    int pushed = gc2_publish_worker(g, obj2gco(th));
    if (!pushed)
      gc2_activation_pin_no_reclaim(g);
    gc2_marks_this_round_add(g, 1);
    lj_gc2_worker_wake(g);
  }
  if (registry_lease)
    lj_gc2_smr_read_leave(g);
}

enum {
  GC2_TRAVERSE_RETRY = 0,
  GC2_TRAVERSE_DONE = 1,
  GC2_TRAVERSE_STALE = 2
};

static int gc2_traverse_obj(global_State *g, GCobj *o)
{
  GC2MarkScope scope;
  int status, retry = 0;
  uint32_t gct;
  if (g && o && mainthread_acq(g) && o == obj2gco(mainthread_acq(g))) {
    gc2_traverse_thread(g, mainthread_acq(g), NULL);
    return GC2_TRAVERSE_DONE;
  }
  /* Queue membership is not a lifetime pin. Re-enter the exact arena rescue
  ** protocol before reading gct or any payload and retain the counted
  ** admission through the complete one-object traversal. Terminal sweep never
  ** waits for this reader: it fails its seal/commit predicate and retries. */
  status = gc2_markobj_preserve_scoped_status(g, o, &gct, &scope, &retry);
  if (LJ_UNLIKELY(status == GC2_MARK_DEAD))
    return retry ? GC2_TRAVERSE_RETRY : GC2_TRAVERSE_STALE;
  if (LJ_UNLIKELY(gct == 0 || gct < ~LJ_TSTR || gct > ~LJ_TUDATA)) {
    /*
    ** Grey queues are lock-free handoff lists. A queued arena body can be
    ** reclaimed and reused before a duplicate/stale entry drains; non-GC type
    ** bytes are not traversable payloads for the current cycle.
    */
    goto out;
  } else if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    (void)gc2_traverse_tab_admitted(
      g, gco2tab(o), gc2_phase_acq(g) != LJ_GC2_SWEEP);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    gc2_traverse_func(g, gco2func(o));
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    if (gc2_valid_proto_for_traverse_held(pt))
      gc2_traverse_proto(g, pt, 0);
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    gc2_traverse_thread(g, gco2th(o), &scope);
  } else if (gct == ~LJ_TUPVAL) {
    gc2_traverse_upval(g, gco2uv(o));
  } else if (gct == ~LJ_TUDATA) {
    gc2_traverse_udata(g, gco2ud(o));
    /* UDATA has no owner-handoff counter or requeue path. This admitted
    ** traversal consumes its exact NEEDSCAN queue membership outside SWEEP.
    ** During SWEEP the bit is the current-major private-cycle token and remains
    ** set until the next unique NEW transition or best-effort IDLE cleanup. */
    if (gc2_phase_acq(g) != LJ_GC2_SWEEP)
      (void)gc2_rescan_pending_clear(o);
#if LJ_HASJIT
  } else if (gct == ~LJ_TTRACE) {
    gc2_traverse_trace(g, gco2trace(o));
#endif
  } else {
    lj_assertG(gct == ~LJ_TSTR || gct == ~LJ_TCDATA,
	       "bad GC type %d", gct);
  }
out:
  gc2_mark_scope_leave(&scope);
  return GC2_TRAVERSE_DONE;
}

static void gc2_recovery_count_complete(global_State *g)
{
  if (LJ_UNLIKELY(!gc2_recovery_items_dec(g))) {
    /* Every completion caller already cleared its authoritative recovery
    ** state. There is no locator with which to replay an aggregate underflow,
    ** and returning would permit caller post-effects after count corruption. */
    gc2_recovery_fail_closed(g);
    abort();
  }
  gc2_recovery_drained_add(g, 1);
}

static void gc2_recovery_huge_count_complete(global_State *g)
{
  if (LJ_UNLIKELY(!gc2_recovery_huge_items_dec(g))) {
    /* recovery_complete already erased/folded the sole authoritative HugeTab
    ** state. No retry locator exists, and continuing could unmap the body or
    ** publish sweep progress with an orphaned aggregate identity. Preserve
    ** fail-closed evidence for the crash dump, then fail-stop before either
    ** caller post-completion effect. */
    gc2_recovery_fail_closed(g);
    abort();
  }
  gc2_recovery_count_complete(g);
}

#if defined(LJ_GC2_TEST_HELPERS)
void lj_gc2_test_recovery_huge_count_complete(global_State *g)
{
  gc2_recovery_huge_count_complete(g);
}
#endif

static void gc2_recovery_complete_main(global_State *g)
{
  uint32_t expect = LJ_ARENA_RECOVERY_CLAIMED;
  if (gc2_recovery_main_state_cas(g, &expect,
					  LJ_ARENA_RECOVERY_IDLE)) {
    gc2_recovery_count_complete(g);
    return;
  }
  if (expect == LJ_ARENA_RECOVERY_REDIRTY) {
    uint32_t redirty = expect;
    if (gc2_recovery_main_state_cas(g, &redirty,
					    LJ_ARENA_RECOVERY_PENDING)) {
      lj_gc2_worker_wake(g);
      return;
    }
  }
  gc2_recovery_fail_closed(g);
}

static void gc2_recovery_complete_small(global_State *g, GCArena *a,
						 uint32_t start)
{
  if (lj_arena_recovery_state_cas(a, start, LJ_ARENA_RECOVERY_CLAIMED,
					  LJ_ARENA_RECOVERY_IDLE)) {
    gc2_recovery_count_complete(g);
    lj_arena_recovery_complete_wake(a);
    return;
  }
  if (lj_arena_recovery_state_cas(a, start, LJ_ARENA_RECOVERY_REDIRTY,
					  LJ_ARENA_RECOVERY_PENDING)) {
    lj_gc2_worker_wake(g);
    return;
  }
  gc2_recovery_fail_closed(g);
}

static void gc2_recovery_retry_small(global_State *g, GCArena *a,
					      uint32_t start)
{
  if (lj_arena_recovery_state_cas(a, start, LJ_ARENA_RECOVERY_CLAIMED,
					  LJ_ARENA_RECOVERY_PENDING) ||
      lj_arena_recovery_state_cas(a, start, LJ_ARENA_RECOVERY_REDIRTY,
					  LJ_ARENA_RECOVERY_PENDING)) {
    lj_gc2_worker_wake(g);
    return;
  }
  if (lj_arena_recovery_state_acq(a, start) !=
      LJ_ARENA_RECOVERY_PENDING)
    gc2_recovery_fail_closed(g);
}

static void gc2_recovery_complete_huge(global_State *g, HugeTab *ht,
						void *base)
{
  LJHugeInfo hi;
  int completed = lj_arena_hugetab_recovery_complete(ht, base, &hi);
  if (completed == LJ_ARENA_HUGE_RECOVERY_COMPLETE_LIVE ||
      completed == LJ_ARENA_HUGE_RECOVERY_COMPLETE_SWEEP ||
      completed == LJ_ARENA_HUGE_RECOVERY_COMPLETE_UNMAP) {
    gc2_recovery_huge_count_complete(g);
    if (completed == LJ_ARENA_HUGE_RECOVERY_COMPLETE_UNMAP)
      lj_arena_huge_unmap_claimed(base, hi.size);
    else if (completed == LJ_ARENA_HUGE_RECOVERY_COMPLETE_SWEEP) {
      gc2_sweep_grace_needed_rel(g, 1);
      lj_gc2_worker_wake(g);
    }
    return;
  }
  if (completed == LJ_ARENA_HUGE_RECOVERY_COMPLETE_REQUEUED) {
    lj_gc2_worker_wake(g);
    return;
  }
  if (lj_arena_hugetab_recovery_state_cas(
	  ht, base, LJ_ARENA_RECOVERY_REDIRTY,
	  LJ_ARENA_RECOVERY_PENDING, NULL)) {
    lj_gc2_worker_wake(g);
    return;
  }
  gc2_recovery_fail_closed(g);
}

static int gc2_recovery_drain_main_one(global_State *g)
{
  uint32_t expect = LJ_ARENA_RECOVERY_PENDING;
  lua_State *mainL;
  if (!gc2_recovery_main_state_cas(g, &expect,
					   LJ_ARENA_RECOVERY_CLAIMED))
    return 0;
  mainL = mainthread_acq(g);
  if (mainL)
    /* GG_State lifetime is the admission for this one embedded graph object;
    ** generic arena validation deliberately rejects it. */
    gc2_traverse_thread(g, mainL, NULL);
  gc2_recovery_test_pause_at(LJ_GC2_RECOVERY_TEST_PRE_COMPLETE);
  gc2_recovery_complete_main(g);
  return 1;
}

static int gc2_recovery_drain_small_one(global_State *g)
{
  HugeTab *registry = (HugeTab *)gc2_small_arena_tab_acq(g);
  uint32_t saved_slot, saved_cell, pass;
  if (!registry)
    return 0;
  if (!lj_gc2_smr_read_try(g))
    return 0;
  saved_slot = gc2_recovery_small_slot_rlx(g);
  saved_cell = gc2_recovery_small_cell_rlx(g);
  if (saved_cell < LJ_AFIRST_CELL || saved_cell >= LJ_ARENA_CELLS)
    saved_cell = LJ_AFIRST_CELL;
  for (pass = 0;
       pass < ((saved_slot || saved_cell != LJ_AFIRST_CELL) ? 2u : 1u);
       pass++) {
    uint32_t cursor = pass == 0 ? saved_slot : 0;
    void *p;
    LJHugeInfo hi;
    while (lj_arena_hugetab_next(registry, &cursor, &p, &hi)) {
      GCArena *a = (GCArena *)p;
      uint32_t entry = cursor - 1u;
      uint32_t cell = pass == 0 && entry == saved_slot ?
	  saved_cell : LJ_AFIRST_CELL;
      uint32_t endcell = pass != 0 && entry == saved_slot ?
	  saved_cell : LJ_ARENA_CELLS;
      UNUSED(hi);
      if (pass != 0 && entry > saved_slot)
	break;  /* One exact wrapped registry pass. */
      if (!a || (lj_arena_flags_acq(a) &
		  (LJ_AF_REGISTERED|LJ_AF_TRAVERSABLE)) !=
		 (LJ_AF_REGISTERED|LJ_AF_TRAVERSABLE))
	continue;
      for (; cell < endcell; cell++) {
	uint32_t origin;
	if (lj_arena_recovery_state_acq(a, cell) !=
	    LJ_ARENA_RECOVERY_PENDING)
	  continue;
	origin = lj_arena_lifetime_state_acq(a, cell);
	if ((origin != LJ_ARENA_LIFETIME_LIVE &&
	     origin != LJ_ARENA_LIFETIME_CONSTRUCT) ||
	    !lj_arena_lifetime_state_cas(a, cell, origin,
					   LJ_ARENA_LIFETIME_MUTATING))
	  continue;  /* Free or another recovery traversal owns the bytes. */
	if (!gc2_recovery_small_exact_ready(
	      a, (GCobj *)lj_arena_cellptr(a, cell), cell,
	      LJ_ARENA_LIFETIME_MUTATING)) {
	  (void)gc2_recovery_small_lifetime_release(
	    g, a, cell, origin, LJ_ARENA_LIFETIME_MUTATING);
	  gc2_recovery_fail_closed(g);
	  continue;
	}
	if (lj_arena_late_get(a, cell) &&
	    (origin != LJ_ARENA_LIFETIME_LIVE ||
	     lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE)) {
	  /* Construction and intrusive-root ownership still name a live object.
	  ** Leave PENDING until that owner resolves; late[] alone cannot consume
	  ** recovery for an identity which remains semantically retained. */
	  (void)gc2_recovery_small_lifetime_release(
	    g, a, cell, origin, LJ_ARENA_LIFETIME_MUTATING);
	  continue;
	}
	if (lj_arena_recovery_state_acq(a, cell) !=
	      LJ_ARENA_RECOVERY_PENDING ||
	    !lj_arena_recovery_state_cas(a, cell,
	      LJ_ARENA_RECOVERY_PENDING, LJ_ARENA_RECOVERY_CLAIMED)) {
	  (void)gc2_recovery_small_lifetime_release(
	    g, a, cell, origin, LJ_ARENA_LIFETIME_MUTATING);
	  continue;
	}
	if (cell + 1u < LJ_ARENA_CELLS) {
	  gc2_recovery_small_slot_store_rlx(g, entry);
	  gc2_recovery_small_cell_store_rlx(g, cell + 1u);
	} else {
	  gc2_recovery_small_slot_store_rlx(g, cursor);
	  gc2_recovery_small_cell_store_rlx(g, LJ_AFIRST_CELL);
	}
	gc2_recovery_test_pause_at(LJ_GC2_RECOVERY_TEST_POST_CLAIM);
	if (lj_arena_late_get(a, cell)) {
	  /* A free excluded by recovery publishes only this durable logical-free
	  ** intent. Recheck semantic ownership after CLAIMED: late may have appeared
	  ** after the preclaim veto, and a constructor/root owner still requires the
	  ** recovery identity to remain durable until it resolves. Only an ordinary
	  ** unrooted LIVE allocation may consume recovery without traversal. */
	  int retained = origin != LJ_ARENA_LIFETIME_LIVE ||
	    lj_arena_root_state_acq(a, cell) != LJ_ARENA_ROOT_NONE;
	  if (gc2_recovery_small_lifetime_release(
		g, a, cell, origin, LJ_ARENA_LIFETIME_MUTATING)) {
	    if (retained)
	      gc2_recovery_retry_small(g, a, cell);
	    else
	      gc2_recovery_complete_small(g, a, cell);
	  }
	  lj_gc2_smr_read_leave(g);
	  return 1;
	}
	/* CLAIMED now vetoes free on its own. Restore lifetime before entering the
	** ordinary counted-reader traversal; holding MUTATING here would be
	** indistinguishable from a destructive free owner and must be rejected. */
	if (!gc2_recovery_small_lifetime_release(
	      g, a, cell, origin, LJ_ARENA_LIFETIME_MUTATING)) {
	  lj_gc2_smr_read_leave(g);
	  return 1;
	}
	if (!gc2_traverse_obj(g, (GCobj *)lj_arena_cellptr(a, cell))) {
	  /* A non-destructive descriptor owner may transiently reject reader
	  ** admission, while an excluded free publishes late without touching bytes.
	  ** A late intent consumes this old recovery incarnation; a bare transient
	  ** rejection retains the count and replays. */
	  if (lj_arena_late_get(a, cell))
	    gc2_recovery_complete_small(g, a, cell);
	  else
	    gc2_recovery_retry_small(g, a, cell);
	  lj_gc2_smr_read_leave(g);
	  return 1;
	}
	gc2_recovery_test_pause_at(LJ_GC2_RECOVERY_TEST_PRE_COMPLETE);
	gc2_recovery_complete_small(g, a, cell);
	lj_gc2_smr_read_leave(g);
	return 1;
      }
    }
  }
  lj_gc2_smr_read_leave(g);
  return 0;
}

static int gc2_recovery_drain_hugetab_range(global_State *g, HugeTab *ht,
						     uint32_t start,
						     uint32_t stop,
						     int bounded)
{
  uint32_t cursor = start;
  void *p;
  LJHugeInfo hi;
  if (!ht)
    return 0;
  while (lj_arena_hugetab_recovery_next(ht, &cursor, &p, &hi)) {
    uint32_t entry = cursor - 1u;
    uint32_t state = lj_arena_huge_recovery_state(hi.flags);
    if (bounded && entry >= stop)
      break;
    if (state != LJ_ARENA_RECOVERY_PENDING ||
	!lj_arena_hugetab_recovery_state_cas(
	  ht, p, state, LJ_ARENA_RECOVERY_CLAIMED, &hi))
      continue;
    /* Only graph-bearing exact allocation bases publish recovery. A stale or
    ** corrupted cdata/interior entry is leaf work and is safely consumed. */
    if (!(hi.flags & LJ_HUGEF_CDATA))
      gc2_traverse_obj(g, (GCobj *)p);
    gc2_recovery_test_pause_at(LJ_GC2_RECOVERY_TEST_PRE_COMPLETE);
    gc2_recovery_complete_huge(g, ht, p);
    gc2_recovery_huge_slot_store_rlx(g, cursor);
    return 1;
  }
  return 0;
}

static int gc2_recovery_drain_huge_one(global_State *g)
{
  TGState *tg;
  uint32_t start, pass;
#if defined(LJ_GC2_TEST_HELPERS)
  la_add32_rlx(&gc2_recovery_test_huge_scans, 1);
#endif
  if (!lj_gc2_smr_read_try(g))
    return 0;
  start = gc2_recovery_huge_slot_rlx(g);
  for (pass = 0; pass < (start ? 2u : 1u); pass++) {
    uint32_t cursor = pass == 0 ? start : 0;
    for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
      if (lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
	  gc2_recovery_drain_hugetab_range(
	    g, &tg->huge, cursor, start, pass != 0)) {
	lj_gc2_smr_read_leave(g);
	return 1;
      }
    }
    tg = g->main_tg;
    if (tg && lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
	gc2_recovery_drain_hugetab_range(
	  g, &tg->huge, cursor, start, pass != 0)) {
      lj_gc2_smr_read_leave(g);
      return 1;
    }
  }
  lj_gc2_smr_read_leave(g);
  return 0;
}

static int gc2_recovery_drain_one(global_State *g)
{
  uint32_t schedule, start, credit, n;
  /* recovery_failed is an absorbing reclamation veto, not a queue identity.
  ** Only the exact item count can justify walking directory-sized lanes. */
  if (!gc2_recovery_work_pending(g))
    return 0;
  schedule = gc2_recovery_scan_lane_rlx(g);
  start = schedule & GC2_RECOVERY_LANE_MASK;
  credit = schedule >> GC2_RECOVERY_LANE_BITS;
  if (start >= GC2_RECOVERY_LANES) {
    start = 0;
    credit = 0;
  }
  for (n = 0; n < GC2_RECOVERY_LANES; n++) {
    uint32_t lane = (start + n) % GC2_RECOVERY_LANES;
    int drained = lane == 0 ? gc2_recovery_drain_main_one(g) :
	(lane == 1 ? gc2_recovery_drain_small_one(g) :
	 (gc2_recovery_huge_items_acq(g) != 0 ?
	  gc2_recovery_drain_huge_one(g) : 0));
    if (drained) {
      /* Empty HugeTab/arena lanes are directory-sized scans. Retain a busy
      ** lane for a bounded quantum so a small-object backlog amortizes those
      ** misses, then rotate to keep main/small/huge starvation bounded. */
      credit = lane == start ? credit + 1u : 1u;
      if (credit >= GC2_RECOVERY_LANE_QUANTUM) {
	gc2_recovery_scan_lane_store_rlx(
	  g, (lane + 1u) % GC2_RECOVERY_LANES);
      } else {
	gc2_recovery_scan_lane_store_rlx(
	  g, (credit << GC2_RECOVERY_LANE_BITS) | lane);
      }
      return 1;
    }
  }
  gc2_recovery_scan_lane_store_rlx(
    g, (start + 1u) % GC2_RECOVERY_LANES);
  return 0;
}

static uint32_t gc2_recovery_drain_owned(global_State *g, uint32_t limit)
{
  uint32_t n = 0;
  while (g && n < limit && gc2_recovery_work_pending(g)) {
    if (!gc2_recovery_drain_one(g))
      break;  /* A producer may be between counter reserve and state CAS. */
    n++;
  }
  return n;
}

static uint32_t gc2_recovery_discard_hugetab_terminal(HugeTab *ht)
{
  uint32_t cursor = 0, n = 0;
  void *p;
  LJHugeInfo hi;
  if (!ht)
    return 0;
  while (lj_arena_hugetab_recovery_next(ht, &cursor, &p, &hi)) {
    int discarded = lj_arena_hugetab_recovery_discard_terminal(
	  ht, p, &hi);
    if (discarded == LJ_ARENA_HUGE_RECOVERY_TERMINAL_CLEARED ||
	discarded == LJ_ARENA_HUGE_RECOVERY_TERMINAL_UNMAP) {
      n++;
      if (discarded == LJ_ARENA_HUGE_RECOVERY_TERMINAL_UNMAP)
	lj_arena_huge_unmap_claimed(p, hi.size);
    }
  }
  return n;
}

static uint64_t gc2_recovery_count_hugetab_terminal(HugeTab *ht)
{
  uint32_t cursor = 0;
  uint64_t n = 0;
  void *p;
  LJHugeInfo hi;
  if (!ht)
    return 0;
  while (lj_arena_hugetab_recovery_next(ht, &cursor, &p, &hi)) {
    UNUSED(p); UNUSED(hi);
    n++;
  }
  return n;
}

static int gc2_recovery_terminal_preflight(global_State *g,
					    uint64_t *totalp,
					    uint64_t *hugep)
{
  HugeTab *registry;
  TGState *tg;
  uint32_t cursor = 0;
  uint64_t n = 0, huge_n = 0;
  int saw_main = 0;
  void *p;
  LJHugeInfo hi;
  if (!g)
    return 0;
  if (gc2_recovery_main_state_acq(g) != LJ_ARENA_RECOVERY_IDLE)
    n++;
  registry = (HugeTab *)gc2_small_arena_tab_acq(g);
  while (registry && lj_arena_hugetab_next(
	   registry, &cursor, &p, &hi)) {
    GCArena *a = (GCArena *)p;
    uint32_t cell;
    UNUSED(hi);
    for (cell = LJ_AFIRST_CELL; cell < LJ_ARENA_CELLS; cell++)
      if (lj_arena_recovery_state_acq(a, cell) != LJ_ARENA_RECOVERY_IDLE)
	n++;
  }
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (tg == g->main_tg)
      saw_main = 1;
    if (lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      huge_n += gc2_recovery_count_hugetab_terminal(&tg->huge);
  }
  if (!saw_main && g->main_tg &&
      lj_tg_flags_test_acq(g->main_tg, TGF_HUGETAB))
    huge_n += gc2_recovery_count_hugetab_terminal(&g->main_tg->huge);
  n += huge_n;
  if (LJ_UNLIKELY(gc2_recovery_items_acq(g) != n ||
		  gc2_recovery_huge_items_acq(g) != huge_n)) {
    /* No locator or mapping has changed yet. Preserve the complete mismatch
    ** evidence and fail-stop before terminal discard can erase it. */
    gc2_recovery_fail_closed(g);
    return 0;
  }
  if (totalp)
    *totalp = n;
  if (hugep)
    *hugep = huge_n;
  return 1;
}

#if defined(LJ_GC2_TEST_HELPERS)
int lj_gc2_test_recovery_terminal_preflight(global_State *g)
{
  return g ? gc2_recovery_terminal_preflight(g, NULL, NULL) : 0;
}
#endif

static uint32_t gc2_recovery_discard_terminal(global_State *g)
{
  HugeTab *registry;
  TGState *tg;
  uint32_t cursor = 0;
  uint64_t n = 0, huge_n = 0;
  uint64_t preflight_n = 0, preflight_huge_n = 0;
  int saw_main = 0;
  void *p;
  LJHugeInfo hi;
  if (!g)
    return 0;
  lj_assertG(gc2_n_workers_acq(g) == 0 &&
	     gc2_worker_active_acq(g) == 0,
	     "terminal recovery discard overlaps GC worker");
  if (LJ_UNLIKELY(!gc2_recovery_terminal_preflight(
	 g, &preflight_n, &preflight_huge_n)))
    abort();
  for (;;) {
    uint32_t state = gc2_recovery_main_state_acq(g);
    uint32_t expect = state;
    if (state == LJ_ARENA_RECOVERY_IDLE)
      break;
    if (gc2_recovery_main_state_cas(
	  g, &expect, LJ_ARENA_RECOVERY_IDLE)) {
      n++;
      break;
    }
  }
  registry = (HugeTab *)gc2_small_arena_tab_acq(g);
  while (registry && lj_arena_hugetab_next(
	   registry, &cursor, &p, &hi)) {
    GCArena *a = (GCArena *)p;
    uint32_t cell;
    UNUSED(hi);
    for (cell = LJ_AFIRST_CELL; cell < LJ_ARENA_CELLS; cell++) {
      for (;;) {
	uint32_t state = lj_arena_recovery_state_acq(a, cell);
	if (state == LJ_ARENA_RECOVERY_IDLE)
	  break;
	if (lj_arena_recovery_state_cas(
	      a, cell, state, LJ_ARENA_RECOVERY_IDLE)) {
	  n++;
	  break;
	}
      }
    }
  }
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (tg == g->main_tg)
      saw_main = 1;
    if (lj_tg_flags_test_acq(tg, TGF_HUGETAB))
      huge_n += gc2_recovery_discard_hugetab_terminal(&tg->huge);
  }
  tg = g->main_tg;
  if (!saw_main && tg && lj_tg_flags_test_acq(tg, TGF_HUGETAB))
    huge_n += gc2_recovery_discard_hugetab_terminal(&tg->huge);
  n += huge_n;
  if (LJ_UNLIKELY(n != preflight_n || huge_n != preflight_huge_n)) {
    /* Joined-world discard must consume every identity proven by preflight.
    ** A divergence is internal corruption; locators may now be cleared or a
    ** deferred huge mapping unmapped, so only fail-stop is sound. */
    gc2_recovery_fail_closed(g);
    abort();
  }
  if (LJ_UNLIKELY(gc2_recovery_items_acq(g) != preflight_n ||
		  gc2_recovery_huge_items_acq(g) != preflight_huge_n)) {
    gc2_recovery_fail_closed(g);
    abort();
  }
  /* Keep the aggregate close veto visible until every lane subcount is gone. */
  gc2_recovery_huge_items_store_rlx(g, 0);
  gc2_recovery_items_store_rlx(g, 0);
  gc2_recovery_failed_store_rlx(g, 0);
  return n > ~(uint32_t)0 ? ~(uint32_t)0 : (uint32_t)n;
}

int lj_gc2_terminal_prefree(global_State *g)
{
  TGState *tg;
  HugeTab *registry;
  int saw_main = 0;
  if (!g)
    return 1;
  /* This is not a runtime recovery shortcut. Every producer/worker/finalizer
  ** must already have joined, so an identity or C|P gate observed here can be
  ** consumed without racing a semantic publisher. */
  if (gc2_n_workers_acq(g) != 0 || gc2_worker_active_acq(g) != 0 ||
      gc2_ssb_consumer_active_acq(g) != 0 ||
      gc2_finalizer_active_acq(g) != 0 || gc2_n_threads_acq(g) > 1u)
    abort();
  /* Prove every mapping certificate before discarding any recovery identity.
  ** Recovery discard is deliberately destructive: on a later reader or
  ** descriptor veto there is no sound way to reconstruct the exact locator
  ** which was just consumed. The allocator certificate pass may collapse the
  ** sole joined-world CLOSED|PENDING gate generation, but that repair is
  ** exact, idempotent and retains every object/recovery locator. */
  registry = (HugeTab *)gc2_small_arena_tab_acq(g);
  if (registry &&
      !lj_arena_hugetab_terminal_certificate_ready(registry))
    return 0;
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (tg == g->main_tg)
      saw_main = 1;
    if (lj_tg_flags_test_acq(tg, TGF_HUGETAB) &&
	!lj_arena_hugetab_terminal_certificate_ready(&tg->huge))
      return 0;
  }
  /* Partial initialization can leave the embedded main TG outside the list;
  ** it still owns allocator lists which terminal free must reconcile. */
  if (!saw_main && g->main_tg) {
    if (lj_tg_flags_test_acq(g->main_tg, TGF_HUGETAB) &&
	!lj_arena_hugetab_terminal_certificate_ready(&g->main_tg->huge))
      return 0;
  }
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL) &&
	!lj_arena_alloc_terminal_certificate_ready(&tg->alloc))
      return 0;
  }
  if (!saw_main && g->main_tg &&
      lj_tg_flags_test_acq(g->main_tg, TGF_ARENA_INTERNAL) &&
      !lj_arena_alloc_terminal_certificate_ready(&g->main_tg->alloc))
    return 0;

  (void)gc2_recovery_discard_terminal(g);
  if (!gc2_recovery_empty(g) || gc2_recovery_items_acq(g) != 0)
    abort();

  /* Joined-world terminal ownership must not trust the opportunistic
  ** remote-free hint. Consume every durable owned-arena queue after the full
  ** certificate pass, including a record whose advisory wake was cleared by
  ** an earlier failed/diagnostic drain, then recheck the gates it touched. */
  for (tg = gc2_tg_list_acq(g); tg != NULL; tg = lj_tg_next_acq(tg)) {
    if (lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL)) {
      (void)lj_arena_remote_free_drain_force(&tg->alloc);
      if (!lj_arena_alloc_terminal_certificate_ready(&tg->alloc))
	abort();
    }
  }
  if (!saw_main && g->main_tg &&
      lj_tg_flags_test_acq(g->main_tg, TGF_ARENA_INTERNAL)) {
    (void)lj_arena_remote_free_drain_force(&g->main_tg->alloc);
    if (!lj_arena_alloc_terminal_certificate_ready(&g->main_tg->alloc))
      abort();
  }
  return 1;
}

static uint32_t gc2_drain_grey(global_State *g, uint32_t limit)
{
  uint32_t n = 0;
  while (g && n < limit && !gc2_grey_empty(g)) {
    GCobj *o = gc2_grey_pop(g);
    if (o) {
      int traversed = gc2_traverse_obj(g, o);
      if (traversed == GC2_TRAVERSE_RETRY) {
	/* The pop removed the only concrete locator. Republish before yielding,
	** then stop this quantum so a descheduled lifetime owner cannot induce a
	** tight pop/requeue loop in an otherwise-unbounded drain. */
	(void)gc2_publish_worker(g, o);
	gc2_marks_this_round_add(g, 1);
	lj_gc2_worker_wake(g);
	break;
      }
      n++;
    }
  }
  if (n)
    gc2_grey_drained_add(g, n);
  return n;
}

uint32_t lj_gc2_preserve_sweep_root(global_State *g, GCobj *o)
{
  int status;
  uint32_t phase;
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_SWEEP)
    return (uint32_t)lj_gc2_markobj(g, o);
  /*
  ** The GC root list is an ownership spine, not the Lua semantic root set.
  ** At the sweep bridge we preserve these bodies from arena reuse so the root
  ** spine sweeper can own destructors and unlinking. Payload traversal belongs to
  ** lj_gc2_trace_sweep_roots(), which walks actual roots such as stacks,
  ** registry, globals, JIT/FFI side roots and thread handles.
  */
  status = gc2_markobj_preserve_status(g, o, NULL, NULL, NULL);
  return (uint32_t)(status == GC2_MARK_NEW);
}

static uint32_t gc2_trace_sweep_edge(global_State *g, GCobj *o,
				      int worker_edge)
{
  int status;
  int traversable;
  uint32_t gct;
  if (!g || !o)
    return 0;
  if (o == obj2gco(&g->strempty)) {
    /* The canonical empty string is an immortal leaf embedded in
    ** global_State, not an arena allocation. It needs neither marking nor a
    ** recovery identity, but a corrupted embedded header is still terminal. */
    lj_assertG(la_load8_acq(&o->gch.gct) == (uint8_t)~LJ_TSTR,
	       "invalid embedded empty-string type");
    return 1;
  }
  if (mainthread_acq(g) && o == obj2gco(mainthread_acq(g)))
    return (uint32_t)gc2_publish_mutator(g, o);
  if (gc2_phase_acq(g) != LJ_GC2_SWEEP)
    return (uint32_t)lj_gc2_markobj(g, o);
  status = gc2_markobj_preserve_status(g, o, NULL, &gct, &traversable);
  if (status == GC2_MARK_DEAD) {
    /* A valid post-write barrier can meet a destructive owner only at the
    ** SC lifetime handshake: either that owner observed our counted reader and
    ** aborts, or recovery must retain a durable alternative. RECOVERY coalesces
    ** with the publisher which already owns the count; generic MUTATING alone
    ** fails closed. Once the owner restores LIVE, publication is allocation-
    ** free; otherwise sticky NO_RECLAIM vetoes every irreversible boundary. */
    if (!gc2_recovery_publish(g, o))
      gc2_recovery_fail_closed(g);
    return 1;
  }
  if (gct == (uint32_t)~LJ_TTHREAD &&
      gc2_thread_scan_current(g, gco2th(o))) {
    /* The owner acknowledgement synchronously traversed this exact stack and
    ** published its cycle/dirty stamp before the global root pass reached the
    ** registry identity. Requeueing the already-covered thread here would make
    ** traversal consume the handoff stamp, manufacture another NEEDSCAN, and
    ** force the SWEEP bridge into an unbounded root-handshake loop while the
    ** owner remains parked in native code. The preserve mark above is the body
    ** lifetime edge; the current stamp is the complete payload proof. */
    return 0;
  }
  if (worker_edge && gct == (uint32_t)~LJ_TTAB &&
      !(lj_obj_gcflags(o) & LJ_GC_NEEDSCAN) &&
      gc2_table_scan_current(g, gco2tab(o))) {
    /* This is graph discovery by an admitted worker traversal, not a semantic
    ** mutator/root publication. The current stamp and clear NEEDSCAN prove the
    ** table payload was already covered. Republishing it from a cyclic graph
    ** can only REDIRTY its CLAIMED recovery incarnation forever. External
    ** barriers retain the public path below and therefore still force a scan
    ** even if they race a raw payload mutation without a dirty-epoch update. */
#if defined(LJ_GC2_TEST_HELPERS)
    la_add32_rlx(&gc2_recovery_test_worker_table_skips, 1);
#endif
    return 0;
  }
  if (worker_edge && gct != (uint32_t)~LJ_TTAB &&
      gct != (uint32_t)~LJ_TTHREAD && gc2_gct_may_traverse(gct) &&
      !gc2_rescan_pending_set(o)) {
    /* Preserve-path NEW clears prior-cycle uncounted membership before graph
    ** scheduling; other NEW winners publish explicit queue work which must
    ** drain before close. With public minor sweep disabled, a remaining bit
    ** therefore names this allocation's current major-cycle traversal. Consume
    ** only private descendants; public root/barrier publication is unfiltered. */
    return 0;
  }
  if (traversable || gct == (uint32_t)~LJ_TUDATA) {
    /*
    ** Mutators never push/pop the single-owner global grey deque. Publish the
    ** rescued container on this TG's SSB; the worker converts it to grey work
    ** before the post-retire grace/reclaim boundary. This also makes every
    ** sweep-time semantic publication visible to the close predicate.
    */
    return (uint32_t)gc2_publish_mutator(g, o);
  }
  return 0;
}

uint32_t lj_gc2_trace_sweep_root(global_State *g, GCobj *o)
{
  return gc2_trace_sweep_edge(g, o, 0);
}

static uint32_t gc2_trace_sweep_worker_edge(global_State *g, GCobj *o)
{
  return gc2_trace_sweep_edge(g, o, 1);
}

void lj_gc2_trace_sweep_roots(global_State *g)
{
  if (!g || gc2_phase_acq(g) != LJ_GC2_SWEEP)
    return;
  /*
  ** Root marking is phase-aware: during SWEEP the ordinary owner/global
  ** scanners route semantic objects through lj_gc2_trace_sweep_root(). Reuse
  ** that split under a complete acknowledgement boundary instead of carrying a
  ** second global scanner or walking live TG stacks from the sweep worker.
  */
  (void)lj_gc2_handshake(g,
    LJ_GC2_HS_SCAN_ROOTS|LJ_GC2_HS_FLUSH_SSB);
}

static uint32_t gc2_worker_sweep_progress(global_State *g, uint32_t limit)
{
  TGState *tg;
  uint32_t n = 0;
  if (gc2_jit_recorder_active(g)) {
    /* Recorder construction is denied in SWEEP. Fail closed if a forced or
    ** stale active state is nevertheless observed before physical work. */
    lj_trace_abort(g);
    return 0;
  }
  if (gc2_sweep_blocked_by_finalizer(g))
    return 0;
  /* Sweep-time publishers enqueue rescued containers on SSB. Close that graph
  ** before taking the post-retirement grace or running any destructor. */
  while (n < limit) {
    uint32_t moved = gc2_drain_published_ssb_to_grey(g, 1);
    GCobj *o = gc2_grey_pop(g);
    if (o) {
      int traversed = gc2_traverse_obj(g, o);
      if (traversed == GC2_TRAVERSE_RETRY) {
	(void)gc2_publish_worker(g, o);
	gc2_marks_this_round_add(g, 1);
	lj_gc2_worker_wake(g);
	break;
      }
      n++;
      continue;
    }
    if (!moved) {
      uint32_t recovered = gc2_recovery_drain_owned(g, 1);
      if (!recovered)
	break;
      n += recovered;
      continue;
    }
    n += moved;
  }
  if (n)
    return n;
  /* A peer can publish a below-capacity active SSB suffix after the SWEEP
  ** bridge snapshot. The current logical owner was rotated before taking the
  ** worker token, but another native/VM TG can leave semantic rescue work in
  ** its private suffix. The close predicate deliberately treats that suffix
  ** as non-empty; rotate it here before deciding that the worker has no work,
  ** otherwise a secondary explicit collector and a native-parked main TG can
  ** wait forever for a final close handshake that the non-empty predicate
  ** itself prevents. */
  if (gc2_ssb_published_empty(g) && gc2_grey_empty(g) &&
      gc2_recovery_empty(g) && !lj_gc2_ssb_empty(g)) {
    (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
    return 1;
  }
  /* A zero-progress drain is not proof of semantic emptiness: the exact
  ** recovery locator may still be behind its reserve-before-publish LP, and a
  ** sticky classification failure deliberately has no drainable identity.
  ** Gate every irreversible sweep subsystem on the complete work predicate. */
  if (!lj_gc2_ssb_empty(g) || !gc2_recovery_empty(g))
    return 0;
  if (gc2_sweep_bridge_ready_acq(g) &&
      gc2_sweep_root_scanned_acq(g) != 1)
    return 0;
  if (gc2_sweep_grace_needed_acq(g)) {
    uint32_t grace;
    /* This synchronous API returns only after every target acknowledged. The
    ** worker token prevents a second retire producer, so clearing the latch
    ** afterward proves all currently quarantined gcw/raw readers crossed a
    ** complete epoch. The next worker pass drains roots published by the ACK. */
    for (grace = 0; grace < LJ_GC2_GRACE_EPOCHS; grace++)
      (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP)
      gc2_sweep_grace_needed_rel(g, 0);
    return 1;
  }
  if (gc2_sweep_bridge_ready_acq(g)) {
    uint32_t strings = lj_str_gc2_sweep_step(g, limit);
    if (strings)
      return strings;
  }
  for (tg = gc2_tg_list_acq(g);
       tg != NULL && n < limit;
       tg = lj_tg_next_acq(tg)) {
    uint8_t flags = lj_tg_flags_acq(tg);
    int finished = 0;
    if ((flags & (TGF_DEAD|TGF_ARENA_INTERNAL)) != TGF_ARENA_INTERNAL)
      continue;
    n += lj_gc2_sweep_owner_progress(g, tg, limit - n, &finished);
    if (finished)
      break;
  }
  return n;  /* 05 section 5.6.3 worker-owned sweep bridge. */
}

static uint32_t gc2_worker_progress_add(uint32_t a, uint32_t b)
{
  return b > ~(uint32_t)0 - a ? ~(uint32_t)0 : a + b;
}

static int gc2_worker_finalizer_drain_phase(global_State *g)
{
  uint32_t phase = gc2_phase_acq(g);
  return phase == LJ_GC2_IDLE || phase == LJ_GC2_SWEEP;
}

static uint32_t gc2_worker_finalizer_drain(global_State *g, uint32_t limit)
{
  uint32_t owner;
  uint64_t before, after, delta;
  if (!g || limit == 0 || !gc2_worker_finalizer_drain_phase(g) ||
      gc2_finalizer_mpsc_acq(g) == NULL)
    return 0;
  /*
  ** The idle MPSC-to-ring splice is guarded by finalizer_active/finalizer_owner.
  ** worker_active remains the single-owner token for grey deque, weak, sweep,
  ** and phase-close state; finalizer draining does not touch those paths. In
  ** SWEEP the queue already blocks arena reclamation until callbacks run.
  */
  owner = gc2_finalizer_current_owner(g);
  if (owner == 0 || owner == ~(uint32_t)0)
    return 0;  /* Do not share the TLS-less pseudo-owner across workers. */
  if (!lj_gc2_finalizer_try_enter(g))
    return 0;
  if (!gc2_worker_finalizer_drain_phase(g) ||
      gc2_finalizer_mpsc_acq(g) == NULL) {
    lj_gc2_finalizer_leave(g);
    return 0;
  }
  before = gc2_finalizer_mpsc_drained_acq(g);
  lj_gc2_finalizer_drain_owned(g);
  after = gc2_finalizer_mpsc_drained_acq(g);
  lj_gc2_finalizer_leave(g);
  delta = after - before;
  return delta > ~(uint32_t)0 ? ~(uint32_t)0 : (uint32_t)delta;
}

static uint32_t gc2_worker_drain_inner(global_State *g, TGState *logical_tg,
				       uint32_t limit, uint32_t *progress,
				       int hold_mark_gate)
{
  uint32_t phase, n = 0, converted = 0, recovered = 0, weak = 0, sweep = 0;
  uint32_t finalizer = 0;
  uint32_t total;
  int mark_gate_closed = 0;
  int sweep_gate_closed = 0;
  if (progress)
    *progress = 0;
  if (!g || limit == 0)
    return 0;
  finalizer = gc2_worker_finalizer_drain(g, limit);
  if (finalizer) {
    gc2_worker_runs_add(g, 1);
    if (progress)
      *progress = finalizer;
    return finalizer;  /* 05 section 5.8: worker drains finalizer queue work. */
  }
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK &&
      phase != LJ_GC2_SWEEP)
    return 0;
  /* Publish a proven logical owner's active suffix before attempting a MARK
  ** close. Remote FLUSH_SSB deliberately cannot rotate a no-cur_L GC worker,
  ** and traversal can leave below-capacity FUNC/TAB rescans in that worker's
  ** own buffer. Flushing after the intent branch would let a losing helper
  ** spin forever while the close owner waits for this invisible suffix. */
  if (logical_tg && logical_tg->gl == g &&
      !lj_tg_flags_test_acq(logical_tg, TGF_DEAD))
    (void)gc2_flush_ssb(g, logical_tg, 0);
  if (phase == LJ_GC2_MARK && gc2_mark_close_intent_acq(g) != 0) {
    lua_State *closeL = logical_tg ? lj_tg_load_cur_L(logical_tg) : NULL;
    uint32_t closed = gc2_mark_close_help(g, closeL, 1, limit);
    if (closed || gc2_phase_acq(g) != LJ_GC2_MARK) {
      if (progress)
	*progress = 1;
      return 1;
    }
    if (gc2_mark_close_intent_acq(g) != 0) {
      /* A helper which lost ownership before reaching its bounded miss may
      ** leave intent for the current owner. Report a scheduling unit so this
      ** worker loops instead of parking on that owner's wake. */
      if (progress)
	*progress = 1;
      return 1;
    }
    return 0;
  }
  if (!gc2_worker_claim_count_busy(g))
    return 0;  /* 05 section 5.6.3 staged grey/weak/sweep owner token. */
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK && phase != LJ_GC2_WEAK &&
      phase != LJ_GC2_SWEEP) {
    gc2_worker_release(g);
    return 0;
  }
  if (phase == LJ_GC2_MARK) {
    if (gc2_jit_phase_gate_acq(g) != 0 &&
	gc2_jit_mark_turn_deferred(g)) {
      gc2_worker_release(g);
      return 0;
    }
    if (gc2_jit_phase_gate_acq(g) != 0)
      gc2_jit_phase_gate_close(g);
    if (lj_tg_any_jit_active(g)) {
      /* Closing entry/XPOLL is sufficient. A traced FFI call may block, so
      ** return immediately and let its ordinary native return publish zero. */
      gc2_worker_release(g);
      return 0;
    }
    mark_gate_closed = gc2_jit_mark_resume_authorized(g);
    if (gc2_phase_acq(g) != LJ_GC2_MARK) {
      gc2_worker_release(g);
      return 0;
    }
  }
  if (phase == LJ_GC2_SWEEP) {
    if (gc2_sweep_bridge_ready_acq(g)) {
      /* A previous reclaim quantum which displaced native execution grants a
      ** bounded native scheduling turn. Returning zero is deliberate: the
      ** parked worker backs off, while an interpreter driver exhausts its
      ** bounded step loop and reaches JLOOP dispatch. The deadline also bounds
      ** a continuously running trace before the next reclaim quantum. */
      if (gc2_jit_sweep_turn_deferred(g)) {
	gc2_worker_release(g);
	return 0;
      }
      gc2_jit_phase_gate_close(g);
      if (lj_tg_any_jit_active(g)) {
	/* Gate closure is itself the asynchronous exit request. Never wait on
	** jit_base: a blocking FFI call simply defers this reclaim quantum. */
	gc2_jit_sweep_displaced_rel(g, 1);
	gc2_worker_release(g);
	return 0;
      }
      sweep_gate_closed = 1;
      if (gc2_phase_acq(g) != LJ_GC2_SWEEP) {
	gc2_worker_release(g);
	return 0;
      }
    }
    sweep = gc2_worker_sweep_progress(g, limit);
  } else {
    while (n + converted + recovered < limit) {
      uint32_t moved = 0;
      GCobj *o;
      if (converted < limit)
	moved = gc2_drain_published_ssb_to_grey(g, 1);
      if (moved)
	converted += moved;
      o = gc2_grey_pop(g);
      if (o) {
	int traversed = gc2_traverse_obj(g, o);
	if (traversed == GC2_TRAVERSE_RETRY) {
	  (void)gc2_publish_worker(g, o);
	  gc2_marks_this_round_add(g, 1);
	  lj_gc2_worker_wake(g);
	  break;
	}
	n++;
	continue;
      }
      if (!moved) {
	uint32_t r = gc2_recovery_drain_owned(g, 1);
	if (!r)
	  break;
	recovered += r;
	continue;
      }
    }
    if (phase == LJ_GC2_WEAK) {
      uint32_t work = gc2_worker_progress_add(n, converted);
      work = gc2_worker_progress_add(work, recovered);
      if (work < limit)
	weak = lj_gc2_weak_drain(g, limit - work);  /* 05 section 5.8. */
    }
  }
  total = gc2_worker_progress_add(n, converted);
  total = gc2_worker_progress_add(total, recovered);
  total = gc2_worker_progress_add(total, weak);
  total = gc2_worker_progress_add(total, sweep);
  if (total)
    gc2_worker_runs_add(g, 1);
  if (n) {
    gc2_grey_drained_add(g, n);
    gc2_worker_grey_drained_add(g, n);
  }
  if (converted)
    gc2_worker_ssb_converted_add(g, converted);
  if (weak)
    gc2_worker_weak_drained_add(g, weak);
  if (!total)
    gc2_worker_idle_declares_add(g, 1);
  if (progress)
    *progress = total > limit ? limit : total;
  /* A leader-side fixpoint round already owns a closed-gate observation across
  ** both drains and its root snapshot. Its nested non-owner drain must not
  ** convert that proof into an ordinary cooperative mutator turn. Background
  ** and public worker quanta retain the bounded reopen behavior. */
  if (!hold_mark_gate && mark_gate_closed &&
      gc2_phase_acq(g) == LJ_GC2_MARK &&
      gc2_mark_close_intent_acq(g) == 0)
    gc2_jit_phase_gate_open_mark(g, 1);
  if (sweep_gate_closed && gc2_phase_acq(g) == LJ_GC2_SWEEP)
    gc2_jit_phase_gate_open_sweep(g, 0);
  gc2_worker_release(g);
  return total > limit ? limit : total;  /* 05 section 5.6.3 bounded progress. */
}

uint32_t lj_gc2_worker_drain(global_State *g, uint32_t limit)
{
  TGState *tg = lj_thr_get_tg();
  return gc2_worker_drain_inner(g, tg && tg->gl == g ? tg : NULL,
					limit, NULL, 0);
}

static uint32_t gc2_worker_drain_logical(global_State *g, TGState *tg,
					 uint32_t limit, int hold_mark_gate)
{
  return gc2_worker_drain_inner(g, tg, limit, NULL, hold_mark_gate);
}

static uint32_t gc2_worker_drain_budget(global_State *g, TGState *tg,
					uint32_t limit)
{
  uint32_t n = 0;
  while (n < limit && (!gc2_grey_empty(g) || !lj_gc2_ssb_empty(g))) {
    uint32_t step = gc2_worker_drain_logical(g, tg, limit - n, 1);
    if (step == 0)
      break;
    if (step > limit - n)
      n = limit;
    else
      n += step;
  }
  return n;
}

static uint32_t gc2_fixpoint_drain(global_State *g, TGState *logical_tg,
				    uint32_t limit, int worker_owned)
{
  if (!worker_owned)
    return gc2_worker_drain_budget(g, logical_tg, limit);
  if (logical_tg && logical_tg->gl == g &&
      !lj_tg_flags_test_acq(logical_tg, TGF_DEAD))
    (void)gc2_flush_ssb(g, logical_tg, 0);
  return gc2_mark_drain_owned_bounded(g, limit);
}

static void gc2_mark_root_snapshot_reopen(global_State *g)
{
  uint32_t expect = 1;
  (void)gc2_mark_root_scanned_cas(g, &expect, 0);
}

static void gc2_mark_root_snapshot_abort(global_State *g)
{
  uint32_t expect = 2;
  (void)gc2_mark_root_scanned_cas(g, &expect, 0);
}

static int gc2_mark_root_snapshot(global_State *g)
{
  uint32_t state, expect, cycle, actions, thread_pending;
  if (!g || gc2_phase_acq(g) != LJ_GC2_MARK)
    return 0;
  if (gc2_jit_phase_gate_acq(g) != 0)
    gc2_jit_phase_gate_close(g);
  if (lj_tg_any_jit_active(g))
    return 0;
  if (gc2_jit_recorder_active(g)) {
    lj_trace_abort(g);
    gc2_root_scan_retry(g);
    return 0;
  }
  lj_assertG(gc2_jit_phase_gate_acq(g) == 0,
	     "MARK root snapshot entered with native gate open");
  state = gc2_mark_root_scanned_acq(g);
  if (state == 2)
    return 0;
  if (state != 0 && state != 1)
    return 0;
  expect = state;
  if (!gc2_mark_root_scanned_cas(g, &expect, 2))
    return 0;
  cycle = gc2_cycle_acq(g);
  thread_pending = gc2_thread_scan_needscan_pending_acq(g);
  actions = LJ_GC2_HS_FLUSH_SSB |
    (state == 1 && thread_pending == 0 ? LJ_GC2_HS_SCAN_OWNER_ROOTS :
				       LJ_GC2_HS_SCAN_ROOTS);
  (void)lj_gc2_handshake(g, actions);
  if (gc2_phase_acq(g) != LJ_GC2_MARK ||
      gc2_cycle_acq(g) != cycle || gc2_jit_phase_gate_acq(g) != 0 ||
      lj_tg_any_jit_active(g) || gc2_jit_recorder_active(g)) {
    if (gc2_jit_recorder_active(g)) {
      lj_trace_abort(g);
      gc2_root_scan_retry(g);
    }
    gc2_mark_root_snapshot_abort(g);
    return 0;
  }
  expect = 2;
  return gc2_mark_root_scanned_cas(g, &expect, 1);
}

static uint32_t gc2_fixpoint_round(global_State *g, lua_State *L,
				    uint32_t limit, int worker_owned)
{
  TGState *logical_tg = L ? L2TG(L) : lj_thr_get_tg();
  uint32_t phase, fixpoint, work_pending;
  if (!g || limit == 0)
    return 0;
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK)
    return 0;
  if (gc2_jit_phase_gate_acq(g) != 0)
    gc2_jit_phase_gate_close(g);
  if (gc2_jit_recorder_active(g)) {
    lj_trace_abort(g);
    gc2_mark_root_snapshot_reopen(g);
    gc2_root_scan_retry(g);
    gc2_fixpoint_rounds_add(g, 1);
    return 0;
  }
  if (lj_tg_any_jit_active(g)) {
    gc2_mark_root_snapshot_reopen(g);
    gc2_fixpoint_rounds_add(g, 1);
    return 0;
  }
  (void)gc2_marks_this_round_xchg_acqrel(g, 0);
  (void)gc2_fixpoint_drain(g, logical_tg, limit, worker_owned);
  work_pending = !gc2_grey_empty(g) || !gc2_recovery_empty(g) ||
    !(worker_owned ? gc2_ssb_published_empty(g) :
		       gc2_ssb_detached_empty(g));
  if (work_pending && gc2_thread_scan_needscan_pending_acq(g) == 0) {
    /*
    ** A root snapshot cannot close the mark fixpoint while older grey work is
    ** still queued. Defer the global root handshake until the queue is drained;
    ** NEEDSCAN handoffs are the exception because only the owner root scan can
    ** make that work item visible again.
    */
    gc2_fixpoint_rounds_add(g, 1);
    return 0;
  }
  if (gc2_mark_root_scanned_acq(g) != 1 ||
      gc2_thread_scan_needscan_pending_acq(g) != 0) {
    /* A completed snapshot remains authoritative for this MARK close. Stack
    ** dirty epochs after it are protected by barriers; only a concrete owner
    ** NEEDSCAN handoff or a JIT reopen requests another all-TG snapshot. */
    if (!gc2_mark_root_snapshot(g)) {
      gc2_fixpoint_rounds_add(g, 1);
      return 0;
    }
  }
  (void)gc2_fixpoint_drain(g, logical_tg, limit, worker_owned);
  if (gc2_mark_root_scanned_acq(g) == 1 && gc2_grey_empty(g) &&
      gc2_ssb_published_empty(g) && !lj_gc2_ssb_empty(g)) {
    /* The persistent semantic snapshot suppresses repeat SCAN_ROOTS, but a
    ** resumed peer can still publish a below-capacity owner-local SSB. Rotate
    ** those buffers with a flush-only handshake so the close owner can drain
    ** their concrete barrier work. */
    (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
    (void)gc2_fixpoint_drain(g, logical_tg, limit, worker_owned);
  }
  if (gc2_phase_acq(g) != LJ_GC2_MARK ||
      gc2_jit_phase_gate_acq(g) != 0 || lj_tg_any_jit_active(g) ||
      gc2_jit_recorder_active(g)) {
    if (gc2_jit_recorder_active(g)) {
      lj_trace_abort(g);
      gc2_root_scan_retry(g);
    }
    gc2_mark_root_snapshot_reopen(g);
    gc2_fixpoint_rounds_add(g, 1);
    return 0;
  }
  if (gc2_table_rescan_pending_acq(g) != 0 &&
      gc2_grey_empty(g) && lj_gc2_ssb_empty(g) &&
      gc2_marks_this_round_acq(g) == 0) {
    /* The exact token may name a publisher just before queue visibility. Keep
    ** the fixpoint open; blind zeroing races both INSTALLING and COUNTED. */
    gc2_fixpoint_rounds_add(g, 1);
    return 0;
  }
  fixpoint = gc2_phase_acq(g) == LJ_GC2_MARK &&
	     gc2_jit_phase_gate_acq(g) == 0 &&
	     !lj_tg_any_jit_active(g) && !gc2_jit_recorder_active(g) &&
	     gc2_mark_root_scanned_acq(g) == 1 &&
	     gc2_marks_this_round_acq(g) == 0 &&
	     lj_gc2_ssb_empty(g) &&
	     gc2_thread_scan_needscan_pending_acq(g) == 0 &&
	     gc2_table_rescan_pending_acq(g) == 0;
  gc2_fixpoint_rounds_add(g, 1);
  if (fixpoint)
    gc2_fixpoint_hits_add(g, 1);
  return fixpoint;
}

uint32_t lj_gc2_fixpoint_round(global_State *g, lua_State *L, uint32_t limit)
{
  return gc2_fixpoint_round(g, L, limit, 0);
}

static uint32_t lj_gc2_fixpoint_run(global_State *g, lua_State *L,
				    uint32_t max_rounds, uint32_t limit,
				    int worker_owned)
{
  uint32_t i;
  if (!g || max_rounds == 0 || limit == 0)
    return 0;
  for (i = 0; i < max_rounds; i++)
    if (gc2_fixpoint_round(g, L, limit, worker_owned))
      return 1;
  return 0;
}

static uint32_t gc2_mark_close_help(global_State *g, lua_State *L,
				    uint32_t max_rounds, uint32_t limit)
{
  uint32_t hit = 0, phase;
  int claimed;
  if (!g || gc2_mark_close_intent_acq(g) == 0)
    return 0;
  if (max_rounds == 0 || limit == 0) {
    gc2_mark_close_intent_rel(g, 0);
    lj_gc2_worker_wake(g);
    return 0;
  }
  phase = gc2_phase_acq(g);
  if (phase != LJ_GC2_MARK) {
    /* Intent is a Boolean advisory until it is cycle-tagged. Do not clear it
    ** from an unowned old-phase sample: this helper could be descheduled
    ** through IDLE into a new MARK and erase that cycle's request. Phase-gated
    ** ordinary claims ignore the stale bit, and serialized IDLE resets it. */
    lj_gc2_worker_wake(g);
    return phase == LJ_GC2_WEAK;
  }
  if (gc2_jit_phase_gate_acq(g) != 0)
    gc2_jit_phase_gate_close(g);
  if (lj_tg_any_jit_active(g)) {
    /* A blocking traced FFI call owns its published jit_base until it returns.
    ** Gate closure is the asynchronous request; MARK never waits for it. Drop
    ** the advisory close intent as well, otherwise a background worker would
    ** report fake progress and busy-loop for the duration of the foreign call. */
    gc2_mark_close_intent_rel(g, 0);
    lj_gc2_worker_wake(g);
    return 0;
  }
  claimed = gc2_worker_claim_mark_close(g);
  if (!claimed)
    return 0;
  if (gc2_phase_acq(g) != LJ_GC2_MARK) {
    phase = gc2_phase_acq(g);
    gc2_mark_close_intent_rel(g, 0);
    gc2_worker_release(g);
    lj_gc2_worker_wake(g);
    return phase == LJ_GC2_WEAK;
  }
  lj_assertG(gc2_assist_active_acq(g) == 0,
	     "MARK close token overlaps assist owner");
  hit = lj_gc2_fixpoint_run(g, L, max_rounds, limit, 1);
  if (hit && gc2_phase_acq(g) == LJ_GC2_MARK &&
      gc2_jit_phase_gate_acq(g) == 0 && !lj_tg_any_jit_active(g)) {
    /* Hold worker_active through the MARK->WEAK CAS. Splitting successful
    ** completion from lj_gc2_mark_to_weak let a background worker repeatedly
    ** steal the zero window and starve the transition. */
    lj_gc2_mark_to_weak(g);
  }
  /* Completion success is the combined transition LP, not merely a stale
  ** fixpoint hit observed before a recorder/JIT edge reopened MARK. */
  hit = gc2_phase_acq(g) == LJ_GC2_WEAK;
  if (hit)
    gc2_mark_complete_hits_add(g, 1);
  phase = gc2_phase_acq(g);
  /* Intent is phase-local. Clear it while worker_active still excludes a new
  ** IDLE->MARK owner, so preemption cannot expose WEAK with a stale intent and
  ** no close owner. Ordinary claims also phase-gate the bit as a defensive
  ** backstop for forced/test transitions. */
  if (phase != LJ_GC2_MARK) {
    gc2_mark_close_intent_rel(g, 0);
  } else if (!hit) {
    /* A bounded close miss is not permission to monopolize MARK. Barriers and
    ** black allocation make the completed portion stable, so grant another
    ** bounded native turn and let a later owner retry the fixpoint. */
    gc2_mark_close_intent_rel(g, 0);
    gc2_jit_phase_gate_open_mark(g, 1);
  }
  gc2_worker_release(g);
  hit = phase == LJ_GC2_WEAK;
  /* A bounded miss reopened MARK and cleared intent; a successful transition
  ** consumed it. Wake a worker or mutator helper after releasing the token. */
  lj_gc2_worker_wake(g);
  return hit;  /* Successful completion already published MARK->WEAK. */
}

uint32_t lj_gc2_mark_complete(global_State *g, lua_State *L,
			      uint32_t max_rounds, uint32_t limit)
{
  uint32_t expect = 0;
  if (!g || max_rounds == 0 || limit == 0 ||
      gc2_phase_acq(g) != LJ_GC2_MARK)
    return 0;
  gc2_mark_complete_runs_add(g, 1);
  if (gc2_mark_close_intent_cas(g, &expect, 1)) {
    lj_gc2_worker_wake(g);  /* Any worker may help if this caller is descheduled. */
  }
  lj_gc2_jit_mark_request_exit(g);
  if (gc2_mark_close_help(g, L, max_rounds, limit))
    return 1;
  gc2_mark_complete_peer_waits_add(g, 1);
  gc2_peer_wait_l(L);  /* One bounded handoff opportunity; never owner-wait. */
  return gc2_phase_acq(g) == LJ_GC2_WEAK;
}

static int gc2_ismarked_small_cell(global_State *g, GCArena *a,
				    uint32_t cell)
{
  uint32_t state, flags;
  uint64_t block;
  int admission, result;
  if (!a || cell < LJ_AFIRST_CELL || cell >= LJ_ARENA_CELLS)
    return -1;
  admission = gc2_small_registered_rescue_enter(g, a);
  if (admission == LJ_ARENA_RESCUE_RETRY)
    return -1;
  la_fence_seq();
  if ((lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) &&
      !gc2_small_lifetime_readable(a, cell)) {
    result = -1;
    goto out;
  }
  /* Terminal apply release-publishes block before resetting state to WHITE.
  ** Sample in the matching state->block order: an old FREEING rejects before
  ** the reset, while a new WHITE can only pair with the committed block bit. */
  state = lj_arena_sweep_state_acq(a, cell);
  block = la_load64_acq(&a->block[cell >> 6]);
  if (!((block >> (cell & 63)) & 1u) || lj_arena_late_get(a, cell)) {
    result = -1;
    goto out;
  }
  if (admission == LJ_ARENA_RESCUE_COMMITTED) {
    flags = lj_arena_flags_acq(a);
    if (gc2_committed_generation_marks(flags)) {
      if (state == LJ_ARENA_SWEEP_FREEING)
	result = -1;
      else
	result = (int)((la_load64_acq(&a->mark[cell >> 6]) >>
			(cell & 63)) & 1u);
      if (lj_arena_late_get(a, cell) ||
	  lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_FREEING)
	result = -1;
    } else {
      result = state == LJ_ARENA_SWEEP_WHITE ? 1 : -1;
    }
    goto out;
  }
  if (state == LJ_ARENA_SWEEP_FREEING) {
    result = -1;
    goto out;
  }
  flags = lj_arena_flags_acq(a);
  if (flags & (LJ_AF_NEEDSWEEP|LJ_AF_QUARANTINE|LJ_AF_PREPSWEEP)) {
    if (state == LJ_ARENA_SWEEP_LIVE)
      result = 1;
    else if (state == LJ_ARENA_SWEEP_RETIRED)
      result = 0;
    else
      result = (int)((la_load64_acq(&a->mark[cell >> 6]) >>
		      (cell & 63)) & 1u);
  } else {
    result = (int)((la_load64_acq(&a->mark[cell >> 6]) >>
		    (cell & 63)) & 1u);
  }
out:
  if (result >= 0 && (lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) &&
      !gc2_small_lifetime_readable(a, cell))
    result = -1;
  if (gc2_mark_admission_counted(admission))
    lj_arena_rescue_leave(a);
  return result;
}

int lj_gc2_ismarkedmem(global_State *g, void *p)
{
  TGState *tg;
  GCArena *a;
  uint32_t cell;
  int result;
  if (!g || !p)
    return -1;
  if (la_load32_acq(&g->allocf_arena) == 0)
    return -1;
  /* Registry proof precedes every arena-header read. Mark queries are exposed
  ** to stale queue/test pointers and must fail closed on unmapped addresses. */
  if (!lj_gc2_smr_read_try(g))
    return -1;
  tg = gc2_tg_for_registered_mem(g, p);
  if (!tg || !lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL)) {
    result = -1;
    goto out;
  }
  a = lj_arena_of(p);
  if (lj_arena_ishuge(a)) {
    LJHugeInfo hi;
    if (!lj_tg_flags_test_acq(tg, TGF_HUGETAB) ||
	lj_arena_hugetab_lookup(&tg->huge, p, &hi) != 1 ||
	(hi.flags & LJ_HUGEF_FREEING)) {
      result = -1;
      goto out;
    }
    result = (hi.flags & LJ_HUGEF_MARK) != 0;
    goto out;
  }
  cell = lj_arena_cellof(p);
  result = gc2_ismarked_small_cell(g, a, cell);
out:
  lj_gc2_smr_read_leave(g);
  return result;
}

int lj_gc2_ismarked(global_State *g, GCobj *o)
{
  GCArena *a;
  void *base = NULL;
  uint32_t gct;
  if (!g || !o || !checkptrGC(o) ||
      ((uintptr_t)o & (uintptr_t)(sizeof(void *) - 1u)) != 0)
    return -1;
  if (la_load32_acq(&g->allocf_arena) == 0) {
    base = o;
#if LJ_HASFFI
    gct = (uint32_t)la_load8_acq(&o->gch.gct);
    if (gct == (uint32_t)~LJ_TCDATA && cdataisv(gco2cd(o)) &&
	!lj_cdata_validate(g, gco2cd(o), &base, NULL))
      return -1;
#endif
    return lj_gc2_ismarkedmem(g, base);
  }
  a = lj_arena_of(o);
  if (gc2_small_arena_known(g, a)) {
    int admission;
    uint32_t cell, start, checkstart;
    int result;
    admission = gc2_small_registered_rescue_enter(g, a);
    if (admission == LJ_ARENA_RESCUE_RETRY)
      return -1;

    la_fence_seq();
    cell = lj_arena_cellof(o);
    if (!(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE) ||
	!gc2_small_lifetime_containing_start(a, cell, &start) ||
	!gc2_small_containing_start(a, cell, &checkstart) ||
	checkstart != start ||
	!lj_arena_ready_get(a, start)) {
      if (gc2_mark_admission_counted(admission))
	lj_arena_rescue_leave(a);
      return -1;
    }
    base = lj_arena_cellptr(a, start);
    result = gc2_ismarked_small_cell(g, a, start);
    if (result >= 0 &&
	(!gc2_small_lifetime_readable(a, start) ||
	 !gc2_retained_candidate_valid(g, o, base, a, start, 0,
				      o != (GCobj *)base,
				      &gct) ||
	 !gc2_small_lifetime_readable(a, start)))
      result = -1;
    if (gc2_mark_admission_counted(admission))
      lj_arena_rescue_leave(a);
    return result;
  } else {
    GC2MarkScope scope;
    LJHugeInfo hi;
    int result;
    if (gc2_huge_observed_scoped(g, o, &base, &hi, &scope) !=
	LJ_ARENA_HUGE_READER_ACQUIRED)
      return -1;
    /* Mark queries are observational. The counted reader protects validation;
    ** unlike the retaining path above, it does not set MARK to answer. */
    if ((hi.flags & LJ_HUGEF_FREEING) ||
	((hi.flags & LJ_HUGEF_BUSY) &&
	 (hi.flags & (LJ_HUGEF_TICKET|LJ_HUGEF_MARK)) !=
	   (LJ_HUGEF_TICKET|LJ_HUGEF_MARK)) ||
	(hi.flags & (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY)) !=
	  (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_READY) ||
	!gc2_retained_candidate_valid(g, o, base, NULL, 0, hi.size,
	  o != (GCobj *)base, &gct) ||
	((gct == (uint32_t)~LJ_TCDATA) !=
	 ((hi.flags & LJ_HUGEF_CDATA) != 0))) {
      gc2_mark_scope_leave(&scope);
      return -1;
    }
    result = (hi.flags & LJ_HUGEF_MARK) != 0;
    gc2_mark_scope_leave(&scope);
    return result;
  }
}

#if LJ_GC2_PARANOIA
static int gc2_root_oracle_liveobj(GCobj *o)
{
  uint8_t flags = lj_obj_gcflags(o);
  return !iswhite(o) || (flags & (LJ_GC_FIXED|LJ_GC_SFIXED));
}

static int gc2_root_oracle_has_base(global_State *g, void *p)
{
  GCobj *o;
  GC2MarkScope scope;
  uint32_t n = 0;
  (void)lj_gc_flush_root_pending(g);
  (void)lj_gc_repair_root_spine(g);
  o = lj_gc_root_acq(g);
  if (o && !gc2_root_spine_admit(g, o, &scope))
    o = NULL;
  while (o != NULL) {
    GC2MarkScope nextscope;
    GCobj *next;
    if (gc2_root_oracle_liveobj(o) && gc2_mark_base(g, o) == p) {
      gc2_mark_scope_leave(&scope);
      return 1;
    }
    if (o->gch.gct == ~LJ_TTHREAD) {
      GCobj *uv;
      GC2MarkScope uvscope;
      uint32_t nuv = 0;

      uv = lj_state_openupval_acq(gco2th(o));
      if (uv && !gc2_root_spine_admit(g, uv, &uvscope))
	uv = NULL;
      while (uv != NULL) {
	GC2MarkScope nextuvscope;
	GCobj *nextuv;
	if (gc2_root_oracle_liveobj(uv) && gc2_mark_base(g, uv) == p) {
	  gc2_mark_scope_leave(&uvscope);
	  gc2_mark_scope_leave(&scope);
	  return 1;
	}
	if (++nuv >= LJ_GC2_ROOT_SCAN_LIMIT) {
	  gc2_mark_scope_leave(&uvscope);
	  break;
	}
	if (!gc2_root_spine_handoff(g, uv, &uvscope, &nextuv,
					    &nextuvscope))
	  break;
	if (nextuv == uv) {
	  gc2_mark_scope_leave(&nextuvscope);
	  break;
	}
	uv = nextuv;
	uvscope = nextuvscope;
      }
    }
    if (++n >= LJ_GC2_ROOT_SCAN_LIMIT) {
      gc2_mark_scope_leave(&scope);
      break;
    }
    if (!gc2_root_spine_handoff(g, o, &scope, &next, &nextscope))
      break;
    if (next == o) {
      gc2_mark_scope_leave(&nextscope);
      break;
    }
    o = next;
    scope = nextscope;
  }
  return 0;
}

static uint32_t gc2_paranoia_scan_arena(global_State *g, GCArena *a)
{
  uint32_t w, bad = 0;
  for (w = 0; w < LJ_ARENA_WORDS; w++) {
    uint64_t b = la_load64_acq(&a->block[w]);
    uint64_t m = b & la_load64_acq(&a->mark[w]);
    while (m) {
      uint32_t bit = (uint32_t)__builtin_ctzll(m);
      uint32_t cell = (w << 6) + bit;
      uint32_t dtor_kind;
      m &= m - 1u;
      if (cell < LJ_AFIRST_CELL)
	continue;
      dtor_kind = lj_arena_dtor_kind_acq(a, cell);
      if ((dtor_kind == LJ_ARENA_DTOR_LFUNC1 ||
	   dtor_kind == LJ_ARENA_DTOR_CLOSED_UV ||
	   dtor_kind == LJ_ARENA_DTOR_LFUNC0) &&
	  lj_arena_ready_get(a, cell) &&
	  lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_NONE) {
	uint32_t life = lj_arena_lifetime_state_acq(a, cell);
	if (life == LJ_ARENA_LIFETIME_LIVE ||
	    life == LJ_ARENA_LIFETIME_CONSTRUCT ||
	    life == LJ_ARENA_LIFETIME_RESCUE)
	  continue;  /* Exact arena-owned destructor identity replaces root. */
      }
      if (!gc2_root_oracle_has_base(g, lj_arena_cellptr(a, cell)))
	bad++;
    }
  }
  return bad;
}

static uint32_t lj_gc2_paranoia_root_diff(global_State *g)
{
  TGState *tg;
  GCArena *a;
  uint32_t bad = 0;
  if (!g)
    return 0;
  for (tg = gc2_tg_list_acq(g);
       tg != NULL;
       tg = lj_tg_next_acq(tg)) {
    uint8_t flags = lj_tg_flags_acq(tg);
    if ((flags & (TGF_DEAD|TGF_ARENA_INTERNAL)) != TGF_ARENA_INTERNAL)
      continue;
    for (a = tg->alloc.owned[LJ_ARENAK_TRAVERSABLE];
	 a != NULL; a = lj_arena_next_acq(a))
      bad += gc2_paranoia_scan_arena(g, a);
    for (a = tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE]; a != NULL;
	 a = lj_arena_next_acq(a))
      bad += gc2_paranoia_scan_arena(g, a);
  }
  return bad;
}

uint32_t lj_gc2_test_paranoia_root_diff(global_State *g)
{
  return lj_gc2_paranoia_root_diff(g);
}

#endif
