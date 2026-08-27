/*
** Live macOS ARM64 contract for heap-published JIT exit targets.
**
** The fixture records the one admitted integer BC_LOOP root, proves that its
** immutable gates are backed by a separately allocated target table, drives
** the default XPOLL fallback, and then exercises production retargeting of
** deterministic exit 8. Signal-producing target modes run in fresh children
** so SIGTRAP and ARM64e pointer-authentication failures cannot be mistaken for
** assertions or ordinary Lua errors.
*/

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS)

#if defined(__arm64e__)
#include <ptrauth.h>
#endif

#include "lj_obj.h"
#include "lj_asm.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_mcode.h"
#include "lj_profile.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

#if !LJ_TARGET_OSX || !LJ_TARGET_ARM64 || !LJ_HASJIT || \
    !LJ_ARM64_JIT_EXIT_TARGET_SLOTS || \
    LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-exittab requires the admitted ARM64 root and exit slots"
#endif

#if !LJ_HASPROFILE || !LJ_PROFILE_TGLOCAL
#error "t-arm64-jit-exittab requires ARM64 TG-local profile polling"
#endif

extern char **environ;
extern void lj_test_arm64_exittab_trap(void);

enum {
  R_I = REF_FIRST,
  R_X,
  R_I_NEXT,
  R_X_NEXT,
  R_N,
  R_PRECOND,
  R_LOOP,
  R_XPOLL,
  R_I_BODY,
  R_X_BODY,
  R_COND,
  R_I_PHI,
  R_X_PHI,
  R_RENAME_I,
  R_RENAME_X,
  R_END
};

#define ENTRY_WAIT_TIMEOUT_NS U64x(00000006,fc23ac00)  /* 30 seconds. */

enum {
  XPOLL_EXIT = 5,
  TERMINAL_EXIT = 8
};

typedef enum TargetMode {
  TARGET_CONTROL,
  TARGET_PUBLISHED,
  TARGET_RESET,
#if LJ_ABI_PAUTH
  TARGET_RAW_NEGATIVE,
  TARGET_CTX0_NEGATIVE,
  TARGET_TRACE_NEGATIVE,
  TARGET_WRONG_GLOBAL_NEGATIVE,
#endif
  TARGET_BAD
} TargetMode;

typedef struct ProfilePublisher {
  global_State *g;
  TGState *tg;
  uint64_t epoch;
  uint32_t saw_stage;
  uint32_t saw_jit_base;
  uint32_t published;
} ProfilePublisher;

static const IRRef expected_snaprefs[] = {
  R_I, R_I_NEXT, R_X_NEXT, R_N, R_PRECOND, R_LOOP,
  R_I_BODY, R_X_BODY, R_COND
};

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 exit-table chunk failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void signal_ready(void);

static int call_sum(lua_State *L, lua_Integer n, lua_Integer expected,
	int announce_target)
{
  int status;
  lua_getglobal(L, "__arm64_exittab_integer_loop");
  if (!lua_isfunction(L, -1))
    return 71;
  lua_pushinteger(L, n);
  if (announce_target)
    signal_ready();
  status = lua_pcall(L, 1, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 exit-table call failed: %s\n",
	    lua_tostring(L, -1));
    lua_pop(L, 1);
    return 72;
  }
  if (!lua_isnumber(L, -1) || lua_tointeger(L, -1) != expected) {
    lua_pop(L, 1);
    return 73;
  }
  lua_pop(L, 1);
  return 0;
}

static GCproto *global_proto(lua_State *L)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, "__arm64_exittab_integer_loop");
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static void expect_ir(const IRIns *ir, IRRef ref, IROp op, uint8_t type,
	IRRef op1, IRRef op2)
{
  assert(ir[ref].o == op);
  assert(ir[ref].t.irt == type);
  assert(ir[ref].op1 == op1);
  assert(ir[ref].op2 == op2);
}

static void expect_ir_shape(const GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  const IRRef one = REF_TRUE - 1u;
  IRRef ref, k;

  assert(ir != NULL);
  assert(trace_nins_acq(T) == R_END);
  assert(trace_nk_acq(T) == REF_TRUE - 1u);
  assert(ir[REF_TRUE-1u].o == IR_KINT);
  assert(ir[REF_TRUE-1u].t.irt == IRT_INT);
  assert(ir[REF_TRUE-1u].i == 1);
  for (k = REF_TRUE; k <= REF_NIL; k++) {
    assert(ir[k].o == IR_KPRI);
    assert(ir[k].t.irt == (uint8_t)(REF_NIL-k));
    assert(ir[k].op12 == 0);
  }

  expect_ir(ir, REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  expect_ir(ir, R_I, IR_SLOAD, IRT_INT|IRT_GUARD,
	    3, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_X, IR_SLOAD, IRT_INT|IRT_GUARD,
	    4, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_I_NEXT, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, R_I, one);
  expect_ir(ir, R_X_NEXT, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, R_I_NEXT, R_X);
  expect_ir(ir, R_N, IR_SLOAD, IRT_INT|IRT_GUARD,
	    2, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_PRECOND, IR_GT, IRT_INT|IRT_GUARD, R_N, R_I_NEXT);
  expect_ir(ir, R_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  expect_ir(ir, R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  expect_ir(ir, R_I_BODY, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, R_I_NEXT, one);
  expect_ir(ir, R_X_BODY, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, R_I_BODY, R_X_NEXT);
  expect_ir(ir, R_COND, IR_LT, IRT_INT|IRT_GUARD, R_I_BODY, R_N);
  expect_ir(ir, R_I_PHI, IR_PHI, IRT_INT, R_I_NEXT, R_I_BODY);
  expect_ir(ir, R_X_PHI, IR_PHI, IRT_INT, R_X_NEXT, R_X_BODY);
  expect_ir(ir, R_RENAME_I, IR_RENAME, IRT_NIL, R_I_NEXT, 5);
  expect_ir(ir, R_RENAME_X, IR_RENAME, IRT_NIL, R_X_NEXT, 5);

  for (ref = REF_BASE; ref < trace_nins_acq(T); ref++)
    assert(!ra_hasspill(ir[ref].s));
}

static void expect_snapshot_shape(const GCtrace *T)
{
  SnapShot *snap = trace_snap_acq(T);
  SnapNo sn;
  assert(snap != NULL && trace_snapmap_acq(T) != NULL);
  assert(trace_nsnap_acq(T) ==
	 (SnapNo)(sizeof(expected_snaprefs)/sizeof(expected_snaprefs[0])));
  for (sn = 0; sn < trace_nsnap_acq(T); sn++)
    assert(snap_ref_acq(&snap[sn]) == expected_snaprefs[sn]);
}

static int ptr_in_span(const void *ptr, const void *base, size_t size)
{
  uintptr_t p = (uintptr_t)ptr;
  uintptr_t b = (uintptr_t)base;
  return p >= b && p-b < size;
}

static int range_in_span(const void *ptr, size_t len,
	const void *base, size_t size)
{
  uintptr_t p = (uintptr_t)ptr;
  uintptr_t b = (uintptr_t)base;
  return len <= size && p >= b && p-b <= size-len;
}

static MCode *active_mcode_rx_area_for_range(jit_State *J, const void *ptr,
	size_t len)
{
  MCode *area = J->mcarea;
  while (area != NULL) {
    size_t size = ((const MCLink *)area)->size;
    if (range_in_span(ptr, len, area, size))
      return area;
    area = mcode_area_next_acq(area);
  }
  return NULL;
}

static int ptr_in_active_mcode(jit_State *J, const void *ptr)
{
  MCode *area = J->mcarea;
  while (area != NULL) {
    size_t size = ((const MCLink *)area)->size;
    MCode *rw = lj_mcode_area_rw(area);
    if (ptr_in_span(ptr, area, size) ||
	(rw != NULL && ptr_in_span(ptr, rw, size)))
      return 1;
    area = mcode_area_next_acq(area);
  }
  return 0;
}

static void *slot_bits(const GCtrace *T, ExitNo exitno)
{
  MCode **exittab = trace_exittab_acq(T);
  return la_loadptr_acq((void *const *)&exittab[exitno]);
}

static MCode *landing_raw(void)
{
#if LJ_ABI_PAUTH
  ASMFunction fn = ptrauth_nop_cast(ASMFunction,
				     lj_test_arm64_exittab_trap);
  return ptrauth_nop_cast(MCode *,
	 ptrauth_strip(fn, ptrauth_key_function_pointer));
#else
  return (MCode *)(void *)lj_test_arm64_exittab_trap;
#endif
}

static void expect_slot_auth(global_State *g, const GCtrace *T,
	ExitNo exitno, MCode *raw)
{
  void *bits = slot_bits(T, exitno);
  assert(trace_exittarget_arm64_acq(T, exitno) == raw);
#if LJ_ABI_PAUTH
  {
    ASMFunction target = ptrauth_nop_cast(ASMFunction, bits);
    void *authenticated = ptrauth_auth_data(
	 ptrauth_nop_cast(void *, target), ptrauth_key_function_pointer, g);
    assert(authenticated == (void *)raw);
    assert((uintptr_t)bits != (uintptr_t)(void *)raw);
  }
#else
  UNUSED(g);
  assert(bits == (void *)raw);
#endif
}

static void expect_other_slots_default(global_State *g, const GCtrace *T,
	ExitNo changed, MCode *fallback)
{
  ExitNo i;
  for (i = 0; i < trace_exittab_nslots_acq(T); i++)
    if (i != changed)
      expect_slot_auth(g, T, i, fallback);
}

static void publish_exit_target(jit_State *J, GCtrace *T, ExitNo exitno,
	MCode *target)
{
  assert(lj_jit_token_try(J));
  lj_asm_patchexit(J, T, exitno, target);
  lj_jit_token_release(J);
}

static void expect_exit_layout(lua_State *L, jit_State *J, const GCtrace *T)
{
  global_State *g = G(L);
  MCode **exittab = trace_exittab_acq(T);
  MCode *gates = trace_exitstub_acq(T);
  MCode *fallback;
  intptr_t k64ofs =
    (intptr_t)((char *)&J->k64[LJ_K64_VM_EXIT_HANDLER] - (char *)g);
  MSize nslots = trace_exittab_nslots_acq(T);
  size_t layout_size =
    (ARM64_EXIT_FALLBACK_WORDS + ARM64_EXIT_GATE_WORDS * nslots) *
    sizeof(MCode);
  MCode *layout_area;
  MCode *layout_rw;
  ExitNo i;

  assert(exittab != NULL && gates != NULL);
  fallback = exitstub_trace_fallback_addr_(gates);
  assert(nslots == (MSize)trace_nsnap_acq(T));
  assert(nslots == 9);
  assert(!trace_exittab_ismcode(T));
  assert(((uintptr_t)(void *)exittab & 7u) == 0);
  assert(((uintptr_t)(void *)gates & 7u) == 0);
  assert(lj_gc2_mem_registered(g, exittab));
  assert(!ptr_in_active_mcode(J, exittab));
  assert(ptr_in_active_mcode(J, trace_mcode_acq(T)));
  assert(ptr_in_active_mcode(J, fallback));
  assert(ptr_in_active_mcode(J, gates));
  layout_area = active_mcode_rx_area_for_range(J, fallback, layout_size);
  assert(layout_area != NULL);
  layout_rw = lj_mcode_rx2rw(layout_area, fallback);
  assert(range_in_span(layout_rw, layout_size,
	lj_mcode_area_rw(layout_area), ((const MCLink *)layout_area)->size));

  assert(fallback[0] == A64I_BTI_J);
  assert(k64ofs >= 0 && (k64ofs & 7) == 0 &&
	 (uint32_t)(k64ofs >> 3) < 4096);
  assert(fallback[1] ==
	 (A64I_LDRx | A64F_D(RID_LR) | A64F_N(RID_GL) |
	  A64F_U12((uint32_t)(k64ofs >> 3))));
  assert(fallback[2] == (A64I_BLR_AUTH | A64F_N(RID_LR)));
  assert(fallback[3] ==
	 (A64I_MOVZw | A64F_D(RID_X0) | A64F_U16(1)));

  for (i = 0; i < nslots; i++) {
    MCode *gate = exitstub_trace_addr_(gates, i);
    uint64_t literal = 0;
    memcpy(&literal, &gate[6], sizeof(literal));
    assert(gate == gates + ARM64_EXIT_GATE_WORDS * i);
    assert(gate[0] ==
	   (A64I_MOVZw | A64F_D(RID_LR) | A64F_U16(i)));
    assert(gate[1] ==
	   (A64I_STRx | A64F_D(RID_LR) | A64F_N(RID_SP)));
    assert(gate[2] ==
	   (A64I_LDRLx | A64F_D(RID_LR) | A64F_S19(4)));
    assert(gate[3] ==
	   (A64I_LDARx | A64F_D(RID_LR) | A64F_N(RID_LR)));
    assert(gate[4] == (A64I_BR_G_AUTH | A64F_N(RID_LR)));
    assert(gate[5] == A64I_NOP);
    assert(literal == (uint64_t)(uintptr_t)(void *)&exittab[i]);
    expect_slot_auth(g, T, i, fallback);
  }
}

static void expect_only_root(lua_State *L, jit_State *J, GCproto *pt)
{
  GCtrace *T = traceref_safe(J, 1);
  TraceNo traceno;
  const BCIns *pc;
  BCIns live;

  if (T == NULL || !trace_runnable_acq(T, 1)) {
    fprintf(stderr,
      "ARM64 exit-table root missing: slot=%p state=%u alloc=%u free=%u "
      "last_alloc=%u last_free=%u retries=%u aborts=%u last_error=%u "
      "szall=%zu freetrace=%u\n",
      (void *)T,
      (unsigned)lj_trace_state_load(J),
      (unsigned)lj_trace_test_exittab_allocs(),
      (unsigned)lj_trace_test_exittab_frees(),
      (unsigned)lj_trace_test_exittab_last_alloc_slots(),
      (unsigned)lj_trace_test_exittab_last_free_slots(),
      (unsigned)lj_trace_test_mcode_retries(),
      (unsigned)lj_trace_test_abort_count(),
      (unsigned)lj_trace_test_last_abort_error(), J->szallmcarea,
      (unsigned)J->freetrace);
  }
  assert(T != NULL && trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1);
  assert(trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 1);
  assert(trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(T) == 0);
  assert(trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == pt);
  assert((la_load8_acq(&T->unused1) &
	  TRACE_ARM64_INT_LOOP_ADMITTED) != 0);
  assert((la_load8_acq(&T->unused1) &
	  (TRACE_ENTRY_GATED|TRACE_RETIRED_UNPUBLISHED)) == 0);
  assert(trace_spadjust_acq(T) == 0);
  assert(trace_mcode_acq(T) != NULL);
  assert(trace_mcloop_acq(T) > 0 &&
	 trace_mcloop_acq(T) < trace_szmcode_acq(T));
  pc = trace_startpc_acq(T);
  assert(pc != NULL && trace_startpt_acq(T) == pt);
  assert(bc_op(trace_startins_acq(T)) == BC_LOOP);
  live = (BCIns)la_load32_acq((const uint32_t *)pc);
  assert(bc_op(live) == BC_JLOOP && (TraceNo)bc_d(live) == 1);
  assert(proto_trace_acq(pt) == 1);

  /* Ownership moved to the registered body. The assembler scratch must not
  ** retain aliases that could later double-free the heap table. */
  assert(trace_traceno_acq(&J->cur) == 0);
  assert(trace_exittab_acq(&J->cur) == NULL);
  assert(trace_exitstub_acq(&J->cur) == NULL);
  assert(J->curfinal == NULL);

  expect_ir_shape(T);
  expect_snapshot_shape(T);
  expect_exit_layout(L, J, T);
  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
}

static void wait_for_postadmission(ProfilePublisher *publisher)
{
  struct timespec ts;
  uint64_t deadline;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  deadline = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec +
    ENTRY_WAIT_TIMEOUT_NS;
  for (;;) {
    if (lj_trace_test_root_entry_paused() ==
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION) {
      la_store32_rel(&publisher->saw_stage, 1);
      return;
    }
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    if ((uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec >= deadline)
      break;
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"ARM64 exit-table entry did not reach post-admission pause");
}

static void *publish_profile_request(void *arg)
{
  ProfilePublisher *publisher = (ProfilePublisher *)arg;
  wait_for_postadmission(publisher);
  assert(gc2_hs_epoch_acq(publisher->g) == publisher->epoch);
  assert(lj_tg_hs_epoch_ack_acq(publisher->tg) == publisher->epoch);
  assert(gc2_hs_pending_acq(publisher->g) == 0);
  assert(lj_tg_reqmask_acq(publisher->tg) == 0);
  assert(lj_tg_poll_acq(publisher->tg) == 0);
  assert(lj_tg_profile_request_acq(publisher->tg) == 0);
  if (lj_tg_load_jit_base(publisher->tg) != NULL)
    la_store32_rel(&publisher->saw_jit_base, 1);
  assert(la_load32_acq(&publisher->saw_jit_base) == 1);
  lj_tg_profile_request_rel(publisher->tg, 1);
  la_store32_rel(&publisher->published, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static void exercise_default_xpoll(lua_State *L, jit_State *J, TGState *tg,
	GCproto *pt)
{
  ProfilePublisher publisher = {
    G(L), tg, gc2_hs_epoch_acq(G(L)), 0, 0, 0
  };
  pthread_t worker;

  assert(lj_tg_hs_epoch_ack_acq(tg) == publisher.epoch);
  assert(gc2_hs_pending_acq(publisher.g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  assert(pthread_create(&worker, NULL, publish_profile_request,
		&publisher) == 0);
  assert(call_sum(L, 20, 210, 0) == 0);
  assert(pthread_join(worker, NULL) == 0);

  assert(la_load32_acq(&publisher.saw_stage) == 1);
  assert(la_load32_acq(&publisher.saw_jit_base) == 1);
  assert(la_load32_acq(&publisher.published) == 1);
  assert(lj_trace_test_root_entry_paused() == 0);
  assert(lj_trace_test_root_entry_publishes() == 2);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 2);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == XPOLL_EXIT);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == TERMINAL_EXIT);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  expect_only_root(L, J, pt);
}

static void expect_normal_terminal_exit(lua_State *L)
{
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  assert(call_sum(L, 20, 210, 0) == 0);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == TERMINAL_EXIT);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == TERMINAL_EXIT);
}

static void close_and_expect_exittab_balance(lua_State *L)
{
  lua_close(L);
  assert(lj_trace_test_exittab_allocs() == 2);
  assert(lj_trace_test_exittab_frees() == 2);
  assert(lj_trace_test_exittab_last_alloc_slots() == 9);
  assert(lj_trace_test_exittab_last_free_slots() == 9);
}

static void signal_ready(void)
{
  const char ready = 'R';
  ssize_t written;
  do {
    written = write(3, &ready, sizeof(ready));
  } while (written < 0 && errno == EINTR);
  if (written != (ssize_t)sizeof(ready))
    _exit(85);
}

#if LJ_ABI_PAUTH
static void publish_negative_target(jit_State *J, global_State *g, GCtrace *T,
	TargetMode mode, MCode *raw)
{
  ASMFunction target = ptrauth_nop_cast(ASMFunction, raw);
  void *bits;
  uintptr_t wrong_global_context = 0;
  void *context;

  assert(mode == TARGET_RAW_NEGATIVE || mode == TARGET_CTX0_NEGATIVE ||
	 mode == TARGET_TRACE_NEGATIVE ||
	 mode == TARGET_WRONG_GLOBAL_NEGATIVE);
  assert(lj_jit_token_held(J));
  if (mode == TARGET_RAW_NEGATIVE) {
    bits = (void *)raw;
  } else {
    if (mode == TARGET_CTX0_NEGATIVE)
      context = NULL;
    else if (mode == TARGET_TRACE_NEGATIVE)
      context = T;
    else
      context = &wrong_global_context;
    assert(context != g);
    target = ptrauth_sign_unauthenticated(target,
	 ptrauth_key_function_pointer, context);
    bits = ptrauth_nop_cast(void *, target);
  }
  la_storeptr_rel((void **)&trace_exittab_acq(T)[TERMINAL_EXIT], bits);
  assert(slot_bits(T, TERMINAL_EXIT) == bits);
  assert(trace_exittarget_arm64_acq(T, TERMINAL_EXIT) == raw);
}
#endif

static int child_mode(TargetMode mode)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  TGState *tg;
  GCproto *pt;
  GCtrace *T;
  MCode *fallback, *gates, *landing;
  MCode fallback_copy[ARM64_EXIT_FALLBACK_WORDS];
  MCode gate_copy[ARM64_EXIT_GATE_WORDS];
  void *saved_cframe;
  int32_t saved_vmstate;

  assert(L != NULL);
  luaL_openlibs(L);
  J = L2J(L);
  tg = L2TG(L);
  assert(J != NULL && tg != NULL);
  saved_cframe = L->cframe;
  saved_vmstate = lj_tg_vmstate_load_acq(tg);
  run_lua(L, "jit.opt.start('maxsnap=100000')");
  assert(jit_param_acq(J, JIT_P_maxsnap) == UINT16_MAX);
  run_lua(L, "jit.opt.start('maxsnap=500')");
  lj_trace_test_reset_exittab_stats();
  lj_asm_arm64_test_force_exitstub_mcode_retry(1);
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "local function f(n) "
      "local i,x=0,0 "
      "while i<n do i=i+1 x=x+i end "
      "return x "
    "end "
    "__arm64_exittab_integer_loop=f");

  assert(call_sum(L, 20, 210, 0) == 0);
  assert(L->cframe == saved_cframe);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
  assert(lj_tg_load_jit_base(tg) == NULL);
  pt = global_proto(L);
  expect_only_root(L, J, pt);
  assert(lj_trace_test_exittab_allocs() == 2);
  assert(lj_trace_test_exittab_frees() == 1);
  assert(lj_trace_test_mcode_retries() == 1);
  assert(lj_trace_test_exittab_last_alloc_slots() == 9);
  assert(lj_trace_test_exittab_last_free_slots() == 9);
  exercise_default_xpoll(L, J, tg, pt);

  T = traceref_safe(J, 1);
  assert(T != NULL && trace_runnable_acq(T, 1));
  gates = trace_exitstub_acq(T);
  assert(gates != NULL);
  fallback = exitstub_trace_fallback_addr_(gates);
  landing = landing_raw();
  assert(landing != NULL && landing != fallback);
  assert(!ptr_in_active_mcode(J, landing));
  assert(landing[0] == A64I_BTI_J);
  assert((landing[1] & 0xffe0001fu) == 0xd4200000u);  /* BRK #imm16. */
  memcpy(fallback_copy, fallback, sizeof(fallback_copy));
  memcpy(gate_copy, exitstub_trace_addr_(gates, TERMINAL_EXIT),
	 sizeof(gate_copy));

  if (mode == TARGET_CONTROL) {
    expect_normal_terminal_exit(L);
    expect_only_root(L, J, pt);
    close_and_expect_exittab_balance(L);
    return 0;
  }

  if (mode == TARGET_RESET) {
    publish_exit_target(J, T, TERMINAL_EXIT, landing);
    expect_slot_auth(G(L), T, TERMINAL_EXIT, landing);
    expect_other_slots_default(G(L), T, TERMINAL_EXIT, fallback);
    assert(memcmp(fallback_copy, fallback, sizeof(fallback_copy)) == 0);
    assert(memcmp(gate_copy, exitstub_trace_addr_(gates, TERMINAL_EXIT),
		  sizeof(gate_copy)) == 0);
    publish_exit_target(J, T, TERMINAL_EXIT, fallback);
    expect_slot_auth(G(L), T, TERMINAL_EXIT, fallback);
    expect_normal_terminal_exit(L);
    expect_only_root(L, J, pt);
    assert(L->cframe == saved_cframe);
    assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
    assert(lj_tg_load_jit_base(tg) == NULL);
    close_and_expect_exittab_balance(L);
    return 0;
  }

  if (mode == TARGET_PUBLISHED) {
    /* This is a raw pointer publication on ordinary ARM64 and a global_State
    ** authenticated publication on ARM64e. */
    publish_exit_target(J, T, TERMINAL_EXIT, landing);
    expect_slot_auth(G(L), T, TERMINAL_EXIT, landing);
  }
#if LJ_ABI_PAUTH
  else {
    assert(lj_jit_token_try(J));
    publish_negative_target(J, G(L), T, mode, landing);
    lj_jit_token_release(J);
  }
#else
  else {
    return 86;
  }
#endif
  expect_other_slots_default(G(L), T, TERMINAL_EXIT, fallback);
  assert(memcmp(fallback_copy, fallback, sizeof(fallback_copy)) == 0);
  assert(memcmp(gate_copy, exitstub_trace_addr_(gates, TERMINAL_EXIT),
		sizeof(gate_copy)) == 0);
  assert(trace_nchild_acq(T) == 0 && trace_nextside_acq(T) == 0);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  fprintf(stderr, "ARM64 exit-table attempting target mode %d\n", (int)mode);
  fflush(stderr);
  /* Correct publication reaches the assembly BRK and terminates with SIGTRAP.
  ** A malformed ARM64e slot must fail BRAA first and terminate with SIGBUS.
  ** Restore the fallback if either path unexpectedly returns. */
  if (call_sum(L, 20, 210, 1) != 0)
    return 87;
  publish_exit_target(J, T, TERMINAL_EXIT, fallback);
  return 88;
}

static TargetMode parse_mode(const char *name)
{
  if (strcmp(name, "control") == 0)
    return TARGET_CONTROL;
  if (strcmp(name, "published") == 0)
    return TARGET_PUBLISHED;
  if (strcmp(name, "reset") == 0)
    return TARGET_RESET;
#if LJ_ABI_PAUTH
  if (strcmp(name, "raw-negative") == 0)
    return TARGET_RAW_NEGATIVE;
  if (strcmp(name, "ctx0-negative") == 0)
    return TARGET_CTX0_NEGATIVE;
  if (strcmp(name, "trace-negative") == 0)
    return TARGET_TRACE_NEGATIVE;
  if (strcmp(name, "wrong-global-negative") == 0)
    return TARGET_WRONG_GLOBAL_NEGATIVE;
#endif
  return TARGET_BAD;
}

static void terminate_child(pid_t pid)
{
  int status;
  pid_t got;
  (void)kill(pid, SIGKILL);
  do {
    got = waitpid(pid, &status, 0);
  } while (got < 0 && errno == EINTR);
}

static int waitpid_bounded(pid_t pid, int *status)
{
  const struct timespec pause = { 0, 100000000L };
  uint32_t pollno;
  for (pollno = 0; pollno < 300u; pollno++) {
    pid_t got = waitpid(pid, status, WNOHANG);
    if (got == pid)
      return 0;
    if (got < 0 && errno != EINTR)
      return errno;
    (void)nanosleep(&pause, NULL);
  }
  terminate_child(pid);
  return ETIMEDOUT;
}

static int spawn_mode(const char *self, const char *mode, int expected_signal)
{
  char *const child_argv[] = { (char *)self, (char *)mode, NULL };
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_t *actionsp = NULL;
  int ready_pipe[2] = { -1, -1 };
  char ready = 0;
  ssize_t nread = 0;
  pid_t pid;
  int status, err, actions_initialized = 0;

  if (expected_signal != 0) {
    if (pipe(ready_pipe) != 0) {
      fprintf(stderr, "pipe %s failed: %s\n", mode, strerror(errno));
      return 1;
    }
    err = posix_spawn_file_actions_init(&actions);
    if (err == 0)
      actions_initialized = 1;
    if (err == 0)
      err = posix_spawn_file_actions_addclose(&actions, ready_pipe[0]);
    if (err == 0)
      err = posix_spawn_file_actions_adddup2(&actions, ready_pipe[1], 3);
    if (err == 0 && ready_pipe[1] != 3)
      err = posix_spawn_file_actions_addclose(&actions, ready_pipe[1]);
    if (err != 0) {
      fprintf(stderr, "spawn actions %s failed: %s\n", mode,
	      strerror(err));
      if (actions_initialized)
	(void)posix_spawn_file_actions_destroy(&actions);
      close(ready_pipe[0]);
      close(ready_pipe[1]);
      return 1;
    }
    actionsp = &actions;
  }

  err = posix_spawn(&pid, self, actionsp, NULL, child_argv, environ);
  if (expected_signal != 0) {
    (void)posix_spawn_file_actions_destroy(&actions);
    close(ready_pipe[1]);
  }
  if (err != 0) {
    fprintf(stderr, "posix_spawn %s failed: %s\n", mode, strerror(err));
    if (expected_signal != 0)
      close(ready_pipe[0]);
    return 1;
  }

  if (expected_signal != 0) {
    struct pollfd ready_event = { ready_pipe[0], POLLIN|POLLHUP, 0 };
    int polled;
    do {
      polled = poll(&ready_event, 1, 30000);
    } while (polled < 0 && errno == EINTR);
    if (polled <= 0) {
      fprintf(stderr, "%s child readiness timed out or failed\n", mode);
      close(ready_pipe[0]);
      terminate_child(pid);
      return 1;
    }
    do {
      nread = read(ready_pipe[0], &ready, sizeof(ready));
    } while (nread < 0 && errno == EINTR);
    close(ready_pipe[0]);
  }
  err = waitpid_bounded(pid, &status);
  if (err != 0) {
    fprintf(stderr, "waitpid %s failed: %s\n", mode, strerror(err));
    return 1;
  }

  if (expected_signal == 0) {
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fprintf(stderr, "%s child did not exit cleanly (status=0x%x)\n",
	      mode, status);
      return 1;
    }
    printf("ARM64 exit-table %s completed normally\n", mode);
    return 0;
  }

  if (nread != (ssize_t)sizeof(ready) || ready != 'R' ||
      !WIFSIGNALED(status) || WTERMSIG(status) != expected_signal) {
    if (nread != (ssize_t)sizeof(ready) || ready != 'R')
      fprintf(stderr, "%s child faulted before target readiness\n", mode);
    if (WIFEXITED(status))
      fprintf(stderr, "%s child returned %d instead of signal %d\n",
	      mode, WEXITSTATUS(status), expected_signal);
    else if (WIFSIGNALED(status))
      fprintf(stderr, "%s child got signal %d instead of %d\n",
	      mode, WTERMSIG(status), expected_signal);
    else
      fprintf(stderr, "%s child had unexpected status 0x%x\n", mode,
	      status);
    return 1;
  }
  printf("ARM64 exit-table %s produced signal %d\n", mode,
	 expected_signal);
  return 0;
}

static int supervise(const char *self)
{
  if (spawn_mode(self, "control", 0) != 0 ||
      spawn_mode(self, "published", SIGTRAP) != 0 ||
      spawn_mode(self, "reset", 0) != 0)
    return 1;
#if LJ_ABI_PAUTH
  if (spawn_mode(self, "raw-negative", SIGBUS) != 0 ||
      spawn_mode(self, "ctx0-negative", SIGBUS) != 0 ||
      spawn_mode(self, "trace-negative", SIGBUS) != 0 ||
      spawn_mode(self, "wrong-global-negative", SIGBUS) != 0)
    return 1;
#endif
  puts("t-arm64-jit-exittab OK");
  return 0;
}

int main(int argc, char **argv)
{
  TargetMode mode;
  if (argc == 1 || (argc == 2 && strcmp(argv[1], "supervise") == 0))
    return supervise(argv[0]);
  if (argc != 2)
    return 64;
  mode = parse_mode(argv[1]);
  if (mode == TARGET_BAD)
    return 64;
  return child_mode(mode);
}

#else

int main(void)
{
  puts("t-arm64-jit-exittab SKIP");
  return 0;
}

#endif
