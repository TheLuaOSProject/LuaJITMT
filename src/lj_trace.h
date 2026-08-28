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
#if LJ_TARGET_ARM64 && LJ_HASJIT
/* AAPCS64 returns this 16-byte non-HFA aggregate in x0/x1. The ARM64 VM keeps
** x0 as the exact trace identity (and PAUTH modifier) and branches through x1
** only after both members are non-NULL. */
typedef struct LJTraceRootEntry {
  GCtrace *trace;
  ASMFunction target;
} LJTraceRootEntry;
LJ_STATIC_ASSERT(sizeof(LJTraceRootEntry) == 16);
LJ_STATIC_ASSERT(offsetof(LJTraceRootEntry, trace) == 0);
LJ_STATIC_ASSERT(offsetof(LJTraceRootEntry, target) == 8);

LJ_FUNCA LJTraceRootEntry LJ_FASTCALL
lj_trace_enter_root(jit_State *J, const BCIns *pc, TraceNo traceno,
		    lua_State *L, TValue *base, BCIns sourceins);

/* Read-only first-level ARM64 side-recording checkpoint. The caller must keep
** trace metadata resident with either a GC2 SMR reader or the recorder token.
** `continuation` is the immutable selected snapshot PC; `pc` is the current
** incoming bytecode. METADATA validates only the published generation. IDLE
** and OWNER/START require both PCs to match; OWNER/RECORD permits later PCs in
** the same prototype while pinning recorder scratch to `continuation`. None of
** these modes claims the token, updates an exit count or starts recording. */
#define LJ_TRACE_ARM64_SIDE_CONTEXT_METADATA	0u
#define LJ_TRACE_ARM64_SIDE_CONTEXT_IDLE		1u
#define LJ_TRACE_ARM64_SIDE_CONTEXT_OWNER	2u
LJ_FUNC int lj_trace_arm64_first_side_loop_valid(
  jit_State *J, lua_State *L, TraceNo parent, ExitNo exitno,
  const BCIns *continuation, const BCIns *pc, uint32_t context);

/* First-side parent lifetime/authentication API. All operations are bounded
** and token-private. The ARM64 assembler is the only production consumer;
** recorder ingress and publication remain independently closed. Capture and
** revalidation require the exact active ASM owner; SMR contention is reported
** separately from a stale identity. Clear is lifecycle-only: initialization
** or exact token-owner teardown before selector/scratch destruction and
** terminal token release. */
typedef enum LJTraceArm64SideParentResult {
  LJ_TRACE_ARM64_SIDE_PARENT_SMR_RETRY = -1,
  LJ_TRACE_ARM64_SIDE_PARENT_RETRY = 0,
  LJ_TRACE_ARM64_SIDE_PARENT_OK = 1
} LJTraceArm64SideParentResult;
LJ_FUNC void lj_trace_arm64_side_parent_clear(jit_State *J);
LJ_FUNC LJTraceArm64SideParentResult
lj_trace_arm64_side_parent_capture(jit_State *J);
LJ_FUNC LJTraceArm64SideParentResult
lj_trace_arm64_side_parent_revalidate(jit_State *J);
#if defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_ARM64_SIDE_ASM_TEST)
/* Test-only real-body round trip through the production compact initializer
** and rollback constructor. All result words are exact booleans. */
LJ_FUNC int lj_trace_test_arm64_side_compact_roundtrip(jit_State *J,
  GCtrace *T, uint32_t *geometry_reject, uint32_t *init_ok,
  uint32_t *reset_ok, uint32_t *pauth_ok);
/* Test-only dry seal: proves and enters exact PUBLISH, exercises an asynchronous
** abort against it, then restores ASM before the mandatory unpublished abort. */
LJ_FUNC int lj_trace_test_arm64_side_publish_seal(jit_State *J, GCtrace *T);
LJ_FUNC uint32_t lj_trace_test_arm64_side_publish_seal_failure(void);
LJ_FUNC uint32_t lj_trace_test_arm64_side_publish_raw_negative(void);
#endif
#endif

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
/* Bounded, callback-free pre-MT flush. Returns nonzero instead of waiting for
** another JIT-token owner or crossing an mt_entering transition. */
LJ_FUNC int lj_trace_flushall_try_noevent(lua_State *L);
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

#if LJ_TARGET_ARM64 && LJ_HASJIT
#define LJ_TRACE_ROOT_ENTRY_PAUSE_PREPUBLISH 1u
#define LJ_TRACE_ROOT_ENTRY_PAUSE_POSTPUBLISH 2u
#define LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA 3u
#define LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION 4u
#ifdef LJ_TRACE_TEST_HELPERS
LJ_FUNC void lj_trace_test_root_entry_reset(void);
LJ_FUNC void lj_trace_test_root_entry_pause(uint32_t stage);
LJ_FUNC uint32_t lj_trace_test_root_entry_paused(void);
LJ_FUNC void lj_trace_test_root_entry_release(void);
LJ_FUNC uint32_t lj_trace_test_root_entry_publishes(void);
LJ_FUNC uint32_t lj_trace_test_root_entry_cleanups(void);
LJ_FUNC void lj_trace_test_root_entry_retry_restore(BCIns *pc, BCIns ins);
LJ_FUNC uint32_t lj_trace_test_root_entry_startins_calls(void);
LJ_FUNC int lj_trace_test_arm64_first_side_loop_valid(
  jit_State *J, lua_State *L, TraceNo parent, ExitNo exitno,
  const BCIns *continuation, const BCIns *pc, uint32_t context);
#endif
#endif

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
  /* Appended internal schema: all pre-existing member offsets stay stable. */
  uint32_t attachment_state;
  uint32_t callback_root_count;
  GCfunc *callback_handler;
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
  /* The reader's SMR scope keeps this address memory-stable through release.
  ** It is a callable ACTIVE-session root only while the publication remains
  ** exact; a stale release after close grants no callback authority. */
  uint32_t attachment_state;
  uint32_t callback_root_count;
  GCfunc *callback_handler;
} LJJitEventSessionSnapshot;

/* Linear authority for one per-TG protected JIT event callback. The immutable
** ACTIVE session publication supplied to claim remains the handler/root
** authority; its temporary snapshot reader may be released after claim so Lua
** callback code never carries a long GC2 SMR read scope. This handle only
** names bounded owner-state transitions. */
typedef struct LJJitEventCallbackHandle {
  TGState *tg;
  lua_State *owner_L;
  uint64_t generation;
  uint64_t stream_generation;
  uint64_t session_generation;
  uint32_t owner_actor;
  uint32_t event;
  uint32_t session_slot;
} LJJitEventCallbackHandle;

typedef struct LJJitEventCallbackSnapshot {
  TGState *tg;
  lua_State *owner_L;
  uint64_t sequence;
  uint64_t next_generation;
  uint64_t generation;
  uint64_t stream_generation;
  uint64_t session_generation;
  uint32_t state;
  uint32_t owner_actor;
  uint32_t event;
  uint32_t session_slot;
} LJJitEventCallbackSnapshot;

/* Linear handle for one standalone TRACE "flush" stream. Structural
** admission uses a nonzero published-generation nonce; production callback
** admission retains the exact clocked attachment classification, generation
** and handler root as part of close identity. */
typedef struct LJJitTraceStreamHandle {
  uint64_t generation;
  uint64_t attachment_generation;
  LJTGRegistryKey owner_key;
  uint32_t owner_tid;
  uint32_t owner_actor;
  LJJitEventSessionHandle terminal_session;
  /* Appended production callback schema.  The handler pointer is comparison
  ** identity only and remains rooted by terminal_session until exact close. */
  uint32_t attachment_state;
  uint32_t callback_root_count;
  GCfunc *callback_handler;
} LJJitTraceStreamHandle;

typedef struct LJJitTraceStreamSnapshot {
  uint64_t sequence;
  uint64_t next_generation;
  uint64_t generation;
  uint64_t event_ordinal;
  LJTGRegistryKey owner_key;
  uint32_t owner_tid;
  uint32_t owner_actor;
  uint32_t phase;
  uint32_t traceno;
  uint32_t callback_event;
  uint32_t callback_slot;
  uint64_t callback_session_generation;
  uint32_t terminal_event;
  uint32_t terminal_slot;
  uint64_t terminal_session_generation;
  uint32_t terminal_reason;
  uint32_t flags;
} LJJitTraceStreamSnapshot;

#define LJ_JIT_EVENT_SNAPSHOT_RETRY	(-1)
#define LJ_JIT_EVENT_SNAPSHOT_IDLE	0
#define LJ_JIT_EVENT_SNAPSHOT_ACTIVE	1

#define LJ_JIT_EVENT_CALLBACK_SNAPSHOT_RETRY	(-1)
#define LJ_JIT_EVENT_CALLBACK_SNAPSHOT_IDLE	0
#define LJ_JIT_EVENT_CALLBACK_SNAPSHOT_ACTIVE	1

#define LJ_JIT_STREAM_SNAPSHOT_RETRY	(-1)
#define LJ_JIT_STREAM_SNAPSHOT_IDLE	0
#define LJ_JIT_STREAM_SNAPSHOT_ACTIVE	1

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
						 uint32_t root_count,
						 uint32_t attachment_state,
						 uint64_t attachment_generation,
						 uint32_t callback_root_count);
/* The caller must already own an exact live TG identity. This function adds a
** nonwaiting GC2 SMR lease and retains it in ACTIVE snapshots until release. */
LJ_FUNC int lj_jit_event_session_snapshot_acquire(
  global_State *g, TGState *tg, LJJitEventSessionSnapshot *snapshot);
LJ_FUNC int lj_jit_event_session_snapshot_release(
  LJJitEventSessionSnapshot *snapshot);
/* Low-level owner claim requires a reader-held exact ACTIVE session with one
** rooted handler. Production Lua execution additionally requires the exact
** canonical stream named by stream_generation; the protected call boundary
** enforces this. Raw claims without a stream are structural-test-only and must
** not allocate, enter GC or execute Lua. Once claim succeeds the owner prevents
** session close, so release the reader before entering Lua.
** CALLING->UNWINDING occurs only after the protected call returns; release is
** valid only after every owner-local VM/stack field has been restored. */
LJ_FUNC int lj_jit_event_callback_claim_l(
  lua_State *L, uint64_t stream_generation,
  const LJJitEventSessionSnapshot *session,
  LJJitEventCallbackHandle *handle);
LJ_FUNC int lj_jit_event_callback_unwind_l(
  lua_State *L, const LJJitEventCallbackHandle *handle);
LJ_FUNC int lj_jit_event_callback_release_l(
  lua_State *L, const LJJitEventCallbackHandle *handle);
LJ_FUNC int lj_jit_event_callback_snapshot(
  TGState *tg, LJJitEventCallbackSnapshot *snapshot);
LJ_FUNC int lj_jit_event_callback_idle(TGState *tg);
LJ_FUNC int lj_jit_event_frozen_view_valid(const LJJitEventFrozenView *view);
/* Structural-only FLUSH transaction. Admission publishes the payload-free
** detached session and exact universe descriptor before releasing low-token
** ownership to zero. No production VM-event callback is invoked by this API.
** Close makes the grammar IDLE first, then closes the detached session; an
** impossible latter failure is fail-stop. */
LJ_FUNC int lj_jit_trace_flush_admit_l(lua_State *L, jit_State *J,
					uint64_t attachment_generation,
					LJJitTraceStreamHandle *handle);
/* Production handler-rooted FLUSH admission.  Success publishes the stream
** through DETACHED_CALLBACK, claims the per-TG callback owner, drops the
** temporary session reader, and releases the exact low JIT token.  Failure
** retains the caller's token/J->L and leaves every output/publication idle. */
LJ_FUNC int lj_jit_trace_flush_callback_admit_l(
  lua_State *L, jit_State *J, uint32_t attachment_state,
  uint64_t attachment_generation, GCfunc *callback_handler,
  LJJitTraceStreamHandle *stream_handle,
  LJJitEventCallbackHandle *callback_handle);
LJ_FUNC int lj_jit_trace_flush_close_l(lua_State *L, jit_State *J,
					const LJJitTraceStreamHandle *handle);
LJ_FUNC int lj_jit_trace_stream_snapshot(global_State *g,
					  LJJitTraceStreamSnapshot *snapshot);
LJ_FUNC int lj_jit_trace_stream_idle(global_State *g);
LJ_FUNC int lj_jit_trace_stream_names_tg(global_State *g, TGState *tg);
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
#ifdef LJ_TRACE_TEST_HELPERS
LJ_FUNC int lj_trace_test_body_mcode_refs(global_State *g, GCtrace *T,
					  MCode *area, size_t size);
LJ_FUNC void lj_trace_test_reset_exittab_stats(void);
LJ_FUNC void lj_trace_test_note_exittab_alloc(MSize nslots);
LJ_FUNC void lj_trace_test_note_exittab_free(MSize nslots);
LJ_FUNC uint32_t lj_trace_test_exittab_allocs(void);
LJ_FUNC uint32_t lj_trace_test_exittab_frees(void);
LJ_FUNC MSize lj_trace_test_exittab_last_alloc_slots(void);
LJ_FUNC MSize lj_trace_test_exittab_last_free_slots(void);
LJ_FUNC uint32_t lj_trace_test_mcode_retries(void);
LJ_FUNC uint32_t lj_trace_test_abort_count(void);
LJ_FUNC TraceError lj_trace_test_last_abort_error(void);
#else
#define lj_trace_test_note_exittab_alloc(nslots) ((void)(nslots))
#define lj_trace_test_note_exittab_free(nslots) ((void)(nslots))
#endif
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
LJ_FUNC TraceNo lj_trace_test_first_exit_parent(void);
LJ_FUNC ExitNo lj_trace_test_first_exitno(void);
LJ_FUNC TraceNo lj_trace_test_last_exit_parent(void);
LJ_FUNC ExitNo lj_trace_test_last_exitno(void);
LJ_FUNC void lj_trace_test_force_event_handoff_failure(uint32_t count);
#endif

#define LJ_TRACE_TEST_ADMISSION_ENTRY		1u
#define LJ_TRACE_TEST_ADMISSION_AFTER_TOKEN	2u
#define LJ_TRACE_TEST_ADMISSION_TRACE_STATE	3u
#define LJ_TRACE_TEST_ADMISSION_SIDE_ENTRY	4u
#define LJ_TRACE_TEST_ADMISSION_SIDE_AFTER_TOKEN	5u
#define LJ_TRACE_TEST_REQUEST_COUNTED		1u
#define LJ_TRACE_TEST_REQUEST_PROFILE		2u
#define LJ_TRACE_TEST_REQUEST_OBSERVE		3u
#ifdef LJ_TRACE_TEST_HELPERS
LJ_FUNC void lj_trace_test_admission_reset(void);
LJ_FUNC void lj_trace_test_admission_arm(uint32_t stage, uint32_t request,
					 uint32_t actions);
LJ_FUNC void lj_trace_test_admission_clobber_cleanup_errno(uint32_t errnum);
LJ_FUNC uint32_t lj_trace_test_admission_hits(void);
LJ_FUNC uint32_t lj_trace_test_admission_clean_releases(void);
LJ_FUNC uint32_t lj_trace_test_admission_protected_polls(void);
LJ_FUNC uint32_t lj_trace_test_admission_side_gate_blocks(void);
LJ_FUNC uint32_t lj_trace_test_admission_side_clean_releases(void);
LJ_FUNC uint32_t lj_trace_test_admission_observer_waiting(void);
LJ_FUNC uint32_t lj_trace_test_admission_armed(void);
LJ_FUNC uint32_t lj_trace_test_admission_hotcount_index(void);
LJ_FUNC uint32_t lj_trace_test_admission_hotcount_before(void);
LJ_FUNC TraceNo lj_trace_test_admission_side_parent(void);
LJ_FUNC ExitNo lj_trace_test_admission_side_exitno(void);
LJ_FUNC uint32_t lj_trace_test_admission_side_snapshot_before(void);
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
#if LJ_TARGET_X64 || LJ_TARGET_ARM64
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
#elif LJ_TARGET_X64 || LJ_TARGET_ARM64
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
    /* PUBLISH is the irreversible side-trace seam. If the exact ASM->PUBLISH
    ** CAS won, the token owner must finish the bounded suffix; if an abort CAS
    ** won first, the publish CAS observes the inactive ASM word and fails. */
    if (old == (uint32_t)LJ_TRACE_PUBLISH)
      return;
    if (la_cas32((uint32_t *)&J->state, &old, next, LA_ACQ_REL, LA_ACQ))
      break;  /* 08 section 8.7: publish async recorder abort. */
  }
}

static LJ_AINLINE int lj_trace_state_publish_try(jit_State *J)
{
  uint32_t expect = (uint32_t)LJ_TRACE_ASM;
  return la_cas32((uint32_t *)&J->state, &expect,
	(uint32_t)LJ_TRACE_PUBLISH, LA_ACQ_REL, LA_ACQ);
}

#define lj_trace_end(J)		lj_trace_state_store_active((J), LJ_TRACE_END)

#else

#define lj_trace_flushall(L)	(UNUSED(L), 0)
#define lj_trace_flushall_hs(L)	(UNUSED(L), 0)
#define lj_trace_flushall_hs_noevent(L)	(UNUSED(L), 0)
#define lj_trace_flushall_try_noevent(L)	(UNUSED(L), 0)
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
