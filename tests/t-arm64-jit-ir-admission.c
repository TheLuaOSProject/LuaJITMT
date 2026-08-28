/*
** Synthetic positive/negative contract for the ARM64 scalar trace IR gate.
** This fixture validates policy only; no generated code runs here.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL)

#include "lj_obj.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_ir.h"
#include "lj_ircall.h"
#include "lj_jit.h"
#include "lj_target.h"
#include "lj_trace.h"
#include "lj_asm.h"
#include "lj_vm.h"

#if !LJ_HASJIT || LJ_ARM64_JIT_ROOT_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_LOOP_NATIVE_ENTRY_FAIL_CLOSED || \
    LJ_ARM64_JIT_FORL_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_RECORDER_FAIL_CLOSED || \
    LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED || \
    !LJ_ARM64_JIT_STITCH_NATIVE_ENTRY_FAIL_CLOSED
#error "t-arm64-jit-ir-admission requires the admitted ARM64 root gate split"
#endif

enum {
  K_ZERO = REF_TRUE - 3,
  K_STOP = REF_TRUE - 2,
  K_STEP = REF_TRUE - 1,

  R_A = REF_FIRST,
  R_B,
  R_C,
  R_SUM1,
  R_SUM2,
  R_PRECOND,
  R_LOOP,
  R_XPOLL,
  R_BODY1,
  R_BODY2,
  R_LOOPCOND,
  R_PHI1,
  R_PHI2,
  R_END,
  ADMISSION_IR_CAP = REF_BASE + 256
};

/* Exact recorder order for the first admitted mixed INT/NUM BC_LOOP root.
** Keep these names distinct from the older all-integer policy fixture above:
** the two fixtures deliberately exercise separate admission grammars. */
enum {
  N_K_ONE = REF_TRUE - 1,

  N_R_I = REF_FIRST,
  N_R_X,
  N_R_STEP,
  N_R_I_PRE,
  N_R_X_PRE,
  N_R_LIMIT,
  N_R_PRE_GUARD,
  N_R_LOOP,
  N_R_XPOLL,
  N_R_I_BODY,
  N_R_X_BODY,
  N_R_BODY_GUARD,
  N_R_I_PHI,
  N_R_X_PHI,
  N_R_SEMANTIC_END,
  N_R_RENAME = N_R_SEMANTIC_END,
  N_R_POSTRA_END
};

/* Exact recorder order for the first pure-NUM BC_LOOP root. The KNUM
** occupies a header plus the following 64-bit payload slot. */
enum {
  H_K_HALF = REF_TRUE - 2,
  H_K_HALF_PAYLOAD = REF_TRUE - 1,

  H_R_X = REF_FIRST,
  H_R_X_PRE,
  H_R_LIMIT,
  H_R_PRE_GUARD,
  H_R_LOOP,
  H_R_XPOLL,
  H_R_X_BODY,
  H_R_BODY_GUARD,
  H_R_X_PHI,
  H_R_SEMANTIC_END,
  H_R_NOP = H_R_SEMANTIC_END,
  H_R_POSTRA_END
};

/* Exact recorder order for the pure-NUM root whose step is an invariant
** parameter. Unlike H_*, this trace has no IR constants: the prototype's
** +0.5 initializes x before the trace starts, while step is an SLOAD. */
enum {
  D_R_X = REF_FIRST,
  D_R_STEP,
  D_R_X_PRE,
  D_R_LIMIT,
  D_R_PRE_GUARD,
  D_R_LOOP,
  D_R_XPOLL,
  D_R_X_BODY,
  D_R_BODY_GUARD,
  D_R_X_PHI,
  D_R_SEMANTIC_END,
  D_R_NOP = D_R_SEMANTIC_END,
  D_R_POSTRA_END
};

/* Exact recorder order for the pure-NUM root whose accumulator, limit and
** step are all parameters. Like D_*, this trace has no IR constants, but its
** prototype and stack-slot certificate are intentionally distinct. */
enum {
  A_R_X = REF_FIRST,
  A_R_STEP,
  A_R_X_PRE,
  A_R_LIMIT,
  A_R_PRE_GUARD,
  A_R_LOOP,
  A_R_XPOLL,
  A_R_X_BODY,
  A_R_BODY_GUARD,
  A_R_X_PHI,
  A_R_SEMANTIC_END,
  A_R_NOP = A_R_SEMANTIC_END,
  A_R_POSTRA_END
};

enum {
  NUMACC_FIXTURE_ADD_LT = 1u,
  NUMACC_FIXTURE_ADD_LE = 2u,
  NUMACC_FIXTURE_SUB_GT = 3u
};

typedef struct NumaccFixtureProfile {
  unsigned id;
  BCOp comparison_bc;
  BCReg comparison_a;
  BCReg comparison_d;
  BCOp recurrence_bc;
  IROp recurrence_op;
  IRRef pre_left;
  IRRef pre_right;
  IROp precondition_op;
  IROp body_op;
} NumaccFixtureProfile;

static const NumaccFixtureProfile numacc_fixture_profiles[] = {
  { NUMACC_FIXTURE_ADD_LT, BC_ISGE, 3, 4, BC_ADDVV, IR_ADD,
    A_R_STEP, A_R_X, IR_GT, IR_LT },
  { NUMACC_FIXTURE_ADD_LE, BC_ISGT, 3, 4, BC_ADDVV, IR_ADD,
    A_R_STEP, A_R_X, IR_GE, IR_LE },
  { NUMACC_FIXTURE_SUB_GT, BC_ISGE, 4, 3, BC_SUBVV, IR_SUB,
    A_R_X, A_R_STEP, IR_LT, IR_GT }
};

typedef struct AdmissionFixture {
  GCtrace T;
  IRIns ir[ADMISSION_IR_CAP];
  SnapShot snap[7];
  SnapEntry snapmap[32];
} AdmissionFixture;

LJ_STATIC_ASSERT(REF_FIRST + 3*LJ_MAX_PHI + 2u <= ADMISSION_IR_CAP);

static AdmissionFixture fx;
static GCproto *fixture_pt;
static const BCIns *fixture_forl_pc;
static const BCIns *fixture_loop_pc;
static const BCIns *fixture_snapshot_pc;
static GCproto *numeric_fixture_pt;
static const BCIns *numeric_fixture_loop_pc;
static const BCIns *numeric_fixture_snapshot_pc;
static GCproto *numhalf_fixture_pt;
static const BCIns *numhalf_fixture_loop_pc;
static GCproto *numstep_fixture_pt;
static const BCIns *numstep_fixture_loop_pc;
static GCproto *numacc_strict_fixture_pt;
static const BCIns *numacc_strict_fixture_loop_pc;
static GCproto *numacc_inclusive_fixture_pt;
static const BCIns *numacc_inclusive_fixture_loop_pc;
static GCproto *numacc_sub_gt_fixture_pt;
static const BCIns *numacc_sub_gt_fixture_loop_pc;
/* Active full-shape prototype. The shared synthetic geometry is always
** rebuilt from this exact source certificate before every mutation. */
static GCproto *numacc_fixture_pt;
static const BCIns *numacc_fixture_loop_pc;
static const NumaccFixtureProfile *numacc_fixture_profile;

static BCIns loadbc(const BCIns *pc)
{
  return (BCIns)la_load32_acq((const uint32_t *)pc);
}

static void select_numacc_fixture(unsigned profile_id)
{
  assert(profile_id >= NUMACC_FIXTURE_ADD_LT &&
	 profile_id <= NUMACC_FIXTURE_SUB_GT);
  numacc_fixture_profile = &numacc_fixture_profiles[profile_id-1u];
  assert(numacc_fixture_profile->id == profile_id);
  if (profile_id == NUMACC_FIXTURE_ADD_LT) {
    assert(numacc_strict_fixture_pt != NULL &&
	   numacc_strict_fixture_loop_pc != NULL);
    numacc_fixture_pt = numacc_strict_fixture_pt;
    numacc_fixture_loop_pc = numacc_strict_fixture_loop_pc;
  } else if (profile_id == NUMACC_FIXTURE_ADD_LE) {
    assert(numacc_inclusive_fixture_pt != NULL &&
	   numacc_inclusive_fixture_loop_pc != NULL);
    numacc_fixture_pt = numacc_inclusive_fixture_pt;
    numacc_fixture_loop_pc = numacc_inclusive_fixture_loop_pc;
  } else {
    assert(numacc_sub_gt_fixture_pt != NULL &&
	   numacc_sub_gt_fixture_loop_pc != NULL);
    numacc_fixture_pt = numacc_sub_gt_fixture_pt;
    numacc_fixture_loop_pc = numacc_sub_gt_fixture_loop_pc;
  }
}

static void set_snapshot_payload(SnapNo snapno, const BCIns *pc,
	uint8_t basedelta)
{
  const SnapShot *snap = &fx.snap[snapno];
  uint64_t pcbase = ((uint64_t)(uintptr_t)pc << 8) | basedelta;
  memcpy(&fx.snapmap[snap->mapofs+snap->nent], &pcbase, sizeof(pcbase));
}

static void setir(IRRef ref, IROp op, IRType type, IRRef op1, IRRef op2)
{
  fx.ir[ref].op1 = (IRRef1)op1;
  fx.ir[ref].op2 = (IRRef1)op2;
  fx.ir[ref].t.irt = (uint8_t)type;
  fx.ir[ref].o = (IROp1)op;
  fx.ir[ref].prev = 0;
}

static void make_trace(jit_State *J)
{
  assert(fixture_pt != NULL && fixture_loop_pc != NULL &&
	 fixture_snapshot_pc != NULL);
  memset(&fx, 0, sizeof(fx));

  setir(K_ZERO, IR_KINT, IRT_INT, 0, 0);
  setir(K_STOP, IR_KINT, IRT_INT, 20, 0);
  setir(K_STEP, IR_KINT, IRT_INT, 1, 0);
  setir(REF_TRUE, IR_KPRI, IRT_TRUE, 0, 0);
  setir(REF_FALSE, IR_KPRI, IRT_FALSE, 0, 0);
  setir(REF_NIL, IR_KPRI, IRT_NIL, 0, 0);

  setir(REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  setir(R_A, IR_SLOAD, IRT_INT|IRT_GUARD, 2, IRSLOAD_TYPECHECK);
  setir(R_B, IR_SLOAD, IRT_INT|IRT_GUARD, 3, IRSLOAD_TYPECHECK);
  setir(R_C, IR_SLOAD, IRT_INT|IRT_GUARD, 4, IRSLOAD_TYPECHECK);
  setir(R_SUM1, IR_SUBOV, IRT_INT|IRT_GUARD|IRT_ISPHI, R_A, R_B);
  setir(R_SUM2, IR_MULOV, IRT_INT|IRT_GUARD|IRT_ISPHI, R_SUM1, R_C);
  setir(R_PRECOND, IR_GE, IRT_INT|IRT_GUARD, R_SUM2, K_STOP);
  setir(R_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  setir(R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  setir(R_BODY1, IR_SUBOV, IRT_INT|IRT_GUARD|IRT_ISPHI, R_SUM2, R_C);
  setir(R_BODY2, IR_MULOV, IRT_INT|IRT_GUARD|IRT_ISPHI, R_BODY1, K_STEP);
  setir(R_LOOPCOND, IR_LE, IRT_INT|IRT_GUARD, R_BODY2, K_ZERO);
  setir(R_PHI1, IR_PHI, IRT_INT, R_SUM1, R_BODY1);
  setir(R_PHI2, IR_PHI, IRT_INT, R_SUM2, R_BODY2);

  fx.snap[0].ref = R_A;
  fx.snap[0].mapofs = 0;
  fx.snap[0].nslots = 5;
  fx.snap[0].topslot = fixture_pt->framesize;
  fx.snap[0].nent = 0;

  fx.snap[1] = fx.snap[0];
  fx.snap[1].ref = R_PRECOND;
  fx.snap[1].mapofs = 2;
  fx.snap[1].nent = 2;
  fx.snapmap[2] = SNAP(2, 0, R_SUM1);
  fx.snapmap[3] = SNAP(3, 0, R_SUM2);

  fx.snap[2] = fx.snap[1];
  fx.snap[2].ref = R_LOOP;
  fx.snap[2].mapofs = 6;
  fx.snapmap[6] = SNAP(2, 0, R_SUM1);
  fx.snapmap[7] = SNAP(3, 0, R_SUM2);

  fx.snap[3] = fx.snap[1];
  fx.snap[3].ref = R_LOOPCOND;
  fx.snap[3].mapofs = 10;
  fx.snapmap[10] = SNAP(2, 0, R_BODY1);
  fx.snapmap[11] = SNAP(3, 0, R_BODY2);

  fx.T.nk = K_ZERO;
  fx.T.nins = R_END;
  fx.T.ir = fx.ir;
  fx.T.nsnap = 4;
  fx.T.snap = fx.snap;
  fx.T.nsnapmap = 14;
  fx.T.snapmap = fx.snapmap;
  fx.T.traceno = 1;
  fx.T.link = 1;
  fx.T.root = 0;
  fx.T.linktype = LJ_TRLINK_LOOP;
  fx.T.sinktags = 0;
  fx.T.startins = loadbc(fixture_loop_pc);
  trace_startpt_rel(&fx.T, fixture_pt);
  setmref(fx.T.startpc, fixture_loop_pc);

  set_snapshot_payload(0, fixture_snapshot_pc, 0);
  set_snapshot_payload(1, fixture_snapshot_pc, 0);
  set_snapshot_payload(2, fixture_snapshot_pc, 0);
  set_snapshot_payload(3, fixture_snapshot_pc, 0);

  J->parent = 0;
  J->exitno = 0;
  J->pt = fixture_pt;
  J->baseslot = 1 + LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->loopref = R_LOOP;
  J->startpc = fixture_loop_pc;
}

static void make_forl_trace(jit_State *J)
{
  assert(fixture_forl_pc != NULL);
  make_trace(J);
  fx.T.startins = loadbc(fixture_forl_pc);
  setmref(fx.T.startpc, fixture_forl_pc);
  J->startpc = fixture_forl_pc;
  /* Normalize the synthetic hidden IDX/STOP loads so this negative reaches
  ** the dedicated induction-shape proof instead of failing at slot layout. */
  fx.ir[R_B].op2 = IRSLOAD_TYPECHECK|IRSLOAD_INHERIT;
  fx.ir[R_C].t.irt = IRT_INT;
  fx.ir[R_C].op2 = IRSLOAD_READONLY|IRSLOAD_INHERIT;
}

static void make_numeric_trace(jit_State *J)
{
  static const IRRef snaprefs[7] = {
    N_R_I, N_R_I_PRE, N_R_LIMIT, N_R_PRE_GUARD,
    N_R_LOOP, N_R_I_BODY, N_R_BODY_GUARD
  };
  static const uint16_t mapofs[7] = { 0, 2, 5, 10, 14, 18, 23 };
  static const uint8_t nent[7] = { 0, 1, 3, 2, 2, 3, 2 };
  static const uint8_t nslots[7] = { 6, 7, 7, 6, 6, 7, 6 };
  SnapNo snapno;

  assert(numeric_fixture_pt != NULL && numeric_fixture_loop_pc != NULL &&
	 numeric_fixture_snapshot_pc != NULL);
  assert(numeric_fixture_pt->framesize == 6);
  memset(&fx, 0, sizeof(fx));

  setir(N_K_ONE, IR_KINT, IRT_INT, 1, 0);
  setir(REF_TRUE, IR_KPRI, IRT_TRUE, 0, 0);
  setir(REF_FALSE, IR_KPRI, IRT_FALSE, 0, 0);
  setir(REF_NIL, IR_KPRI, IRT_NIL, 0, 0);

  setir(REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  setir(N_R_I, IR_SLOAD, IRT_INT|IRT_GUARD,
	5, IRSLOAD_TYPECHECK);
  setir(N_R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,
	3, IRSLOAD_TYPECHECK);
  setir(N_R_STEP, IR_SLOAD, IRT_NUM|IRT_GUARD,
	4, IRSLOAD_TYPECHECK);
  setir(N_R_I_PRE, IR_ADDOV, IRT_INT|IRT_GUARD|IRT_ISPHI,
	N_R_I, N_K_ONE);
  setir(N_R_X_PRE, IR_ADD, IRT_NUM|IRT_ISPHI,
	N_R_STEP, N_R_X);
  setir(N_R_LIMIT, IR_SLOAD, IRT_INT|IRT_GUARD,
	2, IRSLOAD_TYPECHECK);
  setir(N_R_PRE_GUARD, IR_GT, IRT_INT|IRT_GUARD,
	N_R_LIMIT, N_R_I_PRE);
  setir(N_R_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  setir(N_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  setir(N_R_I_BODY, IR_ADDOV, IRT_INT|IRT_GUARD|IRT_ISPHI,
	N_R_I_PRE, N_K_ONE);
  setir(N_R_X_BODY, IR_ADD, IRT_NUM|IRT_ISPHI,
	N_R_X_PRE, N_R_STEP);
  setir(N_R_BODY_GUARD, IR_LT, IRT_INT|IRT_GUARD,
	N_R_I_BODY, N_R_LIMIT);
  setir(N_R_I_PHI, IR_PHI, IRT_INT, N_R_I_PRE, N_R_I_BODY);
  setir(N_R_X_PHI, IR_PHI, IRT_NUM, N_R_X_PRE, N_R_X_BODY);

  for (snapno = 0; snapno < 7; snapno++) {
    fx.snap[snapno].ref = snaprefs[snapno];
    fx.snap[snapno].mapofs = mapofs[snapno];
    fx.snap[snapno].nent = nent[snapno];
    fx.snap[snapno].nslots = nslots[snapno];
    fx.snap[snapno].topslot = 6;
  }
  fx.snapmap[2] = SNAP(6, 0, N_R_I);
  fx.snapmap[5] = SNAP(3, 0, N_R_X_PRE);
  fx.snapmap[6] = SNAP(5, 0, N_R_I_PRE);
  fx.snapmap[7] = SNAP(6, 0, N_R_I_PRE);
  fx.snapmap[10] = SNAP(3, 0, N_R_X_PRE);
  fx.snapmap[11] = SNAP(5, 0, N_R_I_PRE);
  fx.snapmap[14] = SNAP(3, 0, N_R_X_PRE);
  fx.snapmap[15] = SNAP(5, 0, N_R_I_PRE);
  fx.snapmap[18] = SNAP(3, 0, N_R_X_PRE);
  fx.snapmap[19] = SNAP(5, 0, N_R_I_PRE);
  fx.snapmap[20] = SNAP(6, 0, N_R_I_PRE);
  fx.snapmap[23] = SNAP(3, 0, N_R_X_BODY);
  fx.snapmap[24] = SNAP(5, 0, N_R_I_BODY);

  fx.T.nk = N_K_ONE;
  fx.T.nins = N_R_SEMANTIC_END;
  fx.T.ir = fx.ir;
  fx.T.nsnap = 7;
  fx.T.snap = fx.snap;
  fx.T.nsnapmap = 27;
  fx.T.snapmap = fx.snapmap;
  fx.T.traceno = 1;
  fx.T.link = 1;
  fx.T.root = 0;
  fx.T.linktype = LJ_TRLINK_LOOP;
  fx.T.sinktags = 0;
  fx.T.startins = loadbc(numeric_fixture_loop_pc);
  trace_startpt_rel(&fx.T, numeric_fixture_pt);
  setmref(fx.T.startpc, numeric_fixture_loop_pc);
  for (snapno = 0; snapno < 7; snapno++)
    set_snapshot_payload(snapno, numeric_fixture_snapshot_pc, 0);

  J->parent = 0;
  J->exitno = 0;
  J->pt = numeric_fixture_pt;
  J->baseslot = 1 + LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->loopref = N_R_LOOP;
  J->startpc = numeric_fixture_loop_pc;
}

static void make_numhalf_trace(jit_State *J)
{
  static const IRRef snaprefs[5] = {
    H_R_X, H_R_LIMIT, H_R_PRE_GUARD, H_R_LOOP, H_R_BODY_GUARD
  };
  static const uint16_t mapofs[5] = { 0, 2, 6, 9, 12 };
  static const uint8_t nent[5] = { 0, 2, 1, 1, 1 };
  static const uint8_t nslots[5] = { 4, 5, 4, 4, 4 };
  static const MSize pcpos[5] = { 7, 3, 11, 7, 11 };
  SnapNo snapno;

  assert(numhalf_fixture_pt != NULL && numhalf_fixture_loop_pc != NULL);
  assert(numhalf_fixture_pt->framesize == 4);
  assert(numhalf_fixture_pt->sizebc == 13);
  memset(&fx, 0, sizeof(fx));

  setir(H_K_HALF, IR_KNUM, IRT_NUM, 0, 0);
  fx.ir[H_K_HALF_PAYLOAD].tv.u64 = UINT64_C(0x3fe0000000000000);
  setir(REF_TRUE, IR_KPRI, IRT_TRUE, 0, 0);
  setir(REF_FALSE, IR_KPRI, IRT_FALSE, 0, 0);
  setir(REF_NIL, IR_KPRI, IRT_NIL, 0, 0);

  setir(REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  setir(H_R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,
	3, IRSLOAD_TYPECHECK);
  setir(H_R_X_PRE, IR_ADD, IRT_NUM|IRT_ISPHI,
	H_R_X, H_K_HALF);
  setir(H_R_LIMIT, IR_SLOAD, IRT_NUM|IRT_GUARD,
	2, IRSLOAD_TYPECHECK);
  setir(H_R_PRE_GUARD, IR_GT, IRT_NUM|IRT_GUARD,
	H_R_LIMIT, H_R_X_PRE);
  setir(H_R_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  setir(H_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  setir(H_R_X_BODY, IR_ADD, IRT_NUM|IRT_ISPHI,
	H_R_X_PRE, H_K_HALF);
  setir(H_R_BODY_GUARD, IR_LT, IRT_NUM|IRT_GUARD,
	H_R_X_BODY, H_R_LIMIT);
  setir(H_R_X_PHI, IR_PHI, IRT_NUM, H_R_X_PRE, H_R_X_BODY);

  for (snapno = 0; snapno < 5; snapno++) {
    fx.snap[snapno].ref = snaprefs[snapno];
    fx.snap[snapno].mapofs = mapofs[snapno];
    fx.snap[snapno].nent = nent[snapno];
    fx.snap[snapno].nslots = nslots[snapno];
    fx.snap[snapno].topslot = 4;
  }
  fx.snapmap[2] = SNAP(3, 0, H_R_X_PRE);
  fx.snapmap[3] = SNAP(4, 0, H_R_X_PRE);
  fx.snapmap[6] = SNAP(3, 0, H_R_X_PRE);
  fx.snapmap[9] = SNAP(3, 0, H_R_X_PRE);
  fx.snapmap[12] = SNAP(3, 0, H_R_X_BODY);

  fx.T.nk = H_K_HALF;
  fx.T.nins = H_R_SEMANTIC_END;
  fx.T.ir = fx.ir;
  fx.T.nsnap = 5;
  fx.T.snap = fx.snap;
  fx.T.nsnapmap = 15;
  fx.T.snapmap = fx.snapmap;
  fx.T.traceno = 1;
  fx.T.link = 1;
  fx.T.root = 0;
  fx.T.linktype = LJ_TRLINK_LOOP;
  fx.T.sinktags = 0;
  fx.T.startins = loadbc(numhalf_fixture_loop_pc);
  trace_startpt_rel(&fx.T, numhalf_fixture_pt);
  setmref(fx.T.startpc, numhalf_fixture_loop_pc);
  for (snapno = 0; snapno < 5; snapno++)
    set_snapshot_payload(snapno, proto_bc(numhalf_fixture_pt)+pcpos[snapno], 0);

  J->parent = 0;
  J->exitno = 0;
  J->pt = numhalf_fixture_pt;
  J->baseslot = 1 + LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->loopref = H_R_LOOP;
  J->startpc = numhalf_fixture_loop_pc;
}

static void make_numstep_trace(jit_State *J)
{
  static const IRRef snaprefs[5] = {
    D_R_X, D_R_LIMIT, D_R_PRE_GUARD, D_R_LOOP, D_R_BODY_GUARD
  };
  static const uint16_t mapofs[5] = { 0, 2, 6, 9, 12 };
  static const uint8_t nent[5] = { 0, 2, 1, 1, 1 };
  static const uint8_t nslots[5] = { 5, 6, 5, 5, 5 };
  static const MSize pcpos[5] = { 7, 3, 12, 7, 12 };
  SnapNo snapno;

  assert(numstep_fixture_pt != NULL && numstep_fixture_loop_pc != NULL);
  assert(numstep_fixture_pt->framesize == 5);
  assert(numstep_fixture_pt->sizebc == 14);
  assert(numstep_fixture_pt->numparams == 2);
  memset(&fx, 0, sizeof(fx));

  setir(REF_TRUE, IR_KPRI, IRT_TRUE, 0, 0);
  setir(REF_FALSE, IR_KPRI, IRT_FALSE, 0, 0);
  setir(REF_NIL, IR_KPRI, IRT_NIL, 0, 0);

  setir(REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  setir(D_R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,
	4, IRSLOAD_TYPECHECK);
  setir(D_R_STEP, IR_SLOAD, IRT_NUM|IRT_GUARD,
	3, IRSLOAD_TYPECHECK);
  setir(D_R_X_PRE, IR_ADD, IRT_NUM|IRT_ISPHI,
	D_R_STEP, D_R_X);
  setir(D_R_LIMIT, IR_SLOAD, IRT_NUM|IRT_GUARD,
	2, IRSLOAD_TYPECHECK);
  setir(D_R_PRE_GUARD, IR_GT, IRT_NUM|IRT_GUARD,
	D_R_LIMIT, D_R_X_PRE);
  setir(D_R_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  setir(D_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  setir(D_R_X_BODY, IR_ADD, IRT_NUM|IRT_ISPHI,
	D_R_X_PRE, D_R_STEP);
  setir(D_R_BODY_GUARD, IR_LT, IRT_NUM|IRT_GUARD,
	D_R_X_BODY, D_R_LIMIT);
  setir(D_R_X_PHI, IR_PHI, IRT_NUM, D_R_X_PRE, D_R_X_BODY);

  for (snapno = 0; snapno < 5; snapno++) {
    fx.snap[snapno].ref = snaprefs[snapno];
    fx.snap[snapno].mapofs = mapofs[snapno];
    fx.snap[snapno].nent = nent[snapno];
    fx.snap[snapno].nslots = nslots[snapno];
    fx.snap[snapno].topslot = 5;
  }
  fx.snapmap[2] = SNAP(4, 0, D_R_X_PRE);
  fx.snapmap[3] = SNAP(5, 0, D_R_X_PRE);
  fx.snapmap[6] = SNAP(4, 0, D_R_X_PRE);
  fx.snapmap[9] = SNAP(4, 0, D_R_X_PRE);
  fx.snapmap[12] = SNAP(4, 0, D_R_X_BODY);

  fx.T.nk = REF_TRUE;
  fx.T.nins = D_R_SEMANTIC_END;
  fx.T.ir = fx.ir;
  fx.T.nsnap = 5;
  fx.T.snap = fx.snap;
  fx.T.nsnapmap = 15;
  fx.T.snapmap = fx.snapmap;
  fx.T.traceno = 1;
  fx.T.link = 1;
  fx.T.root = 0;
  fx.T.linktype = LJ_TRLINK_LOOP;
  fx.T.sinktags = 0;
  fx.T.startins = loadbc(numstep_fixture_loop_pc);
  trace_startpt_rel(&fx.T, numstep_fixture_pt);
  setmref(fx.T.startpc, numstep_fixture_loop_pc);
  for (snapno = 0; snapno < 5; snapno++)
    set_snapshot_payload(snapno, proto_bc(numstep_fixture_pt)+pcpos[snapno], 0);

  J->parent = 0;
  J->exitno = 0;
  J->pt = numstep_fixture_pt;
  J->baseslot = 1 + LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->loopref = D_R_LOOP;
  J->startpc = numstep_fixture_loop_pc;
}

static unsigned numacc_fixture_full_shape(void)
{
  BCIns comparison, arithmetic;
  MSize n;
  assert(numacc_fixture_pt != NULL && numacc_fixture_profile != NULL);
  assert(numacc_fixture_pt->sizebc == 13);
  comparison = loadbc(proto_bc(numacc_fixture_pt)+3);
  arithmetic = loadbc(proto_bc(numacc_fixture_pt)+8);
  for (n = 0;
	 n < sizeof(numacc_fixture_profiles)/sizeof(numacc_fixture_profiles[0]);
	 n++) {
    const NumaccFixtureProfile *profile = &numacc_fixture_profiles[n];
    if (bc_op(comparison) == profile->comparison_bc &&
	bc_a(comparison) == profile->comparison_a &&
	bc_d(comparison) == profile->comparison_d &&
	bc_op(arithmetic) == profile->recurrence_bc &&
	bc_a(arithmetic) == 3 && bc_b(arithmetic) == 3 &&
	bc_c(arithmetic) == 4)
      return profile->id;
  }
  return 0;
}

static const NumaccFixtureProfile *numacc_active_profile(void)
{
  assert(numacc_fixture_profile != NULL);
  assert(numacc_fixture_full_shape() == numacc_fixture_profile->id);
  return numacc_fixture_profile;
}

static void make_numacc_trace(jit_State *J)
{
  static const IRRef snaprefs[5] = {
    A_R_X, A_R_LIMIT, A_R_PRE_GUARD, A_R_LOOP, A_R_BODY_GUARD
  };
  static const uint16_t mapofs[5] = { 0, 2, 6, 9, 12 };
  static const uint8_t nent[5] = { 0, 2, 1, 1, 1 };
  static const uint8_t nslots[5] = { 5, 6, 5, 5, 5 };
  static const MSize pcpos[5] = { 6, 2, 11, 6, 11 };
  SnapNo snapno;
  const NumaccFixtureProfile *profile;

  assert(numacc_fixture_pt != NULL && numacc_fixture_loop_pc != NULL);
  assert(numacc_fixture_pt->framesize == 5);
  assert(numacc_fixture_pt->sizebc == 13);
  assert(numacc_fixture_pt->numparams == 3);
  profile = numacc_active_profile();
  memset(&fx, 0, sizeof(fx));

  setir(REF_TRUE, IR_KPRI, IRT_TRUE, 0, 0);
  setir(REF_FALSE, IR_KPRI, IRT_FALSE, 0, 0);
  setir(REF_NIL, IR_KPRI, IRT_NIL, 0, 0);

  setir(REF_BASE, IR_BASE, IRT_PGC, 0, 0);
  setir(A_R_X, IR_SLOAD, IRT_NUM|IRT_GUARD,
	2, IRSLOAD_TYPECHECK);
  setir(A_R_STEP, IR_SLOAD, IRT_NUM|IRT_GUARD,
	4, IRSLOAD_TYPECHECK);
  setir(A_R_X_PRE, profile->recurrence_op, IRT_NUM|IRT_ISPHI,
	profile->pre_left, profile->pre_right);
  setir(A_R_LIMIT, IR_SLOAD, IRT_NUM|IRT_GUARD,
	3, IRSLOAD_TYPECHECK);
  setir(A_R_PRE_GUARD, profile->precondition_op, IRT_NUM|IRT_GUARD,
	A_R_LIMIT, A_R_X_PRE);
  setir(A_R_LOOP, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  setir(A_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  setir(A_R_X_BODY, profile->recurrence_op, IRT_NUM|IRT_ISPHI,
	A_R_X_PRE, A_R_STEP);
  setir(A_R_BODY_GUARD, profile->body_op, IRT_NUM|IRT_GUARD,
	A_R_X_BODY, A_R_LIMIT);
  setir(A_R_X_PHI, IR_PHI, IRT_NUM, A_R_X_PRE, A_R_X_BODY);

  for (snapno = 0; snapno < 5; snapno++) {
    fx.snap[snapno].ref = snaprefs[snapno];
    fx.snap[snapno].mapofs = mapofs[snapno];
    fx.snap[snapno].nent = nent[snapno];
    fx.snap[snapno].nslots = nslots[snapno];
    fx.snap[snapno].topslot = 5;
  }
  fx.snapmap[2] = SNAP(2, 0, A_R_X_PRE);
  fx.snapmap[3] = SNAP(5, 0, A_R_X_PRE);
  fx.snapmap[6] = SNAP(2, 0, A_R_X_PRE);
  fx.snapmap[9] = SNAP(2, 0, A_R_X_PRE);
  fx.snapmap[12] = SNAP(2, 0, A_R_X_BODY);

  fx.T.nk = REF_TRUE;
  fx.T.nins = A_R_SEMANTIC_END;
  fx.T.ir = fx.ir;
  fx.T.nsnap = 5;
  fx.T.snap = fx.snap;
  fx.T.nsnapmap = 15;
  fx.T.snapmap = fx.snapmap;
  fx.T.traceno = 1;
  fx.T.link = 1;
  fx.T.root = 0;
  fx.T.linktype = LJ_TRLINK_LOOP;
  fx.T.sinktags = 0;
  fx.T.startins = loadbc(numacc_fixture_loop_pc);
  trace_startpt_rel(&fx.T, numacc_fixture_pt);
  setmref(fx.T.startpc, numacc_fixture_loop_pc);
  for (snapno = 0; snapno < 5; snapno++)
    set_snapshot_payload(snapno, proto_bc(numacc_fixture_pt)+pcpos[snapno], 0);

  J->parent = 0;
  J->exitno = 0;
  J->pt = numacc_fixture_pt;
  J->baseslot = 1 + LJ_FR2;
  J->framedepth = 0;
  J->retdepth = 0;
  J->loopref = A_R_LOOP;
  J->startpc = numacc_fixture_loop_pc;
}

static LJArm64IRReject expect_reject(jit_State *J,
		LJArm64IRRejectReason reason, IROp op)
{
  LJArm64IRReject reject;
  if (lj_asm_arm64_ir_admit(J, &fx.T, &reject))
    fprintf(stderr, "unexpected admission: wanted reason=%d op=%u\n",
	(int)reason, (unsigned)op);
  assert(reject.reason != LJ_ARM64_IR_REJECT_NONE);
  if (reject.reason != reason)
    fprintf(stderr, "reject reason mismatch: wanted=%d got=%d ref=%u "
	    "op=%u detail=%u\n", (int)reason, (int)reject.reason,
	    (unsigned)reject.ref, (unsigned)reject.op,
	    (unsigned)reject.detail);
  assert(reject.reason == reason);
  if (reject.op != op)
    fprintf(stderr, "reject opcode mismatch: reason=%d wanted=%u got=%u detail=%u\n",
	(int)reason, (unsigned)op, (unsigned)reject.op,
	(unsigned)reject.detail);
  assert(reject.op == op);
  return reject;
}

static LJArm64PostRAView make_postra_view(jit_State *J)
{
  LJArm64PostRAView view;
  make_trace(J);
  setir(R_END, IR_NOP, IRT_NIL, 0, 0);
  view.ir = fx.ir;
  view.snap = fx.snap;
  view.snapmap = fx.snapmap;
  view.proto_bc = proto_bc(fixture_pt);
  view.nins = R_END+1u;
  view.nk = fx.T.nk;
  view.nsnap = fx.T.nsnap;
  view.nsnapmap = fx.T.nsnapmap;
  view.spadjust = 0;
  view.proto_sizebc = fixture_pt->sizebc;
  view.root_topslot = fixture_pt->framesize;
  view.startins = fx.T.startins;
  view.base_delta = 0;
  return view;
}

static void expect_postra_result(LJArm64PostRAView *view, int admitted)
{
  IRRef semantic_nins = 0;
  int result = lj_asm_arm64_postra_admit(view, &semantic_nins);
  assert(result == admitted);
  if (admitted)
    assert(semantic_nins == R_END);
}

static LJArm64PostRAView make_numeric_postra_view(jit_State *J)
{
  LJArm64PostRAView view;
  make_numeric_trace(J);

  /* Exact post-RA allocation observed for the admitted trace. The PHIs match
  ** their right/body operands, as required by asm_phi() and its shuffle. */
  fx.ir[N_R_I].r = RID_X2;
  fx.ir[N_R_X].r = RID_D1;
  fx.ir[N_R_STEP].r = RID_D0;
  fx.ir[N_R_I_PRE].r = RID_X28;
  fx.ir[N_R_X_PRE].r = RID_D15;
  fx.ir[N_R_LIMIT].r = RID_X0;
  fx.ir[N_R_I_BODY].r = RID_X28;
  fx.ir[N_R_X_BODY].r = RID_D15;
  fx.ir[N_R_I_PHI].r = RID_X28;
  fx.ir[N_R_X_PHI].r = RID_D15;

  /* The real trace has one suffix: move the loop-carried integer from x28 to
  ** x27 starting at snapshot #4. */
  setir(N_R_RENAME, IR_RENAME, IRT_NIL, N_R_I_PRE, 4);
  fx.ir[N_R_RENAME].r = RID_X27;

  view.ir = fx.ir;
  view.snap = fx.snap;
  view.snapmap = fx.snapmap;
  view.proto_bc = proto_bc(numeric_fixture_pt);
  view.nins = N_R_POSTRA_END;
  view.nk = fx.T.nk;
  view.nsnap = fx.T.nsnap;
  view.nsnapmap = fx.T.nsnapmap;
  view.spadjust = 0;
  view.proto_sizebc = numeric_fixture_pt->sizebc;
  view.root_topslot = 6;
  view.startins = fx.T.startins;
  view.base_delta = 0;
  return view;
}

static void expect_numeric_postra_result(LJArm64PostRAView *view,
	int admitted)
{
  IRRef semantic_nins = 0;
  int result = lj_asm_arm64_postra_admit(view, &semantic_nins);
  assert(result == admitted);
  if (admitted)
    assert(semantic_nins == N_R_SEMANTIC_END);
}

static LJArm64PostRAView make_numhalf_postra_view(jit_State *J)
{
  LJArm64PostRAView view;
  make_numhalf_trace(J);

  /* Exact observed spill-free allocation. KNUM is rematerialized and keeps
  ** the allocator's initial marker; the loop-carried value stays in d15. */
  fx.ir[H_K_HALF].r = RID_INIT;
  fx.ir[H_K_HALF].s = SPS_NONE;
  fx.ir[H_R_X].r = RID_D2;
  fx.ir[H_R_X_PRE].r = RID_D15;
  fx.ir[H_R_LIMIT].r = RID_D0;
  fx.ir[H_R_X_BODY].r = RID_D15;
  fx.ir[H_R_X_PHI].r = RID_D15;
  setir(H_R_NOP, IR_NOP, IRT_NIL, 0, 0);

  view.ir = fx.ir;
  view.snap = fx.snap;
  view.snapmap = fx.snapmap;
  view.proto_bc = proto_bc(numhalf_fixture_pt);
  view.nins = H_R_POSTRA_END;
  view.nk = fx.T.nk;
  view.nsnap = fx.T.nsnap;
  view.nsnapmap = fx.T.nsnapmap;
  view.spadjust = 0;
  view.proto_sizebc = numhalf_fixture_pt->sizebc;
  view.root_topslot = 4;
  view.startins = fx.T.startins;
  view.base_delta = 0;
  return view;
}

static void expect_numhalf_postra_result(LJArm64PostRAView *view,
	int admitted)
{
  IRRef semantic_nins = 0;
  int result = lj_asm_arm64_postra_admit(view, &semantic_nins);
  assert(result == admitted);
  if (admitted)
    assert(semantic_nins == H_R_SEMANTIC_END);
}

static LJArm64PostRAView make_numstep_postra_view(jit_State *J)
{
  LJArm64PostRAView view;
  make_numstep_trace(J);

  /* Exact observed spill-free allocation. STEP and LIMIT stay invariant in
  ** distinct FPRs while the loop-carried family remains in d15. */
  fx.ir[D_R_X].r = RID_D2;
  fx.ir[D_R_STEP].r = RID_D1;
  fx.ir[D_R_X_PRE].r = RID_D15;
  fx.ir[D_R_LIMIT].r = RID_D0;
  fx.ir[D_R_X_BODY].r = RID_D15;
  fx.ir[D_R_X_PHI].r = RID_D15;
  setir(D_R_NOP, IR_NOP, IRT_NIL, 0, 0);

  view.ir = fx.ir;
  view.snap = fx.snap;
  view.snapmap = fx.snapmap;
  view.proto_bc = proto_bc(numstep_fixture_pt);
  view.nins = D_R_POSTRA_END;
  view.nk = fx.T.nk;
  view.nsnap = fx.T.nsnap;
  view.nsnapmap = fx.T.nsnapmap;
  view.spadjust = 0;
  view.proto_sizebc = numstep_fixture_pt->sizebc;
  view.root_topslot = 5;
  view.startins = fx.T.startins;
  view.base_delta = 0;
  return view;
}

static void expect_numstep_postra_result(LJArm64PostRAView *view,
	int admitted)
{
  IRRef semantic_nins = 0;
  int result = lj_asm_arm64_postra_admit(view, &semantic_nins);
  assert(result == admitted);
  if (admitted)
    assert(semantic_nins == D_R_SEMANTIC_END);
}

static LJArm64PostRAView make_numacc_postra_view(jit_State *J)
{
  LJArm64PostRAView view;
  make_numacc_trace(J);

  /* Exact observed spill-free allocation. STEP and LIMIT stay invariant in
  ** distinct FPRs while the loop-carried accumulator remains in d15. */
  fx.ir[A_R_X].r = RID_D2;
  fx.ir[A_R_STEP].r = RID_D1;
  fx.ir[A_R_X_PRE].r = RID_D15;
  fx.ir[A_R_LIMIT].r = RID_D0;
  fx.ir[A_R_X_BODY].r = RID_D15;
  fx.ir[A_R_X_PHI].r = RID_D15;
  setir(A_R_NOP, IR_NOP, IRT_NIL, 0, 0);

  view.ir = fx.ir;
  view.snap = fx.snap;
  view.snapmap = fx.snapmap;
  view.proto_bc = proto_bc(numacc_fixture_pt);
  view.nins = A_R_POSTRA_END;
  view.nk = fx.T.nk;
  view.nsnap = fx.T.nsnap;
  view.nsnapmap = fx.T.nsnapmap;
  view.spadjust = 0;
  view.proto_sizebc = numacc_fixture_pt->sizebc;
  view.root_topslot = 5;
  view.startins = fx.T.startins;
  view.base_delta = 0;
  return view;
}

static void expect_numacc_postra_result(LJArm64PostRAView *view,
	int admitted)
{
  IRRef semantic_nins = 0;
  int result = lj_asm_arm64_postra_admit(view, &semantic_nins);
  assert(result == admitted);
  if (admitted)
    assert(semantic_nins == A_R_SEMANTIC_END);
}

static void expect_numstep_reject(jit_State *J)
{
  LJArm64IRReject reject;
  assert(!lj_asm_arm64_ir_admit(J, &fx.T, &reject));
  assert(reject.reason != LJ_ARM64_IR_REJECT_NONE);
}

static void expect_numacc_reject(jit_State *J)
{
  LJArm64IRReject reject;
  assert(!lj_asm_arm64_ir_admit(J, &fx.T, &reject));
  assert(reject.reason != LJ_ARM64_IR_REJECT_NONE);
}

static void expect_numacc_semantic_result(jit_State *J, int admitted)
{
  LJArm64IRReject reject;
  int result = lj_asm_arm64_ir_admit(J, &fx.T, &reject);
  assert(result == admitted);
  if (admitted)
    assert(reject.reason == LJ_ARM64_IR_REJECT_NONE);
  else
    assert(reject.reason != LJ_ARM64_IR_REJECT_NONE);
}

static void expect_numhalf_reject(jit_State *J)
{
  LJArm64IRReject reject;
  assert(!lj_asm_arm64_ir_admit(J, &fx.T, &reject));
  assert(reject.reason != LJ_ARM64_IR_REJECT_NONE);
}

static void test_numhalf_postra_layout(jit_State *J)
{
  static const IRRef fprrefs[] = {
    H_R_X, H_R_X_PRE, H_R_LIMIT, H_R_X_BODY, H_R_X_PHI
  };
  LJArm64PostRAView view;
  MSize i;

  view = make_numhalf_postra_view(J);
  assert(fx.ir[H_K_HALF].o == IR_KNUM);
  assert(fx.ir[H_K_HALF].r == RID_INIT);
  assert(fx.ir[H_K_HALF].s == SPS_NONE);
  assert(fx.ir[H_K_HALF_PAYLOAD].tv.u64 ==
	 UINT64_C(0x3fe0000000000000));
  assert(fx.ir[H_R_X].r == RID_D2);
  assert(fx.ir[H_R_X_PRE].r == RID_D15);
  assert(fx.ir[H_R_LIMIT].r == RID_D0);
  assert(fx.ir[H_R_X_BODY].r == RID_D15);
  assert(fx.ir[H_R_X_PHI].r == RID_D15);
  assert(fx.ir[H_R_NOP].o == IR_NOP);
  expect_numhalf_postra_result(&view, 1);

  /* Invariants may use another allocatable FPR. The PHI family may move as
  ** a unit, but a partial move is not a realizable asm_phi() result. */
  view = make_numhalf_postra_view(J);
  fx.ir[H_R_X].r = RID_D3;
  fx.ir[H_R_LIMIT].r = RID_D4;
  expect_numhalf_postra_result(&view, 1);
  view = make_numhalf_postra_view(J);
  fx.ir[H_R_X_PRE].r = RID_D14;
  fx.ir[H_R_X_BODY].r = RID_D14;
  fx.ir[H_R_X_PHI].r = RID_D14;
  expect_numhalf_postra_result(&view, 1);
  view = make_numhalf_postra_view(J);
  fx.ir[H_R_X_PRE].r = RID_D14;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  fx.ir[H_R_X_BODY].r = RID_D14;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  fx.ir[H_R_X_PHI].r = RID_D14;
  expect_numhalf_postra_result(&view, 0);

  for (i = 0; i < sizeof(fprrefs)/sizeof(fprrefs[0]); i++) {
    view = make_numhalf_postra_view(J);
    fx.ir[fprrefs[i]].r = RID_X0;
    expect_numhalf_postra_result(&view, 0);
    view = make_numhalf_postra_view(J);
    fx.ir[fprrefs[i]].r = RID_MAX_FPR;
    expect_numhalf_postra_result(&view, 0);
    view = make_numhalf_postra_view(J);
    fx.ir[fprrefs[i]].s = 2;
    view.spadjust = 16;
    expect_numhalf_postra_result(&view, 0);
  }
  view = make_numhalf_postra_view(J);
  view.spadjust = 16;
  expect_numhalf_postra_result(&view, 0);

  /* KNUM is a two-slot constant and is rematerialized, never allocated. */
  view = make_numhalf_postra_view(J);
  fx.ir[H_K_HALF].r = RID_D0;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  fx.ir[H_K_HALF].s = 2;
  view.spadjust = 16;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  fx.ir[H_K_HALF].op12 = 1;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  fx.ir[H_K_HALF].o = IR_KINT64;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  fx.ir[H_K_HALF_PAYLOAD].tv.u64 ^= UINT64_C(1);
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  view.nk = H_K_HALF_PAYLOAD;
  expect_numhalf_postra_result(&view, 0);

  /* The pure-NUM profile has exactly one trailing NOP and no RENAME suffix. */
  view = make_numhalf_postra_view(J);
  fx.ir[H_R_NOP].t.irt = IRT_PGC;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  fx.ir[H_R_NOP].op1 = 1;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  fx.ir[H_R_NOP].prev = 1;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  setir(H_R_NOP, IR_RENAME, IRT_NIL, H_R_X_PRE, 3);
  fx.ir[H_R_NOP].r = RID_D14;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  view.nins = H_R_SEMANTIC_END;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  setir(H_R_POSTRA_END, IR_NOP, IRT_NIL, 0, 0);
  view.nins++;
  expect_numhalf_postra_result(&view, 0);

  /* Post-RA validation independently rechecks the exact semantic and
  ** snapshot certificate instead of trusting the recorder pass. */
  view = make_numhalf_postra_view(J);
  fx.ir[H_R_PRE_GUARD].o = IR_GE;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  fx.ir[H_R_BODY_GUARD].o = IR_LE;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  fx.ir[H_R_X_BODY].op2 = H_R_LIMIT;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  fx.snap[1].nent = 1;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  fx.snapmap[12] = SNAP(3, 0, H_R_X_PRE);
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  set_snapshot_payload(3, proto_bc(numhalf_fixture_pt)+8, 0);
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  view.nsnapmap = 14;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  view.proto_sizebc = 12;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  view.root_topslot = 5;
  expect_numhalf_postra_result(&view, 0);
  view = make_numhalf_postra_view(J);
  view.startins = loadbc(fixture_forl_pc);
  expect_numhalf_postra_result(&view, 0);
}

static void test_numstep_postra_layout(jit_State *J)
{
  static const IRRef value_refs[] = {
    D_R_X, D_R_STEP, D_R_X_PRE, D_R_LIMIT, D_R_X_BODY, D_R_X_PHI
  };
  static const IRRef semantic_refs[] = {
    D_R_X, D_R_STEP, D_R_X_PRE, D_R_LIMIT, D_R_PRE_GUARD,
    D_R_LOOP, D_R_XPOLL, D_R_X_BODY, D_R_BODY_GUARD, D_R_X_PHI
  };
  static const uint16_t entryofs[5] = { 2, 3, 6, 9, 12 };
  LJArm64PostRAView view;
  MSize i;

  view = make_numstep_postra_view(J);
  assert(fx.ir[D_R_X].r == RID_D2);
  assert(fx.ir[D_R_STEP].r == RID_D1);
  assert(fx.ir[D_R_X_PRE].r == RID_D15);
  assert(fx.ir[D_R_LIMIT].r == RID_D0);
  assert(fx.ir[D_R_X_BODY].r == RID_D15);
  assert(fx.ir[D_R_X_PHI].r == RID_D15);
  assert(fx.ir[D_R_NOP].o == IR_NOP);
  expect_numstep_postra_result(&view, 1);

  /* Any allocatable FPR assignment is valid when the liveness aliases hold. */
  view = make_numstep_postra_view(J);
  fx.ir[D_R_X].r = RID_D3;
  fx.ir[D_R_STEP].r = RID_D4;
  fx.ir[D_R_LIMIT].r = RID_D5;
  expect_numstep_postra_result(&view, 1);
  view = make_numstep_postra_view(J);
  fx.ir[D_R_X_PRE].r = RID_D14;
  fx.ir[D_R_X_BODY].r = RID_D14;
  fx.ir[D_R_X_PHI].r = RID_D14;
  expect_numstep_postra_result(&view, 1);

  /* X dies in the first ADD and may alias LIMIT or its destination. */
  view = make_numstep_postra_view(J);
  fx.ir[D_R_X].r = fx.ir[D_R_LIMIT].r;
  expect_numstep_postra_result(&view, 1);
  view = make_numstep_postra_view(J);
  fx.ir[D_R_X].r = fx.ir[D_R_X_PHI].r;
  expect_numstep_postra_result(&view, 1);

  /* STEP, LIMIT and the loop-carried family overlap in lifetime. X and STEP
  ** are simultaneous first-ADD inputs. Every prohibited alias fails closed. */
  view = make_numstep_postra_view(J);
  fx.ir[D_R_STEP].r = fx.ir[D_R_X_PHI].r;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  fx.ir[D_R_LIMIT].r = fx.ir[D_R_X_PHI].r;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  fx.ir[D_R_STEP].r = fx.ir[D_R_LIMIT].r;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  fx.ir[D_R_X].r = fx.ir[D_R_STEP].r;
  expect_numstep_postra_result(&view, 0);

  /* PHI allocation is indivisible. */
  view = make_numstep_postra_view(J);
  fx.ir[D_R_X_PRE].r = RID_D14;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  fx.ir[D_R_X_BODY].r = RID_D14;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  fx.ir[D_R_X_PHI].r = RID_D14;
  expect_numstep_postra_result(&view, 0);

  for (i = 0; i < sizeof(value_refs)/sizeof(value_refs[0]); i++) {
    view = make_numstep_postra_view(J);
    fx.ir[value_refs[i]].r = RID_X0;
    expect_numstep_postra_result(&view, 0);
    view = make_numstep_postra_view(J);
    fx.ir[value_refs[i]].r = RID_MAX_FPR;
    expect_numstep_postra_result(&view, 0);
    view = make_numstep_postra_view(J);
    fx.ir[value_refs[i]].s = 2;
    view.spadjust = 16;
    expect_numstep_postra_result(&view, 0);
  }
  view = make_numstep_postra_view(J);
  view.spadjust = 16;
  expect_numstep_postra_result(&view, 0);

  /* Exactly one zeroed NOP follows the semantic root; spills and RENAMEs are
  ** never part of this profile. */
  view = make_numstep_postra_view(J);
  fx.ir[D_R_NOP].t.irt = IRT_PGC;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  fx.ir[D_R_NOP].op1 = 1;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  fx.ir[D_R_NOP].prev = 1;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  setir(D_R_NOP, IR_RENAME, IRT_NIL, D_R_X_PRE, 3);
  fx.ir[D_R_NOP].r = RID_D14;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  view.nins = D_R_SEMANTIC_END;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  setir(D_R_POSTRA_END, IR_NOP, IRT_NIL, 0, 0);
  view.nins++;
  expect_numstep_postra_result(&view, 0);

  /* Recheck every semantic tuple after allocation. */
  for (i = 0; i < sizeof(semantic_refs)/sizeof(semantic_refs[0]); i++) {
    IRRef ref = semantic_refs[i];
    view = make_numstep_postra_view(J);
    fx.ir[ref].o = IR_NOP;
    expect_numstep_postra_result(&view, 0);
    view = make_numstep_postra_view(J);
    fx.ir[ref].t.irt ^= IRT_GUARD;
    expect_numstep_postra_result(&view, 0);
    view = make_numstep_postra_view(J);
    fx.ir[ref].op1 ^= 1u;
    expect_numstep_postra_result(&view, 0);
    view = make_numstep_postra_view(J);
    fx.ir[ref].op2 ^= 1u;
    expect_numstep_postra_result(&view, 0);
  }
  view = make_numstep_postra_view(J);
  fx.ir[REF_BASE].o = IR_NOP;
  expect_numstep_postra_result(&view, 0);

  /* Snapshot headers, entries and footers are independently immutable. */
  for (i = 0; i < 5; i++) {
    view = make_numstep_postra_view(J);
    fx.snap[i].ref++;
    expect_numstep_postra_result(&view, 0);
    view = make_numstep_postra_view(J);
    fx.snap[i].mapofs++;
    expect_numstep_postra_result(&view, 0);
    view = make_numstep_postra_view(J);
    fx.snap[i].nent++;
    expect_numstep_postra_result(&view, 0);
    view = make_numstep_postra_view(J);
    fx.snap[i].nslots++;
    expect_numstep_postra_result(&view, 0);
    view = make_numstep_postra_view(J);
    fx.snap[i].topslot = 4;
    expect_numstep_postra_result(&view, 0);
    view = make_numstep_postra_view(J);
    set_snapshot_payload((SnapNo)i, proto_bc(numstep_fixture_pt)+1, 0);
    expect_numstep_postra_result(&view, 0);
    view = make_numstep_postra_view(J);
    set_snapshot_payload((SnapNo)i, proto_bc(numstep_fixture_pt)+
	(i == 2 || i == 4 ? 12 : 7), 1);
    expect_numstep_postra_result(&view, 0);
  }
  for (i = 0; i < sizeof(entryofs)/sizeof(entryofs[0]); i++) {
    view = make_numstep_postra_view(J);
    fx.snapmap[entryofs[i]] ^= 1u;
    expect_numstep_postra_result(&view, 0);
  }

  view = make_numstep_postra_view(J);
  view.nk = REF_TRUE-1u;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  view.nsnap = 4;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  view.nsnap = 6;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  view.nsnapmap = 14;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  view.nsnapmap = 16;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  view.proto_sizebc = 13;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  view.root_topslot = 4;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  view.base_delta = 1;
  expect_numstep_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  view.startins = loadbc(fixture_forl_pc);
  expect_numstep_postra_result(&view, 0);
}

static void test_numacc_postra_layout(jit_State *J)
{
  const NumaccFixtureProfile *profile = numacc_active_profile();
  static const IRRef value_refs[] = {
    A_R_X, A_R_STEP, A_R_X_PRE, A_R_LIMIT, A_R_X_BODY, A_R_X_PHI
  };
  static const IRRef semantic_refs[] = {
    A_R_X, A_R_STEP, A_R_X_PRE, A_R_LIMIT, A_R_PRE_GUARD,
    A_R_LOOP, A_R_XPOLL, A_R_X_BODY, A_R_BODY_GUARD, A_R_X_PHI
  };
  static const uint16_t entryofs[5] = { 2, 3, 6, 9, 12 };
  static const IROp comparison_ops[] = {
    IR_GT, IR_GE, IR_LT, IR_LE, IR_EQ, IR_NE,
    IR_ULT, IR_UGE, IR_ULE, IR_UGT
  };
  LJArm64PostRAView view;
  MSize i;
  IROp expected_pre = profile->precondition_op;
  IROp expected_body = profile->body_op;

  view = make_numacc_postra_view(J);
  assert(fx.ir[A_R_X].r == RID_D2);
  assert(fx.ir[A_R_STEP].r == RID_D1);
  assert(fx.ir[A_R_X_PRE].r == RID_D15);
  assert(fx.ir[A_R_LIMIT].r == RID_D0);
  assert(fx.ir[A_R_X_BODY].r == RID_D15);
  assert(fx.ir[A_R_X_PHI].r == RID_D15);
  assert(fx.ir[A_R_NOP].o == IR_NOP);
  expect_numacc_postra_result(&view, 1);

  /* Register numbers are not frozen, but all liveness relationships are. */
  view = make_numacc_postra_view(J);
  fx.ir[A_R_X].r = RID_D3;
  fx.ir[A_R_STEP].r = RID_D4;
  fx.ir[A_R_LIMIT].r = RID_D5;
  expect_numacc_postra_result(&view, 1);
  view = make_numacc_postra_view(J);
  fx.ir[A_R_X_PRE].r = RID_D14;
  fx.ir[A_R_X_BODY].r = RID_D14;
  fx.ir[A_R_X_PHI].r = RID_D14;
  expect_numacc_postra_result(&view, 1);

  /* X dies in the first recurrence and may alias LIMIT or its destination. */
  view = make_numacc_postra_view(J);
  fx.ir[A_R_X].r = fx.ir[A_R_LIMIT].r;
  expect_numacc_postra_result(&view, 1);
  view = make_numacc_postra_view(J);
  fx.ir[A_R_X].r = fx.ir[A_R_X_PHI].r;
  expect_numacc_postra_result(&view, 1);

  /* STEP, LIMIT and the PHI family overlap; X and STEP are simultaneous
  ** first-recurrence inputs. These four aliases are therefore impossible. */
  view = make_numacc_postra_view(J);
  fx.ir[A_R_STEP].r = fx.ir[A_R_X_PHI].r;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  fx.ir[A_R_LIMIT].r = fx.ir[A_R_X_PHI].r;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  fx.ir[A_R_STEP].r = fx.ir[A_R_LIMIT].r;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  fx.ir[A_R_X].r = fx.ir[A_R_STEP].r;
  expect_numacc_postra_result(&view, 0);

  view = make_numacc_postra_view(J);
  fx.ir[A_R_X_PRE].r = RID_D14;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  fx.ir[A_R_X_BODY].r = RID_D14;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  fx.ir[A_R_X_PHI].r = RID_D14;
  expect_numacc_postra_result(&view, 0);

  for (i = 0; i < sizeof(value_refs)/sizeof(value_refs[0]); i++) {
    view = make_numacc_postra_view(J);
    fx.ir[value_refs[i]].r = RID_X0;
    expect_numacc_postra_result(&view, 0);
    view = make_numacc_postra_view(J);
    fx.ir[value_refs[i]].r = RID_MAX_FPR;
    expect_numacc_postra_result(&view, 0);
    view = make_numacc_postra_view(J);
    fx.ir[value_refs[i]].s = 2;
    view.spadjust = 16;
    expect_numacc_postra_result(&view, 0);
  }
  view = make_numacc_postra_view(J);
  view.spadjust = 16;
  expect_numacc_postra_result(&view, 0);

  /* Exactly one zeroed NOP follows the semantic root. */
  view = make_numacc_postra_view(J);
  fx.ir[A_R_NOP].t.irt = IRT_PGC;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  fx.ir[A_R_NOP].op1 = 1;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  fx.ir[A_R_NOP].op2 = 1;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  fx.ir[A_R_NOP].prev = 1;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  setir(A_R_NOP, IR_RENAME, IRT_NIL, A_R_X_PRE, 3);
  fx.ir[A_R_NOP].r = RID_D14;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  view.nins = A_R_SEMANTIC_END;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  setir(A_R_POSTRA_END, IR_NOP, IRT_NIL, 0, 0);
  view.nins++;
  expect_numacc_postra_result(&view, 0);

  /* Post-RA rechecks every semantic tuple, not only allocation metadata. */
  for (i = 0; i < sizeof(semantic_refs)/sizeof(semantic_refs[0]); i++) {
    IRRef ref = semantic_refs[i];
    view = make_numacc_postra_view(J);
    fx.ir[ref].o = IR_NOP;
    expect_numacc_postra_result(&view, 0);
    view = make_numacc_postra_view(J);
    fx.ir[ref].t.irt ^= IRT_GUARD;
    expect_numacc_postra_result(&view, 0);
    view = make_numacc_postra_view(J);
    fx.ir[ref].op1 ^= 1u;
    expect_numacc_postra_result(&view, 0);
    view = make_numacc_postra_view(J);
    fx.ir[ref].op2 ^= 1u;
    expect_numacc_postra_result(&view, 0);
  }
  view = make_numacc_postra_view(J);
  fx.ir[REF_BASE].o = IR_NOP;
  expect_numacc_postra_result(&view, 0);

  /* Arithmetic operand direction is exact, including for commutative ADD. */
  view = make_numacc_postra_view(J);
  fx.ir[A_R_X_PRE].op1 = (IRRef1)profile->pre_right;
  fx.ir[A_R_X_PRE].op2 = (IRRef1)profile->pre_left;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  fx.ir[A_R_X_BODY].op1 = A_R_STEP;
  fx.ir[A_R_X_BODY].op2 = A_R_X_PRE;
  expect_numacc_postra_result(&view, 0);

  /* Bytecode comparison polarity selects exactly one signed IR pair. Both
  ** unsigned neighbours and reversed operands remain invalid after RA. */
  for (i = 0; i < sizeof(comparison_ops)/sizeof(comparison_ops[0]); i++) {
    if (comparison_ops[i] != expected_pre) {
      view = make_numacc_postra_view(J);
      fx.ir[A_R_PRE_GUARD].o = (IROp1)comparison_ops[i];
      expect_numacc_postra_result(&view, 0);
    }
    if (comparison_ops[i] != expected_body) {
      view = make_numacc_postra_view(J);
      fx.ir[A_R_BODY_GUARD].o = (IROp1)comparison_ops[i];
      expect_numacc_postra_result(&view, 0);
    }
  }
  view = make_numacc_postra_view(J);
  fx.ir[A_R_PRE_GUARD].op1 = A_R_X_PRE;
  fx.ir[A_R_PRE_GUARD].op2 = A_R_LIMIT;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  fx.ir[A_R_BODY_GUARD].op1 = A_R_LIMIT;
  fx.ir[A_R_BODY_GUARD].op2 = A_R_X_BODY;
  expect_numacc_postra_result(&view, 0);

  /* Every snapshot header, restored entry and footer remains exact. */
  for (i = 0; i < 5; i++) {
    view = make_numacc_postra_view(J);
    fx.snap[i].ref++;
    expect_numacc_postra_result(&view, 0);
    view = make_numacc_postra_view(J);
    fx.snap[i].mapofs++;
    expect_numacc_postra_result(&view, 0);
    view = make_numacc_postra_view(J);
    fx.snap[i].nent++;
    expect_numacc_postra_result(&view, 0);
    view = make_numacc_postra_view(J);
    fx.snap[i].nslots++;
    expect_numacc_postra_result(&view, 0);
    view = make_numacc_postra_view(J);
    fx.snap[i].topslot = 4;
    expect_numacc_postra_result(&view, 0);
    view = make_numacc_postra_view(J);
    set_snapshot_payload((SnapNo)i, proto_bc(numacc_fixture_pt)+1, 0);
    expect_numacc_postra_result(&view, 0);
    view = make_numacc_postra_view(J);
    set_snapshot_payload((SnapNo)i, proto_bc(numacc_fixture_pt)+
	(i == 2 || i == 4 ? 11 : 6), 1);
    expect_numacc_postra_result(&view, 0);
  }
  for (i = 0; i < sizeof(entryofs)/sizeof(entryofs[0]); i++) {
    view = make_numacc_postra_view(J);
    fx.snapmap[entryofs[i]] ^= 1u;
    expect_numacc_postra_result(&view, 0);
  }

  view = make_numacc_postra_view(J);
  view.nk = REF_TRUE-1u;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  view.nsnap = 4;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  view.nsnap = 6;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  view.nsnapmap = 14;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  view.nsnapmap = 16;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  view.proto_sizebc = 12;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  view.proto_sizebc = 14;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  view.root_topslot = 4;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  view.root_topslot = 6;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  view.base_delta = 1;
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  view.startins ^= (BCIns)(UINT32_C(1) << 8);
  expect_numacc_postra_result(&view, 0);
  view = make_numacc_postra_view(J);
  view.startins = loadbc(fixture_forl_pc);
  expect_numacc_postra_result(&view, 0);

  /* Dispatching either no-constant profile through the other's prototype and
  ** snapshot certificate cannot silently broaden the grammar. */
  view = make_numacc_postra_view(J);
  view.proto_bc = proto_bc(numstep_fixture_pt);
  view.proto_sizebc = numstep_fixture_pt->sizebc;
  view.startins = loadbc(numstep_fixture_loop_pc);
  expect_numacc_postra_result(&view, 0);
  view = make_numstep_postra_view(J);
  view.proto_bc = proto_bc(numacc_fixture_pt);
  view.proto_sizebc = numacc_fixture_pt->sizebc;
  view.startins = loadbc(numacc_fixture_loop_pc);
  expect_numacc_postra_result(&view, 0);
}

static void test_postra_spill_layout(jit_State *J)
{
  LJArm64PostRAView view;

  view = make_postra_view(J);
  expect_postra_result(&view, 1);

  view = make_postra_view(J);
  fx.ir[R_SUM1].s = 2;
  fx.ir[R_SUM2].s = 3;
  expect_postra_result(&view, 1);

  view = make_postra_view(J);
  fx.ir[R_SUM1].s = 4;
  view.spadjust = 16;
  expect_postra_result(&view, 1);

  view = make_postra_view(J);
  fx.ir[R_SUM1].s = 255;
  view.spadjust = 1008;
  expect_postra_result(&view, 1);

  view = make_postra_view(J);
  view.spadjust = 4;
  expect_postra_result(&view, 0);
  view.spadjust = 8;
  expect_postra_result(&view, 0);
  view.spadjust = 12;
  expect_postra_result(&view, 0);
  view.spadjust = 1024;
  expect_postra_result(&view, 0);

  view = make_postra_view(J);
  fx.ir[R_SUM1].s = 4;
  expect_postra_result(&view, 0);
  view.spadjust = 32;
  expect_postra_result(&view, 0);

  view = make_postra_view(J);
  fx.ir[R_SUM1].s = 255;
  view.spadjust = 992;
  expect_postra_result(&view, 0);

  view = make_postra_view(J);
  fx.ir[R_SUM1].s = 1;
  expect_postra_result(&view, 0);

  view = make_postra_view(J);
  fx.ir[R_SUM1].prev = REGSP(RID_INIT, SPS_NONE);
  expect_postra_result(&view, 0);

  view = make_postra_view(J);
  fx.ir[R_PRECOND].s = 2;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.ir[R_LOOP].s = 2;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.ir[R_XPOLL].s = 2;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.ir[R_SUM1].t.irt = IRT_NUM|IRT_GUARD|IRT_ISPHI;
  fx.ir[R_SUM1].s = 2;
  expect_postra_result(&view, 0);

  view = make_postra_view(J);
  setir(R_END, IR_RENAME, IRT_NIL, R_SUM1, 0);
  fx.ir[R_END].r = RID_X0;
  fx.ir[R_SUM1].s = 2;
  expect_postra_result(&view, 1);

  view = make_postra_view(J);
  setir(R_END, IR_RENAME, IRT_NIL, R_PRECOND, 0);
  fx.ir[R_END].r = RID_X0;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  setir(R_END, IR_RENAME, IRT_NIL, R_SUM1, fx.T.nsnap);
  fx.ir[R_END].r = RID_X0;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  setir(R_END, IR_RENAME, IRT_NIL, R_SUM1, 0);
  fx.ir[R_END].r = RID_MAX_GPR;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  setir(R_END, IR_RENAME, IRT_NIL, R_SUM1, 0);
  fx.ir[R_END].r = RID_X0;
  fx.ir[R_END].s = 2;
  expect_postra_result(&view, 0);

  view = make_postra_view(J);
  setir(R_END, IR_RENAME, IRT_NIL, R_SUM1, 0);
  fx.ir[R_END].r = RID_X0;
  setir(R_END+1u, IR_NOP, IRT_NIL, 0, 0);
  view.nins++;
  expect_postra_result(&view, 0);

  view = make_postra_view(J);
  fx.snap[1].mapofs = fx.T.nsnapmap+1u;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snap[1].nent = fx.T.nsnapmap;
  expect_postra_result(&view, 0);

  /* Revalidate the exact frozen snapshot partition, not just its outer
  ** allocation bounds. These mutations stay in-bounds but must never be
  ** reinterpreted as another snapshot's payload. */
  view = make_postra_view(J);
  fx.snap[1].mapofs = 1;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snap[1].nent = 1;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snap[1].nslots = 3;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snap[1].nslots = (uint8_t)(fixture_pt->framesize+2u+LJ_FR2);
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snap[1].topslot = (uint8_t)(fixture_pt->framesize-1u);
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snap[1].topslot = (uint8_t)(fixture_pt->framesize+1u);
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snapmap[7] = SNAP(2, 0, R_SUM2);
  expect_postra_result(&view, 0);

  view = make_postra_view(J);
  fx.snapmap[2] = SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL);
  expect_postra_result(&view, 1);

  view = make_postra_view(J);
  fx.snapmap[6] = SNAP(2, SNAP_NORESTORE, R_SUM1);
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snapmap[6] = SNAP(2, SNAP_FRAME, R_SUM1);
  expect_postra_result(&view, 0);

  /* A dynamic value must already exist at the snapshot reference. */
  view = make_postra_view(J);
  fx.snapmap[6] = SNAP(2, 0, R_BODY1);
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snap[1].ref = R_SUM1;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snap[0].ref = REF_FIRST-1u;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snap[3].ref = R_END;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snap[3].ref = R_PRECOND;
  fx.snapmap[10] = SNAP(2, 0, R_SUM1);
  fx.snapmap[11] = SNAP(3, 0, R_SUM2);
  expect_postra_result(&view, 0);

  /* Integer constants are rematerialized only from the exact admitted
  ** [nk, REF_TRUE) KINT interval. */
  view = make_postra_view(J);
  fx.snapmap[6] = SNAP(2, 0, K_STEP);
  expect_postra_result(&view, 1);
  view = make_postra_view(J);
  fx.snapmap[6] = SNAP(2, 0, K_ZERO-1u);
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  fx.snapmap[6] = SNAP(2, 0, K_STEP);
  setir(K_STEP, IR_KNUM, IRT_NUM, 0, 0);
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  view.nk = 0;
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  view.nk = REF_TRUE+1u;
  expect_postra_result(&view, 0);

  /* The exact tail payload is part of exit restoration: its low byte is the
  ** root base delta and its pointer must be aligned inside the held proto. */
  view = make_postra_view(J);
  set_snapshot_payload(2, fixture_snapshot_pc, 1);
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  set_snapshot_payload(2,
	(const BCIns *)((uintptr_t)proto_bc(fixture_pt)+1u), 0);
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  set_snapshot_payload(2, (const BCIns *)((uintptr_t)proto_bc(fixture_pt)-
	sizeof(BCIns)), 0);
  expect_postra_result(&view, 0);
  view = make_postra_view(J);
  set_snapshot_payload(2, proto_bc(fixture_pt)+fixture_pt->sizebc, 0);
  expect_postra_result(&view, 0);
}

static void test_numeric_postra_layout(jit_State *J)
{
  LJArm64PostRAView view;

  view = make_numeric_postra_view(J);
  assert(fx.ir[N_R_RENAME].o == IR_RENAME);
  assert(fx.ir[N_R_RENAME].op1 == N_R_I_PRE);
  assert(fx.ir[N_R_RENAME].op2 == 4);
  assert(fx.ir[N_R_RENAME].r == RID_X27);
  expect_numeric_postra_result(&view, 1);

  /* asm_phi() assigns the right/body register to the PHI and its final
  ** shuffle resolves the left/head value into the same register. A different
  ** FPR is valid only when all three move together. */
  view = make_numeric_postra_view(J);
  fx.ir[N_R_X_PHI].r = RID_D14;
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  fx.ir[N_R_X_PRE].r = RID_D14;
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  fx.ir[N_R_X_BODY].r = RID_D14;
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  fx.ir[N_R_X_PRE].r = RID_D14;
  fx.ir[N_R_X_BODY].r = RID_D14;
  fx.ir[N_R_X_PHI].r = RID_D14;
  expect_numeric_postra_result(&view, 1);
  view = make_numeric_postra_view(J);
  fx.ir[N_R_I_PHI].r = RID_X27;
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  fx.ir[N_R_X_PHI].s = 2;
  view.spadjust = 16;
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  fx.ir[N_R_X_PHI].r = RID_X0;
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  fx.ir[N_R_X_PHI].r = RID_MAX_FPR;
  expect_numeric_postra_result(&view, 0);

  /* A NUM producer is invalid even when no snapshot refers to it. */
  view = make_numeric_postra_view(J);
  fx.ir[N_R_X].r = RID_X0;
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  fx.ir[N_R_X].r = RID_MAX_FPR;
  expect_numeric_postra_result(&view, 0);

  /* Snapshot-restored NUM values are subject to the same FPR certificate. */
  view = make_numeric_postra_view(J);
  fx.ir[N_R_X_PRE].r = RID_X0;
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  fx.ir[N_R_X_PRE].r = RID_MAX_FPR;
  expect_numeric_postra_result(&view, 0);

  view = make_numeric_postra_view(J);
  fx.ir[N_R_X_PRE].s = 2;
  view.spadjust = 16;
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  view.spadjust = 16;
  expect_numeric_postra_result(&view, 0);

  /* A type-directed synthetic NUM RENAME may choose any allocatable FPR. */
  view = make_numeric_postra_view(J);
  fx.ir[N_R_RENAME].op1 = N_R_X_PRE;
  fx.ir[N_R_RENAME].r = RID_D14;
  expect_numeric_postra_result(&view, 1);
  view = make_numeric_postra_view(J);
  fx.ir[N_R_RENAME].op1 = N_R_X_PRE;
  fx.ir[N_R_RENAME].r = RID_X27;
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  fx.ir[N_R_RENAME].op1 = N_R_X_PRE;
  fx.ir[N_R_RENAME].r = RID_MAX_FPR;
  expect_numeric_postra_result(&view, 0);

  /* The post-RA pass independently retains the adjacent-family closures. */
  view = make_numeric_postra_view(J);
  setir(N_K_ONE, IR_KNUM, IRT_NUM, 0, 0);
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  setir(N_R_PRE_GUARD, IR_GT, IRT_NUM|IRT_GUARD,
	N_R_X_PRE, N_R_X);
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  setir(N_R_X_PRE, IR_CONV, IRT_NUM|IRT_ISPHI,
	N_R_X, IRCONV_NUM_INT);
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  setir(N_R_X_PRE, IR_SUB, IRT_NUM|IRT_ISPHI, N_R_STEP, N_R_X);
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  setir(N_R_X_PRE, IR_MUL, IRT_NUM|IRT_ISPHI, N_R_STEP, N_R_X);
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  setir(N_R_X_PRE, IR_DIV, IRT_NUM|IRT_ISPHI, N_R_STEP, N_R_X);
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  setir(N_R_X_PRE, IR_CALLN, IRT_NUM, N_R_X, IRCALL_lj_vm_modi);
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  setir(N_R_X_PRE, IR_TNEW, IRT_TAB, 0, 0);
  expect_numeric_postra_result(&view, 0);
  view = make_numeric_postra_view(J);
  view.startins = loadbc(fixture_forl_pc);
  expect_numeric_postra_result(&view, 0);
}

typedef struct ThrowContext {
  jit_State *J;
  GCtrace *T;
} ThrowContext;

static TValue *validate_throw_cp(lua_State *L, lua_CFunction dummy, void *ud)
{
  ThrowContext *ctx = (ThrowContext *)ud;
  UNUSED(L); UNUSED(dummy);
  lj_asm_trace(ctx->J, ctx->T);
  return NULL;
}

static void test_deterministic_trace_error(lua_State *L, jit_State *J)
{
  ThrowContext ctx;
  int status;
  make_trace(J);
  setir(R_SUM1, IR_AREF, IRT_PGC, R_A, R_B);
  ctx.J = J;
  ctx.T = &fx.T;
  status = lj_vm_cpcall(L, NULL, &ctx, validate_throw_cp);
  assert(status == LUA_ERRRUN);
  assert(tvisint(L->top-1));
  assert(numberVint(L->top-1) == LJ_TRERR_NYIIR);
  assert(tvisint(&J->errinfo));
  assert(numberVint(&J->errinfo) == IR_AREF);
  L->top--;
}

static void test_snapshot_payload_rejections(jit_State *J)
{
  uintptr_t lo = (uintptr_t)proto_bc(fixture_pt);
  uintptr_t hi = lo + (uintptr_t)fixture_pt->sizebc*sizeof(BCIns);

  make_trace(J);
  set_snapshot_payload(2, fixture_snapshot_pc, 1);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  set_snapshot_payload(2, fixture_snapshot_pc, 0xff);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  set_snapshot_payload(2, (const BCIns *)(lo+1), 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  set_snapshot_payload(2, (const BCIns *)(lo-sizeof(BCIns)), 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  set_snapshot_payload(2, (const BCIns *)hi, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  set_snapshot_payload(2, (const BCIns *)(hi+sizeof(BCIns)), 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);
}

static void test_start_metadata_rejections(jit_State *J)
{
  const BCIns *bc = proto_bc(fixture_pt);
  uintptr_t lo = (uintptr_t)bc;
  uintptr_t hi = lo + (uintptr_t)fixture_pt->sizebc*sizeof(BCIns);
  BCIns loop = loadbc(fixture_loop_pc);
  const BCIns *loopbackpc = fixture_loop_pc + bc_j(loop);
  BCIns loopback = loadbc(loopbackpc);
  MSize sizebc = fixture_pt->sizebc;

  make_trace(J);
  setmref(fx.T.startpc, fixture_snapshot_pc);
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);

  make_trace(J);
  fx.T.startins = BCINS_AJ(BC_LOOP, bc_a(loop)^1u, bc_j(loop));
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);

  make_trace(J);
  setmref(fx.T.startpc, (const BCIns *)(lo+1));
  J->startpc = (const BCIns *)(lo+1);
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);

  make_trace(J);
  setmref(fx.T.startpc, (const BCIns *)(lo-sizeof(BCIns)));
  J->startpc = (const BCIns *)(lo-sizeof(BCIns));
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);

  make_trace(J);
  setmref(fx.T.startpc, (const BCIns *)hi);
  J->startpc = (const BCIns *)hi;
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);

  make_trace(J);
  setmref(fx.T.startpc, (const BCIns *)(hi+sizeof(BCIns)));
  J->startpc = (const BCIns *)(hi+sizeof(BCIns));
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);

  make_trace(J);
  fixture_pt->sizebc = 0;
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);
  fixture_pt->sizebc = sizebc;

  make_trace(J);
  fixture_pt->sizebc = (MSize)(fixture_loop_pc-bc);
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);
  fixture_pt->sizebc = sizebc;

  make_forl_trace(J);
  /* The paired bytecode and hidden loads are valid, but this LOOP-shaped
  ** fixture has no exact pair of FORL induction ADDs. */
  expect_reject(J, LJ_ARM64_IR_REJECT_OPERAND, IR_ADD);

  bc_publish((const uint32_t *)fixture_loop_pc,
	     BCINS_AJ(BC_LOOP, bc_a(loop), 0));
  make_trace(J);
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);
  bc_publish((const uint32_t *)fixture_loop_pc, loop);

  make_trace(J);
  bc_publish((const uint32_t *)loopbackpc,
	     BCINS_AJ(BC_MOV, bc_a(loopback), bc_j(loopback)));
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);
  bc_publish((const uint32_t *)loopbackpc, loopback);

  make_trace(J);
  bc_publish((const uint32_t *)loopbackpc,
	     BCINS_AJ(BC_JMP, bc_a(loopback),
		      fixture_loop_pc-loopbackpc));
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);
  bc_publish((const uint32_t *)loopbackpc, loopback);

  assert((MSize)fixture_pt->framesize + 1u <= 0xffu);
  bc_publish((const uint32_t *)fixture_loop_pc,
	     BCINS_AJ(BC_LOOP, fixture_pt->framesize+1u, bc_j(loop)));
  make_trace(J);
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);
  bc_publish((const uint32_t *)fixture_loop_pc, loop);
}

static void test_phi_and_xpoll_rejections(jit_State *J)
{
  IRRef ref, pre, loop, xpoll, post, phi, end;

  make_trace(J);
  setir(R_BODY2, IR_PHI, IRT_INT, R_SUM1, R_BODY1);
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LE);

  make_trace(J);
  fx.ir[R_PHI1].op1 = R_BODY2;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_PHI);

  make_trace(J);
  fx.ir[R_PHI1].op2 = R_A;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_PHI);

  make_trace(J);
  fx.ir[R_SUM1].t.irt &= (uint8_t)~IRT_ISPHI;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_PHI);

  make_trace(J);
  fx.ir[R_A].t.irt |= IRT_ISPHI;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SLOAD);

  make_trace(J);
  fx.ir[R_PHI1].t.irt |= IRT_ISPHI;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_PHI);

  make_trace(J);
  fx.ir[R_PHI2].op1 = R_SUM1;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_PHI);

  make_trace(J);
  pre = REF_FIRST;
  loop = pre + LJ_MAX_PHI + 1u;
  xpoll = loop + 1u;
  post = xpoll + 1u;
  phi = post + LJ_MAX_PHI + 1u;
  end = phi + LJ_MAX_PHI + 1u;
  assert(end <= sizeof(fx.ir)/sizeof(fx.ir[0]));
  for (ref = pre; ref < loop; ref++)
    setir(ref, IR_SLOAD, IRT_INT|IRT_GUARD|IRT_ISPHI, 2,
	  IRSLOAD_TYPECHECK);
  setir(loop, IR_LOOP, IRT_NIL|IRT_GUARD, 0, 0);
  setir(xpoll, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
  for (ref = post; ref < phi; ref++)
    setir(ref, IR_ADDOV, IRT_INT|IRT_GUARD|IRT_ISPHI,
	  pre+(ref-post), K_STEP);
  for (ref = phi; ref < end; ref++)
    setir(ref, IR_PHI, IRT_INT, pre+(ref-phi), post+(ref-phi));
  fx.T.nins = end;
  J->loopref = loop;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_PHI);

  make_trace(J);
  fx.snap[3].ref = R_XPOLL;
  fx.snapmap[10] = SNAP(2, 0, R_SUM1);
  fx.snapmap[11] = SNAP(3, 0, R_SUM2);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);
}

static void test_numeric_positive_and_negative(jit_State *J)
{
  LJArm64IRReject reject;

  make_numeric_trace(J);
  if (!lj_asm_arm64_ir_admit(J, &fx.T, &reject))
    fprintf(stderr, "numeric admission failed: reason=%d ref=%u op=%u "
	    "detail=%u\n", (int)reject.reason, (unsigned)reject.ref,
	    (unsigned)reject.op, (unsigned)reject.detail);
  assert(reject.reason == LJ_ARM64_IR_REJECT_NONE);

  /* NUM SLOADs use exactly the ordinary type-checking stack-load mode. */
  make_numeric_trace(J);
  fx.ir[N_R_X].t.irt = IRT_NUM;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SLOAD);
  make_numeric_trace(J);
  fx.ir[N_R_X].op1 = 1;
  expect_reject(J, LJ_ARM64_IR_REJECT_OPERAND, IR_SLOAD);
  make_numeric_trace(J);
  fx.ir[N_R_X].op2 = IRSLOAD_READONLY;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SLOAD);

  /* NUM ADD is loop-carried, reads only earlier NUM producers, and never
  ** accepts a rematerialized KINT as an implicit conversion. */
  make_numeric_trace(J);
  fx.ir[N_R_X_PRE].t.irt = IRT_NUM;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_ADD);
  make_numeric_trace(J);
  fx.ir[N_R_X_PRE].op1 = N_R_I;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_ADD);
  make_numeric_trace(J);
  fx.ir[N_R_X_PRE].op1 = N_R_X_BODY;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_ADD);
  make_numeric_trace(J);
  fx.ir[N_R_X_PRE].op1 = N_K_ONE;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_ADD);

  make_numeric_trace(J);
  fx.ir[N_R_X_PHI].op2 = N_R_I_BODY;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_PHI);

  make_numeric_trace(J);
  setir(N_K_ONE, IR_KNUM, IRT_NUM, 0, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_CONSTANT, IR_KNUM);

  make_numeric_trace(J);
  setir(N_R_PRE_GUARD, IR_GT, IRT_NUM|IRT_GUARD,
	N_R_X_PRE, N_R_X);
  /* Ordered NUM guards now reach the exact per-root shape discriminator;
  ** this mixed-root mutation must still fail, but rejection order is not API. */
  expect_numhalf_reject(J);

#define REJECT_NUMERIC_ADJACENT(op, type, left, right) \
  do { \
    make_numeric_trace(J); \
    setir(N_R_X_PRE, (op), (type), (left), (right)); \
    expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, (op)); \
  } while (0)
  REJECT_NUMERIC_ADJACENT(IR_CONV, IRT_NUM|IRT_ISPHI,
	N_R_X, IRCONV_NUM_INT);
  REJECT_NUMERIC_ADJACENT(IR_MUL, IRT_NUM|IRT_ISPHI,
	N_R_STEP, N_R_X);
  REJECT_NUMERIC_ADJACENT(IR_DIV, IRT_NUM|IRT_ISPHI,
	N_R_STEP, N_R_X);
#undef REJECT_NUMERIC_ADJACENT

  /* SUB now has a dedicated NUM profile, so this mixed-loop mutation reaches
  ** the profile/type discriminator rather than the generic opcode closure. */
  make_numeric_trace(J);
  setir(N_R_X_PRE, IR_SUB, IRT_NUM|IRT_ISPHI, N_R_STEP, N_R_X);
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SUB);

  make_numeric_trace(J);
  setir(N_R_X_PRE, IR_CALLN, IRT_NUM, N_R_X, IRCALL_lj_vm_modi);
  reject = expect_reject(J, LJ_ARM64_IR_REJECT_CALL, IR_CALLN);
  assert(reject.detail == IRCALL_lj_vm_modi);

  make_numeric_trace(J);
  setir(N_R_X_PRE, IR_TNEW, IRT_TAB, 0, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, IR_TNEW);

  make_numeric_trace(J);
  fx.T.startins = loadbc(fixture_forl_pc);
  setmref(fx.T.startpc, fixture_forl_pc);
  J->startpc = fixture_forl_pc;
  assert(!lj_asm_arm64_ir_admit(J, &fx.T, &reject));
  assert(reject.reason != LJ_ARM64_IR_REJECT_NONE);
}

static void test_numhalf_positive_and_negative(jit_State *J)
{
  static const IROp wrong_pre_guards[] = {
    IR_LT, IR_GE, IR_LE, IR_EQ, IR_NE
  };
  static const IROp wrong_body_guards[] = {
    IR_GT, IR_GE, IR_LE, IR_EQ, IR_NE
  };
  static const MSize wrong_pcpos[5] = { 8, 4, 10, 8, 10 };
  LJArm64IRReject reject;
  MSize i;

  make_numhalf_trace(J);
  if (!lj_asm_arm64_ir_admit(J, &fx.T, &reject))
    fprintf(stderr, "NUM half admission failed: reason=%d ref=%u op=%u "
	    "detail=%u\n", (int)reject.reason, (unsigned)reject.ref,
	    (unsigned)reject.op, (unsigned)reject.detail);
  assert(reject.reason == LJ_ARM64_IR_REJECT_NONE);

  /* The only admitted FP constant is the canonical two-slot +0.5 KNUM. */
  make_numhalf_trace(J);
  fx.ir[H_K_HALF].o = IR_KINT64;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_K_HALF].t.irt = IRT_I64;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_K_HALF].op12 = 1;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_K_HALF_PAYLOAD].tv.u64 ^= UINT64_C(1);
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_K_HALF_PAYLOAD].tv.u64 = UINT64_C(0xbfe0000000000000);
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_K_HALF_PAYLOAD].tv.u64 = UINT64_C(0x7ff0000000000000);
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_K_HALF_PAYLOAD].tv.u64 = UINT64_C(0x7ff8000000000000);
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_K_HALF_PAYLOAD].o = IR_KNUM;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.T.nk = H_K_HALF_PAYLOAD;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.T.nk = H_K_HALF-1u;
  setir(H_K_HALF-1u, IR_KINT, IRT_INT, 7, 0);
  expect_numhalf_reject(J);

  /* Slots, modes, operand order and PHI marks are frozen to the observed
  ** recorder output; the constant payload slot is never a usable IR ref. */
  make_numhalf_trace(J);
  fx.ir[H_R_X].op1 = 2;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X].op2 = IRSLOAD_READONLY;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X].t.irt = IRT_NUM;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_LIMIT].op1 = 3;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_LIMIT].t.irt = IRT_INT|IRT_GUARD;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X_PRE].op1 = H_R_LIMIT;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X_PRE].op1 = H_K_HALF;
  fx.ir[H_R_X_PRE].op2 = H_R_X;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X_PRE].op2 = H_K_HALF_PAYLOAD;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X_PRE].t.irt = IRT_NUM;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X_BODY].op1 = H_R_X;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X_BODY].op1 = H_K_HALF;
  fx.ir[H_R_X_BODY].op2 = H_R_X_PRE;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X_BODY].op2 = H_R_LIMIT;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X_BODY].t.irt = IRT_NUM;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X_PHI].op1 = H_R_X;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X_PHI].op2 = H_R_X_PRE;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_X_PHI].t.irt = IRT_NUM|IRT_ISPHI;
  expect_numhalf_reject(J);

  for (i = 0;
	 i < sizeof(wrong_pre_guards)/sizeof(wrong_pre_guards[0]); i++) {
    make_numhalf_trace(J);
    fx.ir[H_R_PRE_GUARD].o = (IROp1)wrong_pre_guards[i];
    expect_numhalf_reject(J);
  }
  for (i = 0;
	 i < sizeof(wrong_body_guards)/sizeof(wrong_body_guards[0]); i++) {
    make_numhalf_trace(J);
    fx.ir[H_R_BODY_GUARD].o = (IROp1)wrong_body_guards[i];
    expect_numhalf_reject(J);
  }
  make_numhalf_trace(J);
  fx.ir[H_R_PRE_GUARD].op1 = H_R_X_PRE;
  fx.ir[H_R_PRE_GUARD].op2 = H_R_LIMIT;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_BODY_GUARD].op1 = H_R_LIMIT;
  fx.ir[H_R_BODY_GUARD].op2 = H_R_X_BODY;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_PRE_GUARD].t.irt = IRT_INT|IRT_GUARD;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_PRE_GUARD].t.irt = IRT_NUM|IRT_GUARD|IRT_ISPHI;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_BODY_GUARD].t.irt = IRT_NUM;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_LOOP].t.irt = IRT_NIL;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.ir[H_R_XPOLL].op1 = 0;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  J->loopref = H_R_LOOP-1u;
  expect_numhalf_reject(J);

#define REJECT_NUMHALF_ADJACENT(op, type, left, right) \
  do { \
    make_numhalf_trace(J); \
    setir(H_R_X_PRE, (op), (type), (left), (right)); \
    expect_numhalf_reject(J); \
  } while (0)
  REJECT_NUMHALF_ADJACENT(IR_CONV, IRT_NUM|IRT_ISPHI,
	H_R_X, IRCONV_NUM_INT);
  REJECT_NUMHALF_ADJACENT(IR_SUB, IRT_NUM|IRT_ISPHI,
	H_R_X, H_K_HALF);
  REJECT_NUMHALF_ADJACENT(IR_MUL, IRT_NUM|IRT_ISPHI,
	H_R_X, H_K_HALF);
  REJECT_NUMHALF_ADJACENT(IR_DIV, IRT_NUM|IRT_ISPHI,
	H_R_X, H_K_HALF);
#undef REJECT_NUMHALF_ADJACENT
  make_numhalf_trace(J);
  setir(H_R_X_PRE, IR_CALLN, IRT_NUM, H_R_X, IRCALL_lj_vm_modi);
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  setir(H_R_X_PRE, IR_TNEW, IRT_TAB, 0, 0);
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  setir(H_R_SEMANTIC_END, IR_NOP, IRT_NIL, 0, 0);
  fx.T.nins++;
  expect_numhalf_reject(J);

  /* Every snapshot field and footer position is part of this root's proof. */
  for (i = 0; i < 5; i++) {
    make_numhalf_trace(J);
    fx.snap[i].ref++;
    expect_numhalf_reject(J);
    make_numhalf_trace(J);
    fx.snap[i].mapofs++;
    expect_numhalf_reject(J);
    make_numhalf_trace(J);
    fx.snap[i].nent++;
    expect_numhalf_reject(J);
    make_numhalf_trace(J);
    fx.snap[i].nslots++;
    expect_numhalf_reject(J);
    make_numhalf_trace(J);
    fx.snap[i].topslot = 3;
    expect_numhalf_reject(J);
    make_numhalf_trace(J);
    set_snapshot_payload((SnapNo)i,
	proto_bc(numhalf_fixture_pt)+wrong_pcpos[i], 0);
    expect_numhalf_reject(J);
  }
  make_numhalf_trace(J);
  fx.snapmap[2] = SNAP(2, 0, H_R_X_PRE);
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.snapmap[3] = SNAP(4, 0, H_R_X);
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.snapmap[6] = SNAP(3, 0, H_R_X);
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.snapmap[6] = SNAP(3, 0, H_K_HALF);
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.snapmap[9] = SNAP(3, SNAP_NORESTORE, H_R_X_PRE);
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.snapmap[12] = SNAP(3, 0, H_R_X_PRE);
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.T.nsnap = 4;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.T.nsnap = 6;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.T.nsnapmap = 14;
  expect_numhalf_reject(J);
  make_numhalf_trace(J);
  fx.T.nsnapmap = 16;
  expect_numhalf_reject(J);

  /* Prototype geometry is part of the exact profile, too. */
  make_numhalf_trace(J);
  numhalf_fixture_pt->framesize = 5;
  expect_numhalf_reject(J);
  numhalf_fixture_pt->framesize = 4;
  make_numhalf_trace(J);
  numhalf_fixture_pt->sizebc = 12;
  expect_numhalf_reject(J);
  numhalf_fixture_pt->sizebc = 13;

  make_numhalf_trace(J);
  fx.T.startins = loadbc(fixture_forl_pc);
  setmref(fx.T.startpc, fixture_forl_pc);
  J->startpc = fixture_forl_pc;
  expect_numhalf_reject(J);
}

static void test_numstep_positive_and_negative(jit_State *J)
{
  static const IRRef semantic_refs[] = {
    D_R_X, D_R_STEP, D_R_X_PRE, D_R_LIMIT, D_R_PRE_GUARD,
    D_R_LOOP, D_R_XPOLL, D_R_X_BODY, D_R_BODY_GUARD, D_R_X_PHI
  };
  static const IROp wrong_pre_guards[] = {
    IR_LT, IR_GE, IR_LE, IR_EQ, IR_NE
  };
  static const IROp wrong_body_guards[] = {
    IR_GT, IR_GE, IR_LE, IR_EQ, IR_NE
  };
  static const MSize wrong_pcpos[5] = { 8, 4, 11, 8, 11 };
  static const uint16_t entryofs[5] = { 2, 3, 6, 9, 12 };
  LJArm64IRReject reject;
  MSize i, bitno;

  make_numstep_trace(J);
  if (!lj_asm_arm64_ir_admit(J, &fx.T, &reject))
    fprintf(stderr, "NUM dynamic-step admission failed: reason=%d ref=%u "
	    "op=%u detail=%u\n", (int)reject.reason,
	    (unsigned)reject.ref, (unsigned)reject.op,
	    (unsigned)reject.detail);
  assert(reject.reason == LJ_ARM64_IR_REJECT_NONE);

  /* There are no IR constants in this profile. Neither an integer constant
  ** nor the separately admitted two-slot half constant may contaminate it. */
  make_numstep_trace(J);
  fx.T.nk = REF_TRUE-1u;
  setir(REF_TRUE-1u, IR_KINT, IRT_INT, 1, 0);
  expect_numstep_reject(J);
  make_numstep_trace(J);
  fx.T.nk = H_K_HALF;
  setir(H_K_HALF, IR_KNUM, IRT_NUM, 0, 0);
  fx.ir[H_K_HALF_PAYLOAD].tv.u64 = UINT64_C(0x3fe0000000000000);
  expect_numstep_reject(J);

  /* Conversely, hiding constants below nk cannot turn any older fixture into
  ** this root: profile selection and exact shape must agree. */
  make_numhalf_trace(J);
  fx.T.nk = REF_TRUE;
  expect_numstep_reject(J);
  make_numeric_trace(J);
  fx.T.nk = REF_TRUE;
  expect_numstep_reject(J);
  make_trace(J);
  fx.T.nk = REF_TRUE;
  expect_numstep_reject(J);

  /* Every IR tuple field in the exact ten-reference grammar is significant. */
  for (i = 0; i < sizeof(semantic_refs)/sizeof(semantic_refs[0]); i++) {
    IRRef ref = semantic_refs[i];
    make_numstep_trace(J);
    fx.ir[ref].o = IR_NOP;
    expect_numstep_reject(J);
    make_numstep_trace(J);
    fx.ir[ref].t.irt ^= IRT_GUARD;
    expect_numstep_reject(J);
    make_numstep_trace(J);
    fx.ir[ref].op1 ^= 1u;
    expect_numstep_reject(J);
    make_numstep_trace(J);
    fx.ir[ref].op2 ^= 1u;
    expect_numstep_reject(J);
  }
  make_numstep_trace(J);
  fx.ir[REF_BASE].o = IR_NOP;
  expect_numstep_reject(J);
  make_numstep_trace(J);
  fx.ir[D_R_X_PRE].op1 = D_R_X;
  fx.ir[D_R_X_PRE].op2 = D_R_STEP;
  expect_numstep_reject(J);
  make_numstep_trace(J);
  fx.ir[D_R_X_BODY].op1 = D_R_STEP;
  fx.ir[D_R_X_BODY].op2 = D_R_X_PRE;
  expect_numstep_reject(J);
  make_numstep_trace(J);
  fx.ir[D_R_X_PHI].op1 = D_R_X;
  expect_numstep_reject(J);
  make_numstep_trace(J);
  fx.ir[D_R_X_PHI].op2 = D_R_X_PRE;
  expect_numstep_reject(J);
  make_numstep_trace(J);
  J->loopref = D_R_LOOP-1u;
  expect_numstep_reject(J);
  make_numstep_trace(J);
  fx.T.nins--;
  expect_numstep_reject(J);
  make_numstep_trace(J);
  setir(D_R_SEMANTIC_END, IR_NOP, IRT_NIL, 0, 0);
  fx.T.nins++;
  expect_numstep_reject(J);

  for (i = 0;
	 i < sizeof(wrong_pre_guards)/sizeof(wrong_pre_guards[0]); i++) {
    make_numstep_trace(J);
    fx.ir[D_R_PRE_GUARD].o = (IROp1)wrong_pre_guards[i];
    expect_numstep_reject(J);
  }
  for (i = 0;
	 i < sizeof(wrong_body_guards)/sizeof(wrong_body_guards[0]); i++) {
    make_numstep_trace(J);
    fx.ir[D_R_BODY_GUARD].o = (IROp1)wrong_body_guards[i];
    expect_numstep_reject(J);
  }

#define REJECT_NUMSTEP_ADJACENT(op, type, left, right) \
  do { \
    make_numstep_trace(J); \
    setir(D_R_X_PRE, (op), (type), (left), (right)); \
    expect_numstep_reject(J); \
  } while (0)
  REJECT_NUMSTEP_ADJACENT(IR_CONV, IRT_NUM|IRT_ISPHI,
	D_R_X, IRCONV_NUM_INT);
  REJECT_NUMSTEP_ADJACENT(IR_SUB, IRT_NUM|IRT_ISPHI,
	D_R_STEP, D_R_X);
  REJECT_NUMSTEP_ADJACENT(IR_MUL, IRT_NUM|IRT_ISPHI,
	D_R_STEP, D_R_X);
  REJECT_NUMSTEP_ADJACENT(IR_DIV, IRT_NUM|IRT_ISPHI,
	D_R_STEP, D_R_X);
#undef REJECT_NUMSTEP_ADJACENT
  make_numstep_trace(J);
  setir(D_R_X_PRE, IR_CALLN, IRT_NUM, D_R_X, IRCALL_lj_vm_modi);
  expect_numstep_reject(J);
  make_numstep_trace(J);
  setir(D_R_X_PRE, IR_TNEW, IRT_TAB, 0, 0);
  expect_numstep_reject(J);

  /* Every snapshot header, restored entry and footer is exact. */
  for (i = 0; i < 5; i++) {
    make_numstep_trace(J);
    fx.snap[i].ref++;
    expect_numstep_reject(J);
    make_numstep_trace(J);
    fx.snap[i].mapofs++;
    expect_numstep_reject(J);
    make_numstep_trace(J);
    fx.snap[i].nent++;
    expect_numstep_reject(J);
    make_numstep_trace(J);
    fx.snap[i].nslots++;
    expect_numstep_reject(J);
    make_numstep_trace(J);
    fx.snap[i].topslot = 4;
    expect_numstep_reject(J);
    make_numstep_trace(J);
    set_snapshot_payload((SnapNo)i,
	proto_bc(numstep_fixture_pt)+wrong_pcpos[i], 0);
    expect_numstep_reject(J);
    make_numstep_trace(J);
    set_snapshot_payload((SnapNo)i,
	proto_bc(numstep_fixture_pt)+wrong_pcpos[i], 1);
    expect_numstep_reject(J);
  }
  for (i = 0; i < sizeof(entryofs)/sizeof(entryofs[0]); i++) {
    make_numstep_trace(J);
    fx.snapmap[entryofs[i]] ^= 1u;
    expect_numstep_reject(J);
  }
  make_numstep_trace(J);
  fx.snapmap[9] = SNAP(4, SNAP_NORESTORE, D_R_X_PRE);
  expect_numstep_reject(J);
  make_numstep_trace(J);
  fx.T.nsnap = 4;
  expect_numstep_reject(J);
  make_numstep_trace(J);
  fx.T.nsnap = 6;
  expect_numstep_reject(J);
  make_numstep_trace(J);
  fx.T.nsnapmap = 14;
  expect_numstep_reject(J);
  make_numstep_trace(J);
  fx.T.nsnapmap = 16;
  expect_numstep_reject(J);
  make_numstep_trace(J);
  J->baseslot = 2 + LJ_FR2;
  expect_numstep_reject(J);

  /* The source prototype is also a complete certificate. Mutate opcode, A
  ** and D/B/C bits at every bytecode position and restore after each probe. */
  for (i = 0; i < 14; i++) {
    const BCIns *pc = proto_bc(numstep_fixture_pt)+i;
    BCIns saved = loadbc(pc);
    static const uint32_t masks[3] = {
      UINT32_C(1), UINT32_C(1) << 8, UINT32_C(1) << 16
    };
    for (bitno = 0; bitno < 3; bitno++) {
      make_numstep_trace(J);
      bc_publish((const uint32_t *)pc, saved ^ masks[bitno]);
      expect_numstep_reject(J);
      bc_publish((const uint32_t *)pc, saved);
    }
  }

  make_numstep_trace(J);
  numstep_fixture_pt->framesize = 4;
  expect_numstep_reject(J);
  numstep_fixture_pt->framesize = 5;
  make_numstep_trace(J);
  numstep_fixture_pt->framesize = 6;
  expect_numstep_reject(J);
  numstep_fixture_pt->framesize = 5;
  make_numstep_trace(J);
  numstep_fixture_pt->sizebc = 13;
  expect_numstep_reject(J);
  numstep_fixture_pt->sizebc = 14;
  make_numstep_trace(J);
  numstep_fixture_pt->sizebc = 15;
  expect_numstep_reject(J);
  numstep_fixture_pt->sizebc = 14;
  make_numstep_trace(J);
  numstep_fixture_pt->numparams = 1;
  expect_numstep_reject(J);
  numstep_fixture_pt->numparams = 2;
  make_numstep_trace(J);
  numstep_fixture_pt->numparams = 3;
  expect_numstep_reject(J);
  numstep_fixture_pt->numparams = 2;
  make_numstep_trace(J);
  numstep_fixture_pt->sizeuv = 1;
  expect_numstep_reject(J);
  numstep_fixture_pt->sizeuv = 0;
  make_numstep_trace(J);
  numstep_fixture_pt->sizekn = 0;
  expect_numstep_reject(J);
  numstep_fixture_pt->sizekn = 1;
  make_numstep_trace(J);
  numstep_fixture_pt->sizekn = 2;
  expect_numstep_reject(J);
  numstep_fixture_pt->sizekn = 1;
  make_numstep_trace(J);
  numstep_fixture_pt->sizekgc = 1;
  expect_numstep_reject(J);
  numstep_fixture_pt->sizekgc = 0;

  /* The prototype KNUM is exactly finite +0.5, even though it is not part of
  ** the recorded IR constant range. */
  make_numstep_trace(J);
  proto_knumtv(numstep_fixture_pt, 0)->u64 ^= UINT64_C(1);
  expect_numstep_reject(J);
  proto_knumtv(numstep_fixture_pt, 0)->u64 = UINT64_C(0x3fe0000000000000);
  make_numstep_trace(J);
  proto_knumtv(numstep_fixture_pt, 0)->u64 = UINT64_C(0xbfe0000000000000);
  expect_numstep_reject(J);
  proto_knumtv(numstep_fixture_pt, 0)->u64 = UINT64_C(0x3fe0000000000000);
  make_numstep_trace(J);
  proto_knumtv(numstep_fixture_pt, 0)->u64 = UINT64_C(0x7ff0000000000000);
  expect_numstep_reject(J);
  proto_knumtv(numstep_fixture_pt, 0)->u64 = UINT64_C(0x3fe0000000000000);
  make_numstep_trace(J);
  proto_knumtv(numstep_fixture_pt, 0)->u64 = UINT64_C(0x7ff8000000000000);
  expect_numstep_reject(J);
  proto_knumtv(numstep_fixture_pt, 0)->u64 = UINT64_C(0x3fe0000000000000);

  make_numstep_trace(J);
  setmref(fx.T.startpc, proto_bc(numstep_fixture_pt)+5);
  J->startpc = proto_bc(numstep_fixture_pt)+5;
  expect_numstep_reject(J);
  make_numstep_trace(J);
  fx.T.startins ^= (BCIns)(UINT32_C(1) << 8);
  expect_numstep_reject(J);
  make_numstep_trace(J);
  fx.T.startins = loadbc(fixture_forl_pc);
  setmref(fx.T.startpc, fixture_forl_pc);
  J->startpc = fixture_forl_pc;
  expect_numstep_reject(J);
}

static void test_numacc_positive_and_negative(jit_State *J)
{
  const NumaccFixtureProfile *profile = numacc_active_profile();
  static const IRRef semantic_refs[] = {
    A_R_X, A_R_STEP, A_R_X_PRE, A_R_LIMIT, A_R_PRE_GUARD,
    A_R_LOOP, A_R_XPOLL, A_R_X_BODY, A_R_BODY_GUARD, A_R_X_PHI
  };
  static const IROp wrong_pre_guards[] = {
    IR_GT, IR_GE, IR_LT, IR_LE, IR_EQ, IR_NE,
    IR_ULT, IR_UGE, IR_ULE, IR_UGT
  };
  static const IROp wrong_body_guards[] = {
    IR_LT, IR_LE, IR_GT, IR_GE, IR_EQ, IR_NE,
    IR_ULT, IR_UGE, IR_ULE, IR_UGT
  };
  static const MSize wrong_pcpos[5] = { 7, 3, 10, 7, 10 };
  static const uint16_t entryofs[5] = { 2, 3, 6, 9, 12 };
  LJArm64IRReject reject;
  MSize i, bitno;
  IROp expected_pre = profile->precondition_op;
  IROp expected_body = profile->body_op;

  make_numacc_trace(J);
  if (!lj_asm_arm64_ir_admit(J, &fx.T, &reject))
	  fprintf(stderr, "NUM dynamic-accumulator profile %u admission failed: "
	    "reason=%d ref=%u op=%u detail=%u\n",
	    profile->id, (int)reject.reason,
	    (unsigned)reject.ref, (unsigned)reject.op,
	    (unsigned)reject.detail);
  assert(reject.reason == LJ_ARM64_IR_REJECT_NONE);

  /* This profile has no trace constants and no prototype constants. */
  make_numacc_trace(J);
  fx.T.nk = REF_TRUE-1u;
  setir(REF_TRUE-1u, IR_KINT, IRT_INT, 1, 0);
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.T.nk = H_K_HALF;
  setir(H_K_HALF, IR_KNUM, IRT_NUM, 0, 0);
  fx.ir[H_K_HALF_PAYLOAD].tv.u64 = UINT64_C(0x3fe0000000000000);
  expect_numacc_reject(J);

  /* Older constant-bearing roots cannot masquerade as this grammar. */
  make_numhalf_trace(J);
  fx.T.nk = REF_TRUE;
  expect_numacc_reject(J);
  make_numeric_trace(J);
  fx.T.nk = REF_TRUE;
  expect_numacc_reject(J);
  make_trace(J);
  fx.T.nk = REF_TRUE;
  expect_numacc_reject(J);

  /* The two no-constant NUM roots differ in both stack-slot semantics and
  ** source prototype. Combining either half with the other must stay closed. */
  make_numacc_trace(J);
  fx.ir[A_R_X].op1 = 4;
  fx.ir[A_R_STEP].op1 = 3;
  fx.ir[A_R_LIMIT].op1 = 2;
  expect_numacc_reject(J);
  make_numstep_trace(J);
  fx.ir[D_R_X].op1 = 2;
  fx.ir[D_R_STEP].op1 = 4;
  fx.ir[D_R_LIMIT].op1 = 3;
  expect_numacc_reject(J);

  /* Every field of every semantic IR tuple is independently required. */
  for (i = 0; i < sizeof(semantic_refs)/sizeof(semantic_refs[0]); i++) {
    IRRef ref = semantic_refs[i];
    make_numacc_trace(J);
    fx.ir[ref].o = IR_NOP;
    expect_numacc_reject(J);
    make_numacc_trace(J);
    fx.ir[ref].t.irt ^= IRT_GUARD;
    expect_numacc_reject(J);
    make_numacc_trace(J);
    fx.ir[ref].op1 ^= 1u;
    expect_numacc_reject(J);
    make_numacc_trace(J);
    fx.ir[ref].op2 ^= 1u;
    expect_numacc_reject(J);
  }
  make_numacc_trace(J);
  fx.ir[REF_BASE].o = IR_NOP;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.ir[A_R_X_PRE].op1 = profile->pre_right;
  fx.ir[A_R_X_PRE].op2 = profile->pre_left;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.ir[A_R_X_BODY].op1 = A_R_STEP;
  fx.ir[A_R_X_BODY].op2 = A_R_X_PRE;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.ir[A_R_X_PHI].op1 = A_R_X;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.ir[A_R_X_PHI].op2 = A_R_X_PRE;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  J->loopref = A_R_LOOP-1u;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.T.nins--;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  setir(A_R_SEMANTIC_END, IR_NOP, IRT_NIL, 0, 0);
  fx.T.nins++;
  expect_numacc_reject(J);

  for (i = 0;
	 i < sizeof(wrong_pre_guards)/sizeof(wrong_pre_guards[0]); i++) {
    if (wrong_pre_guards[i] == expected_pre)
      continue;
    make_numacc_trace(J);
    fx.ir[A_R_PRE_GUARD].o = (IROp1)wrong_pre_guards[i];
    expect_numacc_reject(J);
  }
  for (i = 0;
	 i < sizeof(wrong_body_guards)/sizeof(wrong_body_guards[0]); i++) {
    if (wrong_body_guards[i] == expected_body)
      continue;
    make_numacc_trace(J);
    fx.ir[A_R_BODY_GUARD].o = (IROp1)wrong_body_guards[i];
    expect_numacc_reject(J);
  }
  make_numacc_trace(J);
  fx.ir[A_R_PRE_GUARD].op1 = A_R_X_PRE;
  fx.ir[A_R_PRE_GUARD].op2 = A_R_LIMIT;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.ir[A_R_BODY_GUARD].op1 = A_R_LIMIT;
  fx.ir[A_R_BODY_GUARD].op2 = A_R_X_BODY;
  expect_numacc_reject(J);

#define REJECT_NUMACC_ADJACENT(op, type, left, right) \
  do { \
    make_numacc_trace(J); \
    setir(A_R_X_PRE, (op), (type), (left), (right)); \
    expect_numacc_reject(J); \
  } while (0)
  REJECT_NUMACC_ADJACENT(IR_CONV, IRT_NUM|IRT_ISPHI,
	A_R_X, IRCONV_NUM_INT);
  if (profile->recurrence_op == IR_ADD)
    REJECT_NUMACC_ADJACENT(IR_SUB, IRT_NUM|IRT_ISPHI,
	  A_R_X, A_R_STEP);
  else
    REJECT_NUMACC_ADJACENT(IR_ADD, IRT_NUM|IRT_ISPHI,
	  A_R_STEP, A_R_X);
  make_numacc_trace(J);
  fx.ir[A_R_X_BODY].o =
    (IROp1)(profile->recurrence_op == IR_ADD ? IR_SUB : IR_ADD);
  expect_numacc_reject(J);
  REJECT_NUMACC_ADJACENT(IR_MUL, IRT_NUM|IRT_ISPHI,
	A_R_X, A_R_STEP);
  REJECT_NUMACC_ADJACENT(IR_DIV, IRT_NUM|IRT_ISPHI,
	A_R_X, A_R_STEP);
#undef REJECT_NUMACC_ADJACENT
  make_numacc_trace(J);
  setir(A_R_X_PRE, IR_CALLN, IRT_NUM, A_R_X, IRCALL_lj_vm_modi);
  expect_numacc_reject(J);
  make_numacc_trace(J);
  setir(A_R_X_PRE, IR_TNEW, IRT_TAB, 0, 0);
  expect_numacc_reject(J);

  /* Every snapshot header, restored entry and footer is exact. */
  for (i = 0; i < 5; i++) {
    make_numacc_trace(J);
    fx.snap[i].ref++;
    expect_numacc_reject(J);
    make_numacc_trace(J);
    fx.snap[i].mapofs++;
    expect_numacc_reject(J);
    make_numacc_trace(J);
    fx.snap[i].nent++;
    expect_numacc_reject(J);
    make_numacc_trace(J);
    fx.snap[i].nslots++;
    expect_numacc_reject(J);
    make_numacc_trace(J);
    fx.snap[i].topslot = 4;
    expect_numacc_reject(J);
    make_numacc_trace(J);
    set_snapshot_payload((SnapNo)i,
	proto_bc(numacc_fixture_pt)+wrong_pcpos[i], 0);
    expect_numacc_reject(J);
    make_numacc_trace(J);
    set_snapshot_payload((SnapNo)i,
	proto_bc(numacc_fixture_pt)+wrong_pcpos[i], 1);
    expect_numacc_reject(J);
  }
  for (i = 0; i < sizeof(entryofs)/sizeof(entryofs[0]); i++) {
    make_numacc_trace(J);
    fx.snapmap[entryofs[i]] ^= 1u;
    expect_numacc_reject(J);
  }
  make_numacc_trace(J);
  fx.snapmap[9] = SNAP(2, SNAP_NORESTORE, A_R_X_PRE);
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.T.nsnap = 4;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.T.nsnap = 6;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.T.nsnapmap = 14;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.T.nsnapmap = 16;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  J->baseslot = 2 + LJ_FR2;
  expect_numacc_reject(J);

  /* Mutate opcode, A, D/C and B bits at every one of the 13 bytecodes. */
  for (i = 0; i < 13; i++) {
    const BCIns *pc = proto_bc(numacc_fixture_pt)+i;
    BCIns saved = loadbc(pc);
    static const uint32_t masks[4] = {
      UINT32_C(1), UINT32_C(1) << 8, UINT32_C(1) << 16,
      UINT32_C(1) << 24
    };
    for (bitno = 0; bitno < 4; bitno++) {
      make_numacc_trace(J);
      bc_publish((const uint32_t *)pc, saved ^ masks[bitno]);
      expect_numacc_reject(J);
      bc_publish((const uint32_t *)pc, saved);
    }
  }
  {
    const BCIns *comparepc = proto_bc(numacc_fixture_pt)+3;
    BCIns saved = loadbc(comparepc);
    make_numacc_trace(J);
    bc_publish((const uint32_t *)comparepc,
	BCINS_AD(bc_op(saved), profile->comparison_d,
	  profile->comparison_a));
    expect_numacc_reject(J);
    bc_publish((const uint32_t *)comparepc, saved);
  }
  {
    const BCIns *arithmeticpc = proto_bc(numacc_fixture_pt)+8;
    BCIns saved = loadbc(arithmeticpc);
    BCOp adjacent = profile->recurrence_op == IR_ADD ? BC_SUBVV : BC_ADDVV;
    make_numacc_trace(J);
    bc_publish((const uint32_t *)arithmeticpc,
	BCINS_ABC(adjacent, 3, 3, 4));
    expect_numacc_reject(J);
    bc_publish((const uint32_t *)arithmeticpc, saved);
  }

  /* All prototype identity fields are part of the certificate. */
  make_numacc_trace(J);
  numacc_fixture_pt->framesize = 4;
  expect_numacc_reject(J);
  numacc_fixture_pt->framesize = 5;
  make_numacc_trace(J);
  numacc_fixture_pt->framesize = 6;
  expect_numacc_reject(J);
  numacc_fixture_pt->framesize = 5;
  make_numacc_trace(J);
  numacc_fixture_pt->sizebc = 12;
  expect_numacc_reject(J);
  numacc_fixture_pt->sizebc = 13;
  make_numacc_trace(J);
  numacc_fixture_pt->sizebc = 14;
  expect_numacc_reject(J);
  numacc_fixture_pt->sizebc = 13;
  make_numacc_trace(J);
  numacc_fixture_pt->numparams = 2;
  expect_numacc_reject(J);
  numacc_fixture_pt->numparams = 3;
  make_numacc_trace(J);
  numacc_fixture_pt->numparams = 4;
  expect_numacc_reject(J);
  numacc_fixture_pt->numparams = 3;
  make_numacc_trace(J);
  numacc_fixture_pt->sizeuv = 1;
  expect_numacc_reject(J);
  numacc_fixture_pt->sizeuv = 0;
  make_numacc_trace(J);
  numacc_fixture_pt->sizekn = 1;
  expect_numacc_reject(J);
  numacc_fixture_pt->sizekn = 0;
  make_numacc_trace(J);
  numacc_fixture_pt->sizekgc = 1;
  expect_numacc_reject(J);
  numacc_fixture_pt->sizekgc = 0;
  make_numacc_trace(J);
  numacc_fixture_pt->flags2 = 0;
  expect_numacc_reject(J);
  numacc_fixture_pt->flags2 = PROTO2_CELLOPS;
  make_numacc_trace(J);
  numacc_fixture_pt->flags2 = PROTO2_CELLOPS|PROTO2_CELLUV;
  expect_numacc_reject(J);
  numacc_fixture_pt->flags2 = PROTO2_CELLOPS;

  make_numacc_trace(J);
  setmref(fx.T.startpc, proto_bc(numacc_fixture_pt)+4);
  J->startpc = proto_bc(numacc_fixture_pt)+4;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  setmref(fx.T.startpc, proto_bc(numacc_fixture_pt)+6);
  J->startpc = proto_bc(numacc_fixture_pt)+6;
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.T.startins ^= (BCIns)(UINT32_C(1) << 8);
  expect_numacc_reject(J);
  make_numacc_trace(J);
  fx.T.startins = loadbc(fixture_forl_pc);
  setmref(fx.T.startpc, fixture_forl_pc);
  J->startpc = fixture_forl_pc;
  expect_numacc_reject(J);
}

static void test_numacc_shape_cross_product(jit_State *J)
{
  static const unsigned profiles[3] = {
    NUMACC_FIXTURE_ADD_LT, NUMACC_FIXTURE_ADD_LE,
    NUMACC_FIXTURE_SUB_GT
  };
  static const IROp arithmetic_ops[2] = { IR_ADD, IR_SUB };
  static const IROp preops[3] = { IR_GT, IR_GE, IR_LT };
  static const IROp bodyops[3] = { IR_LT, IR_LE, IR_GT };
  MSize p, prearith, bodyarith, pre, body;
  MSize combinations = 0, semantic_admissions = 0, postra_admissions = 0;

  /* Exercise the complete 3x2x2x3x3 source-profile, pre-arithmetic,
  ** body-arithmetic, pre-guard and body-guard product. Exactly ADD_LT,
  ** ADD_LE and SUB_GT are coherent at both semantic and post-RA gates. */
  for (p = 0; p < 3; p++) {
    const IROp expected_arithmetic = p == 2 ? IR_SUB : IR_ADD;
    select_numacc_fixture(profiles[p]);
    assert(numacc_fixture_full_shape() == profiles[p]);
    for (prearith = 0; prearith < 2; prearith++) {
      for (bodyarith = 0; bodyarith < 2; bodyarith++) {
	for (pre = 0; pre < 3; pre++) {
	  for (body = 0; body < 3; body++) {
	    IROp pre_arithmetic = arithmetic_ops[prearith];
	    IROp body_arithmetic = arithmetic_ops[bodyarith];
	    IRRef pre_left = pre_arithmetic == IR_ADD ? A_R_STEP : A_R_X;
	    IRRef pre_right = pre_arithmetic == IR_ADD ? A_R_X : A_R_STEP;
	    int admitted = pre_arithmetic == expected_arithmetic &&
	      body_arithmetic == expected_arithmetic && pre == p && body == p;
	    LJArm64PostRAView view;

	    make_numacc_trace(J);
	    fx.ir[A_R_X_PRE].o = (IROp1)pre_arithmetic;
	    fx.ir[A_R_X_PRE].op1 = (IRRef1)pre_left;
	    fx.ir[A_R_X_PRE].op2 = (IRRef1)pre_right;
	    fx.ir[A_R_X_BODY].o = (IROp1)body_arithmetic;
	    fx.ir[A_R_PRE_GUARD].o = (IROp1)preops[pre];
	    fx.ir[A_R_BODY_GUARD].o = (IROp1)bodyops[body];
	    expect_numacc_semantic_result(J, admitted);
	    semantic_admissions += (MSize)admitted;

	    view = make_numacc_postra_view(J);
	    fx.ir[A_R_X_PRE].o = (IROp1)pre_arithmetic;
	    fx.ir[A_R_X_PRE].op1 = (IRRef1)pre_left;
	    fx.ir[A_R_X_PRE].op2 = (IRRef1)pre_right;
	    fx.ir[A_R_X_BODY].o = (IROp1)body_arithmetic;
	    fx.ir[A_R_PRE_GUARD].o = (IROp1)preops[pre];
	    fx.ir[A_R_BODY_GUARD].o = (IROp1)bodyops[body];
	    expect_numacc_postra_result(&view, admitted);
	    postra_admissions += (MSize)admitted;
	    combinations++;
	  }
	}
      }
    }
  }
  assert(combinations == 3u*2u*2u*3u*3u);
  assert(combinations == 108);
  assert(semantic_admissions == 3 && postra_admissions == 3);

  /* Semantic admission and post-RA independently re-read the exact compare
  ** operand direction and recurrence opcode from the live prototype. */
  for (p = 0; p < 3; p++) {
    const NumaccFixtureProfile *profile;
    const BCIns *comparepc, *arithmeticpc;
    BCIns saved_compare, saved_arithmetic;
    BCOp adjacent;
    LJArm64PostRAView view;

    select_numacc_fixture(profiles[p]);
    profile = numacc_active_profile();
    comparepc = proto_bc(numacc_fixture_pt)+3;
    arithmeticpc = proto_bc(numacc_fixture_pt)+8;
    saved_compare = loadbc(comparepc);
    saved_arithmetic = loadbc(arithmeticpc);
    adjacent = profile->recurrence_op == IR_ADD ? BC_SUBVV : BC_ADDVV;

    make_numacc_trace(J);
    bc_publish((const uint32_t *)comparepc,
	BCINS_AD(bc_op(saved_compare), profile->comparison_d,
	  profile->comparison_a));
    expect_numacc_reject(J);
    bc_publish((const uint32_t *)comparepc, saved_compare);

    view = make_numacc_postra_view(J);
    bc_publish((const uint32_t *)comparepc,
	BCINS_AD(bc_op(saved_compare), profile->comparison_d,
	  profile->comparison_a));
    expect_numacc_postra_result(&view, 0);
    bc_publish((const uint32_t *)comparepc, saved_compare);

    make_numacc_trace(J);
    bc_publish((const uint32_t *)arithmeticpc,
	BCINS_ABC(adjacent, 3, 3, 4));
    expect_numacc_reject(J);
    bc_publish((const uint32_t *)arithmeticpc, saved_arithmetic);

    view = make_numacc_postra_view(J);
    bc_publish((const uint32_t *)arithmeticpc,
	BCINS_ABC(adjacent, 3, 3, 4));
    expect_numacc_postra_result(&view, 0);
    bc_publish((const uint32_t *)arithmeticpc, saved_arithmetic);
  }

  /* The coherent neighbouring descending-inclusive profile remains closed:
  ** ISGT A4,D3, SUB, LE(limit,xpre), GE(xbody,limit). */
  select_numacc_fixture(NUMACC_FIXTURE_SUB_GT);
  {
    const BCIns *comparepc = proto_bc(numacc_fixture_pt)+3;
    BCIns saved = loadbc(comparepc);
    LJArm64PostRAView view;
    make_numacc_trace(J);
    fx.ir[A_R_PRE_GUARD].o = IR_LE;
    fx.ir[A_R_BODY_GUARD].o = IR_GE;
    bc_publish((const uint32_t *)comparepc, BCINS_AD(BC_ISGT, 4, 3));
    expect_numacc_reject(J);
    bc_publish((const uint32_t *)comparepc, saved);

    view = make_numacc_postra_view(J);
    fx.ir[A_R_PRE_GUARD].o = IR_LE;
    fx.ir[A_R_BODY_GUARD].o = IR_GE;
    bc_publish((const uint32_t *)comparepc, BCINS_AD(BC_ISGT, 4, 3));
    expect_numacc_postra_result(&view, 0);
    bc_publish((const uint32_t *)comparepc, saved);
  }
  select_numacc_fixture(NUMACC_FIXTURE_ADD_LT);
}

static void test_positive_and_negative(lua_State *L)
{
  jit_State *J = L2J(L);
  LJArm64IRReject reject;
  static const IROp signed_guards[] = {
    IR_LT, IR_GE, IR_LE, IR_GT, IR_EQ, IR_NE
  };
  static const IROp arithmetic_ops[] = {
    IR_ADDOV, IR_SUBOV, IR_MULOV
  };
  static const IROp unsigned_guards[] = {
    IR_ULT, IR_UGE, IR_ULE, IR_UGT
  };
  MSize guardno, leftno, rightno;

  make_trace(J);
  assert(fixture_snapshot_pc != fixture_loop_pc);
  if (!lj_asm_arm64_ir_admit(J, &fx.T, &reject))
    fprintf(stderr, "positive admission failed: reason=%d ref=%u op=%u detail=%u\n",
	(int)reject.reason, (unsigned)reject.ref, (unsigned)reject.op,
	(unsigned)reject.detail);
  assert(reject.reason == LJ_ARM64_IR_REJECT_NONE);
  assert(reject.detail == LJ_ARM64_IR_CALL_NONE);

  for (guardno = 0;
	 guardno < sizeof(signed_guards)/sizeof(signed_guards[0]);
	 guardno++) {
    make_trace(J);
    fx.ir[R_PRECOND].o = (IROp1)signed_guards[guardno];
    fx.ir[R_LOOPCOND].o = (IROp1)signed_guards[
	(sizeof(signed_guards)/sizeof(signed_guards[0])-1u)-guardno];
    assert(lj_asm_arm64_ir_admit(J, &fx.T, &reject));
    assert(reject.reason == LJ_ARM64_IR_REJECT_NONE);
  }

  for (leftno = 0;
	 leftno < sizeof(arithmetic_ops)/sizeof(arithmetic_ops[0]);
	 leftno++) {
    for (rightno = 0;
	   rightno < sizeof(arithmetic_ops)/sizeof(arithmetic_ops[0]);
	   rightno++) {
      make_trace(J);
      fx.ir[R_SUM1].o = fx.ir[R_BODY1].o = (IROp1)arithmetic_ops[leftno];
      fx.ir[R_SUM2].o = fx.ir[R_BODY2].o = (IROp1)arithmetic_ops[rightno];
      assert(lj_asm_arm64_ir_admit(J, &fx.T, &reject));
      assert(reject.reason == LJ_ARM64_IR_REJECT_NONE);
    }
  }

  for (guardno = 0;
	 guardno < sizeof(unsigned_guards)/sizeof(unsigned_guards[0]);
	 guardno++) {
    make_trace(J);
    fx.ir[R_PRECOND].o = (IROp1)unsigned_guards[guardno];
    expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, unsigned_guards[guardno]);
  }

  make_trace(J);
  setir(R_SUM1, IR_AREF, IRT_PGC, R_A, R_B);
  expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, IR_AREF);

  make_trace(J);
  setir(R_SUM1, IR_UREFO, IRT_PGC, R_A, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, IR_UREFO);

  make_trace(J);
  setir(R_SUM1, IR_TNEW, IRT_TAB, 0, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, IR_TNEW);

  make_trace(J);
  setir(R_SUM1, IR_SNEW, IRT_STR, R_A, R_B);
  expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, IR_SNEW);

  make_trace(J);
  setir(R_SUM1, IR_BUFHDR, IRT_PGC, R_A, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, IR_BUFHDR);

  make_trace(J);
  setir(R_SUM1, IR_XSAVE, IRT_NIL, 0, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, IR_XSAVE);

  make_trace(J);
  setir(R_SUM1, IR_XBAR, IRT_NIL, 0, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, IR_XBAR);

  make_trace(J);
  setir(R_SUM1, IR_OBAR, IRT_NIL, R_A, R_B);
  expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, IR_OBAR);

  make_trace(J);
  setir(R_SUM1, IR_TBAR, IRT_NIL, R_A, R_B);
  expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, IR_TBAR);

  make_trace(J);
  setir(R_SUM1, IR_MOD, IRT_INT, R_A, K_STEP);
  expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, IR_MOD);

  make_trace(J);
  setir(R_SUM1, IR_CALLN, IRT_INT, R_A, IRCALL_lj_vm_modi);
  reject = expect_reject(J, LJ_ARM64_IR_REJECT_CALL, IR_CALLN);
  assert(reject.detail == IRCALL_lj_vm_modi);

  make_trace(J);
  setir(R_SUM1, IR_CALLXS, IRT_NIL, R_A, R_B);
  reject = expect_reject(J, LJ_ARM64_IR_REJECT_CALL, IR_CALLXS);
  assert(reject.detail == LJ_ARM64_IR_CALL_NONE);

#define REJECT_REMOVED(ref, op, type, op1, op2) \
  do { \
    make_trace(J); \
    setir((ref), (op), (type), (op1), (op2)); \
    expect_reject(J, LJ_ARM64_IR_REJECT_OPCODE, (op)); \
  } while (0)
  REJECT_REMOVED(R_SUM1, IR_NOP, IRT_NIL, 0, 0);
  REJECT_REMOVED(R_SUM1, IR_CONV, IRT_INT, R_A, IRCONV_INT_NUM);
  REJECT_REMOVED(R_SUM1, IR_MUL, IRT_INT, R_A, R_B);
  REJECT_REMOVED(R_SUM1, IR_DIV, IRT_NUM, R_A, R_B);
  REJECT_REMOVED(R_SUM1, IR_USE, IRT_INT, R_A, 0);
#undef REJECT_REMOVED

  /* SUB has a dedicated case only for the exact NUM SUB_GT profile. */
  make_trace(J);
  setir(R_SUM1, IR_SUB, IRT_INT, R_A, R_B);
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SUB);

  /* IR_ADD belongs only to the exact FORL induction grammar. A LOOP root
  ** reaches its dedicated case, but may not use the producer. */
  make_trace(J);
  setir(R_SUM1, IR_ADD, IRT_INT, R_A, R_B);
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_ADD);

  make_trace(J);
  setir(K_STEP, IR_KGC, IRT_TAB, 0, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_CONSTANT, IR_KGC);

  make_trace(J);
  setir(REF_TRUE, IR_KINT, IRT_INT, 1, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_CONSTANT, IR_KPRI);

  make_trace(J);
  setir(REF_FALSE, IR_KNUM, IRT_NUM, 0, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_CONSTANT, IR_KPRI);

  make_trace(J);
  fx.ir[REF_TRUE].t.irt = IRT_STR;
  expect_reject(J, LJ_ARM64_IR_REJECT_CONSTANT, IR_KPRI);

  make_trace(J);
  setir(K_STOP, IR_KNUM, IRT_NUM, 0, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_CONSTANT, IR_KNUM);

  /* An otherwise integer-only root cannot smuggle in the exact admitted
  ** half-KNUM profile as an unused constant family. Keep its operands
  ** dynamic so this reaches the scalar/profile discriminator itself. */
  make_trace(J);
  fx.T.nk = H_K_HALF;
  setir(H_K_HALF, IR_KNUM, IRT_NUM, 0, 0);
  fx.ir[H_K_HALF_PAYLOAD].tv.u64 = UINT64_C(0x3fe0000000000000);
  fx.ir[R_PRECOND].op2 = R_A;
  fx.ir[R_BODY2].op2 = R_C;
  fx.ir[R_LOOPCOND].op2 = R_A;
  expect_reject(J, LJ_ARM64_IR_REJECT_CONSTANT, IR_KNUM);

  make_trace(J);
  fx.ir[R_A].t.irt = IRT_TAB|IRT_GUARD;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SLOAD);

  make_trace(J);
  fx.ir[R_A].op2 |= IRSLOAD_KEYINDEX;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SLOAD);

  make_trace(J);
  fx.ir[R_A].op2 = IRSLOAD_READONLY|IRSLOAD_TYPECHECK;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SLOAD);

  make_trace(J);
  fx.ir[R_A].t.irt &= (uint8_t)~IRT_GUARD;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SLOAD);

  make_trace(J);
  fx.ir[R_A].t.irt = IRT_NUM|IRT_GUARD;
  /* NUM SLOAD itself is now part of the mixed-loop grammar; this otherwise
  ** integer fixture still fails when SUBOV consumes it as an integer. */
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SUBOV);

  make_trace(J);
  fx.ir[R_A].op1 = 1;
  expect_reject(J, LJ_ARM64_IR_REJECT_OPERAND, IR_SLOAD);

  make_trace(J);
  fx.ir[R_A].op1 = fx.snap[0].nslots;
  expect_reject(J, LJ_ARM64_IR_REJECT_OPERAND, IR_SLOAD);

  make_trace(J);
  fx.ir[R_A].op1 = fixture_pt->framesize + 1 + LJ_FR2;
  {
    SnapNo n;
    for (n = 0; n < fx.T.nsnap; n++)
      fx.snap[n].nslots = (uint8_t)(fx.ir[R_A].op1 + 1u);
  }
  expect_reject(J, LJ_ARM64_IR_REJECT_OPERAND, IR_SLOAD);

  make_trace(J);
  fx.ir[R_SUM1].t.irt &= (uint8_t)~IRT_GUARD;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SUBOV);

  make_trace(J);
  setir(R_SUM1, IR_ADDOV, IRT_INT|IRT_ISPHI, R_A, R_B);
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_ADDOV);

  make_trace(J);
  fx.ir[R_SUM1].t.irt = IRT_NUM|IRT_GUARD|IRT_ISPHI;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SUBOV);

  make_trace(J);
  fx.ir[R_SUM1].op1 = R_BODY1;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SUBOV);

  make_trace(J);
  fx.ir[R_BODY1].op1 = R_PRECOND;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_SUBOV);

  make_trace(J);
  fx.ir[R_SUM2].t.irt &= (uint8_t)~IRT_GUARD;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_MULOV);

  make_trace(J);
  fx.ir[R_SUM2].op2 = R_LOOPCOND;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_MULOV);

  make_trace(J);
  fx.ir[R_PRECOND].t.irt = IRT_NUM|IRT_GUARD;
  expect_reject(J, LJ_ARM64_IR_REJECT_TYPE, IR_GE);

  make_trace(J);
  fx.T.sinktags = 1;
  expect_reject(J, LJ_ARM64_IR_REJECT_SINK, IR_TNEW);

  make_trace(J);
  fx.T.root = 1;
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);

  make_trace(J);
  fx.T.linktype = LJ_TRLINK_STITCH;
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_LOOP);

  make_trace(J);
  fx.T.startins = BCINS_AD(BC_FUNCF, 0, 0);
  expect_reject(J, LJ_ARM64_IR_REJECT_TRACE, IR_XPOLL);

  make_trace(J);
  fx.ir[R_XPOLL].op1 = 0;
  expect_reject(J, LJ_ARM64_IR_REJECT_XPOLL, IR_XPOLL);

  make_trace(J);
  J->loopref = R_LOOP-1;
  expect_reject(J, LJ_ARM64_IR_REJECT_XPOLL, IR_XPOLL);

  make_trace(J);
  fx.snap[2].ref = R_XPOLL;
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.snapmap[6] = SNAP(2, SNAP_KEYINDEX, R_SUM1);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.snapmap[6] = SNAP(2, SNAP_NORESTORE, R_SUM1);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_SLOAD);

  make_trace(J);
  fx.snapmap[6] |= 0x00200000u;
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.snapmap[6] = SNAP(2, 0, REF_BASE);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.snapmap[6] = SNAP(2, 0, R_BODY1);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.snapmap[6] = SNAP(2, 0, R_PRECOND);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.snapmap[7] = SNAP(2, 0, R_SUM2);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.snapmap[7] = SNAP(5, 0, R_SUM2);
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.snap[0].topslot = 1;
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.snap[2].mapofs = 5;
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.snap[0].mapofs = fx.T.nsnapmap+1u;
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.snap[1].mapofs = fx.T.nsnapmap+1u;
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.T.nsnapmap = 13;
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_XPOLL);

  make_trace(J);
  fx.snap[0].ref = R_PRECOND;
  expect_reject(J, LJ_ARM64_IR_REJECT_SNAPSHOT, IR_SLOAD);

  test_snapshot_payload_rejections(J);
  test_start_metadata_rejections(J);
  test_phi_and_xpoll_rejections(J);

  test_deterministic_trace_error(L, J);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  jit_State *J;
  lua_State *savedL;
  TraceNo savedparent;
  ExitNo savedexit;
  IRRef savedloop;
  GCproto *savedpt;
  BCReg savedbaseslot;
  int32_t savedframedepth;
  int32_t savedretdepth;
  const BCIns *savedstartpc;
  MSize i;
  assert(L != NULL);
  assert(luaL_loadstring(L,
	"local acc = 0; "
	"for i = 1, 20 do acc = acc + i end; "
	"local j = 0; "
	"while j < 20 do j = j + 1; acc = acc + j end; "
	"return acc") == 0);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  fixture_pt = funcproto(funcV(L->top-1));
  assert(fixture_pt->framesize >= 5);
  for (i = 0; i < fixture_pt->sizebc; i++) {
    const BCIns *pc = &proto_bc(fixture_pt)[i];
    BCOp op = bc_op(loadbc(pc));
    if (op == BC_FORL && fixture_forl_pc == NULL)
      fixture_forl_pc = pc;
    if (op == BC_LOOP && fixture_loop_pc == NULL)
      fixture_loop_pc = pc;
  }
  assert(fixture_forl_pc != NULL && fixture_loop_pc != NULL);
  assert(bc_j(loadbc(fixture_forl_pc)) < 0);
  fixture_snapshot_pc = fixture_forl_pc + 1 + bc_j(loadbc(fixture_forl_pc));
  assert(fixture_snapshot_pc < proto_bc(fixture_pt)+fixture_pt->sizebc);
  assert(bc_op(loadbc(fixture_loop_pc+bc_j(loadbc(fixture_loop_pc)))) == BC_JMP);

  assert(luaL_loadstring(L,
	"return function(n,x,step) local i=0 "
	"while i<n do i=i+1; x=x+step end return x end") == 0);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  numeric_fixture_pt = funcproto(funcV(L->top-1));
  assert(numeric_fixture_pt->framesize == 6);
  for (i = 0; i < numeric_fixture_pt->sizebc; i++) {
    const BCIns *pc = &proto_bc(numeric_fixture_pt)[i];
    if (bc_op(loadbc(pc)) == BC_LOOP && numeric_fixture_loop_pc == NULL)
      numeric_fixture_loop_pc = pc;
  }
  assert(numeric_fixture_loop_pc != NULL);
  numeric_fixture_snapshot_pc = numeric_fixture_loop_pc +
	bc_j(loadbc(numeric_fixture_loop_pc));
  assert(numeric_fixture_snapshot_pc >= proto_bc(numeric_fixture_pt));
  assert(numeric_fixture_snapshot_pc <
	 proto_bc(numeric_fixture_pt)+numeric_fixture_pt->sizebc);
  assert(bc_op(loadbc(numeric_fixture_snapshot_pc)) == BC_JMP);

  assert(luaL_loadstring(L,
	"return function(limit) local x=0.5 "
	"while x<limit do x=x+0.5 end return x end") == 0);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  numhalf_fixture_pt = funcproto(funcV(L->top-1));
  assert(numhalf_fixture_pt->framesize == 4);
  assert(numhalf_fixture_pt->sizebc == 13);
  assert(numhalf_fixture_pt->numparams == 1);
  for (i = 0; i < numhalf_fixture_pt->sizebc; i++) {
    const BCIns *pc = &proto_bc(numhalf_fixture_pt)[i];
    if (bc_op(loadbc(pc)) == BC_LOOP && numhalf_fixture_loop_pc == NULL)
      numhalf_fixture_loop_pc = pc;
  }
  assert(numhalf_fixture_loop_pc == proto_bc(numhalf_fixture_pt)+6);
  assert(bc_j(loadbc(numhalf_fixture_loop_pc)) > 0);
  assert(bc_op(loadbc(numhalf_fixture_loop_pc+
	bc_j(loadbc(numhalf_fixture_loop_pc)))) == BC_JMP);

  assert(luaL_loadstring(L,
	"return function(limit,step) local x=0.5 "
	"while x<limit do x=x+step end return x end") == 0);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  numstep_fixture_pt = funcproto(funcV(L->top-1));
  assert(numstep_fixture_pt->framesize == 5);
  assert(numstep_fixture_pt->sizebc == 14);
  assert(numstep_fixture_pt->numparams == 2);
  assert(numstep_fixture_pt->sizeuv == 0);
  assert(numstep_fixture_pt->sizekn == 1);
  assert(numstep_fixture_pt->sizekgc == 0);
  assert(proto_knumtv(numstep_fixture_pt, 0)->u64 ==
	 UINT64_C(0x3fe0000000000000));
  for (i = 0; i < numstep_fixture_pt->sizebc; i++) {
    const BCIns *pc = &proto_bc(numstep_fixture_pt)[i];
    if (bc_op(loadbc(pc)) == BC_LOOP && numstep_fixture_loop_pc == NULL)
      numstep_fixture_loop_pc = pc;
  }
  assert(numstep_fixture_loop_pc == proto_bc(numstep_fixture_pt)+6);
  assert(bc_j(loadbc(numstep_fixture_loop_pc)) > 0);
  assert(bc_op(loadbc(numstep_fixture_loop_pc+
	bc_j(loadbc(numstep_fixture_loop_pc)))) == BC_JMP);

  assert(luaL_loadstring(L,
	"return function(x,limit,step) "
	"while x<limit do x=x+step end return x end") == 0);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  numacc_strict_fixture_pt = funcproto(funcV(L->top-1));
  numacc_fixture_pt = numacc_strict_fixture_pt;
  assert(numacc_fixture_pt->framesize == 5);
  assert(numacc_fixture_pt->sizebc == 13);
  assert(numacc_fixture_pt->numparams == 3);
  assert(numacc_fixture_pt->sizeuv == 0);
  assert(numacc_fixture_pt->sizekn == 0);
  assert(numacc_fixture_pt->sizekgc == 0);
  assert(numacc_fixture_pt->flags2 == PROTO2_CELLOPS);
  for (i = 0; i < numacc_fixture_pt->sizebc; i++) {
    const BCIns *pc = &proto_bc(numacc_fixture_pt)[i];
    if (bc_op(loadbc(pc)) == BC_LOOP && numacc_fixture_loop_pc == NULL)
      numacc_fixture_loop_pc = pc;
  }
  assert(numacc_fixture_loop_pc == proto_bc(numacc_fixture_pt)+5);
  assert(bc_j(loadbc(numacc_fixture_loop_pc)) > 0);
  assert(bc_op(loadbc(numacc_fixture_loop_pc+
	bc_j(loadbc(numacc_fixture_loop_pc)))) == BC_JMP);
  assert(bc_op(loadbc(proto_bc(numacc_fixture_pt)+3)) == BC_ISGE);
  numacc_strict_fixture_loop_pc = numacc_fixture_loop_pc;

  assert(luaL_loadstring(L,
	"return function(x,limit,step) "
	"while x<=limit do x=x+step end return x end") == 0);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  numacc_inclusive_fixture_pt = funcproto(funcV(L->top-1));
  assert(numacc_inclusive_fixture_pt->framesize == 5);
  assert(numacc_inclusive_fixture_pt->sizebc == 13);
  assert(numacc_inclusive_fixture_pt->numparams == 3);
  assert(numacc_inclusive_fixture_pt->sizeuv == 0);
  assert(numacc_inclusive_fixture_pt->sizekn == 0);
  assert(numacc_inclusive_fixture_pt->sizekgc == 0);
  assert(numacc_inclusive_fixture_pt->flags2 == PROTO2_CELLOPS);
  for (i = 0; i < numacc_inclusive_fixture_pt->sizebc; i++) {
    const BCIns *pc = &proto_bc(numacc_inclusive_fixture_pt)[i];
    if (bc_op(loadbc(pc)) == BC_LOOP &&
	numacc_inclusive_fixture_loop_pc == NULL)
      numacc_inclusive_fixture_loop_pc = pc;
  }
  assert(numacc_inclusive_fixture_loop_pc ==
	 proto_bc(numacc_inclusive_fixture_pt)+5);
  assert(bc_j(loadbc(numacc_inclusive_fixture_loop_pc)) > 0);
  assert(bc_op(loadbc(numacc_inclusive_fixture_loop_pc+
	bc_j(loadbc(numacc_inclusive_fixture_loop_pc)))) == BC_JMP);
  assert(bc_op(loadbc(proto_bc(numacc_inclusive_fixture_pt)+3)) == BC_ISGT);

  assert(luaL_loadstring(L,
	"return function(x,limit,step) "
	"while x>limit do x=x-step end return x end") == 0);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  assert(tvisfunc(L->top-1) && isluafunc(funcV(L->top-1)));
  numacc_sub_gt_fixture_pt = funcproto(funcV(L->top-1));
  assert(numacc_sub_gt_fixture_pt->framesize == 5);
  assert(numacc_sub_gt_fixture_pt->sizebc == 13);
  assert(numacc_sub_gt_fixture_pt->numparams == 3);
  assert(numacc_sub_gt_fixture_pt->sizeuv == 0);
  assert(numacc_sub_gt_fixture_pt->sizekn == 0);
  assert(numacc_sub_gt_fixture_pt->sizekgc == 0);
  assert(numacc_sub_gt_fixture_pt->flags2 == PROTO2_CELLOPS);
  for (i = 0; i < numacc_sub_gt_fixture_pt->sizebc; i++) {
    const BCIns *pc = &proto_bc(numacc_sub_gt_fixture_pt)[i];
    if (bc_op(loadbc(pc)) == BC_LOOP &&
	numacc_sub_gt_fixture_loop_pc == NULL)
      numacc_sub_gt_fixture_loop_pc = pc;
  }
  assert(numacc_sub_gt_fixture_loop_pc ==
	 proto_bc(numacc_sub_gt_fixture_pt)+5);
  assert(bc_j(loadbc(numacc_sub_gt_fixture_loop_pc)) > 0);
  assert(bc_op(loadbc(numacc_sub_gt_fixture_loop_pc+
	bc_j(loadbc(numacc_sub_gt_fixture_loop_pc)))) == BC_JMP);
  {
    BCIns comparison = loadbc(proto_bc(numacc_sub_gt_fixture_pt)+3);
    BCIns arithmetic = loadbc(proto_bc(numacc_sub_gt_fixture_pt)+8);
    assert(bc_op(comparison) == BC_ISGE);
    assert(bc_a(comparison) == 4 && bc_d(comparison) == 3);
    assert(bc_op(arithmetic) == BC_SUBVV && bc_a(arithmetic) == 3);
    assert(bc_b(arithmetic) == 3 && bc_c(arithmetic) == 4);
  }
  J = L2J(L);
  savedL = J->L;
  savedparent = J->parent;
  savedexit = J->exitno;
  savedloop = J->loopref;
  savedpt = J->pt;
  savedbaseslot = J->baseslot;
  savedframedepth = J->framedepth;
  savedretdepth = J->retdepth;
  savedstartpc = J->startpc;
  J->L = L;
  test_positive_and_negative(L);
  test_postra_spill_layout(J);
  test_numeric_positive_and_negative(J);
  test_numeric_postra_layout(J);
  test_numhalf_positive_and_negative(J);
  test_numhalf_postra_layout(J);
  test_numstep_positive_and_negative(J);
  test_numstep_postra_layout(J);
  select_numacc_fixture(NUMACC_FIXTURE_ADD_LT);
  test_numacc_positive_and_negative(J);
  test_numacc_postra_layout(J);
  select_numacc_fixture(NUMACC_FIXTURE_ADD_LE);
  test_numacc_positive_and_negative(J);
  test_numacc_postra_layout(J);
  select_numacc_fixture(NUMACC_FIXTURE_SUB_GT);
  test_numacc_positive_and_negative(J);
  test_numacc_postra_layout(J);
  test_numacc_shape_cross_product(J);
  J->L = savedL;
  J->parent = savedparent;
  J->exitno = savedexit;
  J->loopref = savedloop;
  J->pt = savedpt;
  J->baseslot = savedbaseslot;
  J->framedepth = savedframedepth;
  J->retdepth = savedretdepth;
  J->startpc = savedstartpc;
  L->top -= 7;
  lua_close(L);
  puts("arm64_jit_ir_admission OK: integer, mixed NUM, fixed-half, dynamic-step and ADD_LT/ADD_LE/SUB_GT dynamic-accumulator pure NUM LOOP/FORL policy verified");
  return 0;
}

#else

int main(void)
{
  puts("arm64_jit_ir_admission SKIP: requires native experimental macOS ARM64");
  return 0;
}

#endif
