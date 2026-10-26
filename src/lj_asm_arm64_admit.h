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
** self-linked scalar BC_LOOP roots, separately certified integer BC_FORL
** evolution, fixed-function scalar bodies, and first scalar side traces.
** Independent semantic and allocator certificates cover checked INT
** arithmetic, NUM arithmetic/conversions, comparisons and same-type PHIs.
** Rooted helpers have per-operation input, output and retry contracts,
** independent of trace length, table count or constant key values. Unsunk
** table construction and fixed scalar FFI calls have their own publication,
** ABI and snapshot contracts. Unproved heap and call effects remain closed.
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
/* ABI-driven CALLXS graph walk. This scan intentionally derives roles from
** operation relationships and bounds, not from recorder-assigned IR ordinals. */
typedef struct LJArm64CallXSGraph {
  IRRef loop;
  IRRef poll;
} LJArm64CallXSGraph;

static int arm64_callxs_ref_valid(const IRIns *ir, IRRef nk, IRRef nins,
				 IRRef limit, IRRef ref);
static int arm64_callxs_phi_types_match(const IRIns *ir, IRRef nk,
		IRRef ref, const IRIns *phi);

static int arm64_callxs_value_kind(const IRIns *ir, IRRef ref,
				   IRRef nk, IRRef nins, IRRef limit,
				   LJArm64ABIKind kind, unsigned size,
				   int is_unsigned, int on_stack)
{
  IRIns value;
  IRType type;
  if (!arm64_callxs_ref_valid(ir, nk, nins, limit, ref))
    return 0;
  value = ir_load_acq(&ir[ref]);
  type = (IRType)irt_type(value.t);
  if (value.o == IR_CONV) {
    IRType from;
    if (value.op1 >= ref ||
	!arm64_callxs_ref_valid(ir, nk, nins, ref, value.op1))
      return 0;
    from = (IRType)irt_type(ir_load_acq(&ir[value.op1]).t);
    if (kind == LJ_ARM64_ABI_KIND_FLOAT && size == 8 &&
	(type == IRT_NUM && from == IRT_INT &&
	 (value.op2 & IRCONV_MODEMASK) == IRCONV_NUM_INT &&
	 ((value.op2 & IRCONV_CONVMASK) == 0 ||
	  (value.op2 & IRCONV_CONVMASK) == IRCONV_ANY))) {
      /* This is the recorder's defined integral-to-double conversion. */
    } else if (kind == LJ_ARM64_ABI_KIND_FLOAT && size == 4 &&
	       type == IRT_FLOAT &&
	       (from == IRT_NUM || from == IRT_INT) &&
	       value.t.irt == IRT_FLOAT &&
	       value.op2 == ((IRT_FLOAT << IRCONV_DSH) | from)) {
	/* Narrow scalar conversion for a fixed float argument. */
    } else if (kind == LJ_ARM64_ABI_KIND_FLOAT && size == 8 &&
	       type == IRT_NUM && from == IRT_FLOAT &&
	       value.t.irt == IRT_NUM &&
	       value.op2 == ((IRT_NUM << IRCONV_DSH) | IRT_FLOAT)) {
	/* Widen a float result before passing it as a double argument. */
    } else if ((kind == LJ_ARM64_ABI_KIND_INTEGER ||
                kind == LJ_ARM64_ABI_KIND_ENUM) && on_stack &&
               (size == 1 || size == 2) && from == IRT_INT &&
               type == (size == 1 ? IRT_U8 : IRT_U16) &&
               value.t.irt == type &&
               value.op2 == ((type << IRCONV_DSH)|IRT_INT)) {
      /* The store consumes the low CType-width bits. Signed and unsigned
      ** fixed stack arguments use the same unpromoted carrier. */
    } else return 0;
  }
  switch (kind) {
  case LJ_ARM64_ABI_KIND_INTEGER:
  case LJ_ARM64_ABI_KIND_ENUM:
	if (size == 8)
	  return type == (is_unsigned ? IRT_U64 : IRT_I64);
	if (size == 4)
	  return type == (is_unsigned ? IRT_U32 : IRT_INT);
	/* Registers carry sign/zero-extended INTs. Fixed stack arguments retain
	** their CType width; the store consumes the low bits of that carrier. */
	return (size == 1 || size == 2) &&
          type == (on_stack ? (size == 1 ? IRT_U8 : IRT_U16) : IRT_INT);
  case LJ_ARM64_ABI_KIND_FLOAT:
	return (size == 4 && type == IRT_FLOAT) ||
	       (size == 8 && type == IRT_NUM);
  case LJ_ARM64_ABI_KIND_POINTER:
    if (type == IRT_P64 && value.o == IR_ADD &&
	value.op1 >= REF_FIRST && value.op1 < ref &&
	irt_type(ir_load_acq(&ir[value.op1]).t) == IRT_STR) {
      IRIns offset;
      if (value.op2 < nk || value.op2+1u >= REF_TRUE)
	return 0;
      offset = ir_load_acq(&ir[value.op2]);
      if (offset.o != IR_KINT64 || offset.t.irt != IRT_I64 ||
	  offset.op12 != 0 ||
	  ir_load_acq(&ir[value.op2+1u]).tv.u64 != (uint64_t)sizeof(GCstr))
	return 0;
    }
    return type == IRT_P64;
  default:
    return 0;
  }
}

static IRType arm64_callxs_result_type(const LJArm64ABISignature *abi)
{
  if (abi->result_kind == LJ_ARM64_ABI_KIND_VOID)
    return IRT_NIL;
  if (abi->result_kind == LJ_ARM64_ABI_KIND_POINTER)
    return IRT_P64;
  if (abi->result_kind == LJ_ARM64_ABI_KIND_FLOAT)
    return abi->result_size == 4 ? IRT_FLOAT : IRT_NUM;
  if (abi->result_kind == LJ_ARM64_ABI_KIND_INTEGER ||
      abi->result_kind == LJ_ARM64_ABI_KIND_ENUM) {
    switch (abi->result_size) {
    case 1: return abi->result_unsigned ? IRT_U8 : IRT_I8;
    case 2: return abi->result_unsigned ? IRT_U16 : IRT_I16;
    case 4: return abi->result_unsigned ? IRT_U32 : IRT_INT;
    case 8: return abi->result_unsigned ? IRT_U64 : IRT_I64;
    default: return IRT_CDATA;
    }
  }
  return IRT_CDATA;
}

/* These scalar ABI results are returned to Lua in a fresh CData root. The
** argument plan, including promoted variadic arguments, remains independent. */
static int arm64_callxs_result_boxed(const LJArm64ABISignature *abi)
{
  return abi->result_class == LJ_ARM64_ABI_GPR && abi->result_size == 8 &&
    (abi->result_kind == LJ_ARM64_ABI_KIND_POINTER ||
     abi->result_kind == LJ_ARM64_ABI_KIND_INTEGER);
}

static IRRef arm64_callxs_const_next(const IRIns *ir, IRRef ref)
{
	IRIns k = ir_load_acq(&ir[ref]);
	if (k.o == IR_KNUM || k.o == IR_KINT64 ||
	    (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR ||
			 k.o == IR_KKPTR)))
		return ref+1u < REF_TRUE ? ref+2u : REF_DROP;
	return ref+1u;
}

static int arm64_callxs_const_member(const IRIns *ir, IRRef nk, IRRef target)
{
	IRRef ref;
	if (target < nk || target >= REF_TRUE) return 0;
	for (ref = nk; ref < REF_TRUE; ) {
		IRIns k = ir_load_acq(&ir[ref]);
		if (ref == target) {
		  IRType t = irt_type(k.t);
		  if ((k.o == IR_KNUM || k.o == IR_KINT64 ||
		       (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR ||
				    k.o == IR_KKPTR))) && ref+1u >= REF_TRUE)
		    return 0;
		  switch ((IROp)k.o) {
		  case IR_KNUM: return t == IRT_NUM || t == IRT_P64;
		  case IR_KINT: return t == IRT_INT || t == IRT_I8 || t == IRT_U8 ||
			  t == IRT_I16 || t == IRT_U16 || t == IRT_U32;
		  case IR_KINT64: return t == IRT_I64 || t == IRT_U64 || t == IRT_P64;
		  case IR_KGC: return t == IRT_PGC || t == IRT_P64 || t == IRT_STR ||
			  t == IRT_FUNC || t == IRT_TAB || t == IRT_CDATA ||
			  t == IRT_UDATA || t == IRT_PROTO || t == IRT_THREAD;
		  case IR_KPTR: case IR_KKPTR:
		    return t == IRT_P32 || t == IRT_P64 || t == IRT_LIGHTUD ||
			   t == IRT_STR || t == IRT_FUNC || t == IRT_TAB;
		  case IR_KPRI: return t == IRT_FALSE || t == IRT_TRUE || t == IRT_NIL;
		  case IR_KNULL: return t == IRT_NIL || t == IRT_CDATA;
		  default: return 0;
		  }
		}
		if (k.o == IR_KNUM || k.o == IR_KINT64 ||
		    (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR ||
				 k.o == IR_KKPTR))) {
			if (ref+1u >= REF_TRUE) return 0;
			ref += 2u;
		} else if (k.o == IR_KINT || k.o == IR_KGC || k.o == IR_KPTR ||
			   k.o == IR_KKPTR || k.o == IR_KPRI || k.o == IR_KNULL) {
			ref++;
		} else return 0;
	}
	return 0;
}

static int arm64_callxs_ref_valid(const IRIns *ir, IRRef nk, IRRef nins,
				 IRRef limit, IRRef ref)
{
  if (ref >= REF_DROP) return 0;
  if (ref >= REF_TRUE && ref < REF_FIRST)
	return ref == REF_TRUE || ref == REF_FALSE || ref == REF_NIL;
  if (ref < REF_TRUE)
	return arm64_callxs_const_member(ir, nk, ref);
  return ref >= REF_FIRST && ref < nins && ref < limit;
}

static IRType arm64_callxs_ref_type(const IRIns *ir, IRRef nk, IRRef ref)
{
	if (ref == REF_NIL) return IRT_NIL;
	if (ref == REF_FALSE) return IRT_FALSE;
	if (ref == REF_TRUE) return IRT_TRUE;
	if (ref < REF_TRUE && !arm64_callxs_const_member(ir, nk, ref))
		return IRT__MAX;
	return (IRType)irt_type(ir_load_acq(&ir[ref]).t);
}

static int arm64_callxs_integer_type(IRType t)
{
	return t >= IRT_I8 && t <= IRT_U64;
}

static int arm64_callxs_types_comparable(IRType a, IRType b)
{
	return a == b ||
	    (a >= IRT_LIGHTUD && a <= IRT_UDATA &&
	     b >= IRT_LIGHTUD && b <= IRT_UDATA) ||
	    (arm64_callxs_integer_type(a) && arm64_callxs_integer_type(b) &&
	     ((IRT_IS64 >> a) & 1u) == ((IRT_IS64 >> b) & 1u));
}

static int arm64_callxs_add_shape(const IRIns *ir, IRRef nk,
				  IRType result_type, IRRef op1, IRRef op2)
{
	IRType a = arm64_callxs_ref_type(ir, nk, op1);
	IRType b = arm64_callxs_ref_type(ir, nk, op2);
	if (result_type == IRT_INT || result_type == IRT_NUM)
		return a == result_type && b == result_type;
	/* Address ADD is the other recorded form: a pointer-class base plus an
	** explicit integer offset. */
	if (result_type >= IRT_LIGHTUD && result_type <= IRT_UDATA &&
	    a >= IRT_LIGHTUD && a <= IRT_UDATA &&
	    (b == IRT_INT || b == IRT_U32 || b == IRT_I64 || b == IRT_U64))
		return 1;
	return 0;
}

static int arm64_callxs_int_overflow_shape(const IRIns *ir, IRRef nk,
		IRRef nins, IRRef ref, const IRIns *ins)
{
	return (ins->o == IR_ADDOV || ins->o == IR_SUBOV || ins->o == IR_MULOV) &&
	    ins->t.irt == (IRT_INT|IRT_GUARD) && ins->op1 < ref &&
	    ins->op2 < ref &&
	    arm64_callxs_ref_valid(ir, nk, nins, ref, ins->op1) &&
	    arm64_callxs_ref_valid(ir, nk, nins, ref, ins->op2) &&
	    arm64_callxs_ref_type(ir, nk, ins->op1) == IRT_INT &&
	    arm64_callxs_ref_type(ir, nk, ins->op2) == IRT_INT;
}

/* Walk constant-pool members, skipping the payload word paired with 64-bit
** constants. This prevents forged refs into KNUM/KINT64 payloads. */
static int arm64_callxs_kint_value(const IRIns *ir, IRRef nk, IRRef target,
		int32_t *valuep)
{
	IRRef ref;
	for (ref = nk; ref < REF_TRUE; ) {
	  IRIns k = ir_load_acq(&ir[ref]);
	  if (ref == target) {
	    if (k.o != IR_KINT || k.t.irt != IRT_INT) return 0;
	    if (valuep != NULL) *valuep = k.i;
	    return 1;
	  }
	  if (k.o == IR_KNUM || k.o == IR_KINT64 ||
	      (LJ_GC64 && (k.o == IR_KGC || k.o == IR_KPTR ||
			   k.o == IR_KKPTR))) {
	    if (ref+1u >= REF_TRUE) return 0;
	    ref += 2u;
	  } else if (k.o == IR_KINT || k.o == IR_KGC || k.o == IR_KPTR ||
	     k.o == IR_KKPTR || k.o == IR_KPRI || k.o == IR_KNULL) {
	    ref++;
	  } else return 0;
	}
	return 0;
}

static int arm64_callxs_xsave_anchor(const IRIns *ir, IRRef nins,
	IRRef xsave, const BCIns *proto_bc, MSize proto_sizebc,
	uintptr_t *returnpcp, BCIns *sourceinsp)
{
	IRIns marker;
	uint32_t offset;
	BCOp op;
	if (xsave < REF_FIRST || xsave >= nins || proto_bc == NULL ||
	    proto_sizebc == 0)
		return 0;
	marker = ir_load_acq(&ir[xsave]);
	if (marker.o != IR_XSAVE || marker.t.irt != IRT_NIL ||
	    marker.op1 >= REF_BIAS || marker.op2 >= REF_BIAS ||
	    (marker.op1 == 0 && marker.op2 == 0))
		return 0;
	offset = (uint32_t)marker.op1 | ((uint32_t)marker.op2 << 15);
	if (offset >= LJ_MAX_BCINS || (uint64_t)offset >= proto_sizebc)
		return 0;
	op = bc_op((BCIns)la_load32_acq(
		(const uint32_t *)&proto_bc[(MSize)offset-1u]));
	if (op != BC_CALL && op != BC_CALLM)
		return 0;
	if (returnpcp != NULL)
		*returnpcp = (uintptr_t)(const void *)&proto_bc[offset];
	if (sourceinsp != NULL)
		*sourceinsp = (BCIns)la_load32_acq(
		  (const uint32_t *)&proto_bc[(MSize)offset-1u]);
	return 1;
}

/* CALLM is admitted only when its complete variable tail is the single
** result of the immediately preceding multires CALL. */
static int arm64_callxs_callm_pair(const BCIns *bc, MSize sizebc,
	uintptr_t producer_returnpc, uintptr_t consumer_returnpc,
	unsigned nargs)
{
	uintptr_t lo = (uintptr_t)(const void *)bc;
	uintptr_t hi;
	uintptr_t consumer_pc;
	MSize consumer_index;
	BCIns producer, consumer;
	if (bc == NULL || sizebc < 3u || sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns))
		return 0;
	hi = lo+(uintptr_t)sizebc*sizeof(BCIns);
	if (consumer_returnpc < lo+2u*sizeof(BCIns) || consumer_returnpc >= hi ||
	    producer_returnpc != consumer_returnpc-sizeof(BCIns) ||
	    ((consumer_returnpc-lo) % sizeof(BCIns)) != 0)
		return 0;
	consumer_pc = consumer_returnpc-sizeof(BCIns);
	consumer_index = (MSize)((consumer_pc-lo)/sizeof(BCIns));
	if (consumer_index == 0 || consumer_index >= sizebc)
		return 0;
	producer = (BCIns)la_load32_acq((const uint32_t *)&bc[consumer_index-1u]);
	consumer = (BCIns)la_load32_acq((const uint32_t *)&bc[consumer_index]);
	if (bc_op(producer) != BC_CALL || bc_b(producer) != 0 ||
	    bc_op(consumer) != BC_CALLM ||
	    nargs != (unsigned)bc_c(consumer)+1u ||
	    (unsigned)bc_a(producer) != (unsigned)bc_a(consumer)+
	      (unsigned)bc_c(consumer)+1u+LJ_FR2)
		return 0;
	return 1;
}

static int arm64_callxs_offset_value(const IRIns *ir, IRRef nk,
				     IRRef target, int64_t *valuep)
{
	IRIns k;
	if (!arm64_callxs_const_member(ir, nk, target)) return 0;
	k = ir_load_acq(&ir[target]);
	if (k.o == IR_KINT && k.t.irt == IRT_INT) {
	  if (valuep != NULL) *valuep = k.i;
	  return 1;
	}
	if (k.o == IR_KINT64 && k.t.irt == IRT_I64 && k.op12 == 0 &&
	    target+1u < REF_TRUE) {
	  uint64_t value = ir_load_acq(&ir[target+1u]).tv.u64;
	  if (value > INT64_MAX) return 0;
	  if (valuep != NULL) *valuep = (int64_t)value;
	  return 1;
	}
	return 0;
}

/* Reconstruct the ABI for the concrete cdata/function pointer consumed by a
** CALLXS. A trace may call unrelated function cdata values with different
** signatures; no trace-global CType or function pointer is authoritative. */
static int arm64_callxs_signature_for_call(CTState *cts, const IRIns *ir,
		IRRef nk, IRRef nins, IRRef call,
		LJArm64ABISignature *abi, IRRef *funcptrp)
{
	IRIns c, fp;
	IRRef fpref, cdref, ref;
	int32_t wrapper_id = 0;
	int found = 0;
	if (call < REF_FIRST || call >= nins ||
	    (c = ir_load_acq(&ir[call])).o != IR_CALLXS ||
	    (fpref = c.op2) < REF_FIRST || fpref >= call)
	  return 0;
	fp = ir_load_acq(&ir[fpref]);
	/* The generic recorder carries the variadic CType on the callable, not
	** in the argument list. Its identity must match the guarded cdata. */
	if (fp.o == IR_CARG) {
	  if (fp.t.irt != IRT_NIL || fp.op1 < REF_FIRST || fp.op1 >= fpref ||
	      !arm64_callxs_kint_value(ir, nk, fp.op2, &wrapper_id) ||
	      wrapper_id <= 0)
	    return 0;
	  fpref = fp.op1;
	  fp = ir_load_acq(&ir[fpref]);
	}
	if (fp.o != IR_FLOAD || fp.op2 != IRFL_CDATA_PTR ||
	    (cdref = fp.op1) < REF_FIRST || cdref >= fpref ||
	    ir_load_acq(&ir[cdref]).o != IR_SLOAD)
	  return 0;
	for (ref = REF_FIRST; ref < call; ref++) {
	  IRIns id = ir_load_acq(&ir[ref]);
	  IRRef guard;
	  if (id.o != IR_FLOAD || id.op1 != cdref ||
	      id.op2 != IRFL_CDATA_CTYPEID)
	    continue;
	  for (guard = ref+1u; guard < call; guard++) {
	    IRIns eq = ir_load_acq(&ir[guard]);
	    IRRef kref;
	    IRIns k;
	    if (eq.o != IR_EQ || !irt_isguard(eq.t) ||
		(eq.op1 != ref && eq.op2 != ref))
	      continue;
	    kref = eq.op1 == ref ? eq.op2 : eq.op1;
	    if (kref < nk || kref >= REF_TRUE ||
		!arm64_callxs_kint_value(ir, nk, kref, NULL))
		      return 0;
	    k = ir_load_acq(&ir[kref]);
	    if (k.i <= 0 || !lj_ccall_arm64_signature(cts, (CTypeID)k.i, abi)) {
	      return 0;
	    }
	    if ((abi->is_vararg && wrapper_id != k.i) ||
		(!abi->is_vararg && wrapper_id != 0) || found)
	      return 0;
	    found = 1;
	    break;
	  }
	}
	if (found && funcptrp != NULL) *funcptrp = fpref;
	return found;
}

/* Prove that one CNEW is the root box allocated for one boxed-scalar
** CALLXS. The call's XSTORE must use this box and the exact GCcdata payload
** offset; the signature's result CType must match the allocation CType. */
static int arm64_callxs_box_call(CTState *cts, const IRIns *ir, IRRef nk,
		IRRef nins, IRRef boxref, IRRef *callrefp, CTypeID *ctypep)
{
	IRIns box;
	IRRef ref, found = 0;
	int32_t ctype;
	if (boxref < REF_FIRST || boxref >= nins ||
	    (box = ir_load_acq(&ir[boxref])).o != IR_CNEW ||
	    irt_type(box.t) != IRT_CDATA || !irt_isguard(box.t) ||
	    box.op2 != REF_NIL || box.op1 < nk ||
	    !arm64_callxs_kint_value(ir, nk, box.op1, &ctype) || ctype <= 0)
		return 0;
	for (ref = REF_FIRST; ref < nins; ref++) {
		IRIns store = ir_load_acq(&ir[ref]);
		IRIns payload;
		int64_t offset;
		IRRef callref;
		LJArm64ABISignature abi;
		if (store.o != IR_XSTORE || store.op1 < REF_FIRST ||
		    store.op1 >= ref || store.op2 < REF_FIRST || store.op2 >= ref)
			continue;
		payload = ir_load_acq(&ir[store.op1]);
		if (payload.o != IR_ADD || irt_type(payload.t) != IRT_P64 ||
		    payload.op1 != boxref ||
		    !arm64_callxs_offset_value(ir, nk, payload.op2, &offset) ||
		    offset != (int64_t)sizeof(GCcdata))
			continue;
		callref = store.op2;
		if (!arm64_callxs_signature_for_call(cts, ir, nk, nins,
		      callref, &abi, NULL) ||
		    !arm64_callxs_result_boxed(&abi) ||
		    abi.result_id != (CTypeID)ctype ||
		    irt_type(store.t) != arm64_callxs_result_type(&abi) ||
		    irt_type(ir_load_acq(&ir[callref]).t) !=
		      arm64_callxs_result_type(&abi) || found != 0)
			return 0;
		found = callref;
	}
	if (found == 0) return 0;
	if (callrefp) *callrefp = found;
	if (ctypep) *ctypep = (CTypeID)ctype;
	return 1;
}

/* A CData loop PHI may join two distinct per-iteration CNEW boxes. Validate
** each incoming allocation against its own independently proven CALLXS store. */
static int arm64_callxs_cdata_phi(CTState *cts, const IRIns *ir,
		IRRef nk, IRRef nins, IRRef ref, const IRIns *phi)
{
	IRRef refs[2];
	CTypeID ctypes[2] = { 0, 0 };
	unsigned i;
	if (irt_type(phi->t) != IRT_CDATA) return 1;
	if (phi->op1 < REF_FIRST || phi->op1 >= ref ||
	    phi->op2 < REF_FIRST || phi->op2 >= ref)
		return 0;
	refs[0] = phi->op1;
	refs[1] = phi->op2;
	for (i = 0; i < 2; i++) {
		IRIns input = ir_load_acq(&ir[refs[i]]);
		if (input.o == IR_CNEW) {
			if (!arm64_callxs_box_call(cts, ir, nk, nins, refs[i],
			      NULL, &ctypes[i]))
				return 0;
		} else if (input.o != IR_SLOAD || irt_type(input.t) != IRT_CDATA ||
			   !irt_isguard(input.t) || input.op1 < 1u+LJ_FR2 ||
			   input.op2 & ~(IRSLOAD_PARENT|IRSLOAD_FRAME|IRSLOAD_TYPECHECK|
			     IRSLOAD_CONVERT|IRSLOAD_READONLY|IRSLOAD_INHERIT|IRSLOAD_KEYINDEX)) return 0;
	}
	if (ctypes[0] != 0 && ctypes[1] != 0 && ctypes[0] != ctypes[1])
		return 0;
	return 1;
}

static int arm64_callxs_has_equal_guard(const IRIns *ir, IRRef nins,
				       IRRef nk, IRRef value, IRType type)
{
  IRRef ref;
  for (ref = value+1u; ref < nins; ref++) {
    IRIns use = ir_load_acq(&ir[ref]);
    IRRef other;
    IRType otype;
    if (use.op1 != value && use.op2 != value)
      continue;
    /* The identity check must dominate the first consumption of the loaded
    ** value. A later equality cannot retroactively guard an earlier use. */
    if (use.o != IR_EQ || !irt_isguard(use.t) ||
	irt_type(use.t) != type)
      return 0;
    other = use.op1 == value ? use.op2 : use.op1;
    if (!arm64_callxs_ref_valid(ir, nk, nins, ref, other)) return 0;
    otype = arm64_callxs_ref_type(ir, nk, other);
    return (type == IRT_TAB || type == IRT_FUNC) ? otype == type :
	arm64_callxs_types_comparable(type, otype);
  }
  return 0;
}

/* A result need not be compared at all. If the graph does compare it, every
** equality guard must preserve the result's IR type. */
static int arm64_callxs_result_guard_types(const IRIns *ir, IRRef nins,
					   IRRef nk, IRRef value, IRType type)
{
  IRRef ref;
  int found = 0;
  for (ref = value+1u; ref < nins; ref++) {
    IRIns eq = ir_load_acq(&ir[ref]);
    if (eq.o == IR_EQ && irt_isguard(eq.t) &&
	(eq.op1 == value || eq.op2 == value)) {
      found = 1;
      break;
    }
  }
  if (!found) return 1;
  if (!arm64_callxs_has_equal_guard(ir, nins, nk, value, type)) return 0;
  return 1;
}

static int arm64_callxs_carg_values(const IRIns *ir, IRRef nk, IRRef nins,
				    IRRef call, unsigned nargs,
				    IRRef *values)
{
  IRIns ins = ir_load_acq(&ir[call]);
  IRRef ref = ins.op1;
  unsigned i;
  if (nargs == 0)
    return ref == 0;
  if (nargs > LJ_ARM64_ABI_MAX_ARGS)
    return 0;
  for (i = nargs; i > 1u; i--) {
    IRIns carg;
    if (ref < REF_FIRST || ref >= call || ref >= nins) return 0;
    carg = ir_load_acq(&ir[ref]);
    if (carg.o != IR_CARG || carg.t.irt != IRT_NIL ||
	carg.op1 < nk || carg.op1 >= ref ||
	carg.op2 < nk || carg.op2 >= ref)
      return 0;
    values[i-1u] = carg.op2;
    ref = carg.op1;
  }
  if (ref < nk || ref >= call || ref >= nins)
    return 0;
  values[0] = ref;
  return 1;
}

/* Decode only the bounded CARG tree. The semantic and allocated passes
** separately certify its fixed prefix, variadic marker and source arity. */
static int arm64_callxs_arglist(const IRIns *ir, IRRef nk, IRRef nins,
                                IRRef call, IRRef *values, unsigned *countp)
{
  IRRef reversed[LJ_ARM64_ABI_MAX_ARGS];
  IRRef ref = ir_load_acq(&ir[call]).op1;
  unsigned n = 0, i;
  /* Empty fixed signatures remain outside this admission tranche. */
  if (ref == REF_NIL) return 0;
  for (;;) {
    IRIns value;
    if (n == LJ_ARM64_ABI_MAX_ARGS ||
        !arm64_callxs_ref_valid(ir, nk, nins, call, ref))
      return 0;
    value = ir_load_acq(&ir[ref]);
    if (value.o != IR_CARG) {
      reversed[n++] = ref;
      break;
    }
    if (value.t.irt != IRT_NIL ||
        !arm64_callxs_ref_valid(ir, nk, nins, ref, value.op1) ||
        !arm64_callxs_ref_valid(ir, nk, nins, ref, value.op2))
      return 0;
    reversed[n++] = value.op2;
    ref = value.op1;
  }
  for (i = 0; i < n; i++) values[i] = reversed[n-i-1u];
  *countp = n;
  return 1;
}

/* Bind one ABI operand to the value staged by the source Lua frame. This
** semantic proof is separate from the allocated snapshot/carrier proof. */
static int arm64_callxs_source_value(const GCtrace *T, IRRef xsave,
                                    IRRef source, IRRef value, int tail)
{
  IRIns src, dst;
  IRType st, dt;
  if (!arm64_callxs_ref_valid(T->ir, T->nk, T->nins, xsave, source) ||
      !arm64_callxs_ref_valid(T->ir, T->nk, T->nins, xsave, value))
    return 0;
  src = ir_load_acq(&T->ir[source]);
  dst = ir_load_acq(&T->ir[value]);
  st = arm64_callxs_ref_type(T->ir, T->nk, source);
  dt = arm64_callxs_ref_type(T->ir, T->nk, value);
  if (!tail && source == value) return 1;
  /* C integer conversion drops the overflow check from an already guarded
  ** Lua arithmetic value. Its operands must still be exactly the same. */
  if (!tail && st == IRT_INT && dt == IRT_INT && irt_isguard(src.t) &&
      !irt_isguard(dst.t) && src.op1 == dst.op1 && src.op2 == dst.op2 &&
      ((src.o == IR_ADDOV && dst.o == IR_ADD) ||
       (src.o == IR_SUBOV && dst.o == IR_SUB) ||
       (src.o == IR_MULOV && dst.o == IR_MUL)))
    return 1;
  if (!tail && dst.o == IR_CONV && dst.op1 == source) {
    /* The independent CType/carrier and operation checks certify the
    ** conversion itself. Here its source must be the actual caller value. */
    return 1;
  }
  if ((st == IRT_INT || st == IRT_NUM) && dt == IRT_NUM) {
    if (st == IRT_NUM && source == value) return 1;
    if (st == IRT_INT && dst.o == IR_CONV && dst.op1 == source &&
        dst.t.irt == IRT_NUM && dst.op2 == IRCONV_NUM_INT)
      return 1;
    if (src.o == IR_KINT && st == IRT_INT && dst.o == IR_KNUM &&
        dst.t.irt == IRT_NUM && dst.op12 == 0 && value+1u < REF_TRUE) {
      TValue converted;
      setnumV(&converted, (lua_Number)src.i);
      return converted.u64 == ir_load_acq(&T->ir[value+1u]).tv.u64;
    }
    return 0;
  }
  if (st == IRT_TRUE || st == IRT_FALSE)
    return dst.o == IR_KINT && dst.t.irt == IRT_INT &&
           dst.i == (st == IRT_TRUE);
  if ((st == IRT_STR || st == IRT_NIL) && dt == IRT_P64) {
    if (st == IRT_STR && dst.o == IR_ADD && dst.op1 == source) {
      int64_t offset;
      return arm64_callxs_offset_value(T->ir, T->nk, dst.op2, &offset) &&
             offset == (int64_t)sizeof(GCstr);
    }
    if ((dst.o == IR_KPTR || dst.o == IR_KKPTR) &&
        dst.t.irt == IRT_P64 && dst.op12 == 0 && value+1u < REF_TRUE) {
      uint64_t ptr = ir_load_acq(&T->ir[value+1u]).tv.u64;
      if (st == IRT_NIL) return ptr == 0;
      if (src.o == IR_KGC && src.t.irt == IRT_STR && src.op12 == 0 &&
          source+1u < REF_TRUE) {
        uint64_t str = ir_load_acq(&T->ir[source+1u]).tv.u64;
        return str <= UINTPTR_MAX-sizeof(GCstr) &&
               ptr == str+sizeof(GCstr);
      }
    }
  }
  return 0;
}

/* Read one exact, restorable slot from an effect-boundary snapshot. This is
** metadata validation only: each admission pass separately derives the box,
** marker, caller slot and frame geometry from its own CALLXS lifecycle. */
static int arm64_callxs_snapshot_box_slot(const SnapShot *snapshots,
    const SnapEntry *map, MSize nsnap, MSize nsnapmap, IRRef at,
    MSize base, MSize slot, IRRef box, int hidden)
{
  const SnapShot *snap = NULL;
  MSize s, ofs, nent, nslots, i;
  unsigned found = 0;
  SnapEntry footer_words[1+LJ_FR2];
  uint64_t footer;
  if (snapshots == NULL || map == NULL || base < 1u+LJ_FR2 ||
      base > UINT8_MAX || slot > UINT8_MAX || box < REF_FIRST || box >= at)
    return 0;
  for (s = 0; s < nsnap; s++) {
    if (snap_ref_acq(&snapshots[s]) == at) {
      if (snap != NULL) return 0;
      snap = &snapshots[s];
    }
  }
  if (snap == NULL || snap_count_acq(snap) != SNAPCOUNT_DONE) return 0;
  ofs = snap_mapofs_acq(snap);
  nent = snap_nent_acq(snap);
  nslots = snap_nslots_acq(snap);
  if (ofs > nsnapmap || nent > nsnapmap-ofs ||
      nsnapmap-ofs-nent < 1u+LJ_FR2 || slot >= nslots ||
      (hidden && nslots != slot+1u)) return 0;
  for (i = 0; i < 1u+LJ_FR2; i++)
    footer_words[i] = snapentry_acq(&map[ofs+nent+i]);
  memcpy(&footer, footer_words, sizeof(footer));
  if ((uint8_t)footer != base-(1u+LJ_FR2)) return 0;
  for (i = 0; i < nent; i++) {
    SnapEntry entry = snapentry_acq(&map[ofs+i]);
    if (snap_slot(entry) == slot) {
      /* A fresh CNEW cannot be an unchanged SLOAD or a NORESTORE value. */
      if (entry != SNAP(slot, 0, box) || found++) return 0;
    }
  }
  return found == 1;
}

static int arm64_callxs_vararg_sources(const GCtrace *T, IRRef xsave,
    IRRef fpref, BCIns sourceins, uintptr_t returnpc,
    const LJArm64ABISignature *abi, unsigned nargs, const IRRef *values)
{
  const SnapShot *snapshot = NULL;
  IRRef sources[LJ_ARM64_ABI_MAX_ARGS];
  IRRef frame = 0, callable = 0, cdata = 0;
  MSize base = 1u+LJ_FR2+(MSize)bc_a(sourceins)+1u+LJ_FR2;
  MSize ofs, nent, nslots, i, prior_slot = 0;
  SnapNo s;
  uint64_t footer;
  SnapEntry footer_words[1+LJ_FR2];
  IRIns fn, pc, fp;
  GCobj *obj;
  if (nargs > LJ_ARM64_ABI_MAX_ARGS || nargs < abi->nargs ||
      base > UINT8_MAX || base+1u+nargs > UINT8_MAX ||
      T->snap == NULL || T->snapmap == NULL)
    return 0;
  memset(sources, 0, sizeof(sources));
  for (s = 0; s < T->nsnap; s++) {
    if (snap_ref_acq(&T->snap[s]) == xsave) {
      if (snapshot != NULL) return 0;
      snapshot = &T->snap[s];
    }
  }
  if (snapshot == NULL || snap_count_acq(snapshot) != SNAPCOUNT_DONE) return 0;
  ofs = snap_mapofs_acq(snapshot);
  nent = snap_nent_acq(snapshot);
  nslots = snap_nslots_acq(snapshot);
  if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs ||
      T->nsnapmap-ofs-nent < 1u+LJ_FR2 || base+1u+nargs > nslots)
    return 0;
  for (i = 0; i < 1u+LJ_FR2; i++)
    footer_words[i] = snapentry_acq(&T->snapmap[ofs+nent+i]);
  memcpy(&footer, footer_words, sizeof(footer));
  if ((uint8_t)footer != base-(1u+LJ_FR2)) return 0;
  for (i = 0; i < nent; i++) {
    SnapEntry entry = snapentry_acq(&T->snapmap[ofs+i]);
    MSize slot = snap_slot(entry);
    IRRef value = snap_ref(entry);
    uint32_t flags = entry & 0x00ff0000u;
    if (slot >= nslots || (i != 0 && slot <= prior_slot)) return 0;
    prior_slot = slot;
    if (slot == base-1u) {
      if (flags != SNAP_FRAME) return 0;
      frame = value;
    } else if (slot == base-1u-LJ_FR2) {
      if (flags != 0) return 0;
      callable = value;
    } else if (slot == base) {
      if (flags != 0) return 0;
      cdata = value;
    } else if (slot > base && slot <= base+nargs) {
      if (flags != 0) return 0;
      sources[slot-base-1u] = value;
    }
  }
  if (!arm64_callxs_const_member(T->ir, T->nk, frame) ||
      !arm64_callxs_const_member(T->ir, T->nk, callable) ||
      fpref < REF_FIRST || fpref >= xsave) return 0;
  pc = ir_load_acq(&T->ir[frame]);
  fn = ir_load_acq(&T->ir[callable]);
  fp = ir_load_acq(&T->ir[fpref]);
  if (pc.o != IR_KNUM || pc.t.irt != IRT_NUM || pc.op12 != 0 ||
      frame+1u >= REF_TRUE ||
      ir_load_acq(&T->ir[frame+1u]).tv.u64 != (uint64_t)returnpc ||
      fn.o != IR_KGC || fn.t.irt != IRT_FUNC ||
      fp.o != IR_FLOAD || fp.op2 != IRFL_CDATA_PTR || fp.op1 != cdata)
    return 0;
  obj = ir_kgc_load_acq(&T->ir[callable]);
  if (obj == NULL || !checkptrGC(obj) ||
      obj->gch.gct != (uint32_t)~LJ_TFUNC ||
      lj_func_ffid_acq(gco2func(obj)) != FF_ffi_meta___call ||
      (uintptr_t)(footer >> 8) !=
        (uintptr_t)mref_acq(gco2func(obj)->c.pc, const BCIns))
    return 0;
  for (i = 0; i < nargs; i++)
    if (!arm64_callxs_source_value(T, xsave, sources[i], values[i],
                                  i >= abi->nargs)) return 0;
  return 1;
}

static int arm64_callxs_ir_graph(const jit_State *J, const GCtrace *T,
				 LJArm64CallXSGraph *g)
{
  enum { CALLXS_CLOSED, CALLXS_SAVED, CALLXS_ENTERED,
	 CALLXS_READY, CALLXS_ACTIVE, CALLXS_LEFT };
  CTState *cts;
  IRRef ref, nk, nins, enter = 0, call = 0, leave = 0, xsave = 0;
  IRRef enterargs[3] = { 0, 0, 0 };
  unsigned state = CALLXS_CLOSED, calls = 0, enters = 0, leaves = 0;
  IRRef producer_call = 0, completed_producer = 0;
  uintptr_t xsave_returnpc = 0, producer_returnpc = 0;
  IRType producer_type = IRT_NIL;
  int producer_pending = 0, producer_scalar = 0;
  int ctype_seen = 0;
  IRRef boxref = 0;
  CTypeID box_ctype = 0;
  IRType call_result_type = IRT_NIL;
  int result_is_cdata = 0, box_store_seen = 0;
  MSize result_slot = 0;
  if (J == NULL || T == NULL || T->ir == NULL || g == NULL)
    return 0;
  nk = trace_nk_acq((GCtrace *)T);
  nins = trace_nins_acq((GCtrace *)T);
  if (nk >= REF_TRUE || nins <= REF_FIRST || nins >= REF_DROP ||
      bc_op(T->startins) != BC_FORL)
    return 0;
  memset(g, 0, sizeof(*g));
  cts = ctype_ctsG(J2G(J));
  if (cts == NULL || J->ktrace < nk || J->ktrace >= REF_TRUE ||
      (!arm64_callxs_kint_value(T->ir, nk, J->ktrace, NULL) &&
	 ir_load_acq(&T->ir[J->ktrace]).o != IR_KGC &&
	 ir_load_acq(&T->ir[J->ktrace]).o != IR_KNUM))
    return 0;
  if (J->curfinal == NULL) {
    IRIns ktrace = ir_load_acq(&T->ir[J->ktrace]);
    if (ktrace.o != IR_KNUM || ktrace.t.irt != IRT_P64 || ktrace.op12 != 0)
      return 0;
  } else {
    IRIns ktrace = ir_load_acq(&T->ir[J->ktrace]);
    if (ktrace.o != IR_KGC || ktrace.t.irt != IRT_P64 ||
	ktrace.op12 != 0 ||
	ir_kgc_load_acq(&T->ir[J->ktrace]) != obj2gco(J->curfinal))
      return 0;
  }
  for (ref = REF_FIRST; ref < nins; ref++) {
    IRIns ins = ir_load_acq(&T->ir[ref]);
    if ((state == CALLXS_SAVED || state == CALLXS_READY) &&
	irt_isguard(ins.t))
	return 0;
    if (state == CALLXS_ENTERED && irt_isguard(ins.t) && ins.o != IR_NE)
	return 0;
    if (state == CALLXS_ACTIVE && irt_isguard(ins.t) &&
	(ins.o != IR_CALLS ||
	 ins.op2 != IRCALL_lj_ffi_native_trace_leave))
	return 0;
    if (state == CALLXS_LEFT && ins.o != IR_EQ && ins.o != IR_NOP)
	  return 0;
    switch ((IROp)ins.o) {
    case IR_BASE:
      if (ref != REF_BASE || ins.t.irt != IRT_PGC || ins.op1 || ins.op2)
	return 0;
      break;
    case IR_NOP:
      if (ins.t.irt != IRT_NIL || ins.op1 != 0 || ins.op2 != 0)
        return 0;
      break;
    case IR_LOOP:
      if (state != CALLXS_CLOSED || g->loop ||
	  irt_type(ins.t) != IRT_NIL || !irt_isguard(ins.t))
	return 0;
      g->loop = ref;
      completed_producer = 0;
      producer_returnpc = 0;
      producer_type = IRT_NIL;
      break;
    case IR_XPOLL:
      if (state != CALLXS_CLOSED || g->poll ||
	  irt_type(ins.t) != IRT_NIL || !irt_isguard(ins.t))
	return 0;
      g->poll = ref;
      completed_producer = 0;
      producer_returnpc = 0;
      producer_type = IRT_NIL;
      break;
    case IR_XSAVE:
      if (state != CALLXS_CLOSED || irt_type(ins.t) != IRT_NIL ||
	  !arm64_callxs_xsave_anchor(T->ir, nins, ref,
	    proto_bc(J->pt), J->pt->sizebc, &xsave_returnpc, NULL))
	return 0;
      xsave = ref;
      state = CALLXS_SAVED;
      break;
    case IR_CALLS:
      if (ins.op2 == IRCALL_lj_ffi_native_trace_enter) {
	IRRef args[3];
	if (state != CALLXS_SAVED || irt_type(ins.t) != IRT_INT ||
	    irt_isguard(ins.t) ||
	    !arm64_callxs_carg_values(T->ir, nk, nins, ref, 3, args))
	  return 0;
	enter = ref;
	memcpy(enterargs, args, sizeof(args));
	enters++;
	state = CALLXS_ENTERED;
      } else if (ins.op2 == IRCALL_lj_ffi_native_trace_leave) {
	if (state != CALLXS_ACTIVE || ins.op1 != REF_NIL ||
	    irt_type(ins.t) != IRT_INT || !irt_isguard(ins.t))
	  return 0;
	if (ref <= call || xsave >= enter || enter >= call)
	  return 0;
	if ((result_is_cdata && (!boxref || !box_store_seen)) ||
	    (!result_is_cdata && (boxref || box_store_seen)))
	  return 0;
	if (result_is_cdata && result_slot != 0 &&
	    !arm64_callxs_snapshot_box_slot(T->snap, T->snapmap,
	      T->nsnap, T->nsnapmap, ref, 1u+LJ_FR2,
	      result_slot, boxref, 0)) return 0;
	leaves++;
	leave = ref;
	state = CALLXS_LEFT;
      } else if (ins.op2 == IRCALL_lj_tab_gettv_rooted) {
	IRRef args[3];
	if (state != CALLXS_CLOSED || irt_type(ins.t) != IRT_P64 ||
	    !irt_isguard(ins.t) ||
	    !arm64_callxs_carg_values(T->ir, nk, nins, ref, 3, args))
	  return 0;
	/* The table/root cell is also the output root; key is independently rooted. */
	{
	  IRIns a0 = ir_load_acq(&T->ir[args[0]]);
	  IRIns a1 = ir_load_acq(&T->ir[args[1]]);
	  if (args[2] != args[0] || a0.o != IR_TMPREF || a1.o != IR_TMPREF ||
	      a0.t.irt != IRT_PGC || a1.t.irt != IRT_PGC ||
	      a0.op2 != (IRTMPREF_IN1|IRTMPREF_OUT1) ||
	      a1.op2 != IRTMPREF_IN2 ||
	      !arm64_callxs_ref_valid(T->ir, nk, nins, ref, a0.op1) ||
	      !arm64_callxs_ref_valid(T->ir, nk, nins, ref, a1.op1))
	    return 0;
	}
      } else return 0;
      break;
    case IR_NE:
      if (state == CALLXS_ENTERED) {
	IRRef other = ins.op1 == enter ? ins.op2 : ins.op2 == enter ? ins.op1 : 0;
	if (other == 0 || ins.t.irt != (IRT_INT|IRT_GUARD) ||
	    !arm64_callxs_kint_value(T->ir, nk, other, NULL) ||
	    ir_load_acq(&T->ir[other]).i != 0)
	  return 0;
	state = CALLXS_READY;
      } else {
	IRType t1, t2;
	if (!irt_isguard(ins.t) || ins.op1 >= ref || ins.op2 >= ref ||
	    !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op1) ||
	    !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op2))
	  return 0;
	t1 = arm64_callxs_ref_type(T->ir, nk, ins.op1);
	t2 = arm64_callxs_ref_type(T->ir, nk, ins.op2);
	if (t1 != t2 || (t1 != IRT_INT && t1 != IRT_NUM && t1 != IRT_P64 &&
	    t1 != IRT_CDATA && t1 != IRT_TAB && t1 != IRT_FUNC &&
	    t1 != IRT_STR))
	  return 0;
	if (state == CALLXS_LEFT) {
	  IRRef zero = ins.op1 == leave ? ins.op2 :
	      ins.op2 == leave ? ins.op1 : 0;
	  if (zero == 0 || ins.t.irt != (IRT_INT|IRT_GUARD) ||
	      !arm64_callxs_kint_value(T->ir, nk, zero, NULL) ||
	      ir_load_acq(&T->ir[zero]).i != 0)
	    return 0;
	  state = CALLXS_CLOSED;
	  if (producer_pending && producer_scalar) {
	    completed_producer = producer_call;
	    producer_returnpc = xsave_returnpc;
	    producer_type = call_result_type;
	  } else {
	    completed_producer = 0;
	    producer_returnpc = 0;
	    producer_type = IRT_NIL;
	  }
	  producer_pending = 0;
	  boxref = 0;
	  box_ctype = 0;
	  box_store_seen = 0;
	  result_is_cdata = 0;
	}
      }
      break;
    case IR_CALLXS: {
      LJArm64ABISignature abi;
      IRRef fpref, values[LJ_ARM64_ABI_MAX_ARGS];
      unsigned i, rawcount, actual_count, tail_count;
      unsigned gpr = 0, fpr = 0, stack_size = 0;
      if (state != CALLXS_READY)
	return 0;
      if (!arm64_callxs_signature_for_call(cts, T->ir, nk, nins,
	ref, &abi, &fpref))
	return 0;

      if (enterargs[0] != J->ktrace || enterargs[1] != fpref) return 0;
      if (arm64_callxs_result_boxed(&abi) ?
	  (!boxref || enterargs[2] != boxref || box_ctype != abi.result_id) :
	  (boxref || enterargs[2] < nk || enterargs[2] >= REF_TRUE ||
	   ir_load_acq(&T->ir[enterargs[2]]).o != IR_KNULL ||
	   irt_type(ir_load_acq(&T->ir[enterargs[2]]).t) != IRT_CDATA))
	return 0;
      if (!arm64_callxs_arglist(T->ir, nk, nins, ref, values, &rawcount) ||
	  rawcount > LJ_ARM64_ABI_MAX_ARGS) return 0;
      if (rawcount > abi.nargs) {
	if (!abi.is_vararg || rawcount <= abi.nargs+1u ||
	    values[abi.nargs] != REF_NIL) return 0;
	for (i = 0; i < rawcount; i++)
	  if (i != abi.nargs && values[i] == REF_NIL) return 0;
	actual_count = rawcount-1u;
	for (i = abi.nargs; i < actual_count; i++) values[i] = values[i+1u];
      } else {
	if (rawcount != abi.nargs) return 0;
	actual_count = rawcount;
	for (i = 0; i < actual_count; i++)
	  if (values[i] == REF_NIL) return 0;
      }
      tail_count = actual_count-abi.nargs;
      {
	BCIns sourceins;
	BCOp sourceop;
	if (xsave_returnpc == 0 ||
	    !arm64_callxs_xsave_anchor(T->ir, nins, xsave,
	      proto_bc(J->pt), J->pt->sizebc, NULL, &sourceins)) return 0;
	sourceop = bc_op(sourceins);
	if (sourceop == BC_CALLM) {
	  IRRef value;
	  IRIns carrier;
	  if (actual_count == 0 || completed_producer == 0 ||
	      producer_type == IRT_NIL ||
	      !arm64_callxs_callm_pair(proto_bc(J->pt), J->pt->sizebc,
		producer_returnpc, xsave_returnpc, actual_count)) return 0;
	  value = values[actual_count-1u];
	  carrier = ir_load_acq(&T->ir[value]);
	  if (value != completed_producer &&
	      (carrier.o != IR_CONV || carrier.op1 != completed_producer ||
	       !((irt_type(carrier.t) == IRT_FLOAT &&
		  (producer_type == IRT_NUM || producer_type == IRT_INT) &&
		  carrier.op2 == ((IRT_FLOAT << IRCONV_DSH)|producer_type) &&
		  carrier.t.irt == IRT_FLOAT) ||
		 (irt_type(carrier.t) == IRT_NUM && producer_type == IRT_FLOAT &&
		  carrier.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT) &&
		  carrier.t.irt == IRT_NUM)))) return 0;
	} else if (sourceop != BC_CALL || bc_c(sourceins) == 0 ||
		   (unsigned)bc_c(sourceins)-1u != actual_count) return 0;
        if (abi.is_vararg && !arm64_callxs_vararg_sources(T, xsave, fpref,
              sourceins, xsave_returnpc, &abi, actual_count, values)) return 0;
        result_slot = 0;
        if (arm64_callxs_result_boxed(&abi)) {
          MSize base = 1u+LJ_FR2+(MSize)bc_a(sourceins)+1u+LJ_FR2;
          /* The meta-call frame holds the callable cdata before its native
          ** arguments. CNEW occupies the next slot, past both. */
          if (!arm64_callxs_snapshot_box_slot(T->snap, T->snapmap,
                T->nsnap, T->nsnapmap, xsave, base,
                base+1u+actual_count, boxref, 1)) return 0;
          /* B=1 discards the result. Otherwise lj_record_ret replaces the
          ** caller's function slot with this exact box before native leave. */
          if (bc_b(sourceins) != 1)
            result_slot = 1u+LJ_FR2+(MSize)bc_a(sourceins);
        }
      }
      for (i = 0; i < abi.nargs; i++) {
	LJArm64ABIArg *a = &abi.arg[i];
	if (!arm64_callxs_value_kind(T->ir, values[i], nk, nins, ref,
		(LJArm64ABIKind)a->kind, a->size, a->is_unsigned,
		a->reg == LJ_ARM64_ABI_STACK))
	  return 0;
        /* Match the CType plan to the IR carriers consumed by asm_gencall.
        ** In particular, a promoted INT cannot stand for a byte/halfword
        ** stack argument, and exhausting one bank cannot consume the other. */
        if (a->cls == LJ_ARM64_ABI_FPR && fpr < CCALL_NARG_FPR) {
          if (a->reg != fpr++) return 0;
        } else if (a->cls == LJ_ARM64_ABI_GPR && gpr < CCALL_NARG_GPR) {
          if (a->reg != gpr++) return 0;
        } else {
          IRIns carrier = ir_load_acq(&T->ir[values[i]]);
          unsigned width = lj_ir_type_size[irt_type(carrier.t)];
          if (width != a->size || width == 0 || width > 8) return 0;
          stack_size = (stack_size + width-1u) & ~(width-1u);
          if (a->reg != LJ_ARM64_ABI_STACK ||
              a->stack_offset != stack_size) return 0;
          stack_size += width;
        }
      }
      if (stack_size != abi.stack_size) return 0;
      if (tail_count != 0) {
	stack_size = (stack_size+7u) & ~7u;
	for (i = abi.nargs; i < actual_count; i++) {
	  IRType type = arm64_callxs_ref_type(T->ir, nk, values[i]);
	  LJArm64ABIKind kind;
	  unsigned width;
	  if (type == IRT_NUM) kind = LJ_ARM64_ABI_KIND_FLOAT, width = 8;
	  else if (type == IRT_INT) kind = LJ_ARM64_ABI_KIND_INTEGER, width = 4;
	  else if (type == IRT_P64) kind = LJ_ARM64_ABI_KIND_POINTER, width = 8;
	  else return 0;
	  if (!arm64_callxs_value_kind(T->ir, values[i], nk, nins, ref,
		kind, width, 0, 1)) return 0;
	  stack_size += 8u;
	}
      }
      if (irt_type(ins.t) != arm64_callxs_result_type(&abi))
	return 0;
      if (abi.result_kind != LJ_ARM64_ABI_KIND_VOID &&
	  !arm64_callxs_result_guard_types(T->ir, nins, nk, ref,
					   (IRType)irt_type(ins.t)))
	return 0;
      call = ref;
      result_is_cdata = arm64_callxs_result_boxed(&abi);
      call_result_type = (IRType)irt_type(ins.t);
      {
	BCIns sourceins;
	if (!arm64_callxs_xsave_anchor(T->ir, nins, xsave,
	      proto_bc(J->pt), J->pt->sizebc, NULL, &sourceins)) return 0;
	producer_call = ref;
	producer_pending = 1;
	producer_scalar = !result_is_cdata &&
	  bc_op(sourceins) == BC_CALL && bc_b(sourceins) == 0 &&
	  actual_count+1u == (unsigned)bc_c(sourceins) &&
	  (abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER ||
	   abi.result_kind == LJ_ARM64_ABI_KIND_FLOAT ||
	   abi.result_kind == LJ_ARM64_ABI_KIND_ENUM);
      }
      calls++;
      state = CALLXS_ACTIVE;
      break;
    }
    case IR_FLOAD:
      if (ins.op2 == IRFL_CDATA_PTR) {
	if (ins.op1 < REF_FIRST || ins.op1 >= ref ||
	    ir_load_acq(&T->ir[ins.op1]).o != IR_SLOAD ||
	    irt_type(ir_load_acq(&T->ir[ins.op1]).t) != IRT_CDATA ||
	    irt_type(ins.t) != IRT_P64)
	  return 0;
      } else if (ins.op2 == IRFL_CDATA_CTYPEID) {
	if (ins.op1 < REF_FIRST || ins.op1 >= ref ||
	    ir_load_acq(&T->ir[ins.op1]).o != IR_SLOAD ||
	    irt_type(ir_load_acq(&T->ir[ins.op1]).t) != IRT_CDATA)
	  return 0;
	ctype_seen = 1;
      } else if (ins.op2 == IRFL_TAB_META ||
		 ins.op2 == (GG_OFS(g.gcroot[GCROOT_BASEMT+(~LJ_TCDATA)]) >> 2)) {
	if (ins.op1 != REF_NIL || irt_type(ins.t) != IRT_TAB ||
	    !arm64_callxs_has_equal_guard(T->ir, nins, nk, ref, IRT_TAB))
	  return 0;
      } else return 0;
      break;
    case IR_VLOAD:
      if (irt_type(ins.t) != IRT_FUNC || !irt_isguard(ins.t) ||
	  ins.op2 != 0 || ins.op1 < REF_FIRST || ins.op1 >= ref ||
	  ir_load_acq(&T->ir[ins.op1]).o != IR_CALLS ||
	  ir_load_acq(&T->ir[ins.op1]).op2 != IRCALL_lj_tab_gettv_rooted ||
	  !arm64_callxs_has_equal_guard(T->ir, nins, nk, ref, IRT_FUNC))
	return 0;
      break;
    case IR_TMPREF:
      if (ins.t.irt != IRT_PGC ||
	  (ins.op2 & ~(IRTMPREF_IN1|IRTMPREF_OUT1|
		       IRTMPREF_IN2|IRTMPREF_OUT2)) != 0 ||
	  (ins.op2 & (IRTMPREF_IN1|IRTMPREF_OUT1|
		      IRTMPREF_IN2|IRTMPREF_OUT2)) == 0 ||
	  !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op1))
	return 0;
      break;
    case IR_CARG:
      if (irt_type(ins.t) != IRT_NIL || ins.op1 >= ref ||
	  ins.op2 >= ref ||
	  !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op1) ||
	  !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op2))
	return 0;
      break;
    case IR_EQ:
      if (!irt_isguard(ins.t) || ins.op1 >= ref || ins.op2 >= ref ||
	  !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op1) ||
	  !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op2) ||
	  !arm64_callxs_types_comparable(
	    arm64_callxs_ref_type(T->ir, nk, ins.op1),
	    arm64_callxs_ref_type(T->ir, nk, ins.op2)))
	return 0;
	  if (state == CALLXS_LEFT) {
	    IRRef zero = ins.op1 == leave ? ins.op2 :
		ins.op2 == leave ? ins.op1 : 0;
	    if (zero == 0 || ins.t.irt != (IRT_INT|IRT_GUARD) ||
		!arm64_callxs_kint_value(T->ir, nk, zero, NULL) ||
		ir_load_acq(&T->ir[zero]).i != 0)
	      return 0;
	    state = CALLXS_CLOSED;
	    if (producer_pending && producer_scalar) {
	      completed_producer = producer_call;
	      producer_returnpc = xsave_returnpc;
	      producer_type = call_result_type;
	    } else {
	      completed_producer = 0;
	      producer_returnpc = 0;
	      producer_type = IRT_NIL;
	    }
	    producer_pending = 0;
	    boxref = 0;
	    box_ctype = 0;
	    box_store_seen = 0;
	    result_is_cdata = 0;
	  }
      break;
    case IR_LT: case IR_GE: case IR_LE: case IR_GT:
      if (ins.t.irt != (IRT_INT|IRT_GUARD) || ins.op1 >= ref ||
          ins.op2 >= ref ||
          !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op1) ||
          !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op2) ||
          arm64_callxs_ref_type(T->ir, nk, ins.op1) !=
            arm64_callxs_ref_type(T->ir, nk, ins.op2) ||
          (arm64_callxs_ref_type(T->ir, nk, ins.op1) != IRT_INT &&
           arm64_callxs_ref_type(T->ir, nk, ins.op1) != IRT_NUM))
        return 0;
      break;
    case IR_ULE: case IR_UGE:
      /* Negated floating range checks must preserve unordered (NaN) cases. */
      if (ins.t.irt != (IRT_NUM|IRT_GUARD) || ins.op1 >= ref ||
          ins.op2 >= ref ||
          !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op1) ||
          !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op2) ||
          arm64_callxs_ref_type(T->ir, nk, ins.op1) != IRT_NUM ||
          arm64_callxs_ref_type(T->ir, nk, ins.op2) != IRT_NUM)
        return 0;
      break;
    case IR_PHI:
	if (irt_isguard(ins.t) || !arm64_callxs_phi_types_match(T->ir, nk,
	    ref, &ins) || !arm64_callxs_cdata_phi(cts, T->ir,
	    nk, nins, ref, &ins) ||
	    ins.op1 >= ref || ins.op2 >= ref ||
	  !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op1) ||
	  !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op2))
	  return 0;
      break;
    case IR_XSTORE:
      if (state != CALLXS_ACTIVE || !result_is_cdata || !boxref ||
	  box_store_seen || ins.op2 != call ||
	  ins.op1 < REF_FIRST || ins.op1 >= ref)
	return 0;
      {
	IRIns payload = ir_load_acq(&T->ir[ins.op1]);
	int64_t offset;
	if (payload.o != IR_ADD || irt_type(payload.t) != IRT_P64 ||
	    payload.op1 != boxref ||
	    !arm64_callxs_offset_value(T->ir, nk, payload.op2, &offset) ||
	    offset != (int64_t)sizeof(GCcdata) ||
	    irt_type(ins.t) != call_result_type) return 0;
	box_store_seen = 1;
      }
      break;
    case IR_SLOAD:
      /* SLOAD operands are frame-slot and load-mode immediates, not SSA refs. */
      if (ins.op1 < 1u+LJ_FR2 || ins.op1 >= J->pt->framesize ||
          (ins.op2 & ~(IRSLOAD_PARENT|IRSLOAD_FRAME|IRSLOAD_TYPECHECK|
                       IRSLOAD_CONVERT|IRSLOAD_READONLY|IRSLOAD_INHERIT|
                       IRSLOAD_KEYINDEX)) != 0 ||
          (ins.t.irt & ~(IRT_TYPE|IRT_GUARD|IRT_ISPHI)) != 0 ||
          (irt_isguard(ins.t) && !(ins.op2 & IRSLOAD_TYPECHECK)) ||
          (irt_type(ins.t) != IRT_INT && irt_type(ins.t) != IRT_NUM &&
           irt_type(ins.t) != IRT_FLOAT && irt_type(ins.t) != IRT_CDATA &&
           irt_type(ins.t) != IRT_P64 && irt_type(ins.t) != IRT_TAB &&
           irt_type(ins.t) != IRT_FUNC && irt_type(ins.t) != IRT_STR))
        return 0;
      break;
    case IR_ADD:
      {
      int valid_a = arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op1);
      int valid_b = arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op2);
      int add_shape = arm64_callxs_add_shape(T->ir, nk, irt_type(ins.t),
						ins.op1, ins.op2);
      if ((irt_type(ins.t) != IRT_INT && irt_type(ins.t) != IRT_NUM &&
	   !((irt_type(ins.t) >= IRT_LIGHTUD) &&
	     (irt_type(ins.t) <= IRT_UDATA))) ||
	  (ins.t.irt & ~(IRT_TYPE|IRT_ISPHI)) != 0 || ins.op1 >= ref ||
	  ins.op2 >= ref || !valid_a || !valid_b || !add_shape)
        return 0;
      break;
      }
    case IR_ADDOV: case IR_SUBOV: case IR_MULOV:
      /* Overflow-checked arithmetic must retain its integer guard. */
      if (!arm64_callxs_int_overflow_shape(T->ir, nk, nins, ref, &ins))
        return 0;
      break;
    case IR_SUB: case IR_MUL:
      if (irt_type(ins.t) != IRT_NUM || ins.t.irt != IRT_NUM ||
          ins.op1 >= ref || ins.op2 >= ref ||
          !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op1) ||
          !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op2) ||
          !arm64_callxs_add_shape(T->ir, nk, IRT_NUM, ins.op1, ins.op2))
        return 0;
      break;
    case IR_CONV:
      {
	IRIns src;
	IRType dst = irt_type(ins.t);
	IRType from = (IRType)(ins.op2 & IRCONV_SRCMASK);
	if ((ins.t.irt & ~(IRT_TYPE|IRT_ISPHI)) != 0 ||
	    !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op1))
	  return 0;
	src = ir_load_acq(&T->ir[ins.op1]);
	if (irt_type(src.t) != from)
	  return 0;
	if (dst == IRT_NUM && from == IRT_INT &&
	    ins.op2 == IRCONV_NUM_INT && ins.t.irt == IRT_NUM)
	  break;
	if ((dst == IRT_U8 || dst == IRT_U16) && from == IRT_INT &&
	    ins.op2 == ((dst << IRCONV_DSH)|IRT_INT) && ins.t.irt == dst)
	  break;
	if (dst == IRT_FLOAT && (from == IRT_NUM || from == IRT_INT) &&
	    ins.op2 == ((IRT_FLOAT << IRCONV_DSH)|from) &&
	    ins.t.irt == IRT_FLOAT)
	  break;
	if (dst == IRT_NUM && from == IRT_FLOAT &&
	    ins.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT) &&
	    ins.t.irt == IRT_NUM)
	  break;
	return 0;
      }
      break;
    case IR_USE:
      if (irt_type(ins.t) != IRT_NIL || ins.op2 != 0 ||
          !arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op1))
        return 0;
      break;
    /* These pure ops are consumed only through bounded SSA refs above and
    ** by ABI/result checks; all effectful/unknown operations stay closed. */
    case IR_CNEW:
      if (state != CALLXS_CLOSED || boxref != 0 ||
	  irt_type(ins.t) != IRT_CDATA || (ins.t.irt & IRT_GUARD) == 0 ||
	  ins.op2 != REF_NIL ||
	  ins.op1 < nk || ins.op1 >= REF_TRUE ||
	  !arm64_callxs_kint_value(T->ir, nk, ins.op1, NULL))
	return 0;
      boxref = ref;
      box_ctype = (CTypeID)ir_load_acq(&T->ir[ins.op1]).i;
      break;
    default:
      /* IR constants are below REF_FIRST. This is an explicit effect
      ** whitelist, so new memory/IO/control operations remain fail-closed. */
      return 0;
    }
  }
  if (state != CALLXS_CLOSED || calls == 0 || calls != enters ||
      calls != leaves || !ctype_seen || !g->loop || !g->poll ||
      g->poll <= g->loop)
    return 0;
  return 1;
}

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
	IRRef semantic_nins, int *dynamic_stepp, int allow_wrapping)
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
    if (!allow_wrapping && ins.o == IR_ADD && irt_type(ins.t) == IRT_INT &&
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
static int arm64_callxs_snapshot_frame_pc(const GCtrace *T,
	MSize ofs, MSize nent,
	uintptr_t proto_lo, uintptr_t proto_hi, uintptr_t *pcp)
{
	MSize i;
	uintptr_t found = 0;
	for (i = 0; i < nent; i++) {
		SnapEntry se = snapentry_acq(&T->snapmap[ofs+i]);
		IRRef ref = snap_ref(se);
		IRIns k;
		uintptr_t pc;
		if (!(se & SNAP_FRAME) || ref < T->nk || ref >= REF_TRUE ||
		    !arm64_callxs_const_member(T->ir, T->nk, ref))
			continue;
		k = ir_load_acq(&T->ir[ref]);
		if (k.o != IR_KNUM || k.t.irt != IRT_NUM || k.op12 != 0 ||
		    ref+1u >= REF_TRUE)
			continue;
		pc = (uintptr_t)ir_load_acq(&T->ir[ref+1u]).tv.u64;
		if (pc < proto_lo || pc >= proto_hi ||
		    ((pc-proto_lo) % sizeof(BCIns)) != 0)
			continue;
		if (found != 0 && found != pc)
			return 0;
		found = pc;
	}
	if (found == 0)
		return 0;
	*pcp = found;
	return 1;
}

static int arm64_callxs_snapshot_precedes_leave(const GCtrace *T, IRRef at,
						IRRef *leavep)
{
	IRRef ref;
	for (ref = at; ref < T->nins; ref++) {
		IRIns ins = ir_load_acq(&T->ir[ref]);
		if (ref > at && (ins.o == IR_XSAVE || ins.o == IR_CALLXS))
			return 0;
		if (ins.o == IR_CALLS &&
		    ins.op2 == IRCALL_lj_ffi_native_trace_leave) {
			*leavep = ref;
			return 1;
		}
	}
	return 0;
}

static int arm64_callxs_snapshot_call_before_leave(const GCtrace *T,
						   IRRef leave, IRRef *callp)
{
	IRRef ref = leave;
	while (ref > REF_FIRST) {
		IRIns ins = ir_load_acq(&T->ir[--ref]);
		if (ins.o == IR_CALLXS) {
			*callp = ref;
			return 1;
		}
		if (ins.o == IR_XSAVE ||
		    (ins.o == IR_CALLS &&
		     ins.op2 == IRCALL_lj_ffi_native_trace_leave))
			return 0;
	}
	return 0;
}

static int arm64_callxs_snapshot_anchor_for_call(const GCtrace *T,
	IRRef call, const GCproto *pt, uintptr_t *returnpcp)
{
	IRRef ref;
	if (call >= T->nins) return 0;
	for (ref = call; ref > REF_FIRST; ) {
		IRIns ins = ir_load_acq(&T->ir[--ref]);
		if (ins.o == IR_XSAVE)
			return arm64_callxs_xsave_anchor(T->ir, T->nins,
				ref, proto_bc(pt), pt->sizebc, returnpcp, NULL);
		if (ins.o == IR_CALLXS ||
		    (ins.o == IR_CALLS &&
		     ins.op2 == IRCALL_lj_ffi_native_trace_leave))
			return 0;
	}
	return 0;
}

static int arm64_callxs_snapshot_call_after_save(const GCtrace *T,
						 IRRef at, IRRef *callp)
{
	IRRef ref;
	int saved = 0;
	if (at >= T->nins)
		return 0;
	for (ref = at+1u; ref > REF_FIRST; ) {
		IRIns ins = ir_load_acq(&T->ir[--ref]);
		if (ins.o == IR_CALLXS)
			return 0;
		if (ins.o == IR_XSAVE) {
			saved = 1;
			break;
		}
	}
	if (!saved)
		return 0;
	for (ref = at; ref < T->nins; ref++) {
		IRIns ins = ir_load_acq(&T->ir[ref]);
		if (ins.o == IR_XSAVE && ref != at)
			return 0;
		if (ins.o == IR_CALLXS) {
			*callp = ref;
			return 1;
		}
	}
	return 0;
}

static int arm64_callxs_snapshot_allocation_call(const GCtrace *T,
						 IRRef at, IRRef end, IRRef *callp)
{
	IRRef ref, xsave;
	IRRef alloc = 0;
	if (at >= end || end > T->nins) return 0;
	/* CNEW belongs to the snapshot whose active interval contains it. Do not
	** let an earlier setup/entry snapshot claim a later allocation effect. */
	for (ref = at; ref < end; ref++) {
		IRIns ins = ir_load_acq(&T->ir[ref]);
		if (ins.o == IR_CNEW) {
			alloc = ref;
			break;
		}
	}
	if (alloc == 0) return 0;
	for (ref = alloc+1u; ref < T->nins; ref++) {
		IRIns ins = ir_load_acq(&T->ir[ref]);
		if (ins.o == IR_XSAVE) {
			xsave = ref;
			break;
		}
		if (ins.o == IR_LOOP || ins.o == IR_XPOLL ||
		    ins.o == IR_CALLXS ||
		    (ins.o == IR_CALLS &&
		     ins.op2 == IRCALL_lj_ffi_native_trace_leave))
			return 0;
	}
	if (ref == T->nins) return 0;
	for (ref = xsave+1u; ref < T->nins; ref++) {
		IRIns ins = ir_load_acq(&T->ir[ref]);
		if (ins.o == IR_CALLXS) {
			*callp = ref;
			return 1;
		}
		if (ins.o == IR_XSAVE || ins.o == IR_LOOP || ins.o == IR_XPOLL ||
		    (ins.o == IR_CALLS &&
		     ins.op2 == IRCALL_lj_ffi_native_trace_leave))
			return 0;
	}
	return 0;
}

static int arm64_callxs_replay_setup_op(BCOp op)
{
	/* The replay snapshot may precede the exact anchored CALL only through
	** side-effect-free argument setup; crossing any foreign effect could replay it. */
	switch (op) {
	case BC_MOV:
	case BC_KPRI:
	case BC_KSHORT:
	case BC_KNUM:
	case BC_KSTR:
	case BC_KCDATA:
	case BC_CGET:
		return 1;
	default:
		return 0;
	}
}

static int arm64_callxs_snapshot_bounds(const GCtrace *T,
	const GCproto *pt)
{
#define SNAP_BOUNDS_FAIL() do return 0; while (0)
  MSize s, expected = 0;
  IRRef previous = REF_FIRST;
  const BCIns *bc, *meta_pc = NULL;
  uintptr_t lo, hi;
  IRRef r;
  if (T == NULL || pt == NULL || T->snap == NULL || T->snapmap == NULL ||
      (bc = proto_bc(pt)) == NULL || T->nsnap == 0 || T->nsnapmap == 0 ||
      pt->framesize == 0 || pt->framesize > UINT8_MAX)
    SNAP_BOUNDS_FAIL();
  lo = (uintptr_t)(const void *)bc;
  if (pt->sizebc == 0 || pt->sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns))
    SNAP_BOUNDS_FAIL();
  hi = lo + (uintptr_t)pt->sizebc*sizeof(BCIns);
  for (r = T->nk; r < REF_TRUE; ) {
    IRIns k = ir_load_acq(&T->ir[r]);
    GCobj *o;
    IRRef next;
    if (!arm64_callxs_const_member(T->ir, T->nk, r)) return 0;
    next = arm64_callxs_const_next(T->ir, r);
    if (next <= r || next > REF_TRUE) return 0;
    if (k.o == IR_KGC && k.t.irt == IRT_FUNC) {
	  o = ir_kgc_load_acq(&T->ir[r]);
	  if (o != NULL && checkptrGC(o) &&
	      o->gch.gct == (uint32_t)~LJ_TFUNC &&
	      lj_func_ffid_acq(gco2func(o)) == FF_ffi_meta___call) {
	    meta_pc = mref_acq(gco2func(o)->c.pc, const BCIns);
	    if (meta_pc == NULL || ((uintptr_t)(const void *)meta_pc &
		(sizeof(BCIns)-1u)) != 0 ||
		bc_op((BCIns)la_load32_acq((const uint32_t *)meta_pc)) != BC_FUNCC)
	      return 0;
	    break;
	  }
    }
    r = next;
  }
  for (s = 0; s < T->nsnap; s++) {
    const SnapShot *snap = &T->snap[s];
    MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
    MSize next = s+1u < T->nsnap ? snap_mapofs_acq(&T->snap[s+1u]) :
	T->nsnapmap;
    MSize slots = snap_nslots_acq(snap), i, frame_slot = UINT_MAX;
    IRRef at = snap_ref_acq(snap);
    IRRef next_at = s+1u < T->nsnap ?
	snap_ref_acq(&T->snap[s+1u]) : T->nins;
    IRRef leave_ref = 0, call_ref = 0;
    uintptr_t frame_pc = 0;
    uint64_t pcbase;
    SnapEntry footer[1+LJ_FR2];
    if (at < REF_FIRST || at >= T->nins || at < previous ||
	next_at < at || next_at > T->nins ||
	ofs != expected || ofs > T->nsnapmap || next > T->nsnapmap ||
	next < ofs || nent > next-ofs || next-ofs-nent != 1u+LJ_FR2 ||
	slots < LJ_FR2+1u || slots > (MSize)pt->framesize+LJ_ARM64_ABI_MAX_ARGS+1u+LJ_FR2 ||
	snap_topslot_acq(snap) > (MSize)pt->framesize+LJ_ARM64_ABI_MAX_ARGS) {

      SNAP_BOUNDS_FAIL();
    }
    expected = next;
    previous = at;
    for (i = 0; i < nent; i++) {
      SnapEntry se = snapentry_acq(&T->snapmap[ofs+i]);
      IRRef value = snap_ref(se);
      if (snap_slot(se) >= slots ||
	  (i && snap_slot(se) <= snap_slot(
	    snapentry_acq(&T->snapmap[ofs+i-1u]))))
	SNAP_BOUNDS_FAIL();
      if (se & SNAP_FRAME)
	frame_slot = snap_slot(se);
      if (se == SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL))
	continue;
      if (value >= REF_TRUE && value <= REF_NIL) {
	IRIns primitive = ir_load_acq(&T->ir[value]);
	if ((se & 0x00ff0000u) != 0 || snap_slot(se) < 1u+LJ_FR2 ||
	    primitive.o != IR_KPRI || primitive.op12 != 0 ||
	    primitive.t.irt != (uint8_t)(REF_NIL-value))
	  SNAP_BOUNDS_FAIL();
	continue;
      }
      if (snap_slot(se) < 1u+LJ_FR2 ||
	  ((se & 0x00ff0000u) &
	   ~(SNAP_FRAME|SNAP_NORESTORE)) != 0 ||
	  !arm64_callxs_ref_valid(T->ir, T->nk, T->nins, at, value))
	SNAP_BOUNDS_FAIL();
    }
    for (i = 0; i < 1u+LJ_FR2; i++)
      footer[i] = snapentry_acq(&T->snapmap[ofs+nent+i]);
    memcpy(&pcbase, footer, sizeof(pcbase));
    if ((uintptr_t)(pcbase>>8) >= lo && (uintptr_t)(pcbase>>8) < hi) {
      if ((uint8_t)pcbase != 0) SNAP_BOUNDS_FAIL();
      /* This is the interpreter PC for the caller frame. A snapshot that
      ** immediately precedes native leave must resume exactly at the caller
      ** continuation captured by the paired meta-frame snapshot. */
      if (at < T->nins &&
	  arm64_callxs_snapshot_precedes_leave(T, at, &leave_ref)) {
	uintptr_t pc = (uintptr_t)(pcbase >> 8);
	MSize pcindex = (MSize)((pc-lo)/sizeof(BCIns));
	uintptr_t expected_return_pc = 0, source_return_pc = 0;
	MSize prior;
	if (!arm64_callxs_snapshot_call_before_leave(T, leave_ref,
		&call_ref) ||
	    !arm64_callxs_snapshot_anchor_for_call(T, call_ref, pt,
		&source_return_pc))
	  SNAP_BOUNDS_FAIL();
	/* Bind this caller continuation to the meta-frame snapshot for this
	** specific CALLXS region. A valid continuation from another BC_CALL in
	** the same prototype is not interchangeable. */
	for (prior = 0; prior < s; prior++) {
	  const SnapShot *meta_snap = &T->snap[prior];
	  MSize meta_ofs = snap_mapofs_acq(meta_snap);
	  MSize meta_nent = snap_nent_acq(meta_snap);
	  SnapEntry meta_footer[1+LJ_FR2];
	  uint64_t meta_pcbase;
	  IRRef meta_at = snap_ref_acq(meta_snap), meta_call;
	  uintptr_t candidate;
	  MSize j;
	  if (meta_ofs > T->nsnapmap || meta_nent > T->nsnapmap-meta_ofs ||
	      T->nsnapmap-meta_ofs-meta_nent < 1u+LJ_FR2)
	    SNAP_BOUNDS_FAIL();
	  for (j = 0; j < 1u+LJ_FR2; j++)
	    meta_footer[j] = snapentry_acq(
	      &T->snapmap[meta_ofs+meta_nent+j]);
	  memcpy(&meta_pcbase, meta_footer, sizeof(meta_pcbase));
	  if ((uintptr_t)(meta_pcbase>>8) != (uintptr_t)(const void *)meta_pc)
	    continue;
	  if (meta_at >= T->nins ||
	      !arm64_callxs_snapshot_call_after_save(T, meta_at, &meta_call) ||
	      meta_call != call_ref)
	    continue;
	  if (!arm64_callxs_snapshot_frame_pc(T, meta_ofs,
		meta_nent, lo, hi, &candidate))
	    SNAP_BOUNDS_FAIL();
	  if (expected_return_pc != 0 && expected_return_pc != candidate)
	    SNAP_BOUNDS_FAIL();
	  expected_return_pc = candidate;
	}
	if (expected_return_pc == 0 || pc != expected_return_pc ||
	    pc != source_return_pc || pcindex == 0 ||
	    pcindex >= pt->sizebc ||
	      (bc_op((BCIns)la_load32_acq(
		(const uint32_t *)&bc[pcindex-1u])) != BC_CALL &&
	       bc_op((BCIns)la_load32_acq(
		(const uint32_t *)&bc[pcindex-1u])) != BC_CALLM))
	  SNAP_BOUNDS_FAIL();
      }
      {
	IRRef probe_leave, alloc_call;
	if (!arm64_callxs_snapshot_precedes_leave(T, at, &probe_leave) &&
	    arm64_callxs_snapshot_allocation_call(T, at, next_at,
		&alloc_call)) {
	  uintptr_t source_return_pc;
	  uintptr_t pc = (uintptr_t)(pcbase >> 8);
	  MSize pcindex, callindex, setup;
	  if (!arm64_callxs_snapshot_anchor_for_call(T, alloc_call, pt,
		&source_return_pc) || source_return_pc < lo+sizeof(BCIns) ||
	      pc < lo || pc >= source_return_pc ||
	      ((pc-lo) % sizeof(BCIns)) != 0 ||
	      ((source_return_pc-lo) % sizeof(BCIns)) != 0) {
	    SNAP_BOUNDS_FAIL();
	  }
	  pcindex = (MSize)((pc-lo)/sizeof(BCIns));
	  callindex = (MSize)((source_return_pc-lo)/sizeof(BCIns))-1u;
	  if (callindex >= pt->sizebc || pcindex > callindex ||
	      (bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[callindex])) !=
		BC_CALL &&
	       bc_op((BCIns)la_load32_acq((const uint32_t *)&bc[callindex])) !=
		BC_CALLM)) {
	    SNAP_BOUNDS_FAIL();
	  }
	  for (setup = pcindex; setup < callindex; setup++)
	    if (!arm64_callxs_replay_setup_op(bc_op((BCIns)la_load32_acq(
		(const uint32_t *)&bc[setup])))) {
	      SNAP_BOUNDS_FAIL();
	    }
	}
      }
    } else if (meta_pc == NULL ||
	(uintptr_t)(pcbase>>8) != (uintptr_t)(const void *)meta_pc ||
	frame_slot == UINT_MAX || frame_slot < 1u ||
	(uint8_t)pcbase != frame_slot-1u) {

      SNAP_BOUNDS_FAIL();
    } else {
      /* Allocation exits precede XSAVE and are not necessarily paired with
      ** a leave snapshot. Bind their nested caller PC directly to the same
      ** source authority as snapshots between XSAVE and native entry. */
      if (!arm64_callxs_snapshot_frame_pc(T, ofs, nent, lo, hi,
		&frame_pc))
	SNAP_BOUNDS_FAIL();
      if (arm64_callxs_snapshot_allocation_call(T, at, next_at, &call_ref) ||
	  arm64_callxs_snapshot_call_after_save(T, at, &call_ref)) {
	uintptr_t source_return_pc;
	if (!arm64_callxs_snapshot_anchor_for_call(T, call_ref, pt,
		&source_return_pc) || frame_pc != source_return_pc)
	  SNAP_BOUNDS_FAIL();
      }
    }
  }
  if (expected != T->nsnapmap) SNAP_BOUNDS_FAIL();
#undef SNAP_BOUNDS_FAIL
  return 1;
}

static int arm64_ir_guard_snapshots(const GCtrace *T,
	LJArm64IRReject *reject);

static int arm64_ir_callxs_shape(const jit_State *J, const GCtrace *T,
	const GCproto *pt, LJArm64IRReject *reject)
{
  LJArm64CallXSGraph graph;
  if (!arm64_callxs_ir_graph(J, T, &graph))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL,
	REF_BASE, IR_CALLXS, 12);
  if (J->loopref != graph.loop)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
	graph.loop, IR_XSAVE, 51);
  if (!arm64_callxs_snapshot_bounds(T, pt))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
	graph.loop, IR_XSAVE, 52);
  if (!arm64_ir_guard_snapshots(T, reject))
    return 0;
  return 1;
}

static int arm64_postra_callxs_snapshot_locations(
	const LJArm64PostRAView *view, IRRef semantic_nins, MSize capacity)
{
	MSize s, i;
	IRRef reg_owner[RID_MAX];
	IRRef spill_owner[SPS_LIMIT];
	MSize n;
	for (s = 0; s < view->nsnap; s++) {
		const SnapShot *snap = &view->snap[s];
		MSize ofs = snap_mapofs_acq(snap);
		MSize nent = snap_nent_acq(snap);
		IRRef at = snap_ref_acq(snap);
		for (n = 0; n < RID_MAX; n++) reg_owner[n] = 0;
		for (n = 0; n < SPS_LIMIT; n++) spill_owner[n] = 0;
		for (i = 0; i < nent; i++) {
			SnapEntry se = snapentry_acq(&view->snapmap[ofs+i]);
			IRRef value = snap_ref(se);
			IRIns source;
			RegSP rs;
			Reg reg;
			RegSet set;
			if (se == SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL) ||
			    irref_isk(value))
				continue;
			if (value < REF_FIRST || value >= at || value >= semantic_nins)
				return 0;
			source = ir_load_acq(&view->ir[value]);
			rs = source.prev;
			if (ra_hasspill(regsp_spill(rs))) {
			MSize slot = regsp_spill(rs);
			MSize width = irt_is64(source.t) ? 2u : 1u;
			MSize end = slot + width, k;
			if (!arm64_postra_spill_slot(slot, capacity) ||
			    end > capacity ||
			    (width == 2u && ((slot-SPS_FIRST)&1u) != 0))
					return 0;
			for (k = slot; k < end; k++) {
				if (spill_owner[k] != 0 && spill_owner[k] != value)
					return 0;
				spill_owner[k] = value;
			}
			} else {
				reg = regsp_reg(rs);
				set = irt_type(source.t) == IRT_NUM ||
				      irt_type(source.t) == IRT_FLOAT ?
				      RSET_FPR : RSET_GPR;
				if (reg >= RID_MAX_FPR || !rset_test(set, reg) ||
				    (reg_owner[reg] != 0 && reg_owner[reg] != value))
					return 0;
				reg_owner[reg] = value;
			}
		}
	}
	return 1;
}

static int arm64_postra_callxs_result_unused(
	const LJArm64PostRAView *view, IRRef nins, IRRef call)
{
	IRRef r;
	MSize s, i;
	for (r = call+1u; r < nins; r++) {
		IRIns ins = ir_load_acq(&view->ir[r]);
		IRMode mode;
		if (ins.o > IR__MAX) return 0;
		mode = lj_ir_mode[ins.o];
		if ((irm_op1(mode) == IRMref && ins.op1 == call) ||
		    (irm_op2(mode) == IRMref && ins.op2 == call))
			return 0;
	}
	for (s = 0; s < view->nsnap; s++) {
		const SnapShot *snap = &view->snap[s];
		MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
		for (i = 0; i < nent; i++)
			if (snap_ref(snapentry_acq(&view->snapmap[ofs+i])) == call)
				return 0;
	}
	return 1;
}

static int arm64_postra_callxs_value_survives(const LJArm64PostRAView *view,
		IRRef value, IRRef call, MSize capacity, unsigned depth)
{
	IRIns ins;
	int fp;
	if (value < REF_FIRST) return 1;
	if (value >= call || depth > 32u) return 0;
	ins = ir_load_acq(&view->ir[value]);
	if (ins.s != SPS_NONE) {
		MSize width = irt_is64(ins.t) ? 2u : 1u;
		MSize end = (MSize)ins.s+width;
		return arm64_postra_spill_slot(ins.s, capacity) && end <= capacity &&
		       (width != 2u || (((MSize)ins.s-SPS_FIRST)&1u) == 0);
	}
	if (ra_hasreg(ins.r)) {
		fp = irt_type(ins.t) == IRT_NUM || irt_type(ins.t) == IRT_FLOAT;
		return fp ? (ins.r >= RID_D8 && ins.r <= RID_D15) :
			    (ins.r >= RID_X19 && ins.r <= RID_X28);
	}
	if (ins.o == IR_CONV)
		return arm64_postra_callxs_value_survives(view, ins.op1, call,
			capacity, depth+1u);
	if (ins.o == IR_ADD)
		return arm64_postra_callxs_value_survives(view, ins.op1, call,
			capacity, depth+1u) &&
		       arm64_postra_callxs_value_survives(view, ins.op2, call,
			capacity, depth+1u);
	return 0;
}

static int arm64_postra_callxs_live_across(const LJArm64PostRAView *view,
		IRRef semantic_nins, IRRef call, MSize capacity)
{
	IRRef value, ref;
	MSize s, i;
	for (value = REF_BASE; value < semantic_nins; value++) {
		IRIns ins = ir_load_acq(&view->ir[value]);
		int live = 0;
		if (value >= call && ins.o != IR_PHI) continue;
		if (ins.o == IR_BASE || irref_isk(value) || ins.o == IR_NOP ||
		    ins.o == IR_CARG || ins.o == IR_TMPREF || ins.o == IR_XSAVE)
			continue;
		for (ref = call+1u; ref < semantic_nins && !live; ref++) {
			IRIns use = ir_load_acq(&view->ir[ref]);
			live = use.op1 == value || use.op2 == value;
		}
		for (s = 0; s < view->nsnap && !live; s++) {
			const SnapShot *snap = &view->snap[s];
			MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
			if (snap_ref_acq(snap) < call) continue;
			for (i = 0; i < nent; i++)
				if (snap_ref(snapentry_acq(&view->snapmap[ofs+i])) == value) {
					live = 1;
					break;
				}
		}
		if (!live) continue;
		if (!arm64_postra_callxs_value_survives(view, value, call,
			capacity, 0))
			return 0;
	}
	return 1;
}

/* Independent post-RA check that each CData PHI input owns a matching
** boxed-scalar CALLXS root/store lifecycle. */
static int arm64_postra_callxs_cdata_phi(const LJArm64PostRAView *view,
		IRRef nins, IRRef ref, const IRIns *phi)
{
	IRRef boxrefs[2], i, j;
	CTypeID ctypes[2];
	if (irt_type(phi->t) != IRT_CDATA) return 1;
	if (phi->op1 < REF_FIRST || phi->op1 >= ref ||
	    phi->op2 < REF_FIRST || phi->op2 >= ref) return 0;
	boxrefs[0] = phi->op1;
	boxrefs[1] = phi->op2;
	for (i = 0; i < 2; i++) {
		IRIns box = ir_load_acq(&view->ir[boxrefs[i]]);
		IRRef store_ref, found = 0;
		int32_t ctype;
		if (box.o == IR_SLOAD && irt_type(box.t) == IRT_CDATA &&
		    irt_isguard(box.t) && box.op1 >= 1u+LJ_FR2 &&
		    (box.op2 & ~(IRSLOAD_PARENT|IRSLOAD_FRAME|IRSLOAD_TYPECHECK|
		      IRSLOAD_CONVERT|IRSLOAD_READONLY|IRSLOAD_INHERIT|IRSLOAD_KEYINDEX)) == 0) {
			ctypes[i] = 0;
			continue;
		}
		if (box.o != IR_CNEW || irt_type(box.t) != IRT_CDATA ||
		    !irt_isguard(box.t) || box.op2 != REF_NIL ||
		    box.op1 < view->nk ||
		    !arm64_callxs_kint_value(view->ir, view->nk, box.op1, &ctype) ||
		    ctype <= 0)
			return 0;
		ctypes[i] = (CTypeID)ctype;
		for (store_ref = REF_FIRST; store_ref < nins; store_ref++) {
			IRIns store = ir_load_acq(&view->ir[store_ref]);
			IRIns payload;
			IRRef callref;
			int64_t offset;
			LJArm64ABISignature abi;
			if (store.o != IR_XSTORE || store.op1 < REF_FIRST ||
			    store.op1 >= store_ref || store.op2 < REF_FIRST ||
			    store.op2 >= store_ref)
				continue;
			payload = ir_load_acq(&view->ir[store.op1]);
			if (payload.o != IR_ADD || irt_type(payload.t) != IRT_P64 ||
			    payload.op1 != boxrefs[i] ||
			    !arm64_callxs_offset_value(view->ir, view->nk,
			      payload.op2, &offset) ||
			    offset != (int64_t)sizeof(GCcdata))
				continue;
			callref = store.op2;
			if (!arm64_callxs_signature_for_call(view->cts, view->ir,
			      view->nk, nins, callref, &abi, NULL) ||
			    !arm64_callxs_result_boxed(&abi) ||
			    abi.result_id != ctypes[i] ||
			    irt_type(store.t) != arm64_callxs_result_type(&abi) ||
			    irt_type(ir_load_acq(&view->ir[callref]).t) !=
			      arm64_callxs_result_type(&abi) ||
			    found != 0)
				return 0;
			found = callref;
		}
		if (found == 0) return 0;
	}
	for (j = 0; j < 2; j++)
		if (ctypes[0] != 0 && ctypes[1] != 0 && ctypes[0] != ctypes[1])
			return 0;
	return 1;
}

static int arm64_callxs_phi_types_match(const IRIns *ir, IRRef nk,
		IRRef ref, const IRIns *phi)
{
	IRType type = irt_type(phi->t);
	IRType type1, type2;
	if (irt_isguard(phi->t) || phi->op1 >= ref || phi->op2 >= ref ||
	    !arm64_callxs_ref_valid(ir, nk, ref, ref, phi->op1) ||
	    !arm64_callxs_ref_valid(ir, nk, ref, ref, phi->op2))
		return 0;
	type1 = arm64_callxs_ref_type(ir, nk, phi->op1);
	type2 = arm64_callxs_ref_type(ir, nk, phi->op2);
	/* A PHI is a representation-preserving merge. CData has its stronger
	** allocation/root proof above; all other values must preserve the IR type. */
	return type1 == type && type2 == type;
}

/* Independent allocator-side CALLXS proof. Unlike the semantic walk above,
** this validates concrete ABI result registers, spill bounds and snapshot
** locations from the published IR and CType snapshot. */
static int arm64_postra_callxs_rename_valid(
	const LJArm64PostRAView *view, const IRIns *rename,
	IRRef semantic_nins, MSize capacity)
{
	IRIns source;
	int fp;
	const SnapShot *snap;
	MSize i, ofs, nent;
	IRRef ref;
	if (rename->op1 < REF_FIRST || rename->op1 >= semantic_nins ||
	    rename->r >= RID_MAX || !ra_hasreg(rename->r) ||
	    rename->op2 >= view->nsnap)
		return 0;
	source = ir_load_acq(&view->ir[rename->op1]);
	if (irt_type(source.t) == IRT_NIL || source.o == IR_RENAME)
		return 0;
	fp = irt_type(source.t) == IRT_NUM || irt_type(source.t) == IRT_FLOAT;
	if (!rset_test(fp ? RSET_FPR : RSET_GPR, rename->r)) return 0;
	for (ref = REF_BASE; ref < view->nins; ref++) {
		IRIns sibling = ir_load_acq(&view->ir[ref]);
		if (sibling.o != IR_RENAME || sibling.op2 != rename->op2)
			continue;
		if (sibling.op1 < REF_FIRST || sibling.op1 >= semantic_nins ||
		    sibling.r >= RID_MAX || !ra_hasreg(sibling.r) ||
		    (sibling.r == rename->r && sibling.op1 != rename->op1))
			return 0;
	}
	snap = &view->snap[rename->op2];
	ofs = snap_mapofs_acq(snap);
	nent = snap_nent_acq(snap);
	if (ofs > view->nsnapmap || nent > view->nsnapmap-ofs)
		return 0;
	for (i = 0; i < nent; i++) {
		IRRef value = snap_ref(snapentry_acq(&view->snapmap[ofs+i]));
		IRIns live;
		RegSP rs;
		if (value == rename->op1 || irref_isk(value)) continue;
		if (value < REF_FIRST || value >= view->nins) return 0;
		live = ir_load_acq(&view->ir[value]);
		rs = live.prev;
		if (ra_hasspill(regsp_spill(rs))) {
			MSize slot = regsp_spill(rs);
			MSize width = irt_is64(live.t) ? 2u : 1u;
			if (!arm64_postra_spill_slot(slot, capacity) ||
			    (MSize)slot+width > capacity ||
			    (width == 2u && ((slot-SPS_FIRST)&1u) != 0))
				return 0;
		} else if (regsp_reg(rs) == rename->r) {
			return 0;
		}
	}
	return 1;
}

/* Independently bind allocated variadic operands to the saved Lua frame. */
static int arm64_postra_snapshot_find(const GCtrace *T, IRRef xsave,
		MSize *indexp, MSize *ofsp, MSize *nentp, MSize *nextp)
{
	MSize s, found = (MSize)-1, ofs, nent, next, slots;
	for (s = 0; s < T->nsnap; s++) {
		if (snap_ref_acq(&T->snap[s]) != xsave) continue;
		if (found != (MSize)-1) return 0;
		found = s;
	}
	if (found == (MSize)-1) return 0;
	{
		const SnapShot *snap = &T->snap[found];
		if (snap_count_acq(snap) != SNAPCOUNT_DONE) return 0;
		ofs = snap_mapofs_acq(snap);
		nent = snap_nent_acq(snap);
		next = found+1u < T->nsnap ?
		  snap_mapofs_acq(&T->snap[found+1u]) : T->nsnapmap;
		slots = snap_nslots_acq(snap);
	}
	if (ofs > T->nsnapmap || next > T->nsnapmap || next < ofs ||
	    nent > next-ofs || next-ofs-nent != 1u+LJ_FR2 ||
	    slots < 1u+LJ_FR2) return 0;
	for (s = 0; s < nent; s++) {
		SnapEntry se = snapentry_acq(&T->snapmap[ofs+s]);
		IRRef r = snap_ref(se);
		if (snap_slot(se) >= slots || (s != 0 &&
		    snap_slot(se) <= snap_slot(snapentry_acq(&T->snapmap[ofs+s-1u]))))
			return 0;
		if (r >= REF_DROP ||
		    !arm64_callxs_ref_valid(T->ir, T->nk, T->nins, xsave, r))
			return 0;
	}
	*indexp = found;
	*ofsp = ofs;
	*nentp = nent;
	*nextp = next;
	return 1;
}

static int arm64_postra_snapshot_slot_ref(const GCtrace *T, IRRef xsave,
						  unsigned slot, IRRef *refp)
{
	MSize index, ofs, nent, next, i;
	if (!arm64_postra_snapshot_find(T, xsave, &index, &ofs, &nent, &next) ||
	    slot >= snap_nslots_acq(&T->snap[index])) return 0;
	UNUSED(next);
	for (i = 0; i < nent; i++) {
		SnapEntry se = snapentry_acq(&T->snapmap[ofs+i]);
		if (snap_slot(se) == slot) {
			IRRef r = snap_ref(se);
			/* Source slots must be ordinary values with no frame,
			** continuation, restore, soft-FP, or traversal marker flags. */
			if ((se & 0x00ff0000u) != 0 || r >= REF_DROP ||
			    !arm64_callxs_ref_valid(T->ir, T->nk, T->nins, xsave, r))
				return 0;
			*refp = r;
			return 1;
		}
	}
	return 0;
}

static int arm64_postra_snapshot_frame_raw(const GCtrace *T, IRRef xsave,
						  unsigned slot, uint64_t *rawp)
{
	MSize index, ofs, nent, next, i;
	if (!arm64_postra_snapshot_find(T, xsave, &index, &ofs, &nent, &next))
		return 0;
	UNUSED(index); UNUSED(next);
	for (i = 0; i < nent; i++) {
		SnapEntry se = snapentry_acq(&T->snapmap[ofs+i]);
		IRRef r = snap_ref(se);
		IRIns k;
		if (snap_slot(se) != slot ||
		    (se & 0x00ff0000u) != SNAP_FRAME || r < T->nk || r >= REF_TRUE ||
		    !arm64_callxs_const_member(T->ir, T->nk, r) ||
		    r+1u >= REF_TRUE) continue;
		k = ir_load_acq(&T->ir[r]);
		if (k.o != IR_KNUM || k.t.irt != IRT_NUM || k.op12 != 0)
			return 0;
		*rawp = ir_load_acq(&T->ir[r+1u]).tv.u64;
		return 1;
	}
	return 0;
}

/* Match the numeric bits produced by the interpreter's double conversion. */
static int arm64_postra_num_constant_is_int(const IRIns *ir, IRRef nk,
						    IRRef numref, int32_t i)
{
	IRIns k;
	TValue tv;
	if (!arm64_callxs_const_member(ir, nk, numref)) return 0;
	k = ir_load_acq(&ir[numref]);
	if (k.o != IR_KNUM || k.t.irt != IRT_NUM || k.op12 != 0 ||
	    numref+1u >= REF_TRUE) return 0;
	setnumV(&tv, (lua_Number)i);
	return ir_load_acq(&ir[numref+1u]).tv.u64 == tv.u64;
}

static int arm64_postra_source_value_matches(const IRIns *ir, IRRef nk,
		IRRef nins, IRRef call, IRRef source, IRRef carrier,
		IRType source_type)
{
	IRIns c;
	if (!arm64_callxs_ref_valid(ir, nk, nins, call, source) ||
	    !arm64_callxs_ref_valid(ir, nk, nins, call, carrier)) return 0;
	if (carrier == source) return 1;
	if (carrier == REF_TRUE || carrier == REF_FALSE || carrier == REF_NIL)
		return 0;
	c = ir_load_acq(&ir[carrier]);
	if (source_type == IRT_INT && irt_type(c.t) == IRT_INT &&
	    !irt_isguard(c.t)) {
		IRIns original = ir_load_acq(&ir[source]);
		if (irt_isguard(original.t) && original.op1 == c.op1 &&
		    original.op2 == c.op2 &&
		    ((original.o == IR_ADDOV && c.o == IR_ADD) ||
		     (original.o == IR_SUBOV && c.o == IR_SUB) ||
		     (original.o == IR_MULOV && c.o == IR_MUL))) return 1;
	}
	/* Source-linked conversions are separately checked against the exact
	** conversion table by the enclosing graph validators and ABI classifier. */
	if (c.o == IR_CONV && c.op1 == source) return 1;
	if (source_type == IRT_INT && arm64_callxs_const_member(ir, nk, source) &&
	    ir_load_acq(&ir[source]).o == IR_KINT &&
	    ir_load_acq(&ir[source]).t.irt == IRT_INT &&
	    arm64_postra_num_constant_is_int(ir, nk, carrier,
	      ir_load_acq(&ir[source]).i)) return 1;
	if (source_type == IRT_TRUE || source_type == IRT_FALSE) {
		int32_t expected = source_type == IRT_TRUE ? 1 : 0;
		return arm64_callxs_kint_value(ir, nk, carrier, NULL) &&
		       ir_load_acq(&ir[carrier]).i == expected;
	}
	if (source_type == IRT_STR) {
		int64_t offset;
		if (c.o == IR_ADD && c.t.irt == IRT_P64 &&
		    ((c.op1 == source && arm64_callxs_offset_value(ir, nk,
			c.op2, &offset) && offset == (int64_t)sizeof(GCstr)) ||
	     (c.op2 == source && arm64_callxs_offset_value(ir, nk,
			c.op1, &offset) && offset == (int64_t)sizeof(GCstr)))) return 1;
		if (arm64_callxs_const_member(ir, nk, source) &&
		    ir_load_acq(&ir[source]).o == IR_KGC &&
		    ir_load_acq(&ir[source]).t.irt == IRT_STR &&
		    arm64_callxs_const_member(ir, nk, carrier)) {
			GCobj *str = ir_kgc_load_acq(&ir[source]);
			IRIns k = ir_load_acq(&ir[carrier]);
			if (str != NULL && k.t.irt == IRT_P64 &&
			    (k.o == IR_KPTR || k.o == IR_KKPTR) &&
			    (uintptr_t)(const void *)str <= UINTPTR_MAX-sizeof(GCstr) &&
			    (uintptr_t)ir_kptr(&ir[carrier]) ==
			      (uintptr_t)(const void *)str + sizeof(GCstr)) return 1;
		}
	}
	if (source_type == IRT_NIL && carrier < REF_TRUE &&
	    arm64_callxs_const_member(ir, nk, carrier)) {
		IRIns k = ir_load_acq(&ir[carrier]);
		return (k.o == IR_KPTR || k.o == IR_KKPTR) &&
		       k.t.irt == IRT_P64 && ir_kptr(&ir[carrier]) == NULL;
	}
	return 0;
}

static int arm64_postra_vararg_promoted(const IRIns *ir, IRRef nk,
						IRRef nins, IRRef call,
						IRRef source, IRRef carrier,
						IRType source_type)
{
	if (!arm64_callxs_ref_valid(ir, nk, nins, call, source) ||
	    !arm64_callxs_ref_valid(ir, nk, nins, call, carrier)) return 0;
	if (source_type == IRT_INT) {
		IRIns c;
		if (arm64_callxs_ref_type(ir, nk, carrier) != IRT_NUM) return 0;
		c = ir_load_acq(&ir[carrier]);
		if (c.o == IR_CONV && irt_type(c.t) == IRT_NUM &&
		    c.t.irt == IRT_NUM && c.op1 == source &&
		    c.op2 == IRCONV_NUM_INT) return 1;
		return arm64_callxs_const_member(ir, nk, source) &&
		       ir_load_acq(&ir[source]).o == IR_KINT &&
		       ir_load_acq(&ir[source]).t.irt == IRT_INT &&
		       arm64_postra_num_constant_is_int(ir, nk, carrier,
			 ir_load_acq(&ir[source]).i);
	}
	if (source_type == IRT_NUM) return carrier == source;
	if (source_type == IRT_TRUE || source_type == IRT_FALSE)
		return arm64_callxs_ref_type(ir, nk, carrier) == IRT_INT &&
		       arm64_postra_source_value_matches(ir, nk, nins, call,
			 source, carrier, source_type);
	if (source_type == IRT_STR)
		return arm64_callxs_ref_type(ir, nk, carrier) == IRT_P64 &&
		       arm64_postra_source_value_matches(ir, nk, nins, call,
			 source, carrier, source_type);
	if (source_type == IRT_NIL)
		return arm64_callxs_ref_type(ir, nk, carrier) == IRT_P64 &&
		       arm64_postra_source_value_matches(ir, nk, nins, call,
			 source, carrier, source_type);
	return 0;
}

/*
** Check the real interpreter XSAVE before the CALLXS. `values` is the
** normalized physical CARG list (the NIL vararg separator has already been
** removed by the caller). Fixed arguments use a bounded set of source-linked
** normalizations; each suffix carrier is tied to its exact stack slot.
*/
static int arm64_postra_vararg_source_proof(const GCtrace *T,
		IRRef xsave, IRRef call, const BCIns *proto_bc, MSize proto_sizebc,
		uintptr_t returnpc, BCIns sourceins,
		IRRef callable_cdata_ref, unsigned fixed_count,
		unsigned actual_count, const IRRef *values)
{
	uint64_t frame_pc, footer_pcbase;
	uintptr_t anchored_pc;
	BCIns anchored_ins;
	SnapEntry footer[1+LJ_FR2];
	MSize index, ofs, nent, next, i;
	unsigned argbase, metaslot, frameslot, cdataslot, a;
	IRRef meta_fn, cdata;
	GCobj *fn;
	const BCIns *meta_pc;
	if (T == NULL || T->ir == NULL || T->snap == NULL ||
	    T->snapmap == NULL || T->nsnapmap == 0 || proto_bc == NULL ||
	    proto_sizebc == 0 || values == NULL ||
	    xsave >= call || call >= T->nins ||
	    (bc_op(sourceins) != BC_CALL && bc_op(sourceins) != BC_CALLM) ||
	    actual_count == 0 || actual_count < fixed_count ||
	    actual_count > LJ_ARM64_ABI_MAX_ARGS ||
	    !arm64_postra_snapshot_find(T, xsave, &index, &ofs, &nent, &next))
		return 0;
	if (!arm64_callxs_xsave_anchor(T->ir, T->nins, xsave,
	    proto_bc, proto_sizebc, &anchored_pc, &anchored_ins) ||
	    anchored_pc != returnpc || anchored_ins != sourceins) return 0;
	UNUSED(next);
	metaslot = (unsigned)bc_a(sourceins)+1u+LJ_FR2;
	frameslot = metaslot+LJ_FR2;
	cdataslot = frameslot+1u;
	argbase = cdataslot+1u;
	if (argbase+actual_count > UINT8_MAX ||
	    snap_nslots_acq(&T->snap[index]) < argbase+actual_count ||
	    snap_topslot_acq(&T->snap[index]) < argbase+actual_count-1u) return 0;
	for (i = 0; i < 1u+LJ_FR2; i++)
		footer[i] = snapentry_acq(&T->snapmap[ofs+nent+i]);
	memcpy(&footer_pcbase, footer, sizeof(footer_pcbase));
	if ((uint8_t)footer_pcbase != (uint8_t)metaslot) return 0;
	if (!arm64_postra_snapshot_frame_raw(T, xsave, frameslot, &frame_pc) ||
	    frame_pc != (uint64_t)returnpc) return 0;
	/* Slot A+2 is the active FFI __call function and the footer's high part
	** is its C-frame PC. */
	if (!arm64_postra_snapshot_slot_ref(T, xsave,
	    metaslot, &meta_fn) ||
	    meta_fn < T->nk || meta_fn >= REF_TRUE ||
	    !arm64_callxs_const_member(T->ir, T->nk, meta_fn) ||
	    ir_load_acq(&T->ir[meta_fn]).o != IR_KGC ||
	    ir_load_acq(&T->ir[meta_fn]).t.irt != IRT_FUNC) return 0;
	fn = ir_kgc_load_acq(&T->ir[meta_fn]);
	if (fn == NULL || !checkptrGC(fn) ||
	    fn->gch.gct != (uint32_t)~LJ_TFUNC ||
	    lj_func_ffid_acq(gco2func(fn)) != FF_ffi_meta___call) return 0;
	meta_pc = mref_acq(gco2func(fn)->c.pc, const BCIns);
	if (meta_pc == NULL || (uintptr_t)(footer_pcbase >> 8) !=
	    (uintptr_t)(const void *)meta_pc) return 0;
	if (callable_cdata_ref < REF_FIRST || callable_cdata_ref >= xsave ||
	    ir_load_acq(&T->ir[callable_cdata_ref]).o != IR_SLOAD ||
	    !arm64_postra_snapshot_slot_ref(T, xsave,
	      cdataslot, &cdata) ||
	    cdata != callable_cdata_ref) return 0;
	for (a = 0; a < actual_count; a++) {
		IRRef source;
		IRType stype;
		if (values[a] >= xsave ||
		    !arm64_postra_snapshot_slot_ref(T, xsave, argbase+a, &source) ||
		    !arm64_callxs_ref_valid(T->ir, T->nk, T->nins, xsave, source) ||
		    !arm64_callxs_ref_valid(T->ir, T->nk, T->nins, xsave, values[a]))
			return 0;
		stype = arm64_callxs_ref_type(T->ir, T->nk, source);
		if (a < fixed_count) {
			if (!arm64_postra_source_value_matches(T->ir, T->nk,
			    T->nins, xsave, source, values[a], stype)) return 0;
		} else if (!arm64_postra_vararg_promoted(T->ir, T->nk,
		    T->nins, xsave, source, values[a], stype)) return 0;
	}
	return 1;
}

static int arm64_postra_callxs_generic(const LJArm64PostRAView *view,
	IRRef *semantic_ninsp)
{
	IRRef semantic_nins, ref, active_call = 0, active_enter = 0;
	IRRef active_leave = 0;
	IRRef active_xsave = 0, producer_call = 0, completed_producer = 0;
	IRRef enterargs[3] = { 0, 0, 0 };
	IRRef lookupargs[3] = { 0, 0, 0 };
	IRRef boxref = 0;
	CTypeID box_ctype = 0;
	int box_store_seen = 0, result_is_cdata = 0;
	MSize result_slot = 0;
	uintptr_t xsave_returnpc = 0, producer_returnpc = 0;
	IRType producer_type = IRT_NIL;
	int producer_pending = 0, producer_scalar = 0;
	unsigned protocol = 0, nc = 0, nx = 0, nloop = 0, npoll = 0;
	MSize capacity, highest = 0, outgoing = 0;
	IRRef layout_xsave = 0;
	uintptr_t layout_producer_returnpc = 0;
	GCtrace temp;
	GCproto *pt;
	uintptr_t pclo, pchi, startpc;
	BCIns liveins;
	if (view == NULL || view->owner == NULL || view->ir == NULL ||
	    view->cts == NULL || view->snap == NULL || view->snapmap == NULL ||
	    view->proto_bc == NULL || view->proto == NULL || view->startpc == NULL ||
	    bc_op(view->startins) != BC_FORL ||
	    view->nins <= REF_FIRST || view->nins >= REF_DROP ||
	    view->nk == 0 || view->nk >= REF_TRUE || view->nk >= view->nins ||
	    view->base_delta != 0 || (view->spadjust & 15u) != 0 ||
	    view->spadjust > (MSize)sps_scale(SPS_LIMIT-SPS_FIXED))
	  return 0;
	capacity = SPS_FIXED + view->spadjust / sizeof(int32_t);
	if (capacity > SPS_LIMIT) return 0;
	semantic_nins = view->nins;
	while (semantic_nins > REF_FIRST) {
	  IRIns tail = ir_load_acq(&view->ir[semantic_nins-1u]);
	  if (tail.o == IR_NOP && tail.t.irt == IRT_NIL &&
	      tail.op1 == 0 && tail.op2 == 0 && tail.prev == 0) {
	    semantic_nins--;
	    break;
	  }
	  if (tail.o == IR_RENAME && tail.t.irt == IRT_NIL &&
	      tail.op1 >= REF_FIRST && tail.op1 < semantic_nins-1u &&
	      tail.op2 < view->nsnap && tail.s == SPS_NONE) {
	    semantic_nins--;
	    continue;
	  }
	  break;
	}
	for (ref = semantic_nins; ref < view->nins; ref++) {
	  IRIns rename = ir_load_acq(&view->ir[ref]);
	  /* The assembler reserves one zero NOP before its RENAME suffix. */
	  if (ref == semantic_nins && rename.o == IR_NOP &&
	      rename.t.irt == IRT_NIL && rename.op1 == 0 &&
	      rename.op2 == 0 && rename.prev == 0)
	    continue;
	  if (rename.o != IR_RENAME || rename.op1 < REF_FIRST ||
	      rename.op1 >= semantic_nins || rename.op2 >= view->nsnap ||
	      rename.t.irt != IRT_NIL || rename.s != SPS_NONE)
	    return 0;
	  if (!arm64_postra_callxs_rename_valid(view, &rename,
		semantic_nins, capacity))
	    return 0;
	}
	if (semantic_nins <= REF_FIRST || (!view->owner_private &&
	    (view->owner->root != 0 || trace_traceno_acq((GCtrace *)view->owner) == 0 ||
	     trace_startins_acq((GCtrace *)view->owner) != view->startins)))
	  return 0;
	pt = (GCproto *)view->proto;
	if (proto_bc(pt) != view->proto_bc || pt->sizebc != view->proto_sizebc ||
	    pt->numparams != view->proto_numparams)
	  return 0;
	pclo = (uintptr_t)(const void *)view->proto_bc;
	pchi = pclo + (uintptr_t)view->proto_sizebc*sizeof(BCIns);
	startpc = (uintptr_t)(const void *)view->startpc;
	if (pchi <= pclo || startpc < pclo || startpc >= pchi ||
	    ((startpc-pclo) % sizeof(BCIns)) != 0)
	  return 0;
	liveins = (BCIns)la_load32_acq((const uint32_t *)startpc);
	if (view->owner_private) {
	  /* During assembly the source FORL has not yet been patched. Accept only
	  ** the exact captured source instruction, with the owner still private. */
	  if (liveins != view->startins)
	    return 0;
	} else if (bc_op(liveins) != BC_JFORL ||
		   bc_a(liveins) != bc_a(view->startins) ||
		   bc_d(liveins) != trace_traceno_acq((GCtrace *)view->owner) ||
		   !trace_runnable_acq((GCtrace *)view->owner,
		     trace_traceno_acq((GCtrace *)view->owner)) ||
		   trace_link_acq((GCtrace *)view->owner) !=
		     trace_traceno_acq((GCtrace *)view->owner) ||
		   trace_linktype_acq((GCtrace *)view->owner) != LJ_TRLINK_LOOP ||
		   trace_startptgco_acq((GCtrace *)view->owner) != obj2gco(pt) ||
		   trace_startpc_acq((GCtrace *)view->owner) != view->startpc) {
	  return 0;
	}
	temp = *view->owner;
	temp.ir = (IRIns *)view->ir;
	temp.nins = semantic_nins;
	temp.nk = view->nk;
	temp.snap = (SnapShot *)view->snap;
	temp.snapmap = (SnapEntry *)view->snapmap;
	temp.nsnap = view->nsnap;
	temp.nsnapmap = view->nsnapmap;
	if (!arm64_callxs_snapshot_bounds(&temp, pt))
	  return 0;
	for (ref = REF_BASE; ref < semantic_nins; ref++) {
	  IRIns ins = ir_load_acq(&view->ir[ref]);
	  if ((protocol == 1 || protocol == 3) && irt_isguard(ins.t))
	    return 0;
	  if (protocol == 2 && irt_isguard(ins.t) && ins.o != IR_NE)
	    return 0;
	  if (protocol == 4 && irt_isguard(ins.t) &&
	      (ins.o != IR_CALLS ||
	       ins.op2 != IRCALL_lj_ffi_native_trace_leave))
	    return 0;
	  if (protocol == 5 && ins.o != IR_EQ && ins.o != IR_NOP)
	    return 0;
	  if (ins.o == IR_XSAVE) {
	    if (protocol != 0 || ins.t.irt != IRT_NIL ||
		!arm64_callxs_xsave_anchor(view->ir, semantic_nins,
		  ref, view->proto_bc, view->proto_sizebc,
		  &xsave_returnpc, NULL))
	      return 0;
	    active_xsave = ref;
	    protocol = 1;
	    nx++;
	  } else if (ins.o == IR_CALLS &&
		     ins.op2 == IRCALL_lj_ffi_native_trace_enter) {
	    if (protocol != 1 || irt_type(ins.t) != IRT_INT || irt_isguard(ins.t) ||
		!arm64_callxs_carg_values(view->ir, view->nk, semantic_nins,
			ref, 3, enterargs))
	      return 0;
	    if (!arm64_callxs_const_member(view->ir, view->nk, enterargs[0]) ||
		arm64_callxs_ref_type(view->ir, view->nk, enterargs[0]) != IRT_P64 ||
		enterargs[1] < REF_FIRST || enterargs[1] >= ref ||
		ir_load_acq(&view->ir[enterargs[1]]).o != IR_FLOAD ||
		ir_load_acq(&view->ir[enterargs[1]]).op2 != IRFL_CDATA_PTR ||
		(enterargs[2] >= view->nk &&
		 ir_load_acq(&view->ir[enterargs[2]]).o != IR_KNULL &&
		 ir_load_acq(&view->ir[enterargs[2]]).o != IR_CNEW))
	      return 0;
	    active_enter = ref;
	    protocol = 2;
	  } else if (ins.o == IR_NE && protocol == 2) {
	    IRRef zero = ins.op1 == active_enter ? ins.op2 :
		ins.op2 == active_enter ? ins.op1 : 0;
	    if (ins.t.irt != (IRT_INT|IRT_GUARD) || zero == 0 ||
		!arm64_callxs_kint_value(view->ir, view->nk, zero, NULL) ||
		ir_load_acq(&view->ir[zero]).i != 0)
	      return 0;
	    protocol = 3;
	  } else if (ins.o == IR_CALLXS) {
	    LJArm64ABISignature abi;
	    IRRef fpref, values[LJ_ARM64_ABI_MAX_ARGS];
	    unsigned rawcount, actual_count, i;
	    BCIns sourceins;
	    if (protocol != 3 || active_call != 0)
	      return 0;
	    if (!arm64_callxs_signature_for_call(view->cts, view->ir, view->nk,
		semantic_nins, ref, &abi, &fpref))
	      return 0;
	    if (active_xsave == 0 || xsave_returnpc == 0 ||
		!arm64_callxs_xsave_anchor(view->ir, semantic_nins, active_xsave,
		  view->proto_bc, view->proto_sizebc, NULL, &sourceins))
	      return 0;
	    if (!arm64_callxs_arglist(view->ir, view->nk, semantic_nins,
		ref, values, &rawcount) || rawcount > LJ_ARM64_ABI_MAX_ARGS)
	      return 0;
	    if (rawcount > abi.nargs) {
	      if (!abi.is_vararg || rawcount <= abi.nargs+1u ||
		  values[abi.nargs] != REF_NIL) return 0;
	      for (i = 0; i < rawcount; i++)
		if (i != abi.nargs && values[i] == REF_NIL) return 0;
	      actual_count = rawcount-1u;
	      for (i = abi.nargs; i < actual_count; i++)
		values[i] = values[i+1u];
	    } else {
	      if (rawcount != abi.nargs) return 0;
	      actual_count = rawcount;
	      for (i = 0; i < actual_count; i++)
		if (values[i] == REF_NIL) return 0;
	    }
	    if (bc_op(sourceins) == BC_CALLM) {
	      IRIns carrier;
	      IRRef value;
	      if (actual_count == 0 || completed_producer == 0 ||
		  producer_type == IRT_NIL ||
		  !arm64_callxs_callm_pair(view->proto_bc, view->proto_sizebc,
		    producer_returnpc, xsave_returnpc, actual_count)) return 0;
	      value = values[actual_count-1u];
	      carrier = ir_load_acq(&view->ir[value]);
	      if (value != completed_producer &&
		  (carrier.o != IR_CONV || carrier.op1 != completed_producer ||
		   !((irt_type(carrier.t) == IRT_FLOAT &&
		      (producer_type == IRT_NUM || producer_type == IRT_INT) &&
		      carrier.op2 == ((IRT_FLOAT << IRCONV_DSH)|producer_type) &&
		      carrier.t.irt == IRT_FLOAT) ||
		     (irt_type(carrier.t) == IRT_NUM && producer_type == IRT_FLOAT &&
		      carrier.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT) &&
		      carrier.t.irt == IRT_NUM)))) return 0;
	    } else if (bc_op(sourceins) != BC_CALL || bc_c(sourceins) == 0 ||
		       (unsigned)bc_c(sourceins)-1u != actual_count) return 0;
	    producer_call = ref;
	    producer_pending = 1;
	    producer_scalar = !arm64_callxs_result_boxed(&abi) &&
	      bc_op(sourceins) == BC_CALL && bc_b(sourceins) == 0 &&
	      actual_count+1u == (unsigned)bc_c(sourceins) &&
	      (abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER ||
	       abi.result_kind == LJ_ARM64_ABI_KIND_FLOAT ||
	       abi.result_kind == LJ_ARM64_ABI_KIND_ENUM);
	    producer_type = (IRType)irt_type(ir_load_acq(&view->ir[ref]).t);
	    if (fpref != enterargs[1])
	      return 0;
	    if (arm64_callxs_result_boxed(&abi) ?
		(boxref == 0 || enterargs[2] != boxref || box_ctype != abi.result_id) :
		(boxref != 0 || !arm64_callxs_const_member(view->ir, view->nk, enterargs[2]) ||
		 ir_load_acq(&view->ir[enterargs[2]]).o != IR_KNULL ||
		 arm64_callxs_ref_type(view->ir, view->nk, enterargs[2]) != IRT_CDATA)) return 0;
	    result_is_cdata = arm64_callxs_result_boxed(&abi);
	    result_slot = 0;
	    if (result_is_cdata) {
	      MSize base = 1u+LJ_FR2+(MSize)bc_a(sourceins)+1u+LJ_FR2;
	      if (!arm64_callxs_snapshot_box_slot(view->snap, view->snapmap,
		    view->nsnap, view->nsnapmap, active_xsave, base,
		    base+1u+actual_count, boxref, 1)) return 0;
	      if (bc_b(sourceins) != 1)
		result_slot = 1u+LJ_FR2+(MSize)bc_a(sourceins);
	    }
	    active_call = ref;
	    protocol = 4;
	    nc++;
	  } else if (ins.o == IR_CALLS &&
		     ins.op2 == IRCALL_lj_ffi_native_trace_leave) {
	    if (protocol != 4 || ins.op1 != REF_NIL ||
		irt_type(ins.t) != IRT_INT || !irt_isguard(ins.t) ||
		ref <= active_call ||
		(result_is_cdata && (boxref == 0 || !box_store_seen)) ||
		(!result_is_cdata && (boxref != 0 || box_store_seen)))
	      return 0;
	    if (result_is_cdata && result_slot != 0 &&
		!arm64_callxs_snapshot_box_slot(view->snap, view->snapmap,
		  view->nsnap, view->nsnapmap, ref, 1u+LJ_FR2,
		  result_slot, boxref, 0)) return 0;
	    active_call = 0;
	    active_leave = ref;
	    protocol = 5;
	  } else if (ins.o == IR_EQ && protocol == 5) {
	    IRRef zero = ins.op1 == active_leave ? ins.op2 :
		ins.op2 == active_leave ? ins.op1 : 0;
	    if (ins.t.irt != (IRT_INT|IRT_GUARD) || zero == 0 ||
		!arm64_callxs_kint_value(view->ir, view->nk, zero, NULL) ||
		ir_load_acq(&view->ir[zero]).i != 0)
	      return 0;
	    active_leave = 0;
	    if (producer_pending && producer_scalar) {
	      completed_producer = producer_call;
	      producer_returnpc = xsave_returnpc;
	    } else {
	      completed_producer = 0;
	      producer_returnpc = 0;
	      producer_type = IRT_NIL;
	    }
	    producer_pending = 0;
	    active_xsave = 0;
	    protocol = 0;
	    boxref = 0;
	    box_ctype = 0;
	    box_store_seen = 0;
	    result_is_cdata = 0;
	  } else if (ins.o == IR_EQ) {
	    if (!irt_isguard(ins.t) || ins.op1 >= ref || ins.op2 >= ref ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1) ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op2) ||
		!arm64_callxs_types_comparable(
		  arm64_callxs_ref_type(view->ir, view->nk, ins.op1),
		  arm64_callxs_ref_type(view->ir, view->nk, ins.op2)))
	      return 0;
	  } else if (ins.o == IR_NE) {
	    if (!irt_isguard(ins.t) || ins.op1 >= ref || ins.op2 >= ref ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1) ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op2) ||
		!arm64_callxs_types_comparable(
		  arm64_callxs_ref_type(view->ir, view->nk, ins.op1),
		  arm64_callxs_ref_type(view->ir, view->nk, ins.op2)))
	      return 0;
	  } else if (ins.o == IR_CALLS &&
		     ins.op2 == IRCALL_lj_tab_gettv_rooted) {
	    if (protocol != 0 || irt_type(ins.t) != IRT_P64 ||
		!irt_isguard(ins.t) ||
		!arm64_callxs_carg_values(view->ir, view->nk, semantic_nins,
			ref, 3, lookupargs))
	      return 0;
	    if (lookupargs[2] != lookupargs[0] ||
		lookupargs[0] >= ref || lookupargs[1] >= ref ||
		ir_load_acq(&view->ir[lookupargs[0]]).o != IR_TMPREF ||
		ir_load_acq(&view->ir[lookupargs[1]]).o != IR_TMPREF ||
		ir_load_acq(&view->ir[lookupargs[0]]).t.irt != IRT_PGC ||
		ir_load_acq(&view->ir[lookupargs[1]]).t.irt != IRT_PGC ||
		ir_load_acq(&view->ir[lookupargs[0]]).op2 !=
		  (IRTMPREF_IN1|IRTMPREF_OUT1) ||
		ir_load_acq(&view->ir[lookupargs[1]]).op2 != IRTMPREF_IN2 ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref,
		  ir_load_acq(&view->ir[lookupargs[0]]).op1) ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref,
		  ir_load_acq(&view->ir[lookupargs[1]]).op1))
	      return 0;
	  } else if (ins.o == IR_LOOP) {
	    if (protocol != 0 || ins.t.irt != (IRT_NIL|IRT_GUARD) ||
		ins.op1 != 0 || ins.op2 != 0)
	      return 0;
	    nloop++;
	    completed_producer = 0;
	    producer_returnpc = 0;
	    producer_type = IRT_NIL;
	  } else if (ins.o == IR_XPOLL) {
	    if (protocol != 0 || ins.t.irt != (IRT_NIL|IRT_GUARD) ||
		ins.op1 != 1 || ins.op2 != 0)
	      return 0;
	    npoll++;
	    completed_producer = 0;
	    producer_returnpc = 0;
	    producer_type = IRT_NIL;
	  } else if (ins.o == IR_BASE) {
	    if (ref != REF_BASE || ins.t.irt != IRT_PGC || ins.op1 || ins.op2)
	      return 0;
	  } else if (ins.o == IR_NOP) {
	    if (ins.t.irt != IRT_NIL || ins.op1 || ins.op2)
	      return 0;
	  } else if (ins.o == IR_FLOAD) {
	    if (ins.op1 >= ref || !arm64_callxs_ref_valid(view->ir, view->nk,
		semantic_nins, ref, ins.op1))
	      return 0;
	    if (ins.op2 == IRFL_CDATA_PTR) {
	      IRIns src = ir_load_acq(&view->ir[ins.op1]);
	      if (src.o != IR_SLOAD || irt_type(src.t) != IRT_CDATA ||
		irt_type(ins.t) != IRT_P64)
	        return 0;
	    } else if (ins.op2 == IRFL_CDATA_CTYPEID) {
	      IRIns src = ir_load_acq(&view->ir[ins.op1]);
	      if (src.o != IR_SLOAD || irt_type(src.t) != IRT_CDATA ||
		!arm64_callxs_integer_type(irt_type(ins.t)) ||
		((IRT_IS64 >> irt_type(ins.t)) & 1u) != 0)
	        return 0;
	    } else if (ins.op2 == IRFL_TAB_META ||
		ins.op2 == (GG_OFS(g.gcroot[GCROOT_BASEMT+(~LJ_TCDATA)]) >> 2)) {
	      if (ins.op1 != REF_NIL || irt_type(ins.t) != IRT_TAB ||
		!arm64_callxs_has_equal_guard(view->ir, semantic_nins, view->nk,
		  ref, IRT_TAB))
	        return 0;
	    } else {
	      return 0;
	    }
	  } else if (ins.o == IR_VLOAD) {
	    IRIns src;
	    if (irt_type(ins.t) != IRT_FUNC || !irt_isguard(ins.t) ||
		ins.op2 != 0 || ins.op1 >= ref ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1))
	      return 0;
	    src = ir_load_acq(&view->ir[ins.op1]);
	    if (src.o != IR_CALLS || src.op2 != IRCALL_lj_tab_gettv_rooted ||
		!arm64_callxs_has_equal_guard(view->ir, semantic_nins, view->nk,
		  ref, IRT_FUNC))
	      return 0;
	  } else if (ins.o == IR_TMPREF) {
	    if (ins.t.irt != IRT_PGC ||
		(ins.op2 & ~(IRTMPREF_IN1|IRTMPREF_OUT1|IRTMPREF_IN2|IRTMPREF_OUT2)) != 0 ||
		(ins.op2 & (IRTMPREF_IN1|IRTMPREF_OUT1|IRTMPREF_IN2|IRTMPREF_OUT2)) == 0 ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1))
	      return 0;
	  } else if (ins.o == IR_CARG) {
	    if (ins.t.irt != IRT_NIL || ins.op1 >= ref || ins.op2 >= ref ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1) ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op2))
	      return 0;
	  } else if (ins.o == IR_PHI) {
	    if (irt_isguard(ins.t) ||
		!arm64_callxs_phi_types_match(view->ir, view->nk, ref, &ins) ||
		!arm64_postra_callxs_cdata_phi(view, semantic_nins, ref, &ins) ||
		ins.op1 >= ref || ins.op2 >= ref ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1) ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op2))
	      return 0;
	  } else if (ins.o == IR_SLOAD) {
	    if (ins.op1 < 1u+LJ_FR2 || ins.op1 >= pt->framesize ||
		(ins.op2 & ~(IRSLOAD_PARENT|IRSLOAD_FRAME|IRSLOAD_TYPECHECK|
		 IRSLOAD_CONVERT|IRSLOAD_READONLY|IRSLOAD_INHERIT|IRSLOAD_KEYINDEX)) != 0 ||
		(irt_type(ins.t) != IRT_INT && irt_type(ins.t) != IRT_NUM &&
		 irt_type(ins.t) != IRT_FLOAT && irt_type(ins.t) != IRT_CDATA &&
		 irt_type(ins.t) != IRT_P64 && irt_type(ins.t) != IRT_TAB &&
		 irt_type(ins.t) != IRT_FUNC && irt_type(ins.t) != IRT_STR))
	      return 0;
	  } else if (ins.o == IR_ADD || ins.o == IR_SUB || ins.o == IR_MUL) {
	    if (ins.op1 >= ref || ins.op2 >= ref ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1) ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op2) ||
		(ins.o != IR_ADD && ins.t.irt != IRT_NUM) ||
		(ins.o == IR_ADD &&
		 (ins.t.irt & ~(IRT_TYPE|IRT_ISPHI)) != 0) ||
		!arm64_callxs_add_shape(view->ir, view->nk, irt_type(ins.t), ins.op1, ins.op2))
	      return 0;
	  } else if (ins.o == IR_ADDOV || ins.o == IR_SUBOV ||
		     ins.o == IR_MULOV) {
	    if (!arm64_callxs_int_overflow_shape(view->ir, view->nk,
		semantic_nins, ref, &ins))
	      return 0;
	  } else if (ins.o == IR_CONV) {
	    IRIns src;
	    IRType dst = irt_type(ins.t);
	    IRType from = (IRType)(ins.op2 & IRCONV_SRCMASK);
	    if ((ins.t.irt & ~(IRT_TYPE|IRT_ISPHI)) != 0 ||
		ins.op1 >= ref || !arm64_callxs_ref_valid(view->ir, view->nk,
		semantic_nins, ref, ins.op1))
	      return 0;
	    src = ir_load_acq(&view->ir[ins.op1]);
	    if (irt_type(src.t) != from ||
		!((dst == IRT_NUM && from == IRT_INT &&
		   ins.op2 == IRCONV_NUM_INT &&
		   ins.t.irt == IRT_NUM) ||
		  ((dst == IRT_U8 || dst == IRT_U16) && from == IRT_INT &&
		   ins.op2 == ((dst << IRCONV_DSH)|IRT_INT) &&
		   ins.t.irt == dst) ||
		  (dst == IRT_FLOAT &&
		   (from == IRT_NUM || from == IRT_INT) &&
		   ins.op2 == ((IRT_FLOAT << IRCONV_DSH)|from) &&
		   ins.t.irt == IRT_FLOAT) ||
		  (dst == IRT_NUM && from == IRT_FLOAT &&
		   ins.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT) &&
		   ins.t.irt == IRT_NUM)))
	      return 0;
	  } else if (ins.o == IR_USE) {
	    if (irt_type(ins.t) != IRT_NIL || ins.op2 != 0 ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1))
	      return 0;
	  } else if (ins.o == IR_CNEW) {
	    if (boxref != 0 || irt_type(ins.t) != IRT_CDATA ||
		(ins.t.irt & IRT_GUARD) == 0 || ins.op2 != REF_NIL ||
		!arm64_callxs_const_member(view->ir, view->nk, ins.op1) ||
		!arm64_callxs_kint_value(view->ir, view->nk, ins.op1, NULL))
	      return 0;
	    boxref = ref;
	    box_ctype = (CTypeID)ir_load_acq(&view->ir[ins.op1]).i;
	  } else if (ins.o == IR_XSTORE) {
	    IRIns payload;
	    int64_t offset;
	    if (protocol != 4 || !result_is_cdata || boxref == 0 || box_store_seen ||
		ins.op2 != active_call || ins.op1 >= ref ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1))
	      return 0;
	    payload = ir_load_acq(&view->ir[ins.op1]);
	    if (payload.o != IR_ADD || irt_type(payload.t) != IRT_P64 ||
		payload.op1 != boxref ||
		!arm64_callxs_offset_value(view->ir, view->nk, payload.op2, &offset) ||
		offset != (int64_t)sizeof(GCcdata) ||
		irt_type(ins.t) != irt_type(ir_load_acq(&view->ir[active_call]).t))
	      return 0;
	    box_store_seen = 1;
	  } else if (ins.o == IR_LT || ins.o == IR_GE || ins.o == IR_LE || ins.o == IR_GT) {
	    IRType t1, t2;
	    if (ins.t.irt != (IRT_INT|IRT_GUARD) || ins.op1 >= ref || ins.op2 >= ref ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1) ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op2))
	      return 0;
	    t1 = arm64_callxs_ref_type(view->ir, view->nk, ins.op1);
	    t2 = arm64_callxs_ref_type(view->ir, view->nk, ins.op2);
	    if (t1 != t2 || (t1 != IRT_INT && t1 != IRT_NUM))
	      return 0;
	  } else if (ins.o == IR_ULE || ins.o == IR_UGE) {
	    if (ins.t.irt != (IRT_NUM|IRT_GUARD) || ins.op1 >= ref ||
		ins.op2 >= ref ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1) ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op2) ||
		arm64_callxs_ref_type(view->ir, view->nk, ins.op1) != IRT_NUM ||
		arm64_callxs_ref_type(view->ir, view->nk, ins.op2) != IRT_NUM)
	      return 0;
	  } else {
	    /* Every CALLS ID and opcode must have matched a validated branch above. */
	    return 0;
	  }
	  if (ins.s != SPS_NONE) {
	    MSize width = irt_is64(ins.t) ? 2u : 1u;
	    MSize end = (MSize)ins.s + width;
	    if (!arm64_postra_spill_slot(ins.s, capacity) || end > capacity ||
		(width == 2u && (((MSize)ins.s - SPS_FIRST) & 1u) != 0))
	      return 0;
	    if (end > highest) highest = end;
	  }
	  if (ra_hasreg(ins.r)) {
	    RegSet rs = irt_type(ins.t) == IRT_FLOAT || irt_type(ins.t) == IRT_NUM ?
		RSET_FPR : RSET_GPR;
	    if (ins.r >= RID_MAX_FPR || !rset_test(rs, ins.r))
	      return 0;
	  }
	}

	for (ref = view->nk; ref < REF_TRUE; ) {
	  IRIns k = ir_load_acq(&view->ir[ref]);
	  IRRef next;
	  if (!arm64_callxs_const_member(view->ir, view->nk, ref))
	    return 0;
	  if ((k.o == IR_KINT || k.o == IR_KINT64 || k.o == IR_KNUM ||
	       k.o == IR_KGC || k.o == IR_KPTR || k.o == IR_KPRI) &&
	      (k.r != RID_INIT || k.s != SPS_NONE))
	    return 0;
	  next = arm64_callxs_const_next(view->ir, ref);
	  if (next <= ref || next > REF_TRUE)
	    return 0;
	  ref = next;
	}
	if (nc == 0 || protocol != 0 || active_call != 0 ||
	    nx != nc || nloop != 1 || npoll != 1 || highest > capacity || !arm64_postra_callxs_snapshot_locations(view,
		semantic_nins, capacity))
	  return 0;
	for (ref = REF_BASE; ref < semantic_nins; ref++) {
	  IRRef call = ref, fpref, cdref, typeref, guard;
	  IRIns c = ir_load_acq(&view->ir[call]);
	  LJArm64ABISignature abi;
	  IRRef values[LJ_ARM64_ABI_MAX_ARGS];
	  unsigned ngpr = 0, nfpr = 0, bytes = 0;
	  unsigned rawcount, actual_count, tail_count, i;
	  int32_t wrapper_id = 0;
	  BCIns sourceins;
	  uintptr_t source_returnpc;
	  int found_type = 0;
	  if (c.o == IR_XSAVE) {
	    if (layout_xsave != 0) return 0;
	    layout_xsave = call;
	    continue;
	  }
	  if (c.o != IR_CALLXS) continue;
	  if (layout_xsave == 0 ||
	      !arm64_callxs_xsave_anchor(view->ir, semantic_nins, layout_xsave,
		view->proto_bc, view->proto_sizebc, &source_returnpc, &sourceins))
	    return 0;
	  if (c.op2 < REF_FIRST || c.op2 >= call) return 0;
	  fpref = c.op2;
	  if (ir_load_acq(&view->ir[fpref]).o == IR_CARG) {
	    IRIns wrapper = ir_load_acq(&view->ir[fpref]);
	    if (wrapper.t.irt != IRT_NIL ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins,
		  fpref, wrapper.op1) ||
		!arm64_callxs_kint_value(view->ir, view->nk, wrapper.op2,
		  &wrapper_id) || wrapper_id <= 0)
	      return 0;
	    fpref = wrapper.op1;
	  }
	  if (ir_load_acq(&view->ir[fpref]).o != IR_FLOAD ||
	      ir_load_acq(&view->ir[fpref]).op2 != IRFL_CDATA_PTR)
	    return 0;
	  /* The captured function pointer can already occupy an argument GPR
	  ** (for example X2). asm_callx relocates indirect targets into its
	  ** dedicated non-argument register before emitting BLR_AUTH. */
	  if (!ra_hasreg(ir_load_acq(&view->ir[fpref]).r) ||
	      !rset_test(RSET_GPR,
		ir_load_acq(&view->ir[fpref]).r))
	    return 0;
	  cdref = ir_load_acq(&view->ir[fpref]).op1;
	  if (cdref < REF_FIRST || cdref >= fpref ||
	      ir_load_acq(&view->ir[cdref]).o != IR_SLOAD)
	    return 0;
	  for (typeref = REF_FIRST; typeref < call && !found_type; typeref++) {
	    IRIns typeid = ir_load_acq(&view->ir[typeref]);
	    if (typeid.o != IR_FLOAD || typeid.op1 != cdref ||
		typeid.op2 != IRFL_CDATA_CTYPEID)
	      continue;
	    for (guard = typeref+1u; guard < call; guard++) {
	      IRIns eq = ir_load_acq(&view->ir[guard]);
	      IRRef kref;
	      IRIns k;
	      if (eq.o != IR_EQ || !irt_isguard(eq.t) ||
		  (eq.op1 != typeref && eq.op2 != typeref))
		continue;
	      kref = eq.op1 == typeref ? eq.op2 : eq.op1;
	      if (kref < view->nk || kref >= REF_TRUE) continue;
	      k = ir_load_acq(&view->ir[kref]);
	      if (k.o == IR_KINT && k.t.irt == IRT_INT && k.i > 0 &&
		  lj_ccall_arm64_signature(view->cts, (CTypeID)k.i, &abi) &&
		  ((abi.is_vararg && wrapper_id == k.i) ||
		   (!abi.is_vararg && wrapper_id == 0)))
		found_type = 1;
	      break;
	    }
	  }
	  if (!found_type || !arm64_callxs_arglist(view->ir, view->nk,
	      semantic_nins, call, values, &rawcount) ||
	      rawcount > LJ_ARM64_ABI_MAX_ARGS) return 0;
	  if (rawcount > abi.nargs) {
	    if (!abi.is_vararg || rawcount <= abi.nargs+1u ||
		values[abi.nargs] != REF_NIL) return 0;
	    for (i = 0; i < rawcount; i++)
	      if (i != abi.nargs && values[i] == REF_NIL) return 0;
	    actual_count = rawcount-1u;
	    for (i = abi.nargs; i < actual_count; i++) values[i] = values[i+1u];
	  } else {
	    if (rawcount != abi.nargs) return 0;
	    actual_count = rawcount;
	    for (i = 0; i < actual_count; i++)
	      if (values[i] == REF_NIL) return 0;
	  }
	  tail_count = actual_count-abi.nargs;
	  if (bc_op(sourceins) == BC_CALLM) {
	    if (layout_producer_returnpc == 0 ||
		!arm64_callxs_callm_pair(view->proto_bc, view->proto_sizebc,
		  layout_producer_returnpc, source_returnpc, actual_count)) return 0;
	    layout_producer_returnpc = 0;
	  } else if (bc_op(sourceins) == BC_CALL) {
	    if (bc_c(sourceins) == 0 ||
		(unsigned)bc_c(sourceins)-1u != actual_count) return 0;
	    layout_producer_returnpc = bc_b(sourceins) == 0 ? source_returnpc : 0;
	  } else return 0;
	  if (abi.is_vararg && !arm64_postra_vararg_source_proof(&temp,
	      layout_xsave, call, view->proto_bc, view->proto_sizebc,
	      source_returnpc, sourceins, cdref, abi.nargs, actual_count, values))
	    return 0;
	  layout_xsave = 0;
	  for (guard = 0; guard < abi.nargs; guard++) {
	    LJArm64ABIArg *a = &abi.arg[guard];
	    if (!arm64_callxs_value_kind(view->ir, values[guard], view->nk,
		semantic_nins, call,
		(LJArm64ABIKind)a->kind, a->size, a->is_unsigned,
		a->reg == LJ_ARM64_ABI_STACK)) return 0;
	    {
	      IRIns carrier = ir_load_acq(&view->ir[values[guard]]);
	      unsigned width = lj_ir_type_size[irt_type(carrier.t)];
	      int fp = irt_isfp(carrier.t);
	      unsigned *bank = fp ? &nfpr : &ngpr;
	      unsigned limit = fp ? REGARG_NUMFPR : REGARG_NUMGPR;
	      if ((fp ? LJ_ARM64_ABI_FPR : LJ_ARM64_ABI_GPR) != a->cls)
		return 0;
	      if (*bank < limit) {
		if (a->reg != (*bank)++) return 0;
	      } else {
		if (width == 0 || width > 8 || width != a->size ||
		    a->reg != LJ_ARM64_ABI_STACK) return 0;
		bytes = (bytes + width-1u) & ~(width-1u);
		if (a->stack_offset != bytes) return 0;
		bytes += width;
	      }
	    }
	  }
	  if (bytes != abi.stack_size) return 0;
	  if (tail_count != 0) {
	    bytes = (bytes+7u) & ~7u;
	    for (i = abi.nargs; i < actual_count; i++) {
	      IRType type = arm64_callxs_ref_type(view->ir, view->nk, values[i]);
	      LJArm64ABIKind kind;
	      unsigned width;
	      if (type == IRT_NUM) kind = LJ_ARM64_ABI_KIND_FLOAT, width = 8;
	      else if (type == IRT_INT) kind = LJ_ARM64_ABI_KIND_INTEGER, width = 4;
	      else if (type == IRT_P64) kind = LJ_ARM64_ABI_KIND_POINTER, width = 8;
	      else return 0;
	      if (!arm64_callxs_value_kind(view->ir, values[i], view->nk,
		  semantic_nins, call, kind, width, 0, 1)) return 0;
	      bytes += 8u;
	    }
	  }
	  if (bytes > capacity*sizeof(int32_t))
	    return 0;
	  if (bytes > outgoing) outgoing = bytes;
	  if (irt_type(c.t) != arm64_callxs_result_type(&abi) ||
	      (abi.result_class == LJ_ARM64_ABI_GPR &&
	       !ra_hasspill(regsp_spill(c.prev)) && c.r != RID_RET &&
	       !(c.r == RID_NONE && c.s == SPS_NONE &&
		 !ra_hasspill(regsp_spill(c.prev)) &&
		 arm64_postra_callxs_result_unused(view, semantic_nins, call))) ||
	      /* ra_destreg moves S0/D0 into an already allocated destination.
	      ** A nested result may need a preserved FPR across the leave/enter
	      ** helpers before its consumer places it in an outgoing stack slot. */
	      (abi.result_class == LJ_ARM64_ABI_FPR && c.r != RID_FPRET &&
	       !(irt_type(c.t) == IRT_FLOAT &&
		 c.r >= RID_D8 && c.r <= RID_D15)))
	    return 0;
	  if (!arm64_postra_callxs_live_across(view, semantic_nins, call, capacity))
	    return 0;
	}
	if (layout_xsave != 0) return 0;
	/* The assembler reserves the largest outgoing area before allocating
	** any spills. A call may overwrite that entire area, including dead
	** argument slots, so no snapshot or live value may own part of it. */
	for (ref = REF_BASE; ref < semantic_nins; ref++) {
	  IRIns value = ir_load_acq(&view->ir[ref]);
	  if (ra_hasspill(value.s) &&
	      (MSize)sps_scale(value.s) < outgoing)
	    return 0;
	}
	if (semantic_ninsp) *semantic_ninsp = semantic_nins;
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
  if (bc_op(view->startins) == BC_FORL) {
    IRRef callref;
    for (callref = REF_BASE; callref < view->nins; callref++) {
      if (ir_load_acq(&view->ir[callref]).o == IR_CALLXS)
	return arm64_postra_callxs_generic(view, semantic_ninsp);
    }
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
	&forl_dynamic_step, 0))
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
      } else return 0;
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

static int arm64_side_parent_ref_valid(const LJArm64SideIRView *side,
	IRRef ref);
static int arm64_side_parent_location(const LJArm64SideIRView *side,
	IRRef value, MSize ordinal, RegSP *rsp);
static int arm64_scalar_postra_register(Reg r, IRType type);
static int arm64_side_parent_snapshot_valid(const LJArm64SideIRView *side);
static int arm64_side_scalar_topology(const LJArm64SideIRView *side,
	const GCtrace *owner);
static int arm64_side_scalar_context_postra(const LJArm64PostRAView *view,
	const LJArm64SideIRView *side);

int lj_asm_arm64_side_prehead_admit(const LJArm64SidePostRAView *view,
	IRRef *semantic_ninsp)
{
  const IRIns *ir;
  IRIns ins;
  IRRef ref, first_parent = 0, last_parent = 0;
  MSize mapneed = 0;

  if (view == NULL || (ir = view->semantic.ir) == NULL ||
	!view->owner || view->owner->traceno != view->semantic.traceno ||
	view->owner->root != view->semantic.parent ||
	!arm64_side_scalar_topology(&view->semantic, view->owner) ||
	view->owner->sinktags ||
	view->owner->startins != BCINS_AD(BC_JMP, 0, 0) ||
	trace_startpc_acq((GCtrace *)view->owner) !=
	  view->semantic.parent_continuation ||
	view->semantic.root != view->semantic.parent ||
	!arm64_side_parent_snapshot_valid(&view->semantic) ||
	view->nins != view->semantic.nins+1u ||
	view->orignins != view->semantic.nins ||
	view->spadjust != 0 || view->parent_spadjust != 0 ||
	(view->entry != NULL && view->topslot != view->semantic.root_topslot) ||
	(view->entry == NULL && view->topslot != 0 &&
	 view->topslot != view->semantic.root_topslot) ||
	view->parent_topslot != view->semantic.parent_topslot ||
	(view->parentmap_n != 0 && view->parentmap == NULL) ||
	view->parentmap_n > LJ_MAX_JSLOTS ||
	(view->parentmap != NULL &&
	 ((uintptr_t)(const void *)view->parentmap &
	 (sizeof(uint16_t)-1u)) != 0))
    return 0;
  for (ref = REF_FIRST; ref < view->semantic.nins; ref++) {
    ins = ir_load_acq(&ir[ref]);
    if (ins.o == IR_SLOAD &&
	(ins.op2 & (IRSLOAD_PARENT|IRSLOAD_INHERIT)) ==
	 (IRSLOAD_PARENT|IRSLOAD_INHERIT)) {
	if (!first_parent) first_parent = ref;
	last_parent = ref;
	mapneed++;
    }
  }

  if ((first_parent && view->stopins != last_parent) ||
	(!first_parent && view->stopins != REF_BASE) ||
	view->parentmap_n != mapneed ||
	view->parentmap_n != view->semantic.parent_nregspmap) {
	return 0;
	}
  ins = ir_load_acq(&ir[view->semantic.nins]);

  if (view->entry == NULL) {
    if (ins.o != IR_NOP || ins.t.irt != IRT_NIL ||
	ins.op1 != 0 || ins.op2 != 0 || ins.r != 0 || ins.s != 0)
	return 0;
  } else if (ins.o == IR_RENAME) {
	if (ins.t.irt != IRT_NIL || ins.op1 < REF_FIRST ||
	    ins.op1 >= view->semantic.nins ||
	    ins.op2 >= view->semantic.nsnap || !ra_hasreg(ins.r) ||
	    ins.s != SPS_NONE)
	  return 0;
  } else if (ins.o != IR_NOP || ins.t.irt != IRT_NIL ||
	ins.op1 != 0 || ins.op2 != 0 || ins.r != 0 || ins.s != SPS_NONE) {
	return 0;
  }
  ins = ir_load_acq(&ir[REF_BASE]);
  if (ins.r != RID_BASE || ins.s != SPS_NONE)
    { return 0; }

  /* Parent values are copied from the selected parent exit's register/spill
  ** map. Check every inherited slot by its actual IR reference so this gate
  ** remains independent of allocator register choices and parent exit ID. */
	{
	  MSize parent_ordinal = 0;
	  for (ref = REF_FIRST; ref < view->semantic.nins; ref++) {
	    ins = ir_load_acq(&ir[ref]);
	    if (ins.o != IR_SLOAD ||
		ins.op2 != (IRSLOAD_PARENT|IRSLOAD_INHERIT))
	      continue;
	    {
	      RegSP rs = view->parentmap[parent_ordinal];
	      IRType type = (IRType)irt_type(ins.t);

	      if (rs != view->semantic.parent_regspmap[parent_ordinal] ||
		  !ra_hasreg(ins.r) ||
		  !arm64_scalar_postra_register(ins.r, type) || ins.s != SPS_NONE ||
		  !arm64_scalar_postra_register(regsp_reg(rs), type) ||
		  ra_hasspill(regsp_spill(rs)))
		return 0;
	    }
	    parent_ordinal++;
	  }
	}
  if (semantic_ninsp)
    *semantic_ninsp = view->semantic.nins;
  return 1;
}

int lj_asm_arm64_side_postra_admit(const LJArm64SidePostRAView *view,
	IRRef *semantic_ninsp)
{
  IRRef semantic_nins;
  LJArm64PostRAView core;
  int pre = lj_asm_arm64_side_prehead_admit(view, &semantic_nins);
  if (!pre ||
	view->entry == NULL ||
	((uintptr_t)(const void *)view->entry & (sizeof(MCode)-1u)) != 0 ||
	view->branch_track != (uint8_t)LJ_ABI_BRANCH_TRACK ||
	view->parentmap_n > LJ_MAX_JSLOTS || !view->owner)
    return 0;
  /* Semantic and pre-head checks precede machine loop-label assignment.
  ** Final post-RA admission binds the emitted label to this child body. */
  if (view->semantic.linktype == LJ_TRLINK_LOOP &&
	(view->loop_mcofs == 0 ||
	 view->loop_mcofs >= view->entry_words*sizeof(MCode) ||
	 (view->loop_mcofs & (sizeof(MCode)-1u)) != 0))
	return 0;
  memset(&core, 0, sizeof(core));
  core.ir = view->semantic.ir;
  core.owner = view->owner;
  core.snap = view->semantic.snap;
  core.snapmap = view->semantic.snapmap;
  core.proto_bc = view->semantic.proto_bc;
  /* Post-RA needs the complete allocator view, including validated trailing
  ** RENAMEs; the scalar walker strips those to recover its semantic extent. */
  core.nins = view->nins;
  core.nk = view->semantic.nk;
  core.nsnap = view->semantic.nsnap;
  core.nsnapmap = view->semantic.nsnapmap;
  core.spadjust = view->spadjust;
  core.proto_sizebc = view->semantic.proto_sizebc;
  core.root_topslot = view->topslot;
  core.startins = view->semantic.startins;

  if (!lj_asm_arm64_side_scalar_postra_admit(
	&core, &view->semantic, semantic_ninsp)) {

    return 0;
  }

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
	int allow_wrapping, LJArm64IRReject *reject)
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
    if (!allow_wrapping && ins->o == IR_ADD && irt_type(ins->t) == IRT_INT &&
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
  if (startop == BC_FORL) {
    IRRef ref, nins = trace_nins_acq((GCtrace *)T);
    for (ref = REF_FIRST; ref < nins; ref++) {
      if (ir_load_acq(&T->ir[ref]).o == IR_CALLXS)
	return arm64_ir_callxs_shape(J, T, pt, reject);
    }
  }
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
	&forl_dynamic_step, 0, reject))
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

/* Visible GC TValue types only. The IR range also contains raw pointers and
** prototypes, which are not ordinary table values and must not escape here. */
static int arm64_rooted_gc_type(IRType type)
{
  return type == IRT_STR || type == IRT_TAB || type == IRT_FUNC ||
         type == IRT_THREAD || type == IRT_UDATA || type == IRT_CDATA;
}

/* These fast functions record scalar operations without a remaining call.
** Keep allocation (bit.tohex), cdata arithmetic and other fast functions out
** of this identity-guard capability; their IR still needs its own proof. */
static int arm64_scalar_builtin_ffid(uint32_t ffid)
{
  return ffid == FF_rawset || lj_ff_is_bit_scalar(ffid);
}

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
       ir->op2 == IRCALL_lj_tab_gettv_rooted_hit_try ||
       ir->op2 == IRCALL_lj_tab_getnil_nometa_rooted_try ||
       ir->op2 == IRCALL_lj_tab_storenum_rooted ||
       ir->op2 == IRCALL_lj_tab_storebool_rooted ||
       ir->op2 == IRCALL_lj_tab_storegc_rooted ||
       ir->op2 == IRCALL_lj_tab_storenew_rooted ||
       ir->op2 == IRCALL_lj_tab_storeraw_rooted ||
       ir->op2 == IRCALL_lj_tab_storenil_rooted ||
       ir->op2 == IRCALL_lj_func_getuv_rooted_try ||
       ir->op2 == IRCALL_lj_func_storeuv_rooted_try ||
       ir->op2 == IRCALL_lj_func_getcell_rooted_try ||
       ir->op2 == IRCALL_lj_func_storecell_rooted_try);
  /* Lightuserdata is an opaque segmented token. Like a GC pointer, it can
  ** only originate in an exact tagged load here, never arithmetic or CONV. */
  /* The current function is an immutable frame root (not a type-checked
  ** mutable Lua slot). It is admitted only as SLOAD slot zero. */
  if (type == IRT_FUNC && ir->o == IR_SLOAD && ir->op1 == 0 &&
      ir->t.irt == IRT_FUNC && ir->op2 == IRSLOAD_READONLY)
    return 1;
  if (arm64_rooted_gc_type(type) || type == IRT_LIGHTUD)
    return (ir->o == IR_SLOAD || ir->o == IR_VLOAD ||
            (type == IRT_TAB && ir->o == IR_TNEW)) &&
      (ir->t.irt & ~IRT_ISPHI) == (type|IRT_GUARD);
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
  case IR_BNOT: case IR_BSWAP:
  case IR_BAND: case IR_BOR: case IR_BXOR:
  case IR_BSHL: case IR_BSHR: case IR_BSAR: case IR_BROR:
  case IR_NEG: case IR_TOBIT:
    return 1;
  default:
    return 0;
  }
}

static int arm64_rooted_upvalue_call(IRCallID id)
{
  return id == IRCALL_lj_func_getuv_rooted_try ||
         id == IRCALL_lj_func_storeuv_rooted_try;
}

static int arm64_rooted_cell_call(IRCallID id)
{
  return id == IRCALL_lj_func_getcell_rooted_try ||
         id == IRCALL_lj_func_storecell_rooted_try;
}

/* Exact rooted function operand used by the bounded closed-upvalue helpers. */
static int arm64_upvalue_fnroot_ir(const GCtrace *T, IRRef root,
                                  IRRef before, uint16_t mode)
{
  const IRIns *tmp, *src;
  if (!root || root >= before || root >= T->nins) return 0;
  tmp = &T->ir[root];
  if (tmp->o != IR_TMPREF || tmp->t.irt != IRT_PGC || tmp->op2 != mode ||
      tmp->op1 < REF_FIRST || tmp->op1 >= root)
    return 0;
  src = &T->ir[tmp->op1];
  return src->o == IR_SLOAD && src->op1 == 0 &&
         src->t.irt == IRT_FUNC && src->op2 == IRSLOAD_READONLY;
}

static int arm64_scalar_ir_gc_ref(const GCtrace *T, IRRef value, IRRef before)
{
  IRType type;
  if (value < T->nk || value >= before || value == REF_BASE)
    return 0;
  type = irt_type(T->ir[value].t);
  return arm64_rooted_gc_type(type) &&
         arm64_scalar_ir_ref(T, value, before, type);
}

/* Local cell operands use only visible TValue sources. In particular, raw
** P32 cell identities are never accepted as rooted payloads. */
static int arm64_cell_value_ir_ref(const GCtrace *T, IRRef value,
                                  IRRef before)
{
  return value == REF_NIL ||
         arm64_scalar_ir_ref(T, value, before, IRT_INT) ||
         arm64_scalar_ir_ref(T, value, before, IRT_NUM) ||
         arm64_scalar_ir_ref(T, value, before, IRT_TRUE) ||
         arm64_scalar_ir_ref(T, value, before, IRT_FALSE) ||
         arm64_scalar_ir_ref(T, value, before, IRT_LIGHTUD) ||
         arm64_scalar_ir_gc_ref(T, value, before);
}

/* A P32 local-cell SLOAD is permitted only as an unmaterialized type guard.
** It must not acquire an SSA consumer or appear in any exit snapshot. */
static int arm64_cell_guard_ir(const GCtrace *T, IRRef guardref)
{
  IRRef ref;
  MSize s;
  IRIns guard;
  if (!T || !T->snap || guardref < REF_FIRST || guardref >= T->nins)
    return 0;
  guard = T->ir[guardref];
  if (guard.o != IR_SLOAD || guard.t.irt != (IRT_P32|IRT_GUARD) ||
      guard.op2 != IRSLOAD_TYPECHECK)
    return 0;
  for (ref = guardref+1u; ref < T->nins; ref++) {
    IRIns use = T->ir[ref];
    switch ((IROp)use.o) {
    case IR_BASE: case IR_SLOAD: case IR_KINT: case IR_KNUM: case IR_KGC:
    case IR_KPRI: case IR_LOOP: case IR_XPOLL:
      break;
    case IR_CALLS:
      if (use.op1 == guardref) return 0;
      break;
    default:
      if (use.op1 == guardref || use.op2 == guardref) return 0;
      break;
    }
  }
  for (s = 0; s < T->nsnap; s++) {
    const SnapShot *snap = &T->snap[s];
    MSize ofs = snap->mapofs;
    MSize i;
    if (!T->snapmap || ofs > T->nsnapmap ||
        snap->nent > T->nsnapmap-ofs ||
        T->nsnapmap-ofs-snap->nent < 1u+LJ_FR2)
      return 0;
    for (i = 0; i < snap->nent; i++) {
      SnapEntry se = T->snapmap[ofs+i];
      if (snap_ref(se) == guardref || snap_slot(se) == guard.op1) return 0;
    }
  }
  return 1;
}

static int arm64_cell_guard_proto(const GCproto *pt, MSize slot)
{
  return pt && proto_cellops(pt) && slot >= 1u+LJ_FR2 &&
         slot < 1u+LJ_FR2+pt->framesize;
}

/* Match a TMPREF only when a later exact rooted-cell call consumes it. This
** keeps the broader visible-value root grammar local to this operation. */
static int arm64_cell_root_ir(const GCtrace *T, IRRef root)
{
  const IRIns *tmp;
  IRRef callref;
  if (!T || root < REF_FIRST || root >= T->nins) return 0;
  tmp = &T->ir[root];
  if (tmp->o != IR_TMPREF || tmp->t.irt != IRT_PGC) return 0;
  for (callref = root+1u; callref < T->nins; callref++) {
    const IRIns *call = &T->ir[callref], *tail, *head;
    int get;
    if (call->o != IR_CALLS || !arm64_rooted_cell_call((IRCallID)call->op2))
      continue;
    get = call->op2 == IRCALL_lj_func_getcell_rooted_try;
    if (call->op1 < REF_FIRST || call->op1 >= callref) continue;
    tail = &T->ir[call->op1];
    if (tail->o != IR_CARG || tail->op1 < REF_FIRST ||
        tail->op1 >= call->op1 || tail->op2 != root)
      continue;
    head = &T->ir[tail->op1];
    if (head->o == IR_CARG && head->op2 >= T->nk &&
        head->op2 < REF_TRUE && T->ir[head->op2].o == IR_KINT &&
        head->op1 == REF_BASE &&
        tmp->op2 == (get ? (IRTMPREF_IN1|IRTMPREF_OUT1) : IRTMPREF_IN1) &&
        (get ? tmp->op1 == REF_NIL :
         arm64_cell_value_ir_ref(T, tmp->op1, root)))
      return 1;
  }
  return 0;
}

static int arm64_cell_head_carg_ir(const GCtrace *T, IRRef headref)
{
  IRRef callref;
  for (callref = headref+1u; callref < T->nins; callref++) {
    const IRIns *call = &T->ir[callref], *tail, *head;
    if (call->o != IR_CALLS || !arm64_rooted_cell_call((IRCallID)call->op2) ||
        call->op1 < REF_FIRST || call->op1 >= callref)
      continue;
    tail = &T->ir[call->op1];
    if (tail->o != IR_CARG || tail->op1 < REF_FIRST ||
        tail->op1 >= call->op1)
      continue;
    head = &T->ir[tail->op1];
    if (tail->op1 == headref && head->o == IR_CARG &&
        head->op1 == REF_BASE && head->op2 >= T->nk &&
        head->op2 < REF_TRUE && T->ir[head->op2].o == IR_KINT)
      return 1;
  }
  return 0;
}

static int arm64_rooted_cell_ir_effect(const GCtrace *T, IRRef ref,
                                      IRRef root1, IRRef root2,
                                      IRRef root3)
{
  const IRIns *call = &T->ir[ref], *tmp, *tail, *head, *guard;
  IRRef out;
  int get = call->op2 == IRCALL_lj_func_getcell_rooted_try;
  if (!root1 || root1 >= ref || root2 || root3 || ref+1u >= T->nins ||
      call->t.irt != (IRT_INT|IRT_GUARD) || !arm64_cell_root_ir(T, root1) ||
      call->op1 < REF_FIRST || call->op1 >= ref)
    return 0;
  tmp = &T->ir[root1];
  tail = &T->ir[call->op1];
  if (tail->o != IR_CARG || tail->t.irt != IRT_NIL ||
      tail->op1 < REF_FIRST || tail->op1 >= call->op1 ||
      tail->op2 != root1)
    return 0;
  head = &T->ir[tail->op1];
  if (head->o != IR_CARG || head->t.irt != IRT_NIL ||
      head->op1 != REF_BASE || head->op2 < T->nk ||
      head->op2 >= REF_TRUE || T->ir[head->op2].o != IR_KINT ||
      T->ir[head->op2].t.irt != IRT_INT || head->op2 >= tail->op1 ||
      tail->op1 >= call->op1 || head->op1 >= tail->op1)
    return 0;
  if (tmp->op2 != (get ? (IRTMPREF_IN1|IRTMPREF_OUT1) : IRTMPREF_IN1) ||
      (get ? tmp->op1 != REF_NIL :
       !arm64_cell_value_ir_ref(T, tmp->op1, root1)))
    return 0;
  guard = &T->ir[ref+1u];
  if (guard->o != IR_NE || guard->t.irt != (IRT_INT|IRT_GUARD) ||
      !((guard->op1 == ref && irref_isk(guard->op2)) ||
        (guard->op2 == ref && irref_isk(guard->op1))))
    return 0;
  out = guard->op1 == ref ? guard->op2 : guard->op1;
  if (!arm64_ir_int_kref(T, out) || T->ir[out].i != 0) return 0;
  if (!get) return 5;
  if (ref+2u >= T->nins) return 0;
  {
    const IRIns *value = &T->ir[ref+2u];
    IRType type = irt_type(value->t);
    if (value->o != IR_VLOAD || value->op1 != root1 || value->op2 ||
        (type != IRT_NIL && type != IRT_INT && type != IRT_NUM &&
         type != IRT_TRUE && type != IRT_FALSE && type != IRT_LIGHTUD &&
         !arm64_rooted_gc_type(type)) || !irt_isguard(value->t) ||
        (value->t.irt & ~(IRT_TYPE|IRT_GUARD|IRT_ISPHI)))
      return 0;
  }
  return 4;
}

/* Each rooted helper is an operation with explicit input/output effects.
** Scratch values may be overwritten only after the preceding operation has
** consumed them. Neither call count, table count, key value nor placement in
** the unrolled loop is part of this contract. */
static int arm64_rooted_ir_effect(const GCtrace *T, IRRef ref,
                                 IRRef root1, IRRef root2, IRRef root3)
{
  const IRIns *call = &T->ir[ref], *guard;
  IRRef key;
  if (arm64_rooted_cell_call((IRCallID)call->op2))
    return arm64_rooted_cell_ir_effect(T, ref, root1, root2, root3);
  int tvread = call->op2 == IRCALL_lj_tab_gettv_rooted_hit_try;
  int nilread = call->op2 == IRCALL_lj_tab_getnil_nometa_rooted_try;
  int read = call->op2 == IRCALL_lj_tab_getscalar_rooted_try || tvread || nilread;
  int gcstore = call->op2 == IRCALL_lj_tab_storegc_rooted;
  int newstore = call->op2 == IRCALL_lj_tab_storenew_rooted;
  int rawstore = call->op2 == IRCALL_lj_tab_storeraw_rooted;
  int nilstore = call->op2 == IRCALL_lj_tab_storenil_rooted;
  int uvget = call->op2 == IRCALL_lj_func_getuv_rooted_try;
  int uvset = call->op2 == IRCALL_lj_func_storeuv_rooted_try;
  int store = call->op2 == IRCALL_lj_tab_storenum_rooted ||
              call->op2 == IRCALL_lj_tab_storebool_rooted || gcstore ||
              newstore || rawstore || nilstore;
  if (uvget || uvset) {
    const IRIns *tail, *head, *guard;
    IRRef index, out;
    int get = uvget;
    if (!root1 || ref + 1 >= T->nins ||
        call->t.irt != (IRT_INT|IRT_GUARD) ||
        (get ? root2 != 0 || root3 != 0 : !root2 || root3 != 0) ||
        (!get && root2 >= ref) ||
        !arm64_upvalue_fnroot_ir(T, root1, ref,
              get ? (IRTMPREF_IN1|IRTMPREF_OUT1) : IRTMPREF_IN1) ||
        (!get && (T->ir[root2].o != IR_TMPREF ||
                  T->ir[root2].t.irt != IRT_PGC ||
                  T->ir[root2].op2 != IRTMPREF_IN2 ||
                  root2 >= ref ||
                  !(arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_INT) ||
                    arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_NUM) ||
                    arm64_scalar_ir_gc_ref(T, T->ir[root2].op1, root2) ||
                    arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_LIGHTUD) ||
                    T->ir[root2].op1 == REF_NIL ||
                    arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_TRUE) ||
                    arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_FALSE)))) ||
        call->op1 < REF_FIRST || call->op1 >= ref)
      return 0;
    tail = &T->ir[call->op1];
    if (tail->o != IR_CARG || tail->t.irt != IRT_NIL ||
        tail->op1 < REF_FIRST || tail->op1 >= call->op1 ||
        tail->op2 != (get ? root1 : root2))
      return 0;
    head = &T->ir[tail->op1];
    if (head->o != IR_CARG || head->t.irt != IRT_NIL ||
        head->op1 != root1 || head->op2 < T->nk || head->op2 >= REF_TRUE ||
        T->ir[head->op2].o != IR_KINT ||
        T->ir[head->op2].t.irt != IRT_INT)
      return 0;
    index = head->op2;
    UNUSED(index);
    guard = &T->ir[ref+1];
    if (guard->o != IR_NE || guard->t.irt != (IRT_INT|IRT_GUARD) ||
        !((guard->op1 == ref && irref_isk(guard->op2)) ||
          (guard->op2 == ref && irref_isk(guard->op1))))
      return 0;
    out = guard->op1 == ref ? guard->op2 : guard->op1;
    if (!arm64_ir_int_kref(T, out) || T->ir[out].i != 0)
      return 0;
    if (get) {
      const IRIns *value;
      if (ref+2 >= T->nins) return 0;
      value = &T->ir[ref+2];
      if (value->o != IR_VLOAD || value->op1 != root1 || value->op2 ||
          (irt_type(value->t) != IRT_NIL && irt_type(value->t) != IRT_INT &&
           irt_type(value->t) != IRT_NUM && irt_type(value->t) != IRT_TRUE &&
           irt_type(value->t) != IRT_FALSE &&
           !arm64_rooted_gc_type(irt_type(value->t)) &&
           irt_type(value->t) != IRT_LIGHTUD) || !irt_isguard(value->t) ||
          (value->t.irt & ~(IRT_TYPE|IRT_GUARD|IRT_ISPHI)))
        return 0;
      return 4;
    }
    return 5;
  }
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
    /* The optimized scalar helper only hashes string/number/boolean keys.
    ** Other visible GC keys need the generic helper's exact key lease. */
    if (!nilread && T->ir[root2].op1 == REF_NIL)
      return 0;
    if (!tvread && !nilread &&
        !arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_STR) &&
        !arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_INT) &&
        !arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_NUM) &&
        !arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_TRUE) &&
        !arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_FALSE))
      return 0;
    value = &T->ir[ref+2];
    if (value->o != IR_VLOAD || value->op1 != root1 || value->op2 ||
        (nilread ? value->t.irt != (IRT_NIL|IRT_GUARD) :
         (irt_type(value->t) != IRT_INT && irt_type(value->t) != IRT_NUM &&
         irt_type(value->t) != IRT_TRUE && irt_type(value->t) != IRT_FALSE &&
         !(tvread && (arm64_rooted_gc_type(irt_type(value->t)) ||
                      irt_type(value->t) == IRT_LIGHTUD)))) ||
        !irt_isguard(value->t) ||
        (value->t.irt & ~(IRT_TYPE|IRT_GUARD|IRT_ISPHI)))
      return 0;
  } else if (store) {
    const IRIns *tail, *head;
    IRType type = call->op2 == IRCALL_lj_tab_storenum_rooted ? IRT_NUM : IRT_INT;
    IRRef chain = call->op1;
    if (!root2 || (nilstore && root3) || chain < REF_FIRST || chain >= ref ||
        T->ir[root2].op1 == REF_NIL)
      return 0;
    tail = &T->ir[chain];
    if (nilstore) {
      if (tail->o != IR_CARG || tail->t.irt != IRT_NIL ||
          tail->op1 != root1 || tail->op2 != root2 ||
          root1 >= chain || root2 >= chain ||
          T->ir[root1].op2 != IRTMPREF_IN1 ||
          T->ir[root2].op2 != IRTMPREF_IN2 ||
          T->ir[root2].op1 == REF_NIL ||
          (!arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_STR) &&
          !arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_INT) &&
          !arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_NUM) &&
          !arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_TRUE) &&
          !arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_FALSE) &&
          !arm64_scalar_ir_gc_ref(T, T->ir[root2].op1, root2) &&
          !arm64_scalar_ir_ref(T, T->ir[root2].op1, root2, IRT_LIGHTUD)))
        return 0;
    } else {
      if (tail->o != IR_CARG || tail->t.irt != IRT_NIL ||
          tail->op1 < REF_FIRST || tail->op1 >= chain ||
          ((gcstore || newstore || rawstore) ? (!root3 || tail->op2 != root3 || root3 >= chain ||
                      T->ir[root3].op2 != IRTMPREF_IN3 ||
                      !(arm64_scalar_ir_gc_ref(T, T->ir[root3].op1, root3) ||
                        arm64_scalar_ir_ref(T, T->ir[root3].op1, root3, IRT_LIGHTUD) ||
                        ((newstore || rawstore) && (arm64_scalar_ir_ref(T, T->ir[root3].op1, root3, IRT_NUM) ||
                                      arm64_scalar_ir_ref(T, T->ir[root3].op1, root3, IRT_INT) ||
                                      arm64_scalar_ir_ref(T, T->ir[root3].op1, root3, IRT_TRUE) ||
                                      arm64_scalar_ir_ref(T, T->ir[root3].op1, root3, IRT_FALSE))))) :
           !arm64_scalar_ir_ref(T, tail->op2, chain, type)))
        return 0;
      head = &T->ir[tail->op1];
      if (head->o != IR_CARG || head->t.irt != IRT_NIL ||
          head->op1 != root1 || head->op2 != root2 ||
          root1 >= tail->op1 || root2 >= tail->op1 ||
          T->ir[root1].op2 != IRTMPREF_IN1 ||
          T->ir[root2].op2 != IRTMPREF_IN2)
        return 0;
    }
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

/* Called only after the complete root/snapshot and helper-operand passes.
** Raw stores use an exact caller CALL snapshot for refusal/unwind and an
** exact caller continuation for all later exits. No C frame is restored. */
static int arm64_rawset_ir_snapshots(const GCtrace *T, IRRef ref, SnapNo s)
{
  const IRIns *call = &T->ir[ref], *chain;
  const SnapShot *pre = &T->snap[s], *post;
  const BCIns *pc = snap_pc(&T->snapmap[pre->mapofs+pre->nent]);
  BCIns bc = (BCIns)la_load32_acq((const uint32_t *)pc);
  IRRef args[4] = { 0, 0, 0, 0 }, values[3], fnref, r;
  MSize slot, i;
  GCobj *fn;
  int raw = call->op2 == IRCALL_lj_tab_storeraw_rooted;
  if (bc_op(bc) != BC_CALL) return !raw;
  if ((!raw && call->op2 != IRCALL_lj_tab_storenil_rooted) ||
      bc_c(bc) != 4 || (bc_b(bc) != 1 && bc_b(bc) != 2) ||
      s+1u >= T->nsnap)
    return 0;
  post = &T->snap[s+1u];
  if (post->ref != ref+2u || post->count != SNAPCOUNT_DONE ||
      snap_pc(&T->snapmap[post->mapofs+post->nent]) != pc+1)
    return 0;
  slot = 1u+LJ_FR2+bc_a(bc);
  for (i = 0; i < pre->nent; i++) {
    SnapEntry se = T->snapmap[pre->mapofs+i];
    MSize at = snap_slot(se);
    if (at == slot) args[0] = snap_ref(se);
    else if (at >= slot+1u+LJ_FR2 && at < slot+4u+LJ_FR2)
      args[at-slot-LJ_FR2] = snap_ref(se);
  }
  fnref = args[0];
  if (fnref >= REF_FIRST) {
    fnref = 0;
    for (r = REF_FIRST; r < ref; r++) {
      const IRIns *g = &T->ir[r];
      if (g->o == IR_EQ && g->t.irt == (IRT_FUNC|IRT_GUARD) &&
          g->op1 == args[0]) fnref = g->op2;
    }
  }
  if (!arm64_scalar_ir_ref(T, fnref, ref, IRT_FUNC) ||
      fnref >= REF_TRUE || T->ir[fnref].o != IR_KGC)
    return 0;
  fn = ir_kgc_acq(&T->ir[fnref]);
  if (!fn || !checkptrGC(fn) || fn->gch.gct != (uint32_t)~LJ_TFUNC ||
      lj_func_ffid_acq(gco2func(fn)) != FF_rawset)
    return 0;
  chain = &T->ir[call->op1];
  values[2] = raw ? T->ir[chain->op2].op1 : REF_NIL;
  if (raw) chain = &T->ir[chain->op1];
  values[0] = T->ir[chain->op1].op1;
  values[1] = T->ir[chain->op2].op1;
  for (i = 0; i < 3; i++) {
    const IRIns *v;
    if (args[i+1] == values[i]) continue;
    if (!args[i+1] || values[i] < REF_FIRST) return 0;
    v = &T->ir[values[i]];
    if (v->o != IR_CONV || (v->t.irt & ~IRT_ISPHI) != IRT_NUM ||
        v->op2 != IRCONV_NUM_INT || v->op1 != args[i+1])
      return 0;
  }
  if (post->nslots != slot+bc_b(bc)-1u) return 0;
  if (bc_b(bc) == 2) {
    for (i = 0; i < post->nent; i++) {
      SnapEntry se = T->snapmap[post->mapofs+i];
      if (snap_slot(se) == slot) return snap_ref(se) == values[0];
    }
    return 0;
  }
  return 1;
}

static int arm64_upvalue_ir_source(const jit_State *J, const GCtrace *T,
                                  IRRef ref, SnapNo s, IRRef root1,
                                  IRRef root2)
{
  const IRIns *call = &T->ir[ref], *tail, *head;
  const SnapShot *pre = &T->snap[s], *post;
  const BCIns *pc = snap_pc(&T->snapmap[pre->mapofs+pre->nent]);
  BCIns bc = (BCIns)la_load32_acq((const uint32_t *)pc);
  int get = call->op2 == IRCALL_lj_func_getuv_rooted_try;
  uint32_t uv;
  IRRef value = 0;
  MSize i, slot;
  if (!arm64_upvalue_fnroot_ir(T, root1, ref, IRTMPREF_IN1) &&
      !arm64_upvalue_fnroot_ir(T, root1, ref,
                               IRTMPREF_IN1|IRTMPREF_OUT1))
    return 0;
  if ((get && bc_op(bc) != BC_UGET) ||
      (!get && bc_op(bc) != BC_USETV && bc_op(bc) != BC_USETS &&
       bc_op(bc) != BC_USETN && bc_op(bc) != BC_USETP))
    return 0;
  uv = get ? bc_d(bc) : bc_a(bc);
  if (!J || !J->pt || uv >= J->pt->sizeuv) return 0;
  if (call->op1 < REF_FIRST || call->op1 >= ref) return 0;
  tail = &T->ir[call->op1];
  if (tail->o != IR_CARG || tail->op1 < REF_FIRST ||
      tail->op1 >= call->op1 || tail->t.irt != IRT_NIL ||
      tail->op2 != (get ? root1 : root2)) return 0;
  head = &T->ir[tail->op1];
  if (head->o != IR_CARG || head->t.irt != IRT_NIL ||
      head->op1 != root1 || root1 >= tail->op1 ||
      head->op2 < T->nk || head->op2 >= REF_TRUE ||
      T->ir[head->op2].o != IR_KINT ||
      T->ir[head->op2].t.irt != IRT_INT ||
      T->ir[head->op2].i != (int32_t)uv) return 0;
  if (s+1u >= T->nsnap) return 0;
  post = &T->snap[s+1u];
  if (post->ref != ref+2u ||
      snap_pc(&T->snapmap[post->mapofs+post->nent]) != (get ? pc : pc+1))
    return 0;
  if (get) return 1;
  if (post->count != SNAPCOUNT_DONE || !root2) return 0;
  value = T->ir[root2].op1;
  if (bc_op(bc) == BC_USETV) {
    slot = 1u+LJ_FR2+bc_d(bc);
    for (i = 0; i < pre->nent; i++) {
      SnapEntry se = T->snapmap[pre->mapofs+i];
      if (snap_slot(se) == slot) {
        IRRef snapvalue = snap_ref(se);
        if (value == snapvalue) return 1;
        if (value >= REF_FIRST && value < ref &&
            T->ir[value].o == IR_CONV &&
            (T->ir[value].t.irt & ~IRT_ISPHI) == IRT_NUM &&
            T->ir[value].op2 == IRCONV_NUM_INT &&
            T->ir[value].op1 == snapvalue)
          return 1;
        return 0;
      }
    }
    /* Readonly/unmodified SLOAD slots are elided from snapshot maps. Accept
    ** only the exact source register load, with an optional NUM_INT cast. */
    if (value >= REF_FIRST && value < ref &&
        T->ir[value].o == IR_CONV &&
        (T->ir[value].t.irt & ~IRT_ISPHI) == IRT_NUM &&
        T->ir[value].op2 == IRCONV_NUM_INT)
      value = T->ir[value].op1;
    return value >= REF_FIRST && value < ref &&
           T->ir[value].o == IR_SLOAD && T->ir[value].op1 == slot &&
           T->ir[value].op2 == IRSLOAD_TYPECHECK;
  }
  if (bc_op(bc) == BC_USETP)
    return bc_d(bc) <= 2u && value == (IRRef)(REF_NIL-bc_d(bc));
  if (value < T->nk || value >= REF_TRUE) return 0;
  if (bc_op(bc) == BC_USETN) {
    TValue expected;
    const IRIns *k = &T->ir[value];
    if (bc_d(bc) >= J->pt->sizekn) return 0;
    lj_tv_load_acq(&expected, proto_knumtv(J->pt, bc_d(bc)));
    if (tvisint(&expected))
      return k->o == IR_KINT && k->t.irt == IRT_INT && k->i == intV(&expected);
    if (k->o != IR_KNUM || k->t.irt != IRT_NUM) return 0;
    return tv_rawload(&expected) == tv_rawload(ir_knum(k));
  } else {
    GCobj *expected, *actual;
    const IRIns *k = &T->ir[value];
    ptrdiff_t idx = ~(ptrdiff_t)bc_d(bc);
    if (k->o != IR_KGC || idx >= 0 ||
        (uintptr_t)idx < ~(uintptr_t)proto_sizekgc_acq(J->pt)+1u)
      return 0;
    expected = proto_kgc_acq(J->pt, idx);
    actual = ir_kgc_acq(k);
    return expected != NULL && expected->gch.gct == (uint32_t)~LJ_TSTR &&
           k->t.irt == IRT_STR && actual == expected;
  }
}

static int arm64_cell_rhs_ir(const GCtrace *T, const SnapShot *pre,
                             MSize slot, IRRef value, IRRef callref)
{
  MSize i;
  for (i = 0; i < pre->nent; i++) {
    SnapEntry se = T->snapmap[pre->mapofs+i];
    if (snap_slot(se) == slot) {
      IRRef saved = snap_ref(se);
      if (value == saved) return 1;
      if (value >= REF_FIRST && value < callref) {
        const IRIns *conv = &T->ir[value];
        return conv->o == IR_CONV &&
               (conv->t.irt & ~IRT_ISPHI) == IRT_NUM &&
               conv->op2 == IRCONV_NUM_INT && conv->op1 == saved;
      }
      return 0;
    }
  }
  if (value >= REF_FIRST && value < callref &&
      T->ir[value].o == IR_CONV &&
      (T->ir[value].t.irt & ~IRT_ISPHI) == IRT_NUM &&
      T->ir[value].op2 == IRCONV_NUM_INT)
    value = T->ir[value].op1;
  return value >= REF_FIRST && value < callref &&
         T->ir[value].o == IR_SLOAD && T->ir[value].op1 == slot &&
         T->ir[value].op2 == IRSLOAD_TYPECHECK;
}

/* Bind a cell helper to the exact CGET/CSET bytecode and its guard-only
** local-cell SLOAD. No cell pointer is accepted as a TValue operand. */
static int arm64_cell_ir_source(const jit_State *J, const GCtrace *T,
                                IRRef ref, SnapNo s, IRRef root1)
{
  const SnapShot *pre, *post;
  const BCIns *pc;
  BCIns bc;
  const IRIns *call = &T->ir[ref];
  IRRef guardref;
  uint32_t local;
  MSize slot, count = 0;
  int get = call->op2 == IRCALL_lj_func_getcell_rooted_try;
  if (!J || !J->pt || !T->snap || !T->snapmap || s >= T->nsnap ||
      (bc_op(T->startins) != BC_LOOP &&
                       bc_op(T->startins) != BC_FORL) ||
      !proto_cellops(J->pt) || J->pt->framesize == 0 ||
      s+1u >= T->nsnap)
    return 0;
  pre = &T->snap[s];
  post = &T->snap[s+1u];
  if (pre->mapofs > T->nsnapmap ||
      pre->nent > T->nsnapmap-pre->mapofs ||
      T->nsnapmap-pre->mapofs-pre->nent < 1u+LJ_FR2 ||
      post->mapofs > T->nsnapmap ||
      post->nent > T->nsnapmap-post->mapofs ||
      T->nsnapmap-post->mapofs-post->nent < 1u+LJ_FR2)
    return 0;
  pc = snap_pc(&T->snapmap[pre->mapofs+pre->nent]);
  bc = (BCIns)la_load32_acq((const uint32_t *)pc);
  if (pre->ref != root1 ||
      (get ? bc_op(bc) != BC_CGET : bc_op(bc) != BC_CSET)) return 0;
  if ((get && bc_a(bc) >= J->pt->framesize) ||
      (!get && bc_d(bc) >= J->pt->framesize))
    return 0;
  local = get ? bc_d(bc) : bc_a(bc);
  if (local >= J->pt->framesize) return 0;
  slot = 1u+LJ_FR2+local;
  if (!arm64_cell_guard_proto(J->pt, slot)) return 0;
  if (post->ref != ref+2u ||
      snap_pc(&T->snapmap[post->mapofs+post->nent]) !=
        (get ? pc : pc+1))
    return 0;
  if (!get && (post->count != SNAPCOUNT_DONE ||
               bc_d(bc) >= J->pt->framesize ||
               !arm64_cell_rhs_ir(T, pre, 1u+LJ_FR2+bc_d(bc),
                                  T->ir[root1].op1, ref)))
    return 0;
  for (guardref = REF_FIRST; guardref < ref; guardref++) {
    const IRIns *guard = &T->ir[guardref];
    if (guard->o == IR_SLOAD && guard->op1 == slot &&
        guard->t.irt == (IRT_P32|IRT_GUARD) &&
        guard->op2 == IRSLOAD_TYPECHECK) {
      if (!arm64_cell_guard_ir(T, guardref)) return 0;
      count++;
    }
  }
  if (count != 1u) return 0;
  {
    IRRef tailref = T->ir[ref].op1;
    const IRIns *tail, *head;
    if (tailref < REF_FIRST || tailref >= ref) return 0;
    tail = &T->ir[tailref];
    if (tail->o != IR_CARG || tail->op1 < REF_FIRST ||
        tail->op1 >= tailref)
      return 0;
    head = &T->ir[tail->op1];
    if (head->o != IR_CARG || head->op2 < T->nk ||
        head->op2 >= REF_TRUE)
      return 0;
    IRRef kref = head->op2;
    return kref >= T->nk && kref < REF_TRUE &&
           T->ir[kref].o == IR_KINT &&
           T->ir[kref].i == (int32_t)local;
  }
}

static int arm64_cell_ir_snapshots(const jit_State *J, const GCtrace *T,
                                   IRRef ref, SnapNo s, IRRef root1)
{
  const IRIns *call = &T->ir[ref];
  int get = call->op2 == IRCALL_lj_func_getcell_rooted_try;
  return arm64_cell_ir_source(J, T, ref, s, root1) &&
         (get || T->snap[s+1u].count == SNAPCOUNT_DONE);
}

static int arm64_upvalue_ir_snapshots(const jit_State *J, const GCtrace *T,
                                     IRRef ref, SnapNo s,
                                     IRRef root1, IRRef root2)
{
  const SnapShot *pre = &T->snap[s];
  const BCIns *pc = snap_pc(&T->snapmap[pre->mapofs+pre->nent]);
  int get = T->ir[ref].op2 == IRCALL_lj_func_getuv_rooted_try;
  if (pre->ref != root1 ||
      !arm64_upvalue_ir_source(J, T, ref, s, root1, root2))
    return 0;
  if (!get && pre->count == SNAPCOUNT_DONE) return 0;
  if (get && snap_pc(&T->snapmap[T->snap[s+1u].mapofs+
                                  T->snap[s+1u].nent]) != pc)
    return 0;
  return 1;
}

static int arm64_scalar_side_geometry_ir(const LJArm64SideIRView *side,
	uintptr_t lo, uintptr_t hi, MSize *continuation_pos)
{
  MSize header, end, cont;
  int64_t target;
  BCIns h, back;
  if (!side || !side->proto_bc || !side->parent_continuation ||
	  side->loop_start_pc >= side->loop_end_pc ||
	  side->loop_end_pc >= side->proto_sizebc ||
	  side->loop_header_pc >= side->loop_end_pc)
    return 0;
  header = side->loop_header_pc;
  end = side->loop_end_pc;
  h = (BCIns)la_load32_acq((const uint32_t *)&side->proto_bc[header]);
  back = (BCIns)la_load32_acq((const uint32_t *)&side->proto_bc[end]);
  target = (int64_t)end + 1 + (int64_t)bc_j(back);
  if (bc_op(h) != BC_JLOOP || bc_d(h) != side->parent ||
	  bc_op(back) != BC_JMP || bc_j(back) >= 0 ||
	  (int64_t)end + 1 + (int64_t)bc_j(back) < 0 ||
	  target < 0 || (MSize)target != side->loop_start_pc ||
      !arm64_ir_pcpos((uintptr_t)(const void *)side->parent_continuation,
	    lo, hi, &cont) || cont < (MSize)target || cont >= end)
    return 0;
  if (continuation_pos)
    *continuation_pos = cont;
  return 1;
}

static int arm64_side_parent_ref_valid(const LJArm64SideIRView *side,
	IRRef ref)
{
	MSize i;
	if (!side || !side->parent_snap ||
	    !side->parent_map_values ||
	    side->parent_nmap_values != snap_nent_acq(side->parent_snap) ||
	    side->parent_nins <= REF_FIRST ||
	    side->parent_nins >= REF_DROP || !side->parent_nk ||
	    side->parent_nk > REF_TRUE || side->parent_nk >= REF_BASE ||
	    side->parent_snap->ref < REF_FIRST ||
	    side->parent_snap->ref >= side->parent_nins)
	  return 0;
	if (irref_isk(ref)) {
	  if (ref >= REF_BASE)
	    return 0;
	  for (i = 0; i < side->parent_nmap_values; i++) {
		const LJArm64SideParentMapValue *mv = &side->parent_map_values[i];
		if (mv->ref != ref) continue;
		if (ref >= REF_TRUE && ref <= REF_NIL)
		  return mv->value.o == IR_KPRI &&
			 mv->value.t.irt == REF_NIL-ref && mv->value.op12 == 0;
		return ref >= side->parent_nk && ref < REF_TRUE && mv->ref_start &&
		  (mv->value.o == IR_KINT || mv->value.o == IR_KNUM ||
		   mv->value.o == IR_KGC || mv->value.o == IR_KPTR ||
		   mv->value.o == IR_KNULL || mv->value.o == IR_KINT64);
	  }
	  return 0;
	}
	if (ref < REF_FIRST || ref >= side->parent_nins ||
	    ref >= side->parent_snap->ref)
	  return 0;
	for (i = 0; i < side->parent_nmap_values; i++)
	  if (side->parent_map_values[i].ref == ref)
		return side->parent_map_values[i].ref_start == 0;
	return 0;
}

static int arm64_side_parent_location(const LJArm64SideIRView *side,
	IRRef value, MSize ordinal, RegSP *rsp)
{
	if (!side || !rsp || !side->parent_bindings ||
	    ordinal >= side->parent_nregspmap ||
	    !arm64_side_parent_ref_valid(side, value) ||
	    side->parent_bindings[ordinal].ref != value)
	  return 0;
	*rsp = (RegSP)side->parent_bindings[ordinal].regsp;
	return regsp_used(*rsp);
}

static int arm64_side_parent_sload_valid(const LJArm64SideIRView *side,
	const IRIns *load, MSize ordinal)
{
  MSize i, nent;
  IRRef ref;
  IRType type;
  RegSP rs;
  if (!side || !load || !side->parent_snap ||
	  load->op2 != (IRSLOAD_PARENT|IRSLOAD_INHERIT) ||
	  (irt_type(load->t) != IRT_INT && irt_type(load->t) != IRT_NUM) ||
	  (load->t.irt & ~IRT_TYPE) || ordinal >= side->parent_nregspmap ||
	  side->parent_nslots > LJ_MAX_JSLOTS ||
	  snap_nent_acq(side->parent_snap) > side->parent_nsnapmap ||
	  side->parent_nsnapmap-snap_nent_acq(side->parent_snap) < 1u+LJ_FR2)
	return 0;
  nent = snap_nent_acq(side->parent_snap);
  for (i = 0; i < nent; i++) {
	SnapEntry sn = snapentry_acq(&side->parent_snapmap[i]);
		if (snap_slot(sn) != load->op1)
	  continue;
	ref = snap_ref(sn);
	if (!arm64_side_parent_ref_valid(side, ref) ||
	    !side->parent_bindings || ordinal >= side->parent_nregspmap ||
	    side->parent_bindings[ordinal].slot != load->op1 ||
	    side->parent_bindings[ordinal].ref != ref)
	  return 0;
	if (i >= side->parent_nmap_values ||
	    side->parent_map_values[i].slot != load->op1 ||
	    side->parent_map_values[i].ref != ref ||
	    side->parent_map_values[i].value.o !=
	      side->parent_bindings[ordinal].value.o ||
	    side->parent_map_values[i].value.t.irt !=
	      side->parent_bindings[ordinal].value.t.irt ||
	    side->parent_map_values[i].value.op12 !=
	      side->parent_bindings[ordinal].value.op12)
	  return 0;
	type = (IRType)irt_type(side->parent_bindings[ordinal].value.t);
	if (irref_isk(ref) &&
	    !((type == IRT_INT && side->parent_bindings[ordinal].value.o == IR_KINT) ||
	      (type == IRT_NUM && side->parent_bindings[ordinal].value.o == IR_KNUM)))
	  return 0;
	if (type != irt_type(load->t))
	  return 0;
	rs = (RegSP)side->parent_regspmap[ordinal];
	  {
	    RegSP actual;
	    return arm64_side_parent_location(side, ref, ordinal, &actual) &&
		   actual == rs &&
		   !ra_hasspill(regsp_spill(rs)) &&
		   arm64_scalar_postra_register(regsp_reg(rs), type);
	  }
  }
  return 0;
}

static int arm64_scalar_ir_admit_mode(const jit_State *J, const GCtrace *T,
				 BCOp rootop, const LJArm64SideIRView *side,
				 LJArm64IRReject *reject)
{
  IRRef ref = REF_BASE, loop = 0, poll = 0, firstphi = 0;
  IRRef root1 = 0, root2 = 0, root3 = 0, resultroot = 0, resultref = 0;
  IRRef previous_snap = 0, poll_snap = 0, completed_store = 0;
  MSize expected_ofs = 0, maxslots;
  uintptr_t proto_lo, proto_hi;
  unsigned nphi = 0, loop_snap = 0;
  int forl_dynamic_step = 0;
  MSize parent_sload = 0;
  SnapNo s;
  if (reject) {
    reject->reason = LJ_ARM64_IR_REJECT_NONE;
    reject->ref = 0;
    reject->op = IR_NOP;
    reject->detail = LJ_ARM64_IR_CALL_NONE;
  }
  if (!J || !T || !T->ir || !J->pt ||
      (!side && (J->parent || J->exitno || T->root ||
	bc_op(T->startins) != rootop)) ||
      (side && (J->parent != side->parent || J->exitno != side->exitno ||
	T->root != side->parent || side->root != side->parent ||
	  side->traceno != T->traceno ||
	  !arm64_side_scalar_topology(side, T) ||
	T->startins != BCINS_AD(BC_JMP, 0, 0) ||
	trace_startpc_acq((GCtrace *)T) != side->parent_continuation ||
	  J->startpc != side->parent_continuation ||
	  (side->linktype == LJ_TRLINK_LOOP &&
	   (J->pc != J->startpc || J->pc != side->parent_continuation)) ||
	  !side->parent_snap)) ||
      !T->traceno || T->nins <= REF_FIRST || T->nins >= REF_DROP || !T->nk ||
      T->nk > REF_TRUE ||
      (!side && (bc_op(T->startins) != rootop ||
       (rootop != BC_LOOP && rootop != BC_FORL) ||
       T->link != T->traceno || T->linktype != LJ_TRLINK_LOOP)) ||
      J->baseslot != 1 + LJ_FR2 || J->framedepth || J->retdepth ||
      trace_startptgco_acq((GCtrace *)T) != obj2gco(J->pt) ||
      !checkptrGC(obj2gco(J->pt)) || J->pt->gct != (uint32_t)~LJ_TPROTO ||
      !J->pt->framesize)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE, IR_LOOP,
                           0);
  if (T->sinktags)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SINK, REF_BASE, IR_TNEW,
                           T->sinktags);
  if (side) {
	if (!arm64_ir_proto_range(J->pt, &proto_lo, &proto_hi) ||
	    !arm64_scalar_side_geometry_ir(side, proto_lo, proto_hi, NULL))
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE,
			 REF_BASE, IR_LOOP, (uint16_t)side->parent);
  } else if (!arm64_ir_start(J, T, J->pt, &proto_lo, &proto_hi, reject)) {
	return 0;
  }
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
    if (k->o == IR_KGC && arm64_rooted_gc_type((IRType)k->t.irt) && !k->op12 &&
        (!side || k->t.irt == IRT_STR) &&
        ref + LJ_GC64 < REF_TRUE) {
      GCobj *o = ir_kgc_acq(k);
      if (!o || !checkptrGC(o) || o->gch.gct != k->t.irt)
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
      (!side && T->ir[REF_BASE].op12) ||
      (side && (T->ir[REF_BASE].op1 != side->parent ||
	T->ir[REF_BASE].op2 != side->exitno))) {
    ref = REF_BASE;
    goto bad_operand;
  }
  for (ref = REF_FIRST; ref < T->nins; ref++) {
    const IRIns *ir = &T->ir[ref];
    IRType type = (IRType)irt_type(ir->t);
    unsigned flags = ir->t.irt & ~IRT_TYPE;
    int binary = 0;
    /* Child traces currently prove only scalar effects. Root capability
    ** growth must not silently widen the side-publication boundary. */
    if (side) {
      if (type != IRT_INT && type != IRT_NUM && type != IRT_NIL)
        goto bad_type;
      switch ((IROp)ir->o) {
      case IR_NOP: case IR_SLOAD: case IR_ADDOV: case IR_SUBOV: case IR_MULOV:
      case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_CONV:
      case IR_LT: case IR_GE: case IR_LE: case IR_GT: case IR_EQ: case IR_NE:
      case IR_LOOP: case IR_XPOLL: case IR_PHI: case IR_USE:
        break;
      default:
        goto bad_operand;
      }
    }
    if (firstphi && ir->o != IR_PHI)
      goto bad_operand;
    switch ((IROp)ir->o) {
    case IR_NOP:
      /* Narrowing can remove a superseded overflow guard. NOP defines no
      ** value; normal operand/snapshot provenance cannot reference it. */
      if (ir->t.irt != IRT_NIL || ir->op12)
        goto bad_operand;
      break;
    case IR_SLOAD:
      if (side && ir->op2 == (IRSLOAD_PARENT|IRSLOAD_INHERIT)) {
	if (!arm64_side_parent_sload_valid(side, ir, parent_sload++))
	  goto bad_operand;
	break;
      }
      if (!side && ir->t.irt == (IRT_P32|IRT_GUARD) &&
          ir->op2 == IRSLOAD_TYPECHECK &&
          arm64_cell_guard_proto(J->pt, ir->op1) &&
          arm64_cell_guard_ir(T, ref))
	break;
      if (!side && type == IRT_FUNC && flags == 0 && ir->op1 == 0 &&
          ir->op2 == IRSLOAD_READONLY && ir->t.irt == IRT_FUNC)
        break;
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
      if (arm64_rooted_gc_type(type) || type == IRT_LIGHTUD ||
          type == IRT_TRUE || type == IRT_FALSE || (!side && type == IRT_NIL)) {
        if ((flags & ~((arm64_rooted_gc_type(type) || type == IRT_LIGHTUD) ?
                       IRT_ISPHI : 0)) != IRT_GUARD ||
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
        if (!arm64_scalar_ir_ref(T, ir->op1, ref, IRT_TAB) &&
            !(type == IRT_PGC && ir->op2 == IRTMPREF_IN1 &&
              arm64_upvalue_fnroot_ir(T, ref, ref+1u, IRTMPREF_IN1)) &&
            !(type == IRT_PGC && ir->op2 == (IRTMPREF_IN1|IRTMPREF_OUT1) &&
              arm64_upvalue_fnroot_ir(T, ref, ref+1u,
                                      IRTMPREF_IN1|IRTMPREF_OUT1)) &&
            !(type == IRT_PGC && arm64_cell_root_ir(T, ref)))
          goto bad_operand;
        root1 = ref;
      } else if (ir->op2 == IRTMPREF_IN2) {
        if (!arm64_scalar_ir_ref(T, ir->op1, ref, IRT_INT) &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_NUM) &&
            !arm64_scalar_ir_gc_ref(T, ir->op1, ref) &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_LIGHTUD) &&
            ir->op1 != REF_NIL &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_TRUE) &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_FALSE))
          goto bad_operand;
        root2 = ref;
      } else if (ir->op2 == IRTMPREF_IN3) {
        if (!arm64_scalar_ir_gc_ref(T, ir->op1, ref) &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_LIGHTUD) &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_NUM) &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_INT) &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_TRUE) &&
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_FALSE))
          goto bad_operand;
        root3 = ref;
      } else goto bad_operand;
      break;
    case IR_VLOAD:
      if (ref != resultref || !resultroot || ir->op1 != resultroot ||
          ir->op2 || (type != IRT_INT && type != IRT_NUM &&
                      type != IRT_TRUE && type != IRT_FALSE && type != IRT_NIL &&
                      !arm64_rooted_gc_type(type) && type != IRT_LIGHTUD) ||
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
      /* The stock bit narrowing pass uses wrapping INT ADD/SUB. These are
      ** pure 32-bit operations, distinct from guarded Lua arithmetic. */
      if (type == IRT_INT && ((!side && !(flags & ~IRT_ISPHI)) ||
          (rootop == BC_FORL && flags == IRT_ISPHI))) {
        binary = 1;
        break;
      }
      if (type != IRT_NUM || (flags & ~IRT_ISPHI))
        goto bad_type;
      binary = 1;
      break;
    case IR_SUB:
      if (!side && type == IRT_INT && !(flags & ~IRT_ISPHI)) {
        binary = 1;
        break;
      }
      /* fallthrough */
    case IR_MUL:
    case IR_DIV:
      if (type != IRT_NUM || (flags & ~IRT_ISPHI))
        goto bad_type;
      binary = 1;
      break;
    case IR_BNOT: case IR_BSWAP:
      if (type != IRT_INT || (flags & ~IRT_ISPHI) || ir->op2 ||
          !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_INT))
        goto bad_operand;
      break;
    case IR_NEG:
      /* ARM64 unifies ROL with ROR of the negated 32-bit count. */
      if (type != IRT_INT || (flags & ~IRT_ISPHI) ||
          ir->op2 != ir->op1 ||
          !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_INT))
        goto bad_operand;
      break;
    case IR_BAND: case IR_BOR: case IR_BXOR:
    case IR_BSHL: case IR_BSHR: case IR_BSAR: case IR_BROR:
      if (type != IRT_INT || (flags & ~IRT_ISPHI))
        goto bad_type;
      binary = 1;
      break;
    case IR_TOBIT:
      if (type != IRT_INT || (flags & ~IRT_ISPHI) ||
          !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_NUM) ||
          !arm64_scalar_ir_ref(T, ir->op2, ref, IRT_NUM) ||
          !irref_isk(ir->op2) || T->ir[ir->op2].o != IR_KNUM ||
          T->ir[ir->op2+1u].tv.u64 != U64x(43380000,00000000))
        goto bad_operand;
      break;
    case IR_LT:
    case IR_GE:
    case IR_LE:
    case IR_GT:
    case IR_EQ:
    case IR_NE:
      if (type == IRT_FUNC && ir->o == IR_EQ && flags == IRT_GUARD) {
        GCobj *o;
        /* Specialize only exact supported builtin closures. */
        if (ir->op1 < REF_FIRST ||
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_FUNC) ||
            !arm64_scalar_ir_ref(T, ir->op2, ref, IRT_FUNC) ||
            ir->op2 >= REF_TRUE || T->ir[ir->op2].o != IR_KGC)
          goto bad_operand;
        o = ir_kgc_acq(&T->ir[ir->op2]);
        if (o == NULL || !checkptrGC(o) || o->gch.gct != (uint32_t)~LJ_TFUNC ||
            !arm64_scalar_builtin_ffid(lj_func_ffid_acq(gco2func(o))))
          goto bad_operand;
        break;
      }
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
	  if (poll || (side ?
	      (side->linktype == LJ_TRLINK_ROOT ? loop != 0 :
	       (!loop || ref != loop + 1)) : (!loop || ref != loop + 1)) ||
          ir->t.irt != (IRT_NIL | IRT_GUARD) || ir->op1 != 1 || ir->op2)
        goto bad_poll;
      poll = ref;
      break;
    case IR_PHI:
      if (!firstphi)
        firstphi = ref;
      if (++nphi > LJ_MAX_PHI || !poll || flags ||
          (type != IRT_INT && type != IRT_NUM &&
           !arm64_rooted_gc_type(type) && type != IRT_LIGHTUD) ||
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
      if (type != IRT_NIL || flags || ir->op1 >= ref)
        goto bad_operand;
      if (ir->op1 == REF_BASE && arm64_cell_head_carg_ir(T, ref))
        break;
      if (ir->op1 < REF_FIRST ||
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
        if (side && (arm64_rooted_upvalue_call((IRCallID)ir->op2) ||
                     arm64_rooted_cell_call((IRCallID)ir->op2)))
          return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL, ref,
                                 IR_CALLS, ir->op2);
        int effect = arm64_rooted_ir_effect(T, ref, root1, root2, root3);
        if (!effect)
          return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL, ref,
                                 IR_CALLS, ir->op2);
        if (effect == 2 || effect == 4) {
          resultroot = root1; resultref = ref+2;
        }
        if (effect == 5) completed_store = ref;
        root1 = root2 = root3 = 0;
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
  if (side && parent_sload != side->parent_nregspmap)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND, REF_BASE,
			       IR_SLOAD, (uint16_t)parent_sload);
  ref = loop;
  if (side && side->linktype == LJ_TRLINK_ROOT) {
	if (loop || !poll || J->loopref != 0)
	  goto bad_poll;
  } else if (!loop || poll != loop + 1 || J->loopref != loop) {
	goto bad_poll;
  }
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
                             1, reject))
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
           !arm64_scalar_ir_gc_ref(T, value, ref) &&
           !arm64_scalar_ir_ref(T, value, ref, IRT_LIGHTUD) &&
           (side || value != REF_NIL) &&
           !arm64_scalar_ir_ref(T, value, ref, IRT_TRUE) &&
           !arm64_scalar_ir_ref(T, value, ref, IRT_FALSE)))
        goto bad_snapshot;
    }
    memcpy(&pcbase, &T->snapmap[snap->mapofs + snap->nent], sizeof(pcbase));
    if ((uint8_t)pcbase ||
        !arm64_ir_pcpos((uintptr_t)(pcbase >> 8), proto_lo, proto_hi, &pcpos))
      goto bad_snapshot;
    if (side && ((s == 0 && pcpos != proto_bcpos(J->pt,
	       side->parent_continuation)) ||
	       (pcpos < side->loop_start_pc ||
	        (pcpos >= side->loop_end_pc &&
	         !(s != 0 && pcpos == side->loop_end_pc+1u))))) {
	  goto bad_snapshot;
	}
  }
  if (side && side->linktype == LJ_TRLINK_ROOT) {
	if (loop_snap || poll_snap != poll)
	  goto bad_snapshot;
  } else if (!loop_snap || poll_snap != loop) {
	goto bad_snapshot;
  }
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
         ir->op2 == IRCALL_lj_tab_storebool_rooted ||
         ir->op2 == IRCALL_lj_tab_storegc_rooted ||
         ir->op2 == IRCALL_lj_tab_storenew_rooted ||
         ir->op2 == IRCALL_lj_tab_storeraw_rooted ||
         ir->op2 == IRCALL_lj_tab_storenil_rooted)) {
      if (!arm64_rawset_ir_snapshots(T, ref, s)) goto bad_snapshot;
      completed_store = ref;
    }
    if (ir->o == IR_CALLS && arm64_rooted_upvalue_call((IRCallID)ir->op2)) {
      const IRIns *tail = &T->ir[ir->op1], *head = &T->ir[tail->op1];
      int get = ir->op2 == IRCALL_lj_func_getuv_rooted_try;
      IRRef fnroot = head->op1;
      IRRef valroot = get ? 0 : tail->op2;
      if (!arm64_upvalue_ir_snapshots(J, T, ref, s, fnroot, valroot))
        goto bad_snapshot;
      if (!get) completed_store = ref;
    }
    if (ir->o == IR_CALLS && arm64_rooted_cell_call((IRCallID)ir->op2)) {
      const IRIns *tail = &T->ir[ir->op1];
      IRRef root = tail->op2;
      int get = ir->op2 == IRCALL_lj_func_getcell_rooted_try;
      if (!arm64_cell_ir_snapshots(J, T, ref, s, root))
        goto bad_snapshot;
      if (!get) completed_store = ref;
    }
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
  return arm64_scalar_ir_admit_mode(J, T, BC_LOOP, NULL, reject);
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
  case IR_BNOT: case IR_BSWAP:
  case IR_BAND: case IR_BOR: case IR_BXOR:
  case IR_BSHL: case IR_BSHR: case IR_BSAR: case IR_BROR:
  case IR_NEG: case IR_TOBIT:
    return (p.t.irt & ~IRT_ISPHI) == IRT_INT;
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
           (p.o == IR_SLOAD && p.t.irt == IRT_FUNC &&
            p.op1 == 0 && p.op2 == IRSLOAD_READONLY) ||
           (p.o == IR_SLOAD &&
            (arm64_rooted_gc_type(irt_type(p.t)) ||
             irt_type(p.t) == IRT_LIGHTUD) &&
            (p.t.irt & ~IRT_ISPHI) == (irt_type(p.t)|IRT_GUARD));
  case IR_TNEW:
    return (p.t.irt & ~IRT_ISPHI) == (IRT_TAB|IRT_GUARD);
  case IR_TMPREF:
    return irt_type(p.t) == IRT_PGC;
  case IR_CALLS:
    return irt_type(p.t) == IRT_INT &&
           (p.op2 == IRCALL_lj_tab_len_rooted_try ||
            p.op2 == IRCALL_lj_tab_getscalar_rooted_try ||
            p.op2 == IRCALL_lj_tab_gettv_rooted_hit_try ||
            p.op2 == IRCALL_lj_tab_getnil_nometa_rooted_try ||
            p.op2 == IRCALL_lj_tab_storenum_rooted ||
            p.op2 == IRCALL_lj_tab_storebool_rooted ||
            p.op2 == IRCALL_lj_tab_storegc_rooted ||
            p.op2 == IRCALL_lj_tab_storenew_rooted ||
            p.op2 == IRCALL_lj_tab_storeraw_rooted ||
            p.op2 == IRCALL_lj_tab_storenil_rooted ||
            p.op2 == IRCALL_lj_func_getuv_rooted_try ||
            p.op2 == IRCALL_lj_func_storeuv_rooted_try ||
            p.op2 == IRCALL_lj_func_getcell_rooted_try ||
            p.op2 == IRCALL_lj_func_storecell_rooted_try);
  case IR_VLOAD:
    return (irt_type(p.t) == IRT_INT || irt_type(p.t) == IRT_NUM ||
            irt_type(p.t) == IRT_NIL || irt_type(p.t) == IRT_TRUE ||
            irt_type(p.t) == IRT_FALSE ||
            arm64_rooted_gc_type(irt_type(p.t)) ||
            irt_type(p.t) == IRT_LIGHTUD) &&
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
  if (type == IRT_FUNC && p.o == IR_SLOAD && p.op1 == 0 &&
      p.t.irt == IRT_FUNC && p.op2 == IRSLOAD_READONLY)
    return 1;
  return arm64_scalar_postra_value(p) && irt_type(p.t) == type &&
         ((!arm64_rooted_gc_type(type) && type != IRT_LIGHTUD) ||
          ((p.o == IR_SLOAD || p.o == IR_VLOAD ||
            (type == IRT_TAB && p.o == IR_TNEW)) &&
           (p.t.irt & ~IRT_ISPHI) == (type|IRT_GUARD)));
}
static int arm64_scalar_postra_gc_ref(const LJArm64PostRAView *v, IRRef ref,
                                      IRRef before)
{
  IRIns p;
  if (ref < v->nk || ref >= before || ref == REF_BASE)
    return 0;
  p = ir_load_acq(&v->ir[ref]);
  return arm64_rooted_gc_type(irt_type(p.t)) &&
         arm64_scalar_postra_ref(v, ref, before, irt_type(p.t));
}

static int arm64_cell_value_postra_ref(const LJArm64PostRAView *v,
                                       IRRef value, IRRef before)
{
  return value == REF_NIL ||
         arm64_scalar_postra_ref(v, value, before, IRT_INT) ||
         arm64_scalar_postra_ref(v, value, before, IRT_NUM) ||
         arm64_scalar_postra_ref(v, value, before, IRT_TRUE) ||
         arm64_scalar_postra_ref(v, value, before, IRT_FALSE) ||
         arm64_scalar_postra_ref(v, value, before, IRT_LIGHTUD) ||
         arm64_scalar_postra_gc_ref(v, value, before);
}

static int arm64_cell_guard_postra(const LJArm64PostRAView *v, IRRef guardref)
{
  IRRef ref;
  MSize s;
  IRIns guard;
  if (!v || !v->snap || guardref < REF_FIRST || guardref >= v->nins)
    return 0;
  guard = ir_load_acq(&v->ir[guardref]);
  if (guard.o != IR_SLOAD || guard.t.irt != (IRT_P32|IRT_GUARD) ||
      guard.op2 != IRSLOAD_TYPECHECK || guard.s != SPS_NONE ||
      ra_hasreg(guard.r))
    return 0;
  for (ref = guardref+1u; ref < v->nins; ref++) {
    IRIns use = ir_load_acq(&v->ir[ref]);
    switch ((IROp)use.o) {
    case IR_BASE: case IR_SLOAD: case IR_KINT: case IR_KNUM: case IR_KGC:
    case IR_KPRI: case IR_LOOP: case IR_XPOLL:
      break;
    case IR_CALLS:
      if (use.op1 == guardref) return 0;
      break;
    default:
      if (use.op1 == guardref || use.op2 == guardref) return 0;
      break;
    }
  }
  for (s = 0; s < v->nsnap; s++) {
    const SnapShot *snap = &v->snap[s];
    MSize ofs = snap_mapofs_acq(snap);
    MSize i;
    if (!v->snapmap || ofs > v->nsnapmap ||
        snap_nent_acq(snap) > v->nsnapmap-ofs ||
        v->nsnapmap-ofs-snap_nent_acq(snap) < 1u+LJ_FR2)
      return 0;
    for (i = 0; i < snap_nent_acq(snap); i++) {
      SnapEntry se = snapentry_acq(&v->snapmap[ofs+i]);
      if (snap_ref(se) == guardref || snap_slot(se) == guard.op1) return 0;
    }
  }
  return 1;
}

static int arm64_cell_root_postra(const LJArm64PostRAView *v, IRRef root)
{
  IRIns tmp;
  IRRef callref;
  if (!v || root < REF_FIRST || root >= v->nins) return 0;
  tmp = ir_load_acq(&v->ir[root]);
  if (tmp.o != IR_TMPREF || tmp.t.irt != IRT_PGC || !ra_hasreg(tmp.r) ||
      !arm64_scalar_postra_register(tmp.r, IRT_PGC) || ra_hasspill(tmp.s))
    return 0;
  for (callref = root+1u; callref < v->nins; callref++) {
    IRIns call = ir_load_acq(&v->ir[callref]), tail, head;
    int get;
    if (call.o != IR_CALLS || !arm64_rooted_cell_call((IRCallID)call.op2) ||
        call.op1 < REF_FIRST || call.op1 >= callref)
      continue;
    get = call.op2 == IRCALL_lj_func_getcell_rooted_try;
    tail = ir_load_acq(&v->ir[call.op1]);
    if (tail.o != IR_CARG || tail.op1 < REF_FIRST ||
        tail.op1 >= call.op1 || tail.op2 != root)
      continue;
    head = ir_load_acq(&v->ir[tail.op1]);
    if (head.o == IR_CARG && head.op1 == REF_BASE &&
        head.op2 >= v->nk && head.op2 < REF_TRUE &&
        ir_load_acq(&v->ir[head.op2]).o == IR_KINT &&
        tmp.op2 == (get ? (IRTMPREF_IN1|IRTMPREF_OUT1) : IRTMPREF_IN1) &&
        (get ? tmp.op1 == REF_NIL :
         arm64_cell_value_postra_ref(v, tmp.op1, root)))
      return 1;
  }
  return 0;
}

static int arm64_cell_head_carg_postra(const LJArm64PostRAView *v,
                                       IRRef headref)
{
  IRRef callref;
  for (callref = headref+1u; callref < v->nins; callref++) {
    IRIns call = ir_load_acq(&v->ir[callref]), tail, head;
    if (call.o != IR_CALLS || !arm64_rooted_cell_call((IRCallID)call.op2) ||
        call.op1 < REF_FIRST || call.op1 >= callref)
      continue;
    tail = ir_load_acq(&v->ir[call.op1]);
    if (tail.o != IR_CARG || tail.op1 < REF_FIRST ||
        tail.op1 >= call.op1)
      continue;
    head = ir_load_acq(&v->ir[tail.op1]);
    if (tail.op1 == headref && head.o == IR_CARG &&
        head.op1 == REF_BASE && head.op2 >= v->nk &&
        head.op2 < REF_TRUE && ir_load_acq(&v->ir[head.op2]).o == IR_KINT)
      return 1;
  }
  return 0;
}
static int arm64_scalar_postra_register(Reg r, IRType type)
{
  return type == IRT_NUM
             ? r >= RID_MIN_FPR && r < RID_MAX_FPR && rset_test(RSET_FPR, r)
             : r < RID_MAX_GPR && rset_test(RSET_GPR, r);
}

/* Operand locations for the stock target's 32-bit instruction fusions. Every
** reference is bounded before a future consumer is inspected: its main-pass
** validation has not necessarily happened yet. */
static int arm64_scalar_postra_int_location(const LJArm64PostRAView *v,
                                            IRRef ref, IRRef before)
{
  IRIns p;
  if (!arm64_scalar_postra_ref(v, ref, before, IRT_INT)) return 0;
  if (irref_isk(ref)) return 1;
  p = ir_load_acq(&v->ir[ref]);
  return (ra_hasreg(p.r) && arm64_scalar_postra_register(p.r, IRT_INT)) ||
         ra_hasspill(p.s);
}

static int arm64_scalar_postra_kint(const LJArm64PostRAView *v, IRRef ref,
                                    int32_t *kp)
{
  IRIns p;
  if (!arm64_scalar_postra_kref(v, ref, IRT_INT)) return 0;
  p = ir_load_acq(&v->ir[ref]);
  if (p.o != IR_KINT) return 0;
  if (kp) *kp = p.i;
  return 1;
}

/* asm_fuseopm consumes at most one unmaterialized shift/double operation.
** Its input must have a real location; this is not recursive permission to
** erase an arbitrary chain. Match mayfuse's loop boundary at the consumer;
** unlike canfuse paths, an invariant shift cannot fuse into the loop body.
** A rotate is available only to logical opcodes. */
static int arm64_scalar_postra_fuseopm(const LJArm64PostRAView *v,
                                      IRRef ref, IRRef before, IRRef loop,
                                      int logical)
{
  IRIns p;
  if (ref < REF_FIRST || ref >= before || ref <= loop) return 0;
  p = ir_load_acq(&v->ir[ref]);
  if (p.t.irt != IRT_INT ||
      !arm64_scalar_postra_int_location(v, p.op1, ref)) return 0;
  if (p.o == IR_ADD && p.op1 == p.op2) return 1;
  return (p.o == IR_BSHL || p.o == IR_BSHR || p.o == IR_BSAR ||
          (logical && p.o == IR_BROR)) &&
         arm64_scalar_postra_kint(v, p.op2, NULL);
}

/* The target chooses the shifted operand before allocation, using only IR.
** Match that choice rather than accepting an absent left operand too. */
static int arm64_scalar_postra_swapops(const LJArm64PostRAView *v,
                                      IRRef left, IRRef right)
{
  IRIns p;
  if (irref_isk(right)) return 0;
  if (irref_isk(left)) return 1;
  p = ir_load_acq(&v->ir[right]);
  if ((p.o >= IR_BSHL && p.o <= IR_BROR) ||
      (p.o == IR_ADD && p.op1 == p.op2)) return 0;
  p = ir_load_acq(&v->ir[left]);
  return (p.o >= IR_BSHL && p.o <= IR_BROR) ||
         (p.o == IR_ADD && p.op1 == p.op2);
}

static int arm64_scalar_postra_int_fusion(const LJArm64PostRAView *v,
                                         IRRef value, IRRef consumer,
                                         IRRef loop)
{
  IRIns source = ir_load_acq(&v->ir[value]);
  IRIns use = ir_load_acq(&v->ir[consumer]);
  IRRef left = use.op1, right = use.op2;
  int32_t k, shift;
  int logical = use.o == IR_BAND || use.o == IR_BOR || use.o == IR_BXOR;
  int compare = use.o == IR_LT || use.o == IR_GE || use.o == IR_LE ||
                use.o == IR_GT || use.o == IR_EQ || use.o == IR_NE;
  if (source.t.irt != IRT_INT || irt_type(use.t) != IRT_INT) return 0;

  /* ORN/EON can in turn consume a shifted NOT input, exactly one level. */
  if (source.o == IR_BNOT && (use.o == IR_BOR || use.o == IR_BXOR) &&
      source.op2 == 0 && left != right &&
      arm64_scalar_postra_ref(v, left, consumer, IRT_INT) &&
      arm64_scalar_postra_ref(v, right, consumer, IRT_INT)) {
    IRRef notref = (!irref_isk(left) &&
      ir_load_acq(&v->ir[left]).o == IR_BNOT) ? left : right;
    IRRef other = value == left ? right : left;
    if (notref == value && !irref_isk(other) &&
        arm64_scalar_postra_int_location(v, other, consumer) &&
        (arm64_scalar_postra_int_location(v, source.op1, value) ||
         arm64_scalar_postra_fuseopm(v, source.op1, value, loop, 1))) return 1;
  }
  if (use.o == IR_BNOT && (use.t.irt & ~IRT_ISPHI) == IRT_INT &&
      use.op2 == 0 &&
      left == value && arm64_scalar_postra_fuseopm(v, value, consumer, loop, 1))
    return 1;

  /* UBFM/SBFM replace a left shift followed by a constant right shift. The
  ** outer shift needs a location: another instruction's shifted operand
  ** does not also encode this two-shift transformation. */
  if (source.o == IR_BSHL && (use.o == IR_BSHR || use.o == IR_BSAR) &&
      left == value && arm64_scalar_postra_kint(v, source.op2, NULL) &&
      arm64_scalar_postra_kint(v, right, NULL) &&
      arm64_scalar_postra_int_location(v, source.op1, value) &&
      (ra_hasreg(use.r) || ra_hasspill(use.s))) return 1;

  /* UBFM for BAND(constant-shift, low contiguous mask). */
  if (use.o == IR_BAND && left == value &&
      (source.o == IR_BSHL || source.o == IR_BSHR) &&
      arm64_scalar_postra_kint(v, source.op2, &shift) &&
      arm64_scalar_postra_kint(v, right, &k) &&
      arm64_scalar_postra_int_location(v, source.op1, value) &&
      (ra_hasreg(use.r) || ra_hasspill(use.s))) {
    uint64_t mask = (uint64_t)(int64_t)k;
    if (source.o == IR_BSHL) mask >>= (uint32_t)shift & 31u;
    if (mask && !((mask+1u) & mask)) return 1;
  }

  /* EXTR consumes two complementary constant shifts without either result. */
  if (use.o == IR_BOR && left != right &&
      left >= REF_FIRST && left < consumer &&
      right >= REF_FIRST && right < consumer) {
    IRIns a = ir_load_acq(&v->ir[left]), b = ir_load_acq(&v->ir[right]);
    if (a.t.irt == IRT_INT && b.t.irt == IRT_INT &&
        ((a.o == IR_BSHL && b.o == IR_BSHR) ||
         (a.o == IR_BSHR && b.o == IR_BSHL)) &&
        arm64_scalar_postra_kint(v, a.op2, &shift) &&
        arm64_scalar_postra_kint(v, b.op2, &k) &&
        (uint32_t)shift + (uint32_t)k == 32u &&
        arm64_scalar_postra_int_location(v, a.op1, left) &&
        arm64_scalar_postra_int_location(v, b.op1, right)) return 1;
  }

  /* Adjacent BAND-vs-zero is TST or TBZ/TBNZ. Both inputs are independently
  ** located, except the exact shifted operand accepted by TST. */
  if (source.o == IR_BAND && compare && consumer == value+1u &&
      use.t.irt == (IRT_INT|IRT_GUARD) && left != right &&
      (use.o == IR_EQ || use.o == IR_NE ||
       (left == value && (use.o == IR_LT || use.o == IR_GE)) ||
       (right == value && (use.o == IR_GT || use.o == IR_LE)))) {
    IRRef other = left == value ? right : left;
    if (arm64_scalar_postra_kint(v, other, &k) && k == 0 &&
        arm64_scalar_postra_ref(v, source.op1, value, IRT_INT) &&
        arm64_scalar_postra_ref(v, source.op2, value, IRT_INT)) {
      IRRef a = source.op1, b = source.op2;
      if (arm64_scalar_postra_swapops(v, a, b)) {
        IRRef tmp = a; a = b; b = tmp;
      }
      if (arm64_scalar_postra_int_location(v, a, value) &&
          (arm64_scalar_postra_int_location(v, b, value) ||
           arm64_scalar_postra_fuseopm(v, b, value, loop, 1))) return 1;
    }
  }

  /* Ordinary shifted-register arithmetic, logical operations and compares. */
  if (!(logical || compare || use.o == IR_ADD || use.o == IR_SUB ||
        use.o == IR_ADDOV || use.o == IR_SUBOV) || left == right ||
      !arm64_scalar_postra_ref(v, left, consumer, IRT_INT) ||
      !arm64_scalar_postra_ref(v, right, consumer, IRT_INT)) return 0;
  if (use.o != IR_SUB && use.o != IR_SUBOV &&
      arm64_scalar_postra_swapops(v, left, right)) {
    IRRef tmp = left; left = right; right = tmp;
  }
  return right == value &&
         arm64_scalar_postra_int_location(v, left, consumer) &&
         arm64_scalar_postra_fuseopm(v, value, consumer, loop, logical);
}

/* A missing location is legal only for an unused scalar or a proven target
** fusion at every use. Snapshot sources are checked separately after renames. */
static int arm64_scalar_postra_nolocation(const LJArm64PostRAView *v,
                                          IRRef value, IRRef end, IRRef loop)
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
    case IR_LOOP:
      loop = ref;
      continue;
    case IR_SLOAD:
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
    if (source.t.irt == IRT_INT &&
        arm64_scalar_postra_int_fusion(v, value, ref, loop))
      continue;
    if (value <= loop || source.o != IR_MUL || source.t.irt != IRT_NUM ||
        (use.o != IR_ADD && use.o != IR_SUB) || !irt_isnum(use.t) ||
        use.op1 == use.op2) {
      return 0;
    }
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
** In addition to the ABI graph, all scratch pointers must have distinct GPR
** locations. The general scalar pass proves every input and snapshot location. */
static int arm64_rooted_postra_effect(const LJArm64PostRAView *v, IRRef n,
                                     IRRef ref, IRRef root1, IRRef root2,
                                     IRRef root3)
{
  IRIns c = ir_load_acq(&v->ir[ref]), g, r1;
  IRRef sentinel;
  int tvread = c.op2 == IRCALL_lj_tab_gettv_rooted_hit_try;
  int nilread = c.op2 == IRCALL_lj_tab_getnil_nometa_rooted_try;
  int read = c.op2 == IRCALL_lj_tab_getscalar_rooted_try || tvread || nilread;
  int gcstore = c.op2 == IRCALL_lj_tab_storegc_rooted;
  int newstore = c.op2 == IRCALL_lj_tab_storenew_rooted;
  int rawstore = c.op2 == IRCALL_lj_tab_storeraw_rooted;
  int nilstore = c.op2 == IRCALL_lj_tab_storenil_rooted;
  int uvget = c.op2 == IRCALL_lj_func_getuv_rooted_try;
  int uvset = c.op2 == IRCALL_lj_func_storeuv_rooted_try;
  int store = c.op2 == IRCALL_lj_tab_storenum_rooted ||
              c.op2 == IRCALL_lj_tab_storebool_rooted || gcstore ||
              newstore || rawstore || nilstore;
  if (arm64_rooted_cell_call((IRCallID)c.op2)) {
    IRIns tail, head, guard, tmp;
    IRRef out;
    int get = c.op2 == IRCALL_lj_func_getcell_rooted_try;
    if (!root1 || root1 >= ref || root2 || root3 || ref+1u >= n ||
        c.t.irt != (IRT_INT|IRT_GUARD) ||
        !arm64_cell_root_postra(v, root1) || c.op1 < REF_FIRST ||
        c.op1 >= ref)
      return 0;
    tmp = ir_load_acq(&v->ir[root1]);
    tail = ir_load_acq(&v->ir[c.op1]);
    if (tail.o != IR_CARG || tail.t.irt != IRT_NIL ||
        tail.op1 < REF_FIRST || tail.op1 >= c.op1 || tail.op2 != root1)
      return 0;
    head = ir_load_acq(&v->ir[tail.op1]);
    if (head.o != IR_CARG || head.t.irt != IRT_NIL ||
        head.op1 != REF_BASE || head.op2 < v->nk || head.op2 >= REF_TRUE ||
        head.op2 >= tail.op1 || ir_load_acq(&v->ir[head.op2]).o != IR_KINT ||
        !arm64_scalar_postra_kref(v, head.op2, IRT_INT))
      return 0;
    if (tmp.op2 != (get ? (IRTMPREF_IN1|IRTMPREF_OUT1) : IRTMPREF_IN1) ||
        (get ? tmp.op1 != REF_NIL :
         !arm64_cell_value_postra_ref(v, tmp.op1, root1)))
      return 0;
    guard = ir_load_acq(&v->ir[ref+1u]);
    if (guard.o != IR_NE || guard.t.irt != (IRT_INT|IRT_GUARD) ||
        !((guard.op1 == ref && irref_isk(guard.op2)) ||
          (guard.op2 == ref && irref_isk(guard.op1))))
      return 0;
    out = guard.op1 == ref ? guard.op2 : guard.op1;
    if (!arm64_scalar_postra_kref(v, out, IRT_INT) ||
        ir_load_acq(&v->ir[out]).o != IR_KINT ||
        ir_load_acq(&v->ir[out]).i != 0)
      return 0;
    if (!get) return 5;
    if (ref+2u >= n) return 0;
    {
      IRIns value = ir_load_acq(&v->ir[ref+2u]);
      IRType type = irt_type(value.t);
      if (value.o != IR_VLOAD || value.op1 != root1 || value.op2 ||
          (type != IRT_NIL && type != IRT_INT && type != IRT_NUM &&
           type != IRT_TRUE && type != IRT_FALSE && type != IRT_LIGHTUD &&
           !arm64_rooted_gc_type(type)) || !irt_isguard(value.t) ||
          (value.t.irt & ~(IRT_TYPE|IRT_GUARD|IRT_ISPHI)))
        return 0;
    }
    return 4;
  }
  if (uvget || uvset) {
    IRIns tail, head, guard, r2;
    IRRef out;
    int get = uvget;
    if (!root1 || root1 >= ref || ref+1 >= n ||
        c.t.irt != (IRT_INT|IRT_GUARD) ||
        (get ? root2 != 0 || root3 != 0 : !root2 || root3 != 0))
      return 0;
    r1 = ir_load_acq(&v->ir[root1]);
    if (r1.o != IR_TMPREF || r1.t.irt != IRT_PGC ||
        r1.op2 != (get ? (IRTMPREF_IN1|IRTMPREF_OUT1) : IRTMPREF_IN1) ||
        r1.op1 < REF_FIRST || r1.op1 >= root1 ||
        !arm64_scalar_postra_ref(v, r1.op1, root1, IRT_FUNC) ||
        ir_load_acq(&v->ir[r1.op1]).o != IR_SLOAD ||
        ir_load_acq(&v->ir[r1.op1]).op1 != 0 ||
        ir_load_acq(&v->ir[r1.op1]).op2 != IRSLOAD_READONLY ||
        !ra_hasreg(r1.r) || !arm64_scalar_postra_register(r1.r, IRT_PGC) ||
        ra_hasspill(r1.s)) return 0;
    if (c.op1 < REF_FIRST || c.op1 >= ref) return 0;
    tail = ir_load_acq(&v->ir[c.op1]);
    if (tail.o != IR_CARG || tail.t.irt != IRT_NIL ||
        tail.op1 < REF_FIRST || tail.op1 >= c.op1 ||
        tail.op2 != (get ? root1 : root2)) return 0;
    head = ir_load_acq(&v->ir[tail.op1]);
    if (head.o != IR_CARG || head.t.irt != IRT_NIL ||
        head.op1 != root1 || root1 >= tail.op1 ||
        head.op2 < v->nk || head.op2 >= REF_TRUE ||
        ir_load_acq(&v->ir[head.op2]).o != IR_KINT ||
        ir_load_acq(&v->ir[head.op2]).t.irt != IRT_INT) return 0;
    if (!get) {
      if (root2 >= ref) return 0;
      r2 = ir_load_acq(&v->ir[root2]);
      if (root2 >= ref || r2.o != IR_TMPREF || r2.t.irt != IRT_PGC ||
          r2.op2 != IRTMPREF_IN2 || r1.r == r2.r || !ra_hasreg(r2.r) ||
          !arm64_scalar_postra_register(r2.r, IRT_PGC) || ra_hasspill(r2.s) ||
          (!arm64_scalar_postra_ref(v, r2.op1, root2, IRT_INT) &&
           !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_NUM) &&
           !arm64_scalar_postra_gc_ref(v, r2.op1, root2) &&
           !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_LIGHTUD) &&
           r2.op1 != REF_NIL &&
           !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_TRUE) &&
           !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_FALSE))) return 0;
    }
    guard = ir_load_acq(&v->ir[ref+1]);
    if (guard.o != IR_NE || guard.t.irt != (IRT_INT|IRT_GUARD) ||
        !((guard.op1 == ref && irref_isk(guard.op2)) ||
          (guard.op2 == ref && irref_isk(guard.op1)))) return 0;
    out = guard.op1 == ref ? guard.op2 : guard.op1;
    if (!arm64_scalar_postra_kref(v, out, IRT_INT) ||
        ir_load_acq(&v->ir[out]).o != IR_KINT ||
        ir_load_acq(&v->ir[out]).i != 0) return 0;
    if (get) {
      IRIns value;
      if (ref+2 >= n) return 0;
      value = ir_load_acq(&v->ir[ref+2]);
      if (value.o != IR_VLOAD || value.op1 != root1 || value.op2 ||
          (irt_type(value.t) != IRT_NIL && irt_type(value.t) != IRT_INT &&
           irt_type(value.t) != IRT_NUM && irt_type(value.t) != IRT_TRUE &&
           irt_type(value.t) != IRT_FALSE &&
           !arm64_rooted_gc_type(irt_type(value.t)) &&
           irt_type(value.t) != IRT_LIGHTUD) || !irt_isguard(value.t) ||
          (value.t.irt & ~(IRT_TYPE|IRT_GUARD|IRT_ISPHI))) return 0;
      return 4;
    }
    return 5;
  }
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
    if (!nilread && r2.op1 == REF_NIL) return 0;
    if (!tvread && !nilread &&
        !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_STR) &&
        !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_INT) &&
        !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_NUM) &&
        !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_FALSE) &&
        !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_TRUE)) return 0;
    value = ir_load_acq(&v->ir[ref+2]);
    if (value.o != IR_VLOAD || value.op1 != root1 || value.op2 ||
        (nilread ? value.t.irt != (IRT_NIL|IRT_GUARD) :
         (irt_type(value.t) != IRT_INT && irt_type(value.t) != IRT_NUM &&
         irt_type(value.t) != IRT_FALSE && irt_type(value.t) != IRT_TRUE &&
         !(tvread && (arm64_rooted_gc_type(irt_type(value.t)) ||
                      irt_type(value.t) == IRT_LIGHTUD)))) ||
        !irt_isguard(value.t) ||
        (value.t.irt & ~(IRT_TYPE|IRT_GUARD|IRT_ISPHI))) return 0;
  } else if (store) {
    IRIns outer, inner, r2;
    IRType type = c.op2 == IRCALL_lj_tab_storenum_rooted ? IRT_NUM : IRT_INT;
    if (!root2 || (nilstore && root3) || root2 >= ref ||
        c.op1 < REF_FIRST || c.op1 >= ref)
      return 0;
    outer = ir_load_acq(&v->ir[c.op1]);
    r2 = ir_load_acq(&v->ir[root2]);
    if (nilstore) {
      if (outer.o != IR_CARG || outer.t.irt != IRT_NIL ||
          outer.op1 != root1 || outer.op2 != root2 ||
          root1 >= c.op1 || root2 >= c.op1 || r2.op1 == REF_NIL ||
          r1.op2 != IRTMPREF_IN1 || r2.o != IR_TMPREF ||
          r2.t.irt != IRT_PGC || r2.op2 != IRTMPREF_IN2 ||
          !ra_hasreg(r2.r) || !arm64_scalar_postra_register(r2.r, IRT_PGC) ||
          ra_hasspill(r2.s) || r1.r == r2.r ||
          (!arm64_scalar_postra_ref(v, r2.op1, root2, IRT_STR) &&
           !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_INT) &&
           !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_NUM) &&
           !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_TRUE) &&
           !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_FALSE) &&
           !arm64_scalar_postra_gc_ref(v, r2.op1, root2) &&
           !arm64_scalar_postra_ref(v, r2.op1, root2, IRT_LIGHTUD)))
        return 0;
    } else {
      if (outer.o != IR_CARG || outer.t.irt != IRT_NIL ||
          outer.op1 < REF_FIRST || outer.op1 >= c.op1 ||
          ((gcstore || newstore || rawstore) ? (!root3 || root3 >= c.op1 || outer.op2 != root3) :
           !arm64_scalar_postra_ref(v, outer.op2, c.op1, type)))
        return 0;
      inner = ir_load_acq(&v->ir[outer.op1]);
      if (r2.op1 == REF_NIL || inner.o != IR_CARG || inner.t.irt != IRT_NIL ||
          inner.op1 != root1 || inner.op2 != root2 ||
          root1 >= outer.op1 || root2 >= outer.op1 ||
          r1.op2 != IRTMPREF_IN1 || r2.o != IR_TMPREF ||
        r2.t.irt != IRT_PGC || r2.op2 != IRTMPREF_IN2 ||
        !ra_hasreg(r2.r) || !arm64_scalar_postra_register(r2.r, IRT_PGC) ||
        ra_hasspill(r2.s) || r1.r == r2.r)
        return 0;
      if (gcstore || newstore || rawstore) {
      IRIns r3 = ir_load_acq(&v->ir[root3]);
      if (r3.o != IR_TMPREF || r3.t.irt != IRT_PGC ||
          r3.op2 != IRTMPREF_IN3 || !ra_hasreg(r3.r) ||
          !arm64_scalar_postra_register(r3.r, IRT_PGC) ||
          ra_hasspill(r3.s) || r3.r == r1.r || r3.r == r2.r ||
          !(arm64_scalar_postra_gc_ref(v, r3.op1, root3) ||
            arm64_scalar_postra_ref(v, r3.op1, root3, IRT_LIGHTUD) ||
            ((newstore || rawstore) && (arm64_scalar_postra_ref(v, r3.op1, root3, IRT_NUM) ||
                          arm64_scalar_postra_ref(v, r3.op1, root3, IRT_INT) ||
                          arm64_scalar_postra_ref(v, r3.op1, root3, IRT_TRUE) ||
                          arm64_scalar_postra_ref(v, r3.op1, root3, IRT_FALSE)))))
        return 0;
      }
    }
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

/* Independent allocated-view proof of rawset's replay and return boundary. */
static int arm64_rawset_postra_snapshots(const LJArm64PostRAView *v,
                                        IRRef ref, MSize s)
{
  IRIns effect = ir_load_acq(&v->ir[ref]), tail, head;
  const SnapShot *before = &v->snap[s], *after;
  const SnapEntry *map = &v->snapmap[snap_mapofs_acq(before)];
  const BCIns *pc = snap_pc_acq(&map[snap_nent_acq(before)]);
  BCIns ins = (BCIns)la_load32_acq((const uint32_t *)pc);
  IRRef actual[4] = { 0, 0, 0, 0 }, input[3], target, r;
  MSize n, base;
  GCobj *fn;
  int raw = effect.op2 == IRCALL_lj_tab_storeraw_rooted;
  if (bc_op(ins) != BC_CALL) return !raw;
  if ((!raw && effect.op2 != IRCALL_lj_tab_storenil_rooted) ||
      bc_c(ins) != 4 || bc_b(ins) < 1 || bc_b(ins) > 2 || s+1u >= v->nsnap)
    return 0;
  after = &v->snap[s+1u];
  if (snap_ref_acq(after) != ref+2u || snap_count_acq(after) != SNAPCOUNT_DONE ||
      snap_pc_acq(&v->snapmap[snap_mapofs_acq(after)+snap_nent_acq(after)]) != pc+1)
    return 0;
  base = 1u+LJ_FR2+bc_a(ins);
  for (n = 0; n < snap_nent_acq(before); n++) {
    SnapEntry se = snapentry_acq(&map[n]);
    MSize slot = snap_slot(se);
    if (slot == base) actual[0] = snap_ref(se);
    else if (slot > base+LJ_FR2 && slot <= base+LJ_FR2+3u)
      actual[slot-base-LJ_FR2] = snap_ref(se);
  }
  target = actual[0];
  if (target >= REF_FIRST) {
    target = 0;
    for (r = ref; r-- > REF_FIRST; ) {
      IRIns guard = ir_load_acq(&v->ir[r]);
      if (guard.o == IR_EQ && guard.t.irt == (IRT_FUNC|IRT_GUARD) &&
          guard.op1 == actual[0]) { target = guard.op2; break; }
    }
  }
  if (target >= REF_TRUE || !arm64_scalar_postra_kref(v, target, IRT_FUNC) ||
      ir_load_acq(&v->ir[target]).o != IR_KGC)
    return 0;
  fn = ir_kgc_load_acq(&v->ir[target]);
  if (!fn || !checkptrGC(fn) || la_load8_acq(&fn->gch.gct) != (uint32_t)~LJ_TFUNC ||
      lj_func_ffid_acq(gco2func(fn)) != FF_rawset)
    return 0;
  tail = ir_load_acq(&v->ir[effect.op1]);
  input[2] = raw ? ir_load_acq(&v->ir[tail.op2]).op1 : REF_NIL;
  head = raw ? ir_load_acq(&v->ir[tail.op1]) : tail;
  input[0] = ir_load_acq(&v->ir[head.op1]).op1;
  input[1] = ir_load_acq(&v->ir[head.op2]).op1;
  for (n = 0; n < 3; n++) {
    IRIns converted;
    if (actual[n+1u] == input[n]) continue;
    if (actual[n+1u] == 0 || input[n] < REF_FIRST) return 0;
    converted = ir_load_acq(&v->ir[input[n]]);
    if (converted.o != IR_CONV || converted.op1 != actual[n+1u] ||
        converted.op2 != IRCONV_NUM_INT ||
        (converted.t.irt & ~IRT_ISPHI) != IRT_NUM)
      return 0;
  }
  if (snap_nslots_acq(after) != base+bc_b(ins)-1u) return 0;
  if (bc_b(ins) == 2) {
    map = &v->snapmap[snap_mapofs_acq(after)];
    for (n = 0; n < snap_nent_acq(after); n++) {
      SnapEntry se = snapentry_acq(&map[n]);
      if (snap_slot(se) == base) return snap_ref(se) == input[0];
    }
    return 0;
  }
  return 1;
}

static int arm64_upvalue_postra_snapshots(const LJArm64PostRAView *v,
                                         IRRef ref, MSize s, IRRef root1,
                                         IRRef root2)
{
  IRIns call = ir_load_acq(&v->ir[ref]), tail, head;
  const SnapShot *pre = &v->snap[s], *post;
  const SnapEntry *map = &v->snapmap[snap_mapofs_acq(pre)];
  const BCIns *pc = snap_pc_acq(&map[snap_nent_acq(pre)]);
  BCIns bc = (BCIns)la_load32_acq((const uint32_t *)pc);
  int get = call.op2 == IRCALL_lj_func_getuv_rooted_try;
  uint32_t uv = get ? bc_d(bc) : bc_a(bc);
  IRRef value = 0;
  MSize n, slot;
  /* Bind the constant pool and upvalue count to the published/private owner
  ** of this exact bytecode view. Other scalar helpers do not consume these
  ** prototype fields and retain their existing view contract. */
  {
    const GCproto *pt = v->proto;
    GCtrace *owner = (GCtrace *)v->owner;
    if (!pt || !owner || proto_bc(pt) != v->proto_bc ||
        pt->sizebc != v->proto_sizebc || pt->numparams != v->proto_numparams ||
        pt->framesize != v->root_topslot || v->startpc == NULL ||
        trace_startptgco_acq(owner) != obj2gco((GCproto *)pt) ||
        trace_startins_acq(owner) != v->startins ||
        trace_startpc_acq(owner) != v->startpc ||
        (!v->owner_private && owner->root != 0))
      return 0;
  }
  {
    IRIns froot = ir_load_acq(&v->ir[root1]);
    IRIns source;
    if (froot.o != IR_TMPREF ||
        froot.op2 != (get ? (IRTMPREF_IN1|IRTMPREF_OUT1) : IRTMPREF_IN1) ||
        froot.op1 < REF_FIRST || froot.op1 >= root1)
      return 0;
    source = ir_load_acq(&v->ir[froot.op1]);
    if (source.o != IR_SLOAD || source.op1 != 0 ||
        source.t.irt != IRT_FUNC || source.op2 != IRSLOAD_READONLY)
      return 0;
  }
  if (!v->proto || uv >= v->proto->sizeuv ||
      (get ? bc_op(bc) != BC_UGET :
       bc_op(bc) != BC_USETV && bc_op(bc) != BC_USETS &&
       bc_op(bc) != BC_USETN && bc_op(bc) != BC_USETP) ||
      s+1u >= v->nsnap || snap_ref_acq(pre) != root1)
    return 0;
  if (call.op1 < REF_FIRST || call.op1 >= ref) return 0;
  tail = ir_load_acq(&v->ir[call.op1]);
  if (tail.o != IR_CARG || tail.t.irt != IRT_NIL ||
      tail.op1 < REF_FIRST || tail.op1 >= call.op1 ||
      tail.op2 != (get ? root1 : root2))
    return 0;
  head = ir_load_acq(&v->ir[tail.op1]);
  if (
      head.o != IR_CARG || head.op1 != root1 || root1 >= tail.op1 ||
      head.op2 < v->nk || head.op2 >= REF_TRUE ||
      ir_load_acq(&v->ir[head.op2]).o != IR_KINT ||
      ir_load_acq(&v->ir[head.op2]).t.irt != IRT_INT ||
      ir_load_acq(&v->ir[head.op2]).i != (int32_t)uv)
    return 0;
  post = &v->snap[s+1u];
  if (snap_ref_acq(post) != ref+2u ||
      snap_pc_acq(&v->snapmap[snap_mapofs_acq(post)+
                               snap_nent_acq(post)]) != (get ? pc : pc+1))
    return 0;
  if (get) return 1;
  if (snap_count_acq(post) != SNAPCOUNT_DONE || !root2) return 0;
  value = ir_load_acq(&v->ir[root2]).op1;
  if (bc_op(bc) == BC_USETV) {
    slot = 1u+LJ_FR2+bc_d(bc);
    for (n = 0; n < snap_nent_acq(pre); n++) {
      SnapEntry se = snapentry_acq(&map[n]);
      if (snap_slot(se) == slot) {
        IRRef saved = snap_ref(se);
        IRIns actual;
        if (value == saved) return 1;
        if (value < REF_FIRST || value >= ref) return 0;
        actual = ir_load_acq(&v->ir[value]);
        return actual.o == IR_CONV &&
               (actual.t.irt & ~IRT_ISPHI) == IRT_NUM &&
               actual.op2 == IRCONV_NUM_INT && actual.op1 == saved;
      }
    }
    if (value >= REF_FIRST && value < ref) {
      IRIns actual = ir_load_acq(&v->ir[value]);
      if (actual.o == IR_CONV &&
          (actual.t.irt & ~IRT_ISPHI) == IRT_NUM &&
          actual.op2 == IRCONV_NUM_INT)
        value = actual.op1;
    }
    if (value < REF_FIRST || value >= ref) return 0;
    {
      IRIns source = ir_load_acq(&v->ir[value]);
      return source.o == IR_SLOAD && source.op1 == slot &&
             source.op2 == IRSLOAD_TYPECHECK;
    }
  }
  if (bc_op(bc) == BC_USETP)
    return bc_d(bc) <= 2u && value == (IRRef)(REF_NIL-bc_d(bc));
  if (value < v->nk || value >= REF_TRUE) return 0;
  if (bc_op(bc) == BC_USETN) {
    IRIns k = ir_load_acq(&v->ir[value]);
    TValue expected;
    if (bc_d(bc) >= v->proto->sizekn) return 0;
    lj_tv_load_acq(&expected, proto_knumtv(v->proto, bc_d(bc)));
    if (tvisint(&expected))
      return k.o == IR_KINT && k.t.irt == IRT_INT && k.i == intV(&expected);
    if (k.o != IR_KNUM || k.t.irt != IRT_NUM) return 0;
    return tv_rawload(&expected) == tv_rawload(ir_knum(&v->ir[value]));
  } else {
    IRIns k = ir_load_acq(&v->ir[value]);
    ptrdiff_t idx = ~(ptrdiff_t)bc_d(bc);
    GCobj *expected, *actual;
    if (k.o != IR_KGC || idx >= 0 ||
        (uintptr_t)idx < ~(uintptr_t)proto_sizekgc_acq(v->proto)+1u)
      return 0;
    expected = proto_kgc_acq(v->proto, idx);
    actual = ir_kgc_load_acq(&v->ir[value]);
    return expected && expected->gch.gct == (uint32_t)~LJ_TSTR &&
           k.t.irt == IRT_STR && expected == actual;
  }
}

static int arm64_cell_rhs_postra(const LJArm64PostRAView *v,
                                 const SnapShot *pre,
                                 const SnapEntry *map, MSize slot,
                                 IRRef value, IRRef callref)
{
  MSize i;
  for (i = 0; i < snap_nent_acq(pre); i++) {
    SnapEntry se = snapentry_acq(&map[i]);
    if (snap_slot(se) == slot) {
      IRRef saved = snap_ref(se);
      if (value == saved) return 1;
      if (value >= REF_FIRST && value < callref) {
        IRIns conv = ir_load_acq(&v->ir[value]);
        return conv.o == IR_CONV &&
               (conv.t.irt & ~IRT_ISPHI) == IRT_NUM &&
               conv.op2 == IRCONV_NUM_INT && conv.op1 == saved;
      }
      return 0;
    }
  }
  if (value >= REF_FIRST && value < callref) {
    IRIns conv = ir_load_acq(&v->ir[value]);
    if (conv.o == IR_CONV &&
        (conv.t.irt & ~IRT_ISPHI) == IRT_NUM &&
        conv.op2 == IRCONV_NUM_INT)
      value = conv.op1;
  }
  if (value < REF_FIRST || value >= callref) return 0;
  {
    IRIns source = ir_load_acq(&v->ir[value]);
    return source.o == IR_SLOAD && source.op1 == slot &&
           source.op2 == IRSLOAD_TYPECHECK;
  }
}

static int arm64_cell_postra_snapshots(const LJArm64PostRAView *v,
                                       IRRef ref, MSize s, IRRef root1)
{
  IRIns call = ir_load_acq(&v->ir[ref]);
  const SnapShot *pre, *post;
  const SnapEntry *map;
  const BCIns *pc;
  BCIns bc;
  int get = call.op2 == IRCALL_lj_func_getcell_rooted_try;
  uint32_t local;
  MSize slot, count = 0;
  IRRef guardref;
  if (!v->proto || !v->snap || !v->snapmap || s >= v->nsnap ||
      !proto_cellops(v->proto) ||
      (bc_op(v->startins) != BC_LOOP && bc_op(v->startins) != BC_FORL) ||
      s+1u >= v->nsnap)
    return 0;
  pre = &v->snap[s];
  post = &v->snap[s+1u];
  {
    MSize ofs = snap_mapofs_acq(pre), nent = snap_nent_acq(pre);
    MSize postofs = snap_mapofs_acq(post), postnent = snap_nent_acq(post);
    if (ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
        v->nsnapmap-ofs-nent < 1u+LJ_FR2 ||
        postofs > v->nsnapmap || postnent > v->nsnapmap-postofs ||
        v->nsnapmap-postofs-postnent < 1u+LJ_FR2)
      return 0;
    map = &v->snapmap[ofs];
    pc = snap_pc_acq(&map[nent]);
  }
  bc = (BCIns)la_load32_acq((const uint32_t *)pc);
  if (snap_ref_acq(pre) != root1 ||
      (get ? bc_op(bc) != BC_CGET : bc_op(bc) != BC_CSET)) return 0;
  if ((get && bc_a(bc) >= v->proto->framesize) ||
      (!get && bc_d(bc) >= v->proto->framesize)) return 0;
  local = get ? bc_d(bc) : bc_a(bc);
  if (local >= v->proto->framesize) return 0;
  slot = 1u+LJ_FR2+local;
  if (!arm64_cell_guard_proto(v->proto, slot)) return 0;
  if (snap_ref_acq(post) != ref+2u ||
      snap_pc_acq(&v->snapmap[snap_mapofs_acq(post)+
                               snap_nent_acq(post)]) != (get ? pc : pc+1))
    return 0;
  if (!get && (snap_count_acq(post) != SNAPCOUNT_DONE ||
               bc_d(bc) >= v->proto->framesize ||
               !arm64_cell_rhs_postra(v, pre, map,
                 1u+LJ_FR2+bc_d(bc),
                 ir_load_acq(&v->ir[root1]).op1, ref)))
    return 0;
  for (guardref = REF_FIRST; guardref < ref; guardref++) {
    IRIns guard = ir_load_acq(&v->ir[guardref]);
    if (guard.o == IR_SLOAD && guard.op1 == slot &&
        guard.t.irt == (IRT_P32|IRT_GUARD) &&
        guard.op2 == IRSLOAD_TYPECHECK) {
      if (!arm64_cell_guard_postra(v, guardref)) return 0;
      count++;
    }
  }
  if (count != 1u) return 0;
  {
    IRRef tailref = call.op1;
    IRIns tail, head;
    if (tailref < REF_FIRST || tailref >= ref) return 0;
    tail = ir_load_acq(&v->ir[tailref]);
    if (tail.o != IR_CARG || tail.op1 < REF_FIRST ||
        tail.op1 >= tailref)
      return 0;
    head = ir_load_acq(&v->ir[tail.op1]);
    if (head.o != IR_CARG || head.op2 < v->nk || head.op2 >= REF_TRUE)
      return 0;
    IRRef kref = head.op2;
    return kref >= v->nk && kref < REF_TRUE &&
           ir_load_acq(&v->ir[kref]).o == IR_KINT &&
           ir_load_acq(&v->ir[kref]).i == (int32_t)local;
  }
}

/* Match snap_renameref for a bounded source at this exact exit. */
static RegSP arm64_scalar_postra_snapshot_regsp(const LJArm64PostRAView *v,
                                               IRRef n, IRRef ref, MSize s)
{
  IRRef ren;
  RegSP rs = ir_load_acq(&v->ir[ref]).prev;
  for (ren = v->nins; ren-- > n;) {
    IRIns q = ir_load_acq(&v->ir[ren]);
    if (q.o != IR_RENAME) break;
    if (q.op1 == ref && q.op2 <= s) rs = q.prev;
  }
  return rs;
}

static int arm64_scalar_postra_admit_mode(const LJArm64PostRAView *v,
                                         BCOp rootop,
                                         const LJArm64SideIRView *side,
                                         IRRef *ninsout)
{
  IRRef ref, n, loop = 0, poll = 0, firstphi = 0;
  IRRef root1 = 0, root2 = 0, root3 = 0, resultroot = 0, resultref = 0;
  MSize cap = 0, highest = 0, s, expectedofs = 0, continuation_pos = 0;
  IRRef prevsnap = 0, poll_snap = 0, completed_store = 0;
  unsigned nphi = 0, loopsnap = 0;
  MSize parent_sload = 0;
  SnapNo loopsnapno = 0;
  IRRef1 spillowner[SPS_LIMIT];
  int spillowners_initialized = 0, forl_dynamic_step = 0;
  uintptr_t lo, hi;
  if (!v || !v->ir || !v->snap || !v->snapmap || !v->proto_bc ||
      v->nins <= REF_FIRST || v->nins >= REF_DROP || !v->nk ||
      v->nk > REF_TRUE || !v->nsnap || !v->nsnapmap || !v->root_topslot ||
      v->root_topslot > UINT8_MAX || v->base_delta ||
      (!side && (bc_op(v->startins) != rootop ||
       (rootop != BC_LOOP && rootop != BC_FORL))) || !v->proto_sizebc)
    return 0;
  lo = (uintptr_t)v->proto_bc;
  if ((lo & 3u) || v->proto_sizebc > (UINTPTR_MAX - lo) / sizeof(BCIns))
    return 0;
  hi = lo + v->proto_sizebc * sizeof(BCIns);
  if (side) {
	const GCtrace *owner = v->owner;
	MSize header = side->loop_header_pc, end = side->loop_end_pc;
	int64_t target;
	BCIns h, back;
	MSize cont;

	if (!owner || v->proto_bc != side->proto_bc ||
	    owner->traceno != side->traceno || owner->root != side->parent ||
	    !arm64_side_scalar_topology(side, owner) || owner->sinktags ||
	    owner->startins != BCINS_AD(BC_JMP, 0, 0) ||
	    trace_startpc_acq((GCtrace *)owner) != side->parent_continuation ||
	    side->parent == 0 ||
	    side->root != side->parent ||
	  v->root_topslot != side->root_topslot ||
	  !side->parent_snap ||
	    side->loop_start_pc >= end || end >= side->proto_sizebc ||
	    header >= end)
	  return 0;
	h = (BCIns)la_load32_acq((const uint32_t *)&side->proto_bc[header]);
	back = (BCIns)la_load32_acq((const uint32_t *)&side->proto_bc[end]);
	target = (int64_t)end + 1 + (int64_t)bc_j(back);
	if (bc_op(h) != BC_JLOOP || bc_d(h) != side->parent ||
	    bc_op(back) != BC_JMP || bc_j(back) >= 0 || target < 0 ||
	    (MSize)target != side->loop_start_pc ||
	    !arm64_ir_pcpos((uintptr_t)(const void *)side->parent_continuation,
	      lo, hi, &cont) || cont < (MSize)target || cont >= end)
	  return 0;
	continuation_pos = cont;

  }
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    IRIns p = ir_load_acq(&v->ir[ref]);
    if (p.o != IR_KPRI || p.t.irt != REF_NIL - ref || p.op12)
      return 0;
  }
  for (ref = v->nk; ref < REF_TRUE; ref++) {
    IRIns k = ir_load_acq(&v->ir[ref]);
    if (k.o == IR_KINT && k.t.irt == IRT_INT)
      continue;
    if (k.o == IR_KGC && arm64_rooted_gc_type((IRType)k.t.irt) && k.op12 == 0 &&
        (!side || k.t.irt == IRT_STR) &&
        ref + LJ_GC64 < REF_TRUE) {
      GCobj *o = ir_kgc_load_acq(&v->ir[ref]);
      if (o == NULL || !checkptrGC(o) ||
          la_load8_acq(&o->gch.gct) != k.t.irt)
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
        !arm64_postra_forl_dynamic_shape(v, n, &forl_dynamic_step, 1))
      return 0;
  }
  {
    IRIns base = ir_load_acq(&v->ir[REF_BASE]);
    if (base.o != IR_BASE || base.t.irt != IRT_PGC ||
	(side ? (base.op1 != side->parent || base.op2 != side->exitno) :
	      base.op12 != 0) ||
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
    /* Independently keep child allocation proof within scalar effects. */
    if (side) {
      if (type != IRT_INT && type != IRT_NUM && type != IRT_NIL)
        return 0;
      switch ((IROp)p.o) {
      case IR_NOP: case IR_SLOAD: case IR_ADDOV: case IR_SUBOV: case IR_MULOV:
      case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_CONV:
      case IR_LT: case IR_GE: case IR_LE: case IR_GT: case IR_EQ: case IR_NE:
      case IR_LOOP: case IR_XPOLL: case IR_PHI: case IR_USE:
        break;
      default:
        return 0;
      }
    }
    switch ((IROp)p.o) {
    case IR_NOP:
      if (p.t.irt != IRT_NIL || p.op12 || p.r != RID_INIT ||
	  p.s != SPS_NONE)
	return 0;
      break;
    case IR_SLOAD:
      if (side && p.op2 == (IRSLOAD_PARENT|IRSLOAD_INHERIT)) {
	MSize ord = parent_sload++;
	if (!arm64_side_parent_sload_valid(side, &p, ord)) {

	  return 0;
	}
	break;
      }
      if (!side && p.t.irt == (IRT_P32|IRT_GUARD) &&
          p.op2 == IRSLOAD_TYPECHECK &&
          arm64_cell_guard_proto(v->proto, p.op1) &&
          arm64_cell_guard_postra(v, ref))
	break;
      if (!side && type == IRT_FUNC && flags == 0 && p.op1 == 0 &&
          p.op2 == IRSLOAD_READONLY && p.t.irt == IRT_FUNC)
        break;
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
      if (arm64_rooted_gc_type(type) || type == IRT_LIGHTUD ||
          type == IRT_FALSE || type == IRT_TRUE || (!side && type == IRT_NIL)) {
        if ((p.t.irt & ~((arm64_rooted_gc_type(type) || type == IRT_LIGHTUD) ?
                         IRT_ISPHI : 0)) != (type|IRT_GUARD) ||
            p.op2 != IRSLOAD_TYPECHECK || p.op1 < 1 + LJ_FR2 ||
            p.op1 >= v->root_topslot + 1 + LJ_FR2)
          return 0;
        if (type == IRT_NIL && (p.r != RID_INIT || p.s != SPS_NONE))
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
        if (!arm64_scalar_postra_ref(v, p.op1, ref, IRT_TAB) &&
            !(p.op2 == IRTMPREF_IN1 && p.op1 >= REF_FIRST &&
              arm64_scalar_postra_ref(v, p.op1, ref, IRT_FUNC) &&
              ir_load_acq(&v->ir[p.op1]).o == IR_SLOAD &&
              ir_load_acq(&v->ir[p.op1]).op1 == 0 &&
              ir_load_acq(&v->ir[p.op1]).op2 == IRSLOAD_READONLY) &&
            !(p.op2 == (IRTMPREF_IN1|IRTMPREF_OUT1) &&
              p.op1 >= REF_FIRST &&
              arm64_scalar_postra_ref(v, p.op1, ref, IRT_FUNC) &&
              ir_load_acq(&v->ir[p.op1]).o == IR_SLOAD &&
              ir_load_acq(&v->ir[p.op1]).op1 == 0 &&
              ir_load_acq(&v->ir[p.op1]).op2 == IRSLOAD_READONLY) &&
            !arm64_cell_root_postra(v, ref)) return 0;
        root1 = ref;
      } else if (p.op2 == IRTMPREF_IN2) {
        if (!arm64_scalar_postra_ref(v, p.op1, ref, IRT_INT) &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_NUM) &&
            !arm64_scalar_postra_gc_ref(v, p.op1, ref) &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_LIGHTUD) &&
            p.op1 != REF_NIL &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_FALSE) &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_TRUE)) return 0;
        root2 = ref;
      } else if (p.op2 == IRTMPREF_IN3) {
        if (!arm64_scalar_postra_gc_ref(v, p.op1, ref) &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_LIGHTUD) &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_NUM) &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_INT) &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_TRUE) &&
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_FALSE)) return 0;
        root3 = ref;
      } else return 0;
      break;
    case IR_CALLS:
      {
        if (side && (arm64_rooted_upvalue_call((IRCallID)p.op2) ||
                     arm64_rooted_cell_call((IRCallID)p.op2))) return 0;
        int effect = arm64_rooted_postra_effect(v, n, ref, root1, root2, root3);
        if (!effect || !ra_hasreg(p.r) ||
            !arm64_scalar_postra_register(p.r, IRT_INT)) return 0;
        if (effect == 2 || effect == 4) {
          resultroot = root1; resultref = ref+2;
        }
        if (effect == 5) completed_store = ref;
        root1 = root2 = root3 = 0;
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
      if (type == IRT_INT && ((!side && !(flags & ~IRT_ISPHI)) ||
          (rootop == BC_FORL && flags == IRT_ISPHI))) {
        binary = 1;
        break;
      }
      if (type != IRT_NUM || (flags & ~IRT_ISPHI))
        return 0;
      binary = 1;
      break;
    case IR_SUB:
      if (!side && type == IRT_INT && !(flags & ~IRT_ISPHI)) {
        binary = 1;
        break;
      }
      /* fallthrough */
    case IR_MUL:
    case IR_DIV:
      if (type != IRT_NUM || (flags & ~IRT_ISPHI))
        return 0;
      binary = 1;
      break;
    case IR_BNOT: case IR_BSWAP:
      if (type != IRT_INT || (flags & ~IRT_ISPHI) || p.op2 ||
          !arm64_scalar_postra_ref(v, p.op1, ref, IRT_INT))
        return 0;
      break;
    case IR_NEG:
      if (type != IRT_INT || (flags & ~IRT_ISPHI) || p.op2 != p.op1 ||
          !arm64_scalar_postra_ref(v, p.op1, ref, IRT_INT))
        return 0;
      break;
    case IR_BAND: case IR_BOR: case IR_BXOR:
    case IR_BSHL: case IR_BSHR: case IR_BSAR: case IR_BROR:
      if (type != IRT_INT || (flags & ~IRT_ISPHI))
        return 0;
      binary = 1;
      break;
    case IR_TOBIT:
      if (type != IRT_INT || (flags & ~IRT_ISPHI) ||
          !arm64_scalar_postra_ref(v, p.op1, ref, IRT_NUM) ||
          !arm64_scalar_postra_kref(v, p.op2, IRT_NUM) ||
          ir_load_acq(&v->ir[p.op2]).o != IR_KNUM ||
          ir_load_acq(&v->ir[p.op2+1u]).tv.u64 != U64x(43380000,00000000))
        return 0;
      break;
    case IR_LT:
    case IR_GE:
    case IR_LE:
    case IR_GT:
    case IR_EQ:
    case IR_NE:
      if (type == IRT_FUNC && p.o == IR_EQ && flags == IRT_GUARD) {
        IRIns k;
        GCobj *fn;
        /* Recheck function provenance independently after allocation. */
        if (p.op1 < REF_FIRST ||
            !arm64_scalar_postra_ref(v, p.op1, ref, IRT_FUNC) ||
            p.op2 >= REF_TRUE ||
            !arm64_scalar_postra_kref(v, p.op2, IRT_FUNC))
          return 0;
        k = ir_load_acq(&v->ir[p.op2]);
        if (k.o != IR_KGC) return 0;
        fn = ir_kgc_load_acq(&v->ir[p.op2]);
        if (fn == NULL || !checkptrGC(fn) ||
            la_load8_acq(&fn->gch.gct) != (uint32_t)~LJ_TFUNC ||
            !arm64_scalar_builtin_ffid(lj_func_ffid_acq(gco2func(fn))))
          return 0;
        break;
      }
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
      if (p.t.irt != IRT_NIL || p.op1 >= ref) return 0;
      if (p.op1 == REF_BASE && arm64_cell_head_carg_postra(v, ref)) break;
      if (p.op1 < REF_FIRST) return 0;
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
           type != IRT_FALSE && type != IRT_TRUE && type != IRT_NIL &&
           !arm64_rooted_gc_type(type) && type != IRT_LIGHTUD) ||
          !(flags & IRT_GUARD) ||
          (flags & ~(IRT_GUARD|IRT_ISPHI))) return 0;
      if (type == IRT_NIL && (p.r != RID_INIT || p.s != SPS_NONE)) return 0;
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
	  if (poll || (side ?
	      (side->linktype == LJ_TRLINK_ROOT ? loop != 0 :
	       (!loop || ref != loop + 1)) : (!loop || ref != loop + 1)) ||
          p.t.irt != (IRT_NIL | IRT_GUARD) || p.op1 != 1 || p.op2)
        return 0;
      poll = ref;
      break;
    case IR_PHI:
      if (!firstphi)
        firstphi = ref;
      if (++nphi > LJ_MAX_PHI || !poll || flags ||
          (type != IRT_INT && type != IRT_NUM &&
           !arm64_rooted_gc_type(type) && type != IRT_LIGHTUD) ||
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
        /* Preserve the complete 64-bit NUM, GC pointer or lightuserdata token.
        ** GC pointers additionally rely on the native phase lease until exit
        ** restoration; lightuserdata tokens carry no GC edge. */
        if (!value || (type != IRT_INT && type != IRT_NUM &&
                       !arm64_rooted_gc_type(type) && type != IRT_LIGHTUD) ||
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
          if (!ra_hasspill(p.s) &&
              !arm64_scalar_postra_nolocation(v, ref, n, loop))
            return 0;
        }
      }
    }
  }

  if (side && parent_sload != side->parent_nregspmap)
	return 0;
  if (side ?
	(side->linktype == LJ_TRLINK_ROOT ? (loop != 0 || poll == 0) :
	 (!loop || poll != loop + 1)) : (!loop || poll != loop + 1))
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
    if ((!side || side->linktype == LJ_TRLINK_LOOP) && at == loop) {
      loopsnap = 1;
      loopsnapno = (SnapNo)s;
    }
    if (at <= poll)
      poll_snap = at;
    for (j = 0; j < nent; j++) {
      SnapEntry sn = snapentry_acq(&v->snapmap[ofs + j]);
      IRRef r = snap_ref(sn);
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
           !arm64_scalar_postra_gc_ref(v, r, at) &&
           !arm64_scalar_postra_ref(v, r, at, IRT_LIGHTUD) &&
           (side || r != REF_NIL) &&
           !arm64_scalar_postra_ref(v, r, at, IRT_FALSE) &&
           !arm64_scalar_postra_ref(v, r, at, IRT_TRUE)))
        return 0;
      if (irref_isk(r))
        continue;
      p = ir_load_acq(&v->ir[r]);
      rs = arm64_scalar_postra_snapshot_regsp(v, n, r, s);
      /* asm_snap_alloc1 can leave a snapshot-only NUM<-INT conversion
      ** unmaterialized. snap_restoreval reconstructs it from the integer at
      ** this exit. Certify that exact one-level source and its own renames;
      ** an arbitrary missing snapshot location remains a refusal. */
      if (!side && !ra_hasspill(regsp_spill(rs)) &&
          ra_noreg(regsp_reg(rs)) && p.o == IR_CONV &&
          p.t.irt == IRT_NUM && p.op2 == IRCONV_NUM_INT) {
        if (!arm64_scalar_postra_ref(v, p.op1, r, IRT_INT)) return 0;
        r = p.op1;
        if (irref_isk(r)) continue;
        p = ir_load_acq(&v->ir[r]);
        rs = arm64_scalar_postra_snapshot_regsp(v, n, r, s);
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
	if (side && ((s == 0 && pos != continuation_pos) ||
		(pos < side->loop_start_pc ||
		 (pos >= side->loop_end_pc &&
		  !(s != 0 && pos == side->loop_end_pc+1u)))))
	  return 0;
  }
  if (side ?
	(side->linktype == LJ_TRLINK_ROOT ?
	 (loopsnap || poll_snap != poll) :
	 (!loopsnap || poll_snap != loop)) : (!loopsnap || poll_snap != loop))
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
         ir.op2 == IRCALL_lj_tab_storebool_rooted ||
         ir.op2 == IRCALL_lj_tab_storegc_rooted ||
         ir.op2 == IRCALL_lj_tab_storenew_rooted ||
         ir.op2 == IRCALL_lj_tab_storeraw_rooted ||
         ir.op2 == IRCALL_lj_tab_storenil_rooted)) {
      if (!arm64_rawset_postra_snapshots(v, ref, s)) return 0;
      completed_store = ref;
    }
    if (ir.o == IR_CALLS && arm64_rooted_upvalue_call((IRCallID)ir.op2)) {
      IRIns tail = ir_load_acq(&v->ir[ir.op1]);
      IRIns head = ir_load_acq(&v->ir[tail.op1]);
      int get = ir.op2 == IRCALL_lj_func_getuv_rooted_try;
      if (!arm64_upvalue_postra_snapshots(v, ref, s, head.op1,
                                          get ? 0 : tail.op2))
        return 0;
      if (!get) completed_store = ref;
    }
    if (ir.o == IR_CALLS && arm64_rooted_cell_call((IRCallID)ir.op2)) {
      IRIns tail = ir_load_acq(&v->ir[ir.op1]);
      int get = ir.op2 == IRCALL_lj_func_getcell_rooted_try;
      if (!arm64_cell_postra_snapshots(v, ref, s, tail.op2))
        return 0;
      if (!get) completed_store = ref;
    }
    if (rootop != BC_FORL && ir.o == IR_SLOAD &&
        ir.op1 >= snap_nslots_acq(&v->snap[s]))
      return 0;
  }
  if (ninsout)
    *ninsout = n;
  return 1;
}
static int arm64_side_parent_snapshot_valid(const LJArm64SideIRView *side)
{
  uint64_t pcbase;
  SnapEntry footer[1+LJ_FR2];
  MSize i, nent, extent;
  if (!side || !side->parent_snap || !side->parent_snapmap ||
	  !side->parent_nins || !side->parent_nk ||
	  side->parent_nins >= REF_DROP || side->parent_nk > REF_TRUE ||
	  !side->parent_nsnap || side->parent_nsnap >= REF_DROP ||
	  side->exitno >= side->parent_nsnap ||
	  side->parent_nregspmap > LJ_MAX_JSLOTS ||
	  (side->parent_nregspmap && !side->parent_regspmap) ||
	  (side->parent_nregspmap && !side->parent_bindings) ||
	  snap_mapofs_acq(side->parent_snap) != 0 ||
	  snap_nslots_acq(side->parent_snap) != side->parent_nslots ||
	  snap_topslot_acq(side->parent_snap) != side->parent_topslot ||
	  side->parent_topslot != side->root_topslot ||
	  snap_nslots_acq(side->parent_snap) < 1u+LJ_FR2 ||
	  snap_topslot_acq(side->parent_snap) > snap_nslots_acq(side->parent_snap))
    return 0;
  nent = snap_nent_acq(side->parent_snap);
  if (nent > LJ_MAX_JSLOTS || nent > side->parent_nslots ||
	  nent > (MSize)-1-(1u+LJ_FR2))
	return 0;
  extent = nent+1u+LJ_FR2;
  if (side->parent_nsnapmap != extent)
	return 0;
  for (i = 0; i < nent; i++) {
	SnapEntry sn = snapentry_acq(&side->parent_snapmap[i]);
	IRRef ref = snap_ref(sn);
	if (snap_slot(sn) >= side->parent_nslots ||
	    (i && snap_slot(sn) <=
	       snap_slot(snapentry_acq(&side->parent_snapmap[i-1]))) ||
	    (sn != SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL) &&
	     (sn & 0x00ff0000u)))
	  return 0;
	if (sn == SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL))
	  continue;
	if (!arm64_side_parent_ref_valid(side, ref))
	  return 0;
	}
	if (side->parent_snap->ref < REF_FIRST ||
	    side->parent_snap->ref >= side->parent_nins)
	  return 0;
	for (i = 0; i < side->parent_nregspmap; i++) {
	const LJArm64SideParentBinding *binding = &side->parent_bindings[i];
	RegSP rs = (RegSP)side->parent_regspmap[i];
	MSize j;
	if (binding->regsp != rs || binding->slot >= side->parent_nslots ||
	    !regsp_used(rs) || ra_hasspill(regsp_spill(rs)) ||
	    (irt_type(binding->value.t) != IRT_INT &&
	     irt_type(binding->value.t) != IRT_NUM) ||
	    !arm64_scalar_postra_register(regsp_reg(rs),
					 (IRType)irt_type(binding->value.t)))
	  return 0;
	for (j = 0; j < nent; j++)
	  if (snap_slot(snapentry_acq(&side->parent_snapmap[j])) == binding->slot &&
	      snap_ref(snapentry_acq(&side->parent_snapmap[j])) == binding->ref)
	    break;
	if (j == nent || !arm64_side_parent_ref_valid(side, binding->ref))
	  return 0;
	}
  for (i = 0; i < 1u+LJ_FR2; i++)
	footer[i] = snapentry_acq(&side->parent_snapmap[nent+i]);
  memcpy(&pcbase, footer, sizeof(pcbase));
  return (uint8_t)pcbase == 0 &&
	(uintptr_t)(pcbase >> 8) ==
	  (uintptr_t)(const void *)side->parent_continuation;
}

static int arm64_side_scalar_topology(const LJArm64SideIRView *side,
	const GCtrace *owner)
{
	MSize continuation_pos = 0;
	uintptr_t lo;
	if (!side || !owner)
	  return 0;
	if (side->link == side->parent && side->linktype == LJ_TRLINK_ROOT &&
	    owner->link == side->parent && owner->linktype == LJ_TRLINK_ROOT &&
	    owner->mcloop == 0)
	  return 1;
	lo = (uintptr_t)(const void *)side->proto_bc;
	if (side->link != side->traceno || side->linktype != LJ_TRLINK_LOOP ||
	    owner->link != side->traceno || owner->linktype != LJ_TRLINK_LOOP || !lo ||
	    side->proto_sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns))
	  return 0;
	return arm64_scalar_side_geometry_ir(side, lo,
	    lo+side->proto_sizebc*sizeof(BCIns), &continuation_pos);
}

static int arm64_side_scalar_context_ir(const jit_State *J,
	const GCtrace *T, const LJArm64SideIRView *side)
{
	if (!J || !T || !side || !arm64_side_scalar_topology(side, T) ||
	    (side->linktype == LJ_TRLINK_LOOP &&
	     (J->pc != J->startpc || J->pc != side->parent_continuation)))
	  return 0;
	return J->parent == side->parent &&
	J->exitno == side->exitno &&
	J->pt && side->traceno == T->traceno && side->parent != 0 &&
	side->parent != side->traceno && side->root == side->parent &&
	T->root == side->parent &&
	T->startins == BCINS_AD(BC_JMP, 0, 0) &&
	trace_startptgco_acq((GCtrace *)T) == obj2gco(J->pt) &&
	trace_startpc_acq((GCtrace *)T) == side->parent_continuation &&
	J->startpc == side->parent_continuation &&
	J->baseslot == 1u+LJ_FR2 && !J->framedepth && !J->retdepth &&
	side->proto_bc == proto_bc(J->pt) &&
	side->proto_sizebc == J->pt->sizebc &&
	side->ir == T->ir && side->snap == T->snap &&
	side->snapmap == T->snapmap && side->nins == T->nins &&
	side->nk == T->nk && side->nsnap == T->nsnap &&
	side->nsnapmap == T->nsnapmap &&
	arm64_side_parent_snapshot_valid(side);
}

static int arm64_side_scalar_context_postra(const LJArm64PostRAView *v,
	const LJArm64SideIRView *side)
{
  const GCtrace *owner;
  if (!v || !side || !(owner = v->owner) ||
	  owner->traceno != side->traceno || owner->root != side->parent ||
	  owner->sinktags || owner->startins != BCINS_AD(BC_JMP, 0, 0) ||
	  trace_startpc_acq((GCtrace *)owner) != side->parent_continuation ||
	  v->proto_bc != side->proto_bc || v->proto_sizebc != side->proto_sizebc ||
	  v->root_topslot != side->root_topslot ||
	  v->snap != side->snap || v->snapmap != side->snapmap ||
	  v->nsnap != side->nsnap || v->nsnapmap != side->nsnapmap ||
	  !arm64_side_parent_snapshot_valid(side) ||
	  !arm64_side_scalar_topology(side, owner))
    return 0;
  return 1;
}

int lj_asm_arm64_side_scalar_ir_admit(const jit_State *J, const GCtrace *T,
	const LJArm64SideIRView *side, LJArm64IRReject *reject)
{
	if (!arm64_side_scalar_context_ir(J, T, side)) {
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE, REF_BASE,
			       IR_LOOP, 0);
	}
  return arm64_scalar_ir_admit_mode(J, T, BC_LOOP, side, reject);
}

int lj_asm_arm64_side_scalar_postra_admit(const LJArm64PostRAView *view,
	const LJArm64SideIRView *side, IRRef *semantic_ninsp)
{
  if (!arm64_side_scalar_context_postra(view, side))
	return 0;
  return arm64_scalar_postra_admit_mode(view, BC_LOOP, side, semantic_ninsp);
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
    return arm64_scalar_ir_admit_mode(J, T, BC_FORL, NULL, reject);
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
    return arm64_scalar_postra_admit_mode(view, BC_LOOP, NULL, semantic_ninsp);
  }
  if (view && bc_op(view->startins) == BC_FORL)
    return arm64_scalar_postra_admit_mode(view, BC_FORL, NULL, semantic_ninsp);
  return arm64_postra_nonloop_admit(view, semantic_ninsp);
}

#endif
