/*
** macOS ARM64 native-entry contract for the first fixed FUNCF root.
** The exact function(a, b) return true end trace is recorded, published as
** JFUNCF and entered through the certified fixed-function header path.
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
#include "lj_asm.h"
#include "lj_bc.h"
#include "lj_buf.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_trace.h"
#include "lj_vm.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-funcf-record requires native ARM64 JFUNCF entry"
#endif

enum {
  FUNCF_R_SEPARATOR = REF_BASE+1,
  FUNCF_R_XPOLL,
  FUNCF_R_SUFFIX,
  FUNCF_R_END
};

typedef struct FuncfWordRace {
  BCIns *word;
  BCIns replacement;
  uint32_t saw_pause;
  uint32_t worker_done;
} FuncfWordRace;

typedef struct FuncfProfileRace {
  TGState *tg;
  uint32_t saw_pause;
  uint32_t saw_jit_base;
  uint32_t worker_done;
} FuncfProfileRace;

typedef struct FuncfStopRace {
  global_State *g;
  TGState *tg;
  uint64_t epoch;
  uint32_t saw_pause;
  uint32_t saw_jit_base;
  uint32_t worker_done;
} FuncfStopRace;

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 FUNCF record chunk failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static GCproto *global_proto(lua_State *L, const char *name)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static GCfunc *global_lfunc(lua_State *L, const char *name)
{
  GCfunc *fn;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  lua_pop(L, 1);
  return fn;
}

static void push_integer_args(lua_State *L, int nargs, lua_Integer value)
{
  int i;
  for (i = 0; i < nargs; i++)
    lua_pushinteger(L, value+i);
}

static void call_expect_true(lua_State *L, const char *name, int nargs)
{
  int status;
  int top = lua_gettop(L);
  void *cframe = L->cframe;
  lua_getglobal(L, name);
  push_integer_args(L, nargs, 40);
  status = lua_pcall(L, nargs, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 FUNCF true call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isboolean(L, -1));
  assert(lua_toboolean(L, -1) != 0);
  lua_pop(L, 1);
  assert(lua_gettop(L) == top);
  assert(L->cframe == cframe);
}

static void expect_exact_true_bytecode(const GCproto *pt, BCIns *startp)
{
  const BCIns *bc = proto_bc(pt);
  BCIns start = (BCIns)la_load32_acq((const uint32_t *)&bc[0]);
  BCIns kpri = (BCIns)la_load32_acq((const uint32_t *)&bc[1]);
  BCIns ret = (BCIns)la_load32_acq((const uint32_t *)&bc[2]);
  BCReg result;

  assert(pt->sizebc == 3);
  assert(pt->numparams == 2);
  assert(pt->framesize == 3);
  assert((pt->flags & PROTO_VARARG) == 0);
  result = (BCReg)(pt->framesize-1u);
  assert(bc_op(start) == BC_FUNCF);
  assert(bc_a(start) == pt->framesize && bc_d(start) == 0);
  assert(bc_op(kpri) == BC_KPRI);
  assert(bc_a(kpri) == result && bc_d(kpri) == 2u);
  assert(bc_op(ret) == BC_RET1);
  assert(bc_a(ret) == result && bc_d(ret) == 2u);
  *startp = start;
}

static int is_exact_true_bytecode(const GCproto *pt)
{
  const BCIns *bc = proto_bc(pt);
  BCIns start, kpri, ret;
  BCReg result;
  if (pt->sizebc != 3 || pt->numparams != 2 || pt->framesize == 0 ||
      (pt->flags & PROTO_VARARG) != 0)
    return 0;
  start = (BCIns)la_load32_acq((const uint32_t *)&bc[0]);
  kpri = (BCIns)la_load32_acq((const uint32_t *)&bc[1]);
  ret = (BCIns)la_load32_acq((const uint32_t *)&bc[2]);
  result = (BCReg)(pt->framesize-1u);
  return bc_op(start) == BC_FUNCF && bc_a(start) == pt->framesize &&
	 bc_d(start) == 0 && bc_op(kpri) == BC_KPRI &&
	 bc_a(kpri) == result && bc_d(kpri) == 2u &&
	 bc_op(ret) == BC_RET1 && bc_a(ret) == result && bc_d(ret) == 2u;
}

static void expect_ir(const IRIns *ir, IRRef ref, IROp op, uint8_t type,
	IRRef op1, IRRef op2)
{
  IRIns ins = ir_load_acq(&ir[ref]);
  assert(ins.o == op);
  assert(ins.t.irt == type);
  assert(ins.op1 == op1);
  assert(ins.op2 == op2);
  assert(ins.s == SPS_NONE);
}

static void expect_exact_snapshots(const GCtrace *T, const GCproto *pt)
{
  const BCIns *bc = proto_bc(pt);
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *map = trace_snapmap_acq(T);
  uint64_t pcbase;
  MSize result_slot = (MSize)pt->framesize+LJ_FR2;

  assert(trace_nsnap_acq(T) == 2);
  assert(trace_nsnapmap_acq(T) == 5);
  assert(snap_ref_acq(&snap[0]) == FUNCF_R_SEPARATOR);
  assert(snap_mapofs_acq(&snap[0]) == 0);
  assert(snap_nent_acq(&snap[0]) == 0);
  assert(snap_nslots_acq(&snap[0]) == result_slot);
  assert(snap_topslot_acq(&snap[0]) == (MSize)pt->framesize);
  memcpy(&pcbase, &map[0], sizeof(pcbase));
  assert((uint8_t)pcbase == 0);
  assert((const BCIns *)(uintptr_t)(pcbase >> 8) == &bc[1]);

  assert(snap_ref_acq(&snap[1]) == FUNCF_R_XPOLL);
  assert(snap_mapofs_acq(&snap[1]) == 2);
  assert(snap_nent_acq(&snap[1]) == 1);
  assert(snap_nslots_acq(&snap[1]) == result_slot+1u);
  assert(snap_topslot_acq(&snap[1]) == (MSize)pt->framesize);
  assert(snapentry_acq(&map[2]) == SNAP(result_slot, 0, REF_TRUE));
  memcpy(&pcbase, &map[3], sizeof(pcbase));
  assert((uint8_t)pcbase == 0);
  assert((const BCIns *)(uintptr_t)(pcbase >> 8) == &bc[2]);
}

static void expect_exact_ir(const GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  assert(trace_nk_acq(T) == REF_TRUE);
  assert(trace_nins_acq(T) == FUNCF_R_END);
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    IRIns k = ir_load_acq(&ir[ref]);
    assert(k.o == IR_KPRI);
    assert(k.t.irt == (uint8_t)(REF_NIL-ref));
    assert(k.op12 == 0);
  }
  expect_ir(ir, REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  expect_ir(ir, FUNCF_R_SEPARATOR, IR_NOP, IRT_NIL, 0, 0);
  expect_ir(ir, FUNCF_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  expect_ir(ir, FUNCF_R_SUFFIX, IR_NOP, IRT_NIL, 0, 0);
  assert(ir_load_acq(&ir[FUNCF_R_SUFFIX]).prev == 0);
}

static void expect_funcf_mcode_tail(jit_State *J, const GCtrace *T,
	int expect_indirect)
{
  MCode *mcode = trace_mcode_acq(T);
  MSize szmcode = trace_szmcode_acq(T);
  MSize nins, i, nadd = 0, nsub = 0;
  MCode *tail;
  MCode add_fixed = (A64I_ADDx^A64I_K12) | A64F_U12(16) |
		    A64F_D(RID_SP) | A64F_N(RID_SP);
  MCode sub_fixed = 0xd10043ffu;
  MCode indirect = A64I_BR_AUTH | A64F_N(RID_LR);

  assert(mcode != NULL && szmcode > 2u*sizeof(MCode));
  assert((szmcode & (sizeof(MCode)-1u)) == 0);
  nins = szmcode / sizeof(MCode);
  tail = &mcode[nins];
  for (i = 0; i < nins; i++) {
    nadd += mcode[i] == add_fixed;
    nsub += mcode[i] == sub_fixed;
  }
  assert(nadd == 1 && nsub == 0);
#if LJ_ABI_BRANCH_TRACK
  assert(mcode[0] == A64I_BTI_J);
#endif
  if (expect_indirect) {
    intptr_t k64ofs =
      (intptr_t)((char *)&J->k64[LJ_K64_VM_EXIT_INTERP] -
		 (char *)&J2GG(J)->g);
    MCode ldr_interp;
    assert(tail[-1] == indirect);
    assert(k64ofs >= 0 && (k64ofs & 7) == 0 &&
	   (k64ofs >> 3) < 4096);
    ldr_interp = A64I_LDRx | A64F_D(RID_LR) | A64F_N(RID_GL) |
		 A64F_U12((uint32_t)(k64ofs >> 3));
    assert(tail[-3] == add_fixed);
    assert(tail[-2] == ldr_interp);
  } else {
    intptr_t bytes = (intptr_t)(uintptr_t)(void *)lj_vm_exit_interp -
		     (intptr_t)(uintptr_t)(void *)&tail[-1];
    assert(tail[-1] != indirect);
    assert((bytes & (intptr_t)(sizeof(MCode)-1u)) == 0);
    assert(tail[-2] == add_fixed);
    assert(tail[-1] == (A64I_B |
	   A64F_S26(bytes/(intptr_t)sizeof(MCode))));
  }
}

static GCtrace *expect_published_true(lua_State *L, GCproto *pt,
	BCIns startins, int expect_indirect)
{
  jit_State *J = L2J(L);
  GCtrace *T = traceref_safe(J, 1);
  const BCIns *bc = proto_bc(pt);
  BCIns live;
  TraceNo traceno;
  uint8_t flags;

  assert(trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1);
  assert(trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 0);
  assert(trace_linktype_acq(T) == LJ_TRLINK_RETURN);
  assert(trace_nchild_acq(T) == 0);
  assert(trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == pt);
  assert(trace_startpc_acq(T) == &bc[0]);
  assert(trace_startins_acq(T) == startins);
  live = (BCIns)la_load32_acq((const uint32_t *)&bc[0]);
  assert(live == BCINS_AD(BC_JFUNCF, bc_a(startins), 1));
  assert(proto_jit_startins_acq(pt, &bc[0]) == startins);

  flags = la_load8_acq(&T->unused1);
  assert(flags == TRACE_ARM64_TRUE_FUNCF_ADMITTED);
  assert((flags & (TRACE_ARM64_INT_LOOP_ADMITTED |
	 TRACE_ARM64_INT_FORL_ADMITTED)) == 0);
  assert(trace_mcode_acq(T) != NULL);
  assert(trace_szmcode_acq(T) > 0);
  assert(trace_mcloop_acq(T) == 0);
  assert(trace_spadjust_acq(T) == 0);
  assert(trace_topslot_acq(T) == (MSize)pt->framesize);
  expect_exact_ir(T);
  expect_exact_snapshots(T, pt);
  expect_funcf_mcode_tail(J, T, expect_indirect);

  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
  return T;
}

static GCtrace *hotcall_until_published(lua_State *L, const char *name)
{
  jit_State *J = L2J(L);
  unsigned i;
  for (i = 0; i < 256; i++) {
    /* Zero supplied arguments makes both fixed parameters genuinely missing
    ** on the C-driven hotcall which eventually starts the recorder. */
    call_expect_true(L, name, 0);
    if (trace_runnable_acq(traceref_safe(J, 1), 1))
      return traceref_safe(J, 1);
  }
  assert(!"fixed true FUNCF root did not publish");
  return NULL;
}

static void expect_native_jfuncf_boundary(lua_State *L,
	uint32_t publishes)
{
  TGState *tg = L->tg_hint;
  assert(tg != NULL);
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  assert(lj_trace_test_root_entry_publishes() == publishes);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
}

static void expect_native_jfuncf_calls(lua_State *L, const char *name)
{
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  call_expect_true(L, name, 0);  /* Both fixed parameters missing. */
  expect_native_jfuncf_boundary(L, 1);
  call_expect_true(L, name, 1);  /* One fixed parameter missing. */
  expect_native_jfuncf_boundary(L, 2);
  call_expect_true(L, name, 2);  /* No fixed parameter missing. */
  expect_native_jfuncf_boundary(L, 3);
  call_expect_true(L, name, 3);  /* One extra argument is ignored. */
  expect_native_jfuncf_boundary(L, 4);
}

static void test_funcf_entry_view_preflight(GCtrace *T, GCproto *pt)
{
  LJArm64PostRAView view;
  BCIns live = (BCIns)la_load32_acq(
	(const uint32_t *)&proto_bc(pt)[0]);

  assert(!lj_asm_arm64_postra_funcf_entry_admit(NULL, live, NULL));
  memset(&view, 0, sizeof(view));
  view.snap = trace_snap_acq(T);
  view.snapmap = trace_snapmap_acq(T);
  view.proto_bc = proto_bc(pt);
  view.nins = trace_nins_acq(T);
  view.nk = trace_nk_acq(T);
  view.nsnap = trace_nsnap_acq(T);
  view.nsnapmap = trace_nsnapmap_acq(T);
  view.spadjust = trace_spadjust_acq(T);
  view.proto_sizebc = pt->sizebc;
  view.root_topslot = trace_topslot_acq(T);
  view.startins = trace_startins_acq(T);
  view.base_delta = 0;
  assert(view.ir == NULL);
  assert(!lj_asm_arm64_postra_funcf_entry_admit(&view, live, NULL));
}

static LJTraceRootEntry invoke_funcf_entry_helper(lua_State *L, GCfunc *fn,
	GCtrace *T)
{
  TGState *tg = L2TG(L);
  TValue saved_func;
  int32_t saved_vmstate = lj_tg_vmstate_load_acq(tg);
  lua_State *saved_tmpbuf_L = sbufL(&tg->tmpbuf);
  const BCIns *pc = trace_startpc_acq(T);
  BCIns live = (BCIns)la_load32_acq((const uint32_t *)pc);
  LJTraceRootEntry entry;

  copyTV(L, &saved_func, L->base-2);
  setfuncV(L, L->base-2, fn);
  assert(curr_func(L) == fn);
  lj_tg_vmstate_store_rel(tg, (int32_t)~LJ_VMST_INTERP);
  entry = lj_trace_enter_root(L2J(L), pc, trace_traceno_acq(T),
	L, L->base, live);
  if (entry.trace != NULL) {
    assert(entry.trace == T && entry.target != NULL);
    assert((uintptr_t)lj_ptr_strip(entry.target) ==
	   (uintptr_t)(void *)trace_mcode_acq(T));
    assert(lj_tg_load_jit_base(tg) == L->base);
    lj_tg_store_jit_base(tg, NULL);
  }
  assert(lj_tg_load_jit_base(tg) == NULL);
  setsbufL(&tg->tmpbuf, saved_tmpbuf_L);
  lj_tg_vmstate_store_rel(tg, saved_vmstate);
  copyTV(L, L->base-2, &saved_func);
  return entry;
}

static void expect_funcf_entry_reject(lua_State *L, GCfunc *fn, GCtrace *T)
{
  LJTraceRootEntry entry;
  lj_trace_test_root_entry_reset();
  entry = invoke_funcf_entry_helper(L, fn, T);
  assert(entry.trace == NULL && entry.target == NULL);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 1);
  assert(lj_trace_test_root_entry_startins_calls() == 0);
}

static void test_funcf_metadata_certificate(lua_State *L, GCfunc *fn,
	GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  uint8_t admission = la_load8_acq(&T->unused1);
  LJTraceRootEntry entry;

  lj_trace_test_root_entry_reset();
  entry = invoke_funcf_entry_helper(L, fn, T);
  assert(entry.trace == T && entry.target != NULL);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);

  trace_link_rel(T, 1);
  expect_funcf_entry_reject(L, fn, T);
  trace_link_rel(T, 0);

  T->linktype = LJ_TRLINK_LOOP;
  expect_funcf_entry_reject(L, fn, T);
  T->linktype = LJ_TRLINK_RETURN;

  T->mcloop = sizeof(MCode);
  expect_funcf_entry_reject(L, fn, T);
  T->mcloop = 0;

  T->spadjust = 16;
  expect_funcf_entry_reject(L, fn, T);
  T->spadjust = 0;

  T->nchild = 1;
  expect_funcf_entry_reject(L, fn, T);
  T->nchild = 0;

  trace_nextside_rel(T, 1);
  expect_funcf_entry_reject(L, fn, T);
  trace_nextside_rel(T, 0);

  T->unused1 = TRACE_ARM64_TRUE_FUNCF_ADMITTED |
		TRACE_ARM64_INT_LOOP_ADMITTED;
  expect_funcf_entry_reject(L, fn, T);
  T->unused1 = 0;
  expect_funcf_entry_reject(L, fn, T);
  T->unused1 = admission;

  ir[FUNCF_R_SUFFIX].s = SPS_FIRST;
  expect_funcf_entry_reject(L, fn, T);
  ir[FUNCF_R_SUFFIX].s = SPS_NONE;

  lj_trace_test_root_entry_reset();
  entry = invoke_funcf_entry_helper(L, fn, T);
  assert(entry.trace == T && entry.target != NULL);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
}

static void wait_for_entry_pause(uint32_t stage, uint32_t *saw_pause)
{
  uint32_t i;
  for (i = 0; i < 10000000u; i++) {
    if (lj_trace_test_root_entry_paused() == stage) {
      la_store32_rel(saw_pause, 1);
      return;
    }
    (void)lj_thr_retry_yield(NULL);
  }
  assert(!"ARM64 JFUNCF helper did not reach requested pause");
}

static void *mutate_funcf_word(void *arg)
{
  FuncfWordRace *race = (FuncfWordRace *)arg;
  wait_for_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA,
	&race->saw_pause);
  bc_publish(race->word, race->replacement);
  lj_trace_test_root_entry_release();
  la_store32_rel(&race->worker_done, 1);
  return NULL;
}

static void test_funcf_generation_race(lua_State *L, GCfunc *fn, GCtrace *T,
	BCIns *word, BCIns replacement)
{
  FuncfWordRace race;
  BCIns original = (BCIns)la_load32_acq((const uint32_t *)word);
  LJTraceRootEntry entry;
  pthread_t worker;

  memset(&race, 0, sizeof(race));
  race.word = word;
  race.replacement = replacement;
  lj_trace_test_root_entry_reset();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTMETADATA);
  assert(pthread_create(&worker, NULL, mutate_funcf_word, &race) == 0);
  entry = invoke_funcf_entry_helper(L, fn, T);
  assert(entry.trace == NULL && entry.target == NULL);
  assert(pthread_join(worker, NULL) == 0);
  assert(la_load32_acq(&race.saw_pause) == 1);
  assert(la_load32_acq(&race.worker_done) == 1);
  assert((BCIns)la_load32_acq((const uint32_t *)word) == replacement);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 1);
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  assert(lj_tg_load_jit_base(L2TG(L)) == NULL);
  bc_publish(word, original);
}

static void *publish_funcf_profile(void *arg)
{
  FuncfProfileRace *race = (FuncfProfileRace *)arg;
  wait_for_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION,
	&race->saw_pause);
  la_store32_rel(&race->saw_jit_base,
	lj_tg_load_jit_base(race->tg) != NULL);
  lj_tg_profile_request_rel(race->tg, 1);
  lj_trace_test_root_entry_release();
  la_store32_rel(&race->worker_done, 1);
  return NULL;
}

static void clear_funcf_stopreq(TGState *tg)
{
  (void)lj_tg_flags_and_rlx(tg,
	(uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
}

static void *publish_funcf_stopreq(void *arg)
{
  FuncfStopRace *race = (FuncfStopRace *)arg;
  wait_for_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION,
	&race->saw_pause);
  assert(gc2_hs_epoch_acq(race->g) == race->epoch);
  assert(lj_tg_hs_epoch_ack_acq(race->tg) == race->epoch);
  assert(gc2_hs_pending_acq(race->g) == 0);
  assert(lj_tg_reqmask_acq(race->tg) == 0);
  assert(lj_tg_poll_acq(race->tg) == 0);
  assert(lj_tg_profile_request_acq(race->tg) == 0);
  la_store32_rel(&race->saw_jit_base,
	lj_tg_load_jit_base(race->tg) != NULL);
  assert(la_load32_acq(&race->saw_jit_base) == 1);
  gc2_hs_actions_rel(race->g, LJ_GC2_HS_STOPREQ);
  gc2_hs_pending_rel(race->g, 1);
  gc2_hs_epoch_rel(race->g, race->epoch+1u);
  lj_tg_reqmask_rel(race->tg, LJ_GC2_HS_STOPREQ);
  lj_tg_poll_rel(race->tg, 1);
  lj_trace_test_root_entry_release();
  la_store32_rel(&race->worker_done, 1);
  return NULL;
}

static void test_funcf_native_xpoll(lua_State *L, const char *name,
	GCtrace *T)
{
  TGState *tg = L2TG(L);
  global_State *g = G(L);
  FuncfProfileRace race;
  pthread_t worker;
  TValue *saved_base = L->base;
  void *saved_cframe = L->cframe;
  int saved_top = lua_gettop(L);
  int32_t saved_vmstate = lj_tg_vmstate_load_acq(tg);
  uint64_t saved_epoch = gc2_hs_epoch_acq(g);

  memset(&race, 0, sizeof(race));
  race.tg = tg;
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  assert(pthread_create(&worker, NULL, publish_funcf_profile, &race) == 0);
  call_expect_true(L, name, 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(la_load32_acq(&race.saw_pause) == 1);
  assert(la_load32_acq(&race.saw_jit_base) == 1);
  assert(la_load32_acq(&race.worker_done) == 1);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == 1);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == 1);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(gc2_hs_epoch_acq(g) == saved_epoch);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
  assert(L->base == saved_base);
  assert(L->cframe == saved_cframe);
  assert(lua_gettop(L) == saved_top);
  assert(trace_runnable_acq(T, 1));
}

static void test_funcf_native_stopreq(lua_State *L, const char *name,
	GCtrace *T)
{
  TGState *tg = L2TG(L);
  global_State *g = G(L);
  FuncfStopRace race;
  pthread_t worker;
  TValue *saved_base = L->base;
  void *saved_cframe = L->cframe;
  int saved_top = lua_gettop(L);
  int32_t saved_vmstate = lj_tg_vmstate_load_acq(tg);
  uint64_t saved_epoch = gc2_hs_epoch_acq(g);
  int status;

  memset(&race, 0, sizeof(race));
  race.g = g;
  race.tg = tg;
  race.epoch = saved_epoch;
  clear_funcf_stopreq(tg);
  assert(lj_tg_hs_epoch_ack_acq(tg) == saved_epoch);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  lj_trace_test_root_entry_pause(LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION);
  assert(pthread_create(&worker, NULL, publish_funcf_stopreq, &race) == 0);
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  status = lua_pcall(L, 0, 1, 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(status == LUA_ERRRUN);
  assert(lua_isstring(L, -1));
  assert(strstr(lua_tostring(L, -1),
	"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);

  assert(la_load32_acq(&race.saw_pause) == 1);
  assert(la_load32_acq(&race.saw_jit_base) == 1);
  assert(la_load32_acq(&race.worker_done) == 1);
  assert(lj_trace_test_root_entry_publishes() == 1);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == 1);
  assert(lj_trace_test_first_exitno() == 1);
  assert(lj_trace_test_last_exit_parent() == 1);
  assert(lj_trace_test_last_exitno() == 1);
  assert(gc2_hs_actions_acq(g) == LJ_GC2_HS_STOPREQ);
  assert(gc2_hs_epoch_acq(g) == saved_epoch+1u);
  assert(lj_tg_hs_epoch_ack_acq(tg) == saved_epoch+1u);
  assert(gc2_hs_leader_acq(g) == 0);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_profile_request_acq(tg) == 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ) != 0);
  assert((lj_tg_flags_acq(tg) & TGF_STOPREQ_FRESH) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
  assert(L->base == saved_base);
  assert(L->cframe == saved_cframe);
  assert(lua_gettop(L) == saved_top);
  assert(trace_runnable_acq(T, 1));
  clear_funcf_stopreq(tg);
  assert((lj_tg_flags_acq(tg) &
	(TGF_STOPREQ|TGF_STOPREQ_FRESH)) == 0);
}

static void run_positive(int expect_indirect)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  GCfunc *fn;
  GCproto *pt;
  GCtrace *T;
  const BCIns *startpc;
  BCIns startins;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=4')\n"
    "function __arm64_funcf_true(a, b) return true end\n");
  J = L2J(L);
  fn = global_lfunc(L, "__arm64_funcf_true");
  pt = global_proto(L, "__arm64_funcf_true");
  expect_exact_true_bytecode(pt, &startins);
  startpc = proto_bc(pt);
  assert(!trace_runnable_acq(traceref_safe(J, 1), 1));

  T = hotcall_until_published(L, "__arm64_funcf_true");
  assert(T == expect_published_true(L, pt, startins, expect_indirect));
  test_funcf_entry_view_preflight(T, pt);
  expect_native_jfuncf_calls(L, "__arm64_funcf_true");
  test_funcf_metadata_certificate(L, fn, T);
  test_funcf_generation_race(L, fn, T, (BCIns *)&proto_bc(pt)[0],
	BCINS_AD(BC_JFUNCF, (BCReg)(bc_a(startins)+1u), 1));
  test_funcf_generation_race(L, fn, T, (BCIns *)&proto_bc(pt)[1],
	BCINS_AD(BC_KPRI, (BCReg)(pt->framesize-1u), 1));
  test_funcf_generation_race(L, fn, T, (BCIns *)&proto_bc(pt)[2],
	BCINS_AD(BC_RET0, (BCReg)(pt->framesize-1u), 1));
  test_funcf_native_xpoll(L, "__arm64_funcf_true", T);
  assert(T == expect_published_true(L, pt, startins, expect_indirect));
  test_funcf_native_stopreq(L, "__arm64_funcf_true", T);
  assert(T == expect_published_true(L, pt, startins, expect_indirect));
  expect_native_jfuncf_calls(L, "__arm64_funcf_true");

  /* jit.flush restores bytecode/slots; resetting the public hotloop parameter
  ** publishes a fresh owner-TG hotcount generation for deterministic reuse. */
  run_lua(L, "jit.flush(); jit.opt.start('hotloop=1')\n");
  assert((BCIns)la_load32_acq((const uint32_t *)startpc) == startins);
  assert(proto_jit_startins_acq(pt, startpc) == startins);
  assert(!trace_runnable_acq(traceref_safe(J, 1), 1));
  assert(lj_tg_load_jit_base(L->tg_hint) == NULL);

  /* Full flush returns trace number one to the allocator. Republish the same
  ** immutable generation and re-prove native JFUNCF entry. */
  T = hotcall_until_published(L, "__arm64_funcf_true");
  assert(trace_traceno_acq(T) == 1);
  assert(trace_startpc_acq(T) == startpc);
  assert(T == expect_published_true(L, pt, startins, expect_indirect));
  expect_native_jfuncf_calls(L, "__arm64_funcf_true");
  lua_close(L);
}

typedef enum NegativeKind {
  NEG_FALSE,
  NEG_NIL,
  NEG_NUMBER,
  NEG_NONTRIVIAL
} NegativeKind;

static void call_negative(lua_State *L, const char *name, NegativeKind kind)
{
  int status;
  int nargs = kind == NEG_NONTRIVIAL ? 2 : 0;
  int top = lua_gettop(L);
  void *cframe = L->cframe;
  lua_getglobal(L, name);
  if (kind == NEG_NONTRIVIAL) {
    lua_pushinteger(L, 17);
    lua_pushinteger(L, 17);
  }
  status = lua_pcall(L, nargs, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 FUNCF negative call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  switch (kind) {
  case NEG_FALSE:
    assert(lua_isboolean(L, -1) && lua_toboolean(L, -1) == 0);
    break;
  case NEG_NIL:
    assert(lua_isnil(L, -1));
    break;
  case NEG_NUMBER:
    assert(lua_isnumber(L, -1) && lua_tointeger(L, -1) == 42);
    break;
  case NEG_NONTRIVIAL:
    assert(lua_isboolean(L, -1) && lua_toboolean(L, -1) != 0);
    break;
  }
  lua_pop(L, 1);
  assert(lua_gettop(L) == top);
  assert(L->cframe == cframe);
}

static void run_rejection(const char *name, const char *definition,
	NegativeKind kind)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  GCproto *pt;
  unsigned i;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.flush(); jit.on()\n"
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=4')\n");
  run_lua(L, definition);
  J = L2J(L);
  pt = global_proto(L, name);
  assert(!is_exact_true_bytecode(pt));
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 256; i++)
    call_negative(L, name, kind);
  assert(!trace_runnable_acq(traceref_safe(J, 1), 1));
  assert(bc_op((BCIns)la_load32_acq(
	(const uint32_t *)proto_bc(pt))) != BC_JFUNCF);
  assert(lj_trace_test_root_entry_publishes() == 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 0);
  assert(lj_tg_load_jit_base(L->tg_hint) == NULL);
  lua_close(L);
}

static void run_rejections(void)
{
  run_rejection("__arm64_funcf_false",
    "function __arm64_funcf_false(a, b) return false end\n", NEG_FALSE);
  run_rejection("__arm64_funcf_nil",
    "function __arm64_funcf_nil(a, b) return nil end\n", NEG_NIL);
  run_rejection("__arm64_funcf_number",
    "function __arm64_funcf_number(a, b) return 42 end\n", NEG_NUMBER);
  run_rejection("__arm64_funcf_nontrivial",
    "function __arm64_funcf_nontrivial(a, b) return a == b end\n",
    NEG_NONTRIVIAL);
}

int main(int argc, char **argv)
{
  int expect_indirect;
  assert(argc == 2);
  assert(strcmp(argv[1], "direct") == 0 ||
	 strcmp(argv[1], "indirect") == 0);
  expect_indirect = strcmp(argv[1], "indirect") == 0;
  run_positive(expect_indirect);
  run_rejections();
  puts("arm64_jit_funcf_record OK: exact true FUNCF published and entered natively");
  return 0;
}

#else

int main(void)
{
  puts("arm64_jit_funcf_record SKIP: requires experimental macOS ARM64 JIT");
  return 0;
}

#endif
