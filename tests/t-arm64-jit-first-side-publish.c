/*
** Exact one-shot publication proof for the first ARM64 side trace.
** The broad ARM64 side recorder stays fail-closed. This dedicated seam closes
** the ordinary production canary and admits only parent 1 / exit 2, publishing
** its child as trace 2.
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
    defined(LJ_TRACE_TEST_HELPERS) && \
    defined(LJ_ARM64_FIRST_SIDE_PUBLISH_TEST)

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

#if !LJ_TARGET_OSX || !LJ_TARGET_ARM64 || !LJ_HASJIT || \
    LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED != 1 || \
    LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED != 1 || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED
#error "first-side publication probe requires admitted roots and closed production sides"
#endif

enum {
  PROBE_PARENT = 1,
  PROBE_CHILD = 2,
  PROBE_GRANDCHILD = 3,
  PROBE_EXIT = 2,
  PROBE_CHILD_EXIT = 3,
  PROBE_CONTINUATION_POS = 13,
  PROBE_TOPSLOT = 5,
  PROBE_CHILD_NSNAP = 5,
  PROBE_CHILD_NSNAPMAP = 17,
  PROBE_CHILD_K_ONE = REF_TRUE-1u,
  PROBE_CHILD_R_PARENT = REF_BASE+1u,
  PROBE_CHILD_R_CGET = REF_BASE+2u,
  PROBE_CHILD_R_ADD = REF_BASE+3u,
  PROBE_CHILD_R_LIMIT = REF_BASE+4u,
  PROBE_CHILD_R_GT = REF_BASE+5u,
  PROBE_CHILD_R_XPOLL = REF_BASE+6u,
  PROBE_CHILD_SEMANTIC_NINS = REF_BASE+7u
};

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 first-side publication setup failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
}

static lua_Integer call_probe(lua_State *L, lua_Integer n, lua_Integer bias)
{
  lua_Integer result;
  int status;
  lua_getglobal(L, "__arm64_first_side_publish_probe");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, n);
  lua_pushinteger(L, bias);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 first-side publication call failed: %s\n",
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
  lua_getglobal(L, "__arm64_first_side_publish_probe");
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

static GCtrace *retired_find(jit_State *J, GCtrace *needle)
{
  GCtrace *T;
  for (T = trace_retired_head_acq(J);
	 T != NULL;
	 T = trace_retired_next_acq(T))
    if (T == needle)
      return T;
  return NULL;
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

static void expect_root_shape(jit_State *J, GCproto *pt, GCtrace **rootp,
	const BCIns **continuationp, MCode **fallbackp)
{
  GCtrace *root = traceref_safe(J, PROBE_PARENT);
  const BCIns *startpc;
  const BCIns *continuation;
  BCIns live;

  assert(pt != NULL && pt->sizebc == 19 && pt->numparams == 2 &&
	 pt->framesize == PROBE_TOPSLOT);
  assert(root != NULL && trace_runnable_acq(root, PROBE_PARENT));
  assert(trace_traceno_acq(root) == PROBE_PARENT);
  assert(trace_root_acq(root) == 0);
  assert(trace_link_acq(root) == PROBE_PARENT);
  assert(trace_linktype_acq(root) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(root) == 0);
  assert(trace_nextside_acq(root) == 0);
  assert(trace_startpt_acq(root) == pt);
  assert(trace_topslot_acq(root) == PROBE_TOPSLOT);
  assert(trace_spadjust_acq(root) == 0);
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
  *fallbackp = exitstub_trace_fallback_addr_(trace_exitstub_acq(root));
  assert(trace_exittarget_arm64_acq(root, PROBE_EXIT) == *fallbackp);
}

static void expect_child_shape(jit_State *J, GCproto *pt, GCtrace *child,
	const BCIns *continuation)
{
  static const IRRef snaprefs[PROBE_CHILD_NSNAP] = {
    PROBE_CHILD_R_CGET, PROBE_CHILD_R_ADD, PROBE_CHILD_R_LIMIT,
    PROBE_CHILD_R_GT, PROBE_CHILD_R_XPOLL
  };
  static const MSize mapofs[PROBE_CHILD_NSNAP] = { 0, 3, 7, 11, 14 };
  static const uint8_t nent[PROBE_CHILD_NSNAP] = { 1, 2, 2, 1, 1 };
  static const uint8_t nslots[PROBE_CHILD_NSNAP] = { 5, 6, 6, 5, 5 };
  static const MSize pcpos[PROBE_CHILD_NSNAP] = { 13, 14, 3, 17, 7 };
  GCArena *arena;
  IRIns *ir;
  IRIns ins;
  SnapShot *snap;
  SnapEntry *snapmap;
  MCode *mcode;
  MCode *fallback;
  MCode **exittab;
  SnapNo snapno;
  MSize i;

  assert(child != NULL && trace_runnable_acq(child, PROBE_CHILD));
  assert(trace_traceno_acq(child) == PROBE_CHILD);
  assert(trace_root_acq(child) == PROBE_PARENT);
  assert(trace_link_acq(child) == PROBE_PARENT);
  assert(trace_linktype_acq(child) == LJ_TRLINK_ROOT);
  assert(trace_nextroot_acq(child) == 0);
  assert(trace_nextside_acq(child) == 0);
  assert(trace_nchild_acq(child) == 0);
  assert(trace_startpt_acq(child) == pt);
  assert(trace_startpc_acq(child) == continuation);
  assert(trace_startins_acq(child) == BCINS_AD(BC_JMP, 0, 0));
  assert(trace_topslot_acq(child) == PROBE_TOPSLOT);
  assert(trace_spadjust_acq(child) == 0);
  assert(trace_nsnap_acq(child) == PROBE_CHILD_NSNAP);
  assert(trace_nsnapmap_acq(child) == PROBE_CHILD_NSNAPMAP);
  assert(trace_exittab_nslots_acq(child) == PROBE_CHILD_NSNAP+1u);
  ir = trace_ir_acq(child);
  snap = trace_snap_acq(child);
  snapmap = trace_snapmap_acq(child);
  assert(ir != NULL && snap != NULL && snapmap != NULL);
  assert(trace_nk_acq(child) == PROBE_CHILD_K_ONE);
  assert(trace_nins_acq(child) == PROBE_CHILD_SEMANTIC_NINS+1u);
  assert(la_load8_acq(&child->unused1) == TRACE_ARM64_INT_SIDE_ADMITTED);
  assert(trace_native_pinword_acq(child) == 0);
  assert(la_load64_acq(&child->retire_epoch) == 0);

#define EXPECT_CHILD_IR(ref, op, type, a, b) \
  do { \
    ins = ir_load_acq(&ir[(ref)]); \
    assert(ins.o == (op) && ins.t.irt == (type)); \
    assert(ins.op1 == (a) && ins.op2 == (b)); \
  } while (0)
  EXPECT_CHILD_IR(REF_BASE, IR_BASE, IRT_PGC, PROBE_PARENT, PROBE_EXIT);
  EXPECT_CHILD_IR(PROBE_CHILD_R_PARENT, IR_SLOAD, IRT_INT, 4,
	IRSLOAD_PARENT|IRSLOAD_INHERIT);
  EXPECT_CHILD_IR(PROBE_CHILD_R_CGET, IR_NOP, IRT_NIL, 0, 0);
  EXPECT_CHILD_IR(PROBE_CHILD_R_ADD, IR_ADDOV, IRT_INT|IRT_GUARD,
	PROBE_CHILD_R_PARENT, PROBE_CHILD_K_ONE);
  EXPECT_CHILD_IR(PROBE_CHILD_R_LIMIT, IR_SLOAD, IRT_INT|IRT_GUARD, 2,
	IRSLOAD_TYPECHECK);
  EXPECT_CHILD_IR(PROBE_CHILD_R_GT, IR_GT, IRT_INT|IRT_GUARD,
	PROBE_CHILD_R_LIMIT, PROBE_CHILD_R_ADD);
  EXPECT_CHILD_IR(PROBE_CHILD_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  EXPECT_CHILD_IR(PROBE_CHILD_SEMANTIC_NINS, IR_NOP, IRT_NIL, 0, 0);
#undef EXPECT_CHILD_IR
  ins = ir_load_acq(&ir[PROBE_CHILD_K_ONE]);
  assert(ins.o == IR_KINT && ins.t.irt == IRT_INT && ins.i == 1);
  ins = ir_load_acq(&ir[REF_BASE]);
  assert(ins.r == RID_BASE && ins.s == SPS_NONE);
  ins = ir_load_acq(&ir[PROBE_CHILD_R_PARENT]);
  assert(ins.r == RID_X27 && ins.s == SPS_NONE);
  ins = ir_load_acq(&ir[PROBE_CHILD_R_CGET]);
  assert(ins.r == RID_INIT && ins.s == SPS_NONE);
  ins = ir_load_acq(&ir[PROBE_CHILD_R_ADD]);
  assert(ins.r == RID_X28 && ins.s == SPS_NONE);
  ins = ir_load_acq(&ir[PROBE_CHILD_R_LIMIT]);
  assert(ins.r == RID_X27 && ins.s == SPS_NONE);

  for (snapno = 0; snapno < PROBE_CHILD_NSNAP; snapno++) {
    SnapShot *ss = &snap[snapno];
    assert(snap_ref_acq(ss) == snaprefs[snapno]);
    assert(snap_mapofs_acq(ss) == mapofs[snapno]);
    assert(snap_nent_acq(ss) == nent[snapno]);
    assert(snap_nslots_acq(ss) == nslots[snapno]);
    assert(snap_topslot_acq(ss) == PROBE_TOPSLOT);
    assert(snap_pc_acq(&snapmap[mapofs[snapno]+nent[snapno]]) ==
	   proto_bc(pt)+pcpos[snapno]);
  }
  assert(snapentry_acq(&snapmap[0]) ==
	 SNAP(4, 0, PROBE_CHILD_R_PARENT));
  assert(snapentry_acq(&snapmap[3]) ==
	 SNAP(4, 0, PROBE_CHILD_R_PARENT));
  assert(snapentry_acq(&snapmap[4]) ==
	 SNAP(5, 0, PROBE_CHILD_R_PARENT));
  assert(snapentry_acq(&snapmap[7]) ==
	 SNAP(4, 0, PROBE_CHILD_R_ADD));
  assert(snapentry_acq(&snapmap[8]) ==
	 SNAP(5, 0, PROBE_CHILD_R_ADD));
  assert(snapentry_acq(&snapmap[11]) ==
	 SNAP(4, 0, PROBE_CHILD_R_ADD));
  assert(snapentry_acq(&snapmap[14]) ==
	 SNAP(4, 0, PROBE_CHILD_R_ADD));

  mcode = trace_mcode_acq(child);
  exittab = trace_exittab_acq(child);
  assert(mcode != NULL && trace_szmcode_acq(child) > sizeof(MCode));
  assert(exittab != NULL && trace_exitstub_acq(child) != NULL);
  fallback = exitstub_trace_fallback_addr_(trace_exitstub_acq(child));
  for (i = 0; i < trace_exittab_nslots_acq(child); i++) {
    assert(pointer_bits(la_loadptr_acq((void *const *)&exittab[i])) ==
	   pointer_bits(trace_exittarget_arm64_encode(J2G(J), fallback)));
    assert(trace_exittarget_arm64_acq(child, (ExitNo)i) == fallback);
  }

#if LJ_ABI_BRANCH_TRACK
  assert(mcode[0] == A64I_LE(A64I_BTI_J));
#endif
  assert(mcode[LJ_ABI_BRANCH_TRACK] ==
	 A64I_LE(A64I_MOVx | A64F_D(RID_X27) | A64F_M(RID_X28)));
#if LJ_ABI_PAUTH
  {
    ASMFunction actual = trace_mcauth_acq(child);
    ASMFunction expected = lj_ptr_sign(
	  ptrauth_nop_cast(ASMFunction, mcode), child);
    assert(function_bits(actual) == function_bits(expected));
    assert(ptrauth_nop_cast(MCode *, lj_ptr_strip(actual)) == mcode);
  }
#endif

  arena = lj_arena_of(child);
  assert(arena != NULL);
  assert(lj_arena_lifetime_state_acq(arena, lj_arena_cellof(child)) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(arena, lj_arena_cellof(child)) ==
	 LJ_ARENA_ROOT_MEMBER);
}

static void expect_child_native_exit(lua_State *L, jit_State *J)
{
  uint32_t calls;
  TraceNo first_parent;
  TraceNo last_parent;
  ExitNo first_exit;
  ExitNo last_exit;
  lj_trace_test_reset_exit_stats();
  assert(call_probe(L, 3, 1) == 4);
  calls = lj_trace_test_exit_calls();
  first_parent = lj_trace_test_first_exit_parent();
  first_exit = lj_trace_test_first_exitno();
  last_parent = lj_trace_test_last_exit_parent();
  last_exit = lj_trace_test_last_exitno();
  if (calls != 1 || first_parent != PROBE_CHILD ||
      first_exit != PROBE_CHILD_EXIT || last_parent != PROBE_CHILD ||
      last_exit != PROBE_CHILD_EXIT) {
    fprintf(stderr,
      "published child exit calls=%u first=%u/%u last=%u/%u slot3=%p\n",
      (unsigned)calls, (unsigned)first_parent, (unsigned)first_exit,
      (unsigned)last_parent, (unsigned)last_exit,
      (void *)traceref_safe(J, PROBE_GRANDCHILD));
  }
  assert(calls == 1);
  assert(first_parent == PROBE_CHILD);
  assert(first_exit == PROBE_CHILD_EXIT);
  assert(last_parent == PROBE_CHILD);
  assert(last_exit == PROBE_CHILD_EXIT);
  assert(traceref_safe(J, PROBE_GRANDCHILD) == NULL);
}

static void expect_root_native_fallback(lua_State *L, jit_State *J)
{
  uint32_t calls;
  TraceNo first_parent;
  TraceNo last_parent;
  ExitNo first_exit;
  ExitNo last_exit;
  lj_trace_test_reset_exit_stats();
  assert(call_probe(L, 1, 1) == 2);
  calls = lj_trace_test_exit_calls();
  first_parent = lj_trace_test_first_exit_parent();
  first_exit = lj_trace_test_first_exitno();
  last_parent = lj_trace_test_last_exit_parent();
  last_exit = lj_trace_test_last_exitno();
  if (calls != 1 || first_parent != PROBE_PARENT ||
      first_exit != PROBE_EXIT || last_parent != PROBE_PARENT ||
      last_exit != PROBE_EXIT) {
    fprintf(stderr,
      "retired child fallback calls=%u first=%u/%u last=%u/%u slot2=%p\n",
      (unsigned)calls, (unsigned)first_parent, (unsigned)first_exit,
      (unsigned)last_parent, (unsigned)last_exit,
      (void *)traceref_safe(J, PROBE_CHILD));
  }
  assert(calls == 1);
  assert(first_parent == PROBE_PARENT);
  assert(first_exit == PROBE_EXIT);
  assert(last_parent == PROBE_PARENT);
  assert(last_exit == PROBE_EXIT);
}

static void expect_retired_first_side(lua_State *L, jit_State *J,
	global_State *g, GCproto *pt, GCtrace *root, GCtrace *child,
	const BCIns *continuation, SnapShot *root_snap, MCode **root_exittab,
	MCode *root_fallback, MCode *child_mcode)
{
  IRIns *child_ir = trace_ir_acq(child);
  SnapShot *child_snap = trace_snap_acq(child);
  SnapEntry *child_snapmap = trace_snapmap_acq(child);
  MCode **child_exittab = trace_exittab_acq(child);
  MCode *child_exitstub = trace_exitstub_acq(child);
  MSize child_szmcode = trace_szmcode_acq(child);
  GCArena *child_arena = lj_arena_of(child);
  uint32_t child_cell = lj_arena_cellof(child);
  void *raw_child = trace_exittarget_arm64_encode(g, child_mcode);
  void *raw_fallback = trace_exittarget_arm64_encode(g, root_fallback);
  uintptr_t retired_link;
  uint64_t retire_stamp;

  assert(pointer_bits(raw_child) != pointer_bits(raw_fallback));
  assert(pointer_bits(
	la_loadptr_acq((void *const *)&root_exittab[PROBE_EXIT])) ==
	 pointer_bits(raw_child));
  assert(snap_topslot_acq(&root_snap[PROBE_EXIT]) == PROBE_TOPSLOT);
  assert(snap_count_acq(&root_snap[PROBE_EXIT]) == SNAPCOUNT_DONE);

  lj_trace_test_reset_retire_publish_calls();
  assert(lj_trace_retire_gc_claim(g, child) == 1);
  retire_stamp = la_load64_acq(&child->retire_epoch);
  retired_link = trace_retired_link_acq(child);
  assert(retire_stamp != 0);
  assert(lj_trace_test_retire_publish_calls() == 1u);
  assert(trace_retired_link_listed_acq(child));
  assert(trace_retired_next_acq(child) != child);
  assert(retired_find(J, child) == child);
  assert(!lj_trace_body_destroyed_acq(child));
  assert(trace_native_pin_closed_acq(child));
  assert(trace_native_pins_acq(child) == 0);
  assert(!trace_runnable_acq(child, PROBE_CHILD));
  assert(traceref_safe(J, PROBE_CHILD) == child);
  assert(trace_traceno_acq(child) == PROBE_CHILD);
  assert(trace_root_acq(child) == PROBE_PARENT);
  assert(trace_link_acq(child) == 0);
  assert(trace_nextroot_acq(child) == 0);
  assert(trace_nextside_acq(child) == 0);

  /* Retirement restores native reachability before removing topology. The
  ** publication snapshot remains deliberately terminal and conservative. */
  assert(traceref_safe(J, PROBE_PARENT) == root);
  assert(trace_runnable_acq(root, PROBE_PARENT));
  assert(trace_nchild_acq(root) == 0);
  assert(trace_nextside_acq(root) == 0);
  assert(trace_exittarget_arm64_acq(root, PROBE_EXIT) == root_fallback);
  assert(pointer_bits(
	la_loadptr_acq((void *const *)&root_exittab[PROBE_EXIT])) ==
	 pointer_bits(raw_fallback));
  assert(snap_topslot_acq(&root_snap[PROBE_EXIT]) == PROBE_TOPSLOT);
  assert(snap_count_acq(&root_snap[PROBE_EXIT]) == SNAPCOUNT_DONE);

  /* The public slot and complete compact/native body remain list-owned for
  ** their grace period. The second production claim is an exact no-op. */
  assert(trace_startpt_acq(child) == pt);
  assert(trace_startpc_acq(child) == continuation);
  assert(trace_ir_acq(child) == child_ir);
  assert(trace_snap_acq(child) == child_snap);
  assert(trace_snapmap_acq(child) == child_snapmap);
  assert(trace_mcode_acq(child) == child_mcode);
  assert(trace_szmcode_acq(child) == child_szmcode);
  assert(trace_exittab_acq(child) == child_exittab);
  assert(trace_exitstub_acq(child) == child_exitstub);
  assert(lj_arena_lifetime_state_acq(child_arena, child_cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(child_arena, child_cell) ==
	 LJ_ARENA_ROOT_MEMBER);

  assert(lj_trace_retire_gc_claim(g, child) == 1);
  assert(lj_trace_test_retire_publish_calls() == 1u);
  assert(la_load64_acq(&child->retire_epoch) == retire_stamp);
  assert(trace_retired_link_acq(child) == retired_link);
  assert(retired_find(J, child) == child);
  assert(!lj_trace_body_destroyed_acq(child));
  assert(trace_mcode_acq(child) == child_mcode);
  assert(trace_exittab_acq(child) == child_exittab);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(jit_token_acq(g) == 0);
  assert(jit_owner_l_acq(J) == NULL);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);

  expect_root_native_fallback(L, J);
  expect_root_native_fallback(L, J);
  assert(trace_nchild_acq(root) == 0 && trace_nextside_acq(root) == 0);
  assert(trace_exittarget_arm64_acq(root, PROBE_EXIT) == root_fallback);
  assert(retired_find(J, child) == child);
  assert(!lj_trace_body_destroyed_acq(child));
  assert(gc2_smr_readers_acq(g) == 0);
  assert(jit_token_acq(g) == 0);
}

static void expect_full_flush_first_side(lua_State *L, jit_State *J,
	global_State *g, GCproto *pt, GCtrace *root, GCtrace *child,
	const BCIns *continuation, SnapShot *root_snap, MCode **root_exittab,
	MCode *root_fallback, MCode *child_mcode)
{
  const BCIns *root_startpc = trace_startpc_acq(root);
  BCIns root_startins = trace_startins_acq(root);
  IRIns *root_ir = trace_ir_acq(root);
  SnapEntry *root_snapmap = trace_snapmap_acq(root);
  MCode *root_mcode = trace_mcode_acq(root);
  MCode *root_exitstub = trace_exitstub_acq(root);
  IRIns *child_ir = trace_ir_acq(child);
  SnapShot *child_snap = trace_snap_acq(child);
  SnapEntry *child_snapmap = trace_snapmap_acq(child);
  MCode **child_exittab = trace_exittab_acq(child);
  MCode *child_exitstub = trace_exitstub_acq(child);
  MSize child_szmcode = trace_szmcode_acq(child);
  GCArena *root_arena = lj_arena_of(root);
  GCArena *child_arena = lj_arena_of(child);
  uint32_t root_cell = lj_arena_cellof(root);
  uint32_t child_cell = lj_arena_cellof(child);
  void *raw_child = trace_exittarget_arm64_encode(g, child_mcode);
  void *raw_fallback = trace_exittarget_arm64_encode(g, root_fallback);

  assert(pointer_bits(raw_child) != pointer_bits(raw_fallback));
  assert(pointer_bits(
	la_loadptr_acq((void *const *)&root_exittab[PROBE_EXIT])) ==
	 pointer_bits(raw_child));
  assert(proto_trace_acq(pt) == PROBE_PARENT);
  assert(bc_op(loadbc(root_startpc)) == BC_JLOOP);
  assert(bc_d(loadbc(root_startpc)) == PROBE_PARENT);

  lj_trace_test_reset_retire_publish_calls();
  assert(lj_trace_flushall_gc(L) == 0);

  /* The structural prepass retires and disconnects the child while its parent
  ** is still live. The destructive pass then retires each body exactly once,
  ** regardless of the trace-number scan order. */
  assert(lj_trace_test_retire_publish_calls() == 2u);
  assert(la_load64_acq(&root->retire_epoch) != 0);
  assert(la_load64_acq(&child->retire_epoch) != 0);
  assert(trace_retired_link_listed_acq(root));
  assert(trace_retired_link_listed_acq(child));
  assert(retired_find(J, root) == root);
  assert(retired_find(J, child) == child);
  assert(!lj_trace_body_destroyed_acq(root));
  assert(!lj_trace_body_destroyed_acq(child));
  assert(trace_native_pin_closed_acq(root));
  assert(trace_native_pin_closed_acq(child));
  assert(trace_native_pins_acq(root) == 0);
  assert(trace_native_pins_acq(child) == 0);
  assert(trace_traceno_acq(root) == 0);
  assert(trace_traceno_acq(child) == 0);

  assert(trace_root_acq(root) == 0);
  assert(trace_link_acq(root) == 0);
  assert(trace_nchild_acq(root) == 0);
  assert(trace_nextside_acq(root) == 0);
  assert(trace_root_acq(child) == PROBE_PARENT);
  assert(trace_link_acq(child) == 0);
  assert(trace_nextside_acq(child) == 0);
  assert(pointer_bits(
	la_loadptr_acq((void *const *)&root_exittab[PROBE_EXIT])) ==
	 pointer_bits(raw_fallback));
  assert(trace_exittarget_arm64_acq(root, PROBE_EXIT) == root_fallback);
  assert(snap_topslot_acq(&root_snap[PROBE_EXIT]) == PROBE_TOPSLOT);
  assert(snap_count_acq(&root_snap[PROBE_EXIT]) == SNAPCOUNT_DONE);

  /* Full flush removes interpreter/prototype entry, but list ownership keeps
  ** both compact bodies and their retired mcode area inspectable through the
  ** grace interval. */
  assert(proto_trace_acq(pt) == 0);
  assert(loadbc(root_startpc) == root_startins);
  assert(bc_op(root_startins) == BC_LOOP);
  assert(trace_startpt_acq(root) == pt);
  assert(trace_startpt_acq(child) == pt);
  assert(trace_startpc_acq(child) == continuation);
  assert(trace_ir_acq(root) == root_ir);
  assert(trace_snap_acq(root) == root_snap);
  assert(trace_snapmap_acq(root) == root_snapmap);
  assert(trace_mcode_acq(root) == root_mcode);
  assert(trace_exittab_acq(root) == root_exittab);
  assert(trace_exitstub_acq(root) == root_exitstub);
  assert(trace_ir_acq(child) == child_ir);
  assert(trace_snap_acq(child) == child_snap);
  assert(trace_snapmap_acq(child) == child_snapmap);
  assert(trace_mcode_acq(child) == child_mcode);
  assert(trace_szmcode_acq(child) == child_szmcode);
  assert(trace_exittab_acq(child) == child_exittab);
  assert(trace_exitstub_acq(child) == child_exitstub);
  assert(lj_arena_lifetime_state_acq(root_arena, root_cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_lifetime_state_acq(child_arena, child_cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(root_arena, root_cell) ==
	 LJ_ARENA_ROOT_MEMBER);
  assert(lj_arena_root_state_acq(child_arena, child_cell) ==
	 LJ_ARENA_ROOT_MEMBER);
  assert(J->mcarea == NULL && J->mctop == NULL && J->mcbot == NULL);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(jit_token_acq(g) == 0);
  assert(jit_owner_l_acq(J) == NULL);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);
}

static void expect_scoped_retired_first_side(lua_State *L, jit_State *J,
	global_State *g, GCproto *pt, GCtrace *root, GCtrace *child,
	const BCIns *continuation, SnapShot *root_snap, MCode **root_exittab,
	MCode *root_fallback, MCode *child_mcode)
{
  IRIns *child_ir = trace_ir_acq(child);
  SnapShot *child_snap = trace_snap_acq(child);
  SnapEntry *child_snapmap = trace_snapmap_acq(child);
  MCode **child_exittab = trace_exittab_acq(child);
  MCode *child_exitstub = trace_exitstub_acq(child);
  GCArena *child_arena = lj_arena_of(child);
  uint32_t child_cell = lj_arena_cellof(child);
  const BCIns *root_startpc = trace_startpc_acq(root);
  void *raw_child = trace_exittarget_arm64_encode(g, child_mcode);
  void *raw_fallback = trace_exittarget_arm64_encode(g, root_fallback);

  assert(pointer_bits(raw_child) != pointer_bits(raw_fallback));
  assert(pointer_bits(
	la_loadptr_acq((void *const *)&root_exittab[PROBE_EXIT])) ==
	 pointer_bits(raw_child));
  lj_trace_test_reset_retire_publish_calls();
  assert(lj_trace_flushscope(J, PROBE_CHILD) == 1u);

  assert(lj_trace_test_retire_publish_calls() == 1u);
  assert(la_load64_acq(&child->retire_epoch) != 0);
  assert(trace_retired_link_listed_acq(child));
  assert(retired_find(J, child) == child);
  assert(!lj_trace_body_destroyed_acq(child));
  assert(trace_native_pin_closed_acq(child));
  assert(trace_native_pins_acq(child) == 0);
  assert(trace_traceno_acq(child) == 0);
  assert(trace_root_acq(child) == PROBE_PARENT);
  assert(trace_link_acq(child) == 0);
  assert(trace_nextside_acq(child) == 0);

  assert(traceref_safe(J, PROBE_PARENT) == root);
  assert(trace_runnable_acq(root, PROBE_PARENT));
  assert(la_load64_acq(&root->retire_epoch) == 0);
  assert(!trace_retired_link_listed_acq(root));
  assert(trace_nchild_acq(root) == 0);
  assert(trace_nextside_acq(root) == 0);
  assert(pointer_bits(
	la_loadptr_acq((void *const *)&root_exittab[PROBE_EXIT])) ==
	 pointer_bits(raw_fallback));
  assert(trace_exittarget_arm64_acq(root, PROBE_EXIT) == root_fallback);
  assert(snap_topslot_acq(&root_snap[PROBE_EXIT]) == PROBE_TOPSLOT);
  assert(snap_count_acq(&root_snap[PROBE_EXIT]) == SNAPCOUNT_DONE);
  assert(proto_trace_acq(pt) == PROBE_PARENT);
  assert(bc_op(loadbc(root_startpc)) == BC_JLOOP);
  assert(bc_d(loadbc(root_startpc)) == PROBE_PARENT);

  assert(trace_startpt_acq(child) == pt);
  assert(trace_startpc_acq(child) == continuation);
  assert(trace_ir_acq(child) == child_ir);
  assert(trace_snap_acq(child) == child_snap);
  assert(trace_snapmap_acq(child) == child_snapmap);
  assert(trace_mcode_acq(child) == child_mcode);
  assert(trace_exittab_acq(child) == child_exittab);
  assert(trace_exitstub_acq(child) == child_exitstub);
  assert(lj_arena_lifetime_state_acq(child_arena, child_cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(child_arena, child_cell) ==
	 LJ_ARENA_ROOT_MEMBER);
  assert(gc2_smr_readers_acq(g) == 0);
  assert(jit_token_acq(g) == 0);
  assert(jit_owner_l_acq(J) == NULL);
  assert(lj_trace_state_load(J) == LJ_TRACE_IDLE);

  expect_root_native_fallback(L, J);
  expect_root_native_fallback(L, J);
  assert(lj_trace_test_retire_publish_calls() == 1u);
  assert(trace_nchild_acq(root) == 0 && trace_nextside_acq(root) == 0);
  assert(pointer_bits(
	la_loadptr_acq((void *const *)&root_exittab[PROBE_EXIT])) ==
	 pointer_bits(raw_fallback));
  assert(gc2_smr_readers_acq(g) == 0);
  assert(jit_token_acq(g) == 0);
}

static void dump_probe(const LJTraceArm64FirstSidePublishProbe *probe)
{
  fprintf(stderr,
    "first-side publish state=%u attempts=%u publishes=%u failure=%u "
    "parent=%u child=%u exit=%u\n",
    (unsigned)probe->state, (unsigned)probe->attempts,
    (unsigned)probe->publishes, (unsigned)probe->failure,
    (unsigned)probe->parent, (unsigned)probe->child,
    (unsigned)probe->exitno);
}

static const char *ir_op_name(IROp op)
{
  switch (op) {
#define IR_DUMP_NAME(name, mode, op1, op2) case IR_##name: return #name;
  IRDEF(IR_DUMP_NAME)
#undef IR_DUMP_NAME
  default: return "?";
  }
}

static void dump_child_body(GCproto *pt, const GCtrace *child)
{
  IRIns *ir = trace_ir_acq(child);
  SnapShot *snap = trace_snap_acq(child);
  SnapEntry *map = trace_snapmap_acq(child);
  IRRef i, nk = trace_nk_acq(child), nins = trace_nins_acq(child);
  SnapNo s, nsnap = trace_nsnap_acq(child);
  MSize nmap = trace_nsnapmap_acq(child);
  MCode *mcode = trace_mcode_acq(child);
  MSize words = trace_szmcode_acq(child) / sizeof(MCode);

  fprintf(stderr,
    "child header trace=%u root=%u link=%u type=%u nins=%u nk=%u "
    "snap=%u map=%u top=%u sp=%u mcode=%p words=%u\n",
    (unsigned)trace_traceno_acq(child), (unsigned)trace_root_acq(child),
    (unsigned)trace_link_acq(child), (unsigned)trace_linktype_acq(child),
    (unsigned)nins, (unsigned)nk, (unsigned)nsnap, (unsigned)nmap,
    (unsigned)trace_topslot_acq(child),
    (unsigned)trace_spadjust_acq(child), (void *)mcode, (unsigned)words);
  for (i = nk; i < nins; i++) {
    IRIns ins = ir_load_acq(&ir[i]);
    fprintf(stderr,
      "  ir %d %-7s t=%#x r=%#x s=%#x op1=%u op2=%u raw=%#llx\n",
      (int)i-(int)REF_BIAS, ir_op_name((IROp)ins.o),
      (unsigned)ins.t.irt, (unsigned)ins.r, (unsigned)ins.s,
      (unsigned)ins.op1, (unsigned)ins.op2,
      (unsigned long long)ins.tv.u64);
  }
  for (s = 0; s < nsnap; s++) {
    SnapShot *ss = &snap[s];
    MSize ofs = snap_mapofs_acq(ss);
    MSize nent = snap_nent_acq(ss);
    MSize n;
    fprintf(stderr,
      "  snap %u ref=%u mcofs=%u ofs=%u nent=%u nslots=%u top=%u count=%u\n",
      (unsigned)s, (unsigned)snap_ref_acq(ss),
      (unsigned)snap_mcofs_acq(ss), (unsigned)ofs, (unsigned)nent,
      (unsigned)snap_nslots_acq(ss), (unsigned)snap_topslot_acq(ss),
      (unsigned)snap_count_acq(ss));
    for (n = 0; n < nent && ofs+n < nmap; n++) {
      SnapEntry entry = snapentry_acq(&map[ofs+n]);
      fprintf(stderr, "    map %u raw=%#x slot=%u ref=%u flags=%#x\n",
        (unsigned)(ofs+n), (unsigned)entry, (unsigned)snap_slot(entry),
        (unsigned)snap_ref(entry),
        (unsigned)(entry & (SnapEntry)0x00ff0000u));
    }
    if (ofs+nent < nmap) {
      const BCIns *pc = snap_pc_acq(&map[ofs+nent]);
      fprintf(stderr, "    pc=%p proto+%ld ins=%#x op=%u\n", (void *)pc,
        pt != NULL && pc != NULL ? (long)(pc-proto_bc(pt)) : -1L,
        pc != NULL ? (unsigned)loadbc(pc) : 0u,
        pc != NULL ? (unsigned)bc_op(loadbc(pc)) : 0u);
    }
  }
  for (i = 0; i < words; i++)
    fprintf(stderr, "  mcode %u %p %#08x\n", (unsigned)i,
      (void *)&mcode[i], (unsigned)mcode[i]);
}

int main(int argc, char **argv)
{
  const char *mode = argc > 1 ? argv[1] : "gc-claim";
  lua_State *L = luaL_newstate();
  jit_State *J;
  global_State *g;
  TGState *tg;
  GCproto *pt;
  GCtrace *root;
  GCtrace *child;
  SnapShot *root_snap;
  MCode **root_exittab;
  MCode *root_fallback;
  MCode *child_mcode;
  const BCIns *continuation;
  LJTraceArm64FirstSidePublishProbe probe;
  lua_Integer publication_result;
  void *saved_cframe;
  int32_t saved_vmstate;

  assert(argc <= 2);
  assert(strcmp(mode, "gc-claim") == 0 ||
	 strcmp(mode, "full-flush") == 0 ||
	 strcmp(mode, "scoped") == 0);
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
    "__arm64_first_side_publish_probe=f");

  /* Publish the independently admitted root, then prove ordinary side
  ** recording remains closed when the one-shot test seam is not armed. */
  assert(call_probe(L, 3, 0) == 3);
  pt = probe_proto(L);
  expect_root_shape(J, pt, &root, &continuation, &root_fallback);
  root_snap = trace_snap_acq(root);
  root_exittab = trace_exittab_acq(root);
  memset(&probe, 0, sizeof(probe));
  assert(!lj_trace_test_arm64_first_side_publish_read(&probe));
  assert(probe.state == LJ_TRACE_ARM64_FIRST_SIDE_PUBLISH_IDLE);
  assert(call_probe(L, 3, 1) == 4);
  assert(traceref_safe(J, PROBE_CHILD) == NULL);
  assert(trace_nchild_acq(root) == 0 && trace_nextside_acq(root) == 0);
  assert(trace_exittarget_arm64_acq(root, PROBE_EXIT) == root_fallback);

  /* The exact one-shot arm is the sole bypass of the production closed gate. */
  assert(lj_trace_test_arm64_first_side_publish_arm(
	J, PROBE_PARENT, PROBE_EXIT));
  memset(&probe, 0, sizeof(probe));
  assert(!lj_trace_test_arm64_first_side_publish_read(&probe));
  assert(probe.state == LJ_TRACE_ARM64_FIRST_SIDE_PUBLISH_ARMED);
  assert(probe.attempts == 0 && probe.publishes == 0 && probe.failure == 0);
  lj_trace_test_reset_exit_stats();
  publication_result = call_probe(L, 3, 1);
  if (publication_result != 4)
    fprintf(stderr,
      "first published-side call returned %lld exits=%u first=%u/%u "
      "last=%u/%u\n", (long long)publication_result,
      (unsigned)lj_trace_test_exit_calls(),
      (unsigned)lj_trace_test_first_exit_parent(),
      (unsigned)lj_trace_test_first_exitno(),
      (unsigned)lj_trace_test_last_exit_parent(),
      (unsigned)lj_trace_test_last_exitno());

  memset(&probe, 0, sizeof(probe));
  if (!lj_trace_test_arm64_first_side_publish_read(&probe))
    dump_probe(&probe);
  assert(probe.state == LJ_TRACE_ARM64_FIRST_SIDE_PUBLISH_DONE);
  assert(probe.attempts == 1);
  assert(probe.publishes == 1);
  assert(probe.failure == 0);
  assert(probe.parent == PROBE_PARENT);
  assert(probe.child == PROBE_CHILD);
  assert(probe.exitno == PROBE_EXIT);

  child = traceref_safe(J, PROBE_CHILD);
  expect_child_shape(J, pt, child, continuation);
  child_mcode = trace_mcode_acq(child);
  if (publication_result != 4) {
    fprintf(stderr, "publication call result=%lld expected=4\n",
      (long long)publication_result);
    dump_child_body(pt, child);
  }
  assert(publication_result == 4);

  /* The parent topology and selected exit form the exact publication edge. */
  assert(traceref_safe(J, PROBE_PARENT) == root);
  assert(trace_runnable_acq(root, PROBE_PARENT));
  assert(trace_nchild_acq(root) == 1);
  assert(trace_nextside_acq(root) == PROBE_CHILD);
  assert(snap_topslot_acq(&root_snap[PROBE_EXIT]) == PROBE_TOPSLOT);
  assert(snap_count_acq(&root_snap[PROBE_EXIT]) == SNAPCOUNT_DONE);
  assert(trace_exittarget_arm64_acq(root, PROBE_EXIT) == child_mcode);
  assert(pointer_bits(
	la_loadptr_acq((void *const *)&root_exittab[PROBE_EXIT])) ==
	 pointer_bits(trace_exittarget_arm64_encode(g, child_mcode)));

  /* All private transaction ownership is gone before DONE is observable. */
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

  /* Both executions must enter the published child and take its exact C exit;
  ** the ordinary side-of-side gate remains closed, so trace 3 never appears. */
  expect_child_native_exit(L, J);
  expect_child_native_exit(L, J);
  assert(traceref_safe(J, PROBE_CHILD) == child);
  assert(trace_runnable_acq(child, PROBE_CHILD));
  assert(trace_exittarget_arm64_acq(root, PROBE_EXIT) == child_mcode);
  assert(!lj_trace_test_arm64_first_side_publish_arm(
	J, PROBE_PARENT, PROBE_EXIT));

  /* Exercise independent production inverses in fresh fixture processes. */
  if (strcmp(mode, "full-flush") == 0) {
    expect_full_flush_first_side(L, J, g, pt, root, child, continuation,
	root_snap, root_exittab, root_fallback, child_mcode);
  } else if (strcmp(mode, "scoped") == 0) {
    expect_scoped_retired_first_side(L, J, g, pt, root, child,
	continuation, root_snap, root_exittab, root_fallback, child_mcode);
  } else {
    expect_retired_first_side(L, J, g, pt, root, child, continuation,
	root_snap, root_exittab, root_fallback, child_mcode);
  }

  lua_close(L);
  printf("t-arm64-jit-first-side-publish %s OK\n", mode);
  return 0;
}

#else

int main(int argc, char **argv)
{
  (void)argc; (void)argv;
  puts("t-arm64-jit-first-side-publish SKIP");
  return 0;
}

#endif
