/*
** macOS ARM64 recorder/publication contract for scalar FORL.
** Integer and NUM JFORL roots enter their certified native paths after the
** VM update; integer overflow recovery remains independently covered.
*/

#include <assert.h>
#include <math.h>
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
#include "lj_func.h"
#include "lj_ir.h"
#include "lj_asm.h"
#include "lj_jit.h"
#include "lj_snap.h"
#include "lj_target.h"
#include "lj_trace.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-forl-record requires executable ARM64 FORL gates"
#endif

enum {
  R_STOP = REF_FIRST,
  R_LIMIT_PROOF,
  R_IDX,
  R_SUM,
  R_SUM_PRE,
  R_IDX_PRE,
  R_PRECOND,
  R_LOOP,
  R_XPOLL,
  R_SUM_BODY,
  R_IDX_BODY,
  R_COND,
  R_IDX_PHI,
  R_SUM_PHI,
  R_RENAME,
  R_END
};

enum {
  N_IDX = REF_FIRST,
  N_SUM,
  N_SUM_PRE,
  N_IDX_PRE,
  N_PRECOND,
  N_LOOP,
  N_XPOLL,
  N_SUM_BODY,
  N_IDX_BODY,
  N_COND,
  N_IDX_PHI,
  N_SUM_PHI,
  N_RENAME,
  N_END
};

enum {
  D_STOP = REF_FIRST,
  D_STEP,
  D_DIRECTION,
  D_OVERFLOW,
  D_USE,
  D_IDX,
  D_SUM,
  D_SUM_PRE,
  D_IDX_PRE,
  D_PRECOND,
  D_LOOP,
  D_XPOLL,
  D_SUM_BODY,
  D_IDX_BODY,
  D_COND,
  D_IDX_PHI,
  D_SUM_PHI,
  D_RENAME,
  D_END
};

static const IRRef positive_snaprefs[] = {
  R_STOP, R_SUM_PRE, R_PRECOND, R_LOOP, R_SUM_BODY, R_COND
};

static const uint8_t positive_mapofs[] = { 0, 2, 10, 13, 20, 29 };
static const uint8_t positive_nent[] = { 0, 6, 1, 5, 7, 1 };
static const uint8_t positive_nslots[] = { 2, 10, 4, 8, 10, 4 };

static const IRRef dynamic_snaprefs[] = {
  D_STOP, D_SUM_PRE, D_PRECOND, D_LOOP, D_SUM_BODY, D_COND
};

static const uint8_t dynamic_nslots[] = { 2, 12, 6, 10, 12, 6 };

static void run_lua(lua_State *L, const char *chunk)
{
  int status = luaL_dostring(L, chunk);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 FORL record chunk failed: %s\n",
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

static lua_Integer call1(lua_State *L, const char *name, lua_Integer arg)
{
  int status;
  lua_Integer result;
  void *cframe = L->cframe;
  lua_getglobal(L, name);
  lua_pushinteger(L, arg);
  status = lua_pcall(L, 1, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 FORL call failed: %s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == cframe);
  return result;
}

static lua_Number callnum1(lua_State *L, const char *name, lua_Number arg)
{
  int status;
  lua_Number result;
  void *cframe = L->cframe;
  lua_getglobal(L, name);
  lua_pushnumber(L, arg);
  status = lua_pcall(L, 1, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 numeric FORL call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == cframe);
  return result;
}

static lua_Number callnum3(lua_State *L, const char *name, lua_Number a,
	lua_Number b, lua_Number c)
{
  int status;
  lua_Number result;
  void *cframe = L->cframe;
  lua_getglobal(L, name);
  lua_pushnumber(L, a);
  lua_pushnumber(L, b);
  lua_pushnumber(L, c);
  status = lua_pcall(L, 3, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 dynamic NUM FORL call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == cframe);
  return result;
}

static lua_Number callint3num(lua_State *L, const char *name,
	lua_Integer a, lua_Integer b, lua_Integer c)
{
  int status;
  lua_Number result;
  void *cframe = L->cframe;
  lua_getglobal(L, name);
  lua_pushinteger(L, a);
  lua_pushinteger(L, b);
  lua_pushinteger(L, c);
  status = lua_pcall(L, 3, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 INT FORL transition failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tonumber(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == cframe);
  return result;
}

static lua_Integer call3(lua_State *L, const char *name, lua_Integer a,
	lua_Integer b, lua_Integer step)
{
  int status;
  lua_Integer result;
  void *cframe = L->cframe;
  lua_getglobal(L, name);
  lua_pushinteger(L, a);
  lua_pushinteger(L, b);
  lua_pushinteger(L, step);
  status = lua_pcall(L, 3, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 dynamic FORL call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == cframe);
  return result;
}

static lua_Integer call2(lua_State *L, const char *name, lua_Integer a,
	lua_Integer b)
{
  int status;
  lua_Integer result;
  void *cframe = L->cframe;
  lua_getglobal(L, name);
  lua_pushinteger(L, a);
  lua_pushinteger(L, b);
  status = lua_pcall(L, 2, 1, 0);
  if (status != LUA_OK) {
    fprintf(stderr, "ARM64 two-argument FORL call failed: %s\n",
	    lua_tostring(L, -1));
    assert(status == LUA_OK);
  }
  assert(lua_isnumber(L, -1));
  result = lua_tointeger(L, -1);
  lua_pop(L, 1);
  assert(L->cframe == cframe);
  return result;
}

static GCtrace *expect_published_forl(lua_State *L, GCproto *pt);
static void expect_native_entry(void);
static void reset_native_activity(void);

static IRRef find_kint(const GCtrace *T, int32_t value)
{
  IRIns *ir = trace_ir_acq(T);
  IRRef ref;
  for (ref = trace_nk_acq(T); ref < REF_TRUE; ref++)
    if (ir[ref].o == IR_KINT && ir[ref].t.irt == IRT_INT &&
	ir[ref].i == value)
      return ref;
  return 0;
}

static void expect_ir(const IRIns *ir, IRRef ref, IROp op, uint8_t type,
	IRRef op1, IRRef op2)
{
  assert(ir[ref].o == op);
  assert(ir[ref].t.irt == type);
  assert(ir[ref].op1 == op1);
  assert(ir[ref].op2 == op2);
}

static void expect_forl_pair(const GCtrace *T, const GCproto *pt)
{
  const BCIns *bc = proto_bc(pt);
  const BCIns *forlpc = trace_startpc_acq(T);
  BCIns forl = trace_startins_acq(T);
  BCIns patched, fori;
  int64_t pos, bodypos, foripos, exitpos;

  assert(forlpc >= bc && forlpc < bc+pt->sizebc);
  assert(bc_op(forl) == BC_FORL);
  assert(bc_j(forl) < 0);
  assert((MSize)bc_a(forl)+FORL_EXT < (MSize)pt->framesize);
  pos = (int64_t)proto_bcpos(pt, forlpc);
  bodypos = pos+1+(int64_t)bc_j(forl);
  foripos = bodypos-1;
  assert(bodypos > 0 && bodypos <= pos);
  assert(foripos >= 0 && foripos < (int64_t)pt->sizebc);
  fori = (BCIns)la_load32_acq(
    (const uint32_t *)&bc[(BCPos)foripos]);
  assert(bc_op(fori) == BC_FORI);
  assert(bc_a(fori) == bc_a(forl));
  assert(bc_j(fori) > 0);
  exitpos = foripos+1+(int64_t)bc_j(fori);
  assert(exitpos == pos+1);
  patched = (BCIns)la_load32_acq((const uint32_t *)forlpc);
  assert(bc_op(patched) == BC_JFORL);
  assert(bc_a(patched) == bc_a(forl));
  assert((TraceNo)bc_d(patched) == trace_traceno_acq(T));
  assert(proto_jit_startins_acq(pt, forlpc) == forl);
}

static void expect_positive_ir(const GCtrace *T, const GCproto *pt)
{
  IRIns *ir = trace_ir_acq(T);
  IRRef one = find_kint(T, 1);
  IRRef limit = find_kint(T, INT32_MAX-1);
  IRRef ref;
  assert(one != 0 && limit != 0);
  assert(trace_nins_acq(T) == R_END);
  expect_ir(ir, REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  expect_ir(ir, R_STOP, IR_SLOAD, IRT_INT,
	    5, IRSLOAD_READONLY|IRSLOAD_INHERIT);
  expect_ir(ir, R_LIMIT_PROOF, IR_LE, IRT_INT|IRT_GUARD,
	    R_STOP, limit);
  expect_ir(ir, R_IDX, IR_SLOAD, IRT_INT|IRT_GUARD,
	    4, IRSLOAD_TYPECHECK|IRSLOAD_INHERIT);
  expect_ir(ir, R_SUM, IR_SLOAD, IRT_INT|IRT_GUARD,
	    3, IRSLOAD_TYPECHECK);
  expect_ir(ir, R_SUM_PRE, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, R_SUM, R_IDX);
  expect_ir(ir, R_IDX_PRE, IR_ADD, IRT_INT|IRT_ISPHI, R_IDX, one);
  expect_ir(ir, R_PRECOND, IR_LE, IRT_INT|IRT_GUARD, R_IDX_PRE, R_STOP);
  expect_ir(ir, R_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  expect_ir(ir, R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  expect_ir(ir, R_SUM_BODY, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, R_IDX_PRE, R_SUM_PRE);
  expect_ir(ir, R_IDX_BODY, IR_ADD, IRT_INT|IRT_ISPHI, R_IDX_PRE, one);
  expect_ir(ir, R_COND, IR_LE, IRT_INT|IRT_GUARD, R_IDX_BODY, R_STOP);
  expect_ir(ir, R_IDX_PHI, IR_PHI, IRT_INT, R_IDX_PRE, R_IDX_BODY);
  expect_ir(ir, R_SUM_PHI, IR_PHI, IRT_INT, R_SUM_PRE, R_SUM_BODY);
  expect_ir(ir, R_RENAME, IR_RENAME, IRT_NIL, R_SUM_PRE, 3);
  assert(ir[R_RENAME].r < RID_MAX_GPR);
  assert(!ra_hasspill(ir[R_RENAME].s));
  for (ref = REF_BASE; ref < trace_nins_acq(T); ref++)
    assert(!ra_hasspill(ir[ref].s));
  assert(trace_spadjust_acq(T) == 0);
  assert(trace_topslot_acq(T) == (MSize)pt->framesize);
}

static void expect_positive_snapshots(const GCtrace *T,
	const GCproto *pt)
{
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *map = trace_snapmap_acq(T);
  SnapNo sn;
  assert(trace_nsnap_acq(T) == 6);
  assert(trace_nsnapmap_acq(T) == 32);
  for (sn = 0; sn < 6; sn++) {
    MSize n;
    MSize end = sn+1u < 6 ? positive_mapofs[sn+1u] : 32;
    uint64_t pcbase;
    const BCIns *pc;
    assert(snap_ref_acq(&snap[sn]) == positive_snaprefs[sn]);
    assert(snap_mapofs_acq(&snap[sn]) == positive_mapofs[sn]);
    assert(snap_nent_acq(&snap[sn]) == positive_nent[sn]);
    assert(snap_nslots_acq(&snap[sn]) == positive_nslots[sn]);
    assert(snap_topslot_acq(&snap[sn]) == (MSize)pt->framesize);
    assert(end-positive_mapofs[sn]-positive_nent[sn] == 1u+LJ_FR2);
    for (n = 1; n < positive_nent[sn]; n++)
      assert(snap_slot(map[positive_mapofs[sn]+n-1u]) <
	     snap_slot(map[positive_mapofs[sn]+n]));
    memcpy(&pcbase, &map[positive_mapofs[sn]+positive_nent[sn]],
	   sizeof(pcbase));
    assert((uint8_t)pcbase == 0);
    pc = (const BCIns *)(uintptr_t)(pcbase >> 8);
    assert(pc >= proto_bc(pt) && pc < proto_bc(pt)+pt->sizebc);
  }

  assert(map[2] == SNAP(4, SNAP_NORESTORE, R_IDX));
  assert(map[3] == SNAP(5, SNAP_NORESTORE, R_STOP));
  assert(map[5] == SNAP(7, 0, R_IDX));
  assert(map[6] == SNAP(8, 0, R_SUM));
  assert(map[7] == SNAP(9, 0, R_IDX));
  assert(map[10] == SNAP(3, 0, R_SUM_PRE));
  assert(map[13] == SNAP(3, 0, R_SUM_PRE));
  assert(map[14] == SNAP(4, 0, R_IDX_PRE));
  assert(map[15] == SNAP(5, SNAP_NORESTORE, R_STOP));
  assert(map[17] == SNAP(7, 0, R_IDX_PRE));
  assert(map[20] == SNAP(3, 0, R_SUM_PRE));
  assert(map[21] == SNAP(4, 0, R_IDX_PRE));
  assert(map[22] == SNAP(5, 0, R_STOP));
  assert(map[24] == SNAP(7, 0, R_IDX_PRE));
  assert(map[25] == SNAP(8, 0, R_SUM_PRE));
  assert(map[26] == SNAP(9, 0, R_IDX_PRE));
  assert(map[29] == SNAP(3, 0, R_SUM_BODY));
}

static void expect_dynamic_ir(const GCtrace *T, const GCproto *pt,
	int negative)
{
  IRIns *ir = trace_ir_acq(T);
  IRRef zero = find_kint(T, 0);
  MSize idxslot = (MSize)(1u+LJ_FR2+bc_a(trace_startins_acq(T)));
  IROp direction = negative ? IR_LT : IR_GE;
  IROp compare = negative ? IR_GE : IR_LE;
  IRRef ref;
  assert(zero != 0);
  assert(trace_nins_acq(T) == D_END);
  expect_ir(ir, REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  expect_ir(ir, D_STOP, IR_SLOAD, IRT_INT,
	    (IRRef)(idxslot+FORL_STOP),
	    IRSLOAD_READONLY|IRSLOAD_INHERIT);
  expect_ir(ir, D_STEP, IR_SLOAD, IRT_INT,
	    (IRRef)(idxslot+FORL_STEP),
	    IRSLOAD_READONLY|IRSLOAD_INHERIT);
  expect_ir(ir, D_DIRECTION, direction, IRT_INT|IRT_GUARD,
	    D_STEP, zero);
  expect_ir(ir, D_OVERFLOW, IR_ADDOV, IRT_INT|IRT_GUARD,
	    D_STEP, D_STOP);
  expect_ir(ir, D_USE, IR_USE, IRT_INT, D_OVERFLOW, 0);
  expect_ir(ir, D_IDX, IR_SLOAD, IRT_INT|IRT_GUARD, idxslot,
	    IRSLOAD_TYPECHECK|IRSLOAD_INHERIT);
  expect_ir(ir, D_SUM, IR_SLOAD, IRT_INT|IRT_GUARD,
	    (IRRef)(idxslot-1u), IRSLOAD_TYPECHECK);
  expect_ir(ir, D_SUM_PRE, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, D_SUM, D_IDX);
  expect_ir(ir, D_IDX_PRE, IR_ADD, IRT_INT|IRT_ISPHI,
	    D_IDX, D_STEP);
  expect_ir(ir, D_PRECOND, compare, IRT_INT|IRT_GUARD,
	    D_IDX_PRE, D_STOP);
  expect_ir(ir, D_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  expect_ir(ir, D_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  expect_ir(ir, D_SUM_BODY, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, D_IDX_PRE, D_SUM_PRE);
  expect_ir(ir, D_IDX_BODY, IR_ADD, IRT_INT|IRT_ISPHI,
	    D_IDX_PRE, D_STEP);
  expect_ir(ir, D_COND, compare, IRT_INT|IRT_GUARD,
	    D_IDX_BODY, D_STOP);
  expect_ir(ir, D_IDX_PHI, IR_PHI, IRT_INT, D_IDX_PRE, D_IDX_BODY);
  expect_ir(ir, D_SUM_PHI, IR_PHI, IRT_INT, D_SUM_PRE, D_SUM_BODY);
  expect_ir(ir, D_RENAME, IR_RENAME, IRT_NIL, D_SUM_PRE, 3);
  assert(ir[D_USE].r == RID_INIT && !ra_hasspill(ir[D_USE].s));
  assert(ir[D_OVERFLOW].r < RID_MAX_GPR &&
	 rset_test(RSET_GPR, ir[D_OVERFLOW].r) &&
	 !ra_hasspill(ir[D_OVERFLOW].s));
  for (ref = REF_BASE; ref < trace_nins_acq(T); ref++)
    assert(!ra_hasspill(ir[ref].s));
  assert(trace_spadjust_acq(T) == 0);
  assert(trace_topslot_acq(T) == (MSize)pt->framesize);
}

static void expect_dynamic_snapshots(const GCtrace *T,
	const GCproto *pt)
{
  SnapShot *snap = trace_snap_acq(T);
  SnapEntry *map = trace_snapmap_acq(T);
  SnapNo sn;
  assert(trace_nsnap_acq(T) == 6);
  assert(trace_nsnapmap_acq(T) == 32);
  for (sn = 0; sn < 6; sn++) {
    MSize n;
    MSize end = sn+1u < 6 ? positive_mapofs[sn+1u] : 32;
    uint64_t pcbase;
    const BCIns *pc;
    assert(snap_ref_acq(&snap[sn]) == dynamic_snaprefs[sn]);
    assert(snap_mapofs_acq(&snap[sn]) == positive_mapofs[sn]);
    assert(snap_nent_acq(&snap[sn]) == positive_nent[sn]);
    assert(snap_nslots_acq(&snap[sn]) == dynamic_nslots[sn]);
    assert(snap_topslot_acq(&snap[sn]) == (MSize)pt->framesize);
    assert(end-positive_mapofs[sn]-positive_nent[sn] == 1u+LJ_FR2);
    for (n = 1; n < positive_nent[sn]; n++)
      assert(snap_slot(map[positive_mapofs[sn]+n-1u]) <
	     snap_slot(map[positive_mapofs[sn]+n]));
    memcpy(&pcbase, &map[positive_mapofs[sn]+positive_nent[sn]],
	   sizeof(pcbase));
    assert((uint8_t)pcbase == 0);
    pc = (const BCIns *)(uintptr_t)(pcbase >> 8);
    assert(pc >= proto_bc(pt) && pc < proto_bc(pt)+pt->sizebc);
  }

  assert(map[2] == SNAP(6, SNAP_NORESTORE, D_IDX));
  assert(map[3] == SNAP(7, SNAP_NORESTORE, D_STOP));
  assert(map[4] == SNAP(8, SNAP_NORESTORE, D_STEP));
  assert(map[5] == SNAP(9, 0, D_IDX));
  assert(map[6] == SNAP(10, 0, D_SUM));
  assert(map[7] == SNAP(11, 0, D_IDX));
  assert(map[10] == SNAP(5, 0, D_SUM_PRE));
  assert(map[13] == SNAP(5, 0, D_SUM_PRE));
  assert(map[14] == SNAP(6, 0, D_IDX_PRE));
  assert(map[15] == SNAP(7, SNAP_NORESTORE, D_STOP));
  assert(map[16] == SNAP(8, SNAP_NORESTORE, D_STEP));
  assert(map[17] == SNAP(9, 0, D_IDX_PRE));
  assert(map[20] == SNAP(5, 0, D_SUM_PRE));
  assert(map[21] == SNAP(6, 0, D_IDX_PRE));
  assert(map[22] == SNAP(7, 0, D_STOP));
  assert(map[23] == SNAP(8, 0, D_STEP));
  assert(map[24] == SNAP(9, 0, D_IDX_PRE));
  assert(map[25] == SNAP(10, 0, D_SUM_PRE));
  assert(map[26] == SNAP(11, 0, D_IDX_PRE));
  assert(map[29] == SNAP(5, 0, D_SUM_BODY));
}

static void expect_numeric_forl_shape(const GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  MSize idxslot = 1u+LJ_FR2+bc_a(trace_startins_acq(T));
  IRRef ref, idx = 0, pre = 0, post = 0, loop = 0, poll = 0;
  IRRef precmp = 0, postcmp = 0, phi = 0;
  for (ref = REF_FIRST; ref < trace_nins_acq(T); ref++) {
    IRIns x = ir[ref];
    if (x.o == IR_SLOAD && x.op1 == idxslot) {
      assert(idx == 0 && x.t.irt == (IRT_NUM|IRT_GUARD));
      assert(x.op2 == (IRSLOAD_TYPECHECK|IRSLOAD_INHERIT));
      idx = ref;
    }
    if (x.o == IR_LOOP) { assert(loop == 0); loop = ref; }
    if (x.o == IR_XPOLL) { assert(poll == 0); poll = ref; }
  }
  assert(idx && loop && poll == loop+1u);
  for (ref = REF_FIRST; ref < trace_nins_acq(T); ref++) {
    IRIns x = ir[ref];
    if (x.o == IR_ADD && x.t.irt == (IRT_NUM|IRT_ISPHI) &&
        x.op1 == idx && ref < loop) { assert(pre == 0); pre = ref; }
    if (x.o == IR_ADD && x.t.irt == (IRT_NUM|IRT_ISPHI) && pre &&
        x.op1 == pre && x.op2 == ir[pre].op2 && ref > poll) {
      assert(post == 0); post = ref;
    }
  }
  assert(pre && post && ir[post].op2 == ir[pre].op2);
  assert(ir[ir[pre].op2].o == IR_KNUM);
  for (ref = pre+1u; ref < loop; ref++)
    if ((ir[ref].o == IR_LE || ir[ref].o == IR_GE) &&
        ir[ref].t.irt == (IRT_NUM|IRT_GUARD) && ir[ref].op1 == pre) {
      assert(precmp == 0); precmp = ref;
    }
  for (ref = poll+1u; ref < trace_nins_acq(T); ref++)
    if ((ir[ref].o == IR_LE || ir[ref].o == IR_GE) &&
        ir[ref].t.irt == (IRT_NUM|IRT_GUARD) && ir[ref].op1 == post &&
        ir[ref].op2 == ir[precmp].op2) {
      assert(postcmp == 0); postcmp = ref;
    }
  assert(precmp && postcmp && ir[precmp].o == IR_LE &&
         ir[postcmp].o == IR_LE && ir[precmp].op2 == ir[postcmp].op2);
  for (ref = postcmp+1u; ref < trace_nins_acq(T); ref++)
    if (ir[ref].o == IR_PHI && ir[ref].op1 == pre && ir[ref].op2 == post) {
      assert(phi == 0); phi = ref;
    }
  assert(phi && ir[phi].t.irt == IRT_NUM);
}

typedef struct NumericForlAdmissionCopy {
  GCtrace trace;
  jit_State J;
  IRIns *ir;
  SnapShot *snap;
  SnapEntry *snapmap;
  LJArm64PostRAView post;
  IRRef semantic_nins;
} NumericForlAdmissionCopy;

static void numeric_forl_admission_copy_init(NumericForlAdmissionCopy *copy,
					     lua_State *L, const GCtrace *source)
{
  GCproto *pt = trace_startpt_acq((GCtrace *)source);
  IRRef ref, loop = 0, semantic_nins = 0;
  assert(copy && source && pt);
  memset(copy, 0, sizeof(*copy));
  copy->trace = *source;
  copy->ir = calloc(source->nins, sizeof(*copy->ir));
  copy->snap = malloc(source->nsnap * sizeof(*copy->snap));
  copy->snapmap = malloc(source->nsnapmap * sizeof(*copy->snapmap));
  assert(copy->ir && copy->snap && copy->snapmap);
  /* T->ir is biased; only [nk,nins) is an allocated range. */
  memcpy(copy->ir + source->nk, trace_ir_acq(source) + source->nk,
         (size_t)(source->nins - source->nk) * sizeof(*copy->ir));
  memcpy(copy->snap, trace_snap_acq((GCtrace *)source),
         source->nsnap * sizeof(*copy->snap));
  memcpy(copy->snapmap, trace_snapmap_acq((GCtrace *)source),
         source->nsnapmap * sizeof(*copy->snapmap));
  copy->trace.ir = copy->ir;
  copy->trace.snap = copy->snap;
  copy->trace.snapmap = copy->snapmap;
  for (ref = REF_FIRST; ref < source->nins; ref++)
    if (copy->ir[ref].o == IR_LOOP) { assert(!loop); loop = ref; }
  assert(loop);
  copy->J = *L2J(L);
  copy->J.pt = pt;
  copy->J.loopref = loop;
  copy->J.startpc = (BCIns *)(void *)trace_startpc_acq((GCtrace *)source);

  copy->post.ir = copy->ir;
  copy->post.owner = &copy->trace;
  copy->post.snap = copy->snap;
  copy->post.snapmap = copy->snapmap;
  copy->post.proto = pt;
  copy->post.proto_bc = proto_bc(pt);
  copy->post.startpc = copy->J.startpc;
  copy->post.nins = source->nins;
  copy->post.nk = source->nk;
  copy->post.nsnap = source->nsnap;
  copy->post.nsnapmap = source->nsnapmap;
  copy->post.spadjust = source->spadjust;
  copy->post.proto_sizebc = pt->sizebc;
  copy->post.proto_numparams = pt->numparams;
  copy->post.root_topslot = pt->framesize;
  copy->post.startins = source->startins;
  copy->post.base_delta = 0;
  assert(lj_asm_arm64_forl_postra_admit(&copy->post, &semantic_nins, NULL));
  assert(semantic_nins > REF_FIRST && semantic_nins <= source->nins);
  copy->semantic_nins = semantic_nins;
  copy->trace.nins = semantic_nins;
}

static void numeric_forl_admission_copy_free(NumericForlAdmissionCopy *copy)
{
  free(copy->ir);
  free(copy->snap);
  free(copy->snapmap);
}

/* Each validator independently rejects a dynamic NUM loop whose ordered
** comparator uses a different, valid NUM SLOAD as its bound. */
static void test_numeric_forl_bound_binding(lua_State *L, const GCtrace *T)
{
  NumericForlAdmissionCopy copy;
  IRRef ref, idx = 0, stop = 0, pre = 0, post = 0, loop = 0;
  IRRef precmp = 0, postcmp = 0;
  MSize idxslot = 1u+LJ_FR2+bc_a(T->startins);
  const BCIns *sourcepc = trace_startpc_acq((GCtrace *)T);
  BCIns savedpc = (BCIns)la_load32_acq((const uint32_t *)sourcepc);
  IRRef semantic_nins;
  int direction;
  LJArm64IRReject reject;
  /* The published prototype has JFORL installed. Admission replays the
  ** original FORL source instruction while this synchronous test owns Lua. */
  la_store32_rel((uint32_t *)(void *)sourcepc, T->startins);
  numeric_forl_admission_copy_init(&copy, L, T);
  for (ref = REF_FIRST; ref < copy.post.nins; ref++) {
    IRIns x = copy.ir[ref];
    if (x.o == IR_LOOP) loop = ref;
    if (x.o == IR_SLOAD && x.op1 == idxslot &&
        x.t.irt == (IRT_NUM|IRT_GUARD)) idx = ref;
    if (x.o == IR_SLOAD && x.op1 == idxslot+FORL_STOP &&
        x.t.irt == IRT_NUM) stop = ref;
  }
  assert(loop && idx);
  for (ref = REF_FIRST; ref < loop; ref++) {
    IRIns x = copy.ir[ref];
    if (x.o == IR_ADD && x.t.irt == (IRT_NUM|IRT_ISPHI) &&
        x.op1 == idx) pre = ref;
    if ((x.o == IR_LE || x.o == IR_GE) &&
        x.t.irt == (IRT_NUM|IRT_GUARD) && x.op1 == pre && pre)
      precmp = ref;
  }
  for (ref = loop+2u; ref < copy.post.nins; ref++) {
    IRIns x = copy.ir[ref];
    if (x.o == IR_ADD && x.t.irt == (IRT_NUM|IRT_ISPHI) &&
        x.op1 == pre) post = ref;
    if ((x.o == IR_LE || x.o == IR_GE) &&
        x.t.irt == (IRT_NUM|IRT_GUARD) && x.op1 == post && post)
      postcmp = ref;
  }
  assert(pre && post && precmp && postcmp);
  if (!(stop == copy.ir[precmp].op2 && stop >= REF_FIRST && stop < pre &&
        stop != idx && copy.ir[stop].o == IR_SLOAD &&
        copy.ir[stop].t.irt == IRT_NUM)) {
    fprintf(stderr, "NUM FORL bound fixture: idxslot=%u idx=%u stop=%u pre=%u cmp=%u op2=%u\n",
            (unsigned)idxslot, (unsigned)idx, (unsigned)stop,
            (unsigned)pre, (unsigned)precmp,
            (unsigned)copy.ir[precmp].op2);
    fprintf(stderr, "  predicates: eq=%d ref=%d before=%d distinct=%d op=%u type=%u wanttype=%u\n",
            stop == copy.ir[precmp].op2, stop > REF_FIRST, stop < pre,
            stop != idx, (unsigned)copy.ir[stop].o,
            (unsigned)copy.ir[stop].t.irt, (unsigned)IRT_NUM);
    for (ref = REF_FIRST; ref < loop; ref++) {
      IRIns x = copy.ir[ref];
      if (x.o == IR_SLOAD || x.o == IR_LE || x.o == IR_GE ||
          (x.o == IR_ADD && irt_type(x.t) == IRT_NUM))
        fprintf(stderr, "  r%u op=%u type=%u op1=%u op2=%u\n",
                (unsigned)ref, (unsigned)x.o, (unsigned)x.t.irt,
                (unsigned)x.op1, (unsigned)x.op2);
    }
    abort();
  }

  /* Prove both independent authorities accept the untouched real trace. */
  if (!lj_asm_arm64_forl_ir_admit(&copy.J, &copy.trace, &reject, &direction)) {
    fprintf(stderr, "NUM FORL private semantic baseline reject=%u ref=%u op=%u detail=%u nins=%u sem=%u start=%u\n",
            (unsigned)reject.reason, (unsigned)reject.ref,
            (unsigned)reject.op, (unsigned)reject.detail,
            (unsigned)copy.post.nins, (unsigned)copy.semantic_nins,
            (unsigned)bc_op(copy.trace.startins));
    abort();
  }
  assert(direction == 1 || direction == -1);
  assert(lj_asm_arm64_forl_postra_admit(&copy.post, &semantic_nins,
                                        &direction));
  assert(direction == 1 || direction == -1);

  /* The alternative is another in-range, guarded NUM SLOAD, not malformed
  ** IR. Keep semantic and allocated copies independently mutated. */
  copy.ir[precmp].op2 = (IRRef1)idx;
  copy.ir[postcmp].op2 = (IRRef1)idx;
  assert(!lj_asm_arm64_forl_ir_admit(&copy.J, &copy.trace, NULL, &direction));
  numeric_forl_admission_copy_free(&copy);

  numeric_forl_admission_copy_init(&copy, L, T);
  for (ref = REF_FIRST; ref < copy.post.nins; ref++) {
    IRIns x = copy.ir[ref];
    if (x.o == IR_LOOP) loop = ref;
    if (x.o == IR_SLOAD && x.op1 == idxslot &&
        x.t.irt == (IRT_NUM|IRT_GUARD)) idx = ref;
  }
  pre = precmp = 0;
  for (ref = REF_FIRST; ref < loop; ref++) {
    IRIns x = copy.ir[ref];
    if (x.o == IR_ADD && x.t.irt == (IRT_NUM|IRT_ISPHI) &&
        x.op1 == idx) pre = ref;
    if ((x.o == IR_LE || x.o == IR_GE) &&
        x.t.irt == (IRT_NUM|IRT_GUARD) && x.op1 == pre && pre)
      precmp = ref;
  }
  assert(pre && precmp && copy.ir[precmp].op2 == stop);
  post = postcmp = 0;
  for (ref = loop+2u; ref < copy.post.nins; ref++) {
    IRIns x = copy.ir[ref];
    if (x.o == IR_ADD && x.t.irt == (IRT_NUM|IRT_ISPHI) &&
        x.op1 == pre) post = ref;
    if ((x.o == IR_LE || x.o == IR_GE) &&
        x.t.irt == (IRT_NUM|IRT_GUARD) && x.op1 == post && post)
      postcmp = ref;
  }
  assert(post && postcmp && copy.ir[postcmp].op2 == stop);
  copy.ir[precmp].op2 = (IRRef1)idx;
  copy.ir[postcmp].op2 = (IRRef1)idx;
  copy.trace.nins = copy.post.nins;
  assert(!lj_asm_arm64_forl_postra_admit(&copy.post, &semantic_nins,
                                         &direction));
  numeric_forl_admission_copy_free(&copy);
  la_store32_rel((uint32_t *)(void *)sourcepc, savedpc);
}

static void run_numeric(void)
{
  lua_State *L = luaL_newstate();
  GCproto *pt;
  GCtrace *T;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.opt.start('hotloop=2','hotexit=1000','maxtrace=128')\n"
    "function __arm64_forl_numeric(n)\n"
    "  local s = 0.0\n"
    "  for i = 0.5, n, 0.5 do s = s + i end\n"
    "  return s\n"
    "end\n");
  pt = global_proto(L, "__arm64_forl_numeric");
  reset_native_activity();
  assert(callnum1(L, "__arm64_forl_numeric", 3.5) == 14.0);
  T = expect_published_forl(L, pt);
  assert(trace_arm64_admission_acq(T) ==
	 (TRACE_ARM64_NUM_FORL_ADMITTED | TRACE_ARM64_NUM_FORL_ASCENDING));
  expect_numeric_forl_shape(T);
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);

  /* Terminal exits restore the loop through the exact NUM comparison map. */
  reset_native_activity();
  assert(callnum1(L, "__arm64_forl_numeric", 4.5) == 22.5);
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  run_lua(L, "collectgarbage('collect')\n");
  reset_native_activity();
  assert(callnum1(L, "__arm64_forl_numeric", 3.5) == 14.0);
  T = expect_published_forl(L, pt);
  assert(trace_arm64_admission_acq(T) ==
	 (TRACE_ARM64_NUM_FORL_ADMITTED | TRACE_ARM64_NUM_FORL_ASCENDING));
  expect_numeric_forl_shape(T);
  expect_native_entry();
  lua_close(L);
}

static void expect_mode_rejected(TGState *tg);
static GCtrace *expect_published_forl(lua_State *L, GCproto *pt);
static void expect_branch_only(void);
static void expect_native_entry(void);
static void reset_native_activity(void);

static void run_numeric_dynamic(int negative)
{
  lua_State *L = luaL_newstate();
  GCproto *pt;
  GCtrace *T;
  lua_Number start = negative ? 4.5 : 0.5;
  lua_Number stop = negative ? 0.5 : 3.5;
  lua_Number step = negative ? -0.5 : 0.5;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.opt.start('hotloop=2','hotexit=1000','maxtrace=128')\n"
    "function __arm64_forl_numeric_dynamic(start, stop, step)\n"
    "  local s = 0.0\n"
    "  for i = start, stop, step do s = s + i end\n"
    "  return s\n"
    "end\n"
    "function __arm64_forl_numeric_oracle(start, stop, step)\n"
    "  local s = 0.0\n"
    "  for i = start, stop, step do s = s + i end\n"
    "  return s\n"
    "end\n");
  run_lua(L, "jit.off(__arm64_forl_numeric_oracle, true)\n");
  pt = global_proto(L, "__arm64_forl_numeric_dynamic");
  reset_native_activity();
  assert(callnum3(L, "__arm64_forl_numeric_dynamic", start, stop, step) ==
	 (negative ? 22.5 : 14.0));
  T = expect_published_forl(L, pt);
  assert(trace_arm64_admission_acq(T) ==
	 (TRACE_ARM64_NUM_FORL_ADMITTED |
	  (negative ? TRACE_ARM64_NUM_FORL_DESCENDING :
		      TRACE_ARM64_NUM_FORL_ASCENDING)));
  test_numeric_forl_bound_binding(L, T);
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);

  reset_native_activity();
  stop = negative ? -0.5 : 4.5;
  assert(callnum3(L, "__arm64_forl_numeric_dynamic", start, stop, step) ==
	 (negative ? 22.0 : 22.5));
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  run_lua(L, "collectgarbage('collect')\n");
  reset_native_activity();
  stop = negative ? 0.5 : 3.5;
  assert(callnum3(L, "__arm64_forl_numeric_dynamic", start, stop, step) ==
	 (negative ? 22.5 : 14.0));
  T = expect_published_forl(L, pt);
  assert(trace_arm64_admission_acq(T) ==
	 (TRACE_ARM64_NUM_FORL_ADMITTED |
	  (negative ? TRACE_ARM64_NUM_FORL_DESCENDING :
		      TRACE_ARM64_NUM_FORL_ASCENDING)));
  expect_native_entry();
  if (!negative) {
    reset_native_activity();
	assert(callint3num(L, "__arm64_forl_numeric_dynamic", 1, 5, 1) ==
	   15.0);
    assert(lj_trace_test_root_entry_publishes() != 0);
    assert(lj_trace_test_root_entry_cleanups() ==
	   lj_trace_test_root_entry_publishes());
    assert(lj_trace_test_exit_calls() == 0);
    assert(trace_runnable_acq(T, 1));
    /* The VM chooses a descending edge for this call. The previously
    ** compiled ascending root must be cleaned up before it can transfer. */
    reset_native_activity();
    assert(callnum3(L, "__arm64_forl_numeric_dynamic", 4.5, 0.5, -0.5) ==
	   callnum3(L, "__arm64_forl_numeric_oracle", 4.5, 0.5, -0.5));
    expect_mode_rejected(L2TG(L));
    assert(trace_runnable_acq(T, 1));
  } else {
    /* And an ascending edge cannot enter a descending-specialized root. */
    reset_native_activity();
    assert(callnum3(L, "__arm64_forl_numeric_dynamic", 0.5, 3.5, 0.5) ==
	   callnum3(L, "__arm64_forl_numeric_oracle", 0.5, 3.5, 0.5));
    expect_mode_rejected(L2TG(L));
    assert(trace_runnable_acq(T, 1));
  }
  /* These IEEE edge cases terminate before a native backedge. FORL chooses
  ** its direction from the step sign bit, so -0 follows the descending arm. */
  reset_native_activity();
  assert(callnum3(L, "__arm64_forl_numeric_dynamic", 2.5, 1.5, 0.0) ==
	 callnum3(L, "__arm64_forl_numeric_oracle", 2.5, 1.5, 0.0));
  assert(callnum3(L, "__arm64_forl_numeric_dynamic", 0.5, 3.5, -0.0) ==
	 callnum3(L, "__arm64_forl_numeric_oracle", 0.5, 3.5, -0.0));
  assert(callnum3(L, "__arm64_forl_numeric_dynamic", 0.5, NAN, 0.5) ==
	 callnum3(L, "__arm64_forl_numeric_oracle", 0.5, NAN, 0.5));
  {
    lua_Number nan_step = negative ? -NAN : NAN;
    lua_Number nan_start = negative ? 0.5 : 2.5;
    lua_Number nan_stop = negative ? 3.5 : 1.5;
    assert(callnum3(L, "__arm64_forl_numeric_dynamic", nan_start, nan_stop,
		    nan_step) ==
	   callnum3(L, "__arm64_forl_numeric_oracle", nan_start, nan_stop,
		    nan_step));
  }
  assert(callnum3(L, "__arm64_forl_numeric_dynamic", 0.5, 3.5,
		  INFINITY) ==
	 callnum3(L, "__arm64_forl_numeric_oracle", 0.5, 3.5, INFINITY));
  assert(callnum3(L, "__arm64_forl_numeric_dynamic", 4.5, 0.5,
		  -INFINITY) ==
	 callnum3(L, "__arm64_forl_numeric_oracle", 4.5, 0.5, -INFINITY));
  expect_branch_only();
  lua_close(L);
}

static GCtrace *expect_published_forl(lua_State *L, GCproto *pt)
{
  jit_State *J = L2J(L);
  GCtrace *T = traceref_safe(J, 1);
  TraceNo traceno;
  uint32_t flags;
  assert(trace_runnable_acq(T, 1));
  assert(trace_traceno_acq(T) == 1);
  assert(trace_root_acq(T) == 0);
  assert(trace_link_acq(T) == 1);
  assert(trace_linktype_acq(T) == LJ_TRLINK_LOOP);
  assert(trace_nchild_acq(T) == 0);
  assert(trace_nextside_acq(T) == 0);
  assert(trace_startpt_acq(T) == pt);
  flags = trace_arm64_admission_acq(T);
  assert(flags == TRACE_ARM64_INT_FORL_ADMITTED ||
         flags == (TRACE_ARM64_NUM_FORL_ADMITTED |
		   TRACE_ARM64_NUM_FORL_ASCENDING) ||
         flags == (TRACE_ARM64_NUM_FORL_ADMITTED |
		   TRACE_ARM64_NUM_FORL_DESCENDING));
  assert((flags & TRACE_ARM64_INT_LOOP_ADMITTED) == 0);
  assert((la_load8_acq(&T->unused1) & TRACE_ENTRY_GATED) == 0);
  assert(trace_mcode_acq(T) != NULL);
  assert(trace_szmcode_acq(T) > sizeof(MCode));
  assert(trace_mcloop_acq(T) > 0);
  assert(trace_mcloop_acq(T) < trace_szmcode_acq(T));
  expect_forl_pair(T, pt);
  for (traceno = 2; (MSize)traceno < trace_sizetrace_acq(J); traceno++)
    assert(!trace_runnable_acq(traceref_safe(J, traceno), traceno));
  return T;
}

static void expect_branch_only(void)
{
  assert(lj_trace_test_root_entry_publishes() == 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() == 0);
}

static void expect_mode_rejected(TGState *tg)
{
  assert(lj_trace_test_root_entry_publishes() != 0);
  assert(lj_trace_test_root_entry_cleanups() ==
	 lj_trace_test_root_entry_publishes());
  assert(lj_trace_test_exit_calls() == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_tg_load_jit_base(tg) == NULL);
}

static void expect_native_entry(void)
{
  assert(lj_trace_test_root_entry_publishes() != 0);
  assert(lj_trace_test_root_entry_cleanups() == 0);
  assert(lj_trace_test_exit_calls() ==
	 lj_trace_test_root_entry_publishes());
}

static void reset_native_activity(void)
{
  lj_trace_test_root_entry_reset();
  lj_trace_test_reset_exit_stats();
}

static void run_positive(void)
{
  lua_State *L = luaL_newstate();
  GCproto *pt;
  GCtrace *T;
  const BCIns *forlpc;
  BCIns forl;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.opt.start('hotloop=2','hotexit=1')\n"
    "function __arm64_forl_positive(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n");
  pt = global_proto(L, "__arm64_forl_positive");
  reset_native_activity();
  assert(call1(L, "__arm64_forl_positive", 100) == 5050);
  T = expect_published_forl(L, pt);
  forlpc = trace_startpc_acq(T);
  forl = trace_startins_acq(T);
  expect_positive_ir(T, pt);
  expect_positive_snapshots(T, pt);
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  reset_native_activity();
  assert(call1(L, "__arm64_forl_positive", 37) == 703);
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);

  /* A NUM edge cannot enter the earlier INT specialization; the exact mode
  ** mismatch is rejected and the already-tested edge is recovered. */
  reset_native_activity();
  assert(callnum1(L, "__arm64_forl_positive", 3.5) == 6.0);
  expect_mode_rejected(L2TG(L));
  assert(lj_trace_test_root_entry_startins_calls() == 2);

  run_lua(L, "jit.flush()\n");
  assert((BCIns)la_load32_acq((const uint32_t *)forlpc) == forl);
  assert(proto_jit_startins_acq(pt, forlpc) == forl);
  assert(!trace_runnable_acq(traceref_safe(L2J(L), 1), 1));

  reset_native_activity();
  assert(call1(L, "__arm64_forl_positive", 100) == 5050);
  T = expect_published_forl(L, pt);
  assert(trace_startpc_acq(T) == forlpc);
  assert(trace_startins_acq(T) == forl);
  expect_positive_ir(T, pt);
  expect_positive_snapshots(T, pt);
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  lua_close(L);
}

static void run_negative(void)
{
  lua_State *L = luaL_newstate();
  GCproto *pt;
  GCtrace *T;
  IRIns *ir;
  IRRef minus3;
  IRRef one;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.opt.start('hotloop=2','hotexit=1')\n"
    "function __arm64_forl_negative(n)\n"
    "  local s = 0\n"
    "  for i = n, 1, -3 do s = s + i end\n"
    "  return s\n"
    "end\n");
  pt = global_proto(L, "__arm64_forl_negative");
  reset_native_activity();
  assert(call1(L, "__arm64_forl_negative", 100) == 1717);
  T = expect_published_forl(L, pt);
  ir = trace_ir_acq(T);
  minus3 = find_kint(T, -3);
  one = find_kint(T, 1);
  assert(minus3 != 0 && one != 0);
  assert(trace_nins_acq(T) == N_END);
  expect_ir(ir, N_IDX, IR_SLOAD, IRT_INT|IRT_GUARD,
	    4, IRSLOAD_TYPECHECK|IRSLOAD_INHERIT);
  expect_ir(ir, N_SUM, IR_SLOAD, IRT_INT|IRT_GUARD,
	    3, IRSLOAD_TYPECHECK);
  expect_ir(ir, N_SUM_PRE, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, N_SUM, N_IDX);
  expect_ir(ir, N_IDX_PRE, IR_ADD, IRT_INT|IRT_ISPHI, N_IDX, minus3);
  expect_ir(ir, N_PRECOND, IR_GE, IRT_INT|IRT_GUARD, N_IDX_PRE, one);
  expect_ir(ir, N_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  expect_ir(ir, N_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  expect_ir(ir, N_SUM_BODY, IR_ADDOV,
	    IRT_INT|IRT_GUARD|IRT_ISPHI, N_IDX_PRE, N_SUM_PRE);
  expect_ir(ir, N_IDX_BODY, IR_ADD, IRT_INT|IRT_ISPHI,
	    N_IDX_PRE, minus3);
  expect_ir(ir, N_COND, IR_GE, IRT_INT|IRT_GUARD, N_IDX_BODY, one);
  expect_ir(ir, N_IDX_PHI, IR_PHI, IRT_INT, N_IDX_PRE, N_IDX_BODY);
  expect_ir(ir, N_SUM_PHI, IR_PHI, IRT_INT, N_SUM_PRE, N_SUM_BODY);
  expect_ir(ir, N_RENAME, IR_RENAME, IRT_NIL, N_SUM_PRE, 3);
  assert(trace_spadjust_acq(T) == 0);
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  reset_native_activity();
  assert(call1(L, "__arm64_forl_negative", 40) == 287);
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  lua_close(L);
}

static void run_dynamic(int negative)
{
  lua_State *L = luaL_newstate();
  GCproto *pt;
  GCtrace *T;
  const BCIns *forlpc;
  BCIns forl;
  lua_Integer a = negative ? 40 : 1;
  lua_Integer b = negative ? 1 : 40;
  lua_Integer step = negative ? -2 : 2;
  lua_Integer expected = negative ? 420 : 400;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.opt.start('hotloop=2','hotexit=1000')\n"
    "function __arm64_forl_dynamic(a, b, step)\n"
    "  local s = 0\n"
    "  for i = a, b, step do s = s + i end\n"
    "  return s\n"
    "end\n");
  pt = global_proto(L, "__arm64_forl_dynamic");
  reset_native_activity();
  assert(call3(L, "__arm64_forl_dynamic", a, b, step) == expected);
  T = expect_published_forl(L, pt);
  forlpc = trace_startpc_acq(T);
  forl = trace_startins_acq(T);
  expect_dynamic_ir(T, pt, negative);
  expect_dynamic_snapshots(T, pt);
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);

  reset_native_activity();
  assert(call3(L, "__arm64_forl_dynamic", a, b,
	negative ? -3 : 3) == 287);
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);

  /* The stop+step proof must leave before either unchecked induction ADD. */
  reset_native_activity();
  assert(call3(L, "__arm64_forl_dynamic", 0,
	negative ? (lua_Integer)INT32_MIN : (lua_Integer)INT32_MAX,
	negative ? (lua_Integer)INT32_MIN : (lua_Integer)INT32_MAX) ==
	(negative ? (lua_Integer)INT32_MIN : (lua_Integer)INT32_MAX));
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);

  /* The hoisted direction guard exits before either unchecked induction ADD.
  ** The interpreter must finish the opposite-sign loop without a replay. */
  reset_native_activity();
  assert(call3(L, "__arm64_forl_dynamic", negative ? 1 : 40,
	negative ? 40 : 1, negative ? 2 : -2) ==
	(negative ? 400 : 420));
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  assert(trace_runnable_acq(traceref_safe(L2J(L), 1), 1));

  run_lua(L, "jit.flush()\n");
  assert((BCIns)la_load32_acq((const uint32_t *)forlpc) == forl);
  assert(proto_jit_startins_acq(pt, forlpc) == forl);
  assert(!trace_runnable_acq(traceref_safe(L2J(L), 1), 1));

  reset_native_activity();
  assert(call3(L, "__arm64_forl_dynamic", a, b, step) == expected);
  T = expect_published_forl(L, pt);
  assert(trace_startpc_acq(T) == forlpc);
  assert(trace_startins_acq(T) == forl);
  expect_dynamic_ir(T, pt, negative);
  expect_dynamic_snapshots(T, pt);
  expect_native_entry();
  lua_close(L);
}

static void run_rejections(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.opt.start('hotloop=2','hotexit=1')\n"
    "function __arm64_forl_constant_stop(a, step)\n"
    "  local s = 0\n"
    "  for i = a, 40, step do s = s + i end\n"
    "  return s\n"
    "end\n"
    "function __arm64_forl_zero(_)\n"
    "  local c = 0\n"
    "  for i = 1, 1, 0 do\n"
    "    c = c + 1\n"
    "    if c == 10 then break end\n"
    "  end\n"
    "  return c\n"
    "end\n");
  J = L2J(L);
  reset_native_activity();
  assert(call2(L, "__arm64_forl_constant_stop", 1, 2) == 400);
  assert(!trace_runnable_acq(traceref_safe(J, 1), 1));
  expect_branch_only();
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  assert(call1(L, "__arm64_forl_zero", 0) == 10);
  assert(!trace_runnable_acq(traceref_safe(J, 1), 1));
  expect_branch_only();
  lua_close(L);
}

static void run_negative_dynamic_stop(void)
{
  lua_State *L = luaL_newstate();
  GCproto *pt;
  GCtrace *T;
  IRIns *ir;
  IRRef minus3, bound;
  MSize idxslot;
  assert(L != NULL);
  luaL_openlibs(L);
  run_lua(L,
    "jit.opt.start('hotloop=2','hotexit=1')\n"
    "function __arm64_forl_negative_stop(n, stop)\n"
    "  local s = 0\n"
    "  for i = n, stop, -3 do s = s + i end\n"
    "  return s\n"
    "end\n");
  pt = global_proto(L, "__arm64_forl_negative_stop");
  reset_native_activity();
  assert(call2(L, "__arm64_forl_negative_stop", 100, 1) == 1717);
  T = expect_published_forl(L, pt);
  ir = trace_ir_acq(T);
  minus3 = find_kint(T, -3);
  bound = find_kint(T, INT32_MIN+3);
  idxslot = (MSize)(1u+LJ_FR2+bc_a(trace_startins_acq(T)));
  assert(minus3 != 0 && bound != 0);
  expect_ir(ir, R_STOP, IR_SLOAD, IRT_INT,
	    (IRRef)(idxslot+FORL_STOP), IRSLOAD_READONLY|IRSLOAD_INHERIT);
  expect_ir(ir, R_LIMIT_PROOF, IR_GE, IRT_INT|IRT_GUARD,
	    R_STOP, bound);
  expect_ir(ir, R_IDX, IR_SLOAD, IRT_INT|IRT_GUARD,
	    (IRRef)idxslot, IRSLOAD_TYPECHECK|IRSLOAD_INHERIT);
  assert(ir[R_IDX_PRE].o == IR_ADD && ir[R_IDX_PRE].op2 == minus3);
  assert(ir[R_PRECOND].o == IR_GE && ir[R_PRECOND].op2 == R_STOP);
  assert(ir[R_IDX_BODY].o == IR_ADD && ir[R_IDX_BODY].op2 == minus3);
  assert(ir[R_COND].o == IR_GE && ir[R_COND].op2 == R_STOP);
  expect_native_entry();
  assert(lj_trace_test_root_entry_startins_calls() == 0);
  lua_close(L);
}

int main(void)
{
  run_positive();
  run_negative();
  run_negative_dynamic_stop();
  run_dynamic(0);
  run_dynamic(1);
  run_numeric();
  run_numeric_dynamic(0);
  run_numeric_dynamic(1);
  run_rejections();
  puts("arm64_jit_forl_record OK: integer and NUM FORL roots published "
       "and entered");
  return 0;
}

#else

int main(void)
{
  puts("arm64_jit_forl_record SKIP: requires executable macOS ARM64 FORL");
  return 0;
}

#endif
