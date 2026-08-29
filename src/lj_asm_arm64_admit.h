/*
** Fail-closed ARM64 trace admission and post-allocation certification.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_ASM_ARM64_ADMIT_H
#define _LJ_ASM_ARM64_ADMIT_H

/* -- Address-safe ARM64 B26 encoding ------------------------------------ */

/* Encode the signed, word-scaled immediate of an unconditional branch.
** Compute only ordered unsigned differences: source and target need not
** belong to the same C object, and no signed-address conversion can wrap. */
int lj_asm_arm64_b26_encode(uintptr_t source, uintptr_t target, MCode *insp)
{
  uintptr_t distance;
  uint32_t immediate;
  if (source == 0 || target == 0 || insp == NULL ||
	((source | target) & 3u) != 0)
    return 0;
  if (target >= source) {
    distance = target - source;
    if (distance > UINT32_C(0x07fffffc))
      return 0;
    immediate = (uint32_t)(distance >> 2);
  } else {
    distance = source - target;
    if (distance > UINT32_C(0x08000000))
      return 0;
    immediate = (0u - (uint32_t)(distance >> 2)) & UINT32_C(0x03ffffff);
  }
  *insp = (MCode)(A64I_B | immediate);
  return 1;
}

/* -- Initial ARM64 IR admission ------------------------------------------ */

/*
** The stock ARM64 backend can encode a much larger IR surface than the
** lockless runtime has proved safe. Keep native publication to optimized,
** self-linked scalar BC_LOOP roots, separately certified constant-step and
** exact variable-stop/variable-step integer BC_FORL roots, and the exact
** literal-true fixed-function root.
** Numeric LOOP admission is presently limited to four exact spill-free
** accumulator families: one dynamic mixed INT/NUM root, one pure NUM root with
** a canonical +0.5 constant, one fixed-initializer root with a dynamic NUM
** step, and one all-parameter NUM root with an exact ADD_LT, ADD_LE, ADD_GT,
** ADD_GE, SUB_GT, SUB_GE, MUL_LT, MUL_LE, DIV_LT, DIV_LE, DIV_GT or DIV_GE
** recurrence grammar. The last root admits either all-NUM arguments, one
** invariant INT step widened by one exact INT-to-NUM conversion, or one INT
** accumulator widened once and repaired by one exact checked NUM-to-INT
** conversion. ADD_LT alone also admits one invariant INT limit widened once
** to NUM. Only the exact MUL_LT and MUL_LE profiles admit FMUL, and only the
** exact DIV_LT, DIV_LE, DIV_GT and DIV_GE profiles admit FDIV; this list
** admits no IR CALL helper ID and no heap operation.
*/

static int arm64_ir_reject(LJArm64IRReject *reject,
	LJArm64IRRejectReason reason, IRRef ref, IROp op, uint16_t detail)
{
  if (reject) {
    reject->reason = reason;
    reject->ref = ref;
    reject->op = op;
    reject->detail = detail;
  }
  return 0;
}

static int arm64_ir_type_flags(IRType1 t, IRType type, uint8_t require,
	uint8_t allow)
{
  uint8_t flags = (uint8_t)(t.irt & ~IRT_TYPE);
  return irt_type(t) == type && (flags & require) == require &&
	 (flags & ~allow) == 0;
}

static int arm64_ir_proto_range(const GCproto *pt, uintptr_t *lop,
	uintptr_t *hip)
{
  uintptr_t lo = (uintptr_t)proto_bc(pt);
  uintptr_t nbc = (uintptr_t)pt->sizebc;
  uintptr_t bytes;
  LJ_STATIC_ASSERT((sizeof(BCIns) & (sizeof(BCIns)-1)) == 0);
  if (lo == 0 || (lo & (sizeof(BCIns)-1)) != 0 || nbc == 0 ||
	nbc > (UINTPTR_MAX-lo) / sizeof(BCIns))
    return 0;
  bytes = nbc * sizeof(BCIns);
  *lop = lo;
  *hip = lo + bytes;
  return *hip > lo;
}

static int arm64_ir_pcpos(uintptr_t pc, uintptr_t lo, uintptr_t hi,
	MSize *posp)
{
  uintptr_t delta;
  if (pc < lo || pc >= hi || (pc & (sizeof(BCIns)-1)) != 0)
    return 0;
  delta = pc - lo;
  if ((delta & (sizeof(BCIns)-1)) != 0)
    return 0;
  *posp = (MSize)(delta / sizeof(BCIns));
  return 1;
}

static BCIns arm64_ir_bc_acq(uintptr_t lo, MSize pos)
{
  const uint32_t *pc = (const uint32_t *)(lo + (uintptr_t)pos*sizeof(BCIns));
  return (BCIns)la_load32_acq(pc);
}

/* Exact immutable bytecode grammar for the first fixed-function root. The
** trace can only return literal true from the final frame slot. */
static int arm64_ir_funcf_bytecode(const BCIns *bc, MSize sizebc,
	MSize framesize, BCIns startins, BCIns liveins)
{
  uintptr_t lo = (uintptr_t)bc;
  BCIns kpri, ret;
  BCReg result;
  LJ_STATIC_ASSERT((sizeof(BCIns) & (sizeof(BCIns)-1)) == 0);
  if (bc == NULL || (lo & (sizeof(BCIns)-1)) != 0 || sizebc != 3 ||
	(uintptr_t)sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns) || framesize == 0 ||
	framesize > UINT8_MAX || bc_op(startins) != BC_FUNCF ||
	bc_a(startins) != framesize || bc_d(startins) != 0 ||
	!((liveins == startins) ||
	  (bc_op(liveins) == BC_JFUNCF &&
	   bc_a(liveins) == bc_a(startins) && bc_d(liveins) != 0)) ||
	(BCIns)la_load32_acq((const uint32_t *)&bc[0]) != liveins)
    return 0;
  result = (BCReg)(framesize-1u);
  kpri = (BCIns)la_load32_acq((const uint32_t *)&bc[1]);
  ret = (BCIns)la_load32_acq((const uint32_t *)&bc[2]);
  return bc_op(kpri) == BC_KPRI && bc_a(kpri) == result &&
	 bc_d(kpri) == 2u && bc_op(ret) == BC_RET1 &&
	 bc_a(ret) == result && bc_d(ret) == 2u &&
	 (BCIns)la_load32_acq((const uint32_t *)&bc[0]) == liveins &&
	 (BCIns)la_load32_acq((const uint32_t *)&bc[1]) == kpri &&
	 (BCIns)la_load32_acq((const uint32_t *)&bc[2]) == ret;
}

static int arm64_ir_start(const jit_State *J, const GCtrace *T,
	const GCproto *pt, uintptr_t *lop, uintptr_t *hip,
	LJArm64IRReject *reject)
{
  const BCIns *startpc = trace_startpc_acq(T);
  uintptr_t lo, hi, pc;
  MSize pos;
  BCIns startins = T->startins;
  BCOp op = bc_op(startins);
  BCReg slot = bc_a(startins);
  if (!arm64_ir_proto_range(pt, &lo, &hi) || startpc == NULL ||
	startpc != J->startpc)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			   IR_LOOP, (uint16_t)op);
  pc = (uintptr_t)startpc;
  if (!arm64_ir_pcpos(pc, lo, hi, &pos) ||
	arm64_ir_bc_acq(lo, pos) != startins)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			   IR_LOOP, (uint16_t)op);
  if (op == BC_LOOP) {
    int64_t endpos = (int64_t)pos + (int64_t)bc_j(startins);
    BCIns back;
    int64_t target;
    if (bc_j(startins) <= 0 || endpos < 0 ||
	endpos >= (int64_t)pt->sizebc)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			     IR_LOOP, (uint16_t)op);
    back = arm64_ir_bc_acq(lo, (MSize)endpos);
    target = endpos + 1 + (int64_t)bc_j(back);
    if (bc_op(back) != BC_JMP || bc_j(back) >= 0 || target < 0 ||
	target > (int64_t)pos || target >= (int64_t)pt->sizebc ||
	(MSize)slot > (MSize)pt->framesize)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			     IR_LOOP, (uint16_t)op);
  } else if (op == BC_FORL) {
    int64_t bodypos = (int64_t)pos + 1 + (int64_t)bc_j(startins);
    int64_t foripos = bodypos - 1;
    BCIns fori;
    int64_t exitpos;
    if (bc_j(startins) >= 0 || bodypos <= 0 || bodypos > (int64_t)pos ||
	foripos < 0 || foripos >= (int64_t)pt->sizebc ||
	(MSize)slot + FORL_EXT >= (MSize)pt->framesize)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			     IR_LOOP, (uint16_t)op);
    fori = arm64_ir_bc_acq(lo, (MSize)foripos);
    exitpos = foripos + 1 + (int64_t)bc_j(fori);
    if (bc_op(fori) != BC_FORI || bc_a(fori) != slot || bc_j(fori) <= 0 ||
	exitpos != (int64_t)pos + 1 ||
	arm64_ir_bc_acq(lo, pos) != startins ||
	arm64_ir_bc_acq(lo, (MSize)foripos) != fori)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			     IR_LOOP, (uint16_t)op);
  } else if (op == BC_FUNCF) {
    if (LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED || pos != 0 ||
	(pt->flags & PROTO_VARARG) != 0 ||
	!arm64_ir_funcf_bytecode(proto_bc(pt), pt->sizebc, pt->framesize,
	  startins, startins) ||
	pt->numparams > (BCReg)(pt->framesize-1u))
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			     IR_XPOLL, (uint16_t)op);
  } else {
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			   IR_LOOP, (uint16_t)op);
  }
  *lop = lo;
  *hip = hi;
  return 1;
}

enum {
  ARM64_IR_SCALAR_INT = 1u,
  ARM64_IR_SCALAR_NUM = 2u
};

enum {
  ARM64_IR_KPROFILE_INT = 1u,
  ARM64_IR_KPROFILE_HALF = 2u
};

#if LJ_HASJIT_FFI_CALLXS
/* First Darwin ARM64 native-call root. Keep the complete reference geometry
** explicit: LOOP substitution duplicates the rooted metatable lookup and both
** native lifecycles, while the exact function identity remains shared. */
typedef enum LJArm64CallXSProfile {
  ARM64_CALLXS_PROFILE_NONE,
  ARM64_CALLXS_PROFILE_I32,
  ARM64_CALLXS_PROFILE_DOUBLE
} LJArm64CallXSProfile;

enum {
  ARM64_CALLXS_K_ZERO = REF_TRUE-15u,
  ARM64_CALLXS_K_ROOT = REF_TRUE-14u,
  ARM64_CALLXS_K_TRACE = REF_TRUE-13u,
  ARM64_CALLXS_K_CTYPE = REF_TRUE-11u,
  ARM64_CALLXS_K_FTSZ = REF_TRUE-10u,
  ARM64_CALLXS_K_META = REF_TRUE-8u,
  ARM64_CALLXS_K_KEY = REF_TRUE-6u,
  ARM64_CALLXS_K_TABLE = REF_TRUE-4u,
  ARM64_CALLXS_K_LIMITMAX = REF_TRUE-2u,
  ARM64_CALLXS_K_ONE = REF_TRUE-1u,

  ARM64_CALLXS_R_LIMIT = REF_FIRST,
  ARM64_CALLXS_R_LIMIT_GUARD,
  ARM64_CALLXS_R_INDEX,
  ARM64_CALLXS_R_FUNC,
  ARM64_CALLXS_R_MT,
  ARM64_CALLXS_R_MT_GUARD,
  ARM64_CALLXS_R_TABLE_ROOT,
  ARM64_CALLXS_R_KEY_ROOT,
  ARM64_CALLXS_R_LOOKUP_ARGS,
  ARM64_CALLXS_R_LOOKUP_OUT,
  ARM64_CALLXS_R_LOOKUP,
  ARM64_CALLXS_R_MOBJ,
  ARM64_CALLXS_R_MOBJ_GUARD,
  ARM64_CALLXS_R_CTYPE,
  ARM64_CALLXS_R_CTYPE_GUARD,
  ARM64_CALLXS_R_FUNCPTR,
  ARM64_CALLXS_R_XSAVE_PRE,
  ARM64_CALLXS_R_ENTER_ARGS,
  ARM64_CALLXS_R_ENTER_ROOT,
  ARM64_CALLXS_R_ENTER_PRE,
  ARM64_CALLXS_R_ENTER_GUARD_PRE,
  ARM64_CALLXS_R_CALL_PRE,
  ARM64_CALLXS_R_LEAVE_PRE,
  ARM64_CALLXS_R_LEAVE_GUARD_PRE,
  ARM64_CALLXS_R_RESULT_GUARD_PRE,
  ARM64_CALLXS_R_INDEX_PRE,
  ARM64_CALLXS_R_BOUND_GUARD_PRE,
  ARM64_CALLXS_R_LOOP,
  ARM64_CALLXS_R_XPOLL,
  ARM64_CALLXS_R_MT_BODY,
  ARM64_CALLXS_R_MT_GUARD_BODY,
  ARM64_CALLXS_R_TABLE_ROOT_BODY,
  ARM64_CALLXS_R_KEY_ROOT_BODY,
  ARM64_CALLXS_R_LOOKUP_ARGS_BODY,
  ARM64_CALLXS_R_LOOKUP_OUT_BODY,
  ARM64_CALLXS_R_LOOKUP_BODY,
  ARM64_CALLXS_R_MOBJ_BODY,
  ARM64_CALLXS_R_MOBJ_GUARD_BODY,
  ARM64_CALLXS_R_XSAVE_BODY,
  ARM64_CALLXS_R_ENTER_BODY,
  ARM64_CALLXS_R_ENTER_GUARD_BODY,
  ARM64_CALLXS_R_CALL_BODY,
  ARM64_CALLXS_R_LEAVE_BODY,
  ARM64_CALLXS_R_LEAVE_GUARD_BODY,
  ARM64_CALLXS_R_RESULT_GUARD_BODY,
  ARM64_CALLXS_R_INDEX_BODY,
  ARM64_CALLXS_R_BOUND_GUARD_BODY,
  ARM64_CALLXS_R_INDEX_PHI,
  ARM64_CALLXS_SEMANTIC_NINS
};

/* double(double) adds one conversion immediately before each XSAVE. */
enum {
  ARM64_CALLXS_D_R_ARG_PRE = ARM64_CALLXS_R_XSAVE_PRE,
  ARM64_CALLXS_D_R_XSAVE_PRE = ARM64_CALLXS_R_XSAVE_PRE+1u,
  ARM64_CALLXS_D_R_INDEX_PRE = ARM64_CALLXS_R_INDEX_PRE+1u,
  ARM64_CALLXS_D_R_XPOLL = ARM64_CALLXS_R_XPOLL+1u,
  ARM64_CALLXS_D_R_MT_BODY = ARM64_CALLXS_R_MT_BODY+1u,
  ARM64_CALLXS_D_R_ARG_BODY = ARM64_CALLXS_R_XSAVE_BODY+1u,
  ARM64_CALLXS_D_R_XSAVE_BODY = ARM64_CALLXS_R_XSAVE_BODY+2u,
  ARM64_CALLXS_D_R_INDEX_PHI = ARM64_CALLXS_R_INDEX_PHI+2u,
  ARM64_CALLXS_D_SEMANTIC_NINS = ARM64_CALLXS_SEMANTIC_NINS+2u
};

/* Exact const char *(const char *) root. Its boxed result deliberately uses a
** separate certificate: allocation snapshots and result rooting make it
** structurally different from either scalar profile. */
enum {
  ARM64_CALLXS_PTR_K_ZERO = REF_TRUE-19u,
  ARM64_CALLXS_PTR_K_TRACE = REF_TRUE-18u,
  ARM64_CALLXS_PTR_K_PAYLOAD_OFS = REF_TRUE-16u,
  ARM64_CALLXS_PTR_K_BOX_CTYPE = REF_TRUE-14u,
  ARM64_CALLXS_PTR_K_STRING_OFS = REF_TRUE-13u,
  ARM64_CALLXS_PTR_K_CTYPE = REF_TRUE-11u,
  ARM64_CALLXS_PTR_K_FTSZ = REF_TRUE-10u,
  ARM64_CALLXS_PTR_K_META = REF_TRUE-8u,
  ARM64_CALLXS_PTR_K_KEY = REF_TRUE-6u,
  ARM64_CALLXS_PTR_K_TABLE = REF_TRUE-4u,
  ARM64_CALLXS_PTR_K_LIMITMAX = REF_TRUE-2u,
  ARM64_CALLXS_PTR_K_ONE = REF_TRUE-1u,

  ARM64_CALLXS_PTR_R_LIMIT = REF_FIRST,
  ARM64_CALLXS_PTR_R_LIMIT_GUARD,
  ARM64_CALLXS_PTR_R_INDEX,
  ARM64_CALLXS_PTR_R_FUNC,
  ARM64_CALLXS_PTR_R_STRING,
  ARM64_CALLXS_PTR_R_MT,
  ARM64_CALLXS_PTR_R_MT_GUARD,
  ARM64_CALLXS_PTR_R_TABLE_ROOT,
  ARM64_CALLXS_PTR_R_KEY_ROOT,
  ARM64_CALLXS_PTR_R_LOOKUP_ARGS,
  ARM64_CALLXS_PTR_R_LOOKUP_OUT,
  ARM64_CALLXS_PTR_R_LOOKUP,
  ARM64_CALLXS_PTR_R_MOBJ,
  ARM64_CALLXS_PTR_R_MOBJ_GUARD,
  ARM64_CALLXS_PTR_R_CTYPE,
  ARM64_CALLXS_PTR_R_CTYPE_GUARD,
  ARM64_CALLXS_PTR_R_FUNCPTR,
  ARM64_CALLXS_PTR_R_STRING_PTR,
  ARM64_CALLXS_PTR_R_BOX_PRE,
  ARM64_CALLXS_PTR_R_PAYLOAD_PRE,
  ARM64_CALLXS_PTR_R_XSAVE_PRE,
  ARM64_CALLXS_PTR_R_ENTER_ARGS,
  ARM64_CALLXS_PTR_R_ENTER_ROOT_PRE,
  ARM64_CALLXS_PTR_R_ENTER_PRE,
  ARM64_CALLXS_PTR_R_ENTER_GUARD_PRE,
  ARM64_CALLXS_PTR_R_CALL_PRE,
  ARM64_CALLXS_PTR_R_STORE_PRE,
  ARM64_CALLXS_PTR_R_LEAVE_PRE,
  ARM64_CALLXS_PTR_R_LEAVE_GUARD_PRE,
  ARM64_CALLXS_PTR_R_RESULT_PRE,
  ARM64_CALLXS_PTR_R_INDEX_PRE,
  ARM64_CALLXS_PTR_R_BOUND_GUARD_PRE,
  ARM64_CALLXS_PTR_R_LOOP,
  ARM64_CALLXS_PTR_R_XPOLL,
  ARM64_CALLXS_PTR_R_MT_BODY,
  ARM64_CALLXS_PTR_R_MT_GUARD_BODY,
  ARM64_CALLXS_PTR_R_TABLE_ROOT_BODY,
  ARM64_CALLXS_PTR_R_KEY_ROOT_BODY,
  ARM64_CALLXS_PTR_R_LOOKUP_ARGS_BODY,
  ARM64_CALLXS_PTR_R_LOOKUP_OUT_BODY,
  ARM64_CALLXS_PTR_R_LOOKUP_BODY,
  ARM64_CALLXS_PTR_R_MOBJ_BODY,
  ARM64_CALLXS_PTR_R_MOBJ_GUARD_BODY,
  ARM64_CALLXS_PTR_R_BOX_BODY,
  ARM64_CALLXS_PTR_R_PAYLOAD_BODY,
  ARM64_CALLXS_PTR_R_XSAVE_BODY,
  ARM64_CALLXS_PTR_R_ENTER_ROOT_BODY,
  ARM64_CALLXS_PTR_R_ENTER_BODY,
  ARM64_CALLXS_PTR_R_ENTER_GUARD_BODY,
  ARM64_CALLXS_PTR_R_CALL_BODY,
  ARM64_CALLXS_PTR_R_STORE_BODY,
  ARM64_CALLXS_PTR_R_LEAVE_BODY,
  ARM64_CALLXS_PTR_R_LEAVE_GUARD_BODY,
  ARM64_CALLXS_PTR_R_INDEX_BODY,
  ARM64_CALLXS_PTR_R_BOUND_GUARD_BODY,
  ARM64_CALLXS_PTR_R_INDEX_PHI,
  ARM64_CALLXS_PTR_R_RESULT_PHI,
  ARM64_CALLXS_PTR_SEMANTIC_NINS
};
#endif

enum {
  ARM64_NUMDYN_ADD_LT = 1u,
  ARM64_NUMDYN_ADD_LE = 2u,
  ARM64_NUMDYN_SUB_GT = 3u,
  ARM64_NUMDYN_SUB_GE = 4u,
  ARM64_NUMDYN_ADD_GT = 5u,
  ARM64_NUMDYN_ADD_GE = 6u,
  ARM64_NUMDYN_MUL_LT = 7u,
  ARM64_NUMDYN_MUL_LE = 8u,
  ARM64_NUMDYN_DIV_LT = 9u,
  ARM64_NUMDYN_DIV_LE = 10u,
  ARM64_NUMDYN_DIV_GT = 11u,
  ARM64_NUMDYN_DIV_GE = 12u
};

enum {
  ARM64_NUMDYN_ARGS_NUM = 1u,
  ARM64_NUMDYN_ARGS_INT_STEP = 2u,
  ARM64_NUMDYN_ARGS_INT_LIMIT = 3u,
  ARM64_NUMDYN_ARGS_INT_X = 4u
};

static int arm64_numdynamic_is_sub(unsigned grammar_profile)
{
  return grammar_profile == ARM64_NUMDYN_SUB_GT ||
	 grammar_profile == ARM64_NUMDYN_SUB_GE;
}

static int arm64_numdynamic_is_mul(unsigned grammar_profile)
{
  return grammar_profile == ARM64_NUMDYN_MUL_LT ||
	 grammar_profile == ARM64_NUMDYN_MUL_LE;
}

static int arm64_numdynamic_is_div(unsigned grammar_profile)
{
  return grammar_profile == ARM64_NUMDYN_DIV_LT ||
	 grammar_profile == ARM64_NUMDYN_DIV_LE ||
	 grammar_profile == ARM64_NUMDYN_DIV_GT ||
	 grammar_profile == ARM64_NUMDYN_DIV_GE;
}

enum {
  ARM64_NUMADD_K_ONE = REF_TRUE-1u,
  ARM64_NUMADD_R_I = REF_FIRST,
  ARM64_NUMADD_R_X,
  ARM64_NUMADD_R_STEP,
  ARM64_NUMADD_R_I_PRE,
  ARM64_NUMADD_R_X_PRE,
  ARM64_NUMADD_R_LIMIT,
  ARM64_NUMADD_R_PRE_GUARD,
  ARM64_NUMADD_R_LOOP,
  ARM64_NUMADD_R_XPOLL,
  ARM64_NUMADD_R_I_BODY,
  ARM64_NUMADD_R_X_BODY,
  ARM64_NUMADD_R_BODY_GUARD,
  ARM64_NUMADD_R_I_PHI,
  ARM64_NUMADD_R_X_PHI,
  ARM64_NUMADD_SEMANTIC_NINS
};

enum {
  ARM64_NUMHALF_K_HALF = REF_TRUE-2u,
  ARM64_NUMHALF_R_X = REF_FIRST,
  ARM64_NUMHALF_R_X_PRE,
  ARM64_NUMHALF_R_LIMIT,
  ARM64_NUMHALF_R_PRE_GUARD,
  ARM64_NUMHALF_R_LOOP,
  ARM64_NUMHALF_R_XPOLL,
  ARM64_NUMHALF_R_X_BODY,
  ARM64_NUMHALF_R_BODY_GUARD,
  ARM64_NUMHALF_R_X_PHI,
  ARM64_NUMHALF_SEMANTIC_NINS
};

enum {
  ARM64_NUMSTEP_R_X = REF_FIRST,
  ARM64_NUMSTEP_R_STEP,
  ARM64_NUMSTEP_R_X_PRE,
  ARM64_NUMSTEP_R_LIMIT,
  ARM64_NUMSTEP_R_PRE_GUARD,
  ARM64_NUMSTEP_R_LOOP,
  ARM64_NUMSTEP_R_XPOLL,
  ARM64_NUMSTEP_R_X_BODY,
  ARM64_NUMSTEP_R_BODY_GUARD,
  ARM64_NUMSTEP_R_X_PHI,
  ARM64_NUMSTEP_SEMANTIC_NINS
};

/* The all-parameter accumulator has the same compact IR numbering as the
** fixed-initial dynamic-step root. Keep separate names for its independent
** prototype and snapshot certificate without duplicating the IR policy. */
enum {
  ARM64_NUMACC_R_X = ARM64_NUMSTEP_R_X,
  ARM64_NUMACC_R_STEP = ARM64_NUMSTEP_R_STEP,
  ARM64_NUMACC_R_X_PRE = ARM64_NUMSTEP_R_X_PRE,
  ARM64_NUMACC_R_LIMIT = ARM64_NUMSTEP_R_LIMIT,
  ARM64_NUMACC_R_PRE_GUARD = ARM64_NUMSTEP_R_PRE_GUARD,
  ARM64_NUMACC_R_LOOP = ARM64_NUMSTEP_R_LOOP,
  ARM64_NUMACC_R_XPOLL = ARM64_NUMSTEP_R_XPOLL,
  ARM64_NUMACC_R_X_BODY = ARM64_NUMSTEP_R_X_BODY,
  ARM64_NUMACC_R_BODY_GUARD = ARM64_NUMSTEP_R_BODY_GUARD,
  ARM64_NUMACC_R_X_PHI = ARM64_NUMSTEP_R_X_PHI,
  ARM64_NUMACC_SEMANTIC_NINS = ARM64_NUMSTEP_SEMANTIC_NINS
};

/* One orthogonal all-parameter step kind inserts a single hoisted widening
** before the recurrence. Keep its shifted numbering explicit so neither
** admission pass can accidentally interpret one layout as the other. */
enum {
  ARM64_NUMACC_INTSTEP_R_X = REF_FIRST,
  ARM64_NUMACC_INTSTEP_R_STEP_INT,
  ARM64_NUMACC_INTSTEP_R_STEP_NUM,
  ARM64_NUMACC_INTSTEP_R_X_PRE,
  ARM64_NUMACC_INTSTEP_R_LIMIT,
  ARM64_NUMACC_INTSTEP_R_PRE_GUARD,
  ARM64_NUMACC_INTSTEP_R_LOOP,
  ARM64_NUMACC_INTSTEP_R_XPOLL,
  ARM64_NUMACC_INTSTEP_R_X_BODY,
  ARM64_NUMACC_INTSTEP_R_BODY_GUARD,
  ARM64_NUMACC_INTSTEP_R_X_PHI,
  ARM64_NUMACC_INTSTEP_SEMANTIC_NINS
};

/* The exact INT-limit geometry widens the invariant comparison bound after
** recording the first recurrence. It is intentionally distinct from the
** otherwise equal-sized INT-step layout. */
enum {
  ARM64_NUMACC_INTLIMIT_R_X = REF_FIRST,
  ARM64_NUMACC_INTLIMIT_R_STEP,
  ARM64_NUMACC_INTLIMIT_R_X_PRE,
  ARM64_NUMACC_INTLIMIT_R_LIMIT_INT,
  ARM64_NUMACC_INTLIMIT_R_LIMIT_NUM,
  ARM64_NUMACC_INTLIMIT_R_PRE_GUARD,
  ARM64_NUMACC_INTLIMIT_R_LOOP,
  ARM64_NUMACC_INTLIMIT_R_XPOLL,
  ARM64_NUMACC_INTLIMIT_R_X_BODY,
  ARM64_NUMACC_INTLIMIT_R_BODY_GUARD,
  ARM64_NUMACC_INTLIMIT_R_X_PHI,
  ARM64_NUMACC_INTLIMIT_SEMANTIC_NINS
};

/* Widening the loop-carried accumulator also makes the loop optimizer repair
** its original INT type after XPOLL. Keep both conversions and their ordering
** explicit: neither result may be mistaken for an ordinary NUM argument. */
enum {
  ARM64_NUMACC_INTX_R_X_INT = REF_FIRST,
  ARM64_NUMACC_INTX_R_STEP,
  ARM64_NUMACC_INTX_R_X_NUM,
  ARM64_NUMACC_INTX_R_X_PRE,
  ARM64_NUMACC_INTX_R_LIMIT,
  ARM64_NUMACC_INTX_R_PRE_GUARD,
  ARM64_NUMACC_INTX_R_LOOP,
  ARM64_NUMACC_INTX_R_XPOLL,
  ARM64_NUMACC_INTX_R_X_CHECK,
  ARM64_NUMACC_INTX_R_X_BODY,
  ARM64_NUMACC_INTX_R_BODY_GUARD,
  ARM64_NUMACC_INTX_R_X_PHI,
  ARM64_NUMACC_INTX_SEMANTIC_NINS
};

#define ARM64_NUMHALF_BITS UINT64_C(0x3fe0000000000000)

static int arm64_ir_numhalf_constant(const IRIns *ir, IRRef nk)
{
  const IRIns *k;
  if (ir == NULL || nk != ARM64_NUMHALF_K_HALF)
    return 0;
  k = &ir[ARM64_NUMHALF_K_HALF];
  return k->o == IR_KNUM && k->t.irt == IRT_NUM && k->op12 == 0 &&
	 k[1].tv.u64 == ARM64_NUMHALF_BITS;
}

static int arm64_postra_numhalf_constant(const IRIns *ir, IRRef nk,
	int require_evict)
{
  IRIns k, payload;
  if (ir == NULL || nk != ARM64_NUMHALF_K_HALF)
    return 0;
  k = ir_load_acq(&ir[ARM64_NUMHALF_K_HALF]);
  payload = ir_load_acq(&ir[ARM64_NUMHALF_K_HALF+1u]);
  return k.o == IR_KNUM && k.t.irt == IRT_NUM && k.op12 == 0 &&
	 (!require_evict || (k.r == RID_INIT && k.s == SPS_NONE)) &&
	 payload.tv.u64 == ARM64_NUMHALF_BITS;
}

static int arm64_numhalf_snapshots(const SnapShot *snap,
	const SnapEntry *snapmap, MSize nsnap, MSize nsnapmap,
	const BCIns *proto_bc, MSize proto_sizebc, uint8_t base_delta)
{
  static const IRRef refs[5] = {
    ARM64_NUMHALF_R_X, ARM64_NUMHALF_R_LIMIT,
    ARM64_NUMHALF_R_PRE_GUARD, ARM64_NUMHALF_R_LOOP,
    ARM64_NUMHALF_R_BODY_GUARD
  };
  static const uint16_t mapofs[5] = { 0, 2, 6, 9, 12 };
  static const uint8_t nent[5] = { 0, 2, 1, 1, 1 };
  static const uint8_t nslots[5] = { 4, 5, 4, 4, 4 };
  static const uint8_t pcpos[5] = { 7, 3, 11, 7, 11 };
  static const SnapEntry entries[5] = {
    SNAP(3, 0, ARM64_NUMHALF_R_X_PRE),
    SNAP(4, 0, ARM64_NUMHALF_R_X_PRE),
    SNAP(3, 0, ARM64_NUMHALF_R_X_PRE),
    SNAP(3, 0, ARM64_NUMHALF_R_X_PRE),
    SNAP(3, 0, ARM64_NUMHALF_R_X_BODY)
  };
  MSize snapno, entry = 0;
  if (snap == NULL || snapmap == NULL || proto_bc == NULL ||
	nsnap != 5 || nsnapmap != 15 || proto_sizebc != 13)
    return 0;
  for (snapno = 0; snapno < 5; snapno++) {
    const SnapShot *s = &snap[snapno];
    SnapEntry pcraw[1+LJ_FR2];
    uint64_t pcbase;
    uintptr_t expected;
    MSize n;
    if (snap_ref_acq(s) != refs[snapno] ||
	snap_mapofs_acq(s) != mapofs[snapno] ||
	snap_nent_acq(s) != nent[snapno] ||
	snap_nslots_acq(s) != nslots[snapno] ||
	snap_topslot_acq(s) != 4)
      return 0;
    for (n = 0; n < nent[snapno]; n++)
      if (snapentry_acq(&snapmap[mapofs[snapno]+n]) != entries[entry++])
	return 0;
    for (n = 0; n < 1u+LJ_FR2; n++)
      pcraw[n] = snapentry_acq(&snapmap[mapofs[snapno]+nent[snapno]+n]);
    LJ_STATIC_ASSERT(sizeof(pcraw) == sizeof(pcbase));
    memcpy(&pcbase, pcraw, sizeof(pcbase));
    expected = (uintptr_t)(const void *)(proto_bc+pcpos[snapno]);
    if ((uint8_t)pcbase != base_delta ||
	(uintptr_t)(pcbase >> 8) != expected)
      return 0;
  }
  return entry == 5;
}

static int arm64_numstep_snapshots(const SnapShot *snap,
	const SnapEntry *snapmap, MSize nsnap, MSize nsnapmap,
	const BCIns *proto_bc, MSize proto_sizebc, uint8_t base_delta)
{
  static const IRRef refs[5] = {
    ARM64_NUMSTEP_R_X, ARM64_NUMSTEP_R_LIMIT,
    ARM64_NUMSTEP_R_PRE_GUARD, ARM64_NUMSTEP_R_LOOP,
    ARM64_NUMSTEP_R_BODY_GUARD
  };
  static const uint16_t mapofs[5] = { 0, 2, 6, 9, 12 };
  static const uint8_t nent[5] = { 0, 2, 1, 1, 1 };
  static const uint8_t nslots[5] = { 5, 6, 5, 5, 5 };
  static const uint8_t pcpos[5] = { 7, 3, 12, 7, 12 };
  static const SnapEntry entries[5] = {
    SNAP(4, 0, ARM64_NUMSTEP_R_X_PRE),
    SNAP(5, 0, ARM64_NUMSTEP_R_X_PRE),
    SNAP(4, 0, ARM64_NUMSTEP_R_X_PRE),
    SNAP(4, 0, ARM64_NUMSTEP_R_X_PRE),
    SNAP(4, 0, ARM64_NUMSTEP_R_X_BODY)
  };
  MSize snapno, entry = 0;
  if (snap == NULL || snapmap == NULL || proto_bc == NULL ||
	nsnap != 5 || nsnapmap != 15 || proto_sizebc != 14)
    return 0;
  for (snapno = 0; snapno < 5; snapno++) {
    const SnapShot *s = &snap[snapno];
    SnapEntry pcraw[1+LJ_FR2];
    uint64_t pcbase;
    uintptr_t expected;
    MSize n;
    if (snap_ref_acq(s) != refs[snapno] ||
	snap_mapofs_acq(s) != mapofs[snapno] ||
	snap_nent_acq(s) != nent[snapno] ||
	snap_nslots_acq(s) != nslots[snapno] ||
	snap_topslot_acq(s) != 5)
      return 0;
    for (n = 0; n < nent[snapno]; n++)
      if (snapentry_acq(&snapmap[mapofs[snapno]+n]) != entries[entry++])
	return 0;
    for (n = 0; n < 1u+LJ_FR2; n++)
      pcraw[n] = snapentry_acq(&snapmap[mapofs[snapno]+nent[snapno]+n]);
    LJ_STATIC_ASSERT(sizeof(pcraw) == sizeof(pcbase));
    memcpy(&pcbase, pcraw, sizeof(pcbase));
    expected = (uintptr_t)(const void *)(proto_bc+pcpos[snapno]);
    if ((uint8_t)pcbase != base_delta ||
	(uintptr_t)(pcbase >> 8) != expected)
      return 0;
  }
  return entry == 5;
}

static int arm64_numacc_snapshots(const SnapShot *snap,
	const SnapEntry *snapmap, MSize nsnap, MSize nsnapmap,
	const BCIns *proto_bc, MSize proto_sizebc, uint8_t base_delta,
	unsigned args_kind)
{
  static const IRRef refs[4][5] = {
    { ARM64_NUMACC_R_X, ARM64_NUMACC_R_LIMIT,
      ARM64_NUMACC_R_PRE_GUARD, ARM64_NUMACC_R_LOOP,
      ARM64_NUMACC_R_BODY_GUARD },
    { ARM64_NUMACC_INTSTEP_R_X, ARM64_NUMACC_INTSTEP_R_LIMIT,
      ARM64_NUMACC_INTSTEP_R_PRE_GUARD, ARM64_NUMACC_INTSTEP_R_LOOP,
      ARM64_NUMACC_INTSTEP_R_BODY_GUARD },
    { ARM64_NUMACC_INTLIMIT_R_X, ARM64_NUMACC_INTLIMIT_R_LIMIT_INT,
      ARM64_NUMACC_INTLIMIT_R_PRE_GUARD, ARM64_NUMACC_INTLIMIT_R_LOOP,
      ARM64_NUMACC_INTLIMIT_R_BODY_GUARD },
    { ARM64_NUMACC_INTX_R_X_INT, ARM64_NUMACC_INTX_R_LIMIT,
      ARM64_NUMACC_INTX_R_PRE_GUARD, ARM64_NUMACC_INTX_R_LOOP,
      ARM64_NUMACC_INTX_R_BODY_GUARD }
  };
  static const uint16_t mapofs[5] = { 0, 2, 6, 9, 12 };
  static const uint8_t nent[5] = { 0, 2, 1, 1, 1 };
  static const uint8_t nslots[5] = { 5, 6, 5, 5, 5 };
  static const uint8_t pcpos[5] = { 6, 2, 11, 6, 11 };
  static const SnapEntry entries[4][5] = {
    { SNAP(2, 0, ARM64_NUMACC_R_X_PRE),
      SNAP(5, 0, ARM64_NUMACC_R_X_PRE),
      SNAP(2, 0, ARM64_NUMACC_R_X_PRE),
      SNAP(2, 0, ARM64_NUMACC_R_X_PRE),
      SNAP(2, 0, ARM64_NUMACC_R_X_BODY) },
    { SNAP(2, 0, ARM64_NUMACC_INTSTEP_R_X_PRE),
      SNAP(5, 0, ARM64_NUMACC_INTSTEP_R_X_PRE),
      SNAP(2, 0, ARM64_NUMACC_INTSTEP_R_X_PRE),
      SNAP(2, 0, ARM64_NUMACC_INTSTEP_R_X_PRE),
      SNAP(2, 0, ARM64_NUMACC_INTSTEP_R_X_BODY) },
    { SNAP(2, 0, ARM64_NUMACC_INTLIMIT_R_X_PRE),
      SNAP(5, 0, ARM64_NUMACC_INTLIMIT_R_X_PRE),
      SNAP(2, 0, ARM64_NUMACC_INTLIMIT_R_X_PRE),
      SNAP(2, 0, ARM64_NUMACC_INTLIMIT_R_X_PRE),
      SNAP(2, 0, ARM64_NUMACC_INTLIMIT_R_X_BODY) },
    { SNAP(2, 0, ARM64_NUMACC_INTX_R_X_PRE),
      SNAP(5, 0, ARM64_NUMACC_INTX_R_X_PRE),
      SNAP(2, 0, ARM64_NUMACC_INTX_R_X_PRE),
      SNAP(2, 0, ARM64_NUMACC_INTX_R_X_PRE),
      SNAP(2, 0, ARM64_NUMACC_INTX_R_X_BODY) }
  };
  MSize kindidx;
  MSize snapno, entry = 0;
  if (snap == NULL || snapmap == NULL || proto_bc == NULL ||
	nsnap != 5 || nsnapmap != 15 || proto_sizebc != 13)
    return 0;
  switch (args_kind) {
  case ARM64_NUMDYN_ARGS_NUM: kindidx = 0; break;
  case ARM64_NUMDYN_ARGS_INT_STEP: kindidx = 1; break;
  case ARM64_NUMDYN_ARGS_INT_LIMIT: kindidx = 2; break;
  case ARM64_NUMDYN_ARGS_INT_X: kindidx = 3; break;
  default: return 0;
  }
  for (snapno = 0; snapno < 5; snapno++) {
    const SnapShot *s = &snap[snapno];
    SnapEntry pcraw[1+LJ_FR2];
    uint64_t pcbase;
    uintptr_t expected;
    MSize n;
    if (snap_ref_acq(s) != refs[kindidx][snapno] ||
	snap_mapofs_acq(s) != mapofs[snapno] ||
	snap_nent_acq(s) != nent[snapno] ||
	snap_nslots_acq(s) != nslots[snapno] ||
	snap_topslot_acq(s) != 5)
      return 0;
    for (n = 0; n < nent[snapno]; n++)
      if (snapentry_acq(&snapmap[mapofs[snapno]+n]) !=
	  entries[kindidx][entry++])
	return 0;
    for (n = 0; n < 1u+LJ_FR2; n++)
      pcraw[n] = snapentry_acq(&snapmap[mapofs[snapno]+nent[snapno]+n]);
    LJ_STATIC_ASSERT(sizeof(pcraw) == sizeof(pcbase));
    memcpy(&pcbase, pcraw, sizeof(pcbase));
    expected = (uintptr_t)(const void *)(proto_bc+pcpos[snapno]);
    if ((uint8_t)pcbase != base_delta ||
	(uintptr_t)(pcbase >> 8) != expected)
      return 0;
  }
  return entry == 5;
}

/* Return true only for an admitted integer constant. */
static int arm64_ir_int_kref(const GCtrace *T, IRRef target)
{
  IRRef ref;
  if (target < T->nk || target >= REF_TRUE)
    return 0;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    const IRIns *ir = &T->ir[ref];
    if (ref == target)
      return ir->o == IR_KINT && ir->t.irt == IRT_INT;
  }
  return 0;
}

static int arm64_ir_int_value_op(IROp op, int allow_add)
{
  switch (op) {
  case IR_SLOAD: case IR_ADDOV: case IR_SUBOV: case IR_MULOV:
    return 1;
  case IR_ADD:
    return allow_add;
  default:
    return 0;
  }
}

static int arm64_ir_num_value_op(IROp op, int allow_sub, int allow_mul,
	int allow_div)
{
  return op == IR_SLOAD || op == IR_ADD || (allow_sub && op == IR_SUB) ||
	 (allow_mul && op == IR_MUL) || (allow_div && op == IR_DIV);
}

static int arm64_ir_sload_layout(IRIns ir, BCOp rootop, MSize forl_idxslot,
	MSize maxslots, IRType type)
{
  if (ir.op1 < 1u+LJ_FR2 || ir.op1 >= maxslots)
    return 0;
  if (type == IRT_NUM)
    return rootop == BC_LOOP && ir.t.irt == (IRT_NUM|IRT_GUARD) &&
	   ir.op2 == IRSLOAD_TYPECHECK;
  if (type != IRT_INT)
    return 0;
  if (rootop == BC_FORL) {
    if (ir.op1 == forl_idxslot)
      return arm64_ir_type_flags(ir.t, IRT_INT, IRT_GUARD,
				     IRT_GUARD|IRT_ISPHI) &&
		     ir.op2 == (IRSLOAD_TYPECHECK|IRSLOAD_INHERIT);
    if (ir.op1 == forl_idxslot+FORL_STOP)
      return ir.t.irt == IRT_INT &&
		     ir.op2 == (IRSLOAD_READONLY|IRSLOAD_INHERIT);
    if (ir.op1 == forl_idxslot+FORL_STEP)
      return ir.t.irt == IRT_INT &&
		     ir.op2 == (IRSLOAD_READONLY|IRSLOAD_INHERIT);
    if (ir.op1 == forl_idxslot+FORL_EXT)
      return 0;
  }
  return arm64_ir_type_flags(ir.t, IRT_INT, IRT_GUARD,
			     IRT_GUARD|IRT_ISPHI) &&
	 ir.op2 == IRSLOAD_TYPECHECK;
}

static int arm64_postra_int_value(IRIns ir, BCOp rootop,
	MSize forl_idxslot, MSize maxslots)
{
  int allow_add = rootop == BC_FORL;
  if (!arm64_ir_int_value_op((IROp)ir.o, allow_add))
    return 0;
  if (ir.o == IR_SLOAD)
    return arm64_ir_sload_layout(ir, rootop, forl_idxslot, maxslots, IRT_INT);
  if (ir.o == IR_ADD)
    return arm64_ir_type_flags(ir.t, IRT_INT, IRT_ISPHI, IRT_ISPHI);
  return arm64_ir_type_flags(ir.t, IRT_INT, IRT_GUARD,
			     IRT_GUARD|IRT_ISPHI);
}

static int arm64_postra_num_value(IRIns ir, BCOp rootop, MSize maxslots,
	int allow_sub, int allow_mul, int allow_div)
{
  if (rootop != BC_LOOP ||
	!arm64_ir_num_value_op((IROp)ir.o, allow_sub, allow_mul, allow_div))
    return 0;
  if (ir.o == IR_SLOAD)
    return arm64_ir_sload_layout(ir, rootop, 0, maxslots, IRT_NUM);
  return ir.t.irt == (IRT_NUM|IRT_ISPHI);
}

static int arm64_postra_spill_slot(MSize slot, MSize capacity)
{
  return slot >= SPS_FIRST && slot < capacity && slot < SPS_LIMIT;
}

static int arm64_postra_constants(const LJArm64PostRAView *view,
	unsigned *scalar_modep, unsigned *constant_profilep)
{
  IRRef ref;
  unsigned scalar_mode = 0;
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    IRIns k = ir_load_acq(&view->ir[ref]);
    if (k.o != IR_KPRI || k.t.irt != (uint8_t)(REF_NIL-ref) || k.op12 != 0)
      return 0;
  }
  if (arm64_postra_numhalf_constant(view->ir, view->nk, 0)) {
    *scalar_modep = ARM64_IR_SCALAR_NUM;
    *constant_profilep = ARM64_IR_KPROFILE_HALF;
    return 1;
  }
  for (ref = view->nk; ref < REF_TRUE; ref++) {
    IRIns k = ir_load_acq(&view->ir[ref]);
    if (k.o != IR_KINT || k.t.irt != IRT_INT)
      return 0;
    scalar_mode |= ARM64_IR_SCALAR_INT;
  }
  *scalar_modep = scalar_mode;
  *constant_profilep = ARM64_IR_KPROFILE_INT;
  return 1;
}

static int arm64_postra_scalar_kref(const LJArm64PostRAView *view,
	IRRef target, IRType type)
{
  IRRef ref;
  if (target < view->nk || target >= REF_TRUE)
    return 0;
  for (ref = view->nk; ref < REF_TRUE; ref++) {
    IRIns k = ir_load_acq(&view->ir[ref]);
    if (ref == target)
      return type == IRT_INT && k.o == IR_KINT && k.t.irt == IRT_INT;
  }
  return 0;
}

static int arm64_postra_scalar_value(IRIns ir, BCOp rootop,
	MSize forl_idxslot, MSize maxslots, IRType type, int allow_sub,
	int allow_mul, int allow_div)
{
  return type == IRT_NUM ?
    arm64_postra_num_value(ir, rootop, maxslots, allow_sub, allow_mul,
	allow_div) :
    arm64_postra_int_value(ir, rootop, forl_idxslot, maxslots);
}

static int arm64_postra_numadd_shape(const LJArm64PostRAView *view,
	IRRef semantic_nins)
{
  IRIns k, ipre, ibody, iphi, xpre, xbody, xphi;
  if (bc_op(view->startins) != BC_LOOP ||
	view->nk != ARM64_NUMADD_K_ONE ||
	semantic_nins != ARM64_NUMADD_SEMANTIC_NINS)
    return 0;
  k = ir_load_acq(&view->ir[ARM64_NUMADD_K_ONE]);
  if (k.o != IR_KINT || k.t.irt != IRT_INT || k.i != 1)
    return 0;
#define ARM64_NUMADD_POSTRA_INS(ref, op, left, right) \
  (ir_load_acq(&view->ir[(ref)]).o == (op) && \
   ir_load_acq(&view->ir[(ref)]).op1 == (left) && \
   ir_load_acq(&view->ir[(ref)]).op2 == (right))
  if (!ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_I, IR_SLOAD, 5,
	IRSLOAD_TYPECHECK) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_X, IR_SLOAD, 3,
	IRSLOAD_TYPECHECK) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_STEP, IR_SLOAD, 4,
	IRSLOAD_TYPECHECK) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_I_PRE, IR_ADDOV,
	ARM64_NUMADD_R_I, ARM64_NUMADD_K_ONE) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_X_PRE, IR_ADD,
	ARM64_NUMADD_R_STEP, ARM64_NUMADD_R_X) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_LIMIT, IR_SLOAD, 2,
	IRSLOAD_TYPECHECK) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_PRE_GUARD, IR_GT,
	ARM64_NUMADD_R_LIMIT, ARM64_NUMADD_R_I_PRE) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_LOOP, IR_LOOP, 0, 0) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_XPOLL, IR_XPOLL, 1, 0) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_I_BODY, IR_ADDOV,
	ARM64_NUMADD_R_I_PRE, ARM64_NUMADD_K_ONE) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_X_BODY, IR_ADD,
	ARM64_NUMADD_R_X_PRE, ARM64_NUMADD_R_STEP) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_BODY_GUARD, IR_LT,
	ARM64_NUMADD_R_I_BODY, ARM64_NUMADD_R_LIMIT) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_I_PHI, IR_PHI,
	ARM64_NUMADD_R_I_PRE, ARM64_NUMADD_R_I_BODY) ||
      !ARM64_NUMADD_POSTRA_INS(ARM64_NUMADD_R_X_PHI, IR_PHI,
	ARM64_NUMADD_R_X_PRE, ARM64_NUMADD_R_X_BODY)) {
    return 0;
  }
#undef ARM64_NUMADD_POSTRA_INS
  ipre = ir_load_acq(&view->ir[ARM64_NUMADD_R_I_PRE]);
  ibody = ir_load_acq(&view->ir[ARM64_NUMADD_R_I_BODY]);
  iphi = ir_load_acq(&view->ir[ARM64_NUMADD_R_I_PHI]);
  xpre = ir_load_acq(&view->ir[ARM64_NUMADD_R_X_PRE]);
  xbody = ir_load_acq(&view->ir[ARM64_NUMADD_R_X_BODY]);
  xphi = ir_load_acq(&view->ir[ARM64_NUMADD_R_X_PHI]);
  /* asm_phi() assigns the PHI register from its right (loop-body) value and
  ** asm_phi_shuffle() resolves the left value into that same register. With
  ** spills categorically closed, any mismatch is not a realizable layout. */
  return iphi.r == ipre.r && iphi.r == ibody.r &&
    xphi.r == xpre.r && xphi.r == xbody.r;
}

static int arm64_postra_numhalf_shape(const LJArm64PostRAView *view,
	IRRef semantic_nins)
{
  IRIns xpre, xbody, xphi;
  if (bc_op(view->startins) != BC_LOOP || bc_a(view->startins) != 2 ||
	view->root_topslot != 4 || view->proto_sizebc != 13 ||
	view->nk != ARM64_NUMHALF_K_HALF ||
	semantic_nins != ARM64_NUMHALF_SEMANTIC_NINS ||
	!arm64_postra_numhalf_constant(view->ir, view->nk, 1) ||
	!arm64_numhalf_snapshots(view->snap, view->snapmap,
	  view->nsnap, view->nsnapmap, view->proto_bc,
	  view->proto_sizebc, view->base_delta))
    return 0;
#define ARM64_NUMHALF_POSTRA_INS(ref, op, type, left, right) \
  (ir_load_acq(&view->ir[(ref)]).o == (op) && \
   ir_load_acq(&view->ir[(ref)]).t.irt == (type) && \
   ir_load_acq(&view->ir[(ref)]).op1 == (left) && \
   ir_load_acq(&view->ir[(ref)]).op2 == (right))
  if (!ARM64_NUMHALF_POSTRA_INS(ARM64_NUMHALF_R_X, IR_SLOAD,
	IRT_NUM|IRT_GUARD, 3, IRSLOAD_TYPECHECK) ||
      !ARM64_NUMHALF_POSTRA_INS(ARM64_NUMHALF_R_X_PRE, IR_ADD,
	IRT_NUM|IRT_ISPHI, ARM64_NUMHALF_R_X, ARM64_NUMHALF_K_HALF) ||
      !ARM64_NUMHALF_POSTRA_INS(ARM64_NUMHALF_R_LIMIT, IR_SLOAD,
	IRT_NUM|IRT_GUARD, 2, IRSLOAD_TYPECHECK) ||
      !ARM64_NUMHALF_POSTRA_INS(ARM64_NUMHALF_R_PRE_GUARD, IR_GT,
	IRT_NUM|IRT_GUARD, ARM64_NUMHALF_R_LIMIT, ARM64_NUMHALF_R_X_PRE) ||
      !ARM64_NUMHALF_POSTRA_INS(ARM64_NUMHALF_R_LOOP, IR_LOOP,
	IRT_NIL|IRT_GUARD, 0, 0) ||
      !ARM64_NUMHALF_POSTRA_INS(ARM64_NUMHALF_R_XPOLL, IR_XPOLL,
	IRT_NIL|IRT_GUARD, 1, 0) ||
      !ARM64_NUMHALF_POSTRA_INS(ARM64_NUMHALF_R_X_BODY, IR_ADD,
	IRT_NUM|IRT_ISPHI, ARM64_NUMHALF_R_X_PRE,
	ARM64_NUMHALF_K_HALF) ||
      !ARM64_NUMHALF_POSTRA_INS(ARM64_NUMHALF_R_BODY_GUARD, IR_LT,
	IRT_NUM|IRT_GUARD, ARM64_NUMHALF_R_X_BODY,
	ARM64_NUMHALF_R_LIMIT) ||
      !ARM64_NUMHALF_POSTRA_INS(ARM64_NUMHALF_R_X_PHI, IR_PHI,
	IRT_NUM, ARM64_NUMHALF_R_X_PRE, ARM64_NUMHALF_R_X_BODY))
    return 0;
#undef ARM64_NUMHALF_POSTRA_INS
  xpre = ir_load_acq(&view->ir[ARM64_NUMHALF_R_X_PRE]);
  xbody = ir_load_acq(&view->ir[ARM64_NUMHALF_R_X_BODY]);
  xphi = ir_load_acq(&view->ir[ARM64_NUMHALF_R_X_PHI]);
  return xphi.r == xpre.r && xphi.r == xbody.r;
}

static int arm64_postra_numdynamic_kernel(const LJArm64PostRAView *view,
	IRRef xslot, IRRef stepslot, IRRef limitslot,
	unsigned grammar_profile, unsigned args_kind)
{
  IRIns x_int, x, step_int, step, xpre, limit_int, limit, xcheck, xbody, xphi;
  IRRef xintref, xref, stepintref, stepref, xpreref;
  IRRef limitintref, limitref, preguardref;
  IRRef loopref, xpollref, xcheckref, xbodyref, bodyguardref, xphiref;
  IRRef first_left, first_right;
  IROp recurrence_op, preop, bodyop;
  if (args_kind == ARM64_NUMDYN_ARGS_NUM) {
    xintref = 0;
    xref = ARM64_NUMSTEP_R_X;
    stepintref = 0;
    stepref = ARM64_NUMSTEP_R_STEP;
    xpreref = ARM64_NUMSTEP_R_X_PRE;
    limitintref = 0;
    limitref = ARM64_NUMSTEP_R_LIMIT;
    preguardref = ARM64_NUMSTEP_R_PRE_GUARD;
    loopref = ARM64_NUMSTEP_R_LOOP;
    xpollref = ARM64_NUMSTEP_R_XPOLL;
    xcheckref = 0;
    xbodyref = ARM64_NUMSTEP_R_X_BODY;
    bodyguardref = ARM64_NUMSTEP_R_BODY_GUARD;
    xphiref = ARM64_NUMSTEP_R_X_PHI;
  } else if (args_kind == ARM64_NUMDYN_ARGS_INT_STEP) {
    xintref = 0;
    xref = ARM64_NUMACC_INTSTEP_R_X;
    stepintref = ARM64_NUMACC_INTSTEP_R_STEP_INT;
    stepref = ARM64_NUMACC_INTSTEP_R_STEP_NUM;
    xpreref = ARM64_NUMACC_INTSTEP_R_X_PRE;
    limitintref = 0;
    limitref = ARM64_NUMACC_INTSTEP_R_LIMIT;
    preguardref = ARM64_NUMACC_INTSTEP_R_PRE_GUARD;
    loopref = ARM64_NUMACC_INTSTEP_R_LOOP;
    xpollref = ARM64_NUMACC_INTSTEP_R_XPOLL;
    xcheckref = 0;
    xbodyref = ARM64_NUMACC_INTSTEP_R_X_BODY;
    bodyguardref = ARM64_NUMACC_INTSTEP_R_BODY_GUARD;
    xphiref = ARM64_NUMACC_INTSTEP_R_X_PHI;
  } else if (args_kind == ARM64_NUMDYN_ARGS_INT_LIMIT &&
	grammar_profile == ARM64_NUMDYN_ADD_LT) {
    xintref = 0;
    xref = ARM64_NUMACC_INTLIMIT_R_X;
    stepintref = 0;
    stepref = ARM64_NUMACC_INTLIMIT_R_STEP;
    xpreref = ARM64_NUMACC_INTLIMIT_R_X_PRE;
    limitintref = ARM64_NUMACC_INTLIMIT_R_LIMIT_INT;
    limitref = ARM64_NUMACC_INTLIMIT_R_LIMIT_NUM;
    preguardref = ARM64_NUMACC_INTLIMIT_R_PRE_GUARD;
    loopref = ARM64_NUMACC_INTLIMIT_R_LOOP;
    xpollref = ARM64_NUMACC_INTLIMIT_R_XPOLL;
    xcheckref = 0;
    xbodyref = ARM64_NUMACC_INTLIMIT_R_X_BODY;
    bodyguardref = ARM64_NUMACC_INTLIMIT_R_BODY_GUARD;
    xphiref = ARM64_NUMACC_INTLIMIT_R_X_PHI;
  } else if (args_kind == ARM64_NUMDYN_ARGS_INT_X) {
    xintref = ARM64_NUMACC_INTX_R_X_INT;
    xref = ARM64_NUMACC_INTX_R_X_NUM;
    stepintref = 0;
    stepref = ARM64_NUMACC_INTX_R_STEP;
    xpreref = ARM64_NUMACC_INTX_R_X_PRE;
    limitintref = 0;
    limitref = ARM64_NUMACC_INTX_R_LIMIT;
    preguardref = ARM64_NUMACC_INTX_R_PRE_GUARD;
    loopref = ARM64_NUMACC_INTX_R_LOOP;
    xpollref = ARM64_NUMACC_INTX_R_XPOLL;
    xcheckref = ARM64_NUMACC_INTX_R_X_CHECK;
    xbodyref = ARM64_NUMACC_INTX_R_X_BODY;
    bodyguardref = ARM64_NUMACC_INTX_R_BODY_GUARD;
    xphiref = ARM64_NUMACC_INTX_R_X_PHI;
  } else {
    return 0;
  }
  if (grammar_profile == ARM64_NUMDYN_ADD_LT) {
    recurrence_op = IR_ADD;
    first_left = stepref;
    first_right = xref;
    preop = IR_GT;
    bodyop = IR_LT;
  } else if (grammar_profile == ARM64_NUMDYN_ADD_LE) {
    recurrence_op = IR_ADD;
    first_left = stepref;
    first_right = xref;
    preop = IR_GE;
    bodyop = IR_LE;
  } else if (grammar_profile == ARM64_NUMDYN_ADD_GT) {
    recurrence_op = IR_ADD;
    first_left = stepref;
    first_right = xref;
    preop = IR_LT;
    bodyop = IR_GT;
  } else if (grammar_profile == ARM64_NUMDYN_ADD_GE) {
    recurrence_op = IR_ADD;
    first_left = stepref;
    first_right = xref;
    preop = IR_LE;
    bodyop = IR_GE;
  } else if (grammar_profile == ARM64_NUMDYN_SUB_GT) {
    recurrence_op = IR_SUB;
    first_left = xref;
    first_right = stepref;
    preop = IR_LT;
    bodyop = IR_GT;
  } else if (grammar_profile == ARM64_NUMDYN_SUB_GE) {
    recurrence_op = IR_SUB;
    first_left = xref;
    first_right = stepref;
    preop = IR_LE;
    bodyop = IR_GE;
  } else if (grammar_profile == ARM64_NUMDYN_MUL_LT) {
    recurrence_op = IR_MUL;
    first_left = stepref;
    first_right = xref;
    preop = IR_GT;
    bodyop = IR_LT;
  } else if (grammar_profile == ARM64_NUMDYN_MUL_LE) {
    recurrence_op = IR_MUL;
    first_left = stepref;
    first_right = xref;
    preop = IR_GE;
    bodyop = IR_LE;
  } else if (grammar_profile == ARM64_NUMDYN_DIV_LT) {
    recurrence_op = IR_DIV;
    first_left = xref;
    first_right = stepref;
    preop = IR_GT;
    bodyop = IR_LT;
  } else if (grammar_profile == ARM64_NUMDYN_DIV_LE) {
    recurrence_op = IR_DIV;
    first_left = xref;
    first_right = stepref;
    preop = IR_GE;
    bodyop = IR_LE;
  } else if (grammar_profile == ARM64_NUMDYN_DIV_GT) {
    recurrence_op = IR_DIV;
    first_left = xref;
    first_right = stepref;
    preop = IR_LT;
    bodyop = IR_GT;
  } else if (grammar_profile == ARM64_NUMDYN_DIV_GE) {
    recurrence_op = IR_DIV;
    first_left = xref;
    first_right = stepref;
    preop = IR_LE;
    bodyop = IR_GE;
  } else {
    return 0;
  }
  if (args_kind == ARM64_NUMDYN_ARGS_INT_X) {
    first_left = xref;
    first_right = stepref;
  }
#define ARM64_NUMDYN_POSTRA_INS(ref, op, type, left, right) \
  (ir_load_acq(&view->ir[(ref)]).o == (op) && \
   ir_load_acq(&view->ir[(ref)]).t.irt == (type) && \
   ir_load_acq(&view->ir[(ref)]).op1 == (left) && \
   ir_load_acq(&view->ir[(ref)]).op2 == (right))
  if ((args_kind != ARM64_NUMDYN_ARGS_INT_X ?
       !ARM64_NUMDYN_POSTRA_INS(xref, IR_SLOAD,
	 IRT_NUM|IRT_GUARD, xslot, IRSLOAD_TYPECHECK) :
       (!ARM64_NUMDYN_POSTRA_INS(xintref, IR_SLOAD,
	  IRT_INT|IRT_GUARD, xslot, IRSLOAD_TYPECHECK) ||
	!ARM64_NUMDYN_POSTRA_INS(xref, IR_CONV,
	  IRT_NUM, xintref, IRCONV_NUM_INT))) ||
      (args_kind != ARM64_NUMDYN_ARGS_INT_STEP ?
       !ARM64_NUMDYN_POSTRA_INS(stepref, IR_SLOAD,
	 IRT_NUM|IRT_GUARD, stepslot, IRSLOAD_TYPECHECK) :
       (!ARM64_NUMDYN_POSTRA_INS(stepintref, IR_SLOAD,
	  IRT_INT|IRT_GUARD, stepslot, IRSLOAD_TYPECHECK) ||
	!ARM64_NUMDYN_POSTRA_INS(stepref, IR_CONV,
	  IRT_NUM, stepintref, IRCONV_NUM_INT))) ||
      !ARM64_NUMDYN_POSTRA_INS(xpreref, recurrence_op,
	IRT_NUM|IRT_ISPHI, first_left, first_right) ||
      (args_kind != ARM64_NUMDYN_ARGS_INT_LIMIT ?
       !ARM64_NUMDYN_POSTRA_INS(limitref, IR_SLOAD,
	 IRT_NUM|IRT_GUARD, limitslot, IRSLOAD_TYPECHECK) :
       (!ARM64_NUMDYN_POSTRA_INS(limitintref, IR_SLOAD,
	  IRT_INT|IRT_GUARD, limitslot, IRSLOAD_TYPECHECK) ||
	!ARM64_NUMDYN_POSTRA_INS(limitref, IR_CONV,
	  IRT_NUM, limitintref, IRCONV_NUM_INT))) ||
      !ARM64_NUMDYN_POSTRA_INS(preguardref, preop,
	IRT_NUM|IRT_GUARD, limitref, xpreref) ||
      !ARM64_NUMDYN_POSTRA_INS(loopref, IR_LOOP,
	IRT_NIL|IRT_GUARD, 0, 0) ||
      !ARM64_NUMDYN_POSTRA_INS(xpollref, IR_XPOLL,
	IRT_NIL|IRT_GUARD, 1, 0) ||
      (args_kind == ARM64_NUMDYN_ARGS_INT_X &&
       !ARM64_NUMDYN_POSTRA_INS(xcheckref, IR_CONV,
	 IRT_INT|IRT_GUARD, xpreref, IRCONV_INT_NUM|IRCONV_CHECK)) ||
      !ARM64_NUMDYN_POSTRA_INS(xbodyref, recurrence_op,
	IRT_NUM|IRT_ISPHI, xpreref, stepref) ||
      !ARM64_NUMDYN_POSTRA_INS(bodyguardref, bodyop,
	IRT_NUM|IRT_GUARD, xbodyref, limitref) ||
      !ARM64_NUMDYN_POSTRA_INS(xphiref, IR_PHI,
	IRT_NUM, xpreref, xbodyref))
    return 0;
#undef ARM64_NUMDYN_POSTRA_INS
  x = ir_load_acq(&view->ir[xref]);
  step = ir_load_acq(&view->ir[stepref]);
  xpre = ir_load_acq(&view->ir[xpreref]);
  limit = ir_load_acq(&view->ir[limitref]);
  xbody = ir_load_acq(&view->ir[xbodyref]);
  xphi = ir_load_acq(&view->ir[xphiref]);
  if (args_kind == ARM64_NUMDYN_ARGS_INT_X) {
    x_int = ir_load_acq(&view->ir[xintref]);
    xcheck = ir_load_acq(&view->ir[xcheckref]);
    if (x_int.s != SPS_NONE || x_int.r >= RID_MAX_GPR ||
	!rset_test(RSET_GPR, x_int.r) || x.s != SPS_NONE ||
	x.r < RID_MIN_FPR || x.r >= RID_MAX_FPR ||
	!rset_test(RSET_FPR, x.r) || xcheck.s != SPS_NONE ||
	xcheck.r >= RID_MAX_GPR || !rset_test(RSET_GPR, xcheck.r))
      return 0;
  }
  if (args_kind == ARM64_NUMDYN_ARGS_INT_STEP) {
    step_int = ir_load_acq(&view->ir[stepintref]);
    if (step_int.s != SPS_NONE || step_int.r >= RID_MAX_GPR ||
	!rset_test(RSET_GPR, step_int.r) || step.s != SPS_NONE ||
	step.r < RID_MIN_FPR || step.r >= RID_MAX_FPR ||
	!rset_test(RSET_FPR, step.r))
      return 0;
  }
  if (args_kind == ARM64_NUMDYN_ARGS_INT_LIMIT) {
    limit_int = ir_load_acq(&view->ir[limitintref]);
    if (limit_int.s != SPS_NONE || limit_int.r >= RID_MAX_GPR ||
	!rset_test(RSET_GPR, limit_int.r) || limit.s != SPS_NONE ||
	limit.r < RID_MIN_FPR || limit.r >= RID_MAX_FPR ||
	!rset_test(RSET_FPR, limit.r))
      return 0;
  }
  /* STEP and LIMIT remain live across the loop and cannot alias each other or
  ** the loop-carried family. X and STEP are simultaneous first-recurrence
  ** inputs. X may alias LIMIT or the first recurrence destination after it
  ** dies. */
  return xphi.r == xpre.r && xphi.r == xbody.r &&
    step.r != xphi.r && limit.r != xphi.r && step.r != limit.r &&
    x.r != step.r;
}

static int arm64_postra_numstep_shape(const LJArm64PostRAView *view,
	IRRef semantic_nins)
{
  if (bc_op(view->startins) != BC_LOOP || bc_a(view->startins) != 3 ||
	view->root_topslot != 5 || view->proto_sizebc != 14 ||
	view->nk != REF_TRUE ||
	semantic_nins != ARM64_NUMSTEP_SEMANTIC_NINS ||
	!arm64_numstep_snapshots(view->snap, view->snapmap,
	  view->nsnap, view->nsnapmap, view->proto_bc,
	  view->proto_sizebc, view->base_delta))
    return 0;
  return arm64_postra_numdynamic_kernel(view, 4, 3, 2,
	ARM64_NUMDYN_ADD_LT, ARM64_NUMDYN_ARGS_NUM);
}

static unsigned arm64_numacc_grammar_profile(const BCIns *proto_bc,
	MSize proto_sizebc)
{
  BCIns compare, recurrence;
  if (proto_bc == NULL || proto_sizebc != 13)
    return 0;
  compare = arm64_ir_bc_acq((uintptr_t)proto_bc, 3);
  recurrence = arm64_ir_bc_acq((uintptr_t)proto_bc, 8);
  if (bc_op(recurrence) == BC_ADDVV && bc_a(recurrence) == 3 &&
	bc_b(recurrence) == 3 && bc_c(recurrence) == 4) {
    if (bc_op(compare) == BC_ISGE && bc_a(compare) == 3 &&
	bc_d(compare) == 4)
      return ARM64_NUMDYN_ADD_LT;
    if (bc_op(compare) == BC_ISGT && bc_a(compare) == 3 &&
	bc_d(compare) == 4)
      return ARM64_NUMDYN_ADD_LE;
    if (bc_op(compare) == BC_ISGE && bc_a(compare) == 4 &&
	bc_d(compare) == 3)
      return ARM64_NUMDYN_ADD_GT;
    if (bc_op(compare) == BC_ISGT && bc_a(compare) == 4 &&
	bc_d(compare) == 3)
      return ARM64_NUMDYN_ADD_GE;
  } else if (bc_op(recurrence) == BC_SUBVV && bc_a(recurrence) == 3 &&
	bc_b(recurrence) == 3 && bc_c(recurrence) == 4) {
    if (bc_op(compare) == BC_ISGE && bc_a(compare) == 4 &&
	bc_d(compare) == 3)
      return ARM64_NUMDYN_SUB_GT;
    if (bc_op(compare) == BC_ISGT && bc_a(compare) == 4 &&
	bc_d(compare) == 3)
      return ARM64_NUMDYN_SUB_GE;
  } else if (bc_op(recurrence) == BC_MULVV && bc_a(recurrence) == 3 &&
	bc_b(recurrence) == 3 && bc_c(recurrence) == 4) {
    if (bc_op(compare) == BC_ISGE && bc_a(compare) == 3 &&
	bc_d(compare) == 4)
      return ARM64_NUMDYN_MUL_LT;
    if (bc_op(compare) == BC_ISGT && bc_a(compare) == 3 &&
	bc_d(compare) == 4)
      return ARM64_NUMDYN_MUL_LE;
  } else if (bc_op(recurrence) == BC_DIVVV && bc_a(recurrence) == 3 &&
	bc_b(recurrence) == 3 && bc_c(recurrence) == 4) {
    if (bc_op(compare) == BC_ISGE && bc_a(compare) == 3 &&
	bc_d(compare) == 4)
      return ARM64_NUMDYN_DIV_LT;
    if (bc_op(compare) == BC_ISGT && bc_a(compare) == 3 &&
	bc_d(compare) == 4)
      return ARM64_NUMDYN_DIV_LE;
    if (bc_op(compare) == BC_ISGE && bc_a(compare) == 4 &&
	bc_d(compare) == 3)
      return ARM64_NUMDYN_DIV_GT;
    if (bc_op(compare) == BC_ISGT && bc_a(compare) == 4 &&
	bc_d(compare) == 3)
      return ARM64_NUMDYN_DIV_GE;
  }
  return 0;
}

static int arm64_postra_numacc_shape(const LJArm64PostRAView *view,
	IRRef semantic_nins, unsigned args_kind)
{
  unsigned grammar_profile = arm64_numacc_grammar_profile(
	view->proto_bc, view->proto_sizebc);
  if ((args_kind != ARM64_NUMDYN_ARGS_NUM &&
	args_kind != ARM64_NUMDYN_ARGS_INT_STEP &&
	args_kind != ARM64_NUMDYN_ARGS_INT_LIMIT &&
	args_kind != ARM64_NUMDYN_ARGS_INT_X) ||
	(args_kind == ARM64_NUMDYN_ARGS_INT_LIMIT &&
	 grammar_profile != ARM64_NUMDYN_ADD_LT) ||
	bc_op(view->startins) != BC_LOOP || bc_a(view->startins) != 3 ||
	view->root_topslot != 5 || view->proto_sizebc != 13 ||
	view->nk != REF_TRUE ||
	grammar_profile == 0 ||
	semantic_nins != (args_kind == ARM64_NUMDYN_ARGS_NUM ?
	  ARM64_NUMACC_SEMANTIC_NINS :
	  args_kind == ARM64_NUMDYN_ARGS_INT_STEP ?
	  ARM64_NUMACC_INTSTEP_SEMANTIC_NINS :
	  args_kind == ARM64_NUMDYN_ARGS_INT_LIMIT ?
	  ARM64_NUMACC_INTLIMIT_SEMANTIC_NINS :
	  ARM64_NUMACC_INTX_SEMANTIC_NINS) ||
	!arm64_numacc_snapshots(view->snap, view->snapmap,
	  view->nsnap, view->nsnapmap, view->proto_bc,
	  view->proto_sizebc, view->base_delta, args_kind))
    return 0;
  return arm64_postra_numdynamic_kernel(view, 2, 4, 3,
	grammar_profile, args_kind);
}

static int arm64_ir_funcf_snapshots(const SnapShot *snap,
	const SnapEntry *snapmap, MSize nsnap, MSize nsnapmap,
	const BCIns *proto_bc, MSize proto_sizebc, MSize root_topslot,
	uint8_t base_delta);

static int arm64_postra_funcf_admit(const LJArm64PostRAView *view,
	BCIns liveins, IRRef *semantic_ninsp)
{
  const IRIns *ir;
  IRRef ref;
  if (view == NULL || (ir = view->ir) == NULL || view->snap == NULL ||
	view->snapmap == NULL || view->proto_bc == NULL ||
	view->nins <= REF_FIRST || view->nins >= REF_DROP ||
	view->nk == 0 || view->nk > REF_TRUE || view->nsnap == 0 ||
	view->nsnapmap == 0 || view->proto_sizebc == 0 ||
	view->root_topslot == 0 || view->root_topslot > UINT8_MAX ||
	view->base_delta != 0 ||
	view->nk != REF_TRUE || view->nins != REF_BASE+4u ||
	view->spadjust != 0 ||
	!arm64_ir_funcf_bytecode(view->proto_bc, view->proto_sizebc,
	  view->root_topslot, view->startins, liveins))
    return 0;
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    IRIns k = ir_load_acq(&ir[ref]);
    if (k.o != IR_KPRI || k.t.irt != (uint8_t)(REF_NIL-ref) ||
	k.op12 != 0)
      return 0;
  }
  {
    IRIns base = ir_load_acq(&ir[REF_BASE]);
    IRIns entry = ir_load_acq(&ir[REF_BASE+1u]);
    IRIns poll = ir_load_acq(&ir[REF_BASE+2u]);
    IRIns suffix = ir_load_acq(&ir[REF_BASE+3u]);
    if (base.o != IR_BASE || base.t.irt != IRT_PGC ||
	base.op1 != 0 || base.op2 != 0 || base.s != SPS_NONE ||
	entry.o != IR_NOP || entry.t.irt != IRT_NIL ||
	entry.op1 != 0 || entry.op2 != 0 || entry.s != SPS_NONE ||
	poll.o != IR_XPOLL || poll.t.irt != (IRT_NIL|IRT_GUARD) ||
	poll.op1 != 1 || poll.op2 != 0 || poll.s != SPS_NONE ||
	suffix.o != IR_NOP || suffix.t.irt != IRT_NIL ||
	suffix.op1 != 0 || suffix.op2 != 0 || suffix.s != SPS_NONE ||
	suffix.prev != 0)
      return 0;
  }
  if (!arm64_ir_funcf_snapshots(view->snap, view->snapmap, view->nsnap,
	view->nsnapmap, view->proto_bc, view->proto_sizebc,
	view->root_topslot, view->base_delta))
    return 0;
  if (semantic_ninsp)
    *semantic_ninsp = REF_BASE+3u;
  return 1;
}

/* Validate the exact spill-free variable-stop/variable-step FORL proof after
** register allocation. Constant-step roots have neither a STEP load nor
** IR_USE and retain the existing allocator certificate. */
static int arm64_postra_forl_dynamic_shape(const LJArm64PostRAView *view,
	IRRef semantic_nins, int *dynamic_stepp)
{
  const IRIns *ir = view->ir;
  MSize idxslot = (MSize)(1u+LJ_FR2+bc_a(view->startins));
  MSize stopslot = idxslot+FORL_STOP;
  MSize stepslot = idxslot+FORL_STEP;
  IRRef ref, stepref = 0, useref = 0, loopref = 0, xpollref = 0;
  IRRef firstphi = 0, preadd = 0, postadd = 0;
  IRRef indexphi = 0, accumphi = 0;
  unsigned nstep = 0, nuse = 0, nloop = 0, nxpoll = 0, nadd = 0;
  unsigned nphi = 0;
  IRIns stop, step, direction, overflow, use, idx;
  IRIns sum, accpre, accpost, pre, post, precmp, postcmp, zero;
  IROp cmpop;

  *dynamic_stepp = 0;
  for (ref = REF_FIRST; ref < semantic_nins; ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    if (ins.o == IR_SLOAD && ins.op1 == stepslot) {
      nstep++;
      stepref = ref;
    } else if (ins.o == IR_USE) {
      nuse++;
      useref = ref;
    } else if (ins.o == IR_LOOP) {
      nloop++;
      loopref = ref;
    } else if (ins.o == IR_XPOLL) {
      nxpoll++;
      xpollref = ref;
    } else if (ins.o == IR_PHI && firstphi == 0) {
      firstphi = ref;
    }
  }
  if (nstep == 0 && nuse == 0)
    return 1;
  if (nstep != 1u || nuse != 1u || nloop != 1u || nxpoll != 1u ||
      xpollref != loopref+1u || firstphi == 0)
    return 0;

  for (ref = REF_FIRST; ref < firstphi; ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    if (ins.o != IR_ADD)
      continue;
    nadd++;
    if (ref < loopref) {
      if (preadd != 0)
	return 0;
      preadd = ref;
    } else if (ref > xpollref) {
      if (postadd != 0)
	return 0;
      postadd = ref;
    } else {
      return 0;
    }
  }
  if (nadd != 2u || preadd <= REF_FIRST || postadd <= xpollref+1u ||
      preadd+1u >= loopref || postadd+1u >= firstphi)
    return 0;

  accpre = ir_load_acq(&ir[preadd-1u]);
  accpost = ir_load_acq(&ir[postadd-1u]);
  pre = ir_load_acq(&ir[preadd]);
  post = ir_load_acq(&ir[postadd]);
  precmp = ir_load_acq(&ir[preadd+1u]);
  postcmp = ir_load_acq(&ir[postadd+1u]);
  if (pre.op1 < REF_FIRST || pre.op1 >= preadd ||
      precmp.op2 < REF_FIRST || precmp.op2 >= stepref ||
      pre.op2 != stepref || post.op1 != preadd || post.op2 != stepref ||
      pre.t.irt != (IRT_INT|IRT_ISPHI) ||
      post.t.irt != (IRT_INT|IRT_ISPHI) ||
      precmp.op2+1u != stepref || stepref+4u != pre.op1)
    return 0;

  if (accpre.o != IR_ADDOV ||
      accpre.t.irt != (IRT_INT|IRT_GUARD|IRT_ISPHI) ||
      accpre.op1 < REF_FIRST || accpre.op1 >= preadd-1u ||
      accpre.op2 != pre.op1 || accpost.o != IR_ADDOV ||
      accpost.t.irt != (IRT_INT|IRT_GUARD|IRT_ISPHI) ||
      accpost.op1 != preadd || accpost.op2 != preadd-1u)
    return 0;

  stop = ir_load_acq(&ir[precmp.op2]);
  step = ir_load_acq(&ir[stepref]);
  direction = ir_load_acq(&ir[stepref+1u]);
  overflow = ir_load_acq(&ir[stepref+2u]);
  use = ir_load_acq(&ir[stepref+3u]);
  idx = ir_load_acq(&ir[pre.op1]);
  sum = ir_load_acq(&ir[accpre.op1]);
  if (stop.o != IR_SLOAD || stop.op1 != stopslot ||
      stop.op2 != (IRSLOAD_READONLY|IRSLOAD_INHERIT) ||
      stop.t.irt != IRT_INT || stop.s != SPS_NONE ||
      stop.r >= RID_MAX_GPR || !rset_test(RSET_GPR, stop.r) ||
      step.o != IR_SLOAD || step.op1 != stepslot ||
      step.op2 != (IRSLOAD_READONLY|IRSLOAD_INHERIT) ||
      step.t.irt != IRT_INT || step.s != SPS_NONE ||
      step.r >= RID_MAX_GPR || !rset_test(RSET_GPR, step.r) ||
      idx.o != IR_SLOAD || idx.op1 != idxslot ||
      idx.op2 != (IRSLOAD_TYPECHECK|IRSLOAD_INHERIT) ||
      idx.t.irt != (IRT_INT|IRT_GUARD) || idx.s != SPS_NONE ||
      idx.r >= RID_MAX_GPR || !rset_test(RSET_GPR, idx.r) ||
      sum.o != IR_SLOAD || sum.op1 != idxslot-1u ||
      sum.op2 != IRSLOAD_TYPECHECK ||
      sum.t.irt != (IRT_INT|IRT_GUARD) || sum.s != SPS_NONE ||
      sum.r >= RID_MAX_GPR || !rset_test(RSET_GPR, sum.r) ||
      accpre.s != SPS_NONE || accpre.r >= RID_MAX_GPR ||
      !rset_test(RSET_GPR, accpre.r) ||
      accpost.s != SPS_NONE || accpost.r >= RID_MAX_GPR ||
      !rset_test(RSET_GPR, accpost.r) ||
      pre.s != SPS_NONE || pre.r >= RID_MAX_GPR ||
      !rset_test(RSET_GPR, pre.r) ||
      post.s != SPS_NONE || post.r >= RID_MAX_GPR ||
      !rset_test(RSET_GPR, post.r))
    return 0;
  if ((direction.o != IR_GE && direction.o != IR_LT) ||
      direction.op1 != stepref || direction.t.irt != (IRT_INT|IRT_GUARD) ||
      !arm64_postra_scalar_kref(view, direction.op2, IRT_INT))
    return 0;
  zero = ir_load_acq(&ir[direction.op2]);
  if (zero.i != 0)
    return 0;
  cmpop = direction.o == IR_GE ? IR_LE : IR_GE;
  if (overflow.o != IR_ADDOV || overflow.op1 != stepref ||
      overflow.op2 != precmp.op2 ||
      overflow.t.irt != (IRT_INT|IRT_GUARD) ||
      overflow.s != SPS_NONE || overflow.r >= RID_MAX_GPR ||
      !rset_test(RSET_GPR, overflow.r) ||
      useref != stepref+3u || use.o != IR_USE || use.t.irt != IRT_INT ||
      use.op1 != stepref+2u || use.op2 != 0 || use.s != SPS_NONE ||
      precmp.o != cmpop || precmp.op1 != preadd ||
      precmp.t.irt != (IRT_INT|IRT_GUARD) ||
      postcmp.o != cmpop || postcmp.op1 != postadd ||
      postcmp.op2 != precmp.op2 ||
      postcmp.t.irt != (IRT_INT|IRT_GUARD))
    return 0;
  for (ref = firstphi; ref < semantic_nins; ref++) {
    IRIns phi = ir_load_acq(&ir[ref]);
    nphi++;
    if (phi.o != IR_PHI || phi.t.irt != IRT_INT ||
	phi.s != SPS_NONE || phi.r >= RID_MAX_GPR ||
	!rset_test(RSET_GPR, phi.r))
      return 0;
    if (phi.op1 == preadd && phi.op2 == postadd) {
      if (indexphi != 0)
	return 0;
      indexphi = ref;
    } else if (phi.op1 == preadd-1u && phi.op2 == postadd-1u) {
      if (accumphi != 0)
	return 0;
      accumphi = ref;
    } else {
      return 0;
    }
  }
  if (nphi != 2u || indexphi == 0 || accumphi == 0)
    return 0;
  {
    IRIns iphi = ir_load_acq(&ir[indexphi]);
    IRIns aphi = ir_load_acq(&ir[accumphi]);
    Reg ri = iphi.r;
    Reg ra = aphi.r;
    if (ri != pre.r || ri != post.r ||
	ra != accpre.r || ra != accpost.r ||
	stop.r == step.r || stop.r == ri || stop.r == ra ||
	step.r == ri || step.r == ra || ri == ra ||
	idx.r == stop.r || idx.r == step.r || idx.r == ra ||
	sum.r == stop.r || sum.r == step.r || sum.r == idx.r ||
	overflow.r == stop.r || overflow.r == step.r)
      return 0;
  }
  *dynamic_stepp = 1;
  return 1;
}

#if LJ_HASJIT_FFI_CALLXS
/* -- Exact scalar CALLXS root ------------------------------------------- */

static int arm64_callxs_ins(const IRIns *ir, IRRef ref, IROp op,
	IRType type, IRRef op1, IRRef op2)
{
  IRIns ins = ir_load_acq(&ir[ref]);
  return ins.o == op && ins.t.irt == type &&
	 ins.op1 == op1 && ins.op2 == op2;
}

static int arm64_callxs_constant_shape(const IRIns *ir, IRRef nk,
	const GCtrace *owner, const BCIns *proto_bc, MSize proto_sizebc)
{
  IRIns k;
  GCobj *o;
  IRRef ref;
  if (ir == NULL || proto_bc == NULL || proto_sizebc != 16 ||
	(uintptr_t)(const void *)proto_bc >
	  UINTPTR_MAX-8u*sizeof(BCIns) ||
	nk != ARM64_CALLXS_K_ZERO)
    return 0;
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    k = ir_load_acq(&ir[ref]);
    if (k.o != IR_KPRI || k.t.irt != (uint8_t)(REF_NIL-ref) ||
	k.op12 != 0)
      return 0;
  }
  k = ir_load_acq(&ir[ARM64_CALLXS_K_ZERO]);
  if (k.o != IR_KINT || k.t.irt != IRT_INT || k.i != 0)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_K_ROOT]);
  if (k.o != IR_KNULL || k.t.irt != IRT_CDATA || k.i != 0)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_K_TRACE]);
  if (owner == NULL) {
    if (k.o != IR_KNUM || k.t.irt != IRT_P64 || k.op12 != 0)
      return 0;
  } else {
    if (k.o != IR_KGC || k.t.irt != IRT_P64 || k.op12 != 0 ||
	ir_kgc_load_acq(&ir[ARM64_CALLXS_K_TRACE]) != obj2gco(owner))
      return 0;
  }
  k = ir_load_acq(&ir[ARM64_CALLXS_K_CTYPE]);
  if (k.o != IR_KINT || k.t.irt != IRT_INT || k.i <= 0 ||
	(uint32_t)k.i >= CTID_MAX)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_K_FTSZ]);
  if (k.o != IR_KNUM || k.t.irt != IRT_NUM || k.op12 != 0 ||
	ir_load_acq(&ir[ARM64_CALLXS_K_FTSZ+1u]).tv.u64 !=
	  (uint64_t)(uintptr_t)(const void *)(proto_bc+8))
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_K_META]);
  if (k.o != IR_KGC || k.t.irt != IRT_FUNC || k.op12 != 0)
    return 0;
  o = ir_kgc_load_acq(&ir[ARM64_CALLXS_K_META]);
  if (o == NULL || !checkptrGC(o) || o->gch.gct != (uint32_t)~LJ_TFUNC ||
	lj_func_ffid_acq(gco2func(o)) != FF_ffi_meta___call)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_K_KEY]);
  if (k.o != IR_KGC || k.t.irt != IRT_STR || k.op12 != 0)
    return 0;
  o = ir_kgc_load_acq(&ir[ARM64_CALLXS_K_KEY]);
  if (o == NULL || !checkptrGC(o) || o->gch.gct != (uint32_t)~LJ_TSTR ||
	o->str.len != 6 || memcmp(strdata(&o->str), "__call", 6) != 0)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_K_TABLE]);
  if (k.o != IR_KGC || k.t.irt != IRT_TAB || k.op12 != 0)
    return 0;
  o = ir_kgc_load_acq(&ir[ARM64_CALLXS_K_TABLE]);
  if (o == NULL || !checkptrGC(o) || o->gch.gct != (uint32_t)~LJ_TTAB)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_K_LIMITMAX]);
  if (k.o != IR_KINT || k.t.irt != IRT_INT || k.i != INT32_MAX-1)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_K_ONE]);
  return k.o == IR_KINT && k.t.irt == IRT_INT && k.i == 1;
}

static LJArm64CallXSProfile arm64_callxs_signature(const jit_State *J,
	const IRIns *ir)
{
  CTState *cts;
  CType ctf, field;
  CTInfo info, finfo;
  CTypeID id, fid, result;
  IRIns k;
  if (J == NULL || ir == NULL)
    return ARM64_CALLXS_PROFILE_NONE;
  k = ir_load_acq(&ir[ARM64_CALLXS_K_CTYPE]);
  if (k.o != IR_KINT || k.i <= 0)
    return ARM64_CALLXS_PROFILE_NONE;
  id = (CTypeID)k.i;
  cts = ctype_ctsG(J2G(J));
  if (lj_ctype_snapshot(cts, id, &ctf) <= 0)
    return ARM64_CALLXS_PROFILE_NONE;
  info = ctype_info_acq(&ctf);
  if (!ctype_isfunc(info) || ctype_size_acq(&ctf) != 1 ||
	(info & CTF_VARARG) != 0 ||
	ctype_cconv(info) != CTCC_CDECL)
    return ARM64_CALLXS_PROFILE_NONE;
  result = ctype_cid(info);
  if (result != CTID_INT32 && result != CTID_DOUBLE)
    return ARM64_CALLXS_PROFILE_NONE;
  fid = ctype_sib_acq(&ctf);
  if (fid == 0 || lj_ctype_snapshot(cts, fid, &field) <= 0)
    return ARM64_CALLXS_PROFILE_NONE;
  finfo = ctype_info_acq(&field);
  if (!ctype_isfield(finfo) || ctype_cid(finfo) != result ||
	ctype_sib_acq(&field) != 0)
    return ARM64_CALLXS_PROFILE_NONE;
  return result == CTID_INT32 ? ARM64_CALLXS_PROFILE_I32 :
	 ARM64_CALLXS_PROFILE_DOUBLE;
}

static int arm64_callxs_bytecode(const BCIns *bc, MSize sizebc,
	MSize framesize, MSize numparams, BCIns startins,
	const GCtrace *owner)
{
  BCIns ins[16];
  BCOp startop;
  MSize i;
  if (bc == NULL || sizebc != 16 || framesize != 9 || numparams != 2 ||
	((uintptr_t)(const void *)bc & (sizeof(BCIns)-1u)) != 0 ||
	(uintptr_t)(const void *)bc > UINTPTR_MAX-15u*sizeof(BCIns))
    return 0;
  for (i = 0; i < 16; i++)
    ins[i] = arm64_ir_bc_acq((uintptr_t)(const void *)bc, i);
  startop = bc_op(ins[13]);
  if (owner == NULL) {
    if (ins[13] != startins || startop != BC_FORL)
      return 0;
  } else {
    if ((startop == BC_FORL && ins[13] != startins) ||
	(startop != BC_FORL &&
	 (startop != BC_JFORL ||
	  bc_d(ins[13]) != trace_traceno_acq(owner))))
      return 0;
    if (bc_a(ins[13]) != bc_a(startins))
      return 0;
  }
  return bc_op(ins[0]) == BC_FUNCF && bc_a(ins[0]) == 9 &&
	 bc_d(ins[0]) == 0 &&
	 bc_op(ins[1]) == BC_KSHORT && bc_a(ins[1]) == 2 &&
	 bc_d(ins[1]) == 1 &&
	 bc_op(ins[2]) == BC_CGET && bc_a(ins[2]) == 3 &&
	 bc_d(ins[2]) == 1 &&
	 bc_op(ins[3]) == BC_KSHORT && bc_a(ins[3]) == 4 &&
	 bc_d(ins[3]) == 1 &&
	 bc_op(ins[4]) == BC_FORI && bc_a(ins[4]) == 2 &&
	 bc_j(ins[4]) == 9 &&
	 bc_op(ins[5]) == BC_CGET && bc_a(ins[5]) == 6 &&
	 bc_d(ins[5]) == 0 &&
	 bc_op(ins[6]) == BC_CGET && bc_a(ins[6]) == 8 &&
	 bc_d(ins[6]) == 5 &&
	 bc_op(ins[7]) == BC_CALL && bc_a(ins[7]) == 6 &&
	 bc_b(ins[7]) == 2 && bc_c(ins[7]) == 2 &&
	 bc_op(ins[8]) == BC_CGET && bc_a(ins[8]) == 7 &&
	 bc_d(ins[8]) == 5 &&
	 bc_op(ins[9]) == BC_ISEQV && bc_a(ins[9]) == 6 &&
	 bc_d(ins[9]) == 7 &&
	 bc_op(ins[10]) == BC_JMP && bc_a(ins[10]) == 6 &&
	 bc_j(ins[10]) == 2 &&
	 bc_op(ins[11]) == BC_KPRI && bc_a(ins[11]) == 6 &&
	 bc_d(ins[11]) == 1 &&
	 bc_op(ins[12]) == BC_RET1 && bc_a(ins[12]) == 6 &&
	 bc_d(ins[12]) == 2 &&
	 bc_a(ins[13]) == 2 && bc_j(startins) == -9 &&
	 bc_op(ins[14]) == BC_KPRI && bc_a(ins[14]) == 2 &&
	 bc_d(ins[14]) == 2 &&
	 bc_op(ins[15]) == BC_RET1 && bc_a(ins[15]) == 2 &&
	 bc_d(ins[15]) == 2;
}

static IRRef arm64_callxs_profile_ref(LJArm64CallXSProfile profile, IRRef ref)
{
  if (profile == ARM64_CALLXS_PROFILE_DOUBLE) {
    if (ref >= ARM64_CALLXS_R_XSAVE_BODY)
      return ref+2u;
    if (ref >= ARM64_CALLXS_R_XSAVE_PRE)
      return ref+1u;
  }
  return ref;
}

static int arm64_callxs_ir_shape(const IRIns *ir,
	LJArm64CallXSProfile profile)
{
#define ARM64_CALLXS_INS(ref, op, type, left, right) \
  arm64_callxs_ins(ir, (ref), (op), (type), (left), (right))
#define ARM64_CALLXS_REF(ref) arm64_callxs_profile_ref(profile, (ref))
  IRRef argpre, argbody;
  IRType calltype;
  if (profile != ARM64_CALLXS_PROFILE_I32 &&
	profile != ARM64_CALLXS_PROFILE_DOUBLE)
    return 0;
  argpre = profile == ARM64_CALLXS_PROFILE_DOUBLE ?
	ARM64_CALLXS_D_R_ARG_PRE : ARM64_CALLXS_R_INDEX;
  argbody = profile == ARM64_CALLXS_PROFILE_DOUBLE ?
	ARM64_CALLXS_D_R_ARG_BODY : ARM64_CALLXS_R_INDEX_PRE;
  calltype = profile == ARM64_CALLXS_PROFILE_DOUBLE ? IRT_NUM : IRT_INT;
  return ARM64_CALLXS_INS(REF_BASE, IR_BASE, IRT_PGC, 0, 0) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LIMIT),
	IR_SLOAD, IRT_INT,
	5, IRSLOAD_READONLY|IRSLOAD_INHERIT) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LIMIT_GUARD),
	IR_LE, IRT_INT|IRT_GUARD, ARM64_CALLXS_REF(ARM64_CALLXS_R_LIMIT),
	ARM64_CALLXS_K_LIMITMAX) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_INDEX), IR_SLOAD,
	IRT_INT|IRT_GUARD, 4, IRSLOAD_TYPECHECK|IRSLOAD_INHERIT) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_FUNC), IR_SLOAD,
	IRT_CDATA|IRT_GUARD, 2, IRSLOAD_TYPECHECK) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_MT), IR_FLOAD, IRT_TAB,
	REF_NIL, GG_OFS(g.gcroot[GCROOT_BASEMT+(~LJ_TCDATA)]) >> 2) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_MT_GUARD), IR_EQ,
	IRT_TAB|IRT_GUARD, ARM64_CALLXS_REF(ARM64_CALLXS_R_MT),
	ARM64_CALLXS_K_TABLE) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_TABLE_ROOT),
	IR_TMPREF, IRT_PGC,
	ARM64_CALLXS_K_TABLE, IRTMPREF_IN1|IRTMPREF_OUT1) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_KEY_ROOT),
	IR_TMPREF, IRT_PGC,
	ARM64_CALLXS_K_KEY, IRTMPREF_IN2) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOKUP_ARGS),
	IR_CARG, IRT_NIL, ARM64_CALLXS_REF(ARM64_CALLXS_R_TABLE_ROOT),
	ARM64_CALLXS_REF(ARM64_CALLXS_R_KEY_ROOT)) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOKUP_OUT),
	IR_CARG, IRT_NIL, ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOKUP_ARGS),
	ARM64_CALLXS_REF(ARM64_CALLXS_R_TABLE_ROOT)) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOKUP), IR_CALLS,
	IRT_P64|IRT_GUARD, ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOKUP_OUT),
	IRCALL_lj_tab_gettv_rooted) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_MOBJ), IR_VLOAD,
	IRT_FUNC|IRT_GUARD, ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOKUP), 0) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_MOBJ_GUARD), IR_EQ,
	IRT_FUNC|IRT_GUARD, ARM64_CALLXS_REF(ARM64_CALLXS_R_MOBJ),
	ARM64_CALLXS_K_META) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_CTYPE), IR_FLOAD,
	IRT_U16, ARM64_CALLXS_REF(ARM64_CALLXS_R_FUNC),
	IRFL_CDATA_CTYPEID) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_CTYPE_GUARD), IR_EQ,
	IRT_INT|IRT_GUARD, ARM64_CALLXS_REF(ARM64_CALLXS_R_CTYPE),
	ARM64_CALLXS_K_CTYPE) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_FUNCPTR), IR_FLOAD,
	IRT_P64, ARM64_CALLXS_REF(ARM64_CALLXS_R_FUNC), IRFL_CDATA_PTR) &&
    (profile != ARM64_CALLXS_PROFILE_DOUBLE ||
      ARM64_CALLXS_INS(ARM64_CALLXS_D_R_ARG_PRE, IR_CONV, IRT_NUM,
	ARM64_CALLXS_R_INDEX, IRCONV_NUM_INT)) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_XSAVE_PRE),
	IR_XSAVE, IRT_NIL, 0, 0) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_ENTER_ARGS),
	IR_CARG, IRT_NIL, ARM64_CALLXS_K_TRACE,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_FUNCPTR)) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_ENTER_ROOT),
	IR_CARG, IRT_NIL, ARM64_CALLXS_REF(ARM64_CALLXS_R_ENTER_ARGS),
	ARM64_CALLXS_K_ROOT) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_ENTER_PRE),
	IR_CALLS, IRT_INT, ARM64_CALLXS_REF(ARM64_CALLXS_R_ENTER_ROOT),
	IRCALL_lj_ffi_native_trace_enter) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_ENTER_GUARD_PRE),
	IR_NE, IRT_INT|IRT_GUARD,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_ENTER_PRE),
	ARM64_CALLXS_K_ZERO) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_CALL_PRE), IR_CALLXS,
	calltype, argpre, ARM64_CALLXS_REF(ARM64_CALLXS_R_FUNCPTR)) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LEAVE_PRE), IR_CALLS,
	IRT_INT|IRT_GUARD, REF_NIL, IRCALL_lj_ffi_native_trace_leave) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LEAVE_GUARD_PRE),
	IR_EQ, IRT_INT|IRT_GUARD,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_LEAVE_PRE),
	ARM64_CALLXS_K_ZERO) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_RESULT_GUARD_PRE),
	IR_EQ, calltype|IRT_GUARD,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_CALL_PRE), argpre) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_INDEX_PRE), IR_ADD,
	IRT_INT|IRT_ISPHI, ARM64_CALLXS_REF(ARM64_CALLXS_R_INDEX),
	ARM64_CALLXS_K_ONE) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_BOUND_GUARD_PRE),
	IR_LE, IRT_INT|IRT_GUARD,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_INDEX_PRE),
	ARM64_CALLXS_REF(ARM64_CALLXS_R_LIMIT)) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOP), IR_LOOP,
	IRT_NIL|IRT_GUARD, 0, 0) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_XPOLL), IR_XPOLL,
	IRT_NIL|IRT_GUARD, 1, 0) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_MT_BODY), IR_FLOAD,
	IRT_TAB,
	REF_NIL, GG_OFS(g.gcroot[GCROOT_BASEMT+(~LJ_TCDATA)]) >> 2) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_MT_GUARD_BODY),
	IR_EQ, IRT_TAB|IRT_GUARD,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_MT_BODY), ARM64_CALLXS_K_TABLE) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_TABLE_ROOT_BODY),
	IR_TMPREF, IRT_PGC,
	ARM64_CALLXS_K_TABLE, IRTMPREF_IN1|IRTMPREF_OUT1) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_KEY_ROOT_BODY),
	IR_TMPREF, IRT_PGC,
	ARM64_CALLXS_K_KEY, IRTMPREF_IN2) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOKUP_ARGS_BODY),
	IR_CARG, IRT_NIL,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_TABLE_ROOT_BODY),
	ARM64_CALLXS_REF(ARM64_CALLXS_R_KEY_ROOT_BODY)) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOKUP_OUT_BODY),
	IR_CARG, IRT_NIL,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOKUP_ARGS_BODY),
	ARM64_CALLXS_REF(ARM64_CALLXS_R_TABLE_ROOT_BODY)) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOKUP_BODY),
	IR_CALLS, IRT_P64|IRT_GUARD,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOKUP_OUT_BODY),
	IRCALL_lj_tab_gettv_rooted) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_MOBJ_BODY),
	IR_VLOAD, IRT_FUNC|IRT_GUARD,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_LOOKUP_BODY), 0) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_MOBJ_GUARD_BODY),
	IR_EQ, IRT_FUNC|IRT_GUARD,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_MOBJ_BODY),
	ARM64_CALLXS_K_META) &&
    (profile != ARM64_CALLXS_PROFILE_DOUBLE ||
      ARM64_CALLXS_INS(ARM64_CALLXS_D_R_ARG_BODY, IR_CONV, IRT_NUM,
	ARM64_CALLXS_D_R_INDEX_PRE, IRCONV_NUM_INT)) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_XSAVE_BODY),
	IR_XSAVE, IRT_NIL, 0, 0) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_ENTER_BODY),
	IR_CALLS, IRT_INT,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_ENTER_ROOT),
	IRCALL_lj_ffi_native_trace_enter) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_ENTER_GUARD_BODY),
	IR_NE, IRT_INT|IRT_GUARD,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_ENTER_BODY),
	ARM64_CALLXS_K_ZERO) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_CALL_BODY), IR_CALLXS,
	calltype, argbody, ARM64_CALLXS_REF(ARM64_CALLXS_R_FUNCPTR)) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LEAVE_BODY), IR_CALLS,
	IRT_INT|IRT_GUARD, REF_NIL, IRCALL_lj_ffi_native_trace_leave) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_LEAVE_GUARD_BODY),
	IR_EQ, IRT_INT|IRT_GUARD,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_LEAVE_BODY),
	ARM64_CALLXS_K_ZERO) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_RESULT_GUARD_BODY),
	IR_EQ, calltype|IRT_GUARD,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_CALL_BODY), argbody) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_INDEX_BODY), IR_ADD,
	IRT_INT|IRT_ISPHI, ARM64_CALLXS_REF(ARM64_CALLXS_R_INDEX_PRE),
	ARM64_CALLXS_K_ONE) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_BOUND_GUARD_BODY),
	IR_LE, IRT_INT|IRT_GUARD,
	ARM64_CALLXS_REF(ARM64_CALLXS_R_INDEX_BODY),
	ARM64_CALLXS_REF(ARM64_CALLXS_R_LIMIT)) &&
    ARM64_CALLXS_INS(ARM64_CALLXS_REF(ARM64_CALLXS_R_INDEX_PHI),
	IR_PHI, IRT_INT, ARM64_CALLXS_REF(ARM64_CALLXS_R_INDEX_PRE),
	ARM64_CALLXS_REF(ARM64_CALLXS_R_INDEX_BODY));
#undef ARM64_CALLXS_REF
#undef ARM64_CALLXS_INS
}

static int arm64_callxs_snapshots(const IRIns *ir, const SnapShot *snap,
	const SnapEntry *snapmap, MSize nsnap, MSize nsnapmap,
	const BCIns *proto_bc, MSize proto_sizebc,
	LJArm64CallXSProfile profile)
{
  static const IRRef refs[15] = {
    ARM64_CALLXS_R_LIMIT, ARM64_CALLXS_R_MT, ARM64_CALLXS_R_MOBJ,
    ARM64_CALLXS_R_CTYPE, ARM64_CALLXS_R_XSAVE_PRE,
    ARM64_CALLXS_R_LEAVE_PRE, ARM64_CALLXS_R_RESULT_GUARD_PRE,
    ARM64_CALLXS_R_BOUND_GUARD_PRE, ARM64_CALLXS_R_LOOP,
    ARM64_CALLXS_R_MT_BODY, ARM64_CALLXS_R_MOBJ_BODY,
    ARM64_CALLXS_R_XSAVE_BODY, ARM64_CALLXS_R_LEAVE_BODY,
    ARM64_CALLXS_R_RESULT_GUARD_BODY, ARM64_CALLXS_R_BOUND_GUARD_BODY
  };
  static const uint16_t mapofs[15] = {
    0, 2, 10, 18, 28, 38, 45, 48, 50, 56, 64, 72, 82, 89, 95
  };
  static const uint8_t nent[15] = {
    0, 6, 6, 8, 8, 5, 1, 0, 4, 6, 6, 8, 5, 4, 0
  };
  static const uint8_t nslots[15] = {
    2, 11, 11, 12, 12, 9, 8, 4, 8, 11, 11, 12, 9, 8, 4
  };
  static const uint8_t topslot[15] = {
    9, 9, 9, 11, 11, 9, 9, 9, 9, 9, 9, 11, 9, 9, 9
  };
  static const uint8_t counts[15] = {
    0, 0, 0, 0, SNAPCOUNT_DONE, SNAPCOUNT_DONE, 0, 0, 0,
    0, 0, SNAPCOUNT_DONE, SNAPCOUNT_DONE, 0, 0
  };
  static const uint8_t pcpos[15] = {
    5, 7, 7, 0, 0, 8, 11, 14, 5, 7, 7, 0, 8, 11, 14
  };
  static const SnapEntry entries[15][8] = {
    { 0 },
    { SNAP(4, SNAP_NORESTORE, ARM64_CALLXS_R_INDEX),
      SNAP(5, SNAP_NORESTORE, ARM64_CALLXS_R_LIMIT),
      SNAP(6, 0, ARM64_CALLXS_K_ONE),
      SNAP(7, 0, ARM64_CALLXS_R_INDEX),
      SNAP(8, 0, ARM64_CALLXS_R_FUNC),
      SNAP(10, 0, ARM64_CALLXS_R_INDEX) },
    { SNAP(4, SNAP_NORESTORE, ARM64_CALLXS_R_INDEX),
      SNAP(5, SNAP_NORESTORE, ARM64_CALLXS_R_LIMIT),
      SNAP(6, 0, ARM64_CALLXS_K_ONE),
      SNAP(7, 0, ARM64_CALLXS_R_INDEX),
      SNAP(8, 0, ARM64_CALLXS_R_FUNC),
      SNAP(10, 0, ARM64_CALLXS_R_INDEX) },
    { SNAP(4, SNAP_NORESTORE, ARM64_CALLXS_R_INDEX),
      SNAP(5, SNAP_NORESTORE, ARM64_CALLXS_R_LIMIT),
      SNAP(6, 0, ARM64_CALLXS_K_ONE),
      SNAP(7, 0, ARM64_CALLXS_R_INDEX),
      SNAP(8, 0, ARM64_CALLXS_K_META),
      SNAP(9, SNAP_FRAME, ARM64_CALLXS_K_FTSZ),
      SNAP(10, 0, ARM64_CALLXS_R_FUNC),
      SNAP(11, 0, ARM64_CALLXS_R_INDEX) },
    { SNAP(4, SNAP_NORESTORE, ARM64_CALLXS_R_INDEX),
      SNAP(5, SNAP_NORESTORE, ARM64_CALLXS_R_LIMIT),
      SNAP(6, 0, ARM64_CALLXS_K_ONE),
      SNAP(7, 0, ARM64_CALLXS_R_INDEX),
      SNAP(8, 0, ARM64_CALLXS_K_META),
      SNAP(9, SNAP_FRAME, ARM64_CALLXS_K_FTSZ),
      SNAP(10, 0, ARM64_CALLXS_R_FUNC),
      SNAP(11, 0, ARM64_CALLXS_R_INDEX) },
    { SNAP(4, SNAP_NORESTORE, ARM64_CALLXS_R_INDEX),
      SNAP(5, SNAP_NORESTORE, ARM64_CALLXS_R_LIMIT),
      SNAP(6, 0, ARM64_CALLXS_K_ONE),
      SNAP(7, 0, ARM64_CALLXS_R_INDEX),
      SNAP(8, 0, ARM64_CALLXS_R_CALL_PRE) },
    { SNAP(7, 0, ARM64_CALLXS_R_INDEX) },
    { 0 },
    { SNAP(4, 0, ARM64_CALLXS_R_INDEX_PRE),
      SNAP(5, SNAP_NORESTORE, ARM64_CALLXS_R_LIMIT),
      SNAP(6, 0, ARM64_CALLXS_K_ONE),
      SNAP(7, 0, ARM64_CALLXS_R_INDEX_PRE) },
    { SNAP(4, 0, ARM64_CALLXS_R_INDEX_PRE),
      SNAP(5, 0, ARM64_CALLXS_R_LIMIT),
      SNAP(6, 0, ARM64_CALLXS_K_ONE),
      SNAP(7, 0, ARM64_CALLXS_R_INDEX_PRE),
      SNAP(8, 0, ARM64_CALLXS_R_FUNC),
      SNAP(10, 0, ARM64_CALLXS_R_INDEX_PRE) },
    { SNAP(4, 0, ARM64_CALLXS_R_INDEX_PRE),
      SNAP(5, 0, ARM64_CALLXS_R_LIMIT),
      SNAP(6, 0, ARM64_CALLXS_K_ONE),
      SNAP(7, 0, ARM64_CALLXS_R_INDEX_PRE),
      SNAP(8, 0, ARM64_CALLXS_R_FUNC),
      SNAP(10, 0, ARM64_CALLXS_R_INDEX_PRE) },
    { SNAP(4, 0, ARM64_CALLXS_R_INDEX_PRE),
      SNAP(5, 0, ARM64_CALLXS_R_LIMIT),
      SNAP(6, 0, ARM64_CALLXS_K_ONE),
      SNAP(7, 0, ARM64_CALLXS_R_INDEX_PRE),
      SNAP(8, 0, ARM64_CALLXS_K_META),
      SNAP(9, SNAP_FRAME, ARM64_CALLXS_K_FTSZ),
      SNAP(10, 0, ARM64_CALLXS_R_FUNC),
      SNAP(11, 0, ARM64_CALLXS_R_INDEX_PRE) },
    { SNAP(4, 0, ARM64_CALLXS_R_INDEX_PRE),
      SNAP(5, 0, ARM64_CALLXS_R_LIMIT),
      SNAP(6, 0, ARM64_CALLXS_K_ONE),
      SNAP(7, 0, ARM64_CALLXS_R_INDEX_PRE),
      SNAP(8, 0, ARM64_CALLXS_R_CALL_BODY) },
    { SNAP(4, 0, ARM64_CALLXS_R_INDEX_PRE),
      SNAP(5, SNAP_NORESTORE, ARM64_CALLXS_R_LIMIT),
      SNAP(6, 0, ARM64_CALLXS_K_ONE),
      SNAP(7, 0, ARM64_CALLXS_R_INDEX_PRE) },
    { 0 }
  };
  uint64_t xsave_pcbase = 0;
  const BCIns *meta_pc;
  GCobj *meta;
  IRIns kmeta;
  uintptr_t proto, expected;
  MSize snapno, n;
  if (ir == NULL || snap == NULL || snapmap == NULL || proto_bc == NULL ||
	nsnap != 15 || nsnapmap != 97 || proto_sizebc != 16 ||
	(profile != ARM64_CALLXS_PROFILE_I32 &&
	 profile != ARM64_CALLXS_PROFILE_DOUBLE))
    return 0;
  kmeta = ir_load_acq(&ir[ARM64_CALLXS_K_META]);
  meta = ir_kgc_load_acq(&ir[ARM64_CALLXS_K_META]);
  if (kmeta.o != IR_KGC || kmeta.t.irt != IRT_FUNC || meta == NULL ||
	!checkptrGC(meta) || meta->gch.gct != (uint32_t)~LJ_TFUNC ||
	lj_func_ffid_acq(gco2func(meta)) != FF_ffi_meta___call)
    return 0;
  meta_pc = mref_acq(gco2func(meta)->c.pc, const BCIns);
  if (meta_pc == NULL ||
	((uintptr_t)(const void *)meta_pc & (sizeof(BCIns)-1u)) != 0 ||
	bc_op((BCIns)la_load32_acq((const uint32_t *)meta_pc)) != BC_FUNCC)
    return 0;
  proto = (uintptr_t)(const void *)proto_bc;
  for (snapno = 0; snapno < 15; snapno++) {
    const SnapShot *s = &snap[snapno];
    SnapEntry pcraw[1+LJ_FR2];
    MSize nextofs = snapno+1u < nsnap ?
	snap_mapofs_acq(&snap[snapno+1u]) : nsnapmap;
    uint64_t pcbase;
    if (snap_ref_acq(s) != arm64_callxs_profile_ref(profile, refs[snapno]) ||
	snap_mapofs_acq(s) != mapofs[snapno] ||
	snap_nent_acq(s) != nent[snapno] ||
	snap_nslots_acq(s) != nslots[snapno] ||
	snap_topslot_acq(s) != topslot[snapno] ||
	snap_count_acq(s) != counts[snapno] ||
	nextofs != mapofs[snapno]+nent[snapno]+1u+LJ_FR2)
      return 0;
    for (n = 0; n < nent[snapno]; n++) {
      SnapEntry entry = entries[snapno][n];
      IRRef ref = snap_ref(entry);
      if (ref >= REF_FIRST)
	entry = (entry & 0xffff0000u) |
		arm64_callxs_profile_ref(profile, ref);
      if (snapentry_acq(&snapmap[mapofs[snapno]+n]) != entry)
	return 0;
    }
    for (n = 0; n < 1u+LJ_FR2; n++)
      pcraw[n] = snapentry_acq(
	&snapmap[mapofs[snapno]+nent[snapno]+n]);
    LJ_STATIC_ASSERT(sizeof(pcraw) == sizeof(pcbase));
    memcpy(&pcbase, pcraw, sizeof(pcbase));
    if (snapno == 3 || snapno == 4 || snapno == 11) {
      if ((uint8_t)pcbase != 8 ||
	  (uintptr_t)(pcbase >> 8) != (uintptr_t)(const void *)meta_pc ||
	  (xsave_pcbase != 0 && pcbase != xsave_pcbase))
	return 0;
      xsave_pcbase = pcbase;
    } else {
      if (pcpos[snapno] >= proto_sizebc ||
	  (uintptr_t)pcpos[snapno] >
	    (UINTPTR_MAX-proto)/sizeof(BCIns))
	return 0;
      expected = proto+(uintptr_t)pcpos[snapno]*sizeof(BCIns);
      if ((uint8_t)pcbase != 0 || (uintptr_t)(pcbase >> 8) != expected)
	return 0;
    }
  }
  return xsave_pcbase != 0;
}

static int arm64_callxs_ptr_kint64(const IRIns *ir, IRRef ref,
	uint64_t value)
{
  IRIns k = ir_load_acq(&ir[ref]);
  return k.o == IR_KINT64 && k.t.irt == IRT_I64 && k.op12 == 0 &&
	 ir_load_acq(&ir[ref+1u]).tv.u64 == value;
}

static int arm64_callxs_ptr_constant_shape(const IRIns *ir, IRRef nk,
	const GCtrace *owner, const BCIns *proto_bc, MSize proto_sizebc)
{
  IRIns k;
  GCobj *o;
  IRRef ref;
  if (ir == NULL || proto_bc == NULL || proto_sizebc != 13 ||
	(uintptr_t)(const void *)proto_bc >
	  UINTPTR_MAX-9u*sizeof(BCIns) ||
	nk != ARM64_CALLXS_PTR_K_ZERO)
    return 0;
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    k = ir_load_acq(&ir[ref]);
    if (k.o != IR_KPRI || k.t.irt != (uint8_t)(REF_NIL-ref) ||
	k.op12 != 0)
      return 0;
  }
  k = ir_load_acq(&ir[ARM64_CALLXS_PTR_K_ZERO]);
  if (k.o != IR_KINT || k.t.irt != IRT_INT || k.i != 0)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_PTR_K_TRACE]);
  if (owner == NULL) {
    if (k.o != IR_KNUM || k.t.irt != IRT_P64 || k.op12 != 0)
      return 0;
  } else {
    if (k.o != IR_KGC || k.t.irt != IRT_P64 || k.op12 != 0 ||
	ir_kgc_load_acq(&ir[ARM64_CALLXS_PTR_K_TRACE]) != obj2gco(owner))
      return 0;
  }
  if (!arm64_callxs_ptr_kint64(ir, ARM64_CALLXS_PTR_K_PAYLOAD_OFS,
	(uint64_t)sizeof(GCcdata)))
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_PTR_K_BOX_CTYPE]);
  if (k.o != IR_KINT || k.t.irt != IRT_INT || k.i != CTID_P_CCHAR)
    return 0;
  if (!arm64_callxs_ptr_kint64(ir, ARM64_CALLXS_PTR_K_STRING_OFS,
	(uint64_t)sizeof(GCstr)))
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_PTR_K_CTYPE]);
  if (k.o != IR_KINT || k.t.irt != IRT_INT || k.i <= 0 ||
	(uint32_t)k.i >= CTID_MAX)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_PTR_K_FTSZ]);
  if (k.o != IR_KNUM || k.t.irt != IRT_NUM || k.op12 != 0 ||
	ir_load_acq(&ir[ARM64_CALLXS_PTR_K_FTSZ+1u]).tv.u64 !=
	  (uint64_t)(uintptr_t)(const void *)(proto_bc+9))
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_PTR_K_META]);
  if (k.o != IR_KGC || k.t.irt != IRT_FUNC || k.op12 != 0)
    return 0;
  o = ir_kgc_load_acq(&ir[ARM64_CALLXS_PTR_K_META]);
  if (o == NULL || !checkptrGC(o) || o->gch.gct != (uint32_t)~LJ_TFUNC ||
	lj_func_ffid_acq(gco2func(o)) != FF_ffi_meta___call)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_PTR_K_KEY]);
  if (k.o != IR_KGC || k.t.irt != IRT_STR || k.op12 != 0)
    return 0;
  o = ir_kgc_load_acq(&ir[ARM64_CALLXS_PTR_K_KEY]);
  if (o == NULL || !checkptrGC(o) || o->gch.gct != (uint32_t)~LJ_TSTR ||
	o->str.len != 6 || memcmp(strdata(&o->str), "__call", 6) != 0)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_PTR_K_TABLE]);
  if (k.o != IR_KGC || k.t.irt != IRT_TAB || k.op12 != 0)
    return 0;
  o = ir_kgc_load_acq(&ir[ARM64_CALLXS_PTR_K_TABLE]);
  if (o == NULL || !checkptrGC(o) || o->gch.gct != (uint32_t)~LJ_TTAB)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_PTR_K_LIMITMAX]);
  if (k.o != IR_KINT || k.t.irt != IRT_INT || k.i != INT32_MAX-1)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_PTR_K_ONE]);
  return k.o == IR_KINT && k.t.irt == IRT_INT && k.i == 1;
}

static int arm64_callxs_ptr_signature(const jit_State *J, const IRIns *ir)
{
  CTState *cts;
  CType ctf, field;
  CTInfo info, finfo;
  CTypeID id, fid;
  IRIns k;
  if (J == NULL || ir == NULL)
    return 0;
  k = ir_load_acq(&ir[ARM64_CALLXS_PTR_K_CTYPE]);
  if (k.o != IR_KINT || k.i <= 0)
    return 0;
  id = (CTypeID)k.i;
  cts = ctype_ctsG(J2G(J));
  if (lj_ctype_snapshot(cts, id, &ctf) <= 0)
    return 0;
  info = ctype_info_acq(&ctf);
  if (!ctype_isfunc(info) || ctype_size_acq(&ctf) != 1 ||
	(info & CTF_VARARG) != 0 || ctype_cconv(info) != CTCC_CDECL ||
	ctype_cid(info) != CTID_P_CCHAR)
    return 0;
  fid = ctype_sib_acq(&ctf);
  if (fid == 0 || lj_ctype_snapshot(cts, fid, &field) <= 0)
    return 0;
  finfo = ctype_info_acq(&field);
  return ctype_isfield(finfo) && ctype_cid(finfo) == CTID_P_CCHAR &&
	 ctype_sib_acq(&field) == 0;
}

static int arm64_callxs_ptr_bytecode(const BCIns *bc, MSize sizebc,
	MSize framesize, MSize numparams, BCIns startins,
	const GCtrace *owner)
{
  BCIns ins[13];
  BCOp startop;
  MSize i;
  if (bc == NULL || sizebc != 13 || framesize != 11 || numparams != 3 ||
	((uintptr_t)(const void *)bc & (sizeof(BCIns)-1u)) != 0 ||
	(uintptr_t)(const void *)bc > UINTPTR_MAX-12u*sizeof(BCIns))
    return 0;
  for (i = 0; i < 13; i++)
    ins[i] = arm64_ir_bc_acq((uintptr_t)(const void *)bc, i);
  startop = bc_op(ins[10]);
  if (owner == NULL) {
    if (ins[10] != startins || startop != BC_FORL)
      return 0;
  } else {
    if ((startop == BC_FORL && ins[10] != startins) ||
	(startop != BC_FORL &&
	 (startop != BC_JFORL ||
	  bc_d(ins[10]) != trace_traceno_acq(owner))))
      return 0;
    if (bc_a(ins[10]) != bc_a(startins))
      return 0;
  }
  return bc_op(ins[0]) == BC_FUNCF && bc_a(ins[0]) == 11 &&
	 bc_d(ins[0]) == 0 &&
	 bc_op(ins[1]) == BC_KPRI && bc_a(ins[1]) == 3 &&
	 bc_d(ins[1]) == 0 &&
	 bc_op(ins[2]) == BC_KSHORT && bc_a(ins[2]) == 4 &&
	 bc_d(ins[2]) == 1 &&
	 bc_op(ins[3]) == BC_CGET && bc_a(ins[3]) == 5 &&
	 bc_d(ins[3]) == 1 &&
	 bc_op(ins[4]) == BC_KSHORT && bc_a(ins[4]) == 6 &&
	 bc_d(ins[4]) == 1 &&
	 bc_op(ins[5]) == BC_FORI && bc_a(ins[5]) == 4 &&
	 bc_j(ins[5]) == 5 &&
	 bc_op(ins[6]) == BC_CGET && bc_a(ins[6]) == 8 &&
	 bc_d(ins[6]) == 0 &&
	 bc_op(ins[7]) == BC_CGET && bc_a(ins[7]) == 10 &&
	 bc_d(ins[7]) == 2 &&
	 bc_op(ins[8]) == BC_CALL && bc_a(ins[8]) == 8 &&
	 bc_b(ins[8]) == 2 && bc_c(ins[8]) == 2 &&
	 bc_op(ins[9]) == BC_CSET && bc_a(ins[9]) == 3 &&
	 bc_d(ins[9]) == 8 &&
	 bc_a(ins[10]) == 4 && bc_j(startins) == -5 &&
	 bc_op(ins[11]) == BC_CGET && bc_a(ins[11]) == 4 &&
	 bc_d(ins[11]) == 3 &&
	 bc_op(ins[12]) == BC_RET1 && bc_a(ins[12]) == 4 &&
	 bc_d(ins[12]) == 2;
}

static int arm64_callxs_ptr_ir_shape(const IRIns *ir)
{
#define ARM64_CALLXS_PTR_INS(ref, op, type, left, right) \
  arm64_callxs_ins(ir, (ref), (op), (type), (left), (right))
  return ARM64_CALLXS_PTR_INS(REF_BASE, IR_BASE, IRT_PGC, 0, 0) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LIMIT, IR_SLOAD, IRT_INT,
	7, IRSLOAD_READONLY|IRSLOAD_INHERIT) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LIMIT_GUARD, IR_LE,
	IRT_INT|IRT_GUARD, ARM64_CALLXS_PTR_R_LIMIT,
	ARM64_CALLXS_PTR_K_LIMITMAX) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_INDEX, IR_SLOAD,
	IRT_INT|IRT_GUARD, 6, IRSLOAD_TYPECHECK|IRSLOAD_INHERIT) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_FUNC, IR_SLOAD,
	IRT_CDATA|IRT_GUARD, 2, IRSLOAD_TYPECHECK) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_STRING, IR_SLOAD,
	IRT_STR|IRT_GUARD, 4, IRSLOAD_TYPECHECK) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_MT, IR_FLOAD, IRT_TAB,
	REF_NIL, GG_OFS(g.gcroot[GCROOT_BASEMT+(~LJ_TCDATA)]) >> 2) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_MT_GUARD, IR_EQ,
	IRT_TAB|IRT_GUARD, ARM64_CALLXS_PTR_R_MT,
	ARM64_CALLXS_PTR_K_TABLE) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_TABLE_ROOT, IR_TMPREF,
	IRT_PGC, ARM64_CALLXS_PTR_K_TABLE,
	IRTMPREF_IN1|IRTMPREF_OUT1) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_KEY_ROOT, IR_TMPREF,
	IRT_PGC, ARM64_CALLXS_PTR_K_KEY, IRTMPREF_IN2) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LOOKUP_ARGS, IR_CARG,
	IRT_NIL, ARM64_CALLXS_PTR_R_TABLE_ROOT,
	ARM64_CALLXS_PTR_R_KEY_ROOT) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LOOKUP_OUT, IR_CARG,
	IRT_NIL, ARM64_CALLXS_PTR_R_LOOKUP_ARGS,
	ARM64_CALLXS_PTR_R_TABLE_ROOT) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LOOKUP, IR_CALLS,
	IRT_P64|IRT_GUARD, ARM64_CALLXS_PTR_R_LOOKUP_OUT,
	IRCALL_lj_tab_gettv_rooted) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_MOBJ, IR_VLOAD,
	IRT_FUNC|IRT_GUARD, ARM64_CALLXS_PTR_R_LOOKUP, 0) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_MOBJ_GUARD, IR_EQ,
	IRT_FUNC|IRT_GUARD, ARM64_CALLXS_PTR_R_MOBJ,
	ARM64_CALLXS_PTR_K_META) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_CTYPE, IR_FLOAD, IRT_U16,
	ARM64_CALLXS_PTR_R_FUNC, IRFL_CDATA_CTYPEID) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_CTYPE_GUARD, IR_EQ,
	IRT_INT|IRT_GUARD, ARM64_CALLXS_PTR_R_CTYPE,
	ARM64_CALLXS_PTR_K_CTYPE) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_FUNCPTR, IR_FLOAD, IRT_P64,
	ARM64_CALLXS_PTR_R_FUNC, IRFL_CDATA_PTR) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_STRING_PTR, IR_ADD, IRT_P64,
	ARM64_CALLXS_PTR_R_STRING, ARM64_CALLXS_PTR_K_STRING_OFS) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_BOX_PRE, IR_CNEW,
	IRT_CDATA|IRT_GUARD|IRT_ISPHI, ARM64_CALLXS_PTR_K_BOX_CTYPE,
	REF_NIL) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_PAYLOAD_PRE, IR_ADD, IRT_P64,
	ARM64_CALLXS_PTR_R_BOX_PRE, ARM64_CALLXS_PTR_K_PAYLOAD_OFS) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_XSAVE_PRE, IR_XSAVE,
	IRT_NIL, 0, 0) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_ENTER_ARGS, IR_CARG,
	IRT_NIL, ARM64_CALLXS_PTR_K_TRACE,
	ARM64_CALLXS_PTR_R_FUNCPTR) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_ENTER_ROOT_PRE, IR_CARG,
	IRT_NIL, ARM64_CALLXS_PTR_R_ENTER_ARGS,
	ARM64_CALLXS_PTR_R_BOX_PRE) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_ENTER_PRE, IR_CALLS,
	IRT_INT, ARM64_CALLXS_PTR_R_ENTER_ROOT_PRE,
	IRCALL_lj_ffi_native_trace_enter) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_ENTER_GUARD_PRE, IR_NE,
	IRT_INT|IRT_GUARD, ARM64_CALLXS_PTR_R_ENTER_PRE,
	ARM64_CALLXS_PTR_K_ZERO) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_CALL_PRE, IR_CALLXS,
	IRT_P64, ARM64_CALLXS_PTR_R_STRING_PTR,
	ARM64_CALLXS_PTR_R_FUNCPTR) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_STORE_PRE, IR_XSTORE,
	IRT_P64, ARM64_CALLXS_PTR_R_PAYLOAD_PRE,
	ARM64_CALLXS_PTR_R_CALL_PRE) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LEAVE_PRE, IR_CALLS,
	IRT_INT|IRT_GUARD, REF_NIL, IRCALL_lj_ffi_native_trace_leave) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LEAVE_GUARD_PRE, IR_EQ,
	IRT_INT|IRT_GUARD, ARM64_CALLXS_PTR_R_LEAVE_PRE,
	ARM64_CALLXS_PTR_K_ZERO) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_RESULT_PRE, IR_SLOAD,
	IRT_CDATA|IRT_GUARD, 5, IRSLOAD_TYPECHECK) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_INDEX_PRE, IR_ADD,
	IRT_INT|IRT_ISPHI, ARM64_CALLXS_PTR_R_INDEX,
	ARM64_CALLXS_PTR_K_ONE) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_BOUND_GUARD_PRE, IR_LE,
	IRT_INT|IRT_GUARD, ARM64_CALLXS_PTR_R_INDEX_PRE,
	ARM64_CALLXS_PTR_R_LIMIT) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LOOP, IR_LOOP,
	IRT_NIL|IRT_GUARD, 0, 0) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_XPOLL, IR_XPOLL,
	IRT_NIL|IRT_GUARD, 1, 0) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_MT_BODY, IR_FLOAD, IRT_TAB,
	REF_NIL, GG_OFS(g.gcroot[GCROOT_BASEMT+(~LJ_TCDATA)]) >> 2) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_MT_GUARD_BODY, IR_EQ,
	IRT_TAB|IRT_GUARD, ARM64_CALLXS_PTR_R_MT_BODY,
	ARM64_CALLXS_PTR_K_TABLE) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_TABLE_ROOT_BODY, IR_TMPREF,
	IRT_PGC, ARM64_CALLXS_PTR_K_TABLE,
	IRTMPREF_IN1|IRTMPREF_OUT1) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_KEY_ROOT_BODY, IR_TMPREF,
	IRT_PGC, ARM64_CALLXS_PTR_K_KEY, IRTMPREF_IN2) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LOOKUP_ARGS_BODY, IR_CARG,
	IRT_NIL, ARM64_CALLXS_PTR_R_TABLE_ROOT_BODY,
	ARM64_CALLXS_PTR_R_KEY_ROOT_BODY) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LOOKUP_OUT_BODY, IR_CARG,
	IRT_NIL, ARM64_CALLXS_PTR_R_LOOKUP_ARGS_BODY,
	ARM64_CALLXS_PTR_R_TABLE_ROOT_BODY) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LOOKUP_BODY, IR_CALLS,
	IRT_P64|IRT_GUARD, ARM64_CALLXS_PTR_R_LOOKUP_OUT_BODY,
	IRCALL_lj_tab_gettv_rooted) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_MOBJ_BODY, IR_VLOAD,
	IRT_FUNC|IRT_GUARD, ARM64_CALLXS_PTR_R_LOOKUP_BODY, 0) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_MOBJ_GUARD_BODY, IR_EQ,
	IRT_FUNC|IRT_GUARD, ARM64_CALLXS_PTR_R_MOBJ_BODY,
	ARM64_CALLXS_PTR_K_META) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_BOX_BODY, IR_CNEW,
	IRT_CDATA|IRT_GUARD|IRT_ISPHI, ARM64_CALLXS_PTR_K_BOX_CTYPE,
	REF_NIL) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_PAYLOAD_BODY, IR_ADD,
	IRT_P64, ARM64_CALLXS_PTR_R_BOX_BODY,
	ARM64_CALLXS_PTR_K_PAYLOAD_OFS) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_XSAVE_BODY, IR_XSAVE,
	IRT_NIL, 0, 0) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_ENTER_ROOT_BODY, IR_CARG,
	IRT_NIL, ARM64_CALLXS_PTR_R_ENTER_ARGS,
	ARM64_CALLXS_PTR_R_BOX_BODY) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_ENTER_BODY, IR_CALLS,
	IRT_INT, ARM64_CALLXS_PTR_R_ENTER_ROOT_BODY,
	IRCALL_lj_ffi_native_trace_enter) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_ENTER_GUARD_BODY, IR_NE,
	IRT_INT|IRT_GUARD, ARM64_CALLXS_PTR_R_ENTER_BODY,
	ARM64_CALLXS_PTR_K_ZERO) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_CALL_BODY, IR_CALLXS,
	IRT_P64, ARM64_CALLXS_PTR_R_STRING_PTR,
	ARM64_CALLXS_PTR_R_FUNCPTR) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_STORE_BODY, IR_XSTORE,
	IRT_P64, ARM64_CALLXS_PTR_R_PAYLOAD_BODY,
	ARM64_CALLXS_PTR_R_CALL_BODY) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LEAVE_BODY, IR_CALLS,
	IRT_INT|IRT_GUARD, REF_NIL, IRCALL_lj_ffi_native_trace_leave) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_LEAVE_GUARD_BODY, IR_EQ,
	IRT_INT|IRT_GUARD, ARM64_CALLXS_PTR_R_LEAVE_BODY,
	ARM64_CALLXS_PTR_K_ZERO) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_INDEX_BODY, IR_ADD,
	IRT_INT|IRT_ISPHI, ARM64_CALLXS_PTR_R_INDEX_PRE,
	ARM64_CALLXS_PTR_K_ONE) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_BOUND_GUARD_BODY, IR_LE,
	IRT_INT|IRT_GUARD, ARM64_CALLXS_PTR_R_INDEX_BODY,
	ARM64_CALLXS_PTR_R_LIMIT) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_INDEX_PHI, IR_PHI,
	IRT_INT, ARM64_CALLXS_PTR_R_INDEX_PRE,
	ARM64_CALLXS_PTR_R_INDEX_BODY) &&
    ARM64_CALLXS_PTR_INS(ARM64_CALLXS_PTR_R_RESULT_PHI, IR_PHI,
	IRT_CDATA, ARM64_CALLXS_PTR_R_BOX_PRE,
	ARM64_CALLXS_PTR_R_BOX_BODY);
#undef ARM64_CALLXS_PTR_INS
}

static int arm64_callxs_ptr_snapshots(const IRIns *ir,
	const SnapShot *snap, const SnapEntry *snapmap, MSize nsnap,
	MSize nsnapmap, const BCIns *proto_bc, MSize proto_sizebc)
{
  static const IRRef refs[18] = {
    ARM64_CALLXS_PTR_R_LIMIT, ARM64_CALLXS_PTR_R_MT,
    ARM64_CALLXS_PTR_R_MOBJ, ARM64_CALLXS_PTR_R_CTYPE,
    ARM64_CALLXS_PTR_R_BOX_PRE, ARM64_CALLXS_PTR_R_XSAVE_PRE,
    ARM64_CALLXS_PTR_R_ENTER_ARGS, ARM64_CALLXS_PTR_R_LEAVE_PRE,
    ARM64_CALLXS_PTR_R_RESULT_PRE,
    ARM64_CALLXS_PTR_R_BOUND_GUARD_PRE, ARM64_CALLXS_PTR_R_LOOP,
    ARM64_CALLXS_PTR_R_MT_BODY, ARM64_CALLXS_PTR_R_MOBJ_BODY,
    ARM64_CALLXS_PTR_R_BOX_BODY, ARM64_CALLXS_PTR_R_XSAVE_BODY,
    ARM64_CALLXS_PTR_R_ENTER_ROOT_BODY,
    ARM64_CALLXS_PTR_R_LEAVE_BODY,
    ARM64_CALLXS_PTR_R_BOUND_GUARD_BODY
  };
  static const uint16_t mapofs[18] = {
    0, 2, 9, 16, 25, 34, 44, 53, 59,
    65, 68, 75, 84, 93, 104, 116, 127, 135
  };
  static const uint8_t nent[18] = {
    0, 5, 5, 7, 7, 8, 7, 4, 4,
    1, 5, 7, 7, 9, 10, 9, 6, 1
  };
  static const uint8_t nslots[18] = {
    2, 13, 13, 14, 14, 15, 14, 11, 11,
    6, 10, 13, 13, 14, 15, 14, 11, 6
  };
  static const uint8_t topslot[18] = {
    11, 11, 11, 13, 13, 13, 13, 11, 11,
    11, 11, 11, 11, 13, 13, 13, 11, 11
  };
  static const uint8_t counts[18] = {
    0, 0, 0, 0, SNAPCOUNT_DONE, SNAPCOUNT_DONE,
    SNAPCOUNT_DONE, SNAPCOUNT_DONE, 0, 0, 0, 0, 0,
    SNAPCOUNT_DONE, SNAPCOUNT_DONE, SNAPCOUNT_DONE,
    SNAPCOUNT_DONE, 0
  };
  static const uint8_t pcpos[18] = {
    6, 8, 8, 0, 0, 0, 0, 9, 9,
    11, 6, 8, 8, 0, 0, 0, 9, 11
  };
  static const SnapEntry entries[18][10] = {
    { 0 },
    { SNAP(6, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_INDEX),
      SNAP(7, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(10, 0, ARM64_CALLXS_PTR_R_FUNC),
      SNAP(12, 0, ARM64_CALLXS_PTR_R_STRING) },
    { SNAP(6, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_INDEX),
      SNAP(7, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(10, 0, ARM64_CALLXS_PTR_R_FUNC),
      SNAP(12, 0, ARM64_CALLXS_PTR_R_STRING) },
    { SNAP(6, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_INDEX),
      SNAP(7, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(10, 0, ARM64_CALLXS_PTR_K_META),
      SNAP(11, SNAP_FRAME, ARM64_CALLXS_PTR_K_FTSZ),
      SNAP(12, 0, ARM64_CALLXS_PTR_R_FUNC),
      SNAP(13, 0, ARM64_CALLXS_PTR_R_STRING) },
    { SNAP(6, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_INDEX),
      SNAP(7, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(10, 0, ARM64_CALLXS_PTR_K_META),
      SNAP(11, SNAP_FRAME, ARM64_CALLXS_PTR_K_FTSZ),
      SNAP(12, 0, ARM64_CALLXS_PTR_R_FUNC),
      SNAP(13, 0, ARM64_CALLXS_PTR_R_STRING) },
    { SNAP(6, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_INDEX),
      SNAP(7, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(10, 0, ARM64_CALLXS_PTR_K_META),
      SNAP(11, SNAP_FRAME, ARM64_CALLXS_PTR_K_FTSZ),
      SNAP(12, 0, ARM64_CALLXS_PTR_R_FUNC),
      SNAP(13, 0, ARM64_CALLXS_PTR_R_STRING),
      SNAP(14, 0, ARM64_CALLXS_PTR_R_BOX_PRE) },
    { SNAP(6, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_INDEX),
      SNAP(7, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(10, 0, ARM64_CALLXS_PTR_K_META),
      SNAP(11, SNAP_FRAME, ARM64_CALLXS_PTR_K_FTSZ),
      SNAP(12, 0, ARM64_CALLXS_PTR_R_FUNC),
      SNAP(13, 0, ARM64_CALLXS_PTR_R_STRING) },
    { SNAP(6, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_INDEX),
      SNAP(7, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(10, 0, ARM64_CALLXS_PTR_R_BOX_PRE) },
    { SNAP(6, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_INDEX),
      SNAP(7, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(10, 0, ARM64_CALLXS_PTR_R_BOX_PRE) },
    { SNAP(5, 0, ARM64_CALLXS_PTR_R_BOX_PRE) },
    { SNAP(5, 0, ARM64_CALLXS_PTR_R_BOX_PRE),
      SNAP(6, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(7, SNAP_NORESTORE, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(9, 0, ARM64_CALLXS_PTR_R_INDEX_PRE) },
    { SNAP(5, 0, ARM64_CALLXS_PTR_R_BOX_PRE),
      SNAP(6, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(7, 0, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(9, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(10, 0, ARM64_CALLXS_PTR_R_FUNC),
      SNAP(12, 0, ARM64_CALLXS_PTR_R_STRING) },
    { SNAP(5, 0, ARM64_CALLXS_PTR_R_BOX_PRE),
      SNAP(6, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(7, 0, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(9, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(10, 0, ARM64_CALLXS_PTR_R_FUNC),
      SNAP(12, 0, ARM64_CALLXS_PTR_R_STRING) },
    { SNAP(5, 0, ARM64_CALLXS_PTR_R_BOX_PRE),
      SNAP(6, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(7, 0, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(9, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(10, 0, ARM64_CALLXS_PTR_K_META),
      SNAP(11, SNAP_FRAME, ARM64_CALLXS_PTR_K_FTSZ),
      SNAP(12, 0, ARM64_CALLXS_PTR_R_FUNC),
      SNAP(13, 0, ARM64_CALLXS_PTR_R_STRING) },
    { SNAP(5, 0, ARM64_CALLXS_PTR_R_BOX_PRE),
      SNAP(6, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(7, 0, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(9, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(10, 0, ARM64_CALLXS_PTR_K_META),
      SNAP(11, SNAP_FRAME, ARM64_CALLXS_PTR_K_FTSZ),
      SNAP(12, 0, ARM64_CALLXS_PTR_R_FUNC),
      SNAP(13, 0, ARM64_CALLXS_PTR_R_STRING),
      SNAP(14, 0, ARM64_CALLXS_PTR_R_BOX_BODY) },
    { SNAP(5, 0, ARM64_CALLXS_PTR_R_BOX_PRE),
      SNAP(6, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(7, 0, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(9, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(10, 0, ARM64_CALLXS_PTR_K_META),
      SNAP(11, SNAP_FRAME, ARM64_CALLXS_PTR_K_FTSZ),
      SNAP(12, 0, ARM64_CALLXS_PTR_R_FUNC),
      SNAP(13, 0, ARM64_CALLXS_PTR_R_STRING) },
    { SNAP(5, 0, ARM64_CALLXS_PTR_R_BOX_PRE),
      SNAP(6, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(7, 0, ARM64_CALLXS_PTR_R_LIMIT),
      SNAP(8, 0, ARM64_CALLXS_PTR_K_ONE),
      SNAP(9, 0, ARM64_CALLXS_PTR_R_INDEX_PRE),
      SNAP(10, 0, ARM64_CALLXS_PTR_R_BOX_BODY) },
    { SNAP(5, 0, ARM64_CALLXS_PTR_R_BOX_BODY) }
  };
  uint64_t xsave_pcbase = 0;
  const BCIns *meta_pc;
  GCobj *meta;
  IRIns kmeta;
  uintptr_t proto, expected;
  MSize snapno, n;
  if (ir == NULL || snap == NULL || snapmap == NULL || proto_bc == NULL ||
	nsnap != 18 || nsnapmap != 138 || proto_sizebc != 13)
    return 0;
  kmeta = ir_load_acq(&ir[ARM64_CALLXS_PTR_K_META]);
  meta = ir_kgc_load_acq(&ir[ARM64_CALLXS_PTR_K_META]);
  if (kmeta.o != IR_KGC || kmeta.t.irt != IRT_FUNC || meta == NULL ||
	!checkptrGC(meta) || meta->gch.gct != (uint32_t)~LJ_TFUNC ||
	lj_func_ffid_acq(gco2func(meta)) != FF_ffi_meta___call)
    return 0;
  meta_pc = mref_acq(gco2func(meta)->c.pc, const BCIns);
  if (meta_pc == NULL ||
	((uintptr_t)(const void *)meta_pc & (sizeof(BCIns)-1u)) != 0 ||
	bc_op((BCIns)la_load32_acq((const uint32_t *)meta_pc)) != BC_FUNCC)
    return 0;
  proto = (uintptr_t)(const void *)proto_bc;
  for (snapno = 0; snapno < 18; snapno++) {
    const SnapShot *s = &snap[snapno];
    SnapEntry pcraw[1+LJ_FR2];
    MSize nextofs = snapno+1u < nsnap ?
	snap_mapofs_acq(&snap[snapno+1u]) : nsnapmap;
    uint64_t pcbase;
    if (snap_ref_acq(s) != refs[snapno] ||
	snap_mapofs_acq(s) != mapofs[snapno] ||
	snap_nent_acq(s) != nent[snapno] ||
	snap_nslots_acq(s) != nslots[snapno] ||
	snap_topslot_acq(s) != topslot[snapno] ||
	snap_count_acq(s) != counts[snapno] ||
	nextofs != mapofs[snapno]+nent[snapno]+1u+LJ_FR2)
      return 0;
    for (n = 0; n < nent[snapno]; n++)
      if (snapentry_acq(&snapmap[mapofs[snapno]+n]) !=
	  entries[snapno][n])
	return 0;
    for (n = 0; n < 1u+LJ_FR2; n++)
      pcraw[n] = snapentry_acq(
	&snapmap[mapofs[snapno]+nent[snapno]+n]);
    LJ_STATIC_ASSERT(sizeof(pcraw) == sizeof(pcbase));
    memcpy(&pcbase, pcraw, sizeof(pcbase));
    if ((snapno >= 3 && snapno <= 6) ||
	(snapno >= 13 && snapno <= 15)) {
      if ((uint8_t)pcbase != 10 ||
	  (uintptr_t)(pcbase >> 8) != (uintptr_t)(const void *)meta_pc ||
	  (xsave_pcbase != 0 && pcbase != xsave_pcbase))
	return 0;
      xsave_pcbase = pcbase;
    } else {
      if (pcpos[snapno] >= proto_sizebc ||
	  (uintptr_t)pcpos[snapno] >
	    (UINTPTR_MAX-proto)/sizeof(BCIns))
	return 0;
      expected = proto+(uintptr_t)pcpos[snapno]*sizeof(BCIns);
      if ((uint8_t)pcbase != 0 ||
	  (uintptr_t)(pcbase >> 8) != expected)
	return 0;
    }
  }
  return xsave_pcbase != 0;
}

static int arm64_ir_callxs_shape(const jit_State *J, const GCtrace *T,
	const GCproto *pt, LJArm64IRReject *reject)
{
  const GCtrace *owner = J->curfinal;
  LJArm64CallXSProfile profile;
  IRRef callpre, loopref, semantic_nins, xsavepre;
  if (bc_op(T->startins) != BC_FORL)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_R_CALL_PRE, IR_CALLXS, 1);
  if (T->nk != ARM64_CALLXS_K_ZERO)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_K_ZERO, IR_KINT, 3);
  if (J->ktrace != ARM64_CALLXS_K_TRACE)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_K_TRACE, IR_KGC, 4);
  if (!arm64_callxs_bytecode(proto_bc(pt), pt->sizebc, pt->framesize,
	pt->numparams, T->startins, NULL))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_R_LOOP, IR_LOOP, 9);
  if (trace_startpc_acq((GCtrace *)T) != proto_bc(pt)+13)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_R_LOOP, IR_LOOP, 6);
  if (!arm64_callxs_constant_shape(T->ir, T->nk, owner, proto_bc(pt),
	pt->sizebc))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_K_TRACE, IR_KGC, 7);
  profile = arm64_callxs_signature(J, T->ir);
  if (profile == ARM64_CALLXS_PROFILE_NONE)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_K_CTYPE, IR_KINT, 8);
  semantic_nins = profile == ARM64_CALLXS_PROFILE_DOUBLE ?
	ARM64_CALLXS_D_SEMANTIC_NINS : ARM64_CALLXS_SEMANTIC_NINS;
  callpre = arm64_callxs_profile_ref(profile, ARM64_CALLXS_R_CALL_PRE);
  loopref = arm64_callxs_profile_ref(profile, ARM64_CALLXS_R_LOOP);
  xsavepre = arm64_callxs_profile_ref(profile, ARM64_CALLXS_R_XSAVE_PRE);
  if (T->nins != semantic_nins)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	callpre, IR_CALLXS, 2);
  if (J->loopref != loopref)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	loopref, IR_LOOP, 5);
  if (!arm64_callxs_ir_shape(T->ir, profile))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	callpre, IR_CALLXS, 10);
  if (!arm64_callxs_snapshots(T->ir, T->snap, T->snapmap, T->nsnap,
	T->nsnapmap, proto_bc(pt), pt->sizebc, profile))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
	xsavepre, IR_XSAVE, 11);
  return 1;
}

static int arm64_ir_callxs_ptr_shape(const jit_State *J, const GCtrace *T,
	const GCproto *pt, LJArm64IRReject *reject)
{
  const GCtrace *owner = J->curfinal;
  if (bc_op(T->startins) != BC_FORL)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_PTR_R_CALL_PRE, IR_CALLXS, 1);
  if (T->nk != ARM64_CALLXS_PTR_K_ZERO)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_PTR_K_ZERO, IR_KINT, 3);
  if (J->ktrace != ARM64_CALLXS_PTR_K_TRACE)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_PTR_K_TRACE, IR_KGC, 4);
  if (!arm64_callxs_ptr_bytecode(proto_bc(pt), pt->sizebc, pt->framesize,
	pt->numparams, T->startins, NULL))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_PTR_R_LOOP, IR_LOOP, 9);
  if (pt->sizeuv != 0 || pt->sizekn != 0 || pt->sizekgc != 0 ||
	pt->flags2 != PROTO2_CELLOPS ||
	trace_startpc_acq((GCtrace *)T) != proto_bc(pt)+10)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_PTR_R_LOOP, IR_LOOP, 6);
  if (!arm64_callxs_ptr_constant_shape(T->ir, T->nk, owner, proto_bc(pt),
	pt->sizebc))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_PTR_K_TRACE, IR_KGC, 7);
  if (!arm64_callxs_ptr_signature(J, T->ir))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_PTR_K_CTYPE, IR_KINT, 8);
  if (T->nins != ARM64_CALLXS_PTR_SEMANTIC_NINS)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_PTR_R_CALL_PRE, IR_CALLXS, 2);
  if (J->loopref != ARM64_CALLXS_PTR_R_LOOP)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_PTR_R_LOOP, IR_LOOP, 5);
  if (!arm64_callxs_ptr_ir_shape(T->ir))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	ARM64_CALLXS_PTR_R_CALL_PRE, IR_CALLXS, 10);
  if (!arm64_callxs_ptr_snapshots(T->ir, T->snap, T->snapmap, T->nsnap,
	T->nsnapmap, proto_bc(pt), pt->sizebc))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
	ARM64_CALLXS_PTR_R_XSAVE_PRE, IR_XSAVE, 11);
  return 1;
}

static int arm64_postra_callxs_layout(const IRIns *ir)
{
  static const uint8_t regs[ARM64_CALLXS_SEMANTIC_NINS-REF_BASE] = {
    RID_X23, RID_X19, RID_INIT, RID_X28, RID_X21, RID_X1,
    RID_INIT, RID_X1, RID_X2, RID_INIT, RID_INIT, RID_X0,
    RID_X5, RID_INIT, RID_X5, RID_INIT, RID_X20, RID_INIT,
    RID_INIT, RID_INIT, RID_X0, RID_NONE, RID_X0, RID_X1,
    RID_NONE, RID_INIT, RID_X28, RID_INIT, RID_INIT, RID_INIT,
    RID_X27, RID_INIT, RID_X1, RID_X2, RID_INIT, RID_INIT,
    RID_X0, RID_X27, RID_INIT, RID_INIT, RID_X0, RID_NONE,
    RID_X0, RID_X27, RID_NONE, RID_INIT, RID_X28, RID_INIT,
    RID_X28
  };
  static const uint8_t spills[ARM64_CALLXS_SEMANTIC_NINS-REF_BASE] = {
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, 3, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    2, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE
  };
  static const IRRef krefs[] = {
    ARM64_CALLXS_K_ZERO, ARM64_CALLXS_K_ROOT, ARM64_CALLXS_K_TRACE,
    ARM64_CALLXS_K_CTYPE, ARM64_CALLXS_K_FTSZ, ARM64_CALLXS_K_META,
    ARM64_CALLXS_K_KEY, ARM64_CALLXS_K_TABLE,
    ARM64_CALLXS_K_LIMITMAX, ARM64_CALLXS_K_ONE,
    REF_TRUE, REF_FALSE, REF_NIL
  };
  IRRef ref;
  MSize n;
  for (ref = REF_BASE; ref < ARM64_CALLXS_SEMANTIC_NINS; ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    MSize idx = (MSize)(ref-REF_BASE);
    if (ins.r != regs[idx] || ins.s != spills[idx])
      return 0;
  }
  for (n = 0; n < sizeof(krefs)/sizeof(krefs[0]); n++) {
    IRIns ins = ir_load_acq(&ir[krefs[n]]);
    if (ins.r != RID_INIT || ins.s != SPS_NONE)
      return 0;
  }
  return 1;
}

static int arm64_postra_callxs_double_layout(const IRIns *ir)
{
  static const uint8_t regs[ARM64_CALLXS_D_SEMANTIC_NINS-REF_BASE] = {
    RID_X23, RID_X19, RID_INIT, RID_X28, RID_X21, RID_X1,
    RID_INIT, RID_X1, RID_X2, RID_INIT, RID_INIT, RID_X0,
    RID_X5, RID_INIT, RID_X5, RID_INIT, RID_X20, RID_D9,
    RID_INIT, RID_INIT, RID_INIT, RID_X0, RID_NONE, RID_D0,
    RID_X0, RID_NONE, RID_INIT, RID_X28, RID_INIT, RID_INIT,
    RID_INIT, RID_X27, RID_INIT, RID_X1, RID_X2, RID_INIT,
    RID_INIT, RID_X0, RID_X27, RID_INIT, RID_D15, RID_INIT,
    RID_X0, RID_NONE, RID_D0, RID_X0, RID_NONE, RID_INIT,
    RID_X28, RID_INIT, RID_X28
  };
  static const uint8_t spills[ARM64_CALLXS_D_SEMANTIC_NINS-REF_BASE] = {
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, 4,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, 2, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE
  };
  static const IRRef krefs[] = {
    ARM64_CALLXS_K_ZERO, ARM64_CALLXS_K_ROOT, ARM64_CALLXS_K_TRACE,
    ARM64_CALLXS_K_CTYPE, ARM64_CALLXS_K_FTSZ, ARM64_CALLXS_K_META,
    ARM64_CALLXS_K_KEY, ARM64_CALLXS_K_TABLE,
    ARM64_CALLXS_K_LIMITMAX, ARM64_CALLXS_K_ONE,
    REF_TRUE, REF_FALSE, REF_NIL
  };
  IRRef ref;
  MSize n;
  for (ref = REF_BASE; ref < ARM64_CALLXS_D_SEMANTIC_NINS; ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    MSize idx = (MSize)(ref-REF_BASE);
    if (ins.r != regs[idx] || ins.s != spills[idx])
      return 0;
  }
  for (n = 0; n < sizeof(krefs)/sizeof(krefs[0]); n++) {
    IRIns ins = ir_load_acq(&ir[krefs[n]]);
    if (ins.r != RID_INIT || ins.s != SPS_NONE)
      return 0;
  }
  return 1;
}

static int arm64_postra_callxs_ptr_layout(const IRIns *ir)
{
  static const uint8_t regs[
      ARM64_CALLXS_PTR_SEMANTIC_NINS-REF_BASE] = {
    RID_NONE|RID_X19, RID_X19, RID_INIT, RID_X28, RID_X21, RID_X23,
    RID_X1, RID_INIT, RID_X1, RID_X2, RID_INIT, RID_INIT,
    RID_X0, RID_X2, RID_INIT, RID_X2, RID_INIT, RID_X20,
    RID_X27, RID_X27, RID_NONE, RID_INIT, RID_INIT, RID_INIT,
    RID_X1, RID_NONE, RID_X0, RID_INIT, RID_X2, RID_NONE,
    RID_INIT, RID_X28, RID_INIT, RID_INIT, RID_INIT, RID_X26,
    RID_INIT, RID_X1, RID_X2, RID_INIT, RID_INIT, RID_X0,
    RID_X26, RID_INIT, RID_X26, RID_NONE, RID_INIT, RID_INIT,
    RID_X24, RID_NONE, RID_X0, RID_INIT, RID_X26, RID_NONE,
    RID_X28, RID_INIT, RID_X28, RID_X0
  };
  static const uint8_t spills[
      ARM64_CALLXS_PTR_SEMANTIC_NINS-REF_BASE] = {
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    4, 6, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, 2, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE,
    SPS_NONE, SPS_NONE, SPS_NONE, SPS_NONE
  };
  static const IRRef krefs[] = {
    ARM64_CALLXS_PTR_K_ZERO, ARM64_CALLXS_PTR_K_TRACE,
    ARM64_CALLXS_PTR_K_PAYLOAD_OFS, ARM64_CALLXS_PTR_K_BOX_CTYPE,
    ARM64_CALLXS_PTR_K_STRING_OFS, ARM64_CALLXS_PTR_K_CTYPE,
    ARM64_CALLXS_PTR_K_FTSZ, ARM64_CALLXS_PTR_K_META,
    ARM64_CALLXS_PTR_K_KEY, ARM64_CALLXS_PTR_K_TABLE,
    ARM64_CALLXS_PTR_K_LIMITMAX, ARM64_CALLXS_PTR_K_ONE,
    REF_TRUE, REF_FALSE, REF_NIL
  };
  IRRef ref;
  MSize n;
  for (ref = REF_BASE; ref < ARM64_CALLXS_PTR_SEMANTIC_NINS; ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    MSize idx = (MSize)(ref-REF_BASE);
    if (ins.r != regs[idx] || ins.s != spills[idx])
      return 0;
  }
  for (n = 0; n < sizeof(krefs)/sizeof(krefs[0]); n++) {
    IRIns ins = ir_load_acq(&ir[krefs[n]]);
    if (ins.r != RID_INIT || ins.s != SPS_NONE)
      return 0;
  }
  return 1;
}

static int arm64_postra_callxs_admit(const LJArm64PostRAView *view,
	IRRef *semantic_ninsp)
{
  IRRef semantic_nins;
  LJArm64CallXSProfile profile;
  IRIns last;
  if (view->owner == NULL || view->ir == NULL || view->snap == NULL ||
	view->snapmap == NULL || view->proto_bc == NULL ||
	view->nk != ARM64_CALLXS_K_ZERO || view->nsnap != 15 ||
	view->nsnapmap != 97 || view->root_topslot != 9 ||
	view->proto_sizebc != 16 || view->proto_numparams != 2 ||
	view->base_delta != 0 ||
	bc_op(view->startins) != BC_FORL)
    return 0;
  if (view->nins == ARM64_CALLXS_SEMANTIC_NINS+1u &&
	view->spadjust == 0) {
    profile = ARM64_CALLXS_PROFILE_I32;
    semantic_nins = ARM64_CALLXS_SEMANTIC_NINS;
  } else if (view->nins == ARM64_CALLXS_D_SEMANTIC_NINS+1u &&
	view->spadjust == 16) {
    profile = ARM64_CALLXS_PROFILE_DOUBLE;
    semantic_nins = ARM64_CALLXS_D_SEMANTIC_NINS;
  } else {
    return 0;
  }
  last = ir_load_acq(&view->ir[semantic_nins]);
  if (last.o != IR_NOP || last.t.irt != IRT_NIL ||
      last.op1 != 0 || last.op2 != 0 || last.prev != 0 ||
	!arm64_callxs_bytecode(view->proto_bc, view->proto_sizebc,
	  view->root_topslot, view->proto_numparams, view->startins,
	  view->owner) ||
	!arm64_callxs_constant_shape(view->ir, view->nk, view->owner,
	  view->proto_bc, view->proto_sizebc) ||
	!arm64_callxs_ir_shape(view->ir, profile) ||
	!arm64_callxs_snapshots(view->ir, view->snap, view->snapmap, view->nsnap,
	  view->nsnapmap, view->proto_bc, view->proto_sizebc, profile) ||
	(profile == ARM64_CALLXS_PROFILE_I32 ?
	 !arm64_postra_callxs_layout(view->ir) :
	 !arm64_postra_callxs_double_layout(view->ir)))
    return 0;
  if (semantic_ninsp)
    *semantic_ninsp = semantic_nins;
  return 1;
}

static int arm64_postra_callxs_ptr_admit(const LJArm64PostRAView *view,
	IRRef *semantic_ninsp)
{
  IRIns rename0, rename1;
  if (view->owner == NULL || view->ir == NULL || view->snap == NULL ||
	view->snapmap == NULL || view->proto_bc == NULL ||
	view->nk != ARM64_CALLXS_PTR_K_ZERO || view->nsnap != 18 ||
	view->nsnapmap != 138 || view->root_topslot != 11 ||
	view->proto_sizebc != 13 || view->proto_numparams != 3 ||
	view->base_delta != 0 || view->spadjust != 16 ||
	view->nins != ARM64_CALLXS_PTR_SEMANTIC_NINS+2u ||
	bc_op(view->startins) != BC_FORL)
    return 0;
  rename0 = ir_load_acq(&view->ir[ARM64_CALLXS_PTR_SEMANTIC_NINS]);
  rename1 = ir_load_acq(&view->ir[ARM64_CALLXS_PTR_SEMANTIC_NINS+1u]);
  if (rename0.o != IR_RENAME || rename0.t.irt != IRT_NIL ||
	rename0.op1 != ARM64_CALLXS_PTR_R_BOX_PRE || rename0.op2 != 10 ||
	rename0.r != RID_X27 || rename0.s != SPS_NONE ||
	rename1.o != IR_RENAME || rename1.t.irt != IRT_NIL ||
	rename1.op1 != ARM64_CALLXS_PTR_R_BOX_PRE || rename1.op2 != 10 ||
	rename1.r != RID_X0 || rename1.s != SPS_NONE ||
	!arm64_callxs_ptr_bytecode(view->proto_bc, view->proto_sizebc,
	  view->root_topslot, view->proto_numparams, view->startins,
	  view->owner) ||
	!arm64_callxs_ptr_constant_shape(view->ir, view->nk, view->owner,
	  view->proto_bc, view->proto_sizebc) ||
	!arm64_callxs_ptr_ir_shape(view->ir) ||
	!arm64_callxs_ptr_snapshots(view->ir, view->snap, view->snapmap,
	  view->nsnap, view->nsnapmap, view->proto_bc,
	  view->proto_sizebc) ||
	!arm64_postra_callxs_ptr_layout(view->ir))
    return 0;
  if (semantic_ninsp)
    *semantic_ninsp = ARM64_CALLXS_PTR_SEMANTIC_NINS;
  return 1;
}
#endif

/* Validate the immutable allocator layout used by native execution and exit
** restoration. This is deliberately independent of recorder state: the same
** bounded scan runs while assembling and after root entry publishes jit_base
** as its trace-body lifetime lease. */
int lj_asm_arm64_postra_admit(const LJArm64PostRAView *view,
	IRRef *semantic_ninsp)
{
  const IRIns *ir;
  IRRef semantic_nins, ref, renref;
  MSize spadjust, capacity, highest_end = 0;
  MSize nsnap, nsnapmap;
  MSize nrename = 0;
  MSize maxslots, forl_idxslot = 0;
  uintptr_t proto_lo, proto_hi, proto_bytes;
  BCOp rootop;
  unsigned nintadd = 0, scalar_mode = 0, constant_profile = 0;
  unsigned numdynamic_profile = 0;
  unsigned numdynamic_args_kind = ARM64_NUMDYN_ARGS_NUM;
  int suffix_is_nop = 0, allow_num_sub = 0, allow_num_mul = 0;
  int allow_num_div = 0, forl_dynamic_step = 0;

  LJ_STATIC_ASSERT(SPS_FIRST == 2);
  LJ_STATIC_ASSERT(SPS_FIXED == 4);
  LJ_STATIC_ASSERT(SPS_LIMIT == 256);

  if (view == NULL || (ir = view->ir) == NULL || view->snap == NULL ||
	view->snapmap == NULL || view->proto_bc == NULL ||
	view->nins <= REF_FIRST ||
	view->nins >= REF_DROP || view->nk == 0 || view->nk > REF_TRUE ||
	view->nsnap == 0 || view->nsnapmap == 0 ||
	view->proto_sizebc == 0 || view->root_topslot == 0 ||
	view->root_topslot > UINT8_MAX || view->base_delta != 0)
    return 0;
#if LJ_HASJIT_FFI_CALLXS
  if (view->nk == ARM64_CALLXS_K_ZERO &&
      bc_op(view->startins) == BC_FORL) {
    IRIns ktrace = ir_load_acq(&view->ir[ARM64_CALLXS_K_TRACE]);
    if (ktrace.o == IR_KGC && ktrace.t.irt == IRT_P64)
      return arm64_postra_callxs_admit(view, semantic_ninsp);
  } else if (view->nk == ARM64_CALLXS_PTR_K_ZERO &&
	     bc_op(view->startins) == BC_FORL) {
    IRIns ktrace = ir_load_acq(&view->ir[ARM64_CALLXS_PTR_K_TRACE]);
    if (ktrace.o == IR_KGC && ktrace.t.irt == IRT_P64)
      return arm64_postra_callxs_ptr_admit(view, semantic_ninsp);
  }
#endif
  rootop = bc_op(view->startins);
  if (rootop != BC_LOOP && rootop != BC_FORL && rootop != BC_FUNCF)
    return 0;
  if (rootop == BC_FUNCF)
    return arm64_postra_funcf_admit(view, view->startins, semantic_ninsp);
  if (!arm64_postra_constants(view, &scalar_mode, &constant_profile))
    return 0;
  maxslots = view->root_topslot+1u+LJ_FR2;
  if (rootop == BC_FORL) {
    MSize ra = bc_a(view->startins);
    if (ra+FORL_EXT >= view->root_topslot)
      return 0;
    forl_idxslot = 1u+LJ_FR2+ra;
  }
  proto_lo = (uintptr_t)view->proto_bc;
  if ((proto_lo & (sizeof(BCIns)-1u)) != 0 ||
	(uintptr_t)view->proto_sizebc >
	  (UINTPTR_MAX-proto_lo)/sizeof(BCIns))
    return 0;
  proto_bytes = (uintptr_t)view->proto_sizebc*sizeof(BCIns);
  proto_hi = proto_lo+proto_bytes;
  if (proto_hi <= proto_lo)
    return 0;
  if (rootop == BC_LOOP) {
    numdynamic_profile = arm64_numacc_grammar_profile(view->proto_bc,
	view->proto_sizebc);
    allow_num_sub = arm64_numdynamic_is_sub(numdynamic_profile);
    allow_num_mul = arm64_numdynamic_is_mul(numdynamic_profile);
    allow_num_div = arm64_numdynamic_is_div(numdynamic_profile);
  }
  nsnap = view->nsnap;
  nsnapmap = view->nsnapmap;
  spadjust = view->spadjust;
  if ((spadjust & 15u) != 0 ||
	spadjust > (MSize)sps_scale(SPS_LIMIT-SPS_FIXED))
    return 0;
  capacity = SPS_FIXED + spadjust / sizeof(int32_t);
  if (capacity > SPS_LIMIT)
    return 0;

  semantic_nins = view->nins;
  {
    IRIns last = ir_load_acq(&ir[semantic_nins-1u]);
    if (last.o == IR_NOP) {
      if (last.t.irt != IRT_NIL || last.op1 != 0 || last.op2 != 0 ||
	  last.prev != 0)
	return 0;
      semantic_nins--;
      suffix_is_nop = 1;
    } else {
      while (semantic_nins > REF_FIRST) {
	IRIns ren = ir_load_acq(&ir[semantic_nins-1u]);
	if (ren.o != IR_RENAME)
	  break;
	semantic_nins--;
	nrename++;
      }
      if (nrename == 0 || nrename > LJ_MAX_PHI)
	return 0;
    }
  }
  if (semantic_nins <= REF_FIRST)
    return 0;
  if (rootop == BC_FORL &&
      !arm64_postra_forl_dynamic_shape(view, semantic_nins,
	&forl_dynamic_step))
    return 0;

  for (ref = REF_BASE; ref < semantic_nins; ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    MSize slot = ins.s;
    switch ((IROp)ins.o) {
    case IR_BASE:
      if (ref != REF_BASE || ins.t.irt != IRT_PGC || slot != SPS_NONE)
	return 0;
      break;
    case IR_SLOAD:
      if (irt_type(ins.t) == IRT_INT) {
	scalar_mode |= ARM64_IR_SCALAR_INT;
	if (!arm64_postra_int_value(ins, rootop, forl_idxslot, maxslots))
	  return 0;
      } else if (irt_type(ins.t) == IRT_NUM) {
	scalar_mode |= ARM64_IR_SCALAR_NUM;
	if (!arm64_postra_num_value(ins, rootop, maxslots, allow_num_sub,
		allow_num_mul, allow_num_div) ||
	    slot != SPS_NONE || ins.r < RID_MIN_FPR || ins.r >= RID_MAX_FPR ||
	    !rset_test(RSET_FPR, ins.r))
	  return 0;
      } else {
	return 0;
      }
      break;
    case IR_CONV:
      if (numdynamic_profile == 0 || slot != SPS_NONE)
	return 0;
      if (numdynamic_args_kind == ARM64_NUMDYN_ARGS_NUM &&
	  ins.t.irt == IRT_NUM && ins.op2 == IRCONV_NUM_INT &&
	  ins.r >= RID_MIN_FPR && ins.r < RID_MAX_FPR &&
	  rset_test(RSET_FPR, ins.r)) {
	if (ref == ARM64_NUMACC_INTSTEP_R_STEP_NUM &&
	    ins.op1 == ARM64_NUMACC_INTSTEP_R_STEP_INT) {
	  numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_STEP;
	} else if (numdynamic_profile == ARM64_NUMDYN_ADD_LT &&
	    ref == ARM64_NUMACC_INTLIMIT_R_LIMIT_NUM &&
	    ins.op1 == ARM64_NUMACC_INTLIMIT_R_LIMIT_INT) {
	  numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_LIMIT;
	} else if (ref == ARM64_NUMACC_INTX_R_X_NUM &&
	    ins.op1 == ARM64_NUMACC_INTX_R_X_INT) {
	  numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_X;
	} else {
	  return 0;
	}
      } else if (numdynamic_args_kind == ARM64_NUMDYN_ARGS_INT_X &&
	  ref == ARM64_NUMACC_INTX_R_X_CHECK &&
	  ins.t.irt == (IRT_INT|IRT_GUARD) &&
	  ins.op1 == ARM64_NUMACC_INTX_R_X_PRE &&
	  ins.op2 == (IRCONV_INT_NUM|IRCONV_CHECK) &&
	  ins.r < RID_MAX_GPR && rset_test(RSET_GPR, ins.r)) {
	/* Exact loop type-instability repair; its guard result is unused. */
      } else {
	return 0;
      }
      scalar_mode |= ARM64_IR_SCALAR_NUM;
      break;
    case IR_ADDOV: case IR_SUBOV: case IR_MULOV:
      if (!arm64_postra_int_value(ins, rootop, forl_idxslot, maxslots))
	return 0;
      scalar_mode |= ARM64_IR_SCALAR_INT;
      break;
    case IR_ADD:
      if (irt_type(ins.t) == IRT_INT) {
	nintadd++;
	scalar_mode |= ARM64_IR_SCALAR_INT;
	if (!arm64_postra_int_value(ins, rootop, forl_idxslot, maxslots))
	  return 0;
      } else if (irt_type(ins.t) == IRT_NUM) {
	scalar_mode |= ARM64_IR_SCALAR_NUM;
	if (!arm64_postra_num_value(ins, rootop, maxslots, allow_num_sub,
		allow_num_mul, allow_num_div) ||
	    slot != SPS_NONE || ins.r < RID_MIN_FPR || ins.r >= RID_MAX_FPR ||
	    !rset_test(RSET_FPR, ins.r))
	  return 0;
      } else {
	return 0;
      }
      break;
    case IR_SUB:
      if (!allow_num_sub || irt_type(ins.t) != IRT_NUM)
	return 0;
      scalar_mode |= ARM64_IR_SCALAR_NUM;
      if (!arm64_postra_num_value(ins, rootop, maxslots, allow_num_sub,
		allow_num_mul, allow_num_div) ||
	  slot != SPS_NONE || ins.r < RID_MIN_FPR || ins.r >= RID_MAX_FPR ||
	  !rset_test(RSET_FPR, ins.r))
	return 0;
      break;
    case IR_MUL:
      if (!allow_num_mul || irt_type(ins.t) != IRT_NUM)
	return 0;
      scalar_mode |= ARM64_IR_SCALAR_NUM;
      if (!arm64_postra_num_value(ins, rootop, maxslots, allow_num_sub,
		allow_num_mul, allow_num_div) ||
	  slot != SPS_NONE || ins.r < RID_MIN_FPR || ins.r >= RID_MAX_FPR ||
	  !rset_test(RSET_FPR, ins.r))
	return 0;
      break;
    case IR_DIV:
      if (!allow_num_div || irt_type(ins.t) != IRT_NUM)
	return 0;
      scalar_mode |= ARM64_IR_SCALAR_NUM;
      if (!arm64_postra_num_value(ins, rootop, maxslots, allow_num_sub,
		allow_num_mul, allow_num_div) ||
	  slot != SPS_NONE || ins.r < RID_MIN_FPR || ins.r >= RID_MAX_FPR ||
	  !rset_test(RSET_FPR, ins.r))
	return 0;
      break;
    case IR_LT: case IR_GE: case IR_LE: case IR_GT:
      if (irt_type(ins.t) == IRT_INT) {
	if (!arm64_ir_type_flags(ins.t, IRT_INT, IRT_GUARD, IRT_GUARD))
	  return 0;
	scalar_mode |= ARM64_IR_SCALAR_INT;
      } else if (irt_type(ins.t) == IRT_NUM) {
	if (!arm64_ir_type_flags(ins.t, IRT_NUM, IRT_GUARD, IRT_GUARD))
	  return 0;
	scalar_mode |= ARM64_IR_SCALAR_NUM;
      } else {
	return 0;
      }
      if (slot != SPS_NONE)
	return 0;
      break;
    case IR_EQ: case IR_NE:
      if (!arm64_ir_type_flags(ins.t, IRT_INT, IRT_GUARD, IRT_GUARD) ||
	  slot != SPS_NONE)
	return 0;
      scalar_mode |= ARM64_IR_SCALAR_INT;
      break;
    case IR_LOOP: case IR_XPOLL:
      if (!arm64_ir_type_flags(ins.t, IRT_NIL, IRT_GUARD, IRT_GUARD) ||
	  slot != SPS_NONE)
	return 0;
      break;
    case IR_USE:
      if (rootop != BC_FORL || ins.t.irt != IRT_INT || ins.op2 != 0 ||
	  slot != SPS_NONE)
	return 0;
      break;
    case IR_PHI:
      if (ins.t.irt != IRT_INT && ins.t.irt != IRT_NUM)
	return 0;
      if (ins.t.irt == IRT_NUM) {
	scalar_mode |= ARM64_IR_SCALAR_NUM;
	if (slot != SPS_NONE || ins.r < RID_MIN_FPR || ins.r >= RID_MAX_FPR ||
	    !rset_test(RSET_FPR, ins.r))
	  return 0;
      } else {
	scalar_mode |= ARM64_IR_SCALAR_INT;
      }
      break;
    default:
      return 0;
    }
    if (slot != SPS_NONE) {
      MSize end = slot + 1u;
      if (!arm64_postra_spill_slot(slot, capacity))
	return 0;
      if (end > highest_end)
	highest_end = end;
    }
  }

  if ((rootop == BC_FORL && nintadd != 2u) ||
      (rootop == BC_LOOP && nintadd != 0u))
    return 0;

  if ((scalar_mode & ARM64_IR_SCALAR_NUM) != 0) {
    if (scalar_mode == (ARM64_IR_SCALAR_INT|ARM64_IR_SCALAR_NUM)) {
      if (numdynamic_args_kind != ARM64_NUMDYN_ARGS_NUM) {
	if (constant_profile != ARM64_IR_KPROFILE_INT || !suffix_is_nop ||
	    nrename != 0 || spadjust != 0 || highest_end != 0 ||
	    !arm64_postra_numacc_shape(view, semantic_nins,
	      numdynamic_args_kind))
	  return 0;
      } else if (constant_profile != ARM64_IR_KPROFILE_INT ||
	  suffix_is_nop || nrename != 1u || spadjust != 0 ||
	  highest_end != 0 ||
	  !arm64_postra_numadd_shape(view, semantic_nins)) {
	return 0;
      }
    } else if (scalar_mode == ARM64_IR_SCALAR_NUM) {
      if (!suffix_is_nop || nrename != 0 || spadjust != 0 || highest_end != 0)
	return 0;
      if (constant_profile == ARM64_IR_KPROFILE_HALF) {
	if (!arm64_postra_numhalf_shape(view, semantic_nins))
	  return 0;
      } else if (constant_profile == ARM64_IR_KPROFILE_INT) {
	/* The all-KINT profile is vacuously true for the two exact no-constant
	** dynamic-NUM root families. Select by their distinct prototype sizes;
	** the all-parameter family then independently rechecks its complete
	** arithmetic/comparison grammar before the shared kernel certificate. */
	if (view->proto_sizebc == 14) {
	  if (!arm64_postra_numstep_shape(view, semantic_nins))
	    return 0;
	} else if (view->proto_sizebc == 13) {
	  if (!arm64_postra_numacc_shape(view, semantic_nins,
		ARM64_NUMDYN_ARGS_NUM))
	    return 0;
	} else {
	  return 0;
	}
      } else {
	return 0;
      }
    } else {
      return 0;
    }
  }

  if (highest_end <= SPS_FIXED) {
    if (spadjust != 0)
      return 0;
  } else {
    MSize expected = (MSize)sps_scale(sps_align(highest_end));
    if (spadjust != expected || highest_end > capacity)
      return 0;
  }

  if (!suffix_is_nop) {
    for (ref = semantic_nins; ref < view->nins; ref++) {
      IRIns ren = ir_load_acq(&ir[ref]);
      IRIns source;
      if (ren.o != IR_RENAME || ren.t.irt != IRT_NIL ||
	  ren.op1 < REF_FIRST || ren.op1 >= semantic_nins ||
	  ren.op2 >= nsnap || ren.s != SPS_NONE)
	return 0;
      source = ir_load_acq(&ir[ren.op1]);
      if (irt_type(source.t) == IRT_INT) {
	if (!arm64_postra_int_value(source, rootop, forl_idxslot, maxslots) ||
	    ren.r >= RID_MAX_GPR || !rset_test(RSET_GPR, ren.r))
	  return 0;
      } else if (irt_type(source.t) == IRT_NUM) {
	if (!arm64_postra_num_value(source, rootop, maxslots,
		allow_num_sub, allow_num_mul, allow_num_div) ||
	    ren.r < RID_MIN_FPR || ren.r >= RID_MAX_FPR ||
	    !rset_test(RSET_FPR, ren.r))
	  return 0;
      } else {
	return 0;
      }
    }
  }

  {
    MSize snapno;
    MSize expected_mapofs = 0;
    IRRef prev_snapref = 0;
    for (snapno = 0; snapno < nsnap; snapno++) {
      const SnapShot *snap = &view->snap[snapno];
      MSize mapofs = snap_mapofs_acq(snap);
      MSize nent = snap_nent_acq(snap);
      MSize nslots = snap_nslots_acq(snap);
      MSize topslot = snap_topslot_acq(snap);
      MSize nextofs = snapno+1u < nsnap ?
	  snap_mapofs_acq(&view->snap[snapno+1u]) : nsnapmap;
      IRRef snapat = snap_ref_acq(snap);
      SnapEntry pcraw[1+LJ_FR2];
      uint64_t pcbase;
      uintptr_t snappc;
      MSize snappos;
      MSize n;
      if (snapat < REF_FIRST || snapat >= semantic_nins ||
	  snapat < prev_snapref || mapofs > nsnapmap ||
	  nextofs < mapofs || nextofs > nsnapmap ||
	  mapofs != expected_mapofs || nent > nextofs-mapofs ||
	  nextofs-mapofs-nent != 1u+LJ_FR2 ||
	  nslots < 1u+LJ_FR2 || topslot != view->root_topslot ||
	  nslots > view->root_topslot+1u+LJ_FR2)
	return 0;
      expected_mapofs = nextofs;
      prev_snapref = snapat;
      for (n = 0; n < nent; n++) {
	SnapEntry sn = snapentry_acq(&view->snapmap[mapofs+n]);
	IRRef valueref = snap_ref(sn);
	BCReg slot = snap_slot(sn);
	uint32_t flags = sn & 0x00ff0000u;
	IRIns source;
	RegSP rs;
	if (slot >= nslots || (n != 0 &&
	    slot <= snap_slot(snapentry_acq(&view->snapmap[mapofs+n-1u]))))
	  return 0;
	if (sn == SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL))
	  continue;
	if (slot < 1u+LJ_FR2 ||
	    (flags != 0 && flags != SNAP_NORESTORE))
	  return 0;
	if (irref_isk(valueref)) {
	  if (flags != 0 || valueref < view->nk || valueref >= REF_TRUE)
	    return 0;
	  if (!arm64_postra_scalar_kref(view, valueref, IRT_INT))
	    return 0;
	  continue;
	}
	if (valueref < REF_FIRST || valueref >= snapat)
	  return 0;
	source = ir_load_acq(&ir[valueref]);
	if ((irt_type(source.t) != IRT_INT && irt_type(source.t) != IRT_NUM) ||
	    !arm64_postra_scalar_value(source, rootop, forl_idxslot, maxslots,
				       (IRType)irt_type(source.t),
				       allow_num_sub, allow_num_mul,
				       allow_num_div) ||
	    (flags == SNAP_NORESTORE &&
	     (source.o != IR_SLOAD || source.op1 != slot ||
	      rootop != BC_FORL ||
	      (slot != forl_idxslot && slot != forl_idxslot+FORL_STOP &&
	       (!forl_dynamic_step ||
		slot != forl_idxslot+FORL_STEP)))))
	  return 0;
	rs = source.prev;
	for (renref = view->nins; renref-- > semantic_nins; ) {
	  IRIns ren = ir_load_acq(&ir[renref]);
	  if (ren.o != IR_RENAME)
	    break;
	  if (ren.op1 == valueref && ren.op2 <= snapno)
	    rs = ren.prev;
	}
	if (ra_hasspill(regsp_spill(rs))) {
	  if (irt_type(source.t) == IRT_NUM ||
	      !arm64_postra_spill_slot(regsp_spill(rs), capacity))
	    return 0;
	} else if (irt_type(source.t) == IRT_NUM) {
	  if (regsp_reg(rs) < RID_MIN_FPR || regsp_reg(rs) >= RID_MAX_FPR ||
	      !rset_test(RSET_FPR, regsp_reg(rs)))
	    return 0;
	} else {
	  if (regsp_reg(rs) >= RID_MAX_GPR ||
	      !rset_test(RSET_GPR, regsp_reg(rs)))
	    return 0;
	}
      }
      LJ_STATIC_ASSERT(sizeof(pcraw) == sizeof(pcbase));
      for (n = 0; n < 1u+LJ_FR2; n++)
	pcraw[n] = snapentry_acq(&view->snapmap[mapofs+nent+n]);
      memcpy(&pcbase, pcraw, sizeof(pcbase));
      snappc = (uintptr_t)(pcbase >> 8);
      if ((uint8_t)pcbase != view->base_delta ||
	  !arm64_ir_pcpos(snappc, proto_lo, proto_hi, &snappos))
	return 0;
    }
  }

  if (semantic_ninsp)
    *semantic_ninsp = semantic_nins;
  return 1;
}

/* Re-run the exact fixed-function allocator certificate after publication.
** Assembly sees the original FUNCF word, whereas native entry must prove the
** full patched JFUNCF generation without weakening the assembly-time API. */
int lj_asm_arm64_postra_funcf_entry_admit(
	const LJArm64PostRAView *view, BCIns liveins,
	IRRef *semantic_ninsp)
{
  /* This helper is exported for the entry gate, so retain fail-closed pointer
  ** behavior even if a future caller omits lj_trace.c's metadata preflight. */
  if (view == NULL || bc_op(view->startins) != BC_FUNCF ||
	bc_op(liveins) != BC_JFUNCF ||
	bc_a(liveins) != bc_a(view->startins) || bc_d(liveins) == 0)
    return 0;
  return arm64_postra_funcf_admit(view, liveins, semantic_ninsp);
}

static int arm64_ir_int_ref(const GCtrace *T, IRRef ref, IRRef before,
	int allow_add)
{
  const IRIns *ir;
  if (ref < REF_BASE)
    return arm64_ir_int_kref(T, ref);
  if (ref < REF_FIRST || ref >= T->nins || ref >= before)
    return 0;
  ir = &T->ir[ref];
  return irt_type(ir->t) == IRT_INT &&
	 arm64_ir_int_value_op((IROp)ir->o, allow_add);
}

static int arm64_ir_num_ref(const GCtrace *T, IRRef ref, IRRef before,
	int allow_sub, int allow_mul, int allow_div)
{
  const IRIns *ir;
  if (ref < REF_FIRST || ref >= T->nins || ref >= before)
    return 0;
  ir = &T->ir[ref];
  if (ir->o == IR_CONV) {
    const IRIns *source;
    IRRef sourceref;
    IRRef sourceslot;
    if (ref == ARM64_NUMACC_INTSTEP_R_STEP_NUM &&
	ir->op1 == ARM64_NUMACC_INTSTEP_R_STEP_INT) {
      sourceref = ARM64_NUMACC_INTSTEP_R_STEP_INT;
      sourceslot = 4;
    } else if (ref == ARM64_NUMACC_INTLIMIT_R_LIMIT_NUM &&
	ir->op1 == ARM64_NUMACC_INTLIMIT_R_LIMIT_INT) {
      sourceref = ARM64_NUMACC_INTLIMIT_R_LIMIT_INT;
      sourceslot = 3;
    } else if (ref == ARM64_NUMACC_INTX_R_X_NUM &&
	ir->op1 == ARM64_NUMACC_INTX_R_X_INT) {
      sourceref = ARM64_NUMACC_INTX_R_X_INT;
      sourceslot = 2;
    } else {
      return 0;
    }
    if (ir->t.irt != IRT_NUM || ir->op1 != sourceref ||
	ir->op2 != IRCONV_NUM_INT)
      return 0;
    source = &T->ir[sourceref];
    return source->o == IR_SLOAD &&
	   source->t.irt == (IRT_INT|IRT_GUARD) &&
	   source->op1 == sourceslot &&
	   source->op2 == IRSLOAD_TYPECHECK;
  }
  return irt_type(ir->t) == IRT_NUM &&
	 arm64_ir_num_value_op((IROp)ir->o, allow_sub, allow_mul, allow_div);
}

static int arm64_ir_num_add_ref(const GCtrace *T, IRRef ref, IRRef before)
{
  if (ref == ARM64_NUMHALF_K_HALF)
    return arm64_ir_numhalf_constant(T->ir, T->nk);
  return arm64_ir_num_ref(T, ref, before, 0, 0, 0);
}

static int arm64_ir_constants(const GCtrace *T, LJArm64IRReject *reject,
	unsigned *constant_profilep)
{
  IRRef ref;
  if (T->nk > REF_TRUE || T->nk == 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT, T->nk,
			   IR_KPRI, 0);
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    const IRIns *ir = &T->ir[ref];
    IRType expected = (IRType)(REF_NIL-ref);
    if (ir->o != IR_KPRI || ir->t.irt != expected || ir->op12 != 0)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT, ref,
			     IR_KPRI, ir->t.irt);
  }
  if (arm64_ir_numhalf_constant(T->ir, T->nk)) {
    *constant_profilep = ARM64_IR_KPROFILE_HALF;
    return 1;
  }
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    const IRIns *ir = &T->ir[ref];
    if (ir->o != IR_KINT || ir->t.irt != IRT_INT)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT, ref,
			     (IROp)ir->o, 0);
  }
  *constant_profilep = ARM64_IR_KPROFILE_INT;
  return 1;
}

/* -- Pure ARM64 first-side admission ------------------------------------- */

enum {
  ARM64_SIDE_K_ADDEND = REF_TRUE-1u,
  ARM64_SIDE_R_PARENT = REF_BASE+1u,
  ARM64_SIDE_R_CGET = REF_BASE+2u,
  ARM64_SIDE_R_ADD = REF_BASE+3u,
  ARM64_SIDE_R_LIMIT = REF_BASE+4u,
  ARM64_SIDE_R_GT = REF_BASE+5u,
  ARM64_SIDE_R_XPOLL = REF_BASE+6u,
  ARM64_SIDE_SEMANTIC_NINS = REF_BASE+7u
};

const LJArm64SideShape *lj_asm_arm64_side_shape(ExitNo exitno)
{
  static const LJArm64SideShape shapes[] = {
    { 2u, 8u, 13u, { 13u, 14u, 3u, 17u, 7u },
      RID_X28, RID_X27, { 1, 1 } },
    { 6u, 9u, 10u, { 10u, 11u, 3u, 17u, 7u },
      RID_X27, RID_X28, { 1, 2 } },
    { 7u, 11u, 13u, { 13u, 14u, 3u, 17u, 7u },
      RID_X28, RID_X27, { 1, 1 } }
  };
  MSize i;
  for (i = 0; i < sizeof(shapes)/sizeof(shapes[0]); i++)
    if (shapes[i].exitno == exitno)
      return &shapes[i];
  return NULL;
}

static int arm64_side_snapshot_footer(const LJArm64SideIRView *view,
	MSize snapno, MSize expected_pcpos)
{
  const SnapShot *snap = &view->snap[snapno];
  SnapEntry pcraw[1+LJ_FR2];
  uint64_t pcbase;
  uintptr_t proto, expected;
  MSize n;
  LJ_STATIC_ASSERT(LJ_FR2 == 1);
  LJ_STATIC_ASSERT(sizeof(pcraw) == sizeof(pcbase));
  proto = (uintptr_t)(const void *)view->proto_bc;
  if (expected_pcpos >= view->proto_sizebc ||
	(uintptr_t)expected_pcpos >
	  (UINTPTR_MAX-proto)/sizeof(BCIns))
    return 0;
  expected = proto+(uintptr_t)expected_pcpos*sizeof(BCIns);
  for (n = 0; n < 1u+LJ_FR2; n++)
    pcraw[n] = snapentry_acq(
	&view->snapmap[snap_mapofs_acq(snap)+snap_nent_acq(snap)+n]);
  memcpy(&pcbase, pcraw, sizeof(pcbase));
  if (expected > (uintptr_t)(UINT64_MAX >> 8))
    return 0;
  return (uint8_t)pcbase == 0 && (uintptr_t)(pcbase >> 8) == expected;
}

int lj_asm_arm64_side_ir_admit(const LJArm64SideIRView *view,
	LJArm64IRReject *reject)
{
  static const IRRef snaprefs[LJ_ARM64_SIDE_CHILD_NSNAP] = {
    ARM64_SIDE_R_CGET, ARM64_SIDE_R_ADD, ARM64_SIDE_R_LIMIT,
    ARM64_SIDE_R_GT, ARM64_SIDE_R_XPOLL
  };
  static const MSize mapofs[LJ_ARM64_SIDE_CHILD_NSNAP] =
    { 0, 3, 7, 11, 14 };
  static const uint8_t nent[LJ_ARM64_SIDE_CHILD_NSNAP] =
    { 1, 2, 2, 1, 1 };
  static const uint8_t nslots[LJ_ARM64_SIDE_CHILD_NSNAP] =
    { 5, 6, 6, 5, 5 };
  const LJArm64SideShape *shape;
  const IRIns *ir;
  IRIns ins;
  uintptr_t proto;
  MSize snapno;

  if (reject) {
    reject->reason = LJ_ARM64_IR_REJECT_NONE;
    reject->ref = 0;
    reject->op = IR_NOP;
    reject->detail = LJ_ARM64_IR_CALL_NONE;
  }
  if (view == NULL || (ir = view->ir) == NULL || view->snap == NULL ||
	view->snapmap == NULL || view->proto_bc == NULL)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE,
	REF_BASE, IR_BASE, 0);
  shape = lj_asm_arm64_side_shape(view->exitno);
  proto = (uintptr_t)(const void *)view->proto_bc;
  if (shape == NULL || (proto & (sizeof(BCIns)-1u)) != 0 ||
	view->proto_sizebc != 19u ||
	(uintptr_t)view->proto_sizebc >
	  (UINTPTR_MAX-proto)/sizeof(BCIns) ||
	view->nins != ARM64_SIDE_SEMANTIC_NINS ||
	view->nk != ARM64_SIDE_K_ADDEND || view->nsnap != 5u ||
	view->nsnapmap != 17u || view->baseslot != 1u+LJ_FR2 ||
	view->root_topslot != 5u || view->traceno == 0 ||
	view->traceno > UINT16_MAX || view->parent == 0 ||
	view->parent > UINT16_MAX || view->traceno == view->parent ||
	view->root != view->parent ||
	view->link != view->parent ||
	view->startins != BCINS_AD(BC_JMP, 0, 0) ||
	view->linktype != LJ_TRLINK_ROOT || view->sinktags != 0 ||
	view->base_delta != 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE,
	REF_BASE, IR_BASE, (uint16_t)view->exitno);

  ins = ir_load_acq(&ir[ARM64_SIDE_K_ADDEND]);
  if (ins.o != IR_KINT || ins.t.irt != IRT_INT ||
	(ins.i != shape->addends[0] && ins.i != shape->addends[1]))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT,
	ARM64_SIDE_K_ADDEND, (IROp)ins.o, ins.t.irt);
  for (snapno = REF_TRUE; snapno <= REF_NIL; snapno++) {
    IRType expected = (IRType)(REF_NIL-snapno);
    ins = ir_load_acq(&ir[snapno]);
    if (ins.o != IR_KPRI || ins.t.irt != expected || ins.op12 != 0)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT,
	(IRRef)snapno, (IROp)ins.o, ins.t.irt);
  }

#define ARM64_SIDE_REQUIRE(ref, op, type, a, b) \
  do { \
    ins = ir_load_acq(&ir[(ref)]); \
    if (ins.o != (op) || ins.t.irt != (type) || \
	ins.op1 != (a) || ins.op2 != (b)) \
      return arm64_ir_reject(reject, \
	ins.o != (op) ? LJ_ARM64_IR_REJECT_OPCODE : \
	ins.t.irt != (type) ? LJ_ARM64_IR_REJECT_TYPE : \
	LJ_ARM64_IR_REJECT_OPERAND, (ref), (IROp)ins.o, ins.op2); \
  } while (0)
  ARM64_SIDE_REQUIRE(REF_BASE, IR_BASE, IRT_PGC,
	view->parent, view->exitno);
  ARM64_SIDE_REQUIRE(ARM64_SIDE_R_PARENT, IR_SLOAD, IRT_INT, 4,
	IRSLOAD_PARENT|IRSLOAD_INHERIT);
  ARM64_SIDE_REQUIRE(ARM64_SIDE_R_CGET, IR_NOP, IRT_NIL, 0, 0);
  ARM64_SIDE_REQUIRE(ARM64_SIDE_R_ADD, IR_ADDOV, IRT_INT|IRT_GUARD,
	ARM64_SIDE_R_PARENT, ARM64_SIDE_K_ADDEND);
  ARM64_SIDE_REQUIRE(ARM64_SIDE_R_LIMIT, IR_SLOAD, IRT_INT|IRT_GUARD, 2,
	IRSLOAD_TYPECHECK);
  ARM64_SIDE_REQUIRE(ARM64_SIDE_R_GT, IR_GT, IRT_INT|IRT_GUARD,
	ARM64_SIDE_R_LIMIT, ARM64_SIDE_R_ADD);
  ARM64_SIDE_REQUIRE(ARM64_SIDE_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
#undef ARM64_SIDE_REQUIRE

  for (snapno = 0; snapno < LJ_ARM64_SIDE_CHILD_NSNAP; snapno++) {
    const SnapShot *snap = &view->snap[snapno];
    MSize nextofs = snapno+1u < LJ_ARM64_SIDE_CHILD_NSNAP ?
	mapofs[snapno+1u] : 17u;
    if (snap_ref_acq(snap) != snaprefs[snapno] ||
	snap_mapofs_acq(snap) != mapofs[snapno] ||
	snap_nent_acq(snap) != nent[snapno] ||
	snap_nslots_acq(snap) != nslots[snapno] ||
	snap_topslot_acq(snap) != 5u ||
	nextofs-mapofs[snapno] != nent[snapno]+1u+LJ_FR2 ||
	!arm64_side_snapshot_footer(
	  view, snapno, shape->child_pcpos[snapno]))
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
	snaprefs[snapno], IR_XPOLL, (uint16_t)snapno);
  }
  if (snapentry_acq(&view->snapmap[0]) !=
	SNAP(4, 0, ARM64_SIDE_R_PARENT) ||
      snapentry_acq(&view->snapmap[3]) !=
	SNAP(4, 0, ARM64_SIDE_R_PARENT) ||
      snapentry_acq(&view->snapmap[4]) !=
	SNAP(5, 0, ARM64_SIDE_R_PARENT) ||
      snapentry_acq(&view->snapmap[7]) !=
	SNAP(4, 0, ARM64_SIDE_R_ADD) ||
      snapentry_acq(&view->snapmap[8]) !=
	SNAP(5, 0, ARM64_SIDE_R_ADD) ||
      snapentry_acq(&view->snapmap[11]) !=
	SNAP(4, 0, ARM64_SIDE_R_ADD) ||
      snapentry_acq(&view->snapmap[14]) !=
	SNAP(4, 0, ARM64_SIDE_R_ADD))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
	ARM64_SIDE_R_ADD, IR_XPOLL, 0xffffu);
  return 1;
}

int lj_asm_arm64_side_prehead_admit(const LJArm64SidePostRAView *view,
	IRRef *semantic_ninsp)
{
  Reg valueregs[4];
  const LJArm64SideShape *shape;
  const IRIns *ir;
  IRIns ins;
  IRRef ref;
  if (view == NULL || (ir = view->semantic.ir) == NULL ||
	!lj_asm_arm64_side_ir_admit(&view->semantic, NULL) ||
	view->nins != ARM64_SIDE_SEMANTIC_NINS+1u ||
	view->stopins != ARM64_SIDE_R_PARENT ||
	view->orignins != ARM64_SIDE_SEMANTIC_NINS ||
	view->spadjust != 0 || view->parent_spadjust != 0 ||
	view->topslot != 5u || view->parent_topslot != 5u ||
	view->parentmap == NULL || view->parentmap_n != 1u ||
	((uintptr_t)(const void *)view->parentmap &
	 (sizeof(uint16_t)-1u)) != 0)
    return 0;

  shape = lj_asm_arm64_side_shape(view->semantic.exitno);
  if (shape == NULL)
    return 0;
  valueregs[0] = shape->sload_reg;
  valueregs[1] = RID_INIT;
  valueregs[2] = shape->inherited_reg;
  valueregs[3] = shape->sload_reg;

  ins = ir_load_acq(&ir[ARM64_SIDE_SEMANTIC_NINS]);
  if (ins.o != IR_NOP || ins.t.irt != IRT_NIL ||
	ins.op1 != 0 || ins.op2 != 0 || ins.r != 0 || ins.s != 0)
    return 0;

  /* The map extent comes directly from lj_snap_regspmap(). The later full
  ** post-RA certificate proves that asm_head_side() consumes the descriptor's
  ** sole inherited value into its allocator-selected SLOAD register before
  ** any body instruction. */
  if (view->parentmap[0] != REGSP(shape->inherited_reg, SPS_NONE))
    return 0;
  ins = ir_load_acq(&ir[REF_BASE]);
  if (ins.r != RID_BASE || ins.s != SPS_NONE)
    return 0;
  for (ref = ARM64_SIDE_R_PARENT; ref <= ARM64_SIDE_R_LIMIT; ref++) {
    ins = ir_load_acq(&ir[ref]);
    if (ins.r != valueregs[ref-ARM64_SIDE_R_PARENT] ||
	ins.s != SPS_NONE)
      return 0;
  }
  ins = ir_load_acq(&ir[ARM64_SIDE_R_GT]);
  if (ins.r != RID_INIT || ins.s != SPS_NONE)
    return 0;
  ins = ir_load_acq(&ir[ARM64_SIDE_R_XPOLL]);
  if (ins.r != RID_INIT || ins.s != SPS_NONE)
    return 0;
  for (ref = ARM64_SIDE_K_ADDEND; ref <= REF_NIL; ref++) {
    ins = ir_load_acq(&ir[ref]);
    if (ins.r != RID_INIT || ins.s != SPS_NONE)
      return 0;
  }
  if (semantic_ninsp)
    *semantic_ninsp = ARM64_SIDE_SEMANTIC_NINS;
  return 1;
}

int lj_asm_arm64_side_postra_admit(const LJArm64SidePostRAView *view,
	IRRef *semantic_ninsp)
{
  const LJArm64SideShape *shape;
  IRRef semantic_nins;
  MSize headidx;
  MCode vmstore;
  if (!lj_asm_arm64_side_prehead_admit(view, &semantic_nins) ||
	(shape = lj_asm_arm64_side_shape(view->semantic.exitno)) == NULL ||
	view->entry == NULL ||
	((uintptr_t)(const void *)view->entry & (sizeof(MCode)-1u)) != 0 ||
	view->branch_track != (uint8_t)LJ_ABI_BRANCH_TRACK)
    return 0;
  headidx = (MSize)LJ_ABI_BRANCH_TRACK;
  vmstore = A64I_STRw | A64F_D(RID_TMP) | A64F_N(RID_DISPATCH) |
	    A64F_U12((uint32_t)DISPATCH_TG(vmstate) >> 2);
  if (view->entry_words < headidx+4u ||
	(view->branch_track && view->entry[0] != A64I_LE(A64I_BTI_J)) ||
	view->entry[headidx] != A64I_LE(A64I_MOVx |
				     A64F_D(shape->sload_reg) |
				     A64F_M(shape->inherited_reg)) ||
	view->entry[headidx+1u] != A64I_LE(A64I_MOVZw |
				     A64F_U16(view->semantic.traceno) |
				     A64F_D(RID_TMP)) ||
	view->entry[headidx+2u] != A64I_LE(A64I_DMB_ISH) ||
	view->entry[headidx+3u] != A64I_LE(vmstore))
    return 0;
  if (semantic_ninsp)
    *semantic_ninsp = semantic_nins;
  return 1;
}

static int arm64_ir_funcf_snapshots(const SnapShot *snap,
	const SnapEntry *snapmap, MSize nsnap, MSize nsnapmap,
	const BCIns *proto_bc, MSize proto_sizebc, MSize root_topslot,
	uint8_t base_delta)
{
  MSize result_slot;
  SnapEntry pcraw[1+LJ_FR2];
  uint64_t pcbase;
  uintptr_t lo, hi, snappc;
  MSize snappos;
  const SnapShot *s0, *s1;
  if (snap == NULL || snapmap == NULL || nsnap != 2 || nsnapmap != 5 ||
	root_topslot == 0 || root_topslot > UINT8_MAX || base_delta != 0)
    return 0;
  lo = (uintptr_t)proto_bc;
  LJ_STATIC_ASSERT((sizeof(BCIns) & (sizeof(BCIns)-1)) == 0);
  if (proto_sizebc != 3 || lo == 0 ||
	(lo & (sizeof(BCIns)-1)) != 0 ||
	(uintptr_t)proto_sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns))
    return 0;
  hi = lo+(uintptr_t)proto_sizebc*sizeof(BCIns);
  if (hi <= lo)
    return 0;
  result_slot = root_topslot+LJ_FR2;
  if (result_slot > UINT8_MAX)
    return 0;
  s0 = &snap[0];
  s1 = &snap[1];
  if (snap_ref_acq(s0) != REF_BASE+1u || snap_mapofs_acq(s0) != 0 ||
	snap_nent_acq(s0) != 0 || snap_nslots_acq(s0) != result_slot ||
	snap_topslot_acq(s0) != root_topslot ||
	snap_ref_acq(s1) != REF_BASE+2u || snap_mapofs_acq(s1) != 2 ||
	snap_nent_acq(s1) != 1 || snap_nslots_acq(s1) != result_slot+1u ||
	snap_topslot_acq(s1) != root_topslot ||
	snapentry_acq(&snapmap[2]) != SNAP(result_slot, 0, REF_TRUE))
    return 0;
  pcraw[0] = snapentry_acq(&snapmap[0]);
  pcraw[1] = snapentry_acq(&snapmap[1]);
  LJ_STATIC_ASSERT(sizeof(pcraw) == sizeof(pcbase));
  memcpy(&pcbase, pcraw, sizeof(pcbase));
  snappc = (uintptr_t)(pcbase >> 8);
  if ((uint8_t)pcbase != base_delta ||
	!arm64_ir_pcpos(snappc, lo, hi, &snappos) || snappos != 1u)
    return 0;
  pcraw[0] = snapentry_acq(&snapmap[3]);
  pcraw[1] = snapentry_acq(&snapmap[4]);
  memcpy(&pcbase, pcraw, sizeof(pcbase));
  snappc = (uintptr_t)(pcbase >> 8);
  return (uint8_t)pcbase == base_delta &&
	 arm64_ir_pcpos(snappc, lo, hi, &snappos) && snappos == 2u;
}

static int arm64_ir_funcf_shape(const jit_State *J, const GCtrace *T,
	const GCproto *pt, LJArm64IRReject *reject)
{
  const IRIns *ir = T->ir;
  if (J->loopref != 0 || T->nins != REF_BASE+3u ||
	T->nk != REF_TRUE ||
	ir[REF_BASE].o != IR_BASE || ir[REF_BASE].t.irt != IRT_PGC ||
	ir[REF_BASE].op1 != 0 || ir[REF_BASE].op2 != 0 ||
	ir[REF_BASE+1u].o != IR_NOP ||
	ir[REF_BASE+1u].t.irt != IRT_NIL ||
	ir[REF_BASE+1u].op1 != 0 || ir[REF_BASE+1u].op2 != 0 ||
	ir[REF_BASE+2u].o != IR_XPOLL ||
	ir[REF_BASE+2u].t.irt != (IRT_NIL|IRT_GUARD) ||
	ir[REF_BASE+2u].op1 != 1 || ir[REF_BASE+2u].op2 != 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPCODE,
			   REF_BASE, IR_XPOLL, (uint16_t)bc_op(T->startins));
  if (!arm64_ir_funcf_snapshots(T->snap, T->snapmap, T->nsnap,
	T->nsnapmap, proto_bc(pt), pt->sizebc, pt->framesize,
	(uint8_t)(J->baseslot-2u)))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
			   REF_BASE+2u, IR_XPOLL, 0);
  return 1;
}

static int arm64_ir_int_binary(const GCtrace *T, const IRIns *ir,
	IRRef before, int allow_add)
{
  return arm64_ir_int_ref(T, ir->op1, before, allow_add) &&
	 arm64_ir_int_ref(T, ir->op2, before, allow_add);
}

static int arm64_ir_num_binary(const GCtrace *T, const IRIns *ir,
	IRRef before, int allow_sub, int allow_mul, int allow_div)
{
  return arm64_ir_num_ref(T, ir->op1, before, allow_sub, allow_mul,
	allow_div) &&
	 arm64_ir_num_ref(T, ir->op2, before, allow_sub, allow_mul,
	allow_div);
}

static int arm64_ir_num_add_binary(const GCtrace *T, const IRIns *ir,
	IRRef before)
{
  return arm64_ir_num_add_ref(T, ir->op1, before) &&
	 arm64_ir_num_add_ref(T, ir->op2, before);
}

/* First spill-free NUM execution canary. Integer induction keeps every
** comparison and overflow edge in the already-certified family, while a
** dynamic NUM accumulator proves FPR loads, ADD, PHI, snapshots and exits.
** Constants remain KINT-only and no numeric conversion is needed. */
static int arm64_ir_numadd_shape(const GCtrace *T, IRRef firstphi,
	LJArm64IRReject *reject)
{
  const IRIns *ir = T->ir;
  if (T->nk != ARM64_NUMADD_K_ONE ||
	T->nins != ARM64_NUMADD_SEMANTIC_NINS ||
	firstphi != ARM64_NUMADD_R_I_PHI ||
	ir[ARM64_NUMADD_K_ONE].o != IR_KINT ||
	ir[ARM64_NUMADD_K_ONE].t.irt != IRT_INT ||
	ir[ARM64_NUMADD_K_ONE].i != 1)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE,
	ARM64_NUMADD_R_I, IR_ADD, 1);
#define ARM64_NUMADD_INS(ref, op, left, right) \
  (ir[(ref)].o == (op) && ir[(ref)].op1 == (left) && \
   ir[(ref)].op2 == (right))
  if (!ARM64_NUMADD_INS(ARM64_NUMADD_R_I, IR_SLOAD, 5,
	IRSLOAD_TYPECHECK) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_X, IR_SLOAD, 3,
	IRSLOAD_TYPECHECK) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_STEP, IR_SLOAD, 4,
	IRSLOAD_TYPECHECK) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_I_PRE, IR_ADDOV,
	ARM64_NUMADD_R_I, ARM64_NUMADD_K_ONE) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_X_PRE, IR_ADD,
	ARM64_NUMADD_R_STEP, ARM64_NUMADD_R_X) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_LIMIT, IR_SLOAD, 2,
	IRSLOAD_TYPECHECK) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_PRE_GUARD, IR_GT,
	ARM64_NUMADD_R_LIMIT, ARM64_NUMADD_R_I_PRE) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_LOOP, IR_LOOP, 0, 0) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_XPOLL, IR_XPOLL, 1, 0) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_I_BODY, IR_ADDOV,
	ARM64_NUMADD_R_I_PRE, ARM64_NUMADD_K_ONE) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_X_BODY, IR_ADD,
	ARM64_NUMADD_R_X_PRE, ARM64_NUMADD_R_STEP) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_BODY_GUARD, IR_LT,
	ARM64_NUMADD_R_I_BODY, ARM64_NUMADD_R_LIMIT) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_I_PHI, IR_PHI,
	ARM64_NUMADD_R_I_PRE, ARM64_NUMADD_R_I_BODY) ||
      !ARM64_NUMADD_INS(ARM64_NUMADD_R_X_PHI, IR_PHI,
	ARM64_NUMADD_R_X_PRE, ARM64_NUMADD_R_X_BODY)) {
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
	ARM64_NUMADD_R_I, IR_ADD, 2);
  }
#undef ARM64_NUMADD_INS
  return 1;
}

static int arm64_ir_numhalf_bytecode(const GCproto *pt,
	const BCIns *startpc)
{
  const BCIns *bc;
  BCIns ins;
  if (pt == NULL || startpc == NULL || pt->framesize != 4 ||
	pt->sizebc != 13 || pt->numparams != 1 || pt->sizeuv != 0 ||
	pt->sizekn != 1 || pt->sizekgc != 0 ||
	proto_knumtv(pt, 0)->u64 != ARM64_NUMHALF_BITS)
    return 0;
  bc = proto_bc(pt);
  if (startpc != bc+6)
    return 0;
#define ARM64_NUMHALF_BC_AD(pos, op, a, d) \
  (bc_op((ins = arm64_ir_bc_acq((uintptr_t)bc, (pos)))) == (op) && \
   bc_a(ins) == (a) && bc_d(ins) == (d))
  if (!ARM64_NUMHALF_BC_AD(0, BC_FUNCF, 4, 0) ||
      !ARM64_NUMHALF_BC_AD(1, BC_KNUM, 1, 0) ||
      !ARM64_NUMHALF_BC_AD(2, BC_CGET, 2, 1) ||
      !ARM64_NUMHALF_BC_AD(3, BC_CGET, 3, 0) ||
      !ARM64_NUMHALF_BC_AD(4, BC_ISGE, 2, 3) ||
      !ARM64_NUMHALF_BC_AD(11, BC_CGET, 2, 1) ||
      !ARM64_NUMHALF_BC_AD(12, BC_RET1, 2, 2))
    return 0;
#undef ARM64_NUMHALF_BC_AD
  ins = arm64_ir_bc_acq((uintptr_t)bc, 5);
  if (bc_op(ins) != BC_JMP || bc_a(ins) != 2 || bc_j(ins) != 5)
    return 0;
  ins = arm64_ir_bc_acq((uintptr_t)bc, 6);
  if (bc_op(ins) != BC_LOOP || bc_a(ins) != 2 || bc_j(ins) != 4)
    return 0;
  ins = arm64_ir_bc_acq((uintptr_t)bc, 7);
  if (bc_op(ins) != BC_CGET || bc_a(ins) != 2 || bc_d(ins) != 1)
    return 0;
  ins = arm64_ir_bc_acq((uintptr_t)bc, 8);
  if (bc_op(ins) != BC_ADDVN || bc_a(ins) != 2 ||
	bc_b(ins) != 2 || bc_c(ins) != 0)
    return 0;
  ins = arm64_ir_bc_acq((uintptr_t)bc, 9);
  if (bc_op(ins) != BC_CSET || bc_a(ins) != 1 || bc_d(ins) != 2)
    return 0;
  ins = arm64_ir_bc_acq((uintptr_t)bc, 10);
  return bc_op(ins) == BC_JMP && bc_a(ins) == 2 && bc_j(ins) == -9;
}

/* Exact pure-NUM canary for canonical KNUM loading and ordered FP guards. */
static int arm64_ir_numhalf_shape(const jit_State *J, const GCtrace *T,
	const GCproto *pt, IRRef firstphi, LJArm64IRReject *reject)
{
  const IRIns *ir = T->ir;
  if (T->nk != ARM64_NUMHALF_K_HALF ||
	T->nins != ARM64_NUMHALF_SEMANTIC_NINS ||
	firstphi != ARM64_NUMHALF_R_X_PHI ||
	!arm64_ir_numhalf_constant(ir, T->nk) ||
	!arm64_ir_numhalf_bytecode(pt, trace_startpc_acq((GCtrace *)T)) ||
	!arm64_numhalf_snapshots(T->snap, T->snapmap, T->nsnap,
	  T->nsnapmap, proto_bc(pt), pt->sizebc,
	  (uint8_t)(J->baseslot-2u)))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE,
	ARM64_NUMHALF_R_X, IR_ADD, 3);
#define ARM64_NUMHALF_INS(ref, op, type, left, right) \
  (ir[(ref)].o == (op) && ir[(ref)].t.irt == (type) && \
   ir[(ref)].op1 == (left) && ir[(ref)].op2 == (right))
  if (!ARM64_NUMHALF_INS(ARM64_NUMHALF_R_X, IR_SLOAD,
	IRT_NUM|IRT_GUARD, 3, IRSLOAD_TYPECHECK) ||
      !ARM64_NUMHALF_INS(ARM64_NUMHALF_R_X_PRE, IR_ADD,
	IRT_NUM|IRT_ISPHI, ARM64_NUMHALF_R_X, ARM64_NUMHALF_K_HALF) ||
      !ARM64_NUMHALF_INS(ARM64_NUMHALF_R_LIMIT, IR_SLOAD,
	IRT_NUM|IRT_GUARD, 2, IRSLOAD_TYPECHECK) ||
      !ARM64_NUMHALF_INS(ARM64_NUMHALF_R_PRE_GUARD, IR_GT,
	IRT_NUM|IRT_GUARD, ARM64_NUMHALF_R_LIMIT, ARM64_NUMHALF_R_X_PRE) ||
      !ARM64_NUMHALF_INS(ARM64_NUMHALF_R_LOOP, IR_LOOP,
	IRT_NIL|IRT_GUARD, 0, 0) ||
      !ARM64_NUMHALF_INS(ARM64_NUMHALF_R_XPOLL, IR_XPOLL,
	IRT_NIL|IRT_GUARD, 1, 0) ||
      !ARM64_NUMHALF_INS(ARM64_NUMHALF_R_X_BODY, IR_ADD,
	IRT_NUM|IRT_ISPHI, ARM64_NUMHALF_R_X_PRE,
	ARM64_NUMHALF_K_HALF) ||
      !ARM64_NUMHALF_INS(ARM64_NUMHALF_R_BODY_GUARD, IR_LT,
	IRT_NUM|IRT_GUARD, ARM64_NUMHALF_R_X_BODY,
	ARM64_NUMHALF_R_LIMIT) ||
      !ARM64_NUMHALF_INS(ARM64_NUMHALF_R_X_PHI, IR_PHI,
	IRT_NUM, ARM64_NUMHALF_R_X_PRE, ARM64_NUMHALF_R_X_BODY))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
	ARM64_NUMHALF_R_X, IR_ADD, 4);
#undef ARM64_NUMHALF_INS
  return 1;
}

static int arm64_ir_numstep_bytecode(const GCproto *pt,
	const BCIns *startpc)
{
  const BCIns *bc;
  BCIns ins;
  if (pt == NULL || startpc == NULL || pt->framesize != 5 ||
	pt->sizebc != 14 || pt->numparams != 2 || pt->sizeuv != 0 ||
	pt->sizekn != 1 || pt->sizekgc != 0 ||
	proto_knumtv(pt, 0)->u64 != ARM64_NUMHALF_BITS)
    return 0;
  bc = proto_bc(pt);
  if (startpc != bc+6)
    return 0;
#define ARM64_NUMSTEP_BC_AD(pos, op, a, d) \
  (bc_op((ins = arm64_ir_bc_acq((uintptr_t)bc, (pos)))) == (op) && \
   bc_a(ins) == (a) && bc_d(ins) == (d))
  if (!ARM64_NUMSTEP_BC_AD(0, BC_FUNCF, 5, 0) ||
      !ARM64_NUMSTEP_BC_AD(1, BC_KNUM, 2, 0) ||
      !ARM64_NUMSTEP_BC_AD(2, BC_CGET, 3, 2) ||
      !ARM64_NUMSTEP_BC_AD(3, BC_CGET, 4, 0) ||
      !ARM64_NUMSTEP_BC_AD(4, BC_ISGE, 3, 4) ||
      !ARM64_NUMSTEP_BC_AD(7, BC_CGET, 3, 2) ||
      !ARM64_NUMSTEP_BC_AD(8, BC_CGET, 4, 1) ||
      !ARM64_NUMSTEP_BC_AD(10, BC_CSET, 2, 3) ||
      !ARM64_NUMSTEP_BC_AD(12, BC_CGET, 3, 2) ||
      !ARM64_NUMSTEP_BC_AD(13, BC_RET1, 3, 2))
    return 0;
#undef ARM64_NUMSTEP_BC_AD
  ins = arm64_ir_bc_acq((uintptr_t)bc, 5);
  if (bc_op(ins) != BC_JMP || bc_a(ins) != 3 || bc_j(ins) != 6)
    return 0;
  ins = arm64_ir_bc_acq((uintptr_t)bc, 6);
  if (bc_op(ins) != BC_LOOP || bc_a(ins) != 3 || bc_j(ins) != 5)
    return 0;
  ins = arm64_ir_bc_acq((uintptr_t)bc, 9);
  if (bc_op(ins) != BC_ADDVV || bc_a(ins) != 3 ||
	bc_b(ins) != 3 || bc_c(ins) != 4)
    return 0;
  ins = arm64_ir_bc_acq((uintptr_t)bc, 11);
  return bc_op(ins) == BC_JMP && bc_a(ins) == 3 && bc_j(ins) == -10;
}

static int arm64_ir_numacc_bytecode(const GCproto *pt,
	const BCIns *startpc, unsigned *grammar_profile)
{
  const BCIns *bc;
  BCIns ins;
  unsigned profile;
  if (pt == NULL || startpc == NULL || grammar_profile == NULL ||
	pt->framesize != 5 ||
	pt->sizebc != 13 || pt->numparams != 3 || pt->sizeuv != 0 ||
	pt->sizekn != 0 || pt->sizekgc != 0 ||
	pt->flags2 != PROTO2_CELLOPS)
    return 0;
  bc = proto_bc(pt);
  if (startpc != bc+5)
    return 0;
  profile = arm64_numacc_grammar_profile(bc, pt->sizebc);
  if (profile == 0)
    return 0;
#define ARM64_NUMACC_BC_AD(pos, op, a, d) \
  (bc_op((ins = arm64_ir_bc_acq((uintptr_t)bc, (pos)))) == (op) && \
   bc_a(ins) == (a) && bc_d(ins) == (d))
  if (!ARM64_NUMACC_BC_AD(0, BC_FUNCF, 5, 0) ||
      !ARM64_NUMACC_BC_AD(1, BC_CGET, 3, 0) ||
      !ARM64_NUMACC_BC_AD(2, BC_CGET, 4, 1) ||
      !ARM64_NUMACC_BC_AD(6, BC_CGET, 3, 0) ||
      !ARM64_NUMACC_BC_AD(7, BC_CGET, 4, 2) ||
      !ARM64_NUMACC_BC_AD(9, BC_CSET, 0, 3) ||
      !ARM64_NUMACC_BC_AD(11, BC_CGET, 3, 0) ||
      !ARM64_NUMACC_BC_AD(12, BC_RET1, 3, 2))
    return 0;
#undef ARM64_NUMACC_BC_AD
  ins = arm64_ir_bc_acq((uintptr_t)bc, 4);
  if (bc_op(ins) != BC_JMP || bc_a(ins) != 3 || bc_j(ins) != 6)
    return 0;
  ins = arm64_ir_bc_acq((uintptr_t)bc, 5);
  if (bc_op(ins) != BC_LOOP || bc_a(ins) != 3 || bc_j(ins) != 5)
    return 0;
  ins = arm64_ir_bc_acq((uintptr_t)bc, 10);
  if (bc_op(ins) != BC_JMP || bc_a(ins) != 3 || bc_j(ins) != -10 ||
	arm64_numacc_grammar_profile(bc, pt->sizebc) != profile)
    return 0;
  *grammar_profile = profile;
  return 1;
}

static int arm64_ir_numdynamic_kernel(const GCtrace *T, IRRef xslot,
	IRRef stepslot, IRRef limitslot, unsigned grammar_profile,
	unsigned args_kind)
{
  const IRIns *ir = T->ir;
  IRRef xintref, xref, stepintref, stepref, xpreref;
  IRRef limitintref, limitref, preguardref;
  IRRef loopref, xpollref, xcheckref, xbodyref, bodyguardref, xphiref;
  IRRef first_left, first_right;
  IROp recurrence_op, preop, bodyop;
  if (args_kind == ARM64_NUMDYN_ARGS_NUM) {
    xintref = 0;
    xref = ARM64_NUMSTEP_R_X;
    stepintref = 0;
    stepref = ARM64_NUMSTEP_R_STEP;
    xpreref = ARM64_NUMSTEP_R_X_PRE;
    limitintref = 0;
    limitref = ARM64_NUMSTEP_R_LIMIT;
    preguardref = ARM64_NUMSTEP_R_PRE_GUARD;
    loopref = ARM64_NUMSTEP_R_LOOP;
    xpollref = ARM64_NUMSTEP_R_XPOLL;
    xcheckref = 0;
    xbodyref = ARM64_NUMSTEP_R_X_BODY;
    bodyguardref = ARM64_NUMSTEP_R_BODY_GUARD;
    xphiref = ARM64_NUMSTEP_R_X_PHI;
  } else if (args_kind == ARM64_NUMDYN_ARGS_INT_STEP) {
    xintref = 0;
    xref = ARM64_NUMACC_INTSTEP_R_X;
    stepintref = ARM64_NUMACC_INTSTEP_R_STEP_INT;
    stepref = ARM64_NUMACC_INTSTEP_R_STEP_NUM;
    xpreref = ARM64_NUMACC_INTSTEP_R_X_PRE;
    limitintref = 0;
    limitref = ARM64_NUMACC_INTSTEP_R_LIMIT;
    preguardref = ARM64_NUMACC_INTSTEP_R_PRE_GUARD;
    loopref = ARM64_NUMACC_INTSTEP_R_LOOP;
    xpollref = ARM64_NUMACC_INTSTEP_R_XPOLL;
    xcheckref = 0;
    xbodyref = ARM64_NUMACC_INTSTEP_R_X_BODY;
    bodyguardref = ARM64_NUMACC_INTSTEP_R_BODY_GUARD;
    xphiref = ARM64_NUMACC_INTSTEP_R_X_PHI;
  } else if (args_kind == ARM64_NUMDYN_ARGS_INT_LIMIT &&
	grammar_profile == ARM64_NUMDYN_ADD_LT) {
    xintref = 0;
    xref = ARM64_NUMACC_INTLIMIT_R_X;
    stepintref = 0;
    stepref = ARM64_NUMACC_INTLIMIT_R_STEP;
    xpreref = ARM64_NUMACC_INTLIMIT_R_X_PRE;
    limitintref = ARM64_NUMACC_INTLIMIT_R_LIMIT_INT;
    limitref = ARM64_NUMACC_INTLIMIT_R_LIMIT_NUM;
    preguardref = ARM64_NUMACC_INTLIMIT_R_PRE_GUARD;
    loopref = ARM64_NUMACC_INTLIMIT_R_LOOP;
    xpollref = ARM64_NUMACC_INTLIMIT_R_XPOLL;
    xcheckref = 0;
    xbodyref = ARM64_NUMACC_INTLIMIT_R_X_BODY;
    bodyguardref = ARM64_NUMACC_INTLIMIT_R_BODY_GUARD;
    xphiref = ARM64_NUMACC_INTLIMIT_R_X_PHI;
  } else if (args_kind == ARM64_NUMDYN_ARGS_INT_X) {
    xintref = ARM64_NUMACC_INTX_R_X_INT;
    xref = ARM64_NUMACC_INTX_R_X_NUM;
    stepintref = 0;
    stepref = ARM64_NUMACC_INTX_R_STEP;
    xpreref = ARM64_NUMACC_INTX_R_X_PRE;
    limitintref = 0;
    limitref = ARM64_NUMACC_INTX_R_LIMIT;
    preguardref = ARM64_NUMACC_INTX_R_PRE_GUARD;
    loopref = ARM64_NUMACC_INTX_R_LOOP;
    xpollref = ARM64_NUMACC_INTX_R_XPOLL;
    xcheckref = ARM64_NUMACC_INTX_R_X_CHECK;
    xbodyref = ARM64_NUMACC_INTX_R_X_BODY;
    bodyguardref = ARM64_NUMACC_INTX_R_BODY_GUARD;
    xphiref = ARM64_NUMACC_INTX_R_X_PHI;
  } else {
    return 0;
  }
  if (grammar_profile == ARM64_NUMDYN_ADD_LT) {
    recurrence_op = IR_ADD;
    first_left = stepref;
    first_right = xref;
    preop = IR_GT;
    bodyop = IR_LT;
  } else if (grammar_profile == ARM64_NUMDYN_ADD_LE) {
    recurrence_op = IR_ADD;
    first_left = stepref;
    first_right = xref;
    preop = IR_GE;
    bodyop = IR_LE;
  } else if (grammar_profile == ARM64_NUMDYN_ADD_GT) {
    recurrence_op = IR_ADD;
    first_left = stepref;
    first_right = xref;
    preop = IR_LT;
    bodyop = IR_GT;
  } else if (grammar_profile == ARM64_NUMDYN_ADD_GE) {
    recurrence_op = IR_ADD;
    first_left = stepref;
    first_right = xref;
    preop = IR_LE;
    bodyop = IR_GE;
  } else if (grammar_profile == ARM64_NUMDYN_SUB_GT) {
    recurrence_op = IR_SUB;
    first_left = xref;
    first_right = stepref;
    preop = IR_LT;
    bodyop = IR_GT;
  } else if (grammar_profile == ARM64_NUMDYN_SUB_GE) {
    recurrence_op = IR_SUB;
    first_left = xref;
    first_right = stepref;
    preop = IR_LE;
    bodyop = IR_GE;
  } else if (grammar_profile == ARM64_NUMDYN_MUL_LT) {
    recurrence_op = IR_MUL;
    first_left = stepref;
    first_right = xref;
    preop = IR_GT;
    bodyop = IR_LT;
  } else if (grammar_profile == ARM64_NUMDYN_MUL_LE) {
    recurrence_op = IR_MUL;
    first_left = stepref;
    first_right = xref;
    preop = IR_GE;
    bodyop = IR_LE;
  } else if (grammar_profile == ARM64_NUMDYN_DIV_LT) {
    recurrence_op = IR_DIV;
    first_left = xref;
    first_right = stepref;
    preop = IR_GT;
    bodyop = IR_LT;
  } else if (grammar_profile == ARM64_NUMDYN_DIV_LE) {
    recurrence_op = IR_DIV;
    first_left = xref;
    first_right = stepref;
    preop = IR_GE;
    bodyop = IR_LE;
  } else if (grammar_profile == ARM64_NUMDYN_DIV_GT) {
    recurrence_op = IR_DIV;
    first_left = xref;
    first_right = stepref;
    preop = IR_LT;
    bodyop = IR_GT;
  } else if (grammar_profile == ARM64_NUMDYN_DIV_GE) {
    recurrence_op = IR_DIV;
    first_left = xref;
    first_right = stepref;
    preop = IR_LE;
    bodyop = IR_GE;
  } else {
    return 0;
  }
  if (args_kind == ARM64_NUMDYN_ARGS_INT_X) {
    first_left = xref;
    first_right = stepref;
  }
#define ARM64_NUMDYN_INS(ref, op, type, left, right) \
  (ir[(ref)].o == (op) && ir[(ref)].t.irt == (type) && \
   ir[(ref)].op1 == (left) && ir[(ref)].op2 == (right))
  if ((args_kind != ARM64_NUMDYN_ARGS_INT_X ?
       !ARM64_NUMDYN_INS(xref, IR_SLOAD,
	 IRT_NUM|IRT_GUARD, xslot, IRSLOAD_TYPECHECK) :
       (!ARM64_NUMDYN_INS(xintref, IR_SLOAD,
	  IRT_INT|IRT_GUARD, xslot, IRSLOAD_TYPECHECK) ||
	!ARM64_NUMDYN_INS(xref, IR_CONV,
	  IRT_NUM, xintref, IRCONV_NUM_INT))) ||
      (args_kind != ARM64_NUMDYN_ARGS_INT_STEP ?
       !ARM64_NUMDYN_INS(stepref, IR_SLOAD,
	 IRT_NUM|IRT_GUARD, stepslot, IRSLOAD_TYPECHECK) :
       (!ARM64_NUMDYN_INS(stepintref, IR_SLOAD,
	  IRT_INT|IRT_GUARD, stepslot, IRSLOAD_TYPECHECK) ||
	!ARM64_NUMDYN_INS(stepref, IR_CONV,
	  IRT_NUM, stepintref, IRCONV_NUM_INT))) ||
      !ARM64_NUMDYN_INS(xpreref, recurrence_op,
	IRT_NUM|IRT_ISPHI, first_left, first_right) ||
      (args_kind != ARM64_NUMDYN_ARGS_INT_LIMIT ?
       !ARM64_NUMDYN_INS(limitref, IR_SLOAD,
	 IRT_NUM|IRT_GUARD, limitslot, IRSLOAD_TYPECHECK) :
       (!ARM64_NUMDYN_INS(limitintref, IR_SLOAD,
	  IRT_INT|IRT_GUARD, limitslot, IRSLOAD_TYPECHECK) ||
	!ARM64_NUMDYN_INS(limitref, IR_CONV,
	  IRT_NUM, limitintref, IRCONV_NUM_INT))) ||
      !ARM64_NUMDYN_INS(preguardref, preop,
	IRT_NUM|IRT_GUARD, limitref, xpreref) ||
      !ARM64_NUMDYN_INS(loopref, IR_LOOP,
	IRT_NIL|IRT_GUARD, 0, 0) ||
      !ARM64_NUMDYN_INS(xpollref, IR_XPOLL,
	IRT_NIL|IRT_GUARD, 1, 0) ||
      (args_kind == ARM64_NUMDYN_ARGS_INT_X &&
       !ARM64_NUMDYN_INS(xcheckref, IR_CONV,
	 IRT_INT|IRT_GUARD, xpreref, IRCONV_INT_NUM|IRCONV_CHECK)) ||
      !ARM64_NUMDYN_INS(xbodyref, recurrence_op,
	IRT_NUM|IRT_ISPHI, xpreref, stepref) ||
      !ARM64_NUMDYN_INS(bodyguardref, bodyop,
	IRT_NUM|IRT_GUARD, xbodyref, limitref) ||
      !ARM64_NUMDYN_INS(xphiref, IR_PHI,
	IRT_NUM, xpreref, xbodyref))
    return 0;
#undef ARM64_NUMDYN_INS
  return 1;
}

/* Exact pure-NUM canary for one invariant dynamic NUM step. */
static int arm64_ir_numstep_shape(const jit_State *J, const GCtrace *T,
	const GCproto *pt, IRRef firstphi, LJArm64IRReject *reject)
{
  if (T->nk != REF_TRUE || T->nins != ARM64_NUMSTEP_SEMANTIC_NINS ||
	firstphi != ARM64_NUMSTEP_R_X_PHI ||
	!arm64_ir_numstep_bytecode(pt, trace_startpc_acq((GCtrace *)T)) ||
	!arm64_numstep_snapshots(T->snap, T->snapmap, T->nsnap,
	  T->nsnapmap, proto_bc(pt), pt->sizebc,
	  (uint8_t)(J->baseslot-2u)))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE,
	ARM64_NUMSTEP_R_X, IR_ADD, 5);
  if (!arm64_ir_numdynamic_kernel(T, 4, 3, 2, ARM64_NUMDYN_ADD_LT,
	ARM64_NUMDYN_ARGS_NUM))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
	ARM64_NUMSTEP_R_X, IR_ADD, 6);
  return 1;
}

/* Exact all-parameter NUM accumulator canary for admitted argument kinds. */
static int arm64_ir_numacc_shape(const jit_State *J, const GCtrace *T,
	const GCproto *pt, IRRef firstphi, unsigned args_kind,
	LJArm64IRReject *reject)
{
  unsigned grammar_profile = arm64_numacc_grammar_profile(
	proto_bc(pt), pt->sizebc);
  IRRef semantic_nins = args_kind == ARM64_NUMDYN_ARGS_NUM ?
	ARM64_NUMACC_SEMANTIC_NINS :
	args_kind == ARM64_NUMDYN_ARGS_INT_STEP ?
	ARM64_NUMACC_INTSTEP_SEMANTIC_NINS :
	args_kind == ARM64_NUMDYN_ARGS_INT_LIMIT ?
	ARM64_NUMACC_INTLIMIT_SEMANTIC_NINS :
	ARM64_NUMACC_INTX_SEMANTIC_NINS;
  IRRef xref = args_kind == ARM64_NUMDYN_ARGS_NUM ?
	ARM64_NUMACC_R_X :
	args_kind == ARM64_NUMDYN_ARGS_INT_STEP ?
	ARM64_NUMACC_INTSTEP_R_X :
	args_kind == ARM64_NUMDYN_ARGS_INT_LIMIT ?
	ARM64_NUMACC_INTLIMIT_R_X : ARM64_NUMACC_INTX_R_X_INT;
  IRRef xphiref = args_kind == ARM64_NUMDYN_ARGS_NUM ?
	ARM64_NUMACC_R_X_PHI :
	args_kind == ARM64_NUMDYN_ARGS_INT_STEP ?
	ARM64_NUMACC_INTSTEP_R_X_PHI :
	args_kind == ARM64_NUMDYN_ARGS_INT_LIMIT ?
	ARM64_NUMACC_INTLIMIT_R_X_PHI : ARM64_NUMACC_INTX_R_X_PHI;
  IROp recurrence_op = arm64_numdynamic_is_sub(grammar_profile) ? IR_SUB :
	arm64_numdynamic_is_mul(grammar_profile) ? IR_MUL :
	arm64_numdynamic_is_div(grammar_profile) ? IR_DIV : IR_ADD;
  if ((args_kind != ARM64_NUMDYN_ARGS_NUM &&
	args_kind != ARM64_NUMDYN_ARGS_INT_STEP &&
	args_kind != ARM64_NUMDYN_ARGS_INT_LIMIT &&
	args_kind != ARM64_NUMDYN_ARGS_INT_X) ||
	(args_kind == ARM64_NUMDYN_ARGS_INT_LIMIT &&
	 grammar_profile != ARM64_NUMDYN_ADD_LT) ||
	T->nk != REF_TRUE || T->nins != semantic_nins ||
	firstphi != xphiref ||
	!arm64_ir_numacc_bytecode(pt, trace_startpc_acq((GCtrace *)T),
	  &grammar_profile) ||
	!arm64_numacc_snapshots(T->snap, T->snapmap, T->nsnap,
	  T->nsnapmap, proto_bc(pt), pt->sizebc,
	  (uint8_t)(J->baseslot-2u), args_kind))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE,
	xref, recurrence_op, 7);
  if (!arm64_ir_numdynamic_kernel(T, 2, 4, 3, grammar_profile,
	args_kind))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
	xref, recurrence_op, 8);
  return 1;
}

static int arm64_ir_kint_value(const GCtrace *T, IRRef ref, int32_t *value)
{
  if (!arm64_ir_int_kref(T, ref))
    return 0;
  *value = T->ir[ref].i;
  return 1;
}

/* Prove the exact narrowed scalar evolution emitted for an integer FORL
** root. Constant steps use a range guard for a dynamic stop. A dynamic step
** and stop use the recorder's direction guard plus a live ADDOV proof. */
static int arm64_ir_forl_shape(const jit_State *J, const GCtrace *T,
	IRRef loopref, IRRef xpollref, IRRef firstphi, int *dynamic_stepp,
	LJArm64IRReject *reject)
{
  MSize idxslot = (MSize)(1u+LJ_FR2+bc_a(T->startins));
  MSize stopslot = idxslot+FORL_STOP;
  MSize stepslot = idxslot+FORL_STEP;
  IRRef ref, preadd = 0, postadd = 0, indexphi = 0;
  IRRef idxload, stepref, stopref, stepload = 0, useref = 0;
  IRIns pre, post, precmp, postcmp, idx, stop;
  IROp cmpop;
  int32_t step = 0, stopvalue;
  unsigned nadd = 0, nstepload = 0, nuse = 0;

  UNUSED(J);
  *dynamic_stepp = 0;
  for (ref = REF_FIRST; ref < T->nins; ref++) {
    const IRIns *ir = &T->ir[ref];
    if (ir->o == IR_SLOAD && ir->op1 == stepslot) {
      nstepload++;
      stepload = ref;
    } else if (ir->o == IR_USE) {
      nuse++;
      useref = ref;
    }
    if (ir->o != IR_ADD)
      continue;
    nadd++;
    if (ref < loopref) {
      if (preadd != 0)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			       ref, IR_ADD, 1);
      preadd = ref;
    } else if (ref > xpollref && (!firstphi || ref < firstphi)) {
      if (postadd != 0)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			       ref, IR_ADD, 2);
      postadd = ref;
    } else {
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     ref, IR_ADD, 3);
    }
  }
  if (nadd != 2u || preadd == 0 || postadd == 0 ||
      preadd+1u >= loopref || postadd+1u >= (firstphi ? firstphi : T->nins))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			   preadd ? preadd : postadd, IR_ADD, (uint16_t)nadd);

  pre = T->ir[preadd];
  post = T->ir[postadd];
  precmp = T->ir[preadd+1u];
  postcmp = T->ir[postadd+1u];
  idxload = pre.op1;
  stepref = pre.op2;
  stopref = precmp.op2;
  if (idxload < REF_FIRST || idxload >= preadd ||
      (!irref_isk(stepref) &&
       (stepref < REF_FIRST || stepref >= preadd)) ||
      post.op1 != preadd || post.op2 != stepref ||
      pre.t.irt != (IRT_INT|IRT_ISPHI) ||
      post.t.irt != (IRT_INT|IRT_ISPHI))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			   preadd, IR_ADD, 4);
  idx = T->ir[idxload];
  if (idx.o != IR_SLOAD || idx.op1 != idxslot ||
      idx.op2 != (IRSLOAD_TYPECHECK|IRSLOAD_INHERIT) ||
      idx.t.irt != (IRT_INT|IRT_GUARD))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			   idxload, IR_SLOAD, idx.op2);

  if (irref_isk(stepref)) {
    if (nstepload != 0 || nuse != 0 ||
	!arm64_ir_kint_value(T, stepref, &step) || step == 0)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     stepref, IR_ADD, 5);
    cmpop = step > 0 ? IR_LE : IR_GE;
  } else {
    IRIns direction, overflow, use;
    int32_t zero;
    if (nstepload != 1u || stepload != stepref || nuse != 1u ||
	stopref < REF_FIRST || stopref+1u != stepref ||
	stepref+4u != idxload)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     stepref, IR_SLOAD, (uint16_t)nstepload);
    stop = T->ir[stopref];
    if (stop.o != IR_SLOAD || stop.op1 != stopslot ||
	stop.op2 != (IRSLOAD_READONLY|IRSLOAD_INHERIT) ||
	stop.t.irt != IRT_INT)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     stopref, IR_SLOAD, stop.op2);
    if (T->ir[stepref].o != IR_SLOAD ||
	T->ir[stepref].op1 != stepslot ||
	T->ir[stepref].op2 != (IRSLOAD_READONLY|IRSLOAD_INHERIT) ||
	T->ir[stepref].t.irt != IRT_INT)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     stepref, IR_SLOAD, T->ir[stepref].op2);
    direction = T->ir[stepref+1u];
    if ((direction.o != IR_GE && direction.o != IR_LT) ||
	direction.op1 != stepref ||
	!arm64_ir_kint_value(T, direction.op2, &zero) || zero != 0 ||
	direction.t.irt != (IRT_INT|IRT_GUARD))
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     stepref+1u, (IROp)direction.o, 6);
    cmpop = direction.o == IR_GE ? IR_LE : IR_GE;
    overflow = T->ir[stepref+2u];
    use = T->ir[stepref+3u];
    if (overflow.o != IR_ADDOV || overflow.op1 != stepref ||
	overflow.op2 != stopref ||
	overflow.t.irt != (IRT_INT|IRT_GUARD) ||
	useref != stepref+3u || use.o != IR_USE ||
	use.t.irt != IRT_INT || use.op1 != stepref+2u || use.op2 != 0)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     stepref+2u, IR_ADDOV, 7);
    *dynamic_stepp = 1;
  }

  if (precmp.o != cmpop || postcmp.o != cmpop ||
      precmp.op1 != preadd || postcmp.op1 != postadd ||
      postcmp.op2 != stopref ||
      precmp.t.irt != (IRT_INT|IRT_GUARD) ||
      postcmp.t.irt != (IRT_INT|IRT_GUARD))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			   preadd+1u, cmpop, 8);

  if (!*dynamic_stepp) {
    if (irref_isk(stopref)) {
      int64_t sum;
      if (!arm64_ir_kint_value(T, stopref, &stopvalue))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT,
			       stopref, IR_KINT, 0);
      sum = (int64_t)stopvalue + (int64_t)step;
      if (sum < INT32_MIN || sum > INT32_MAX)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			       stopref, IR_ADD, 9);
    } else {
      IRRef guardref;
      IRIns boundguard;
      int32_t boundvalue, expected;
      int64_t expected64;
      if (stopref < REF_FIRST || stopref >= preadd)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			       stopref, IR_SLOAD, 10);
      stop = T->ir[stopref];
      if (stop.o != IR_SLOAD || stop.op1 != stopslot ||
	  stop.op2 != (IRSLOAD_READONLY|IRSLOAD_INHERIT) ||
	  stop.t.irt != IRT_INT)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			       stopref, IR_SLOAD, stop.op2);
      guardref = stopref+1u;
      if (guardref >= preadd)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			       guardref, cmpop, 11);
      boundguard = T->ir[guardref];
      expected64 = step > 0 ? (int64_t)INT32_MAX-(int64_t)step :
				   (int64_t)INT32_MIN-(int64_t)step;
      if (expected64 < INT32_MIN || expected64 > INT32_MAX)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			       guardref, cmpop, 12);
      expected = (int32_t)expected64;
      if (boundguard.o != cmpop || boundguard.op1 != stopref ||
	  !arm64_ir_kint_value(T, boundguard.op2, &boundvalue) ||
	  boundvalue != expected ||
	  boundguard.t.irt != (IRT_INT|IRT_GUARD))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			       guardref, cmpop, 13);
    }
  }

  for (ref = firstphi; ref && ref < T->nins; ref++) {
    const IRIns *phi = &T->ir[ref];
    if (phi->op1 == preadd && phi->op2 == postadd) {
      if (indexphi != 0)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE,
			       ref, IR_PHI, 14);
      indexphi = ref;
    }
  }
  if (indexphi == 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE,
			   preadd, IR_PHI, 15);
  return 1;
}

static int arm64_ir_snapshots(const GCtrace *T, IRRef loopref,
	IRRef xpollref, MSize root_topslot, const jit_State *J,
	uintptr_t proto_lo, uintptr_t proto_hi, BCOp rootop,
	unsigned scalar_mode, int allow_num_sub, int allow_num_mul,
	int allow_num_div, int allow_forl_step,
	LJArm64IRReject *reject)
{
  SnapNo snapno;
  IRRef prevref = 0;
  MSize expected_mapofs = 0;
  int loopsnap = 0;
  IRRef xpollsnap = 0;
  LJ_STATIC_ASSERT(sizeof(SnapEntry)*(1+LJ_FR2) == sizeof(uint64_t));
  if (T->nsnap == 0 || T->snap == NULL || T->snapmap == NULL)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
			   loopref, IR_XPOLL, 0);
  for (snapno = 0; snapno < T->nsnap; snapno++) {
    const SnapShot *snap = &T->snap[snapno];
    MSize mapofs = snap->mapofs;
    MSize nent = snap->nent;
    MSize nslots = snap->nslots;
    MSize topslot = snap->topslot;
    MSize nextofs = snapno+1u < T->nsnap ?
	T->snap[snapno+1u].mapofs : T->nsnapmap;
    IRRef snapref = snap->ref;
    uint64_t pcbase;
    uintptr_t snappc;
    MSize snappos;
    MSize n;
    if (snapref < REF_FIRST || snapref >= T->nins || snapref < prevref ||
	mapofs > T->nsnapmap || nextofs > T->nsnapmap ||
	mapofs != expected_mapofs || nextofs < mapofs ||
	nent > nextofs - mapofs ||
	nextofs - mapofs - nent != 1u + LJ_FR2 ||
	nslots < 1u + LJ_FR2 || topslot != root_topslot ||
	nslots > root_topslot + 1u + LJ_FR2)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
			     snapref, IR_XPOLL, (uint16_t)snapno);
    expected_mapofs = nextofs;
    prevref = snapref;
    if (snapref == loopref)
      loopsnap = 1;
    if (snapref <= xpollref)
      xpollsnap = snapref;
    for (n = 0; n < nent; n++) {
      SnapEntry sn = T->snapmap[mapofs+n];
      IRRef ref = snap_ref(sn);
      BCReg slot = snap_slot(sn);
      uint32_t flags = sn & 0x00ff0000u;
      if (slot >= nslots || (n != 0 &&
	  slot <= snap_slot(T->snapmap[mapofs+n-1])))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
			       ref, IR_XPOLL, (uint16_t)snapno);
      if (sn == SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL))
	continue;  /* The sole root-frame sentinel carries no object. */
      if (slot < 1 + LJ_FR2 ||
	  (flags != 0 && flags != SNAP_NORESTORE) ||
	  (!arm64_ir_int_ref(T, ref, snapref, rootop == BC_FORL) &&
	   (!(scalar_mode & ARM64_IR_SCALAR_NUM) ||
	    !arm64_ir_num_ref(T, ref, snapref, allow_num_sub,
		allow_num_mul, allow_num_div))))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
			       ref, IR_XPOLL, (uint16_t)snapno);
      if (flags == SNAP_NORESTORE) {
	const IRIns *source;
	MSize idxslot = (MSize)(1u+LJ_FR2+bc_a(T->startins));
	if (rootop != BC_FORL || irref_isk(ref) ||
	    ref < REF_FIRST || ref >= snapref ||
	    (slot != idxslot && slot != idxslot+FORL_STOP &&
	     (!allow_forl_step || slot != idxslot+FORL_STEP)))
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
				 ref, IR_SLOAD, (uint16_t)snapno);
	source = &T->ir[ref];
	if (source->o != IR_SLOAD || source->op1 != slot ||
	    (source->op2 & (IRSLOAD_CONVERT|IRSLOAD_PARENT)) != 0)
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
				 ref, IR_SLOAD, (uint16_t)snapno);
      }
    }
    memcpy(&pcbase, &T->snapmap[mapofs+nent], sizeof(pcbase));
    snappc = (uintptr_t)(pcbase >> 8);
    if ((uint8_t)pcbase != (uint8_t)(J->baseslot-2) ||
	!arm64_ir_pcpos(snappc, proto_lo, proto_hi, &snappos))
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
			     snapref, IR_XPOLL, (uint16_t)snapno);
  }
  if (!loopsnap || xpollsnap != loopref)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
			   loopref, IR_XPOLL, 0xffffu);
  return 1;
}

static int arm64_ir_phi_marks(const GCtrace *T, IRRef firstphi, IRRef end,
	LJArm64IRReject *reject)
{
  IRRef ref, phiref;
  IRRef limit = firstphi ? firstphi : end;
  for (ref = REF_FIRST; ref < limit; ref++) {
    int operand = 0;
    for (phiref = firstphi; phiref && phiref < end; phiref++) {
      const IRIns *phi = &T->ir[phiref];
      if (phi->o != IR_PHI)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, phiref,
			       (IROp)phi->o, 0);
      if (phi->op1 == ref || phi->op2 == ref)
	operand = 1;
    }
    if (!!irt_isphi(T->ir[ref].t) != operand)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
			     (IROp)T->ir[ref].o, T->ir[ref].t.irt);
  }
  return 1;
}

static int arm64_ir_guard_snapshots(const GCtrace *T,
	LJArm64IRReject *reject)
{
  IRRef ref;
  SnapNo snapno = 0;
  for (ref = REF_FIRST; ref < T->nins; ref++) {
    const IRIns *ir = &T->ir[ref];
    if (!irt_isguard(ir->t))
      continue;
    if (T->snap[0].ref > ref)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT, ref,
			     (IROp)ir->o, 0);
    while (snapno+1u < T->nsnap && T->snap[snapno+1u].ref <= ref)
      snapno++;
    if (T->snap[snapno].ref > ref)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT, ref,
			     (IROp)ir->o, (uint16_t)snapno);
  }
  return 1;
}

int lj_asm_arm64_ir_admit(const jit_State *J, const GCtrace *T,
			   LJArm64IRReject *reject)
{
  IRRef ref, loopref = 0, xpollref = 0, firstphi = 0;
  MSize maxslots = 0;
  MSize root_topslot;
  MSize forl_idxslot = 0;
  GCobj *startpt;
  GCproto *pt;
  uintptr_t proto_lo, proto_hi;
  unsigned nloop = 0, nxpoll = 0, nphi = 0;
  unsigned scalar_mode = 0, constant_profile = 0;
  unsigned numdynamic_profile = 0;
  unsigned numdynamic_args_kind = ARM64_NUMDYN_ARGS_NUM;
  int allow_num_sub = 0, allow_num_mul = 0, allow_num_div = 0;
  int forl_dynamic_step = 0;
  BCOp startop;
  if (reject) {
    reject->reason = LJ_ARM64_IR_REJECT_NONE;
    reject->ref = 0;
    reject->op = IR_NOP;
    reject->detail = LJ_ARM64_IR_CALL_NONE;
  }
  if (J == NULL || T == NULL || T->ir == NULL)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			   IR_BASE, 0);
  startop = bc_op(T->startins);
  if (J->parent != 0 || J->exitno != 0 || T->root != 0 ||
	T->traceno == 0 || T->nins <= REF_FIRST || T->nins >= REF_DROP)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			   IR_LOOP, (uint16_t)startop);
  if (startop == BC_FUNCF) {
    if (LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED || T->link != 0 ||
	T->linktype != LJ_TRLINK_RETURN || J->loopref != 0)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			     IR_XPOLL, (uint16_t)startop);
  } else if ((startop != BC_LOOP && startop != BC_FORL) ||
	T->link != T->traceno || T->linktype != LJ_TRLINK_LOOP) {
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			   IR_LOOP, (uint16_t)startop);
  }
  startpt = trace_startptgco_acq((GCtrace *)T);
  if (J->pt == NULL || startpt != obj2gco(J->pt) ||
      !checkptrGC(startpt) ||
	startpt->gch.gct != (uint32_t)~LJ_TPROTO)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			   IR_SLOAD, 0);
  pt = J->pt;
  root_topslot = pt->framesize;
  if (root_topslot == 0 || J->pt != pt ||
	J->baseslot != 1 + LJ_FR2 || J->framedepth != 0 || J->retdepth != 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			   IR_SLOAD, (uint16_t)root_topslot);
  if (!arm64_ir_start(J, T, pt, &proto_lo, &proto_hi, reject))
    return 0;
  if (startop == BC_LOOP &&
	arm64_ir_numacc_bytecode(pt, trace_startpc_acq((GCtrace *)T),
	  &numdynamic_profile)) {
    allow_num_sub = arm64_numdynamic_is_sub(numdynamic_profile);
    allow_num_mul = arm64_numdynamic_is_mul(numdynamic_profile);
    allow_num_div = arm64_numdynamic_is_div(numdynamic_profile);
  }
  if (startop == BC_FORL)
    forl_idxslot = (MSize)(1u+LJ_FR2+bc_a(T->startins));
  if (T->sinktags != 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SINK, REF_BASE,
			   IR_TNEW, T->sinktags);
  if (T->nsnap == 0 || T->snap == NULL || T->snapmap == NULL)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
			   REF_BASE, IR_XPOLL, 0);
#if LJ_HASJIT_FFI_CALLXS
  if (startop == BC_FORL && T->nk == ARM64_CALLXS_K_ZERO &&
      J->ktrace == ARM64_CALLXS_K_TRACE)
    return arm64_ir_callxs_shape(J, T, pt, reject);
  if (startop == BC_FORL && T->nk == ARM64_CALLXS_PTR_K_ZERO &&
      J->ktrace == ARM64_CALLXS_PTR_K_TRACE)
    return arm64_ir_callxs_ptr_shape(J, T, pt, reject);
#endif
  if (!arm64_ir_constants(T, reject, &constant_profile))
    return 0;
  if (startop == BC_FUNCF)
    return arm64_ir_funcf_shape(J, T, pt, reject);
  {
    SnapNo snapno;
    for (snapno = 0; snapno < T->nsnap; snapno++)
      if (T->snap[snapno].nslots > maxslots)
	maxslots = T->snap[snapno].nslots;
  }

  for (ref = REF_BASE; ref < T->nins; ref++) {
    const IRIns *ir = &T->ir[ref];
    uint8_t flags = (uint8_t)(ir->t.irt & ~IRT_TYPE);
    if (firstphi != 0 && ir->o != IR_PHI)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, ref,
			     (IROp)ir->o, 0);
    switch ((IROp)ir->o) {
    case IR_BASE:
      if (ref != REF_BASE || ir->t.irt != IRT_PGC ||
	  ir->op1 != 0 || ir->op2 != 0)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND, ref,
				 IR_BASE, ir->op2);
      break;
    case IR_SLOAD:
      if (ir->op1 < 1 + LJ_FR2 || ir->op1 >= maxslots ||
	  ir->op1 >= root_topslot + 1u + LJ_FR2)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND, ref,
				 IR_SLOAD, ir->op1);
      if (irt_type(ir->t) == IRT_INT) {
	scalar_mode |= ARM64_IR_SCALAR_INT;
	if (!arm64_ir_sload_layout(*ir, startop, forl_idxslot,
					  root_topslot+1u+LJ_FR2, IRT_INT))
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
					 IR_SLOAD, ir->op2);
      } else if (irt_type(ir->t) == IRT_NUM) {
	scalar_mode |= ARM64_IR_SCALAR_NUM;
	if (!arm64_ir_sload_layout(*ir, startop, forl_idxslot,
					  root_topslot+1u+LJ_FR2, IRT_NUM))
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
					 IR_SLOAD, ir->op2);
      } else {
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
					 IR_SLOAD, ir->op2);
      }
      break;
    case IR_CONV:
      if (numdynamic_profile == 0)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
			       IR_CONV, ir->op2);
      if (numdynamic_args_kind == ARM64_NUMDYN_ARGS_NUM &&
	  ir->t.irt == IRT_NUM && ir->op2 == IRCONV_NUM_INT) {
	if (ref == ARM64_NUMACC_INTSTEP_R_STEP_NUM &&
	    ir->op1 == ARM64_NUMACC_INTSTEP_R_STEP_INT) {
	  numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_STEP;
	} else if (numdynamic_profile == ARM64_NUMDYN_ADD_LT &&
	    ref == ARM64_NUMACC_INTLIMIT_R_LIMIT_NUM &&
	    ir->op1 == ARM64_NUMACC_INTLIMIT_R_LIMIT_INT) {
	  numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_LIMIT;
	} else if (ref == ARM64_NUMACC_INTX_R_X_NUM &&
	    ir->op1 == ARM64_NUMACC_INTX_R_X_INT) {
	  numdynamic_args_kind = ARM64_NUMDYN_ARGS_INT_X;
	} else {
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				 IR_CONV, ir->op2);
	}
      } else if (numdynamic_args_kind == ARM64_NUMDYN_ARGS_INT_X &&
	  ref == ARM64_NUMACC_INTX_R_X_CHECK &&
	  ir->t.irt == (IRT_INT|IRT_GUARD) &&
	  ir->op1 == ARM64_NUMACC_INTX_R_X_PRE &&
	  ir->op2 == (IRCONV_INT_NUM|IRCONV_CHECK)) {
	/* Exact loop type-instability repair; its guard result is unused. */
      } else {
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
			       IR_CONV, ir->op2);
      }
      scalar_mode |= ARM64_IR_SCALAR_NUM;
      break;
    case IR_LT: case IR_GE: case IR_LE: case IR_GT:
      if (irt_type(ir->t) == IRT_INT) {
	if (!arm64_ir_type_flags(ir->t, IRT_INT, IRT_GUARD, IRT_GUARD) ||
	    !arm64_ir_int_binary(T, ir, ref, startop == BC_FORL))
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				   (IROp)ir->o, ir->t.irt);
	scalar_mode |= ARM64_IR_SCALAR_INT;
      } else if (irt_type(ir->t) == IRT_NUM) {
	if (!arm64_ir_type_flags(ir->t, IRT_NUM, IRT_GUARD, IRT_GUARD) ||
	    !arm64_ir_num_binary(T, ir, ref, allow_num_sub, allow_num_mul,
		allow_num_div))
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				   (IROp)ir->o, ir->t.irt);
	scalar_mode |= ARM64_IR_SCALAR_NUM;
      } else {
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				 (IROp)ir->o, ir->t.irt);
      }
      break;
    case IR_EQ: case IR_NE:
      if (!arm64_ir_type_flags(ir->t, IRT_INT, IRT_GUARD, IRT_GUARD) ||
	  !arm64_ir_int_binary(T, ir, ref, startop == BC_FORL))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				 (IROp)ir->o, ir->t.irt);
      scalar_mode |= ARM64_IR_SCALAR_INT;
      break;
    case IR_ADDOV: case IR_SUBOV: case IR_MULOV:
      if (!arm64_ir_type_flags(ir->t, IRT_INT, IRT_GUARD,
			       IRT_GUARD|IRT_ISPHI) ||
	  !arm64_ir_int_binary(T, ir, ref, startop == BC_FORL))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
					 (IROp)ir->o, ir->t.irt);
      scalar_mode |= ARM64_IR_SCALAR_INT;
      break;
    case IR_ADD:
      if (irt_type(ir->t) == IRT_INT) {
	scalar_mode |= ARM64_IR_SCALAR_INT;
	if (startop != BC_FORL || ir->t.irt != (IRT_INT|IRT_ISPHI) ||
	    !arm64_ir_int_binary(T, ir, ref, 1))
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
					 IR_ADD, ir->t.irt);
      } else if (irt_type(ir->t) == IRT_NUM) {
	scalar_mode |= ARM64_IR_SCALAR_NUM;
	if (startop != BC_LOOP || ir->t.irt != (IRT_NUM|IRT_ISPHI) ||
	    !arm64_ir_num_add_binary(T, ir, ref))
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
					 IR_ADD, ir->t.irt);
      } else {
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				       IR_ADD, ir->t.irt);
      }
      break;
    case IR_SUB:
      if (!allow_num_sub || startop != BC_LOOP ||
	  ir->t.irt != (IRT_NUM|IRT_ISPHI) ||
	  !arm64_ir_num_binary(T, ir, ref, allow_num_sub, allow_num_mul,
		allow_num_div))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				 IR_SUB, ir->t.irt);
      scalar_mode |= ARM64_IR_SCALAR_NUM;
      break;
    case IR_MUL:
      if (!allow_num_mul || startop != BC_LOOP ||
	  ir->t.irt != (IRT_NUM|IRT_ISPHI) ||
	  !arm64_ir_num_binary(T, ir, ref, allow_num_sub, allow_num_mul,
		allow_num_div))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				 IR_MUL, ir->t.irt);
      scalar_mode |= ARM64_IR_SCALAR_NUM;
      break;
    case IR_DIV:
      if (!allow_num_div || startop != BC_LOOP ||
	  ir->t.irt != (IRT_NUM|IRT_ISPHI) ||
	  !arm64_ir_num_binary(T, ir, ref, allow_num_sub, allow_num_mul,
		allow_num_div))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				 IR_DIV, ir->t.irt);
      scalar_mode |= ARM64_IR_SCALAR_NUM;
      break;
    case IR_PHI:
      if (firstphi == 0)
	firstphi = ref;
      nphi++;
      if ((irt_type(ir->t) != IRT_INT && irt_type(ir->t) != IRT_NUM) ||
	  flags != 0 ||
	  nphi > LJ_MAX_PHI ||
	  loopref == 0 || xpollref != loopref + 1u || ref <= xpollref ||
	  ir->op1 < REF_FIRST || ir->op1 >= loopref ||
	  ir->op2 <= xpollref || ir->op2 >= firstphi ||
	  (irt_type(ir->t) == IRT_INT ?
	   !arm64_ir_int_binary(T, ir, ref, startop == BC_FORL) :
	   !arm64_ir_num_binary(T, ir, ref, allow_num_sub, allow_num_mul,
		allow_num_div)) ||
	  !irt_isphi(T->ir[ir->op1].t) ||
	  !irt_isphi(T->ir[ir->op2].t))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
					 IR_PHI, ir->t.irt);
      scalar_mode |= irt_type(ir->t) == IRT_NUM ?
	ARM64_IR_SCALAR_NUM : ARM64_IR_SCALAR_INT;
      {
	IRRef prevphi;
	for (prevphi = firstphi; prevphi < ref; prevphi++)
	  if (T->ir[prevphi].op1 == ir->op1)
	    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				   IR_PHI, ir->op1);
      }
      break;
    case IR_LOOP:
      if (ir->t.irt != (IRT_NIL|IRT_GUARD) ||
	  ir->op1 != 0 || ir->op2 != 0)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_XPOLL, ref,
				 IR_LOOP, 0);
      loopref = ref;
      nloop++;
      break;
    case IR_XPOLL:
      /* First ARM64 traces always service gate, poll and profile requests. */
      if (ir->t.irt != (IRT_NIL|IRT_GUARD) ||
	  ir->op1 != 1 || ir->op2 != 0)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_XPOLL, ref,
				 IR_XPOLL, ir->op1);
      xpollref = ref;
      nxpoll++;
      break;
    case IR_CALLN: case IR_CALLA: case IR_CALLL: case IR_CALLS:
      /* Empty helper allowlist: detail identifies the rejected helper ID. */
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL, ref,
			     (IROp)ir->o, ir->op2);
    case IR_CALLXS:
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL, ref,
			     IR_CALLXS, LJ_ARM64_IR_CALL_NONE);
    case IR_USE:
      if (startop != BC_FORL)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPCODE, ref,
			       IR_USE, 0);
      if (ir->t.irt != IRT_INT || ir->op2 != 0 ||
	  ir->op1 < REF_FIRST || ir->op1 >= ref ||
	  T->ir[ir->op1].o != IR_ADDOV ||
	  T->ir[ir->op1].t.irt != (IRT_INT|IRT_GUARD))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND, ref,
			       IR_USE, ir->op2);
      break;
    default:
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPCODE, ref,
			     (IROp)ir->o, 0);
    }
  }

  if (nloop != 1 || nxpoll != 1 || xpollref != loopref + 1u ||
	J->loopref != loopref)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_XPOLL,
			   xpollref ? xpollref : loopref, IR_XPOLL,
			   (uint16_t)((nloop << 8) | nxpoll));
  if (!arm64_ir_phi_marks(T, firstphi, T->nins, reject))
    return 0;
  if ((scalar_mode & ARM64_IR_SCALAR_NUM) != 0) {
    if (scalar_mode == (ARM64_IR_SCALAR_INT|ARM64_IR_SCALAR_NUM)) {
      if (numdynamic_args_kind != ARM64_NUMDYN_ARGS_NUM) {
	if (!arm64_ir_numacc_shape(J, T, pt, firstphi,
	      numdynamic_args_kind, reject))
	  return 0;
      } else if (constant_profile != ARM64_IR_KPROFILE_INT ||
	  startop != BC_LOOP ||
	  !arm64_ir_numadd_shape(T, firstphi, reject)) {
	return 0;
      }
    } else if (scalar_mode == ARM64_IR_SCALAR_NUM) {
      if (startop != BC_LOOP)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE,
	  firstphi ? firstphi : REF_BASE, IR_LOOP, (uint16_t)startop);
      if (constant_profile == ARM64_IR_KPROFILE_HALF) {
	if (!arm64_ir_numhalf_shape(J, T, pt, firstphi, reject))
	  return 0;
      } else if (constant_profile == ARM64_IR_KPROFILE_INT &&
		 T->nk == REF_TRUE) {
	if (pt->sizebc == 14) {
	  if (!arm64_ir_numstep_shape(J, T, pt, firstphi, reject))
	    return 0;
	} else if (pt->sizebc == 13) {
	  if (!arm64_ir_numacc_shape(J, T, pt, firstphi,
		ARM64_NUMDYN_ARGS_NUM, reject))
	    return 0;
	} else {
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE,
	    ARM64_NUMSTEP_R_X, IR_ADD, (uint16_t)pt->sizebc);
	}
      } else {
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT,
	  T->nk, IR_KNUM, (uint16_t)constant_profile);
      }
    } else {
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE,
	firstphi ? firstphi : REF_BASE, IR_PHI, (uint16_t)scalar_mode);
    }
  } else if (constant_profile != ARM64_IR_KPROFILE_INT) {
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT,
	T->nk, IR_KNUM, (uint16_t)constant_profile);
  }
  if (startop == BC_FORL &&
      !arm64_ir_forl_shape(J, T, loopref, xpollref, firstphi,
	&forl_dynamic_step, reject))
    return 0;
  if (!arm64_ir_snapshots(T, loopref, xpollref, root_topslot, J,
				  proto_lo, proto_hi, startop,
				  scalar_mode, allow_num_sub, allow_num_mul,
				  allow_num_div, forl_dynamic_step,
				  reject))
    return 0;
  if (!arm64_ir_guard_snapshots(T, reject))
    return 0;
  return 1;
}

#endif
