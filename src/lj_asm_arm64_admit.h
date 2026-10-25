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
** Scalar LOOP admission uses independent semantic and allocator certificates
** for checked INT arithmetic, NUM arithmetic/conversions, comparisons and
** same-type PHIs. Rooted helpers have per-operation input, output and retry
** contracts, independent of trace length, table count or constant key values.
** Other heap effects and helper calls remain closed.
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

/* Check the immutable, side-effect-free bytecode subset supported for a
** fixed-function root. Register state is derived from bytecode definitions:
** CGET imports only a real fixed parameter; constants and MOV define local
** values, and arithmetic requires values established by this bytecode walk. */
static int arm64_ir_funcf_bytecode(const BCIns *bc, MSize sizebc,
	MSize framesize, MSize numparams, BCIns startins, BCIns liveins,
	int prepublication)
{
  uintptr_t lo = (uintptr_t)bc;
  BCIns word, ret;
  BCIns generation[UINT8_MAX+1u];
  uint8_t initialized[UINT8_MAX+1u] = { 0 };
  uint8_t numeric[UINT8_MAX+1u] = { 0 };
  MSize pos;
  LJ_STATIC_ASSERT((sizeof(BCIns) & (sizeof(BCIns)-1)) == 0);
  if (bc == NULL || (lo & (sizeof(BCIns)-1)) != 0 || sizebc < 3 ||
	sizebc > UINT8_MAX ||
	(uintptr_t)sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns) || framesize == 0 ||
	framesize > UINT8_MAX || numparams >= framesize ||
	bc_op(startins) != BC_FUNCF ||
	bc_a(startins) != framesize || bc_d(startins) != 0 ||
	!((liveins == startins) ||
	  (bc_op(liveins) == BC_JFUNCF &&
	   bc_a(liveins) == bc_a(startins) && bc_d(liveins) != 0)) ||
	(BCIns)la_load32_acq((const uint32_t *)&bc[0]) !=
	  (prepublication ? startins : liveins))
    return 0;
  for (pos = 0; pos < numparams; pos++)
    initialized[pos] = 1;
  for (pos = 0; pos < sizebc; pos++)
    generation[pos] = arm64_ir_bc_acq(lo, pos);
  for (pos = 1; pos < sizebc; pos++) {
    BCOp op;
    BCReg a, b, c;
    word = generation[pos];
    op = bc_op(word);
    a = bc_a(word); b = bc_b(word); c = bc_c(word);
    if (op == BC_RET1) {
      if (pos+1u != sizebc || bc_d(word) != 2u || a >= framesize ||
	  !initialized[a])
	return 0;
      ret = word;
      goto check_generation;
    }
    if (op == BC_KPRI) {
      if (a >= framesize || bc_d(word) > 2u)
	return 0;
      initialized[a] = 1;
      numeric[a] = 2;
    } else if (op == BC_KSHORT) {
      if (a >= framesize)
	return 0;
      initialized[a] = numeric[a] = 1;
    } else if (op == BC_MOV) {
      c = bc_d(word);
      if (a >= framesize || c >= framesize || !initialized[c])
	return 0;
      initialized[a] = 1;
      numeric[a] = numeric[c];
    } else if (op == BC_CGET) {
      BCReg src = bc_d(word);
      if (a >= framesize || src >= framesize ||
          (src >= numparams && !initialized[src]))
	return 0;
      initialized[a] = 1;
      numeric[a] = numeric[src];
    } else if (op == BC_ADDVV || op == BC_SUBVV || op == BC_MULVV ||
	       op == BC_DIVVV) {
      if (a >= framesize || b >= framesize || c >= framesize ||
	  !initialized[b] || !initialized[c] || numeric[b] == 2 ||
	  numeric[c] == 2)
	return 0;
      initialized[a] = numeric[a] = 1;
    } else {
      /* Calls, upvalues, branches, heap access and all other effects are
      ** outside this scalar root certificate. */
      return 0;
    }
  }
  return 0;
check_generation:
  for (pos = 0; pos < sizebc; pos++)
    if (arm64_ir_bc_acq(lo, pos) != generation[pos]) return 0;
  if (generation[0] != (prepublication ? startins : liveins) ||
      generation[sizebc-1u] != ret)
    return 0;
  return 1;
}

int lj_asm_arm64_funcf_bytecode(const BCIns *bc, MSize sizebc,
	MSize framesize, MSize numparams, BCIns startins, BCIns liveins,
	int prepublication)
{
  return arm64_ir_funcf_bytecode(bc, sizebc, framesize, numparams,
	startins, liveins, prepublication);
}

static int arm64_ir_funcf_slot_source(const BCIns *bc, MSize sizebc,
	MSize numparams, MSize baseslot, MSize slot)
{
  uintptr_t lo = (uintptr_t)bc;
  MSize pos;
  if (bc == NULL || sizebc > UINT8_MAX ||
	(uintptr_t)sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns)) return 0;
  for (pos = 1; pos+1u < sizebc; pos++) {
    BCIns ins = arm64_ir_bc_acq(lo, pos);
    if (bc_op(ins) == BC_CGET && bc_d(ins) < numparams &&
	 slot == baseslot + bc_d(ins))
      return 1;
  }
  return 0;
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
	  pt->numparams, startins, startins, 1) ||
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

#if LJ_HASJIT_FFI_CALLXS
/* Exact Darwin ARM64 native-call roots. Keep the complete reference geometry
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

static int arm64_ir_sload_layout(IRIns ir, BCOp rootop, MSize forl_idxslot,
	MSize maxslots, IRType type)
{
  if (ir.op1 < 1u+LJ_FR2 || ir.op1 >= maxslots)
    return 0;
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

static int arm64_postra_spill_slot(MSize slot, MSize capacity)
{
  return slot >= SPS_FIRST && slot < capacity && slot < SPS_LIMIT;
}

static int arm64_postra_constants(const LJArm64PostRAView *view)
{
  IRRef ref;
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    IRIns k = ir_load_acq(&view->ir[ref]);
    if (k.o != IR_KPRI || k.t.irt != (uint8_t)(REF_NIL-ref) || k.op12 != 0)
      return 0;
  }
  for (ref = view->nk; ref < REF_TRUE; ref++) {
    IRIns k = ir_load_acq(&view->ir[ref]);
    if (k.o != IR_KINT || k.t.irt != IRT_INT)
      return 0;
  }
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

static int arm64_scalar_postra_register(Reg r, IRType type);


static int arm64_scalar_ir_ref(const GCtrace *T, IRRef value, IRRef before,
	IRType type);

static int arm64_ir_funcf_const_ref(const IRIns *ir, IRRef nk,
	IRRef value, IRType type)
{
  IRRef k;
  if (value == REF_TRUE || value == REF_FALSE || value == REF_NIL)
	return irt_type(ir_load_acq(&ir[value]).t) == type;
  if (value < nk || value >= REF_TRUE) return 0;
  for (k = nk; k < REF_TRUE; k++) {
	IRIns ins = ir_load_acq(&ir[k]);
	if (ins.o == IR_KNUM) {
	  if (k == value) return type == IRT_NUM && irt_type(ins.t) == IRT_NUM;
	  k++;  /* KNUM payload is not an independently addressable constant. */
	} else if (k == value) {
	  return irt_type(ins.t) == type &&
		 (ins.o == IR_KINT || ins.o == IR_KPRI);
	}
  }
  return 0;
}

/* Find the recorded value for one bytecode scalar operation. The bytecode
** state walker below invokes this once per operation, keeping even repeated
** expressions linear in bytecode size rather than recursively recomputing
** every operand tree. */
static int arm64_ir_funcf_find_value(const IRIns *ir, IRRef nins, BCOp op,
	IRRef x, IRRef y, IRRef *valuep)
{
  IRRef ref;
  for (ref = REF_FIRST; ref < nins; ref++) {
	IRIns ins = ir_load_acq(&ir[ref]);
	IRType type = irt_type(ins.t);
	int commutative = op == BC_ADDVV || op == BC_MULVV;
	int operands = (ins.op1 == x && ins.op2 == y) ||
		(commutative && ins.op1 == y && ins.op2 == x);
	if (!operands) continue;
	if (op == BC_ADDVV && ins.o == IR_ADDOV && type == IRT_INT &&
	    ins.t.irt == (IRT_INT|IRT_GUARD)) {
	  *valuep = ref;
	  return 1;
	}
	if (type != IRT_NUM || (ins.t.irt & ~(IRT_TYPE|IRT_ISPHI)))
	  continue;
	if ((op == BC_ADDVV && ins.o == IR_ADD) ||
	    (op == BC_SUBVV && ins.o == IR_SUB) ||
	    (op == BC_MULVV && ins.o == IR_MUL) ||
	    (op == BC_DIVVV && ins.o == IR_DIV)) {
	  *valuep = ref;
	  return 1;
	}
  }
  return 0;
}

static int arm64_ir_funcf_short_value(const IRIns *ir, IRRef nk,
	int16_t immediate, IRRef *valuep)
{
  IRRef k;
  for (k = nk; k < REF_TRUE; k++) {
	IRIns ins = ir_load_acq(&ir[k]);
	if (ins.o == IR_KNUM) {
	  if (k+1u >= REF_TRUE) return 0;
	  k++;
	} else if (ins.o == IR_KINT && ins.t.irt == IRT_INT &&
		   ins.i == immediate) {
	  *valuep = k;
	  return 1;
	}
  }
  return 0;
}

/* Advance an allocated interpreter-register map to a snapshot PC. Parameters
** begin at exact guarded SLOADs. Every bytecode definition updates one slot;
** local reads copy the current value, so repeated dependency graphs do not
** cause recursive/exponential work. */
static int arm64_ir_funcf_advance(const IRIns *ir, IRRef nins, IRRef nk,
	const BCIns *bc, MSize sizebc, MSize framesize,
	MSize target, MSize *posp, IRRef *values,
	uint8_t *valid)
{
  uintptr_t lo = (uintptr_t)bc;
  MSize pos;
  if (!bc || ((uintptr_t)bc & (sizeof(BCIns)-1u)) != 0 || !ir ||
	!posp || !values || !valid || target > sizebc ||
	*posp > target || framesize > UINT8_MAX || sizebc > UINT8_MAX ||
	(uintptr_t)sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns)) return 0;
  for (pos = *posp; pos < target; pos++) {
	BCIns ins = arm64_ir_bc_acq(lo, pos);
	BCOp op = bc_op(ins);
	BCReg a = bc_a(ins), b = bc_b(ins), c = bc_c(ins);
	IRRef value;
	if (op == BC_CGET) {
	  BCReg src = bc_d(ins);
	  if (a >= framesize || src >= framesize || !valid[src]) return 0;
	  values[a] = values[src];
	  valid[a] = 1;
	} else if (op == BC_MOV) {
	  BCReg src = bc_d(ins);
	  if (a >= framesize || src >= framesize || !valid[src]) return 0;
	  values[a] = values[src];
	  valid[a] = 1;
	} else if (op == BC_KPRI) {
	  if (a >= framesize || bc_d(ins) > 2u) return 0;
	  values[a] = (IRRef)(REF_NIL-bc_d(ins));
	  valid[a] = 1;
	} else if (op == BC_KSHORT) {
	  if (a >= framesize || !arm64_ir_funcf_short_value(ir, nk,
	      (int16_t)bc_d(ins), &value)) return 0;
	  values[a] = value;
	  valid[a] = 1;
	} else if (op == BC_ADDVV || op == BC_SUBVV || op == BC_MULVV ||
	   op == BC_DIVVV) {
	  if (a >= framesize || b >= framesize || c >= framesize ||
	      !valid[b] || !valid[c] ||
	      !arm64_ir_funcf_find_value(ir, nins, op, values[b], values[c],
					 &value)) return 0;
	  values[a] = value;
	  valid[a] = 1;
	} else if (op != BC_RET1) {
	  return 0;
	}
  }
  *posp = target;
  return 1;
}

static int arm64_ir_funcf_result_matches(const BCIns *bc,
	MSize sizebc, MSize framesize, const IRRef *values,
	const uint8_t *valid, IRRef value)
{
  uintptr_t lo = (uintptr_t)bc;
  BCIns ret;
  if (!bc || !values || !valid || sizebc < 2 || sizebc > UINT8_MAX ||
	(uintptr_t)sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns)) return 0;
  ret = arm64_ir_bc_acq(lo, sizebc-1u);
  return bc_op(ret) == BC_RET1 && bc_d(ret) == 2u &&
	bc_a(ret) < framesize && valid[bc_a(ret)] &&
	values[bc_a(ret)] == value;
}

static int arm64_ir_funcf_scalar_snapshots(const IRIns *ir, IRRef nins,
	IRRef nk, const SnapShot *snaps, MSize nsnap,
	const SnapEntry *snapmap, MSize nsnapmap, const BCIns *proto_bc,
	MSize proto_sizebc, MSize numparams, MSize framesize,
	MSize baseslot, IRRef pollref, uint8_t base_delta)
{
  MSize s, expected = 0, maxslots = framesize+1u+LJ_FR2;
  IRRef previous = 0;
  IRRef values[UINT8_MAX];
  uint8_t valid[UINT8_MAX] = { 0 };
  MSize bytecode_pos = 1;
  MSize previous_pc = 0;
  uintptr_t lo = (uintptr_t)proto_bc, hi;
  BCIns ret;
  MSize result_slot;
  int saw_result = 0;
  if (!proto_bc || proto_sizebc == 0 || framesize == 0 ||
	!ir || nins <= REF_FIRST || nk == 0 || !snaps || !snapmap ||
	!nsnap || !nsnapmap ||
	proto_sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns)) return 0;
  ret = arm64_ir_bc_acq(lo, proto_sizebc-1u);
  result_slot = 1u+LJ_FR2+bc_a(ret);
  hi = lo+(uintptr_t)proto_sizebc*sizeof(BCIns);
  if (hi <= lo) return 0;
  for (s = REF_FIRST; s < nins; s++) {
    IRIns ins = ir_load_acq(&ir[s]);
    if (ins.o == IR_SLOAD && ins.op1 >= baseslot &&
	ins.op1-baseslot < numparams &&
	ins.op2 == IRSLOAD_TYPECHECK && (ins.t.irt & IRT_GUARD) &&
	(irt_type(ins.t) == IRT_NUM || irt_type(ins.t) == IRT_INT)) {
      BCReg reg = (BCReg)(ins.op1-baseslot);
      values[reg] = (IRRef)s;
      valid[reg] = 1;
    }
  }
  for (s = 0; s < nsnap; s++) {
	const SnapShot *snap = &snaps[s];
    MSize ofs = snap->mapofs;
	MSize next = s+1u < nsnap ? snaps[s+1u].mapofs : nsnapmap;
    MSize i, pcpos, previous_slot = 0;
    uint64_t pcbase;
	if (snap->ref < REF_FIRST || snap->ref >= nins ||
	snap->ref < previous || ofs != expected || next < ofs ||
	    next > nsnapmap || snap->nent > next-ofs ||
	next-ofs-snap->nent != 1u+LJ_FR2 || snap->nslots < 1u+LJ_FR2 ||
	    snap->nslots > maxslots || snap->topslot != framesize)
      return 0;
    previous = snap->ref;
    expected = next;
    for (i = 0; i < snap->nent; i++) {
	    SnapEntry entry = snapmap[ofs+i];
	IRRef value = snap_ref(entry);
	IRType type;
	if (snap_slot(entry) < 1u+LJ_FR2 || snap_slot(entry) >= snap->nslots ||
	    (i != 0 && snap_slot(entry) <= previous_slot) ||
	    (entry & 0x00ff0000u) != 0 ||
	    (value < REF_BASE ?
	     !((value >= nk && value < REF_TRUE) ||
	       (value >= REF_TRUE && value <= REF_NIL)) :
	     (value < REF_FIRST || value >= snap->ref)))
	  return 0;
	    type = irt_type(ir_load_acq(&ir[value]).t);
	if (type != IRT_INT && type != IRT_NUM && type != IRT_TRUE &&
	    type != IRT_FALSE && type != IRT_NIL)
	  return 0;
	if (irref_isk(value) && !arm64_ir_funcf_const_ref(ir, nk, value, type)) {
	  return 0; }
	if (snap_slot(entry) == result_slot && snap->ref == pollref) {
	  saw_result = 1;
	}
	previous_slot = snap_slot(entry);
    }
    {
	SnapEntry raw[1+LJ_FR2];
	MSize i;
	for (i = 0; i < 1u+LJ_FR2; i++) raw[i] = snapmap[ofs+snap->nent+i];
	memcpy(&pcbase, raw, sizeof(pcbase));
	if ((uint8_t)pcbase != base_delta ||
	    !arm64_ir_pcpos((uintptr_t)(pcbase >> 8), lo, hi, &pcpos) ||
	    pcpos <= previous_pc || pcpos > proto_sizebc-1u ||
	    (s+1u == nsnap && pcpos != proto_sizebc-1u))
	  return 0;
	previous_pc = pcpos;
	if (!arm64_ir_funcf_advance(ir, nins, nk, proto_bc, proto_sizebc,
	    framesize, pcpos, &bytecode_pos, values, valid))
	  return 0;
        for (i = 0; i < snap->nent; i++) {
          SnapEntry entry = snapmap[ofs+i];
          MSize slot = snap_slot(entry);
          IRRef value = snap_ref(entry);
          BCReg reg;
          if (slot < 1u+LJ_FR2 || slot >= 1u+LJ_FR2+framesize ||
              (reg = (BCReg)(slot-(1u+LJ_FR2))) >= framesize ||
              !valid[reg] || values[reg] != value)
            return 0;
          if (slot == result_slot && snap->ref == pollref &&
	      !arm64_ir_funcf_result_matches(proto_bc, proto_sizebc,
		framesize, values, valid, value)) return 0;
        }
    }
  }
  return expected == nsnapmap && saw_result &&
	 snaps[nsnap-1u].ref == pollref;
}

/* Frame slots simultaneously present in a native snapshot are concurrently
** live. Their allocated sources must have distinct physical locations unless
** the bytecode state proves the slots contain the same SSA value. */
static int arm64_postra_funcf_locations(const LJArm64PostRAView *view,
	IRRef semantic_nins)
{
	enum { MAX_FUNC_DYNAMIC_REFS = 2u*UINT8_MAX+8u };
  IRRef nins = semantic_nins;
	IRRef last_ir_use[MAX_FUNC_DYNAMIC_REFS];
	IRRef last_snap_use[MAX_FUNC_DYNAMIC_REFS] = { 0 };
	IRRef ref;
	MSize snapno;
	if (nins < REF_FIRST || nins-REF_FIRST > MAX_FUNC_DYNAMIC_REFS)
	  return 0;
	for (ref = REF_FIRST; ref < nins; ref++) {
	  IRRef index = ref-REF_FIRST;
	  IRIns ins = ir_load_acq(&view->ir[ref]);
	  last_ir_use[index] = ref;
	  if (ins.o == IR_ADDOV || ins.o == IR_ADD || ins.o == IR_SUB ||
	      ins.o == IR_MUL || ins.o == IR_DIV) {
	    if (ins.op1 < REF_FIRST || ins.op1 >= ref ||
		ins.op2 < REF_FIRST || ins.op2 >= ref)
	      return 0;
	    last_ir_use[ins.op1-REF_FIRST] = ref;
	    last_ir_use[ins.op2-REF_FIRST] = ref;
	  }
	}
	for (snapno = 0; snapno < view->nsnap; snapno++) {
	  const SnapShot *snap = &view->snap[snapno];
	  MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap), n;
	  IRRef snapref = snap_ref_acq(snap);
	  if (snapref >= nins) return 0;
	  for (n = 0; n < nent; n++) {
	    IRRef value = snap_ref(snapentry_acq(&view->snapmap[ofs+n]));
	    if (value >= REF_FIRST && value < nins &&
		last_snap_use[value-REF_FIRST] < snapref)
	      last_snap_use[value-REF_FIRST] = snapref;
	  }
	}
	for (ref = REF_FIRST; ref < nins; ref++) {
	  IRIns current = ir_load_acq(&view->ir[ref]);
	  IRType type = irt_type(current.t);
	  Reg output;
	  IRRef old;
	  if (current.o != IR_SLOAD && current.o != IR_ADDOV &&
	      current.o != IR_ADD && current.o != IR_SUB &&
	      current.o != IR_MUL && current.o != IR_DIV)
	    continue;
	  output = current.r;
	  for (old = REF_FIRST; old < ref; old++) {
	    IRIns previous = ir_load_acq(&view->ir[old]);
	    IRType oldtype = irt_type(previous.t);
	    if ((previous.o != IR_SLOAD && previous.o != IR_ADDOV &&
		previous.o != IR_ADD && previous.o != IR_SUB &&
		previous.o != IR_MUL && previous.o != IR_DIV) ||
		oldtype != type || previous.r != output)
	      continue;
	    /* A dying operand may be reused after its instruction. A value in
	    ** that instruction's deopt snapshot must remain intact even if its
	    ** last ordinary IR use is the instruction itself. */
	    if (last_ir_use[old-REF_FIRST] > ref ||
		last_snap_use[old-REF_FIRST] >= ref)
	      return 0;
	  }
	}
  for (snapno = 0; snapno < view->nsnap; snapno++) {
	const SnapShot *snap = &view->snap[snapno];
	MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap), n, m;
	IRRef refs[UINT8_MAX];
	RegSP locs[UINT8_MAX];
	if (nent > UINT8_MAX) return 0;
	for (n = 0; n < nent; n++) {
	  SnapEntry entry = snapentry_acq(&view->snapmap[ofs+n]);
	  IRRef value = snap_ref(entry);
	  IRIns source;
	  RegSP loc;
	  if (irref_isk(value)) {
	    refs[n] = value;
	    locs[n] = REGSP_INIT;
	    continue;
	  }
	  if (value < REF_FIRST || value >= snap_ref_acq(snap) || value >= nins)
	    return 0;
	  source = ir_load_acq(&view->ir[value]);
	  loc = source.prev;
	  if (ra_hasspill(regsp_spill(loc)) ||
	      !arm64_scalar_postra_register(regsp_reg(loc), irt_type(source.t)))
	    return 0;
	  refs[n] = value;
	  locs[n] = loc;
	}
	for (n = 0; n < nent; n++) {
	  if (irref_isk(refs[n])) continue;
	  for (m = n+1u; m < nent; m++)
	    if (!irref_isk(refs[m]) && refs[n] != refs[m] &&
		locs[n] == locs[m])
	      return 0;
	}
  }
  return 1;
}

static int arm64_postra_funcf_admit(const LJArm64PostRAView *view,
	BCIns liveins, int prepublication, IRRef *semantic_ninsp)
{
  const IRIns *ir;
  IRRef ref, pollref, semantic_nins;
  IRIns base, suffix;
  if (view == NULL || (ir = view->ir) == NULL || view->snap == NULL ||
	view->snapmap == NULL || view->proto_bc == NULL ||
	view->nins <= REF_FIRST || view->nins >= REF_DROP ||
	view->nk == 0 || view->nk > REF_TRUE || view->nsnap == 0 ||
	view->nsnapmap == 0 || view->proto_sizebc == 0 ||
	view->root_topslot == 0 || view->root_topslot > UINT8_MAX ||
	view->base_delta != 0 || view->spadjust != 0 ||
	!arm64_ir_funcf_bytecode(view->proto_bc, view->proto_sizebc,
	  view->root_topslot, view->proto_numparams, view->startins, liveins,
	  prepublication) || !arm64_postra_constants(view)) {
	return 0;
	}
  semantic_nins = view->nins;
  suffix = ir_load_acq(&ir[semantic_nins-1u]);
  if (suffix.o != IR_NOP || suffix.t.irt != IRT_NIL || suffix.op1 ||
	suffix.op2 || suffix.s != SPS_NONE || suffix.prev)
    return 0;
  semantic_nins--;
  if (semantic_nins <= REF_BASE+1u) return 0;
  pollref = semantic_nins-1u;
  base = ir_load_acq(&ir[REF_BASE]);
  if (base.o != IR_BASE || base.t.irt != IRT_PGC || base.op1 || base.op2) return 0;
  for (ref = REF_BASE+1u; ref < semantic_nins; ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    IRType type = irt_type(ins.t);
    if (ref == pollref) {
      if (ins.o != IR_XPOLL || ins.t.irt != (IRT_NIL|IRT_GUARD) ||
          ins.op1 != 1 || ins.op2 || ins.s != SPS_NONE)
        return 0;
    } else if (ins.o == IR_NOP) {
      if (ins.t.irt != IRT_NIL || ins.op1 || ins.op2 ||
	  ins.s != SPS_NONE || ins.prev != 0xff)
        return 0;
    } else if (ins.o == IR_SLOAD) {
      if ((type != IRT_NUM && type != IRT_INT) ||
          !arm64_ir_type_flags(ins.t, type, IRT_GUARD,
		IRT_GUARD|IRT_ISPHI) || ins.op2 != IRSLOAD_TYPECHECK ||
          ins.op1 < 1u+LJ_FR2 ||
          ins.op1 >= view->root_topslot+1u+LJ_FR2 ||
          !arm64_ir_funcf_slot_source(view->proto_bc, view->proto_sizebc,
		view->proto_numparams, 1u+LJ_FR2, ins.op1) || !ra_hasreg(ins.r) ||
          !arm64_scalar_postra_register(ins.r, type) || ra_hasspill(ins.s))
        return 0;
    } else if (ins.o == IR_ADDOV || ins.o == IR_ADD || ins.o == IR_SUB ||
               ins.o == IR_MUL || ins.o == IR_DIV) {
      IRIns a, b;
      if ((ins.o == IR_ADDOV ?
           (type != IRT_INT || ins.t.irt != (IRT_INT|IRT_GUARD)) :
           (type != IRT_NUM || (ins.t.irt & ~(IRT_TYPE|IRT_ISPHI)))) ||
          ins.op1 < REF_FIRST || ins.op1 >= ref ||
          ins.op2 < REF_FIRST || ins.op2 >= ref)
        return 0;
      a = ir_load_acq(&ir[ins.op1]);
      b = ir_load_acq(&ir[ins.op2]);
      if (irt_type(a.t) != type || irt_type(b.t) != type ||
	  ra_hasspill(regsp_spill(a.prev)) ||
	  ra_hasspill(regsp_spill(b.prev)) ||
	  !arm64_scalar_postra_register(regsp_reg(a.prev), type) ||
	  !arm64_scalar_postra_register(regsp_reg(b.prev), type) ||
	  (ins.op1 != ins.op2 && regsp_reg(a.prev) == regsp_reg(b.prev)) ||
          !ra_hasreg(ins.r) ||
          !arm64_scalar_postra_register(ins.r, type) || ra_hasspill(ins.s))
        return 0;
    } else {
      return 0;
    }
  }
  if (!arm64_ir_funcf_scalar_snapshots(ir, semantic_nins, view->nk,
      view->snap, view->nsnap, view->snapmap, view->nsnapmap,
      view->proto_bc, view->proto_sizebc, view->proto_numparams,
      view->root_topslot, 1u+LJ_FR2, pollref,
      view->base_delta) ||
      !arm64_postra_funcf_locations(view, semantic_nins))
    return 0;
  if (semantic_ninsp) *semantic_ninsp = semantic_nins;
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
  IRRef indexphi = 0, idxload = 0, precmpref = 0, postcmpref = 0;
  IRRef directionref = 0, overflowref = 0;
  unsigned nstep = 0, nuse = 0, nloop = 0, nxpoll = 0, nidx = 0;
  IRIns stop, step, direction, overflow, use, idx, pre, post, precmp, postcmp;
  IRIns zero;
  IROp cmpop;

  *dynamic_stepp = 0;
  for (ref = REF_FIRST; ref < semantic_nins; ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    if (ins.o == IR_SLOAD && ins.op1 == idxslot) {
      nidx++;
      idxload = ref;
    } else if (ins.o == IR_SLOAD && ins.op1 == stepslot) {
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
  if ((nstep == 0) != (nuse == 0) || nstep > 1u || nuse > 1u)
    return 0;
  for (ref = REF_FIRST; ref < firstphi; ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    if (ins.o == IR_ADD && ref < loopref && ins.op1 == idxload) {
      if (preadd != 0) return 0;
      preadd = ref;
    } else if (ins.o == IR_ADD && ref > xpollref && ins.op1 == preadd) {
      if (postadd != 0) return 0;
      postadd = ref;
    }
  }
  if (nidx != 1u || nloop != 1u ||
      nxpoll != 1u || xpollref != loopref+1u || firstphi == 0 ||
      preadd <= REF_FIRST || postadd <= xpollref)
    return 0;
  pre = ir_load_acq(&ir[preadd]);
  post = ir_load_acq(&ir[postadd]);
  for (ref = REF_FIRST; ref < semantic_nins; ref++) {
    IRIns ins = ir_load_acq(&ir[ref]);
    if (ins.o == IR_ADD && irt_type(ins.t) == IRT_INT &&
        ref != preadd && ref != postadd)
      return 0;
  }
  /* Keep the uniquely discovered SLOAD as the authority. Checking only the
  ** generic SSA walk later is too late: this proof dereferences stepref. */
  if (nstep != 0 && (pre.op2 != stepref || post.op2 != stepref))
    return 0;
  for (ref = preadd+1u; ref < loopref; ref++) {
    IRIns cmp = ir_load_acq(&ir[ref]);
    if ((cmp.o == IR_LE || cmp.o == IR_GE) && cmp.op1 == preadd &&
        cmp.t.irt == (IRT_INT|IRT_GUARD)) {
      if (precmpref != 0) return 0;
      precmpref = ref;
    }
  }
  if (precmpref == 0) return 0;
  precmp = ir_load_acq(&ir[precmpref]);
  for (ref = xpollref+1u; ref < firstphi; ref++) {
    IRIns cmp = ir_load_acq(&ir[ref]);
    if ((cmp.o == IR_LE || cmp.o == IR_GE) && cmp.op1 == postadd &&
        cmp.op2 == precmp.op2 && cmp.t.irt == (IRT_INT|IRT_GUARD)) {
      if (postcmpref != 0) return 0;
      postcmpref = ref;
    }
  }
  if (postcmpref == 0) return 0;
  postcmp = ir_load_acq(&ir[postcmpref]);
  if (pre.op1 != idxload ||
      post.op1 != preadd ||
      pre.t.irt != (IRT_INT|IRT_ISPHI) ||
      post.t.irt != (IRT_INT|IRT_ISPHI))
    return 0;

  if (nstep == 0) {
    IRRef stopref = precmp.op2, boundref = 0, phi_ref = 0;
    IRIns stepconst, stopins, idxins, phi;
    int32_t stepvalue;
    int64_t expected64;
    if (nuse != 0 || post.op2 != pre.op2 ||
        !arm64_postra_scalar_kref(view, pre.op2, IRT_INT))
      return 0;
    stepconst = ir_load_acq(&ir[pre.op2]);
    if (stepconst.o != IR_KINT || stepconst.t.irt != IRT_INT ||
        stepconst.i == 0)
      return 0;
    stepvalue = stepconst.i;
    cmpop = stepvalue > 0 ? IR_LE : IR_GE;
    if (precmp.o != cmpop || postcmp.o != cmpop ||
        postcmp.op2 != stopref ||
        !((stopref >= REF_FIRST && stopref < precmpref) ||
          arm64_postra_scalar_kref(view, stopref, IRT_INT)))
      return 0;
    stopins = ir_load_acq(&ir[stopref]);
    if (stopins.o == IR_SLOAD) {
      if (stopins.op1 != stopslot ||
          stopins.op2 != (IRSLOAD_READONLY|IRSLOAD_INHERIT) ||
          stopins.t.irt != IRT_INT || stopins.s != SPS_NONE ||
          !ra_hasreg(stopins.r) ||
          !arm64_scalar_postra_register(stopins.r, IRT_INT))
        return 0;
      expected64 = stepvalue > 0 ? (int64_t)INT32_MAX-stepvalue :
                                  (int64_t)INT32_MIN-stepvalue;
      if (expected64 < INT32_MIN || expected64 > INT32_MAX)
        return 0;
      for (ref = stopref+1u; ref < preadd; ref++) {
        IRIns candidate = ir_load_acq(&ir[ref]);
        if (candidate.o == cmpop && candidate.op1 == stopref &&
            candidate.t.irt == (IRT_INT|IRT_GUARD) &&
            arm64_postra_scalar_kref(view, candidate.op2, IRT_INT)) {
          IRIns bound = ir_load_acq(&ir[candidate.op2]);
          if (bound.o == IR_KINT && bound.i == (int32_t)expected64) {
            if (boundref != 0) return 0;
            boundref = ref;
          }
        }
      }
      if (boundref == 0)
        return 0;
    } else if (stopins.o == IR_KINT) {
      int64_t sum = (int64_t)stopins.i + stepvalue;
      if (stopins.t.irt != IRT_INT ||
          sum < INT32_MIN || sum > INT32_MAX)
        return 0;
    } else {
      return 0;
    }
    idxins = ir_load_acq(&ir[idxload]);
    if (idxins.o != IR_SLOAD || idxins.op1 != idxslot ||
        idxins.op2 != (IRSLOAD_TYPECHECK|IRSLOAD_INHERIT) ||
        idxins.t.irt != (IRT_INT|IRT_GUARD) || idxins.s != SPS_NONE ||
        !ra_hasreg(idxins.r) ||
        !arm64_scalar_postra_register(idxins.r, IRT_INT) ||
        pre.s != SPS_NONE || !ra_hasreg(pre.r) ||
        !arm64_scalar_postra_register(pre.r, IRT_INT) ||
        post.s != SPS_NONE || !ra_hasreg(post.r) ||
        !arm64_scalar_postra_register(post.r, IRT_INT) ||
        (stopins.o == IR_SLOAD &&
         (stopins.r == idxins.r || stopins.r == pre.r)))
      return 0;
    for (ref = firstphi; ref < semantic_nins; ref++) {
      IRIns candidate = ir_load_acq(&ir[ref]);
      if (candidate.o == IR_PHI && candidate.op1 == preadd &&
          candidate.op2 == postadd) {
        if (phi_ref != 0) return 0;
        phi_ref = ref;
      }
    }
    if (phi_ref == 0) return 0;
    phi = ir_load_acq(&ir[phi_ref]);
    if (phi.t.irt != IRT_INT || phi.s != SPS_NONE ||
        phi.r != pre.r || phi.r != post.r)
      return 0;
    return 1;
  }

  if (precmp.op2 < REF_FIRST || precmp.op2 >= stepref ||
      stepref >= idxload || idxload >= semantic_nins)
    return 0;
  stop = ir_load_acq(&ir[precmp.op2]);
  step = ir_load_acq(&ir[stepref]);
  for (ref = stepref+1u; ref < idxload; ref++) {
    IRIns candidate = ir_load_acq(&ir[ref]);
    IRIns zero;
    if ((candidate.o == IR_GE || candidate.o == IR_LT) &&
        candidate.op1 == stepref &&
        arm64_postra_scalar_kref(view, candidate.op2, IRT_INT)) {
      zero = ir_load_acq(&ir[candidate.op2]);
      if (zero.i == 0 && candidate.t.irt == (IRT_INT|IRT_GUARD)) {
        if (directionref != 0) return 0;
        directionref = ref;
      }
    }
  }
  if (directionref == 0) return 0;
  direction = ir_load_acq(&ir[directionref]);
  for (ref = directionref+1u; ref < idxload; ref++) {
    IRIns candidate = ir_load_acq(&ir[ref]);
    if (candidate.o == IR_ADDOV && candidate.op1 == stepref &&
        candidate.op2 == precmp.op2 &&
        candidate.t.irt == (IRT_INT|IRT_GUARD)) {
      if (overflowref != 0) return 0;
      overflowref = ref;
    }
  }
  if (overflowref == 0) return 0;
  overflow = ir_load_acq(&ir[overflowref]);
  use = ir_load_acq(&ir[useref]);
  idx = ir_load_acq(&ir[pre.op1]);
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
      useref <= overflowref || use.o != IR_USE || use.t.irt != IRT_INT ||
      use.op1 != overflowref || use.op2 != 0 || use.s != SPS_NONE ||
      precmp.o != cmpop || precmp.op1 != preadd ||
      precmp.t.irt != (IRT_INT|IRT_GUARD) ||
      postcmp.o != cmpop || postcmp.op1 != postadd ||
      postcmp.op2 != precmp.op2 ||
      postcmp.t.irt != (IRT_INT|IRT_GUARD))
    return 0;
  for (ref = firstphi; ref < semantic_nins; ref++) {
    IRIns phi = ir_load_acq(&ir[ref]);
    if (phi.op1 == preadd && phi.op2 == postadd) {
      if (indexphi != 0)
	return 0;
      indexphi = ref;
    }
  }
  if (indexphi == 0)
    return 0;
  {
    IRIns iphi = ir_load_acq(&ir[indexphi]);
    Reg ri = iphi.r;
    if (iphi.o != IR_PHI || iphi.t.irt != IRT_INT ||
	iphi.s != SPS_NONE || ri >= RID_MAX_GPR ||
	!rset_test(RSET_GPR, ri) || ri != pre.r || ri != post.r ||
	stop.r == step.r || stop.r == ri || step.r == ri ||
	idx.r == stop.r || idx.r == step.r ||
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
static int arm64_postra_nonloop_admit(const LJArm64PostRAView *view,
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
  unsigned nintadd = 0;
  int suffix_is_nop = 0, forl_dynamic_step = 0;

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
  if (rootop != BC_FORL && rootop != BC_FUNCF)
    return 0;
  if (rootop == BC_FUNCF)
    return arm64_postra_funcf_admit(view, view->startins, 0, semantic_ninsp);
  if (!arm64_postra_constants(view))
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
    case IR_SLOAD: case IR_ADDOV: case IR_SUBOV: case IR_MULOV:
      if (!arm64_postra_int_value(ins, rootop, forl_idxslot, maxslots))
        return 0;
      break;
    case IR_ADD:
      nintadd++;
      if (!arm64_postra_int_value(ins, rootop, forl_idxslot, maxslots))
        return 0;
      break;
    case IR_LT: case IR_GE: case IR_LE: case IR_GT: case IR_EQ: case IR_NE:
      if (!arm64_ir_type_flags(ins.t, IRT_INT, IRT_GUARD, IRT_GUARD) ||
          slot != SPS_NONE)
        return 0;
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
      if (ins.t.irt != IRT_INT)
        return 0;
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

  if (nintadd != 2u)
    return 0;

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
	if (irt_type(source.t) != IRT_INT ||
	    !arm64_postra_int_value(source, rootop, forl_idxslot, maxslots) ||
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
	  if (!arm64_postra_spill_slot(regsp_spill(rs), capacity))
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
  return arm64_postra_funcf_admit(view, liveins, 0, semantic_ninsp);
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

static int arm64_ir_constants(const GCtrace *T, LJArm64IRReject *reject)
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
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    const IRIns *ir = &T->ir[ref];
    if (ir->o != IR_KINT || ir->t.irt != IRT_INT)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT, ref,
			     (IROp)ir->o, 0);
  }
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


static int arm64_ir_funcf_shape(const jit_State *J, const GCtrace *T,
	const GCproto *pt, LJArm64IRReject *reject)
{
  const IRIns *ir = T->ir;
  IRRef ref, poll = 0;
  if (J->loopref != 0 || T->nins <= REF_BASE+1u ||
      !arm64_ir_funcf_bytecode(proto_bc(pt), pt->sizebc, pt->framesize,
	pt->numparams, T->startins, T->startins, 1) ||
      ir[REF_BASE].o != IR_BASE || ir[REF_BASE].t.irt != IRT_PGC ||
      ir[REF_BASE].op1 != 0 || ir[REF_BASE].op2 != 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE,
	REF_BASE, IR_BASE, BC_FUNCF);
  for (ref = REF_BASE+1u; ref < T->nins; ref++) {
    const IRIns *ins = &ir[ref];
    IRType type = irt_type(ins->t);
    if (ins->o == IR_XPOLL) {
      if (poll || ref+1u != T->nins ||
          ins->t.irt != (IRT_NIL|IRT_GUARD) || ins->op1 != 1 || ins->op2)
        goto bad_funcf;
      poll = ref;
    } else if (ins->o == IR_NOP) {
      if (ins->t.irt != IRT_NIL || ins->op1 || ins->op2 || ins->s)
        goto bad_funcf;
    } else if (ins->o == IR_SLOAD) {
      if ((type != IRT_NUM && type != IRT_INT) ||
          !arm64_ir_type_flags(ins->t, type, IRT_GUARD,
		IRT_GUARD|IRT_ISPHI) || ins->op2 != IRSLOAD_TYPECHECK ||
          ins->op1 < 1u+LJ_FR2 ||
          ins->op1 >= pt->framesize+1u+LJ_FR2 ||
          !arm64_ir_funcf_slot_source(proto_bc(pt), pt->sizebc,
		pt->numparams, J->baseslot, ins->op1))
        goto bad_funcf;
    } else if (ins->o == IR_ADDOV || ins->o == IR_ADD || ins->o == IR_SUB ||
               ins->o == IR_MUL || ins->o == IR_DIV) {
      if ((ins->o == IR_ADDOV ?
           (type != IRT_INT || ins->t.irt != (IRT_INT|IRT_GUARD)) :
           (type != IRT_NUM || (ins->t.irt & ~(IRT_TYPE|IRT_ISPHI)))) ||
          !arm64_scalar_ir_ref(T, ins->op1, ref, type) ||
          !arm64_scalar_ir_ref(T, ins->op2, ref, type))
        goto bad_funcf;
    } else {
      goto bad_funcf;
    }
  }
  if (!poll || !arm64_ir_funcf_scalar_snapshots(ir, T->nins, T->nk,
      T->snap, T->nsnap, T->snapmap, T->nsnapmap,
      proto_bc(pt), pt->sizebc, pt->numparams, pt->framesize,
      J->baseslot, poll, (uint8_t)(J->baseslot-2u)))
    goto bad_funcf;
  return 1;
 bad_funcf:
  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND, ref,
	(IROp)ir[ref < T->nins ? ref : REF_BASE].o, BC_FUNCF);
}

static int arm64_ir_int_binary(const GCtrace *T, const IRIns *ir,
	IRRef before, int allow_add)
{
  return arm64_ir_int_ref(T, ir->op1, before, allow_add) &&
	 arm64_ir_int_ref(T, ir->op2, before, allow_add);
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
  IRRef idxload = 0, stepref, stopref, stepload = 0, useref = 0;
  IRRef precmpref = 0, postcmpref = 0, directionref = 0, overflowref = 0;
  IRIns pre, post, precmp, postcmp, idx, stop;
  IROp cmpop;
  int32_t step = 0, stopvalue;
  unsigned nidxload = 0, nstepload = 0, nuse = 0;

  UNUSED(J);
  *dynamic_stepp = 0;
  for (ref = REF_FIRST; ref < T->nins; ref++) {
    const IRIns *ir = &T->ir[ref];
    if (ir->o == IR_SLOAD && ir->op1 == idxslot) {
      nidxload++;
      idxload = ref;
    } else if (ir->o == IR_SLOAD && ir->op1 == stepslot) {
      nstepload++;
      stepload = ref;
    } else if (ir->o == IR_USE) {
      nuse++;
      useref = ref;
    }
    if (ir->o == IR_ADD && ref < loopref && ir->op1 == idxload) {
      if (preadd != 0)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			       ref, IR_ADD, 1);
      preadd = ref;
    } else if (ir->o == IR_ADD && ref > xpollref &&
	       (!firstphi || ref < firstphi) && ir->op1 == preadd) {
      if (postadd != 0)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			       ref, IR_ADD, 2);
      postadd = ref;
    }
  }
  if (nidxload != 1u || idxload == 0 || preadd == 0 || postadd == 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			   preadd ? preadd : postadd, IR_ADD, (uint16_t)nidxload);

  pre = T->ir[preadd];
  post = T->ir[postadd];
  for (ref = REF_FIRST; ref < T->nins; ref++) {
    const IRIns *ins = &T->ir[ref];
    if (ins->o == IR_ADD && irt_type(ins->t) == IRT_INT &&
        ref != preadd && ref != postadd)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
                             ref, IR_ADD, 23);
  }
  stepref = pre.op2;
  for (ref = preadd+1u; ref < loopref; ref++) {
    IRIns cmp = T->ir[ref];
    if ((cmp.o == IR_LE || cmp.o == IR_GE) && cmp.op1 == preadd &&
        cmp.t.irt == (IRT_INT|IRT_GUARD)) {
      if (precmpref != 0)
        return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
                               ref, (IROp)cmp.o, 16);
      precmpref = ref;
    }
  }
  if (precmpref == 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
                           preadd, IR_ADD, 17);
  precmp = T->ir[precmpref];
  stopref = precmp.op2;
  for (ref = xpollref+1u; ref < (firstphi ? firstphi : T->nins); ref++) {
    IRIns cmp = T->ir[ref];
    if ((cmp.o == IR_LE || cmp.o == IR_GE) && cmp.op1 == postadd &&
        cmp.op2 == stopref && cmp.t.irt == (IRT_INT|IRT_GUARD)) {
      if (postcmpref != 0)
        return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
                               ref, (IROp)cmp.o, 18);
      postcmpref = ref;
    }
  }
  if (postcmpref == 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
                           postadd, IR_ADD, 19);
  postcmp = T->ir[postcmpref];
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
    IRIns direction;
    int32_t zero;
    if (nstepload != 1u || stepload != stepref || nuse != 1u ||
        stopref < REF_FIRST || stopref >= stepref || stepref >= idxload)
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
    for (ref = stepref+1u; ref < idxload; ref++) {
      IRIns candidate = T->ir[ref];
      if ((candidate.o == IR_GE || candidate.o == IR_LT) &&
          candidate.op1 == stepref &&
          arm64_ir_kint_value(T, candidate.op2, &zero) && zero == 0 &&
          candidate.t.irt == (IRT_INT|IRT_GUARD)) {
        if (directionref != 0)
          return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
                                 ref, (IROp)candidate.o, 20);
        directionref = ref;
      }
    }
    if (directionref == 0)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
                             stepref, IR_SLOAD, 6);
    direction = T->ir[directionref];
    cmpop = direction.o == IR_GE ? IR_LE : IR_GE;
    for (ref = directionref+1u; ref < idxload; ref++) {
      IRIns candidate = T->ir[ref];
      if (candidate.o == IR_ADDOV && candidate.op1 == stepref &&
          candidate.op2 == stopref &&
          candidate.t.irt == (IRT_INT|IRT_GUARD)) {
        if (overflowref != 0)
          return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
                                 ref, IR_ADDOV, 21);
        overflowref = ref;
      }
    }
    if (overflowref == 0 || useref <= overflowref ||
        T->ir[useref].o != IR_USE || T->ir[useref].t.irt != IRT_INT ||
        T->ir[useref].op1 != overflowref || T->ir[useref].op2 != 0)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
                             overflowref ? overflowref : stepref,
                             IR_ADDOV, 7);
    *dynamic_stepp = 1;
  }

  if (precmp.o != cmpop || postcmp.o != cmpop ||
      precmp.op1 != preadd || postcmp.op1 != postadd ||
      postcmp.op2 != stopref ||
      precmp.t.irt != (IRT_INT|IRT_GUARD) ||
      postcmp.t.irt != (IRT_INT|IRT_GUARD))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
                           precmpref, cmpop, 8);

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
      guardref = 0;
      for (ref = stopref+1u; ref < preadd; ref++) {
        IRIns candidate = T->ir[ref];
        int32_t candidate_bound;
        expected64 = step > 0 ? (int64_t)INT32_MAX-(int64_t)step :
                                     (int64_t)INT32_MIN-(int64_t)step;
        if (expected64 >= INT32_MIN && expected64 <= INT32_MAX &&
            candidate.o == cmpop && candidate.op1 == stopref &&
            arm64_ir_kint_value(T, candidate.op2, &candidate_bound) &&
            candidate_bound == (int32_t)expected64 &&
            candidate.t.irt == (IRT_INT|IRT_GUARD)) {
          if (guardref != 0)
            return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
                                   ref, cmpop, 22);
          guardref = ref;
        }
      }
      if (guardref == 0)
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

static int arm64_scalar_ir_ref(const GCtrace *T, IRRef value, IRRef before,
                               IRType type);

static int arm64_ir_snapshots(const GCtrace *T, IRRef loopref,
	IRRef xpollref, MSize root_topslot, const jit_State *J,
	uintptr_t proto_lo, uintptr_t proto_hi, BCOp rootop,
	int allow_forl_step,
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
	   !(rootop == BC_FORL &&
	     (arm64_scalar_ir_ref(T, ref, snapref, IRT_NUM) ||
	      arm64_scalar_ir_ref(T, ref, snapref, IRT_TAB) ||
	      arm64_scalar_ir_ref(T, ref, snapref, IRT_STR) ||
	      arm64_scalar_ir_ref(T, ref, snapref, IRT_TRUE) ||
	      arm64_scalar_ir_ref(T, ref, snapref, IRT_FALSE)))))
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

static int arm64_ir_nonloop_admit(const jit_State *J, const GCtrace *T,
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
  } else if (startop != BC_FORL ||
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
  if (!arm64_ir_constants(T, reject))
    return 0;
  if (startop == BC_FUNCF) {
    int ok = arm64_ir_funcf_shape(J, T, pt, reject);
    return ok;
  }
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
      if (!arm64_ir_sload_layout(*ir, startop, forl_idxslot,
                                root_topslot+1u+LJ_FR2, IRT_INT))
        return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
                              IR_SLOAD, ir->op2);
      break;
    case IR_LT: case IR_GE: case IR_LE: case IR_GT: case IR_EQ: case IR_NE:
      if (!arm64_ir_type_flags(ir->t, IRT_INT, IRT_GUARD, IRT_GUARD) ||
          !arm64_ir_int_binary(T, ir, ref, 1))
        return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
                              (IROp)ir->o, ir->t.irt);
      break;
    case IR_ADDOV: case IR_SUBOV: case IR_MULOV:
      if (!arm64_ir_type_flags(ir->t, IRT_INT, IRT_GUARD,
                              IRT_GUARD|IRT_ISPHI) ||
          !arm64_ir_int_binary(T, ir, ref, 1))
        return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
                              (IROp)ir->o, ir->t.irt);
      break;
    case IR_ADD:
      if (ir->t.irt != (IRT_INT|IRT_ISPHI) ||
          !arm64_ir_int_binary(T, ir, ref, 1))
        return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
                              IR_ADD, ir->t.irt);
      break;
    case IR_PHI:
      if (firstphi == 0)
	firstphi = ref;
      nphi++;
      if (irt_type(ir->t) != IRT_INT ||
	  flags != 0 ||
	  nphi > LJ_MAX_PHI ||
	  loopref == 0 || xpollref != loopref + 1u || ref <= xpollref ||
	  ir->op1 < REF_FIRST || ir->op1 >= loopref ||
	  ir->op2 <= xpollref || ir->op2 >= firstphi ||
	  !arm64_ir_int_binary(T, ir, ref, 1) ||
	  !irt_isphi(T->ir[ir->op1].t) ||
	  !irt_isphi(T->ir[ir->op2].t))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
					 IR_PHI, ir->t.irt);
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
  if (startop == BC_FORL &&
      !arm64_ir_forl_shape(J, T, loopref, xpollref, firstphi,
	&forl_dynamic_step, reject))
    return 0;
  if (!arm64_ir_snapshots(T, loopref, xpollref, root_topslot, J,
				  proto_lo, proto_hi, startop,
				  forl_dynamic_step,
				  reject))
    return 0;
  if (!arm64_ir_guard_snapshots(T, reject))
    return 0;
  return 1;
}

/* TNEW encodes its array capacity in one IRRef1; all such values fit the
** runtime constructor bound. Hash capacity is checked independently below. */
LJ_STATIC_ASSERT(UINT16_MAX <= LJ_MAX_ASIZE);

/* Recorder-side scalar provenance. Deliberately independent of the immutable
** allocator certificate below: neither pass consumes the other's verdict. */
static int arm64_scalar_ir_ref(const GCtrace *T, IRRef value, IRRef before,
                               IRType type)
{
  const IRIns *ir;
  if (irref_isk(value)) {
    IRRef k;
    if (value == REF_TRUE || value == REF_FALSE)
      return type == (IRType)(REF_NIL - value);
    for (k = T->nk; k < REF_TRUE; k++) {
      if (k == value)
        return irt_type(T->ir[k].t) == type;
      if (T->ir[k].o == IR_KNUM || T->ir[k].o == IR_KGC)
        k++;
    }
    return 0;
  }
  if (value < REF_FIRST || value >= before)
    return 0;
  ir = &T->ir[value];
  if (irt_type(ir->t) != type)
    return 0;
  if (ir->o == IR_CALLS)
    return type == IRT_INT &&
      (ir->op2 == IRCALL_lj_tab_len_rooted_try ||
       ir->op2 == IRCALL_lj_tab_getscalar_rooted_try ||
       ir->op2 == IRCALL_lj_tab_storenum_rooted ||
       ir->op2 == IRCALL_lj_tab_storebool_rooted);
  if (type == IRT_TAB)
    return (ir->o == IR_SLOAD || ir->o == IR_TNEW) &&
      (ir->t.irt & ~IRT_ISPHI) == (IRT_TAB|IRT_GUARD);
  if (type == IRT_STR)
    return ir->o == IR_SLOAD && ir->t.irt == (IRT_STR|IRT_GUARD);
  switch ((IROp)ir->o) {
  case IR_SLOAD:
  case IR_VLOAD:
  case IR_ADDOV:
  case IR_SUBOV:
  case IR_MULOV:
  case IR_ADD:
  case IR_SUB:
  case IR_MUL:
  case IR_DIV:
  case IR_CONV:
    return 1;
  default:
    return 0;
  }
}

/* Each rooted helper is an operation with explicit input/output effects.
** Scratch values may be overwritten only after the preceding operation has
** consumed them. Neither call count, table count, key value nor placement in
** the unrolled loop is part of this contract. */
static int arm64_rooted_ir_effect(const GCtrace *T, IRRef ref,
                                 IRRef root1, IRRef root2)
{
  const IRIns *call = &T->ir[ref], *guard;
  IRRef key;
  int read = call->op2 == IRCALL_lj_tab_getscalar_rooted_try;
  int store = call->op2 == IRCALL_lj_tab_storenum_rooted ||
              call->op2 == IRCALL_lj_tab_storebool_rooted;
  if (!root1 || ref + 1 >= T->nins ||
      call->t.irt != ((read || store) ? (IRT_INT|IRT_GUARD) : IRT_INT))
    return 0;
  if (read) {
    const IRIns *a, *b, *value;
    IRRef chain = call->op1;
    if (!root2 || chain < REF_FIRST || chain >= ref || ref + 2 >= T->nins)
      return 0;
    a = &T->ir[chain];
    if (a->o != IR_CARG || a->t.irt != IRT_NIL ||
        a->op1 < REF_FIRST || a->op1 >= chain || a->op2 != root1)
      return 0;
    b = &T->ir[a->op1];
    if (b->o != IR_CARG || b->t.irt != IRT_NIL ||
        b->op1 != root1 || b->op2 != root2 ||
        root1 >= a->op1 || root2 >= a->op1 ||
        T->ir[root1].op2 != (IRTMPREF_IN1|IRTMPREF_OUT1) ||
        T->ir[root2].op2 != IRTMPREF_IN2)
      return 0;
    value = &T->ir[ref+2];
    if (value->o != IR_VLOAD || value->op1 != root1 || value->op2 ||
        (irt_type(value->t) != IRT_INT && irt_type(value->t) != IRT_NUM &&
         irt_type(value->t) != IRT_TRUE && irt_type(value->t) != IRT_FALSE) ||
        !irt_isguard(value->t) ||
        (value->t.irt & ~(IRT_TYPE|IRT_GUARD|IRT_ISPHI)))
      return 0;
  } else if (store) {
    const IRIns *tail, *head;
    IRType type = call->op2 == IRCALL_lj_tab_storenum_rooted ? IRT_NUM : IRT_INT;
    IRRef chain = call->op1;
    if (!root2 || chain < REF_FIRST || chain >= ref)
      return 0;
    tail = &T->ir[chain];
    if (tail->o != IR_CARG || tail->t.irt != IRT_NIL ||
        tail->op1 < REF_FIRST || tail->op1 >= chain ||
        !arm64_scalar_ir_ref(T, tail->op2, chain, type))
      return 0;
    head = &T->ir[tail->op1];
    if (head->o != IR_CARG || head->t.irt != IRT_NIL ||
        head->op1 != root1 || head->op2 != root2 ||
        root1 >= tail->op1 || root2 >= tail->op1 ||
        T->ir[root1].op2 != IRTMPREF_IN1 ||
        T->ir[root2].op2 != IRTMPREF_IN2)
      return 0;
  } else if (call->op2 != IRCALL_lj_tab_len_rooted_try ||
             call->op1 != root1 || T->ir[root1].op2 != IRTMPREF_IN1) {
    return 0;
  }
  guard = &T->ir[ref+1];
  if (guard->o != IR_NE || guard->t.irt != (IRT_INT|IRT_GUARD))
    return 0;
  if (guard->op1 == ref) key = guard->op2;
  else if (guard->op2 == ref) key = guard->op1;
  else return 0;
  if (!arm64_scalar_ir_ref(T, key, ref, IRT_INT) || !irref_isk(key) ||
      T->ir[key].o != IR_KINT ||
      T->ir[key].i != ((read || store) ? 0 : LJ_TAB_LEN_RETRY))
    return 0;
  return read ? 2 : store ? 3 : 1;
}

static int arm64_scalar_ir_admit_mode(const jit_State *J, const GCtrace *T,
                                      BCOp rootop,
                                      LJArm64IRReject *reject)
{
  IRRef ref = REF_BASE, loop = 0, poll = 0, firstphi = 0;
  IRRef root1 = 0, root2 = 0, resultroot = 0, resultref = 0;
  IRRef previous_snap = 0, poll_snap = 0, completed_store = 0;
  MSize expected_ofs = 0, maxslots;
  uintptr_t proto_lo, proto_hi;
  unsigned nphi = 0, loop_snap = 0;
  int forl_dynamic_step = 0;
  SnapNo s;
  if (reject) {
    reject->reason = LJ_ARM64_IR_REJECT_NONE;
    reject->ref = 0;
    reject->op = IR_NOP;
    reject->detail = LJ_ARM64_IR_CALL_NONE;
  }
  if (!J || !T || !T->ir || !J->pt || J->parent || J->exitno || T->root ||
      !T->traceno || T->nins <= REF_FIRST || T->nins >= REF_DROP || !T->nk ||
      T->nk > REF_TRUE || bc_op(T->startins) != rootop ||
      (rootop != BC_LOOP && rootop != BC_FORL) ||
      T->link != T->traceno || T->linktype != LJ_TRLINK_LOOP ||
      J->baseslot != 1 + LJ_FR2 || J->framedepth || J->retdepth ||
      trace_startptgco_acq((GCtrace *)T) != obj2gco(J->pt) ||
      !checkptrGC(obj2gco(J->pt)) || J->pt->gct != (uint32_t)~LJ_TPROTO ||
      !J->pt->framesize)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE, IR_LOOP,
                           0);
  if (T->sinktags)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SINK, REF_BASE, IR_TNEW,
                           T->sinktags);
  if (!arm64_ir_start(J, T, J->pt, &proto_lo, &proto_hi, reject))
    return 0;
  maxslots = J->pt->framesize + 1u + LJ_FR2;
  if (rootop == BC_FORL &&
      (MSize)bc_a(T->startins) + FORL_EXT >= J->pt->framesize)
    goto bad_operand;
  if (!T->snap || !T->snapmap || !T->nsnap || !T->nsnapmap)
    goto bad_snapshot;
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    const IRIns *k = &T->ir[ref];
    if (k->o != IR_KPRI || k->t.irt != REF_NIL - ref || k->op12)
      goto bad_constant;
  }
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    const IRIns *k = &T->ir[ref];
    if (k->o == IR_KINT && k->t.irt == IRT_INT)
      continue;
    if (k->o == IR_KGC && k->t.irt == IRT_STR && !k->op12 &&
        ref + LJ_GC64 < REF_TRUE) {
      GCobj *o = ir_kgc_acq(k);
      if (!o || !checkptrGC(o) || o->gch.gct != (uint32_t)~LJ_TSTR)
        goto bad_constant;
      ref += LJ_GC64; /* GC64 payload cannot be referenced as an IR value. */
      continue;
    }
    if (k->o != IR_KNUM || k->t.irt != IRT_NUM || k->op12 ||
        ref + 1 >= REF_TRUE || !tvisnum(&T->ir[ref + 1].tv))
      goto bad_constant;
    ref++; /* The payload is not an addressable IR value. */
  }
  if (T->ir[REF_BASE].o != IR_BASE || T->ir[REF_BASE].t.irt != IRT_PGC ||
      T->ir[REF_BASE].op12) {
    ref = REF_BASE;
    goto bad_operand;
  }
  for (ref = REF_FIRST; ref < T->nins; ref++) {
    const IRIns *ir = &T->ir[ref];
    IRType type = (IRType)irt_type(ir->t);
    unsigned flags = ir->t.irt & ~IRT_TYPE;
    int binary = 0;
    if (firstphi && ir->o != IR_PHI)
      goto bad_operand;
    switch ((IROp)ir->o) {
    case IR_SLOAD:
      if (rootop == BC_FORL &&
          ir->op1 == 1u + LJ_FR2 + bc_a(T->startins)) {
        if (ir->t.irt != (IRT_INT|IRT_GUARD) ||
            ir->op2 != (IRSLOAD_TYPECHECK|IRSLOAD_INHERIT))
          goto bad_type;
        break;
      }
      if (rootop == BC_FORL &&
          (ir->op1 == 1u + LJ_FR2 + bc_a(T->startins) + FORL_STOP ||
           ir->op1 == 1u + LJ_FR2 + bc_a(T->startins) + FORL_STEP)) {
        if (ir->t.irt != IRT_INT ||
            ir->op2 != (IRSLOAD_READONLY|IRSLOAD_INHERIT))
          goto bad_type;
        break;
      }
      if (type == IRT_TAB || type == IRT_STR ||
          type == IRT_TRUE || type == IRT_FALSE) {
        if ((flags & ~((type == IRT_TAB) ? IRT_ISPHI : 0)) != IRT_GUARD ||
            ir->op2 != IRSLOAD_TYPECHECK || ir->op1 < 1 + LJ_FR2 ||
            ir->op1 >= maxslots)
          goto bad_type;
        break;
      }
      if ((type != IRT_INT && type != IRT_NUM) || !(flags & IRT_GUARD) ||
          (flags & ~(IRT_GUARD | IRT_ISPHI)) || ir->op2 != IRSLOAD_TYPECHECK ||
          ir->op1 < 1 + LJ_FR2 || ir->op1 >= maxslots)
        goto bad_type;
      break;
    case IR_TNEW:
      /* asm_tnew calls the existing READY-publishing table constructors.
      ** No raw table payload stores or sunk allocations are admitted here. */
      if (type != IRT_TAB || !(flags & IRT_GUARD) ||
          (flags & ~(IRT_GUARD|IRT_ISPHI)) ||
          ir->op2 > LJ_MAX_HBITS)
        goto bad_type;
      break;
    case IR_TMPREF:
      if (type != IRT_PGC || flags) goto bad_type;
      if (ir->op2 == IRTMPREF_IN1 ||
          ir->op2 == (IRTMPREF_IN1|IRTMPREF_OUT1)) {
        if (!arm64_scalar_ir_ref(T, ir->op1, ref, IRT_TAB))
          goto bad_operand;
        root1 = ref;
      } else if (ir->op2 == IRTMPREF_IN2) {
        if (!arm64_scalar_ir_ref(T, ir->op1, ref, IRT_INT) &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_NUM) &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_STR) &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_TRUE) &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_FALSE))
          goto bad_operand;
        root2 = ref;
      } else goto bad_operand;
      break;
    case IR_VLOAD:
      if (ref != resultref || !resultroot || ir->op1 != resultroot ||
          ir->op2 || (type != IRT_INT && type != IRT_NUM &&
                      type != IRT_TRUE && type != IRT_FALSE) ||
          !(flags & IRT_GUARD) || (flags & ~(IRT_GUARD|IRT_ISPHI)))
        goto bad_type;
      resultroot = resultref = 0;
      break;
    case IR_ADDOV:
    case IR_SUBOV:
    case IR_MULOV:
      if (type != IRT_INT || !(flags & IRT_GUARD) ||
          (flags & ~(IRT_GUARD | IRT_ISPHI)))
        goto bad_type;
      binary = 1;
      break;
    case IR_ADD:
      if (rootop == BC_FORL && type == IRT_INT &&
          flags == IRT_ISPHI) {
        binary = 1;
        break;
      }
      if (type != IRT_NUM || (flags & ~IRT_ISPHI))
        goto bad_type;
      binary = 1;
      break;
    case IR_SUB:
    case IR_MUL:
    case IR_DIV:
      if (type != IRT_NUM || (flags & ~IRT_ISPHI))
        goto bad_type;
      binary = 1;
      break;
    case IR_LT:
    case IR_GE:
    case IR_LE:
    case IR_GT:
    case IR_EQ:
    case IR_NE:
      if ((type != IRT_INT && type != IRT_NUM) || flags != IRT_GUARD)
        goto bad_type;
      /* Guard operands obey SSA order. Check this before inspecting either
      ** reference so malformed forward/self/out-of-range refs stay bounded. */
      if ((ir->op1 >= REF_FIRST && ir->op1 >= ref) ||
          (ir->op2 >= REF_FIRST && ir->op2 >= ref))
        goto bad_operand;
      binary = 1;
      break;
    case IR_CONV:
      if (type == IRT_NUM && !(flags & ~IRT_ISPHI) &&
          ir->op2 == IRCONV_NUM_INT &&
          arm64_scalar_ir_ref(T, ir->op1, ref, IRT_INT))
        break;
      if (type == IRT_INT && (flags & IRT_GUARD) &&
          !(flags & ~(IRT_GUARD | IRT_ISPHI)) &&
          ir->op2 == (IRCONV_INT_NUM | IRCONV_CHECK) &&
          arm64_scalar_ir_ref(T, ir->op1, ref, IRT_NUM))
        break;
      goto bad_type;
    case IR_LOOP:
      if (loop || ir->t.irt != (IRT_NIL | IRT_GUARD) || ir->op12)
        goto bad_poll;
      loop = ref;
      break;
    case IR_XPOLL:
      if (poll || !loop || ref != loop + 1 ||
          ir->t.irt != (IRT_NIL | IRT_GUARD) || ir->op1 != 1 || ir->op2)
        goto bad_poll;
      poll = ref;
      break;
    case IR_PHI:
      if (!firstphi)
        firstphi = ref;
      if (++nphi > LJ_MAX_PHI || !poll || flags ||
          (type != IRT_INT && type != IRT_NUM && type != IRT_TAB) ||
          ir->op1 < REF_FIRST ||
          ir->op1 >= loop || ir->op2 < REF_FIRST || ir->op2 >= firstphi)
        goto bad_type;
      {
        IRRef phi;
        for (phi = firstphi; phi < ref; phi++)
          if (T->ir[phi].op1 == ir->op1)
            goto bad_operand;
      }
      binary = 1;
      break;
    case IR_CALLN:
    case IR_CALLA:
    case IR_CALLL:
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL, ref, (IROp)ir->o,
                             ir->op2);
    case IR_CARG:
      if (type != IRT_NIL || flags ||
          ir->op1 < REF_FIRST || ir->op1 >= ref ||
          (T->ir[ir->op1].o != IR_TMPREF &&
           T->ir[ir->op1].o != IR_CARG) ||
          !((ir->op2 >= REF_FIRST && ir->op2 < ref &&
             T->ir[ir->op2].o == IR_TMPREF) ||
            arm64_scalar_ir_ref(T, ir->op2, ref, IRT_NUM) ||
            arm64_scalar_ir_ref(T, ir->op2, ref, IRT_INT)))
        goto bad_operand;
      break;
    case IR_CALLS:
      {
        int effect = arm64_rooted_ir_effect(T, ref, root1, root2);
        if (!effect)
          return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL, ref,
                                 IR_CALLS, ir->op2);
        if (effect == 2) { resultroot = root1; resultref = ref+2; }
        root1 = root2 = 0;
      }
      break;
    case IR_CALLXS:
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL, ref, IR_CALLXS,
                             LJ_ARM64_IR_CALL_NONE);
    case IR_USE:
      if (rootop != BC_FORL)
        return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPCODE, ref,
                               IR_USE, 0);
      if (ir->t.irt != IRT_INT || ir->op2 != 0 ||
          ir->op1 < REF_FIRST || ir->op1 >= ref ||
          T->ir[ir->op1].o != IR_ADDOV ||
          T->ir[ir->op1].t.irt != (IRT_INT|IRT_GUARD))
        goto bad_operand;
      break;
    default:
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPCODE, ref,
                             (IROp)ir->o, 0);
    }
    if (binary && (!arm64_scalar_ir_ref(T, ir->op1, ref, type) ||
                   !arm64_scalar_ir_ref(T, ir->op2, ref, type)))
      goto bad_operand;
  }
  ref = loop;
  if (!loop || poll != loop + 1 || J->loopref != loop)
    goto bad_poll;
  for (ref = REF_FIRST; ref < (firstphi ? firstphi : T->nins); ref++) {
    IRRef phi;
    int marked = 0;
    for (phi = firstphi; phi && phi < T->nins; phi++)
      marked |= T->ir[phi].op1 == ref || T->ir[phi].op2 == ref;
    if (!!irt_isphi(T->ir[ref].t) != marked)
      goto bad_type;
  }
  if (rootop == BC_FORL) {
    if (!arm64_ir_forl_shape(J, T, loop, poll, firstphi, &forl_dynamic_step,
                             reject))
      return 0;
  }
  for (s = 0; s < T->nsnap; s++) {
    const SnapShot *snap = &T->snap[s];
    MSize next = s + 1 < T->nsnap ? T->snap[s + 1].mapofs : T->nsnapmap;
    MSize i, pcpos;
    BCReg previous_slot = 0;
    uint64_t pcbase;
    ref = snap->ref;
    if (ref < REF_FIRST || ref >= T->nins || ref < previous_snap ||
        snap->mapofs != expected_ofs || snap->mapofs > T->nsnapmap ||
        next > T->nsnapmap || next < snap->mapofs ||
        snap->nent > next - snap->mapofs ||
        next - snap->mapofs - snap->nent != 1 + LJ_FR2 ||
        snap->nslots < 1 + LJ_FR2 || snap->nslots > maxslots ||
        snap->topslot != J->pt->framesize)
      goto bad_snapshot;
    previous_snap = ref;
    expected_ofs = next;
    loop_snap |= ref == loop;
    if (ref <= poll)
      poll_snap = ref;
    for (i = 0; i < snap->nent; i++) {
      SnapEntry sn = T->snapmap[snap->mapofs + i];
      BCReg slot = snap_slot(sn);
      IRRef value = snap_ref(sn);
      if (slot >= snap->nslots || (i && slot <= previous_slot)) {
        goto bad_snapshot;
      }
      previous_slot = slot;
      if ((sn & 0x00ff0000u) == SNAP_NORESTORE) {
        MSize idxslot = 1u + LJ_FR2 + bc_a(T->startins);
        const IRIns *source;
        if (rootop != BC_FORL || irref_isk(value) ||
            value < REF_FIRST || value >= ref ||
            (slot != idxslot && slot != idxslot+FORL_STOP &&
             (slot != idxslot+FORL_STEP || !forl_dynamic_step)))
          goto bad_snapshot;
        source = &T->ir[value];
        if (source->o != IR_SLOAD || source->op1 != slot ||
            (source->op2 & (IRSLOAD_CONVERT|IRSLOAD_PARENT)) != 0)
          goto bad_snapshot;
      }
      if (slot < 1 + LJ_FR2 ||
          ((sn & 0x00ff0000u) != 0 &&
           (sn & 0x00ff0000u) != SNAP_NORESTORE) ||
          (!arm64_scalar_ir_ref(T, value, ref, IRT_INT) &&
           !arm64_scalar_ir_ref(T, value, ref, IRT_NUM) &&
           !arm64_scalar_ir_ref(T, value, ref, IRT_TAB) &&
           !arm64_scalar_ir_ref(T, value, ref, IRT_STR) &&
           !arm64_scalar_ir_ref(T, value, ref, IRT_TRUE) &&
           !arm64_scalar_ir_ref(T, value, ref, IRT_FALSE)))
        goto bad_snapshot;
    }
    memcpy(&pcbase, &T->snapmap[snap->mapofs + snap->nent], sizeof(pcbase));
    if ((uint8_t)pcbase ||
        !arm64_ir_pcpos((uintptr_t)(pcbase >> 8), proto_lo, proto_hi, &pcpos))
      goto bad_snapshot;
  }
  if (!loop_snap || poll_snap != loop)
    goto bad_snapshot;
  s = 0;
  for (ref = REF_FIRST; ref < T->nins; ref++) {
    const IRIns *ir = &T->ir[ref];
    if (irt_isguard(ir->t) && T->snap[0].ref > ref)
      goto bad_snapshot;
    while (s + 1 < T->nsnap && T->snap[s + 1].ref <= ref)
      s++;
    /* A successful read's type guard resumes after the helper; a refused
    ** read uses the preceding snapshot and must not consume its output. */
    if (ir->o == IR_VLOAD && T->snap[s].ref != ref)
      goto bad_snapshot;
    /* A refused store resumes assignment at its status guard. Every later
    ** guard must use a post-effect snapshot, so successful writes are never
    ** replayed merely because subsequent arithmetic/type checks exit. */
    if (completed_store && ref > completed_store+1 && irt_isguard(ir->t) &&
        T->snap[s].ref <= completed_store)
      goto bad_snapshot;
    if (ir->o == IR_CALLS &&
        (ir->op2 == IRCALL_lj_tab_storenum_rooted ||
         ir->op2 == IRCALL_lj_tab_storebool_rooted))
      completed_store = ref;
    if (rootop != BC_FORL && ir->o == IR_SLOAD &&
        ir->op1 >= T->snap[s].nslots)
      goto bad_snapshot;
  }
  return 1;
bad_constant:
  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT, ref,
                         (IROp)T->ir[ref].o, 0);
bad_operand:
  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND, ref,
                         (IROp)T->ir[ref].o, 0);
bad_type:
  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
                         (IROp)T->ir[ref].o, 0);
bad_poll:
  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_XPOLL, ref, IR_XPOLL, 0);
bad_snapshot:
  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT, ref, IR_XPOLL,
                         0);
}

static int arm64_scalar_ir_admit(const jit_State *J, const GCtrace *T,
                                 LJArm64IRReject *reject)
{
  return arm64_scalar_ir_admit_mode(J, T, BC_LOOP, reject);
}

/* Immutable allocator-side certificate. It rechecks scalar provenance and
** snapshots independently, then validates concrete locations and renames. */
static int arm64_scalar_postra_kref(const LJArm64PostRAView *v, IRRef target,
                                    IRType type)
{
  IRRef ref;
  if (target == REF_TRUE || target == REF_FALSE)
    return type == (IRType)(REF_NIL - target);
  for (ref = v->nk; ref < REF_TRUE; ref++) {
    IRIns k = ir_load_acq(&v->ir[ref]);
    if (ref == target)
      return irt_type(k.t) == type;
    if (k.o == IR_KNUM || k.o == IR_KGC)
      ref++;
  }
  return 0;
}
static int arm64_scalar_postra_value(IRIns p)
{
  switch ((IROp)p.o) {
  case IR_SLOAD:
  case IR_ADDOV:
  case IR_SUBOV:
  case IR_MULOV:
  case IR_ADD:
  case IR_SUB:
  case IR_MUL:
  case IR_DIV:
  case IR_CONV:
    return irt_type(p.t) == IRT_INT || irt_type(p.t) == IRT_NUM ||
           (p.o == IR_SLOAD &&
            ((p.t.irt & ~IRT_ISPHI) == (IRT_TAB|IRT_GUARD) ||
             p.t.irt == (IRT_STR|IRT_GUARD)));
  case IR_TNEW:
    return (p.t.irt & ~IRT_ISPHI) == (IRT_TAB|IRT_GUARD);
  case IR_TMPREF:
    return irt_type(p.t) == IRT_PGC;
  case IR_CALLS:
    return irt_type(p.t) == IRT_INT &&
           (p.op2 == IRCALL_lj_tab_len_rooted_try ||
            p.op2 == IRCALL_lj_tab_getscalar_rooted_try ||
            p.op2 == IRCALL_lj_tab_storenum_rooted ||
            p.op2 == IRCALL_lj_tab_storebool_rooted);
  case IR_VLOAD:
    return (irt_type(p.t) == IRT_INT || irt_type(p.t) == IRT_NUM) &&
           irt_isguard(p.t);
  default:
    return 0;
  }
}
static int arm64_scalar_postra_ref(const LJArm64PostRAView *v, IRRef ref,
                                   IRRef before, IRType type)
{
  IRIns p;
  if (ref < REF_BASE)
    return arm64_scalar_postra_kref(v, ref, type);
  if (ref < REF_FIRST || ref >= before)
    return 0;
  p = ir_load_acq(&v->ir[ref]);
  return arm64_scalar_postra_value(p) && irt_type(p.t) == type &&
         (type == IRT_TAB ?
           ((p.o == IR_SLOAD || p.o == IR_TNEW) &&
            (p.t.irt & ~IRT_ISPHI) == (IRT_TAB|IRT_GUARD)) :
          type != IRT_STR || (p.o == IR_SLOAD && p.t.irt == (IRT_STR|IRT_GUARD)));
}
static int arm64_scalar_postra_register(Reg r, IRType type)
{
  return type == IRT_NUM
             ? r >= RID_MIN_FPR && r < RID_MAX_FPR && rset_test(RSET_FPR, r)
             : r < RID_MAX_GPR && rset_test(RSET_GPR, r);
}

/* A missing location is legal only for an unused scalar or a multiply fused
** into NUM add/sub consumers. Snapshot sources are checked after renames.
** This mirrors the target's fusion capability, not an IR-number grammar. */
static int arm64_scalar_postra_nolocation(const LJArm64PostRAView *v,
                                          IRRef value, IRRef end)
{
  IRIns source = ir_load_acq(&v->ir[value]);
  IRRef ref;
  if (source.r != RID_INIT &&
      !(ra_hashint(source.r) &&
        arm64_scalar_postra_register(ra_gethint(source.r),
                                     (IRType)irt_type(source.t))))
    return 0;
  if (source.o == IR_PHI)
    return 0;
  for (ref = value + 1; ref < end; ref++) {
    IRIns use = ir_load_acq(&v->ir[ref]);
    int used;
    switch ((IROp)use.o) {
    case IR_SLOAD:
    case IR_LOOP:
    case IR_XPOLL:
      continue;
    case IR_CONV:
      used = use.op1 == value;
      break;
    default:
      used = use.op1 == value || use.op2 == value;
      break;
    }
    if (!used)
      continue;
    if (source.o != IR_MUL || source.t.irt != IRT_NUM ||
        (use.o != IR_ADD && use.o != IR_SUB) || !irt_isnum(use.t) ||
        use.op1 == use.op2)
      return 0;
    {
      IRRef other = use.op1 == value ? use.op2 : use.op1;
      if (!irref_isk(other)) {
        IRIns ir;
        if (other < REF_FIRST || other >= ref)
          return 0;
        ir = ir_load_acq(&v->ir[other]);
        if (!ra_hasreg(ir.r) && !ra_hasspill(ir.s))
          return 0;
      }
    }
  }
  return 1;
}

/* Allocated helper effects are checked independently of recorder admission.
** In addition to the ABI graph, both scratch pointers must have distinct GPR
** locations. The general scalar pass proves every input and snapshot location. */
static int arm64_rooted_postra_effect(const LJArm64PostRAView *v, IRRef n,
                                     IRRef ref, IRRef root1, IRRef root2)
{
  IRIns c = ir_load_acq(&v->ir[ref]), g, r1;
  IRRef sentinel;
  int read = c.op2 == IRCALL_lj_tab_getscalar_rooted_try;
  int store = c.op2 == IRCALL_lj_tab_storenum_rooted ||
              c.op2 == IRCALL_lj_tab_storebool_rooted;
  if (!root1 || root1 >= ref || ref+1 >= n ||
      c.t.irt != ((read || store) ? (IRT_INT|IRT_GUARD) : IRT_INT)) return 0;
  r1 = ir_load_acq(&v->ir[root1]);
  if (r1.o != IR_TMPREF || r1.t.irt != IRT_PGC ||
      !ra_hasreg(r1.r) || !arm64_scalar_postra_register(r1.r, IRT_PGC) ||
      ra_hasspill(r1.s)) return 0;
  if (read) {
    IRIns a, b, r2, value;
    if (!root2 || root2 >= ref || c.op1 < REF_FIRST || c.op1 >= ref ||
        ref+2 >= n) return 0;
    a = ir_load_acq(&v->ir[c.op1]);
    if (a.o != IR_CARG || a.t.irt != IRT_NIL ||
        a.op1 < REF_FIRST || a.op1 >= c.op1 || a.op2 != root1) return 0;
    b = ir_load_acq(&v->ir[a.op1]);
    r2 = ir_load_acq(&v->ir[root2]);
    if (b.o != IR_CARG || b.t.irt != IRT_NIL ||
        b.op1 != root1 || b.op2 != root2 ||
        root1 >= a.op1 || root2 >= a.op1 ||
        r1.op2 != (IRTMPREF_IN1|IRTMPREF_OUT1) ||
        r2.o != IR_TMPREF || r2.t.irt != IRT_PGC ||
        r2.op2 != IRTMPREF_IN2 || !ra_hasreg(r2.r) ||
        !arm64_scalar_postra_register(r2.r, IRT_PGC) ||
        ra_hasspill(r2.s) || r1.r == r2.r) return 0;
    value = ir_load_acq(&v->ir[ref+2]);
    if (value.o != IR_VLOAD || value.op1 != root1 || value.op2 ||
        (irt_type(value.t) != IRT_INT && irt_type(value.t) != IRT_NUM &&
         irt_type(value.t) != IRT_FALSE && irt_type(value.t) != IRT_TRUE) ||
        !irt_isguard(value.t) ||
        (value.t.irt & ~(IRT_TYPE|IRT_GUARD|IRT_ISPHI))) return 0;
  } else if (store) {
    IRIns outer, inner, r2;
    IRType type = c.op2 == IRCALL_lj_tab_storenum_rooted ? IRT_NUM : IRT_INT;
    if (!root2 || root2 >= ref || c.op1 < REF_FIRST || c.op1 >= ref)
      return 0;
    outer = ir_load_acq(&v->ir[c.op1]);
    if (outer.o != IR_CARG || outer.t.irt != IRT_NIL ||
        outer.op1 < REF_FIRST || outer.op1 >= c.op1 ||
        !arm64_scalar_postra_ref(v, outer.op2, c.op1, type))
      return 0;
    inner = ir_load_acq(&v->ir[outer.op1]);
    r2 = ir_load_acq(&v->ir[root2]);
    if (inner.o != IR_CARG || inner.t.irt != IRT_NIL ||
        inner.op1 != root1 || inner.op2 != root2 ||
        root1 >= outer.op1 || root2 >= outer.op1 ||
        r1.op2 != IRTMPREF_IN1 || r2.o != IR_TMPREF ||
        r2.t.irt != IRT_PGC || r2.op2 != IRTMPREF_IN2 ||
        !ra_hasreg(r2.r) || !arm64_scalar_postra_register(r2.r, IRT_PGC) ||
        ra_hasspill(r2.s) || r1.r == r2.r)
      return 0;
  } else if (c.op2 != IRCALL_lj_tab_len_rooted_try ||
             c.op1 != root1 || r1.op2 != IRTMPREF_IN1) return 0;
  g = ir_load_acq(&v->ir[ref+1]);
  if (g.o != IR_NE || g.t.irt != (IRT_INT|IRT_GUARD)) return 0;
  if (g.op1 == ref) sentinel = g.op2;
  else if (g.op2 == ref) sentinel = g.op1;
  else return 0;
  if (!irref_isk(sentinel) ||
      !arm64_scalar_postra_kref(v, sentinel, IRT_INT)) return 0;
  {
    IRIns k = ir_load_acq(&v->ir[sentinel]);
    if (k.o != IR_KINT || k.i != ((read || store) ? 0 : LJ_TAB_LEN_RETRY)) return 0;
  }
  return read ? 2 : store ? 3 : 1;
}

static int arm64_scalar_postra_admit_mode(const LJArm64PostRAView *v,
                                         BCOp rootop, IRRef *ninsout)
{
  IRRef ref, n, loop = 0, poll = 0, firstphi = 0;
  IRRef root1 = 0, root2 = 0, resultroot = 0, resultref = 0;
  MSize cap = 0, highest = 0, s, expectedofs = 0;
  IRRef prevsnap = 0, poll_snap = 0, completed_store = 0;
  unsigned nphi = 0, loopsnap = 0;
  SnapNo loopsnapno = 0;
  IRRef1 spillowner[SPS_LIMIT];
  int spillowners_initialized = 0, forl_dynamic_step = 0;
  uintptr_t lo, hi;
  if (!v || !v->ir || !v->snap || !v->snapmap || !v->proto_bc ||
      v->nins <= REF_FIRST || v->nins >= REF_DROP || !v->nk ||
      v->nk > REF_TRUE || !v->nsnap || !v->nsnapmap || !v->root_topslot ||
      v->root_topslot > UINT8_MAX || v->base_delta ||
      bc_op(v->startins) != rootop ||
      (rootop != BC_LOOP && rootop != BC_FORL) || !v->proto_sizebc)
    return 0;
  lo = (uintptr_t)v->proto_bc;
  if ((lo & 3u) || v->proto_sizebc > (UINTPTR_MAX - lo) / sizeof(BCIns))
    return 0;
  hi = lo + v->proto_sizebc * sizeof(BCIns);
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    IRIns p = ir_load_acq(&v->ir[ref]);
    if (p.o != IR_KPRI || p.t.irt != REF_NIL - ref || p.op12)
      return 0;
  }
  for (ref = v->nk; ref < REF_TRUE; ref++) {
    IRIns k = ir_load_acq(&v->ir[ref]);
    if (k.o == IR_KINT && k.t.irt == IRT_INT)
      continue;
    if (k.o == IR_KGC && k.t.irt == IRT_STR && k.op12 == 0 &&
        ref + LJ_GC64 < REF_TRUE) {
      GCobj *o = ir_kgc_load_acq(&v->ir[ref]);
      if (o == NULL || !checkptrGC(o) ||
          la_load8_acq(&o->gch.gct) != (uint32_t)~LJ_TSTR)
        return 0;
      ref += LJ_GC64;
      continue;
    }
    if (k.o != IR_KNUM || k.t.irt != IRT_NUM || k.op12 || ref + 1 >= REF_TRUE)
      return 0;
    {
      IRIns payload = ir_load_acq(&v->ir[++ref]);
      if (!tvisnum(&payload.tv))
        return 0;
    }
  }
  n = v->nins;
  {
    if (v->spadjust & 15u ||
        v->spadjust > (MSize)sps_scale(SPS_LIMIT - SPS_FIXED))
      return 0;
    cap = SPS_FIXED + v->spadjust / 4;
    if (ir_load_acq(&v->ir[n - 1]).o == IR_NOP) {
      IRIns nop = ir_load_acq(&v->ir[--n]);
      if (nop.t.irt != IRT_NIL || nop.op12 || nop.prev)
        return 0;
    } else {
      while (n > REF_FIRST && ir_load_acq(&v->ir[n - 1]).o == IR_RENAME)
        n--;
      if (n == v->nins || v->nins - n > LJ_MAX_PHI)
        return 0;
    }
  }
  if (rootop == BC_FORL) {
    if ((MSize)bc_a(v->startins) + FORL_EXT >= v->root_topslot ||
        !arm64_postra_forl_dynamic_shape(v, n, &forl_dynamic_step))
      return 0;
  }
  {
    IRIns base = ir_load_acq(&v->ir[REF_BASE]);
    if (base.o != IR_BASE || base.t.irt != IRT_PGC || base.op12 ||
        (base.s != SPS_NONE ||
         (base.r != RID_INIT && base.r != (RID_NONE | RID_BASE) &&
          !arm64_scalar_postra_register(base.r, IRT_INT))))
      return 0;
  }
  for (ref = REF_FIRST; ref < n; ref++) {
    IRIns p = ir_load_acq(&v->ir[ref]);
    IRType type = (IRType)irt_type(p.t);
    unsigned flags = p.t.irt & ~IRT_TYPE;
    int binary = 0;
    if (firstphi && p.o != IR_PHI)
      return 0;
    switch ((IROp)p.o) {
    case IR_SLOAD:
      if (rootop == BC_FORL &&
          p.op1 == 1u + LJ_FR2 + bc_a(v->startins)) {
        if (p.t.irt != (IRT_INT|IRT_GUARD) ||
            p.op2 != (IRSLOAD_TYPECHECK|IRSLOAD_INHERIT) ||
            p.s != SPS_NONE || !ra_hasreg(p.r) ||
            !arm64_scalar_postra_register(p.r, IRT_INT))
          return 0;
        break;
      }
      if (rootop == BC_FORL &&
          (p.op1 == 1u + LJ_FR2 + bc_a(v->startins) + FORL_STOP ||
           p.op1 == 1u + LJ_FR2 + bc_a(v->startins) + FORL_STEP)) {
        if (p.t.irt != IRT_INT ||
            p.op2 != (IRSLOAD_READONLY|IRSLOAD_INHERIT) ||
            p.s != SPS_NONE || !ra_hasreg(p.r) ||
            !arm64_scalar_postra_register(p.r, IRT_INT))
          return 0;
        break;
      }
      if (type == IRT_TAB || type == IRT_STR ||
          type == IRT_FALSE || type == IRT_TRUE) {
        if ((p.t.irt & ~((type == IRT_TAB) ? IRT_ISPHI : 0)) != (type|IRT_GUARD) ||
            p.op2 != IRSLOAD_TYPECHECK || p.op1 < 1 + LJ_FR2 ||
            p.op1 >= v->root_topslot + 1 + LJ_FR2)
          return 0;
        break;
      }
      if ((type != IRT_INT && type != IRT_NUM) || !(flags & IRT_GUARD) ||
          (flags & ~(IRT_GUARD | IRT_ISPHI)) || p.op2 != IRSLOAD_TYPECHECK ||
          p.op1 < 1 + LJ_FR2 || p.op1 >= v->root_topslot + 1 + LJ_FR2)
        return 0;
      break;
    case IR_TNEW:
      if (type != IRT_TAB || !(flags & IRT_GUARD) ||
          (flags & ~(IRT_GUARD|IRT_ISPHI)) ||
          p.op2 > LJ_MAX_HBITS)
        return 0;
      break;
    case IR_TMPREF:
      if (p.t.irt != IRT_PGC || !ra_hasreg(p.r) ||
          !arm64_scalar_postra_register(p.r, IRT_PGC) || ra_hasspill(p.s))
        return 0;
      if (p.op2 == IRTMPREF_IN1 || p.op2 == (IRTMPREF_IN1|IRTMPREF_OUT1)) {
        if (!arm64_scalar_postra_ref(v, p.op1, ref, IRT_TAB)) return 0;
        root1 = ref;
      } else if (p.op2 == IRTMPREF_IN2) {
        if (!arm64_scalar_postra_ref(v, p.op1, ref, IRT_INT) &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_NUM) &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_STR) &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_FALSE) &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_TRUE)) return 0;
        root2 = ref;
      } else return 0;
      break;
    case IR_CALLS:
      {
        int effect = arm64_rooted_postra_effect(v, n, ref, root1, root2);
        if (!effect || !ra_hasreg(p.r) ||
            !arm64_scalar_postra_register(p.r, IRT_INT)) return 0;
        if (effect == 2) { resultroot = root1; resultref = ref+2; }
        root1 = root2 = 0;
      }
      break;
    case IR_USE:
      if (rootop != BC_FORL || p.t.irt != IRT_INT || p.op2 != 0 ||
          p.op1 < REF_FIRST || p.op1 >= ref ||
          ir_load_acq(&v->ir[p.op1]).o != IR_ADDOV || p.s != SPS_NONE)
        return 0;
      break;
    case IR_ADDOV:
    case IR_SUBOV:
    case IR_MULOV:
      if (type != IRT_INT || !(flags & IRT_GUARD) ||
          (flags & ~(IRT_GUARD | IRT_ISPHI)))
        return 0;
      binary = 1;
      break;
    case IR_ADD:
      if (rootop == BC_FORL && type == IRT_INT &&
          flags == IRT_ISPHI) {
        binary = 1;
        break;
      }
      if (type != IRT_NUM || (flags & ~IRT_ISPHI))
        return 0;
      binary = 1;
      break;
    case IR_SUB:
    case IR_MUL:
    case IR_DIV:
      if (type != IRT_NUM || (flags & ~IRT_ISPHI))
        return 0;
      binary = 1;
      break;
    case IR_LT:
    case IR_GE:
    case IR_LE:
    case IR_GT:
    case IR_EQ:
    case IR_NE:
      if ((type != IRT_INT && type != IRT_NUM) || flags != IRT_GUARD)
        return 0;
      /* Do not dereference a candidate call until both operands are known to
      ** be valid earlier SSA refs (or constants). */
      if ((p.op1 >= REF_FIRST && p.op1 >= ref) ||
          (p.op2 >= REF_FIRST && p.op2 >= ref))
        return 0;
      binary = 1;
      break;
    case IR_CARG:
      if (p.t.irt != IRT_NIL || p.op1 < REF_FIRST || p.op1 >= ref) return 0;
      {
        IRIns a = ir_load_acq(&v->ir[p.op1]);
        if ((a.o != IR_TMPREF && a.o != IR_CARG) ||
            !((p.op2 >= REF_FIRST && p.op2 < ref &&
               ir_load_acq(&v->ir[p.op2]).o == IR_TMPREF) ||
              arm64_scalar_postra_ref(v, p.op2, ref, IRT_NUM) ||
              arm64_scalar_postra_ref(v, p.op2, ref, IRT_INT))) return 0;
      }
      break;
    case IR_VLOAD:
      if (!resultroot || ref != resultref || p.op1 != resultroot || p.op2 ||
          (type != IRT_INT && type != IRT_NUM &&
           type != IRT_FALSE && type != IRT_TRUE) || !(flags & IRT_GUARD) ||
          (flags & ~(IRT_GUARD|IRT_ISPHI))) return 0;
      resultroot = resultref = 0;
      break;
    case IR_CONV:
      if (type == IRT_NUM && (flags & ~IRT_ISPHI) == 0 &&
          p.op2 == IRCONV_NUM_INT &&
          arm64_scalar_postra_ref(v, p.op1, ref, IRT_INT))
        break;
      if (type == IRT_INT && (flags & IRT_GUARD) &&
          (flags & ~(IRT_GUARD | IRT_ISPHI)) == 0 &&
          p.op2 == (IRCONV_INT_NUM | IRCONV_CHECK) &&
          arm64_scalar_postra_ref(v, p.op1, ref, IRT_NUM))
        break;
      return 0;
    case IR_LOOP:
      if (loop || p.t.irt != (IRT_NIL | IRT_GUARD) || p.op12)
        return 0;
      loop = ref;
      break;
    case IR_XPOLL:
      if (poll || !loop || ref != loop + 1 ||
          p.t.irt != (IRT_NIL | IRT_GUARD) || p.op1 != 1 || p.op2)
        return 0;
      poll = ref;
      break;
    case IR_PHI:
      if (!firstphi)
        firstphi = ref;
      if (++nphi > LJ_MAX_PHI || !poll || flags ||
          (type != IRT_INT && type != IRT_NUM && type != IRT_TAB) ||
          p.op1 < REF_FIRST ||
          p.op1 >= loop || p.op2 < REF_FIRST || p.op2 >= firstphi)
        return 0;
      {
        IRRef other;
        for (other = firstphi; other < ref; other++)
          if (ir_load_acq(&v->ir[other]).op1 == p.op1)
            return 0;
      }
      binary = 1;
      break;
    default:
      return 0;
    }
    if (binary && (!arm64_scalar_postra_ref(v, p.op1, ref, type) ||
                   !arm64_scalar_postra_ref(v, p.op2, ref, type)))
      return 0;
    {
      int value = arm64_scalar_postra_value(p) || p.o == IR_PHI;
      if (ra_hasspill(p.s)) {
        MSize width = type == IRT_INT ? 1 : 2, slot;
        /* Values are 32-bit INT or 64-bit NUM/TAB/STR. Scratch addresses remain
        ** register-only at their operation boundary; table values may spill
        ** with the same full-width ownership proof as numeric values. */
        if (!value || (type != IRT_INT && type != IRT_NUM &&
                       type != IRT_TAB && type != IRT_STR) ||
            p.s < SPS_FIRST || (width == 2 && (p.s & 1)) ||
            (MSize)p.s + width > cap)
          return 0;
        if ((MSize)p.s + width > highest)
          highest = (MSize)p.s + width;
        /* Entry revalidation also visits spill-free traces. Initialize the
        ** owner table only when needed. ra_spill never releases a slot;
        ** asm_phi alone aliases a PHI with its right value's full span. */
        if (!spillowners_initialized) {
          memset(spillowner, 0, sizeof(spillowner));
          spillowners_initialized = 1;
        }
        for (slot = p.s; slot < (MSize)p.s + width; slot++) {
          IRRef owner = spillowner[slot];
          if (owner && (p.o != IR_PHI || p.op2 != owner ||
                        ir_load_acq(&v->ir[owner]).s != p.s))
            return 0;
          spillowner[slot] = (IRRef1)ref;
        }
      }
      if (value) {
        if (ra_hasreg(p.r)) {
          if (!arm64_scalar_postra_register(p.r, type))
            return 0;
        } else {
          if (p.r != RID_INIT &&
              !(ra_hashint(p.r) &&
                arm64_scalar_postra_register(ra_gethint(p.r), type)))
            return 0;
          if (!ra_hasspill(p.s) && !arm64_scalar_postra_nolocation(v, ref, n))
            return 0;
        }
      }
    }
  }
  if (!loop || poll != loop + 1)
    return 0;
  for (ref = REF_FIRST; ref < (firstphi ? firstphi : n); ref++) {
    IRRef phi;
    int operand = 0;
    for (phi = firstphi; phi && phi < n; phi++) {
      IRIns p = ir_load_acq(&v->ir[phi]);
      operand |= p.op1 == ref || p.op2 == ref;
    }
    if (!!irt_isphi(ir_load_acq(&v->ir[ref]).t) != operand)
      return 0;
  }
  {
    if (v->spadjust !=
        (highest > SPS_FIXED ? (MSize)sps_scale(sps_align(highest)) : 0))
      return 0;
    for (ref = n; ref < v->nins; ref++) {
      IRIns ren = ir_load_acq(&v->ir[ref]), source;
      if (ren.o == IR_NOP && ref == n && ref + 1 == v->nins)
        break;
      if (ren.o != IR_RENAME || ren.t.irt != IRT_NIL || ren.op1 < REF_FIRST ||
          ren.op1 >= n || ren.op2 >= v->nsnap || ren.s != SPS_NONE)
        return 0;
      source = ir_load_acq(&v->ir[ren.op1]);
      if (!arm64_scalar_postra_value(source) ||
          !arm64_scalar_postra_register(ren.r, (IRType)irt_type(source.t)))
        return 0;
    }
  }
  for (s = 0; s < v->nsnap; s++) {
    const SnapShot *snap = &v->snap[s];
    MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap),
          slots = snap_nslots_acq(snap), j, pos;
    MSize next =
        s + 1 < v->nsnap ? snap_mapofs_acq(&v->snap[s + 1]) : v->nsnapmap;
    IRRef at = snap_ref_acq(snap);
    uint64_t pcbase;
    SnapEntry footer[1 + LJ_FR2];
    if (at < REF_FIRST || at >= n || at < prevsnap || ofs != expectedofs ||
        ofs > v->nsnapmap || next > v->nsnapmap || next < ofs ||
        nent > next - ofs || next - ofs - nent != 1 + LJ_FR2 ||
        slots < 1 + LJ_FR2 || slots > v->root_topslot + 1 + LJ_FR2 ||
        snap_topslot_acq(snap) != v->root_topslot)
      return 0;
    expectedofs = next;
    prevsnap = at;
    if (at == loop) {
      loopsnap = 1;
      loopsnapno = (SnapNo)s;
    }
    if (at <= poll)
      poll_snap = at;
    for (j = 0; j < nent; j++) {
      SnapEntry sn = snapentry_acq(&v->snapmap[ofs + j]);
      IRRef r = snap_ref(sn), ren;
      BCReg slot = snap_slot(sn);
      IRIns p;
      RegSP rs;
      if (slot >= slots ||
          (j && slot <= snap_slot(snapentry_acq(&v->snapmap[ofs + j - 1]))))
        return 0;
      if (sn == SNAP(1, SNAP_FRAME | SNAP_NORESTORE, REF_NIL))
        continue;
      if ((sn & 0x00ff0000u) == SNAP_NORESTORE) {
        MSize idxslot = 1u + LJ_FR2 + bc_a(v->startins);
        IRIns source;
        if (rootop != BC_FORL || irref_isk(r) || r < REF_FIRST || r >= at ||
            (slot != idxslot && slot != idxslot+FORL_STOP &&
             (slot != idxslot+FORL_STEP || !forl_dynamic_step)))
          return 0;
        source = ir_load_acq(&v->ir[r]);
        if (source.o != IR_SLOAD || source.op1 != slot ||
            (source.op2 & (IRSLOAD_CONVERT|IRSLOAD_PARENT)) != 0)
          return 0;
      }
      if (slot < 1 + LJ_FR2 ||
          ((sn & 0x00ff0000u) != 0 &&
           (sn & 0x00ff0000u) != SNAP_NORESTORE) ||
          (!arm64_scalar_postra_ref(v, r, at, IRT_INT) &&
           !arm64_scalar_postra_ref(v, r, at, IRT_NUM) &&
           !arm64_scalar_postra_ref(v, r, at, IRT_TAB) &&
           !arm64_scalar_postra_ref(v, r, at, IRT_STR) &&
           !arm64_scalar_postra_ref(v, r, at, IRT_FALSE) &&
           !arm64_scalar_postra_ref(v, r, at, IRT_TRUE)))
        return 0;
      if (irref_isk(r))
        continue;
      p = ir_load_acq(&v->ir[r]);
      rs = p.prev;
      for (ren = v->nins; ren-- > n;) {
        IRIns q = ir_load_acq(&v->ir[ren]);
        if (q.o != IR_RENAME)
          break;
        if (q.op1 == r && q.op2 <= s)
          rs = q.prev;
      }
      if (ra_hasspill(regsp_spill(rs))) {
        MSize slot = regsp_spill(rs);
        MSize width = irt_type(p.t) == IRT_INT ? 1 : 2;
        if (irt_type(p.t) == IRT_PGC ||
            slot < SPS_FIRST || (width == 2 && (slot & 1)) ||
            slot + width > cap)
          return 0;
      } else if (!arm64_scalar_postra_register(regsp_reg(rs),
                                               (IRType)irt_type(p.t)))
        return 0;
    }
    for (j = 0; j < 1 + LJ_FR2; j++)
      footer[j] = snapentry_acq(&v->snapmap[ofs + nent + j]);
    memcpy(&pcbase, footer, sizeof(pcbase));
    if ((uint8_t)pcbase != v->base_delta ||
        !arm64_ir_pcpos((uintptr_t)(pcbase >> 8), lo, hi, &pos))
      return 0;
  }
  if (!loopsnap || poll_snap != loop)
    return 0;
  /* asm_phi uses either a register or a spill shared with the right
  ** value. asm_phi_copyspill copies that slot to the distinct left slot
  ** on backedges. For register PHIs an owned left spill remains authoritative:
  ** preloop ra_rename may change its register without recording a RENAME. */
  for (ref = firstphi; ref && ref < n; ref++) {
    IRIns phi = ir_load_acq(&v->ir[ref]);
    IRIns left = ir_load_acq(&v->ir[phi.op1]);
    IRIns right = ir_load_acq(&v->ir[phi.op2]);
    IRRef ren;
    Reg rightreg = right.r;
    if (ra_hasspill(phi.s)) {
      if (phi.r != RID_INIT || right.s != phi.s || !ra_hasspill(left.s))
        return 0;
      continue;
    }
    for (ren = v->nins; ren-- > n;) {
      IRIns q = ir_load_acq(&v->ir[ren]);
      if (q.o == IR_RENAME && q.op1 == phi.op2 && q.op2 <= loopsnapno)
        rightreg = q.r;
    }
    {
      IRRef other;
      int right_owner = rightreg == phi.r;
      if (!ra_hasreg(phi.r) || (!ra_hasspill(left.s) && left.r != phi.r))
        return 0;
      for (other = firstphi; other < n; other++) {
        IRIns q = ir_load_acq(&v->ir[other]);
        if (other != ref && q.r == phi.r)
          return 0;
        if (q.op2 == phi.op2 && q.r == rightreg)
          right_owner = 1;
      }
      /* An evicted RHS is saved by ra_dest and reloaded by ra_restore
      ** into the PHI register. Its final register names the producer. */
      if (!right_owner && !ra_hasspill(right.s))
        return 0;
    }
  }
  s = 0;
  for (ref = REF_FIRST; ref < n; ref++) {
    IRIns ir = ir_load_acq(&v->ir[ref]);
    if (irt_isguard(ir.t) && snap_ref_acq(&v->snap[0]) > ref)
      return 0;
    while (s + 1 < v->nsnap && snap_ref_acq(&v->snap[s + 1]) <= ref)
      s++;
    if (ir.o == IR_VLOAD && snap_ref_acq(&v->snap[s]) != ref)
      return 0;
    if (completed_store && ref > completed_store+1 && irt_isguard(ir.t) &&
        snap_ref_acq(&v->snap[s]) <= completed_store)
      return 0;
    if (ir.o == IR_CALLS &&
        (ir.op2 == IRCALL_lj_tab_storenum_rooted ||
         ir.op2 == IRCALL_lj_tab_storebool_rooted))
      completed_store = ref;
    if (rootop != BC_FORL && ir.o == IR_SLOAD &&
        ir.op1 >= snap_nslots_acq(&v->snap[s]))
      return 0;
  }
  if (ninsout)
    *ninsout = n;
  return 1;
}
static int arm64_scalar_postra_admit(const LJArm64PostRAView *v,
                                     IRRef *ninsout)
{
  return arm64_scalar_postra_admit_mode(v, BC_LOOP, ninsout);
}

int lj_asm_arm64_ir_admit(const jit_State *J, const GCtrace *T,
                          LJArm64IRReject *reject)
{
#if LJ_HASJIT_FFI_CALLXS
  /* FFI effects require their own call/ABI proof in addition to scalar
  ** operands. Select that proof by effect, never by constant-pool layout. */
  if (T && T->ir && T->nins > REF_FIRST && T->nins < REF_DROP &&
      bc_op(T->startins) == BC_FORL) {
    IRRef ref;
    for (ref = REF_FIRST; ref < T->nins; ref++)
      if (T->ir[ref].o == IR_CALLXS)
        return arm64_ir_nonloop_admit(J, T, reject);
  }
#endif
  if (T && bc_op(T->startins) == BC_LOOP)
    return arm64_scalar_ir_admit(J, T, reject);
  if (T && bc_op(T->startins) == BC_FORL)
    return arm64_scalar_ir_admit_mode(J, T, BC_FORL, reject);
  return arm64_ir_nonloop_admit(J, T, reject);
}
int lj_asm_arm64_postra_admit(const LJArm64PostRAView *view,
                              IRRef *semantic_ninsp)
{
#if LJ_HASJIT_FFI_CALLXS
  if (view && view->ir && view->nins > REF_FIRST && view->nins < REF_DROP &&
      bc_op(view->startins) == BC_FORL) {
    IRRef ref;
    for (ref = REF_FIRST; ref < view->nins; ref++)
      if (ir_load_acq(&view->ir[ref]).o == IR_CALLXS)
        return arm64_postra_nonloop_admit(view, semantic_ninsp);
  }
#endif
  if (view && bc_op(view->startins) == BC_LOOP)
  {
    return arm64_scalar_postra_admit(view, semantic_ninsp);
  }
  if (view && bc_op(view->startins) == BC_FORL)
    return arm64_scalar_postra_admit_mode(view, BC_FORL, semantic_ninsp);
  return arm64_postra_nonloop_admit(view, semantic_ninsp);
}

#endif
