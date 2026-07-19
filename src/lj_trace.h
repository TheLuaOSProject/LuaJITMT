/*
** Trace management.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_TRACE_H
#define _LJ_TRACE_H

#include "lj_obj.h"

#if LJ_HASJIT
#include "lj_atomic.h"
#include "lj_jit.h"
#include "lj_dispatch.h"

/* Trace errors. */
typedef enum {
#define TREDEF(name, msg)	LJ_TRERR_##name,
#include "lj_traceerr.h"
  LJ_TRERR__MAX
} TraceError;

LJ_FUNC_NORET void lj_trace_err(jit_State *J, TraceError e);
LJ_FUNC_NORET void lj_trace_err_info(jit_State *J, TraceError e);

/* Trace management. */
LJ_FUNC GCtrace * LJ_FASTCALL lj_trace_alloc(lua_State *L, GCtrace *T);
LJ_FUNC void LJ_FASTCALL lj_trace_free(global_State *g, GCtrace *T);
LJ_FUNC int LJ_FASTCALL lj_trace_free_gc(global_State *g, GCtrace *T);
LJ_FUNC int LJ_FASTCALL lj_trace_retire_gc_claim(global_State *g,
						 GCtrace *T);
LJ_FUNC int LJ_FASTCALL lj_trace_body_destroyed_acq(const GCtrace *T);
LJ_FUNC int LJ_FASTCALL lj_trace_native_pin(GCtrace *T);
LJ_FUNC void LJ_FASTCALL lj_trace_native_unpin(global_State *g, GCtrace *T);
LJ_FUNC int lj_trace_native_mark_pinned(global_State *g, GCtrace *T,
					 TraceNo traceno);
LJ_FUNC void LJ_FASTCALL lj_trace_free_unpublished(global_State *g, GCtrace *T);
LJ_FUNC void lj_trace_reenableproto(GCproto *pt);
LJ_FUNC uint32_t lj_trace_flushproto(global_State *g, GCproto *pt);
LJ_FUNC uint32_t lj_trace_flush(jit_State *J, TraceNo traceno);
LJ_FUNC uint32_t lj_trace_flush_unlink(jit_State *J, TraceNo traceno);
#define LJ_TRACE_STARTINS_RETRY	((BCIns)~(BCIns)0)
LJ_FUNCA BCIns LJ_FASTCALL lj_trace_stale_startins(jit_State *J,
						   const BCIns *pc,
						   TraceNo traceno,
						   lua_State *L);
LJ_FUNCA BCIns LJ_FASTCALL lj_trace_invalidate_itern(jit_State *J,
						      const BCIns *pc,
						      TraceNo traceno,
						      lua_State *L);
LJ_FUNC uint32_t lj_trace_flushscope(jit_State *J, TraceNo traceno);
LJ_FUNC int lj_trace_hasany(global_State *g);
LJ_FUNC int lj_trace_flushall(lua_State *L);
LJ_FUNC int lj_trace_flushall_gc(lua_State *L);
LJ_FUNC int lj_trace_flushall_hs(lua_State *L);
LJ_FUNC int lj_trace_flushall_hs_noevent(lua_State *L);
LJ_FUNC void lj_trace_flushscope_hs(global_State *g, uint32_t work);
LJ_FUNC uint32_t lj_trace_flushscope_retire_hs(global_State *g,
					       uint64_t epoch);
LJ_FUNC int lj_jit_token_try(jit_State *J);
LJ_FUNC int lj_jit_token_try_l(lua_State *L, jit_State *J);
LJ_FUNC int lj_jit_token_held(jit_State *J);
LJ_FUNC int lj_jit_token_held_l(lua_State *L, jit_State *J);
LJ_FUNC int lj_jit_token_acquire_wait(jit_State *J);
LJ_FUNC void lj_jit_token_release(jit_State *J);
LJ_FUNC void lj_jit_token_release_l(lua_State *L, jit_State *J);
LJ_FUNC int lj_jit_lifecycle_yield_l(lua_State *L, jit_State *J);
LJ_FUNC int lj_jit_lifecycle_resume_l(lua_State *L, jit_State *J);
LJ_FUNC int lj_jit_lifecycle_held_l(lua_State *L, jit_State *J);
LJ_FUNC TGState *lj_jit_owner_tg_l(lua_State *L, jit_State *J);

typedef struct LJJitEventFrozenViewSpec {
  const void *data;
  uint32_t size;
  uint32_t format;
  uint32_t flags;
  LJJitEventFrozenTraceHeader trace;
  LJJitEventFrozenSpan ir;
  LJJitEventFrozenSpan snap;
  LJJitEventFrozenSpan snapmap;
} LJJitEventFrozenViewSpec;

typedef struct LJJitEventSessionSpec {
  uint32_t event;
  uint32_t owner_mode;
  uint32_t edge_proof;
  uint64_t attachment_generation;
  const LJJitEventFrozenViewSpec *view;
  GCobj *const *roots;
  uint32_t root_count;
  GCtrace *source;
  TraceNo source_traceno;
} LJJitEventSessionSpec;

typedef struct LJJitEventSessionHandle {
  uint64_t generation;
  uint32_t slot;
  uint32_t owner_mode;
} LJJitEventSessionHandle;

typedef struct LJJitEventSessionSnapshot {
  global_State *g;
  TGState *tg;
  const LJJitEventSessionSlot *slot;
  uint64_t sequence;
  uint64_t generation;
  uint32_t slot_index;
  uint32_t event;
  uint32_t owner_mode;
  uint32_t edge_proof;
  uint64_t attachment_generation;
} LJJitEventSessionSnapshot;

#define LJ_JIT_EVENT_SNAPSHOT_RETRY	(-1)
#define LJ_JIT_EVENT_SNAPSHOT_IDLE	0
#define LJ_JIT_EVENT_SNAPSHOT_ACTIVE	1

LJ_FUNC void lj_jit_event_sessions_init(TGState *tg);
LJ_FUNC int lj_jit_event_sessions_fini(global_State *g, TGState *tg);
LJ_FUNC int lj_jit_event_sessions_quiescent(TGState *tg);
LJ_FUNC int lj_jit_event_sessions_logical_detach_ready(TGState *tg);
LJ_FUNC int lj_jit_event_sessions_detach_ready(TGState *tg);
/* Composite owner transitions. CONTINUATION publishes before low->high and
** resumes high->low before unpublishing. DETACHED publishes an immutable
** payload before releasing low->zero and later closes by exact TG actor and
** generation without touching a peer's global JIT owner word. */
LJ_FUNC int lj_jit_event_session_begin_l(lua_State *L, jit_State *J,
					  const LJJitEventSessionSpec *spec,
					  LJJitEventSessionHandle *handle);
LJ_FUNC int lj_jit_event_session_end_l(lua_State *L, jit_State *J,
					const LJJitEventSessionHandle *handle);
LJ_FUNC int lj_jit_event_session_contract_valid(uint32_t event,
						 uint32_t owner_mode,
						 uint32_t edge_proof,
						 int has_view,
						 int has_source,
						 uint32_t root_count);
/* The caller must already own an exact live TG identity. This function adds a
** nonwaiting GC2 SMR lease and retains it in ACTIVE snapshots until release. */
LJ_FUNC int lj_jit_event_session_snapshot_acquire(
  global_State *g, TGState *tg, LJJitEventSessionSnapshot *snapshot);
LJ_FUNC int lj_jit_event_session_snapshot_release(
  LJJitEventSessionSnapshot *snapshot);
LJ_FUNC int lj_jit_event_frozen_view_valid(const LJJitEventFrozenView *view);
/* Copy into non-aliasing unpublished frozen backing and compare a live
** snapshot without racing the runtime-mutated
** atomic exit count or depending on structure padding bytes. Count is copied
** as point-in-time information but excluded from exact-source equality. */
LJ_FUNC void lj_jit_event_snapshot_copy_canonical(SnapShot *dst,
						   const SnapShot *src);
LJ_FUNC int lj_jit_event_snapshot_matches_live(const SnapShot *frozen,
						const SnapShot *live);
LJ_FUNC void lj_trace_abort(global_State *g);
LJ_FUNC void lj_trace_abort_owner(lua_State *L);
LJ_FUNC void lj_trace_abort_owner_before_park(lua_State *L);
LJ_FUNC void lj_trace_initstate(global_State *g);
LJ_FUNC void lj_trace_freestate(global_State *g);
LJ_FUNC uint32_t lj_trace_reclaim_retired(global_State *g,
					  uint64_t completed_epoch);
LJ_FUNC int lj_trace_retired_mcode_refs(global_State *g, MCode *area,
					size_t size);
#define LJ_TRACE_MCODE_REF_NONE		0
#define LJ_TRACE_MCODE_REF_ACTIVE	1
#define LJ_TRACE_MCODE_REF_PINNED_ONLY	2
LJ_FUNC void lj_trace_freeretired(global_State *g);
LJ_FUNC int lj_trace_markvecs(global_State *g, int gc2);

#if defined(LJ_TRACE_TEST_HELPERS) || defined(LJ_GC2_TEST_HELPERS)
LJ_FUNC void lj_trace_test_reset_retire_publish_calls(void);
LJ_FUNC uint32_t lj_trace_test_retire_publish_calls(void);
LJ_FUNC void lj_trace_test_force_startins_retry(uint32_t count);
LJ_FUNC void lj_trace_test_reset_exit_stats(void);
LJ_FUNC uint32_t lj_trace_test_exit_calls(void);
LJ_FUNC TraceNo lj_trace_test_last_exit_parent(void);
LJ_FUNC ExitNo lj_trace_test_last_exitno(void);
LJ_FUNC void lj_trace_test_force_event_handoff_failure(uint32_t count);
#endif

#ifdef LJ_TRACE_TEST_HELPERS
LJ_FUNC void lj_trace_test_reset_retention_stats(void);
LJ_FUNC void lj_trace_test_note_call_unroll_abort(TraceNo lnk);
LJ_FUNC uint32_t lj_trace_test_call_unroll_aborts(void);
LJ_FUNC uint32_t lj_trace_test_call_unroll_linked(void);
LJ_FUNC uint32_t lj_trace_test_flush_unlink_calls(void);
LJ_FUNC uint32_t lj_trace_test_flush_unlink_returns(void);
LJ_FUNC uint32_t lj_trace_test_abort_selflinks(void);
LJ_FUNC uint32_t lj_trace_test_slot_release_calls(void);
LJ_FUNC uint32_t lj_trace_test_slot_release_clears(void);
LJ_FUNC uint32_t lj_trace_test_findfree_calls(void);
LJ_FUNC uint32_t lj_trace_test_findfree_reuses(void);
LJ_FUNC uint32_t lj_trace_test_findfree_grows(void);
LJ_FUNC uint32_t lj_trace_test_last_unlinked(void);
LJ_FUNC uint32_t lj_trace_test_last_findfree(void);
LJ_FUNC uint32_t lj_trace_test_last_released(void);
LJ_FUNC int lj_trace_test_preserve_body_candidate(global_State *g, GCobj *o);
LJ_FUNC int lj_trace_test_proto_pc_candidate(global_State *g, GCobj *o,
					     const BCIns *pc);
LJ_FUNC int lj_trace_test_stale_startins_candidate(global_State *g, GCobj *o);
#else
#define lj_trace_test_note_call_unroll_abort(lnk)	((void)(lnk))
#endif

/* Event handling. */
LJ_FUNC void lj_trace_ins(jit_State *J, const BCIns *pc);
#if LJ_TARGET_X64
LJ_FUNCA void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc,
				       lua_State *L);
LJ_FUNCA uint32_t LJ_FASTCALL lj_trace_stitch_probe(jit_State *J, GCtrace *T);
LJ_FUNCA void LJ_FASTCALL lj_trace_stitch(jit_State *J, const BCIns *pc,
					  lua_State *L, TraceNo traceno);
#else
LJ_FUNCA void LJ_FASTCALL lj_trace_hot(jit_State *J, const BCIns *pc);
LJ_FUNCA void LJ_FASTCALL lj_trace_stitch(jit_State *J, const BCIns *pc);
#endif
#if LJ_TARGET_X64 && LJ_ABI_WIN
LJ_FUNCA int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr,
				       lua_State *L, uint32_t exitpair);
#elif LJ_TARGET_X64
LJ_FUNCA int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr, lua_State *L,
				       TraceNo parent, ExitNo exitno);
#else
LJ_FUNCA int LJ_FASTCALL lj_trace_exit(jit_State *J, void *exptr);
#endif
#if LJ_UNWIND_EXT
LJ_FUNC uintptr_t LJ_FASTCALL lj_trace_unwind(jit_State *J, uintptr_t addr, ExitNo *ep);
#endif

/* Signal asynchronous abort of trace or end of trace. */
static LJ_AINLINE TraceState lj_trace_state_load(jit_State *J)
{
  return (TraceState)la_load32_acq((uint32_t *)&J->state);
}

static LJ_AINLINE void lj_trace_state_store(jit_State *J, TraceState st)
{
  la_store32_rel((uint32_t *)&J->state, (uint32_t)st);
}

static LJ_AINLINE int lj_trace_state_aborted(TraceState st)
{
  uint32_t s = (uint32_t)st;
  return s != (uint32_t)LJ_TRACE_IDLE &&
	 (s & (uint32_t)LJ_TRACE_ACTIVE) == 0;
}

static LJ_AINLINE TraceState lj_trace_state_store_active(jit_State *J,
							 TraceState st)
{
  uint32_t old = la_load32_acq((uint32_t *)&J->state);
  for (;;) {
    uint32_t next = (uint32_t)st;
    if (old != (uint32_t)LJ_TRACE_IDLE &&
	(old & (uint32_t)LJ_TRACE_ACTIVE) == 0)
      next &= ~(uint32_t)LJ_TRACE_ACTIVE;
    if (la_cas32((uint32_t *)&J->state, &old, next, LA_ACQ_REL, LA_ACQ))
      return (TraceState)next;  /* 08 section 8.7: preserve async aborts. */
  }
}

static LJ_AINLINE void lj_trace_state_abort(jit_State *J)
{
  uint32_t old = (uint32_t)lj_trace_state_load(J);
  while ((old & (uint32_t)LJ_TRACE_ACTIVE) != 0) {
    uint32_t next = old & ~(uint32_t)LJ_TRACE_ACTIVE;
    if (la_cas32((uint32_t *)&J->state, &old, next, LA_ACQ_REL, LA_ACQ))
      break;  /* 08 section 8.7: publish async recorder abort. */
  }
}

#define lj_trace_end(J)		lj_trace_state_store_active((J), LJ_TRACE_END)

#else

#define lj_trace_flushall(L)	(UNUSED(L), 0)
#define lj_trace_flushall_hs(L)	(UNUSED(L), 0)
#define lj_trace_flushall_hs_noevent(L)	(UNUSED(L), 0)
#define lj_trace_flushscope_retire_hs(g, e)	(UNUSED(g), UNUSED(e), 0)
#define lj_trace_hasany(g)	(UNUSED(g), 0)
#define lj_jit_token_try(J)	(UNUSED(J), 0)
#define lj_jit_token_try_l(L, J)	(UNUSED(L), UNUSED(J), 0)
#define lj_jit_token_held(J)	(UNUSED(J), 0)
#define lj_jit_token_held_l(L, J)	(UNUSED(L), UNUSED(J), 0)
#define lj_jit_token_acquire_wait(J)	(UNUSED(J), 0)
#define lj_jit_token_release(J)	UNUSED(J)
#define lj_jit_token_release_l(L, J)	(UNUSED(L), UNUSED(J))
#define lj_jit_lifecycle_yield_l(L, J)	(UNUSED(L), UNUSED(J), 0)
#define lj_jit_lifecycle_resume_l(L, J)	(UNUSED(L), UNUSED(J), 0)
#define lj_jit_lifecycle_held_l(L, J)	(UNUSED(L), UNUSED(J), 0)
#define lj_jit_owner_tg_l(L, J)	(UNUSED(L), UNUSED(J), NULL)
#define lj_jit_event_sessions_init(tg)	UNUSED(tg)
#define lj_jit_event_sessions_fini(g, tg) \
  (UNUSED(g), UNUSED(tg), 1)
#define lj_jit_event_sessions_quiescent(tg)	(UNUSED(tg), 1)
#define lj_jit_event_sessions_logical_detach_ready(tg) (UNUSED(tg), 1)
#define lj_jit_event_sessions_detach_ready(tg)	(UNUSED(tg), 1)
#define lj_trace_initstate(g)	UNUSED(g)
#define lj_trace_freestate(g)	UNUSED(g)
#define lj_trace_reclaim_retired(g, e)	(UNUSED(g), UNUSED(e), 0)
#define lj_trace_retired_mcode_refs(g, area, size) \
  (UNUSED(g), UNUSED(area), UNUSED(size), 0)
#define lj_trace_freeretired(g)	UNUSED(g)
#define lj_trace_markvecs(g, gc2)	(UNUSED(g), UNUSED(gc2), 1)
#define lj_trace_abort(g)	UNUSED(g)
#define lj_trace_abort_owner(L)	UNUSED(L)
#define lj_trace_abort_owner_before_park(L)	UNUSED(L)
#define lj_trace_retire_gc_claim(g, T)	(UNUSED(g), UNUSED(T), 1)
#define lj_trace_body_destroyed_acq(T)	(UNUSED(T), 1)
#define lj_trace_end(J)		UNUSED(J)

#endif

#endif
