/*
** Native macOS ARM64 execution contract for the first admitted trace shape.
** This deliberately covers one exact integer BC_LOOP root and nothing else.
*/

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_profile.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-native-loop requires the exact first-loop ARM64 gates"
#endif

#if !LJ_HASPROFILE || !LJ_PROFILE_TGLOCAL
#error "t-arm64-jit-native-loop requires ARM64 TG-local profile polling"
#endif

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

static const IRRef expected_snaprefs[] = {
  R_I, R_I_NEXT, R_X_NEXT, R_N, R_PRECOND, R_LOOP,
  R_I_BODY, R_X_BODY, R_COND
};

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 native-loop chunk failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static void call_sum_and_check_cframe(lua_State *L, lua_Integer n,
	lua_Integer expected)
{
  void *saved_cframe = L->cframe;
  int status;
  lua_getglobal(L, "__arm64_native_integer_loop");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  status = lua_pcall(L, 1, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 native-loop call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  assert(lua_tointeger(L, -1) == expected);
  lua_pop(L, 1);
  assert(L->cframe == saved_cframe);
}

typedef enum PostAdmissionRequest {
  POSTADMISSION_PROFILE,
  POSTADMISSION_STOPREQ
} PostAdmissionRequest;

typedef struct PostAdmissionPublisher {
  global_State *g;
  TGState *tg;
  uint64_t epoch;
  PostAdmissionRequest request;
  uint32_t saw_stage;
  uint32_t saw_jit_base;
  uint32_t published;
} PostAdmissionPublisher;

static void clear_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg,
	(uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
}

static void wait_for_postadmission(PostAdmissionPublisher *publisher)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() ==
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION) {
      la_store32_rel(&publisher->saw_stage, 1);
      return;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"ARM64 root entry did not reach post-admission pause");
}

static void *publish_postadmission_request(void *arg)
{
  PostAdmissionPublisher *publisher = (PostAdmissionPublisher *)arg;
  global_State *g = publisher->g;
  TGState *tg = publisher->tg;

  wait_for_postadmission(publisher);
  assert(gc2_hs_epoch_acq(g) == publisher->epoch);
  assert(lj_tg_hs_epoch_ack_acq(tg) == publisher->epoch);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  if (lj_tg_load_jit_base(tg) != NULL)
    la_store32_rel(&publisher->saw_jit_base, 1);
  assert(la_load32_acq(&publisher->saw_jit_base) == 1);

  if (publisher->request == POSTADMISSION_PROFILE) {
    /* SIGPROF publishes only this TG-local word. Native XPOLL must route the
    ** owner through profile consumption without inventing a counted epoch. */
    lj_tg_profile_request_rel(tg, 1);
  } else {
    assert(publisher->request == POSTADMISSION_STOPREQ);
    /* Manual one-TG form of the production counted publication order. The
    ** paused owner is the sole participant, so there is no leader sentinel. */
    gc2_hs_actions_rel(g, LJ_GC2_HS_STOPREQ);
    gc2_hs_pending_rel(g, 1);
    gc2_hs_epoch_rel(g, publisher->epoch + 1u);
    lj_tg_reqmask_rel(tg, LJ_GC2_HS_STOPREQ);
    lj_tg_poll_rel(tg, 1);
  }
  la_store32_rel(&publisher->published, 1);
  lj_trace_test_root_entry_release();
  return NULL;
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, name);
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

static void expect_constants(const GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  IRRef k;
  assert(trace_nk_acq(T) == REF_TRUE - 1u);
  assert(ir[REF_TRUE-1u].o == IR_KINT);
  assert(ir[REF_TRUE-1u].t.irt == IRT_INT);
  assert(ir[REF_TRUE-1u].i == 1);
  for (k = REF_TRUE; k <= REF_NIL; k++) {
    assert(ir[k].o == IR_KPRI);
    assert(ir[k].t.irt == (uint8_t)(REF_NIL-k));
    assert(ir[k].op12 == 0);
  }
}

static void expect_ir_shape(const GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  const IRRef one = REF_TRUE - 1u;

  assert(trace_nins_acq(T) == R_END);
  expect_constants(T);
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

  /* Register allocation may append exactly these two bookkeeping records.
  ** No semantic instruction is permitted after the terminal PHI pair. */
  assert(ir[R_RENAME_I].o == IR_RENAME);
  assert(ir[R_RENAME_I].t.irt == IRT_NIL);
  assert(ir[R_RENAME_X].o == IR_RENAME);
  assert(ir[R_RENAME_X].t.irt == IRT_NIL);
  expect_ir(ir, R_RENAME_I, IR_RENAME, IRT_NIL, R_I_NEXT, 5);
  expect_ir(ir, R_RENAME_X, IR_RENAME, IRT_NIL, R_X_NEXT, 5);

  /* A non-zero spill index is an execution-admission failure for this first
  ** shape. Check the final allocator view, including both RENAMEs. */
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
  assert(trace_topslot_acq(T) == (MSize)pt->framesize);
}

static void expect_exit_stub_path(jit_State *J, const GCtrace *T,
	int expect_indirect)
{
  MCode *e = exitstub_trace_addr(T, 0);
  MCode strlr = A64I_STRx | A64F_D(RID_LR) | A64F_N(RID_SP);
  MCode movtrace = A64I_MOVZw | A64F_U16(1);

  assert(e != NULL);
  assert(e[-1] == movtrace);
  if (expect_indirect) {
    intptr_t k64ofs =
      (intptr_t)((char *)&J->k64[LJ_K64_VM_EXIT_HANDLER] -
		 (char *)&J2GG(J)->g);
    MCode ldrhandler;
    assert(k64ofs >= 0 && (k64ofs & 7) == 0 &&
	   (k64ofs >> 3) < 4096);
    ldrhandler = A64I_LDRx | A64F_D(RID_LR) | A64F_N(RID_GL) |
		 A64F_U12((uint32_t)(k64ofs >> 3));
    assert(e[-4] == strlr);
    assert(e[-3] == ldrhandler);
    assert(e[-2] == (A64I_BLR_AUTH | A64F_N(RID_LR)));
#if LJ_ABI_PAUTH
    assert(e[-2] == (A64I_BLRAAZ | A64F_N(RID_LR)));
#endif
  } else {
    assert(e[-3] == strlr);
    assert((e[-2] & 0xfc000000u) == A64I_BL);
  }
}

static void expect_only_root_trace(jit_State *J, GCproto *pt,
	int expect_indirect)
{
  TraceNo traceno;
  GCtrace *T = traceref_safe(J, 1);
  const BCIns *pc;
  BCIns patched;
  MSize szmcode;

  assert(trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1);
  assert(trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 1);
  assert(trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(T) == 0);
  assert(trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == pt);
  pc = trace_startpc_acq(T);
  assert(pc != NULL);
  expect_loop_geometry(T, pt);
  patched = (BCIns)la_load32_acq((const uint32_t *)pc);
  assert(bc_op(patched) == BC_JLOOP);
  assert((TraceNo)bc_d(patched) == 1);
  assert(proto_trace_acq(pt) == 1);

  assert(trace_spadjust_acq(T) == 0);
  assert((la_load8_acq(&T->unused1) &
	  TRACE_ARM64_INT_LOOP_ADMITTED) != 0);
  assert(trace_mcode_acq(T) != NULL);
  szmcode = trace_szmcode_acq(T);
  assert(szmcode == 168 + (LJ_ABI_BRANCH_TRACK ? sizeof(MCode) : 0));
#if LJ_ABI_BRANCH_TRACK
  assert(trace_mcode_acq(T)[0] == A64I_BTI_J);
#endif
  assert(((uintptr_t)(const void *)trace_mcode_acq(T) &
	  (sizeof(MCode)-1u)) == 0);
  assert((szmcode & (sizeof(MCode)-1u)) == 0);
  assert(trace_mcloop_acq(T) > 0 && trace_mcloop_acq(T) < szmcode);
  assert((trace_mcloop_acq(T) & (sizeof(MCode)-1u)) == 0);
  expect_exit_stub_path(J, T, expect_indirect);
  expect_ir_shape(T);
  expect_snapshot_shape(T);

  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
}

static void expect_single_exit(ExitNo exitno)
{
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == exitno);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == exitno);
}

static void expect_profile_exit_and_reentry(void)
{
  /* Snapshot 5 restores the interrupted loop at its JLOOP site. After owner
  ** profile consumption, that same call re-enters and finishes at snapshot 8.
  ** Preserve both facts so the recovery exit cannot hide the XPOLL exit. */
  assert(lj_trace_test_root_entry_publishes() == 2);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 2);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == 5);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == 8);
}

static void run_postadmission_profile(lua_State *L, jit_State *J, TGState *tg,
	GCproto *pt, int expect_indirect)
{
  global_State *g = G(L);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  void *saved_cframe = L->cframe;
  int32_t saved_vmstate = lj_tg_vmstate_load_acq(tg);
  PostAdmissionPublisher publisher = {
    g, tg, epoch, POSTADMISSION_PROFILE, 0, 0, 0
  };
  pthread_t worker;

  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert((lj_tg_flags_acq(tg) &
	  (TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
		&publisher) == 0);
  call_sum_and_check_cframe(L, 20, 210);
  assert(pthread_join(worker, NULL) == 0);

  assert(la_load32_acq(&publisher.saw_stage) == 1);
  assert(la_load32_acq(&publisher.saw_jit_base) == 1);
  assert(la_load32_acq(&publisher.published) == 1);
  assert(lj_trace_test_root_entry_paused() == 0);
  /* IR_XPOLL inherits the IR_LOOP snapshot. Admission had already completed,
  ** so this exit proves the helper did not catch the late profile request. */
  expect_profile_exit_and_reentry();
  assert(gc2_hs_epoch_acq(g) == epoch);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert((lj_tg_flags_acq(tg) &
	  (TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(L->cframe == saved_cframe);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
  expect_only_root_trace(J, pt, expect_indirect);
}

static void run_postadmission_stopreq(lua_State *L, jit_State *J, TGState *tg,
	GCproto *pt, int expect_indirect)
{
  global_State *g = G(L);
  uint64_t epoch = gc2_hs_epoch_acq(g);
  void *saved_cframe = L->cframe;
  int32_t saved_vmstate = lj_tg_vmstate_load_acq(tg);
  PostAdmissionPublisher publisher = {
    g, tg, epoch, POSTADMISSION_STOPREQ, 0, 0, 0
  };
  pthread_t worker;
  int status;

  clear_stopreq(tg);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(
	LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  assert(pthread_create(&worker, NULL, publish_postadmission_request,
		&publisher) == 0);
  lua_getglobal(L, "__arm64_native_integer_loop");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, 20);
  status = lua_pcall(L, 1, 1, 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == LUA_ERRRUN);
  assert(lua_isstring(L, -1));
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);

  assert(la_load32_acq(&publisher.saw_stage) == 1);
  assert(la_load32_acq(&publisher.saw_jit_base) == 1);
  assert(la_load32_acq(&publisher.published) == 1);
  assert(lj_trace_test_root_entry_paused() == 0);
  /* One admitted entry and XPOLL exit at snapshot 5 prove that the helper's
  ** new stage is after its last request check. vm_exit_interp owns the ACK. */
  expect_single_exit(5);
  assert(gc2_hs_actions_acq(g) == LJ_GC2_HS_STOPREQ);
  assert(gc2_hs_epoch_acq(g) == epoch + 1u);
  assert(lj_tg_hs_epoch_ack_acq(tg) == epoch + 1u);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) != 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ_FRESH) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(L->cframe == saved_cframe);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
  clear_stopreq(tg);
  assert((lj_tg_flags_acq(tg) &
	  (TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);
  expect_only_root_trace(J, pt, expect_indirect);
}

int main(int argc, char **argv)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  TGState *tg;
  GCproto *pt;
  void *saved_cframe;
  int32_t saved_vmstate;
  int expect_indirect = 0;

  assert(argc == 1 || argc == 2);
  if (argc == 2) {
    assert(strcmp(argv[1], "direct") == 0 ||
	   strcmp(argv[1], "indirect") == 0);
    expect_indirect = strcmp(argv[1], "indirect") == 0;
  }

  assert(L != NULL);
  luaL_openlibs(L);
  J = L2J(L);
  tg = L2TG(L);
  assert(J != NULL && tg != NULL);
  assert(lj_tg_load_cur_L(tg) == L);
  assert(lj_tg_load_jit_base(tg) == NULL);
  saved_vmstate = lj_tg_vmstate_load_acq(tg);

  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  saved_cframe = L->cframe;
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=2'); "
    "local function f(n) "
      "local i,x=0,0 "
      "while i<n do i=i+1 x=x+i end "
      "return x "
    "end "
    "__arm64_native_integer_loop=f");

  /* First create and validate the exact constrained root independently of the
  ** lifecycle probes. Their counters start only after admission is proven. */
  call_sum_and_check_cframe(L, 20, 210);
  assert(L->cframe == saved_cframe);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
  pt = global_proto(L, "__arm64_native_integer_loop");
  expect_only_root_trace(J, pt, expect_indirect);

  run_postadmission_profile(L, J, tg, pt, expect_indirect);
  run_postadmission_stopreq(L, J, tg, pt, expect_indirect);

  /* A handled STOPREQ never invalidates the admitted trace. Reset the probe
  ** counters and require the ordinary loop-condition exit on the next call. */
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  call_sum_and_check_cframe(L, 20, 210);
  expect_single_exit(8);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(L->cframe == saved_cframe);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
  expect_only_root_trace(J, pt, expect_indirect);

  {
    const BCIns *pc = trace_startpc_acq(traceref_safe(J, 1));
    BCIns startins = trace_startins_acq(traceref_safe(J, 1));
    GCtrace *after;
    run_lua(L, "jit.flush()");
    assert((BCIns)la_load32_acq((const uint32_t *)pc) == startins);
    assert(bc_op(startins) == BC_LOOP);
    assert(proto_trace_acq(pt) == 0);
    after = traceref_safe(J, 1);
    assert(after == NULL || !trace_runnable_acq(after, 1));
    assert(lj_tg_load_jit_base(tg) == NULL);
  }
  lua_close(L);
  puts("t-arm64-jit-native-loop OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-native-loop SKIP");
  return 0;
}

#endif
