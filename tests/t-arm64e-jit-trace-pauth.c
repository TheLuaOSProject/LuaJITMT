/*
** macOS ARM64e negative contract for authenticated root-trace entry.
**
** Each mutation mode runs in a fresh process. It records and validates the
** one admitted integer BC_LOOP, changes only GCtrace.mcauth, revalidates the
** semantic body, and dispatches the already-patched JLOOP. The supervising
** process requires Darwin to terminate that child with SIGBUS; a helper
** rejection, an assertion, or a successful native entry is not accepted.
*/

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && defined(__arm64e__) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS)

#include <ptrauth.h>

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"

#if !LJ_TARGET_OSX || !LJ_TARGET_ARM64 || !LJ_ABI_PAUTH || \
    !LJ_ABI_BRANCH_TRACK || !LJ_HASJIT || \
    LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64e-jit-trace-pauth requires exact ARM64e loop gates and BTI"
#endif

extern char **environ;

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

typedef enum EntryMode {
  ENTRY_CONTROL,
  ENTRY_RAW,
  ENTRY_IA_ZERO,
  ENTRY_WRONG_TRACE
} EntryMode;

static const IRRef expected_snaprefs[] = {
  R_I, R_I_NEXT, R_X_NEXT, R_N, R_PRECOND, R_LOOP,
  R_I_BODY, R_X_BODY, R_COND
};

static void signal_negative_ready(void);

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64e trace-PAUTH chunk failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static int call_sum(lua_State *L, lua_Integer n, lua_Integer expected,
	int signal_ready)
{
  int status;
  lua_getglobal(L, "__arm64e_pauth_integer_loop");
  if (!lua_isfunction(L, -1))
    return 71;
  lua_pushinteger(L, n);
  if (signal_ready)
    signal_negative_ready();
  status = lua_pcall(L, 1, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64e trace-PAUTH call failed: %s\n",
	    lua_tostring(L, -1));
    return 72;
  }
  if (!lua_isnumber(L, -1) || lua_tointeger(L, -1) != expected)
    return 73;
  lua_pop(L, 1);
  return 0;
}

static GCproto *global_proto(lua_State *L)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, "__arm64e_pauth_integer_loop");
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
  assert(trace_nsnap_acq(T) ==
	 (SnapNo)(sizeof(expected_snaprefs)/sizeof(expected_snaprefs[0])));
  for (sn = 0; sn < trace_nsnap_acq(T); sn++)
    assert(snap_ref_acq(&snap[sn]) == expected_snaprefs[sn]);
}

static void expect_loop_geometry(const GCtrace *T, const GCproto *pt)
{
  const BCIns *bc = proto_bc(pt);
  const BCIns *pc = trace_startpc_acq(T);
  BCIns startins = trace_startins_acq(T);
  BCIns back;
  int64_t pos, endpos, target;

  assert(pc >= bc && pc < bc + pt->sizebc);
  pos = (int64_t)proto_bcpos(pt, pc);
  assert(bc_op(startins) == BC_LOOP);
  assert((MSize)bc_a(startins) <= (MSize)pt->framesize);
  assert(bc_j(startins) > 0);
  endpos = pos + (int64_t)bc_j(startins);
  assert(endpos >= 0 && endpos < (int64_t)pt->sizebc);
  back = (BCIns)la_load32_acq((const uint32_t *)&bc[(BCPos)endpos]);
  assert(bc_op(back) == BC_JMP);
  assert(bc_j(back) < 0);
  target = endpos + 1 + (int64_t)bc_j(back);
  assert(target >= 0 && target <= pos && target < (int64_t)pt->sizebc);
}

static void expect_exact_body(jit_State *J, GCtrace *T, GCproto *pt)
{
  const BCIns *pc;
  BCIns patched;
  TraceNo traceno;

  assert(T != NULL && trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1);
  assert(trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 1);
  assert(trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(T) == 0);
  assert(trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == pt);
  assert(la_load64_acq(&T->retire_epoch) == 0);
  assert((la_load8_acq(&T->unused1) & TRACE_ENTRY_GATED) == 0);
  assert((la_load8_acq(&T->unused1) &
	  TRACE_ARM64_INT_LOOP_ADMITTED) != 0);

  pc = trace_startpc_acq(T);
  assert(pc != NULL);
  expect_loop_geometry(T, pt);
  patched = (BCIns)la_load32_acq((const uint32_t *)pc);
  assert(bc_op(patched) == BC_JLOOP);
  assert((TraceNo)bc_d(patched) == 1);
  assert(proto_trace_acq(pt) == 1);

  assert(trace_spadjust_acq(T) == 0);
  assert(trace_topslot_acq(T) == (MSize)pt->framesize);
  assert(trace_mcode_acq(T) != NULL);
  assert(trace_szmcode_acq(T) == 168 + sizeof(MCode));
  assert(trace_mcode_acq(T)[0] == A64I_BTI_J);
  assert(trace_mcloop_acq(T) > 0 &&
	 trace_mcloop_acq(T) < trace_szmcode_acq(T));
  assert((trace_mcloop_acq(T) & (sizeof(MCode)-1u)) == 0);
  expect_ir_shape(T);
  expect_snapshot_shape(T);

  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
}

static uintptr_t function_bits(ASMFunction fn)
{
  return (uintptr_t)ptrauth_nop_cast(void *, fn);
}

static void expect_open_entry_gates(lua_State *L, TGState *tg)
{
  assert(lj_tg_load_cur_L(tg) == L);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert(lj_gc2_jit_entry_open(G(L)));
}

static void expect_valid_trace_signature(GCtrace *T)
{
  ASMFunction signed_entry = trace_mcauth_acq(T);
  void *raw = (void *)trace_mcode_acq(T);
  void *authenticated;

  assert(signed_entry != NULL);
  assert(function_bits(signed_entry) != (uintptr_t)raw);
  assert((void *)ptrauth_strip(signed_entry,
	 ptrauth_key_function_pointer) == raw);
  authenticated = ptrauth_auth_data(
	 ptrauth_nop_cast(void *, signed_entry),
	 ptrauth_key_function_pointer, T);
  assert(authenticated == raw);
}

static ASMFunction mutated_entry(EntryMode mode, GCtrace *T,
	GCtrace *wrong_discriminator)
{
  ASMFunction valid = trace_mcauth_acq(T);
  ASMFunction raw = ptrauth_strip(valid, ptrauth_key_function_pointer);

  assert((void *)ptrauth_strip(raw, ptrauth_key_function_pointer) ==
	 (void *)trace_mcode_acq(T));
  switch (mode) {
  case ENTRY_RAW:
    return raw;
  case ENTRY_IA_ZERO:
    return ptrauth_sign_unauthenticated(raw,
	 ptrauth_key_function_pointer, 0);
  case ENTRY_WRONG_TRACE:
    assert(wrong_discriminator != T);
    return ptrauth_sign_unauthenticated(raw,
	 ptrauth_key_function_pointer, wrong_discriminator);
  default:
    assert(mode != ENTRY_CONTROL);
    return valid;
  }
}

static void signal_negative_ready(void)
{
  const char ready = 'R';
  ssize_t written;
  do {
    written = write(3, &ready, sizeof(ready));
  } while (written < 0 && errno == EINTR);
  if (written != (ssize_t)sizeof(ready))
    _exit(85);
}

static int child_mode(EntryMode mode)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  TGState *tg;
  GCproto *pt;
  GCtrace *T;
  GCtrace wrong_discriminator;
  ASMFunction original, injected, loaded;
  void *saved_cframe;
  int32_t saved_vmstate;
  int status;

  assert(L != NULL);
  luaL_openlibs(L);
  J = L2J(L);
  tg = L2TG(L);
  assert(J != NULL && tg != NULL);
  assert(lj_tg_load_cur_L(tg) == L);
  assert(lj_tg_load_jit_base(tg) == NULL);
  saved_cframe = L->cframe;
  saved_vmstate = lj_tg_vmstate_load_acq(tg);

  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "local function f(n) "
      "local i,x=0,0 "
      "while i<n do i=i+1 x=x+i end "
      "return x "
    "end "
    "__arm64e_pauth_integer_loop=f");

  /* The first call records and executes the valid baseline. */
  assert(call_sum(L, 20, 210, 0) == 0);
  assert(L->cframe == saved_cframe);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
  pt = global_proto(L);
  T = traceref_safe(J, 1);
  expect_open_entry_gates(L, tg);
  expect_exact_body(J, T, pt);
  expect_valid_trace_signature(T);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  if (mode == ENTRY_CONTROL) {
    assert(call_sum(L, 20, 210, 0) == 0);
    assert(lj_trace_test_root_entry_publishes() == 1);
    assert(lj_trace_test_root_entry_cleanups() == 0);
    assert(lj_trace_test_exit_calls() == 1);
    assert(lj_trace_test_last_exit_parent() == 1);
    assert(lj_trace_test_last_exitno() == 8);
    assert(L->cframe == saved_cframe);
    assert(lj_tg_load_jit_base(tg) == NULL);
    assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
    expect_exact_body(J, T, pt);
    expect_valid_trace_signature(T);
    lua_close(L);
    return 0;
  }

  memset(&wrong_discriminator, 0, sizeof(wrong_discriminator));
  original = trace_mcauth_acq(T);
  injected = mutated_entry(mode, T, &wrong_discriminator);
  assert((void *)ptrauth_strip(injected, ptrauth_key_function_pointer) ==
	 (void *)trace_mcode_acq(T));
  if (mode == ENTRY_RAW) {
    assert(function_bits(injected) ==
	   (uintptr_t)(void *)trace_mcode_acq(T));
  } else {
    void *context = mode == ENTRY_IA_ZERO ? NULL : &wrong_discriminator;
    void *authenticated = ptrauth_auth_data(
	 ptrauth_nop_cast(void *, injected),
	 ptrauth_key_function_pointer, context);
    assert(authenticated == (void *)trace_mcode_acq(T));
  }
  la_storefunc_rel(&T->mcauth, injected);
  loaded = trace_mcauth_acq(T);
  assert(function_bits(loaded) == function_bits(injected));

  /* Re-prove every non-PAUTH admission predicate after the only mutation. */
  expect_open_entry_gates(L, tg);
  expect_exact_body(J, T, pt);
  fprintf(stderr, "ARM64e trace-PAUTH attempting negative mode %d\n",
	  (int)mode);
  fflush(stderr);

  /* Correct behavior never returns: VM JLOOP reaches BRAA x1,x0 and Darwin
  ** reports EXC_ARM_PAC_FAIL as SIGBUS. Restore first if it does return so
  ** shutdown cannot turn an ordinary gate rejection into a false PAC signal. */
  status = call_sum(L, 20, 210, 1);
  la_storefunc_rel(&T->mcauth, original);
  assert(function_bits(trace_mcauth_acq(T)) == function_bits(original));
  if (status != 0)
    return status;
  if (lj_trace_test_root_entry_publishes() != 1)
    return 81;
  if (lj_trace_test_root_entry_cleanups() != 0)
    return 82;
  if (lj_trace_test_exit_calls() != 0)
    return 83;
  return 84;  /* Mutated target entered or otherwise returned without a fault. */
}

static EntryMode parse_mode(const char *name)
{
  if (strcmp(name, "control") == 0)
    return ENTRY_CONTROL;
  if (strcmp(name, "raw") == 0)
    return ENTRY_RAW;
  if (strcmp(name, "ia-zero") == 0)
    return ENTRY_IA_ZERO;
  if (strcmp(name, "wrong-trace") == 0)
    return ENTRY_WRONG_TRACE;
  fprintf(stderr, "unknown ARM64e trace-PAUTH mode: %s\n", name);
  exit(64);
}

static int spawn_mode(const char *self, const char *mode, int expect_bus)
{
  char *const child_argv[] = { (char *)self, (char *)mode, NULL };
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_t *actionsp = NULL;
  int ready_pipe[2] = { -1, -1 };
  char ready = 0;
  ssize_t nread = 0;
  pid_t pid;
  int status;
  int err;

  if (expect_bus) {
    if (pipe(ready_pipe) != 0) {
      fprintf(stderr, "pipe %s failed: %s\n", mode, strerror(errno));
      return 1;
    }
    err = posix_spawn_file_actions_init(&actions);
    if (err != 0) {
      fprintf(stderr, "spawn actions %s failed: %s\n",
	      mode, strerror(err));
      close(ready_pipe[0]);
      close(ready_pipe[1]);
      return 1;
    }
    err = posix_spawn_file_actions_addclose(&actions, ready_pipe[0]);
    if (err == 0)
      err = posix_spawn_file_actions_adddup2(&actions, ready_pipe[1], 3);
    if (err == 0 && ready_pipe[1] != 3)
      err = posix_spawn_file_actions_addclose(&actions, ready_pipe[1]);
    if (err != 0) {
      fprintf(stderr, "spawn actions %s failed: %s\n",
	      mode, strerror(err));
      (void)posix_spawn_file_actions_destroy(&actions);
      close(ready_pipe[0]);
      close(ready_pipe[1]);
      return 1;
    }
    actionsp = &actions;
  }

  err = posix_spawn(&pid, self, actionsp, NULL, child_argv, environ);
  if (expect_bus) {
    (void)posix_spawn_file_actions_destroy(&actions);
    close(ready_pipe[1]);
  }

  if (err != 0) {
    fprintf(stderr, "posix_spawn %s failed: %s\n", mode, strerror(err));
    if (expect_bus)
      close(ready_pipe[0]);
    return 1;
  }
  if (expect_bus) {
    do {
      nread = read(ready_pipe[0], &ready, sizeof(ready));
    } while (nread < 0 && errno == EINTR);
    close(ready_pipe[0]);
  }
  do {
    err = waitpid(pid, &status, 0) < 0 ? errno : 0;
  } while (err == EINTR);
  if (err != 0) {
    fprintf(stderr, "waitpid %s failed: %s\n", mode, strerror(err));
    return 1;
  }

  if (!expect_bus) {
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fprintf(stderr, "control child did not exit cleanly (status=0x%x)\n",
	      status);
      return 1;
    }
    puts("ARM64e trace-PAUTH control entered normally");
    return 0;
  }

  /* Do not accept SIGABRT: assertions and ordinary test failures must remain
  ** distinguishable from Darwin's EXC_ARM_PAC_FAIL delivery. The readiness
  ** byte proves this child completed its baseline and post-mutation semantic
  ** validation before any accepted signal. */
  if (nread != (ssize_t)sizeof(ready) || ready != 'R' ||
      !WIFSIGNALED(status) || WTERMSIG(status) != SIGBUS) {
    if (nread != (ssize_t)sizeof(ready) || ready != 'R')
      fprintf(stderr, "%s child faulted before negative-entry readiness\n",
	      mode);
    if (WIFEXITED(status))
      fprintf(stderr, "%s child returned %d instead of SIGBUS\n",
	      mode, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
      fprintf(stderr, "%s child got signal %d instead of SIGBUS\n",
	      mode, WTERMSIG(status));
    else
      fprintf(stderr, "%s child had unexpected wait status 0x%x\n",
	      mode, status);
    return 1;
  }
  printf("ARM64e trace-PAUTH %s rejected with SIGBUS\n", mode);
  return 0;
}

static int supervise(const char *self)
{
  if (spawn_mode(self, "control", 0) != 0 ||
      spawn_mode(self, "raw", 1) != 0 ||
      spawn_mode(self, "ia-zero", 1) != 0 ||
      spawn_mode(self, "wrong-trace", 1) != 0)
    return 1;
  puts("t-arm64e-jit-trace-pauth OK");
  return 0;
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  if (strcmp(argv[1], "supervise") == 0)
    return supervise(argv[0]);
  return child_mode(parse_mode(argv[1]));
}

#else

int main(void)
{
  puts("t-arm64e-jit-trace-pauth SKIP");
  return 0;
}

#endif
