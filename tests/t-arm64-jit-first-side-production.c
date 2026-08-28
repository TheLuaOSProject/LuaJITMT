/*
** Ordinary-production proof for the exact first ARM64 side trace.
**
** This fixture is intentionally built without either ARM64 side test seam.
** The no-helper build is an embedded-Lua smoke test.  The helper build adds
** white-box checks for four independently recorded prototype/root/child pairs,
** native entry, authenticated exit-table representation, and retirement.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_arch.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)

#if !LJ_TARGET_OSX || !LJ_TARGET_ARM64 || !LJ_HASJIT || \
    LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED != 0 || \
    LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED != 1 || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED
#error "production first-side probe requires admitted roots/first sides"
#endif

#if defined(LJ_ARM64_FIRST_SIDE_PUBLISH_TEST) || \
    defined(LJ_ARM64_SIDE_ASM_TEST)
#error "production first-side probe must not use an ARM64 side test seam"
#endif

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 production first-side Lua failure: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static lua_Integer call_named(lua_State *L, const char *name,
	lua_Integer n, lua_Integer bias)
{
  lua_Integer result;
  int status;
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  lua_pushinteger(L, bias);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 production first-side call %s failed: %s\n",
	    name, lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return result;
}

static void define_probes(lua_State *L)
{
  run_lua(L,
    "jit.flush(); jit.on(); "
    "jit.opt.start('hotloop=1','hotexit=1','maxtrace=16'); "
    "local function decoy(n, bias) "
      "local i=0 "
      "while i<n do "
        "i=i+1 "
        "if bias~=0 then i=i+1 end "
      "end "
      "return i "
    "end "
    "local function first(n, bias) "
      "local i=0 "
      "while i<n do "
        "i=i+1 "
        "if bias~=0 then i=i+1 end "
      "end "
      "return i "
    "end "
    "local function second(n, bias) "
      "local i=0 "
      "while i<n do "
        "i=(i~=0 and i or i)+1 "
      "end "
      "return i "
    "end "
    "local function third(n, bias) "
      "local i=0 "
      "while i<n do "
        "if i==0 then i=i+1 end "
        "i=i+1 "
      "end "
      "return i "
    "end "
    "local function fourth(n, bias) "
      "local i=0 "
      "while i<n do "
        "i=(i~=0 and i or i)+2 "
      "end "
      "return i "
    "end "
    "local function unsupported(n, bias) "
      "local i=0 "
      "while i<n do "
        "i=(i~=0 and i or i)+3 "
      "end "
      "return i "
    "end "
    "__arm64_first_side_production_decoy=decoy; "
    "__arm64_first_side_production_first=first; "
    "__arm64_first_side_production_second=second; "
    "__arm64_first_side_production_third=third; "
    "__arm64_first_side_production_fourth=fourth; "
    "__arm64_first_side_production_unsupported=unsupported");
}

enum { PRODUCTION_ROOT_ATTEMPTS = 64 };

#ifndef LJ_TRACE_TEST_HELPERS

static int smoke_main(void)
{
  lua_State *L = luaL_newstate();
  unsigned attempt;
  assert(L != NULL);
  luaL_openlibs(L);
  define_probes(L);

  /* Run the two same-bytecode exit-6 variants before unrelated hot-count
  ** slots can collide. The +2 root is recorded with n=3 and its admitted
  ** root-linked child with n=5; repeated +3 calls retain only the root. */
  assert(call_named(L, "__arm64_first_side_production_fourth", 3, 0) == 4);
  assert(call_named(L, "__arm64_first_side_production_fourth", 3, 0) == 4);
  assert(call_named(L, "__arm64_first_side_production_fourth", 5, 0) == 6);
  assert(call_named(L, "__arm64_first_side_production_fourth", 5, 0) == 6);
  assert(call_named(L, "__arm64_first_side_production_fourth", 5, 0) == 6);
  assert(call_named(L, "__arm64_first_side_production_fourth", 5, 0) == 6);
  for (attempt = 0; attempt < PRODUCTION_ROOT_ATTEMPTS; attempt++)
    assert(call_named(L, "__arm64_first_side_production_unsupported",
	4, 0) == 6);

  assert(call_named(L, "__arm64_first_side_production_decoy", 3, 0) == 3);
  assert(call_named(L, "__arm64_first_side_production_first", 3, 0) == 3);
  assert(call_named(L, "__arm64_first_side_production_first", 3, 1) == 4);
  assert(call_named(L, "__arm64_first_side_production_first", 3, 1) == 4);
  assert(call_named(L, "__arm64_first_side_production_second", 3, 0) == 3);
  assert(call_named(L, "__arm64_first_side_production_second", 3, 0) == 3);
  assert(call_named(L, "__arm64_first_side_production_second", 2, 0) == 2);
  assert(call_named(L, "__arm64_first_side_production_third", 3, 0) == 3);
  assert(call_named(L, "__arm64_first_side_production_third", 4, 0) == 4);
  assert(call_named(L, "__arm64_first_side_production_third", 4, 0) == 4);
  assert(call_named(L, "__arm64_first_side_production_third", 3, 0) == 3);
  run_lua(L,
    "local util=require('jit.util'); local live, roots, sides=0,0,0; "
    "for tr=1,32 do local i=util.traceinfo(tr); if i then "
      "live=live+1; "
      "if i.linktype=='loop' then roots=roots+1 "
      "elseif i.linktype=='root' then sides=sides+1 end "
    "end end; "
    "assert(live==10, 'expected ten production traces, got '..live); "
    "assert(roots==6, 'expected six roots, got '..roots); "
    "assert(sides==4, 'expected four first sides, got '..sides)");

  lua_close(L);
  puts("t-arm64-jit-first-side-production smoke OK");
  return 0;
}

#else

#include "lj_arena.h"
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

enum {
  PRODUCTION_PAIR_COUNT = 4,
  PRODUCTION_CHILD_EXIT = 3,
  PRODUCTION_TOPSLOT = 5,
  PRODUCTION_CHILD_NSNAP = 5,
  PRODUCTION_CHILD_NSNAPMAP = 17,
  PRODUCTION_CHILD_K_ADDEND = REF_TRUE-1u,
  PRODUCTION_CHILD_R_PARENT = REF_BASE+1u,
  PRODUCTION_CHILD_R_CGET = REF_BASE+2u,
  PRODUCTION_CHILD_R_ADD = REF_BASE+3u,
  PRODUCTION_CHILD_R_LIMIT = REF_BASE+4u,
  PRODUCTION_CHILD_R_GT = REF_BASE+5u,
  PRODUCTION_CHILD_R_XPOLL = REF_BASE+6u,
  PRODUCTION_CHILD_SEMANTIC_NINS = REF_BASE+7u
};

typedef struct ProductionPair {
  const char *name;
  lua_Integer root_n;
  lua_Integer root_bias;
  lua_Integer root_result;
  lua_Integer side_n;
  lua_Integer side_bias;
  lua_Integer side_result;
  lua_Integer native_n;
  lua_Integer native_bias;
  lua_Integer native_result;
  ExitNo exitno;
  MSize root_nsnap;
  MSize continuation_pos;
  MSize child_pcpos[PRODUCTION_CHILD_NSNAP];
  Reg inherited_reg;
  Reg sload_reg;
  int32_t addend;
  GCproto *pt;
  TraceNo rootno;
  TraceNo childno;
  GCtrace *root;
  GCtrace *child;
  const BCIns *continuation;
  SnapShot *root_snap;
  MCode **root_exittab;
  MCode *root_fallback;
  MCode *child_mcode;
} ProductionPair;

static GCproto *named_proto(lua_State *L, const char *name)
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

static BCIns loadbc(const BCIns *pc)
{
  return (BCIns)la_load32_acq((const uint32_t *)pc);
}

static uintptr_t pointer_bits(void *target)
{
  uintptr_t bits;
  _Static_assert(sizeof(bits) == sizeof(target), "pointer width mismatch");
  memcpy(&bits, &target, sizeof(bits));
  return bits;
}

#if LJ_ABI_PAUTH
static uintptr_t function_bits(ASMFunction target)
{
  uintptr_t bits;
  _Static_assert(sizeof(bits) == sizeof(target), "function width mismatch");
  memcpy(&bits, &target, sizeof(bits));
  return bits;
}
#endif

static GCtrace *find_root(jit_State *J, GCproto *pt, TraceNo *tracenop)
{
  TraceNo traceno;
  GCtrace *found = NULL;
  TraceNo foundno = 0;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref_safe(J, traceno);
    if (trace_runnable_acq(T, traceno) && trace_startpt_acq(T) == pt &&
	trace_root_acq(T) == 0 &&
	trace_linktype_acq(T) == LJ_TRLINK_LOOP) {
      assert(found == NULL);
      found = T;
      foundno = traceno;
    }
  }
  if (tracenop != NULL)
    *tracenop = foundno;
  return found;
}

static uint32_t live_trace_count(jit_State *J)
{
  TraceNo traceno;
  uint32_t count = 0;
  for (traceno = 1; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    if (trace_runnable_acq(traceref_safe(J, traceno), traceno))
      count++;
  return count;
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
  return cert->tracev == NULL && cert->body == NULL && cert->mcode == NULL &&
	 cert->continuation == NULL && cert->continuationins == 0 &&
	 cert->parent == 0 && cert->exitno == 0 && cert->child == 0;
}

static void expect_edge(global_State *g, const ProductionPair *pair,
	MCode *target)
{
  void *raw = la_loadptr_acq(
	(void *const *)&pair->root_exittab[pair->exitno]);
  void *encoded = trace_exittarget_arm64_encode(g, target);
  assert(pointer_bits(raw) == pointer_bits(encoded));
  assert(trace_exittarget_arm64_acq(pair->root, pair->exitno) == target);
}

static void record_root(lua_State *L, jit_State *J, ProductionPair *pair)
{
  unsigned attempt;
  pair->pt = named_proto(L, pair->name);
  for (attempt = 0; attempt < PRODUCTION_ROOT_ATTEMPTS; attempt++) {
    assert(call_named(L, pair->name, pair->root_n, pair->root_bias) ==
	   pair->root_result);
    pair->root = find_root(J, pair->pt, &pair->rootno);
    if (pair->root != NULL)
      break;
  }
  assert(pair->root != NULL && pair->rootno != 0);
}

static void expect_root_shape(jit_State *J, global_State *g,
	ProductionPair *pair)
{
  const BCIns *startpc;
  BCIns live;
  assert(pair->pt != NULL && pair->pt->sizebc == 19 &&
	 pair->pt->numparams == 2 &&
	 pair->pt->framesize == PRODUCTION_TOPSLOT);
  assert(trace_runnable_acq(pair->root, pair->rootno));
  assert(trace_traceno_acq(pair->root) == pair->rootno);
  assert(trace_root_acq(pair->root) == 0);
  assert(trace_link_acq(pair->root) == pair->rootno);
  assert(trace_linktype_acq(pair->root) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(pair->root) == 0);
  assert(trace_nextside_acq(pair->root) == 0);
  assert(trace_startpt_acq(pair->root) == pair->pt);
  assert(trace_topslot_acq(pair->root) == PRODUCTION_TOPSLOT);
  assert(trace_spadjust_acq(pair->root) == 0);
  assert(trace_mcode_acq(pair->root) != NULL);
  assert((la_load8_acq(&pair->root->unused1) &
	  TRACE_ARM64_INT_LOOP_ADMITTED) != 0);
  assert((la_load8_acq(&pair->root->unused1) &
	  TRACE_ARM64_INT_SIDE_ADMITTED) == 0);

  startpc = trace_startpc_acq(pair->root);
  assert(startpc != NULL && startpc >= proto_bc(pair->pt) &&
	 startpc < proto_bc(pair->pt)+pair->pt->sizebc);
  assert(bc_op(trace_startins_acq(pair->root)) == BC_LOOP);
  live = loadbc(startpc);
  assert(bc_op(live) == BC_JLOOP && bc_d(live) == pair->rootno);
  assert(proto_trace_acq(pair->pt) == pair->rootno);

  assert(trace_nsnap_acq(pair->root) == pair->root_nsnap &&
	 pair->exitno < pair->root_nsnap);
  assert(trace_exittab_nslots_acq(pair->root) ==
	 trace_nsnap_acq(pair->root));
  pair->root_snap = trace_snap_acq(pair->root);
  pair->root_exittab = trace_exittab_acq(pair->root);
  pair->root_fallback = exitstub_trace_fallback_addr_(
	trace_exitstub_acq(pair->root));
  assert(pair->root_exittab != NULL && pair->root_fallback != NULL);
  assert(pair->continuation_pos < pair->pt->sizebc);
  assert(selected_map_has_slot(pair->root, pair->exitno, 4));
  pair->continuation = selected_continuation(pair->root, pair->exitno);
  assert(pair->continuation ==
	 proto_bc(pair->pt)+pair->continuation_pos);
  assert(pair->child_pcpos[0] == pair->continuation_pos);
  assert(bc_op(loadbc(pair->continuation)) == BC_CGET);
  assert(snap_count_acq(&pair->root_snap[pair->exitno]) != SNAPCOUNT_DONE);
  expect_edge(g, pair, pair->root_fallback);
  assert(traceref_safe(J, pair->rootno) == pair->root);
}

static void record_child(lua_State *L, jit_State *J, ProductionPair *pair)
{
  unsigned attempt;
  for (attempt = 0; attempt < 4; attempt++) {
    TraceNo childno;
    assert(call_named(L, pair->name, pair->side_n, pair->side_bias) ==
	   pair->side_result);
    childno = trace_nextside_acq(pair->root);
    if (childno != 0) {
      GCtrace *child = traceref_safe(J, childno);
      if (trace_runnable_acq(child, childno)) {
	pair->childno = childno;
	pair->child = child;
	break;
      }
    }
  }
  assert(pair->child != NULL && pair->childno != 0);
}

static void expect_post_token_request_cleanup(lua_State *L, jit_State *J,
	global_State *g, TGState *tg, ProductionPair *pair)
{
  uint64_t epoch = gc2_hs_epoch_acq(g);
  uint32_t before = (uint32_t)snap_count_acq(
	&pair->root_snap[pair->exitno]);
  int status;

  assert(before != SNAPCOUNT_DONE);
  lj_trace_test_admission_reset();
  lj_trace_test_admission_arm(LJ_TRACE_TEST_ADMISSION_SIDE_AFTER_TOKEN,
	LJ_TRACE_TEST_REQUEST_COUNTED, LJ_GC2_HS_STOPREQ);
  lua_getglobal(L, pair->name);
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, pair->side_n);
  lua_pushinteger(L, pair->side_bias);
  status = lua_pcall(L, 2, 1, 0);
  assert(status == LUA_ERRRUN && lua_isstring(L, -1));
  assert(strstr(lua_tostring(L, -1),
		"thread interrupted: VM shutdown") != NULL);
  lua_pop(L, 1);
  assert(lj_trace_test_admission_hits() == 1);
  assert(lj_trace_test_admission_armed() == 0);
  assert(lj_trace_test_admission_side_parent() == pair->rootno);
  assert(lj_trace_test_admission_side_exitno() == pair->exitno);
  assert(lj_trace_test_admission_side_snapshot_before() == before);
  assert(lj_trace_test_admission_side_gate_blocks() == 0);
  assert(lj_trace_test_admission_side_clean_releases() == 1);
  assert((uint32_t)snap_count_acq(&pair->root_snap[pair->exitno]) ==
	 before);
  assert(trace_nchild_acq(pair->root) == 0);
  assert(trace_nextside_acq(pair->root) == 0);
  expect_edge(g, pair, pair->root_fallback);
  assert(gc2_hs_epoch_acq(g) == epoch+1u);
  assert(gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0);
  assert(lj_tg_poll_acq(tg) == 0);
  (void)lj_tg_flags_and_rlx(tg,
	(uint8_t)~(TGF_STOPREQ|TGF_STOPREQ_FRESH));
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  assert(jit_token_acq(g) == 0);
  assert(jit_owner_l_acq(J) == NULL);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(side_parent_cert_zero(&J->arm64_side_parent));
}

static void expect_child_shape(jit_State *J, global_State *g,
	ProductionPair *pair)
{
  static const IRRef snaprefs[PRODUCTION_CHILD_NSNAP] = {
    PRODUCTION_CHILD_R_CGET, PRODUCTION_CHILD_R_ADD,
    PRODUCTION_CHILD_R_LIMIT, PRODUCTION_CHILD_R_GT,
    PRODUCTION_CHILD_R_XPOLL
  };
  static const MSize mapofs[PRODUCTION_CHILD_NSNAP] = { 0, 3, 7, 11, 14 };
  static const uint8_t nent[PRODUCTION_CHILD_NSNAP] = { 1, 2, 2, 1, 1 };
  static const uint8_t nslots[PRODUCTION_CHILD_NSNAP] = { 5, 6, 6, 5, 5 };
  IRIns *ir = trace_ir_acq(pair->child);
  SnapShot *snap = trace_snap_acq(pair->child);
  SnapEntry *snapmap = trace_snapmap_acq(pair->child);
  MCode **exittab = trace_exittab_acq(pair->child);
  MCode *fallback = exitstub_trace_fallback_addr_(
	trace_exitstub_acq(pair->child));
  IRIns ins;
  SnapNo snapno;
  MSize i;
  GCArena *arena;

  assert(trace_runnable_acq(pair->child, pair->childno));
  assert(trace_traceno_acq(pair->child) == pair->childno);
  assert(trace_root_acq(pair->child) == pair->rootno);
  assert(trace_link_acq(pair->child) == pair->rootno);
  assert(trace_linktype_acq(pair->child) == LJ_TRLINK_ROOT);
  assert(trace_nextroot_acq(pair->child) == 0);
  assert(trace_nextside_acq(pair->child) == 0);
  assert(trace_nchild_acq(pair->child) == 0);
  assert(trace_startpt_acq(pair->child) == pair->pt);
  assert(trace_startpc_acq(pair->child) == pair->continuation);
  assert(trace_startins_acq(pair->child) == BCINS_AD(BC_JMP, 0, 0));
  assert(trace_topslot_acq(pair->child) == PRODUCTION_TOPSLOT);
  assert(trace_spadjust_acq(pair->child) == 0);
  assert(trace_nsnap_acq(pair->child) == PRODUCTION_CHILD_NSNAP);
  assert(trace_nsnapmap_acq(pair->child) == PRODUCTION_CHILD_NSNAPMAP);
  assert(trace_exittab_nslots_acq(pair->child) ==
	 PRODUCTION_CHILD_NSNAP+1u);
  assert(ir != NULL && snap != NULL && snapmap != NULL && exittab != NULL);
  assert(trace_nk_acq(pair->child) == PRODUCTION_CHILD_K_ADDEND);
  assert(trace_nins_acq(pair->child) ==
	 PRODUCTION_CHILD_SEMANTIC_NINS+1u);
  assert(la_load8_acq(&pair->child->unused1) ==
	 TRACE_ARM64_INT_SIDE_ADMITTED);
  assert(trace_native_pinword_acq(pair->child) == 0);
  assert(la_load64_acq(&pair->child->retire_epoch) == 0);

#define EXPECT_CHILD_IR(ref, op, type, a, b) \
  do { \
    ins = ir_load_acq(&ir[(ref)]); \
    assert(ins.o == (op) && ins.t.irt == (type)); \
    assert(ins.op1 == (a) && ins.op2 == (b)); \
  } while (0)
  EXPECT_CHILD_IR(REF_BASE, IR_BASE, IRT_PGC,
	pair->rootno, pair->exitno);
  EXPECT_CHILD_IR(PRODUCTION_CHILD_R_PARENT, IR_SLOAD, IRT_INT, 4,
	IRSLOAD_PARENT|IRSLOAD_INHERIT);
  EXPECT_CHILD_IR(PRODUCTION_CHILD_R_CGET, IR_NOP, IRT_NIL, 0, 0);
  EXPECT_CHILD_IR(PRODUCTION_CHILD_R_ADD, IR_ADDOV, IRT_INT|IRT_GUARD,
	PRODUCTION_CHILD_R_PARENT, PRODUCTION_CHILD_K_ADDEND);
  EXPECT_CHILD_IR(PRODUCTION_CHILD_R_LIMIT, IR_SLOAD, IRT_INT|IRT_GUARD, 2,
	IRSLOAD_TYPECHECK);
  EXPECT_CHILD_IR(PRODUCTION_CHILD_R_GT, IR_GT, IRT_INT|IRT_GUARD,
	PRODUCTION_CHILD_R_LIMIT, PRODUCTION_CHILD_R_ADD);
  EXPECT_CHILD_IR(PRODUCTION_CHILD_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD,
	1, 0);
  EXPECT_CHILD_IR(PRODUCTION_CHILD_SEMANTIC_NINS, IR_NOP, IRT_NIL, 0, 0);
#undef EXPECT_CHILD_IR
  ins = ir_load_acq(&ir[PRODUCTION_CHILD_K_ADDEND]);
  assert(ins.o == IR_KINT && ins.t.irt == IRT_INT &&
	 ins.i == pair->addend);
  ins = ir_load_acq(&ir[REF_BASE]);
  assert(ins.r == RID_BASE && ins.s == SPS_NONE);
  ins = ir_load_acq(&ir[PRODUCTION_CHILD_R_PARENT]);
  assert(ins.r == pair->sload_reg && ins.s == SPS_NONE);
  ins = ir_load_acq(&ir[PRODUCTION_CHILD_R_CGET]);
  assert(ins.r == RID_INIT && ins.s == SPS_NONE);
  ins = ir_load_acq(&ir[PRODUCTION_CHILD_R_ADD]);
  assert(ins.r == pair->inherited_reg && ins.s == SPS_NONE);
  ins = ir_load_acq(&ir[PRODUCTION_CHILD_R_LIMIT]);
  assert(ins.r == pair->sload_reg && ins.s == SPS_NONE);

  for (snapno = 0; snapno < PRODUCTION_CHILD_NSNAP; snapno++) {
    SnapShot *ss = &snap[snapno];
    assert(snap_ref_acq(ss) == snaprefs[snapno]);
    assert(snap_mapofs_acq(ss) == mapofs[snapno]);
    assert(snap_nent_acq(ss) == nent[snapno]);
    assert(snap_nslots_acq(ss) == nslots[snapno]);
    assert(snap_topslot_acq(ss) == PRODUCTION_TOPSLOT);
    assert(snap_pc_acq(&snapmap[mapofs[snapno]+nent[snapno]]) ==
	   proto_bc(pair->pt)+pair->child_pcpos[snapno]);
  }
  assert(snapentry_acq(&snapmap[0]) ==
	 SNAP(4, 0, PRODUCTION_CHILD_R_PARENT));
  assert(snapentry_acq(&snapmap[3]) ==
	 SNAP(4, 0, PRODUCTION_CHILD_R_PARENT));
  assert(snapentry_acq(&snapmap[4]) ==
	 SNAP(5, 0, PRODUCTION_CHILD_R_PARENT));
  assert(snapentry_acq(&snapmap[7]) ==
	 SNAP(4, 0, PRODUCTION_CHILD_R_ADD));
  assert(snapentry_acq(&snapmap[8]) ==
	 SNAP(5, 0, PRODUCTION_CHILD_R_ADD));
  assert(snapentry_acq(&snapmap[11]) ==
	 SNAP(4, 0, PRODUCTION_CHILD_R_ADD));
  assert(snapentry_acq(&snapmap[14]) ==
	 SNAP(4, 0, PRODUCTION_CHILD_R_ADD));

  pair->child_mcode = trace_mcode_acq(pair->child);
  assert(pair->child_mcode != NULL &&
	 trace_szmcode_acq(pair->child) > sizeof(MCode));
  assert(pointer_bits(trace_exittarget_arm64_encode(g, pair->child_mcode)) !=
	 pointer_bits(trace_exittarget_arm64_encode(g, pair->root_fallback)));
  for (i = 0; i < trace_exittab_nslots_acq(pair->child); i++) {
    void *raw = la_loadptr_acq((void *const *)&exittab[i]);
    void *encoded = trace_exittarget_arm64_encode(g, fallback);
    assert(pointer_bits(raw) == pointer_bits(encoded));
    assert(trace_exittarget_arm64_acq(pair->child, (ExitNo)i) == fallback);
  }
#if LJ_ABI_BRANCH_TRACK
  assert(pair->child_mcode[0] == A64I_LE(A64I_BTI_J));
#endif
  assert(pair->child_mcode[LJ_ABI_BRANCH_TRACK] ==
	 A64I_LE(A64I_MOVx | A64F_D(pair->sload_reg) |
		 A64F_M(pair->inherited_reg)));
#if LJ_ABI_PAUTH
  {
    ASMFunction actual = trace_mcauth_acq(pair->child);
    ASMFunction expected = lj_ptr_sign(
	ptrauth_nop_cast(ASMFunction, pair->child_mcode), pair->child);
    assert(function_bits(actual) == function_bits(expected));
    assert(ptrauth_nop_cast(MCode *, lj_ptr_strip(actual)) ==
	   pair->child_mcode);
  }
#endif
  arena = lj_arena_of(pair->child);
  assert(arena != NULL);
  assert(lj_arena_lifetime_state_acq(arena,
	 lj_arena_cellof(pair->child)) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(arena,
	 lj_arena_cellof(pair->child)) == LJ_ARENA_ROOT_MEMBER);

  assert(trace_nchild_acq(pair->root) == 1);
  assert(trace_nextside_acq(pair->root) == pair->childno);
  assert(snap_topslot_acq(&pair->root_snap[pair->exitno]) ==
	 PRODUCTION_TOPSLOT);
  assert(snap_count_acq(&pair->root_snap[pair->exitno]) ==
	 SNAPCOUNT_DONE);
  expect_edge(g, pair, pair->child_mcode);
  assert(traceref_safe(J, pair->childno) == pair->child);
}

static void expect_native_child(lua_State *L, jit_State *J,
	const ProductionPair *pair)
{
  uint32_t before = live_trace_count(J);
  lj_trace_test_reset_exit_stats();
  assert(call_named(L, pair->name, pair->native_n, pair->native_bias) ==
	 pair->native_result);
  if (lj_trace_test_exit_calls() != 1 ||
	lj_trace_test_first_exit_parent() != pair->childno ||
	lj_trace_test_first_exitno() != PRODUCTION_CHILD_EXIT ||
	lj_trace_test_last_exit_parent() != pair->childno ||
	lj_trace_test_last_exitno() != PRODUCTION_CHILD_EXIT)
    fprintf(stderr,
      "native child %s: calls=%u first=%u/%u last=%u/%u child=%u\n",
      pair->name, (unsigned)lj_trace_test_exit_calls(),
      (unsigned)lj_trace_test_first_exit_parent(),
      (unsigned)lj_trace_test_first_exitno(),
      (unsigned)lj_trace_test_last_exit_parent(),
      (unsigned)lj_trace_test_last_exitno(), (unsigned)pair->childno);
  assert(lj_trace_test_exit_calls() == 1);
  assert(lj_trace_test_first_exit_parent() == pair->childno);
  assert(lj_trace_test_first_exitno() == PRODUCTION_CHILD_EXIT);
  assert(lj_trace_test_last_exit_parent() == pair->childno);
  assert(lj_trace_test_last_exitno() == PRODUCTION_CHILD_EXIT);
  assert(live_trace_count(J) == before);
  assert(trace_nchild_acq(pair->child) == 0);
  assert(trace_nextside_acq(pair->child) == 0);
}

static void expect_unsupported_first_side_closed(lua_State *L, jit_State *J,
	global_State *g, ProductionPair *pair)
{
  uint32_t before, after;
  record_root(L, J, pair);
  expect_root_shape(J, g, pair);
  before = live_trace_count(J);
  lj_trace_test_reset_exittab_stats();
  assert(call_named(L, pair->name, pair->side_n, pair->side_bias) ==
	 pair->side_result);
  assert(call_named(L, pair->name, pair->side_n, pair->side_bias) ==
	 pair->side_result);
  after = (uint32_t)snap_count_acq(&pair->root_snap[pair->exitno]);
  assert(lj_trace_test_abort_count() >= 1);
  assert(lj_trace_test_last_abort_error() == LJ_TRERR_NYIIR);
  assert(after > 0 && after < SNAPCOUNT_DONE);
  assert(live_trace_count(J) == before);
  assert(trace_runnable_acq(pair->root, pair->rootno));
  assert(trace_nchild_acq(pair->root) == 0);
  assert(trace_nextside_acq(pair->root) == 0);
  expect_edge(g, pair, pair->root_fallback);
}

static void expect_return_linked_variant_closed(lua_State *L, jit_State *J,
	global_State *g, ProductionPair *pair)
{
  uint32_t before = live_trace_count(J);
  MSize count_before = snap_count_acq(&pair->root_snap[pair->exitno]);
  MSize count_after;
  assert(pair->exitno == 6 && pair->addend == 2);
  assert(count_before < SNAPCOUNT_DONE-2u);
  lj_trace_test_reset_exittab_stats();
  assert(call_named(L, pair->name, 3, 0) == 4);
  assert(call_named(L, pair->name, 3, 0) == 4);
  count_after = snap_count_acq(&pair->root_snap[pair->exitno]);
  assert(lj_trace_test_abort_count() == 2);
  assert(lj_trace_test_last_abort_error() == LJ_TRERR_NYIIR);
  assert(count_after == count_before+2u && count_after < SNAPCOUNT_DONE);
  assert(live_trace_count(J) == before);
  assert(trace_runnable_acq(pair->root, pair->rootno));
  assert(trace_nchild_acq(pair->root) == 0);
  assert(trace_nextside_acq(pair->root) == 0);
  expect_edge(g, pair, pair->root_fallback);
}

static void expect_quiescent(lua_State *L, jit_State *J, global_State *g,
	TGState *tg, void *saved_cframe, int32_t saved_vmstate)
{
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
  assert(lj_tg_gcroot_pending_owner_acq(tg) ==
	 (uint32_t)LJ_TG_ROOT_PENDING_IDLE);
}

static void expect_gc_claim(jit_State *J, global_State *g,
	ProductionPair *pairs)
{
  unsigned i, j;
  lj_trace_test_reset_retire_publish_calls();
  for (i = 0; i < PRODUCTION_PAIR_COUNT; i++) {
    ProductionPair *pair = &pairs[i];
    uint64_t stamp;
    assert(lj_trace_retire_gc_claim(g, pair->child) == 1);
    stamp = la_load64_acq(&pair->child->retire_epoch);
    assert(stamp != 0);
    assert(lj_trace_test_retire_publish_calls() == i+1u);
    assert(trace_retired_link_listed_acq(pair->child));
    assert(trace_native_pin_closed_acq(pair->child));
    assert(trace_native_pins_acq(pair->child) == 0);
    assert(!trace_runnable_acq(pair->child, pair->childno));
    assert(traceref_safe(J, pair->childno) == pair->child);
    assert(trace_traceno_acq(pair->child) == pair->childno);
    assert(trace_root_acq(pair->child) == pair->rootno);
    assert(trace_link_acq(pair->child) == 0);
    assert(trace_nchild_acq(pair->root) == 0);
    assert(trace_nextside_acq(pair->root) == 0);
    expect_edge(g, pair, pair->root_fallback);
    assert(lj_trace_retire_gc_claim(g, pair->child) == 1);
    assert(la_load64_acq(&pair->child->retire_epoch) == stamp);
    assert(lj_trace_test_retire_publish_calls() == i+1u);
    for (j = i+1u; j < PRODUCTION_PAIR_COUNT; j++)
      expect_edge(g, &pairs[j], pairs[j].child_mcode);
  }
}

static void expect_scoped(jit_State *J, global_State *g,
	ProductionPair *pairs)
{
  unsigned i, j;
  lj_trace_test_reset_retire_publish_calls();
  for (i = 0; i < PRODUCTION_PAIR_COUNT; i++) {
    ProductionPair *pair = &pairs[i];
    assert(lj_trace_flushscope(J, pair->childno) == 1u);
    assert(lj_trace_test_retire_publish_calls() == i+1u);
    assert(la_load64_acq(&pair->child->retire_epoch) != 0);
    assert(trace_retired_link_listed_acq(pair->child));
    assert(trace_native_pin_closed_acq(pair->child));
    assert(trace_native_pins_acq(pair->child) == 0);
    assert(trace_traceno_acq(pair->child) == 0);
    assert(trace_root_acq(pair->child) == pair->rootno);
    assert(trace_link_acq(pair->child) == 0);
    assert(trace_nchild_acq(pair->root) == 0);
    assert(trace_nextside_acq(pair->root) == 0);
    assert(trace_runnable_acq(pair->root, pair->rootno));
    assert(proto_trace_acq(pair->pt) == pair->rootno);
    expect_edge(g, pair, pair->root_fallback);
    for (j = i+1u; j < PRODUCTION_PAIR_COUNT; j++)
      expect_edge(g, &pairs[j], pairs[j].child_mcode);
  }
}

static void expect_full_flush(lua_State *L, jit_State *J, global_State *g,
	GCproto *decoy_pt, GCtrace *decoy, ProductionPair *pairs,
	ProductionPair *unsupported)
{
  unsigned i;
  lj_trace_test_reset_retire_publish_calls();
  assert(lj_trace_flushall_gc(L) == 0);
  assert(lj_trace_test_retire_publish_calls() == 10u);
  assert(trace_traceno_acq(decoy) == 0);
  assert(proto_trace_acq(decoy_pt) == 0);
  for (i = 0; i < PRODUCTION_PAIR_COUNT; i++) {
    ProductionPair *pair = &pairs[i];
    assert(trace_traceno_acq(pair->root) == 0);
    assert(trace_traceno_acq(pair->child) == 0);
    assert(la_load64_acq(&pair->root->retire_epoch) != 0);
    assert(la_load64_acq(&pair->child->retire_epoch) != 0);
    assert(trace_retired_link_listed_acq(pair->root));
    assert(trace_retired_link_listed_acq(pair->child));
    assert(trace_nchild_acq(pair->root) == 0);
    assert(trace_nextside_acq(pair->root) == 0);
    assert(trace_link_acq(pair->child) == 0);
    assert(proto_trace_acq(pair->pt) == 0);
    expect_edge(g, pair, pair->root_fallback);
  }
  assert(trace_traceno_acq(unsupported->root) == 0);
  assert(proto_trace_acq(unsupported->pt) == 0);
  assert(trace_nchild_acq(unsupported->root) == 0);
  assert(trace_nextside_acq(unsupported->root) == 0);
  expect_edge(g, unsupported, unsupported->root_fallback);
  assert(J->mcarea == NULL && J->mctop == NULL && J->mcbot == NULL);
}

static int detailed_main(int argc, char **argv)
{
  const char *mode = argc > 1 ? argv[1] : "gc-claim";
  lua_State *L = luaL_newstate();
  jit_State *J;
  global_State *g;
  TGState *tg;
  GCproto *decoy_pt;
  GCtrace *decoy;
  TraceNo decoy_no = 0;
  ProductionPair pairs[PRODUCTION_PAIR_COUNT] = {
    {
      .name = "__arm64_first_side_production_first",
      .root_n = 3, .root_bias = 0, .root_result = 3,
      .side_n = 3, .side_bias = 1, .side_result = 4,
      .native_n = 3, .native_bias = 1, .native_result = 4,
      .exitno = 2, .root_nsnap = 8, .continuation_pos = 13,
      .child_pcpos = { 13, 14, 3, 17, 7 },
      .inherited_reg = RID_X28, .sload_reg = RID_X27, .addend = 1
    },
    {
      .name = "__arm64_first_side_production_second",
      .root_n = 3, .root_bias = 0, .root_result = 3,
      .side_n = 3, .side_bias = 0, .side_result = 3,
      .native_n = 2, .native_bias = 0, .native_result = 2,
      .exitno = 6, .root_nsnap = 9, .continuation_pos = 10,
      .child_pcpos = { 10, 11, 3, 17, 7 },
      .inherited_reg = RID_X27, .sload_reg = RID_X28, .addend = 1
    },
    {
      .name = "__arm64_first_side_production_third",
      .root_n = 3, .root_bias = 0, .root_result = 3,
      .side_n = 4, .side_bias = 0, .side_result = 4,
      .native_n = 3, .native_bias = 0, .native_result = 3,
      .exitno = 7, .root_nsnap = 11, .continuation_pos = 13,
      .child_pcpos = { 13, 14, 3, 17, 7 },
      .inherited_reg = RID_X28, .sload_reg = RID_X27, .addend = 1
    },
    {
      .name = "__arm64_first_side_production_fourth",
      .root_n = 3, .root_bias = 0, .root_result = 4,
      .side_n = 5, .side_bias = 0, .side_result = 6,
      .native_n = 3, .native_bias = 0, .native_result = 4,
      .exitno = 6, .root_nsnap = 9, .continuation_pos = 10,
      .child_pcpos = { 10, 11, 3, 17, 7 },
      .inherited_reg = RID_X27, .sload_reg = RID_X28, .addend = 2
    }
  };
  ProductionPair unsupported = {
    .name = "__arm64_first_side_production_unsupported",
    .root_n = 4, .root_bias = 0, .root_result = 6,
    .side_n = 7, .side_bias = 0, .side_result = 9,
    .exitno = 6, .root_nsnap = 9, .continuation_pos = 10,
    .child_pcpos = { 10, 11, 3, 17, 7 },
    .inherited_reg = RID_X27, .sload_reg = RID_X28, .addend = 3
  };
  void *saved_cframe;
  int32_t saved_vmstate;
  unsigned i, j;

  assert(argc <= 2);
  assert(strcmp(mode, "gc-claim") == 0 ||
	 strcmp(mode, "scoped") == 0 ||
	 strcmp(mode, "full-flush") == 0);
  assert(L != NULL);
  luaL_openlibs(L);
  J = L2J(L);
  g = G(L);
  tg = L2TG(L);
  assert(J != NULL && g != NULL && tg != NULL);
  define_probes(L);
  saved_cframe = L->cframe;
  saved_vmstate = lj_tg_vmstate_load_acq(tg);

  decoy_pt = named_proto(L, "__arm64_first_side_production_decoy");
  assert(call_named(L, "__arm64_first_side_production_decoy", 3, 0) == 3);
  decoy = find_root(J, decoy_pt, &decoy_no);
  assert(decoy != NULL && decoy_no != 0);

  for (i = 0; i < PRODUCTION_PAIR_COUNT; i++) {
    record_root(L, J, &pairs[i]);
    expect_root_shape(J, g, &pairs[i]);
    assert(pairs[i].rootno > (i == 0 ? decoy_no : pairs[i-1u].childno));
    if (pairs[i].addend == 2)
      expect_return_linked_variant_closed(L, J, g, &pairs[i]);
    expect_post_token_request_cleanup(L, J, g, tg, &pairs[i]);
    record_child(L, J, &pairs[i]);
    expect_child_shape(J, g, &pairs[i]);
    for (j = 0; j < i; j++) {
      assert(pairs[i].rootno != pairs[j].rootno);
      assert(pairs[i].rootno != pairs[j].childno);
      assert(pairs[i].childno != pairs[j].childno);
      assert(pairs[i].exitno != pairs[j].exitno ||
	     pairs[i].addend != pairs[j].addend);
    }
  }
  assert(pairs[1].exitno == 6 && pairs[1].addend == 1);
  assert(pairs[3].exitno == 6 && pairs[3].addend == 2);
  assert(pairs[0].rootno != 1 && pairs[0].childno != 2);
  expect_unsupported_first_side_closed(L, J, g, &unsupported);
  assert(unsupported.rootno > pairs[PRODUCTION_PAIR_COUNT-1u].childno);
  assert(live_trace_count(J) == 10u);

  expect_quiescent(L, J, g, tg, saved_cframe, saved_vmstate);
  for (j = 0; j < 2; j++)
    for (i = 0; i < PRODUCTION_PAIR_COUNT; i++)
      expect_native_child(L, J, &pairs[i]);
  assert(live_trace_count(J) == 10u);
  for (i = 0; i < PRODUCTION_PAIR_COUNT; i++)
    expect_edge(g, &pairs[i], pairs[i].child_mcode);
  expect_quiescent(L, J, g, tg, saved_cframe, saved_vmstate);

  if (strcmp(mode, "full-flush") == 0)
    expect_full_flush(L, J, g, decoy_pt, decoy, pairs, &unsupported);
  else if (strcmp(mode, "scoped") == 0)
    expect_scoped(J, g, pairs);
  else
    expect_gc_claim(J, g, pairs);

  assert(gc2_smr_readers_acq(g) == 0);
  assert(jit_token_acq(g) == 0);
  assert(jit_owner_l_acq(J) == NULL);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
  lua_close(L);
  printf("t-arm64-jit-first-side-production %s OK\n", mode);
  return 0;
}

#endif

int main(int argc, char **argv)
{
#ifdef LJ_TRACE_TEST_HELPERS
  return detailed_main(argc, argv);
#else
  (void)argc;
  (void)argv;
  return smoke_main();
#endif
}

#else

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  puts("t-arm64-jit-first-side-production SKIP");
  return 0;
}

#endif
