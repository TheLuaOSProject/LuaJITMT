/*
** Test-only native consumption proof for the exact first ARM64 side trace.
** The dedicated build admits one parent-1/exit-2 recording attempt, assembles
** it completely, records bounded evidence, and then aborts before trace_stop.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_TRACE_TEST_HELPERS) && defined(LJ_ARM64_SIDE_ASM_TEST)

#include "lj_obj.h"
#include "lj_asm.h"
#include "lj_atomic.h"
#include "lj_bc.h"
#include "lj_func.h"
#include "lj_gc2.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"

#if !LJ_TARGET_OSX || !LJ_TARGET_ARM64 || !LJ_HASJIT || \
    LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED != 1 || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED
#error "side assembler probe requires admitted roots and closed ARM64 sides"
#endif

enum {
  PROBE_PARENT = 1,
  PROBE_CHILD = 2,
  PROBE_EXIT = 2,
  PROBE_CONTINUATION_POS = 13
};

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 side assembler setup failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static lua_Integer call_probe(lua_State *L, lua_Integer n, lua_Integer bias)
{
  lua_Integer result;
  int status;
  lua_getglobal(L, "__arm64_side_asm_probe");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  lua_pushinteger(L, bias);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 side assembler call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static GCproto *probe_proto(lua_State *L)
{
  GCfunc *fn;
  GCproto *pt;
  lua_getglobal(L, "__arm64_side_asm_probe");
  assert(lua_isfunction(L, -1));
  fn = funcV(L->top-1);
  assert(isluafunc(fn));
  pt = funcproto(fn);
  lua_pop(L, 1);
  return pt;
}

static BCIns loadbc(const BCIns *pc)
{
  return (BCIns)la_load32_acq((const uint32_t *)pc);
}

static const BCIns *selected_continuation(const GCtrace *T, ExitNo exitno)
{
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *map = trace_snapmap_acq(T);
  SnapShot *selected;
  assert(snap != NULL && map != NULL && exitno < trace_nsnap_acq(T));
  selected = &snap[exitno];
  return snap_pc_acq(&map[snap_mapofs_acq(selected) +
			  snap_nent_acq(selected)]);
}

static int selected_map_has_slot(const GCtrace *T, ExitNo exitno,
	BCReg wanted)
{
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *map = trace_snapmap_acq(T);
  SnapShot *selected;
  MSize i;
  assert(snap != NULL && map != NULL && exitno < trace_nsnap_acq(T));
  selected = &snap[exitno];
  map += snap_mapofs_acq(selected);
  for (i = 0; i < snap_nent_acq(selected); i++)
    if (snap_slot(snapentry_acq(&map[i])) == wanted)
      return 1;
  return 0;
}

static int side_parent_cert_zero(const LJTraceArm64SideParentCert *cert)
{
  return cert->body == NULL && cert->mcode == NULL &&
	 cert->continuation == NULL && cert->continuationins == 0 &&
	 cert->parent == 0 && cert->exitno == 0;
}

static void expect_root_shape(jit_State *J, GCproto *pt, GCtrace **rootp,
	const BCIns **continuationp)
{
  GCtrace *root = traceref_safe(J, PROBE_PARENT);
  const BCIns *startpc;
  const BCIns *continuation;
  BCIns live;

  assert(pt != NULL && pt->sizebc == 19 && pt->numparams == 2 &&
	 pt->framesize == 5);
  assert(root != NULL && trace_runnable_acq(root, PROBE_PARENT));
  assert(trace_traceno_acq(root) == PROBE_PARENT);
  assert(trace_root_acq(root) == 0);
  assert(trace_link_acq(root) == PROBE_PARENT);
  assert(trace_linktype_acq(root) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(root) == 0);
  assert(trace_nextside_acq(root) == 0);
  assert(trace_startpt_acq(root) == pt);
  assert(trace_topslot_acq(root) == 5 && trace_spadjust_acq(root) == 0);
  assert(trace_mcode_acq(root) != NULL);
  assert((la_load8_acq(&root->unused1) &
	  TRACE_ARM64_INT_LOOP_ADMITTED) != 0);
  assert((la_load8_acq(&root->unused1) &
	  TRACE_ARM64_INT_SIDE_ADMITTED) == 0);

  startpc = trace_startpc_acq(root);
  assert(startpc != NULL && startpc >= proto_bc(pt) &&
	 startpc < proto_bc(pt)+pt->sizebc);
  assert(bc_op(trace_startins_acq(root)) == BC_LOOP);
  live = loadbc(startpc);
  assert(bc_op(live) == BC_JLOOP && bc_d(live) == PROBE_PARENT);
  assert(proto_trace_acq(pt) == PROBE_PARENT);

  assert(trace_nsnap_acq(root) == 8 && PROBE_EXIT < trace_nsnap_acq(root));
  assert(trace_exittab_nslots_acq(root) == trace_nsnap_acq(root));
  assert(selected_map_has_slot(root, PROBE_EXIT, 4));
  continuation = selected_continuation(root, PROBE_EXIT);
  assert(continuation == proto_bc(pt)+PROBE_CONTINUATION_POS);
  assert(bc_op(loadbc(continuation)) == BC_CGET);

  *rootp = root;
  *continuationp = continuation;
}

static void dump_probe(const LJArm64SideAsmProbe *probe)
{
  fprintf(stderr,
    "side assembler probe stages=%#x captures=%u parent=%u child=%u "
    "exit=%u mapn=%u entry_words=%u tail_pc=%p tail=%#x marker=%#x\n",
    (unsigned)probe->stages, (unsigned)probe->capture_count,
    (unsigned)probe->parent, (unsigned)probe->child,
    (unsigned)probe->exitno, (unsigned)probe->parentmap_n,
    (unsigned)probe->entry_words, (void *)probe->tail_pc,
    (unsigned)probe->tail_ins, (unsigned)probe->marker);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  global_State *g;
  TGState *tg;
  GCproto *pt;
  GCtrace *root;
  GCtrace *slot2;
  SnapShot *root_snap;
  const BCIns *root_pc;
  const BCIns *continuation;
  BCIns root_jloop;
  BCIns continuationins;
  MCode *root_mcode;
  MCode *root_fallback;
  MCode *root_exit_target;
  MCode *mctop;
  SnapNo root_nsnap;
  MSize root_nchild;
  TraceNo root_nextside;
  MSize snapcount_before;
  MCode expected_tail;
  LJArm64SideAsmProbe probe;
  void *saved_cframe;
  int32_t saved_vmstate;
  int done;

  assert(L != NULL);
  luaL_openlibs(L);
  J = L2J(L);
  g = G(L);
  tg = L2TG(L);
  assert(J != NULL && g != NULL && tg != NULL);
  saved_cframe = L->cframe;
  saved_vmstate = lj_tg_vmstate_load_acq(tg);

  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=3'); "
    "local function f(n, bias) "
      "local i=0 "
      "while i<n do "
        "i=i+1 "
        "if bias~=0 then i=i+1 end "
      "end "
      "return i "
    "end "
    "__arm64_side_asm_probe=f");

  /* One bias-zero invocation publishes the independently admitted root 1. */
  assert(call_probe(L, 3, 0) == 3);
  pt = probe_proto(L);
  expect_root_shape(J, pt, &root, &continuation);

  root_pc = trace_startpc_acq(root);
  root_jloop = loadbc(root_pc);
  continuationins = loadbc(continuation);
  root_mcode = trace_mcode_acq(root);
  root_snap = trace_snap_acq(root);
  root_nsnap = trace_nsnap_acq(root);
  root_nchild = trace_nchild_acq(root);
  root_nextside = trace_nextside_acq(root);
  root_fallback = exitstub_trace_fallback_addr_(trace_exitstub_acq(root));
  root_exit_target = trace_exittarget_arm64_acq(root, PROBE_EXIT);
  assert(root_exit_target == root_fallback);
  snapcount_before = snap_count_acq(&root_snap[PROBE_EXIT]);
  assert(snapcount_before < SNAPCOUNT_DONE-1u);
  mctop = J->mctop;

  /* The first assembler attempt is forced through MCODELM. Its abort clears
  ** the certificate, and the retry must run semantic admission and capture
  ** again before the second exit-table allocation. */
  lj_trace_test_reset_exittab_stats();
  lj_trace_test_reset_exit_stats();
  lj_asm_arm64_test_side_probe_arm(PROBE_PARENT, PROBE_EXIT);
  lj_asm_arm64_test_force_exitstub_mcode_retry(1);
  assert(call_probe(L, 3, 1) == 4);

  memset(&probe, 0, sizeof(probe));
  done = lj_asm_arm64_test_side_probe_read(&probe);
  if (!done) {
    dump_probe(&probe);
    fprintf(stderr,
      "side exits calls=%u first=%u/%u last=%u/%u snap2=%u before=%u\n",
      (unsigned)lj_trace_test_exit_calls(),
      (unsigned)lj_trace_test_first_exit_parent(),
      (unsigned)lj_trace_test_first_exitno(),
      (unsigned)lj_trace_test_last_exit_parent(),
      (unsigned)lj_trace_test_last_exitno(),
      (unsigned)snap_count_acq(&root_snap[PROBE_EXIT]),
      (unsigned)snapcount_before);
  }
  assert(done);
  assert(probe.stages == LJ_ARM64_SIDE_ASM_PROBE_ALL);
  assert(probe.capture_count == 2);
  assert(probe.parent == PROBE_PARENT);
  assert(probe.child == PROBE_CHILD);
  assert(probe.exitno == PROBE_EXIT);

  /* All six stored certificate identities come from the real parent. */
  assert(probe.cert_body == root);
  assert(probe.cert_mcode == root_mcode);
  assert(probe.cert_continuation == continuation);
  assert(probe.cert_continuationins == continuationins);
  assert(probe.cert_continuationins == loadbc(continuation));

  /* The generic parent-map builder and the real head shuffle consumed the
  ** sole inherited slot from x28 and emitted the canonical x28 -> x27 move. */
  assert(probe.parentmap_n == 1);
  assert(probe.parentmap0 == REGSP(RID_X28, SPS_NONE));
  assert(probe.branch_track == (uint8_t)LJ_ABI_BRANCH_TRACK);
  assert(probe.entry_words > (MSize)LJ_ABI_BRANCH_TRACK);
#if LJ_ABI_BRANCH_TRACK
  assert(probe.entry[0] == A64I_LE(A64I_BTI_J));
#endif
  assert(probe.entry[LJ_ABI_BRANCH_TRACK] ==
	 A64I_LE(A64I_MOVx | A64F_D(RID_X27) | A64F_M(RID_X28)));

  /* Tail evidence is the instruction actually written by asm_tail_fixup,
  ** independently recomputed from its certified raw parent mcode target. */
  assert(probe.tail_target == root_mcode);
  assert(probe.tail_pc != NULL &&
	 ((uintptr_t)(void *)probe.tail_pc & (sizeof(MCode)-1u)) == 0);
  assert(lj_asm_arm64_b26_encode(
	(uintptr_t)(void *)probe.tail_pc,
	(uintptr_t)(void *)probe.tail_target, &expected_tail));
  assert(probe.tail_ins == expected_tail);
  assert(probe.marker == TRACE_ARM64_INT_SIDE_ADMITTED);

  assert(lj_trace_test_mcode_retries() == 1);
  assert(lj_trace_test_abort_count() == 2);
  assert(lj_trace_test_last_abort_error() == LJ_TRERR_NYIIR);
  assert(lj_trace_test_exittab_allocs() == 2);
  assert(lj_trace_test_exittab_frees() == 2);
  assert(lj_trace_test_exittab_last_alloc_slots() == 5);
  assert(lj_trace_test_exittab_last_free_slots() == 5);

  /* The private side was discarded before trace_stop/publication. */
  slot2 = traceref_safe(J, PROBE_CHILD);
  assert(slot2 == NULL);
  assert(J->curfinal == NULL);
  assert(trace_exittab_acq(&J->cur) == NULL);
  assert(trace_exitstub_acq(&J->cur) == NULL);
  assert(trace_traceno_acq(&J->cur) == 0);
  assert(L->cframe == saved_cframe);
  assert(lj_tg_vmstate_load_acq(tg) == saved_vmstate);
  assert(lj_tg_load_jit_base(tg) == NULL);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  assert(jit_token_acq(g) == 0);
  assert(jit_owner_l_acq(J) == NULL);
  assert(side_parent_cert_zero(&J->arm64_side_parent));
  assert(gc2_smr_readers_acq(g) == 0);
  assert(J->mctop == mctop);

  /* Parent topology, exit target, selected generation and native entry are
  ** unchanged. The one ordinary hot-exit claim may only advance count by 1. */
  assert(traceref_safe(J, PROBE_PARENT) == root);
  assert(trace_runnable_acq(root, PROBE_PARENT));
  assert(trace_root_acq(root) == 0);
  assert(trace_link_acq(root) == PROBE_PARENT);
  assert(trace_linktype_acq(root) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(root) == root_nchild && root_nchild == 0);
  assert(trace_nextside_acq(root) == root_nextside && root_nextside == 0);
  assert(trace_snap_acq(root) == root_snap);
  assert(trace_nsnap_acq(root) == root_nsnap);
  assert(snap_count_acq(&root_snap[PROBE_EXIT]) == snapcount_before+1u);
  assert(snap_count_acq(&root_snap[PROBE_EXIT]) != SNAPCOUNT_DONE);
  assert(trace_exittarget_arm64_acq(root, PROBE_EXIT) == root_exit_target);
  assert(trace_exittarget_arm64_acq(root, PROBE_EXIT) == root_fallback);
  assert(trace_mcode_acq(root) == root_mcode);
  assert(trace_startpc_acq(root) == root_pc);
  assert(loadbc(root_pc) == root_jloop);
  assert(proto_trace_acq(pt) == PROBE_PARENT);

  /* Root native execution remains available after the private side abort. */
  assert(call_probe(L, 3, 0) == 3);
  assert(traceref_safe(J, PROBE_PARENT) == root);
  assert(trace_runnable_acq(root, PROBE_PARENT));
  assert(trace_mcode_acq(root) == root_mcode);
  assert(loadbc(root_pc) == root_jloop);
  assert(gc2_smr_readers_acq(g) == 0);

  lua_close(L);
  puts("t-arm64-jit-side-asm-consumption OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-jit-side-asm-consumption SKIP");
  return 0;
}

#endif
