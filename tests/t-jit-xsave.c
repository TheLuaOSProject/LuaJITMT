/*
** Focused dormant-path test for IR_XSAVE snapshot identity, allocation
** materialization and x64 TG-private root staging.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_state.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

#ifndef LJ_XSAVE_TEST_HELPERS
#error "t-jit-xsave requires -DLJ_XSAVE_TEST_HELPERS"
#endif

static int ir_is_alloc(IROp op)
{
  return op == IR_TNEW || op == IR_TDUP || op == IR_CNEW || op == IR_CNEWI;
}

static int ref_reaches_alloc(IRIns *ir, IRRef nins, IRRef ref, unsigned depth)
{
  uint8_t mode;
  if (depth == 0 || irref_isk(ref) || ref < REF_FIRST || ref >= nins)
    return 0;
  if (ir_is_alloc((IROp)ir[ref].o))
    return 1;
  mode = lj_ir_mode[ir[ref].o];
  return (irm_op1(mode) == IRMref &&
	  ref_reaches_alloc(ir, nins, ir[ref].op1, depth - 1)) ||
	 (irm_op2(mode) == IRMref &&
	  ref_reaches_alloc(ir, nins, ir[ref].op2, depth - 1));
}

static GCtrace *find_xsave_trace(jit_State *J)
{
  TraceNo traceno;
  for (traceno = 1; traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref(J, traceno);
    IRIns *ir;
    IRRef ref, nins;
    int has_loop = 0, has_xsave = 0, has_alloc = 0;
    if (!T || trace_traceno_acq(T) != traceno)
      continue;
    ir = trace_ir_acq(T);
    nins = trace_nins_acq(T);
    for (ref = REF_FIRST; ref < nins; ref++) {
      has_loop |= ir[ref].o == IR_LOOP;
      has_xsave |= ir[ref].o == IR_XSAVE;
      has_alloc |= ir_is_alloc((IROp)ir[ref].o);
    }
    if (has_loop && has_xsave && has_alloc)
      return T;
  }
  return NULL;
}

static GCtrace *find_xsave_retf_trace(jit_State *J)
{
  TraceNo traceno;
  for (traceno = 1; traceno < trace_sizetrace_acq(J); traceno++) {
    GCtrace *T = traceref(J, traceno);
    IRIns *ir;
    IRRef ref, nins, xsave = 0;
    if (!T || trace_traceno_acq(T) != traceno)
      continue;
    ir = trace_ir_acq(T);
    nins = trace_nins_acq(T);
    for (ref = REF_FIRST; ref < nins; ref++) {
      if (ir[ref].o == IR_XSAVE && xsave == 0)
	xsave = ref;
      if (ir[ref].o == IR_RETF && xsave != 0 && xsave < ref)
	return T;
    }
  }
  return NULL;
}

static BCReg xsave_baseslot(GCtrace *T, SnapShot *snap, int *gotframe)
{
  SnapEntry *map = &trace_snapmap_acq(T)[snap_mapofs_acq(snap)];
  MSize n;
  *gotframe = 0;
  for (n = snap_nent_acq(snap); n > 0; n--) {
    SnapEntry sn = snapentry_acq(&map[n-1]);
    if ((sn & SNAP_FRAME)) {
      *gotframe = 1;
      return snap_slot(sn) - LJ_FR2;
    }
  }
  return 0;
}

static void assert_xsave_retf_shape(GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  IRRef ref, nins = trace_nins_acq(T), xsave = 0;
  unsigned ordered = 0;
  for (ref = REF_FIRST; ref < nins; ref++) {
    if (ir[ref].o == IR_XSAVE && xsave == 0)
      xsave = ref;
    if (ir[ref].o == IR_RETF && xsave != 0 && xsave < ref)
      ordered++;
  }
  assert(ordered != 0);
}

static void assert_xsave_trace_shape(GCtrace *T)
{
  IRIns *ir = trace_ir_acq(T);
  SnapShot *snaps = trace_snap_acq(T);
  SnapEntry *snapmap = trace_snapmap_acq(T);
  IRRef ref, loopref = 0, nins = trace_nins_acq(T);
  SnapNo nsnap = trace_nsnap_acq(T), sn;
  unsigned nalloc = 0, nallocroot = 0, nstore = 0;
  unsigned nxsave = 0, npre = 0, ncopy = 0, ninlined = 0;

  for (ref = REF_FIRST; ref < nins; ref++) {
    IROp op = (IROp)ir[ref].o;
    if (op == IR_LOOP)
      loopref = ref;
    if (ir_is_alloc(op)) {
      nalloc++;
      assert(ir[ref].r != RID_SINK && ir[ref].r != RID_SUNK);
    }
    if (op == IR_ASTORE || op == IR_HSTORE || op == IR_FSTORE ||
	op == IR_XSTORE || op == IR_NEWREF) {
      nstore++;
      assert(ir[ref].r != RID_SINK && ir[ref].r != RID_SUNK);
    }
  }
  assert(loopref != 0);
  assert(nalloc != 0);
  assert(nstore != 0);

  for (ref = REF_FIRST; ref < nins; ref++) {
    unsigned matches = 0;
    if (ir[ref].o != IR_XSAVE)
      continue;
    nxsave++;
    if (ref < loopref) npre++;
    if (ref > loopref) ncopy++;
    for (sn = 0; sn < nsnap; sn++)
      if (snap_ref_acq(&snaps[sn]) == ref)
	matches++;
    /* The recorder marker and its LOOP-substituted copy must each retain an
    ** assembler-position snapshot instead of sharing a literal snap number. */
    assert(matches != 0);
  }
  assert(nxsave >= 2 && npre != 0 && ncopy != 0);

  for (sn = 0; sn < nsnap; sn++) {
    SnapShot *snap = &snaps[sn];
    IRRef snapref = snap_ref_acq(snap);
    MSize i, nent;
    SnapEntry *map;
    if (snapref < REF_FIRST || snapref >= nins ||
	ir[snapref].o != IR_XSAVE)
      continue;
    {
      int gotframe;
      BCReg baseslot = xsave_baseslot(T, snap, &gotframe);
      if (gotframe && baseslot != 0)
	ninlined++;
    }
    map = &snapmap[snap_mapofs_acq(snap)];
    nent = snap_nent_acq(snap);
    for (i = 0; i < nent; i++)
      if (ref_reaches_alloc(ir, nins, snap_ref(snapentry_acq(&map[i])), 64))
	nallocroot++;
  }
  /* At least one marker snapshot must own the otherwise sinkable table. */
  assert(nallocroot != 0);
  /* Exercise the real inlined-call layout. The separate hot-call trace below
  ** proves the distinct XSAVE-before-RETF backwards-allocation case. */
  assert(ninlined != 0);
}

int main(void)
{
#if !LJ_TARGET_X64
  printf("t-jit-xsave SKIP: x64-only lowering\n");
  return 0;
#else
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  jit_State *J = G2J(g);
  TGState *tg = L2TG(L);
  TValue *stack;
  TValue *maxstack;
  GCtrace *T;

  assert(tg->ffi_xsave_root == NULL);
  assert(tg->ffi_xsave_baseslot == 0);
  assert(tg->ffi_xsave_nslots == 0);
  tg->ffi_xsave_root = (TValue *)(uintptr_t)1;
  tg->ffi_xsave_baseslot = UINT32_MAX;
  tg->ffi_xsave_nslots = UINT32_MAX;
  ljt_lua_dostring(L,
    "local util = require('jit.util')\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1', '+sink')\n"
    "local function inner(i)\n"
    "  local t = { i }\n"
    "  t.keep = i + 1\n"
    "  local x = math.abs(-i)\n"
    "  return x + t[1] + t.keep - (3*i + 1)\n"
    "end\n"
    "local function outer(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do\n"
    "    s = s + inner(i)\n"
    "  end\n"
    "  return s\n"
    "end\n"
    "local function drive(n)\n"
    "  for i = 1, n do assert(outer(80) == 0) end\n"
    "end\n"
    "drive(40)\n"
    "assert(util.traceinfo(1), 'expected XSAVE loop trace')\n"
    "drive(40)\n"
    "local function retrec(n)\n"
    "  if n == 0 then return 0 end\n"
    "  local x = retrec(n - 1)\n"
    "  local y = math.abs(-n)\n"
    "  return x + y\n"
    "end\n"
    "for i = 1, 120 do assert(retrec(12) == 78) end\n"
    "drive(40)\n"
    /* The table-allocation trace may side-exit before its post-allocation
    ** marker under a new arena representation. Stage through a separate
    ** allocation-free marker after both shape traces have been built. */
    "local function stage(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + math.abs(-i) end\n"
    "  return s\n"
    "end\n"
    "for i = 1, 40 do assert(stage(80) == 3240) end\n");

  T = find_xsave_trace(J);
  assert(T != NULL);
  assert_xsave_trace_shape(T);
  T = find_xsave_retf_trace(J);
  assert(T != NULL);
  assert_xsave_retf_shape(T);

  stack = mref(L->stack, TValue);
  maxstack = mref(L->maxstack, TValue);
  assert(tg->ffi_xsave_root != (TValue *)(uintptr_t)1);
  assert(tg->ffi_xsave_root >= stack && tg->ffi_xsave_root < maxstack);
  assert(tg->ffi_xsave_baseslot != UINT32_MAX);
  assert(tg->ffi_xsave_nslots != UINT32_MAX);
  /* The last marker executes inside inner(), so its complete logical root is
  ** the trace root while the current frame begins at a non-zero slot. */
  assert(tg->ffi_xsave_baseslot != 0);
  assert(tg->ffi_xsave_nslots > tg->ffi_xsave_baseslot + LJ_FR2);
  assert(tg->ffi_xsave_root + tg->ffi_xsave_nslots <= maxstack);

  lua_close(L);
  printf("t-jit-xsave OK: copied snapshots materialize and stage exact roots\n");
  return 0;
#endif
}
