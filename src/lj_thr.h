/*
** OS-thread substrate for LuaJIT-MT.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_THR_H
#define _LJ_THR_H

#include <stdint.h>

#include "lj_atomic.h"
#include "lj_obj.h"

#if LJ_TARGET_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef void *(*LJThrFunc)(void *);
struct LJGC2Lease;

/* Per-OS-thread GC2 capabilities. On Windows this record lives beside the
** tagged TG binding in the already-admitted process-lifetime TLS cell. POSIX
** keeps the same record in native compiler TLS. None of these fields may be
** shared by two OS threads: they are ownership proofs, not cached hints. */
typedef struct LJThrGC2TLS {
  global_State *reclaim_g;
  global_State *idle_transition_gate_g;
  global_State *smr_reader_g;
  uint32_t idle_reclaim_gate_owned;
  uint32_t smr_reader_depth;
} LJThrGC2TLS;

typedef struct LJThr {
#if LJ_TARGET_WINDOWS
  HANDLE handle;
  DWORD sysid;
  LJThrFunc func;
  void *arg;
  void *ret;
#else
  pthread_t handle;
#endif
  uint32_t tid;
} LJThr;

typedef struct LJStateClaim {
  lua_State *L;
  TGState *tg_hint;
  TGState *owner_tg;
  LJStateOwner owner_word;
  uint32_t tid;
  uint8_t release;
} LJStateClaim;

/* Physical TG state displaced by a protected preparation step on a
** temporarily resumed coroutine. The exact resume claim stays live until the
** caller has transferred any caught error away from the target stack. */
typedef struct LJStateResumeBoundary {
  TGState *owner_tg;
  lua_State *cur_L;
  uint32_t root_anchor_top;
} LJStateResumeBoundary;

/* Stable TG TLS operations move already-admitted registry borrow handles.
** They are dormant until lifecycle callers publish exact keys before roots.
*/
typedef enum LJThrTGResult {
  LJ_THR_TG_OK = 1,
  LJ_THR_TG_EXPECT_MISMATCH = 0,
  LJ_THR_TG_INVALID = -1,
  LJ_THR_TG_TLS_FAILURE = -2,
  LJ_THR_TG_CORRUPT = -3
} LJThrTGResult;

/* The signal cache is deliberately narrower than generic POSIX support. Its
** pthread identity and generated-code contracts are checked for x86-64 Linux,
** x86-64 macOS and native macOS ARM64. The Apple ARM64 admission is desktop
** only: iOS remains outside this implementation-specific signal ABI proof. */
#define LJ_THR_TG_SIGNAL_CACHE \
  ((LJ_TARGET_X64 && (LJ_TARGET_LINUX || LJ_TARGET_OSX)) || \
   (LJ_TARGET_ARM64 && LJ_TARGET_OSX && !LJ_TARGET_IOS))

/* Exact TLS ownership result matrix:
**
** - OK install/swap consumes new_hold; OK swap/clear activates old_hold.
** - EXPECT_MISMATCH, INVALID and CORRUPT leave the tagged binding and every
**   input/output handle unchanged.
** - TLS_FAILURE leaves all handles and bindings unchanged. On Windows it is
**   an install-only result when the thread TLS cell cannot be admitted. On
**   supported POSIX targets install may return it when the process-stable
**   signal cell cannot be registered; swap may also return it after fork(2),
**   when the inherited cell belongs to the parent process incarnation. Once
**   admitted in the current process, exact publication/swap/clear are
**   infallible atomic stores.
**
** Exact TLS is represented by tagged_body = body|1. Its fungible token count
** prevents body/key reuse, so swap/clear can reconstruct the exact linear
** handle from the protected body's immutable registry_key after the hot LP.
**
** Install/swap additionally require caller-held lifecycle publication
** authority which prevents the new ATTACHING/LIVE key from reaching
** DETACHING until after the hot-body LP. A token snapshot cannot manufacture
** that owner-side exclusion. The caller also holds the universe lifetime
** while reverse validation dereferences TGState.gl and the registry spine.
*/

#define LJ_THREAD_STARTING	0u
#define LJ_THREAD_RUNNING	1u
#define LJ_THREAD_DONE		2u
#define LJ_THREAD_ABORTING	3u
#define LJ_THREAD_OWNER_MAX	0xfffefffeu
#define LJ_THREAD_STRUCT	0xfffeffffu
#define LJ_THREAD_GCPREP	0xfffffffeu
#define LJ_THREAD_GCSCAN	0xffffffffu

/* TG actor zero is a live, explicitly transferable handoff state. Terminal
** detach instead publishes a value outside the process-issued actor range, so
** a binder which sampled pre-DEAD state can never CAS a retired TG from zero. */
#define LJ_THR_ACTOR_RETIRED	0xffffffffu

/* Only process-issued ids may name a live OS-thread/TG owner. The upper
** 64-KiB range is reserved for tagged table-control words and protocol claims;
** none may enter owner lookup, cycle leadership, dirty-epoch routing or
** safepoint leadership. */
static LJ_AINLINE int lj_thr_id_is_owner(uint32_t tid)
{
  return tid != 0 && tid <= LJ_THREAD_OWNER_MAX;
}

/* Process-wide owner ids are never reused. The counter stores the largest id
** already issued and saturates at the final non-sentinel value. Returning zero
** is an explicit admission failure: wrapping would alias an older state/TG
** owner, while issuing the reserved upper range would collide with table or GC
** protocol claims. Relaxed ordering is sufficient because the CAS publishes
** uniqueness, not any object initialized by the eventual id owner. */
static LJ_AINLINE uint32_t lj_thr_id_alloc(uint32_t *counter)
{
  uint32_t current = la_load32_rlx(counter);
  for (;;) {
    uint32_t next;
    if (current >= LJ_THREAD_OWNER_MAX)
      return 0;
    next = current + 1u;
    if (la_cas32(counter, &current, next, LA_RLX, LA_RLX))
      return next;
  }
}

#define LJ_MUTEX_UNLOCKED	0u
#define LJ_MUTEX_LOCKED		1u

typedef struct LJMutex {
  uint32_t state;
} LJMutex;

struct LJThreadLive {
  struct LJThreadLive *next;
  struct LJThreadLive *retired_next;
  GCRef ud;
};

LJ_FUNC GCudata *lj_thread_live_udata_acq(global_State *g,
					  LJThreadLive *node,
					  struct LJGC2Lease *lease);
LJ_FUNC GCudata *lj_thread_state_udata_acq(global_State *g,
					   const lua_State *L,
					   struct LJGC2Lease *lease);
#if defined(LJ_GC2_TEST_HELPERS)
LJ_FUNC void lj_threading_test_live_node_publish(lua_State *L, GCudata *ud,
						 LJThreadLive *node);
LJ_FUNC void lj_threading_test_live_node_retire(global_State *g,
						LJThreadLive *node);
LJ_FUNC void lj_threading_test_start_roots_publish(lua_State *L,
						   GCudata *ud,
						   TValue *roots,
						   uint32_t n);
#endif

static LJ_AINLINE GCobj *
lj_thread_live_udata_ref_acq(const LJThreadLive *node)
{
  return gcref_acq(node->ud);
}

static LJ_AINLINE void lj_thread_live_udata_ref_rel(LJThreadLive *node,
						    GCobj *o)
{
  setgcrefrel(node->ud, o);
}

static LJ_AINLINE LJThreadLive *
lj_thread_live_next_acq(const LJThreadLive *node)
{
  return (LJThreadLive *)la_loadptr_acq((void *const *)&node->next);
}

static LJ_AINLINE void lj_thread_live_next_rel(LJThreadLive *node,
						       LJThreadLive *next)
{
  la_storeptr_rel((void **)&node->next, next);
}

static LJ_AINLINE int lj_thread_live_next_cas(LJThreadLive *node,
					       LJThreadLive **oldp,
					       LJThreadLive *next)
{
  return la_casptr((void **)&node->next, (void **)oldp, next,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE LJThreadLive *
lj_thread_live_retired_next_acq(const LJThreadLive *node)
{
  return (LJThreadLive *)la_loadptr_acq((void *const *)&node->retired_next);
}

static LJ_AINLINE void lj_thread_live_retired_next_rel(LJThreadLive *node,
						       LJThreadLive *next)
{
  la_storeptr_rel((void **)&node->retired_next, next);
}

static LJ_AINLINE LJThreadLive *
lj_thread_live_head_acq(const global_State *g)
{
  return (LJThreadLive *)la_loadptr_acq((void *const *)&g->threading_live);
}

static LJ_AINLINE int lj_thread_live_head_cas(global_State *g,
					      LJThreadLive **oldp,
					      LJThreadLive *node)
{
  return la_casptr((void **)&g->threading_live, (void **)oldp, node,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE LJThreadLive *
lj_thread_live_head_xchg_acqrel(global_State *g, LJThreadLive *node)
{
  return (LJThreadLive *)la_xchgptr_acqrel((void **)&g->threading_live, node);
}

static LJ_AINLINE LJThreadLive *
lj_thread_live_retired_head_acq(const global_State *g)
{
  return (LJThreadLive *)
    la_loadptr_acq((void *const *)&g->threading_live_retired);
}

static LJ_AINLINE int lj_thread_live_retired_head_cas(global_State *g,
						      LJThreadLive **oldp,
						      LJThreadLive *node)
{
  return la_casptr((void **)&g->threading_live_retired, (void **)oldp, node,
		   LA_ACQ_REL, LA_ACQ);
}

static LJ_AINLINE LJThreadLive *
lj_thread_live_retired_head_xchg_acqrel(global_State *g, LJThreadLive *node)
{
  return (LJThreadLive *)
    la_xchgptr_acqrel((void **)&g->threading_live_retired, node);
}

typedef struct LJThread {
  LJThr thr;
  lua_State *L;
  GCudata *ud;
  TGState *tg;
  LJThreadLive *live_node;
  TValue *start_roots;
  uint32_t state;
  uint32_t joined;
  uint32_t futex;
  uint32_t status;
  uint32_t nargs;
  uint32_t nresults;
  uint32_t start_root_count;
  uint32_t start_ready;
  uint32_t main_thread;
} LJThread;

static LJ_AINLINE GCudata *lj_thread_udata_acq(const LJThread *th)
{
  return (GCudata *)la_loadptr_acq((void *const *)&th->ud);
}

static LJ_AINLINE void lj_thread_udata_rel(LJThread *th, GCudata *ud)
{
  la_storeptr_rel((void **)&th->ud, ud);
}

static LJ_AINLINE LJThreadLive *
lj_thread_live_node_acq(const LJThread *th)
{
  return (LJThreadLive *)la_loadptr_acq((void *const *)&th->live_node);
}

static LJ_AINLINE void lj_thread_live_node_rel(LJThread *th,
						       LJThreadLive *node)
{
  la_storeptr_rel((void **)&th->live_node, node);
}

static LJ_AINLINE LJThreadLive *
lj_thread_live_node_xchg_acqrel(LJThread *th, LJThreadLive *node)
{
  return (LJThreadLive *)la_xchgptr_acqrel((void **)&th->live_node, node);
}

static LJ_AINLINE lua_State *lj_thread_state_load_acq(const LJThread *th)
{
  return (lua_State *)la_loadptr_acq((void *const *)&th->L);
}

static LJ_AINLINE void lj_thread_state_store_rel(LJThread *th, lua_State *L)
{
  la_storeptr_rel((void **)&th->L, (void *)L);
}

static LJ_AINLINE TValue *lj_thread_start_roots_acq(const LJThread *th)
{
  return (TValue *)la_loadptr_acq((void *const *)&th->start_roots);
}

static LJ_AINLINE void lj_thread_start_roots_rel(LJThread *th, TValue *roots)
{
  la_storeptr_rel((void **)&th->start_roots, roots);
}

static LJ_AINLINE uint32_t lj_thread_start_root_count_acq(const LJThread *th)
{
  return la_load32_acq(&th->start_root_count);
}

static LJ_AINLINE void lj_thread_start_root_count_rel(LJThread *th,
						      uint32_t count)
{
  la_store32_rel(&th->start_root_count, count);
}

LJ_FUNC int lj_thr_create(LJThr *thr, LJThrFunc func, void *arg);
LJ_FUNC int lj_thr_join(LJThr *thr, void **ret);
LJ_FUNC uint32_t lj_thr_newid(void);
LJ_FUNC uint32_t lj_thr_id(const LJThr *thr);
/* Physical OS-thread actors are process-issued and never reused. The current
** accessor never admits/allocates; ensure is the cold retryable admission. */
LJ_FUNC uint32_t lj_thr_actor_current(void);
LJ_FUNC uint32_t lj_thr_actor_ensure(void);
LJ_FUNC uint32_t lj_thr_current_id(global_State *g);
LJ_FUNC uint64_t lj_thr_now_ns(void);
LJ_FUNC int lj_thr_tg_tls_init(void);
#if LJ_TARGET_WINDOWS
/* Lookup only: never initializes the process key or admits the current
** thread. GC2 uses NULL as an allocation-free, fully-counted SMR fallback and
** as a fail-closed result for exclusive per-thread capabilities. */
LJ_FUNC LJThrGC2TLS *lj_thr_gc2_tls_current(void);
#endif
LJ_FUNC LJThrTGResult lj_thr_tg_install(LJTGRegistryBorrow *new_hold);
LJ_FUNC LJThrTGResult lj_thr_tg_swap(const LJTGRegistryKey *expected_old,
				     LJTGRegistryBorrow *new_hold,
				     LJTGRegistryBorrow *old_hold);
LJ_FUNC LJThrTGResult lj_thr_tg_clear(const LJTGRegistryKey *expected_old,
				      LJTGRegistryBorrow *old_hold);
LJ_FUNC int lj_thr_tg_current_key(LJTGRegistryKey *key);
#if defined(LJ_THR_TLS_TEST_HELPERS)
LJ_FUNC void lj_thr_tls_test_set_word(uintptr_t word);
#if LJ_TARGET_WINDOWS
LJ_FUNC void lj_thr_tls_test_fail_index_alloc(uint32_t nth);
LJ_FUNC void lj_thr_tls_test_fail_cell_alloc(uint32_t nth);
LJ_FUNC void lj_thr_tls_test_fail_cell_publish(uint32_t nth);
#endif
#endif
#if LJ_THR_TG_SIGNAL_CACHE && defined(LJ_THR_SIGNAL_TEST_HELPERS)
LJ_FUNC void lj_thr_tg_signal_test_fail_key_create(uint32_t nth);
LJ_FUNC void lj_thr_tg_signal_test_fail_cell_alloc(uint32_t nth);
LJ_FUNC void lj_thr_tg_signal_test_fail_cell_publish(uint32_t nth);
#if LJ_TARGET_LINUX
LJ_FUNC void lj_thr_tg_signal_test_fail_fork_page(uint32_t nth);
#endif
LJ_FUNC uint64_t lj_thr_tg_signal_test_generation(void);
LJ_FUNC uintptr_t lj_thr_tg_signal_test_process(void);
LJ_FUNC uint32_t lj_thr_tg_signal_test_poisoned(void);
LJ_FUNC void lj_thr_tg_signal_test_force_generation(uint64_t generation);
LJ_FUNC void lj_thr_tg_signal_test_force_process(uintptr_t process);
LJ_FUNC void lj_thr_tg_signal_test_advance_same_process(void);
LJ_FUNC void lj_thr_tg_signal_test_force_building(void);
LJ_FUNC uint32_t lj_thr_tg_signal_test_key_state(void);
LJ_FUNC void lj_thr_tg_signal_test_reset_destructors(void);
LJ_FUNC uint32_t lj_thr_tg_signal_test_destructors(void);
LJ_FUNC uintptr_t lj_thr_tg_signal_test_last_destructor_word(void);
#endif
LJ_FUNC void lj_thr_set_tg(TGState *tg);
LJ_FUNC TGState *lj_thr_get_tg(void);
/* Consume actor zero only for a private/ATTACHING TG. LIVE zero is paired
** handoff authority and may be consumed only by lj_thr_main_close_claim(). */
LJ_FUNC int lj_thr_tg_bind_current(TGState *tg);
LJ_FUNC int lj_thr_tg_retire_current(TGState *tg);
/* Explicit quiescent raw-carrier handoff. This is not a general live-TG
** migration primitive. It clears same-thread TLS before publishing actor 0. */
LJ_FUNC int lj_thr_tg_handoff_current(TGState *expected_tg);
/* Handler-only exact TG lookup. It never consults compiler TLS/TLV state and
** never returns raw compatibility bindings. */
LJ_FUNC TGState *lj_thr_get_tg_signal(void);
/* Transitional profiler-only lookup. It additionally accepts a same-thread
** raw mirror until production lifecycle callers all own exact leases. */
LJ_FUNC TGState *lj_thr_get_tg_profile_signal(void);
/* Cold signal activation. The caller must pin the containing image first,
** because POSIX provides no way to unregister an atfork callback. */
LJ_FUNC int lj_thr_tg_signal_activate(void);
/* Retryable cold admission for the TG already owned by this OS thread. It
** republishes the exact/raw TLS binding into the process-stable signal cell. */
LJ_FUNC int lj_thr_tg_signal_prepare_current(TGState *expected_tg);
/* Cold process-incarnation repair plus an exact nonwrapping generation
** snapshot. Linux repair also consumes the kernel WIPEONFORK witness. */
LJ_FUNC int lj_thr_tg_signal_process_snapshot(uint64_t *generation,
                                               uint32_t *advanced);
LJ_FUNCA TGState *lj_thr_get_tg_fallback(global_State *g);
/* Main-state close arbitration accepts the owning actor, an explicit actor-0
** handoff, or (on Linux) a quiescent actor proven dead by its stable record and
** kernel task-lifetime witness. Stock live-yet-quiescent cross-thread close
** still lacks a public remotely invalidatable carrier/handoff and remains a
** b1.2.1 compatibility blocker; macOS/Windows exited actors fail closed too. */
LJ_FUNC int lj_thr_main_close_claim(lua_State *L);
LJ_FUNC int lj_threading_attach(lua_State *L);
LJ_FUNC int lj_threading_attach_wait(lua_State *L);
LJ_FUNC void lj_threading_detach(lua_State *L, int disown_callbacks);
LJ_FUNC int lj_threading_detach_callback_unwind(lua_State *L);
LJ_FUNC int lj_state_claim(lua_State *L, uint32_t tid);
LJ_FUNC int lj_state_tryclaim(lua_State *L, uint32_t tid, LJStateClaim *claim);
LJ_FUNC int lj_state_resumeclaim(lua_State *L, uint32_t tid,
				 LJStateClaim *claim);
LJ_FUNC void lj_state_resumeboundary_begin(const LJStateClaim *claim,
					   LJStateResumeBoundary *boundary);
LJ_FUNC void lj_state_resumeboundary_restore(const LJStateClaim *claim,
					     LJStateResumeBoundary *boundary,
					     int status);
LJ_FUNC int lj_state_gcscan_claim(lua_State *L, LJStateClaim *claim);
LJ_FUNC void lj_state_dropclaim(LJStateClaim *claim);
LJ_FUNC void lj_state_dropresumeclaim(LJStateClaim *claim);
LJ_FUNC void lj_state_resume_release(lua_State *L, uint32_t tid);
LJ_FUNCA uint32_t lj_state_resume_release_result(lua_State *L, uint32_t tid,
						  uint32_t result);
LJ_FUNC uint32_t lj_state_owner_wait(lua_State *L, lua_State *target,
				     uint32_t owner, int64_t ns);
LJ_FUNC void lj_state_release(lua_State *L, uint32_t tid);
LJ_FUNC uint32_t lj_thr_cpucount(void);
LJ_FUNC void lj_thr_fence(void);
LJ_FUNC uint32_t lj_thr_yield(lua_State *L);
LJ_FUNC uint32_t lj_thr_retry_yield(lua_State *L);
LJ_FUNC uint32_t lj_thr_sleep_ns(lua_State *L, int64_t ns);
LJ_FUNC void lj_threading_shutdown(lua_State *L);
LJ_FUNC int lj_threading_live_retry_tgs_terminal(global_State *g);
LJ_FUNC void lj_threading_live_free_all(global_State *g);

#endif
