/*
** Fail-closed ARM64 trace admission and post-allocation certification.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_ASM_ARM64_ADMIT_H
#define _LJ_ASM_ARM64_ADMIT_H

static int arm64_scalar_postra_ref(const LJArm64PostRAView *v, IRRef ref,
                                   IRRef before, IRType type);

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
static int arm64_ir_funcf_bytecode(const GCproto *pt, BCIns startins,
	BCIns liveins, int prepublication)
{
  const BCIns *bc = pt ? proto_bc(pt) : NULL;
  MSize sizebc = pt ? pt->sizebc : 0;
  MSize framesize = pt ? pt->framesize : 0;
  MSize numparams = pt ? pt->numparams : 0;
  uintptr_t lo = (uintptr_t)bc;
  BCIns word, ret;
  BCIns generation[UINT8_MAX+1u];
  uint8_t initialized[UINT8_MAX+1u] = { 0 };
  uint8_t numeric[UINT8_MAX+1u] = { 0 };
  uint8_t function[UINT8_MAX+1u] = { 0 };
  MSize calls = 0;
  MSize pos, gets = 0;
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
    if (op == BC_CALLT) {
      BCReg arg = (BCReg)(a+1u+LJ_FR2);
      /* A bounded tail root has no post-call RET instruction: its one
      ** returned value resumes into the caller's result slot. The complete
      ** tail source and result map are checked by the independent replay. */
      if (pos+1u != sizebc || calls != 0u || gets != 1u || b != 0u ||
          c != 2u || a >= framesize || arg >= framesize ||
          !initialized[a] || !function[a] || !initialized[arg] ||
          numeric[arg] == 2)
        return 0;
      initialized[a] = numeric[a] = 1;
      function[a] = 0;
      ret = word;
      goto check_generation;
    }
    if (op == BC_RET1 || op == BC_RET) {
      MSize nresults = op == BC_RET1 && bc_d(word) == 2u ? 1u :
                       op == BC_RET && bc_d(word) == 3u ? 2u : 0u;
      MSize result;
      if (pos+1u != sizebc || !nresults || a+nresults > framesize ||
	  gets != calls)
	return 0;
      for (result = a; result < a+nresults; result++)
	if (!initialized[result] ||
	    (nresults == 2u && (numeric[result] == 0 || numeric[result] == 2)))
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
      function[a] = function[src];
    } else if (op == BC_UGET) {
      /* A rooted upvalue read can feed one fixed, one-result Lua leaf call.
      ** The immutable semantic and post-RA walkers later bind this function
      ** to the exact guarded closure and validate its complete frame map. */
      if (gets++ != 0u || a >= framesize || bc_d(word) >= pt->sizeuv || calls ||
          function[a])
        return 0;
      initialized[a] = function[a] = 1;
    } else if (op == BC_ADDVV || op == BC_SUBVV || op == BC_MULVV ||
	       op == BC_DIVVV) {
      if (a >= framesize || b >= framesize || c >= framesize ||
	  !initialized[b] || !initialized[c] || numeric[b] == 2 ||
	  numeric[c] == 2)
	return 0;
      initialized[a] = numeric[a] = 1;
    } else if (op == BC_ADDVN || op == BC_SUBVN || op == BC_MULVN ||
	       op == BC_DIVVN || op == BC_ADDNV || op == BC_SUBNV ||
	       op == BC_MULNV || op == BC_DIVNV) {
      TValue k;
      /* Both VN and NV encode the variable in B and the KNUM in C.  NV
      ** changes the semantic operand order, not the bytecode field layout. */
      BCReg source = b;
      MSize kidx = c;
      if (a >= framesize || source >= framesize || !initialized[source] ||
	  numeric[source] == 2 || kidx >= pt->sizekn)
	return 0;
      proto_knumtv_load_acq(&k, pt, kidx);
      if (!tvisnumber(&k)) return 0;
      initialized[a] = numeric[a] = 1;
    } else if (op == BC_CALL) {
      BCReg arg = (BCReg)(a+1u+LJ_FR2);
      /* One supplied fixed argument and one or two fixed results from a
      ** callee loaded by the earlier UGET. The detailed child arity is
      ** independently bound by the recorder and semantic/post-RA replay. */
      if (calls++ != 0u || (bc_b(word) != 2u && bc_b(word) != 3u) ||
          bc_c(word) != 2u ||
          a >= framesize || arg >= framesize || !initialized[a] ||
          !function[a] || !initialized[arg] || numeric[arg] == 2 ||
          (bc_b(word) == 3u && a+1u >= framesize))
        return 0;
      initialized[a] = numeric[a] = 1;
      if (bc_b(word) == 3u) initialized[a+1u] = numeric[a+1u] = 1;
      function[a] = 0;
    } else {
      /* Branches, heap access and all other effects are outside this scalar
      ** root certificate. */
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

static int arm64_ir_funcf_pair_call_leaf_source(const GCproto *pt,
						MSize *retposp);

int lj_asm_arm64_funcf_bytecode(const GCproto *pt, BCIns startins,
	BCIns liveins, int prepublication)
{
  return arm64_ir_funcf_bytecode(pt, startins, liveins, prepublication);
}

/* Decode one exact JMP ... ITERC ITERL source generation. This only proves
** bytecode geometry; it gives no authority to record, publish or enter native
** iterator code without the separate frame, IR and allocated-state proofs. */
int lj_asm_arm64_iterl_tuple(const GCproto *pt, const BCIns *pc,
    BCIns startins, BCIns liveins, int prepublication,
    LJArm64IteratorSource *source)
{
  uintptr_t lo, hi;
  MSize loop;
  int64_t body, setup, target;
  BCIns call, jump, expected;
  BCReg base = bc_a(startins);
  if (!pt || !source || !pc || !arm64_ir_proto_range(pt, &lo, &hi) ||
      !arm64_ir_pcpos((uintptr_t)pc, lo, hi, &loop) ||
      loop < 2u || loop+1u >= pt->sizebc || bc_op(startins) != BC_ITERL ||
      bc_j(startins) >= 0 || base < 3u ||
      (MSize)base+3u+LJ_FR2 > pt->framesize)
    return 0;
  if (prepublication) {
    if (liveins != startins) return 0;
    expected = startins;
  } else {
    if (bc_op(liveins) != BC_JITERL || bc_a(liveins) != base ||
        bc_d(liveins) == 0) return 0;
    expected = liveins;
  }
  body = (int64_t)loop+1+(int64_t)bc_j(startins);
  setup = body-1;
  if (body <= 1 || body > (int64_t)loop-1 || setup < 1)
    return 0;
  call = arm64_ir_bc_acq(lo, loop-1u);
  jump = arm64_ir_bc_acq(lo, (MSize)setup);
  target = setup+1+(int64_t)bc_j(jump);
  if (bc_op(call) != BC_ITERC || bc_a(call) != base ||
      bc_b(call) != 2 || bc_c(call) != 3 ||
      bc_op(jump) != BC_JMP || bc_a(jump) != base ||
      bc_j(jump) <= 0 || target != (int64_t)loop-1)
    return 0;
  /* Recheck all mutable words defining the tuple after decoding offsets. */
  if (arm64_ir_bc_acq(lo, loop) != expected ||
      arm64_ir_bc_acq(lo, loop-1u) != call ||
      arm64_ir_bc_acq(lo, (MSize)setup) != jump)
    return 0;
  source->body_pc = (MSize)body;
  source->call_pc = loop-1u;
  source->loop_pc = loop;
  source->setup_pc = (MSize)setup;
  source->callins = call;
  source->setupins = jump;
  source->base = base;
  return 1;
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
  } else if (op == BC_ITERL) {
    LJArm64IteratorSource source;
    if (!lj_asm_arm64_iterl_tuple(pt, startpc, startins, startins, 1, &source))
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
	!arm64_ir_funcf_bytecode(pt, startins, startins, 1) ||
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
    } else if (kind == LJ_ARM64_ABI_KIND_FLOAT && size == 8 &&
	       (value.t.irt & ~IRT_ISPHI) == IRT_NUM && from == IRT_U32 &&
	       value.op2 == ((IRT_NUM << IRCONV_DSH) | IRT_U32) &&
	       ir_load_acq(&ir[value.op1]).o == IR_CALLXS &&
	       ir_load_acq(&ir[value.op1]).t.irt == IRT_U32) {
      /* Both graph walks independently certify this native result's
      ** conversion and completed lifetime before it is an argument. */
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
    } else if (kind == LJ_ARM64_ABI_KIND_INTEGER &&
               ((size == 8 && from == IRT_INT &&
                 type == (is_unsigned ? IRT_U64 : IRT_I64) &&
                 value.t.irt == type &&
                 value.op2 == ((type << IRCONV_DSH)|IRT_INT|IRCONV_SEXT)) ||
                (size == 8 && from == IRT_NUM &&
                 type == (is_unsigned ? IRT_U64 : IRT_I64) &&
                 value.t.irt == type &&
                 value.op2 == ((type << IRCONV_DSH)|IRT_NUM|IRCONV_ANY)) ||
                (size == 4 && from == IRT_I64 &&
                 type == (is_unsigned ? IRT_U32 : IRT_INT) &&
                 value.t.irt == type &&
                 value.op2 == ((type << IRCONV_DSH)|IRT_I64)) ||
                (from == IRT_U32 && value.t.irt == type &&
                 ir_load_acq(&ir[value.op1]).o == IR_CALLXS &&
                 ir_load_acq(&ir[value.op1]).t.irt == IRT_U32 &&
                 ((size == 4 && !is_unsigned && type == IRT_INT &&
                   value.op2 == ((IRT_INT << IRCONV_DSH)|IRT_U32)) ||
                  (size == 8 && type == (is_unsigned ? IRT_U64 : IRT_I64) &&
                   value.op2 == ((IRT_I64 << IRCONV_DSH)|IRT_U32)))) ||
                (!on_stack && (size == 1 || size == 2) &&
                 from == IRT_INT && value.t.irt == IRT_INT &&
                 value.op2 == ((IRT_INT << IRCONV_DSH) |
                   (size == 1 ? (is_unsigned ? IRT_U8 : IRT_I8) :
                                (is_unsigned ? IRT_U16 : IRT_I16)) |
                   (is_unsigned ? 0 : IRCONV_SEXT))))) {
      /* Each graph walk independently binds the complete numeric conversion
      ** chain to a fixed argument and its original Lua snapshot value. */
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
  case LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER:
	return ((size == sizeof(float) && type == IRT_FLOAT) ||
		(size == sizeof(double) && type == IRT_NUM)) &&
	       value.o == IR_XLOAD && value.op2 == 0;
  case LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE:
	if ((on_stack && size != 8) || value.o != IR_XLOAD || value.op2 != 0)
	  return 0;
	return (size == 1 && type == IRT_U8) ||
	       (size == 2 && type == IRT_U16) ||
	       (size == 4 && type == IRT_U32) ||
	       (size == 8 && type == IRT_U64);
  case LJ_ARM64_ABI_KIND_INTEGER:
  case LJ_ARM64_ABI_KIND_ENUM:
	if (size == 8)
	  return type == (is_unsigned ? IRT_U64 : IRT_I64);
	if (size == 4)
	  /* Fixed uint32_t consumes the low word of a Lua INT. Enums retain
	  ** their existing carrier boundary; KINT keeps its signed IR tag. */
	  return type == (is_unsigned ? IRT_U32 : IRT_INT) ||
	    (is_unsigned && type == IRT_INT &&
	     (kind == LJ_ARM64_ABI_KIND_INTEGER ||
	      (value.o == IR_KINT && value.t.irt == IRT_INT)));
	/* Registers carry sign/zero-extended INTs. Fixed stack arguments retain
	** their CType width; the store consumes the low bits of that carrier. */
	if (!on_stack && (size == 1 || size == 2)) {
	  if (type != IRT_INT) return 0;
	  /* A raw INT is not a canonical small register argument. CONV was
	  ** checked above; constant folding must leave the exact width range. */
	  if (value.o == IR_CONV) return 1;
	  if (value.o == IR_KINT && value.t.irt == IRT_INT) {
	    int32_t bound = 1 << (size*8u-(is_unsigned ? 0u : 1u));
	    return value.i >= (is_unsigned ? 0 : -bound) && value.i < bound;
	  }
	  return 0;
	}
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
  if (abi->result_kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
      abi->result_hfa_count >= 1 && abi->result_hfa_count <= 4)
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
  if (abi->result_kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE) {
    switch (abi->result_size) {
    case 1: return IRT_U8;
    case 2: return IRT_U16;
    case 4: return IRT_U32;
    case 8: return IRT_U64;
    default: return IRT_CDATA;
    }
  }
  return IRT_CDATA;
}

/* Native-width scalar boxes and direct one-GPR integer aggregates are returned
** to Lua in a fresh CData root. The argument plan remains independent. */
static int arm64_callxs_result_boxed(const LJArm64ABISignature *abi)
{
  return (abi->result_class == LJ_ARM64_ABI_GPR && abi->result_size == 8 &&
    (abi->result_kind == LJ_ARM64_ABI_KIND_POINTER ||
     abi->result_kind == LJ_ARM64_ABI_KIND_INTEGER)) ||
    (abi->result_class == LJ_ARM64_ABI_GPR &&
     abi->result_kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE &&
     (abi->result_size == 1 || abi->result_size == 2 ||
      abi->result_size == 4 || abi->result_size == 8)) ||
    (abi->result_class == LJ_ARM64_ABI_FPR &&
     abi->result_kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
     (abi->result_hfa_count >= 1 && abi->result_hfa_count <= 4) &&
     (abi->result_size == 4 || abi->result_size == 8));
}

static CTypeID arm64_callxs_box_ctype(const LJArm64ABISignature *abi)
{
  return (abi->result_kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE ||
          abi->result_kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER) ?
    abi->result_declared_id : abi->result_id;
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

static int arm64_callxs_replay_setup_op(BCOp op);

/* The CALLXS graph composes bounded scalar/table/CData reads from the
** rooted grammar. Each pass still proves its own current scratch roots,
** table source, snapshots and saved callable before using that operation. */
static int arm64_rooted_ir_effect(const GCtrace *T, IRRef ref,
                                 IRRef root1, IRRef root2, IRRef root3);
static int arm64_rooted_postra_effect(const LJArm64PostRAView *v, IRRef n,
                                     IRRef ref, IRRef root1, IRRef root2,
                                     IRRef root3);

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
	    (ir_load_acq(&ir[cdref]).o != IR_SLOAD &&
	     ir_load_acq(&ir[cdref]).o != IR_VLOAD) ||
	    irt_type(ir_load_acq(&ir[cdref]).t) != IRT_CDATA)
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
	    if ((abi->is_vararg && (CTypeID)wrapper_id != abi->function_id) ||
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
		    arm64_callxs_box_ctype(&abi) != (CTypeID)ctype ||
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
  /* The recorder uses REF_NIL for an empty fixed argument list. The caller
  ** still matches this count against the CType and source CALL arity. */
  if (ref == REF_NIL) {
    *countp = 0;
    return 1;
  }
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

/* Owned scalar CData arguments retain their exact terminal CType. References,
** numeric conversions and CData PHIs need separate payload/root certificates.
** The enclosing protocol walk proves each native entry, store, leave and
** result snapshot; this proof additionally orders the payload read after the
** producing ordinary CALL and restricts its uses to fixed ABI arguments. */
static int arm64_callxs_cdata_integer_argument(const jit_State *J,
                                              const GCtrace *T, IRRef value)
{
  const IRIns *ir = T->ir;
  IRIns load = ir_load_acq(&ir[value]), src;
  CTState *cts = ctype_ctsG(J2G(J));
  LJArm64ABIArg scalar;
  CTypeID source_id = 0, terminal;
  IRRef ref, source = load.op1;
  unsigned uses = 0, width = load.op2 == IRFL_CDATA_INT64 ? 8u : 4u;
  if (load.o != IR_FLOAD ||
      (load.op2 != IRFL_CDATA_INT64 && load.op2 != IRFL_CDATA_INT) ||
      (width == 8 ? (load.t.irt != IRT_I64 && load.t.irt != IRT_U64) :
                    (load.t.irt != IRT_INT && load.t.irt != IRT_U32)) ||
      source < REF_FIRST || source >= value) return 0;
  src = ir_load_acq(&ir[source]);
  if (irt_type(src.t) != IRT_CDATA || !irt_isguard(src.t)) return 0;
  if (src.o == IR_SLOAD || src.o == IR_VLOAD) {
    /* The enclosing graph walk certifies every CData VLOAD as the exact
    ** rooted table hit, with its success guard and retry/result snapshots.
    ** Retain that source while proving its CType and fixed ABI payload. */
    for (ref = source+1u; ref < value; ref++) {
      IRIns eq = ir_load_acq(&ir[ref]), id;
      IRRef idref, kref;
      int32_t ctype;
      if (eq.o != IR_EQ || eq.t.irt != (IRT_INT|IRT_GUARD)) continue;
      idref = eq.op1; kref = eq.op2;
      if (idref < REF_FIRST) { idref = eq.op2; kref = eq.op1; }
      if (idref <= source || idref >= ref) continue;
      id = ir_load_acq(&ir[idref]);
      if (id.o != IR_FLOAD || id.op1 != source ||
          id.op2 != IRFL_CDATA_CTYPEID) continue;
      if (id.t.irt != IRT_U16 || source_id != 0 ||
          !arm64_callxs_kint_value(ir, T->nk, kref, &ctype) || ctype <= 0)
        return 0;
      source_id = (CTypeID)ctype;
    }
  } else if (src.o == IR_CNEW) {
    IRRef producer, anchor = 0, store = 0, leave = 0, closed = 0;
    BCIns sourceins;
    if (width != 8 || !arm64_callxs_box_call(cts, ir, T->nk, T->nins, source,
          &producer, &source_id) || producer <= source || producer >= value)
      return 0;
    for (ref = source+1u; ref < value; ref++) {
      IRIns ins = ir_load_acq(&ir[ref]);
      if (ref < producer && ins.o == IR_XSAVE) anchor = ref;
      if (ref > producer && ins.o == IR_XSTORE && ins.op2 == producer)
        store = ref;
      if (store && ref > store && ins.o == IR_CALLS &&
          ins.op2 == IRCALL_lj_ffi_native_trace_leave) leave = ref;
      if (leave && ins.o == IR_EQ && ins.t.irt == (IRT_INT|IRT_GUARD)) {
        IRRef zero = ins.op1 == leave ? ins.op2 :
                     ins.op2 == leave ? ins.op1 : 0;
        int32_t k;
        if (arm64_callxs_kint_value(ir, T->nk, zero, &k) && k == 0)
          closed = ref;
      }
    }
    if (!closed || !anchor ||
        !arm64_callxs_xsave_anchor(ir, T->nins, anchor, proto_bc(J->pt),
          J->pt->sizebc, NULL, &sourceins) || bc_op(sourceins) != BC_CALL)
      return 0;
  } else return 0;
  memset(&scalar, 0, sizeof(scalar));
  if (!source_id || !lj_ccall_arm64_scalar_class(cts, source_id, 0,
        &scalar, &terminal) || scalar.kind != LJ_ARM64_ABI_KIND_INTEGER ||
      scalar.cls != LJ_ARM64_ABI_GPR || scalar.size != width ||
      load.t.irt != (width == 8 ? (scalar.is_unsigned ? IRT_U64 : IRT_I64) :
                                (scalar.is_unsigned ? IRT_U32 : IRT_INT)))
    return 0;
  /* The CData owns the Lua value. Even a signed 32-bit payload must not
  ** replace it or another Lua integer in a restorable snapshot. */
  if (width == 4) {
    MSize s, i;
    if (T->snap == NULL || T->snapmap == NULL) return 0;
    for (s = 0; s < T->nsnap; s++) {
      MSize ofs = snap_mapofs_acq(&T->snap[s]), n = snap_nent_acq(&T->snap[s]);
      if (ofs > T->nsnapmap || n > T->nsnapmap-ofs) return 0;
      for (i = 0; i < n; i++)
        if (snap_ref(snapentry_acq(&T->snapmap[ofs+i])) == value) return 0;
    }
  }
  for (ref = value+1u; ref < T->nins; ref++) {
    IRIns use = ir_load_acq(&ir[ref]);
    IRMode mode;
    if (use.o >= IR__MAX) return 0;
    mode = lj_ir_mode[use.o];
    if (((irm_op1(mode) == IRMref && use.op1 == value) ||
         (irm_op2(mode) == IRMref && use.op2 == value)) &&
        use.o != IR_CARG && !(use.o == IR_CALLXS && use.op1 == value &&
                             use.op2 != value)) return 0;
    if (use.o == IR_CALLXS) {
      IRRef args[LJ_ARM64_ABI_MAX_ARGS];
      LJArm64ABISignature abi;
      unsigned n, i;
      if (!arm64_callxs_arglist(ir, T->nk, T->nins, ref, args, &n)) return 0;
      for (i = 0; i < n; i++) if (args[i] == value) {
        if (!arm64_callxs_signature_for_call(cts, ir, T->nk, T->nins,
              ref, &abi, NULL) || i >= abi.nargs ||
            abi.arg[i].id != terminal || abi.arg[i].size != width ||
            abi.arg[i].kind != LJ_ARM64_ABI_KIND_INTEGER ||
            abi.arg[i].is_unsigned != scalar.is_unsigned) return 0;
        uses++;
      }
    }
  }
  return uses != 0;
}

/* Prove an exact-width, direct by-value integer aggregate carrier. The raw
** XLOAD may feed only a fixed aggregate argument; its owning CData is separately
** retained in the source XSAVE and in the call's GC-root proof. */
static int arm64_callxs_cdata_integer_aggregate_argument(
	CTState *cts, const GCtrace *T, IRRef value, IRRef expected_source)
{
  const IRIns *ir = T->ir;
  IRIns load, address, src;
  CTypeID source_id = 0, terminal;
  CType type;
  CTInfo info;
  CTSize size;
  IRType expected;
  IRRef source, ref;
  unsigned consumers = 0;
  if (T->snap == NULL || T->snapmap == NULL ||
	value < REF_FIRST || value >= T->nins) return 0;
  load = ir_load_acq(&ir[value]);
  if (load.o != IR_XLOAD || load.op2 != 0 || load.op1 < REF_FIRST ||
      load.op1 >= value) return 0;
  address = ir_load_acq(&ir[load.op1]);
  source = address.op1;
  {
    int64_t offset;
    if (address.o != IR_ADD || address.t.irt != IRT_P64 ||
        source < REF_FIRST || source >= load.op1 ||
        !arm64_callxs_offset_value(ir, T->nk, address.op2, &offset) ||
        offset != (int64_t)sizeof(GCcdata)) return 0;
  }
  src = ir_load_acq(&ir[source]);
  if ((src.o != IR_SLOAD && src.o != IR_VLOAD) ||
      src.t.irt != (IRT_CDATA|IRT_GUARD) ||
      (expected_source != 0 && source != expected_source)) return 0;
  for (ref = source+1u; ref < value; ref++) {
    IRIns field = ir_load_acq(&ir[ref]);
    IRRef guard;
    if (field.o != IR_FLOAD || field.op1 != source ||
	field.op2 != IRFL_CDATA_CTYPEID) continue;
    if (source_id || field.t.irt != IRT_U16) return 0;
    for (guard = ref+1u; guard < value; guard++) {
      IRIns eq = ir_load_acq(&ir[guard]);
      IRRef kref;
      int32_t id;
      if (eq.o != IR_EQ || eq.t.irt != (IRT_INT|IRT_GUARD) ||
	  (eq.op1 != ref && eq.op2 != ref)) continue;
      kref = eq.op1 == ref ? eq.op2 : eq.op1;
      if (!arm64_callxs_kint_value(ir, T->nk, kref, &id) || id <= 0 ||
	source_id) return 0;
      source_id = (CTypeID)id;
    }
  }
  if (!source_id || !lj_ccall_arm64_raw_type(cts, source_id, &type,
	&terminal)) return 0;
  info = ctype_info_acq(&type);
  size = ctype_size_acq(&type);
  if (!ctype_isstruct(info) || ctype_isvltype(info) ||
      ctype_isref(info) || size == 0 || size > 8) return 0;
  switch (size) {
  case 1: expected = IRT_U8; break;
  case 2: expected = IRT_U16; break;
  case 4: expected = IRT_U32; break;
  case 8: expected = IRT_U64; break;
  default: return 0;
  }
  if (load.t.irt != expected) return 0;
  {
    LJArm64ABIArg aggregate;
    if (!lj_ccall_arm64_integer_aggregate_class(cts, source_id,
	  &aggregate, &terminal) || aggregate.size != size) return 0;
  }
  for (ref = 0; ref < T->nsnap; ref++) {
    MSize ofs = snap_mapofs_acq(&T->snap[ref]);
    MSize nent = snap_nent_acq(&T->snap[ref]), i;
    if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs) return 0;
    for (i = 0; i < nent; i++)
	if (snap_ref(snapentry_acq(&T->snapmap[ofs+i])) == value) return 0;
  }
  for (ref = value+1u; ref < T->nins; ref++) {
    IRIns use = ir_load_acq(&ir[ref]);
    IRMode mode;
    if (use.o >= IR__MAX) return 0;
    mode = lj_ir_mode[use.o];
    if (((irm_op1(mode) == IRMref && use.op1 == value) ||
	 (irm_op2(mode) == IRMref && use.op2 == value)) &&
	use.o != IR_CARG && !(use.o == IR_CALLXS && use.op1 == value))
	return 0;
    if (use.o == IR_CALLXS) {
	IRRef args[LJ_ARM64_ABI_MAX_ARGS];
	LJArm64ABISignature abi;
	unsigned nargs, i;
	if (!arm64_callxs_arglist(ir, T->nk, T->nins, ref, args, &nargs) ||
	    !arm64_callxs_signature_for_call(cts, ir, T->nk, T->nins,
	      ref, &abi, NULL)) return 0;
	for (i = 0; i < nargs; i++) if (args[i] == value) {
	  if (i >= abi.nargs ||
	      abi.arg[i].kind != LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE ||
	      abi.arg[i].id != terminal || abi.arg[i].size != size ||
	      !arm64_callxs_value_kind(ir, value, T->nk, T->nins, ref,
		LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE, size, 1,
		abi.arg[i].reg == LJ_ARM64_ABI_STACK)) return 0;
	  consumers++;
	}
    }
  }
  return consumers != 0;
}

/* Prove one float member extracted from the exact rooted HFA CData. Each
** XLOAD has one fixed CALLXS consumer and is tied to its field offset and
** guarded CType identity; the source TValue remains in its original frame. */
static int arm64_callxs_cdata_float_hfa_member(CTState *cts,
	const GCtrace *T, IRRef value, IRRef expected_source,
	const LJArm64ABIArg *arg)
{
	const IRIns *ir = T->ir;
	IRIns load, address, src;
	LJArm64FloatHFA hfa;
	LJArm64ABIArg inferred;
	CTypeID source_id = 0, rawid;
	IRRef source, ref;
	unsigned member_index;
	unsigned member_size;
	unsigned consumers = 0;
	if (T->snap == NULL || T->snapmap == NULL ||
	    value < REF_FIRST || value >= T->nins) return 0;
	load = ir_load_acq(&ir[value]);
	if (load.o != IR_XLOAD ||
	    (load.t.irt != IRT_FLOAT && load.t.irt != IRT_NUM) ||
	    load.op2 != 0 ||
	    load.op1 < REF_FIRST || load.op1 >= value) return 0;
	member_size = load.t.irt == IRT_FLOAT ? sizeof(float) : sizeof(double);
	address = ir_load_acq(&ir[load.op1]);
	source = address.op1;
	{
	  int64_t offset;
	  if (address.o != IR_ADD || address.t.irt != IRT_P64 ||
	      source < REF_FIRST || source >= load.op1 ||
	      !arm64_callxs_offset_value(ir, T->nk, address.op2, &offset) ||
	      offset < (int64_t)sizeof(GCcdata) ||
	      (offset-(int64_t)sizeof(GCcdata))%(int64_t)member_size != 0 ||
	      (offset-(int64_t)sizeof(GCcdata))/(int64_t)member_size >= 4) return 0;
	  member_index = (unsigned)((offset-(int64_t)sizeof(GCcdata))/
			     (int64_t)member_size);
	}
	if (arg != NULL && (arg->kind != LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER ||
	    arg->size != member_size || arg->member_count == 0 ||
	    arg->member_count > 4 || arg->member_index != member_index)) return 0;
	src = ir_load_acq(&ir[source]);
	if ((src.o != IR_SLOAD && src.o != IR_VLOAD) ||
	    src.t.irt != (IRT_CDATA|IRT_GUARD) ||
	    (expected_source != 0 && source != expected_source))
	  return 0;
	for (ref = source+1u; ref < value; ref++) {
	  IRIns field = ir_load_acq(&ir[ref]);
	  IRRef guard;
	  if (field.o != IR_FLOAD || field.op1 != source ||
	      field.op2 != IRFL_CDATA_CTYPEID) continue;
	  if (source_id || field.t.irt != IRT_U16) return 0;
	  for (guard = ref+1u; guard < value; guard++) {
	    IRIns eq = ir_load_acq(&ir[guard]);
	    IRRef kref;
	    int32_t id;
	    if (eq.o != IR_EQ || eq.t.irt != (IRT_INT|IRT_GUARD) ||
		(eq.op1 != ref && eq.op2 != ref)) continue;
	    kref = eq.op1 == ref ? eq.op2 : eq.op1;
	    if (!arm64_callxs_kint_value(ir, T->nk, kref, &id) || id <= 0 ||
		 source_id) return 0;
	    source_id = (CTypeID)id;
	  }
	}
	if (!source_id || !lj_ccall_arm64_float_hfa_class(cts, source_id,
	    &hfa, &rawid) || hfa.member_size != member_size ||
	    member_index >= hfa.count) return 0;
	{
	  SnapNo s;
	  for (s = 0; s < T->nsnap; s++) {
	    MSize ofs = snap_mapofs_acq(&T->snap[s]);
	    MSize nent = snap_nent_acq(&T->snap[s]), i;
	    if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs) return 0;
	    for (i = 0; i < nent; i++)
	      if (snap_ref(snapentry_acq(&T->snapmap[ofs+i])) == value) return 0;
	  }
	}
	memset(&inferred, 0, sizeof(inferred));
	inferred.kind = LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER;
	inferred.id = rawid;
	inferred.size = (uint8_t)member_size;
	inferred.member_count = hfa.count;
	inferred.member_index = (uint8_t)member_index;
	if (arg == NULL) arg = &inferred;
	if (rawid != arg->id || hfa.count != arg->member_count) return 0;
	for (ref = value+1u; ref < T->nins; ref++) {
	  IRIns use = ir_load_acq(&ir[ref]);
	  IRMode mode;
	  if (use.o >= IR__MAX) return 0;
	  mode = lj_ir_mode[use.o];
	  if ((irm_op1(mode) == IRMref && use.op1 == value) ||
	      (irm_op2(mode) == IRMref && use.op2 == value)) {
	    if (use.o != IR_CARG &&
		!(use.o == IR_CALLXS && use.op1 == value)) return 0;
	  }
	  if (use.o == IR_CALLXS) {
	    LJArm64ABISignature abi;
	    IRRef values[LJ_ARM64_ABI_MAX_ARGS];
	    unsigned nargs, i;
	    if (!arm64_callxs_signature_for_call(cts, ir, T->nk, T->nins,
		ref, &abi, NULL) ||
		!arm64_callxs_arglist(ir, T->nk, T->nins, ref, values, &nargs))
	      return 0;
	    for (i = 0; i < nargs; i++) if (values[i] == value) {
	      if (i >= abi.nargs || abi.arg[i].id != arg->id ||
		  abi.arg[i].kind != LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER ||
		  abi.arg[i].size != member_size ||
		  abi.arg[i].member_index != arg->member_index ||
		  !arm64_callxs_value_kind(ir, value, T->nk, T->nins, ref,
			LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER, member_size, 0,
			abi.arg[i].reg == LJ_ARM64_ABI_STACK))
		return 0;
	      consumers++;
	    }
	  }
	}
	return consumers == 1;
}

/* A Lua INT is already a valid fixed 32-bit argument. Smaller register
** arguments and 64-bit arguments need the recorder's exact extension;
** compact stack arguments retain only their CType-width bits. */
static IRRef arm64_callxs_intarg_source(const GCtrace *T, IRRef carrier)
{
  IRIns c;
  if (!arm64_callxs_ref_valid(T->ir, T->nk, T->nins, T->nins, carrier))
    return 0;
  c = ir_load_acq(&T->ir[carrier]);
  if (c.o != IR_CONV ||
      !arm64_callxs_ref_valid(T->ir, T->nk, T->nins, carrier, c.op1) ||
      irt_type(ir_load_acq(&T->ir[c.op1]).t) != IRT_INT) return 0;
  if ((c.t.irt == IRT_INT &&
       (c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_I8|IRCONV_SEXT) ||
        c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_I16|IRCONV_SEXT) ||
        c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_U8) ||
        c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_U16))) ||
      ((c.t.irt == IRT_U8 || c.t.irt == IRT_U16) &&
       c.op2 == ((c.t.irt<<IRCONV_DSH)|IRT_INT)) ||
      ((c.t.irt == IRT_I64 || c.t.irt == IRT_U64) &&
       c.op2 == ((c.t.irt<<IRCONV_DSH)|IRT_INT|IRCONV_SEXT)))
    return c.op1;
  return 0;
}

/* Derived carriers may feed only named integer ABI positions. The original
** INT remains the Lua value in snapshots, even when the carrier is also INT. */
static int arm64_callxs_intarg_member(const jit_State *J, const GCtrace *T,
                                     IRRef value)
{
  IRRef r;
  MSize s, i;
  unsigned uses = 0;
  if (!arm64_callxs_intarg_source(T, value)) return 0;
  for (s = 0; s < T->nsnap; s++) {
    MSize ofs = snap_mapofs_acq(&T->snap[s]), n = snap_nent_acq(&T->snap[s]);
    if (ofs > T->nsnapmap || n > T->nsnapmap-ofs) return 0;
    for (i = 0; i < n; i++)
      if (snap_ref(snapentry_acq(&T->snapmap[ofs+i])) == value) return 0;
  }
  for (r = value+1u; r < T->nins; r++) {
    IRIns use = ir_load_acq(&T->ir[r]);
    IRMode mode;
    if (use.o >= IR__MAX) return 0;
    mode = lj_ir_mode[use.o];
    if (((irm_op1(mode) == IRMref && use.op1 == value) ||
         (irm_op2(mode) == IRMref && use.op2 == value)) &&
        use.o != IR_CARG && !(use.o == IR_CALLXS && use.op1 == value &&
                             use.op2 != value)) return 0;
    if (use.o == IR_CALLXS) {
      LJArm64ABISignature abi;
      IRRef args[LJ_ARM64_ABI_MAX_ARGS];
      unsigned n, a;
      if (!arm64_callxs_arglist(T->ir, T->nk, T->nins, r, args, &n)) return 0;
      for (a = 0; a < n; a++) if (args[a] == value) {
        if (!arm64_callxs_signature_for_call(ctype_ctsG(J2G(J)), T->ir,
              T->nk, T->nins, r, &abi, NULL) || a >= abi.nargs ||
            abi.arg[a].kind != LJ_ARM64_ABI_KIND_INTEGER ||
            !arm64_callxs_value_kind(T->ir, value, T->nk, T->nins, r,
              LJ_ARM64_ABI_KIND_INTEGER, abi.arg[a].size,
              abi.arg[a].is_unsigned, abi.arg[a].reg == LJ_ARM64_ABI_STACK))
          return 0;
        uses++;
      }
    }
  }
  return uses != 0;
}

/* Decode the recorder's NUM-to-fixed-integer carrier, including the fold
** through a U32 result's exact Lua widening. Native carriers are not Lua
** values: the caller snapshot must retain that original NUM. */
static IRRef arm64_callxs_numarg_source(const GCtrace *T, IRRef carrier,
                                      IRRef member)
{
  IRRef r = carrier;
  IRIns c;
  int found = member == 0;
  if (!arm64_callxs_ref_valid(T->ir, T->nk, T->nins, T->nins, r)) return 0;
  c = ir_load_acq(&T->ir[r]);
  found |= r == member;
  if (c.o == IR_CALLXS && c.t.irt == IRT_U32) goto u32_result;
  if (c.o != IR_CONV) return 0;
  if ((c.t.irt == IRT_INT &&
       (c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_I8|IRCONV_SEXT) ||
        c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_I16|IRCONV_SEXT) ||
        c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_U8) ||
        c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_U16))) ||
      ((c.t.irt == IRT_U8 || c.t.irt == IRT_U16) &&
       c.op2 == ((c.t.irt<<IRCONV_DSH)|IRT_INT))) {
    if (!arm64_callxs_ref_valid(T->ir, T->nk, T->nins, r, c.op1)) return 0;
    r = c.op1; c = ir_load_acq(&T->ir[r]); found |= r == member;
    if (c.o != IR_CONV || c.t.irt != IRT_INT) return 0;
  }
  if ((c.t.irt == IRT_INT &&
       c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_U32)) ||
      ((c.t.irt == IRT_I64 || c.t.irt == IRT_U64) &&
       c.op2 == ((IRT_I64<<IRCONV_DSH)|IRT_U32))) {
    if (!arm64_callxs_ref_valid(T->ir, T->nk, T->nins, r, c.op1)) return 0;
    r = c.op1; c = ir_load_acq(&T->ir[r]); found |= r == member;
    goto u32_result;
  }
  if (c.t.irt == IRT_INT || c.t.irt == IRT_U32) {
    if (c.o != IR_CONV || c.op2 != ((c.t.irt<<IRCONV_DSH)|IRT_I64) ||
        !arm64_callxs_ref_valid(T->ir, T->nk, T->nins, r, c.op1)) return 0;
    r = c.op1; c = ir_load_acq(&T->ir[r]); found |= r == member;
    if (c.t.irt != IRT_I64) return 0;
  }
  if (!found || c.o != IR_CONV ||
      (c.t.irt != IRT_I64 && c.t.irt != IRT_U64) ||
      c.op2 != ((c.t.irt<<IRCONV_DSH)|IRT_NUM|IRCONV_ANY) ||
      !arm64_callxs_ref_valid(T->ir, T->nk, T->nins, r, c.op1) ||
      irt_type(ir_load_acq(&T->ir[c.op1]).t) != IRT_NUM) return 0;
  return c.op1;
u32_result:
  if (found && c.o == IR_CALLXS && c.t.irt == IRT_U32) {
    IRRef use, number = 0;
    for (use = r+1u; use < T->nins; use++) {
      IRIns widen = ir_load_acq(&T->ir[use]);
      if (widen.o == IR_CONV && widen.op1 == r &&
          widen.t.irt == IRT_NUM &&
          widen.op2 == ((IRT_NUM<<IRCONV_DSH)|IRT_U32)) {
        if (number) return 0;
        number = use;
      }
    }
    return number;
  }
  return 0;
}

static int arm64_callxs_numarg_member(const jit_State *J, const GCtrace *T,
                                     IRRef value)
{
  IRRef r, source = arm64_callxs_numarg_source(T, value, 0);
  MSize s, i;
  unsigned consumers = 0;
  if (!source) return 0;
  for (s = 0; s < T->nsnap; s++) {
    MSize ofs = snap_mapofs_acq(&T->snap[s]), n = snap_nent_acq(&T->snap[s]);
    if (ofs > T->nsnapmap || n > T->nsnapmap-ofs) return 0;
    for (i = 0; i < n; i++)
      if (snap_ref(snapentry_acq(&T->snapmap[ofs+i])) == value) return 0;
  }
  for (r = value+1u; r < T->nins; r++) {
    IRIns ins = ir_load_acq(&T->ir[r]);
    IRMode mode;
    if (ins.o >= IR__MAX) return 0;
    mode = lj_ir_mode[ins.o];
    if ((irm_op1(mode) == IRMref && ins.op1 == value) ||
        (irm_op2(mode) == IRMref && ins.op2 == value)) {
      if (r == source) {
        /* The raw U32 result also feeds its exact Lua NUM conversion. */
      } else if (ins.o == IR_CONV) {
        if (!arm64_callxs_numarg_source(T, r, value)) return 0;
      } else if (ins.o != IR_CARG &&
                 !(ins.o == IR_CALLXS && ins.op1 == value && ins.op2 != value))
        return 0;
    }
    if (ins.o == IR_CALLXS) {
      LJArm64ABISignature abi;
      IRRef args[LJ_ARM64_ABI_MAX_ARGS];
      unsigned count, a;
      if (!arm64_callxs_signature_for_call(ctype_ctsG(J2G(J)), T->ir,
            T->nk, T->nins, r, &abi, NULL) ||
          !arm64_callxs_arglist(T->ir, T->nk, T->nins, r, args, &count) ||
          count < abi.nargs) return 0;
      for (a = 0; a < count; a++) {
        if (arm64_callxs_numarg_source(T, args[a], value)) {
          LJArm64ABIArg *arg;
          if (a >= abi.nargs) return 0;
          arg = &abi.arg[a];
          if (arg->kind != LJ_ARM64_ABI_KIND_INTEGER ||
              !arm64_callxs_value_kind(T->ir, args[a], T->nk, T->nins, r,
                LJ_ARM64_ABI_KIND_INTEGER, arg->size, arg->is_unsigned,
                arg->reg == LJ_ARM64_ABI_STACK)) return 0;
          consumers++;
        }
      }
    }
  }
  return consumers != 0;
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
  if (!tail && st == IRT_NUM &&
      arm64_callxs_numarg_source(T, value, 0) == source) return 1;
  if (!tail && st == IRT_CDATA && dst.o == IR_FLOAD &&
      dst.op1 == source && dst.op2 == IRFL_CDATA_INT64 &&
      (dst.t.irt == IRT_I64 || dst.t.irt == IRT_U64)) return 1;
  if (!tail && st == IRT_CDATA && dst.o == IR_FLOAD &&
      dst.op1 == source && dst.op2 == IRFL_CDATA_INT &&
      (dst.t.irt == IRT_INT || dst.t.irt == IRT_U32)) return 1;
  if (!tail && st == IRT_CDATA && dst.o == IR_FLOAD &&
      dst.op1 == source && dst.op2 == IRFL_CDATA_PTR &&
      dst.t.irt == IRT_P64) return 1;
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
  /* FLOAT -> NUM -> FLOAT folds back to the original fixed C float. The
  ** saved Lua argument must still be its exact widening conversion. */
  if (!tail && src.o == IR_CONV && src.t.irt == IRT_NUM &&
      src.op1 == value && src.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT) &&
      dst.t.irt == IRT_FLOAT) return 1;
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
** metadata validation only: each admission pass separately derives the value,
** marker, caller slot and frame geometry from its own CALLXS lifecycle.
** A zero slot denotes a discarded Lua result: the completed-call snapshot
** is still mandatory, but it need not retain the result. */
static int arm64_callxs_snapshot_value_slot(const SnapShot *snapshots,
    const SnapEntry *map, MSize nsnap, MSize nsnapmap, IRRef at,
    MSize base, MSize slot, IRRef value, int hidden)
{
  const SnapShot *snap = NULL;
  MSize s, ofs, nent, nslots, i;
  unsigned found = 0;
  SnapEntry footer_words[1+LJ_FR2];
  uint64_t footer;
  if (snapshots == NULL || map == NULL || base < 1u+LJ_FR2 ||
      base > UINT8_MAX || slot > UINT8_MAX || value < REF_FIRST || value >= at)
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
      nsnapmap-ofs-nent < 1u+LJ_FR2 || nslots < base ||
      (slot != 0 && slot >= nslots) || (hidden && slot == 0) ||
      (hidden && nslots != slot+1u)) return 0;
  for (i = 0; i < 1u+LJ_FR2; i++)
    footer_words[i] = snapentry_acq(&map[ofs+nent+i]);
  memcpy(&footer, footer_words, sizeof(footer));
  if ((uint8_t)footer != base-(1u+LJ_FR2)) return 0;
  if (slot == 0) return 1;
  for (i = 0; i < nent; i++) {
    SnapEntry entry = snapentry_acq(&map[ofs+i]);
    if (snap_slot(entry) == slot) {
      /* A newly produced value must be restored, never treated as an SLOAD. */
      if (entry != SNAP(slot, 0, value) || found++) return 0;
    }
  }
  return found == 1;
}

/* Raw aggregate result carriers are internal to the CALLXS/XSTORE pair and
** must never be restored as Lua scalars. */
static int arm64_callxs_result_not_snapshotted(const SnapShot *snaps,
		const SnapEntry *map, MSize nsnap, MSize nsnapmap, IRRef result,
		IRRef result_hi)
{
  MSize s;
  if (snaps == NULL || map == NULL) return 0;
  for (s = 0; s < nsnap; s++) {
    MSize ofs = snap_mapofs_acq(&snaps[s]);
    MSize nent = snap_nent_acq(&snaps[s]), i;
    if (ofs > nsnapmap || nent > nsnapmap-ofs) return 0;
    for (i = 0; i < nent; i++) {
      IRRef snapref = snap_ref(snapentry_acq(&map[ofs+i]));
      if (snapref == result ||
	  (result_hi != 0 && snapref > result && snapref <= result_hi))
        return 0;
    }
  }
  return 1;
}

/* The raw U32 result feeds its Lua NUM conversion and independently checked
** fixed integer arguments. Discarded results may have no conversion after
** DCE, but still keep their DONE snapshot. */
static int arm64_callxs_u32_result_uses(const jit_State *J, const GCtrace *T,
                                      IRRef call, IRRef num)
{
  IRRef ref;
  MSize s, i;
  unsigned converted = 0;
  int integer_args = 0;
  if (ir_load_acq(&T->ir[call]).t.irt != IRT_U32 ||
      T->snap == NULL || T->snapmap == NULL) return 0;
  for (ref = call+1u; ref < T->nins; ref++) {
    IRIns use = ir_load_acq(&T->ir[ref]);
    IRMode mode;
    if (use.o >= IR__MAX) return 0;
    mode = lj_ir_mode[use.o];
    if ((irm_op1(mode) == IRMref && use.op1 == call) ||
        (irm_op2(mode) == IRMref && use.op2 == call)) {
      if (ref == num && use.o == IR_CONV && use.op1 == call) {
        if (converted++) return 0;
      } else if (!num || (!integer_args &&
                 !(integer_args = arm64_callxs_numarg_member(J, T, call))))
        return 0;
    }
  }
  for (s = 0; s < T->nsnap; s++) {
    MSize ofs = snap_mapofs_acq(&T->snap[s]);
    MSize nent = snap_nent_acq(&T->snap[s]);
    if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs) return 0;
    for (i = 0; i < nent; i++)
      if (snap_ref(snapentry_acq(&T->snapmap[ofs+i])) == call) return 0;
  }
  return converted == (num != 0);
}

/* A fixed pointer carrier must read a pointer CData, not reinterpret a
** scalar payload. Its exact owner is separately bound to the XSAVE slot. */
static int arm64_callxs_pointer_argument(CTState *cts, const GCtrace *T,
                                        IRRef value)
{
  IRIns load = ir_load_acq(&T->ir[value]), src;
  IRRef source = load.op1, ref;
  CTypeID id = 0, rawid;
  CType ct;
  CTInfo info;
  if (load.o != IR_FLOAD || load.op2 != IRFL_CDATA_PTR ||
      load.t.irt != IRT_P64 || source < REF_FIRST || source >= value)
    return 0;
  src = ir_load_acq(&T->ir[source]);
  if ((src.o != IR_SLOAD && src.o != IR_VLOAD) ||
      src.t.irt != (IRT_CDATA|IRT_GUARD)) return 0;
  for (ref = source+1u; ref < value; ref++) {
    IRIns eq = ir_load_acq(&T->ir[ref]), type;
    IRRef type_ref, kref;
    int32_t k;
    if (eq.o != IR_EQ || eq.t.irt != (IRT_INT|IRT_GUARD)) continue;
    type_ref = eq.op1; kref = eq.op2;
    if (type_ref < REF_FIRST) { type_ref = eq.op2; kref = eq.op1; }
    if (type_ref <= source || type_ref >= ref) continue;
    type = ir_load_acq(&T->ir[type_ref]);
    if (type.o != IR_FLOAD || type.op1 != source ||
        type.op2 != IRFL_CDATA_CTYPEID) continue;
    if (type.t.irt != IRT_U16 || id ||
        !arm64_callxs_kint_value(T->ir, T->nk, kref, &k) || k <= 0)
      return 0;
    id = (CTypeID)k;
  }
  if (!id || !lj_ccall_arm64_raw_type(cts, id, &ct, &rawid)) return 0;
  info = ctype_info_acq(&ct);
  return ctype_isptr(info) && !(info & CTF_REF) &&
         ctype_size_acq(&ct) == sizeof(void *);
}

static int arm64_callxs_vararg_sources(CTState *cts, const GCtrace *T, IRRef xsave,
    IRRef fpref, BCIns sourceins, uintptr_t returnpc,
    const LJArm64ABISignature *abi, unsigned nargs, const IRRef *values)
{
  const SnapShot *snapshot = NULL;
  IRRef sources[LJ_ARM64_ABI_MAX_ARGS];
  IRRef params[LJ_ARM64_ABI_MAX_ARGS];
  IRRef frame = 0, callable = 0, cdata = 0;
  MSize base = 1u+LJ_FR2+(MSize)bc_a(sourceins)+1u+LJ_FR2;
  MSize ofs, nent, nslots, i, prior_slot = 0;
  unsigned tail_count;
  SnapNo s;
  uint64_t footer;
  SnapEntry footer_words[1+LJ_FR2];
  IRIns fn, pc, fp;
  GCobj *obj;
  if (nargs > LJ_ARM64_ABI_MAX_ARGS || nargs < abi->nargs ||
      abi->nparams > LJ_ARM64_ABI_MAX_ARGS ||
      base > UINT8_MAX || base+1u+abi->nparams+(nargs-abi->nargs) > UINT8_MAX ||
      T->snap == NULL || T->snapmap == NULL)
    return 0;
  tail_count = nargs-abi->nargs;
  if (tail_count && (!abi->is_vararg || abi->nparams+tail_count >
	LJ_ARM64_ABI_MAX_ARGS)) return 0;
  memset(sources, 0, sizeof(sources));
  memset(params, 0, sizeof(params));
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
      T->nsnapmap-ofs-nent < 1u+LJ_FR2 ||
      base+1u+abi->nparams+tail_count > nslots)
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
    } else if (slot > base && slot <= base+abi->nparams+tail_count) {
      if (flags != 0) return 0;
      params[slot-base-1u] = value;
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
  for (i = 0; i < nargs; i++) {
    IRIns value = ir_load_acq(&T->ir[values[i]]);
    int aggregate = i < abi->nargs &&
	abi->arg[i].kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE;
    int hfa = i < abi->nargs &&
	abi->arg[i].kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER;
    if (i < abi->nargs) {
      unsigned source_index = abi->arg[i].source_index;
      if (source_index >= abi->nparams || params[source_index] == 0)
	return 0;
      sources[i] = params[source_index];
    } else {
      unsigned source_index = abi->nparams+i-abi->nargs;
      if (source_index >= abi->nparams+tail_count ||
	  params[source_index] == 0) return 0;
      sources[i] = params[source_index];
    }
    if (i < abi->nargs &&
	aggregate &&
	!arm64_callxs_cdata_integer_aggregate_argument(cts, T, values[i],
	  sources[i])) return 0;
    if (hfa && !arm64_callxs_cdata_float_hfa_member(cts, T, values[i],
	  sources[i], &abi->arg[i])) return 0;
    if (i < abi->nargs && value.o == IR_FLOAD &&
        value.op2 == IRFL_CDATA_PTR &&
        (abi->arg[i].kind != LJ_ARM64_ABI_KIND_POINTER ||
         abi->arg[i].size != sizeof(void *) ||
         !arm64_callxs_pointer_argument(cts, T, values[i]))) return 0;
	if (!aggregate && !hfa && !arm64_callxs_source_value(T, xsave, sources[i],
	    values[i], i >= abi->nargs)) return 0;
  }
  return 1;
}

/* A table hit may retry only its own source read, with the table/key restored.
** The following snapshot carries the fetched value before any foreign effect. */
static int arm64_table_ir_slot(const GCtrace *T, MSize sn, MSize slot,
                               IRRef value)
{
  const SnapShot *s = &T->snap[sn];
  MSize ofs = snap_mapofs_acq(s), nent = snap_nent_acq(s), i;
  int found = 0;
  if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs ||
      slot >= snap_nslots_acq(s)) return 0;
  for (i = 0; i < nent; i++) {
    SnapEntry se = snapentry_acq(&T->snapmap[ofs+i]);
    if (snap_slot(se) == slot) {
      if (found++ || se != SNAP(slot, 0, value)) return 0;
    }
  }
  return found || (T->ir[value].o == IR_SLOAD && T->ir[value].op1 == slot);
}

static int arm64_table_ir_pc(const GCtrace *T, const GCproto *pt,
                             MSize sn, MSize *pos)
{
  const SnapShot *s = &T->snap[sn];
  MSize ofs = snap_mapofs_acq(s), nent = snap_nent_acq(s), i;
  SnapEntry words[1+LJ_FR2];
  uint64_t pcbase;
  uintptr_t lo, hi;
  if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs ||
      T->nsnapmap-ofs-nent < 1u+LJ_FR2 ||
      !arm64_ir_proto_range(pt, &lo, &hi)) return 0;
  for (i = 0; i < 1u+LJ_FR2; i++)
    words[i] = snapentry_acq(&T->snapmap[ofs+nent+i]);
  memcpy(&pcbase, words, sizeof(pcbase));
  return (uint8_t)pcbase == 0 &&
         arm64_ir_pcpos((uintptr_t)(pcbase >> 8), lo, hi, pos);
}

static int arm64_table_ir_snapshots(const GCtrace *T, const GCproto *pt,
    IRRef value, IRRef root1, IRRef root2, uintptr_t effect_floor)
{
  MSize pre = (MSize)-1, at = (MSize)-1, after = (MSize)-1, s;
  MSize prepc, atpc, nextpc, bslot, cslot;
  IRRef prior = 0, table = T->ir[root1].op1, key = T->ir[root2].op1;
  BCIns bc;
  BCOp op;
  if (T->snap == NULL || T->snapmap == NULL || T->nsnap < 3) return 0;
  for (s = 0; s < T->nsnap; s++) {
    IRRef ref = snap_ref_acq(&T->snap[s]);
    if (ref < REF_FIRST || ref >= T->nins || ref < prior) return 0;
    prior = ref;
    if (ref < value) pre = s;
    else if (ref == value) {
      if (at != (MSize)-1) return 0;
      at = s;
    } else if (after == (MSize)-1) after = s;
  }
  if (pre == (MSize)-1 || at == (MSize)-1 || after == (MSize)-1 ||
      snap_ref_acq(&T->snap[pre]) > value-2u ||
      !arm64_table_ir_pc(T, pt, pre, &prepc) ||
      !arm64_table_ir_pc(T, pt, at, &atpc) ||
      !arm64_table_ir_pc(T, pt, after, &nextpc) || prepc != atpc ||
      nextpc <= prepc || (uintptr_t)(proto_bc(pt)+prepc) < effect_floor)
    return 0;
  bc = arm64_ir_bc_acq((uintptr_t)proto_bc(pt), prepc);
  op = bc_op(bc);
  if ((op != BC_TGETS && op != BC_TGETV && op != BC_TGETB) ||
      bc_a(bc) >= pt->framesize || bc_b(bc) >= pt->framesize) return 0;
  bslot = 1u+LJ_FR2+bc_b(bc);
  if (!arm64_table_ir_slot(T, pre, bslot, table) ||
      !arm64_table_ir_slot(T, at, bslot, table)) return 0;
  if (op == BC_TGETV) {
    cslot = 1u+LJ_FR2+bc_c(bc);
    if (bc_c(bc) >= pt->framesize ||
        !arm64_table_ir_slot(T, pre, cslot, key) ||
        !arm64_table_ir_slot(T, at, cslot, key)) return 0;
  } else if (op == BC_TGETS) {
    if (T->ir[key].o != IR_KGC || T->ir[key].t.irt != IRT_STR ||
        bc_c(bc) >= pt->sizekgc ||
        ir_kgc_load_acq(&T->ir[key]) != proto_kgc_acq(pt, ~(int32_t)bc_c(bc)))
      return 0;
  } else if (T->ir[key].o != IR_KINT || T->ir[key].t.irt != IRT_INT ||
             T->ir[key].i != (int32_t)bc_c(bc)) return 0;
  for (s = prepc+1u; s < nextpc; s++)
    if (!arm64_callxs_replay_setup_op(
          bc_op(arm64_ir_bc_acq((uintptr_t)proto_bc(pt), s)))) return 0;
  /* The guarded VLOAD remains the type witness, but the recorder uses the
  ** canonical primitive reference in the destination slot for booleans. */
  if (irt_type(T->ir[value].t) == IRT_TRUE) value = REF_TRUE;
  else if (irt_type(T->ir[value].t) == IRT_FALSE) value = REF_FALSE;
  return arm64_table_ir_slot(T, after, 1u+LJ_FR2+bc_a(bc), value);
}

/* Follow pointer aliases too: a string-data address can outlive the direct
** use of its GC owner. The SSA graph is backward-only; bound work even for a
** malformed or unusually deep PHI/ADD tree. */
static int arm64_callxs_gc_dependency(const IRIns *ir, IRRef nins,
    IRRef ref, IRRef owner, unsigned *budget)
{
  IRIns ins;
  int a, b;
  if (ref == owner) return 1;
  if (ref < REF_FIRST) return 0;
  if (ref >= nins || *budget == 0) return -1;
  --*budget;
  ins = ir_load_acq(&ir[ref]);
  if (irt_type(ins.t) != IRT_P64) return 0;
  if (ins.o == IR_FLOAD && ins.op2 == IRFL_CDATA_PTR)
    return ins.op1 == owner;
  if (ins.o != IR_ADD && ins.o != IR_PHI) return 0;
  if (ins.op1 >= ref || ins.op2 >= ref) return -1;
  a = arm64_callxs_gc_dependency(ir, nins, ins.op1, owner, budget);
  b = arm64_callxs_gc_dependency(ir, nins, ins.op2, owner, budget);
  return a < 0 || b < 0 ? -1 : a || b;
}

/* A callback suspends JIT activity: native registers/spills then cease to be
** protected by the phase lease. Every live GC value must have an enumerated
** root in this call's XSAVE, or be an exact identity pinned by the trace. */
static int arm64_callxs_gc_roots(const GCtrace *T, IRRef call, IRRef xsave,
    IRRef box, IRRef fp, unsigned nargs, const IRRef *args)
{
  IRRef value, ref, loop = 0;
  MSize s, i, saved = (MSize)-1;
  if (T->snap == NULL || T->snapmap == NULL) return 0;
  for (ref = REF_FIRST; ref < T->nins; ref++)
    if (ir_load_acq(&T->ir[ref]).o == IR_LOOP) loop = ref;
  for (s = 0; s < T->nsnap; s++) {
    MSize ofs = snap_mapofs_acq(&T->snap[s]);
    MSize nent = snap_nent_acq(&T->snap[s]);
    if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs) return 0;
    if (snap_ref_acq(&T->snap[s]) == xsave) {
      if (saved != (MSize)-1 ||
          snap_count_acq(&T->snap[s]) != SNAPCOUNT_DONE) return 0;
      saved = s;
    }
  }
  if (saved == (MSize)-1) return 0;
  for (value = REF_FIRST; value < call; value++) {
    IRIns ins = ir_load_acq(&T->ir[value]);
    IRRef start = value < loop && loop < call ? loop : call+1u;
    int live = 0, rooted = 0;
    if (!irt_isgcv(ins.t) || irt_type(ins.t) == IRT_P32 ||
        irt_type(ins.t) == IRT_P64 || value == box) continue;
    /* Keep callable and pointer/payload argument owners during the call too. */
    live = ir_load_acq(&T->ir[fp]).op1 == value;
    for (i = 0; i < nargs && !live; i++) {
      IRIns arg = ir_load_acq(&T->ir[args[i]]);
      unsigned budget = 64;
      int dependency = arm64_callxs_gc_dependency(T->ir, T->nins,
                                                  args[i], value, &budget);
      if (dependency < 0) return 0;
      live = dependency || (arg.o == IR_FLOAD && arg.op1 == value);
    }
    for (ref = start; ref < T->nins && !live; ref++) {
      IRIns use = ir_load_acq(&T->ir[ref]);
      IRMode mode;
      unsigned budget = 64;
      int a, b;
      if (use.o >= IR__MAX) return 0;
      mode = lj_ir_mode[use.o];
      a = irm_op1(mode) == IRMref ? arm64_callxs_gc_dependency(
        T->ir, T->nins, use.op1, value, &budget) : 0;
      b = irm_op2(mode) == IRMref ? arm64_callxs_gc_dependency(
        T->ir, T->nins, use.op2, value, &budget) : 0;
      if (a < 0 || b < 0) return 0;
      live = a || b;
    }
    for (s = 0; s < T->nsnap && !live; s++) {
      MSize ofs = snap_mapofs_acq(&T->snap[s]);
      MSize nent = snap_nent_acq(&T->snap[s]);
      if (snap_ref_acq(&T->snap[s]) < start) continue;
      for (i = 0; i < nent && !live; i++) {
        unsigned budget = 64;
        int dependency = arm64_callxs_gc_dependency(T->ir, T->nins,
          snap_ref(snapentry_acq(&T->snapmap[ofs+i])), value, &budget);
        if (dependency < 0) return 0;
        live = dependency;
      }
    }
    if (!live) continue;
    /* A successful identity guard also pins this exact object through KGC. */
    for (ref = value+1u; ref < xsave && !rooted; ref++) {
      IRIns eq = ir_load_acq(&T->ir[ref]);
      IRRef k = eq.op1 == value ? eq.op2 : eq.op2 == value ? eq.op1 : 0;
      if (eq.o == IR_EQ && eq.t.irt == (irt_type(ins.t)|IRT_GUARD) &&
          arm64_callxs_const_member(T->ir, T->nk, k)) {
        IRIns constant = ir_load_acq(&T->ir[k]);
        rooted = constant.o == IR_KGC &&
                 constant.t.irt == irt_type(ins.t);
      }
    }
    {
      MSize ofs = snap_mapofs_acq(&T->snap[saved]);
      MSize nent = snap_nent_acq(&T->snap[saved]);
      MSize slots = snap_nslots_acq(&T->snap[saved]);
      for (i = 0; i < nent && !rooted; i++) {
        SnapEntry se = snapentry_acq(&T->snapmap[ofs+i]);
        rooted = snap_slot(se) >= 1u+LJ_FR2 && snap_slot(se) < slots &&
                 se == SNAP(snap_slot(se), 0, value);
      }
      if (rooted) continue;
      /* Only an unmodified, non-PHI SLOAD can rely on its original slot.
      ** This grammar has no stack stores/cell operations. Check every XSAVE
      ** after the load, including the loop backedge, for slot clobbers. */
      if (ins.o != IR_SLOAD || ins.t.irt != (irt_type(ins.t)|IRT_GUARD) ||
          ins.op2 != IRSLOAD_TYPECHECK || ins.op1 < 1u+LJ_FR2 ||
          ins.op1 >= slots) return 0;
      for (s = 0; s < T->nsnap; s++) {
        IRRef at = snap_ref_acq(&T->snap[s]);
        if (at <= value || at >= T->nins ||
            ir_load_acq(&T->ir[at]).o != IR_XSAVE) continue;
        ofs = snap_mapofs_acq(&T->snap[s]);
        nent = snap_nent_acq(&T->snap[s]);
        for (i = 0; i < nent; i++) {
          SnapEntry se = snapentry_acq(&T->snapmap[ofs+i]);
          if (snap_slot(se) == ins.op1 &&
              se != SNAP(ins.op1, 0, value) &&
              se != SNAP(ins.op1, SNAP_NORESTORE, value)) return 0;
        }
      }
    }
  }
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
  IRRef root1 = 0, root2 = 0, readref = 0, readroot = 0;
  IRRef scalar_read = 0;
  unsigned state = CALLXS_CLOSED, calls = 0, enters = 0, leaves = 0;
  IRRef producer_call = 0, completed_producer = 0;
  uintptr_t xsave_returnpc = 0, producer_returnpc = 0, read_floor = 0;
  IRType producer_type = IRT_NIL;
  int producer_pending = 0, producer_scalar = 0;
  int ctype_seen = 0;
  IRRef boxref = 0;
  CTypeID box_ctype = 0;
  IRType call_result_type = IRT_NIL;
  int result_is_cdata = 0, box_store_seen = 0;
  unsigned box_store_lanes = 0;
  unsigned result_hfa_count = 0;
  unsigned result_hfa_offset[4] = { 0, 0, 0, 0 };
  int result_is_u32 = 0;
  IRRef number_result = 0;
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
      root1 = root2 = 0;
      read_floor = 0;
      completed_producer = 0;
      producer_returnpc = 0;
      producer_type = IRT_NIL;
      break;
    case IR_XPOLL:
      if (state != CALLXS_CLOSED || g->poll ||
	  irt_type(ins.t) != IRT_NIL || !irt_isguard(ins.t))
	return 0;
      g->poll = ref;
      root1 = root2 = 0;
      read_floor = 0;
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
	if (result_hfa_count > 1 &&
	    box_store_lanes != ((1u << result_hfa_count)-1u)) return 0;
	if (result_is_cdata &&
	    !arm64_callxs_snapshot_value_slot(T->snap, T->snapmap,
	      T->nsnap, T->nsnapmap, ref, 1u+LJ_FR2,
	      result_slot, boxref, 0)) return 0;
	if (result_is_u32 &&
	    ((result_slot && !number_result) ||
	     !arm64_callxs_u32_result_uses(J, T, call, number_result) ||
	     !arm64_callxs_snapshot_value_slot(T->snap, T->snapmap,
	       T->nsnap, T->nsnapmap, ref, 1u+LJ_FR2, result_slot,
	       number_result ? number_result : call, 0))) return 0;
	if (!result_is_cdata &&
	    (call_result_type == IRT_INT || call_result_type == IRT_NUM ||
	     call_result_type == IRT_FLOAT) &&
	    ((result_slot && call_result_type == IRT_FLOAT && !number_result) ||
	     !arm64_callxs_snapshot_value_slot(T->snap, T->snapmap,
	       T->nsnap, T->nsnapmap, ref, 1u+LJ_FR2, result_slot,
	       number_result ? number_result : call, 0))) return 0;
	leaves++;
	leave = ref;
	state = CALLXS_LEFT;
      } else if (ins.op2 == IRCALL_lj_tab_gettv_rooted_hit_try ||
                 ins.op2 == IRCALL_lj_tab_getscalar_rooted_try) {
        IRIns table;
        IRType key, result;
        if (state != CALLXS_CLOSED || !root1 || !root2 ||
            arm64_rooted_ir_effect(T, ref, root1, root2, 0) != 2)
          return 0;
        table = ir_load_acq(&T->ir[T->ir[root1].op1]);
        key = arm64_callxs_ref_type(T->ir, nk, T->ir[root2].op1);
        result = irt_type(T->ir[ref+2u].t);
        if (T->ir[root1].op1 < REF_FIRST ||
            (table.o != IR_SLOAD && table.o != IR_VLOAD) ||
            irt_type(table.t) != IRT_TAB || !irt_isguard(table.t) ||
            (table.o == IR_SLOAD && table.op2 != IRSLOAD_TYPECHECK) ||
            (key != IRT_INT && key != IRT_NUM && key != IRT_STR &&
             key != IRT_FALSE && key != IRT_TRUE && key != IRT_TAB &&
             key != IRT_FUNC && key != IRT_CDATA) ||
            (result != IRT_CDATA && result != IRT_TAB &&
             result != IRT_INT && result != IRT_NUM &&
             result != IRT_TRUE && result != IRT_FALSE) ||
            !arm64_table_ir_snapshots(T, J->pt, ref+2u, root1, root2,
                                     read_floor))
          return 0;
        readref = ref+2u;
        readroot = root1;
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
	  if (args[0] != root1 || args[1] != root2 ||
	      args[2] != args[0] || a0.o != IR_TMPREF || a1.o != IR_TMPREF ||
	      a0.t.irt != IRT_PGC || a1.t.irt != IRT_PGC ||
	      a0.op2 != (IRTMPREF_IN1|IRTMPREF_OUT1) ||
	      a1.op2 != IRTMPREF_IN2 ||
	      !arm64_callxs_ref_valid(T->ir, nk, nins, ref, a0.op1) ||
	      !arm64_callxs_ref_valid(T->ir, nk, nins, ref, a1.op1))
	    return 0;
	}
      } else return 0;
      root1 = root2 = 0;
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
	  read_floor = xsave_returnpc;
	  if (producer_pending && producer_scalar) {
	    completed_producer = result_is_u32 ? number_result : producer_call;
	    producer_returnpc = xsave_returnpc;
	    producer_type = result_is_u32 ? IRT_NUM : call_result_type;
	  } else {
	    completed_producer = 0;
	    producer_returnpc = 0;
	    producer_type = IRT_NIL;
	  }
	  producer_pending = 0;
	  boxref = 0;
	  box_ctype = 0;
	  box_store_seen = 0;
	  box_store_lanes = 0;
	  result_hfa_count = 0;
	  memset(result_hfa_offset, 0, sizeof(result_hfa_offset));
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
      result_is_u32 = abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER &&
        abi.result_class == LJ_ARM64_ABI_GPR && abi.result_size == 4 &&
        abi.result_unsigned;
      number_result = 0;

      if (enterargs[0] != J->ktrace || enterargs[1] != fpref) return 0;
      if (arm64_callxs_result_boxed(&abi) ?
	  (!boxref || enterargs[2] != boxref ||
	   box_ctype != arm64_callxs_box_ctype(&abi)) :
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
	  IRRef value, number_source;
	  IRIns carrier;
	  int numeric_arg;
	  if (actual_count == 0 || completed_producer == 0 ||
	      producer_type == IRT_NIL ||
	      !arm64_callxs_callm_pair(proto_bc(J->pt), J->pt->sizebc,
		producer_returnpc, xsave_returnpc, actual_count)) return 0;
	  value = values[actual_count-1u];
	  carrier = ir_load_acq(&T->ir[value]);
	  /* The sole multires value may occupy a fixed integer parameter. Its
	  ** Lua snapshot retains the completed producer's original INT or NUM. */
	  number_source = arm64_callxs_numarg_source(T, value, 0);
	  numeric_arg = (producer_type == IRT_NUM &&
	    number_source == completed_producer) || (producer_type == IRT_INT &&
	    arm64_callxs_intarg_source(T, value) == completed_producer);
	  if (producer_type == IRT_FLOAT && number_source) {
	    IRIns widen = ir_load_acq(&T->ir[number_source]);
	    numeric_arg = widen.o == IR_CONV && widen.t.irt == IRT_NUM &&
	      widen.op1 == completed_producer &&
	      widen.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT);
	  }
	  numeric_arg = numeric_arg && tail_count == 0 &&
	    abi.arg[actual_count-1u].kind == LJ_ARM64_ABI_KIND_INTEGER &&
	    arm64_callxs_value_kind(T->ir, value, nk, nins, ref,
	      LJ_ARM64_ABI_KIND_INTEGER, abi.arg[actual_count-1u].size,
	      abi.arg[actual_count-1u].is_unsigned,
	      abi.arg[actual_count-1u].reg == LJ_ARM64_ABI_STACK);
	  if (value != completed_producer && !numeric_arg &&
	      (carrier.o != IR_CONV || carrier.op1 != completed_producer ||
	       !((irt_type(carrier.t) == IRT_FLOAT &&
		  (producer_type == IRT_NUM || producer_type == IRT_INT) &&
		  carrier.op2 == ((IRT_FLOAT << IRCONV_DSH)|producer_type) &&
		  carrier.t.irt == IRT_FLOAT) ||
		 (irt_type(carrier.t) == IRT_NUM && producer_type == IRT_FLOAT &&
		  carrier.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT) &&
		  carrier.t.irt == IRT_NUM)))) return 0;
	} else if (sourceop != BC_CALL || bc_c(sourceins) == 0 ||
		   (unsigned)bc_c(sourceins)-1u != abi.nparams+tail_count) return 0;
        {
          IRRef cdref = ir_load_acq(&T->ir[fpref]).op1;
          /* A folded boolean still needs its exact Lua argument slot, even
          ** though the native carrier is now only a KINT zero or one. */
          int rooted_args = scalar_read || abi.is_vararg || sourceop == BC_CALLM ||
            ir_load_acq(&T->ir[cdref]).o == IR_VLOAD;
            for (i = 0; i < abi.nargs; i++) {
            IRIns arg = ir_load_acq(&T->ir[values[i]]);
            if ((arg.o == IR_FLOAD && (arg.op2 == IRFL_CDATA_INT64 ||
                                      arg.op2 == IRFL_CDATA_INT ||
                                      arg.op2 == IRFL_CDATA_PTR)) ||
                (abi.arg[i].kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE) ||
                (abi.arg[i].kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER) ||
                arm64_callxs_numarg_source(T, values[i], 0) ||
                arm64_callxs_intarg_source(T, values[i]) ||
                (abi.arg[i].kind == LJ_ARM64_ABI_KIND_INTEGER &&
                 abi.arg[i].size == 4 && abi.arg[i].is_unsigned &&
                 irt_type(arg.t) == IRT_INT))
              rooted_args = 1;
          }
          if (rooted_args && !arm64_callxs_vararg_sources(cts, T, xsave, fpref,
                sourceins, xsave_returnpc, &abi, actual_count, values)) return 0;
        }
        result_slot = 0;
        if (arm64_callxs_result_boxed(&abi)) {
          MSize base = 1u+LJ_FR2+(MSize)bc_a(sourceins)+1u+LJ_FR2;
          /* The meta-call frame holds the callable cdata before its native
          ** arguments. CNEW occupies the next slot, past both. */
          if (!arm64_callxs_snapshot_value_slot(T->snap, T->snapmap,
                T->nsnap, T->nsnapmap, xsave, base,
                base+1u+abi.nparams+tail_count, boxref, 1)) return 0;
          /* B=1 discards the result. Otherwise lj_record_ret replaces the
          ** caller's function slot with this exact box before native leave. */
          if (bc_b(sourceins) != 1)
            result_slot = 1u+LJ_FR2+(MSize)bc_a(sourceins);
        }
	if (!arm64_callxs_result_boxed(&abi) &&
	    (result_is_u32 || irt_type(ins.t) == IRT_INT ||
	     irt_type(ins.t) == IRT_NUM ||
	     irt_type(ins.t) == IRT_FLOAT) && bc_b(sourceins) != 1)
          result_slot = 1u+LJ_FR2+(MSize)bc_a(sourceins);
      }
      if (!arm64_callxs_gc_roots(T, ref, xsave, boxref, fpref,
                                actual_count, values)) return 0;
      for (i = 0; i < abi.nargs; i++) {
	LJArm64ABIArg *a = &abi.arg[i];
	if (!arm64_callxs_value_kind(T->ir, values[i], nk, nins, ref,
		(LJArm64ABIKind)a->kind, a->size, a->is_unsigned,
		a->reg == LJ_ARM64_ABI_STACK))
	  return 0;
        /* Match the CType plan to the IR carriers consumed by asm_gencall.
        ** In particular, a promoted INT cannot stand for a byte/halfword
        ** stack argument, and exhausting one bank cannot consume the other. */
        if (a->reg != LJ_ARM64_ABI_STACK) {
          if (a->cls == LJ_ARM64_ABI_FPR) {
            if (fpr >= CCALL_NARG_FPR || a->reg != fpr++) return 0;
          } else if (a->cls == LJ_ARM64_ABI_GPR) {
            if (gpr >= CCALL_NARG_GPR || a->reg != gpr++) return 0;
          } else return 0;
        } else {
          IRIns carrier = ir_load_acq(&T->ir[values[i]]);
          unsigned width = lj_ir_type_size[irt_type(carrier.t)];
          if (a->cls == LJ_ARM64_ABI_FPR) {
            if (a->kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER) {
              unsigned first = i-a->member_index, part;
              LJArm64ABIArg *head;
              if (a->member_count == 0 || a->member_index >= a->member_count ||
                  first > i || first+a->member_count > abi.nargs) return 0;
              head = &abi.arg[first];
              for (part = 0; part < a->member_count; part++) {
                LJArm64ABIArg *member = &abi.arg[first+part];
                if (member->kind != LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER ||
                    member->cls != LJ_ARM64_ABI_FPR ||
                    member->reg != LJ_ARM64_ABI_STACK ||
                    member->id != head->id ||
                    member->source_index != head->source_index ||
                    member->member_count != a->member_count ||
                    member->member_index != part ||
                    member->size != head->size ||
                    member->stack_offset != head->stack_offset+
                      part*head->size) return 0;
              }
              if (fpr < CCALL_NARG_FPR) fpr = CCALL_NARG_FPR;
            } else if (fpr < CCALL_NARG_FPR) return 0;
          } else if (a->cls != LJ_ARM64_ABI_GPR ||
                     gpr < CCALL_NARG_GPR) return 0;
          if (width != a->size || width == 0 || width > 8) return 0;
          if (a->kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
              a->member_index == 0)
            stack_size = (stack_size+7u) & ~7u;
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
      result_hfa_count = abi.result_hfa_count;
      for (i = 0; i < 4; i++)
	result_hfa_offset[i] = abi.result_hfa_offset[i];
      if ((abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE ||
           abi.result_kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER) &&
	      !arm64_callxs_result_not_snapshotted(T->snap, T->snapmap,
	    T->nsnap, T->nsnapmap, ref,
	    abi.result_hfa_count > 1 ?
	      ref+abi.result_hfa_count-1u : 0)) return 0;
      call_result_type = (IRType)irt_type(ins.t);
      {
	BCIns sourceins;
	if (!arm64_callxs_xsave_anchor(T->ir, nins, xsave,
	      proto_bc(J->pt), J->pt->sizebc, NULL, &sourceins)) return 0;
	producer_call = ref;
	producer_pending = 1;
	producer_scalar = !result_is_cdata &&
	  bc_op(sourceins) == BC_CALL && bc_b(sourceins) == 0 &&
	  abi.nparams+tail_count+1u == (unsigned)bc_c(sourceins) &&
	  (abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER ||
	   abi.result_kind == LJ_ARM64_ABI_KIND_FLOAT ||
	   abi.result_kind == LJ_ARM64_ABI_KIND_ENUM);
      }
      calls++;
      state = CALLXS_ACTIVE;
      break;
    }
    case IR_FLOAD:
      if (ins.op2 == IRFL_CDATA_INT64 || ins.op2 == IRFL_CDATA_INT) {
        if (state != CALLXS_CLOSED ||
            !arm64_callxs_cdata_integer_argument(J, T, ref))
          return 0;
      } else if (ins.op2 == IRFL_CDATA_PTR) {
	if (ins.op1 < REF_FIRST || ins.op1 >= ref ||
	    (ir_load_acq(&T->ir[ins.op1]).o != IR_SLOAD &&
	     ir_load_acq(&T->ir[ins.op1]).o != IR_VLOAD) ||
	    irt_type(ir_load_acq(&T->ir[ins.op1]).t) != IRT_CDATA ||
	    irt_type(ins.t) != IRT_P64)
	  return 0;
      } else if (ins.op2 == IRFL_CDATA_CTYPEID) {
	if (ins.op1 < REF_FIRST || ins.op1 >= ref ||
	    (ir_load_acq(&T->ir[ins.op1]).o != IR_SLOAD &&
	     ir_load_acq(&T->ir[ins.op1]).o != IR_VLOAD) ||
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
    case IR_XLOAD:
      if (state != CALLXS_CLOSED ||
	  ((ins.t.irt == IRT_FLOAT || ins.t.irt == IRT_NUM) ?
	   !arm64_callxs_cdata_float_hfa_member(ctype_ctsG(J2G(J)), T,
	     ref, 0, NULL) :
	   !arm64_callxs_cdata_integer_aggregate_argument(ctype_ctsG(J2G(J)),
	     T, ref, 0)))
	return 0;
      break;
    case IR_VLOAD:
      if (irt_type(ins.t) == IRT_CDATA || irt_type(ins.t) == IRT_TAB ||
          irt_type(ins.t) == IRT_INT || irt_type(ins.t) == IRT_NUM ||
          irt_type(ins.t) == IRT_TRUE || irt_type(ins.t) == IRT_FALSE) {
        /* Every result keeps its exact read and destination certificate.
        ** Scalar ABI conversions are independently checked at the call. */
        if (state != CALLXS_CLOSED || ref != readref ||
            ins.op1 != readroot || ins.op2 || !irt_isguard(ins.t)) return 0;
        if (irt_type(ins.t) != IRT_CDATA && irt_type(ins.t) != IRT_TAB)
          scalar_read = ref;
        readref = readroot = 0;
        break;
      }
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
      if (ins.op2 & (IRTMPREF_IN1|IRTMPREF_OUT1)) root1 = ref;
      if (ins.op2 & (IRTMPREF_IN2|IRTMPREF_OUT2)) root2 = ref;
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
	    read_floor = xsave_returnpc;
	    if (producer_pending && producer_scalar) {
	      completed_producer = result_is_u32 ? number_result : producer_call;
	      producer_returnpc = xsave_returnpc;
	      producer_type = result_is_u32 ? IRT_NUM : call_result_type;
	    } else {
	      completed_producer = 0;
	      producer_returnpc = 0;
	      producer_type = IRT_NIL;
	    }
	    producer_pending = 0;
	    boxref = 0;
	    box_ctype = 0;
	    box_store_seen = 0;
	    box_store_lanes = 0;
	    result_hfa_count = 0;
	    memset(result_hfa_offset, 0, sizeof(result_hfa_offset));
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
    case IR_HIOP: {
	unsigned lane;
	int32_t lane_tag;
	if (state != CALLXS_ACTIVE || result_hfa_count < 2 ||
	    ref <= call || ref >= call+result_hfa_count ||
	    ins.op1 != call ||
	    !arm64_callxs_kint_value(T->ir, nk, ins.op2, &lane_tag) ||
	    ins.t.irt != (unsigned)call_result_type)
	  return 0;
	lane = ref-call;
	if (lane == 0 || lane >= result_hfa_count || lane_tag != (int32_t)lane)
	  return 0;
      break;
    }
    case IR_XSTORE:
      if (state != CALLXS_ACTIVE || !result_is_cdata || !boxref ||
	  ins.op1 < REF_FIRST || ins.op1 >= ref)
	return 0;
      {
	IRIns payload = ir_load_acq(&T->ir[ins.op1]);
	int64_t offset;
	unsigned member = 0;
	if (payload.o != IR_ADD || irt_type(payload.t) != IRT_P64 ||
	    payload.op1 != boxref ||
	    !arm64_callxs_offset_value(T->ir, nk, payload.op2, &offset) ||
	    offset < (int64_t)sizeof(GCcdata)) return 0;
	if (result_hfa_count > 1) {
	  for (member = 0; member < result_hfa_count; member++)
	    if (offset == (int64_t)sizeof(GCcdata)+result_hfa_offset[member])
	      break;
	  if (member == result_hfa_count || ins.op2 != call+member) return 0;
	} else if (offset != (int64_t)sizeof(GCcdata) || ins.op2 != call) {
	  return 0;
	}
	if (ins.op2 >= ref ||
	    (result_hfa_count > 1 && member > 0 &&
	     ir_load_acq(&T->ir[ins.op2]).o != IR_HIOP) ||
	    irt_type(ins.t) != call_result_type) return 0;
	if (member == 0) {
	  if (box_store_seen) return 0;
	  box_store_seen = 1;
	}
	if (member >= 4 || (box_store_lanes & (1u << member))) return 0;
	box_store_lanes |= 1u << member;
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
          ((irt_type(ins.t) == IRT_FALSE || irt_type(ins.t) == IRT_TRUE) &&
           (ins.t.irt != (irt_type(ins.t)|IRT_GUARD) ||
            ins.op2 != IRSLOAD_TYPECHECK)) ||
          (irt_type(ins.t) != IRT_INT && irt_type(ins.t) != IRT_NUM &&
           irt_type(ins.t) != IRT_FALSE && irt_type(ins.t) != IRT_TRUE &&
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
	if (!arm64_callxs_ref_valid(T->ir, nk, nins, ref, ins.op1))
	  return 0;
	src = ir_load_acq(&T->ir[ins.op1]);
	if (arm64_callxs_numarg_source(T, ref, 0)) {
	  if (state != CALLXS_CLOSED || !arm64_callxs_numarg_member(J, T, ref))
	    return 0;
	  break;
	}
	if (arm64_callxs_intarg_source(T, ref)) {
	  if (state != CALLXS_CLOSED || !arm64_callxs_intarg_member(J, T, ref))
	    return 0;
	  break;
	}
	if (irt_type(src.t) != from)
	  return 0;
	/* The loop optimizer may narrow a numeric result or accumulator. The
	** exact integerness guard may run only while the native frame is closed. */
	if (state == CALLXS_CLOSED && ins.t.irt == (IRT_INT|IRT_GUARD) &&
	    from == IRT_NUM && ins.op2 == (IRCONV_INT_NUM|IRCONV_CHECK))
	  break;
	if ((ins.t.irt & ~(IRT_TYPE|IRT_ISPHI)) != 0) return 0;
	if (dst == IRT_NUM && from == IRT_U32 &&
	    ins.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_U32) &&
	    state == CALLXS_ACTIVE && result_is_u32 && ins.op1 == call &&
	    src.t.irt == IRT_U32 && number_result == 0) {
	  number_result = ref;
	  break;
	}
	if (dst == IRT_NUM && from == IRT_FLOAT &&
	    ins.t.irt == IRT_NUM && src.t.irt == IRT_FLOAT &&
	    ins.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT) &&
	    state == CALLXS_ACTIVE && call_result_type == IRT_FLOAT &&
	    ins.op1 == call && number_result == 0) {
	  number_result = ref;
	  break;
	}
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
    if (ir->o == IR_KNUM || ir->o == IR_KGC || ir->o == IR_KKPTR)
      ref++; /* Payload words are not independently addressable constants. */
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

static int arm64_postra_constant_unused(const LJArm64PostRAView *view,
					IRRef kref)
{
  IRRef ref;
  SnapNo s;
  MSize maxmaps;
  if (!view || !view->ir || !view->snap || !view->snapmap ||
      view->nsnap == 0 || view->nsnap > view->nins)
    return 0;
  maxmaps = (MSize)view->nsnap * (LJ_MAX_JSLOTS + 1u + LJ_FR2);
  if (view->nsnapmap > maxmaps) return 0;
  for (ref = REF_FIRST; ref < view->nins; ref++) {
    IRIns p = ir_load_acq(&view->ir[ref]);
    if (p.op1 == kref || p.op2 == kref ||
	p.op1 == kref+LJ_GC64 || p.op2 == kref+LJ_GC64)
      return 0;
  }
  for (s = 0; s < view->nsnap; s++) {
    const SnapShot *snap = &view->snap[s];
    MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap), i;
    if (ofs > view->nsnapmap || nent > view->nsnapmap-ofs) return 0;
    for (i = 0; i < nent; i++) {
      IRRef snapref = snap_ref(snapentry_acq(&view->snapmap[ofs+i]));
      if (snapref == kref || snapref == kref+LJ_GC64)
	return 0;
    }
  }
  return 1;
}

static int arm64_postra_constants(const LJArm64PostRAView *view,
				  int allow_num, int allow_func)
{
  IRRef ref;
  for (ref = REF_TRUE; ref <= REF_NIL; ref++) {
    IRIns k = ir_load_acq(&view->ir[ref]);
    if (k.o != IR_KPRI || k.t.irt != (uint8_t)(REF_NIL-ref) || k.op12 != 0)
      return 0;
  }
  for (ref = view->nk; ref < REF_TRUE; ref++) {
    IRIns k = ir_load_acq(&view->ir[ref]);
    if (k.o == IR_KNUM && allow_num && k.t.irt == IRT_NUM) {
	if (ref+1u >= REF_TRUE) return 0;
	ref++;
	    } else if (k.o == IR_KGC && allow_func && k.t.irt == IRT_FUNC) {
	if (ref+1u >= REF_TRUE) return 0;
	ref++;
    } else if (k.o != IR_KINT || k.t.irt != IRT_INT) {
	return 0;
    }
  }
  return 1;
}

/* Independent post-RA resolver for a numeric bytecode literal. It binds the
** source KNUM bits to an addressable IR_KNUM head and never asks the semantic
** replay helper whether the operand is valid. */
static int arm64_postra_funcf_knum_ref(const LJArm64PostRAView *view,
	MSize index, IRRef *valuep)
{
  TValue source;
  lua_Number number;
  uint64_t bits;
  IRRef ref;
  if (!view || !view->proto || !view->ir || !valuep ||
	index >= view->proto->sizekn) return 0;
  proto_knumtv_load_acq(&source, view->proto, index);
  if (!tvisnumber(&source)) return 0;
  number = numberVnum(&source);
  if (sizeof(number) != sizeof(bits)) return 0;
  memcpy(&bits, &number, sizeof(bits));
  for (ref = view->nk; ref < REF_TRUE; ref++) {
	IRIns ins = ir_load_acq(&view->ir[ref]);
	if (ins.o == IR_KNUM) {
	  if (ref+1u >= REF_TRUE) return 0;
	  if (ins.t.irt == IRT_NUM && ir_knum(&view->ir[ref])->u64 == bits) {
	    *valuep = ref;
	    return 1;
	  }
	  ref++;
	} else if (ins.o == IR_KGC || ins.o == IR_KKPTR) {
	  if (ref+1u >= REF_TRUE) return 0;
	  ref++;
	}
  }
  return 0;
}

static int arm64_postra_funcf_ref(const LJArm64PostRAView *view,
	IRRef ref, IRRef before, IRType type)
{
  if (ref >= view->nk && ref < REF_TRUE) {
	IRRef k;
	for (k = view->nk; k < REF_TRUE; k++) {
	  IRIns ins = ir_load_acq(&view->ir[k]);
	  if (k == ref)
	    return type == IRT_NUM && ins.o == IR_KNUM &&
		   ins.t.irt == IRT_NUM && k+1u < REF_TRUE;
	  if (ins.o == IR_KNUM || ins.o == IR_KGC || ins.o == IR_KKPTR) {
	    if (k+1u >= REF_TRUE) return 0;
	    k++;
	  }
	}
	return 0;
  }
  return arm64_scalar_postra_ref(view, ref, before, type);
}

static int arm64_postra_funcf_find_value(const LJArm64PostRAView *view,
	IRRef before, BCOp op, IRRef x, IRRef y, IRRef *valuep)
{
  BCOp baseop = op;
  IRRef ref;
  if (op == BC_ADDVN || op == BC_ADDNV) baseop = BC_ADDVV;
  else if (op == BC_SUBVN || op == BC_SUBNV) baseop = BC_SUBVV;
  else if (op == BC_MULVN || op == BC_MULNV) baseop = BC_MULVV;
  else if (op == BC_DIVVN || op == BC_DIVNV) baseop = BC_DIVVV;
  for (ref = REF_FIRST; ref < before; ref++) {
	IRIns ins = ir_load_acq(&view->ir[ref]);
	IRType type = irt_type(ins.t);
	int commutative = baseop == BC_ADDVV || baseop == BC_MULVV;
	int operands = (ins.op1 == x && ins.op2 == y) ||
		(commutative && ins.op1 == y && ins.op2 == x);
	if (!operands) continue;
	if (baseop == BC_ADDVV && ins.o == IR_ADDOV && type == IRT_INT &&
	    ins.t.irt == (IRT_INT|IRT_GUARD)) {
	  *valuep = ref;
	  return 1;
	}
	if (type != IRT_NUM || (ins.t.irt & ~(IRT_TYPE|IRT_ISPHI))) continue;
	if ((baseop == BC_ADDVV && ins.o == IR_ADD) ||
	    (baseop == BC_SUBVV && ins.o == IR_SUB) ||
	    (baseop == BC_MULVV && ins.o == IR_MUL) ||
	    (baseop == BC_DIVVV && ins.o == IR_DIV)) {
	  *valuep = ref;
	  return 1;
	}
  }
  return 0;
}

static int arm64_postra_funcf_short_ref(const LJArm64PostRAView *view,
	int16_t value, IRRef *refp)
{
  IRRef ref;
  for (ref = view->nk; ref < REF_TRUE; ref++) {
	IRIns ins = ir_load_acq(&view->ir[ref]);
	if (ins.o == IR_KNUM || ins.o == IR_KGC || ins.o == IR_KKPTR) {
	  if (ref+1u >= REF_TRUE) return 0;
	  ref++;
	} else if (ins.o == IR_KINT && ins.t.irt == IRT_INT &&
		   ins.i == value) {
	  *refp = ref;
	  return 1;
	}
  }
  return 0;
}

/* Independently replay the fixed function's scalar bytecode against allocated
** IR. This binds VN/NV literals and VV operands to exact sources without
** relying on semantic snapshot replay for the allocator's operation proof. */
static int arm64_postra_funcf_source(const LJArm64PostRAView *view,
	IRRef semantic_nins)
{
  IRRef values[UINT8_MAX] = { 0 };
  uint8_t valid[UINT8_MAX] = { 0 };
  MSize pos;
  for (pos = 0; pos < view->proto_numparams; pos++) {
	IRRef ref;
	MSize slot = 1u+LJ_FR2+pos;
	for (ref = REF_FIRST; ref < semantic_nins; ref++) {
	  IRIns ins = ir_load_acq(&view->ir[ref]);
	  IRType type = irt_type(ins.t);
	  if (ins.o == IR_SLOAD && ins.op1 == slot &&
	      ins.op2 == IRSLOAD_TYPECHECK &&
	      (type == IRT_INT || type == IRT_NUM) &&
	      arm64_ir_type_flags(ins.t, type, IRT_GUARD,
			  IRT_GUARD|IRT_ISPHI)) {
	    values[pos] = ref;
	    valid[pos] = 1;
	    break;
	  }
	}
  }
  for (pos = 1; pos+1u < view->proto_sizebc; pos++) {
	BCIns bc = arm64_ir_bc_acq((uintptr_t)view->proto_bc, pos);
	BCOp op = bc_op(bc);
	BCReg a = bc_a(bc), b = bc_b(bc), c = bc_c(bc);
	IRRef value;
	if (a >= view->root_topslot) return 0;
	if (op == BC_CGET || op == BC_MOV) {
	  BCReg src = bc_d(bc);
	  if (src >= view->root_topslot || !valid[src]) return 0;
	  values[a] = values[src];
	  valid[a] = 1;
	} else if (op == BC_KPRI) {
	  if (bc_d(bc) > 2u) return 0;
	  values[a] = (IRRef)(REF_NIL-bc_d(bc));
	  valid[a] = 1;
	} else if (op == BC_KSHORT) {
	  if (!arm64_postra_funcf_short_ref(view, (int16_t)bc_d(bc), &value))
	    return 0;
	  values[a] = value;
	  valid[a] = 1;
	} else if (op == BC_ADDVV || op == BC_SUBVV || op == BC_MULVV ||
	   op == BC_DIVVV) {
	  if (b >= view->root_topslot || c >= view->root_topslot ||
	      !valid[b] || !valid[c] ||
	      !arm64_postra_funcf_find_value(view, semantic_nins, op,
					 values[b], values[c], &value))
	    return 0;
	  values[a] = value;
	  valid[a] = 1;
	} else if (op == BC_ADDVN || op == BC_SUBVN || op == BC_MULVN ||
	   op == BC_DIVVN || op == BC_ADDNV || op == BC_SUBNV ||
	   op == BC_MULNV || op == BC_DIVNV) {
	  int number_left = op == BC_ADDNV || op == BC_SUBNV ||
			    op == BC_MULNV || op == BC_DIVNV;
	  BCReg source = b;
	  MSize kidx = c;
	  IRRef constant;
	  if (source >= view->root_topslot || !valid[source] ||
	      !arm64_postra_funcf_knum_ref(view, kidx, &constant) ||
	      !arm64_postra_funcf_find_value(view, semantic_nins, op,
		number_left ? constant : values[source],
		number_left ? values[source] : constant, &value)) return 0;
	  values[a] = value;
	  valid[a] = 1;
	} else {
	  return 0;
	}
  }
  {
    BCIns ret = arm64_ir_bc_acq((uintptr_t)view->proto_bc,
	view->proto_sizebc-1u);
    if (bc_op(ret) == BC_RET1 && bc_d(ret) == 2u)
      return valid[bc_a(ret)];
    return bc_op(ret) == BC_RET && bc_d(ret) == 3u &&
	   bc_a(ret)+2u <= view->root_topslot &&
	   valid[bc_a(ret)] && valid[bc_a(ret)+1u];
  }
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
      return k.o != IR_KKPTR && type == IRT_INT &&
	 k.o == IR_KINT && k.t.irt == IRT_INT;
    if (k.o == IR_KNUM || k.o == IR_KGC || k.o == IR_KKPTR)
      ref++;
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
	} else if (ins.o == IR_KGC || ins.o == IR_KKPTR) {
	  if (k == value) return 0;
	  k++;  /* GC/KKPTR payload is not independently addressable. */
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
  BCOp baseop = op;
  if (op == BC_ADDVN || op == BC_ADDNV) baseop = BC_ADDVV;
  else if (op == BC_SUBVN || op == BC_SUBNV) baseop = BC_SUBVV;
  else if (op == BC_MULVN || op == BC_MULNV) baseop = BC_MULVV;
  else if (op == BC_DIVVN || op == BC_DIVNV) baseop = BC_DIVVV;
  for (ref = REF_FIRST; ref < nins; ref++) {
	IRIns ins = ir_load_acq(&ir[ref]);
	IRType type = irt_type(ins.t);
	int commutative = baseop == BC_ADDVV || baseop == BC_MULVV;
	int operands = (ins.op1 == x && ins.op2 == y) ||
		(commutative && ins.op1 == y && ins.op2 == x);
	if (!operands) continue;
	if (baseop == BC_ADDVV && ins.o == IR_ADDOV && type == IRT_INT &&
	    ins.t.irt == (IRT_INT|IRT_GUARD)) {
	  *valuep = ref;
	  return 1;
	}
	if (type != IRT_NUM || (ins.t.irt & ~(IRT_TYPE|IRT_ISPHI)))
	  continue;
	if ((baseop == BC_ADDVV && ins.o == IR_ADD) ||
	    (baseop == BC_SUBVV && ins.o == IR_SUB) ||
	    (baseop == BC_MULVV && ins.o == IR_MUL) ||
	    (baseop == BC_DIVVV && ins.o == IR_DIV)) {
	  *valuep = ref;
	  return 1;
	}
  }
  return 0;
}

/* Resolve one source KNUM to its exact recorder constant. Numeric equality
** is insufficient here: it would merge signed zero and distinct NaN payloads
** before the bytecode result is rebound through a snapshot. */
static int arm64_ir_funcf_knum_ref(const IRIns *ir, IRRef nk,
	const GCproto *pt, MSize index, IRRef *valuep)
{
  TValue source;
  lua_Number number;
  uint64_t bits;
  IRRef k;
  if (!ir || !pt || !valuep || index >= pt->sizekn || nk > REF_TRUE)
	return 0;
  proto_knumtv_load_acq(&source, pt, index);
  if (!tvisnumber(&source)) return 0;
  number = numberVnum(&source);
  if (sizeof(number) != sizeof(bits)) return 0;
  memcpy(&bits, &number, sizeof(bits));
  for (k = nk; k < REF_TRUE; k++) {
	IRIns ins = ir_load_acq(&ir[k]);
	if (ins.o == IR_KNUM) {
	  if (k+1u >= REF_TRUE) return 0;
	  if (ins.t.irt == IRT_NUM && ir_knum(&ir[k])->u64 == bits) {
	    *valuep = k;
	    return 1;
	  }
	  k++;
	} else if (ins.o == IR_KGC || ins.o == IR_KKPTR) {
	  if (k+1u >= REF_TRUE) return 0;
	  k++;
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
	} else if (ins.o == IR_KGC || ins.o == IR_KKPTR) {
	  k++;  /* GC/KKPTR payload is never a scalar literal. */
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
	const GCproto *pt, const BCIns *bc, MSize sizebc, MSize framesize,
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
	} else if (op == BC_ADDVN || op == BC_SUBVN || op == BC_MULVN ||
	   op == BC_DIVVN || op == BC_ADDNV || op == BC_SUBNV ||
	   op == BC_MULNV || op == BC_DIVNV) {
	  int number_left = op == BC_ADDNV || op == BC_SUBNV ||
			    op == BC_MULNV || op == BC_DIVNV;
	  BCReg source = b;
	  MSize kidx = c;
	  IRRef constant;
	  if (a >= framesize || source >= framesize || !valid[source] ||
	      !arm64_ir_funcf_knum_ref(ir, nk, pt, kidx, &constant) ||
	      !arm64_ir_funcf_find_value(ir, nins, op,
		number_left ? constant : values[source],
		number_left ? values[source] : constant, &value)) return 0;
	  values[a] = value;
	  valid[a] = 1;
	} else if (op != BC_RET1 && op != BC_RET) {
	  return 0;
	}
  }
  *posp = target;
  return 1;
}

static int arm64_ir_funcf_result_matches(const BCIns *bc,
	MSize sizebc, MSize framesize, const IRRef *values,
	const uint8_t *valid, MSize slot, IRRef value)
{
  uintptr_t lo = (uintptr_t)bc;
  BCIns ret;
  MSize count, reg;
  if (!bc || !values || !valid || sizebc < 2 || sizebc > UINT8_MAX ||
	(uintptr_t)sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns)) return 0;
  ret = arm64_ir_bc_acq(lo, sizebc-1u);
  count = bc_op(ret) == BC_RET1 && bc_d(ret) == 2u ? 1u :
	  bc_op(ret) == BC_RET && bc_d(ret) == 3u ? 2u : 0u;
  if (!count || slot < 1u+LJ_FR2+bc_a(ret) ||
	  slot-(1u+LJ_FR2+bc_a(ret)) >= count) return 0;
  reg = slot-(1u+LJ_FR2);
  return reg < framesize && valid[reg] && values[reg] == value;
}

static int arm64_ir_funcf_scalar_snapshots(const IRIns *ir, IRRef nins,
	IRRef nk, const SnapShot *snaps, MSize nsnap,
	const SnapEntry *snapmap, MSize nsnapmap, const GCproto *pt,
	const BCIns *proto_bc,
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
  MSize result_slot, result_count;
  unsigned terminal_result_mask = 0;
  int pair_call = arm64_ir_funcf_pair_call_leaf_source(pt, NULL);
  if (!proto_bc || proto_sizebc == 0 || framesize == 0 ||
	!pt || proto_bc != proto_bc(pt) || proto_sizebc != pt->sizebc ||
	framesize != pt->framesize || numparams != pt->numparams ||
	!ir || nins <= REF_FIRST || nk == 0 || !snaps || !snapmap ||
	!nsnap || !nsnapmap ||
	proto_sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns)) return 0;
  ret = arm64_ir_bc_acq(lo, proto_sizebc-1u);
  result_count = bc_op(ret) == BC_RET1 && bc_d(ret) == 2u ? 1u :
	  bc_op(ret) == BC_RET && bc_d(ret) == 3u ? 2u : 0u;
  if (!result_count || bc_a(ret)+result_count > framesize) return 0;
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
	if (pair_call) {
	  /* The separate semantic/post-RA source replay below independently binds
	  ** both CALL results and every derived root value. Here retain snapshot
	  ** geometry, ordering, and type checks without guessing child SSA values. */
	  if (pcpos < bytecode_pos) return 0;
	  bytecode_pos = pcpos;
	} else if (!arm64_ir_funcf_advance(ir, nins, nk, pt, proto_bc,
	    proto_sizebc, framesize, pcpos, &bytecode_pos, values, valid)) {
	  return 0;
	}
        for (i = 0; i < snap->nent; i++) {
          SnapEntry entry = snapmap[ofs+i];
          MSize slot = snap_slot(entry);
          IRRef value = snap_ref(entry);
          BCReg reg;
          if (slot < 1u+LJ_FR2 || slot >= 1u+LJ_FR2+framesize ||
              (reg = (BCReg)(slot-(1u+LJ_FR2))) >= framesize ||
	      (!pair_call && (!valid[reg] || values[reg] != value)))
            return 0;
	  if (s+1u == nsnap && snap->ref == pollref &&
	      slot >= result_slot && slot < result_slot+result_count) {
	    unsigned result_bit = (unsigned)(slot-result_slot);
	    if ((terminal_result_mask & (1u << result_bit)) ||
		(!pair_call && !arm64_ir_funcf_result_matches(proto_bc,
		  proto_sizebc, framesize, values, valid, slot, value))) return 0;
	    terminal_result_mask |= 1u << result_bit;
	  }
        }
    }
  }
	  return expected == nsnapmap &&
	(pair_call || terminal_result_mask == ((1u << result_count)-1u)) &&
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
	    IRRef operands[2] = { ins.op1, ins.op2 };
	    unsigned i;
	    for (i = 0; i < 2u; i++) {
	      if (irref_isk(operands[i])) continue;
	      if (operands[i] < REF_FIRST || operands[i] >= ref) return 0;
	      last_ir_use[operands[i]-REF_FIRST] = ref;
	    }
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

static int arm64_ir_funcf_leaf_call_source(const GCproto *pt);
static int arm64_ir_funcf_tail_one_result_source(const GCproto *pt,
							 MSize *callposp);
static int arm64_ir_funcf_one_result_leaf_source(const GCproto *pt,
							 MSize *retposp);
static int arm64_ir_funcf_tail_source_replay(const jit_State *J,
						    const GCtrace *T,
						    IRRef end);
static const GCfunc *arm64_lua_leaf_ir_function(const GCtrace *T, IRRef fn,
							IRRef before, IRRef *guardp);
static const GCfunc *arm64_lua_leaf_postra_function(
							const LJArm64PostRAView *v,
							IRRef fn, IRRef before,
							IRRef *guardp);
static int arm64_ir_funcf_tail_result_map(const jit_State *J,
					 const GCtrace *T, SnapNo s,
					 uint64_t pcbase);
static int arm64_postra_funcf_tail_result_map(
					 const LJArm64PostRAView *v,
					 MSize s, uint64_t pcbase);
static int arm64_scalar_ir_admit_mode(const jit_State *J, const GCtrace *T,
				 BCOp rootop, const LJArm64SideIRView *side,
				 LJArm64IRReject *reject);
static int arm64_scalar_postra_admit_mode(const LJArm64PostRAView *view,
					 BCOp rootop,
					 const LJArm64SideIRView *side,
					 IRRef *ninsout);
static int arm64_postra_funcf_leaf_source(const LJArm64PostRAView *view,
						 IRRef semantic_nins);
static int arm64_postra_funcf_tail_source_replay(
						 const LJArm64PostRAView *view,
						 IRRef semantic_nins);

static int arm64_postra_funcf_admit(const LJArm64PostRAView *view,
	BCIns liveins, int prepublication, IRRef *semantic_ninsp)
{
  const IRIns *ir;
  IRRef ref, pollref, semantic_nins;
  IRIns base, suffix;
  IRRef generic_nins = 0;
  int leaf_call;
  if (view == NULL || view->proto == NULL) return 0;
  leaf_call = arm64_ir_funcf_leaf_call_source(view->proto);
  if (view == NULL || (ir = view->ir) == NULL || view->snap == NULL ||
	view->snapmap == NULL || view->proto_bc == NULL ||
	view->nins <= REF_FIRST || view->nins >= REF_DROP ||
	view->nk == 0 || view->nk > REF_TRUE || view->nsnap == 0 ||
	view->nsnapmap == 0 || view->proto_sizebc == 0 ||
	view->proto == NULL || view->root_topslot == 0 ||
	view->root_topslot > UINT8_MAX ||
	view->base_delta != 0 || view->spadjust != 0 ||
	view->proto_bc != proto_bc(view->proto) ||
	view->proto_sizebc != view->proto->sizebc ||
	view->root_topslot != view->proto->framesize ||
	view->proto_numparams != view->proto->numparams ||
	!arm64_ir_funcf_bytecode(view->proto, view->startins, liveins,
	  prepublication) || !arm64_postra_constants(view, 1, leaf_call)) {
	return 0;
	}
  if (leaf_call) {
    int generic_ok = arm64_scalar_postra_admit_mode(view, BC_FUNCF, NULL,
                                                   &generic_nins);
	if (!generic_ok) return 0;
    {
      int source_ok = arm64_postra_funcf_leaf_source(view, generic_nins);
      if (source_ok && semantic_ninsp) *semantic_ninsp = generic_nins;
      return source_ok;
    }
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
      IRIns a = {0}, b = {0};
      int aconst = irref_isk(ins.op1), bconst = irref_isk(ins.op2);
      if ((ins.o == IR_ADDOV ?
           (type != IRT_INT || ins.t.irt != (IRT_INT|IRT_GUARD)) :
           (type != IRT_NUM || (ins.t.irt & ~(IRT_TYPE|IRT_ISPHI)))) ||
          !arm64_postra_funcf_ref(view, ins.op1, ref, type) ||
          !arm64_postra_funcf_ref(view, ins.op2, ref, type) ||
          (ins.o == IR_ADDOV && (aconst || bconst)))
        return 0;
	  if (!aconst) a = ir_load_acq(&ir[ins.op1]);
	  if (!bconst) b = ir_load_acq(&ir[ins.op2]);
	  if ((!aconst && (irt_type(a.t) != type ||
	      ra_hasspill(regsp_spill(a.prev)) ||
	      !arm64_scalar_postra_register(regsp_reg(a.prev), type))) ||
	      (!bconst && (irt_type(b.t) != type ||
	      ra_hasspill(regsp_spill(b.prev)) ||
	      !arm64_scalar_postra_register(regsp_reg(b.prev), type))) ||
	      (!aconst && !bconst && ins.op1 != ins.op2 &&
	       regsp_reg(a.prev) == regsp_reg(b.prev)) ||
	      !ra_hasreg(ins.r) ||
	      !arm64_scalar_postra_register(ins.r, type) || ra_hasspill(ins.s))
        return 0;
    } else {
      return 0;
    }
  }
  if (!arm64_postra_funcf_source(view, semantic_nins) ||
      !arm64_ir_funcf_scalar_snapshots(ir, semantic_nins, view->nk,
      view->snap, view->nsnap, view->snapmap, view->nsnapmap,
      view->proto, view->proto_bc, view->proto_sizebc, view->proto_numparams,
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
    if (ins.o == IR_ADD && ref < loopref && ins.op1 == idxload &&
	ins.t.irt == (IRT_INT|IRT_ISPHI)) {
      if (preadd != 0) return 0;
      preadd = ref;
    } else if (ins.o == IR_ADD && ref > xpollref && ins.op1 == preadd &&
	ins.t.irt == (IRT_INT|IRT_ISPHI)) {
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
        (idxins.t.irt & ~IRT_ISPHI) != (IRT_INT|IRT_GUARD) ||
        idxins.s != SPS_NONE ||
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
      (idx.t.irt & ~IRT_ISPHI) != (IRT_INT|IRT_GUARD) || idx.s != SPS_NONE ||
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

/* Darwin AAPCS64 preserved locations, shared only within the allocated
** certificate. Keep this available when JIT FFI calls are disabled. */
static int arm64_postra_value_survives_call(const LJArm64PostRAView *view,
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
		return arm64_postra_value_survives_call(view, ins.op1, call,
			capacity, depth+1u);
	if (ins.o == IR_ADD)
		return arm64_postra_value_survives_call(view, ins.op1, call,
			capacity, depth+1u) &&
		       arm64_postra_value_survives_call(view, ins.op2, call,
			capacity, depth+1u);
	return 0;
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
			IRRef renref, renamed_after = 0;
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
			/* Match snap_renameref: a loop-carried result can move to a
			** different FPR at this exit while the next result is live. */
			for (renref = view->nins; renref-- > semantic_nins; ) {
				IRIns ren = ir_load_acq(&view->ir[renref]);
				if (ren.o != IR_RENAME) break;
				if (ren.op1 == value && ren.op2 <= s) {
					rs = ren.prev;
					renamed_after = snap_ref_acq(&view->snap[ren.op2]);
				}
			}
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
				/* A snapshot-specific location must itself survive calls
				** since that rename took effect. The base IR allocation
				** may be preserved while this renamed location is not. */
				if (renamed_after &&
				    !(set == RSET_FPR ?
				      (reg >= RID_D8 && reg <= RID_D15) :
				      (reg >= RID_X19 && reg <= RID_X28))) {
					IRRef barrier = value+1u;
					if (barrier < renamed_after) barrier = renamed_after;
					for (; barrier <= at && barrier < semantic_nins; barrier++) {
						IRIns call = ir_load_acq(&view->ir[barrier]);
						if (call.o == IR_CALLS || call.o == IR_CALLXS)
							return 0;
					}
				}
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


static int arm64_postra_callxs_gc_dependency(const LJArm64PostRAView *view,
    IRRef nins, IRRef ref, IRRef object, unsigned *remaining)
{
  IRIns node;
  int left, right;
  if (ref == object) return 1;
  if (ref < REF_FIRST) return 0;
  if (ref >= nins || *remaining == 0) return -1;
  --*remaining;
  node = ir_load_acq(&view->ir[ref]);
  if (irt_type(node.t) != IRT_P64) return 0;
  switch (node.o) {
  case IR_FLOAD:
    return node.op2 == IRFL_CDATA_PTR && node.op1 == object;
  case IR_ADD: case IR_PHI:
    if (node.op1 >= ref || node.op2 >= ref) return -1;
    left = arm64_postra_callxs_gc_dependency(view, nins, node.op1,
                                            object, remaining);
    right = arm64_postra_callxs_gc_dependency(view, nins, node.op2,
                                             object, remaining);
    return left < 0 || right < 0 ? -1 : left || right;
  default:
    return 0;
  }
}

/* Independently prove callback-visible GC roots from the allocated trace.
** snapshot_locations separately checks the full-width, renamed locations
** from which each materialized XSAVE value is restored. */
static int arm64_postra_callxs_gc_roots(const LJArm64PostRAView *view,
    IRRef nins, IRRef call, IRRef save, IRRef result, IRRef fp,
    unsigned nargs, const IRRef *args)
{
  const SnapShot *snapshot = NULL;
  IRRef r, v, loop = 0;
  MSize s, i, ofs, nent, slots;
  if (view->snap == NULL || view->snapmap == NULL) return 0;
  for (s = 0; s < view->nsnap; s++) {
    const SnapShot *sn = &view->snap[s];
    ofs = snap_mapofs_acq(sn);
    nent = snap_nent_acq(sn);
    if (ofs > view->nsnapmap || nent > view->nsnapmap-ofs) return 0;
    if (snap_ref_acq(sn) == save) {
      if (snapshot || snap_count_acq(sn) != SNAPCOUNT_DONE) return 0;
      snapshot = sn;
    }
  }
  if (!snapshot) return 0;
  ofs = snap_mapofs_acq(snapshot);
  nent = snap_nent_acq(snapshot);
  slots = snap_nslots_acq(snapshot);
  for (r = REF_FIRST; r < nins; r++)
    if (ir_load_acq(&view->ir[r]).o == IR_LOOP) loop = r;
  for (v = REF_FIRST; v < call; v++) {
    IRIns source = ir_load_acq(&view->ir[v]);
    IRRef first = v < loop && loop < call ? loop : call+1u;
    int needed, found = 0;
    if (!irt_isgcv(source.t) || irt_type(source.t) == IRT_P32 ||
        irt_type(source.t) == IRT_P64 || v == result) continue;
    needed = ir_load_acq(&view->ir[fp]).op1 == v;
    for (i = 0; i < nargs && !needed; i++) {
      IRIns carrier = ir_load_acq(&view->ir[args[i]]);
      unsigned remaining = 64;
      int uses = arm64_postra_callxs_gc_dependency(view, nins, args[i],
                                                  v, &remaining);
      if (uses < 0) return 0;
      needed = uses || (carrier.o == IR_FLOAD && carrier.op1 == v);
    }
    for (r = first; r < nins && !needed; r++) {
      IRIns use = ir_load_acq(&view->ir[r]);
      IRMode mode;
      unsigned remaining = 64;
      int left, right;
      if (use.o >= IR__MAX) return 0;
      mode = lj_ir_mode[use.o];
      left = irm_op1(mode) == IRMref ? arm64_postra_callxs_gc_dependency(
        view, nins, use.op1, v, &remaining) : 0;
      right = irm_op2(mode) == IRMref ? arm64_postra_callxs_gc_dependency(
        view, nins, use.op2, v, &remaining) : 0;
      if (left < 0 || right < 0) return 0;
      needed = left || right;
    }
    for (s = 0; s < view->nsnap && !needed; s++) {
      const SnapShot *sn = &view->snap[s];
      MSize base = snap_mapofs_acq(sn), count = snap_nent_acq(sn);
      if (snap_ref_acq(sn) < first) continue;
      for (i = 0; i < count && !needed; i++) {
        unsigned remaining = 64;
        int uses = arm64_postra_callxs_gc_dependency(view, nins,
          snap_ref(snapentry_acq(&view->snapmap[base+i])), v, &remaining);
        if (uses < 0) return 0;
        needed = uses;
      }
    }
    if (!needed) continue;
    for (i = 0; i < nent && !found; i++) {
      SnapEntry entry = snapentry_acq(&view->snapmap[ofs+i]);
      found = snap_slot(entry) >= 1u+LJ_FR2 && snap_slot(entry) < slots &&
              entry == SNAP(snap_slot(entry), 0, v);
    }
    for (r = v+1u; r < save && !found; r++) {
      IRIns guard = ir_load_acq(&view->ir[r]);
      IRRef k = guard.op1 == v ? guard.op2 : guard.op2 == v ? guard.op1 : 0;
      if (guard.o == IR_EQ &&
          guard.t.irt == (irt_type(source.t)|IRT_GUARD) &&
          arm64_callxs_const_member(view->ir, view->nk, k)) {
        IRIns constant = ir_load_acq(&view->ir[k]);
        found = constant.o == IR_KGC &&
                constant.t.irt == irt_type(source.t);
      }
    }
    if (found) continue;
    if (source.o != IR_SLOAD ||
        source.t.irt != (irt_type(source.t)|IRT_GUARD) ||
        source.op2 != IRSLOAD_TYPECHECK || source.op1 < 1u+LJ_FR2 ||
        source.op1 >= slots) return 0;
    /* An omitted original slot must remain unchanged by every staging save,
    ** including those later in the loop. Cell stores are outside this graph. */
    for (s = 0; s < view->nsnap; s++) {
      const SnapShot *sn = &view->snap[s];
      IRRef at = snap_ref_acq(sn);
      MSize base = snap_mapofs_acq(sn), count = snap_nent_acq(sn);
      if (at <= v || at >= nins ||
          ir_load_acq(&view->ir[at]).o != IR_XSAVE) continue;
      for (i = 0; i < count; i++) {
        SnapEntry entry = snapentry_acq(&view->snapmap[base+i]);
        if (snap_slot(entry) == source.op1 &&
            entry != SNAP(source.op1, 0, v) &&
            entry != SNAP(source.op1, SNAP_NORESTORE, v)) return 0;
      }
    }
  }
  return 1;
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
		if (!arm64_postra_value_survives_call(view, value, call,
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
			    arm64_callxs_box_ctype(&abi) != ctypes[i] ||
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

/* Reconstruct owned integer argument provenance independently after allocation.
** A register/spill carrying payload bits is not the CData root: the caller
** separately binds the exact CData source in the pre-native XSAVE. */
static int arm64_postra_cdata_integer_argument(const LJArm64PostRAView *view,
                                              IRRef nins, IRRef ref)
{
  const IRIns *ir = view->ir;
  IRIns payload = ir_load_acq(&ir[ref]), box;
  IRRef boxref = payload.op1, r;
  CTypeID id = 0, rawid;
  LJArm64ABIArg scalar;
  unsigned consumers = 0, width = payload.op2 == IRFL_CDATA_INT64 ? 8u : 4u;
  if (payload.o != IR_FLOAD ||
      (payload.op2 != IRFL_CDATA_INT64 && payload.op2 != IRFL_CDATA_INT) ||
      (width == 8 ? (payload.t.irt != IRT_I64 && payload.t.irt != IRT_U64) :
                    (payload.t.irt != IRT_INT && payload.t.irt != IRT_U32)) ||
      boxref < REF_FIRST || boxref >= ref) return 0;
  box = ir_load_acq(&ir[boxref]);
  if (irt_type(box.t) != IRT_CDATA || !irt_isguard(box.t)) return 0;
  if (box.o == IR_SLOAD || box.o == IR_VLOAD) {
    /* This pass independently proves the rooted hit and snapshots for each
    ** CData VLOAD before reaching its payload. The native caller still binds
    ** that exact CData source in XSAVE; payload carriers are never roots. */
    for (r = boxref+1u; r < ref; r++) {
      IRIns type = ir_load_acq(&ir[r]);
      IRRef g;
      if (type.o != IR_FLOAD || type.op1 != boxref ||
          type.op2 != IRFL_CDATA_CTYPEID) continue;
      if (type.t.irt != IRT_U16) return 0;
      for (g = r+1u; g < ref; g++) {
        IRIns eq = ir_load_acq(&ir[g]);
        IRRef kref;
        int32_t k;
        if (eq.o != IR_EQ || eq.t.irt != (IRT_INT|IRT_GUARD) ||
            (eq.op1 != r && eq.op2 != r)) continue;
        kref = eq.op1 == r ? eq.op2 : eq.op1;
        if (id || !arm64_callxs_kint_value(ir, view->nk, kref, &k) ||
            k <= 0) return 0;
        id = (CTypeID)k;
      }
    }
  } else if (box.o == IR_CNEW) {
    IRRef stored = 0, producer = 0, anchor = 0, left = 0, done = 0;
    int32_t k;
    BCIns sourceins;
    if (width != 8 || box.op2 != REF_NIL ||
        !arm64_callxs_kint_value(ir, view->nk, box.op1, &k) || k <= 0)
      return 0;
    id = (CTypeID)k;
    for (r = boxref+1u; r < nins; r++) {
      IRIns store = ir_load_acq(&ir[r]), address;
      LJArm64ABISignature result;
      int64_t offset;
      if (store.o != IR_XSTORE || store.op1 < REF_FIRST ||
          store.op1 >= r) continue;
      address = ir_load_acq(&ir[store.op1]);
      if (address.o != IR_ADD || irt_type(address.t) != IRT_P64 ||
          address.op1 != boxref ||
          !arm64_callxs_offset_value(ir, view->nk, address.op2, &offset) ||
          offset != (int64_t)sizeof(GCcdata)) continue;
      if (stored || r >= ref || store.op2 <= boxref || store.op2 >= r ||
          !arm64_callxs_signature_for_call(view->cts, ir, view->nk, nins,
            store.op2, &result, NULL) ||
          result.result_kind != LJ_ARM64_ABI_KIND_INTEGER ||
          result.result_size != 8 || result.result_id != id ||
          irt_type(store.t) != payload.t.irt ||
          ir_load_acq(&ir[store.op2]).t.irt != payload.t.irt) return 0;
      stored = r;
      producer = store.op2;
    }
    if (!stored) return 0;
    for (r = boxref+1u; r < ref; r++) {
      IRIns ins = ir_load_acq(&ir[r]);
      if (r < producer && ins.o == IR_XSAVE) anchor = r;
      if (r > stored && ins.o == IR_CALLS &&
          ins.op2 == IRCALL_lj_ffi_native_trace_leave) left = r;
      if (left && ins.o == IR_EQ && ins.t.irt == (IRT_INT|IRT_GUARD)) {
        IRRef zero = ins.op1 == left ? ins.op2 : ins.op2 == left ? ins.op1 : 0;
        int32_t z;
        if (arm64_callxs_kint_value(ir, view->nk, zero, &z) && z == 0)
          done = r;
      }
    }
    if (!done || !anchor || !arm64_callxs_xsave_anchor(ir, nins, anchor,
          view->proto_bc, view->proto_sizebc, NULL, &sourceins) ||
        bc_op(sourceins) != BC_CALL) return 0;
  } else return 0;
  memset(&scalar, 0, sizeof(scalar));
  if (!id || !lj_ccall_arm64_scalar_class(view->cts, id, 0, &scalar, &rawid) ||
      scalar.cls != LJ_ARM64_ABI_GPR || scalar.kind != LJ_ARM64_ABI_KIND_INTEGER ||
      scalar.size != width ||
      payload.t.irt != (width == 8 ? (scalar.is_unsigned ? IRT_U64 : IRT_I64) :
                                   (scalar.is_unsigned ? IRT_U32 : IRT_INT)))
    return 0;
  if (width == 4) {
    MSize s, i;
    if (view->snap == NULL || view->snapmap == NULL) return 0;
    for (s = 0; s < view->nsnap; s++) {
      MSize ofs = snap_mapofs_acq(&view->snap[s]);
      MSize count = snap_nent_acq(&view->snap[s]);
      if (ofs > view->nsnapmap || count > view->nsnapmap-ofs) return 0;
      for (i = 0; i < count; i++)
        if (snap_ref(snapentry_acq(&view->snapmap[ofs+i])) == ref) return 0;
    }
  }
  for (r = ref+1u; r < nins; r++) {
    IRIns ins = ir_load_acq(&ir[r]);
    IRMode mode;
    if (ins.o >= IR__MAX) return 0;
    mode = lj_ir_mode[ins.o];
    if (((irm_op1(mode) == IRMref && ins.op1 == ref) ||
         (irm_op2(mode) == IRMref && ins.op2 == ref)) &&
        ins.o != IR_CARG && !(ins.o == IR_CALLXS && ins.op1 == ref &&
                             ins.op2 != ref)) return 0;
    if (ins.o == IR_CALLXS) {
      IRRef values[LJ_ARM64_ABI_MAX_ARGS];
      unsigned count, a;
      if (!arm64_callxs_arglist(ir, view->nk, nins, r, values, &count)) return 0;
      for (a = 0; a < count; a++) if (values[a] == ref) {
        LJArm64ABISignature signature;
        if (!arm64_callxs_signature_for_call(view->cts, ir, view->nk, nins,
              r, &signature, NULL) || a >= signature.nargs ||
            signature.arg[a].id != rawid || signature.arg[a].size != width ||
            signature.arg[a].kind != LJ_ARM64_ABI_KIND_INTEGER ||
            signature.arg[a].is_unsigned != scalar.is_unsigned) return 0;
        consumers++;
      }
    }
  }
  return consumers != 0;
}

/* Independently decode the allocated extension from its actual Lua INT.
** No source flag is transferred to the native-only carrier. */
static IRRef arm64_postra_intarg_source(const IRIns *ir, IRRef nk, IRRef nins,
                                       IRRef carrier)
{
  IRIns c, source;
  IRType type;
  unsigned mode;
  if (!arm64_callxs_ref_valid(ir, nk, nins, nins, carrier)) return 0;
  c = ir_load_acq(&ir[carrier]);
  type = irt_type(c.t);
  if (c.o != IR_CONV || c.t.irt != type ||
      !arm64_callxs_ref_valid(ir, nk, nins, carrier, c.op1)) return 0;
  source = ir_load_acq(&ir[c.op1]);
  if (irt_type(source.t) != IRT_INT) return 0;
  mode = (unsigned)type << IRCONV_DSH;
  switch (type) {
  case IRT_INT:
    if (c.op2 != (mode|IRT_I8|IRCONV_SEXT) &&
        c.op2 != (mode|IRT_I16|IRCONV_SEXT) &&
        c.op2 != (mode|IRT_U8) && c.op2 != (mode|IRT_U16)) return 0;
    break;
  case IRT_U8: case IRT_U16:
    if (c.op2 != (mode|IRT_INT)) return 0;
    break;
  case IRT_I64: case IRT_U64:
    if (c.op2 != (mode|IRT_INT|IRCONV_SEXT)) return 0;
    break;
  default:
    return 0;
  }
  return c.op1;
}

static int arm64_postra_intarg_member(const LJArm64PostRAView *view,
                                      IRRef nins, IRRef value)
{
  IRRef ref;
  MSize s, e;
  unsigned consumers = 0;
  if (!arm64_postra_intarg_source(view->ir, view->nk, nins, value)) return 0;
  for (s = 0; s < view->nsnap; s++) {
    MSize ofs = snap_mapofs_acq(&view->snap[s]);
    MSize n = snap_nent_acq(&view->snap[s]);
    if (ofs > view->nsnapmap || n > view->nsnapmap-ofs) return 0;
    for (e = 0; e < n; e++)
      if (snap_ref(snapentry_acq(&view->snapmap[ofs+e])) == value) return 0;
  }
  for (ref = value+1u; ref < nins; ref++) {
    IRIns ins = ir_load_acq(&view->ir[ref]);
    IRMode mode;
    if (ins.o >= IR__MAX) return 0;
    mode = lj_ir_mode[ins.o];
    if ((irm_op1(mode) == IRMref && ins.op1 == value) ||
        (irm_op2(mode) == IRMref && ins.op2 == value)) {
      if (ins.o != IR_CARG &&
          !(ins.o == IR_CALLXS && ins.op1 == value && ins.op2 != value))
        return 0;
    }
    if (ins.o == IR_CALLXS) {
      IRRef args[LJ_ARM64_ABI_MAX_ARGS];
      LJArm64ABISignature abi;
      unsigned count, a;
      if (!arm64_callxs_arglist(view->ir, view->nk, nins, ref, args, &count))
        return 0;
      for (a = 0; a < count; a++) if (args[a] == value) {
        if (!arm64_callxs_signature_for_call(view->cts, view->ir, view->nk,
              nins, ref, &abi, NULL) || a >= abi.nargs ||
            abi.arg[a].kind != LJ_ARM64_ABI_KIND_INTEGER ||
            !arm64_callxs_value_kind(view->ir, value, view->nk, nins, ref,
              LJ_ARM64_ABI_KIND_INTEGER, abi.arg[a].size,
              abi.arg[a].is_unsigned, abi.arg[a].reg == LJ_ARM64_ABI_STACK))
          return 0;
        consumers++;
      }
    }
  }
  return consumers != 0;
}

/* Independently walk an allocated fixed-integer carrier back to its Lua NUM.
** Small register extensions name an I8/U8/I16/U16 source in op2 even though
** the actual low-word operand is INT. A U32 result uses the exact NUM widening
** retained for Lua, even when integer argument conversions bypass it. */
static IRRef arm64_postra_numarg_source(const IRIns *ir, IRRef nk, IRRef nins,
                                       IRRef carrier, IRRef member)
{
  IRRef r = carrier;
  unsigned depth;
  IRType required = IRT_NIL;
  int seen = member == 0;
  for (depth = 0; depth < 3u; depth++) {
    IRIns c;
    IRType t;
    if (!arm64_callxs_ref_valid(ir, nk, nins, nins, r)) return 0;
    c = ir_load_acq(&ir[r]); t = irt_type(c.t);
    if (c.t.irt != t || (required != IRT_NIL && t != required)) return 0;
    seen |= r == member;
    if (c.o == IR_CALLXS && t == IRT_U32) {
      IRRef n, result = 0;
      if (!seen) return 0;
      for (n = r+1u; n < nins; n++) {
        IRIns v = ir_load_acq(&ir[n]);
        if (v.op1 == r && v.o == IR_CONV && v.t.irt == IRT_NUM &&
            v.op2 == ((IRT_NUM<<IRCONV_DSH)|IRT_U32)) {
          if (result) return 0;
          result = n;
        }
      }
      return result;
    }
    if (c.o != IR_CONV ||
        !arm64_callxs_ref_valid(ir, nk, nins, r, c.op1)) return 0;
    if ((t == IRT_INT && c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_U32)) ||
        ((t == IRT_I64 || t == IRT_U64) &&
         c.op2 == ((IRT_I64<<IRCONV_DSH)|IRT_U32))) {
      required = IRT_U32;
    } else if (t == IRT_I64 || t == IRT_U64) {
      return seen && c.op2 == ((t<<IRCONV_DSH)|IRT_NUM|IRCONV_ANY) &&
        irt_type(ir_load_acq(&ir[c.op1]).t) == IRT_NUM ? c.op1 : 0;
    } else if ((t == IRT_INT || t == IRT_U32) &&
        c.op2 == ((t<<IRCONV_DSH)|IRT_I64)) {
      required = IRT_I64;
    } else if (depth == 0 &&
        ((t == IRT_INT &&
          (c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_I8|IRCONV_SEXT) ||
           c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_I16|IRCONV_SEXT) ||
           c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_U8) ||
           c.op2 == ((IRT_INT<<IRCONV_DSH)|IRT_U16))) ||
         ((t == IRT_U8 || t == IRT_U16) &&
          c.op2 == ((t<<IRCONV_DSH)|IRT_INT)))) {
      required = IRT_INT;
    } else return 0;
    r = c.op1;
  }
  return 0;
}

static int arm64_postra_numarg_member(const LJArm64PostRAView *view,
                                      IRRef nins, IRRef value)
{
  IRRef r, source = arm64_postra_numarg_source(view->ir, view->nk,
                                              nins, value, 0);
  MSize s, i;
  unsigned uses = 0;
  if (!source) return 0;
  for (s = 0; s < view->nsnap; s++) {
    MSize ofs = snap_mapofs_acq(&view->snap[s]);
    MSize n = snap_nent_acq(&view->snap[s]);
    if (ofs > view->nsnapmap || n > view->nsnapmap-ofs) return 0;
    for (i = 0; i < n; i++)
      if (snap_ref(snapentry_acq(&view->snapmap[ofs+i])) == value) return 0;
  }
  for (r = value+1u; r < nins; r++) {
    IRIns ins = ir_load_acq(&view->ir[r]);
    IRMode mode;
    if (ins.o >= IR__MAX) return 0;
    mode = lj_ir_mode[ins.o];
    if ((irm_op1(mode) == IRMref && ins.op1 == value) ||
        (irm_op2(mode) == IRMref && ins.op2 == value)) {
      if (r == source) {
        /* The separately certified U32 result widening is a Lua value. */
      } else if (ins.o == IR_CONV) {
        if (!arm64_postra_numarg_source(view->ir, view->nk, nins, r, value))
          return 0;
      } else if (ins.o != IR_CARG &&
                 !(ins.o == IR_CALLXS && ins.op1 == value && ins.op2 != value))
        return 0;
    }
    if (ins.o == IR_CALLXS) {
      IRRef args[LJ_ARM64_ABI_MAX_ARGS];
      LJArm64ABISignature abi;
      unsigned count, a;
      if (!arm64_callxs_arglist(view->ir, view->nk, nins, r, args, &count) ||
          !arm64_callxs_signature_for_call(view->cts, view->ir, view->nk,
            nins, r, &abi, NULL) || count < abi.nargs) return 0;
      for (a = 0; a < count; a++) {
        if (arm64_postra_numarg_source(view->ir, view->nk, nins, args[a], value)) {
          LJArm64ABIArg *arg;
          if (a >= abi.nargs) return 0;
          arg = &abi.arg[a];
          if (arg->kind != LJ_ARM64_ABI_KIND_INTEGER ||
              !arm64_callxs_value_kind(view->ir, args[a], view->nk, nins, r,
                LJ_ARM64_ABI_KIND_INTEGER, arg->size, arg->is_unsigned,
                arg->reg == LJ_ARM64_ABI_STACK)) return 0;
          uses++;
        }
      }
    }
  }
  return uses != 0;
}

static int arm64_postra_source_value_matches(const IRIns *ir, IRRef nk,
		IRRef nins, IRRef call, IRRef source, IRRef carrier,
		IRType source_type)
{
	IRIns c;
	if (!arm64_callxs_ref_valid(ir, nk, nins, call, source) ||
	    !arm64_callxs_ref_valid(ir, nk, nins, call, carrier)) return 0;
	if (carrier == source) return 1;
	if (source_type == IRT_NUM &&
	    arm64_postra_numarg_source(ir, nk, nins, carrier, 0) == source) return 1;
	if (carrier == REF_TRUE || carrier == REF_FALSE || carrier == REF_NIL)
		return 0;
	c = ir_load_acq(&ir[carrier]);
	if (source_type == IRT_CDATA && c.o == IR_FLOAD && c.op1 == source &&
	    c.op2 == IRFL_CDATA_INT64 &&
	    (c.t.irt == IRT_I64 || c.t.irt == IRT_U64)) return 1;
	if (source_type == IRT_CDATA && c.o == IR_FLOAD && c.op1 == source &&
	    c.op2 == IRFL_CDATA_INT &&
	    (c.t.irt == IRT_INT || c.t.irt == IRT_U32)) return 1;
	if (source_type == IRT_CDATA && c.o == IR_FLOAD && c.op1 == source &&
	    c.op2 == IRFL_CDATA_PTR && c.t.irt == IRT_P64) return 1;
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
	if (source_type == IRT_NUM && c.t.irt == IRT_FLOAT) {
		IRIns original = ir_load_acq(&ir[source]);
		/* Fixed arguments may reuse the FLOAT input of the exact Lua
		** widening; promoted NUM tails require carrier == source below. */
		if (original.o == IR_CONV && original.t.irt == IRT_NUM &&
		    original.op1 == carrier &&
		    original.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT)) return 1;
	}
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

/* Independently prove the guarded pointer payload before matching its
** allocated carrier to the saved Lua argument. References remain closed. */
static int arm64_postra_pointer_argument(CTState *cts, const GCtrace *T,
                                        IRRef carrier)
{
  IRIns payload = ir_load_acq(&T->ir[carrier]), owner;
  IRRef source = payload.op1, r;
  CTypeID id = 0, terminal;
  CType type;
  CTInfo info;
  if (payload.o != IR_FLOAD || payload.op2 != IRFL_CDATA_PTR ||
      payload.t.irt != IRT_P64 || source < REF_FIRST || source >= carrier)
    return 0;
  owner = ir_load_acq(&T->ir[source]);
  if (owner.t.irt != (IRT_CDATA|IRT_GUARD) ||
      (owner.o != IR_SLOAD && owner.o != IR_VLOAD)) return 0;
  for (r = source+1u; r < carrier; r++) {
    IRIns field = ir_load_acq(&T->ir[r]);
    IRRef g;
    if (field.o != IR_FLOAD || field.op1 != source ||
        field.op2 != IRFL_CDATA_CTYPEID) continue;
    if (field.t.irt != IRT_U16) return 0;
    for (g = r+1u; g < carrier; g++) {
      IRIns guard = ir_load_acq(&T->ir[g]);
      IRRef kref;
      int32_t k;
      if (guard.o != IR_EQ || guard.t.irt != (IRT_INT|IRT_GUARD) ||
          (guard.op1 != r && guard.op2 != r)) continue;
      kref = guard.op1 == r ? guard.op2 : guard.op1;
      if (id || !arm64_callxs_kint_value(T->ir, T->nk, kref, &k) || k <= 0)
        return 0;
      id = (CTypeID)k;
    }
  }
  if (!id || !lj_ccall_arm64_raw_type(cts, id, &type, &terminal)) return 0;
  info = ctype_info_acq(&type);
  return !(info & CTF_REF) && ctype_isptr(info) &&
         ctype_size_acq(&type) == sizeof(void *);
}

/* Independently reconstruct the one-GPR Darwin carrier for a fixed integer
** aggregate. Its CData owner stays in the interpreter argument slot; the
** scalar XLOAD is neither a Lua value nor a GC root. */
static int arm64_postra_cdata_integer_aggregate_argument(
		CTState *cts, const GCtrace *T, IRRef nins, IRRef value,
		IRRef expected_source)
{
	const IRIns *ir = T->ir;
	IRIns load, address, src;
	CTypeID source_id = 0, terminal;
	CType type;
	CTInfo info;
	CTSize size;
	IRType expected;
	IRRef source, ref;
	unsigned consumers = 0;
	if (T->snap == NULL || T->snapmap == NULL ||
	    value < REF_FIRST || value >= nins) return 0;
	load = ir_load_acq(&ir[value]);
	if (load.o != IR_XLOAD || load.op2 != 0 || load.op1 < REF_FIRST ||
	    load.op1 >= value) return 0;
	address = ir_load_acq(&ir[load.op1]);
	source = address.op1;
	if (address.o != IR_ADD || address.t.irt != IRT_P64 ||
	    source < REF_FIRST || source >= load.op1) return 0;
	{
		int64_t offset;
			if (!arm64_callxs_offset_value(ir, T->nk, address.op2, &offset) ||
		    offset != (int64_t)sizeof(GCcdata)) return 0;
	}
	src = ir_load_acq(&ir[source]);
	if ((src.o != IR_SLOAD && src.o != IR_VLOAD) ||
	    src.t.irt != (IRT_CDATA|IRT_GUARD) ||
	    (expected_source != 0 && source != expected_source)) return 0;
	for (ref = source+1u; ref < value; ref++) {
		IRIns field = ir_load_acq(&ir[ref]);
		IRRef guard;
		if (field.o != IR_FLOAD || field.op1 != source ||
		    field.op2 != IRFL_CDATA_CTYPEID) continue;
		if (source_id || field.t.irt != IRT_U16) return 0;
		for (guard = ref+1u; guard < value; guard++) {
			IRIns eq = ir_load_acq(&ir[guard]);
			IRRef kref;
			int32_t id;
			if (eq.o != IR_EQ || eq.t.irt != (IRT_INT|IRT_GUARD) ||
			    (eq.op1 != ref && eq.op2 != ref)) continue;
			kref = eq.op1 == ref ? eq.op2 : eq.op1;
			if (!arm64_callxs_kint_value(ir, T->nk, kref, &id) ||
			    id <= 0 || source_id) return 0;
			source_id = (CTypeID)id;
		}
	}
	if (!source_id || !lj_ccall_arm64_raw_type(cts, source_id, &type,
	    &terminal)) return 0;
	info = ctype_info_acq(&type);
	size = ctype_size_acq(&type);
	if (!ctype_isstruct(info) || ctype_isvltype(info) || ctype_isref(info) ||
	    size == 0 || size > 8) return 0;
	switch (size) {
	case 1: expected = IRT_U8; break;
	case 2: expected = IRT_U16; break;
	case 4: expected = IRT_U32; break;
	case 8: expected = IRT_U64; break;
	default: return 0;
	}
	if (load.t.irt != expected) return 0;
	{
		LJArm64ABIArg aggregate;
		if (!lj_ccall_arm64_integer_aggregate_class(cts, source_id,
		    &aggregate, &terminal) || aggregate.size != size) return 0;
	}
	for (ref = 0; ref < T->nsnap; ref++) {
		MSize ofs = snap_mapofs_acq(&T->snap[ref]);
		MSize nent = snap_nent_acq(&T->snap[ref]), i;
		if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs) return 0;
		for (i = 0; i < nent; i++)
			if (snap_ref(snapentry_acq(&T->snapmap[ofs+i])) == value)
				return 0;
	}
	for (ref = value+1u; ref < nins; ref++) {
		IRIns use = ir_load_acq(&ir[ref]);
		IRMode mode;
		if (use.o >= IR__MAX) return 0;
		mode = lj_ir_mode[use.o];
		if (((irm_op1(mode) == IRMref && use.op1 == value) ||
		     (irm_op2(mode) == IRMref && use.op2 == value)) &&
		    use.o != IR_CARG && !(use.o == IR_CALLXS && use.op1 == value))
			return 0;
		if (use.o == IR_CALLXS) {
			IRRef args[LJ_ARM64_ABI_MAX_ARGS];
			LJArm64ABISignature abi;
			unsigned nargs, i;
			if (!arm64_callxs_arglist(ir, T->nk, nins, ref, args,
			    &nargs) || !arm64_callxs_signature_for_call(cts, ir,
			    T->nk, nins, ref, &abi, NULL)) return 0;
			for (i = 0; i < nargs; i++) if (args[i] == value) {
				if (i >= abi.nargs ||
				    abi.arg[i].kind != LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE ||
				    abi.arg[i].id != terminal || abi.arg[i].size != size ||
				    !arm64_callxs_value_kind(ir, value, T->nk, nins,
				      ref, LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE, size, 1,
				      abi.arg[i].reg == LJ_ARM64_ABI_STACK))
					return 0;
				consumers++;
			}
		}
	}
	return consumers != 0;
}

/*
** Check the real interpreter XSAVE before the CALLXS. `values` is the
** normalized physical CARG list (the NIL vararg separator has already been
** removed by the caller). Fixed arguments use a bounded set of source-linked
** normalizations; each suffix carrier is tied to its exact stack slot.
*/
static int arm64_postra_cdata_float_hfa_member(CTState *cts,
		const GCtrace *T, IRRef nins, IRRef value,
		IRRef expected_source, const LJArm64ABIArg *arg);

static int arm64_postra_vararg_source_proof(CTState *cts, const GCtrace *T,
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
	unsigned argbase, metaslot, frameslot, cdataslot, source_count, a;
	IRRef meta_fn, cdata;
	GCobj *fn;
	const BCIns *meta_pc;
	LJArm64ABISignature abi;
	if (T == NULL || T->ir == NULL || T->snap == NULL ||
	    T->snapmap == NULL || T->nsnapmap == 0 || proto_bc == NULL ||
	    proto_sizebc == 0 || values == NULL ||
	    xsave >= call || call >= T->nins ||
	    (bc_op(sourceins) != BC_CALL && bc_op(sourceins) != BC_CALLM) ||
	    actual_count < fixed_count ||
	    actual_count > LJ_ARM64_ABI_MAX_ARGS ||
	    !arm64_postra_snapshot_find(T, xsave, &index, &ofs, &nent, &next))
		return 0;
	if (!arm64_callxs_signature_for_call(cts, T->ir, T->nk, T->nins,
	    call, &abi, NULL) || abi.nargs != fixed_count) return 0;
	source_count = abi.nparams+(actual_count-fixed_count);
	if (source_count > LJ_ARM64_ABI_MAX_ARGS ||
	    (actual_count > fixed_count && !abi.is_vararg)) return 0;
	if (!arm64_callxs_xsave_anchor(T->ir, T->nins, xsave,
	    proto_bc, proto_sizebc, &anchored_pc, &anchored_ins) ||
	    anchored_pc != returnpc || anchored_ins != sourceins) return 0;
	UNUSED(next);
	metaslot = (unsigned)bc_a(sourceins)+1u+LJ_FR2;
	frameslot = metaslot+LJ_FR2;
	cdataslot = frameslot+1u;
	argbase = cdataslot+1u;
	if (argbase+source_count > UINT8_MAX ||
	    snap_nslots_acq(&T->snap[index]) < argbase+source_count ||
	    snap_topslot_acq(&T->snap[index]) < argbase+source_count-1u) return 0;
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
	    (ir_load_acq(&T->ir[callable_cdata_ref]).o != IR_SLOAD &&
	     ir_load_acq(&T->ir[callable_cdata_ref]).o != IR_VLOAD) ||
	    irt_type(ir_load_acq(&T->ir[callable_cdata_ref]).t) != IRT_CDATA ||
	    !arm64_postra_snapshot_slot_ref(T, xsave,
	      cdataslot, &cdata) ||
	    cdata != callable_cdata_ref) return 0;
	for (a = 0; a < actual_count; a++) {
		IRRef source;
		IRType stype;
		unsigned source_index = a < fixed_count ?
		  abi.arg[a].source_index : abi.nparams+a-fixed_count;
		if (values[a] >= xsave ||
		    source_index >= source_count ||
		    !arm64_postra_snapshot_slot_ref(T, xsave,
		      argbase+source_index, &source) ||
		    !arm64_callxs_ref_valid(T->ir, T->nk, T->nins, xsave, source) ||
		    !arm64_callxs_ref_valid(T->ir, T->nk, T->nins, xsave, values[a]))
			return 0;
		stype = arm64_callxs_ref_type(T->ir, T->nk, source);
		if (a < fixed_count) {
			IRIns carrier = ir_load_acq(&T->ir[values[a]]);
			if (carrier.o == IR_FLOAD && carrier.op2 == IRFL_CDATA_PTR &&
			    !arm64_postra_pointer_argument(cts, T, values[a])) return 0;
			if (abi.arg[a].kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE) {
				if (!arm64_postra_cdata_integer_aggregate_argument(cts, T,
				    T->nins, values[a], source)) return 0;
			} else if (abi.arg[a].kind ==
				   LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER) {
				if (!arm64_postra_cdata_float_hfa_member(cts, T, T->nins,
				    values[a], source, &abi.arg[a])) return 0;
			} else if (!arm64_postra_source_value_matches(T->ir, T->nk,
			    T->nins, xsave, source, values[a], stype)) return 0;
		} else if (!arm64_postra_vararg_promoted(T->ir, T->nk,
		    T->nins, xsave, source, values[a], stype)) return 0;
	}
	return 1;
}

/* Independent post-RA proof for one HFA float member. */
static int arm64_postra_cdata_float_hfa_member(CTState *cts,
		const GCtrace *T, IRRef nins, IRRef value,
		IRRef expected_source, const LJArm64ABIArg *arg)
{
	const IRIns *ir = T->ir;
	IRIns load, address, src;
	LJArm64FloatHFA hfa;
	LJArm64ABIArg inferred;
	CTypeID source_id = 0, rawid;
	IRRef source, r;
	MSize s, i;
	unsigned member_index;
	unsigned member_size;
	unsigned calls = 0;
	if (value < REF_FIRST || value >= nins) return 0;
	load = ir_load_acq(&ir[value]);
	if (load.o != IR_XLOAD ||
	    (load.t.irt != IRT_FLOAT && load.t.irt != IRT_NUM) ||
	    load.op2 != 0 ||
	    load.op1 < REF_FIRST || load.op1 >= value) return 0;
	member_size = load.t.irt == IRT_FLOAT ? sizeof(float) : sizeof(double);
	address = ir_load_acq(&ir[load.op1]);
	source = address.op1;
	{
	  int64_t offset;
	  if (address.o != IR_ADD || address.t.irt != IRT_P64 ||
	      source < REF_FIRST || source >= load.op1 ||
	      !arm64_callxs_offset_value(ir, T->nk, address.op2, &offset) ||
	      offset < (int64_t)sizeof(GCcdata) ||
	      (offset-(int64_t)sizeof(GCcdata))%(int64_t)member_size != 0 ||
	      (offset-(int64_t)sizeof(GCcdata))/(int64_t)member_size >= 4) return 0;
	  member_index = (unsigned)((offset-(int64_t)sizeof(GCcdata))/
			     (int64_t)member_size);
	}
	if (arg != NULL && (arg->kind != LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER ||
	    arg->size != member_size || arg->member_count == 0 ||
	    arg->member_count > 4 || arg->member_index != member_index)) return 0;
	src = ir_load_acq(&ir[source]);
	if ((src.o != IR_SLOAD && src.o != IR_VLOAD) ||
	    src.t.irt != (IRT_CDATA|IRT_GUARD) ||
	    (expected_source != 0 && source != expected_source))
	  return 0;
	for (r = source+1u; r < value; r++) {
	  IRIns field = ir_load_acq(&ir[r]);
	  IRRef guard;
	  if (field.o != IR_FLOAD || field.op1 != source ||
	      field.op2 != IRFL_CDATA_CTYPEID) continue;
	  if (source_id || field.t.irt != IRT_U16) return 0;
	  for (guard = r+1u; guard < value; guard++) {
	    IRIns eq = ir_load_acq(&ir[guard]);
	    IRRef kref;
	    int32_t id;
	    if (eq.o != IR_EQ || eq.t.irt != (IRT_INT|IRT_GUARD) ||
		(eq.op1 != r && eq.op2 != r)) continue;
	    kref = eq.op1 == r ? eq.op2 : eq.op1;
	    if (!arm64_callxs_kint_value(ir, T->nk, kref, &id) || id <= 0 ||
		 source_id) return 0;
	    source_id = (CTypeID)id;
	  }
	}
	if (!source_id || !lj_ccall_arm64_float_hfa_class(cts, source_id,
	    &hfa, &rawid) || hfa.member_size != member_size ||
	    member_index >= hfa.count) return 0;
	memset(&inferred, 0, sizeof(inferred));
	inferred.kind = LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER;
	inferred.id = rawid;
	inferred.size = (uint8_t)member_size;
	inferred.member_count = hfa.count;
	inferred.member_index = (uint8_t)member_index;
	if (arg == NULL) arg = &inferred;
	if (rawid != arg->id || hfa.count != arg->member_count) return 0;
	for (s = 0; s < T->nsnap; s++) {
	  MSize ofs = snap_mapofs_acq(&T->snap[s]);
	  MSize nent = snap_nent_acq(&T->snap[s]);
	  if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs) return 0;
	  for (i = 0; i < nent; i++)
	    if (snap_ref(snapentry_acq(&T->snapmap[ofs+i])) == value) return 0;
	}
	for (r = value+1u; r < nins; r++) {
	  IRIns use = ir_load_acq(&ir[r]);
	  IRMode mode;
	  if (use.o >= IR__MAX) return 0;
	  mode = lj_ir_mode[use.o];
	  if ((irm_op1(mode) == IRMref && use.op1 == value) ||
	      (irm_op2(mode) == IRMref && use.op2 == value)) {
	    if (use.o != IR_CARG &&
		!(use.o == IR_CALLXS && use.op1 == value)) return 0;
	  }
	  if (use.o == IR_CALLXS) {
	    LJArm64ABISignature abi;
	    IRRef args[LJ_ARM64_ABI_MAX_ARGS];
	    unsigned count, a;
	    if (!arm64_callxs_signature_for_call(cts, ir, T->nk, nins,
		 r, &abi, NULL) ||
		!arm64_callxs_arglist(ir, T->nk, nins, r, args, &count)) return 0;
	    for (a = 0; a < count; a++) if (args[a] == value) {
	      if (a >= abi.nargs || abi.arg[a].id != arg->id ||
		  abi.arg[a].kind != LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER ||
		  abi.arg[a].size != member_size ||
		  abi.arg[a].member_index != arg->member_index ||
		  !arm64_callxs_value_kind(ir, value, T->nk, nins, r,
			LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER, member_size, 0,
			abi.arg[a].reg == LJ_ARM64_ABI_STACK))
		return 0;
	      calls++;
	    }
	  }
	}
	return calls == 1;
}

/* Independently reject raw U32 values in Lua snapshots and all consumers
** other than the exact numeric conversion and certified fixed arguments. */
static int arm64_postra_u32_result_uses(const LJArm64PostRAView *view,
    IRRef nins, IRRef call, IRRef converted)
{
  MSize s, i;
  IRRef r;
  unsigned uses = 0;
  int fixed_args = 0;
  if (ir_load_acq(&view->ir[call]).t.irt != IRT_U32) return 0;
  for (s = 0; s < view->nsnap; s++) {
    MSize ofs = snap_mapofs_acq(&view->snap[s]);
    MSize count = snap_nent_acq(&view->snap[s]);
    if (ofs > view->nsnapmap || count > view->nsnapmap-ofs) return 0;
    for (i = 0; i < count; i++)
      if (snap_ref(snapentry_acq(&view->snapmap[ofs+i])) == call) return 0;
  }
  for (r = call+1u; r < nins; r++) {
    IRIns ins = ir_load_acq(&view->ir[r]);
    IRMode mode;
    if (ins.o >= IR__MAX) return 0;
    mode = lj_ir_mode[ins.o];
    if ((irm_op1(mode) == IRMref && ins.op1 == call) ||
        (irm_op2(mode) == IRMref && ins.op2 == call)) {
      if (r == converted && ins.o == IR_CONV && ins.op1 == call) {
        uses++;
      } else if (!converted || (!fixed_args &&
          !(fixed_args = arm64_postra_numarg_member(view, nins, call))))
        return 0;
    }
  }
  return uses == (converted != 0);
}

static int arm64_postra_table_read_map_slot(const LJArm64PostRAView *v,
    MSize snapno, MSize slot, IRRef expected)
{
  const SnapShot *snap;
  MSize ofs, nent, next, i;
  int found = 0;
  if (v == NULL || v->snap == NULL || v->snapmap == NULL ||
      snapno >= v->nsnap || expected >= v->nins)
    return 0;
  snap = &v->snap[snapno];
  ofs = snap_mapofs_acq(snap);
  nent = snap_nent_acq(snap);
  next = snapno+1u < v->nsnap ? snap_mapofs_acq(&v->snap[snapno+1u]) :
    v->nsnapmap;
  if (ofs > v->nsnapmap || next > v->nsnapmap || next < ofs ||
      nent > next-ofs || next-ofs-nent != 1u+LJ_FR2 ||
      slot >= snap_nslots_acq(snap)) return 0;
  for (i = 0; i < nent; i++) {
    SnapEntry se = snapentry_acq(&v->snapmap[ofs+i]);
    if (snap_slot(se) != slot) continue;
    if (found || (se & 0x00ff0000u) != 0 ||
        snap_ref(se) != expected) return 0;
    found = 1;
  }
  if (found) return 1;

  /* An omitted ordinary local remains in its original interpreter slot.
  ** Accept that fallback only when the rooted input is that exact SLOAD. */
  {
    IRIns src = ir_load_acq(&v->ir[expected]);
    return src.o == IR_SLOAD && src.op1 == slot;
  }
}

static int arm64_postra_table_read_footer(const LJArm64PostRAView *v,
    MSize snapno, MSize *pcpos)
{
  const SnapShot *snap;
  MSize ofs, nent, next;
  SnapEntry footer[1+LJ_FR2];
  uint64_t pcbase;
  uintptr_t lo, hi, pc;
  if (v == NULL || v->proto_bc == NULL || v->proto_sizebc == 0 ||
      v->snap == NULL || v->snapmap == NULL || snapno >= v->nsnap)
    return 0;
  snap = &v->snap[snapno];
  ofs = snap_mapofs_acq(snap);
  nent = snap_nent_acq(snap);
  next = snapno+1u < v->nsnap ? snap_mapofs_acq(&v->snap[snapno+1u]) :
    v->nsnapmap;
  if (ofs > v->nsnapmap || next > v->nsnapmap || next < ofs ||
      nent > next-ofs || next-ofs-nent != 1u+LJ_FR2)
    return 0;
  for (MSize i = 0; i < 1u+LJ_FR2; i++)
    footer[i] = snapentry_acq(&v->snapmap[ofs+nent+i]);
  memcpy(&pcbase, footer, sizeof(pcbase));
  if ((uint8_t)pcbase != 0) return 0;
  lo = (uintptr_t)(const void *)v->proto_bc;
  if (v->proto_sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns)) return 0;
  hi = lo+(uintptr_t)v->proto_sizebc*sizeof(BCIns);
  pc = (uintptr_t)(pcbase >> 8);
  return arm64_ir_pcpos(pc, lo, hi, pcpos);
}

static int arm64_postra_table_read_snapshots(const LJArm64PostRAView *v,
    IRRef n, IRRef readvalue, IRRef root1, IRRef root2, uintptr_t effect_floor)
{
  MSize pre = (MSize)-1, post = (MSize)-1, first_after;
  MSize prepos, postpos, afterpos, table_slot, key_slot, dst_slot;
  MSize s;
  IRIns call, tr, kr, table_src;
  IRRef callref, table_ref, key_ref, previous = 0;
  BCIns source;
  BCOp op;
  uintptr_t sourcepc;
  if (v == NULL || v->ir == NULL || v->owner == NULL ||
      v->snap == NULL || v->snapmap == NULL || v->proto == NULL ||
      readvalue < REF_FIRST+2u || readvalue >= n || n > v->nins ||
      root1 >= readvalue-2u || root2 >= readvalue-2u ||
      v->nsnap < 2 || v->proto_bc == NULL) return 0;
  callref = readvalue-2u;
  call = ir_load_acq(&v->ir[callref]);
  tr = ir_load_acq(&v->ir[root1]);
  kr = ir_load_acq(&v->ir[root2]);
  if (call.o != IR_CALLS ||
      (call.op2 != IRCALL_lj_tab_gettv_rooted_hit_try &&
       call.op2 != IRCALL_lj_tab_getscalar_rooted_try) ||
      tr.o != IR_TMPREF || tr.op1 < REF_FIRST ||
      tr.op2 != (IRTMPREF_IN1|IRTMPREF_OUT1) ||
      kr.o != IR_TMPREF || kr.op1 < v->nk ||
      kr.op2 != IRTMPREF_IN2) return 0;
  table_ref = tr.op1;
  key_ref = kr.op1;
  table_src = ir_load_acq(&v->ir[table_ref]);
  if ((table_src.o != IR_SLOAD && table_src.o != IR_VLOAD) ||
      irt_type(table_src.t) != IRT_TAB ||
      !irt_isguard(table_src.t)) return 0;

  for (s = 0; s < v->nsnap; s++) {
    IRRef at = snap_ref_acq(&v->snap[s]);
    if (at < REF_FIRST || at >= n || (s && at < previous)) return 0;
    previous = at;
    if (at <= readvalue-1u) pre = s;
    if (at == readvalue) {
      if (post != (MSize)-1) return 0;
      post = s;
    }
  }
  if (pre == (MSize)-1 || post == (MSize)-1 || pre >= post ||
      snap_ref_acq(&v->snap[pre]) > readvalue-2u ||
      snap_ref_acq(&v->snap[post]) != readvalue ||
      !arm64_postra_table_read_footer(v, pre, &prepos) ||
      !arm64_postra_table_read_footer(v, post, &postpos) ||
      prepos != postpos) return 0;
  sourcepc = (uintptr_t)(const void *)(v->proto_bc+prepos);
  if (sourcepc < effect_floor) return 0;
  source = (BCIns)la_load32_acq((const uint32_t *)(const void *)sourcepc);
  op = bc_op(source);
  if (op != BC_TGETV && op != BC_TGETS && op != BC_TGETB) return 0;
  if (bc_a(source) >= v->proto->framesize ||
      bc_b(source) >= v->proto->framesize) return 0;
  table_slot = 1u+LJ_FR2+(MSize)bc_b(source);
  dst_slot = 1u+LJ_FR2+(MSize)bc_a(source);
  if (!arm64_postra_table_read_map_slot(v, pre, table_slot, table_ref) ||
      !arm64_postra_table_read_map_slot(v, post, table_slot, table_ref))
    return 0;

  if (op == BC_TGETV) {
    key_slot = 1u+LJ_FR2+(MSize)bc_c(source);
    if (bc_c(source) >= v->proto->framesize ||
        !arm64_postra_table_read_map_slot(v, pre, key_slot, key_ref) ||
        !arm64_postra_table_read_map_slot(v, post, key_slot, key_ref))
      return 0;
  } else {
    IRIns key = ir_load_acq(&v->ir[key_ref]);
    if (op == BC_TGETS) {
      GCobj *expected;
      if (key.o != IR_KGC || irt_type(key.t) != IRT_STR ||
          bc_c(source) >= v->proto->sizekgc) return 0;
      expected = proto_kgc_acq(v->proto, ~(int32_t)bc_c(source));
      if (expected == NULL || !checkptrGC(expected) ||
          expected->gch.gct != (uint32_t)~LJ_TSTR ||
          ir_kgc_load_acq(&v->ir[key_ref]) != expected) return 0;
    } else if (key.o != IR_KINT || key.t.irt != IRT_INT ||
         key.i != (int32_t)bc_c(source)) return 0;
  }

  /* A later completed snapshot must carry the exact generic TValue result
  ** into the destination local; otherwise a later exit can restore stale A. */
  first_after = post+1u;
  if (first_after >= v->nsnap ||
      snap_ref_acq(&v->snap[first_after]) <= readvalue ||
      !arm64_postra_table_read_footer(v, first_after, &afterpos) ||
      afterpos <= prepos) return 0;
  for (s = prepos+1u; s < afterpos; s++)
    if (!arm64_callxs_replay_setup_op(bc_op((BCIns)la_load32_acq(
          (const uint32_t *)&v->proto_bc[s])))) return 0;
  /* Independently bind a folded boolean to the exact type guard. A changed
  ** true/false VLOAD must not restore the other canonical primitive. */
  {
    IRIns result = ir_load_acq(&v->ir[readvalue]);
    if (irt_type(result.t) == IRT_FALSE) readvalue = REF_FALSE;
    else if (irt_type(result.t) == IRT_TRUE) readvalue = REF_TRUE;
  }
  return arm64_postra_table_read_map_slot(v, first_after, dst_slot, readvalue);
}

static int arm64_postra_callxs_generic(const LJArm64PostRAView *view,
	IRRef *semantic_ninsp)
{
	IRRef semantic_nins, ref, active_call = 0, active_enter = 0;
	IRRef active_leave = 0;
	IRRef active_xsave = 0, producer_call = 0, completed_producer = 0;
	IRRef enterargs[3] = { 0, 0, 0 };
	IRRef lookupargs[3] = { 0, 0, 0 };
	IRRef current1 = 0, current2 = 0, readvalue = 0, outputroot = 0;
	IRRef scalar_read = 0;
	IRRef boxref = 0;
	CTypeID box_ctype = 0;
	int box_store_seen = 0, result_is_cdata = 0;
	unsigned box_store_lanes = 0;
	unsigned result_hfa_count = 0, result_hfa_offset[4] = { 0, 0, 0, 0 };
	int result_is_u32 = 0;
	IRRef number_result = 0;
	MSize result_slot = 0;
	uintptr_t xsave_returnpc = 0, producer_returnpc = 0, read_floor = 0;
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
	      IRRef value, number_source;
	      int numeric_arg;
	      if (actual_count == 0 || completed_producer == 0 ||
		  producer_type == IRT_NIL ||
		  !arm64_callxs_callm_pair(view->proto_bc, view->proto_sizebc,
		    producer_returnpc, xsave_returnpc, actual_count)) return 0;
	      value = values[actual_count-1u];
	      carrier = ir_load_acq(&view->ir[value]);
	      number_source = arm64_postra_numarg_source(view->ir, view->nk,
		semantic_nins, value, 0);
	      numeric_arg = (producer_type == IRT_NUM &&
		number_source == completed_producer) || (producer_type == IRT_INT &&
		arm64_postra_intarg_source(view->ir, view->nk, semantic_nins,
		  value) == completed_producer);
	      if (producer_type == IRT_FLOAT && number_source) {
		IRIns widen = ir_load_acq(&view->ir[number_source]);
		numeric_arg = widen.t.irt == IRT_NUM && widen.o == IR_CONV &&
		  widen.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT) &&
		  widen.op1 == completed_producer;
	      }
	      numeric_arg = numeric_arg && actual_count == abi.nargs &&
		abi.arg[actual_count-1u].kind == LJ_ARM64_ABI_KIND_INTEGER &&
		arm64_callxs_value_kind(view->ir, value, view->nk, semantic_nins, ref,
		  LJ_ARM64_ABI_KIND_INTEGER, abi.arg[actual_count-1u].size,
		  abi.arg[actual_count-1u].is_unsigned,
		  abi.arg[actual_count-1u].reg == LJ_ARM64_ABI_STACK);
	      if (value != completed_producer && !numeric_arg &&
		  (carrier.o != IR_CONV || carrier.op1 != completed_producer ||
		   !((irt_type(carrier.t) == IRT_FLOAT &&
		      (producer_type == IRT_NUM || producer_type == IRT_INT) &&
		      carrier.op2 == ((IRT_FLOAT << IRCONV_DSH)|producer_type) &&
		      carrier.t.irt == IRT_FLOAT) ||
		     (irt_type(carrier.t) == IRT_NUM && producer_type == IRT_FLOAT &&
		      carrier.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT) &&
		      carrier.t.irt == IRT_NUM)))) return 0;
	    } else if (bc_op(sourceins) != BC_CALL || bc_c(sourceins) == 0 ||
		       (unsigned)bc_c(sourceins)-1u != abi.nparams+
		         (actual_count-abi.nargs)) return 0;
	    producer_call = ref;
	    producer_pending = 1;
	    producer_scalar = !arm64_callxs_result_boxed(&abi) &&
	      bc_op(sourceins) == BC_CALL && bc_b(sourceins) == 0 &&
	      abi.nparams+(actual_count-abi.nargs)+1u ==
		(unsigned)bc_c(sourceins) &&
	      (abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER ||
	       abi.result_kind == LJ_ARM64_ABI_KIND_FLOAT ||
	       abi.result_kind == LJ_ARM64_ABI_KIND_ENUM);
	    producer_type = (IRType)irt_type(ir_load_acq(&view->ir[ref]).t);
	    if (fpref != enterargs[1])
	      return 0;
	    if (arm64_callxs_result_boxed(&abi) ?
		(boxref == 0 || enterargs[2] != boxref ||
		 box_ctype != arm64_callxs_box_ctype(&abi)) :
		(boxref != 0 || !arm64_callxs_const_member(view->ir, view->nk, enterargs[2]) ||
		 ir_load_acq(&view->ir[enterargs[2]]).o != IR_KNULL ||
		 arm64_callxs_ref_type(view->ir, view->nk, enterargs[2]) != IRT_CDATA)) return 0;
	    result_is_cdata = arm64_callxs_result_boxed(&abi);
	    result_hfa_count = abi.result_hfa_count;
	    for (i = 0; i < 4; i++)
	      result_hfa_offset[i] = abi.result_hfa_offset[i];
	    if ((abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE ||
	 abi.result_kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER) &&
		!arm64_callxs_result_not_snapshotted(view->snap, view->snapmap,
		  view->nsnap, view->nsnapmap, ref,
		  abi.result_hfa_count > 1 ?
		    ref+abi.result_hfa_count-1u : 0)) return 0;
	    result_is_u32 = abi.result_class == LJ_ARM64_ABI_GPR &&
	      abi.result_kind == LJ_ARM64_ABI_KIND_INTEGER &&
	      abi.result_size == 4 && abi.result_unsigned;
	    number_result = 0;
	    result_slot = 0;
	    if (result_is_cdata) {
	      MSize base = 1u+LJ_FR2+(MSize)bc_a(sourceins)+1u+LJ_FR2;
	      if (!arm64_callxs_snapshot_value_slot(view->snap, view->snapmap,
		    view->nsnap, view->nsnapmap, active_xsave, base,
		    base+1u+abi.nparams+(actual_count-abi.nargs), boxref, 1)) return 0;
	      if (bc_b(sourceins) != 1)
		result_slot = 1u+LJ_FR2+(MSize)bc_a(sourceins);
	    }
	    if (!result_is_cdata &&
	        (result_is_u32 || irt_type(ins.t) == IRT_INT ||
	         irt_type(ins.t) == IRT_NUM ||
	         irt_type(ins.t) == IRT_FLOAT) && bc_b(sourceins) != 1)
	      result_slot = 1u+LJ_FR2+(MSize)bc_a(sourceins);
	    if (!arm64_postra_callxs_gc_roots(view, semantic_nins, ref,
	          active_xsave, boxref, fpref, actual_count, values)) return 0;
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
	    if (result_hfa_count > 1 &&
		box_store_lanes != ((1u << result_hfa_count)-1u)) return 0;
	    if (result_is_cdata &&
		!arm64_callxs_snapshot_value_slot(view->snap, view->snapmap,
		  view->nsnap, view->nsnapmap, ref, 1u+LJ_FR2,
		  result_slot, boxref, 0)) return 0;
	    if (result_is_u32 &&
		((result_slot && (!number_result ||
		  !arm64_postra_value_survives_call(view, number_result,
		    ref, capacity, 0))) ||
		 !arm64_postra_u32_result_uses(view, semantic_nins,
		   active_call, number_result) ||
		 !arm64_callxs_snapshot_value_slot(view->snap, view->snapmap,
		   view->nsnap, view->nsnapmap, ref, 1u+LJ_FR2, result_slot,
		   number_result ? number_result : active_call, 0))) return 0;
	    if (!result_is_cdata &&
	        (irt_type(view->ir[active_call].t) == IRT_INT ||
	        irt_type(view->ir[active_call].t) == IRT_NUM ||
	        irt_type(view->ir[active_call].t) == IRT_FLOAT)) {
	      IRRef lua_result = number_result ? number_result : active_call;
	      if ((result_slot &&
	           ((irt_type(view->ir[active_call].t) == IRT_FLOAT &&
	             !number_result) ||
	            !arm64_postra_value_survives_call(view, lua_result,
	              ref, capacity, 0))) ||
	          !arm64_callxs_snapshot_value_slot(view->snap, view->snapmap,
	            view->nsnap, view->nsnapmap, ref, 1u+LJ_FR2, result_slot,
	            lua_result, 0)) return 0;
	    }
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
	      completed_producer = result_is_u32 ? number_result : producer_call;
	      producer_returnpc = xsave_returnpc;
	      if (result_is_u32) producer_type = IRT_NUM;
	    } else {
	      completed_producer = 0;
	      producer_returnpc = 0;
	      producer_type = IRT_NIL;
	    }
	    producer_pending = 0;
	    active_xsave = 0;
	    protocol = 0;
	    read_floor = xsave_returnpc;
	    boxref = 0;
	    box_ctype = 0;
	    box_store_seen = 0;
	    box_store_lanes = 0;
	    result_hfa_count = 0;
	    memset(result_hfa_offset, 0, sizeof(result_hfa_offset));
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
	             (ins.op2 == IRCALL_lj_tab_gettv_rooted_hit_try ||
	              ins.op2 == IRCALL_lj_tab_getscalar_rooted_try)) {
	    IRIns table, r1, r2;
	    IRType key, result;
	    if (protocol != 0 || !current1 || !current2 ||
	        arm64_rooted_postra_effect(view, semantic_nins, ref,
	          current1, current2, 0) != 2) return 0;
	    r1 = ir_load_acq(&view->ir[current1]);
	    r2 = ir_load_acq(&view->ir[current2]);
	    table = ir_load_acq(&view->ir[r1.op1]);
	    key = arm64_callxs_ref_type(view->ir, view->nk, r2.op1);
	    result = irt_type(ir_load_acq(&view->ir[ref+2u]).t);
	    if (r1.op1 < REF_FIRST ||
	        (table.o != IR_SLOAD && table.o != IR_VLOAD) ||
	        irt_type(table.t) != IRT_TAB ||
	        !irt_isguard(table.t) ||
	        (table.o == IR_SLOAD && table.op2 != IRSLOAD_TYPECHECK) ||
	        (key != IRT_INT && key != IRT_NUM && key != IRT_STR &&
	         key != IRT_FALSE && key != IRT_TRUE && key != IRT_TAB &&
	         key != IRT_FUNC && key != IRT_CDATA) ||
	        (result != IRT_CDATA && result != IRT_TAB &&
	         result != IRT_INT && result != IRT_NUM &&
	         result != IRT_FALSE && result != IRT_TRUE) ||
	        !arm64_postra_table_read_snapshots(view, semantic_nins,
	          ref+2u, current1, current2, read_floor))
	      return 0;
	    readvalue = ref+2u;
	    outputroot = current1;
	  } else if (ins.o == IR_CALLS &&
		     ins.op2 == IRCALL_lj_tab_gettv_rooted) {
	    if (protocol != 0 || irt_type(ins.t) != IRT_P64 ||
		!irt_isguard(ins.t) ||
		!arm64_callxs_carg_values(view->ir, view->nk, semantic_nins,
			ref, 3, lookupargs))
	      return 0;
	    if (lookupargs[0] != current1 || lookupargs[1] != current2 ||
	        lookupargs[2] != lookupargs[0] ||
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
	    current1 = current2 = 0;
	    read_floor = 0;
	    completed_producer = 0;
	    producer_returnpc = 0;
	    producer_type = IRT_NIL;
	  } else if (ins.o == IR_XPOLL) {
	    if (protocol != 0 || ins.t.irt != (IRT_NIL|IRT_GUARD) ||
		ins.op1 != 1 || ins.op2 != 0)
	      return 0;
	    npoll++;
	    current1 = current2 = 0;
	    read_floor = 0;
	    completed_producer = 0;
	    producer_returnpc = 0;
	    producer_type = IRT_NIL;
	  } else if (ins.o == IR_BASE) {
	    if (ref != REF_BASE || ins.t.irt != IRT_PGC || ins.op1 || ins.op2)
	      return 0;
	  } else if (ins.o == IR_NOP) {
	    if (ins.t.irt != IRT_NIL || ins.op1 || ins.op2)
	      return 0;
	  } else if (ins.o == IR_XLOAD) {
	    if (protocol != 0 ||
		!((ins.t.irt == IRT_FLOAT || ins.t.irt == IRT_NUM) ?
		  arm64_postra_cdata_float_hfa_member(view->cts, &temp,
		    semantic_nins, ref, 0, NULL) :
		  arm64_postra_cdata_integer_aggregate_argument(view->cts, &temp,
		    semantic_nins, ref, 0)) || !ra_hasreg(ins.r) ||
		!(irt_isfp(ins.t) ?
		  ins.r >= RID_MIN_FPR && ins.r < RID_MAX_FPR &&
		    rset_test(RSET_FPR, ins.r) :
		  arm64_scalar_postra_register(ins.r, irt_type(ins.t)))) return 0;
	  } else if (ins.o == IR_FLOAD) {
	    if (ins.op1 >= ref || !arm64_callxs_ref_valid(view->ir, view->nk,
		semantic_nins, ref, ins.op1))
	      return 0;
	    if (ins.op2 == IRFL_CDATA_INT64 || ins.op2 == IRFL_CDATA_INT) {
	      if (protocol != 0 ||
	          !arm64_postra_cdata_integer_argument(view, semantic_nins, ref))
	        return 0;
	    } else if (ins.op2 == IRFL_CDATA_PTR) {
	      IRIns src = ir_load_acq(&view->ir[ins.op1]);
	      if ((src.o != IR_SLOAD && src.o != IR_VLOAD) ||
	        irt_type(src.t) != IRT_CDATA ||
		irt_type(ins.t) != IRT_P64)
	        return 0;
	    } else if (ins.op2 == IRFL_CDATA_CTYPEID) {
	      IRIns src = ir_load_acq(&view->ir[ins.op1]);
	      if ((src.o != IR_SLOAD && src.o != IR_VLOAD) ||
	        irt_type(src.t) != IRT_CDATA ||
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
	    if (irt_type(ins.t) == IRT_CDATA || irt_type(ins.t) == IRT_TAB ||
	        irt_type(ins.t) == IRT_INT || irt_type(ins.t) == IRT_NUM ||
	        irt_type(ins.t) == IRT_FALSE || irt_type(ins.t) == IRT_TRUE) {
	      /* Preserve the independently certified read and destination;
	      ** the call still needs its exact scalar ABI carrier proof. */
	      if (protocol != 0 || ref != readvalue ||
	          ins.op1 != outputroot || ins.op2 || !irt_isguard(ins.t))
	        return 0;
	      if (!scalar_read && irt_type(ins.t) != IRT_CDATA &&
	          irt_type(ins.t) != IRT_TAB) scalar_read = ref;
	      readvalue = outputroot = 0;
	    } else {
	    if (irt_type(ins.t) != IRT_FUNC || !irt_isguard(ins.t) ||
		ins.op2 != 0 || ins.op1 >= ref ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1))
	      return 0;
	    src = ir_load_acq(&view->ir[ins.op1]);
	    if (src.o != IR_CALLS || src.op2 != IRCALL_lj_tab_gettv_rooted ||
		!arm64_callxs_has_equal_guard(view->ir, semantic_nins, view->nk,
		  ref, IRT_FUNC))
	      return 0;
	    }
	  } else if (ins.o == IR_TMPREF) {
	    if (ins.t.irt != IRT_PGC ||
		(ins.op2 & ~(IRTMPREF_IN1|IRTMPREF_OUT1|IRTMPREF_IN2|IRTMPREF_OUT2)) != 0 ||
		(ins.op2 & (IRTMPREF_IN1|IRTMPREF_OUT1|IRTMPREF_IN2|IRTMPREF_OUT2)) == 0 ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1))
	      return 0;
	    if (ins.op2 & (IRTMPREF_IN1|IRTMPREF_OUT1)) current1 = ref;
	    if (ins.op2 & (IRTMPREF_IN2|IRTMPREF_OUT2)) current2 = ref;
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
		((irt_type(ins.t) == IRT_FALSE || irt_type(ins.t) == IRT_TRUE) &&
		 (ins.t.irt != (irt_type(ins.t)|IRT_GUARD) ||
		  ins.op2 != IRSLOAD_TYPECHECK)) ||
		(irt_type(ins.t) != IRT_INT && irt_type(ins.t) != IRT_NUM &&
		 irt_type(ins.t) != IRT_FALSE && irt_type(ins.t) != IRT_TRUE &&
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
	    int numeric_arg, integer_arg;
	    if (ins.op1 >= ref || !arm64_callxs_ref_valid(view->ir, view->nk,
		semantic_nins, ref, ins.op1))
	      return 0;
	    src = ir_load_acq(&view->ir[ins.op1]);
	    numeric_arg = arm64_postra_numarg_source(view->ir, view->nk,
	      semantic_nins, ref, 0) != 0;
	    if (numeric_arg && (protocol != 0 ||
	        !arm64_postra_numarg_member(view, semantic_nins, ref))) return 0;
	    integer_arg = !numeric_arg && arm64_postra_intarg_source(view->ir,
	      view->nk, semantic_nins, ref) != 0;
	    if (integer_arg && (protocol != 0 ||
	        !arm64_postra_intarg_member(view, semantic_nins, ref))) return 0;
	    if (!numeric_arg && !integer_arg && irt_type(src.t) != from) return 0;
	    if (numeric_arg || integer_arg) {
	      /* Exact fixed-argument chain, including its native-only uses. */
	    } else if (dst == IRT_NUM && from == IRT_U32 &&
		(ins.t.irt & ~IRT_ISPHI) == IRT_NUM &&
		ins.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_U32) &&
		protocol == 4 && result_is_u32 && ins.op1 == active_call &&
		src.t.irt == IRT_U32 && !number_result) {
	      number_result = ref;
	    } else if (dst == IRT_NUM && from == IRT_FLOAT &&
	        ins.t.irt == IRT_NUM && src.t.irt == IRT_FLOAT &&
	        ins.op2 == ((IRT_NUM << IRCONV_DSH)|IRT_FLOAT) && protocol == 4 &&
	        irt_type(view->ir[active_call].t) == IRT_FLOAT &&
	        ins.op1 == active_call && !number_result) {
	      number_result = ref;
	    } else if (protocol == 0 && ins.t.irt == (IRT_INT|IRT_GUARD) &&
		from == IRT_NUM && ins.op2 == (IRCONV_INT_NUM|IRCONV_CHECK)) {
	      /* Exact checked NUM narrowing; each source has its own IR proof. */
	    } else if ((ins.t.irt & ~(IRT_TYPE|IRT_ISPHI)) != 0 ||
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
	  } else if (ins.o == IR_HIOP) {
	    IRIns low;
	    IRRef lane;
	    int32_t lane_tag;
	    if (protocol != 4 || result_hfa_count < 2 ||
		active_call < REF_FIRST || active_call >= ref ||
		ref-active_call >= result_hfa_count) return 0;
	    lane = ref-active_call;
	    low = ir_load_acq(&view->ir[active_call]);
	    if (lane == 0 || ins.op1 != active_call ||
		!arm64_callxs_kint_value(view->ir, view->nk, ins.op2,
		                         &lane_tag) || lane_tag != (int32_t)lane ||
		!irt_isfp(ins.t) ||
		irt_type(ins.t) != irt_type(low.t) ||
		(ins.t.irt & ~(IRT_TYPE|IRT_ISPHI)) != 0 ||
		!ra_hasreg(low.r) || low.r != RID_FPRET ||
		!ra_hasreg(ins.r) || ins.r != (Reg)(RID_D0+lane))
	      return 0;
	  } else if (ins.o == IR_XSTORE) {
	    IRIns payload;
	    int64_t offset;
	    unsigned member = 0;
	    if (protocol != 4 || !result_is_cdata || boxref == 0 ||
		ins.op1 >= ref || ins.op2 >= ref ||
		!arm64_callxs_ref_valid(view->ir, view->nk, semantic_nins, ref, ins.op1))
	      return 0;
	    payload = ir_load_acq(&view->ir[ins.op1]);
	    if (payload.o != IR_ADD || irt_type(payload.t) != IRT_P64 ||
		payload.op1 != boxref ||
		!arm64_callxs_offset_value(view->ir, view->nk, payload.op2, &offset) ||
		irt_type(ins.t) != irt_type(ir_load_acq(&view->ir[active_call]).t))
	      return 0;
	    if (result_hfa_count > 1) {
	      for (member = 0; member < result_hfa_count; member++)
		if (offset == (int64_t)sizeof(GCcdata)+
		    result_hfa_offset[member]) break;
	      if (member == result_hfa_count ||
		  ins.op2 != active_call+member ||
		  (member > 0 &&
		   ir_load_acq(&view->ir[ins.op2]).o != IR_HIOP)) return 0;
	    } else if (offset != (int64_t)sizeof(GCcdata) ||
		ins.op2 != active_call) return 0;
	    if (member == 0) {
	      if (box_store_seen) return 0;
	      box_store_seen = 1;
	    }
	    if (member >= 4 || (box_store_lanes & (1u << member))) return 0;
	    box_store_lanes |= 1u << member;
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
	  if (ins.o == IR_CALLS) current1 = current2 = 0;
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
	      (ir_load_acq(&view->ir[cdref]).o != IR_SLOAD &&
	       ir_load_acq(&view->ir[cdref]).o != IR_VLOAD) ||
	      irt_type(ir_load_acq(&view->ir[cdref]).t) != IRT_CDATA)
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
		  ((abi.is_vararg && (CTypeID)wrapper_id == abi.function_id) ||
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
		(unsigned)bc_c(sourceins)-1u != abi.nparams+tail_count) return 0;
	    layout_producer_returnpc = bc_b(sourceins) == 0 ? source_returnpc : 0;
	  } else return 0;
	  {
	    int roots_required = (scalar_read && scalar_read < call) ||
	      abi.is_vararg || bc_op(sourceins) == BC_CALLM ||
	      ir_load_acq(&view->ir[cdref]).o == IR_VLOAD;
	    for (i = 0; i < abi.nargs; i++) {
	      IRIns arg = ir_load_acq(&view->ir[values[i]]);
	      if ((arg.o == IR_FLOAD && (arg.op2 == IRFL_CDATA_INT64 ||
	                               arg.op2 == IRFL_CDATA_INT ||
	                               arg.op2 == IRFL_CDATA_PTR)) ||
		          abi.arg[i].kind == LJ_ARM64_ABI_KIND_INTEGER_AGGREGATE ||
		          abi.arg[i].kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER ||
	          arm64_postra_numarg_source(view->ir, view->nk, semantic_nins,
	            values[i], 0) ||
	          arm64_postra_intarg_source(view->ir, view->nk, semantic_nins,
	            values[i]) ||
	          (abi.arg[i].kind == LJ_ARM64_ABI_KIND_INTEGER &&
	           abi.arg[i].size == 4 && abi.arg[i].is_unsigned &&
	           irt_type(arg.t) == IRT_INT))
	        roots_required = 1;
	    }
	    if (roots_required && !arm64_postra_vararg_source_proof(view->cts, &temp,
	        layout_xsave, call, view->proto_bc, view->proto_sizebc,
	        source_returnpc, sourceins, cdref, abi.nargs, actual_count, values))
	      return 0;
	  }
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
	      if (fp && a->reg == LJ_ARM64_ABI_STACK) {
		if (a->kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER) {
		  unsigned first = guard-a->member_index, part;
		  LJArm64ABIArg *head;
		  if (a->member_count == 0 || a->member_index >= a->member_count ||
		      first > guard || first+a->member_count > abi.nargs) return 0;
		  head = &abi.arg[first];
		  for (part = 0; part < a->member_count; part++) {
		    LJArm64ABIArg *member = &abi.arg[first+part];
		    if (member->kind != LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER ||
			member->cls != LJ_ARM64_ABI_FPR ||
			member->reg != LJ_ARM64_ABI_STACK ||
			member->id != head->id ||
			member->source_index != head->source_index ||
			member->member_count != a->member_count ||
			member->member_index != part ||
			member->size != head->size ||
			member->stack_offset != head->stack_offset+part*head->size)
		      return 0;
		  }
		  if (nfpr < limit) nfpr = limit;
		} else if (nfpr < limit) return 0;
	      }
	      if (*bank < limit) {
		if (a->reg == LJ_ARM64_ABI_STACK || a->reg != (*bank)++)
		  return 0;
	      } else {
		if (width == 0 || width > 8 || width != a->size ||
		    a->reg != LJ_ARM64_ABI_STACK) return 0;
		if (a->kind == LJ_ARM64_ABI_KIND_FLOAT_HFA_MEMBER &&
		    a->member_index == 0)
		  bytes = (bytes+7u) & ~7u;
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
	       !(c.r >= RID_X19 && c.r <= RID_X28) &&
	       !(c.r == RID_NONE && c.s == SPS_NONE &&
		 !ra_hasspill(regsp_spill(c.prev)) &&
		 arm64_postra_callxs_result_unused(view, semantic_nins, call))) ||
	      /* ra_destreg moves X0/S0/D0 into an already allocated destination.
	      ** Held results may use a preserved register across leave/enter and
	      ** later calls. Independent liveness and snapshot checks still apply. */
	      (abi.result_class == LJ_ARM64_ABI_FPR && c.r != RID_FPRET &&
	       !(c.r >= RID_D8 && c.r <= RID_D15)))
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
  if (rootop != BC_FORL && rootop != BC_ITERL && rootop != BC_FUNCF)
    return 0;
  if (rootop == BC_FUNCF)
    return arm64_postra_funcf_admit(view, view->startins, 0, semantic_ninsp);
  if (!arm64_postra_constants(view, 0, 0))
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
    case IR_ULT: case IR_UGE: case IR_ULE: case IR_UGT:
      if (slot != SPS_NONE) return 0;
      if (ins.t.irt == (IRT_INT|IRT_GUARD)) {
        if (ins.o >= IR_ULT ||
            !arm64_scalar_postra_ref(view, ins.op1, ref, IRT_INT) ||
            !arm64_scalar_postra_ref(view, ins.op2, ref, IRT_INT)) return 0;
      } else if (ins.t.irt == (IRT_NUM|IRT_GUARD)) {
        if ((ins.o < IR_LT || ins.o > IR_UGT) ||
            !arm64_scalar_postra_ref(view, ins.op1, ref, IRT_NUM) ||
            !arm64_scalar_postra_ref(view, ins.op2, ref, IRT_NUM)) return 0;
      } else return 0;
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

/* A constant KKPTR may remain after folding an immutable string byte load.
** It has no executable meaning if no surviving IR operand, allocator rename,
** or snapshot names it. Keep that residue out of the pointer capability: a
** live KKPTR is still rejected by both constant validators. */
static int arm64_ir_kkptr_unused(const GCtrace *T, IRRef kref)
{
  IRRef ref;
  SnapNo s;
  MSize maxmaps;
  if (!T || !T->ir || !T->snap || !T->snapmap || T->nsnap == 0 ||
      T->nsnap > T->nins)
    return 0;
  maxmaps = (MSize)T->nsnap * (LJ_MAX_JSLOTS + 1u + LJ_FR2);
  if (T->nsnapmap > maxmaps) return 0;
  for (ref = REF_FIRST; ref < T->nins; ref++)
    if (T->ir[ref].op1 == kref || T->ir[ref].op2 == kref ||
	T->ir[ref].op1 == kref+LJ_GC64 ||
	T->ir[ref].op2 == kref+LJ_GC64)
      return 0;
  for (s = 0; s < T->nsnap; s++) {
    const SnapShot *snap = &T->snap[s];
    MSize ofs = snap->mapofs, nent = snap->nent, i;
    if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs) return 0;
    for (i = 0; i < nent; i++)
      if (snap_ref(T->snapmap[ofs+i]) == kref ||
	  snap_ref(T->snapmap[ofs+i]) == kref+LJ_GC64)
	return 0;
  }
  return 1;
}

static int arm64_ir_constants(const GCtrace *T, int allow_num,
			      int allow_func, LJArm64IRReject *reject)
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
    if (ir->o == IR_KNUM && allow_num && ir->t.irt == IRT_NUM) {
	if (ref+1u >= REF_TRUE)
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT, ref,
				 (IROp)ir->o, 0);
	ref++;
	continue;
	}
	if (ir->o == IR_KGC && allow_func && ir->t.irt == IRT_FUNC) {
	  if (ref+1u >= REF_TRUE)
	    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT, ref,
				   (IROp)ir->o, 0);
	  ref++;
	  continue;
	}
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
static int arm64_side_immediate_parent_valid(
	const LJArm64SideIRView *side, const GCtrace *owner);
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
	view->owner->root != view->semantic.root ||
	!arm64_side_scalar_topology(&view->semantic, view->owner) ||
	!arm64_side_immediate_parent_valid(&view->semantic, view->owner) ||
	view->owner->sinktags ||
	view->owner->startins != BCINS_AD(BC_JMP, 0, 0) ||
	trace_startpc_acq((GCtrace *)view->owner) !=
	  view->semantic.parent_continuation ||
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
      !arm64_ir_funcf_bytecode(pt, T->startins, T->startins, 1) ||
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
      pt, proto_bc(pt), pt->sizebc, pt->numparams, pt->framesize,
      J->baseslot, poll, (uint8_t)(J->baseslot-2u)))
    goto bad_funcf;
  return 1;
 bad_funcf:
  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND, ref,
	(IROp)ir[ref < T->nins ? ref : REF_BASE].o, BC_FUNCF);
}

/* Distinguish the one fixed CALL grammar from the scalar-only FUNCF path.
** The detailed source/identity/result proof is performed after the leaf
** replay helpers are defined; this is only a dispatch predicate. */
static int arm64_ir_funcf_leaf_call_source(const GCproto *pt)
{
  MSize pos, calls = 0, gets = 0;
  if (!pt || (pt->flags & PROTO_VARARG) || pt->sizebc < 2u ||
      pt->sizebc > UINT8_MAX)
    return 0;
  for (pos = 1; pos+1u < pt->sizebc; pos++) {
    BCIns ins = (BCIns)la_load32_acq(
      (const uint32_t *)&proto_bc(pt)[pos]);
    calls += bc_op(ins) == BC_CALL || bc_op(ins) == BC_CALLT;
    gets += bc_op(ins) == BC_UGET;
  }
  calls += bc_op(arm64_ir_bc_acq((uintptr_t)proto_bc(pt),
				       pt->sizebc-1u)) == BC_CALLT;
  return calls == 1u && gets == 1u;
}

/* Exact one-argument CALLT source for a scalar captured leaf. The argument
** must be copied from the sole fixed parameter into the ABI call window; the
** returned value resumes into the CALLT function/result slot. */
static int arm64_ir_funcf_tail_one_result_source(const GCproto *pt,
							 MSize *callposp)
{
  MSize pos, gets = 0, callpos = 0;
  BCReg getdest = UINT32_MAX, argdest = UINT32_MAX;
  if (!arm64_ir_funcf_leaf_call_source(pt) || pt->sizebc < 4u ||
	pt->framesize > UINT8_MAX || pt->numparams != 1u ||
	(pt->flags & PROTO_VARARG)) return 0;
  for (pos = 1; pos+1u < pt->sizebc; pos++) {
	BCIns ins = arm64_ir_bc_acq((uintptr_t)proto_bc(pt), pos);
	if (bc_op(ins) == BC_UGET) {
	  gets++;
	  getdest = bc_a(ins);
	} else if (bc_op(ins) == BC_CGET && bc_d(ins) == 0u) {
	  if (argdest != UINT32_MAX) return 0;
	  argdest = bc_a(ins);
	}
  }
  {
	BCIns call = arm64_ir_bc_acq((uintptr_t)proto_bc(pt), pt->sizebc-1u);
	if (bc_op(call) != BC_CALLT || bc_b(call) != 0u || bc_c(call) != 2u ||
	    bc_a(call) != getdest ||
	    argdest != (BCReg)(bc_a(call)+1u+LJ_FR2) ||
	    (MSize)bc_a(call)+bc_c(call)+LJ_FR2 > pt->framesize)
	  return 0;
	callpos = pt->sizebc-1u;
  }
  if (gets != 1u) return 0;
  if (callposp) *callposp = callpos;
  return 1;
}

static int arm64_ir_funcf_tail_child_return(const GCfunc *callee)
{
	const GCproto *pt;
	BCIns ret;
	if (!callee || !isluafunc(callee)) return 0;
	pt = funcproto(callee);
	if (!pt || pt->sizebc == 0 || pt->framesize == 0) return 0;
	ret = arm64_ir_bc_acq((uintptr_t)proto_bc(pt), pt->sizebc-1u);
	return bc_op(ret) == BC_RET1 && bc_d(ret) == 2u &&
	       bc_a(ret) < pt->framesize;
}

static int arm64_ir_funcf_tail_result_map(const jit_State *J,
					 const GCtrace *T, SnapNo s,
					 uint64_t pcbase)
{
	MSize callpos, i, markers = 0, outputs = 0, childfunc = 0;
  const SnapShot *snap;
	const GCfunc *callee = NULL;
	MSize target_slot;
	if (!J || !T || s+1u != T->nsnap || (uint8_t)pcbase ||
	    !arm64_ir_funcf_tail_one_result_source(J->pt, &callpos) ||
	    ((uintptr_t)(pcbase >> 8) & (sizeof(BCIns)-1u)))
    return 0;
  snap = &T->snap[s];
	/* CALLT's final exit is at the specialized child's RET1, while its
	** pre-call snapshot remains rooted in the root prototype. Resolve that
	** child only from the exact function slot in the root CALLT snapshot. */
	target_slot = J->baseslot + bc_a(arm64_ir_bc_acq(
	  (uintptr_t)proto_bc(J->pt), callpos));
	for (i = 0; i+1u < T->nsnap; i++) {
	  const SnapShot *candidate = &T->snap[i];
	  MSize ofs = candidate->mapofs, nent = candidate->nent, j;
	  uint64_t footer;
	  if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs ||
	      T->nsnapmap-ofs-nent < 1u+LJ_FR2) return 0;
	  memcpy(&footer, &T->snapmap[ofs+nent], sizeof(footer));
	  if ((uint8_t)footer ||
	      !arm64_ir_pcpos((uintptr_t)(footer >> 8),
	        (uintptr_t)proto_bc(J->pt),
	        (uintptr_t)(proto_bc(J->pt)+J->pt->sizebc), &j) || j != callpos)
	    continue;
	  for (j = 0; j < nent; j++) {
	    SnapEntry e = T->snapmap[ofs+j];
	    if (snap_slot(e) == target_slot) {
	      IRRef guard = 0;
	      if (callee) return 0;
	      callee = arm64_lua_leaf_ir_function(T, snap_ref(e),
	        T->snap[T->nsnap-1u].ref, &guard);
	      if (!callee || (guard && guard >= T->snap[T->nsnap-1u].ref))
		return 0;
	    }
	  }
	}
	if (!callee || !lj_trace_arm64_lua_leaf(callee) ||
	    !arm64_ir_funcf_tail_child_return(callee) ||
	    !arm64_ir_pcpos((uintptr_t)(pcbase >> 8),
	      (uintptr_t)proto_bc(funcproto(callee)),
	      (uintptr_t)(proto_bc(funcproto(callee))+funcproto(callee)->sizebc), &i) ||
	    i != funcproto(callee)->sizebc-1u)
	  return 0;
	if (snap->topslot != funcproto(callee)->framesize ||
      snap->nslots > J->pt->framesize+1u+LJ_FR2 ||
      target_slot >= snap->nslots || snap->nslots < 1u+LJ_FR2 ||
      snap->mapofs > T->nsnapmap ||
      snap->nent > T->nsnapmap-snap->mapofs) return 0;
	for (i = 0; i < snap->nent; i++) {
	  SnapEntry e = T->snapmap[T->snap[s].mapofs+i];
	  IRRef value = snap_ref(e);
	  if (snap_slot(e) == 0 && !(e & 0x00ff0000u) &&
	      irref_isk(value) && value >= T->nk && value < REF_TRUE &&
	      T->ir[value].o == IR_KGC &&
	      ir_kgc_acq(&T->ir[value]) == obj2gco((GCfunc *)callee)) childfunc++;
	  if (e == SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL)) markers++;
	  if (snap_slot(e) == target_slot && !(e & 0x00ff0000u)) outputs++;
	}
	if (markers != 1u || outputs != 1u || childfunc != 1u) return 0;
	return 1;
}

static int arm64_postra_funcf_tail_result_map(
					 const LJArm64PostRAView *v,
					 MSize s, uint64_t pcbase)
{
	MSize callpos, i, ofs, nent, markers = 0, outputs = 0, childfunc = 0,
	  target_slot;
	const GCfunc *callee = NULL;
	if (!v || s+1u != v->nsnap || (uint8_t)pcbase ||
	    !arm64_ir_funcf_tail_one_result_source(v->proto, &callpos) ||
	    ((uintptr_t)(pcbase >> 8) & (sizeof(BCIns)-1u)))
    return 0;
	target_slot = 1u+LJ_FR2+bc_a(arm64_ir_bc_acq(
	  (uintptr_t)v->proto_bc, callpos));
	for (i = 0; i+1u < v->nsnap; i++) {
	  const SnapShot *candidate = &v->snap[i];
	  MSize cofs = snap_mapofs_acq(candidate), cnent = snap_nent_acq(candidate), j;
	  uint64_t footer;
	  if (cofs > v->nsnapmap || cnent > v->nsnapmap-cofs ||
	      v->nsnapmap-cofs-cnent < 1u+LJ_FR2) return 0;
	  memcpy(&footer, &v->snapmap[cofs+cnent], sizeof(footer));
	  if ((uint8_t)footer ||
	      !arm64_ir_pcpos((uintptr_t)(footer >> 8),
	        (uintptr_t)v->proto_bc,
	        (uintptr_t)(v->proto_bc+v->proto_sizebc), &j) || j != callpos)
	    continue;
	  for (j = 0; j < cnent; j++) {
	    SnapEntry e = snapentry_acq(&v->snapmap[cofs+j]);
	    if (snap_slot(e) == target_slot) {
	      if (callee) return 0;
	      IRRef guard = 0;
	      callee = arm64_lua_leaf_postra_function(v, snap_ref(e),
	        snap_ref_acq(&v->snap[v->nsnap-1u]), &guard);
	      if (!callee || (guard && guard >= snap_ref_acq(&v->snap[v->nsnap-1u])))
		return 0;
	    }
	  }
	}
	if (!callee || !lj_trace_arm64_lua_leaf(callee) ||
	    !arm64_ir_funcf_tail_child_return(callee) ||
	    !arm64_ir_pcpos((uintptr_t)(pcbase >> 8),
	      (uintptr_t)proto_bc(funcproto(callee)),
	      (uintptr_t)(proto_bc(funcproto(callee))+funcproto(callee)->sizebc), &i) ||
	    i != funcproto(callee)->sizebc-1u)
	  return 0;
  ofs = snap_mapofs_acq(&v->snap[s]);
  nent = snap_nent_acq(&v->snap[s]);
	if (snap_topslot_acq(&v->snap[s]) != funcproto(callee)->framesize ||
      snap_nslots_acq(&v->snap[s]) > v->proto->framesize+1u+LJ_FR2 ||
      target_slot >= snap_nslots_acq(&v->snap[s]) ||
      snap_nslots_acq(&v->snap[s]) < 1u+LJ_FR2 ||
      ofs > v->nsnapmap || nent > v->nsnapmap-ofs) return 0;
	for (i = 0; i < nent; i++) {
	  SnapEntry e = snapentry_acq(&v->snapmap[ofs+i]);
	  IRRef value = snap_ref(e);
	  if (snap_slot(e) == 0 && !(e & 0x00ff0000u) &&
	      irref_isk(value) && value >= v->nk && value < REF_TRUE &&
	      ir_load_acq(&v->ir[value]).o == IR_KGC &&
	      ir_kgc_load_acq(&v->ir[value]) == obj2gco((GCfunc *)callee))
	    childfunc++;
	  if (e == SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL)) markers++;
	  if (snap_slot(e) == target_slot && !(e & 0x00ff0000u)) outputs++;
	}
	return markers == 1u && outputs == 1u && childfunc == 1u;
}

/* The CALLT identity guard is attached to the root CALLT snapshot, but its
** physical frame is later replaced by the specialized child. Bind the guard
** to the exact function slot in that source snapshot; the independent tail
** replay below binds that same slot to the terminal child return map. */
static int arm64_ir_funcf_tail_guard_snapshot(const jit_State *J,
						      const GCtrace *T,
						      IRRef ref, SnapNo s)
{
	const SnapShot *snap;
	const IRIns *guard;
	MSize callpos, target_slot, i, found = 0, pcpos;
	uint64_t pcbase;
	GCfunc *callee;
	if (!J || !T || ref >= T->nins || s >= T->nsnap ||
	    !arm64_ir_funcf_tail_one_result_source(J->pt, &callpos)) return 0;
	guard = &T->ir[ref];
	snap = &T->snap[s];
	if (snap->ref != ref || guard->o != IR_EQ ||
	    guard->t.irt != (IRT_FUNC|IRT_GUARD) ||
	    guard->op1 < REF_FIRST || guard->op1 >= ref ||
	    !irref_isk(guard->op2) || guard->op2 < T->nk ||
	    guard->op2 >= REF_TRUE || T->ir[guard->op2].o != IR_KGC ||
	    snap->mapofs > T->nsnapmap || snap->nent > T->nsnapmap-snap->mapofs ||
	    T->nsnapmap-snap->mapofs-snap->nent < 1u+LJ_FR2) return 0;
	callee = gco2func(ir_kgc_acq(&T->ir[guard->op2]));
	if (!isluafunc(callee) || !lj_trace_arm64_lua_leaf(callee)) return 0;
	memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], sizeof(pcbase));
	if ((uint8_t)pcbase ||
	    !arm64_ir_pcpos((uintptr_t)(pcbase>>8),
	      (uintptr_t)proto_bc(J->pt),
	      (uintptr_t)(proto_bc(J->pt)+J->pt->sizebc), &pcpos) ||
	    pcpos != callpos) return 0;
	target_slot = J->baseslot+bc_a(arm64_ir_bc_acq(
	  (uintptr_t)proto_bc(J->pt), callpos));
	for (i = 0; i < snap->nent; i++) {
	  SnapEntry entry = T->snapmap[snap->mapofs+i];
	  if (snap_slot(entry) == target_slot) {
	    if (++found != 1u || (entry&0x00ff0000u) ||
		snap_ref(entry) != guard->op1) return 0;
	  }
	}
	return found == 1u;
}

static int arm64_postra_funcf_tail_guard_snapshot(
						 const LJArm64PostRAView *v,
						 IRRef ref, MSize s)
{
	const SnapShot *snap;
	IRIns guard;
	MSize callpos, target_slot, i, found = 0, ofs, nent, pcpos;
	uint64_t pcbase;
	GCfunc *callee;
	if (!v || !v->owner || ref >= v->nins || s >= v->nsnap ||
	    !arm64_ir_funcf_tail_one_result_source(v->proto, &callpos)) return 0;
	guard = ir_load_acq(&v->ir[ref]);
	snap = &v->snap[s];
	ofs = snap_mapofs_acq(snap); nent = snap_nent_acq(snap);
	if (snap_ref_acq(snap) != ref || guard.o != IR_EQ ||
	    guard.t.irt != (IRT_FUNC|IRT_GUARD) ||
	    guard.op1 < REF_FIRST || guard.op1 >= ref ||
	    !irref_isk(guard.op2) || guard.op2 < v->nk ||
	    guard.op2 >= REF_TRUE || ir_load_acq(&v->ir[guard.op2]).o != IR_KGC ||
	    ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
	    v->nsnapmap-ofs-nent < 1u+LJ_FR2) return 0;
	callee = gco2func(ir_kgc_load_acq(&v->ir[guard.op2]));
	if (!isluafunc(callee) || !lj_trace_arm64_lua_leaf(callee)) return 0;
	memcpy(&pcbase, &v->snapmap[ofs+nent], sizeof(pcbase));
	if ((uint8_t)pcbase ||
	    !arm64_ir_pcpos((uintptr_t)(pcbase>>8),
	      (uintptr_t)v->proto_bc,
	      (uintptr_t)(v->proto_bc+v->proto_sizebc), &pcpos) ||
	    pcpos != callpos) return 0;
	target_slot = 1u+LJ_FR2+bc_a(arm64_ir_bc_acq(
	  (uintptr_t)v->proto_bc, callpos));
	for (i = 0; i < nent; i++) {
	  SnapEntry entry = snapentry_acq(&v->snapmap[ofs+i]);
	  if (snap_slot(entry) == target_slot) {
	    if (++found != 1u || (entry&0x00ff0000u) ||
		snap_ref(entry) != guard.op1) return 0;
	  }
	}
	return found == 1u;
}

static int arm64_lua_leaf_ir_eval_multi(const jit_State *J, const GCtrace *T,
	const GCproto *pt, const IRRef *args, MSize pc, IRRef before,
	IRRef end, const GCfunc **ancestors, MSize depth, IRRef *slots,
	IRRef *results, MSize *nresultsp);
static int arm64_lua_leaf_ir_eval(const jit_State *J, const GCtrace *T,
	const GCproto *pt, const IRRef *args, MSize pc, IRRef before,
	IRRef end, const GCfunc **ancestors, MSize depth, IRRef *slots,
	IRRef *result);
static int arm64_lua_leaf_ir_needed(const GCproto *pt, MSize resume,
	uint8_t *needed);

/* The current nonleaf FUNCF slice has one captured callee, one fixed
** one-argument/one-result CALL and one scalar RET1. */
static int arm64_ir_funcf_one_result_leaf_source(const GCproto *pt,
						 MSize *retposp)
{
  MSize pos, calls = 0, gets = 0, callpos = 0, getdest = UINT32_MAX;
  BCIns ret;
  if (!arm64_ir_funcf_leaf_call_source(pt) || pt->sizebc < 4u ||
	pt->framesize > UINT8_MAX || pt->numparams > pt->framesize)
	return 0;
  for (pos = 1; pos+1u < pt->sizebc; pos++) {
	BCIns ins = arm64_ir_bc_acq((uintptr_t)proto_bc(pt), pos);
	if (bc_op(ins) == BC_UGET) {
	  gets++;
	  getdest = bc_a(ins);
	} else if (bc_op(ins) == BC_CALL) {
	  calls++;
	  callpos = pos;
	  if (bc_b(ins) != 2u || bc_c(ins) != 2u ||
		  (MSize)bc_a(ins)+bc_c(ins)+LJ_FR2 > pt->framesize)
	    return 0;
	}
  }
  ret = arm64_ir_bc_acq((uintptr_t)proto_bc(pt), pt->sizebc-1u);
  if (calls != 1u || gets != 1u ||
	getdest != bc_a(arm64_ir_bc_acq((uintptr_t)proto_bc(pt), callpos)) ||
	bc_op(ret) != BC_RET1 || bc_d(ret) != 2u ||
	bc_a(ret) >= pt->framesize)
	return 0;
  if (retposp) *retposp = pt->sizebc-1u;
  return 1;
}

/* A one-argument captured CALL may request the child's two fixed results,
** then feed both to a scalar continuation in the root FUNCF. */
static int arm64_ir_funcf_pair_call_leaf_source(const GCproto *pt,
						MSize *retposp)
{
  MSize pos, calls = 0, gets = 0, callpos = 0, getdest = UINT32_MAX;
  BCIns ret;
  if (!arm64_ir_funcf_leaf_call_source(pt) || pt->sizebc < 5u ||
	pt->framesize > UINT8_MAX || pt->numparams > pt->framesize)
	return 0;
  for (pos = 1; pos+1u < pt->sizebc; pos++) {
	BCIns ins = arm64_ir_bc_acq((uintptr_t)proto_bc(pt), pos);
	if (bc_op(ins) == BC_UGET) { gets++; getdest = bc_a(ins); }
	else if (bc_op(ins) == BC_CALL) {
	  calls++; callpos = pos;
	  if (bc_b(ins) != 3u || bc_c(ins) != 2u ||
	      (MSize)bc_a(ins)+bc_c(ins)+LJ_FR2 > pt->framesize ||
	      (MSize)bc_a(ins)+2u+LJ_FR2 > pt->framesize)
	    return 0;
	}
  }
  ret = arm64_ir_bc_acq((uintptr_t)proto_bc(pt), pt->sizebc-1u);
  if (calls != 1u || gets != 1u ||
	getdest != bc_a(arm64_ir_bc_acq((uintptr_t)proto_bc(pt), callpos)) ||
	bc_op(ret) != BC_RET1 || bc_d(ret) != 2u ||
	bc_a(ret) >= pt->framesize) return 0;
  if (retposp) *retposp = pt->sizebc-1u;
  return 1;
}

/* A FUNCF root with an inlined fixed leaf call has ordinary root snapshots:
** the child's body is replayed into caller slots and no SNAP_FRAME is needed.
** Independently derive each snapshot from the root bytecode and the guarded
** root arguments, then require every source-live local to be represented. */
static int arm64_ir_funcf_slot_unchanged(const GCproto *pt, MSize slot,
					 MSize pc)
{
  MSize p;
  for (p = 1; p < pc && p+1u < pt->sizebc; p++) {
    BCIns ins = arm64_ir_bc_acq((uintptr_t)proto_bc(pt), p);
    BCOp op = bc_op(ins);
    if (bc_a(ins) == slot &&
	(op == BC_CGET || op == BC_MOV || op == BC_CSET ||
	 op == BC_KSHORT || op == BC_KNUM || op == BC_KPRI ||
	 op == BC_UGET || op == BC_CALL ||
	 (op >= BC_ADDVN && op <= BC_DIVVV)))
      return 0;
  }
  return 1;
}

static int arm64_ir_funcf_leaf_source_replay(const jit_State *J,
						    const GCtrace *T,
						    IRRef end)
{
  const GCproto *pt = J->pt;
  IRRef args[UINT8_MAX+1u] = {0};
  const GCfunc *ancestors[1] = {0};
  MSize retpos, p, s;
  unsigned return_maps = 0;
	if (arm64_ir_funcf_tail_one_result_source(pt, NULL)) {
	  return arm64_ir_funcf_tail_source_replay(J, T, end);
	}
  if ((!arm64_ir_funcf_one_result_leaf_source(pt, &retpos) &&
       !arm64_ir_funcf_pair_call_leaf_source(pt, &retpos)) ||
      pt->framesize > UINT8_MAX || pt->numparams > UINT8_MAX ||
      J->baseslot != 1u+LJ_FR2)
    return 0;
  for (p = 0; p < pt->numparams; p++) {
    IRRef r, found = 0;
    MSize slot = J->baseslot+p;
    for (r = REF_FIRST; r < end; r++) {
      const IRIns *ins = &T->ir[r];
      IRType type = irt_type(ins->t);
      if (ins->o == IR_SLOAD && ins->op1 == slot &&
          ins->op2 == IRSLOAD_TYPECHECK &&
          (type == IRT_INT || type == IRT_NUM) &&
          arm64_ir_type_flags(ins->t, type, IRT_GUARD,
			      IRT_GUARD|IRT_ISPHI)) {
	if (found) return 0;
	found = r;
      }
    }
    if (!found) return 0;
    args[p] = found;
  }
  for (s = 0; s < T->nsnap; s++) {
    const SnapShot *snap = &T->snap[s];
    MSize next = s+1u < T->nsnap ? T->snap[s+1u].mapofs : T->nsnapmap;
    MSize i, pcpos = 0;
    uint64_t pcbase;
    IRRef source[UINT8_MAX+1u] = {0}, result = 0;
    uint8_t needed[UINT8_MAX+1u] = {0}, seen[UINT8_MAX+1u] = {0};
    if (snap->mapofs > T->nsnapmap || next < snap->mapofs ||
        next > T->nsnapmap ||
	 snap->nent > next-snap->mapofs ||
	 next-snap->mapofs-snap->nent != 1u+LJ_FR2)
      return 0;
    memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], sizeof(pcbase));
    if ((uint8_t)pcbase ||
	!arm64_ir_pcpos((uintptr_t)(pcbase>>8),
	  (uintptr_t)proto_bc(pt),
	  (uintptr_t)(proto_bc(pt)+pt->sizebc), &pcpos) ||
	!arm64_lua_leaf_ir_eval(J, T, pt, args, pcpos, snap->ref, end,
	  ancestors, 0, source, &result) ||
	!arm64_lua_leaf_ir_needed(pt, pcpos, needed))
      return 0;
    for (i = 0; i < snap->nent; i++) {
      SnapEntry entry = T->snapmap[snap->mapofs+i];
      MSize slot = snap_slot(entry), local;
      if (entry & (SNAP_FRAME|SNAP_NORESTORE)) return 0;
      if (slot < J->baseslot || slot-J->baseslot >= pt->framesize) continue;
      local = slot-J->baseslot;
      if (source[local] && snap_ref(entry) != source[local]) return 0;
      if (needed[local]) {
	if (!source[local] || seen[local]) return 0;
	seen[local] = 1;
      }
    }
    for (i = 0; i < pt->framesize; i++)
      if (needed[i] && !seen[i] &&
	  !(i < pt->numparams && arm64_ir_funcf_slot_unchanged(pt, i, pcpos))) {
        return 0;
      }
    if (pcpos == retpos) {
      BCIns ret = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[retpos]);
      unsigned found = 0;
      for (i = 0; i < snap->nent; i++) {
	SnapEntry entry = T->snapmap[snap->mapofs+i];
	if (snap_slot(entry) == J->baseslot+bc_a(ret)) {
	  if (snap_ref(entry) != result ||
	      !arm64_scalar_ir_ref(T, result, snap->ref, IRT_NUM)) return 0;
	  found++;
	}
      }
      if (found != 1u) return 0;
      return_maps++;
    }
  }
  return return_maps == 1u;
}

/* Replay the narrow terminal CALLT shape independently from its snapshot
** maps. The pre-call map must retain the exact guarded function and argument;
** the final frame sentinel map must bind the child's return to CALLT's result
** slot. No ordinary CALL continuation is inferred for this path. */
static int arm64_ir_funcf_tail_source_replay(const jit_State *J,
						    const GCtrace *T,
						    IRRef end)
{
  const GCproto *pt = J ? J->pt : NULL;
  IRRef args[UINT8_MAX+1u] = {0};
  const GCfunc *ancestors[1] = {0};
  MSize callpos, p, s, argslot, result_slot;
  unsigned call_maps = 0, result_maps = 0;
  BCIns call;
  if (!J || !T || !pt || J->baseslot != 1u+LJ_FR2 ||
      pt->numparams != 1u || pt->framesize > UINT8_MAX ||
      end > T->nins ||
      !arm64_ir_funcf_tail_one_result_source(pt, &callpos)) return 0;
  call = arm64_ir_bc_acq((uintptr_t)proto_bc(pt), callpos);
  argslot = J->baseslot+bc_a(call)+1u+LJ_FR2;
  result_slot = J->baseslot+bc_a(call);
  for (p = 0; p < pt->numparams; p++) {
    IRRef r, found = 0;
    MSize slot = J->baseslot+p;
    for (r = REF_FIRST; r < end; r++) {
      const IRIns *ins = &T->ir[r];
      IRType type = irt_type(ins->t);
      if (ins->o == IR_SLOAD && ins->op1 == slot &&
          ins->op2 == IRSLOAD_TYPECHECK &&
          (type == IRT_INT || type == IRT_NUM) &&
          arm64_ir_type_flags(ins->t, type, IRT_GUARD,
			      IRT_GUARD|IRT_ISPHI)) {
	if (found) return 0;
	found = r;
      }
    }
    if (!found) return 0;
    args[p] = found;
  }
  for (s = 0; s < T->nsnap; s++) {
    const SnapShot *snap = &T->snap[s];
    MSize next = s+1u < T->nsnap ? T->snap[s+1u].mapofs : T->nsnapmap;
    MSize i, pcpos;
    uint64_t pcbase;
    IRRef source[UINT8_MAX+1u] = {0}, result = 0;
    uint8_t needed[UINT8_MAX+1u] = {0}, seen[UINT8_MAX+1u] = {0};
    int terminal;
    if (snap->mapofs > T->nsnapmap || next < snap->mapofs ||
	 next > T->nsnapmap || snap->nent > next-snap->mapofs ||
	 next-snap->mapofs-snap->nent != 1u+LJ_FR2) return 0;
    memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], sizeof(pcbase));
    terminal = arm64_ir_funcf_tail_result_map(J, T, s, pcbase);
    if ((uint8_t)pcbase) return 0;
    if (terminal) pcpos = callpos;
    else if (!arm64_ir_pcpos((uintptr_t)(pcbase>>8),
	  (uintptr_t)proto_bc(pt),
	  (uintptr_t)(proto_bc(pt)+pt->sizebc), &pcpos)) return 0;
	if (!arm64_lua_leaf_ir_eval(J, T, pt, args, pcpos, snap->ref, end,
	  ancestors, 0, source, &result)) return 0;
    if (pcpos == 1u) {
      needed[0] = 1;
    } else if (pcpos == 2u) {
      needed[0] = needed[bc_a(call)] = 1;
    } else if (pcpos == callpos) {
      needed[bc_a(call)] = needed[bc_a(call)+1u+LJ_FR2] = 1;
    } else {
      return 0;
    }
    if (terminal) {
      unsigned marker = 0, output = 0, locals = 0;
      if (snap->ref != T->nins-1u || pcpos != callpos ||
	  !arm64_scalar_ir_ref(T, result, snap->ref, IRT_NUM)) return 0;
      for (i = 0; i < snap->nent; i++) {
	SnapEntry entry = T->snapmap[snap->mapofs+i];
	MSize slot = snap_slot(entry);
	if (entry == SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL)) {
	  marker++;
	  continue;
	}
	if (slot >= J->baseslot && slot-J->baseslot < pt->framesize) {
	  locals++;
	  if (slot == result_slot && snap_ref(entry) == result &&
	      !(entry & 0x00ff0000u)) output++;
	}
      }
      if (marker != 1u || output != 1u || locals != 1u) return 0;
      result_maps++;
      continue;
    }
    if (pcpos == callpos) {
      unsigned function_seen = 0, argument_seen = 0;
      if (snap->ref >= T->nins-1u) return 0;
      for (i = 0; i < snap->nent; i++) {
	SnapEntry entry = T->snapmap[snap->mapofs+i];
	MSize slot = snap_slot(entry), local;
	if (entry & (SNAP_FRAME|SNAP_NORESTORE)) return 0;
	if (slot < J->baseslot || slot-J->baseslot >= pt->framesize) continue;
	local = slot-J->baseslot;
	if (source[local] && snap_ref(entry) != source[local]) return 0;
	if (needed[local]) {
	  if (!source[local] || seen[local]) return 0;
	  seen[local] = 1;
	}
	if (slot == result_slot && source[local] &&
	    snap_ref(entry) == source[local]) function_seen++;
	if (slot == argslot && source[local] &&
	    snap_ref(entry) == source[local]) argument_seen++;
      }
      if (function_seen != 1u || argument_seen != 1u) return 0;
      for (i = 0; i < pt->framesize; i++)
	if (needed[i] && !seen[i]) return 0;
      call_maps++;
    } else {
      for (i = 0; i < snap->nent; i++) {
	SnapEntry entry = T->snapmap[snap->mapofs+i];
	MSize slot = snap_slot(entry), local;
	if (entry & (SNAP_FRAME|SNAP_NORESTORE)) return 0;
	if (slot < J->baseslot || slot-J->baseslot >= pt->framesize) continue;
	local = slot-J->baseslot;
	if (source[local] && snap_ref(entry) != source[local]) return 0;
	if (needed[local]) {
	  if (!source[local] || seen[local]) return 0;
	  seen[local] = 1;
	}
      }
      for (i = 0; i < pt->framesize; i++)
	if (needed[i] && !seen[i] &&
	    !(i < pt->numparams &&
	      arm64_ir_funcf_slot_unchanged(pt, i, pcpos))) return 0;
    }
  }
  return call_maps == 1u && result_maps == 1u;
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
    if (ir->o == IR_ADD && ref < loopref && ir->op1 == idxload &&
	ir->t.irt == (IRT_INT|IRT_ISPHI)) {
      if (preadd != 0)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			       ref, IR_ADD, 1);
      preadd = ref;
    } else if (ir->o == IR_ADD && ref > xpollref &&
	       (!firstphi || ref < firstphi) && ir->op1 == preadd &&
	       ir->t.irt == (IRT_INT|IRT_ISPHI)) {
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
      (idx.t.irt & ~IRT_ISPHI) != (IRT_INT|IRT_GUARD))
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
static int arm64_string_byte_bounds_ir(const GCtrace *T, IRRef strref,
                                       IRRef str, IRRef offset);
static int arm64_string_byte_dynamic_bounds_ir(const GCtrace *T,
                                               IRRef strref, IRRef str,
                                               IRRef offset);
static int arm64_string_byte_dynamic_index_ir(const GCtrace *T, IRRef conv);
static int arm64_string_byte_call_decode(const BCIns *bc, MSize sizebc,
                                         MSize framesize, MSize pc,
                                         BCReg source[2], int cell[2])
{
  BCIns call, arg;
  unsigned i;
  if (!bc || pc < 2 || pc >= sizebc) return 0;
  call = arm64_ir_bc_acq((uintptr_t)bc, pc);
  if (bc_op(call) != BC_CALL || bc_b(call) != 2 || bc_c(call) != 3 ||
      (MSize)bc_a(call)+3u+LJ_FR2 > framesize) return 0;
  for (i = 0; i < 2; i++) {
    BCReg target = (BCReg)(bc_a(call)+1u+LJ_FR2+i);
    arg = arm64_ir_bc_acq((uintptr_t)bc, pc-2u+i);
    if (bc_a(arg) != target ||
        (bc_op(arg) != BC_MOV && bc_op(arg) != BC_CGET)) return 0;
    source[i] = bc_d(arg);
    cell[i] = bc_op(arg) == BC_CGET;
  }
  return 1;
}

/* The source-slot proof is repeated by the semantic and post-RA validators.
** This shared routine only decodes the exact fixed CALL and its two adjacent
** argument-copy instructions. */
static int arm64_string_byte_call_slots_ir(const GCproto *pt, MSize strslot,
                                           MSize indexslot)
{
  const BCIns *bc;
  MSize pc, sizebc;
  unsigned calls = 0, matches = 0;
  if (!pt || strslot < 1u+LJ_FR2 || indexslot < 1u+LJ_FR2) return 0;
  bc = proto_bc(pt); sizebc = pt->sizebc;
  for (pc = 0; pc < sizebc; pc++) {
    BCIns call = arm64_ir_bc_acq((uintptr_t)bc, pc);
    BCReg source[2]; int cell[2];
    unsigned i; int valid = 1;
    if (bc_op(call) != BC_CALL || bc_b(call) != 2 || bc_c(call) != 3)
      continue;
    calls++;
    if (!arm64_string_byte_call_decode(bc, sizebc, pt->framesize, pc,
                                       source, cell)) continue;
    if (1u+LJ_FR2+(MSize)source[0] != strslot ||
        1u+LJ_FR2+(MSize)source[1] != indexslot ||
        source[0] >= pt->numparams || source[1] >= pt->numparams)
      continue;
    for (i = 0; i < 2 && valid; i++) {
      MSize q;
      for (q = 0; q < pc; q++) {
        BCIns ins = arm64_ir_bc_acq((uintptr_t)bc, q);
        BCOp op = bc_op(ins);
        BCMode mode = bcmode_a(op);
        if (((mode == BCMdst || mode == BCMvar) && bc_a(ins) == source[i]) ||
            ((mode == BCMbase || mode == BCMrbase) &&
             bc_a(ins) <= source[i])) { valid = 0; break; }
        if (bcmode_d(op) == BCMjump) {
          int64_t target = (int64_t)q+1+(int64_t)bc_j(ins);
          if (target == (int64_t)pc-1 || target == (int64_t)pc) {
            valid = 0; break;
          }
        }
      }
      if (cell[i]) for (q = 0; q < sizebc; q++) {
        BCIns ins = arm64_ir_bc_acq((uintptr_t)bc, q);
        if (bc_op(ins) == BC_CSET && bc_a(ins) == source[i]) {
          valid = 0; break;
        }
      }
    }
    if (!valid) continue;
    matches++;
  }
  return calls == 1 && matches == 1;
}

static int arm64_string_byte_call_slots_postra(const GCproto *pt,
                                               MSize strslot,
                                               MSize indexslot)
{
  const BCIns *bc;
  MSize pc, sizebc;
  unsigned calls = 0, matches = 0;
  if (!pt || strslot < 1u+LJ_FR2 || indexslot < 1u+LJ_FR2) return 0;
  bc = proto_bc(pt); sizebc = pt->sizebc;
  for (pc = 0; pc < sizebc; pc++) {
    BCIns call = arm64_ir_bc_acq((uintptr_t)bc, pc);
    BCReg source[2]; int cell[2];
    unsigned i; int valid = 1;
    if (bc_op(call) != BC_CALL || bc_b(call) != 2 || bc_c(call) != 3)
      continue;
    calls++;
    if (!arm64_string_byte_call_decode(bc, sizebc, pt->framesize, pc,
                                       source, cell)) continue;
    if (1u+LJ_FR2+(MSize)source[0] != strslot ||
        1u+LJ_FR2+(MSize)source[1] != indexslot ||
        source[0] >= pt->numparams || source[1] >= pt->numparams)
      continue;
    for (i = 0; i < 2 && valid; i++) {
      MSize q;
      for (q = 0; q < pc; q++) {
        BCIns ins = arm64_ir_bc_acq((uintptr_t)bc, q);
        BCOp op = bc_op(ins);
        BCMode mode = bcmode_a(op);
        if (((mode == BCMdst || mode == BCMvar) && bc_a(ins) == source[i]) ||
            ((mode == BCMbase || mode == BCMrbase) &&
             bc_a(ins) <= source[i])) { valid = 0; break; }
        if (bcmode_d(op) == BCMjump) {
          int64_t target = (int64_t)q+1+(int64_t)bc_j(ins);
          if (target == (int64_t)pc-1 || target == (int64_t)pc) {
            valid = 0; break;
          }
        }
      }
      if (cell[i]) for (q = 0; q < sizebc; q++) {
        BCIns ins = arm64_ir_bc_acq((uintptr_t)bc, q);
        if (bc_op(ins) == BC_CSET && bc_a(ins) == source[i]) {
          valid = 0; break;
        }
      }
    }
    if (!valid) continue;
    matches++;
  }
  return calls == 1 && matches == 1;
}

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
      if (ref >= REF_FIRST && ref < snapref &&
          T->ir[ref].o == IR_XLOAD && T->ir[ref].t.irt == IRT_U8)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
			       ref, IR_XLOAD, (uint16_t)snapno);
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
  GCobj *startpt;
  GCproto *pt;
  IRRef ref, loopref = 0, xpollref = 0, firstphi = 0;
  MSize maxslots = 0;
  MSize root_topslot;
  MSize forl_idxslot = 0;
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
  if (!arm64_ir_constants(T, startop == BC_FUNCF,
	  startop == BC_FUNCF && arm64_ir_funcf_leaf_call_source(pt), reject))
    return 0;
  if (startop == BC_FUNCF) {
    if (arm64_ir_funcf_leaf_call_source(pt))
      return arm64_scalar_ir_admit_mode(J, T, BC_FUNCF, NULL, reject);
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
    case IR_ULT: case IR_UGE: case IR_ULE: case IR_UGT:
      if (ir->t.irt == (IRT_INT|IRT_GUARD)) {
        if (ir->o >= IR_ULT || !arm64_ir_int_binary(T, ir, ref, 1))
          return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
                                (IROp)ir->o, ir->t.irt);
      } else if (ir->t.irt == (IRT_NUM|IRT_GUARD)) {
        if (ir->o < IR_LT || ir->o > IR_UGT ||
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_NUM) ||
            !arm64_scalar_ir_ref(T, ir->op2, ref, IRT_NUM))
          return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
                                (IROp)ir->o, ir->t.irt);
      } else {
        return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
                              (IROp)ir->o, ir->t.irt);
      }
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

/* Exact builtin identities whose operations have independent IR proofs. */
static int arm64_scalar_builtin_ffid(uint32_t ffid)
{
  return ffid == FF_rawset || lj_ff_is_bit_scalar(ffid) ||
         ffid == FF_math_floor || ffid == FF_math_ceil ||
         ffid == FF_math_sqrt || ffid == FF_math_abs ||
         ffid == FF_math_min || ffid == FF_math_max ||
         ffid == FF_string_byte || ffid == FF_math_log ||
         ffid == FF_math_log10 || ffid == FF_math_exp ||
         ffid == FF_math_sin || ffid == FF_math_cos || ffid == FF_math_tan ||
         ffid == FF_math_asin || ffid == FF_math_acos || ffid == FF_math_atan ||
         ffid == FF_math_sinh || ffid == FF_math_cosh || ffid == FF_math_tanh;
}

/* Fixed unary, NUM -> NUM helpers. These do not allocate Lua objects, enter
** the VM, release its native phase lease, or access shared Lua state. */
static int arm64_scalar_math_call(unsigned id)
{
  switch ((IRCallID)id) {
  case IRCALL_log10: case IRCALL_exp: case IRCALL_sin: case IRCALL_cos:
  case IRCALL_tan: case IRCALL_asin: case IRCALL_acos: case IRCALL_atan:
  case IRCALL_sinh: case IRCALL_cosh: case IRCALL_tanh:
    return 1;
  default:
    return 0;
  }
}

/* LOG/LOG2 additionally require the allocated call-clobber proof. */
static int arm64_scalar_fpmath(uint32_t op)
{
  return op == IRFPM_FLOOR || op == IRFPM_CEIL || op == IRFPM_SQRT ||
         op == IRFPM_LOG || op == IRFPM_LOG2;
}

/* ABS and numeric NEG carry exact aligned global SIMD masks in op2. */
static int arm64_scalar_ksimd_fload(IRIns ir, unsigned index)
{
  size_t offset = offsetof(GG_State, g.ksimd) + index*16u;
  LJ_STATIC_ASSERT((offsetof(GG_State, g.ksimd) & 15u) == 0);
  return index < LJ_KSIMD__MAX && ir.o == IR_FLOAD &&
         ir.t.irt == IRT_NUM && ir.op1 == REF_NIL && ir.op2 == offset/4u;
}

static int arm64_scalar_ksimd_ir_ref(const GCtrace *T, IRRef value,
                                    IRRef before, unsigned index)
{
  return value >= REF_FIRST && value < before &&
         arm64_scalar_ksimd_fload(T->ir[value], index);
}

static int arm64_string_byte_xload_ir(const GCtrace *T, IRRef ref)
{
  const IRIns *load, *ptr;
  if (ref < REF_FIRST || ref >= T->nins) return 0;
  load = &T->ir[ref];
  if (load->o != IR_XLOAD || load->t.irt != IRT_U8 ||
      load->op2 != IRXLOAD_READONLY || load->op1 < REF_FIRST ||
      load->op1 >= ref) return 0;
  ptr = &T->ir[load->op1];
  return ptr->o == IR_STRREF && ptr->t.irt == IRT_PGC &&
         arm64_string_byte_bounds_ir(T, load->op1, ptr->op1, ptr->op2);
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
        return T->ir[k].o != IR_KKPTR && irt_type(T->ir[k].t) == type;
      if (T->ir[k].o == IR_KNUM || T->ir[k].o == IR_KGC ||
	  T->ir[k].o == IR_KKPTR)
        k++;
    }
    return 0;
  }
  if (value < REF_FIRST || value >= before)
    return 0;
  ir = &T->ir[value];
  if (ir->o == IR_CALLN)
    return type == IRT_NUM && (ir->t.irt & ~IRT_ISPHI) == IRT_NUM &&
           arm64_scalar_math_call(ir->op2);
  /* XLOAD U8 is zero-extended by LDRB and is safe as a Lua INT operand. */
  if (type == IRT_INT && irt_type(ir->t) == IRT_U8 && ir->o == IR_XLOAD)
    return arm64_scalar_ir_ref(T, value, before, IRT_U8);
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
  if (type == IRT_PGC && ir->o == IR_STRREF)
    return ir->t.irt == IRT_PGC &&
      arm64_string_byte_bounds_ir(T, value, ir->op1, ir->op2);
  if (type == IRT_U8 && ir->o == IR_XLOAD)
    return arm64_string_byte_xload_ir(T, value);
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
  case IR_ABS:
  case IR_MIN:
  case IR_MAX:
  case IR_FPMATH:
  case IR_CONV:
  case IR_BNOT: case IR_BSWAP:
  case IR_BAND: case IR_BOR: case IR_BXOR:
  case IR_BSHL: case IR_BSHR: case IR_BSAR: case IR_BROR:
  case IR_NEG: case IR_TOBIT:
    return 1;
  case IR_FLOAD:
    return type == IRT_INT && ir->op2 == IRFL_STR_LEN &&
           arm64_scalar_ir_ref(T, ir->op1, value, IRT_STR);
  default:
    return 0;
  }
}

/* A constant byte offset is safe only with an earlier guard that proves the
** corresponding one-based position is within the same immutable GCstr. This
** intentionally does not admit variable offsets or rely on the trailing NUL. */
static int arm64_string_byte_bounds_ir(const GCtrace *T, IRRef strref,
                                       IRRef str, IRRef offset)
{
  int32_t byte_offset;
  IRRef ref;
  int64_t bound;
  if (!arm64_ir_kint_value(T, offset, &byte_offset) || byte_offset < 0 ||
      byte_offset == INT32_MAX ||
      (uint32_t)byte_offset > 4095u - (uint32_t)sizeof(GCstr) ||
      !arm64_scalar_ir_ref(T, str, strref, IRT_STR))
    return arm64_string_byte_dynamic_bounds_ir(T, strref, str, offset);
  bound = (int64_t)byte_offset + 1;
  for (ref = REF_FIRST; ref < strref; ref++) {
    const IRIns *guard = &T->ir[ref];
    IRRef lenref, boundref;
    int32_t guard_bound;
    if ((guard->o != IR_UGE && guard->o != IR_ULE) ||
        guard->t.irt != (IRT_INT|IRT_GUARD))
      continue;
    lenref = guard->o == IR_UGE ? guard->op1 : guard->op2;
    boundref = guard->o == IR_UGE ? guard->op2 : guard->op1;
    if (lenref < REF_FIRST || lenref >= ref ||
        !arm64_ir_kint_value(T, boundref, &guard_bound) ||
        guard_bound != (int32_t)bound)
      continue;
    {
      const IRIns *len = &T->ir[lenref];
      if (len->o == IR_FLOAD && len->t.irt == IRT_INT &&
          len->op2 == IRFL_STR_LEN && len->op1 == str)
        return 1;
    }
  }
  return 0;
}

static int arm64_string_byte_dynamic_index_ir(const GCtrace *T, IRRef conv)
{
  IRRef ref;
  if (!T || conv < REF_FIRST || conv >= T->nins ||
      T->ir[conv].o != IR_CONV || T->ir[conv].t.irt != IRT_INT ||
      T->ir[conv].op2 != (IRCONV_INT_NUM|IRCONV_ANY)) return 0;
  for (ref = conv+1u; ref < T->nins; ref++) {
    const IRIns *strref = &T->ir[ref];
    const IRIns *offset;
    if (strref->o != IR_STRREF || strref->t.irt != IRT_PGC ||
        strref->op2 < REF_FIRST || strref->op2 >= ref) continue;
    offset = &T->ir[strref->op2];
    if ((offset->o != IR_ADDOV || offset->op1 != conv) &&
        (offset->o != IR_ADD ||
         (offset->op1 != conv && offset->op2 != conv))) continue;
    if (arm64_string_byte_dynamic_bounds_ir(T, ref, strref->op1,
                                            strref->op2))
      return 1;
  }
  return 0;
}

/* Certify the recorder's positive or negative one-based dynamic-index
** lowering. Positive indices need index <= len and checked index-1. Negative
** indices need index < 0 and use len+index. Both paths prove a nonnegative
** byte offset, checked requested width, exact width==1, and the same
** immutable string for the length and byte base. */
static int arm64_string_byte_dynamic_bounds_ir(const GCtrace *T,
                                               IRRef strref, IRRef str,
                                               IRRef offset)
{
  LJ_STATIC_ASSERT(LJ_MAX_STR < INT32_MAX);
  const IRIns *sub, *conv, *source;
  GCobj *startpt;
  GCproto *pt;
  IRRef idx, lenref = 0, upper = 0, subref = 0, widthref = 0;
  IRRef nonneg = 0, negative = 0, endref = 0, ref;
  int negative_path;
  int32_t k;
  if (!T || strref < REF_FIRST || strref >= T->nins ||
      offset < REF_FIRST || offset >= strref ||
      !arm64_scalar_ir_ref(T, str, strref, IRT_STR))
    return 0;
  sub = &T->ir[offset];
  negative_path = sub->o == IR_ADD && sub->t.irt == IRT_INT;
  if (negative_path) {
    IRRef left = sub->op1, right = sub->op2;
    int left_len = left >= REF_FIRST && left < offset &&
      T->ir[left].o == IR_FLOAD && T->ir[left].t.irt == IRT_INT &&
      T->ir[left].op1 == str && T->ir[left].op2 == IRFL_STR_LEN;
    int right_len = right >= REF_FIRST && right < offset &&
      T->ir[right].o == IR_FLOAD && T->ir[right].t.irt == IRT_INT &&
      T->ir[right].op1 == str && T->ir[right].op2 == IRFL_STR_LEN;
    if (left_len == right_len) return 0;
    lenref = left_len ? left : right;
    idx = left_len ? right : left;
    if (idx < REF_FIRST || idx >= offset) return 0;
  } else {
    if (sub->o != IR_ADDOV || sub->t.irt != (IRT_INT|IRT_GUARD) ||
        !arm64_ir_kint_value(T, sub->op2, &k) || k != -1 ||
        sub->op1 < REF_FIRST || sub->op1 >= offset)
      return 0;
    idx = sub->op1;
  }
  if (idx < REF_FIRST || idx >= offset) return 0;
  conv = &T->ir[idx];
  if (conv->o != IR_CONV || conv->t.irt != IRT_INT ||
      conv->op2 != (IRCONV_INT_NUM|IRCONV_ANY) ||
      conv->op1 < REF_FIRST || conv->op1 >= idx)
    return 0;
  source = &T->ir[conv->op1];
  if (source->o != IR_SLOAD || source->t.irt != (IRT_NUM|IRT_GUARD) ||
      source->op2 != IRSLOAD_TYPECHECK)
    return 0;
  if (str < REF_FIRST || str >= strref || T->ir[str].o != IR_SLOAD ||
      T->ir[str].t.irt != (IRT_STR|IRT_GUARD) ||
      T->ir[str].op2 != IRSLOAD_TYPECHECK)
    return 0;
  startpt = trace_startptgco_acq((GCtrace *)T);
  if (!startpt || !checkptrGC(startpt) ||
      startpt->gch.gct != (uint32_t)~LJ_TPROTO) return 0;
  pt = gco2pt(startpt);
  if (!arm64_string_byte_call_slots_ir(pt, T->ir[str].op1, source->op1))
    return 0;
  if (negative_path) {
    for (ref = REF_FIRST; ref < offset; ref++) {
      const IRIns *g = &T->ir[ref];
      int32_t zero;
      if (g->t.irt == (IRT_INT|IRT_GUARD) && g->o == IR_LT &&
          g->op1 == idx && arm64_ir_kint_value(T, g->op2, &zero) &&
          zero == 0) {
        if (negative) return 0;
        negative = ref;
      }
    }
    if (!negative || negative >= offset) return 0;
  } else {
    for (ref = REF_FIRST; ref < offset; ref++) {
      const IRIns *g = &T->ir[ref];
      if (g->t.irt != (IRT_INT|IRT_GUARD)) continue;
      {
        IRRef len_candidate = g->o == IR_ULE && g->op1 == idx ? g->op2 :
                              g->o == IR_UGE && g->op2 == idx ? g->op1 : 0;
        const IRIns *candidate;
        if (!len_candidate || len_candidate < REF_FIRST ||
            len_candidate >= ref)
          continue;
        candidate = &T->ir[len_candidate];
        if (candidate->o == IR_FLOAD && candidate->t.irt == IRT_INT &&
            candidate->op1 == str && candidate->op2 == IRFL_STR_LEN) {
          if (upper) return 0;
          upper = ref; lenref = len_candidate;
        }
      }
    }
    if (!upper || !lenref || lenref >= upper || upper >= offset) return 0;
  }
  for (ref = offset+1u; ref < strref; ref++) {
    const IRIns *g = &T->ir[ref];
    int32_t value;
    if (g->t.irt != (IRT_INT|IRT_GUARD)) continue;
    if (g->o == IR_GE && g->op1 == offset &&
        arm64_ir_kint_value(T, g->op2, &value) && value == 0) {
      if (nonneg) return 0;
      nonneg = ref;
    } else if (g->o == IR_SUBOV &&
               (negative_path ? g->op2 == offset :
                (g->op1 == idx && g->op2 == offset))) {
      if (negative_path) {
        const IRIns *end;
        if (g->op1 <= offset || g->op1 >= ref) continue;
        end = &T->ir[g->op1];
        if (end->o != IR_ADD || end->t.irt != IRT_INT ||
            end->op1 != offset ||
            !arm64_ir_kint_value(T, end->op2, &value) || value != 1)
          continue;
        endref = g->op1;
      }
      if (subref) return 0;
      subref = ref;
    } else if (g->o == IR_EQ && g->op1 == subref &&
               arm64_ir_kint_value(T, g->op2, &value) && value == 1) {
      if (widthref) return 0;
      widthref = ref;
    }
  }
  /* `subref` is itself the guarded subtraction result. Its EQ must follow
  ** it and all three offset/range checks must dominate the load. */
  return nonneg > offset && subref > nonneg && widthref > subref &&
         (!negative_path || (endref && endref < subref)) &&
         widthref < strref;
}

static int arm64_string_byte_dynamic_guard_ir(const GCtrace *T,
                                               IRRef guardref)
{
  const IRIns *guard;
  IRRef ref;
  if (!T || guardref < REF_FIRST || guardref >= T->nins) return 0;
  guard = &T->ir[guardref];
  if ((guard->o != IR_ULE && guard->o != IR_UGE) ||
      guard->t.irt != (IRT_INT|IRT_GUARD)) return 0;
  for (ref = guardref+1u; ref < T->nins; ref++) {
    const IRIns *strref = &T->ir[ref];
    const IRIns *offset, *len;
    IRRef idx, lenref;
    if (strref->o != IR_STRREF || strref->t.irt != IRT_PGC ||
        strref->op2 < REF_FIRST || strref->op2 >= ref) continue;
    offset = &T->ir[strref->op2];
    if (offset->o != IR_ADDOV || guardref >= strref->op2 ||
        offset->op1 < REF_FIRST ||
        offset->op1 >= strref->op2) continue;
    idx = offset->op1;
    lenref = guard->o == IR_ULE && guard->op1 == idx ? guard->op2 :
             guard->o == IR_UGE && guard->op2 == idx ? guard->op1 : 0;
    if (!lenref || lenref < REF_FIRST || lenref >= guardref) continue;
    len = &T->ir[lenref];
    if (len->o == IR_FLOAD && len->t.irt == IRT_INT &&
        len->op1 == strref->op1 && len->op2 == IRFL_STR_LEN &&
        arm64_string_byte_dynamic_bounds_ir(T, ref, strref->op1,
                                            strref->op2))
      return 1;
  }
  return 0;
}

/* UGE/ULE are admitted only as the exact dominating range guard consumed by
** a later STRREF. This prevents unrelated unsigned comparisons from opening. */
static int arm64_string_byte_guard_ir(const GCtrace *T, IRRef guardref)
{
  const IRIns *guard;
  IRRef lenref, boundref, ref;
  int32_t bound;
  if (guardref < REF_FIRST || guardref >= T->nins) return 0;
  guard = &T->ir[guardref];
  if ((guard->o != IR_UGE && guard->o != IR_ULE) ||
      guard->t.irt != (IRT_INT|IRT_GUARD)) return 0;
  lenref = guard->o == IR_UGE ? guard->op1 : guard->op2;
  boundref = guard->o == IR_UGE ? guard->op2 : guard->op1;
  if (lenref < REF_FIRST || lenref >= guardref ||
      (!arm64_ir_kint_value(T, boundref, &bound) || bound <= 0 ||
       (uint32_t)bound > 4096u-(uint32_t)sizeof(GCstr))) {
    return arm64_string_byte_dynamic_guard_ir(T, guardref);
  }
  {
    const IRIns *len = &T->ir[lenref];
    if (len->o != IR_FLOAD || len->t.irt != IRT_INT ||
        len->op2 != IRFL_STR_LEN ||
        !arm64_scalar_ir_ref(T, len->op1, guardref, IRT_STR)) return 0;
    for (ref = guardref+1u; ref < T->nins; ref++) {
      const IRIns *strref = &T->ir[ref];
      int32_t offset;
      if (strref->o == IR_STRREF && strref->t.irt == IRT_PGC &&
          strref->op1 == len->op1 &&
          arm64_ir_kint_value(T, strref->op2, &offset) &&
          offset == bound-1)
        return 1;
    }
  }
  return 0;
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
  const IRIns *tmp;
  if (!root || root >= before || root >= T->nins) return 0;
  tmp = &T->ir[root];
  if (tmp->o != IR_TMPREF || tmp->t.irt != IRT_PGC || tmp->op2 != mode ||
      (!irref_isk(tmp->op1) &&
       (tmp->op1 < REF_FIRST || tmp->op1 >= root)))
    return 0;
  /* The context proof below binds this scalar source to its exact function. */
  return arm64_scalar_ir_ref(T, tmp->op1, root, IRT_FUNC);
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

static int arm64_upvalue_ir_context(const jit_State *J, const GCtrace *T,
                                   const SnapShot *snap, const BCIns *pc,
                                   IRRef root, const GCproto **ptp);

static int arm64_upvalue_ir_source(const jit_State *J, const GCtrace *T,
                                  IRRef ref, SnapNo s, IRRef root1,
                                  IRRef root2)
{
  const IRIns *call, *tail, *head;
  const SnapShot *pre, *post;
  const BCIns *pc;
  BCIns bc;
  const GCproto *sourcept;
  int get;
  uint32_t uv;
  IRRef value = 0;
  MSize i, slot;
  if (!J || !T || !T->ir || !T->snap || !T->snapmap ||
      ref < REF_FIRST || ref >= T->nins || s >= T->nsnap ||
      s+1u >= T->nsnap) return 0;
  pre = &T->snap[s];
  if (pre->mapofs > T->nsnapmap ||
      pre->nent > T->nsnapmap-pre->mapofs ||
      T->nsnapmap-pre->mapofs-pre->nent < 2u) return 0;
  call = &T->ir[ref];
  get = call->op2 == IRCALL_lj_func_getuv_rooted_try;
  pc = snap_pc(&T->snapmap[pre->mapofs+pre->nent]);
  if (pre->ref != root1 ||
      (!arm64_upvalue_fnroot_ir(T, root1, ref, IRTMPREF_IN1) &&
       !arm64_upvalue_fnroot_ir(T, root1, ref,
                                IRTMPREF_IN1|IRTMPREF_OUT1)) ||
      !arm64_upvalue_ir_context(J, T, pre, pc, root1, &sourcept))
    return 0;
  bc = (BCIns)la_load32_acq((const uint32_t *)pc);
  if ((get && bc_op(bc) != BC_UGET) ||
      (!get && bc_op(bc) != BC_USETV && bc_op(bc) != BC_USETS &&
       bc_op(bc) != BC_USETN && bc_op(bc) != BC_USETP))
    return 0;
  uv = get ? bc_d(bc) : bc_a(bc);
  if (uv >= sourcept->sizeuv) return 0;
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
  post = &T->snap[s+1u];
  if (post->mapofs > T->nsnapmap ||
      post->nent > T->nsnapmap-post->mapofs ||
      T->nsnapmap-post->mapofs-post->nent < 2u ||
      post->ref != ref+2u ||
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
    if (bc_d(bc) >= sourcept->sizekn) return 0;
    lj_tv_load_acq(&expected, proto_knumtv(sourcept, bc_d(bc)));
    if (tvisint(&expected))
      return k->o == IR_KINT && k->t.irt == IRT_INT && k->i == intV(&expected);
    if (k->o != IR_KNUM || k->t.irt != IRT_NUM) return 0;
    return tv_rawload(&expected) == tv_rawload(ir_knum(k));
  } else {
    GCobj *expected, *actual;
    const IRIns *k = &T->ir[value];
    ptrdiff_t idx = ~(ptrdiff_t)bc_d(bc);
    if (k->o != IR_KGC || idx >= 0 ||
        (uintptr_t)idx < ~(uintptr_t)proto_sizekgc_acq(sourcept)+1u)
      return 0;
    expected = proto_kgc_acq(sourcept, idx);
    actual = ir_kgc_acq(k);
    return expected != NULL && expected->gch.gct == (uint32_t)~LJ_TSTR &&
           k->t.irt == IRT_STR && actual == expected;
  }
}

static IRRef arm64_lua_leaf_ir_upvalue(const jit_State *J,
                                       const GCtrace *T, const GCproto *pt,
                                       MSize pos, IRRef before, IRRef end)
{
  const BCIns *sourcepc;
  IRRef ref, selected = 0;
  IRRef lower;
  if (!J || !T || !pt || pos >= pt->sizebc || before >= T->nins ||
      end > T->nins || end < before) return 0;
  sourcepc = &proto_bc(pt)[pos];
  lower = J->loopref >= REF_FIRST && J->loopref < before ?
          J->loopref : REF_FIRST;
  /* The source UGET is a bytecode operation in the continuation represented
  ** by this frame snapshot. Its guarded helper result can be emitted just
  ** after the pre-UGET snapshot, so bound by the current trace segment end,
  ** not by the resume snapshot ref. */
  for (ref = lower; ref < end; ref++) {
    const IRIns *call = &T->ir[ref], *tail;
    IRRef root;
    SnapNo s;
    if (call->o != IR_CALLS ||
        call->op2 != IRCALL_lj_func_getuv_rooted_try ||
        ref+2u >= end || call->op1 < REF_FIRST || call->op1 >= ref)
      continue;
    tail = &T->ir[call->op1];
    if (tail->o != IR_CARG || tail->op2 < REF_FIRST || tail->op2 >= ref)
      continue;
    root = tail->op2;
    for (s = 0; s < T->nsnap; s++) {
      const SnapShot *snap = &T->snap[s];
      const BCIns *pc;
      if (snap->ref != root || snap->mapofs > T->nsnapmap ||
          snap->nent > T->nsnapmap-snap->mapofs ||
          T->nsnapmap-snap->mapofs-snap->nent < 2u) continue;
      pc = snap_pc(&T->snapmap[snap->mapofs+snap->nent]);
      if (pc != sourcepc || !arm64_upvalue_ir_source(J, T, ref, s, root, 0))
        continue;
      if (selected) return 0;
      selected = ref+2u;
    }
  }
  return selected;
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
  const SnapShot *pre, *post;
  const BCIns *pc;
  int get;
  if (!J || !T || !T->snap || !T->snapmap || ref < REF_FIRST ||
      !T->ir || ref >= T->nins || s+1u >= T->nsnap) return 0;
  get = T->ir[ref].op2 == IRCALL_lj_func_getuv_rooted_try;
  pre = &T->snap[s];
  if (pre->mapofs > T->nsnapmap ||
      pre->nent > T->nsnapmap-pre->mapofs ||
      T->nsnapmap-pre->mapofs-pre->nent < 2u) return 0;
  pc = snap_pc(&T->snapmap[pre->mapofs+pre->nent]);
  if (pre->ref != root1 ||
      !arm64_upvalue_ir_source(J, T, ref, s, root1, root2))
    return 0;
  if (!get && pre->count == SNAPCOUNT_DONE) return 0;
  if (get) {
    post = &T->snap[s+1u];
    if (post->mapofs > T->nsnapmap ||
        post->nent > T->nsnapmap-post->mapofs ||
        T->nsnapmap-post->mapofs-post->nent < 2u ||
        snap_pc(&T->snapmap[post->mapofs+post->nent]) != pc) return 0;
  }
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
  if (bc_op(side->parent_startins) == BC_FORL) {
	int64_t body = (int64_t)end+1+(int64_t)bc_j(side->parent_startins);
	if (bc_op(h) != BC_FORI || bc_a(h) != bc_a(side->parent_startins) ||
	    header+1u != side->loop_start_pc ||
	    back != BCINS_AD(BC_JFORL, bc_a(side->parent_startins), side->root) ||
	    bc_j(h) <= 0 || (int64_t)header+1+(int64_t)bc_j(h) !=
	      (int64_t)end+1 || body < 0 || (MSize)body != side->loop_start_pc ||
	    !arm64_ir_pcpos((uintptr_t)(const void *)side->parent_continuation,
	      lo, hi, &cont) || cont < (MSize)body || cont >= end)
	  return 0;
	if (continuation_pos)
	  *continuation_pos = cont;
	return 1;
  }
  target = (int64_t)end + 1 + (int64_t)bc_j(back);
  if (bc_op(h) != BC_JLOOP || bc_d(h) != side->root ||
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
  int readonly, no_restore;
  if (!side || !load || !side->parent_snap || !side->parent_map_values ||
	  (load->op2 != (IRSLOAD_PARENT|IRSLOAD_INHERIT) &&
	   load->op2 != (IRSLOAD_PARENT|IRSLOAD_READONLY|IRSLOAD_INHERIT)) ||
	  (irt_type(load->t) != IRT_INT && irt_type(load->t) != IRT_NUM) ||
	  (load->t.irt & ~IRT_TYPE) || ordinal >= side->parent_nregspmap ||
	  side->parent_nslots > LJ_MAX_JSLOTS ||
	  snap_nent_acq(side->parent_snap) > side->parent_nsnapmap ||
	  side->parent_nsnapmap-snap_nent_acq(side->parent_snap) < 1u+LJ_FR2)
	return 0;
  readonly = load->op2 ==
	(IRSLOAD_PARENT|IRSLOAD_READONLY|IRSLOAD_INHERIT);
  no_restore = 0;
  nent = snap_nent_acq(side->parent_snap);
  for (i = 0; i < nent; i++) {
	SnapEntry sn = snapentry_acq(&side->parent_snapmap[i]);
		if (snap_slot(sn) != load->op1)
	  continue;
	ref = snap_ref(sn);
	if ((sn & 0x00ff0000u) == SNAP_NORESTORE) {
	  MSize idxslot = 1u+LJ_FR2+bc_a(side->parent_startins);
	  MSize control;
	  IRIns source, root_source;
	  if (i >= side->parent_nmap_values ||
	      bc_op(side->parent_startins) != BC_FORL ||
	      (load->op2 != (IRSLOAD_PARENT|IRSLOAD_INHERIT) && !readonly) ||
	      (load->op1 != idxslot && load->op1 != idxslot+FORL_STOP &&
	       load->op1 != idxslot+FORL_STEP))
	    return 0;
	  source = side->parent_map_values[i].value;
	  if (source.o != IR_SLOAD || source.op1 != load->op1)
	    return 0;
	  control = load->op1 == idxslot ? 0 :
	    load->op1 == idxslot+FORL_STOP ? 1 : 2;
	  if (side->parent != side->root) {
	    if ((side->parent_root_forl_mask & (1u<<control)) == 0)
	      return 0;
	    root_source = side->parent_root_forl[control];
	    if (root_source.o != IR_SLOAD || root_source.op1 != load->op1 ||
	        source.t.irt != root_source.t.irt ||
	        source.op2 != (root_source.op2|IRSLOAD_PARENT))
	      return 0;
	  } else if (load->op1 == idxslot) {
	    if ((source.t.irt & ~IRT_ISPHI) != (IRT_INT|IRT_GUARD) ||
	        source.op2 != (IRSLOAD_TYPECHECK|IRSLOAD_INHERIT))
	      return 0;
	  } else if (source.t.irt != IRT_INT ||
	             source.op2 != (IRSLOAD_READONLY|IRSLOAD_INHERIT)) {
	    return 0;
	  }
	  no_restore = 1;
	} else if ((sn & 0x00ff0000u) != 0) {
	  return 0;
	}
	if (readonly && !no_restore)
	  return 0;
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

/* A side may also start by rereading an unchanged local directly from the
** interpreter stack. Such an entry SLOAD is safe only when the selected
** parent's exact restart map omits that in-frame slot; a mapped slot must use
** the authenticated parent-binding path above. */
static int arm64_side_entry_sload_valid(const LJArm64SideIRView *side,
	const IRIns *load)
{
  MSize i, nent;
  IRType type;
  if (!side || !load || !side->parent_snap || !side->parent_snapmap ||
	  load->o != IR_SLOAD || load->op2 != IRSLOAD_TYPECHECK ||
	  load->op1 < 1u+LJ_FR2 || load->op1 >= side->parent_nslots ||
	  side->parent_nslots > LJ_MAX_JSLOTS ||
	  side->parent_topslot > side->parent_nslots ||
	  (load->t.irt & ~(IRT_TYPE|IRT_GUARD|IRT_ISPHI)) != 0 ||
	  !(load->t.irt & IRT_GUARD))
	return 0;
  type = (IRType)irt_type(load->t);
  if (type != IRT_INT && type != IRT_NUM)
	return 0;
  nent = snap_nent_acq(side->parent_snap);
  if (nent > side->parent_nmap_values || nent > side->parent_nsnapmap ||
	  side->parent_nsnapmap-nent < 1u+LJ_FR2)
	return 0;
  for (i = 0; i < nent; i++)
	if (snap_slot(snapentry_acq(&side->parent_snapmap[i])) == load->op1)
	  return 0;
  return 1;
}

/* A child FORL update is safe only when it replays the exact source
** index+step operation and carries its own stop range guard before the loop
** comparison consumes the result. The selected parent map binds the current
** index, stop and step to the source loop; parent admission alone is not the
** overflow proof because an in-body exit can precede the parent's guard. */
static int arm64_side_forl_step_source(const LJArm64SideIRView *side,
	int32_t *stepp)
{
  MSize header, idxslot;
  BCIns fori, stepins;
  int32_t step;
  if (!side || !stepp || side->parent_admission !=
	TRACE_ARM64_INT_FORL_ADMITTED ||
	bc_op(side->parent_startins) != BC_FORL ||
	!side->proto_bc || side->loop_header_pc == 0 ||
	side->loop_header_pc >= side->proto_sizebc ||
	side->loop_header_pc >= side->loop_end_pc)
    return 0;
  header = side->loop_header_pc;
  fori = (BCIns)la_load32_acq((const uint32_t *)&side->proto_bc[header]);
  stepins = (BCIns)la_load32_acq((const uint32_t *)&side->proto_bc[header-1u]);
  idxslot = 1u+LJ_FR2+(MSize)bc_a(side->parent_startins);
  step = (int32_t)(int16_t)bc_d(stepins);
  if (bc_op(fori) != BC_FORI ||
	bc_a(fori) != bc_a(side->parent_startins) ||
	bc_op(stepins) != BC_KSHORT ||
	bc_a(stepins) != bc_a(fori)+FORL_STEP || step == 0 ||
	idxslot >= side->parent_topslot ||
	idxslot+FORL_STEP >= side->parent_topslot)
    return 0;
  *stepp = step;
  return 1;
}

static int arm64_side_forl_step_map_valid(const LJArm64SideIRView *side,
	IRRef stepref, int32_t step)
{
  MSize i, matches = 0;
  MSize stepslot = 1u+LJ_FR2+(MSize)bc_a(side->parent_startins)+FORL_STEP;
  if (!side || !side->parent_snap || !side->parent_map_values ||
	  side->parent_nmap_values != snap_nent_acq(side->parent_snap))
    return 0;
  for (i = 0; i < side->parent_nmap_values; i++) {
	const LJArm64SideParentMapValue *mv = &side->parent_map_values[i];
	if (mv->slot != stepslot) continue;
	if (++matches != 1 || mv->ref != stepref ||
	    mv->value.o != IR_KINT || mv->value.t.irt != IRT_INT ||
	    mv->value.i != step)
	  return 0;
  }
  return matches == 1;
}

/* The child range guard runs after the body arithmetic and increment. Its
** cold edge may restart at the loop instruction itself, or at an in-body JMP
** that directly reaches that instruction; restarting at the child entry would
** repeat the completed body and apply its effects twice. */
static int arm64_side_forl_retry_pc_valid(const LJArm64SideIRView *side,
	uintptr_t pc)
{
  uintptr_t lo, hi;
  MSize pos;
  BCIns retry;
  if (!side || !side->proto_bc || !pc ||
	  side->loop_end_pc >= side->proto_sizebc)
	return 0;
  lo = (uintptr_t)(const void *)side->proto_bc;
  if (!lo || side->proto_sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns))
	return 0;
  hi = lo+side->proto_sizebc*sizeof(BCIns);
  if (!arm64_ir_pcpos(pc, lo, hi, &pos)) return 0;
  retry = (BCIns)la_load32_acq(
	(const uint32_t *)&side->proto_bc[side->loop_end_pc]);
  if (bc_op(retry) != BC_JFORL) return 0;
  if (pos == side->loop_end_pc) return 1;
  if (pos < side->loop_start_pc || pos >= side->loop_end_pc) return 0;
  {
	BCIns jump = (BCIns)la_load32_acq((const uint32_t *)&side->proto_bc[pos]);
	int64_t target;
	if (bc_op(jump) != BC_JMP) return 0;
	target = (int64_t)pos+1+(int64_t)bc_j(jump);
	return target >= 0 && (MSize)target == side->loop_end_pc;
  }
}

/* A child-local range guard must retry the source loop boundary with the
** pre-update loop index. The loop ADD is evaluated before this guard, so
** checking only its operands and ordering would permit a cold snapshot to
** resume with the wrapped ADD result. */
static int arm64_side_forl_guard_snapshot_ir(const LJArm64SideIRView *side,
	const GCtrace *T, IRRef guard, MSize idxslot, IRRef oldidx)
{
  uint64_t pcbase;
  MSize s, selected = (MSize)-1, ofs, nent, i, matches = 0;
  if (!side || !T || !T->snap || !T->snapmap ||
	  !side->parent_continuation || idxslot >= LJ_MAX_JSLOTS ||
	  oldidx < REF_FIRST || oldidx >= guard)
	return 0;
  for (s = 0; s < T->nsnap; s++) {
	if (T->snap[s].ref > guard) break;
	selected = s;
  }
  if (selected == (MSize)-1) return 0;
  ofs = snap_mapofs_acq(&T->snap[selected]);
  nent = snap_nent_acq(&T->snap[selected]);
  if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs ||
	  T->nsnapmap-ofs-nent < 1u+LJ_FR2)
	return 0;
  memcpy(&pcbase, &T->snapmap[ofs+nent], sizeof(pcbase));
  for (i = 0; i < nent; i++) {
	SnapEntry sn = snapentry_acq(&T->snapmap[ofs+i]);
	if (snap_slot(sn) == idxslot) {
	  if (snap_ref(sn) != oldidx) return 0;
	  matches++;
	}
  }
  if (matches != 1) return 0;
  return (uint8_t)pcbase == 0 &&
	arm64_side_forl_retry_pc_valid(side, (uintptr_t)(pcbase >> 8));
}

/* A cold edge at FORL retries its increment. Bind all four control slots
** to the inherited pre-update state, including the visible index, so the VM
** cannot skip or repeat an iteration when it leaves a root-linked child. */
static int arm64_side_forl_boundary_snapshot_ir(
    const LJArm64SideIRView *side, const GCtrace *T, const SnapShot *snap)
{
  MSize i, idxslot, mask = 0;
  IRRef oldidx = 0, ext = 0;
  int32_t step;
  if (!side || !T || !snap || side->linktype != LJ_TRLINK_ROOT ||
      !arm64_side_forl_step_source(side, &step) ||
      snap->mapofs > T->nsnapmap || snap->nent > T->nsnapmap-snap->mapofs)
    return 0;
  idxslot = 1u+LJ_FR2+bc_a(side->parent_startins);
  for (i = 0; i < snap->nent; i++) {
    SnapEntry entry = T->snapmap[snap->mapofs+i];
    MSize slot = snap_slot(entry), role;
    IRRef value = snap_ref(entry);
    const IRIns *load;
    if (slot < idxslot || slot > idxslot+FORL_EXT) continue;
    role = slot-idxslot;
    if (mask & (1u << role)) return 0;
    mask |= 1u << role;
    if (role == FORL_STEP) {
      int32_t found;
      if (!arm64_ir_kint_value(T, value, &found) || found != step) return 0;
      continue;
    }
    if (value < REF_FIRST || value >= snap->ref || value >= T->nins)
      return 0;
    load = &T->ir[value];
    if (load->o != IR_SLOAD || load->t.irt != IRT_INT ||
        load->op1 != (role == FORL_EXT ? idxslot : slot) ||
        load->op2 != (IRSLOAD_PARENT|IRSLOAD_INHERIT|
                     (role == FORL_STOP ? IRSLOAD_READONLY : 0))) return 0;
    if (role == FORL_IDX) oldidx = value;
    if (role == FORL_EXT) ext = value;
  }
  return mask == 15u && oldidx != 0 && ext == oldidx;
}

/* A child-of-side can inherit the already certified root FORL range proof.
** The immediate-parent snapshot must still map its controls to the exact root
** sources, and the root stop-bound guard must precede that selected root map. */
static int arm64_side_forl_root_range_proof(const LJArm64SideIRView *side,
	int32_t step)
{
	MSize idxslot, stopslot;
	int64_t expected64;
	IRIns idx, stop, guard, constant;
	if (!side || side->parent == side->root ||
	    !side->parent_root_forl_range_valid ||
	    (side->parent_root_forl_mask & 3u) != 3u ||
	    side->parent_root_forl_nins <= REF_FIRST ||
	    side->parent_root_forl_nins >= REF_DROP ||
	    side->parent_root_forl_bound_guard_ref < REF_FIRST ||
	    side->parent_root_forl_bound_guard_ref >=
	      side->parent_root_forl_nins ||
	    side->parent_root_forl_snapref <=
	      side->parent_root_forl_bound_guard_ref ||
	    side->parent_root_forl_snapref >= side->parent_root_forl_nins ||
	    side->parent_root_forl_stop_ref < REF_FIRST ||
	    side->parent_root_forl_stop_ref >=
	      side->parent_root_forl_bound_guard_ref ||
	    side->parent_root_forl_bound_constant_ref >= REF_TRUE)
	  return 0;
	idxslot = 1u+LJ_FR2+(MSize)bc_a(side->parent_startins);
	stopslot = idxslot+FORL_STOP;
	idx = side->parent_root_forl[0];
	stop = side->parent_root_forl[1];
	if (idx.o != IR_SLOAD || idx.op1 != idxslot ||
	    (idx.t.irt & ~IRT_ISPHI) != (IRT_INT|IRT_GUARD) ||
	    idx.op2 != (IRSLOAD_TYPECHECK|IRSLOAD_INHERIT) ||
	    stop.o != IR_SLOAD || stop.op1 != stopslot ||
	    stop.t.irt != IRT_INT ||
	    stop.op2 != (IRSLOAD_READONLY|IRSLOAD_INHERIT) ||
	    side->parent_root_forl_stop_ref >= side->parent_root_forl_nins)
	  return 0;
	expected64 = step > 0 ? (int64_t)INT32_MAX-step :
	                        (int64_t)INT32_MIN-step;
	if (step == 0 || expected64 < INT32_MIN || expected64 > INT32_MAX)
	  return 0;
	guard = side->parent_root_forl_bound_guard;
	constant = side->parent_root_forl_bound_constant;
	return guard.o == (step > 0 ? IR_LE : IR_GE) &&
	  guard.t.irt == (IRT_INT|IRT_GUARD) &&
	  guard.op1 == side->parent_root_forl_stop_ref &&
	  guard.op2 == side->parent_root_forl_bound_constant_ref &&
	  constant.o == IR_KINT && constant.t.irt == IRT_INT &&
	  constant.i == (int32_t)expected64;
}

/* The equality cold edge may be the exact FORL termination edge. In that
** case the index is dead after the loop, so require it to be absent from the
** snapshot and bind the footer directly to the instruction after FORL. */
static int arm64_side_forl_termination_snapshot_ir(
	const LJArm64SideIRView *side, const GCtrace *T, IRRef guard,
	MSize idxslot)
{
	MSize s, selected = (MSize)-1, ofs, nent, i;
	uint64_t pcbase;
	uintptr_t target;
	if (!side || !T || !T->snap || !T->snapmap ||
	    side->loop_end_pc+1u >= side->proto_sizebc ||
	    idxslot >= LJ_MAX_JSLOTS)
	  return 0;
	for (s = 0; s < T->nsnap; s++) {
	  if (T->snap[s].ref > guard) break;
	  selected = s;
	}
	if (selected == (MSize)-1 || T->snap[selected].ref != guard)
	  return 0;
	ofs = snap_mapofs_acq(&T->snap[selected]);
	nent = snap_nent_acq(&T->snap[selected]);
	if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs ||
	    T->nsnapmap-ofs-nent < 1u+LJ_FR2)
	  return 0;
	for (i = 0; i < nent; i++) {
	  MSize slot = snap_slot(snapentry_acq(&T->snapmap[ofs+i]));
	  if (slot == idxslot || slot == idxslot+FORL_STOP ||
	      slot == idxslot+FORL_STEP)
	    return 0;
	}
	memcpy(&pcbase, &T->snapmap[ofs+nent], sizeof(pcbase));
	if ((uint8_t)pcbase != 0) return 0;
	target = (uintptr_t)(pcbase>>8);
	return target == (uintptr_t)(const void *)
	  (side->proto_bc+side->loop_end_pc+1u);
}

static int arm64_side_forl_snapshot_stop_ir(const LJArm64SideIRView *side,
	const GCtrace *T, IRRef before, SnapEntry sn)
{
  MSize idxslot, slot, ordinal, matches = 0;
  int32_t step;
  IRRef value = snap_ref(sn);
  IRIns load;
  if (!side || !T || (sn & 0x00ff0000u) != SNAP_NORESTORE ||
	  !arm64_side_forl_step_source(side, &step))
	return 0;
  idxslot = 1u+LJ_FR2+(MSize)bc_a(side->parent_startins);
  slot = snap_slot(sn);
  if (slot != idxslot+FORL_STOP || value < REF_FIRST || value >= before ||
	  value >= T->nins)
	return 0;
  load = T->ir[value];
  if (load.o != IR_SLOAD || load.op1 != slot ||
	  (load.t.irt & ~IRT_ISPHI) != IRT_INT ||
	  load.op2 != (IRSLOAD_PARENT|IRSLOAD_READONLY|IRSLOAD_INHERIT))
	return 0;
  for (ordinal = 0; ordinal < side->parent_nregspmap; ordinal++)
	if (side->parent_bindings[ordinal].slot == slot)
	  matches++;
  if (matches != 1) return 0;
  for (ordinal = 0; ordinal < side->parent_nregspmap; ordinal++)
	if (side->parent_bindings[ordinal].slot == slot)
	  return arm64_side_parent_sload_valid(side, &load, ordinal);
  return 0;
}

/* A constant stop folds the runtime range guard. The exact inherited stop
** still has to prove that every taken loop index can be incremented safely. */
static int arm64_side_forl_constant_stop_valid(const LJArm64SideIRView *side,
    IRRef ref, int32_t stop, int32_t step)
{
  MSize slot = 1u+LJ_FR2+bc_a(side->parent_startins)+FORL_STOP;
  MSize matches = 0;
  int64_t limit = step > 0 ? (int64_t)INT32_MAX-step :
                             (int64_t)INT32_MIN-step;
  if (!step || (step > 0 ? (int64_t)stop > limit : (int64_t)stop < limit))
    return 0;
  for (MSize i = 0; i < side->parent_nmap_values; i++) {
    const LJArm64SideParentMapValue *mv = &side->parent_map_values[i];
    if (mv->slot != slot) continue;
    if (++matches != 1 || mv->ref != ref || mv->value.o != IR_KINT ||
        mv->value.t.irt != IRT_INT || mv->value.i != stop) return 0;
  }
  return matches == 1;
}

static int arm64_side_forl_finish_snapshot_ir(const LJArm64SideIRView *side,
    const GCtrace *T, IRRef compare)
{
  MSize selected = (MSize)-1, ofs, nent;
  MSize idxslot = 1u+LJ_FR2+bc_a(side->parent_startins);
  uint64_t pcbase;
  for (MSize i = 0; i < T->nsnap; i++) {
    if (T->snap[i].ref > compare) break;
    selected = i;
  }
  if (selected == (MSize)-1) return 0;
  ofs = T->snap[selected].mapofs; nent = T->snap[selected].nent;
  if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs ||
      T->nsnapmap-ofs-nent < 1u+LJ_FR2) return 0;
  for (MSize i = 0; i < nent; i++)
    if (snap_slot(T->snapmap[ofs+i]) >= idxslot) return 0;
  memcpy(&pcbase, &T->snapmap[ofs+nent], sizeof(pcbase));
  return (uint8_t)pcbase == 0 && side->loop_end_pc+1u < side->proto_sizebc &&
    (pcbase >> 8) == (uintptr_t)(side->proto_bc+side->loop_end_pc+1u);
}

static int arm64_side_forl_index_add_ir(const LJArm64SideIRView *side,
	const GCtrace *T, IRRef ref, IRIns add)
{
  MSize idxslot, stopslot, r;
  int32_t step, source_step;
  unsigned comparisons = 0, rangeguards = 0, constantstops = 0, inherited = 0;
  IRRef rangeguard = 0, inherited_guard = 0;
  int64_t bound64;
  if (!side || !T || add.o != IR_ADD || add.t.irt != IRT_INT ||
	  !arm64_side_forl_step_source(side, &source_step) ||
	  !arm64_ir_kint_value(T, add.op2, &step) || step != source_step ||
      !arm64_side_forl_step_map_valid(side, add.op2, step)) {
    return 0;
  }

  idxslot = 1u+LJ_FR2+(MSize)bc_a(side->parent_startins);
  stopslot = idxslot+FORL_STOP;
  bound64 = step > 0 ? (int64_t)INT32_MAX-step :
                       (int64_t)INT32_MIN-step;
  if (bound64 < INT32_MIN || bound64 > INT32_MAX) return 0;
  if (add.op1 < REF_FIRST || add.op1 >= ref) return 0;
  {
	IRIns idx = T->ir[add.op1];
	if (idx.o != IR_SLOAD || idx.op1 != idxslot ||
	    (idx.t.irt & ~IRT_ISPHI) != IRT_INT ||
	    idx.op2 != (IRSLOAD_PARENT|IRSLOAD_INHERIT)) {
	  return 0;
	}
  }

  for (r = ref+1u; r < T->nins; r++) {
	IRIns cmp = T->ir[r];
	if (cmp.o == (step > 0 ? IR_LE : IR_GE) &&
	    cmp.t.irt == (IRT_INT|IRT_GUARD) && cmp.op1 == ref &&
	    cmp.op2 < r) {
      int32_t stopvalue;
      if (arm64_ir_kint_value(T, cmp.op2, &stopvalue)) {
        if (!arm64_side_forl_constant_stop_valid(side, cmp.op2, stopvalue, step) ||
            !arm64_side_forl_finish_snapshot_ir(side, T, (IRRef)r) ||
            ++comparisons != 1 || ++constantstops != 1) return 0;
        continue;
      }
      if (cmp.op2 < REF_FIRST) return 0;
	  IRIns stop = T->ir[cmp.op2];
	  if (stop.o == IR_SLOAD && stop.op1 == stopslot &&
	      (stop.t.irt & ~IRT_ISPHI) == IRT_INT &&
	      stop.op2 == (IRSLOAD_PARENT|IRSLOAD_READONLY|IRSLOAD_INHERIT)) {
	    IRRef q;
	    int matched = 0;
	    for (q = ref+1u; q < r; q++) {
	      IRIns guard = T->ir[q];
	      int32_t bound;
	      if (guard.o != (step > 0 ? IR_LE : IR_GE) ||
		  guard.t.irt != (IRT_INT|IRT_GUARD) ||
		  guard.op1 != cmp.op2 ||
		  !arm64_ir_kint_value(T, guard.op2, &bound) ||
		  bound != (int32_t)bound64)
		continue;
	      if (++rangeguards != 1) return 0;
	      rangeguard = q;
	      matched = 1;
	    }
	    if (matched) {
	      if (rangeguard >= r || ++comparisons != 1) return 0;
	    } else if (side->parent != side->root &&
		       arm64_side_forl_root_range_proof(side, step)) {
	      if (++inherited != 1) return 0;
	      inherited_guard = r;
	    } else {
	      return 0;
	    }
	  }
	}
  }
  if (comparisons == 1 && inherited == 0)
    return (constantstops == 1 && rangeguards == 0) ||
      (constantstops == 0 && rangeguards == 1 &&
       arm64_side_forl_guard_snapshot_ir(side, T, rangeguard, idxslot, add.op1));
  return comparisons == 0 && rangeguards == 0 && constantstops == 0 &&
    inherited == 1 && arm64_side_forl_root_range_proof(side, step) &&
    (arm64_side_forl_guard_snapshot_ir(side, T, inherited_guard,
                                     idxslot, add.op1) ||
     arm64_side_forl_termination_snapshot_ir(side, T, inherited_guard, idxslot));
}

/* Syntax-only view of a single acyclic comparison diamond. Each arm is an
** arbitrary straight-line range of the existing pure leaf operations; the
** semantic and allocated passes independently replay the selected path. */
typedef struct LJArm64LuaLeafBranch {
  MSize compare, condition_jump, first, second, join, size;
  MSize first_ret, second_ret;
  uint8_t early_return;
  MSize next[UINT8_MAX+1u];
  uint8_t first_path[UINT8_MAX+1u], second_path[UINT8_MAX+1u];
} LJArm64LuaLeafBranch;

/* Bounded, syntax-only view of the inline Lua frames in one snapshot. The
** semantic and post-RA passes resolve and validate each function/link value
** independently after decoding these slot relationships. */
#define LJ_ARM64_LUA_MAX_CHILD_FRAMES 2u
typedef struct LJArm64LuaFrameRow {
  MSize linkslot, baseslot, funcslot;
  IRRef linkref, funcref;
} LJArm64LuaFrameRow;

typedef struct LJArm64LuaFramePath {
  uint8_t depth;
  LJArm64LuaFrameRow child[LJ_ARM64_LUA_MAX_CHILD_FRAMES];
} LJArm64LuaFramePath;

static int arm64_lua_frame_path_limit(const SnapEntry *map, MSize nent,
                                      uint64_t pcbase, MSize maxdepth,
                                      LJArm64LuaFramePath *path)
{
  MSize i, j;
  int rootsentinel = 0;
  if (!path || (nent && !map)) return 0;
  memset(path, 0, sizeof(*path));
  for (i = 0; i < nent; i++) {
    SnapEntry sn = map[i];
    MSize slot = snap_slot(sn);
    uint32_t flags = sn & 0x00ff0000u;
    LJArm64LuaFrameRow *row;
    if (!(flags & SNAP_FRAME)) continue;
    /* LJ_FR2's root function marker is a non-restoring sentinel, not an
    ** inlined child frame. */
    if (slot == 1u && flags == (SNAP_FRAME|SNAP_NORESTORE) &&
        snap_ref(sn) == REF_NIL) {
      rootsentinel = 1;
      continue;
    }
    if (flags != SNAP_FRAME ||
        path->depth >= maxdepth ||
        slot < 1u+LJ_FR2+1u)
      return 0;
    if (path->depth && path->child[path->depth-1u].linkslot >= slot)
      return 0;
    row = &path->child[path->depth++];
    row->linkslot = slot;
    row->baseslot = slot+1u;
    row->funcslot = slot-1u;
    row->linkref = snap_ref(sn);
  }
  for (i = 0; i < path->depth; i++) {
    LJArm64LuaFrameRow *row = &path->child[i];
    MSize found = 0;
    for (j = 0; j < nent; j++) {
      SnapEntry sn = map[j];
      if (snap_slot(sn) == row->funcslot) {
        if (sn & 0x00ff0000u) return 0;
        row->funcref = snap_ref(sn);
        found++;
      }
    }
    if (found != 1u) return 0;
  }
  /* Preserve the old one-child recognizer's fail-closed treatment of a root
  ** sentinel mixed into an inlined-frame map until the complete chain proof
  ** explicitly accounts for every marker. */
  if (rootsentinel && path->depth) return 0;
  if (path->depth) {
    if (path->child[path->depth-1u].baseslot-2u != (uint8_t)pcbase)
      return 0;
  } else if ((uint8_t)pcbase != 0) {
    return 0;
  }
  return 1;
}

/* Existing admission paths retain the one-child bound. Only the complete
** FUNCV proof opts into the paired CALL/FRAME_VARG shape. */
static int arm64_lua_frame_path(const SnapEntry *map, MSize nent,
                                uint64_t pcbase, LJArm64LuaFramePath *path)
{
  return arm64_lua_frame_path_limit(map, nent, pcbase, 1u, path);
}

static const GCfunc *arm64_lua_leaf_ir_function(const GCtrace *T, IRRef fn,
                                                IRRef before, IRRef *guardp);

/* Resolve the exact prototype/function represented by an upvalue helper's
** pre-call snapshot. Child upvalues are accepted only when the helper roots
** the exact function TValue saved in that one-child frame map. */
static int arm64_upvalue_ir_context(const jit_State *J, const GCtrace *T,
                                   const SnapShot *snap, const BCIns *pc,
                                   IRRef root, const GCproto **ptp)
{
  uint64_t pcbase;
  MSize pos;
  if (!J || !J->pt || !T || !snap || !pc || !ptp ||
      snap->mapofs > T->nsnapmap || snap->nent > T->nsnapmap-snap->mapofs ||
      T->nsnapmap-snap->mapofs-snap->nent < 2u)
    return 0;
  if (arm64_ir_pcpos((uintptr_t)pc, (uintptr_t)proto_bc(J->pt),
      (uintptr_t)(proto_bc(J->pt)+J->pt->sizebc), &pos)) {
    const IRIns *tmp;
    if (root < REF_FIRST || root >= T->nins) return 0;
    tmp = &T->ir[root];
    if (tmp->op1 < REF_FIRST || tmp->op1 >= root ||
        T->ir[tmp->op1].o != IR_SLOAD ||
        T->ir[tmp->op1].op1 != 0 ||
        T->ir[tmp->op1].t.irt != IRT_FUNC ||
        T->ir[tmp->op1].op2 != IRSLOAD_READONLY)
      return 0;
    *ptp = J->pt;
    return 1;
  }
  memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], sizeof(pcbase));
  {
    LJArm64LuaFramePath frames;
    IRRef function;
    const GCfunc *fn;
    if (!arm64_lua_frame_path(&T->snapmap[snap->mapofs], snap->nent,
                              pcbase, &frames) || frames.depth != 1u)
      return 0;
    function = frames.child[0].funcref;
    /* The child-frame proof later binds this saved value to its exact caller
    ** CALL and guard. Resolve the identity guard here without treating a
    ** dynamic frame value as a standalone source certificate. */
    fn = arm64_lua_leaf_ir_function(T, function, root, NULL);
    if (!fn || !isluafunc(fn) || root >= T->nins ||
        T->ir[root].op1 != function ||
        !arm64_ir_pcpos((uintptr_t)pc, (uintptr_t)proto_bc(funcproto(fn)),
        (uintptr_t)(proto_bc(funcproto(fn))+funcproto(fn)->sizebc), &pos))
      return 0;
    *ptp = funcproto(fn);
  }
  return 1;
}

static int arm64_lua_leaf_branch(const GCproto *pt,
                                 LJArm64LuaLeafBranch *branch)
{
  BCIns words[UINT8_MAX+1u];
  MSize i, size, cmp = UINT32_MAX, cond = UINT32_MAX, join = UINT32_MAX;
  uint8_t first_reach[UINT8_MAX+1u] = {0};
  uint8_t second_reach[UINT8_MAX+1u] = {0};
  if (!pt || !branch || (size = pt->sizebc) < 3 || size > UINT8_MAX ||
      pt->framesize > UINT8_MAX || pt->numparams > pt->framesize)
    return 0;
  memset(branch, 0, sizeof(*branch));
  branch->compare = UINT32_MAX;
  branch->first_ret = branch->second_ret = UINT32_MAX;
  branch->size = size;
  for (i = 0; i < size; i++) {
    words[i] = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[i]);
    branch->next[i] = UINT32_MAX;
  }
  for (i = 1; i+1u < size; i++) {
    BCOp op = bc_op(words[i]);
    if (op >= BC_ISLT && op <= BC_ISGT) {
      if (cmp != UINT32_MAX) return 0;
      cmp = i;
    }
  }
  if (cmp != UINT32_MAX) {
    ptrdiff_t target;
    cond = cmp+1u;
    if (cond >= size || cmp+2u >= size || bc_op(words[cond]) != BC_JMP ||
        bc_a(words[cmp]) >= pt->framesize || bc_d(words[cmp]) >= pt->framesize ||
        bc_a(words[cond]) > pt->framesize)
      return 0;
    target = (ptrdiff_t)cond+1+bc_j(words[cond]);
    if (target <= (ptrdiff_t)(cmp+2u) || target >= (ptrdiff_t)size) return 0;
    branch->compare = cmp; branch->condition_jump = cond;
    branch->first = cmp+2u; branch->second = (MSize)target;
    branch->next[cmp] = UINT32_MAX; /* comparison edges are proof-selected. */
  }
  /* Copy an acyclic source CFG. Ordered comparisons have exactly one
  ** conditional split; all other accepted control flow is forward-only. */
  for (i = 1; i+1u < size; i++) {
    BCOp op = bc_op(words[i]);
    ptrdiff_t target;
    if (i == cmp) continue;
    if (i == cond) {
      branch->next[i] = UINT32_MAX; /* represented by the compare node. */
    } else if (op == BC_JMP) {
      target = (ptrdiff_t)i+1+bc_j(words[i]);
      if (target <= (ptrdiff_t)i || target >= (ptrdiff_t)size ||
          bc_a(words[i]) > pt->framesize) return 0;
      branch->next[i] = (MSize)target;
    } else if (op == BC_RET1 || op == BC_RET0) {
      branch->next[i] = UINT32_MAX;
    } else if (op >= BC_ISLT && op <= BC_ISGT) {
      return 0;
    } else {
      branch->next[i] = i+1u;
    }
  }
  if (cmp != UINT32_MAX) {
    MSize p;
    first_reach[branch->first] = 1;
    second_reach[branch->second] = 1;
    for (p = cmp+2u; p < size; p++) {
      MSize next = branch->next[p];
      if (next != UINT32_MAX) {
        if (first_reach[p]) first_reach[next] = 1;
        if (second_reach[p]) second_reach[next] = 1;
      }
    }
    for (p = cmp+2u; p < size; p++)
      if (first_reach[p] && second_reach[p]) { join = p; break; }
    if (join != UINT32_MAX) {
      if (join <= branch->second) return 0;
      branch->join = join;
      for (p = cmp+2u; p < join; p++) {
        if (first_reach[p] == second_reach[p]) return 0;
        if (first_reach[p]) branch->first_path[p] = 1;
        else branch->second_path[p] = 1;
        if (branch->next[p] == UINT32_MAX || branch->next[p] > join)
          return 0;
      }
      if (!first_reach[join] || !second_reach[join]) return 0;
    } else {
      unsigned nfirst = 0, nsecond = 0;
      if (bc_op(words[size-1u]) != BC_RET0) return 0;
      for (p = cmp+2u; p+1u < size; p++) {
        if (first_reach[p] && second_reach[p]) return 0;
        if (first_reach[p]) {
          branch->first_path[p] = 1;
          if (bc_op(words[p]) == BC_RET1) {
            if (bc_d(words[p]) != 2u || branch->next[p] != UINT32_MAX)
              return 0;
            branch->first_ret = p; nfirst++;
          } else if (branch->next[p] == UINT32_MAX ||
                     branch->next[p] <= p || branch->next[p] >= size) {
            return 0;
          }
        } else if (second_reach[p]) {
          branch->second_path[p] = 1;
          if (bc_op(words[p]) == BC_RET1) {
            if (bc_d(words[p]) != 2u || branch->next[p] != UINT32_MAX)
              return 0;
            branch->second_ret = p; nsecond++;
          } else if (branch->next[p] == UINT32_MAX ||
                     branch->next[p] <= p || branch->next[p] >= size) {
            return 0;
          }
        } else if (bc_op(words[p]) != BC_JMP ||
                   branch->next[p] != size-1u) {
          return 0;
        }
      }
      if (nfirst != 1u || nsecond != 1u ||
          branch->first_ret == branch->second_ret) return 0;
      branch->early_return = 1;
      branch->join = size-1u;
    }
  }
  for (i = 0; i < size; i++)
    if ((BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[i]) != words[i])
      return 0;
  return 1;
}

/* Fixed result windows are admitted only when source RET arity agrees with
** the caller's BC_CALL B field. Open and padded result counts stay closed. */
static MSize arm64_lua_leaf_return_count(const GCproto *pt)
{
  BCIns ret;
  if (!pt || pt->sizebc < 2u) return 0;
  ret = (BCIns)la_load32_acq(
    (const uint32_t *)&proto_bc(pt)[pt->sizebc-1u]);
  if (bc_op(ret) == BC_RET1 && bc_d(ret) == 2u) return 1u;
  if (bc_op(ret) == BC_RET && bc_d(ret) == 3u) return 2u;
  if (bc_op(ret) == BC_RET0) return 1u;  /* Two early RET1 arms. */
  return 0;
}

/* Independently decode the bounded zero-formal FUNCV input window. Fixed
** VARG results are padded with nil or truncated against the exact CALL input
** count; open and alternate destinations remain outside this slice. */
static int arm64_lua_leaf_ir_varg_shape(const GCproto *pt,
                                        MSize *dstp, MSize *countp)
{
  MSize p, found = 0, dst = 0, count = 0;
  if (!pt || !(la_load8_acq(&pt->flags) & PROTO_VARARG) || pt->numparams != 0 ||
      pt->sizebc < 3u || pt->sizebc > UINT8_MAX ||
      pt->framesize > UINT8_MAX) return 0;
  for (p = 1; p+1u < pt->sizebc; p++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[p]);
    BCOp op = bc_op(ins);
    if (op == BC_VARG) {
      if (++found != 1u || p != 1u || bc_a(ins) != 0u ||
          bc_b(ins) < 2u || bc_c(ins) != 0u) return 0;
      dst = bc_a(ins); count = bc_b(ins)-1u;
      if (dst+count > pt->framesize) return 0;
    }
  }
  if (found != 1u) return 0;
  if (dstp) *dstp = dst;
  if (countp) *countp = count;
  return 1;
}

static int arm64_lua_leaf_numeric_range(const GCproto *pt, MSize first,
                                        MSize last, uint8_t *numeric)
{
  MSize i;
  for (i = first; i < last; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[i]);
    BCOp op = bc_op(ins);
    MSize dst = bc_a(ins), left = bc_b(ins), right = bc_c(ins);
    if (op == BC_VARG) {
      if (!(la_load8_acq(&pt->flags) & PROTO_VARARG) || i != 1u ||
          dst != 0u || bc_b(ins) < 2u || bc_c(ins) != 0u ||
          dst+(bc_b(ins)-1u) > pt->framesize) return 0;
      /* The exact requested values are seeded from the CALL map. */
    } else if (op == BC_CGET || op == BC_MOV || op == BC_CSET) {
      numeric[dst] = numeric[bc_d(ins)];
    } else if (op == BC_KPRI) {
      if (bc_d(ins) > 2u) return 0;
      numeric[dst] = 0;
    } else if (op == BC_CALL) {
      numeric[dst] = 0;  /* A nested result needs the frame-chain proof. */
    } else if (op == BC_UGET) {
      numeric[dst] = 0;
    } else if (op == BC_KSHORT || op == BC_KNUM) {
      numeric[dst] = 1;
    } else {
      if (!numeric[left] ||
          (op >= BC_ADDVV && op <= BC_DIVVV && !numeric[right])) return 0;
      numeric[dst] = 1;
    }
  }
  return 1;
}

static int arm64_lua_leaf_numeric_path(const GCproto *pt,
                                      const LJArm64LuaLeafBranch *branch,
                                      MSize first, uint8_t *numeric)
{
  MSize pc = first, steps = 0;
  MSize terminal = first == branch->first ? branch->first_ret :
                   branch->second_ret;
  while (pc != branch->join && steps++ < branch->size) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[pc]);
    BCOp op = bc_op(ins);
    MSize dst = bc_a(ins), left = bc_b(ins), right = bc_c(ins);
    MSize next = branch->next[pc];
    if (branch->early_return && op == BC_RET1)
      return pc == terminal && bc_d(ins) == 2u && numeric[dst];
    if (next == UINT32_MAX || next <= pc || next > branch->join) return 0;
    if (op != BC_JMP) {
      if (op == BC_CGET || op == BC_MOV || op == BC_CSET) {
        numeric[dst] = numeric[bc_d(ins)];
      } else if (op == BC_KPRI) {
        if (bc_d(ins) > 2u) return 0;
        numeric[dst] = 0;
      } else if (op == BC_CALL) {
        numeric[dst] = 0;
      } else if (op == BC_UGET) {
        numeric[dst] = 0;
      } else if (op == BC_KSHORT || op == BC_KNUM) {
        numeric[dst] = 1;
      } else {
        if (!numeric[left] ||
            (op >= BC_ADDVV && op <= BC_DIVVV && !numeric[right])) return 0;
        numeric[dst] = 1;
      }
    }
    pc = next;
  }
  return pc == branch->join;
}

static int arm64_lua_leaf_type_range(const GCproto *pt, MSize first,
                                     MSize last, IRType *types)
{
  MSize i;
  for (i = first; i < last; i++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[i]);
    BCOp op = bc_op(ins);
    MSize dst = bc_a(ins), left = bc_b(ins), right = bc_c(ins);
    if (op == BC_VARG) {
      if (!(la_load8_acq(&pt->flags) & PROTO_VARARG) || i != 1u ||
          dst != 0u || bc_b(ins) < 2u || bc_c(ins) != 0u ||
          dst+(bc_b(ins)-1u) > pt->framesize) return 0;
      /* The exact requested values are seeded from the CALL map. */
    } else if (op == BC_CGET || op == BC_MOV || op == BC_CSET) {
      types[dst] = types[bc_d(ins)];
    } else if (op == BC_KPRI) {
      if (bc_d(ins) > 2u) return 0;
      types[dst] = (IRType)(IRT_NIL+bc_d(ins));
    } else if (op == BC_CALL) {
      types[dst] = IRT_NIL;  /* Requires the independent nested-frame proof. */
    } else if (op == BC_UGET) {
      if (bc_d(ins) >= pt->sizeuv) return 0;
      types[dst] = IRT_FUNC;
    } else if (op == BC_KSHORT) {
      types[dst] = IRT_INT;
    } else if (op == BC_KNUM) {
      types[dst] = IRT_NUM;
    } else {
      if ((types[left] != IRT_INT && types[left] != IRT_NUM) ||
          (op >= BC_ADDVV && op <= BC_DIVVV &&
           types[right] != IRT_INT && types[right] != IRT_NUM)) return 0;
      types[dst] = IRT_NUM;
    }
  }
  return 1;
}

static int arm64_lua_leaf_type_path(const GCproto *pt,
                                   const LJArm64LuaLeafBranch *branch,
                                   MSize first, IRType *types)
{
  MSize pc = first, steps = 0;
  MSize terminal = first == branch->first ? branch->first_ret :
                   branch->second_ret;
  while (pc != branch->join && steps++ < branch->size) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[pc]);
    BCOp op = bc_op(ins);
    MSize dst = bc_a(ins), left = bc_b(ins), right = bc_c(ins);
    MSize next = branch->next[pc];
    if (branch->early_return && op == BC_RET1)
      return pc == terminal && bc_d(ins) == 2u &&
             (types[dst] == IRT_INT || types[dst] == IRT_NUM);
    if (next == UINT32_MAX || next <= pc || next > branch->join) return 0;
    if (op != BC_JMP) {
      if (op == BC_CGET || op == BC_MOV || op == BC_CSET) {
        types[dst] = types[bc_d(ins)];
      } else if (op == BC_KPRI) {
        if (bc_d(ins) > 2u) return 0;
        types[dst] = (IRType)(IRT_NIL+bc_d(ins));
      } else if (op == BC_CALL) {
        types[dst] = IRT_NIL;
      } else if (op == BC_UGET) {
        if (bc_d(ins) >= pt->sizeuv) return 0;
        types[dst] = IRT_FUNC;
      } else if (op == BC_KSHORT) {
        types[dst] = IRT_INT;
      } else if (op == BC_KNUM) {
        types[dst] = IRT_NUM;
      } else {
        if ((types[left] != IRT_INT && types[left] != IRT_NUM) ||
            (op >= BC_ADDVV && op <= BC_DIVVV &&
             types[right] != IRT_INT && types[right] != IRT_NUM)) return 0;
        types[dst] = IRT_NUM;
      }
    }
    pc = next;
  }
  return pc == branch->join;
}

/* A pure leaf identity guard must restore every supplied caller argument.
** Independently normalize the callee formals like rec_func_setup: discard
** extra arguments and fill missing parameters with nil. */
static const GCfunc *arm64_lua_leaf_ir_function(const GCtrace *T, IRRef fn,
                                                IRRef before, IRRef *guardp);
/* ITERC copies its hidden function/state/control triple before CALL setup.
** A missing hidden slot is unchanged root input, so resolve it only through
** its unique guarded root SLOAD. Explicit restart values take precedence. */
/* The hidden iterator triple is not addressable by Lua source locals. Prove
** the same property for imported bytecode before reusing a LOOP restart map
** as the argument source of the optimized second ITERC. */
static int arm64_iterl_scalar_body(const GCproto *pt,
                                  const LJArm64IteratorSource *iter)
{
  MSize pos;
  for (pos = iter->body_pc; pos < iter->call_pc; pos++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[pos]);
    BCOp op = bc_op(ins);
    if (op == BC_JMP) {
      int64_t target = (int64_t)pos+1+(int64_t)bc_j(ins);
      if (target < (int64_t)iter->body_pc ||
          target > (int64_t)iter->loop_pc+1) return 0;
      continue;
    }
    if (op == BC_ISLT || op == BC_ISGE || op == BC_ISLE || op == BC_ISGT)
      continue;
    if (op != BC_CGET && op != BC_CSET && op != BC_MOV &&
        op != BC_KNUM && op != BC_KSHORT && op != BC_KPRI &&
        !(op >= BC_ADDVN && op <= BC_DIVVV)) return 0;
    if (bc_a(ins) >= iter->base-3u && bc_a(ins) < iter->base) return 0;
  }
  return 1;
}

static IRRef arm64_iterl_ir_slot(const GCtrace *T, const SnapShot *snap,
                                MSize slot)
{
  IRRef value = 0, r;
  MSize i;
  for (i = 0; i < snap->nent; i++) {
    SnapEntry e = T->snapmap[snap->mapofs+i];
    if (snap_slot(e) == slot) {
      if (e & 0x00ff0000u) return 0;
      return snap_ref(e);
    }
  }
  for (r = REF_FIRST; r < snap->ref; r++) {
    const IRIns *ir = &T->ir[r];
    if (ir->o == IR_SLOAD && ir->op1 == slot) {
      if (value || ir->op2 != IRSLOAD_TYPECHECK ||
          (ir->t.irt & ~(IRT_TYPE|IRT_ISPHI)) != IRT_GUARD) return 0;
      value = r;
    }
  }
  return value;
}

static IRRef arm64_lua_leaf_ir_args_at(const GCtrace *T,
                                  SnapNo s, IRRef callee, IRRef *args,
                                  const GCproto *parentpt, MSize framebase)
{
  const SnapShot *snap = &T->snap[s];
  IRRef ref = snap->ref, fn = 0, r;
  const GCfunc *function;
  const GCproto *leaf;
  MSize slot, i, pos, nargs, nparams, count = 0, nresults;
  MSize vargdst = 0, vargcount = 0;
  uint8_t isvararg;
  uint8_t seen[UINT8_MAX+1u] = {0};
  uint8_t numeric[UINT8_MAX+1u] = {0};
  IRRef actual[UINT8_MAX+1u] = {0};
  int loopmap = 0;
  int has_call = 0, iterator = bc_op(T->startins) == BC_ITERL;
  LJArm64IteratorSource iter;
  LJArm64LuaLeafBranch branch;
  uint64_t pcbase;
  BCIns call;
  function = arm64_lua_leaf_ir_function(T, callee, ref, NULL);
  if (!function) return 0;
  if (!lj_trace_arm64_lua_leaf(function) ||
      !arm64_lua_leaf_branch(funcproto(function), &branch)) return 0;
  leaf = funcproto(function); nparams = leaf->numparams;
  isvararg = (uint8_t)((la_load8_acq(&leaf->flags) & PROTO_VARARG) != 0);
  if (isvararg && !arm64_lua_leaf_ir_varg_shape(leaf, &vargdst, &vargcount))
    return 0;
  nresults = arm64_lua_leaf_return_count(leaf);
  if (!nresults || (nresults == 2u && branch.compare != UINT32_MAX))
    return 0;
  for (i = 1; i+1u < leaf->sizebc; i++)
    if (bc_op((BCIns)la_load32_acq(
        (const uint32_t *)&proto_bc(leaf)[i])) == BC_CALL && nresults != 1u)
      return 0;
  for (i = 1; i+1u < leaf->sizebc; i++)
    if (bc_op((BCIns)la_load32_acq(
        (const uint32_t *)&proto_bc(leaf)[i])) == BC_CALL)
      has_call = 1;
  if (isvararg && has_call) return 0;
  memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], sizeof(pcbase));
  if (!parentpt ||
      !arm64_ir_pcpos((uintptr_t)(pcbase>>8),
      (uintptr_t)proto_bc(parentpt),
      (uintptr_t)(proto_bc(parentpt)+parentpt->sizebc), &pos)) return 0;
  if (iterator) {
    IRRef loop;
    if (!lj_asm_arm64_iterl_tuple(parentpt, trace_startpc_acq((GCtrace *)T),
                                 T->startins, T->startins, 1, &iter)) return 0;
    for (loop = REF_FIRST; loop < T->nins && T->ir[loop].o != IR_LOOP; loop++) {}
    loopmap = ref == loop && pos == iter.body_pc;
    if (loopmap && !arm64_iterl_scalar_body(parentpt, &iter)) return 0;
  }
  call = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(parentpt)[
    loopmap ? iter.call_pc : pos]);
  slot = framebase+bc_a(call);
  if (iterator && (parentpt != gco2pt(trace_startptgco_acq((GCtrace *)T)) ||
      framebase != 1u+LJ_FR2 || isvararg || has_call || nparams != 2u ||
      nresults != 1u ||
      !lj_asm_arm64_iterl_tuple(parentpt, trace_startpc_acq((GCtrace *)T),
                               T->startins, T->startins, 1, &iter) ||
      (!loopmap && pos != iter.call_pc) || call != iter.callins)) return 0;
  if ((!iterator && bc_op(T->startins) != BC_FORL &&
       !(bc_op(T->startins) == BC_FUNCF &&
         arm64_ir_funcf_leaf_call_source(parentpt))) ||
      (!iterator && bc_op(call) != BC_CALL) ||
      bc_b(call) != nresults+1u || !bc_c(call) ||
      bc_a(call)+bc_c(call)+LJ_FR2 > parentpt->framesize ||
      bc_a(call)+bc_b(call)-1u+LJ_FR2 > parentpt->framesize) return 0;
  nargs = bc_c(call)-1u;
  if (args) {
    MSize init = nparams;
    if (isvararg && vargdst+vargcount > init) init = vargdst+vargcount;
    for (i = 0; i < init; i++) args[i] = REF_NIL;
  }
  for (i = 0; i < snap->nent; i++) {
    SnapEntry sn = T->snapmap[snap->mapofs+i];
    MSize at = snap_slot(sn);
    if (at == slot) {
      if (sn & 0x00ff0000u) return 0;
      if (!loopmap) fn = snap_ref(sn);
    } else if (at >= slot+1u+LJ_FR2 && at < slot+1u+LJ_FR2+nargs) {
      IRRef arg = snap_ref(sn);
      MSize param = at-slot-1u-LJ_FR2;
      if ((sn & 0x00ff0000u) || seen[param] ||
          !arm64_cell_value_ir_ref(T, arg, ref)) return 0;
      seen[param] = 1;
      if (param >= UINT8_MAX+1u) return 0;
      actual[param] = arg;
      if (param < nparams)
        numeric[param] = arm64_scalar_ir_ref(T, arg, ref, IRT_INT) ||
                         arm64_scalar_ir_ref(T, arg, ref, IRT_NUM);
      if (args) args[param] = arg;
      count++;
    }
  }
  if (iterator) {
    IRRef hiddenfn = arm64_iterl_ir_slot(T, snap, slot-3u);
    if (loopmap) fn = hiddenfn;
    if (count || !hiddenfn || hiddenfn != fn) return 0;
    for (i = 0; i < 2u; i++) {
      IRRef arg = arm64_iterl_ir_slot(T, snap, slot-2u+i);
      if (!arg || !arm64_cell_value_ir_ref(T, arg, ref)) return 0;
      actual[i] = arg;
      numeric[i] = arm64_scalar_ir_ref(T, arg, ref, IRT_INT) ||
                   arm64_scalar_ir_ref(T, arg, ref, IRT_NUM);
      if (args) args[i] = arg;
      count++;
    }
  }
  if (count != nargs || !arm64_scalar_ir_ref(T, fn, ref, IRT_FUNC)) return 0;
  if (isvararg) {
    MSize available = nargs > nparams ? nargs-nparams : 0;
    for (i = 0; i < vargcount; i++) {
      MSize source = nparams+i;
      IRRef arg = source < available+nparams ? actual[source] : REF_NIL;
      numeric[vargdst+i] = arg != REF_NIL &&
        (arm64_scalar_ir_ref(T, arg, ref, IRT_INT) ||
         arm64_scalar_ir_ref(T, arg, ref, IRT_NUM));
      if (args) args[vargdst+i] = arg;
    }
  }
  /* A leaf with only FP arithmetic has no child overflow snapshot. Prove
  ** its argument uses here as well, including values overwritten by CSET. */
  if (branch.compare == UINT32_MAX) {
    /* A nested-call result is validated by the independent recursive source
    ** replay at its child-frame snapshot, including arithmetic continuations. */
    if (!has_call &&
        !arm64_lua_leaf_numeric_range(leaf, 1, leaf->sizebc-1u, numeric))
      return 0;
  } else {
    uint8_t first[UINT8_MAX+1u], second[UINT8_MAX+1u];
    BCIns cmp = (BCIns)la_load32_acq(
      (const uint32_t *)&proto_bc(leaf)[branch.compare]);
    if (!arm64_lua_leaf_numeric_range(leaf, 1, branch.compare, numeric)) {
      return 0;
    }
    if (!numeric[bc_a(cmp)] || !numeric[bc_d(cmp)]) {
      return 0;
    }
    memcpy(first, numeric, sizeof(first));
    memcpy(second, numeric, sizeof(second));
    if (!arm64_lua_leaf_numeric_path(leaf, &branch, branch.first, first) ||
        !arm64_lua_leaf_numeric_path(leaf, &branch, branch.second, second))
      {
        return 0;
      }
    if (!branch.early_return) {
      for (i = 0; i < leaf->framesize; i++)
        numeric[i] = first[i] && second[i];
      i = branch.join;
      if (!arm64_lua_leaf_numeric_range(leaf, i, leaf->sizebc-1u, numeric))
        return 0;
    }
  }
  if (fn == callee) {
    return fn;
  }
  for (r = REF_FIRST; r <= ref && r < T->nins; r++) {
    const IRIns *guard = &T->ir[r];
    if (guard->o == IR_EQ && guard->t.irt == (IRT_FUNC|IRT_GUARD) &&
        guard->op1 == fn && guard->op2 == callee) {
      return fn;
    }
  }
  return 0;
}

static IRRef arm64_lua_leaf_ir_args(const jit_State *J, const GCtrace *T,
                                  SnapNo s, IRRef callee, IRRef *args)
{
  return arm64_lua_leaf_ir_args_at(T, s, callee, args, J->pt,
                                   1u+LJ_FR2);
}

static int arm64_lua_leaf_ir_snapshot(const jit_State *J, const GCtrace *T,
                                    IRRef ref, SnapNo s)
{
  const SnapShot *snap = &T->snap[s];
  uint64_t pcbase;
  LJArm64LuaFramePath frames;
  const GCproto *parentpt = J->pt;
  MSize framebase = 1u+LJ_FR2;
  if (snap->ref != ref) return 0;
  memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], sizeof(pcbase));
  if (!arm64_lua_frame_path(&T->snapmap[snap->mapofs], snap->nent,
                            pcbase, &frames)) return 0;
  if (frames.depth) {
    const GCfunc *parent = arm64_lua_leaf_ir_function(T,
        frames.child[frames.depth-1u].funcref, ref, NULL);
    if (!parent || !lj_trace_arm64_lua_leaf(parent)) return 0;
    parentpt = funcproto(parent);
    framebase = frames.child[frames.depth-1u].baseslot;
  }
  return arm64_lua_leaf_ir_args_at(T, s, T->ir[ref].op2, NULL,
                                   parentpt, framebase) == T->ir[ref].op1;
}

/* Resolve a numeric source literal in the already-validated constant pool.
** Keep negative zero distinct; it cannot be represented by a KINT. */
static IRRef arm64_lua_leaf_ir_literal(const GCtrace *T, lua_Number number,
                                     int numonly)
{
  TValue tv;
  IRRef r;
  int integer;
  setnumV(&tv, number);
  integer = !numonly && lj_num2int_ok(number) && !tvismzero(&tv);
  for (r = T->nk; r < REF_TRUE; r++) {
    const IRIns *k = &T->ir[r];
    if (integer && k->o == IR_KINT && number == (lua_Number)k->i) return r;
    if (!integer && k->o == IR_KNUM &&
        T->ir[r+1u].tv.u64 == tv.u64) return r;
    if (k->o == IR_KNUM || k->o == IR_KGC || k->o == IR_KKPTR) r++;
  }
  return 0;
}

static IRRef arm64_lua_leaf_ir_num(const GCtrace *T, IRRef value, IRRef end)
{
  IRRef r;
  if (arm64_scalar_ir_ref(T, value, end, IRT_NUM)) return value;
  if (!arm64_scalar_ir_ref(T, value, end, IRT_INT)) return 0;
  if (irref_isk(value))
    return arm64_lua_leaf_ir_literal(T, (lua_Number)T->ir[value].i, 1);
  for (r = value+1u; r < end; r++) {
    const IRIns *ir = &T->ir[r];
    if (ir->o == IR_CONV && irt_type(ir->t) == IRT_NUM &&
        ir->op1 == value && ir->op2 == IRCONV_NUM_INT) return r;
  }
  return 0;
}

/* Bind the inlined source condition, its recorder guard and exact cold edge.
** NUM relation variants preserve Lua's unordered/NaN behavior. */
static int arm64_lua_leaf_ir_branch_cold(const GCtrace *T,
                                         const GCproto *pt,
                                         const LJArm64LuaLeafBranch *branch,
                                         const IRRef *slots, IRRef ref,
                                         MSize *coldp)
{
  BCIns source;
  BCOp bcop;
  IROp op;
  IRType lefttype, righttype, type;
  IRRef left, right;
  const IRIns *guard;
  IROp candidates[2];
  MSize selected = 0, matches = 0;
  unsigned ci;
  if (!T || !pt || !branch || branch->compare == UINT32_MAX || !slots ||
      !coldp ||
      ref < REF_FIRST || ref >= T->nins) return 0;
  source = (BCIns)la_load32_acq((const uint32_t *)
    &proto_bc(pt)[branch->compare]);
  bcop = bc_op(source);
  if (bc_a(source) >= pt->framesize || bcop < BC_ISLT || bcop > BC_ISGT ||
      bc_d(source) >= pt->framesize)
    return 0;
  left = slots[bc_a(source)]; right = slots[bc_d(source)];
  if (left < T->nk || (left >= REF_TRUE && left < REF_FIRST) ||
      (left >= REF_FIRST && left >= ref) || right < T->nk ||
      (right >= REF_TRUE && right < REF_FIRST) ||
      (right >= REF_FIRST && right >= ref)) return 0;
  lefttype = irt_type(T->ir[left].t);
  righttype = irt_type(T->ir[right].t);
  if ((lefttype != IRT_INT && lefttype != IRT_NUM) ||
      (righttype != IRT_INT && righttype != IRT_NUM)) return 0;
  type = lefttype == IRT_INT && righttype == IRT_INT ? IRT_INT : IRT_NUM;
  if (type == IRT_NUM) {
    left = arm64_lua_leaf_ir_num(T, left, ref);
    right = arm64_lua_leaf_ir_num(T, right, ref);
    if (!left || !right) return 0;
  }
  op = (IROp)((int)bcop-(int)BC_ISLT+(int)IR_LT);
  if (type == IRT_NUM && (op & 1)) op = (IROp)(op^4);
  guard = &T->ir[ref];
  if (guard->t.irt != (IRT_GUARD|type)) return 0;
  candidates[0] = op;
  candidates[1] = (IROp)(op^(type == IRT_NUM ? 5 : 1));
  for (ci = 0; ci < 2; ci++) {
    MSize cold, found = 0;
    SnapNo s;
    int cond;
    if (!((guard->op1 == left && guard->op2 == right &&
           guard->o == candidates[ci]) ||
          (guard->op1 == right && guard->op2 == left &&
           guard->o == (IROp)(candidates[ci]^3)))) continue;
    cond = ((int)bcop ^ (int)candidates[ci]) & 1;
    cold = cond ? branch->second : branch->first;
    for (s = 0; s < T->nsnap; s++) {
      MSize ofs = T->snap[s].mapofs, nent = T->snap[s].nent;
      uint64_t footer;
      if (ofs > T->nsnapmap || nent > T->nsnapmap-ofs ||
          T->nsnapmap-ofs-nent < 2u) return 0;
      memcpy(&footer, &T->snapmap[ofs+nent], sizeof(footer));
      if (T->snap[s].ref == ref && (uint8_t)footer &&
          (const BCIns *)(uintptr_t)(footer>>8) == &proto_bc(pt)[cold])
        found++;
    }
    if (found == 1u) { selected = cold; matches++; }
  }
  if (matches != 1u) return 0;
  *coldp = selected;
  return 1;
}

static int arm64_lua_leaf_ir_branch_guard(const GCtrace *T,
                                          const GCproto *pt,
                                          const LJArm64LuaLeafBranch *branch,
                                          const IRRef *slots, IRRef ref,
                                          MSize coldpos)
{
  MSize cold;
  int ok = arm64_lua_leaf_ir_branch_cold(T, pt, branch, slots, ref, &cold);
  return ok && coldpos == cold;
}

static int arm64_lua_leaf_ir_branch_hot(const GCtrace *T, const GCproto *pt,
                                        const LJArm64LuaLeafBranch *branch,
                                        const IRRef *slots, IRRef end,
                                        MSize *hotp)
{
  IRRef ref;
  MSize cold, hot, found = 0;
  if (!T || !pt || !branch || branch->compare == UINT32_MAX || !hotp)
    return 0;
  for (ref = REF_FIRST; ref < end && ref < T->nins; ref++) {
    if (!arm64_lua_leaf_ir_branch_cold(T, pt, branch, slots, ref, &cold))
      continue;
    hot = cold == branch->first ? branch->second : branch->first;
    if (found++) return 0;
    *hotp = hot;
  }
  return found == 1u;
}

static IRRef arm64_lua_leaf_postra_num(const LJArm64PostRAView *v,
                                      IRRef value, IRRef end);

static int arm64_lua_leaf_postra_branch_cold(
                                      const LJArm64PostRAView *v,
                                      const GCproto *pt,
                                      const LJArm64LuaLeafBranch *branch,
                                      const IRRef *slots, IRRef ref,
                                      MSize *coldp)
{
  BCIns source;
  BCOp bcop;
  IROp op;
  IRType lefttype, righttype, type;
  IRRef left, right;
  IRIns guard;
  IROp candidates[2];
  MSize selected = 0, matches = 0;
  unsigned ci;
  if (!v || !pt || !branch || branch->compare == UINT32_MAX || !slots ||
      !coldp || ref < REF_FIRST || ref >= v->nins) return 0;
  source = (BCIns)la_load32_acq((const uint32_t *)
    &proto_bc(pt)[branch->compare]);
  bcop = bc_op(source);
  if (bc_a(source) >= pt->framesize || bcop < BC_ISLT || bcop > BC_ISGT ||
      bc_d(source) >= pt->framesize)
    return 0;
  left = slots[bc_a(source)]; right = slots[bc_d(source)];
  if (left < v->nk || (left >= REF_TRUE && left < REF_FIRST) ||
      (left >= REF_FIRST && left >= ref) || right < v->nk ||
      (right >= REF_TRUE && right < REF_FIRST) ||
      (right >= REF_FIRST && right >= ref)) return 0;
  lefttype = irt_type(ir_load_acq(&v->ir[left]).t);
  righttype = irt_type(ir_load_acq(&v->ir[right]).t);
  if ((lefttype != IRT_INT && lefttype != IRT_NUM) ||
      (righttype != IRT_INT && righttype != IRT_NUM)) return 0;
  type = lefttype == IRT_INT && righttype == IRT_INT ? IRT_INT : IRT_NUM;
  if (type == IRT_NUM) {
    left = arm64_lua_leaf_postra_num(v, left, ref);
    right = arm64_lua_leaf_postra_num(v, right, ref);
    if (!left || !right) return 0;
  }
  op = (IROp)((int)bcop-(int)BC_ISLT+(int)IR_LT);
  if (type == IRT_NUM && (op & 1)) op = (IROp)(op^4);
  guard = ir_load_acq(&v->ir[ref]);
  if (guard.t.irt != (IRT_GUARD|type)) return 0;
  candidates[0] = op;
  candidates[1] = (IROp)(op^(type == IRT_NUM ? 5 : 1));
  for (ci = 0; ci < 2; ci++) {
    MSize cold, found = 0;
    MSize s;
    int cond;
    if (!((guard.op1 == left && guard.op2 == right &&
           guard.o == candidates[ci]) ||
          (guard.op1 == right && guard.op2 == left &&
           guard.o == (IROp)(candidates[ci]^3)))) continue;
    cond = ((int)bcop ^ (int)candidates[ci]) & 1;
    cold = cond ? branch->second : branch->first;
    for (s = 0; s < v->nsnap; s++) {
      MSize ofs = snap_mapofs_acq(&v->snap[s]);
      MSize nent = snap_nent_acq(&v->snap[s]);
      SnapEntry words[2];
      uint64_t footer;
      if (ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
          v->nsnapmap-ofs-nent < 2u) return 0;
      words[0] = snapentry_acq(&v->snapmap[ofs+nent]);
      words[1] = snapentry_acq(&v->snapmap[ofs+nent+1u]);
      memcpy(&footer, words, sizeof(footer));
      if (snap_ref_acq(&v->snap[s]) == ref && (uint8_t)footer &&
          (const BCIns *)(uintptr_t)(footer>>8) == &proto_bc(pt)[cold])
        found++;
    }
    if (found == 1u) { selected = cold; matches++; }
  }
  if (matches != 1u) return 0;
  *coldp = selected;
  return 1;
}

static int arm64_lua_leaf_postra_branch_hot(
                                      const LJArm64PostRAView *v,
                                      const GCproto *pt,
                                      const LJArm64LuaLeafBranch *branch,
                                      const IRRef *slots, IRRef end,
                                      MSize *hotp)
{
  IRRef ref;
  MSize cold, hot, found = 0;
  if (!v || !pt || !branch || branch->compare == UINT32_MAX || !hotp)
    return 0;
  for (ref = REF_FIRST; ref < end && ref < v->nins; ref++) {
    if (!arm64_lua_leaf_postra_branch_cold(v, pt, branch, slots, ref, &cold))
      continue;
    hot = cold == branch->first ? branch->second : branch->first;
    if (found++) return 0;
    *hotp = hot;
  }
  return found == 1u;
}

/* Replay a source expression through the recorder's surviving SSA values.
** Integer identities, constant folding, multiply-by-two and commutative CSE
** do not introduce a new value. Other integer arithmetic must retain its
** overflow guard; an unchecked wrapping operation is never a substitute. */
static IRRef arm64_lua_leaf_ir_arith(const GCtrace *T, IROp op,
                                    IRRef left, IRRef right, IRRef end)
{
  IRRef r, a = left, b = right;
  IROp sourceop = op, want = op;
  IRType type = IRT_NUM;
  if (op != IR_DIV && arm64_scalar_ir_ref(T, a, end, IRT_INT) &&
      arm64_scalar_ir_ref(T, b, end, IRT_INT)) {
    if (irref_isk(a) && irref_isk(b)) {
      int64_t x = T->ir[a].i, y = T->ir[b].i;
      int64_t z = op == IR_ADD ? x+y : op == IR_SUB ? x-y : x*y;
      if (z >= INT32_MIN && z <= INT32_MAX)
        return arm64_lua_leaf_ir_literal(T, (lua_Number)z, 0);
    }
    if ((op == IR_ADD || op == IR_MUL) && irref_isk(a)) {
      IRRef tmp = a; a = b; b = tmp;
    }
    if (op == IR_SUB && a == b)
      return arm64_lua_leaf_ir_literal(T, 0, 0);
    if (irref_isk(b)) {
      int32_t k = T->ir[b].i;
      if ((op == IR_ADD || op == IR_SUB) && !k) return a;
      if (op == IR_MUL) {
        if (!k) return b;
        if (k == 1) return a;
        if (k == 2) { op = IR_ADD; b = a; }
      }
    }
    want = (IROp)(op-IR_ADD+IR_ADDOV); type = IRT_INT;
    for (r = REF_FIRST; r < end; r++) {
      const IRIns *ir = &T->ir[r];
      if (ir->o == want && irt_type(ir->t) == type &&
          irt_isguard(ir->t) &&
          ((ir->op1 == a && ir->op2 == b) ||
           (op != IR_SUB && ir->op1 == b && ir->op2 == a))) return r;
    }
    /* An observed integer overflow records NUM arithmetic instead. */
    op = sourceop;
  }
  a = arm64_lua_leaf_ir_num(T, left, end);
  b = arm64_lua_leaf_ir_num(T, right, end);
  if (!a || !b) return 0;
  for (r = REF_FIRST; r < end; r++) {
    const IRIns *ir = &T->ir[r];
    if (ir->o == op && irt_type(ir->t) == IRT_NUM &&
        ((ir->op1 == a && ir->op2 == b) ||
         ((op == IR_ADD || op == IR_MUL) &&
          ir->op1 == b && ir->op2 == a))) return r;
  }
  return 0;
}

/* Resolve a source CALL's exact function value from its IR value and an
** identity guard already present in the trace. This is deliberately separate
** from the post-RA resolver below. */
static const GCfunc *arm64_lua_leaf_ir_function(const GCtrace *T, IRRef fn,
                                                IRRef before, IRRef *guardp)
{
  IRRef r;
  if (guardp) *guardp = 0;
  if (!arm64_scalar_ir_ref(T, fn, before, IRT_FUNC)) return NULL;
  if (irref_isk(fn) && T->ir[fn].o == IR_KGC) {
    GCobj *o = ir_kgc_acq(&T->ir[fn]);
    if (o && o->gch.gct == (uint32_t)~LJ_TFUNC) return gco2func(o);
  }
  for (r = REF_FIRST; r < before && r < T->nins; r++) {
    const IRIns *guard = &T->ir[r];
    IRRef constant;
    if (guard->o != IR_EQ || guard->t.irt != (IRT_FUNC|IRT_GUARD) ||
        (guard->op1 != fn && guard->op2 != fn)) continue;
    constant = guard->op1 == fn ? guard->op2 : guard->op1;
    if (!irref_isk(constant) || constant < T->nk || constant >= REF_TRUE ||
        !arm64_scalar_ir_ref(T, constant, before, IRT_FUNC) ||
        T->ir[constant].o != IR_KGC) continue;
    {
      GCobj *o = ir_kgc_acq(&T->ir[constant]);
      if (o && o->gch.gct == (uint32_t)~LJ_TFUNC) {
        if (guardp) *guardp = r;
        return gco2func(o);
      }
    }
  }
  return NULL;
}

/* Evaluate a straight-line pure leaf through a bounded chain of ordinary
** fixed-result calls. The returned slots are source values at `pc`; nested
** calls are recursively evaluated and their exact IR result feeds the
** caller's continuation. Frame maps and physical locations are validated by
** the separate snapshot/post-RA walks. */
static int arm64_lua_leaf_ir_eval_multi(const jit_State *J, const GCtrace *T,
                                  const GCproto *pt,
                                  const IRRef *args, MSize pc, IRRef before,
                                  IRRef end, const GCfunc **ancestors,
                                  MSize depth, IRRef *slots, IRRef *results,
                                  MSize *nresultsp)
{
  const BCIns *bc;
  IRRef state[UINT8_MAX+1u] = {0};
  MSize p, i;
  int captured = 0;
  if (!T || !pt || !args || !ancestors || !slots || !results || !nresultsp ||
      pt->framesize > UINT8_MAX || pt->numparams > pt->framesize ||
      pc < 1u || pc >= pt->sizebc || depth > LJ_ARM64_LUA_MAX_CHILD_FRAMES)
    return 0;
  bc = proto_bc(pt);
  for (i = 0; i < pt->numparams; i++) state[i] = args[i];
  for (p = 1; p+1u < pt->sizebc; p++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[p]);
    BCOp op = bc_op(ins);
    BCReg a = bc_a(ins), b = bc_b(ins), c = bc_c(ins), d = bc_d(ins);
    IRRef value = 0, left, right;
    TValue k;
    IROp arith;
    if (p == pc) {
      memcpy(slots, state, sizeof(state));
      captured = 1;
    }
    if (a >= pt->framesize) return 0;
    if (op == BC_CGET || op == BC_MOV || op == BC_CSET) {
      if (d >= pt->framesize || !state[d]) return 0;
      value = state[d];
      if (op == BC_CSET &&
          ((state[a] && !arm64_cell_value_ir_ref(T, state[a], before)) ||
           !arm64_cell_value_ir_ref(T, value, before))) return 0;
    } else if (op == BC_KSHORT) {
      value = arm64_lua_leaf_ir_literal(T, (lua_Number)(int16_t)d, end);
    } else if (op == BC_KPRI) {
      if (d > 2u) return 0;
      value = (IRRef)(REF_NIL-d);
    } else if (op == BC_KNUM) {
      if (d >= pt->sizekn) return 0;
      proto_knumtv_load_acq(&k, pt, d);
      if (!tvisnumber(&k)) return 0;
      value = arm64_lua_leaf_ir_literal(T, numberVnum(&k), end);
    } else if (op == BC_UGET) {
      if (d >= pt->sizeuv) return 0;
      value = arm64_lua_leaf_ir_upvalue(J, T, pt, p, before, end);
    } else if (op == BC_CALL) {
      const GCfunc *callee;
      const GCfunc *nested[LJ_ARM64_LUA_MAX_CHILD_FRAMES+1u];
      IRRef childargs[UINT8_MAX+1u], childslots[UINT8_MAX+1u];
      IRRef childresults[2] = {0, 0};
      MSize childnresults = 0;
      MSize nargs;
      if ((bc_b(ins) != 2u && bc_b(ins) != 3u) || bc_c(ins) < 2u ||
          (MSize)a+bc_c(ins)+LJ_FR2 > pt->framesize ||
          depth >= LJ_ARM64_LUA_MAX_CHILD_FRAMES || !state[a]) return 0;
      nargs = bc_c(ins)-1u;
      IRRef callguard = 0;
      callee = arm64_lua_leaf_ir_function(T, state[a], end, &callguard);
      if (!callee || !lj_trace_arm64_lua_leaf(callee)) return 0;
      for (i = 0; i < depth; i++) if (ancestors[i] == callee) return 0;
      memcpy(nested, ancestors, depth*sizeof(*nested));
      nested[depth] = callee;
      for (i = 0; i < funcproto(callee)->numparams; i++) {
        childargs[i] = i < nargs ? state[a+1u+LJ_FR2+i] : REF_NIL;
        if (!arm64_cell_value_ir_ref(T, childargs[i], end)) return 0;
      }
      for (; i < UINT8_MAX+1u; i++) childargs[i] = REF_NIL;
      if (!arm64_lua_leaf_ir_eval_multi(J, T, funcproto(callee), childargs,
          funcproto(callee)->sizebc-1u, before, end, nested, depth+1u,
          childslots, childresults, &childnresults) ||
          childnresults != bc_b(ins)-1u) return 0;
      for (i = 0; i < childnresults; i++)
        if (!arm64_cell_value_ir_ref(T, childresults[i], end) ||
            (callguard && callguard >= childresults[i])) return 0;
      value = childresults[0];
      if (childnresults == 2u) state[a+1u] = childresults[1];
    } else if (op == BC_ADDVV || op == BC_SUBVV || op == BC_MULVV ||
               op == BC_DIVVV || op == BC_ADDVN || op == BC_SUBVN ||
               op == BC_MULVN || op == BC_DIVVN || op == BC_ADDNV ||
               op == BC_SUBNV || op == BC_MULNV || op == BC_DIVNV) {
      left = state[b];
      if (!left) return 0;
      if (op == BC_ADDVV || op == BC_SUBVV || op == BC_MULVV ||
          op == BC_DIVVV) {
        if (c >= pt->framesize || !state[c]) return 0;
        right = state[c];
      } else {
        if (c >= pt->sizekn) return 0;
        proto_knumtv_load_acq(&k, pt, c);
        if (!tvisnumber(&k)) return 0;
        right = arm64_lua_leaf_ir_literal(T, numberVnum(&k), end);
        if (op == BC_ADDNV || op == BC_SUBNV || op == BC_MULNV ||
            op == BC_DIVNV) { IRRef tmp = left; left = right; right = tmp; }
      }
      switch (op) {
      case BC_ADDVV: case BC_ADDVN: case BC_ADDNV: arith = IR_ADD; break;
      case BC_SUBVV: case BC_SUBVN: case BC_SUBNV: arith = IR_SUB; break;
      case BC_MULVV: case BC_MULVN: case BC_MULNV: arith = IR_MUL; break;
      default: arith = IR_DIV; break;
      }
      value = arm64_lua_leaf_ir_arith(T, arith, left, right, end);
      if (!value) return 0;
    } else {
      return 0;  /* First slice has no branch/heap/upvalue effects. */
    }
    if (!value || !arm64_cell_value_ir_ref(T, value, end)) return 0;
    state[a] = value;
    if (op == BC_CALL) {
      MSize firstarg = (MSize)a+1u+LJ_FR2;
      MSize argend = firstarg+bc_c(ins)-1u;
      for (i = firstarg; i < argend; i++) state[i] = 0;
    }
  }
  if (pc == pt->sizebc-1u) {
    memcpy(slots, state, sizeof(state));
    captured = 1;
  }
  if (!captured) return 0;
  {
    BCIns ret = (BCIns)la_load32_acq((const uint32_t *)&bc[pt->sizebc-1u]);
    if (bc_op(ret) == BC_CALLT) {
      const GCfunc *callee;
      const GCfunc *nested[LJ_ARM64_LUA_MAX_CHILD_FRAMES+1u];
      IRRef childargs[UINT8_MAX+1u], childslots[UINT8_MAX+1u];
      IRRef childresults[2] = {0, 0};
      MSize childnresults = 0;
      IRRef callguard = 0;
      BCReg a = bc_a(ret);
      if (bc_b(ret) != 0u || bc_c(ret) != 2u ||
          (MSize)a+bc_c(ret)+LJ_FR2 > pt->framesize || !state[a] ||
          !state[a+1u+LJ_FR2] ||
          depth >= LJ_ARM64_LUA_MAX_CHILD_FRAMES) return 0;
      callee = arm64_lua_leaf_ir_function(T, state[a], end, &callguard);
      if (!callee || !lj_trace_arm64_lua_leaf(callee) ||
          funcproto(callee)->numparams != 1u) return 0;
      for (i = 0; i < depth; i++) if (ancestors[i] == callee) return 0;
      memcpy(nested, ancestors, depth*sizeof(*nested));
      nested[depth] = callee;
      childargs[0] = state[a+1u+LJ_FR2];
      for (i = 1; i < UINT8_MAX+1u; i++) childargs[i] = REF_NIL;
      if (!arm64_cell_value_ir_ref(T, childargs[0], end) ||
          !arm64_lua_leaf_ir_eval_multi(J, T, funcproto(callee), childargs,
            funcproto(callee)->sizebc-1u, before, end, nested, depth+1u,
            childslots, childresults, &childnresults) || childnresults != 1u ||
          !arm64_cell_value_ir_ref(T, childresults[0], end) ||
          (callguard && callguard >= childresults[0])) return 0;
      state[a] = childresults[0];
      results[0] = childresults[0]; *nresultsp = 1u;
      return 1;
    }
    if (bc_op(ret) == BC_RET1 && bc_d(ret) == 2u &&
        bc_a(ret) < pt->framesize && state[bc_a(ret)]) {
      results[0] = state[bc_a(ret)]; *nresultsp = 1u;
      return 1;
    }
    if (bc_op(ret) == BC_RET && bc_d(ret) == 3u &&
        (MSize)bc_a(ret)+1u < pt->framesize && state[bc_a(ret)] &&
        state[bc_a(ret)+1u]) {
      results[0] = state[bc_a(ret)]; results[1] = state[bc_a(ret)+1u];
      *nresultsp = 2u;
      return 1;
    }
    return 0;
  }
}

static int arm64_lua_leaf_ir_eval(const jit_State *J, const GCtrace *T,
                                  const GCproto *pt, const IRRef *args,
                                  MSize pc, IRRef before, IRRef end,
                                  const GCfunc **ancestors, MSize depth,
                                  IRRef *slots, IRRef *result)
{
  IRRef results[2] = {0, 0};
  MSize nresults = 0;
  if (!arm64_lua_leaf_ir_eval_multi(J, T, pt, args, pc, before, end,
      ancestors, depth, slots, results, &nresults) || nresults != 1u)
    return 0;
  *result = results[0];
  return 1;
}

/* Derive the complete child prefix at its resume PC. CGET/CSET request a new
** snapshot, but an arithmetic bytecode need not: later guards may replay
** several pure operations, including operations removed by CSE. */
static int arm64_lua_leaf_ir_prefix(const GCtrace *T, const GCproto *pt,
                                    MSize pc, const IRRef *args,
                                    IRRef at, IRRef end, int unused,
                                    IRRef *saved, MSize *livep,
                                    IRRef *resultp, MSize *nresultp,
                                    MSize *retposp)
{
#define LEAF_PREFIX_FAIL() 0
  const BCIns *bc = proto_bc(pt);
  IRRef slots[UINT8_MAX+1u] = {0};
  MSize p, live = pt->numparams, vargdst = 0, vargcount = 0;
  MSize hotpath = UINT32_MAX;
  MSize retpos = pt->sizebc-1u;
  int matched = 0;
  LJArm64LuaLeafBranch branch;
  BCIns ret = (BCIns)la_load32_acq((const uint32_t *)&bc[pt->sizebc-1u]);
  if (!arm64_lua_leaf_branch(pt, &branch)) return LEAF_PREFIX_FAIL();
  if ((la_load8_acq(&pt->flags) & PROTO_VARARG) &&
      !arm64_lua_leaf_ir_varg_shape(pt, &vargdst, &vargcount))
    return LEAF_PREFIX_FAIL();
  live = pt->numparams;
  if (vargdst+vargcount > live) live = vargdst+vargcount;
  if (live) memcpy(slots, args, live*sizeof(IRRef));
  for (p = 1; p+1u < pt->sizebc; p++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[p]);
    BCOp op = bc_op(ins);
    MSize a = bc_a(ins);
    IRRef value, left, right, limit = p < pc ? at : end;
    IROp arith = IR_NOP;
    TValue k;
    if ((pc == branch.first || pc == branch.second) &&
        p == branch.compare) {
      memcpy(saved, slots, sizeof(slots)); *livep = live;
      if (!arm64_lua_leaf_ir_branch_hot(T, pt, &branch, slots, end,
                                         &hotpath)) return LEAF_PREFIX_FAIL();
      if (branch.early_return) {
        retpos = pc == branch.first ? branch.first_ret : branch.second_ret;
        *retposp = retpos;
        resultp[0] = 0; resultp[1] = 0;
        *nresultp = 0;  /* This cold child has not returned at the guard. */
        return 1;
      }
      matched = 1;
      p = hotpath-1u;
      continue;
    }
    if (branch.compare != UINT32_MAX && p == branch.compare) {
      MSize hot;
      if (!arm64_lua_leaf_ir_branch_hot(T, pt, &branch, slots, end, &hot))
        return LEAF_PREFIX_FAIL();
      hotpath = hot;
      if (branch.early_return)
        retpos = hotpath == branch.first ? branch.first_ret : branch.second_ret;
      p = hot-1u;
      continue;
    }
    if (p == pc) {
      memcpy(saved, slots, sizeof(slots)); *livep = live;
      if (!(branch.early_return && op == BC_RET1) &&
          !((pc == branch.first || pc == branch.second) &&
            branch.compare != UINT32_MAX) &&
          (op != BC_RET1 && op != BC_RET && op != BC_CGET &&
          op != BC_MOV && op != BC_CSET && op != BC_KSHORT &&
          op != BC_KNUM && op != BC_KPRI &&
          op != BC_VARG &&
          (op < BC_ADDVN || op > BC_DIVVV ||
          (op != BC_ADDVN && op != BC_SUBVN && op != BC_MULVN &&
           op != BC_DIVVN && op != BC_ADDNV && op != BC_SUBNV &&
           op != BC_MULNV && op != BC_DIVNV && op != BC_ADDVV &&
           op != BC_SUBVV && op != BC_MULVV && op != BC_DIVVV)))) return LEAF_PREFIX_FAIL();
    }
    if (branch.early_return && op == BC_RET1) {
      if (p != retpos || bc_d(ins) != 2u) return LEAF_PREFIX_FAIL();
      break;
    }
    if ((op >= BC_ISLT && op <= BC_ISGT))
      continue;
    if (op == BC_JMP) {
      if (branch.compare == UINT32_MAX ||
          !((hotpath == branch.first && branch.first_path[p]) ||
            (hotpath == branch.second && branch.second_path[p]))) return LEAF_PREFIX_FAIL();
      p = branch.next[p]-1u;
      continue;
    }
    if (op == BC_VARG) {
      if (!(la_load8_acq(&pt->flags) & PROTO_VARARG) || p != 1u ||
          bc_a(ins) != vargdst || bc_b(ins) != vargcount+1u ||
          bc_c(ins) != 0u) return LEAF_PREFIX_FAIL();
      continue;  /* args_at already seeded exact supplied values and nils. */
    }
    if (op == BC_CGET || op == BC_MOV || op == BC_CSET) {
      value = slots[bc_d(ins)];
      /* Exact visible TValue tags exclude internal cells, proving the raw
      ** assignment path even when the previous value is nil or a GC value. */
      if (op == BC_CSET &&
          ((slots[a] && !arm64_cell_value_ir_ref(T, slots[a], limit)) ||
           (value && !arm64_cell_value_ir_ref(T, value, limit)))) return LEAF_PREFIX_FAIL();
    } else if (op == BC_KSHORT) {
      value = arm64_lua_leaf_ir_literal(T, (int16_t)bc_d(ins), 0);
    } else if (op == BC_KPRI) {
      if (bc_d(ins) > 2u) return LEAF_PREFIX_FAIL();
      value = (IRRef)(REF_NIL-bc_d(ins));
    } else if (op == BC_KNUM) {
      proto_knumtv_load_acq(&k, pt, bc_d(ins));
      value = arm64_lua_leaf_ir_literal(T, numberVnum(&k), 0);
    } else {
      left = slots[bc_b(ins)];
      if (op >= BC_ADDVV && op <= BC_DIVVV) {
        right = slots[bc_c(ins)];
      } else {
        proto_knumtv_load_acq(&k, pt, bc_c(ins));
        right = arm64_lua_leaf_ir_literal(T, numberVnum(&k), 0);
        if (op >= BC_ADDNV && op <= BC_DIVNV) {
          IRRef tmp = left; left = right; right = tmp;
        }
      }
      switch (op) {
      case BC_ADDVN: case BC_ADDNV: case BC_ADDVV: arith = IR_ADD; break;
      case BC_SUBVN: case BC_SUBNV: case BC_SUBVV: arith = IR_SUB; break;
      case BC_MULVN: case BC_MULNV: case BC_MULVV: arith = IR_MUL; break;
      case BC_DIVVN: case BC_DIVNV: case BC_DIVVV: arith = IR_DIV; break;
      default: return LEAF_PREFIX_FAIL();
      }
      value = left && right ?
        arm64_lua_leaf_ir_arith(T, arith, left, right, limit) : 0;
    }
    /* args proved every operation numeric or a visible-value copy. A pure
    ** value removed by DCE has no SSA identity; zero cannot satisfy any
    ** mapped slot or terminal return below. */
    if (p >= pc && value != at && arith != IR_NOP &&
        T->ir[at].o == IR_CONV && T->ir[at].op2 == IRCONV_NUM_INT &&
        arm64_lua_leaf_ir_num(T, left, limit) == at)
      matched = 1;  /* Interpreter retries this bytecode after int-to-num. */
    if (p >= pc && value == at)
      matched = (T->ir[at].t.irt & ~IRT_ISPHI) == (IRT_INT|IRT_GUARD) ||
                (T->ir[at].t.irt & ~IRT_ISPHI) == (IRT_NUM|IRT_GUARD);
    slots[a] = value;
    if (a >= live) live = a+1u;
    if (branch.compare != UINT32_MAX && p < branch.join &&
        ((hotpath == branch.first && branch.first_path[p]) ||
         (hotpath == branch.second && branch.second_path[p])) &&
        branch.next[p] > p+1u)
      p = branch.next[p]-1u;
  }
  ret = (BCIns)la_load32_acq((const uint32_t *)&bc[retpos]);
  *retposp = retpos;
  *nresultp = bc_op(ret) == BC_RET1 && bc_d(ret) == 2u ? 1u :
              bc_op(ret) == BC_RET && bc_d(ret) == 3u ? 2u :
              bc_op(ret) == BC_RET0 ? 1u : 0u;
  if (!*nresultp || bc_a(ret)+*nresultp > pt->framesize) return LEAF_PREFIX_FAIL();
  resultp[0] = slots[bc_a(ret)];
  resultp[1] = *nresultp == 2u ? slots[bc_a(ret)+1u] : 0;
  if (pc == retpos || (!branch.early_return && pc+1u == pt->sizebc)) {
    /* The caller's assignment can retain the child's final return map.
    ** Every returned or mapped value must exist before that caller guard. */
    if ((bc_op(ret) != BC_RET1 && bc_op(ret) != BC_RET) ||
        bc_a(ret)+*nresultp > live ||
        !arm64_cell_value_ir_ref(T, resultp[0], at) ||
        (*nresultp == 2u && !arm64_cell_value_ir_ref(T, resultp[1], at)))
      return LEAF_PREFIX_FAIL();
    memcpy(saved, slots, sizeof(slots)); *livep = live;
    return 1;
  }
  { int ok = (matched || unused || *nresultp == 2u) &&
         arm64_cell_value_ir_ref(T, resultp[0], end) &&
         (*nresultp == 1u || arm64_cell_value_ir_ref(T, resultp[1], end));
    return ok; }
#undef LEAF_PREFIX_FAIL
}

static int arm64_lua_leaf_ir_needed(const GCproto *pt, MSize resume,
                                    uint8_t *needed);

/* A child result may disappear into caller arithmetic before the first root
** map. Replay that pure continuation from the exact CALL map and bind every
** surviving result-derived slot. A visible child return alone is insufficient:
** changing a final CSET to another defined value must not certify old code. */
static int arm64_lua_leaf_ir_result(const jit_State *J, const GCtrace *T,
                                   SnapNo caller, SnapNo child,
                                   MSize retpos, MSize leafret,
                                   BCReg dest, const IRRef *results,
                                   MSize nresults,
                                   const GCproto *leaf, MSize base,
                                   IRRef callee, IRRef link)
{
  const GCproto *pt = J->pt;
  const BCIns *bc = proto_bc(pt);
  const SnapShot *entry = &T->snap[caller], *next;
  IRRef values[UINT8_MAX+1u] = {0};
  uint8_t derived[UINT8_MAX+1u] = {0}, seen[UINT8_MAX+1u] = {0};
  uint8_t written[UINT8_MAX+1u] = {0};
  SnapNo s = child;
  MSize i, pos, stop, found = 0;
  unsigned consumed = 0;
  uint64_t footer;
  IRRef limit = T->snap[child].ref < J->loopref ? J->loopref : T->nins;
  BCIns call;
  if (pt->framesize > UINT8_MAX || !nresults || nresults > 2u ||
      dest+nresults > pt->framesize || !results[0] ||
      (nresults == 2u && !results[1]))
    return 0;
  if (!retpos || retpos >= pt->sizebc) return 0;
  call = (BCIns)la_load32_acq((const uint32_t *)&bc[retpos-1u]);
  if (bc_op(call) != BC_CALL || bc_b(call) != nresults+1u ||
      bc_a(call) != dest || dest+bc_b(call)-1u+LJ_FR2 > pt->framesize)
    return 0;
  do {
    if (++s >= T->nsnap) return 0;
    next = &T->snap[s];
    if (next->ref >= limit || next->mapofs > T->nsnapmap ||
        next->nent > T->nsnapmap-next->mapofs ||
        T->nsnapmap-next->mapofs-next->nent < 2u) return 0;
    memcpy(&footer, &T->snapmap[next->mapofs+next->nent], sizeof(footer));
    if ((uint8_t)footer+2u == base &&
        (const BCIns *)(uintptr_t)(footer>>8) == proto_bc(leaf)+leafret) {
      BCIns ret = (BCIns)la_load32_acq(
        (const uint32_t *)&proto_bc(leaf)[leafret]);
      BCReg first = bc_a(ret);
      unsigned fn = 0, frame = 0, value[2] = {0, 0};
      /* The later terminal map independently binds this same return to
      ** the caller. Preserve its exact function/frame/value identities. */
      for (i = 0; i < next->nent; i++) {
        SnapEntry sn = T->snapmap[next->mapofs+i];
        if (snap_slot(sn) == base-2u)
          fn += !(sn & 0x00ff0000u) && snap_ref(sn) == callee;
        if (snap_slot(sn) == base-1u)
          frame += (sn & 0x00ff0000u) == SNAP_FRAME && snap_ref(sn) == link;
        if (snap_slot(sn) == base+first)
          value[0] += !(sn & 0x00ff0000u) && snap_ref(sn) == results[0];
        if (nresults == 2u && snap_slot(sn) == base+first+1u)
          value[1] += !(sn & 0x00ff0000u) && snap_ref(sn) == results[1];
      }
      if (fn != 1 || frame != 1 || value[0] != 1 ||
          (nresults == 2u && value[1] != 1)) return 0;
      if (nresults == 1u && bc_op(T->startins) != BC_FUNCF) return 1;
    }
  } while ((uint8_t)footer);
  if (!arm64_ir_pcpos((uintptr_t)(footer>>8), (uintptr_t)bc,
        (uintptr_t)(bc+pt->sizebc), &stop) || stop < retpos) return 0;
  for (i = 0; i < entry->nent; i++) {
    SnapEntry sn = T->snapmap[entry->mapofs+i];
    MSize slot = snap_slot(sn);
    if (slot >= 1u+LJ_FR2 && slot-1u-LJ_FR2 < pt->framesize &&
        !(sn & (SNAP_FRAME|SNAP_CONT)))
      values[slot-1u-LJ_FR2] = snap_ref(sn);
  }
  for (i = 0; i < nresults; i++) {
    values[dest+i] = results[i];
    derived[dest+i] = (uint8_t)(1u << i);
    written[dest+i] = 1;
  }
  for (pos = retpos; pos < stop; pos++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[pos]);
    BCOp op = bc_op(ins);
    BCReg a = bc_a(ins), b = bc_b(ins), c = bc_c(ins), d = bc_d(ins);
    IRRef out = 0;
    unsigned depends = 0;
    if (&bc[pos] == J->startpc && pos+1u == stop &&
        (op == BC_FORL || op == BC_JFORL))
      break;
    if (a >= pt->framesize) return 0;
    if (op == BC_CGET || op == BC_MOV || op == BC_CSET) {
      if (d >= pt->framesize) return 0;
      if (!values[d] && !written[d]) {
        /* A first raw caller load may occur after the leaf returned. Such
        ** an unchanged stack slot need not occur in the earlier CALL map. */
        for (IRRef r = entry->ref; r < next->ref; r++) {
          const IRIns *load = &T->ir[r];
          if (load->o == IR_SLOAD && load->op1 == d+1u+LJ_FR2 &&
              irt_isguard(load->t) &&
              arm64_cell_value_ir_ref(T, r, next->ref)) {
            if (values[d]) return 0;
            values[d] = r;
          }
        }
      }
      out = values[d]; depends = derived[d];
      if (out && !arm64_cell_value_ir_ref(T, out, next->ref)) return 0;
      if (op == BC_CSET) {
        int raw = values[a] &&
          arm64_cell_value_ir_ref(T, values[a], next->ref);
        /* The CALL map can omit an unchanged raw stack slot. Its tag guard
        ** may occur at CSET after the leaf, before this root snapshot. */
        for (IRRef r = REF_FIRST; !raw && !values[a] && r < next->ref; r++) {
          const IRIns *g = &T->ir[r];
          if (g->o == IR_SLOAD && g->op1 == a+1u+LJ_FR2 &&
              irt_isguard(g->t) &&
              (irt_ispri(g->t) || arm64_cell_value_ir_ref(T, r, next->ref)))
            raw = 1;
        }
        if (!raw) return 0;
      }
    } else if (op == BC_KSHORT) {
      out = arm64_lua_leaf_ir_literal(T, (int16_t)d, 0);
    } else if (op == BC_KPRI) {
      if (d > 2u) return 0;
      out = (IRRef)(REF_NIL-d);
    } else if (op == BC_KNUM) {
      TValue k;
      if (d >= pt->sizekn) return 0;
      proto_knumtv_load_acq(&k, pt, d);
      if (!tvisnumber(&k)) return 0;
      out = arm64_lua_leaf_ir_literal(T, numberVnum(&k), 0);
    } else {
      IRRef left, right;
      IROp arith;
      if (b >= pt->framesize) return 0;
      left = values[b]; depends = derived[b];
      if (op >= BC_ADDVV && op <= BC_DIVVV) {
        if (c >= pt->framesize) return 0;
        right = values[c]; depends |= derived[c];
      } else if (op >= BC_ADDVN && op <= BC_DIVNV) {
        TValue k;
        if (c >= pt->sizekn) return 0;
        proto_knumtv_load_acq(&k, pt, c);
        if (!tvisnumber(&k)) return 0;
        right = arm64_lua_leaf_ir_literal(T, numberVnum(&k), 0);
        if (op >= BC_ADDNV) { IRRef tmp = left; left = right; right = tmp; }
      } else return 0;
      switch (op) {
      case BC_ADDVN: case BC_ADDNV: case BC_ADDVV: arith = IR_ADD; break;
      case BC_SUBVN: case BC_SUBNV: case BC_SUBVV: arith = IR_SUB; break;
      case BC_MULVN: case BC_MULNV: case BC_MULVV: arith = IR_MUL; break;
      case BC_DIVVN: case BC_DIVNV: case BC_DIVVV: arith = IR_DIV; break;
      default: return 0;
      }
      out = left && right ?
        arm64_lua_leaf_ir_arith(T, arith, left, right, next->ref) : 0;
    }
    if (depends && !out) return 0;
    values[a] = out; derived[a] = (uint8_t)depends; written[a] = 1;
  }
  for (i = 0; i < next->nent; i++) {
    SnapEntry sn = T->snapmap[next->mapofs+i];
    MSize slot = snap_slot(sn);
    if (slot >= 1u+LJ_FR2 && slot-1u-LJ_FR2 < pt->framesize &&
        derived[slot-1u-LJ_FR2]) {
      slot -= 1u+LJ_FR2;
      if ((sn & 0x00ff0000u) || seen[slot] ||
          snap_ref(sn) != values[slot]) return 0;
      seen[slot] = 1; found++;
      consumed |= derived[slot];
    }
  }
  for (i = 0; i < pt->framesize && i+1u+LJ_FR2 < next->nslots; i++)
    if (derived[i] && !seen[i]) return 0;
  if (bc_op(T->startins) == BC_FUNCF) {
    BCIns ret;
    BCReg dst;
    unsigned bound = 0;
    if (stop+1u != pt->sizebc) return 0;
    ret = (BCIns)la_load32_acq((const uint32_t *)&bc[stop]);
    dst = bc_a(ret);
    if (bc_op(ret) != BC_RET1 || bc_d(ret) != 2u ||
        dst >= pt->framesize || !derived[dst] || !seen[dst]) return 0;
    for (i = 0; i < next->nent; i++) {
      SnapEntry sn = T->snapmap[next->mapofs+i];
      if (snap_slot(sn) == dst+1u+LJ_FR2) {
        if ((sn & 0x00ff0000u) || snap_ref(sn) != values[dst]) return 0;
        bound++;
      }
    }
    if (bound != 1u) return 0;
  }
  return found != 0 && consumed == ((1u << nresults)-1u);
}


/* DCE can retain a child map after removing its arithmetic guard. Only an
** empty or NOP-only interval before the next map is certainly nonexecuting. */
static int arm64_lua_leaf_ir_unused(const GCtrace *T, SnapNo s)
{
  IRRef r, end;
  if (s+1u >= T->nsnap) return 0;
  r = T->snap[s].ref; end = T->snap[s+1u].ref;
  if (r < REF_FIRST || r > end || end >= T->nins) return 0;
  for (; r < end; r++)
    if (T->ir[r].o != IR_NOP) return 0;
  return 1;
}

static int arm64_lua_leaf_ir_needed_range(const GCproto *pt, MSize first,
                                          MSize last, uint8_t *needed)
{
  MSize pc = last;
  while (pc-- > first) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[pc]);
    BCOp op = bc_op(ins);
    MSize a = bc_a(ins), b = bc_b(ins), c = bc_c(ins), d = bc_d(ins);
    if (a >= pt->framesize) return 0;
    if (op == BC_RET1) {
      if (d != 2) return 0;
      needed[a] = 1;
      continue;
    }
    if (op == BC_RET) {
      if (d != 3u || a+1u >= pt->framesize) return 0;
      needed[a] = needed[a+1u] = 1;
      continue;
    }
    if (pc+1u == pt->sizebc) return 0;
    if (op == BC_JMP) continue;
    needed[a] = 0;
    switch (op) {
    case BC_VARG: {
      MSize count = bc_b(ins) ? bc_b(ins)-1u : UINT32_MAX;
      if (!count || count == UINT32_MAX || bc_c(ins) != 0u ||
          a+count > pt->framesize) return 0;
      for (MSize reg = a; reg < a+count; reg++) needed[reg] = 0;
      break;
    }
    case BC_CSET:
      needed[a] = 1;
      /* fallthrough */
    case BC_CGET: case BC_MOV:
      if (d >= pt->framesize) return 0;
      needed[d] = 1;
      break;
    case BC_KSHORT: case BC_KNUM:
      break;
    case BC_UGET:
      if (d >= pt->sizeuv) return 0;
      break;
    case BC_CALLT: {
      MSize arg = a+1u+LJ_FR2;
      if (pc+1u != pt->sizebc || b != 0u || c != 2u ||
          arg >= pt->framesize) return 0;
      needed[a] = needed[arg] = 1;
      break;
    }
    case BC_KPRI:
      if (d > 2u) return 0;
      break;
    case BC_CALL: {
      MSize first = a+1u+LJ_FR2;
      MSize end = first+bc_c(ins)-1u;
      if ((bc_b(ins) != 2u && bc_b(ins) != 3u) || bc_c(ins) < 2u ||
          a+bc_c(ins)+LJ_FR2 > pt->framesize)
        return 0;
      if (bc_b(ins) == 3u && a+1u < pt->framesize)
        needed[a+1u] = 0;
      needed[a] = 1;
      for (MSize arg = first; arg < end; arg++) needed[arg] = 1;
      break;
    }
    case BC_ADDVV: case BC_SUBVV: case BC_MULVV: case BC_DIVVV:
      if (c >= pt->framesize) return 0;
      needed[c] = 1;
      /* fallthrough */
    case BC_ADDVN: case BC_SUBVN: case BC_MULVN: case BC_DIVVN:
    case BC_ADDNV: case BC_SUBNV: case BC_MULNV: case BC_DIVNV:
      if (b >= pt->framesize) return 0;
      needed[b] = 1;
      break;
    default:
      return 0;
    }
  }
  return 1;
}

/* Work backwards from RET1 to the resume PC. Every source read matters even
** when its result is dead: arithmetic may fail, and CSET reads the old tag
** before choosing a raw destination or a closed cell. */
static int arm64_lua_leaf_ir_needed(const GCproto *pt, MSize resume,
                                    uint8_t *needed)
{
  MSize pc = pt->sizebc;
  LJArm64LuaLeafBranch branch;
  if (!resume || resume >= pc) return 0;
  if (!arm64_lua_leaf_branch(pt, &branch)) return 0;
  if (branch.compare != UINT32_MAX) {
    uint8_t *path;
    if (resume < branch.compare) return 0;
    if (branch.early_return) {
      MSize terminal;
      if (resume == branch.first || branch.first_path[resume]) {
        path = branch.first_path; terminal = branch.first_ret;
      } else if (resume == branch.second || branch.second_path[resume]) {
        path = branch.second_path; terminal = branch.second_ret;
      } else return 0;
      memset(needed, 0, pt->framesize);
      for (MSize p = terminal+1u; p-- > resume; )
        if ((p == resume || path[p]) &&
            !arm64_lua_leaf_ir_needed_range(pt, p, p+1u, needed)) return 0;
      return 1;
    }
    if (resume >= branch.join)
      return arm64_lua_leaf_ir_needed_range(pt, resume, pc, needed);
    if (resume == branch.first || branch.first_path[resume])
      path = branch.first_path;
    else if (resume == branch.second || branch.second_path[resume])
      path = branch.second_path;
    else return 0;
    memset(needed, 0, pt->framesize);
    if (!arm64_lua_leaf_ir_needed_range(pt, branch.join, pc, needed)) return 0;
    for (MSize p = branch.join; p-- > resume; ) {
      if ((p == resume || path[p]) &&
          !arm64_lua_leaf_ir_needed_range(pt, p, p+1u, needed)) return 0;
    }
    return 1;
  }
  return arm64_lua_leaf_ir_needed_range(pt, resume, pc, needed);
}

/* The incoming caller map owns its CALL. Reconstruct the child source prefix
** and match every mapped child slot, independently of its allocation. Only
** values with no remaining read before a definition may be omitted. */
static int arm64_iterl_ir_result(const jit_State *J, const GCtrace *T,
                                 SnapNo child, IRRef result)
{
  LJArm64IteratorSource iter;
  SnapNo s;
  IRRef key = 0, control = 0, r;
  MSize i;
  uint64_t footer;
  const SnapShot *loopmap = NULL;
  if (!lj_asm_arm64_iterl_tuple(J->pt, trace_startpc_acq((GCtrace *)T),
                               T->startins, T->startins, 1, &iter) ||
      !arm64_iterl_scalar_body(J->pt, &iter)) return 0;
  for (s = 0; s < T->nsnap; s++)
    if (T->snap[s].ref == J->loopref) {
      if (loopmap) return 0;
      loopmap = &T->snap[s];
    }
  if (!loopmap || loopmap->mapofs > T->nsnapmap ||
      loopmap->nent > T->nsnapmap-loopmap->mapofs ||
      T->nsnapmap-loopmap->mapofs-loopmap->nent < 2u) return 0;
  memcpy(&footer, &T->snapmap[loopmap->mapofs+loopmap->nent], sizeof(footer));
  if ((uint8_t)footer || (const BCIns *)(uintptr_t)(footer>>8) !=
      proto_bc(J->pt)+iter.body_pc) return 0;
  for (i = 0; i < loopmap->nent; i++) {
    SnapEntry e = T->snapmap[loopmap->mapofs+i];
    MSize slot = snap_slot(e);
    if (slot == 1u+LJ_FR2+iter.base || slot == LJ_FR2+iter.base) {
      if (e & 0x00ff0000u) return 0;
      if (slot == 1u+LJ_FR2+iter.base) key = snap_ref(e);
      else control = snap_ref(e);
    }
  }
  if (!key || key != control ||
      (!arm64_scalar_ir_ref(T, key, J->loopref, IRT_INT) &&
       !arm64_scalar_ir_ref(T, key, J->loopref, IRT_NUM))) return 0;
  if (T->snap[child].ref < J->loopref || key == result) return key == result;
  for (r = J->loopref+1u; r < T->nins; r++)
    if (T->ir[r].o == IR_PHI && T->ir[r].op1 == key &&
        T->ir[r].op2 == result) return 1;
  return 0;
}

/* Prove both hot returns even when a floating-point leaf has no child exit. */
static int arm64_iterl_ir_returns(const jit_State *J, const GCtrace *T)
{
  IRRef guard = 0, r, args[UINT8_MAX+1u], saved[UINT8_MAX+1u], result[2];
  SnapNo call = 0, loop = 0, s;
  const GCfunc *fn;
  MSize live, nresult, retpos, phase;
  for (r = REF_FIRST; r < J->loopref; r++)
    if (T->ir[r].o == IR_EQ && T->ir[r].t.irt == (IRT_FUNC|IRT_GUARD)) {
      if (guard) return 0;
      guard = r;
    }
  if (!guard) return 0;
  fn = arm64_lua_leaf_ir_function(T, T->ir[guard].op2, guard, NULL);
  if (!fn) return 0;
  for (s = 0; s < T->nsnap; s++) {
    if (T->snap[s].ref == guard) call = s;
    if (T->snap[s].ref == J->loopref) loop = s;
  }
  if (!call || !loop) return 0;
  for (phase = 0; phase < 2u; phase++) {
    s = phase ? loop : call;
    if (!arm64_lua_leaf_ir_args(J, T, s, T->ir[guard].op2, args) ||
        !arm64_lua_leaf_ir_prefix(T, funcproto(fn), 1u, args,
          phase ? J->loopref+1u : guard, phase ? T->nins : J->loopref, 1,
          saved, &live, result, &nresult, &retpos) || nresult != 1u ||
        !arm64_iterl_ir_result(J, T, s, result[0])) return 0;
  }
  return 1;
}

static MSize arm64_lua_leaf_ir_child(const jit_State *J, const GCtrace *T,
                                    SnapNo s, uint64_t pcbase,
                                    MSize *lowerlinkp)
{
  const SnapShot *snap = &T->snap[s];
  MSize i, linkslot = 0, base, retpos, leafretpos, pos, extent, live = 0;
  MSize callnargs = 0;
  IRRef link = 0, varglink = 0, callee = 0, result[2] = {0, 0};
  MSize nresults = 0;
  IRRef saved[UINT8_MAX+1u], args[UINT8_MAX+1u] = {0};
  uint8_t needed[UINT8_MAX+1u] = {0}, seen[UINT8_MAX+1u] = {0};
  SnapNo caller = s;
  const GCfunc *fn;
  const GCproto *pt;
  const BCIns *retpc;
  BCIns call;
  LJArm64LuaFramePath frames;
  LJArm64LuaLeafBranch branch;
  IRRef ref = snap->ref;
  int iterator = bc_op(T->startins) == BC_ITERL;
  LJArm64IteratorSource iter;
  int has_call = 0, inline_frame = (uint8_t)pcbase == 0, paired_varg = 0;
  if (lowerlinkp) *lowerlinkp = 0;
  if ((!iterator && bc_op(T->startins) != BC_FORL &&
       !(bc_op(T->startins) == BC_FUNCF && J && J->pt &&
         arm64_ir_funcf_leaf_call_source(J->pt))) ||
      (!inline_frame && (!(uint8_t)pcbase ||
       !arm64_lua_frame_path_limit(&T->snapmap[snap->mapofs], snap->nent,
                             pcbase, 2u, &frames) || !frames.depth)))
    return 0;
  if (inline_frame) {
    for (i = 0; i < snap->nent; i++) {
      SnapEntry entry = T->snapmap[snap->mapofs+i];
      if (entry & SNAP_FRAME) {
        if (linkslot || (entry & 0x00ff0000u) != SNAP_FRAME)
          return 0;
        linkslot = snap_slot(entry);
        link = snap_ref(entry);
      }
    }
    if (!linkslot || linkslot < 1u+LJ_FR2+1u) return 0;
    base = linkslot+1u;
    for (i = 0; i < snap->nent; i++) {
      SnapEntry entry = T->snapmap[snap->mapofs+i];
      if (snap_slot(entry) == base-2u) {
        if (entry & 0x00ff0000u) return 0;
        callee = snap_ref(entry);
      }
    }
  } else {
    paired_varg = frames.depth == 2u;
    base = frames.child[frames.depth-1u].baseslot;
    link = frames.child[0].linkref;
    callee = frames.child[frames.depth-1u].funcref;
    if (paired_varg) {
      varglink = frames.child[1].linkref;
      if (lowerlinkp) *lowerlinkp = frames.child[0].linkslot;
      if (frames.child[0].funcref != frames.child[1].funcref)
        return 0;
    }
  }
  if (!arm64_scalar_ir_ref(T, link, ref, IRT_NUM) || !irref_isk(link) ||
      link < T->nk || link+1u >= REF_TRUE || link+1u >= T->nins ||
      T->ir[link].o != IR_KNUM) return 0;
  retpc = (const BCIns *)(uintptr_t)T->ir[link+1u].tv.u64;
  if (!arm64_ir_pcpos((uintptr_t)retpc, (uintptr_t)proto_bc(J->pt),
      (uintptr_t)(proto_bc(J->pt)+J->pt->sizebc), &retpos) || !retpos)
    return 0;
  call = (BCIns)la_load32_acq((const uint32_t *)(retpc-1));
  if (iterator && (paired_varg || inline_frame ||
      !lj_asm_arm64_iterl_tuple(J->pt, trace_startpc_acq((GCtrace *)T),
                               T->startins, T->startins, 1, &iter) ||
      retpos != iter.loop_pc || call != iter.callins)) return 0;
  if ((!iterator && bc_op(call) != BC_CALL) || !bc_c(call) ||
      bc_a(call)+bc_c(call)+LJ_FR2 > J->pt->framesize) return 0;
  callnargs = bc_c(call)-1u;
  if (!paired_varg && base != 2u+2u*LJ_FR2+bc_a(call))
    return 0;
  if (inline_frame && !callee) {
    SnapNo si;
    for (si = 0; si < s && !callee; si++) {
      const SnapShot *prior = &T->snap[si];
      MSize j;
      for (j = 0; j < prior->nent; j++) {
        SnapEntry entry = T->snapmap[prior->mapofs+j];
        IRRef value = snap_ref(entry);
        if (snap_slot(entry) == base-2u && !(entry & 0x00ff0000u) &&
            value < T->nins && T->ir[value].o == IR_KGC &&
            irt_type(T->ir[value].t) == IRT_FUNC &&
            lj_trace_arm64_lua_leaf(gco2func(ir_kgc_acq(&T->ir[value]))))
          callee = value;
      }
    }
  }
  fn = arm64_lua_leaf_ir_function(T, callee, ref, NULL);
  if (!lj_trace_arm64_lua_leaf(fn)) return 0;
  pt = funcproto(fn);
  if (paired_varg) {
    MSize delta = bc_c(call)-1u+1u+LJ_FR2;
    if (!(la_load8_acq(&pt->flags) & PROTO_VARARG) || !varglink ||
        !irref_isk(varglink) || varglink < T->nk ||
        varglink+1u >= REF_TRUE || varglink+1u >= T->nins ||
        T->ir[varglink].o != IR_KNUM || !delta ||
        T->ir[varglink+1u].tv.u64 != 8u*delta+FRAME_VARG ||
        base != 2u+2u*LJ_FR2+bc_a(call)+delta)
      return 0;
  }
  if (!arm64_lua_leaf_branch(pt, &branch)) return 0;
  {
    BCIns ret = (BCIns)la_load32_acq(
      (const uint32_t *)&proto_bc(pt)[pt->sizebc-1u]);
    nresults = bc_op(ret) == BC_RET1 && bc_d(ret) == 2u ? 1u :
               bc_op(ret) == BC_RET && bc_d(ret) == 3u ? 2u :
               bc_op(ret) == BC_RET0 ? 1u : 0u;
  }
  if (!nresults || bc_b(call) != nresults+1u ||
      bc_a(call)+bc_b(call)-1u+LJ_FR2 > J->pt->framesize) return 0;
  if (inline_frame && bc_op(T->startins) != BC_FUNCF && nresults != 2u)
    return 0;
  /* The nearest parent map must be this CALL in the same loop iteration.
  ** Its values have undergone the actual LOOP substitution, not an inferred
  ** inverse PHI mapping. Earlier identity guards may dominate both copies. */
  while (caller) {
    const SnapShot *entry = &T->snap[--caller];
    uint64_t footer;
    memcpy(&footer, &T->snapmap[entry->mapofs+entry->nent], sizeof(footer));
    if (iterator && ref > J->loopref && entry->ref != J->loopref) continue;
    if (!(uint8_t)footer) {
      int args_ok;
      args_ok = arm64_lua_leaf_ir_args(J, T, caller, callee, args) != 0;
      if (((!iterator || ref < J->loopref) &&
          ((const BCIns *)(uintptr_t)(footer>>8) != retpc-1 ||
           (entry->ref > J->loopref) != (ref > J->loopref))) ||
          !args_ok) return 0;
      break;
    }
  }
  if (!caller) return 0;
  if (paired_varg) {
    MSize sourcebase = 2u+2u*LJ_FR2+bc_a(call);
    for (i = 0; i < callnargs; i++) {
      MSize j, found = 0, slot = sourcebase+i;
      for (j = 0; j < snap->nent; j++) {
        SnapEntry entry = T->snapmap[snap->mapofs+j];
        if (snap_slot(entry) == slot) {
          if ((entry & 0x00ff0000u) || snap_ref(entry) != args[i])
            return 0;
          found++;
        }
      }
      if (found != 1u) return 0;
    }
  }
  if (!arm64_ir_pcpos((uintptr_t)(pcbase>>8), (uintptr_t)proto_bc(pt),
      (uintptr_t)(proto_bc(pt)+pt->sizebc), &pos) || !pos ||
      !arm64_lua_leaf_branch(pt, &branch)) return 0;
  if ((la_load8_acq(&pt->flags) & PROTO_VARARG) && pos <= 1u)
    return 0;  /* Never snapshot before fixed VARG executes. */
  for (i = 1; i+1u < pt->sizebc; i++)
    if (bc_op((BCIns)la_load32_acq(
        (const uint32_t *)&proto_bc(pt)[i])) == BC_CALL) has_call = 1;
  if (has_call) {
    const GCfunc *ancestors[1];
    if (nresults != 1u) return 0;
    ancestors[0] = fn;
    if (branch.compare != UINT32_MAX ||
        !arm64_lua_leaf_ir_eval(J, T, pt, args, pos, ref,
          ref < J->loopref ? J->loopref : T->nins, ancestors, 1u,
          saved, &result[0])) return 0;
    live = pt->numparams;
    leafretpos = pt->sizebc-1u;
  } else if (!arm64_lua_leaf_ir_prefix(T, pt, pos, args, ref,
        ref < J->loopref ? J->loopref : T->nins,
        arm64_lua_leaf_ir_unused(T, s), saved, &live, result, &nresults,
        &leafretpos)) {
    return 0;
  }
  if ((pos == branch.first || pos == branch.second) &&
      !arm64_lua_leaf_ir_branch_guard(T, pt, &branch, saved, ref, pos)) {
    return 0;
  }
  if (!arm64_lua_leaf_ir_needed(pt, pos, needed)) {
    return 0;
  }
  live = pt->numparams;
  for (i = pt->numparams; i < pt->framesize; i++)
    if (needed[i] && i+1u > live) live = i+1u;
  if (base+live > UINT8_MAX) return 0;
  if (branch.compare == UINT32_MAX) {
    if (snap->nslots != base+live) return 0;
  } else if (snap->nslots < base+live ||
             /* Snapshot nslots includes the FR2 frame-link adjustment. */
             snap->nslots > snap->topslot+1u+LJ_FR2 ||
             snap->nslots > base+pt->framesize) return 0;
  if (iterator) {
    if (!(branch.early_return && !nresults &&
          (pos == branch.first || pos == branch.second)) &&
        (nresults != 1u || !arm64_iterl_ir_result(J, T, s, result[0]))) return 0;
  } else if (pos == leafretpos && bc_op(T->startins) != BC_FUNCF) {
    BCIns assign = (BCIns)la_load32_acq((const uint32_t *)retpc);
    const IRIns *guard = &T->ir[ref];
    MSize oldslot = 1u+LJ_FR2+bc_a(assign);
    MSize nextpos, result_slot, found = 0;
    const SnapShot *next;
    uint64_t footer;
    /* Bind terminal restoration to this caller's old-destination check.
    ** The destination precedes the CALL window, so returning cannot clobber
    ** the original slot before the interpreter repeats this assignment. */
    if (bc_op(assign) != BC_CSET || bc_d(assign) != bc_a(call) ||
        bc_a(assign) >= bc_a(call) || guard->o != IR_SLOAD ||
        !irt_isguard(guard->t) || guard->op1 != oldslot ||
        guard->op2 != IRSLOAD_TYPECHECK ||
        (!irt_ispri(guard->t) &&
         !arm64_cell_value_ir_ref(T, ref, ref+1u) &&
         !(arm64_cell_guard_proto(J->pt, oldslot) &&
           arm64_cell_guard_ir(T, ref)))) return 0;
    /* Bind the returned source value to the caller's native assignment too.
    ** A cell store first maps its RHS at CSET; a raw store maps the destination
    ** after CSET, possibly at the immediately following FORL exit. */
    if (s+1u >= T->nsnap) return 0;
    next = &T->snap[s+1u];
    if (next->ref <= ref || next->ref >=
          (ref < J->loopref ? J->loopref : T->nins) ||
        next->mapofs > T->nsnapmap ||
        next->nent > T->nsnapmap-next->mapofs ||
        T->nsnapmap-next->mapofs-next->nent < 2u) return 0;
    memcpy(&footer, &T->snapmap[next->mapofs+next->nent], sizeof(footer));
    if ((uint8_t)footer || !arm64_ir_pcpos((uintptr_t)(footer>>8),
        (uintptr_t)proto_bc(J->pt),
        (uintptr_t)(proto_bc(J->pt)+J->pt->sizebc), &nextpos)) return 0;
    if (irt_type(guard->t) == IRT_P32) {
      if (nextpos != retpos) return 0;
      result_slot = 1u+LJ_FR2+bc_a(call);
    } else {
      if (nextpos != retpos+1u &&
          !(retpos+1u < J->pt->sizebc && nextpos == retpos+2u &&
            retpc+1 == J->startpc)) return 0;
      result_slot = oldslot;
    }
    for (i = 0; i < next->nent; i++) {
      SnapEntry entry = T->snapmap[next->mapofs+i];
      if (snap_slot(entry) >= result_slot &&
          snap_slot(entry) < result_slot+(nresults ? nresults : 1u)) {
        BCIns ret = (BCIns)la_load32_acq(
          (const uint32_t *)&proto_bc(pt)[pos]);
        if ((entry & 0x00ff0000u) ||
        snap_ref(entry) != saved[bc_a(ret)+(snap_slot(entry)-result_slot)])
          return 0;
        found++;
      }
    }
    if (found != (nresults ? nresults : 1u)) return 0;
  } else if (!(branch.early_return &&
               (pos == branch.first || pos == branch.second)) &&
             !arm64_lua_leaf_ir_result(J, T, caller, s, retpos, leafretpos,
                                      bc_a(call), result, nresults, pt, base,
                                      callee, link)) return 0;
  extent = base-2u+pt->framesize;
  if (extent < J->pt->framesize) extent = J->pt->framesize;
  if (extent > UINT8_MAX || snap->topslot != extent) return 0;
  for (i = 0; i < snap->nent; i++) {
    SnapEntry sn = T->snapmap[snap->mapofs+i];
    MSize slot = snap_slot(sn);
    if (slot >= base) {
      if (slot-base >= pt->framesize || seen[slot-base] ||
          !saved[slot-base] || (sn & 0x00ff0000u) ||
          snap_ref(sn) != saved[slot-base]) return 0;
      seen[slot-base] = 1;
    }
  }
  for (i = 0; i < pt->framesize; i++)
    if (needed[i] && !seen[i]) return 0;
  return base-1u;
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
  unsigned nphi = 0, loop_snap = 0, nforl_side_add = 0;
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
	T->root != side->root || side->root == 0 ||
	!arm64_side_immediate_parent_valid(side, T) ||
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
       (rootop != BC_LOOP && rootop != BC_FORL && rootop != BC_ITERL && rootop != BC_FUNCF) ||
       (rootop == BC_FUNCF ?
        (T->link != 0 || T->linktype != LJ_TRLINK_RETURN || J->loopref != 0 ||
         !arm64_ir_funcf_leaf_call_source(J->pt)) :
        (T->link != T->traceno || T->linktype != LJ_TRLINK_LOOP)))) ||
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
    if (k->o == IR_KKPTR && k->t.irt == IRT_PGC && k->op12 == 0 &&
	ref + LJ_GC64 < REF_TRUE && arm64_ir_kkptr_unused(T, ref)) {
      ref += LJ_GC64; /* Dead folded-string residue, never an IR value. */
      continue;
    }
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
      if (side && (ir->op2 == (IRSLOAD_PARENT|IRSLOAD_INHERIT) ||
	  ir->op2 == (IRSLOAD_PARENT|IRSLOAD_READONLY|IRSLOAD_INHERIT))) {
	if (!arm64_side_parent_sload_valid(side, ir, parent_sload++))
	  goto bad_operand;
	break;
      }
      if (side && arm64_side_entry_sload_valid(side, ir)) {
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
        if ((ir->t.irt & ~IRT_ISPHI) != (IRT_INT|IRT_GUARD) ||
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
    case IR_STRREF:
      if (type != IRT_PGC || flags ||
          !arm64_string_byte_bounds_ir(T, ref, ir->op1, ir->op2))
        goto bad_operand;
      break;
    case IR_XLOAD:
      if (type != IRT_U8 || flags || ir->op2 != IRXLOAD_READONLY ||
          !arm64_string_byte_xload_ir(T, ref))
        goto bad_operand;
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
      if (type == IRT_INT && side &&
          arm64_side_forl_index_add_ir(side, T, ref, *ir)) {
        if (++nforl_side_add != 1u) goto bad_operand;
        binary = 1;
        break;
      }
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
    case IR_FPMATH:
      if (type != IRT_NUM || (flags & ~IRT_ISPHI) ||
          !arm64_scalar_fpmath(ir->op2) ||
          !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_NUM))
        goto bad_operand;
      break;
    case IR_FLOAD:
      if (flags ||
          !((type == IRT_NUM &&
             (arm64_scalar_ksimd_fload(*ir, 0) ||
              arm64_scalar_ksimd_fload(*ir, 1))) ||
            (type == IRT_INT && ir->op2 == IRFL_STR_LEN &&
             arm64_scalar_ir_ref(T, ir->op1, ref, IRT_STR))))
        goto bad_operand;
      break;
    case IR_ABS:
      if (type != IRT_NUM || flags ||
          !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_NUM) ||
          !arm64_scalar_ksimd_ir_ref(T, ir->op2, ref, 0))
        goto bad_operand;
      break;
    case IR_MIN: case IR_MAX:
      if ((type != IRT_INT && type != IRT_NUM) || flags)
        goto bad_type;
      binary = 1;
      break;
    case IR_BNOT: case IR_BSWAP:
      if (type != IRT_INT || (flags & ~IRT_ISPHI) || ir->op2 ||
          !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_INT))
        goto bad_operand;
      break;
    case IR_NEG:
      if (type == IRT_NUM && (flags & ~IRT_ISPHI) == 0 &&
          arm64_scalar_ir_ref(T, ir->op1, ref, IRT_NUM) &&
          arm64_scalar_ksimd_ir_ref(T, ir->op2, ref, 1))
        break;
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
    case IR_ULT:
    case IR_UGE:
    case IR_ULE:
    case IR_UGT:
    case IR_EQ:
    case IR_NE:
      if (type == IRT_FUNC && ir->o == IR_EQ && flags == IRT_GUARD) {
        GCobj *o;
        /* Lua leaves additionally need the exact caller snapshot below. */
        if (ir->op1 < REF_FIRST ||
            !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_FUNC) ||
            !arm64_scalar_ir_ref(T, ir->op2, ref, IRT_FUNC) ||
            ir->op2 >= REF_TRUE || T->ir[ir->op2].o != IR_KGC)
          goto bad_operand;
        o = ir_kgc_acq(&T->ir[ir->op2]);
        if (o == NULL || !checkptrGC(o) ||
            o->gch.gct != (uint32_t)~LJ_TFUNC ||
            (!arm64_scalar_builtin_ffid(lj_func_ffid_acq(gco2func(o))) &&
             (side || (rootop != BC_FORL && rootop != BC_ITERL && rootop != BC_FUNCF) ||
              !lj_trace_arm64_lua_leaf(gco2func(o)))))
          goto bad_operand;
        break;
      }
      if ((type != IRT_INT && type != IRT_NUM) || flags != IRT_GUARD ||
          ((ir->o >= IR_ULT && ir->o <= IR_UGT) && type != IRT_NUM &&
           !arm64_string_byte_guard_ir(T, ref)))
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
      if (type == IRT_INT && flags == 0 &&
          ir->op2 == (IRCONV_INT_NUM | IRCONV_ANY) &&
          arm64_scalar_ir_ref(T, ir->op1, ref, IRT_NUM) &&
          arm64_string_byte_dynamic_index_ir(T, ref))
        break;
      goto bad_type;
    case IR_LOOP:
      if (loop || ir->t.irt != (IRT_NIL | IRT_GUARD) || ir->op12)
        goto bad_poll;
      loop = ref;
      break;
    case IR_XPOLL:
	  if (poll || (rootop == BC_FUNCF ? loop != 0 : side ?
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
      if (side || type != IRT_NUM || (flags & ~IRT_ISPHI) ||
          !arm64_scalar_math_call(ir->op2) ||
          !arm64_scalar_ir_ref(T, ir->op1, ref, IRT_NUM))
        return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CALL, ref,
                               IR_CALLN, ir->op2);
      break;
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
  } else if (rootop == BC_FUNCF) {
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
    MSize i, pcpos, childlink = 0, lowerlink = 0, inlineleaflink = 0;
    int tail_result_map = 0;
    BCReg previous_slot = 0;
    uint64_t pcbase;
    ref = snap->ref;
    if (ref < REF_FIRST || ref >= T->nins || ref < previous_snap ||
        snap->mapofs != expected_ofs || snap->mapofs > T->nsnapmap ||
        next > T->nsnapmap || next < snap->mapofs ||
        snap->nent > next - snap->mapofs ||
        next - snap->mapofs - snap->nent != 1 + LJ_FR2 ||
        snap->nslots < 1 + LJ_FR2)
      goto bad_snapshot;
    memcpy(&pcbase, &T->snapmap[snap->mapofs+snap->nent], sizeof(pcbase));
    if ((uint8_t)pcbase) {
      if (side || !(childlink = arm64_lua_leaf_ir_child(J, T, s, pcbase,
                                                         &lowerlink))) {
        goto bad_snapshot;
      }
    } else {
      int has_frame = 0;
      for (i = 0; i < snap->nent; i++)
        has_frame |= !!(T->snapmap[snap->mapofs+i] & SNAP_FRAME);
      if (has_frame) {
        tail_result_map = !side &&
          arm64_ir_funcf_tail_result_map(J, T, s, pcbase);
        if (!tail_result_map && (side || !(inlineleaflink =
              arm64_lua_leaf_ir_child(J, T, s, pcbase, &lowerlink)) ||
            snap->nslots > maxslots || snap->topslot >= maxslots))
          goto bad_snapshot;
      } else if (snap->nslots > maxslots ||
                 snap->topslot != J->pt->framesize) {
        goto bad_snapshot;
      }
    }
    previous_snap = ref;
    expected_ofs = next;
    loop_snap |= ref == loop;
    if (ref <= poll)
      poll_snap = ref;
    for (i = 0; i < snap->nent; i++) {
      SnapEntry sn = T->snapmap[snap->mapofs + i];
      BCReg slot = snap_slot(sn);
      IRRef value = snap_ref(sn);
#if defined(LJ_TRACE_TEST_HELPERS)
#endif
      if (slot >= snap->nslots || (i && slot <= previous_slot)) {
        goto bad_snapshot;
      }
      previous_slot = slot;
      if (tail_result_map &&
	  sn == SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL)) continue;
      if (tail_result_map && slot == 0 && !(sn & 0x00ff0000u) &&
	  irref_isk(value) && value >= T->nk && value < REF_TRUE &&
	  T->ir[value].o == IR_KGC &&
	  isluafunc(gco2func(ir_kgc_acq(&T->ir[value])))) continue;
      if (childlink && slot == childlink &&
          (sn & 0x00ff0000u) == SNAP_FRAME) continue;
      if (inlineleaflink && slot == inlineleaflink &&
          (sn & 0x00ff0000u) == SNAP_FRAME) continue;
      if (lowerlink && slot == lowerlink &&
          (sn & 0x00ff0000u) == SNAP_FRAME) continue;
      if (value >= REF_FIRST && value < ref &&
          T->ir[value].o == IR_XLOAD && T->ir[value].t.irt == IRT_U8)
	goto bad_snapshot;
      if ((sn & 0x00ff0000u) == SNAP_NORESTORE) {
        MSize idxslot = 1u + LJ_FR2 + bc_a(T->startins);
        const IRIns *source;
        if (side) {
          if (!arm64_side_forl_snapshot_stop_ir(side, T, ref, sn))
	    goto bad_snapshot;
        } else {
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
    if (!tail_result_map && !childlink && !inlineleaflink &&
        ((uint8_t)pcbase ||
        !arm64_ir_pcpos((uintptr_t)(pcbase >> 8), proto_lo, proto_hi, &pcpos)))
      goto bad_snapshot;
    if (side && ((s == 0 && pcpos != proto_bcpos(J->pt,
	       side->parent_continuation)) ||
	       (pcpos < side->loop_start_pc ||
	        (pcpos >= side->loop_end_pc &&
	         !(s != 0 && (pcpos == side->loop_end_pc+1u ||
                   (pcpos == side->loop_end_pc &&
                    arm64_side_forl_boundary_snapshot_ir(side, T, snap)))))))) {
	  goto bad_snapshot;
	}
  }
  if (side && side->linktype == LJ_TRLINK_ROOT) {
	if (loop_snap || poll_snap != poll)
	  goto bad_snapshot;
  } else if (rootop == BC_FUNCF ? (loop_snap || poll_snap != poll) :
             (!loop_snap || poll_snap != loop)) {
	goto bad_snapshot;
  }
  s = 0;
  for (ref = REF_FIRST; ref < T->nins; ref++) {
    const IRIns *ir = &T->ir[ref];
    if (irt_isguard(ir->t) && T->snap[0].ref > ref)
      goto bad_snapshot;
    while (s + 1 < T->nsnap && T->snap[s + 1].ref <= ref)
      s++;
    if (ir->o == IR_EQ && ir->t.irt == (IRT_FUNC|IRT_GUARD) &&
        isluafunc(gco2func(ir_kgc_acq(&T->ir[ir->op2]))) &&
        !arm64_lua_leaf_ir_snapshot(J, T, ref, s) &&
        !arm64_ir_funcf_tail_guard_snapshot(J, T, ref, s)) {
      goto bad_snapshot;
    }
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
  if (rootop == BC_ITERL && !arm64_iterl_ir_returns(J, T))
    goto bad_snapshot;
  if (rootop == BC_FUNCF &&
      !arm64_ir_funcf_leaf_source_replay(J, T, T->nins))
    goto bad_snapshot;
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
      return k.o != IR_KKPTR && irt_type(k.t) == type;
    if (k.o == IR_KNUM || k.o == IR_KGC || k.o == IR_KKPTR)
      ref++;
  }
  return 0;
}
static int arm64_scalar_ksimd_postra_fload(IRIns p, unsigned index)
{
  return index < LJ_KSIMD__MAX && p.o == IR_FLOAD &&
         p.t.irt == IRT_NUM && p.op1 == REF_NIL &&
         p.op2 == (offsetof(GG_State, g.ksimd) + index*16u)/4u;
}

static int arm64_scalar_ksimd_postra_ref(const LJArm64PostRAView *v,
                                         IRRef value, IRRef before,
                                         unsigned index)
{
  return value >= REF_FIRST && value < before &&
         arm64_scalar_ksimd_postra_fload(ir_load_acq(&v->ir[value]), index);
}
static int arm64_scalar_postra_value(IRIns p)
{
  switch ((IROp)p.o) {
  case IR_FLOAD:
    return (p.t.irt == IRT_INT && p.op2 == IRFL_STR_LEN) ||
           arm64_scalar_ksimd_postra_fload(p, 0) ||
           arm64_scalar_ksimd_postra_fload(p, 1);
  case IR_STRREF:
    return p.t.irt == IRT_PGC;
  case IR_XLOAD:
    return p.t.irt == IRT_U8 && p.op2 == IRXLOAD_READONLY;
  case IR_BNOT: case IR_BSWAP:
  case IR_BAND: case IR_BOR: case IR_BXOR:
  case IR_BSHL: case IR_BSHR: case IR_BSAR: case IR_BROR:
  case IR_TOBIT:
    return (p.t.irt & ~IRT_ISPHI) == IRT_INT;
  case IR_NEG:
    return (p.t.irt & ~IRT_ISPHI) == IRT_INT ||
           (p.t.irt & ~IRT_ISPHI) == IRT_NUM;
  case IR_ABS:
    return p.t.irt == IRT_NUM;
  case IR_MIN: case IR_MAX:
    return p.t.irt == IRT_INT || p.t.irt == IRT_NUM;
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
  case IR_FPMATH:
    return (p.t.irt & ~IRT_ISPHI) == IRT_NUM && arm64_scalar_fpmath(p.op2);
  case IR_CALLN:
    return (p.t.irt & ~IRT_ISPHI) == IRT_NUM && arm64_scalar_math_call(p.op2);
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
static int arm64_string_byte_bounds_postra(const LJArm64PostRAView *v,
                                            IRRef strref, IRRef str,
                                            IRRef offset);
static int arm64_string_byte_dynamic_bounds_postra(
    const LJArm64PostRAView *v, IRRef strref, IRRef str, IRRef offset);
static int arm64_string_byte_dynamic_index_postra(
    const LJArm64PostRAView *v, IRRef conv);
static int arm64_string_byte_xload_postra(const LJArm64PostRAView *v,
                                           IRRef xload)
{
  IRIns load, ptr;
  if (xload < REF_FIRST || xload >= v->nins) return 0;
  load = ir_load_acq(&v->ir[xload]);
  if (load.o != IR_XLOAD || load.t.irt != IRT_U8 ||
      load.op2 != IRXLOAD_READONLY || load.op1 < REF_FIRST ||
      load.op1 >= xload) return 0;
  ptr = ir_load_acq(&v->ir[load.op1]);
  return ptr.o == IR_STRREF && ptr.t.irt == IRT_PGC &&
         arm64_string_byte_bounds_postra(v, load.op1, ptr.op1, ptr.op2);
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
  /* Independently retain LDRB's zero-extended integer operand semantics. */
  if (type == IRT_INT && irt_type(p.t) == IRT_U8 && p.o == IR_XLOAD)
    return arm64_scalar_postra_ref(v, ref, before, IRT_U8);
  if (type == IRT_U8 && p.o == IR_XLOAD)
    return p.t.irt == IRT_U8 && p.op2 == IRXLOAD_READONLY &&
           arm64_string_byte_xload_postra(v, ref);
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

/* Independently prove the allocated constant-stop comparison exit. */
static int arm64_side_forl_finish_snapshot_postra(
    const LJArm64SideIRView *side, const LJArm64PostRAView *v, IRRef compare)
{
  MSize selected = (MSize)-1, ofs, nent;
  MSize idxslot = 1u+LJ_FR2+bc_a(side->parent_startins);
  uint64_t pcbase;
  for (MSize i = 0; i < v->nsnap; i++) {
    if (snap_ref_acq(&v->snap[i]) > compare) break;
    selected = i;
  }
  if (selected == (MSize)-1) return 0;
  ofs = snap_mapofs_acq(&v->snap[selected]);
  nent = snap_nent_acq(&v->snap[selected]);
  if (ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
      v->nsnapmap-ofs-nent < 1u+LJ_FR2) return 0;
  for (MSize i = 0; i < nent; i++)
    if (snap_slot(snapentry_acq(&v->snapmap[ofs+i])) >= idxslot) return 0;
  memcpy(&pcbase, &v->snapmap[ofs+nent], sizeof(pcbase));
  return (uint8_t)pcbase == 0 && side->loop_end_pc+1u < side->proto_sizebc &&
    (pcbase >> 8) == (uintptr_t)(side->proto_bc+side->loop_end_pc+1u);
}

/* Allocated-pass copy of the inherited root range certificate check. */
static int arm64_side_forl_root_range_proof_postra(
	const LJArm64SideIRView *side, int32_t step)
{
	MSize idxslot, stopslot;
	int64_t bound;
	IRIns idx, stop, guard, constant;
	if (side == NULL || side->parent == side->root ||
	    side->parent_root_forl_range_valid != 1u ||
	    (side->parent_root_forl_mask & 3u) != 3u ||
	    side->parent_root_forl_nins <= REF_FIRST ||
	    side->parent_root_forl_nins >= REF_DROP ||
	    side->parent_root_forl_stop_ref < REF_FIRST ||
	    side->parent_root_forl_stop_ref >=
	      side->parent_root_forl_bound_guard_ref ||
	    side->parent_root_forl_bound_guard_ref >=
	      side->parent_root_forl_snapref ||
	    side->parent_root_forl_snapref >= side->parent_root_forl_nins ||
	    side->parent_root_forl_bound_constant_ref >= REF_TRUE)
	  return 0;
	idxslot = 1u+LJ_FR2+(MSize)bc_a(side->parent_startins);
	stopslot = idxslot+FORL_STOP;
	idx = side->parent_root_forl[0];
	stop = side->parent_root_forl[1];
	if (idx.o != IR_SLOAD || idx.op1 != idxslot ||
	    idx.t.irt != (IRT_INT|IRT_GUARD) ||
	    idx.op2 != (IRSLOAD_TYPECHECK|IRSLOAD_INHERIT) ||
	    stop.o != IR_SLOAD || stop.op1 != stopslot ||
	    stop.t.irt != IRT_INT ||
	    stop.op2 != (IRSLOAD_READONLY|IRSLOAD_INHERIT) ||
	    side->parent_root_forl_stop_ref >= side->parent_root_forl_nins)
	  return 0;
	bound = step > 0 ? (int64_t)INT32_MAX-(int64_t)step :
	                   (int64_t)INT32_MIN-(int64_t)step;
	if (step == 0 || bound < INT32_MIN || bound > INT32_MAX)
	  return 0;
	guard = side->parent_root_forl_bound_guard;
	constant = side->parent_root_forl_bound_constant;
	if (guard.o != (step > 0 ? IR_LE : IR_GE) ||
	    guard.t.irt != (IRT_INT|IRT_GUARD) ||
	    guard.op1 != side->parent_root_forl_stop_ref ||
	    guard.op2 != side->parent_root_forl_bound_constant_ref ||
	    constant.o != IR_KINT || constant.t.irt != IRT_INT ||
	    constant.i != (int32_t)bound)
	  return 0;
	return 1;
}

static int arm64_side_forl_guard_snapshot_postra(
	const LJArm64SideIRView *side, const LJArm64PostRAView *v, IRRef guard,
	MSize idxslot, IRRef oldidx)
{
	uint64_t pcbase;
	MSize s, selected = (MSize)-1, ofs, nent, i, matches = 0;
	if (!side || !v || !v->snap || !v->snapmap ||
	    !side->parent_continuation || idxslot >= LJ_MAX_JSLOTS ||
	    oldidx < REF_FIRST || oldidx >= guard)
	  return 0;
	for (s = 0; s < v->nsnap; s++) {
	  if (v->snap[s].ref > guard) break;
	  selected = s;
	}
	if (selected == (MSize)-1) return 0;
	ofs = snap_mapofs_acq(&v->snap[selected]);
	nent = snap_nent_acq(&v->snap[selected]);
	if (ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
	    v->nsnapmap-ofs-nent < 1u+LJ_FR2)
	  return 0;
	for (i = 0; i < nent; i++) {
	  SnapEntry sn = snapentry_acq(&v->snapmap[ofs+i]);
	  if (snap_slot(sn) == idxslot) {
	    if (snap_ref(sn) != oldidx) return 0;
	    matches++;
	  }
	}
	if (matches != 1) return 0;
	memcpy(&pcbase, &v->snapmap[ofs+nent], sizeof(pcbase));
	return (uint8_t)pcbase == 0 &&
	  arm64_side_forl_retry_pc_valid(side, (uintptr_t)(pcbase>>8));
}

static int arm64_side_forl_termination_snapshot_postra(
	const LJArm64SideIRView *side, const LJArm64PostRAView *v, IRRef guard,
	MSize idxslot)
{
	MSize s, selected = (MSize)-1, ofs, nent, i;
	uint64_t pcbase;
	uintptr_t target;
	if (side == NULL || v == NULL || v->snap == NULL || v->snapmap == NULL ||
	    side->loop_end_pc+1u >= side->proto_sizebc ||
	    idxslot >= LJ_MAX_JSLOTS)
	  return 0;
	for (s = 0; s < v->nsnap; s++) {
	  if (v->snap[s].ref > guard) break;
	  selected = s;
	}
	if (selected == (MSize)-1 || v->snap[selected].ref != guard)
	  return 0;
	ofs = snap_mapofs_acq(&v->snap[selected]);
	nent = snap_nent_acq(&v->snap[selected]);
	if (ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
	    v->nsnapmap-ofs-nent < 1u+LJ_FR2)
	  return 0;
	for (i = 0; i < nent; i++) {
	  MSize slot = snap_slot(snapentry_acq(&v->snapmap[ofs+i]));
	  if (slot == idxslot || slot == idxslot+FORL_STOP ||
	      slot == idxslot+FORL_STEP)
	    return 0;
	}
	memcpy(&pcbase, &v->snapmap[ofs+nent], sizeof(pcbase));
	if ((uint8_t)pcbase != 0) return 0;
	target = (uintptr_t)(pcbase>>8);
	return target == (uintptr_t)(const void *)
	  (side->proto_bc+side->loop_end_pc+1u);
}

/* Allocated replay of the special FORL index update. This separately checks
** the copied loop admission, source step, parent map and compare shape; the
** generic post-RA walk then proves both operands have valid locations. */
static int arm64_side_forl_index_add_postra(const LJArm64SideIRView *side,
	const LJArm64PostRAView *v, IRRef ref, IRIns add, IRRef n)
{
  MSize idxslot, stopslot, r;
  int32_t source_step, step;
  unsigned comparisons = 0, rangeguards = 0, constantstops = 0, inherited = 0;
  IRRef rangeguard = 0, inherited_guard = 0;
  int64_t bound64;
  if (!side || !v || add.o != IR_ADD || add.t.irt != IRT_INT ||
	  !arm64_side_forl_step_source(side, &source_step) ||
	  !arm64_scalar_postra_kint(v, add.op2, &step) ||
	  step != source_step ||
	  !arm64_side_forl_step_map_valid(side, add.op2, step))
    return 0;
  idxslot = 1u+LJ_FR2+(MSize)bc_a(side->parent_startins);
  stopslot = idxslot+FORL_STOP;
  bound64 = step > 0 ? (int64_t)INT32_MAX-step :
                       (int64_t)INT32_MIN-step;
  if (bound64 < INT32_MIN || bound64 > INT32_MAX) return 0;
  if (add.op1 < REF_FIRST || add.op1 >= ref) return 0;
  {
	IRIns idx = ir_load_acq(&v->ir[add.op1]);
	if (idx.o != IR_SLOAD || idx.op1 != idxslot ||
	    (idx.t.irt & ~IRT_ISPHI) != IRT_INT ||
	    idx.op2 != (IRSLOAD_PARENT|IRSLOAD_INHERIT))
	  return 0;
  }
  for (r = ref+1u; r < n; r++) {
	IRIns cmp = ir_load_acq(&v->ir[r]), stop;
	if (cmp.o != (step > 0 ? IR_LE : IR_GE) ||
	    cmp.t.irt != (IRT_INT|IRT_GUARD) || cmp.op1 != ref ||
	    cmp.op2 >= r)
	  continue;
    {
      int32_t stopvalue;
      if (arm64_scalar_postra_kint(v, cmp.op2, &stopvalue)) {
        if (!arm64_side_forl_constant_stop_valid(side, cmp.op2, stopvalue, step) ||
            !arm64_side_forl_finish_snapshot_postra(side, v, (IRRef)r) ||
            ++comparisons != 1 || ++constantstops != 1) return 0;
        continue;
      }
    }
    if (cmp.op2 < REF_FIRST) return 0;
	stop = ir_load_acq(&v->ir[cmp.op2]);
	if (stop.o != IR_SLOAD || stop.op1 != stopslot ||
	    (stop.t.irt & ~IRT_ISPHI) != IRT_INT ||
	    stop.op2 != (IRSLOAD_PARENT|IRSLOAD_READONLY|IRSLOAD_INHERIT))
	  continue;
	{
	  IRRef q;
	  int matched = 0;
	  for (q = ref+1u; q < r; q++) {
	    IRIns guard = ir_load_acq(&v->ir[q]);
	    int32_t bound;
	    if (guard.o != (step > 0 ? IR_LE : IR_GE) ||
		guard.t.irt != (IRT_INT|IRT_GUARD) ||
		guard.op1 != cmp.op2 ||
		!arm64_scalar_postra_kint(v, guard.op2, &bound) ||
		bound != (int32_t)bound64)
	      continue;
	    if (++rangeguards != 1) return 0;
	    rangeguard = q;
	    matched = 1;
	  }
	  if (matched) {
	    if (rangeguard >= r || ++comparisons != 1) return 0;
	  } else if (side->parent != side->root &&
		     arm64_side_forl_root_range_proof_postra(side, step)) {
	    if (++inherited != 1) return 0;
	    inherited_guard = r;
	  } else {
	    return 0;
	  }
	}
  }
  if (comparisons == 0 && rangeguards == 0 && constantstops == 0 &&
      inherited == 1) {
    if (!arm64_side_forl_root_range_proof_postra(side, step)) return 0;
    return arm64_side_forl_guard_snapshot_postra(side, v, inherited_guard,
                                               idxslot, add.op1) ||
      arm64_side_forl_termination_snapshot_postra(side, v, inherited_guard,
                                                 idxslot);
  }
  if (comparisons != 1 || inherited != 0) return 0;
  if (constantstops == 1) return rangeguards == 0;
  if (rangeguards != 1) return 0;
  /* Independently bind the allocated guard's cold edge to the selected
  ** continuation and pre-update index in the concrete trace snapshot. */
  {
	uint64_t pcbase;
	MSize s, selected = (MSize)-1, ofs, nent, i, matches = 0;
	if (!v->snap || !v->snapmap || !side->parent_continuation ||
	    idxslot >= LJ_MAX_JSLOTS || add.op1 < REF_FIRST || add.op1 >= ref)
	  return 0;
	for (s = 0; s < v->nsnap; s++) {
	  if (v->snap[s].ref > rangeguard) break;
	  selected = s;
	}
	if (selected == (MSize)-1) return 0;
	ofs = snap_mapofs_acq(&v->snap[selected]);
	nent = snap_nent_acq(&v->snap[selected]);
	if (ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
	    v->nsnapmap-ofs-nent < 1u+LJ_FR2)
	  return 0;
	for (i = 0; i < nent; i++) {
	  SnapEntry sn = snapentry_acq(&v->snapmap[ofs+i]);
	  if (snap_slot(sn) == idxslot) {
	    if (snap_ref(sn) != add.op1) return 0;
	    matches++;
	  }
	}
	if (matches != 1) return 0;
	memcpy(&pcbase, &v->snapmap[ofs+nent], sizeof(pcbase));
	return (uint8_t)pcbase == 0 &&
	  arm64_side_forl_retry_pc_valid(side, (uintptr_t)(pcbase >> 8));
  }
}

/* A cold edge at FORL retries its increment. Bind all four control slots
** to the inherited pre-update state, including the visible index, so the VM
** cannot skip or repeat an iteration when it leaves a root-linked child. */
static int arm64_side_forl_boundary_snapshot_postra(
    const LJArm64SideIRView *side, const LJArm64PostRAView *v, const SnapShot *snap)
{
  MSize i, idxslot, mask = 0;
  IRRef oldidx = 0, ext = 0;
  int32_t step;
  if (!side || !v || !snap || side->linktype != LJ_TRLINK_ROOT ||
      !arm64_side_forl_step_source(side, &step) ||
      snap->mapofs > v->nsnapmap || snap->nent > v->nsnapmap-snap->mapofs)
    return 0;
  idxslot = 1u+LJ_FR2+bc_a(side->parent_startins);
  for (i = 0; i < snap->nent; i++) {
    SnapEntry entry = snapentry_acq(&v->snapmap[snap->mapofs+i]);
    MSize slot = snap_slot(entry), role;
    IRRef value = snap_ref(entry);
    IRIns load;
    if (slot < idxslot || slot > idxslot+FORL_EXT) continue;
    role = slot-idxslot;
    if (mask & (1u << role)) return 0;
    mask |= 1u << role;
    if (role == FORL_STEP) {
      int32_t found;
      if (!arm64_scalar_postra_kint(v, value, &found) || found != step) return 0;
      continue;
    }
    if (value < REF_FIRST || value >= snap->ref || value >= v->nins)
      return 0;
    load = ir_load_acq(&v->ir[value]);
    if (load.o != IR_SLOAD || load.t.irt != IRT_INT ||
        load.op1 != (role == FORL_EXT ? idxslot : slot) ||
        load.op2 != (IRSLOAD_PARENT|IRSLOAD_INHERIT|
                     (role == FORL_STOP ? IRSLOAD_READONLY : 0))) return 0;
    if (role == FORL_IDX) oldidx = value;
    if (role == FORL_EXT) ext = value;
  }
  return mask == 15u && oldidx != 0 && ext == oldidx;
}

static int arm64_side_forl_snapshot_stop_postra(
	const LJArm64SideIRView *side, const LJArm64PostRAView *v,
	IRRef before, SnapEntry sn)
{
  MSize idxslot, slot, ordinal, matches = 0;
  int32_t step;
  IRRef value = snap_ref(sn);
  IRIns load;
  if (!side || !v || (sn & 0x00ff0000u) != SNAP_NORESTORE ||
	  !arm64_side_forl_step_source(side, &step))
	return 0;
  idxslot = 1u+LJ_FR2+(MSize)bc_a(side->parent_startins);
  slot = snap_slot(sn);
  if (slot != idxslot+FORL_STOP || value < REF_FIRST || value >= before ||
	  value >= v->nins)
	return 0;
  load = ir_load_acq(&v->ir[value]);
  if (load.o != IR_SLOAD || load.op1 != slot ||
	  (load.t.irt & ~IRT_ISPHI) != IRT_INT ||
	  load.op2 != (IRSLOAD_PARENT|IRSLOAD_READONLY|IRSLOAD_INHERIT))
	return 0;
  for (ordinal = 0; ordinal < side->parent_nregspmap; ordinal++)
	if (side->parent_bindings[ordinal].slot == slot)
	  matches++;
  if (matches != 1) return 0;
  for (ordinal = 0; ordinal < side->parent_nregspmap; ordinal++)
	if (side->parent_bindings[ordinal].slot == slot)
	  return arm64_side_parent_sload_valid(side, &load, ordinal);
  return 0;
}

/* Independently certify a constant byte offset against an earlier guarded
** length load for the same string. Dynamic offsets remain fail-closed. */
static int arm64_string_byte_bounds_postra(const LJArm64PostRAView *v,
                                            IRRef strref, IRRef str,
                                            IRRef offset)
{
  int32_t byte_offset;
  IRRef ref;
  int64_t bound;
  if (!arm64_scalar_postra_kint(v, offset, &byte_offset) || byte_offset < 0 ||
      byte_offset == INT32_MAX ||
      (uint32_t)byte_offset > 4095u - (uint32_t)sizeof(GCstr) ||
      !arm64_scalar_postra_ref(v, str, strref, IRT_STR))
    return arm64_string_byte_dynamic_bounds_postra(v, strref, str, offset);
  bound = (int64_t)byte_offset + 1;
  for (ref = REF_FIRST; ref < strref; ref++) {
    IRIns guard = ir_load_acq(&v->ir[ref]);
    IRRef lenref, boundref;
    int32_t guard_bound;
    if ((guard.o != IR_UGE && guard.o != IR_ULE) ||
        guard.t.irt != (IRT_INT|IRT_GUARD))
      continue;
    lenref = guard.o == IR_UGE ? guard.op1 : guard.op2;
    boundref = guard.o == IR_UGE ? guard.op2 : guard.op1;
    if (lenref < REF_FIRST || lenref >= ref ||
        !arm64_scalar_postra_kint(v, boundref, &guard_bound) ||
        guard_bound != (int32_t)bound)
      continue;
    {
      IRIns len = ir_load_acq(&v->ir[lenref]);
      if (len.o == IR_FLOAD && len.t.irt == IRT_INT &&
          len.op2 == IRFL_STR_LEN && len.op1 == str &&
          arm64_scalar_postra_ref(v, lenref, ref, IRT_INT))
        return 1;
    }
  }
  return 0;
}

static int arm64_string_byte_dynamic_bounds_postra(
    const LJArm64PostRAView *v, IRRef strref, IRRef str, IRRef offset)
{
  LJ_STATIC_ASSERT(LJ_MAX_STR < INT32_MAX);
  GCtrace *owner;
  GCobj *proto_obj;
  IRIns sub, conv, source, string;
  IRRef idx, lenref = 0, upper = 0, subref = 0, widthref = 0;
  IRRef nonneg = 0, negative = 0, endref = 0, ref;
  int negative_path;
  int32_t k;
  if (!v || !v->proto || !v->owner) return 0;
  proto_obj = obj2gco((GCproto *)v->proto);
  if (!checkptrGC(proto_obj) ||
      proto_obj->gch.gct != (uint32_t)~LJ_TPROTO ||
      proto_bc(v->proto) != v->proto_bc ||
      v->proto->sizebc != v->proto_sizebc ||
      v->proto->framesize != v->root_topslot ||
      strref < REF_FIRST || strref >= v->nins ||
      offset < REF_FIRST || offset >= strref ||
      !arm64_scalar_postra_ref(v, str, strref, IRT_STR)) return 0;
  owner = (GCtrace *)v->owner;
  if (trace_startptgco_acq(owner) != obj2gco((GCproto *)v->proto)) return 0;
  sub = ir_load_acq(&v->ir[offset]);
  negative_path = sub.o == IR_ADD && sub.t.irt == IRT_INT;
  if (negative_path) {
    IRRef left = sub.op1, right = sub.op2;
    IRIns left_ir, right_ir;
    int left_len, right_len;
    if (left < REF_FIRST || left >= offset || right < REF_FIRST ||
        right >= offset) return 0;
    left_ir = ir_load_acq(&v->ir[left]);
    right_ir = ir_load_acq(&v->ir[right]);
    left_len = left_ir.o == IR_FLOAD && left_ir.t.irt == IRT_INT &&
      left_ir.op1 == str && left_ir.op2 == IRFL_STR_LEN;
    right_len = right_ir.o == IR_FLOAD && right_ir.t.irt == IRT_INT &&
      right_ir.op1 == str && right_ir.op2 == IRFL_STR_LEN;
    if (left_len == right_len) return 0;
    lenref = left_len ? left : right;
    idx = left_len ? right : left;
  } else {
    if (sub.o != IR_ADDOV || sub.t.irt != (IRT_INT|IRT_GUARD) ||
        !arm64_scalar_postra_kint(v, sub.op2, &k) || k != -1 ||
        sub.op1 < REF_FIRST || sub.op1 >= offset) return 0;
    idx = sub.op1;
  }
  conv = ir_load_acq(&v->ir[idx]);
  if (conv.o != IR_CONV || conv.t.irt != IRT_INT ||
      conv.op2 != (IRCONV_INT_NUM|IRCONV_ANY) ||
      conv.op1 < REF_FIRST || conv.op1 >= idx) return 0;
  source = ir_load_acq(&v->ir[conv.op1]);
  if (source.o != IR_SLOAD || source.t.irt != (IRT_NUM|IRT_GUARD) ||
      source.op2 != IRSLOAD_TYPECHECK ||
      !arm64_scalar_postra_ref(v, conv.op1, idx, IRT_NUM)) return 0;
  if (str < REF_FIRST || str >= strref) return 0;
  string = ir_load_acq(&v->ir[str]);
  if (string.o != IR_SLOAD || string.t.irt != (IRT_STR|IRT_GUARD) ||
      string.op2 != IRSLOAD_TYPECHECK ||
      !arm64_string_byte_call_slots_postra(v->proto, string.op1, source.op1))
    return 0;
  if (negative_path) {
    for (ref = REF_FIRST; ref < offset; ref++) {
      IRIns g = ir_load_acq(&v->ir[ref]);
      if (g.t.irt == (IRT_INT|IRT_GUARD) && g.o == IR_LT &&
          g.op1 == idx && arm64_scalar_postra_kint(v, g.op2, &k) && k == 0) {
        if (negative) return 0;
        negative = ref;
      }
    }
    if (!negative || negative >= offset) return 0;
  } else {
    for (ref = REF_FIRST; ref < offset; ref++) {
      IRIns g = ir_load_acq(&v->ir[ref]);
      if (g.t.irt != (IRT_INT|IRT_GUARD)) continue;
      {
        IRRef len_candidate = g.o == IR_ULE && g.op1 == idx ? g.op2 :
                              g.o == IR_UGE && g.op2 == idx ? g.op1 : 0;
        IRIns candidate;
        if (!len_candidate || len_candidate < REF_FIRST ||
            len_candidate >= ref)
          continue;
        candidate = ir_load_acq(&v->ir[len_candidate]);
        if (candidate.o == IR_FLOAD && candidate.t.irt == IRT_INT &&
            candidate.op1 == str && candidate.op2 == IRFL_STR_LEN) {
          if (upper) return 0;
          upper = ref; lenref = len_candidate;
        }
      }
    }
    if (!upper || !lenref || lenref >= upper || upper >= offset) return 0;
  }
  for (ref = offset+1u; ref < strref; ref++) {
    IRIns g = ir_load_acq(&v->ir[ref]);
    if (g.t.irt != (IRT_INT|IRT_GUARD)) continue;
    if (g.o == IR_GE && g.op1 == offset &&
        arm64_scalar_postra_kint(v, g.op2, &k) && k == 0) {
      if (nonneg) return 0;
      nonneg = ref;
    } else if (g.o == IR_SUBOV &&
               (negative_path ? g.op2 == offset :
                (g.op1 == idx && g.op2 == offset))) {
      if (negative_path) {
        IRIns end;
        if (g.op1 <= offset || g.op1 >= ref) continue;
        end = ir_load_acq(&v->ir[g.op1]);
        if (end.o != IR_ADD || end.t.irt != IRT_INT ||
            end.op1 != offset ||
            !arm64_scalar_postra_kint(v, end.op2, &k) || k != 1)
          continue;
        endref = g.op1;
      }
      if (subref) return 0;
      subref = ref;
    } else if (g.o == IR_EQ && g.op1 == subref &&
               arm64_scalar_postra_kint(v, g.op2, &k) && k == 1) {
      if (widthref) return 0;
      widthref = ref;
    }
  }
  return nonneg > offset && subref > nonneg && widthref > subref &&
         (!negative_path || (endref && endref < subref)) &&
         widthref < strref;
}

static int arm64_string_byte_dynamic_index_postra(
    const LJArm64PostRAView *v, IRRef conv)
{
  IRRef ref;
  IRIns conversion;
  if (!v || conv < REF_FIRST || conv >= v->nins) return 0;
  conversion = ir_load_acq(&v->ir[conv]);
  if (conversion.o != IR_CONV || conversion.t.irt != IRT_INT ||
      conversion.op2 != (IRCONV_INT_NUM|IRCONV_ANY)) return 0;
  for (ref = conv+1u; ref < v->nins; ref++) {
    IRIns strref = ir_load_acq(&v->ir[ref]);
    IRIns offset;
    if (strref.o != IR_STRREF || strref.t.irt != IRT_PGC ||
        strref.op2 < REF_FIRST || strref.op2 >= ref) continue;
    offset = ir_load_acq(&v->ir[strref.op2]);
    if ((offset.o != IR_ADDOV || offset.op1 != conv) &&
        (offset.o != IR_ADD ||
         (offset.op1 != conv && offset.op2 != conv))) continue;
    if (arm64_string_byte_dynamic_bounds_postra(v, ref, strref.op1,
                                                strref.op2)) return 1;
  }
  return 0;
}

static int arm64_string_byte_dynamic_guard_postra(
    const LJArm64PostRAView *v, IRRef guardref)
{
  IRIns guard;
  IRRef ref;
  if (!v || guardref < REF_FIRST || guardref >= v->nins) return 0;
  guard = ir_load_acq(&v->ir[guardref]);
  if ((guard.o != IR_ULE && guard.o != IR_UGE) ||
      guard.t.irt != (IRT_INT|IRT_GUARD)) return 0;
  for (ref = guardref+1u; ref < v->nins; ref++) {
    IRIns strref = ir_load_acq(&v->ir[ref]), offset, len;
    IRRef idx, lenref;
    if (strref.o != IR_STRREF || strref.t.irt != IRT_PGC ||
        strref.op2 < REF_FIRST || strref.op2 >= ref) continue;
    offset = ir_load_acq(&v->ir[strref.op2]);
    if (offset.o != IR_ADDOV || guardref >= strref.op2 ||
        offset.op1 < REF_FIRST ||
        offset.op1 >= strref.op2) continue;
    idx = offset.op1;
    lenref = guard.o == IR_ULE && guard.op1 == idx ? guard.op2 :
             guard.o == IR_UGE && guard.op2 == idx ? guard.op1 : 0;
    if (!lenref || lenref < REF_FIRST || lenref >= guardref) continue;
    len = ir_load_acq(&v->ir[lenref]);
    if (len.o == IR_FLOAD && len.t.irt == IRT_INT &&
        len.op1 == strref.op1 && len.op2 == IRFL_STR_LEN &&
        arm64_string_byte_dynamic_bounds_postra(v, ref, strref.op1,
                                                strref.op2)) return 1;
  }
  return 0;
}

static int arm64_string_byte_guard_postra(const LJArm64PostRAView *v,
                                          IRRef guardref)
{
  IRIns guard;
  IRRef lenref, boundref, ref;
  int32_t bound;
  if (guardref < REF_FIRST || guardref >= v->nins) return 0;
  guard = ir_load_acq(&v->ir[guardref]);
  if ((guard.o != IR_UGE && guard.o != IR_ULE) ||
      guard.t.irt != (IRT_INT|IRT_GUARD)) return 0;
  lenref = guard.o == IR_UGE ? guard.op1 : guard.op2;
  boundref = guard.o == IR_UGE ? guard.op2 : guard.op1;
  if (lenref < REF_FIRST || lenref >= guardref ||
      (!arm64_scalar_postra_kint(v, boundref, &bound) || bound <= 0 ||
       (uint32_t)bound > 4096u-(uint32_t)sizeof(GCstr))) {
    return arm64_string_byte_dynamic_guard_postra(v, guardref);
  }
  {
    IRIns len = ir_load_acq(&v->ir[lenref]);
    if (len.o != IR_FLOAD || len.t.irt != IRT_INT ||
        len.op2 != IRFL_STR_LEN ||
        !arm64_scalar_postra_ref(v, len.op1, guardref, IRT_STR)) return 0;
    for (ref = guardref+1u; ref < v->nins; ref++) {
      IRIns strref = ir_load_acq(&v->ir[ref]);
      int32_t offset;
      if (strref.o == IR_STRREF && strref.t.irt == IRT_PGC &&
          strref.op1 == len.op1 &&
          arm64_scalar_postra_kint(v, strref.op2, &offset) &&
          offset == bound-1)
        return 1;
    }
  }
  return 0;
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
  if (arm64_scalar_ksimd_postra_fload(source, 0) ||
      arm64_scalar_ksimd_postra_fload(source, 1)) {
    unsigned index = arm64_scalar_ksimd_postra_fload(source, 0) ? 0 : 1;
    if (source.r != RID_INIT || ra_hasspill(source.s) || source.s != 0)
      return 0;
    for (ref = value + 1; ref < end; ref++) {
      IRIns use = ir_load_acq(&v->ir[ref]);
      if (use.op1 == value || use.op2 == value) {
        if ((index == 0 && (use.o != IR_ABS || use.op2 != value)) ||
            (index == 1 && (use.o != IR_NEG || use.op2 != value)))
          return 0;
      }
    }
    return 1;
  }
  if (source.r != RID_INIT &&
      !(ra_hashint(source.r) &&
        arm64_scalar_postra_register(ra_gethint(source.r),
                                     (IRType)irt_type(source.t))))
    return 0;
  if (source.o == IR_PHI)
    return 0;
  if (source.o == IR_STRREF && source.t.irt == IRT_PGC) {
    int seen = 0;
    for (ref = value + 1u; ref < end; ref++) {
      IRIns use = ir_load_acq(&v->ir[ref]);
      if (use.op1 != value && use.op2 != value) continue;
      if (use.o != IR_XLOAD || use.op1 != value ||
          use.t.irt != IRT_U8 || use.op2 != IRXLOAD_READONLY)
        return 0;
      seen = 1;
    }
    return seen;
  }
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
        (!irref_isk(r1.op1) && (r1.op1 < REF_FIRST || r1.op1 >= root1)) ||
        !arm64_scalar_postra_ref(v, r1.op1, root1, IRT_FUNC) ||
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

static const GCfunc *arm64_lua_leaf_postra_function(
                                      const LJArm64PostRAView *v, IRRef fn,
                                      IRRef before, IRRef *guardp);

/* A closed-upvalue helper roots the exact child function before reading its
** cell. Permit either the immutable current-frame function root or an exact,
** addressable function constant; do not accept an arbitrary FUNC-typed value. */
static int arm64_upvalue_fnroot_postra(const LJArm64PostRAView *v, IRRef root,
                                      IRRef before, uint16_t mode)
{
  IRIns tmp;
  if (!v || !root || root >= before || root >= v->nins) return 0;
  tmp = ir_load_acq(&v->ir[root]);
  if (tmp.o != IR_TMPREF || tmp.t.irt != IRT_PGC || tmp.op2 != mode ||
      (!irref_isk(tmp.op1) &&
       (tmp.op1 < REF_FIRST || tmp.op1 >= root))) return 0;
  /* The context proof below binds this scalar source to its exact function. */
  return arm64_scalar_postra_ref(v, tmp.op1, root, IRT_FUNC);
}

static int arm64_upvalue_postra_context(const LJArm64PostRAView *v,
                                      const SnapShot *snap, const BCIns *pc,
                                      IRRef root, const GCproto **ptp)
{
  MSize ofs, nent, pos;
  if (!v || !v->proto || !snap || !pc || !ptp) return 0;
  ofs = snap_mapofs_acq(snap); nent = snap_nent_acq(snap);
  if (ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
      v->nsnapmap-ofs-nent < 2u) return 0;
  if (arm64_ir_pcpos((uintptr_t)pc, (uintptr_t)v->proto_bc,
      (uintptr_t)(v->proto_bc+v->proto_sizebc), &pos)) {
    IRIns tmp, src;
    if (root < REF_FIRST || root >= v->nins) return 0;
    tmp = ir_load_acq(&v->ir[root]);
    if (tmp.o != IR_TMPREF || tmp.t.irt != IRT_PGC ||
        tmp.op1 < REF_FIRST || tmp.op1 >= root || tmp.op1 >= v->nins ||
        !arm64_scalar_postra_ref(v, tmp.op1, root, IRT_FUNC)) return 0;
    src = ir_load_acq(&v->ir[tmp.op1]);
    if (src.o != IR_SLOAD || src.op1 != 0 || src.t.irt != IRT_FUNC ||
        src.op2 != IRSLOAD_READONLY) return 0;
    *ptp = v->proto;
    return 1;
  }
  {
    SnapEntry words[2];
    uint64_t pcbase;
    LJArm64LuaFramePath frames;
    IRRef function;
    const GCfunc *fn;
    words[0] = snapentry_acq(&v->snapmap[ofs+nent]);
    words[1] = snapentry_acq(&v->snapmap[ofs+nent+1u]);
    memcpy(&pcbase, words, sizeof(pcbase));
    if (!arm64_lua_frame_path(&v->snapmap[ofs], nent, pcbase, &frames) ||
        frames.depth != 1u) return 0;
    function = frames.child[0].funcref;
    /* The allocated child proof separately binds this saved value to the
    ** caller's exact CALL and identity guard. */
    fn = arm64_lua_leaf_postra_function(v, function, root, NULL);
    if (!fn || !isluafunc(fn) || root >= v->nins ||
        ir_load_acq(&v->ir[root]).op1 != function ||
        !arm64_ir_pcpos((uintptr_t)pc,
        (uintptr_t)proto_bc(funcproto(fn)),
        (uintptr_t)(proto_bc(funcproto(fn))+funcproto(fn)->sizebc), &pos))
      return 0;
    *ptp = funcproto(fn);
  }
  return 1;
}

static int arm64_upvalue_postra_snapshots(const LJArm64PostRAView *v,
                                         IRRef ref, MSize s, IRRef root1,
                                         IRRef root2)
{
  IRIns call, tail, head;
  const SnapShot *pre, *post;
  const SnapEntry *map;
  const BCIns *pc;
  BCIns bc;
  int get;
  uint32_t uv;
  IRRef value = 0;
  MSize n, slot;
  const GCproto *sourcept;
  MSize preofs, prenent;
  if (!v || !v->ir || !v->snap || !v->snapmap || ref < REF_FIRST ||
      ref >= v->nins || s+1u >= v->nsnap) return 0;
  pre = &v->snap[s];
  preofs = snap_mapofs_acq(pre); prenent = snap_nent_acq(pre);
  if (preofs > v->nsnapmap || prenent > v->nsnapmap-preofs ||
      v->nsnapmap-preofs-prenent < 2u) return 0;
  map = &v->snapmap[preofs];
  call = ir_load_acq(&v->ir[ref]);
  get = call.op2 == IRCALL_lj_func_getuv_rooted_try;
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
  pc = snap_pc_acq(&map[prenent]);
  if (snap_ref_acq(pre) != root1 ||
      !arm64_upvalue_fnroot_postra(v, root1, root1+1u,
        get ? (IRTMPREF_IN1|IRTMPREF_OUT1) : IRTMPREF_IN1))
    return 0;
  if (!arm64_upvalue_postra_context(v, pre, pc, root1, &sourcept))
    return 0;
  bc = (BCIns)la_load32_acq((const uint32_t *)pc);
  uv = get ? bc_d(bc) : bc_a(bc);
  if (
      uv >= sourcept->sizeuv ||
      (get ? bc_op(bc) != BC_UGET :
       bc_op(bc) != BC_USETV && bc_op(bc) != BC_USETS &&
       bc_op(bc) != BC_USETN && bc_op(bc) != BC_USETP) ||
      s+1u >= v->nsnap)
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
  {
    MSize postofs = snap_mapofs_acq(post), postnent = snap_nent_acq(post);
    if (postofs > v->nsnapmap || postnent > v->nsnapmap-postofs ||
        v->nsnapmap-postofs-postnent < 2u) return 0;
  }
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
    if (bc_d(bc) >= sourcept->sizekn) return 0;
    lj_tv_load_acq(&expected, proto_knumtv(sourcept, bc_d(bc)));
    if (tvisint(&expected))
      return k.o == IR_KINT && k.t.irt == IRT_INT && k.i == intV(&expected);
    if (k.o != IR_KNUM || k.t.irt != IRT_NUM) return 0;
    return tv_rawload(&expected) == tv_rawload(ir_knum(&v->ir[value]));
  } else {
    IRIns k = ir_load_acq(&v->ir[value]);
    ptrdiff_t idx = ~(ptrdiff_t)bc_d(bc);
    GCobj *expected, *actual;
    if (k.o != IR_KGC || idx >= 0 ||
        (uintptr_t)idx < ~(uintptr_t)proto_sizekgc_acq(sourcept)+1u)
      return 0;
    expected = proto_kgc_acq(sourcept, idx);
    actual = ir_kgc_load_acq(&v->ir[value]);
    return expected && expected->gch.gct == (uint32_t)~LJ_TSTR &&
           k.t.irt == IRT_STR && expected == actual;
  }
}

static IRRef arm64_lua_leaf_postra_upvalue(const LJArm64PostRAView *v,
                                           const GCproto *pt, MSize pos,
                                           IRRef before, IRRef end)
{
  const BCIns *sourcepc;
  IRRef lower = 0, loop = 0, ref, selected = 0;
  if (!v || !pt || pos >= pt->sizebc || before >= v->nins ||
      end > v->nins || end < before) return 0;
  sourcepc = &proto_bc(pt)[pos];
  for (ref = REF_FIRST; ref < v->nins; ref++)
    if (ir_load_acq(&v->ir[ref]).o == IR_LOOP) {
      if (loop) return 0;
      loop = ref;
    }
  if (loop)
    lower = before > loop ? loop : REF_FIRST;
  else if (bc_op(v->startins) == BC_FUNCF &&
           arm64_ir_funcf_leaf_call_source(pt))
    lower = REF_FIRST;
  else
    return 0;
  for (ref = lower; ref < end; ref++) {
    IRIns call;
    IRRef root;
    MSize s;
    if (ir_load_acq(&v->ir[ref]).o != IR_CALLS) continue;
    call = ir_load_acq(&v->ir[ref]);
    if (call.op2 != IRCALL_lj_func_getuv_rooted_try || ref+2u >= end ||
        call.op1 < REF_FIRST || call.op1 >= ref) continue;
    {
      IRIns tail = ir_load_acq(&v->ir[call.op1]);
      if (tail.o != IR_CARG || tail.op2 < REF_FIRST || tail.op2 >= ref)
        continue;
      root = tail.op2;
    }
    for (s = 0; s < v->nsnap; s++) {
      const SnapShot *snap = &v->snap[s];
      MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
      const BCIns *pc;
      if (snap_ref_acq(snap) != root || ofs > v->nsnapmap ||
          nent > v->nsnapmap-ofs || v->nsnapmap-ofs-nent < 2u) continue;
      pc = snap_pc_acq(&v->snapmap[ofs+nent]);
      if (pc != sourcepc ||
          !arm64_upvalue_postra_snapshots(v, ref, s, root, 0)) continue;
      if (selected) return 0;
      selected = ref+2u;
    }
  }
  return selected;
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

/* Pure math calls do not enter Lua or release its native GC phase lease.
** Their only extra machine-state effect is the Darwin AAPCS64 call clobber.
** Check both ordinary uses (including invariant uses across a loop backedge)
** and the exact renamed locations used by exits after each call. */
static int arm64_scalar_math_regsp_survives(RegSP rs, IRType type, MSize cap)
{
  MSize spill = regsp_spill(rs), width = type == IRT_INT || type == IRT_U8 ? 1u : 2u;
  Reg reg = regsp_reg(rs);
  if (ra_hasspill(spill))
    return spill >= SPS_FIRST && (width == 1u || !(spill & 1u)) &&
           spill + width <= cap;
  return type == IRT_NUM ? reg >= RID_D8 && reg <= RID_D15 :
                          reg >= RID_X19 && reg <= RID_X28;
}

static int arm64_scalar_math_live_across(const LJArm64PostRAView *v,
                                        IRRef n, IRRef loop, IRRef call,
                                        MSize cap)
{
  IRRef value, ref;
  MSize s, i;
  for (value = REF_FIRST; value < call; value++) {
    IRIns source = ir_load_acq(&v->ir[value]);
    IRRef first = value < loop && loop < call ? loop+1u : call+1u;
    int needed = 0;
    if (!arm64_scalar_postra_value(source) ||
        arm64_scalar_ksimd_postra_fload(source, 0) ||
        arm64_scalar_ksimd_postra_fload(source, 1)) continue;
    for (ref = first; ref < n && !needed; ref++) {
      IRIns use = ir_load_acq(&v->ir[ref]);
      IRMode mode = lj_ir_mode[use.o];
      needed = (irm_op1(mode) == IRMref && use.op1 == value) ||
               (irm_op2(mode) == IRMref && use.op2 == value);
    }
    if (needed && !arm64_postra_value_survives_call(v, value, call, cap, 0))
      return 0;
    for (s = 0; s < v->nsnap; s++) {
      const SnapShot *snap = &v->snap[s];
      MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
      IRRef at = snap_ref_acq(snap);
      if (at <= call && !(value < loop && loop < call && at > loop)) continue;
      for (i = 0; i < nent; i++) {
        SnapEntry entry = snapentry_acq(&v->snapmap[ofs+i]);
        IRRef material = value;
        IRIns p = source;
        RegSP rs;
        if (snap_ref(entry) != value || (entry & SNAP_NORESTORE)) continue;
        rs = arm64_scalar_postra_snapshot_regsp(v, n, material, s);
        if (!ra_hasspill(regsp_spill(rs)) && ra_noreg(regsp_reg(rs)) &&
            p.o == IR_CONV && p.t.irt == IRT_NUM && p.op2 == IRCONV_NUM_INT) {
          material = p.op1;
          if (irref_isk(material)) continue;
          p = ir_load_acq(&v->ir[material]);
          rs = arm64_scalar_postra_snapshot_regsp(v, n, material, s);
        }
        if (!arm64_scalar_math_regsp_survives(rs, (IRType)irt_type(p.t), cap))
          return 0;
      }
    }
  }
  return 1;
}
static const GCfunc *arm64_lua_leaf_postra_function(
                                      const LJArm64PostRAView *v, IRRef fn,
                                      IRRef before, IRRef *guardp);

/* The allocated pass independently decodes FUNCV's fixed VARG window. */
static int arm64_lua_leaf_postra_varg_shape(const GCproto *pt,
                                            MSize *dstp, MSize *countp)
{
  MSize p, found = 0, dst = 0, count = 0;
  if (!pt || !(la_load8_acq(&pt->flags) & PROTO_VARARG) || pt->numparams != 0 ||
      !pt->sizebc || pt->sizebc > UINT8_MAX || pt->framesize > UINT8_MAX)
    return 0;
  for (p = 1u; p+1u < pt->sizebc; p++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[p]);
    BCOp op = bc_op(ins);
    if (op == BC_VARG) {
      if (found++ != 0u || p != 1u || bc_a(ins) != 0u ||
          bc_b(ins) < 2u || bc_c(ins) != 0u) return 0;
      dst = bc_a(ins); count = bc_b(ins)-1u;
      if (dst+count > pt->framesize) return 0;
    }
  }
  if (found != 1u) return 0;
  if (dstp) *dstp = dst;
  if (countp) *countp = count;
  return 1;
}

/* Re-derive the leaf's caller/argument binding from the allocated view. */
static IRRef arm64_iterl_postra_slot(const LJArm64PostRAView *v,
                                    const SnapShot *snap, MSize slot)
{
  MSize i, ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap);
  IRRef r, at = snap_ref_acq(snap), found = 0;
  for (i = 0; i < nent; i++) {
    SnapEntry e = snapentry_acq(&v->snapmap[ofs+i]);
    if (snap_slot(e) == slot) {
      if (e & 0x00ff0000u) return 0;
      return snap_ref(e);
    }
  }
  for (r = REF_FIRST; r < at; r++) {
    IRIns p = ir_load_acq(&v->ir[r]);
    if (p.o != IR_SLOAD || p.op1 != slot) continue;
    if (found || p.op2 != IRSLOAD_TYPECHECK ||
        (p.t.irt & ~(IRT_TYPE|IRT_ISPHI)) != IRT_GUARD) return 0;
    found = r;
  }
  return found;
}

static IRRef arm64_lua_leaf_postra_args_at(const LJArm64PostRAView *v,
                                      MSize s, IRRef callee, IRRef *args,
                                      const GCproto *parentpt,
                                      MSize framebase)
{
  GCtrace *owner = (GCtrace *)v->owner;
  const SnapShot *snap = &v->snap[s];
  MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap), i, slot, pos;
  MSize nargs, nparams, mapped = 0, nresults;
  MSize vargdst = 0, vargcount = 0;
  uint8_t isvararg;
  uint8_t supplied[UINT8_MAX+1u] = {0};
  int loopmap = 0;
  int has_call = 0, iterator = bc_op(v->startins) == BC_ITERL;
  LJArm64IteratorSource iter;
  IRType types[UINT8_MAX+1u];
  IRRef actual[UINT8_MAX+1u] = {0};
  IRRef ref = snap_ref_acq(snap), fn = 0, r;
  LJArm64LuaLeafBranch branch;
  SnapEntry footer[2];
  uint64_t pcbase;
  const GCfunc *function;
  const GCproto *leaf;
  BCIns call;
  if (!v->proto || !parentpt || !owner ||
      proto_bc(v->proto) != v->proto_bc ||
      v->proto->sizebc != v->proto_sizebc ||
      v->proto->framesize != v->root_topslot ||
      trace_startptgco_acq(owner) != obj2gco((GCproto *)v->proto) ||
      trace_startins_acq(owner) != v->startins ||
      trace_startpc_acq(owner) != v->startpc ||
      (!iterator && bc_op(v->startins) != BC_FORL &&
       !(bc_op(v->startins) == BC_FUNCF &&
         arm64_ir_funcf_leaf_call_source(parentpt)))) return 0;
  function = arm64_lua_leaf_postra_function(v, callee, ref, NULL);
  if (!function) return 0;
  if (!lj_trace_arm64_lua_leaf(function) ||
      !arm64_lua_leaf_branch(funcproto(function), &branch)) return 0;
  leaf = funcproto(function); nparams = leaf->numparams;
  isvararg = (uint8_t)((la_load8_acq(&leaf->flags) & PROTO_VARARG) != 0);
  if (isvararg &&
      !arm64_lua_leaf_postra_varg_shape(leaf, &vargdst, &vargcount))
    return 0;
  nresults = arm64_lua_leaf_return_count(leaf);
  if (!nresults || (nresults == 2u && branch.compare != UINT32_MAX))
    return 0;
  for (i = 1; i+1u < leaf->sizebc; i++)
    if (bc_op((BCIns)la_load32_acq(
        (const uint32_t *)&proto_bc(leaf)[i])) == BC_CALL && nresults != 1u)
      return 0;
  for (i = 1; i+1u < leaf->sizebc; i++)
    if (bc_op((BCIns)la_load32_acq(
        (const uint32_t *)&proto_bc(leaf)[i])) == BC_CALL)
      has_call = 1;
  if (isvararg && has_call) return 0;
  for (i = 0; i < UINT8_MAX+1u; i++) types[i] = IRT_NIL;
  footer[0] = snapentry_acq(&v->snapmap[ofs+nent]);
  footer[1] = snapentry_acq(&v->snapmap[ofs+nent+1u]);
  memcpy(&pcbase, footer, sizeof(pcbase));
  if (!arm64_ir_pcpos((uintptr_t)(pcbase>>8),
      (uintptr_t)proto_bc(parentpt),
      (uintptr_t)(proto_bc(parentpt)+parentpt->sizebc), &pos))
    return 0;
  if (iterator) {
    IRRef loop;
    if (!lj_asm_arm64_iterl_tuple(v->proto, v->startpc, v->startins,
        v->owner_private ? v->startins : BCINS_AD(BC_JITERL,
          bc_a(v->startins), trace_traceno_acq(owner)),
        v->owner_private, &iter)) return 0;
    for (loop = REF_FIRST; loop < v->nins; loop++)
      if (ir_load_acq(&v->ir[loop]).o == IR_LOOP) break;
    loopmap = ref == loop && pos == iter.body_pc;
    if (loopmap && !arm64_iterl_scalar_body(parentpt, &iter)) return 0;
  }
  call = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(parentpt)[
    loopmap ? iter.call_pc : pos]);
  if (iterator && (parentpt != v->proto || framebase != 1u+LJ_FR2 ||
      isvararg || has_call || nparams != 2u || nresults != 1u ||
      !lj_asm_arm64_iterl_tuple(v->proto, v->startpc, v->startins,
        v->owner_private ? v->startins :
          BCINS_AD(BC_JITERL, bc_a(v->startins), trace_traceno_acq(owner)),
        v->owner_private, &iter) || (!loopmap && pos != iter.call_pc) ||
      call != iter.callins)) return 0;
  if ((!iterator && bc_op(call) != BC_CALL) ||
      bc_b(call) != nresults+1u || !bc_c(call) ||
      bc_a(call)+bc_c(call)+LJ_FR2 > parentpt->framesize ||
      bc_a(call)+bc_b(call)-1u+LJ_FR2 > parentpt->framesize) return 0;
  nargs = bc_c(call)-1u;
  if (args) {
    MSize init = nparams;
    if (isvararg && vargdst+vargcount > init) init = vargdst+vargcount;
    for (i = 0; i < init; i++) args[i] = REF_NIL;
  }
  slot = framebase+bc_a(call);
  for (i = 0; i < nent; i++) {
    SnapEntry sn = snapentry_acq(&v->snapmap[ofs+i]);
    MSize at = snap_slot(sn);
    if (at == slot) {
      if (sn & 0x00ff0000u) return 0;
      if (!loopmap) fn = snap_ref(sn);
    } else if (at >= slot+1u+LJ_FR2 && at < slot+1u+LJ_FR2+nargs) {
      IRRef value = snap_ref(sn);
      MSize formal = at-slot-1u-LJ_FR2;
      if ((sn & 0x00ff0000u) || supplied[formal] ||
          !arm64_cell_value_postra_ref(v, value, ref)) return 0;
      supplied[formal] = 1;
      if (formal >= UINT8_MAX+1u) return 0;
      actual[formal] = value;
      if (formal < nparams)
        types[formal] = irt_type(ir_load_acq(&v->ir[value]).t);
      if (args) args[formal] = value;
      mapped++;
    }
  }
  if (iterator) {
    IRRef sourcefn = arm64_iterl_postra_slot(v, snap, slot-3u);
    if (loopmap) fn = sourcefn;
    if (mapped || !sourcefn || sourcefn != fn) return 0;
    for (i = 0; i < 2u; i++) {
      IRRef arg = arm64_iterl_postra_slot(v, snap, slot-2u+i);
      if (!arg || !arm64_cell_value_postra_ref(v, arg, ref)) return 0;
      actual[i] = arg;
      types[i] = irt_type(ir_load_acq(&v->ir[arg]).t);
      if (args) args[i] = arg;
      mapped++;
    }
  }
  if (mapped != nargs || !arm64_scalar_postra_ref(v, fn, ref, IRT_FUNC)) return 0;
  if (isvararg) {
    MSize available = nargs > nparams ? nargs-nparams : 0;
    for (i = 0; i < vargcount; i++) {
      MSize source = nparams+i;
      IRRef arg = source < available+nparams ? actual[source] : REF_NIL;
      types[vargdst+i] = arg == REF_NIL ? IRT_NIL :
                         irt_type(ir_load_acq(&v->ir[arg]).t);
      if (args) args[vargdst+i] = arg;
    }
  }
  /* Independently propagate formal types through the complete bounded body;
  ** not every numeric operation has an executable child snapshot. */
  if (branch.compare == UINT32_MAX) {
    /* The allocated recursive source walk proves call result types and every
    ** arithmetic continuation at the child frame's exact resume point. */
    if (!has_call &&
        !arm64_lua_leaf_type_range(leaf, 1, leaf->sizebc-1u, types))
      return 0;
  } else {
    IRType first[UINT8_MAX+1u], second[UINT8_MAX+1u];
    BCIns cmp = (BCIns)la_load32_acq(
      (const uint32_t *)&proto_bc(leaf)[branch.compare]);
    if (!arm64_lua_leaf_type_range(leaf, 1, branch.compare, types)) return 0;
    IRType left = types[bc_a(cmp)], right = types[bc_d(cmp)];
    if ((left != IRT_INT && left != IRT_NUM) ||
        (right != IRT_INT && right != IRT_NUM)) return 0;
    memcpy(first, types, sizeof(first));
    memcpy(second, types, sizeof(second));
    if (!arm64_lua_leaf_type_path(leaf, &branch, branch.first, first) ||
        !arm64_lua_leaf_type_path(leaf, &branch, branch.second, second))
      return 0;
    if (!branch.early_return) {
      for (i = 0; i < leaf->framesize; i++)
        types[i] = first[i] == second[i] ? first[i] : IRT_NIL;
      if (!arm64_lua_leaf_type_range(leaf, branch.join,
                                     leaf->sizebc-1u, types)) return 0;
    }
  }
  if (fn == callee) return fn;
  for (r = REF_FIRST; r <= ref && r < v->nins; r++) {
    IRIns guard = ir_load_acq(&v->ir[r]);
    if (guard.o == IR_EQ && guard.t.irt == (IRT_FUNC|IRT_GUARD) &&
        guard.op1 == fn && guard.op2 == callee) return fn;
  }
  return 0;
}

static IRRef arm64_lua_leaf_postra_args(const LJArm64PostRAView *v,
                                      MSize s, IRRef callee, IRRef *args)
{
  return arm64_lua_leaf_postra_args_at(v, s, callee, args, v->proto,
                                       1u+LJ_FR2);
}

static int arm64_lua_leaf_postra_snapshot(const LJArm64PostRAView *v,
                                        IRRef ref, MSize s)
{
  IRIns guard = ir_load_acq(&v->ir[ref]);
  const SnapShot *snap = &v->snap[s];
  MSize nent = snap_nent_acq(snap), i;
  SnapEntry map[UINT8_MAX+1u], footer[1+LJ_FR2];
  uint64_t pcbase;
  LJArm64LuaFramePath frames;
  const GCproto *parentpt = v->proto;
  MSize framebase = 1u+LJ_FR2;
  if (snap_ref_acq(snap) != ref || nent > UINT8_MAX+1u ||
      snap_mapofs_acq(snap) > v->nsnapmap ||
      nent > v->nsnapmap-snap_mapofs_acq(snap) ||
      v->nsnapmap-snap_mapofs_acq(snap)-nent < 1u+LJ_FR2)
    return 0;
  for (i = 0; i < nent; i++)
    map[i] = snapentry_acq(&v->snapmap[snap_mapofs_acq(snap)+i]);
  for (i = 0; i < 1u+LJ_FR2; i++)
    footer[i] = snapentry_acq(&v->snapmap[snap_mapofs_acq(snap)+nent+i]);
  memcpy(&pcbase, footer, sizeof(pcbase));
  if (!arm64_lua_frame_path(map, nent, pcbase, &frames)) return 0;
  if (frames.depth) {
    const GCfunc *parent = arm64_lua_leaf_postra_function(v,
        frames.child[frames.depth-1u].funcref, ref, NULL);
    if (!parent || !lj_trace_arm64_lua_leaf(parent)) return 0;
    parentpt = funcproto(parent);
    framebase = frames.child[frames.depth-1u].baseslot;
  }
  {
    IRRef argsfn = arm64_lua_leaf_postra_args_at(v, s, guard.op2, NULL,
						parentpt, framebase);
    if (argsfn != guard.op1) {
      return 0;
    }
    return 1;
  }
}

/* Allocated-view literal and expression resolution deliberately do not use
** recorder-side prefix state or its verdict. RENAME/location coexistence is
** checked separately for the exact snapshot after this value proof. */
static IRRef arm64_lua_leaf_postra_literal(const LJArm64PostRAView *v,
                                          lua_Number number, int numonly)
{
  IRRef k;
  TValue bits;
  int integer;
  setnumV(&bits, number);
  integer = !numonly && lj_num2int_ok(number) && !tvismzero(&bits);
  for (k = v->nk; k < REF_TRUE; k++) {
    IRIns c = ir_load_acq(&v->ir[k]);
    if (integer && c.o == IR_KINT && (lua_Number)c.i == number) return k;
    if (c.o == IR_KNUM) {
      IRIns payload = ir_load_acq(&v->ir[k+1u]);
      if (!integer && payload.tv.u64 == bits.u64) return k;
      k++;
    } else if (c.o == IR_KGC || c.o == IR_KKPTR) {
      k++;
    }
  }
  return 0;
}

static IRRef arm64_lua_leaf_postra_num(const LJArm64PostRAView *v,
                                      IRRef value, IRRef end)
{
  IRRef r;
  if (arm64_scalar_postra_ref(v, value, end, IRT_NUM)) return value;
  if (!arm64_scalar_postra_ref(v, value, end, IRT_INT)) return 0;
  if (irref_isk(value)) {
    IRIns k = ir_load_acq(&v->ir[value]);
    return arm64_lua_leaf_postra_literal(v, (lua_Number)k.i, 1);
  }
  for (r = value+1u; r < end; r++) {
    IRIns p = ir_load_acq(&v->ir[r]);
    if (p.o == IR_CONV && irt_type(p.t) == IRT_NUM &&
        p.op2 == IRCONV_NUM_INT && p.op1 == value) return r;
  }
  return 0;
}

/* Allocated comparison binding independently checks both operand types,
** conversion refs, guard polarity and the exact cold interpreter edge. */
static int arm64_lua_leaf_postra_branch_guard(
                                      const LJArm64PostRAView *v,
                                      const GCproto *pt,
                                      const LJArm64LuaLeafBranch *branch,
                                      const IRRef *slots, IRRef ref,
                                      MSize coldpos)
{
  MSize cold;
  return arm64_lua_leaf_postra_branch_cold(v, pt, branch, slots, ref, &cold) &&
         coldpos == cold;
}

static IRRef arm64_lua_leaf_postra_arith(const LJArm64PostRAView *v,
                                        IROp op, IRRef left, IRRef right,
                                        IRRef end)
{
  IRRef r;
  if (op != IR_DIV && arm64_scalar_postra_ref(v, left, end, IRT_INT) &&
      arm64_scalar_postra_ref(v, right, end, IRT_INT)) {
    IRRef a = left, b = right;
    IRIns x = ir_load_acq(&v->ir[a]), y = ir_load_acq(&v->ir[b]);
    IROp want = (IROp)(op-IR_ADD+IR_ADDOV);
    if (irref_isk(a) && irref_isk(b)) {
      int64_t result;
      switch (op) {
      case IR_ADD: result = (int64_t)x.i+y.i; break;
      case IR_SUB: result = (int64_t)x.i-y.i; break;
      default: result = (int64_t)x.i*y.i; break;
      }
      if (result >= INT32_MIN && result <= INT32_MAX)
        return arm64_lua_leaf_postra_literal(v, (lua_Number)result, 0);
    }
    if (op == IR_SUB && left == right)
      return arm64_lua_leaf_postra_literal(v, 0, 0);
    if (op != IR_SUB && irref_isk(a)) {
      IRRef swap = a; a = b; b = swap; y = x;
    }
    if (irref_isk(b)) {
      if (op != IR_MUL && y.i == 0) return a;
      if (op == IR_MUL && y.i == 0) return b;
      if (op == IR_MUL && y.i == 1) return a;
      if (op == IR_MUL && y.i == 2) { want = IR_ADDOV; b = a; }
    }
    for (r = REF_FIRST; r < end; r++) {
      IRIns p = ir_load_acq(&v->ir[r]);
      if (p.o != want || (p.t.irt & ~IRT_ISPHI) != (IRT_INT|IRT_GUARD))
        continue;
      if (p.op1 == a && p.op2 == b) return r;
      if (want != IR_SUBOV && p.op1 == b && p.op2 == a) return r;
    }
  }
  left = arm64_lua_leaf_postra_num(v, left, end);
  right = arm64_lua_leaf_postra_num(v, right, end);
  if (!left || !right) return 0;
  for (r = REF_FIRST; r < end; r++) {
    IRIns p = ir_load_acq(&v->ir[r]);
    if (p.o != op || (p.t.irt & ~IRT_ISPHI) != IRT_NUM) continue;
    if (p.op1 == left && p.op2 == right) return r;
    if ((op == IR_ADD || op == IR_MUL) &&
        p.op1 == right && p.op2 == left) return r;
  }
  return 0;
}

/* Independent allocated-view callable resolution and source replay. The
** semantic evaluator above is intentionally not reused here. */
static const GCfunc *arm64_lua_leaf_postra_function(
                                      const LJArm64PostRAView *v, IRRef fn,
                                      IRRef before, IRRef *guardp)
{
  IRRef r;
  IRIns value;
  if (guardp) *guardp = 0;
  if (!arm64_scalar_postra_ref(v, fn, before, IRT_FUNC)) return NULL;
  value = ir_load_acq(&v->ir[fn]);
  if (irref_isk(fn) && value.o == IR_KGC) {
    GCobj *o = ir_kgc_load_acq(&v->ir[fn]);
    if (o && o->gch.gct == (uint32_t)~LJ_TFUNC) return gco2func(o);
  }
  for (r = REF_FIRST; r < before && r < v->nins; r++) {
    IRIns guard = ir_load_acq(&v->ir[r]);
    IRRef constant;
    if (guard.o != IR_EQ || guard.t.irt != (IRT_FUNC|IRT_GUARD) ||
        (guard.op1 != fn && guard.op2 != fn)) continue;
    constant = guard.op1 == fn ? guard.op2 : guard.op1;
    if (!irref_isk(constant) || constant < v->nk || constant >= REF_TRUE ||
        !arm64_scalar_postra_ref(v, constant, before, IRT_FUNC) ||
        ir_load_acq(&v->ir[constant]).o != IR_KGC) continue;
    {
      GCobj *o = ir_kgc_load_acq(&v->ir[constant]);
      if (o && o->gch.gct == (uint32_t)~LJ_TFUNC) {
        if (guardp) *guardp = r;
        return gco2func(o);
      }
    }
  }
  return NULL;
}

static int arm64_lua_leaf_postra_eval_multi(const LJArm64PostRAView *v,
                                      const GCproto *pt, const IRRef *args,
                                      MSize pc, IRRef before, IRRef end,
                                      const GCfunc **ancestors, MSize depth,
                                      IRRef *slots, IRRef *results,
                                      MSize *nresultsp)
{
  const BCIns *bc;
  IRRef state[UINT8_MAX+1u] = {0};
  MSize p, i;
  int captured = 0;
  if (!v || !pt || !args || !ancestors || !slots || !results || !nresultsp ||
      pt->framesize > UINT8_MAX || pt->numparams > pt->framesize ||
      pc < 1u || pc >= pt->sizebc || depth > LJ_ARM64_LUA_MAX_CHILD_FRAMES)
    return 0;
  bc = proto_bc(pt);
  for (i = 0; i < pt->numparams; i++) state[i] = args[i];
  for (p = 1; p+1u < pt->sizebc; p++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[p]);
    BCOp op = bc_op(ins);
    BCReg a = bc_a(ins), b = bc_b(ins), c = bc_c(ins), d = bc_d(ins);
    IRRef value = 0, left, right;
    TValue k;
    IROp arith;
    if (p == pc) {
      memcpy(slots, state, sizeof(state));
      captured = 1;
    }
    if (a >= pt->framesize) return 0;
    if (op == BC_CGET || op == BC_MOV || op == BC_CSET) {
      if (d >= pt->framesize || !state[d]) return 0;
      value = state[d];
      if (op == BC_CSET &&
          ((state[a] && !arm64_cell_value_postra_ref(v, state[a], before)) ||
           !arm64_cell_value_postra_ref(v, value, before))) return 0;
    } else if (op == BC_KSHORT) {
      value = arm64_lua_leaf_postra_literal(v, (lua_Number)(int16_t)d, 0);
    } else if (op == BC_KPRI) {
      if (d > 2u) return 0;
      value = (IRRef)(REF_NIL-d);
    } else if (op == BC_KNUM) {
      if (d >= pt->sizekn) return 0;
      proto_knumtv_load_acq(&k, pt, d);
      if (!tvisnumber(&k)) return 0;
      value = arm64_lua_leaf_postra_literal(v, numberVnum(&k), 0);
    } else if (op == BC_UGET) {
      if (d >= pt->sizeuv) return 0;
      value = arm64_lua_leaf_postra_upvalue(v, pt, p, before, end);
    } else if (op == BC_CALL) {
      const GCfunc *callee;
      const GCfunc *nested[LJ_ARM64_LUA_MAX_CHILD_FRAMES+1u];
      IRRef childargs[UINT8_MAX+1u], childslots[UINT8_MAX+1u];
      IRRef childresults[2] = {0, 0};
      MSize childnresults = 0;
      IRRef callguard = 0;
      MSize nargs;
      if ((bc_b(ins) != 2u && bc_b(ins) != 3u) || bc_c(ins) < 2u ||
          (MSize)a+bc_c(ins)+LJ_FR2 > pt->framesize ||
          depth >= LJ_ARM64_LUA_MAX_CHILD_FRAMES || !state[a]) return 0;
      nargs = bc_c(ins)-1u;
      callee = arm64_lua_leaf_postra_function(v, state[a], end, &callguard);
      if (!callee || !lj_trace_arm64_lua_leaf(callee)) return 0;
      for (i = 0; i < depth; i++) if (ancestors[i] == callee) return 0;
      memcpy(nested, ancestors, depth*sizeof(*nested));
      nested[depth] = callee;
      for (i = 0; i < funcproto(callee)->numparams; i++) {
        childargs[i] = i < nargs ? state[a+1u+LJ_FR2+i] : REF_NIL;
        if (!arm64_cell_value_postra_ref(v, childargs[i], end)) return 0;
      }
      for (; i < UINT8_MAX+1u; i++) childargs[i] = REF_NIL;
      if (!arm64_lua_leaf_postra_eval_multi(v, funcproto(callee), childargs,
          funcproto(callee)->sizebc-1u, before, end, nested, depth+1u,
          childslots, childresults, &childnresults) ||
          childnresults != bc_b(ins)-1u) return 0;
      for (i = 0; i < childnresults; i++)
        if (!arm64_cell_value_postra_ref(v, childresults[i], end) ||
            (callguard && callguard >= childresults[i])) return 0;
      value = childresults[0];
      if (childnresults == 2u) state[a+1u] = childresults[1];
    } else if (op == BC_ADDVV || op == BC_SUBVV || op == BC_MULVV ||
               op == BC_DIVVV || op == BC_ADDVN || op == BC_SUBVN ||
               op == BC_MULVN || op == BC_DIVVN || op == BC_ADDNV ||
               op == BC_SUBNV || op == BC_MULNV || op == BC_DIVNV) {
      left = state[b];
      if (!left) return 0;
      if (op == BC_ADDVV || op == BC_SUBVV || op == BC_MULVV ||
          op == BC_DIVVV) {
        if (c >= pt->framesize || !state[c]) return 0;
        right = state[c];
      } else {
        if (c >= pt->sizekn) return 0;
        proto_knumtv_load_acq(&k, pt, c);
        if (!tvisnumber(&k)) return 0;
        right = arm64_lua_leaf_postra_literal(v, numberVnum(&k), 0);
        if (op == BC_ADDNV || op == BC_SUBNV || op == BC_MULNV ||
            op == BC_DIVNV) { IRRef tmp = left; left = right; right = tmp; }
      }
      switch (op) {
      case BC_ADDVV: case BC_ADDVN: case BC_ADDNV: arith = IR_ADD; break;
      case BC_SUBVV: case BC_SUBVN: case BC_SUBNV: arith = IR_SUB; break;
      case BC_MULVV: case BC_MULVN: case BC_MULNV: arith = IR_MUL; break;
      default: arith = IR_DIV; break;
      }
      value = arm64_lua_leaf_postra_arith(v, arith, left, right, end);
      if (!value) return 0;
    } else {
      return 0;
    }
    if (!value || !arm64_cell_value_postra_ref(v, value, end)) return 0;
    state[a] = value;
    if (op == BC_CALL) {
      MSize firstarg = (MSize)a+1u+LJ_FR2;
      MSize argend = firstarg+bc_c(ins)-1u;
      for (i = firstarg; i < argend; i++) state[i] = 0;
    }
  }
  if (pc == pt->sizebc-1u) {
    memcpy(slots, state, sizeof(state));
    captured = 1;
  }
  if (!captured) return 0;
  {
    BCIns ret = (BCIns)la_load32_acq((const uint32_t *)&bc[pt->sizebc-1u]);
    if (bc_op(ret) == BC_CALLT) {
      const GCfunc *callee;
      const GCfunc *nested[LJ_ARM64_LUA_MAX_CHILD_FRAMES+1u];
      IRRef childargs[UINT8_MAX+1u], childslots[UINT8_MAX+1u];
      IRRef childresults[2] = {0, 0};
      MSize childnresults = 0;
      IRRef callguard = 0;
      BCReg a = bc_a(ret);
      if (bc_b(ret) != 0u || bc_c(ret) != 2u ||
          (MSize)a+bc_c(ret)+LJ_FR2 > pt->framesize || !state[a] ||
          !state[a+1u+LJ_FR2] ||
          depth >= LJ_ARM64_LUA_MAX_CHILD_FRAMES) return 0;
      callee = arm64_lua_leaf_postra_function(v, state[a], end, &callguard);
      if (!callee || !lj_trace_arm64_lua_leaf(callee) ||
          funcproto(callee)->numparams != 1u) return 0;
      for (i = 0; i < depth; i++) if (ancestors[i] == callee) return 0;
      memcpy(nested, ancestors, depth*sizeof(*nested));
      nested[depth] = callee;
      childargs[0] = state[a+1u+LJ_FR2];
      for (i = 1; i < UINT8_MAX+1u; i++) childargs[i] = REF_NIL;
      if (!arm64_cell_value_postra_ref(v, childargs[0], end) ||
          !arm64_lua_leaf_postra_eval_multi(v, funcproto(callee), childargs,
            funcproto(callee)->sizebc-1u, before, end, nested, depth+1u,
            childslots, childresults, &childnresults) || childnresults != 1u ||
          !arm64_cell_value_postra_ref(v, childresults[0], end) ||
          (callguard && callguard >= childresults[0])) return 0;
      state[a] = childresults[0];
      results[0] = childresults[0]; *nresultsp = 1u;
      return 1;
    }
    if (bc_op(ret) == BC_RET1 && bc_d(ret) == 2u &&
        bc_a(ret) < pt->framesize && state[bc_a(ret)]) {
      results[0] = state[bc_a(ret)]; *nresultsp = 1u;
      return 1;
    }
    if (bc_op(ret) == BC_RET && bc_d(ret) == 3u &&
        (MSize)bc_a(ret)+1u < pt->framesize && state[bc_a(ret)] &&
        state[bc_a(ret)+1u]) {
      results[0] = state[bc_a(ret)]; results[1] = state[bc_a(ret)+1u];
      *nresultsp = 2u;
      return 1;
    }
    return 0;
  }
}

static int arm64_lua_leaf_postra_eval(const LJArm64PostRAView *v,
                                      const GCproto *pt, const IRRef *args,
                                      MSize pc, IRRef before, IRRef end,
                                      const GCfunc **ancestors, MSize depth,
                                      IRRef *slots, IRRef *result)
{
  IRRef results[2] = {0, 0};
  MSize nresults = 0;
  if (!arm64_lua_leaf_postra_eval_multi(v, pt, args, pc, before, end,
      ancestors, depth, slots, results, &nresults) || nresults != 1u)
    return 0;
  *result = results[0];
  return 1;
}

static int arm64_lua_leaf_postra_prefix(const LJArm64PostRAView *v,
                                        const GCproto *pt, MSize resume,
                                        const IRRef *args,
                                        IRRef at, IRRef end, int unused,
                                        IRRef *saved, MSize *livep,
                                        IRRef *resultp, MSize *nresultp,
                                        MSize *retposp)
{
  IRRef values[UINT8_MAX+1u] = {0};
  const BCIns *bc = proto_bc(pt);
  MSize pc, extent = pt->numparams, vargdst = 0, vargcount = 0;
  MSize hotpath = UINT32_MAX;
  MSize retpos = pt->sizebc-1u;
  int found = 0;
  LJArm64LuaLeafBranch branch;
  BCIns ret = (BCIns)la_load32_acq((const uint32_t *)&bc[pt->sizebc-1u]);
  if (!arm64_lua_leaf_branch(pt, &branch)) return 0;
  if ((la_load8_acq(&pt->flags) & PROTO_VARARG) &&
      !arm64_lua_leaf_postra_varg_shape(pt, &vargdst, &vargcount)) return 0;
  if (vargdst+vargcount > extent) extent = vargdst+vargcount;
  if (extent) memcpy(values, args, extent*sizeof(IRRef));
  for (pc = 1; pc < pt->sizebc-1u; pc++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&bc[pc]);
    BCOp op = bc_op(ins);
    BCReg dst = bc_a(ins);
    IRRef out, lhs, rhs, limit = pc < resume ? at : end;
    IROp arithmetic = IR_NOP;
    int literal = 0, reverse = 0;
    TValue number;
    if ((resume == branch.first || resume == branch.second) &&
        pc == branch.compare) {
      memcpy(saved, values, sizeof(values)); *livep = extent;
      if (!arm64_lua_leaf_postra_branch_hot(v, pt, &branch, values, end,
                                             &hotpath)) return 0;
      if (branch.early_return) {
        retpos = resume == branch.first ? branch.first_ret : branch.second_ret;
        *retposp = retpos;
        resultp[0] = 0; resultp[1] = 0;
        *nresultp = 0;
        return 1;
      }
      if (branch.early_return)
        retpos = hotpath == branch.first ? branch.first_ret : branch.second_ret;
      found = 1;
      pc = hotpath-1u;
      continue;
    }
    if (branch.compare != UINT32_MAX && pc == branch.compare) {
      MSize hot;
      if (!arm64_lua_leaf_postra_branch_hot(v, pt, &branch, values, end, &hot))
        return 0;
      hotpath = hot;
      if (branch.early_return)
        retpos = hotpath == branch.first ? branch.first_ret : branch.second_ret;
      pc = hot-1u;
      continue;
    }
    switch (op) {
    case BC_ADDNV: reverse = 1; /* fallthrough */
    case BC_ADDVN: literal = 1; /* fallthrough */
    case BC_ADDVV: arithmetic = IR_ADD; break;
    case BC_SUBNV: reverse = 1; /* fallthrough */
    case BC_SUBVN: literal = 1; /* fallthrough */
    case BC_SUBVV: arithmetic = IR_SUB; break;
    case BC_MULNV: reverse = 1; /* fallthrough */
    case BC_MULVN: literal = 1; /* fallthrough */
    case BC_MULVV: arithmetic = IR_MUL; break;
    case BC_DIVNV: reverse = 1; /* fallthrough */
    case BC_DIVVN: literal = 1; /* fallthrough */
    case BC_DIVVV: arithmetic = IR_DIV; break;
    default: break;
    }
    if (pc == resume) {
      if (arithmetic == IR_NOP &&
          !(op == BC_CGET || op == BC_MOV || op == BC_KSHORT ||
            op == BC_KPRI || op == BC_KNUM || op == BC_VARG ||
            (branch.early_return && op == BC_RET1)) &&
          !((resume == branch.first || resume == branch.second) &&
            branch.compare != UINT32_MAX))
        return 0;
      memcpy(saved, values, sizeof(values)); *livep = extent;
    }
    if (branch.early_return && op == BC_RET1) {
      if (pc != retpos || bc_d(ins) != 2u) return 0;
      break;
    }
    if (op >= BC_ISLT && op <= BC_ISGT) continue;
    if (op == BC_JMP) {
      if (branch.compare == UINT32_MAX ||
          !((hotpath == branch.first && branch.first_path[pc]) ||
            (hotpath == branch.second && branch.second_path[pc]))) return 0;
      pc = branch.next[pc]-1u;
      continue;
    }
    if (op == BC_VARG) {
      if (!(la_load8_acq(&pt->flags) & PROTO_VARARG) || pc != 1u ||
          bc_a(ins) != vargdst || bc_b(ins) != vargcount+1u ||
          bc_c(ins) != 0u) return 0;
      continue;  /* args_at independently seeded this exact output window. */
    }
    if (arithmetic != IR_NOP) {
      lhs = values[bc_b(ins)];
      if (literal) {
        proto_knumtv_load_acq(&number, pt, bc_c(ins));
        rhs = arm64_lua_leaf_postra_literal(v, numberVnum(&number), 0);
      } else {
        rhs = values[bc_c(ins)];
      }
      out = lhs && rhs ? arm64_lua_leaf_postra_arith(v, arithmetic,
                                      reverse ? rhs : lhs,
                                      reverse ? lhs : rhs, limit) : 0;
    } else if (op == BC_CGET || op == BC_MOV || op == BC_CSET) {
      out = values[bc_d(ins)];
      if (op == BC_CSET) {
        IRRef old = values[dst];
        if ((old && !arm64_cell_value_postra_ref(v, old, limit)) ||
            (out && !arm64_cell_value_postra_ref(v, out, limit))) return 0;
      }
    } else if (op == BC_KSHORT) {
      out = arm64_lua_leaf_postra_literal(v, (int16_t)bc_d(ins), 0);
    } else if (op == BC_KPRI) {
      if (bc_d(ins) > 2u) return 0;
      out = (IRRef)(REF_NIL-bc_d(ins));
    } else if (op == BC_KNUM) {
      proto_knumtv_load_acq(&number, pt, bc_d(ins));
      out = arm64_lua_leaf_postra_literal(v, numberVnum(&number), 0);
    } else {
      return 0;
    }
    /* The independent argument walk proved all source types. Missing pure
    ** SSA outputs remain unknown, never a replacement for a saved value. */
    if (pc >= resume && out != at && arithmetic != IR_NOP &&
        ir_load_acq(&v->ir[at]).o == IR_CONV &&
        ir_load_acq(&v->ir[at]).op2 == IRCONV_NUM_INT &&
        arm64_lua_leaf_postra_num(v, lhs, limit) == at)
      found = 1;  /* The interpreter retries this arithmetic after conversion. */
    if (pc >= resume && out == at) {
      IRIns p = ir_load_acq(&v->ir[at]);
      found = (p.t.irt & ~IRT_ISPHI) == (IRT_INT|IRT_GUARD) ||
              (p.t.irt & ~IRT_ISPHI) == (IRT_NUM|IRT_GUARD);
    }
    values[dst] = out;
    if ((MSize)dst+1u > extent) extent = dst+1u;
    if (branch.compare != UINT32_MAX && pc < branch.join &&
        ((hotpath == branch.first && branch.first_path[pc]) ||
         (hotpath == branch.second && branch.second_path[pc])) &&
        branch.next[pc] > pc+1u)
      pc = branch.next[pc]-1u;
  }
  ret = (BCIns)la_load32_acq((const uint32_t *)&bc[retpos]);
  *retposp = retpos;
  *nresultp = arm64_lua_leaf_return_count(pt);
  if (!*nresultp || bc_a(ret)+*nresultp > pt->framesize) return 0;
  resultp[0] = values[bc_a(ret)];
  resultp[1] = *nresultp == 2u ? values[bc_a(ret)+1u] : 0;
  if (resume == retpos || (!branch.early_return && resume == pt->sizebc-1u)) {
    BCReg source = bc_a(ret);
    if ((bc_op(ret) != BC_RET1 && bc_op(ret) != BC_RET) ||
        source+*nresultp > extent ||
        !arm64_cell_value_postra_ref(v, resultp[0], at) ||
        (*nresultp == 2u &&
         !arm64_cell_value_postra_ref(v, resultp[1], at))) return 0;
    memcpy(saved, values, sizeof(values)); *livep = extent;
    return 1;
  }
  return (found || unused || *nresultp == 2u) &&
         arm64_cell_value_postra_ref(v, resultp[0], end) &&
         (*nresultp == 1u ||
          arm64_cell_value_postra_ref(v, resultp[1], end));
}

static int arm64_lua_leaf_postra_unused(const LJArm64PostRAView *v, MSize s)
{
  IRRef at, limit;
  if (s+1u >= v->nsnap) return 0;
  at = snap_ref_acq(&v->snap[s]);
  limit = snap_ref_acq(&v->snap[s+1u]);
  if (at < REF_FIRST || limit < at || limit >= v->nins) return 0;
  while (at < limit)
    if (ir_load_acq(&v->ir[at++]).o != IR_NOP) return 0;
  return 1;
}

/* The allocated pass independently scans forwards for reads before a new
** definition. Test operands before marking the destination, including aliased
** arithmetic and CSET's old destination tag. No recorder liveness is used. */
static int arm64_lua_leaf_postra_needed(const GCproto *pt, MSize resume,
                                        uint8_t *needed)
{
  uint8_t written[UINT8_MAX+1u] = {0};
  const uint8_t *path = NULL;
  MSize pc, terminal = pt->sizebc-1u;
  LJArm64LuaLeafBranch branch;
  if (!resume || resume >= pt->sizebc) return 0;
  if (!arm64_lua_leaf_branch(pt, &branch)) return 0;
  if (branch.compare != UINT32_MAX && resume < branch.compare) return 0;
  if (branch.early_return) {
    if (resume == branch.first || branch.first_path[resume]) {
      path = branch.first_path; terminal = branch.first_ret;
    } else if (resume == branch.second || branch.second_path[resume]) {
      path = branch.second_path; terminal = branch.second_ret;
    } else return 0;
  }
  if (branch.compare != UINT32_MAX && resume < branch.join) {
    if (path) { /* early-return path already selected */ }
    else if (resume == branch.first || branch.first_path[resume])
      path = branch.first_path;
    else if (resume == branch.second || branch.second_path[resume])
      path = branch.second_path;
    else return 0;
  }
  for (pc = resume; pc <= terminal; pc++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&proto_bc(pt)[pc]);
    BCOp op = bc_op(ins);
    MSize dst = bc_a(ins), left = bc_b(ins), right = bc_c(ins);
    MSize from = bc_d(ins);
    if (branch.compare != UINT32_MAX && pc < branch.join && !path[pc])
      continue;
    if (op == BC_JMP) {
      if (branch.compare == UINT32_MAX || pc >= branch.join ||
          branch.next[pc] <= pc || branch.next[pc] > branch.join) return 0;
      pc = branch.next[pc]-1u;
      continue;
    }
    if (dst >= pt->framesize) return 0;
    if (op == BC_RET1) {
      if (op != BC_RET1 || from != 2) return 0;
      if (!written[dst]) needed[dst] = 1;
      if (branch.early_return) return pc == terminal;
    } else if (op == BC_RET) {
      if (branch.early_return || from != 3u || dst+1u >= pt->framesize)
        return 0;
      if (!written[dst]) needed[dst] = 1;
      if (!written[dst+1u]) needed[dst+1u] = 1;
      return pc == terminal;
    } else if (op == BC_VARG) {
      MSize nresults = bc_b(ins) ? bc_b(ins)-1u : UINT32_MAX;
      MSize reg;
      if (!(la_load8_acq(&pt->flags) & PROTO_VARARG) || pc != 1u ||
          bc_a(ins) != 0u || bc_b(ins) < 2u || nresults == UINT32_MAX ||
          bc_c(ins) != 0u || dst+nresults > pt->framesize) return 0;
      for (reg = dst; reg < dst+nresults; reg++) written[reg] = 1;
    } else if (op == BC_CGET || op == BC_MOV || op == BC_CSET) {
      if (from >= pt->framesize) return 0;
      if (!written[from]) needed[from] = 1;
      if (op == BC_CSET && !written[dst]) needed[dst] = 1;
    } else if (op == BC_KSHORT || op == BC_KNUM) {
      /* The literal defines its destination without reading it. */
    } else if (op == BC_UGET) {
      if (from >= pt->sizeuv) return 0;
    } else if (op == BC_KPRI) {
      if (bc_d(ins) > 2u) return 0;
    } else if (op == BC_CALL) {
      MSize first = dst+1u+LJ_FR2;
      MSize end = first+bc_c(ins)-1u, arg;
      if ((bc_b(ins) != 2u && bc_b(ins) != 3u) || bc_c(ins) < 2u ||
          dst+bc_c(ins)+LJ_FR2 > pt->framesize) return 0;
      if (bc_b(ins) == 3u && dst+1u < pt->framesize)
        needed[dst+1u] = 0;
      if (!written[dst]) needed[dst] = 1;
      for (arg = first; arg < end; arg++)
        if (!written[arg]) needed[arg] = 1;
      if (bc_b(ins) == 3u) written[dst+1u] = 1;
    } else if (op == BC_CALLT) {
      MSize arg = dst+1u+LJ_FR2;
      if (pc+1u != pt->sizebc || bc_b(ins) != 0u ||
          bc_c(ins) != 2u || arg >= pt->framesize) return 0;
      if (!written[dst]) needed[dst] = 1;
      if (!written[arg]) needed[arg] = 1;
    } else {
      switch (op) {
      case BC_ADDVN: case BC_SUBVN: case BC_MULVN: case BC_DIVVN:
      case BC_ADDNV: case BC_SUBNV: case BC_MULNV: case BC_DIVNV:
        break;
      case BC_ADDVV: case BC_SUBVV: case BC_MULVV: case BC_DIVVV:
        if (right >= pt->framesize) return 0;
        if (!written[right]) needed[right] = 1;
        break;
      default:
        return 0;
      }
      if (left >= pt->framesize) return 0;
      if (!written[left]) needed[left] = 1;
    }
    written[dst] = 1;
  }
  return 1;
}

/* Reconstruct the caller continuation independently from allocated snapshots.
** Bind the child return through pure copies/arithmetic to the first root map;
** never borrow the semantic pass's reconstructed value or acceptance. */
static int arm64_lua_leaf_postra_result(const LJArm64PostRAView *v,
                                       MSize caller, MSize child,
                                       MSize resume, BCReg slot,
                                       const IRRef *results, MSize nresults,
                                       IRRef end,
                                       const GCproto *leaf, MSize childbase,
                                       IRRef function, IRRef frame,
                                       MSize leafret)
{
  const GCproto *pt = v->proto;
  IRRef state[UINT8_MAX+1u] = {0};
  uint8_t influenced[UINT8_MAX+1u] = {0}, mapped[UINT8_MAX+1u] = {0};
  uint8_t changed[UINT8_MAX+1u] = {0};
  MSize s, pc, last = 0, ofs = 0, count = 0, i, matches = 0;
  unsigned consumed = 0;
  IRRef before = 0;
  BCIns call;
  if (!pt || pt->framesize > UINT8_MAX || !nresults || nresults > 2u ||
      slot+nresults > pt->framesize || !results[0] ||
      (nresults == 2u && !results[1]))
    return 0;
  if (!resume || resume >= pt->sizebc) return 0;
  call = (BCIns)la_load32_acq((const uint32_t *)&v->proto_bc[resume-1u]);
  if (bc_op(call) != BC_CALL || bc_b(call) != nresults+1u ||
      bc_a(call) != slot || slot+bc_b(call)-1u+LJ_FR2 > pt->framesize)
    return 0;
  for (s = child+1u; s < v->nsnap; s++) {
    const SnapShot *sn = &v->snap[s];
    SnapEntry words[2];
    uint64_t footer;
    ofs = snap_mapofs_acq(sn); count = snap_nent_acq(sn);
    before = snap_ref_acq(sn);
    if (before >= end || ofs > v->nsnapmap || count > v->nsnapmap-ofs ||
        v->nsnapmap-ofs-count < 2u) return 0;
    words[0] = snapentry_acq(&v->snapmap[ofs+count]);
    words[1] = snapentry_acq(&v->snapmap[ofs+count+1u]);
    memcpy(&footer, words, sizeof(footer));
    if ((uint8_t)footer+2u == childbase &&
        (const BCIns *)(uintptr_t)(footer>>8) == proto_bc(leaf)+leafret) {
      unsigned identities = 0, frames = 0, returns = 0;
      BCIns ret = (BCIns)la_load32_acq(
        (const uint32_t *)&proto_bc(leaf)[leafret]);
      for (i = 0; i < count; i++) {
        SnapEntry entry = snapentry_acq(&v->snapmap[ofs+i]);
        MSize at = snap_slot(entry);
        if (at == childbase-2u)
          identities += !(entry & 0x00ff0000u) && snap_ref(entry) == function;
        else if (at == childbase-1u)
          frames += (entry & 0x00ff0000u) == SNAP_FRAME && snap_ref(entry) == frame;
        else if (at >= childbase+bc_a(ret) &&
                 at < childbase+bc_a(ret)+nresults)
          returns += !(entry & 0x00ff0000u) &&
                     snap_ref(entry) == results[at-childbase-bc_a(ret)];
      }
      if (identities != 1 || frames != 1 || returns != nresults) return 0;
      if (nresults == 1u && bc_op(v->startins) != BC_FUNCF) return 1;
    }
    if (!(uint8_t)footer) {
      if (!arm64_ir_pcpos((uintptr_t)(footer>>8), (uintptr_t)v->proto_bc,
          (uintptr_t)(v->proto_bc+v->proto_sizebc), &last) || last < resume)
        return 0;
      break;
    }
  }
  if (s >= v->nsnap) return 0;
  {
    const SnapShot *call = &v->snap[caller];
    MSize at = snap_mapofs_acq(call), n = snap_nent_acq(call);
    for (i = 0; i < n; i++) {
      SnapEntry entry = snapentry_acq(&v->snapmap[at+i]);
      MSize dst = snap_slot(entry);
      if (dst >= 1u+LJ_FR2 && dst-1u-LJ_FR2 < pt->framesize &&
          !(entry & (SNAP_FRAME|SNAP_CONT)))
        state[dst-1u-LJ_FR2] = snap_ref(entry);
    }
  }
  for (i = 0; i < nresults; i++) {
    state[slot+i] = results[i];
    influenced[slot+i] = (uint8_t)(1u << i);
    changed[slot+i] = 1;
  }
  for (pc = resume; pc < last; pc++) {
    BCIns ins = (BCIns)la_load32_acq((const uint32_t *)&v->proto_bc[pc]);
    BCReg dst = bc_a(ins), src = bc_d(ins);
    BCOp op = bc_op(ins);
    IROp arith = IR_NOP;
    int constant = 0, reverse = 0;
    IRRef out = 0, left, right;
    unsigned affected = 0;
    if (v->proto_bc+pc == v->startpc && pc+1u == last &&
        (op == BC_FORL || op == BC_JFORL))
      break;
    if (dst >= pt->framesize) return 0;
    switch (op) {
    case BC_ADDNV: reverse = 1; /* fallthrough */
    case BC_ADDVN: constant = 1; /* fallthrough */
    case BC_ADDVV: arith = IR_ADD; break;
    case BC_SUBNV: reverse = 1; /* fallthrough */
    case BC_SUBVN: constant = 1; /* fallthrough */
    case BC_SUBVV: arith = IR_SUB; break;
    case BC_MULNV: reverse = 1; /* fallthrough */
    case BC_MULVN: constant = 1; /* fallthrough */
    case BC_MULVV: arith = IR_MUL; break;
    case BC_DIVNV: reverse = 1; /* fallthrough */
    case BC_DIVVN: constant = 1; /* fallthrough */
    case BC_DIVVV: arith = IR_DIV; break;
    default: break;
    }
    if (arith != IR_NOP) {
      BCReg b = bc_b(ins), c = bc_c(ins);
      if (b >= pt->framesize) return 0;
      left = state[b]; affected = influenced[b];
      if (constant) {
        TValue k;
        if (c >= pt->sizekn) return 0;
        proto_knumtv_load_acq(&k, pt, c);
        if (!tvisnumber(&k)) return 0;
        right = arm64_lua_leaf_postra_literal(v, numberVnum(&k), 0);
      } else {
        if (c >= pt->framesize) return 0;
        right = state[c]; affected |= influenced[c];
      }
      out = left && right ? arm64_lua_leaf_postra_arith(v, arith,
          reverse ? right : left, reverse ? left : right, before) : 0;
    } else if (op == BC_CGET || op == BC_MOV || op == BC_CSET) {
      if (src >= pt->framesize) return 0;
      if (!state[src] && !changed[src]) {
        IRRef begin = snap_ref_acq(&v->snap[caller]);
        for (IRRef r = begin; r < before; r++) {
          IRIns load = ir_load_acq(&v->ir[r]);
          if (load.o == IR_SLOAD && load.op1 == src+1u+LJ_FR2 &&
              irt_isguard(load.t) &&
              arm64_cell_value_postra_ref(v, r, before)) {
            if (state[src]) return 0;
            state[src] = r;
          }
        }
      }
      out = state[src]; affected = influenced[src];
      if (out && !arm64_cell_value_postra_ref(v, out, before)) return 0;
      if (op == BC_CSET) {
        int raw = state[dst] &&
          arm64_cell_value_postra_ref(v, state[dst], before);
        for (IRRef r = REF_FIRST; !raw && !state[dst] && r < before; r++) {
          IRIns check = ir_load_acq(&v->ir[r]);
          if (check.o == IR_SLOAD && check.op1 == dst+1u+LJ_FR2 &&
              irt_isguard(check.t) &&
              (irt_ispri(check.t) || arm64_cell_value_postra_ref(v, r, before)))
            raw = 1;
        }
        if (!raw) return 0;
      }
    } else if (op == BC_KPRI) {
      if (src > 2u) return 0;
      out = (IRRef)(REF_NIL-src);
    } else if (op == BC_KSHORT) {
      out = arm64_lua_leaf_postra_literal(v, (int16_t)src, 0);
    } else if (op == BC_KNUM) {
      TValue k;
      if (src >= pt->sizekn) return 0;
      proto_knumtv_load_acq(&k, pt, src);
      if (!tvisnumber(&k)) return 0;
      out = arm64_lua_leaf_postra_literal(v, numberVnum(&k), 0);
    } else return 0;
    if (affected && !out) return 0;
    state[dst] = out; influenced[dst] = (uint8_t)affected; changed[dst] = 1;
  }
  for (i = 0; i < count; i++) {
    SnapEntry entry = snapentry_acq(&v->snapmap[ofs+i]);
    MSize dst = snap_slot(entry);
    if (dst < 1u+LJ_FR2 || dst-1u-LJ_FR2 >= pt->framesize) continue;
    dst -= 1u+LJ_FR2;
    if (influenced[dst]) {
      if ((entry & 0x00ff0000u) || mapped[dst] ||
          snap_ref(entry) != state[dst]) return 0;
      mapped[dst] = 1; matches++;
      consumed |= influenced[dst];
    }
  }
  for (i = 0; i < pt->framesize && i+1u+LJ_FR2 <
       snap_nslots_acq(&v->snap[s]); i++)
    if (influenced[i] && !mapped[i]) return 0;
  if (bc_op(v->startins) == BC_FUNCF) {
    BCIns ret;
    BCReg dst;
    unsigned bound = 0;
    if (last+1u != v->proto_sizebc) return 0;
    ret = (BCIns)la_load32_acq((const uint32_t *)&v->proto_bc[last]);
    dst = bc_a(ret);
    if (bc_op(ret) != BC_RET1 || bc_d(ret) != 2u ||
        dst >= pt->framesize || !influenced[dst] || !mapped[dst]) return 0;
    for (i = 0; i < count; i++) {
      SnapEntry entry = snapentry_acq(&v->snapmap[ofs+i]);
      if (snap_slot(entry) == dst+1u+LJ_FR2) {
        if ((entry & 0x00ff0000u) || snap_ref(entry) != state[dst]) return 0;
        bound++;
      }
    }
    if (bound != 1u) return 0;
  }
  return matches != 0 && consumed == ((1u << nresults)-1u);
}

/* Independently reconstruct the two-frame exit from its allocated footer,
** source prefix and caller guard. No semantic child-admission result is used. */
static int arm64_iterl_postra_result(const LJArm64PostRAView *v,
                                     MSize child, IRRef result, IRRef loop)
{
  LJArm64IteratorSource iter;
  const SnapShot *sn = NULL;
  MSize s, i, ofs, nent;
  IRRef key = 0, control = 0, r;
  SnapEntry words[2];
  uint64_t footer;
  if (!v->owner || !lj_asm_arm64_iterl_tuple(v->proto, v->startpc,
      v->startins, v->owner_private ? v->startins : BCINS_AD(BC_JITERL,
        bc_a(v->startins), trace_traceno_acq((GCtrace *)v->owner)),
      v->owner_private, &iter) ||
      !arm64_iterl_scalar_body(v->proto, &iter)) return 0;
  for (s = 0; s < v->nsnap; s++)
    if (snap_ref_acq(&v->snap[s]) == loop) {
      if (sn) return 0;
      sn = &v->snap[s];
    }
  if (!sn) return 0;
  ofs = snap_mapofs_acq(sn); nent = snap_nent_acq(sn);
  if (ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
      v->nsnapmap-ofs-nent < 2u) return 0;
  words[0] = snapentry_acq(&v->snapmap[ofs+nent]);
  words[1] = snapentry_acq(&v->snapmap[ofs+nent+1u]);
  memcpy(&footer, words, sizeof(footer));
  if ((uint8_t)footer || (const BCIns *)(uintptr_t)(footer>>8) !=
      v->proto_bc+iter.body_pc) return 0;
  for (i = 0; i < nent; i++) {
    SnapEntry e = snapentry_acq(&v->snapmap[ofs+i]);
    MSize slot = snap_slot(e);
    if (slot == iter.base+1u+LJ_FR2 || slot == iter.base+LJ_FR2) {
      if (e & 0x00ff0000u) return 0;
      if (slot == iter.base+LJ_FR2) control = snap_ref(e);
      else key = snap_ref(e);
    }
  }
  if (!key || control != key ||
      (!arm64_scalar_postra_ref(v, key, loop, IRT_INT) &&
       !arm64_scalar_postra_ref(v, key, loop, IRT_NUM))) return 0;
  if (snap_ref_acq(&v->snap[child]) < loop || result == key) return result == key;
  for (r = loop+1u; r < v->nins; r++) {
    IRIns phi = ir_load_acq(&v->ir[r]);
    if (phi.o == IR_PHI && phi.op1 == key && phi.op2 == result) return 1;
  }
  return 0;
}

static int arm64_iterl_postra_returns(const LJArm64PostRAView *v, IRRef loop)
{
  IRRef r, guard = 0, callee = 0, args[UINT8_MAX+1u];
  IRRef saved[UINT8_MAX+1u], result[2];
  MSize call = 0, loopmap = 0, s, phase, extent, count, retpos;
  const GCfunc *fn;
  for (r = REF_FIRST; r < loop; r++) {
    IRIns p = ir_load_acq(&v->ir[r]);
    if (p.o == IR_EQ && p.t.irt == (IRT_FUNC|IRT_GUARD)) {
      if (guard) return 0;
      guard = r; callee = p.op2;
    }
  }
  if (!guard) return 0;
  fn = arm64_lua_leaf_postra_function(v, callee, guard, NULL);
  if (!fn) return 0;
  for (s = 0; s < v->nsnap; s++) {
    if (snap_ref_acq(&v->snap[s]) == guard) call = s;
    if (snap_ref_acq(&v->snap[s]) == loop) loopmap = s;
  }
  if (!call || !loopmap) return 0;
  for (phase = 0; phase < 2u; phase++) {
    s = phase ? loopmap : call;
    if (!arm64_lua_leaf_postra_args(v, s, callee, args) ||
        !arm64_lua_leaf_postra_prefix(v, funcproto(fn), 1u, args,
          phase ? loop+1u : guard, phase ? v->nins : loop, 1,
          saved, &extent, result, &count, &retpos) || count != 1u ||
        !arm64_iterl_postra_result(v, s, result[0], loop)) return 0;
  }
  return 1;
}

static MSize arm64_lua_leaf_postra_child(const LJArm64PostRAView *v,
                                        MSize s, uint64_t pcbase,
                                        MSize *lowerlinkp)
{
  const SnapShot *sn = &v->snap[s];
  MSize base = (uint8_t)pcbase ? (uint8_t)pcbase+2u : 0,
        ofs = snap_mapofs_acq(sn);
  MSize count = snap_nent_acq(sn), i, pos, leafretpos, extent, live = 0;
  MSize caller = s;
  MSize callnargs = 0;
  IRRef at = snap_ref_acq(sn), function = 0, frame = 0, vargframe = 0;
  IRRef end = v->nins, loop = 0, result[2] = {0, 0};
  MSize nresults = 0;
  IRRef saved[UINT8_MAX+1u], args[UINT8_MAX+1u] = {0};
  uint8_t needed[UINT8_MAX+1u] = {0}, seen[UINT8_MAX+1u] = {0};
  const GCproto *pt;
  const GCfunc *fn;
  const BCIns *retpc;
  GCtrace *owner = (GCtrace *)v->owner;
  BCIns call;
  LJArm64LuaFramePath frames;
  LJArm64LuaLeafBranch branch;
  SnapEntry frame_map[UINT8_MAX+1u];
  IRIns link;
  int iterator = bc_op(v->startins) == BC_ITERL;
  LJArm64IteratorSource iter;
  int has_call = 0, inline_frame = (uint8_t)pcbase == 0, paired_varg = 0;
  if (lowerlinkp) *lowerlinkp = 0;
  if (!v->proto || !owner ||
      (!iterator && bc_op(v->startins) != BC_FORL &&
       !(bc_op(v->startins) == BC_FUNCF &&
         arm64_ir_funcf_leaf_call_source(v->proto))) ||
      proto_bc(v->proto) != v->proto_bc ||
      v->proto->sizebc != v->proto_sizebc ||
      v->proto->framesize != v->root_topslot ||
      trace_startptgco_acq(owner) != obj2gco((GCproto *)v->proto) ||
      trace_startins_acq(owner) != v->startins ||
      trace_startpc_acq(owner) != v->startpc) return 0;
  if (count > UINT8_MAX+1u) return 0;
  for (i = 0; i < count; i++)
    frame_map[i] = snapentry_acq(&v->snapmap[ofs+i]);
  if (inline_frame) {
    for (i = 0; i < count; i++) {
      SnapEntry entry = frame_map[i];
      if (entry & SNAP_FRAME) {
        if (frame || (entry & 0x00ff0000u) != SNAP_FRAME)
          return 0;
        frame = snap_ref(entry);
        base = snap_slot(entry)+1u;
      }
    }
    if (!frame || base < 2u+2u*LJ_FR2) return 0;
    for (i = 0; i < count; i++)
      if (snap_slot(frame_map[i]) == base-2u) {
        if (frame_map[i] & 0x00ff0000u) return 0;
        function = snap_ref(frame_map[i]);
      }
  } else {
    if (!arm64_lua_frame_path_limit(frame_map, count, pcbase, 2u, &frames) ||
        !frames.depth) return 0;
    paired_varg = frames.depth == 2u;
    base = frames.child[frames.depth-1u].baseslot;
    frame = frames.child[0].linkref;
    function = frames.child[frames.depth-1u].funcref;
    if (paired_varg) {
      vargframe = frames.child[1].linkref;
      if (lowerlinkp) *lowerlinkp = frames.child[0].linkslot;
      if (frames.child[0].funcref != frames.child[1].funcref)
        return 0;
    }
  }
  if (!irref_isk(frame) || frame+1u >= v->nins ||
      !arm64_scalar_postra_ref(v, frame, at, IRT_NUM))
    return 0;
  link = ir_load_acq(&v->ir[frame]);
  if (link.o != IR_KNUM) return 0;
  link = ir_load_acq(&v->ir[frame+1u]);
  retpc = (const BCIns *)(uintptr_t)link.tv.u64;
  if (!arm64_ir_pcpos((uintptr_t)retpc, (uintptr_t)v->proto_bc,
      (uintptr_t)(v->proto_bc+v->proto_sizebc), &pos) || !pos) return 0;
  call = (BCIns)la_load32_acq((const uint32_t *)(retpc-1));
  if (iterator && (paired_varg || inline_frame ||
      !lj_asm_arm64_iterl_tuple(v->proto, v->startpc, v->startins,
        v->owner_private ? v->startins :
          BCINS_AD(BC_JITERL, bc_a(v->startins), trace_traceno_acq(owner)),
        v->owner_private, &iter) || pos != iter.loop_pc ||
      call != iter.callins)) return 0;
  if ((!iterator && bc_op(call) != BC_CALL) || !bc_c(call) ||
      bc_a(call)+bc_c(call)+LJ_FR2 > v->root_topslot) return 0;
  callnargs = bc_c(call)-1u;
  if (inline_frame && !function) {
    SnapNo si;
    for (si = 0; si < s && !function; si++) {
      const SnapShot *prior = &v->snap[si];
      MSize j, priorofs = snap_mapofs_acq(prior), priorcount = snap_nent_acq(prior);
      for (j = 0; j < priorcount; j++) {
        SnapEntry entry = snapentry_acq(&v->snapmap[priorofs+j]);
        IRRef value = snap_ref(entry);
        IRIns candidate;
        if (snap_slot(entry) != base-2u || (entry & 0x00ff0000u) ||
            value >= v->nins) continue;
        candidate = ir_load_acq(&v->ir[value]);
        if (candidate.o == IR_KGC && candidate.t.irt == IRT_FUNC &&
            lj_trace_arm64_lua_leaf(gco2func(ir_kgc_load_acq(&v->ir[value]))))
          function = value;
      }
    }
  }
  fn = arm64_lua_leaf_postra_function(v, function, at, NULL);
  if (!lj_trace_arm64_lua_leaf(fn)) return 0;
  pt = funcproto(fn);
  if (paired_varg) {
    MSize delta = bc_c(call)-1u+1u+LJ_FR2;
    IRIns vlink;
    if (!(la_load8_acq(&pt->flags) & PROTO_VARARG) || !vargframe ||
        !irref_isk(vargframe) || vargframe+1u >= v->nins ||
        (vlink = ir_load_acq(&v->ir[vargframe])).o != IR_KNUM || !delta ||
        ir_load_acq(&v->ir[vargframe+1u]).tv.u64 !=
          8u*delta+FRAME_VARG ||
        base != 2u+2u*LJ_FR2+bc_a(call)+delta) return 0;
  } else if (base != 2u+2u*LJ_FR2+bc_a(call)) {
    return 0;
  }
  nresults = arm64_lua_leaf_return_count(pt);
  if (!nresults || bc_b(call) != nresults+1u ||
      bc_a(call)+bc_b(call)-1u+LJ_FR2 > v->root_topslot ||
      (inline_frame && bc_op(v->startins) != BC_FUNCF && nresults != 2u))
    return 0;
  for (IRRef r = REF_FIRST; r < v->nins; r++) {
    if (ir_load_acq(&v->ir[r]).o == IR_LOOP) {
      loop = r; if (at < r) end = r; break;
    }
  }
  if (!loop && bc_op(v->startins) != BC_FUNCF) return 0;
  while (caller) {
    const SnapShot *entry = &v->snap[--caller];
    MSize footerofs = snap_mapofs_acq(entry)+snap_nent_acq(entry);
    SnapEntry words[2];
    uint64_t footer;
    words[0] = snapentry_acq(&v->snapmap[footerofs]);
    words[1] = snapentry_acq(&v->snapmap[footerofs+1u]);
    memcpy(&footer, words, sizeof(footer));
    if (iterator && at > loop && snap_ref_acq(entry) != loop) continue;
    if (!(uint8_t)footer) {
      if (((!iterator || at < loop) &&
          ((const BCIns *)(uintptr_t)(footer>>8) != retpc-1 ||
           (loop && (snap_ref_acq(entry) > loop) != (at > loop)))) ||
          !arm64_lua_leaf_postra_args(v, caller, function, args)) return 0;
      break;
    }
  }
  if (!caller) return 0;
  if (paired_varg) {
    MSize sourcebase = 2u+2u*LJ_FR2+bc_a(call);
    for (i = 0; i < callnargs; i++) {
      MSize j, found = 0, slot = sourcebase+i;
      for (j = 0; j < count; j++) {
        SnapEntry entry = frame_map[j];
        if (snap_slot(entry) == slot) {
          if ((entry & 0x00ff0000u) || snap_ref(entry) != args[i])
            return 0;
          found++;
        }
      }
      if (found != 1u) return 0;
    }
  }
  if (!arm64_ir_pcpos((uintptr_t)(pcbase>>8), (uintptr_t)proto_bc(pt),
      (uintptr_t)(proto_bc(pt)+pt->sizebc), &pos) || !pos ||
      !arm64_lua_leaf_branch(pt, &branch)) return 0;
  if ((la_load8_acq(&pt->flags) & PROTO_VARARG) && pos <= 1u)
    return 0;
  for (i = 1; i+1u < pt->sizebc; i++)
    if (bc_op((BCIns)la_load32_acq(
        (const uint32_t *)&proto_bc(pt)[i])) == BC_CALL) has_call = 1;
  if (has_call) {
    const GCfunc *ancestors[1];
    if (nresults != 1u) return 0;
    ancestors[0] = fn;
    if (branch.compare != UINT32_MAX ||
        !arm64_lua_leaf_postra_eval(v, pt, args, pos, at, end, ancestors,
          1u, saved, &result[0])) return 0;
    live = pt->numparams;
    leafretpos = pt->sizebc-1u;
  } else if (!arm64_lua_leaf_postra_prefix(v, pt, pos, args, at, end,
        arm64_lua_leaf_postra_unused(v, s), saved, &live, result, &nresults,
        &leafretpos)) {
    return 0;
  }
  if ((pos == branch.first || pos == branch.second) &&
      !arm64_lua_leaf_postra_branch_guard(v, pt, &branch, saved, at, pos)) {
    return 0;
  }
  if (!arm64_lua_leaf_postra_needed(pt, pos, needed)) {
    return 0;
  }
  live = pt->numparams;
  for (i = pt->numparams; i < pt->framesize; i++)
    if (needed[i] && i+1u > live) live = i+1u;
  if (base+live > UINT8_MAX) return 0;
  if (branch.compare == UINT32_MAX) {
    if (snap_nslots_acq(sn) != base+live) return 0;
  } else if (snap_nslots_acq(sn) < base+live ||
             snap_nslots_acq(sn) > snap_topslot_acq(sn)+1u+LJ_FR2 ||
             snap_nslots_acq(sn) > base+pt->framesize) return 0;
  if (iterator) {
    if (!(branch.early_return && !nresults &&
          (pos == branch.first || pos == branch.second)) &&
        (nresults != 1u || !arm64_iterl_postra_result(v, s, result[0], loop)))
      return 0;
  } else if (pos == leafretpos && bc_op(v->startins) != BC_FUNCF) {
    BCIns continuation = (BCIns)la_load32_acq((const uint32_t *)retpc);
    IRIns check = ir_load_acq(&v->ir[at]);
    MSize local = bc_a(continuation), slot = local+1u+LJ_FR2;
    MSize nextpos, cpos = (MSize)(retpc-v->proto_bc), target, found = 0;
    MSize nofs, nent;
    SnapEntry words[2];
    uint64_t footer;
    if (bc_op(continuation) != BC_CSET ||
        bc_d(continuation) != bc_a(call) || local >= bc_a(call) ||
        check.o != IR_SLOAD || !irt_isguard(check.t) ||
        check.op1 != slot || check.op2 != IRSLOAD_TYPECHECK) return 0;
    if (!irt_ispri(check.t) &&
        !arm64_cell_value_postra_ref(v, at, at+1u) &&
        !(arm64_cell_guard_proto(v->proto, slot) &&
          arm64_cell_guard_postra(v, at))) return 0;
    if (s+1u >= v->nsnap || snap_ref_acq(&v->snap[s+1u]) <= at ||
        snap_ref_acq(&v->snap[s+1u]) >= end) return 0;
    nofs = snap_mapofs_acq(&v->snap[s+1u]);
    nent = snap_nent_acq(&v->snap[s+1u]);
    if (nofs > v->nsnapmap || nent > v->nsnapmap-nofs ||
        v->nsnapmap-nofs-nent < 2u) return 0;
    words[0] = snapentry_acq(&v->snapmap[nofs+nent]);
    words[1] = snapentry_acq(&v->snapmap[nofs+nent+1u]);
    memcpy(&footer, words, sizeof(footer));
    if ((uint8_t)footer || !arm64_ir_pcpos((uintptr_t)(footer>>8),
        (uintptr_t)v->proto_bc,
        (uintptr_t)(v->proto_bc+v->proto_sizebc), &nextpos)) return 0;
    target = slot;
    if (irt_type(check.t) == IRT_P32) {
      if (nextpos != cpos) return 0;
      target = bc_a(call)+1u+LJ_FR2;
    } else if (nextpos != cpos+1u &&
               !(cpos+1u < v->proto_sizebc && nextpos == cpos+2u &&
                 retpc+1 == v->startpc)) return 0;
    for (i = 0; i < nent; i++) {
      SnapEntry entry = snapentry_acq(&v->snapmap[nofs+i]);
      if (snap_slot(entry) >= target &&
          snap_slot(entry) < target+(nresults ? nresults : 1u)) {
        BCIns ret = (BCIns)la_load32_acq(
          (const uint32_t *)&proto_bc(pt)[pos]);
        if ((entry & 0x00ff0000u) ||
            snap_ref(entry) != saved[bc_a(ret)+(snap_slot(entry)-target)])
          return 0;
        found++;
      }
    }
    if (found != (nresults ? nresults : 1u)) return 0;
  } else if (!(branch.early_return &&
               (pos == branch.first || pos == branch.second)) &&
        !arm64_lua_leaf_postra_result(v, caller, s,
        (MSize)(retpc-v->proto_bc), bc_a(call), result, nresults, end,
        pt, base, function, frame, leafretpos)) return 0;
  extent = base-2u+pt->framesize;
  if (extent < v->root_topslot) extent = v->root_topslot;
  if (extent > UINT8_MAX || snap_topslot_acq(sn) != extent) return 0;
  for (i = 0; i < count; i++) {
    SnapEntry entry = snapentry_acq(&v->snapmap[ofs+i]);
    MSize slot = snap_slot(entry);
    if (slot < base) continue;
    if (slot-base >= pt->framesize || seen[slot-base] ||
        !saved[slot-base] ||
        (entry & 0x00ff0000u) || snap_ref(entry) != saved[slot-base])
      return 0;
    seen[slot-base] = 1;
  }
  for (i = 0; i < pt->framesize; i++)
    if (needed[i] && !seen[i]) return 0;
  return base-1u;

}

/* Independently replay a one-call FUNCF parent against the allocated trace.
** The rooted UGET helper, child frame and continuation are matched to their
** exact bytecodes before the child's result can feed parent arithmetic. */
static int arm64_postra_funcf_slot_unchanged(const GCproto *pt, MSize slot,
						     MSize pc)
{
	MSize p;
	for (p = 1; p < pc && p+1u < pt->sizebc; p++) {
	  BCIns ins = arm64_ir_bc_acq((uintptr_t)proto_bc(pt), p);
	  BCOp op = bc_op(ins);
	  if (bc_a(ins) == slot &&
	      (op == BC_CGET || op == BC_MOV || op == BC_CSET ||
	       op == BC_KSHORT || op == BC_KNUM || op == BC_KPRI ||
	       op == BC_UGET || op == BC_CALL ||
	       (op >= BC_ADDVN && op <= BC_DIVVV)))
	    return 0;
	}
	return 1;
}

static int arm64_postra_funcf_leaf_source(const LJArm64PostRAView *v,
						 IRRef semantic_nins)
{
	IRRef args[UINT8_MAX+1u] = {0};
	const GCfunc *ancestors[LJ_ARM64_LUA_MAX_CHILD_FRAMES+1u] = {0};
	MSize retpos, p, s;
	unsigned return_maps = 0;
	if (v && v->proto &&
	    arm64_ir_funcf_tail_one_result_source(v->proto, NULL))
	  return arm64_postra_funcf_tail_source_replay(v, semantic_nins);
	if (!v || !v->proto || bc_op(v->startins) != BC_FUNCF ||
	  (!arm64_ir_funcf_one_result_leaf_source(v->proto, &retpos) &&
	   !arm64_ir_funcf_pair_call_leaf_source(v->proto, &retpos)) ||
	  semantic_nins <= REF_FIRST || semantic_nins > v->nins ||
	  v->proto_numparams > UINT8_MAX || v->root_topslot > UINT8_MAX)
	  return 0;
	for (p = 0; p < v->proto_numparams; p++) {
	  IRRef ref, found = 0;
	  MSize slot = 1u+LJ_FR2+p;
	  for (ref = REF_FIRST; ref < semantic_nins; ref++) {
	    IRIns ins = ir_load_acq(&v->ir[ref]);
	    IRType type = irt_type(ins.t);
	    if (ins.o == IR_SLOAD && ins.op1 == slot &&
		ins.op2 == IRSLOAD_TYPECHECK &&
		(type == IRT_INT || type == IRT_NUM) &&
		arm64_ir_type_flags(ins.t, type, IRT_GUARD,
				    IRT_GUARD|IRT_ISPHI)) {
	      if (found) return 0;
	      found = ref;
	    }
	  }
	  if (!found) return 0;
	  args[p] = found;
	}
	for (s = 0; s < v->nsnap; s++) {
	  const SnapShot *snap = &v->snap[s];
	  MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap), i, pcpos;
	  uint64_t pcbase;
	  IRRef source[UINT8_MAX+1u] = {0}, result = 0;
	  uint8_t needed[UINT8_MAX+1u] = {0}, seen[UINT8_MAX+1u] = {0};
	  if (ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
	      v->nsnapmap-ofs-nent < 1u+LJ_FR2) return 0;
	  memcpy(&pcbase, &v->snapmap[ofs+nent], sizeof(pcbase));
	  if ((uint8_t)pcbase ||
	      !arm64_ir_pcpos((uintptr_t)(pcbase>>8),
		(uintptr_t)v->proto_bc,
		(uintptr_t)(v->proto_bc+v->proto_sizebc), &pcpos) ||
	      !arm64_lua_leaf_postra_eval(v, v->proto, args, pcpos,
		snap_ref_acq(snap), semantic_nins, ancestors, 0, source, &result) ||
	      !arm64_lua_leaf_postra_needed(v->proto, pcpos, needed)) return 0;
	  for (i = 0; i < nent; i++) {
	    SnapEntry entry = snapentry_acq(&v->snapmap[ofs+i]);
	    MSize slot = snap_slot(entry), local;
	    if (entry & (SNAP_FRAME|SNAP_NORESTORE)) return 0;
	    if (slot < 1u+LJ_FR2 ||
		slot-(1u+LJ_FR2) >= v->proto->framesize)
	      continue;
	    local = slot-(1u+LJ_FR2);
	    if (source[local] && snap_ref(entry) != source[local]) return 0;
	    if (needed[local]) {
	      if (!source[local] || seen[local]) return 0;
	      seen[local] = 1;
	    }
	  }
	  for (i = 0; i < v->proto->framesize; i++)
	    if (needed[i] && !seen[i] &&
		!(i < v->proto_numparams &&
		  arm64_postra_funcf_slot_unchanged(v->proto, i, pcpos))) return 0;
	  if (pcpos == retpos) {
	    BCIns ret = arm64_ir_bc_acq((uintptr_t)v->proto_bc, retpos);
	    unsigned found = 0;
	    for (i = 0; i < nent; i++) {
	      SnapEntry entry = snapentry_acq(&v->snapmap[ofs+i]);
	      if (snap_slot(entry) == 1u+LJ_FR2+bc_a(ret)) {
		if (snap_ref(entry) != result ||
		    !arm64_scalar_postra_ref(v, result, semantic_nins, IRT_NUM))
		  return 0;
		found++;
	      }
	    }
	    if (found != 1u) return 0;
	    return_maps++;
	  }
	}
	return return_maps == 1u;
}

/* Allocated counterpart to the terminal CALLT frame/result proof. This walk
** uses only immutable post-RA IR, snapshots and locations. */
static int arm64_postra_funcf_tail_source_replay(
						 const LJArm64PostRAView *v,
						 IRRef semantic_nins)
{
	IRRef args[UINT8_MAX+1u] = {0};
	const GCfunc *ancestors[1] = {0};
	MSize callpos, argslot, result_slot, p, s;
	unsigned call_maps = 0, result_maps = 0;
	BCIns call;
	if (!v || !v->proto || bc_op(v->startins) != BC_FUNCF ||
	    v->root_topslot > UINT8_MAX || v->proto->numparams != 1u ||
	    semantic_nins <= REF_FIRST || semantic_nins > v->nins ||
	    !arm64_ir_funcf_tail_one_result_source(v->proto, &callpos)) return 0;
	call = arm64_ir_bc_acq((uintptr_t)v->proto_bc, callpos);
	argslot = 1u+LJ_FR2+bc_a(call)+1u+LJ_FR2;
	result_slot = 1u+LJ_FR2+bc_a(call);
	for (p = 0; p < v->proto_numparams; p++) {
	  IRRef ref, found = 0;
	  MSize slot = 1u+LJ_FR2+p;
	  for (ref = REF_FIRST; ref < semantic_nins; ref++) {
	    IRIns ins = ir_load_acq(&v->ir[ref]);
	    IRType type = irt_type(ins.t);
	    if (ins.o == IR_SLOAD && ins.op1 == slot &&
		ins.op2 == IRSLOAD_TYPECHECK &&
		(type == IRT_INT || type == IRT_NUM) &&
		arm64_ir_type_flags(ins.t, type, IRT_GUARD,
				    IRT_GUARD|IRT_ISPHI)) {
	      if (found) return 0;
	      found = ref;
	    }
	  }
	  if (!found) return 0;
	  args[p] = found;
	}
	for (s = 0; s < v->nsnap; s++) {
	  const SnapShot *snap = &v->snap[s];
	  MSize ofs = snap_mapofs_acq(snap), nent = snap_nent_acq(snap),
		i, pcpos;
	  uint64_t pcbase;
	  IRRef source[UINT8_MAX+1u] = {0}, result = 0;
	  uint8_t needed[UINT8_MAX+1u] = {0}, seen[UINT8_MAX+1u] = {0};
	  int terminal;
	  if (ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
	      v->nsnapmap-ofs-nent < 1u+LJ_FR2) return 0;
	  memcpy(&pcbase, &v->snapmap[ofs+nent], sizeof(pcbase));
	  terminal = arm64_postra_funcf_tail_result_map(v, s, pcbase);
	  if ((uint8_t)pcbase) return 0;
	  if (terminal) pcpos = callpos;
	  else if (!arm64_ir_pcpos((uintptr_t)(pcbase>>8),
		(uintptr_t)v->proto_bc,
		(uintptr_t)(v->proto_bc+v->proto_sizebc), &pcpos)) return 0;
	  if (!arm64_lua_leaf_postra_eval(v, v->proto, args, pcpos,
		snap_ref_acq(snap), semantic_nins, ancestors, 0, source, &result))
	    return 0;
	  if (pcpos == 1u) {
	    needed[0] = 1;
	  } else if (pcpos == 2u) {
	    needed[0] = needed[bc_a(call)] = 1;
	  } else if (pcpos == callpos) {
	    needed[bc_a(call)] = needed[bc_a(call)+1u+LJ_FR2] = 1;
	  } else {
	    return 0;
	  }
	  if (terminal) {
	    unsigned marker = 0, output = 0, locals = 0;
	    if (snap_ref_acq(snap) != semantic_nins-1u || pcpos != callpos ||
		!arm64_scalar_postra_ref(v, result, snap_ref_acq(snap), IRT_NUM))
	      return 0;
	    for (i = 0; i < nent; i++) {
	      SnapEntry entry = snapentry_acq(&v->snapmap[ofs+i]);
	      MSize slot = snap_slot(entry);
	      if (entry == SNAP(1, SNAP_FRAME|SNAP_NORESTORE, REF_NIL)) {
		marker++;
		continue;
	      }
	      if (slot >= 1u+LJ_FR2 &&
		  slot-(1u+LJ_FR2) < v->proto->framesize) {
		locals++;
		if (slot == result_slot && snap_ref(entry) == result &&
		    !(entry & 0x00ff0000u)) output++;
	      }
	    }
	    if (marker != 1u || output != 1u || locals != 1u) return 0;
	    result_maps++;
	    continue;
	  }
	  if (pcpos == callpos) {
	    unsigned function_seen = 0, argument_seen = 0;
	    if (snap_ref_acq(snap) >= v->nins-1u) return 0;
	    for (i = 0; i < nent; i++) {
	      SnapEntry entry = snapentry_acq(&v->snapmap[ofs+i]);
	      MSize slot = snap_slot(entry), local;
	      if (entry & (SNAP_FRAME|SNAP_NORESTORE)) return 0;
	      if (slot < 1u+LJ_FR2 ||
		  slot-(1u+LJ_FR2) >= v->proto->framesize) continue;
	      local = slot-(1u+LJ_FR2);
	      if (source[local] && snap_ref(entry) != source[local]) return 0;
	      if (needed[local]) {
		if (!source[local] || seen[local]) return 0;
		seen[local] = 1;
	      }
	      if (slot == result_slot && source[local] &&
		  snap_ref(entry) == source[local]) function_seen++;
	      if (slot == argslot && source[local] &&
		  snap_ref(entry) == source[local]) argument_seen++;
	    }
	    if (function_seen != 1u || argument_seen != 1u) return 0;
	    for (i = 0; i < v->proto->framesize; i++)
	      if (needed[i] && !seen[i]) return 0;
	    call_maps++;
	  } else {
	    for (i = 0; i < nent; i++) {
	      SnapEntry entry = snapentry_acq(&v->snapmap[ofs+i]);
	      MSize slot = snap_slot(entry), local;
	      if (entry & (SNAP_FRAME|SNAP_NORESTORE)) return 0;
	      if (slot < 1u+LJ_FR2 ||
		  slot-(1u+LJ_FR2) >= v->proto->framesize) continue;
	      local = slot-(1u+LJ_FR2);
	      if (source[local] && snap_ref(entry) != source[local]) return 0;
	      if (needed[local]) {
		if (!source[local] || seen[local]) return 0;
		seen[local] = 1;
	      }
	    }
	    for (i = 0; i < v->proto->framesize; i++)
	      if (needed[i] && !seen[i] &&
		  !(i < v->proto_numparams &&
		    arm64_postra_funcf_slot_unchanged(v->proto, i, pcpos)))
		return 0;
	  }
	}
	return call_maps == 1u && result_maps == 1u;
}

/* LOOP may eliminate the callee identity guard, leaving the retained CALL
** map immediately before a child map at the same IR position. asm_snap_prev
** can only select the latter for native restoration. The former still owns
** the exact incoming argument values, but needs no physical exit locations. */
static int arm64_lua_leaf_postra_callmap(const LJArm64PostRAView *v, MSize s)
{
  const SnapShot *next;
  MSize ofs, nent, pos;
  IRRef at, end;
  SnapEntry words[2];
  uint64_t footer;
  if (s+1u >= v->nsnap) return 0;
  next = &v->snap[s+1u];
  at = snap_ref_acq(&v->snap[s]); end = snap_ref_acq(next);
  if (at < REF_FIRST || end < at || end >= v->nins) return 0;
  if (end > at) {
    /* An ignored computed actual may survive only in the backedge CALL map.
    ** Its interval can contain the unguarded integer loop increment. Neither
    ** ADD nor NOP has an exit, so this map needs no physical restore location. */
    ofs = snap_mapofs_acq(&v->snap[s]);
    nent = snap_nent_acq(&v->snap[s]);
    if (ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
        v->nsnapmap-ofs-nent < 2u) return 0;
    words[0] = snapentry_acq(&v->snapmap[ofs+nent]);
    words[1] = snapentry_acq(&v->snapmap[ofs+nent+1u]);
    memcpy(&footer, words, sizeof(footer));
    if ((uint8_t)footer || !arm64_ir_pcpos((uintptr_t)(footer>>8),
        (uintptr_t)v->proto_bc,
        (uintptr_t)(v->proto_bc+v->proto_sizebc), &pos)) return 0;
    {
      BCIns call = (BCIns)la_load32_acq((const uint32_t *)&v->proto_bc[pos]);
      if (bc_op(call) != BC_CALL || bc_b(call) != 2 || !bc_c(call) ||
          bc_a(call)+bc_c(call)+LJ_FR2 > v->root_topslot) return 0;
    }
    for (; at < end; at++) {
      IRIns op = ir_load_acq(&v->ir[at]);
      if (op.o != IR_NOP &&
          (op.o != IR_ADD || (op.t.irt & ~IRT_ISPHI) != IRT_INT)) return 0;
    }
    return 1;
  }
  ofs = snap_mapofs_acq(next); nent = snap_nent_acq(next);
  if (ofs > v->nsnapmap || nent > v->nsnapmap-ofs ||
      v->nsnapmap-ofs-nent < 2u) return 0;
  words[0] = snapentry_acq(&v->snapmap[ofs+nent]);
  words[1] = snapentry_acq(&v->snapmap[ofs+nent+1u]);
  memcpy(&footer, words, sizeof(footer));
  return (uint8_t)footer && arm64_lua_leaf_postra_child(v, s+1u, footer,
                                      NULL);
}

/* The parent locals and child arguments are live together at this exit.
** Distinct saved values cannot share a register or overlapping spill bytes. */
static int arm64_lua_leaf_postra_locations(const LJArm64PostRAView *v,
                                          IRRef n, MSize s)
{
  const SnapShot *snap = &v->snap[s];
  MSize ofs = snap_mapofs_acq(snap), count = snap_nent_acq(snap), i, j;
  IRRef values[UINT8_MAX];
  RegSP locations[UINT8_MAX];
  MSize widths[UINT8_MAX];
  if (count > UINT8_MAX) return 0;
  for (i = 0; i < count; i++) {
    SnapEntry entry = snapentry_acq(&v->snapmap[ofs+i]);
    IRRef r = snap_ref(entry);
    IRIns p;
    RegSP rs;
    values[i] = 0;
    if (irref_isk(r) || (entry & SNAP_NORESTORE)) continue;
    p = ir_load_acq(&v->ir[r]);
    rs = arm64_scalar_postra_snapshot_regsp(v, n, r, s);
    if (!ra_hasspill(regsp_spill(rs)) && ra_noreg(regsp_reg(rs)) &&
        p.o == IR_CONV && p.t.irt == IRT_NUM && p.op2 == IRCONV_NUM_INT) {
      r = p.op1;
      if (irref_isk(r)) continue;
      p = ir_load_acq(&v->ir[r]);
      rs = arm64_scalar_postra_snapshot_regsp(v, n, r, s);
    }
    values[i] = r; locations[i] = rs;
    widths[i] = irt_type(p.t) == IRT_INT ? 1u : 2u;
    for (j = 0; j < i; j++) {
      MSize a = regsp_spill(rs), b;
      if (!values[j] || values[j] == r) continue;
      b = regsp_spill(locations[j]);
      if (ra_hasspill(a) && ra_hasspill(b)) {
        if (a < b+widths[j] && b < a+widths[i]) return 0;
      } else if (!ra_hasspill(a) && !ra_hasspill(b) &&
                 regsp_reg(rs) == regsp_reg(locations[j])) {
        return 0;
      }
    }
  }
  return 1;
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
  unsigned nforl_side_add = 0;
  uintptr_t lo, hi;
  if (!v || !v->ir || !v->snap || !v->snapmap || !v->proto_bc ||
      v->nins <= REF_FIRST || v->nins >= REF_DROP || !v->nk ||
      v->nk > REF_TRUE || !v->nsnap || !v->nsnapmap || !v->root_topslot ||
      v->root_topslot > UINT8_MAX || v->base_delta ||
      (!side && (bc_op(v->startins) != rootop ||
       (rootop != BC_LOOP && rootop != BC_FORL && rootop != BC_ITERL &&
        !(rootop == BC_FUNCF &&
          arm64_ir_funcf_leaf_call_source(v->proto))))) || !v->proto_sizebc)
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
	    owner->traceno != side->traceno || owner->root != side->root ||
	    !arm64_side_scalar_topology(side, owner) || owner->sinktags ||
	    !arm64_side_immediate_parent_valid(side, owner) ||
	    owner->startins != BCINS_AD(BC_JMP, 0, 0) ||
	    trace_startpc_acq((GCtrace *)owner) != side->parent_continuation ||
	    side->parent == 0 ||
	    side->root == 0 ||
	  v->root_topslot != side->root_topslot ||
	  !side->parent_snap ||
	    side->loop_start_pc >= end || end >= side->proto_sizebc ||
	    header >= end)
	  return 0;
	h = (BCIns)la_load32_acq((const uint32_t *)&side->proto_bc[header]);
	back = (BCIns)la_load32_acq((const uint32_t *)&side->proto_bc[end]);
	if (bc_op(side->parent_startins) == BC_FORL) {
	  target = (int64_t)end+1+(int64_t)bc_j(side->parent_startins);
	  if (bc_op(h) != BC_FORI || bc_a(h) != bc_a(side->parent_startins) ||
	      header+1u != side->loop_start_pc ||
	      back != BCINS_AD(BC_JFORL, bc_a(side->parent_startins), side->root) ||
	      bc_j(h) <= 0 || (int64_t)header+1+(int64_t)bc_j(h) !=
		(int64_t)end+1 || target < 0 ||
	      (MSize)target != side->loop_start_pc ||
	      !arm64_ir_pcpos((uintptr_t)(const void *)side->parent_continuation,
		lo, hi, &cont) || cont < (MSize)target || cont >= end)
	    return 0;
	} else {
	  target = (int64_t)end + 1 + (int64_t)bc_j(back);
	  if (bc_op(h) != BC_JLOOP || bc_d(h) != side->root ||
	      bc_op(back) != BC_JMP || bc_j(back) >= 0 || target < 0 ||
	      (MSize)target != side->loop_start_pc ||
	      !arm64_ir_pcpos((uintptr_t)(const void *)side->parent_continuation,
		lo, hi, &cont) || cont < (MSize)target || cont >= end)
	    return 0;
	}
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
    if (k.o == IR_KKPTR && k.t.irt == IRT_PGC && k.op12 == 0 &&
	ref + LJ_GC64 < REF_TRUE &&
	arm64_postra_constant_unused(v, ref)) {
      ref += LJ_GC64; /* Dead folded-string residue, never an IR value. */
      continue;
    }
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
  if (rootop == BC_ITERL) {
    LJArm64IteratorSource iter;
    if (!v->owner || !v->proto || proto_bc(v->proto) != v->proto_bc ||
        v->proto->sizebc != v->proto_sizebc ||
        v->proto->framesize != v->root_topslot ||
        !lj_asm_arm64_iterl_tuple(v->proto, v->startpc, v->startins,
          v->owner_private ? v->startins : BCINS_AD(BC_JITERL,
            bc_a(v->startins), trace_traceno_acq((GCtrace *)v->owner)),
          v->owner_private, &iter)) return 0;
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
      if (side && (p.op2 == (IRSLOAD_PARENT|IRSLOAD_INHERIT) ||
	  p.op2 == (IRSLOAD_PARENT|IRSLOAD_READONLY|IRSLOAD_INHERIT))) {
	MSize ord = parent_sload++;
	if (!arm64_side_parent_sload_valid(side, &p, ord)) {

	  return 0;
	}
	break;
      }
      if (side && arm64_side_entry_sload_valid(side, &p))
	break;
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
        if ((p.t.irt & ~IRT_ISPHI) != (IRT_INT|IRT_GUARD) ||
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
    case IR_STRREF:
      if (p.t.irt != IRT_PGC ||
          !arm64_string_byte_bounds_postra(v, ref, p.op1, p.op2))
        return 0;
      break;
    case IR_XLOAD:
      if (p.t.irt != IRT_U8 || p.op2 != IRXLOAD_READONLY ||
          !arm64_string_byte_xload_postra(v, ref))
        return 0;
      break;
    case IR_TMPREF:
      if (p.t.irt != IRT_PGC || !ra_hasreg(p.r) ||
          !arm64_scalar_postra_register(p.r, IRT_PGC) || ra_hasspill(p.s))
        return 0;
      if (p.op2 == IRTMPREF_IN1 || p.op2 == (IRTMPREF_IN1|IRTMPREF_OUT1)) {
        if (!arm64_scalar_postra_ref(v, p.op1, ref, IRT_TAB) &&
            !arm64_upvalue_fnroot_postra(v, ref, ref+1u,
              p.op2 == IRTMPREF_IN1 ? IRTMPREF_IN1 :
              (IRTMPREF_IN1|IRTMPREF_OUT1)) &&
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
      if (type == IRT_INT && side &&
          arm64_side_forl_index_add_postra(side, v, ref, p, n)) {
        if (++nforl_side_add != 1u) return 0;
        binary = 1;
        break;
      }
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
    case IR_FPMATH:
      if ((p.t.irt & ~IRT_ISPHI) != IRT_NUM ||
          !arm64_scalar_fpmath(p.op2) ||
          !arm64_scalar_postra_ref(v, p.op1, ref, IRT_NUM))
        return 0;
      break;
    case IR_CALLN:
      if (side || (p.t.irt & ~IRT_ISPHI) != IRT_NUM ||
          !arm64_scalar_math_call(p.op2) ||
          !arm64_scalar_postra_ref(v, p.op1, ref, IRT_NUM))
        return 0;
      break;
    case IR_FLOAD:
      if (arm64_scalar_ksimd_postra_fload(p, 0) ||
          arm64_scalar_ksimd_postra_fload(p, 1)) {
        if (p.r != RID_INIT || ra_hasspill(p.s) || p.s != 0)
          return 0;
      } else if (!(p.t.irt == IRT_INT && p.op2 == IRFL_STR_LEN &&
                   arm64_scalar_postra_ref(v, p.op1, ref, IRT_STR))) {
        return 0;
      }
      break;
    case IR_ABS:
      if (p.t.irt != IRT_NUM ||
          !arm64_scalar_postra_ref(v, p.op1, ref, IRT_NUM) ||
          !arm64_scalar_ksimd_postra_ref(v, p.op2, ref, 0))
        return 0;
      break;
    case IR_MIN: case IR_MAX:
      if ((type != IRT_INT && type != IRT_NUM) || flags)
        return 0;
      binary = 1;
      break;
    case IR_BNOT: case IR_BSWAP:
      if (type != IRT_INT || (flags & ~IRT_ISPHI) || p.op2 ||
          !arm64_scalar_postra_ref(v, p.op1, ref, IRT_INT))
        return 0;
      break;
    case IR_NEG:
      if (type == IRT_NUM && (flags & ~IRT_ISPHI) == 0 &&
          arm64_scalar_postra_ref(v, p.op1, ref, IRT_NUM) &&
          arm64_scalar_ksimd_postra_ref(v, p.op2, ref, 1))
        break;
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
    case IR_ULT:
    case IR_UGE:
    case IR_ULE:
    case IR_UGT:
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
            (!arm64_scalar_builtin_ffid(lj_func_ffid_acq(gco2func(fn))) &&
            (side || (rootop != BC_FORL && rootop != BC_ITERL && rootop != BC_FUNCF) ||
              !lj_trace_arm64_lua_leaf(gco2func(fn)))))
          return 0;
        break;
      }
      if ((type != IRT_INT && type != IRT_NUM) || flags != IRT_GUARD ||
          ((p.o >= IR_ULT && p.o <= IR_UGT) && type != IRT_NUM &&
           !arm64_string_byte_guard_postra(v, ref)))
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
      if (type == IRT_INT && flags == 0 &&
          p.op2 == (IRCONV_INT_NUM | IRCONV_ANY) &&
          arm64_scalar_postra_ref(v, p.op1, ref, IRT_NUM) &&
          arm64_string_byte_dynamic_index_postra(v, ref))
        break;
      return 0;
    case IR_LOOP:
      if (loop || p.t.irt != (IRT_NIL | IRT_GUARD) || p.op12)
        return 0;
      loop = ref;
      break;
    case IR_XPOLL:
	  if (poll || (rootop == BC_FUNCF ? loop != 0 : side ?
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
        MSize width = type == IRT_INT || type == IRT_U8 ? 1 : 2, slot;
        /* Preserve the complete 64-bit NUM, GC pointer or lightuserdata token.
        ** GC pointers additionally rely on the native phase lease until exit
        ** restoration; lightuserdata tokens carry no GC edge. */
        if (!value || (type != IRT_INT && type != IRT_U8 && type != IRT_NUM &&
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
  if (rootop == BC_FUNCF ? (loop != 0 || poll == 0) : side ?
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
          slots = snap_nslots_acq(snap), j, pos, childlink = 0,
          lowerlink = 0;
    MSize next =
        s + 1 < v->nsnap ? snap_mapofs_acq(&v->snap[s + 1]) : v->nsnapmap;
    IRRef at = snap_ref_acq(snap);
    uint64_t pcbase;
    SnapEntry footer[1 + LJ_FR2];
    int map_only = 0, has_frame = 0, tail_result_map = 0;
    if (at < REF_FIRST || at >= n || at < prevsnap || ofs != expectedofs ||
        ofs > v->nsnapmap || next > v->nsnapmap || next < ofs ||
        nent > next - ofs || next - ofs - nent != 1 + LJ_FR2 ||
        slots < 1 + LJ_FR2)
      return 0;
    for (j = 0; j < 1 + LJ_FR2; j++)
      footer[j] = snapentry_acq(&v->snapmap[ofs+nent+j]);
    memcpy(&pcbase, footer, sizeof(pcbase));
    for (j = 0; j < nent; j++)
      has_frame |= !!(snapentry_acq(&v->snapmap[ofs+j]) & SNAP_FRAME);
    if ((uint8_t)pcbase) {
      if (side || !(childlink = arm64_lua_leaf_postra_child(v, s, pcbase,
                                  &lowerlink))) {
        return 0;
      }
      map_only = arm64_lua_leaf_postra_unused(v, s);
    } else if (!arm64_ir_pcpos((uintptr_t)(pcbase >> 8), lo, hi, &pos)) {
      tail_result_map = !side && has_frame &&
          arm64_postra_funcf_tail_result_map(v, s, pcbase);
      if (!tail_result_map && (side || !has_frame ||
          !(childlink = arm64_lua_leaf_postra_child(v, s, pcbase,
                                  &lowerlink))))
        return 0;
      map_only = 0;
    } else if (slots > v->root_topslot+1u+LJ_FR2 ||
               snap_topslot_acq(snap) != v->root_topslot) {
      return 0;
    } else if (!side) {
      map_only = arm64_lua_leaf_postra_callmap(v, s);
    }
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
      if (childlink && slot == childlink &&
          (sn & 0x00ff0000u) == SNAP_FRAME) continue;
      if (lowerlink && slot == lowerlink &&
          (sn & 0x00ff0000u) == SNAP_FRAME) continue;
      if (sn == SNAP(1, SNAP_FRAME | SNAP_NORESTORE, REF_NIL))
        continue;
      if (tail_result_map && slot == 0 && !(sn & 0x00ff0000u) &&
	  irref_isk(r) && r >= v->nk && r < REF_TRUE &&
	  ir_load_acq(&v->ir[r]).o == IR_KGC &&
	  isluafunc(gco2func(ir_kgc_load_acq(&v->ir[r])))) continue;
      if (r >= REF_FIRST && r < at) {
        IRIns value = ir_load_acq(&v->ir[r]);
        /* U8 has no direct Lua stack snapshot encoding in this bounded slice.
        ** It is admitted only as an internal zero-extended arithmetic value. */
        if (value.o == IR_XLOAD && value.t.irt == IRT_U8)
          return 0;
      }
      if ((sn & 0x00ff0000u) == SNAP_NORESTORE) {
        MSize idxslot = 1u + LJ_FR2 + bc_a(v->startins);
        IRIns source;
        if (side) {
          if (!arm64_side_forl_snapshot_stop_postra(side, v, at, sn))
	    return 0;
        } else {
          if (rootop != BC_FORL || irref_isk(r) || r < REF_FIRST || r >= at ||
	      (slot != idxslot && slot != idxslot+FORL_STOP &&
	       (slot != idxslot+FORL_STEP || !forl_dynamic_step)))
	    return 0;
          source = ir_load_acq(&v->ir[r]);
          if (source.o != IR_SLOAD || source.op1 != slot ||
	      (source.op2 & (IRSLOAD_CONVERT|IRSLOAD_PARENT)) != 0)
	    return 0;
        }
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
      if (irref_isk(r) || map_only)
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
    if (childlink && !map_only &&
        !arm64_lua_leaf_postra_locations(v, n, s)) return 0;
    if (!tail_result_map && !childlink &&
        ((uint8_t)pcbase != v->base_delta ||
        !arm64_ir_pcpos((uintptr_t)(pcbase >> 8), lo, hi, &pos))) {
      if (side || (uint8_t)pcbase != v->base_delta || !has_frame ||
          !(childlink = arm64_lua_leaf_postra_child(v, s, pcbase,
                                  &lowerlink)))
        return 0;
      map_only = 0;
    }
	if (side && ((s == 0 && pos != continuation_pos) ||
		(pos < side->loop_start_pc ||
		 (pos >= side->loop_end_pc &&
		  !(s != 0 && (pos == side->loop_end_pc+1u ||
                    (pos == side->loop_end_pc &&
                     arm64_side_forl_boundary_snapshot_postra(side, v, snap))))))))
	  return 0;
  }
  if (rootop == BC_FUNCF ? (loopsnap || poll_snap != poll) : side ?
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
    if (ir.o == IR_EQ && ir.t.irt == (IRT_FUNC|IRT_GUARD) &&
        isluafunc(gco2func(ir_kgc_load_acq(&v->ir[ir.op2]))) &&
        !arm64_lua_leaf_postra_snapshot(v, ref, s) &&
        !arm64_postra_funcf_tail_guard_snapshot(v, ref, s))
      return 0;
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
    if ((ir.o == IR_CALLN || (ir.o == IR_FPMATH && ir.op2 >= IRFPM_LOG)) &&
        !arm64_scalar_math_live_across(v, n, loop, ref, cap))
      return 0;
  }
  if (rootop == BC_ITERL && !arm64_iterl_postra_returns(v, loop)) return 0;
  if (ninsout)
    *ninsout = n;
  return 1;
}
static int arm64_side_parent_forl_norestore_valid(
	const LJArm64SideIRView *side, MSize i, SnapEntry sn)
{
	MSize idxslot, slot, control;
	IRRef ref;
	const IRIns *source, *root_source;
	if (!side || bc_op(side->parent_startins) != BC_FORL ||
	    i >= side->parent_nmap_values ||
	    (sn & 0x00ff0000u) != SNAP_NORESTORE)
	  return 0;
	slot = snap_slot(sn);
	idxslot = 1u+LJ_FR2+(MSize)bc_a(side->parent_startins);
	if (slot != idxslot && slot != idxslot+FORL_STOP &&
	    slot != idxslot+FORL_STEP)
	  return 0;
	ref = snap_ref(sn);
	if (side->parent_map_values[i].slot != slot ||
	    side->parent_map_values[i].ref != ref || irref_isk(ref))
	  return 0;
	source = &side->parent_map_values[i].value;
	if (source->o != IR_SLOAD || source->op1 != slot ||
	    (source->op2 & IRSLOAD_CONVERT) != 0)
	  return 0;
	control = slot == idxslot ? 0 :
	  slot == idxslot+FORL_STOP ? 1 : 2;
	if (side->parent != side->root) {
	  if ((source->op2 & IRSLOAD_PARENT) == 0 ||
	      (side->parent_root_forl_mask & (1u<<control)) == 0)
	    return 0;
	  root_source = &side->parent_root_forl[control];
	  return root_source->o == IR_SLOAD && root_source->op1 == slot &&
	    source->t.irt == root_source->t.irt &&
	    source->op2 == (root_source->op2|IRSLOAD_PARENT) &&
	    (control != 0 ||
	     (source->t.irt & ~IRT_ISPHI) == (IRT_INT|IRT_GUARD));
	}
	if ((source->op2 & IRSLOAD_PARENT) != 0) return 0;
	if (slot == idxslot)
	  return (source->t.irt & ~IRT_ISPHI) == (IRT_INT|IRT_GUARD) &&
	    source->op2 == (IRSLOAD_TYPECHECK|IRSLOAD_INHERIT);
	return source->t.irt == IRT_INT &&
	  source->op2 == (IRSLOAD_READONLY|IRSLOAD_INHERIT);
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
	     (sn & 0x00ff0000u) &&
	     !arm64_side_parent_forl_norestore_valid(side, i, sn)))
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
	if (side->link == side->root && side->root != 0 &&
	    side->linktype == LJ_TRLINK_ROOT &&
	    owner->root == side->root && owner->link == side->root &&
	    owner->linktype == LJ_TRLINK_ROOT && owner->mcloop == 0) {
	  if (bc_op(side->parent_startins) == BC_LOOP)
	    return 1;
	  lo = (uintptr_t)(const void *)side->proto_bc;
	  return lo != 0 &&
	    side->proto_sizebc <= (UINTPTR_MAX-lo)/sizeof(BCIns) &&
	    arm64_scalar_side_geometry_ir(side, lo,
	      lo+side->proto_sizebc*sizeof(BCIns), &continuation_pos);
	}
	lo = (uintptr_t)(const void *)side->proto_bc;
	if (side->link != side->traceno || side->linktype != LJ_TRLINK_LOOP ||
	    owner->link != side->traceno || owner->linktype != LJ_TRLINK_LOOP || !lo ||
	    side->proto_sizebc > (UINTPTR_MAX-lo)/sizeof(BCIns))
	  return 0;
	return arm64_scalar_side_geometry_ir(side, lo,
	    lo+side->proto_sizebc*sizeof(BCIns), &continuation_pos);
}

/* Nested side-of-side admission remains root-owned. Authenticate both the
** child's IR_BASE immediate-parent exit and the copied parent's root edge. */
static int arm64_side_immediate_parent_valid(
	const LJArm64SideIRView *side, const GCtrace *owner)
{
	IRIns base;
	if (!side || !owner || side->parent == 0 || side->root == 0 ||
	    owner->root != side->root || side->parent == side->traceno)
	  return 0;
	if (side->parent == side->root)
	  return 1;
	base = ir_load_acq(&side->ir[REF_BASE]);
	return base.o == IR_BASE && base.t.irt == IRT_PGC &&
	  (TraceNo)base.op1 == side->parent && (ExitNo)base.op2 == side->exitno &&
	  side->parent_base.o == IR_BASE &&
	  side->parent_base.t.irt == IRT_PGC &&
	  (TraceNo)side->parent_base.op1 == side->root &&
	  (MSize)side->parent_base.op2 < side->parent_root_nsnap &&
	  side->parent_root == side->root && side->parent_link == side->root &&
	  side->parent_linktype == LJ_TRLINK_ROOT;
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
	side->parent != side->traceno && side->root != 0 &&
	T->root == side->root &&
	arm64_side_immediate_parent_valid(side, T) &&
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
	  owner->traceno != side->traceno || owner->root != side->root ||
	  owner->sinktags || owner->startins != BCINS_AD(BC_JMP, 0, 0) ||
	  trace_startpc_acq((GCtrace *)owner) != side->parent_continuation ||
	  v->proto_bc != side->proto_bc || v->proto_sizebc != side->proto_sizebc ||
	  v->root_topslot != side->root_topslot ||
	  v->snap != side->snap || v->snapmap != side->snapmap ||
	  v->nsnap != side->nsnap || v->nsnapmap != side->nsnapmap ||
	  !arm64_side_parent_snapshot_valid(side) ||
	  !arm64_side_scalar_topology(side, owner) ||
	  !arm64_side_immediate_parent_valid(side, owner))
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
  if (T && bc_op(T->startins) == BC_ITERL)
    return arm64_scalar_ir_admit_mode(J, T, BC_ITERL, NULL, reject);
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
  if (view && bc_op(view->startins) == BC_ITERL)
    return arm64_scalar_postra_admit_mode(view, BC_ITERL, NULL, semantic_ninsp);
  return arm64_postra_nonloop_admit(view, semantic_ninsp);
}

#endif
