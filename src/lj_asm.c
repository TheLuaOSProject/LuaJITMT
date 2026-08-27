/*
** IR assembler (SSA IR -> machine code).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_asm_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT

#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#endif
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_ircall.h"
#include "lj_iropt.h"
#include "lj_mcode.h"
#include "lj_trace.h"
#include "lj_snap.h"
#include "lj_asm.h"
#include "lj_dispatch.h"
#if LJ_HASPROFILE
#include "lj_profile.h"
#endif
#include "lj_vm.h"
#include "lj_target.h"
#include "lj_prng.h"

#ifdef LUA_USE_ASSERT
#include <stdio.h>
#endif

#if LJ_TARGET_ARM64
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
** self-linked integer BC_LOOP roots, separately certified constant-step
** integer BC_FORL roots, and the exact literal-true fixed-function root.
** In particular, this list admits no IR CALL helper ID and no floating-point
** operation at all.
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
	MSize maxslots)
{
  if (ir.op1 < 1u+LJ_FR2 || ir.op1 >= maxslots)
    return 0;
  if (rootop == BC_FORL) {
    if (ir.op1 == forl_idxslot)
      return arm64_ir_type_flags(ir.t, IRT_INT, IRT_GUARD,
				     IRT_GUARD|IRT_ISPHI) &&
		     ir.op2 == (IRSLOAD_TYPECHECK|IRSLOAD_INHERIT);
    if (ir.op1 == forl_idxslot+FORL_STOP)
      return ir.t.irt == IRT_INT &&
		     ir.op2 == (IRSLOAD_READONLY|IRSLOAD_INHERIT);
    if (ir.op1 == forl_idxslot+FORL_STEP ||
	ir.op1 == forl_idxslot+FORL_EXT)
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
    return arm64_ir_sload_layout(ir, rootop, forl_idxslot, maxslots);
  if (ir.o == IR_ADD)
    return arm64_ir_type_flags(ir.t, IRT_INT, IRT_ISPHI, IRT_ISPHI);
  return arm64_ir_type_flags(ir.t, IRT_INT, IRT_GUARD,
			     IRT_GUARD|IRT_ISPHI);
}

static int arm64_postra_spill_slot(MSize slot, MSize capacity)
{
  return slot >= SPS_FIRST && slot < capacity && slot < SPS_LIMIT;
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
  MSize maxslots, forl_idxslot = 0;
  uintptr_t proto_lo, proto_hi, proto_bytes;
  BCOp rootop;
  unsigned nadd = 0;
  int suffix_is_nop = 0;

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
  rootop = bc_op(view->startins);
  if (rootop != BC_LOOP && rootop != BC_FORL && rootop != BC_FUNCF)
    return 0;
  if (rootop == BC_FUNCF)
    return arm64_postra_funcf_admit(view, view->startins, semantic_ninsp);
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
      MSize nrename = 0;
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
      nadd++;
      if (!arm64_postra_int_value(ins, rootop, forl_idxslot, maxslots))
	return 0;
      break;
    case IR_LT: case IR_GE: case IR_LE: case IR_GT:
    case IR_EQ: case IR_NE:
      if (!arm64_ir_type_flags(ins.t, IRT_INT, IRT_GUARD, IRT_GUARD) ||
	  slot != SPS_NONE)
	return 0;
      break;
    case IR_LOOP: case IR_XPOLL:
      if (!arm64_ir_type_flags(ins.t, IRT_NIL, IRT_GUARD, IRT_GUARD) ||
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

  if ((rootop == BC_FORL && nadd != 2u) ||
      (rootop == BC_LOOP && nadd != 0u))
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
	  ren.op2 >= nsnap || ren.r >= RID_MAX_GPR ||
	  !rset_test(RSET_GPR, ren.r) || ren.s != SPS_NONE)
	return 0;
      source = ir_load_acq(&ir[ren.op1]);
      if (!arm64_postra_int_value(source, rootop, forl_idxslot, maxslots))
	return 0;
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
	  source = ir_load_acq(&ir[valueref]);
	  if (source.o != IR_KINT || source.t.irt != IRT_INT)
	    return 0;
	  continue;
	}
	if (valueref < REF_FIRST || valueref >= snapat)
	  return 0;
	source = ir_load_acq(&ir[valueref]);
	if (!arm64_postra_int_value(source, rootop, forl_idxslot, maxslots) ||
	    (flags == SNAP_NORESTORE &&
	     (source.o != IR_SLOAD || source.op1 != slot ||
	      rootop != BC_FORL ||
	      (slot != forl_idxslot && slot != forl_idxslot+FORL_STOP))))
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
	} else if (regsp_reg(rs) >= RID_MAX_GPR ||
		   !rset_test(RSET_GPR, regsp_reg(rs))) {
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
  ARM64_SIDE_K_ONE = REF_TRUE-1u,
  ARM64_SIDE_R_PARENT = REF_BASE+1u,
  ARM64_SIDE_R_VALUE = REF_BASE+2u,
  ARM64_SIDE_R_ADD = REF_BASE+3u,
  ARM64_SIDE_R_LIMIT = REF_BASE+4u,
  ARM64_SIDE_R_GT = REF_BASE+5u,
  ARM64_SIDE_R_XPOLL = REF_BASE+6u,
  ARM64_SIDE_SEMANTIC_NINS = REF_BASE+7u
};

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
  static const IRRef snaprefs[4] = {
    ARM64_SIDE_R_VALUE, ARM64_SIDE_R_LIMIT,
    ARM64_SIDE_R_GT, ARM64_SIDE_R_XPOLL
  };
  static const MSize mapofs[4] = { 0, 3, 7, 10 };
  static const uint8_t nent[4] = { 1, 2, 1, 1 };
  static const uint8_t nslots[4] = { 5, 6, 5, 5 };
  static const MSize pcpos[4] = { 13, 3, 17, 7 };
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
  proto = (uintptr_t)(const void *)view->proto_bc;
  if ((proto & (sizeof(BCIns)-1u)) != 0 || view->proto_sizebc != 19u ||
	(uintptr_t)view->proto_sizebc >
	  (UINTPTR_MAX-proto)/sizeof(BCIns) ||
	view->nins != ARM64_SIDE_SEMANTIC_NINS ||
	view->nk != ARM64_SIDE_K_ONE || view->nsnap != 4u ||
	view->nsnapmap != 13u || view->baseslot != 1u+LJ_FR2 ||
	view->root_topslot != 5u || view->traceno == 0 ||
	view->traceno > UINT16_MAX || view->parent == 0 ||
	view->parent > UINT16_MAX || view->traceno == view->parent ||
	view->root != view->parent ||
	view->link != view->parent || view->exitno != 2u ||
	view->startins != BCINS_AD(BC_JMP, 0, 0) ||
	view->linktype != LJ_TRLINK_ROOT || view->sinktags != 0 ||
	view->base_delta != 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TRACE,
	REF_BASE, IR_BASE, (uint16_t)view->exitno);

  ins = ir_load_acq(&ir[ARM64_SIDE_K_ONE]);
  if (ins.o != IR_KINT || ins.t.irt != IRT_INT || ins.i != 1)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT,
	ARM64_SIDE_K_ONE, (IROp)ins.o, ins.t.irt);
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
  ARM64_SIDE_REQUIRE(ARM64_SIDE_R_VALUE, IR_SLOAD, IRT_INT|IRT_GUARD, 5,
	IRSLOAD_TYPECHECK);
  ARM64_SIDE_REQUIRE(ARM64_SIDE_R_ADD, IR_ADDOV, IRT_INT|IRT_GUARD,
	ARM64_SIDE_R_VALUE, ARM64_SIDE_K_ONE);
  ARM64_SIDE_REQUIRE(ARM64_SIDE_R_LIMIT, IR_SLOAD, IRT_INT|IRT_GUARD, 2,
	IRSLOAD_TYPECHECK);
  ARM64_SIDE_REQUIRE(ARM64_SIDE_R_GT, IR_GT, IRT_INT|IRT_GUARD,
	ARM64_SIDE_R_LIMIT, ARM64_SIDE_R_ADD);
  ARM64_SIDE_REQUIRE(ARM64_SIDE_R_XPOLL, IR_XPOLL, IRT_NIL|IRT_GUARD, 1, 0);
#undef ARM64_SIDE_REQUIRE

  for (snapno = 0; snapno < 4u; snapno++) {
    const SnapShot *snap = &view->snap[snapno];
    MSize nextofs = snapno+1u < 4u ? mapofs[snapno+1u] : 13u;
    if (snap_ref_acq(snap) != snaprefs[snapno] ||
	snap_mapofs_acq(snap) != mapofs[snapno] ||
	snap_nent_acq(snap) != nent[snapno] ||
	snap_nslots_acq(snap) != nslots[snapno] ||
	snap_topslot_acq(snap) != 5u ||
	nextofs-mapofs[snapno] != nent[snapno]+1u+LJ_FR2 ||
	!arm64_side_snapshot_footer(view, snapno, pcpos[snapno]))
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
	snaprefs[snapno], IR_XPOLL, (uint16_t)snapno);
  }
  if (snapentry_acq(&view->snapmap[0]) !=
	SNAP(4, 0, ARM64_SIDE_R_PARENT) ||
      snapentry_acq(&view->snapmap[3]) !=
	SNAP(4, 0, ARM64_SIDE_R_ADD) ||
      snapentry_acq(&view->snapmap[4]) !=
	SNAP(5, 0, ARM64_SIDE_R_ADD) ||
      snapentry_acq(&view->snapmap[7]) !=
	SNAP(4, 0, ARM64_SIDE_R_ADD) ||
      snapentry_acq(&view->snapmap[10]) !=
	SNAP(4, 0, ARM64_SIDE_R_ADD))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
	ARM64_SIDE_R_ADD, IR_XPOLL, 0xffffu);
  return 1;
}

static int arm64_side_postra_gpr(IRIns ins)
{
  return ins.s == SPS_NONE && ins.r < RID_MAX_GPR &&
	 rset_test(RSET_GPR, ins.r);
}

int lj_asm_arm64_side_postra_admit(const LJArm64SidePostRAView *view,
	IRRef *semantic_ninsp)
{
  const IRIns *ir;
  IRIns ins;
  RegSP parentrs;
  IRRef ref;
  if (view == NULL || (ir = view->semantic.ir) == NULL ||
	!lj_asm_arm64_side_ir_admit(&view->semantic, NULL) ||
	view->nins != ARM64_SIDE_SEMANTIC_NINS+1u ||
	view->spadjust != 0 || view->parent_spadjust != 0 ||
	view->topslot != 5u || view->parent_topslot != 5u)
    return 0;

  ins = ir_load_acq(&ir[ARM64_SIDE_SEMANTIC_NINS]);
  if (ins.o != IR_NOP || ins.t.irt != IRT_NIL ||
	ins.op1 != 0 || ins.op2 != 0 || ins.prev != 0)
    return 0;

  parentrs = view->parent_slot4;
  if (!regsp_used(parentrs) || ra_hasspill(regsp_spill(parentrs)) ||
	regsp_reg(parentrs) >= RID_MAX_GPR ||
	!rset_test(RSET_GPR, regsp_reg(parentrs)))
    return 0;
  ins = ir_load_acq(&ir[REF_BASE]);
  if (ins.r != RID_BASE || ins.s != SPS_NONE)
    return 0;
  for (ref = ARM64_SIDE_R_PARENT; ref <= ARM64_SIDE_R_LIMIT; ref++) {
    ins = ir_load_acq(&ir[ref]);
    if (!arm64_side_postra_gpr(ins))
      return 0;
  }
  ins = ir_load_acq(&ir[ARM64_SIDE_R_PARENT]);
  if (ins.r != regsp_reg(parentrs))
    return 0;
  ins = ir_load_acq(&ir[ARM64_SIDE_R_GT]);
  if (ins.r != RID_INIT || ins.s != SPS_NONE)
    return 0;
  ins = ir_load_acq(&ir[ARM64_SIDE_R_XPOLL]);
  if (ins.r != RID_INIT || ins.s != SPS_NONE)
    return 0;
  for (ref = ARM64_SIDE_K_ONE; ref <= REF_NIL; ref++) {
    ins = ir_load_acq(&ir[ref]);
    if (ins.prev != REGSP_INIT)
      return 0;
  }
  if (semantic_ninsp)
    *semantic_ninsp = ARM64_SIDE_SEMANTIC_NINS;
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

static int arm64_ir_kint_value(const GCtrace *T, IRRef ref, int32_t *value)
{
  if (!arm64_ir_int_kref(T, ref))
    return 0;
  *value = T->ir[ref].i;
  return 1;
}

/* Prove the exact narrowed constant-step scalar evolution emitted for an
** integer FORL root. The two unchecked ADDs are safe only because the loop
** limit plus the non-zero constant step is proved to remain in int32 range. */
static int arm64_ir_forl_shape(const jit_State *J, const GCtrace *T,
	IRRef loopref, IRRef xpollref, IRRef firstphi,
	LJArm64IRReject *reject)
{
  MSize idxslot = (MSize)(1u+LJ_FR2+bc_a(T->startins));
  MSize stopslot = idxslot+FORL_STOP;
  IRRef ref, preadd = 0, postadd = 0, indexphi = 0;
  IRRef idxload, stepref, stopref;
  IRIns pre, post, precmp, postcmp, idx, stop;
  IROp cmpop;
  int32_t step, stopvalue;
  unsigned nadd = 0;

  UNUSED(J);
  for (ref = REF_FIRST; ref < T->nins; ref++) {
    if (T->ir[ref].o != IR_ADD)
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
  if (idxload < REF_FIRST || idxload >= preadd ||
      !arm64_ir_kint_value(T, stepref, &step) || step == 0 ||
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

  cmpop = step > 0 ? IR_LE : IR_GE;
  stopref = precmp.op2;
  if (precmp.o != cmpop || postcmp.o != cmpop ||
      precmp.op1 != preadd || postcmp.op1 != postadd ||
      postcmp.op2 != stopref ||
      precmp.t.irt != (IRT_INT|IRT_GUARD) ||
      postcmp.t.irt != (IRT_INT|IRT_GUARD))
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			   preadd+1u, cmpop, 5);

  if (irref_isk(stopref)) {
    int64_t sum;
    if (!arm64_ir_kint_value(T, stopref, &stopvalue))
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_CONSTANT,
			     stopref, IR_KINT, 0);
    sum = (int64_t)stopvalue + (int64_t)step;
    if (sum < INT32_MIN || sum > INT32_MAX)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     stopref, IR_ADD, 6);
  } else {
    IRRef guardref;
    IRIns boundguard;
    int32_t boundvalue, expected;
    int64_t expected64;
    if (stopref < REF_FIRST || stopref >= preadd)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     stopref, IR_SLOAD, 7);
    stop = T->ir[stopref];
    if (stop.o != IR_SLOAD || stop.op1 != stopslot ||
	stop.op2 != (IRSLOAD_READONLY|IRSLOAD_INHERIT) ||
	stop.t.irt != IRT_INT)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     stopref, IR_SLOAD, stop.op2);
    guardref = stopref+1u;
    if (guardref >= preadd)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     guardref, cmpop, 8);
    boundguard = T->ir[guardref];
    expected64 = step > 0 ? (int64_t)INT32_MAX-(int64_t)step :
				 (int64_t)INT32_MIN-(int64_t)step;
    if (expected64 < INT32_MIN || expected64 > INT32_MAX)
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     guardref, cmpop, 9);
    expected = (int32_t)expected64;
    if (boundguard.o != cmpop || boundguard.op1 != stopref ||
	!arm64_ir_kint_value(T, boundguard.op2, &boundvalue) ||
	boundvalue != expected ||
	boundguard.t.irt != (IRT_INT|IRT_GUARD))
      return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_OPERAND,
			     guardref, cmpop, 10);
  }

  for (ref = firstphi; ref && ref < T->nins; ref++) {
    const IRIns *phi = &T->ir[ref];
    if (phi->op1 == preadd && phi->op2 == postadd) {
      if (indexphi != 0)
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE,
			       ref, IR_PHI, 11);
      indexphi = ref;
    }
  }
  if (indexphi == 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE,
			   preadd, IR_PHI, 12);
  return 1;
}

static int arm64_ir_snapshots(const GCtrace *T, IRRef loopref,
	IRRef xpollref, MSize root_topslot, const jit_State *J,
	uintptr_t proto_lo, uintptr_t proto_hi, BCOp rootop,
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
	  !arm64_ir_int_ref(T, ref, snapref, rootop == BC_FORL))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
			       ref, IR_XPOLL, (uint16_t)snapno);
      if (flags == SNAP_NORESTORE) {
	const IRIns *source;
	MSize idxslot = (MSize)(1u+LJ_FR2+bc_a(T->startins));
	if (rootop != BC_FORL || irref_isk(ref) ||
	    ref < REF_FIRST || ref >= snapref ||
	    (slot != idxslot && slot != idxslot+FORL_STOP))
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
  if (startop == BC_FORL)
    forl_idxslot = (MSize)(1u+LJ_FR2+bc_a(T->startins));
  if (T->sinktags != 0)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SINK, REF_BASE,
			   IR_TNEW, T->sinktags);
  if (!arm64_ir_constants(T, reject))
    return 0;
  if (T->nsnap == 0 || T->snap == NULL || T->snapmap == NULL)
    return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_SNAPSHOT,
			   REF_BASE, IR_XPOLL, 0);
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
      if (!arm64_ir_sload_layout(*ir, startop, forl_idxslot,
				    root_topslot+1u+LJ_FR2))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				 IR_SLOAD, ir->op2);
      break;
    case IR_LT: case IR_GE: case IR_LE: case IR_GT:
    case IR_EQ: case IR_NE:
      if (!arm64_ir_type_flags(ir->t, IRT_INT, IRT_GUARD, IRT_GUARD) ||
	  !arm64_ir_int_binary(T, ir, ref, startop == BC_FORL))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				 (IROp)ir->o, ir->t.irt);
      break;
    case IR_ADDOV: case IR_SUBOV: case IR_MULOV:
      if (!arm64_ir_type_flags(ir->t, IRT_INT, IRT_GUARD,
			       IRT_GUARD|IRT_ISPHI) ||
	  !arm64_ir_int_binary(T, ir, ref, startop == BC_FORL))
	return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				 (IROp)ir->o, ir->t.irt);
      break;
    case IR_ADD:
	if (startop != BC_FORL ||
	    ir->t.irt != (IRT_INT|IRT_ISPHI) ||
	    !arm64_ir_int_binary(T, ir, ref, 1))
	  return arm64_ir_reject(reject, LJ_ARM64_IR_REJECT_TYPE, ref,
				 IR_ADD, ir->t.irt);
	break;
    case IR_PHI:
      if (firstphi == 0)
	firstphi = ref;
      nphi++;
      if (ir->t.irt != IRT_INT || flags != 0 ||
	  nphi > LJ_MAX_PHI ||
	  loopref == 0 || xpollref != loopref + 1u || ref <= xpollref ||
	  ir->op1 < REF_FIRST || ir->op1 >= loopref ||
	  ir->op2 <= xpollref || ir->op2 >= firstphi ||
	  !arm64_ir_int_binary(T, ir, ref, startop == BC_FORL) ||
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
      !arm64_ir_forl_shape(J, T, loopref, xpollref, firstphi, reject))
    return 0;
  if (!arm64_ir_snapshots(T, loopref, xpollref, root_topslot, J,
			  proto_lo, proto_hi, startop, reject))
    return 0;
  if (!arm64_ir_guard_snapshots(T, reject))
    return 0;
  return 1;
}
#endif

/* -- Assembler state and common macros ----------------------------------- */

/* Assembler state. */
typedef struct ASMState {
  RegCost cost[RID_MAX];  /* Reference and blended allocation cost for regs. */

  MCode *mcp;		/* Current MCode pointer (grows down). */
  MCode *mclim;		/* Lower limit for MCode memory + red zone. */
#ifdef LUA_USE_ASSERT
  MCode *mcp_prev;	/* Red zone overflow check. */
#endif

  IRIns *ir;		/* Copy of pointer to IR instructions/constants. */
  jit_State *J;		/* JIT compiler state. */

#if LJ_TARGET_X86ORX64
  x86ModRM mrm;		/* Fused x86 address operand. */
#endif

  RegSet freeset;	/* Set of free registers. */
  RegSet modset;	/* Set of registers modified inside the loop. */
  RegSet weakset;	/* Set of weakly referenced registers. */
  RegSet phiset;	/* Set of PHI registers. */

  uint32_t flags;	/* Copy of JIT compiler flags. */
  int loopinv;		/* Loop branch inversion (0:no, 1:yes, 2:yes+CC_P). */

  int32_t evenspill;	/* Next even spill slot. */
  int32_t oddspill;	/* Next odd spill slot (or 0). */

  IRRef curins;		/* Reference of current instruction. */
  IRRef stopins;	/* Stop assembly before hitting this instruction. */
  IRRef orignins;	/* Original T->nins. */

  IRRef snapref;	/* Current snapshot is active after this reference. */
  IRRef snaprename;	/* Rename highwater mark for snapshot check. */
  SnapNo snapno;	/* Current snapshot number. */
  SnapNo loopsnapno;	/* Loop snapshot number. */
  int snapalloc;	/* Current snapshot needs allocation. */
  BloomFilter snapfilt1, snapfilt2;	/* Filled with snapshot refs. */

  IRRef fuseref;	/* Fusion limit (loopref, 0 or FUSE_DISABLED). */
  IRRef sectref;	/* Section base reference (loopref or 0). */
  IRRef loopref;	/* Reference of LOOP instruction (or 0). */

  BCReg topslot;	/* Number of slots for stack check (unless 0). */
  int32_t gcsteps;	/* Accumulated number of GC steps (per section). */

  GCtrace *T;		/* Trace to assemble. */
  GCtrace *parent;	/* Parent trace (or NULL). */

  MCode *mcbot;		/* Bottom of reserved MCode. */
  MCode *mctop;		/* Top of generated MCode. */
  MCode *mctoporig;	/* Original top of generated MCode. */
  MCode *mcloop;	/* Pointer to loop MCode (or NULL). */
  MCode *invmcp;	/* Points to invertible loop branch (or NULL). */
  MCode *flagmcp;	/* Pending opportunity to merge flag setting ins. */
  MCode *realign;	/* Realign loop if not NULL. */
  MCode *mctail;	/* Tail of trace before stack adjust + jmp. */
#if LJ_TARGET_PPC || LJ_TARGET_ARM64
  MCode *mcexit;	/* Pointer to exit stubs. */
#endif

#ifdef LUAJIT_RANDOM_RA
  /* Randomize register allocation. OK for fuzz testing, not for production. */
  uint64_t prngbits;
  PRNGState prngstate;
#endif

#ifdef RID_NUM_KREF
  intptr_t krefk[RID_NUM_KREF];
#endif
  IRRef1 phireg[RID_MAX];  /* PHI register references. */
  uint16_t parentmap[LJ_MAX_JSLOTS];  /* Parent instruction to RegSP map. */
} ASMState;

#ifdef LUA_USE_ASSERT
#define lj_assertA(c, ...)	lj_assertG_(J2G(as->J), (c), __VA_ARGS__)
#else
#define lj_assertA(c, ...)	((void)as)
#endif

#define IR(ref)			(&as->ir[(ref)])

#define ASMREF_TMP1		REF_TRUE	/* Temp. register. */
#define ASMREF_TMP2		REF_FALSE	/* Temp. register. */
#define ASMREF_L		REF_NIL		/* Stores register for L. */

/* Check for variant to invariant references. */
#define iscrossref(as, ref)	((ref) < as->sectref)

/* Inhibit memory op fusion from variant to invariant references. */
#define FUSE_DISABLED		(~(IRRef)0)
#define mayfuse(as, ref)	((ref) > as->fuseref)
#define neverfuse(as)		(as->fuseref == FUSE_DISABLED)
#define canfuse(as, ir)		(!neverfuse(as) && !irt_isphi((ir)->t))
#define opisfusableload(o) \
  ((o) == IR_ALOAD || (o) == IR_HLOAD || (o) == IR_ULOAD || \
   (o) == IR_FLOAD || (o) == IR_XLOAD || (o) == IR_SLOAD || (o) == IR_VLOAD)

/* Sparse limit checks using a red zone before the actual limit. */
/*
** MT-safe x64 inline helpers and side-trace head checks can emit longer
** straight-line sequences than stock LuaJIT between sparse limit checks. Keep a
** larger reserve so debug red-zone assertions still prove the assembler cannot
** cross the real mcode limit before the next explicit check.
*/
#define MCLIM_REDZONE	128

static LJ_NORET LJ_NOINLINE void asm_mclimit(ASMState *as)
{
#if LJ_TARGET_ARM64 && LJ_ARM64_JIT_EXIT_TARGET_SLOTS
  /* ARM64's fixed exit gates live between mctop and mctoporig. Count them in
  ** every body overflow decision; mctop alone names the fallback boundary. */
  lj_assertA(as->mcp <= as->mctoporig, "bad ARM64 mcode accounting");
  lj_mcode_limiterr(as->J,
	(size_t)(as->mctoporig - as->mcp + 4*MCLIM_REDZONE));
#else
  lj_mcode_limiterr(as->J, (size_t)(as->mctop - as->mcp + 4*MCLIM_REDZONE));
#endif
}

static LJ_AINLINE void checkmclim(ASMState *as)
{
#ifdef LUA_USE_ASSERT
  if (as->mcp + MCLIM_REDZONE < as->mcp_prev) {
    IRIns *ir = IR(as->curins+1);
    lj_assertA(0, "red zone overflow: %p..%p %ld bytes IR %04d  %02d %04d %04d\n",
      as->mcp, as->mcp_prev, (long)(as->mcp_prev - as->mcp),
      as->curins+1-REF_BIAS, ir->o, ir->op1-REF_BIAS, ir->op2-REF_BIAS);
  }
#endif
  if (LJ_UNLIKELY(as->mcp < as->mclim)) asm_mclimit(as);
#ifdef LUA_USE_ASSERT
  as->mcp_prev = as->mcp;
#endif
}

#ifdef RID_NUM_KREF
#define ra_iskref(ref)		((ref) < RID_NUM_KREF)
#define ra_krefreg(ref)		((Reg)(RID_MIN_KREF + (Reg)(ref)))
#define ra_krefk(as, ref)	(as->krefk[(ref)])

static LJ_AINLINE void ra_setkref(ASMState *as, Reg r, intptr_t k)
{
  IRRef ref = (IRRef)(r - RID_MIN_KREF);
  as->krefk[ref] = k;
  as->cost[r] = REGCOST(ref, ref);
}

#else
#define ra_iskref(ref)		0
#define ra_krefreg(ref)		RID_MIN_GPR
#define ra_krefk(as, ref)	0
#endif

/* Arch-specific field offsets. */
static const uint8_t field_ofs[IRFL__MAX+1] = {
#define FLOFS(name, ofs)	(uint8_t)(ofs),
IRFLDEF(FLOFS)
#undef FLOFS
  0
};

#ifdef LUAJIT_RANDOM_RA
/* Return a fixed number of random bits from the local PRNG state. */
static uint32_t ra_random_bits(ASMState *as, uint32_t nbits) {
  uint64_t b = as->prngbits;
  uint32_t res = (1u << nbits) - 1u;
  if (b <= res) b = lj_prng_u64(&as->prngstate) | (1ull << 63);
  res &= (uint32_t)b;
  as->prngbits = b >> nbits;
  return res;
}

/* Pick a random register from a register set. */
static Reg rset_pickrandom(ASMState *as, RegSet rs)
{
  Reg r = rset_pickbot_(rs);
  rs >>= r;
  if (rs > 1) {  /* More than one bit set? */
    while (1) {
      /* We need to sample max. the GPR or FPR half of the set. */
      uint32_t d = ra_random_bits(as, RSET_BITS-1);
      if ((rs >> d) & 1) {
	r += d;
	break;
      }
    }
  }
  return r;
}
#define rset_picktop(rs)	rset_pickrandom(as, rs)
#define rset_pickbot(rs)	rset_pickrandom(as, rs)
#else
#define rset_picktop(rs)	rset_picktop_(rs)
#define rset_pickbot(rs)	rset_pickbot_(rs)
#endif

/* -- Target-specific instruction emitter --------------------------------- */

#if LJ_TARGET_X86ORX64
#include "lj_emit_x86.h"
#elif LJ_TARGET_ARM
#include "lj_emit_arm.h"
#elif LJ_TARGET_ARM64
#include "lj_emit_arm64.h"
#elif LJ_TARGET_PPC
#include "lj_emit_ppc.h"
#elif LJ_TARGET_MIPS
#include "lj_emit_mips.h"
#else
#error "Missing instruction emitter for target CPU"
#endif

/* Generic load/store of register from/to stack slot. */
#define emit_spload(as, ir, r, ofs) \
  emit_loadofs(as, ir, (r), RID_SP, (ofs))
#define emit_spstore(as, ir, r, ofs) \
  emit_storeofs(as, ir, (r), RID_SP, (ofs))

/* -- Register allocator debugging ---------------------------------------- */

/* #define LUAJIT_DEBUG_RA */

#ifdef LUAJIT_DEBUG_RA

#include <stdio.h>
#include <stdarg.h>

#define RIDNAME(name)	#name,
static const char *const ra_regname[] = {
  GPRDEF(RIDNAME)
  FPRDEF(RIDNAME)
  VRIDDEF(RIDNAME)
  NULL
};
#undef RIDNAME

static char ra_dbg_buf[65536];
static char *ra_dbg_p;
static char *ra_dbg_merge;
static MCode *ra_dbg_mcp;

static void ra_dstart(void)
{
  ra_dbg_p = ra_dbg_buf;
  ra_dbg_merge = NULL;
  ra_dbg_mcp = NULL;
}

static void ra_dflush(void)
{
  fwrite(ra_dbg_buf, 1, (size_t)(ra_dbg_p-ra_dbg_buf), stdout);
  ra_dstart();
}

static void ra_dprintf(ASMState *as, const char *fmt, ...)
{
  char *p;
  va_list argp;
  va_start(argp, fmt);
  p = ra_dbg_mcp == as->mcp ? ra_dbg_merge : ra_dbg_p;
  ra_dbg_mcp = NULL;
  p += sprintf(p, "%08x  \e[36m%04d ", (uintptr_t)as->mcp, as->curins-REF_BIAS);
  for (;;) {
    const char *e = strchr(fmt, '$');
    if (e == NULL) break;
    memcpy(p, fmt, (size_t)(e-fmt));
    p += e-fmt;
    if (e[1] == 'r') {
      Reg r = va_arg(argp, Reg) & RID_MASK;
      if (r <= RID_MAX) {
	const char *q;
	for (q = ra_regname[r]; *q; q++)
	  *p++ = *q >= 'A' && *q <= 'Z' ? *q + 0x20 : *q;
      } else {
	*p++ = '?';
	lj_assertA(0, "bad register %d for debug format \"%s\"", r, fmt);
      }
    } else if (e[1] == 'f' || e[1] == 'i') {
      IRRef ref;
      if (e[1] == 'f')
	ref = va_arg(argp, IRRef);
      else
	ref = va_arg(argp, IRIns *) - as->ir;
      if (ref >= REF_BIAS)
	p += sprintf(p, "%04d", ref - REF_BIAS);
      else
	p += sprintf(p, "K%03d", REF_BIAS - ref);
    } else if (e[1] == 's') {
      uint32_t slot = va_arg(argp, uint32_t);
      p += sprintf(p, "[sp+0x%x]", sps_scale(slot));
    } else if (e[1] == 'x') {
      p += sprintf(p, "%08x", va_arg(argp, int32_t));
    } else {
      lj_assertA(0, "bad debug format code");
    }
    fmt = e+2;
  }
  va_end(argp);
  while (*fmt)
    *p++ = *fmt++;
  *p++ = '\e'; *p++ = '['; *p++ = 'm'; *p++ = '\n';
  if (p > ra_dbg_buf+sizeof(ra_dbg_buf)-256) {
    fwrite(ra_dbg_buf, 1, (size_t)(p-ra_dbg_buf), stdout);
    p = ra_dbg_buf;
  }
  ra_dbg_p = p;
}

#define RA_DBG_START()	ra_dstart()
#define RA_DBG_FLUSH()	ra_dflush()
#define RA_DBG_REF() \
  do { char *_p = ra_dbg_p; ra_dprintf(as, ""); \
       ra_dbg_merge = _p; ra_dbg_mcp = as->mcp; } while (0)
#define RA_DBGX(x)	ra_dprintf x

#else
#define RA_DBG_START()	((void)0)
#define RA_DBG_FLUSH()	((void)0)
#define RA_DBG_REF()	((void)0)
#define RA_DBGX(x)	((void)0)
#endif

/* -- Register allocator -------------------------------------------------- */

#define ra_free(as, r)		rset_set(as->freeset, (r))
#define ra_modified(as, r)	rset_set(as->modset, (r))
#define ra_weak(as, r)		rset_set(as->weakset, (r))
#define ra_noweak(as, r)	rset_clear(as->weakset, (r))

#define ra_used(ir)		(ra_hasreg((ir)->r) || ra_hasspill((ir)->s))

/* Setup register allocator. */
static void ra_setup(ASMState *as)
{
  Reg r;
  /* Initially all regs (except the stack pointer) are free for use. */
  as->freeset = RSET_INIT;
  as->modset = RSET_EMPTY;
  as->weakset = RSET_EMPTY;
  as->phiset = RSET_EMPTY;
  memset(as->phireg, 0, sizeof(as->phireg));
  for (r = RID_MIN_GPR; r < RID_MAX; r++)
    as->cost[r] = REGCOST(~0u, 0u);
}

/* Rematerialize constants. */
static Reg ra_rematk(ASMState *as, IRRef ref)
{
  IRIns *ir;
  Reg r;
  if (ra_iskref(ref)) {
    r = ra_krefreg(ref);
    lj_assertA(!rset_test(as->freeset, r), "rematk of free reg %d", r);
    ra_free(as, r);
    ra_modified(as, r);
#if LJ_64
    emit_loadu64(as, r, ra_krefk(as, ref));
#else
    emit_loadi(as, r, ra_krefk(as, ref));
#endif
    return r;
  }
  ir = IR(ref);
  r = ir->r;
  lj_assertA(ra_hasreg(r), "rematk of K%03d has no reg", REF_BIAS - ref);
  lj_assertA(!ra_hasspill(ir->s),
	     "rematk of K%03d has spill slot [%x]", REF_BIAS - ref, ir->s);
  ra_free(as, r);
  ra_modified(as, r);
  ir->r = RID_INIT;  /* Do not keep any hint. */
  RA_DBGX((as, "remat     $i $r", ir, r));
#if !LJ_SOFTFP32
  if (ir->o == IR_KNUM) {
    emit_loadk64(as, r, ir);
  } else
#endif
  if (emit_canremat(REF_BASE) && ir->o == IR_BASE) {
    ra_sethint(ir->r, RID_BASE);  /* Restore BASE register hint. */
    emit_gettg(as, r, jit_base);
  } else if (emit_canremat(ASMREF_L) && ir->o == IR_KPRI) {
    /* REF_NIL stores ASMREF_L register. */
    lj_assertA(irt_isnil(ir->t), "rematk of bad ASMREF_L");
    emit_gettg(as, r, cur_L);
#if LJ_64
  } else if (ir->o == IR_KINT64) {
    emit_loadu64(as, r, ir_kint64(ir)->u64);
#if LJ_GC64
  } else if (ir->o == IR_KGC) {
    emit_loadu64(as, r, (uintptr_t)ir_kgc(ir));
  } else if (ir->o == IR_KPTR || ir->o == IR_KKPTR) {
    emit_loadu64(as, r, (uintptr_t)ir_kptr(ir));
#endif
#endif
  } else {
    lj_assertA(ir->o == IR_KINT || ir->o == IR_KGC ||
	       ir->o == IR_KPTR || ir->o == IR_KKPTR || ir->o == IR_KNULL,
	       "rematk of bad IR op %d", ir->o);
    emit_loadi(as, r, ir->i);
  }
  return r;
}

/* Force a spill. Allocate a new spill slot if needed. */
static int32_t ra_spill(ASMState *as, IRIns *ir)
{
  int32_t slot = ir->s;
  lj_assertA(ir >= as->ir + REF_TRUE,
	     "spill of K%03d", REF_BIAS - (int)(ir - as->ir));
  if (!ra_hasspill(slot)) {
    if (irt_is64(ir->t)) {
      slot = as->evenspill;
      as->evenspill += 2;
    } else if (as->oddspill) {
      slot = as->oddspill;
      as->oddspill = 0;
    } else {
      slot = as->evenspill;
      as->oddspill = slot+1;
      as->evenspill += 2;
    }
    if (as->evenspill > 256)
      lj_trace_err(as->J, LJ_TRERR_SPILLOV);
    ir->s = (uint8_t)slot;
  }
  return sps_scale(slot);
}

/* Release the temporarily allocated register in ASMREF_TMP1/ASMREF_TMP2. */
static Reg ra_releasetmp(ASMState *as, IRRef ref)
{
  IRIns *ir = IR(ref);
  Reg r = ir->r;
  if (LJ_UNLIKELY(!ra_hasreg(r) || ra_hasspill(ir->s)))
    lj_trace_err_info(as->J, LJ_TRERR_NYIIR);
  lj_assertA(ra_hasreg(r), "release of TMP%d has no reg", ref-ASMREF_TMP1+1);
  lj_assertA(!ra_hasspill(ir->s),
	     "release of TMP%d has spill slot [%x]", ref-ASMREF_TMP1+1, ir->s);
  ra_free(as, r);
  ra_modified(as, r);
  ir->r = RID_INIT;
  return r;
}

/* Restore a register (marked as free). Rematerialize or force a spill. */
static Reg ra_restore(ASMState *as, IRRef ref)
{
  if (emit_canremat(ref)) {
    Reg r = ra_rematk(as, ref);
    checkmclim(as);
    return r;
  } else {
    IRIns *ir = IR(ref);
    int32_t ofs = ra_spill(as, ir);  /* Force a spill slot. */
    Reg r = ir->r;
    lj_assertA(ra_hasreg(r), "restore of IR %04d has no reg", ref - REF_BIAS);
    ra_sethint(ir->r, r);  /* Keep hint. */
    ra_free(as, r);
    if (!rset_test(as->weakset, r)) {  /* Only restore non-weak references. */
      ra_modified(as, r);
      RA_DBGX((as, "restore   $i $r", ir, r));
      emit_spload(as, ir, r, ofs);
      checkmclim(as);
    }
    return r;
  }
}

/* Save a register to a spill slot. */
static void ra_save(ASMState *as, IRIns *ir, Reg r)
{
  RA_DBGX((as, "save      $i $r", ir, r));
  emit_spstore(as, ir, r, sps_scale(ir->s));
}

#define MINCOST(name) \
  if (rset_test(RSET_ALL, RID_##name) && \
      LJ_LIKELY(allow&RID2RSET(RID_##name)) && as->cost[RID_##name] < cost) \
    cost = as->cost[RID_##name];

/* Evict the register with the lowest cost, forcing a restore. */
static Reg ra_evict(ASMState *as, RegSet allow)
{
  IRRef ref;
  RegCost cost = ~(RegCost)0;
  Reg r;
  lj_assertA(allow != RSET_EMPTY, "evict from empty set");
  if (RID_NUM_FPR == 0 || allow < RID2RSET(RID_MAX_GPR)) {
    GPRDEF(MINCOST)
  } else {
    FPRDEF(MINCOST)
  }
  ref = regcost_ref(cost);
  lj_assertA(ra_iskref(ref) || (ref >= as->T->nk && ref < as->T->nins),
	     "evict of out-of-range IR %04d", ref - REF_BIAS);
  /* Preferably pick any weak ref instead of a non-weak, non-const ref. */
  if (!irref_isk(ref) && (as->weakset & allow)) {
    IRIns *ir = IR(ref);
    if (!rset_test(as->weakset, ir->r))
      ref = regcost_ref(as->cost[rset_pickbot((as->weakset & allow))]);
  }
  r = ra_restore(as, ref);
  checkmclim(as);
  return r;
}

/* Pick any register (marked as free). Evict on-demand. */
static Reg ra_pick(ASMState *as, RegSet allow)
{
  RegSet pick = as->freeset & allow;
  if (!pick)
    return ra_evict(as, allow);
  else
    return rset_picktop(pick);
}

/* Get a scratch register (marked as free). */
static Reg ra_scratch(ASMState *as, RegSet allow)
{
  Reg r = ra_pick(as, allow);
  ra_modified(as, r);
  RA_DBGX((as, "scratch        $r", r));
  return r;
}

/* Evict all registers from a set (if not free). */
static void ra_evictset(ASMState *as, RegSet drop)
{
  RegSet work;
  as->modset |= drop;
#if !LJ_SOFTFP
  work = (drop & ~as->freeset) & RSET_FPR;
  while (work) {
    Reg r = rset_pickbot(work);
    ra_restore(as, regcost_ref(as->cost[r]));
    rset_clear(work, r);
    checkmclim(as);
  }
#endif
  work = (drop & ~as->freeset);
  while (work) {
    Reg r = rset_pickbot(work);
    ra_restore(as, regcost_ref(as->cost[r]));
    rset_clear(work, r);
    checkmclim(as);
  }
}

/* Evict (rematerialize) all registers allocated to constants. */
static void ra_evictk(ASMState *as)
{
  RegSet work;
#if !LJ_SOFTFP
  work = ~as->freeset & RSET_FPR;
  while (work) {
    Reg r = rset_pickbot(work);
    IRRef ref = regcost_ref(as->cost[r]);
    if (emit_canremat(ref) && irref_isk(ref)) {
      ra_rematk(as, ref);
      checkmclim(as);
    }
    rset_clear(work, r);
  }
#endif
  work = ~as->freeset & RSET_GPR;
  while (work) {
    Reg r = rset_pickbot(work);
    IRRef ref = regcost_ref(as->cost[r]);
    if (emit_canremat(ref) && irref_isk(ref)) {
      ra_rematk(as, ref);
      checkmclim(as);
    }
    rset_clear(work, r);
  }
}

#ifdef RID_NUM_KREF
/* Allocate a register for a constant. */
static Reg ra_allock(ASMState *as, intptr_t k, RegSet allow)
{
  /* First try to find a register which already holds the same constant. */
  RegSet pick, work = ~as->freeset & RSET_GPR;
  Reg r;
  while (work) {
    IRRef ref;
    r = rset_pickbot(work);
    ref = regcost_ref(as->cost[r]);
#if LJ_64
    if (ref < ASMREF_L) {
      if (ra_iskref(ref)) {
	if (k == ra_krefk(as, ref))
	  return r;
      } else {
	IRIns *ir = IR(ref);
	if ((ir->o == IR_KINT64 && k == (int64_t)ir_kint64(ir)->u64) ||
#if LJ_GC64
#if LJ_TARGET_ARM64
	    (ir->o == IR_KINT && (uint64_t)k == (uint32_t)ir->i) ||
#else
	    (ir->o == IR_KINT && k == ir->i) ||
#endif
	    (ir->o == IR_KGC && k == (intptr_t)ir_kgc(ir)) ||
	    ((ir->o == IR_KPTR || ir->o == IR_KKPTR) &&
	     k == (intptr_t)ir_kptr(ir))
#else
	    (ir->o != IR_KINT64 && k == ir->i)
#endif
	   )
	  return r;
      }
    }
#else
    if (ref < ASMREF_L &&
	k == (ra_iskref(ref) ? ra_krefk(as, ref) : IR(ref)->i))
      return r;
#endif
    rset_clear(work, r);
  }
  pick = as->freeset & allow;
  if (pick) {
    /* Constants should preferably get unmodified registers. */
    if ((pick & ~as->modset))
      pick &= ~as->modset;
    r = rset_pickbot(pick);  /* Reduce conflicts with inverse allocation. */
  } else {
    r = ra_evict(as, allow);
  }
  RA_DBGX((as, "allock    $x $r", k, r));
  ra_setkref(as, r, k);
  rset_clear(as->freeset, r);
  ra_noweak(as, r);
  return r;
}

/* Allocate a specific register for a constant. */
static void ra_allockreg(ASMState *as, intptr_t k, Reg r)
{
  Reg kr = ra_allock(as, k, RID2RSET(r));
  if (kr != r) {
    IRIns irdummy;
    irdummy.t.irt = IRT_INT;
    ra_scratch(as, RID2RSET(r));
    emit_movrr(as, &irdummy, r, kr);
  }
}
#else
#define ra_allockreg(as, k, r)		emit_loadi(as, (r), (k))
#endif

/* Allocate a register for ref from the allowed set of registers.
** Note: this function assumes the ref does NOT have a register yet!
** Picks an optimal register, sets the cost and marks the register as non-free.
*/
static Reg ra_allocref(ASMState *as, IRRef ref, RegSet allow)
{
  IRIns *ir = IR(ref);
  RegSet pick = as->freeset & allow;
  Reg r;
  lj_assertA(ra_noreg(ir->r),
	     "IR %04d already has reg %d", ref - REF_BIAS, ir->r);
  if (pick) {
    /* First check register hint from propagation or PHI. */
    if (ra_hashint(ir->r)) {
      r = ra_gethint(ir->r);
      if (rset_test(pick, r))  /* Use hint register if possible. */
	goto found;
      /* Rematerialization is cheaper than missing a hint. */
      if (rset_test(allow, r) && emit_canremat(regcost_ref(as->cost[r]))) {
	ra_rematk(as, regcost_ref(as->cost[r]));
	checkmclim(as);
	goto found;
      }
      RA_DBGX((as, "hintmiss  $f $r", ref, r));
    }
    /* Invariants should preferably get unmodified registers. */
    if (ref < as->loopref && !irt_isphi(ir->t)) {
      if ((pick & ~as->modset))
	pick &= ~as->modset;
      r = rset_pickbot(pick);  /* Reduce conflicts with inverse allocation. */
    } else {
      /* We've got plenty of regs, so get callee-save regs if possible. */
      if (RID_NUM_GPR > 8 && (pick & ~RSET_SCRATCH))
	pick &= ~RSET_SCRATCH;
      r = rset_picktop(pick);
    }
  } else {
    r = ra_evict(as, allow);
  }
found:
  RA_DBGX((as, "alloc     $f $r", ref, r));
  ir->r = (uint8_t)r;
  rset_clear(as->freeset, r);
  ra_noweak(as, r);
  as->cost[r] = REGCOST_REF_T(ref, irt_t(ir->t));
  return r;
}

/* Allocate a register on-demand. */
static Reg ra_alloc1(ASMState *as, IRRef ref, RegSet allow)
{
  Reg r = IR(ref)->r;
  /* Note: allow is ignored if the register is already allocated. */
  if (ra_noreg(r)) r = ra_allocref(as, ref, allow);
  ra_noweak(as, r);
  return r;
}

/* Add a register rename to the IR. */
static void ra_addrename(ASMState *as, Reg down, IRRef ref, SnapNo snapno)
{
  IRRef ren;
  lj_ir_set(as->J, IRT(IR_RENAME, IRT_NIL), ref, snapno);
  ren = tref_ref(lj_ir_emit(as->J));
  as->J->cur.ir[ren].r = (uint8_t)down;
  as->J->cur.ir[ren].s = SPS_NONE;
}

/* Rename register allocation and emit move. */
static void ra_rename(ASMState *as, Reg down, Reg up)
{
  IRRef ref = regcost_ref(as->cost[up] = as->cost[down]);
  IRIns *ir = IR(ref);
  ir->r = (uint8_t)up;
  as->cost[down] = 0;
  lj_assertA((down < RID_MAX_GPR) == (up < RID_MAX_GPR),
	     "rename between GPR/FPR %d and %d", down, up);
  lj_assertA(!rset_test(as->freeset, down), "rename from free reg %d", down);
  lj_assertA(rset_test(as->freeset, up), "rename to non-free reg %d", up);
  ra_free(as, down);  /* 'down' is free ... */
  ra_modified(as, down);
  rset_clear(as->freeset, up);  /* ... and 'up' is now allocated. */
  ra_noweak(as, up);
  RA_DBGX((as, "rename    $f $r $r", regcost_ref(as->cost[up]), down, up));
  emit_movrr(as, ir, down, up);  /* Backwards codegen needs inverse move. */
  checkmclim(as);
  if (!ra_hasspill(IR(ref)->s)) {  /* Add the rename to the IR. */
    /*
    ** The rename is effective at the subsequent (already emitted) exit
    ** branch. This is for the current snapshot (as->snapno). Except if we
    ** haven't yet allocated any refs for the snapshot (as->snapalloc == 1),
    ** then it belongs to the next snapshot.
    ** See also the discussion at asm_snap_checkrename().
    */
    ra_addrename(as, down, ref, as->snapno + as->snapalloc);
  }
}

/* Pick a destination register (marked as free).
** Caveat: allow is ignored if there's already a destination register.
** Use ra_destreg() to get a specific register.
*/
static Reg ra_dest(ASMState *as, IRIns *ir, RegSet allow)
{
  Reg dest = ir->r;
  if (ra_hasreg(dest)) {
    ra_free(as, dest);
    ra_modified(as, dest);
  } else {
    if (ra_hashint(dest) && rset_test((as->freeset&allow), ra_gethint(dest))) {
      dest = ra_gethint(dest);
      ra_modified(as, dest);
      RA_DBGX((as, "dest           $r", dest));
    } else {
      dest = ra_scratch(as, allow);
    }
    ir->r = dest;
  }
  if (LJ_UNLIKELY(ra_hasspill(ir->s))) ra_save(as, ir, dest);
  return dest;
}

/* Force a specific destination register (marked as free). */
static void ra_destreg(ASMState *as, IRIns *ir, Reg r)
{
  Reg dest = ra_dest(as, ir, RID2RSET(r));
  if (dest != r) {
    lj_assertA(rset_test(as->freeset, r), "dest reg %d is not free", r);
    ra_modified(as, r);
    emit_movrr(as, ir, dest, r);
  }
}

#if LJ_TARGET_X86ORX64
/* Propagate dest register to left reference. Emit moves as needed.
** This is a required fixup step for all 2-operand machine instructions.
*/
static void ra_left(ASMState *as, Reg dest, IRRef lref)
{
  IRIns *ir = IR(lref);
  Reg left = ir->r;
  if (ra_noreg(left)) {
    if (irref_isk(lref)) {
      if (ir->o == IR_KNUM) {
	/* FP remat needs a load except for +0. Still better than eviction. */
	if (tvispzero(ir_knum(ir)) || !(as->freeset & RSET_FPR)) {
	  emit_loadk64(as, dest, ir);
	  return;
	}
#if LJ_64
      } else if (ir->o == IR_KINT64) {
	emit_loadk64(as, dest, ir);
	return;
#if LJ_GC64
      } else if (ir->o == IR_KGC || ir->o == IR_KPTR || ir->o == IR_KKPTR) {
	emit_loadk64(as, dest, ir);
	return;
#endif
#endif
      } else if (ir->o != IR_KPRI) {
	lj_assertA(ir->o == IR_KINT || ir->o == IR_KGC ||
		   ir->o == IR_KPTR || ir->o == IR_KKPTR || ir->o == IR_KNULL,
		   "K%03d has bad IR op %d", REF_BIAS - lref, ir->o);
	emit_loadi(as, dest, ir->i);
	return;
      }
    }
    if (!ra_hashint(left) && !iscrossref(as, lref))
      ra_sethint(ir->r, dest);  /* Propagate register hint. */
    left = ra_allocref(as, lref, dest < RID_MAX_GPR ? RSET_GPR : RSET_FPR);
  }
  ra_noweak(as, left);
  /* Move needed for true 3-operand instruction: y=a+b ==> y=a; y+=b. */
  if (dest != left) {
    /* Use register renaming if dest is the PHI reg. */
    if (irt_isphi(ir->t) && as->phireg[dest] == lref) {
      ra_modified(as, left);
      ra_rename(as, left, dest);
    } else {
      emit_movrr(as, ir, dest, left);
    }
  }
}
#else
/* Similar to ra_left, except we override any hints. */
static void ra_leftov(ASMState *as, Reg dest, IRRef lref)
{
  IRIns *ir = IR(lref);
  Reg left = ir->r;
  if (ra_noreg(left)) {
    ra_sethint(ir->r, dest);  /* Propagate register hint. */
    left = ra_allocref(as, lref,
		       (LJ_SOFTFP || dest < RID_MAX_GPR) ? RSET_GPR : RSET_FPR);
  }
  ra_noweak(as, left);
  if (dest != left) {
    /* Use register renaming if dest is the PHI reg. */
    if (irt_isphi(ir->t) && as->phireg[dest] == lref) {
      ra_modified(as, left);
      ra_rename(as, left, dest);
    } else {
      emit_movrr(as, ir, dest, left);
    }
  }
}
#endif

/* Force a RID_RETLO/RID_RETHI destination register pair (marked as free). */
static void ra_destpair(ASMState *as, IRIns *ir)
{
  Reg destlo = ir->r, desthi = (ir+1)->r;
  IRIns *irx = (LJ_64 && !irt_is64(ir->t)) ? ir+1 : ir;
  /* First spill unrelated refs blocking the destination registers. */
  if (!rset_test(as->freeset, RID_RETLO) &&
      destlo != RID_RETLO && desthi != RID_RETLO)
    ra_restore(as, regcost_ref(as->cost[RID_RETLO]));
  if (!rset_test(as->freeset, RID_RETHI) &&
      destlo != RID_RETHI && desthi != RID_RETHI)
    ra_restore(as, regcost_ref(as->cost[RID_RETHI]));
  /* Next free the destination registers (if any). */
  if (ra_hasreg(destlo)) {
    ra_free(as, destlo);
    ra_modified(as, destlo);
  } else {
    destlo = RID_RETLO;
  }
  if (ra_hasreg(desthi)) {
    ra_free(as, desthi);
    ra_modified(as, desthi);
  } else {
    desthi = RID_RETHI;
  }
  /* Check for conflicts and shuffle the registers as needed. */
  if (destlo == RID_RETHI) {
    if (desthi == RID_RETLO) {
#if LJ_TARGET_X86ORX64
      *--as->mcp = XI_XCHGa + RID_RETHI;
      if (LJ_64 && irt_is64(irx->t)) *--as->mcp = 0x48;
#else
      emit_movrr(as, irx, RID_RETHI, RID_TMP);
      emit_movrr(as, irx, RID_RETLO, RID_RETHI);
      emit_movrr(as, irx, RID_TMP, RID_RETLO);
#endif
    } else {
      emit_movrr(as, irx, RID_RETHI, RID_RETLO);
      if (desthi != RID_RETHI) emit_movrr(as, irx, desthi, RID_RETHI);
    }
  } else if (desthi == RID_RETLO) {
    emit_movrr(as, irx, RID_RETLO, RID_RETHI);
    if (destlo != RID_RETLO) emit_movrr(as, irx, destlo, RID_RETLO);
  } else {
    if (desthi != RID_RETHI) emit_movrr(as, irx, desthi, RID_RETHI);
    if (destlo != RID_RETLO) emit_movrr(as, irx, destlo, RID_RETLO);
  }
  /* Restore spill slots (if any). */
  if (ra_hasspill((ir+1)->s)) ra_save(as, ir+1, RID_RETHI);
  if (ra_hasspill(ir->s)) ra_save(as, ir, RID_RETLO);
}

/* -- Snapshot handling --------- ----------------------------------------- */

/* Can we rematerialize a KNUM instead of forcing a spill? */
static int asm_snap_canremat(ASMState *as)
{
  Reg r;
  for (r = RID_MIN_FPR; r < RID_MAX_FPR; r++)
    if (irref_isk(regcost_ref(as->cost[r])))
      return 1;
  return 0;
}

/* Check whether a sunk store corresponds to an allocation. */
static int asm_sunk_store(ASMState *as, IRIns *ira, IRIns *irs)
{
  if (irs->s == 255) {
    if (irs->o == IR_ASTORE || irs->o == IR_HSTORE ||
	irs->o == IR_FSTORE || irs->o == IR_XSTORE) {
      IRIns *irk = IR(irs->op1);
      if (irk->o == IR_AREF || irk->o == IR_HREFK)
	irk = IR(irk->op1);
      return (IR(irk->op1) == ira);
    }
    return 0;
  } else {
    return (ira + irs->s == irs);  /* Quick check. */
  }
}

/* Allocate register or spill slot for a ref that escapes to a snapshot. */
static void asm_snap_alloc1(ASMState *as, IRRef ref)
{
  if (!irref_isk(ref)) {
    IRIns *ir;
    /*
    ** Snapshot maps are normally private to the trace under assembly. In MT
    ** mode a trace can be aborted or retired around helper/safepoint exits while
    ** the recorder is still unwinding into ASM; validate the ref before using it
    ** as an index into the current IR buffer and abort this trace cleanly.
    */
    if (LJ_UNLIKELY(ref < REF_BASE || ref >= as->T->nins))
      lj_trace_err(as->J, LJ_TRERR_BADRA);
    ir = IR(ref);
    bloomset(as->snapfilt1, ref);
    bloomset(as->snapfilt2, hashrot(ref, ref + HASH_BIAS));
    if (ra_used(ir)) return;
    if (ir->r == RID_SINK || ir->r == RID_SUNK) {
      ir->r = RID_SUNK;
#if LJ_HASFFI
      if (ir->o == IR_CNEWI) {  /* Allocate CNEWI value. */
	asm_snap_alloc1(as, ir->op2);
	if (LJ_32 && (ir+1)->o == IR_HIOP)
	  asm_snap_alloc1(as, (ir+1)->op2);
      } else
#endif
      {  /* Allocate stored values for TNEW, TDUP and CNEW. */
	IRIns *irs;
	lj_assertA(ir->o == IR_TNEW || ir->o == IR_TDUP || ir->o == IR_CNEW,
		   "sink of IR %04d has bad op %d", ref - REF_BIAS, ir->o);
	for (irs = IR(as->snapref-1); irs > ir; irs--)
	  if (irs->r == RID_SINK && asm_sunk_store(as, ir, irs)) {
	    lj_assertA(irs->o == IR_ASTORE || irs->o == IR_HSTORE ||
		       irs->o == IR_FSTORE || irs->o == IR_XSTORE,
		       "sunk store IR %04d has bad op %d",
		       (int)(irs - as->ir) - REF_BIAS, irs->o);
	    asm_snap_alloc1(as, irs->op2);
	    if (LJ_32 && (irs+1)->o == IR_HIOP)
	      asm_snap_alloc1(as, (irs+1)->op2);
	  }
      }
    } else {
      RegSet allow;
      if (ir->o == IR_CONV && ir->op2 == IRCONV_NUM_INT) {
	IRIns *irc;
	for (irc = IR(as->curins); irc > ir; irc--)
	  if ((irc->op1 == ref || irc->op2 == ref) &&
	      !(irc->r == RID_SINK || irc->r == RID_SUNK))
	    goto nosink;  /* Don't sink conversion if result is used. */
	asm_snap_alloc1(as, ir->op1);
	return;
      }
    nosink:
      allow = (!LJ_SOFTFP && irt_isfp(ir->t)) ? RSET_FPR : RSET_GPR;
      if ((as->freeset & allow) ||
	       (allow == RSET_FPR && asm_snap_canremat(as))) {
	/* Get a weak register if we have a free one or can rematerialize. */
	Reg r = ra_allocref(as, ref, allow);  /* Allocate a register. */
	if (!irt_isphi(ir->t))
	  ra_weak(as, r);  /* But mark it as weakly referenced. */
	checkmclim(as);
	RA_DBGX((as, "snapreg   $f $r", ref, ir->r));
      } else {
	ra_spill(as, ir);  /* Otherwise force a spill slot. */
	RA_DBGX((as, "snapspill $f $s", ref, ir->s));
      }
    }
  }
}

/* Allocate refs escaping to a snapshot. */
static void asm_snap_alloc(ASMState *as, int snapno)
{
  SnapShot *snap = &as->T->snap[snapno];
  MSize mapofs = snap->mapofs;
  MSize n, nent = snap->nent;
  MSize nsnapmap = as->T->nsnapmap;
  SnapEntry *map;
  if (LJ_UNLIKELY(mapofs >= nsnapmap || nent >= nsnapmap - mapofs))
    lj_trace_err(as->J, LJ_TRERR_BADRA);
  map = &as->T->snapmap[mapofs];
  as->snapfilt1 = as->snapfilt2 = 0;
  for (n = 0; n < nent; n++) {
    SnapEntry sn = map[n];
    IRRef ref = snap_ref(sn);
    if (!irref_isk(ref)) {
      asm_snap_alloc1(as, ref);
      if (LJ_SOFTFP && (sn & SNAP_SOFTFPNUM)) {
	lj_assertA(irt_type(IR(ref+1)->t) == IRT_SOFTFP,
		   "snap %d[%d] points to bad SOFTFP IR %04d",
		   snapno, n, ref - REF_BIAS);
	asm_snap_alloc1(as, ref+1);
      }
    }
  }
}

/* All guards for a snapshot use the same exitno. This is currently the
** same as the snapshot number. Since the exact origin of the exit cannot
** be determined, all guards for the same snapshot must exit with the same
** RegSP mapping.
** A renamed ref which has been used in a prior guard for the same snapshot
** would cause an inconsistency. The easy way out is to force a spill slot.
*/
static int asm_snap_checkrename(ASMState *as, IRRef ren)
{
  if (bloomtest(as->snapfilt1, ren) &&
      bloomtest(as->snapfilt2, hashrot(ren, ren + HASH_BIAS))) {
    IRIns *ir = IR(ren);
    ra_spill(as, ir);  /* Register renamed, so force a spill slot. */
    RA_DBGX((as, "snaprensp $f $s", ren, ir->s));
    return 1;  /* Found. */
  }
  return 0;  /* Not found. */
}

/* Prepare snapshot for next guard or throwing instruction. */
static void asm_snap_prep(ASMState *as)
{
  if (as->snapalloc) {
    /* Alloc on first invocation for each snapshot. */
    as->snapalloc = 0;
    asm_snap_alloc(as, as->snapno);
    as->snaprename = as->T->nins;
  } else {
    /* Check any renames above the highwater mark. */
    for (; as->snaprename < as->T->nins; as->snaprename++) {
      IRIns *ir = &as->T->ir[as->snaprename];
      if (asm_snap_checkrename(as, ir->op1))
	ir->op2 = REF_BIAS-1;  /* Kill rename. */
    }
  }
}

/* Move to previous snapshot when we cross the current snapshot ref. */
static void asm_snap_prev(ASMState *as)
{
  if (as->curins < as->snapref) {
    uintptr_t ofs = (uintptr_t)(as->mctoporig - as->mcp);
    if (ofs >= 0x10000) lj_trace_err(as->J, LJ_TRERR_MCODEOV);
    do {
      if (as->snapno == 0) return;
      as->snapno--;
      as->snapref = as->T->snap[as->snapno].ref;
      as->T->snap[as->snapno].mcofs = (uint16_t)ofs;  /* Remember mcode ofs. */
    } while (as->curins < as->snapref);  /* May have no ins inbetween. */
    as->snapalloc = 1;
  }
}

/* Fixup snapshot mcode offsetst. */
static void asm_snap_fixup_mcofs(ASMState *as)
{
  uint32_t sz = (uint32_t)(as->mctoporig - as->mcp);
  SnapShot *snap = as->T->snap;
  SnapNo i;
  for (i = as->T->nsnap-1; i > 0; i--) {
    uint32_t ofs;
    /* Compute offset from mcode start and store in correct snapshot. */
    lj_assertA(sz >= snap[i-1].mcofs, "bad snapshot mcode offset");
    ofs = sz - snap[i-1].mcofs;
    if (LJ_UNLIKELY(ofs >= 0x10000u))
      lj_trace_err(as->J, LJ_TRERR_MCODEOV);
    snap[i].mcofs = (uint16_t)ofs;
  }
  snap[0].mcofs = 0;
}

/* -- Miscellaneous helpers ----------------------------------------------- */

/* Calculate stack adjustment. */
static int32_t asm_stack_adjust(ASMState *as)
{
  if (as->evenspill <= SPS_FIXED)
    return 0;
  return sps_scale(sps_align(as->evenspill));
}

/* Must match with hash*() in lj_tab.c. */
static uint32_t ir_khash(ASMState *as, IRIns *ir)
{
  uint32_t lo, hi;
  UNUSED(as);
  if (irt_isstr(ir->t)) {
    return ir_kstr(ir)->sid;
  } else if (irt_isnum(ir->t)) {
    lo = ir_knum(ir)->u32.lo;
    hi = ir_knum(ir)->u32.hi << 1;
  } else if (irt_ispri(ir->t)) {
    lj_assertA(!irt_isnil(ir->t), "hash of nil key");
    return irt_type(ir->t)-IRT_FALSE;
  } else {
    lj_assertA(irt_isgcv(ir->t), "hash of bad IR type %d", irt_type(ir->t));
    lo = u32ptr(ir_kgc(ir));
#if LJ_GC64
    hi = (uint32_t)(u64ptr(ir_kgc(ir)) >> 32) | (irt_toitype(ir->t) << 15);
#else
    hi = lo + HASH_BIAS;
#endif
  }
  return hashrot(lo, hi);
}

/* -- Allocations --------------------------------------------------------- */

static void asm_gencall(ASMState *as, const CCallInfo *ci, IRRef *args);
static void asm_setupresult(ASMState *as, IRIns *ir, const CCallInfo *ci);

static void asm_snew(ASMState *as, IRIns *ir)
{
  const CCallInfo *ci = &lj_ir_callinfo[IRCALL_lj_str_new];
  IRRef args[3];
  asm_snap_prep(as);
  args[0] = ASMREF_L;  /* lua_State *L    */
  args[1] = ir->op1;   /* const char *str */
  args[2] = ir->op2;   /* size_t len      */
  as->gcsteps++;
  asm_setupresult(as, ir, ci);  /* GCstr * */
  asm_gencall(as, ci, args);
}

static void asm_tnew(ASMState *as, IRIns *ir)
{
  const CCallInfo *ci = &lj_ir_callinfo[IRCALL_lj_tab_new1];
  IRRef args[2];
  asm_snap_prep(as);
  args[0] = ASMREF_L;     /* lua_State *L    */
  as->gcsteps++;
  if (ir->op1 == 0 && ir->op2 == 0) {
    ci = &lj_ir_callinfo[IRCALL_lj_tab_new0_forjit];
    asm_setupresult(as, ir, ci);  /* GCtab * */
    asm_gencall(as, ci, args);
    return;
  }
  args[1] = ASMREF_TMP1;  /* uint32_t ahsize */
  asm_setupresult(as, ir, ci);  /* GCtab * */
  asm_gencall(as, ci, args);
  ra_allockreg(as, ir->op1 | (ir->op2 << 24), ra_releasetmp(as, ASMREF_TMP1));
}

static void asm_tdup(ASMState *as, IRIns *ir)
{
  const CCallInfo *ci = &lj_ir_callinfo[IRCALL_lj_tab_dup];
  IRRef args[2];
  asm_snap_prep(as);
  args[0] = ASMREF_L;  /* lua_State *L    */
  args[1] = ir->op1;   /* const GCtab *kt */
  as->gcsteps++;
  asm_setupresult(as, ir, ci);  /* GCtab * */
  asm_gencall(as, ci, args);
}

static void asm_gc_check(ASMState *as);

/* Explicit GC step. */
static void asm_gcstep(ASMState *as, IRIns *ir)
{
  IRIns *ira;
  for (ira = IR(as->stopins+1); ira < ir; ira++)
    if ((ira->o == IR_TNEW || ira->o == IR_TDUP ||
	 ira->o == IR_BUFSTR ||
	 (LJ_HASFFI && (ira->o == IR_CNEW || ira->o == IR_CNEWI))) &&
	ra_used(ira))
      as->gcsteps++;
  if (as->gcsteps)
    asm_gc_check(as);
  as->gcsteps = 0x80000000;  /* Prevent implicit GC check further up. */
}

/* -- Buffer operations --------------------------------------------------- */

static void asm_tvptr(ASMState *as, Reg dest, IRRef ref, MSize mode);
#if LJ_TARGET_X86ORX64
static int asm_tmpref_skip_x86(ASMState *as, IRIns *ir);
static int asm_call_inline_x86(ASMState *as, IRIns *ir);
#endif
#if LJ_HASBUFFER
static void asm_bufhdr_write(ASMState *as, Reg sb);
#endif

static void asm_bufhdr(ASMState *as, IRIns *ir)
{
  Reg sb = ra_dest(as, ir, RSET_GPR);
#if LJ_TARGET_X86ORX64
  int tg_tmpbuf = 0;
#endif
  switch (ir->op2) {
  case IRBUFHDR_RESET: {
    Reg tmp = ra_scratch(as, rset_exclude(RSET_GPR, sb));
    IRIns irbp;
    irbp.ot = IRT(0, IRT_PTR);  /* Buffer data pointer type. */
#if LJ_TARGET_X86ORX64
    if (IR(ir->op1)->o == IR_LREF) {
      IRIns irgc;
      irgc.ot = IRT(0, IRT_PGC);  /* GC type. */
      tg_tmpbuf = 1;
      emit_storeofs(as, &irgc, tmp, sb, offsetof(SBuf, L));
      emit_gettg(as, tmp, cur_L);
    }
#endif
    emit_storeofs(as, &irbp, tmp, sb, offsetof(SBuf, w));
    emit_loadofs(as, &irbp, tmp, sb, offsetof(SBuf, b));
    break;
    }
  case IRBUFHDR_APPEND: {
    /* Rematerialize const buffer pointer instead of likely spill. */
    IRIns *irp = IR(ir->op1);
    if (!(ra_hasreg(irp->r) || irp == ir-1 ||
	  (irp == ir-2 && !ra_used(ir-1)))) {
      while (!(irp->o == IR_BUFHDR && irp->op2 == IRBUFHDR_RESET))
	irp = IR(irp->op1);
      if (irref_isk(irp->op1)) {
	ra_weak(as, ra_allocref(as, ir->op1, RSET_GPR));
	ir = irp;
      }
    }
    break;
    }
#if LJ_HASBUFFER
  case IRBUFHDR_WRITE:
    asm_bufhdr_write(as, sb);
    break;
#endif
  default: lj_assertA(0, "bad BUFHDR op2 %d", ir->op2); break;
  }
#if LJ_TARGET_X86ORX64
  if (tg_tmpbuf)
    emit_leatg(as, sb, tmpbuf);
  else
    ra_left(as, sb, ir->op1);
#else
  ra_leftov(as, sb, ir->op1);
#endif
}

#if LJ_TARGET_X86ORX64
static int asm_buf_is_tg_tmpbuf(ASMState *as, IRRef ref)
{
  for (;;) {
    IRIns *ir = IR(ref);
    if (ir->o == IR_BUFPUT) {
      ref = ir->op1;
    } else if (ir->o == IR_BUFHDR) {
      if (ir->op2 == IRBUFHDR_RESET)
	return IR(ir->op1)->o == IR_LREF;
      if (ir->op2 != IRBUFHDR_APPEND)
	return 0;
      ref = ir->op1;
    } else {
      return 0;
    }
  }
}
#endif

#if (defined(__linux__) || LJ_TARGET_OSX) && LJ_TARGET_X64
static int asm_bufput_const_tg_inline(ASMState *as, IRIns *ir, GCstr *s);
#endif

static void asm_bufput(ASMState *as, IRIns *ir)
{
  const CCallInfo *ci = &lj_ir_callinfo[IRCALL_lj_buf_putstr];
  IRRef args[3];
  IRIns *irs;
  int kchar = -129;
#if LJ_TARGET_X86ORX64
  int tg_tmpbuf = asm_buf_is_tg_tmpbuf(as, ir->op1);
  if (tg_tmpbuf)
    ci = &lj_ir_callinfo[IRCALL_lj_buf_putstr_tg];
#endif
  args[0] = ir->op1;  /* SBuf * */
  args[1] = ir->op2;  /* GCstr * */
  irs = IR(ir->op2);
  lj_assertA(irt_isstr(irs->t),
	     "BUFPUT of non-string IR %04d", ir->op2 - REF_BIAS);
  if (irs->o == IR_KGC) {
    GCstr *s = ir_kstr(irs);
#if (defined(__linux__) || LJ_TARGET_OSX) && LJ_TARGET_X64
    if (tg_tmpbuf && asm_bufput_const_tg_inline(as, ir, s))
      return;
#endif
    if (s->len == 1) {  /* Optimize put of single-char string constant. */
      kchar = (int8_t)strdata(s)[0];  /* Signed! */
      args[1] = ASMREF_TMP1;  /* int, truncated to char */
#if LJ_TARGET_X86ORX64
      ci = &lj_ir_callinfo[tg_tmpbuf ? IRCALL_lj_buf_putchar_tg :
				       IRCALL_lj_buf_putchar];
#else
      ci = &lj_ir_callinfo[IRCALL_lj_buf_putchar];
#endif
    }
  } else if (mayfuse(as, ir->op2) && ra_noreg(irs->r)) {
    if (irs->o == IR_TOSTR) {  /* Fuse number to string conversions. */
      if (irs->op2 == IRTOSTR_NUM) {
	args[1] = ASMREF_TMP1;  /* TValue * */
	ci = &lj_ir_callinfo[IRCALL_lj_strfmt_putnum];
      } else {
	lj_assertA(irt_isinteger(IR(irs->op1)->t),
		   "TOSTR of non-numeric IR %04d", irs->op1);
	args[1] = irs->op1;  /* int */
	if (irs->op2 == IRTOSTR_INT) {
#if LJ_TARGET_X86ORX64
	  ci = &lj_ir_callinfo[tg_tmpbuf ? IRCALL_lj_strfmt_putint_tg :
					   IRCALL_lj_strfmt_putint];
#else
	  ci = &lj_ir_callinfo[IRCALL_lj_strfmt_putint];
#endif
	} else {
#if LJ_TARGET_X86ORX64
	  ci = &lj_ir_callinfo[tg_tmpbuf ? IRCALL_lj_buf_putchar_tg :
					   IRCALL_lj_buf_putchar];
#else
	  ci = &lj_ir_callinfo[IRCALL_lj_buf_putchar];
#endif
	}
      }
    } else if (irs->o == IR_SNEW) {  /* Fuse string allocation. */
      args[1] = irs->op1;  /* const void * */
      args[2] = irs->op2;  /* MSize */
#if LJ_TARGET_X86ORX64
      ci = &lj_ir_callinfo[tg_tmpbuf ? IRCALL_lj_buf_putmem_tg :
				       IRCALL_lj_buf_putmem];
#else
      ci = &lj_ir_callinfo[IRCALL_lj_buf_putmem];
#endif
    }
  }
  asm_setupresult(as, ir, ci);  /* SBuf * */
  asm_gencall(as, ci, args);
  if (args[1] == ASMREF_TMP1) {
    Reg tmp = ra_releasetmp(as, ASMREF_TMP1);
    if (kchar == -129)
      asm_tvptr(as, tmp, irs->op1, IRTMPREF_IN1);
    else
      ra_allockreg(as, kchar, tmp);
  }
}

static void asm_bufstr(ASMState *as, IRIns *ir)
{
  const CCallInfo *ci = &lj_ir_callinfo[IRCALL_lj_buf_tostr];
  IRRef args[1];
  /*
  ** Converting a buffer to a string allocates and can trigger a GC step, just
  ** like SNEW/TOSTR. Prepare snapshot spill slots before the call so fresh
  ** trace-local values used by the following store/helper remain reconstructible
  ** across an allocation-side exit.
  */
  asm_snap_prep(as);
#if LJ_TARGET_X86ORX64
  if (asm_buf_is_tg_tmpbuf(as, ir->op1))
    ci = &lj_ir_callinfo[IRCALL_lj_buf_tostr_tg];
#endif
  args[0] = ir->op1;  /* SBuf *sb */
  as->gcsteps++;
  asm_setupresult(as, ir, ci);  /* GCstr * */
  asm_gencall(as, ci, args);
}

/* -- Type conversions ---------------------------------------------------- */

static void asm_tostr(ASMState *as, IRIns *ir)
{
  const CCallInfo *ci;
  IRRef args[2];
  asm_snap_prep(as);
  args[0] = ASMREF_L;
  as->gcsteps++;
  if (ir->op2 == IRTOSTR_NUM) {
    args[1] = ASMREF_TMP1;  /* cTValue * */
    ci = &lj_ir_callinfo[IRCALL_lj_strfmt_num];
  } else {
    args[1] = ir->op1;  /* int32_t k */
    if (ir->op2 == IRTOSTR_INT)
      ci = &lj_ir_callinfo[IRCALL_lj_strfmt_int];
    else
      ci = &lj_ir_callinfo[IRCALL_lj_strfmt_char];
  }
  asm_setupresult(as, ir, ci);  /* GCstr * */
  asm_gencall(as, ci, args);
  if (ir->op2 == IRTOSTR_NUM)
    asm_tvptr(as, ra_releasetmp(as, ASMREF_TMP1), ir->op1, IRTMPREF_IN1);
}

#if LJ_32 && LJ_HASFFI && !LJ_SOFTFP && !LJ_TARGET_X86
static void asm_conv64(ASMState *as, IRIns *ir)
{
  IRType st = (IRType)((ir-1)->op2 & IRCONV_SRCMASK);
  IRType dt = (((ir-1)->op2 & IRCONV_DSTMASK) >> IRCONV_DSH);
  IRCallID id;
  const CCallInfo *ci;
#if LJ_TARGET_ARM && !LJ_ABI_SOFTFP
  CCallInfo cim;
#endif
  IRRef args[2];
  lj_assertA((ir-1)->o == IR_CONV && ir->o == IR_HIOP,
	     "not a CONV/HIOP pair at IR %04d", (int)(ir - as->ir) - REF_BIAS);
  args[LJ_BE] = (ir-1)->op1;
  args[LJ_LE] = ir->op1;
  lj_assertA(st != IRT_FLOAT, "bad CONV *64.float emitted");
  if (st == IRT_NUM) {
    id = IRCALL_lj_vm_num2u64;
    ir--;
    ci = &lj_ir_callinfo[id];
  } else {
    id = IRCALL_fp64_l2d + ((dt == IRT_FLOAT) ? 2 : 0) + (st - IRT_I64);
#if LJ_TARGET_ARM && !LJ_ABI_SOFTFP
    cim = lj_ir_callinfo[id];
    cim.flags |= CCI_VARARG;  /* These calls don't use the hard-float ABI! */
    ci = &cim;
#else
    ci = &lj_ir_callinfo[id];
#endif
  }
  asm_setupresult(as, ir, ci);
  asm_gencall(as, ci, args);
}
#endif

/* -- Memory references --------------------------------------------------- */

static void asm_newref(ASMState *as, IRIns *ir)
{
  const CCallInfo *ci = &lj_ir_callinfo[IRCALL_lj_tab_newkey];
  IRRef args[3];
  if (ir->r == RID_SINK)
    return;
  asm_snap_prep(as);
  args[0] = ASMREF_L;     /* lua_State *L */
  args[1] = ir->op1;      /* GCtab *t     */
  args[2] = ASMREF_TMP1;  /* cTValue *key */
  asm_setupresult(as, ir, ci);  /* TValue * */
  asm_gencall(as, ci, args);
  asm_tvptr(as, ra_releasetmp(as, ASMREF_TMP1), ir->op2, IRTMPREF_IN1);
}

static void asm_tmpref(ASMState *as, IRIns *ir)
{
#if LJ_TARGET_X86ORX64
  if (asm_tmpref_skip_x86(as, ir))
    return;
#endif
  Reg r = ra_dest(as, ir, RSET_GPR);
  asm_tvptr(as, r, ir->op1, ir->op2);
}

static void asm_lref(ASMState *as, IRIns *ir)
{
  Reg r = ra_dest(as, ir, RSET_GPR);
#if LJ_TARGET_X86ORX64
  ra_left(as, r, ASMREF_L);
#else
  ra_leftov(as, r, ASMREF_L);
#endif
}

/* -- Calls --------------------------------------------------------------- */

/* Collect arguments from CALL* and CARG instructions. */
static void asm_collectargs(ASMState *as, IRIns *ir,
			    const CCallInfo *ci, IRRef *args)
{
  uint32_t n = CCI_XNARGS(ci);
  /* Account for split args. */
  lj_assertA(n <= CCI_NARGS_MAX*2, "too many args %d to collect", n);
  if ((ci->flags & CCI_L)) { *args++ = ASMREF_L; n--; }
  while (n-- > 1) {
    ir = IR(ir->op1);
    lj_assertA(ir->o == IR_CARG, "malformed CALL arg tree");
    args[n] = ir->op2 == REF_NIL ? 0 : ir->op2;
  }
  args[0] = ir->op1 == REF_NIL ? 0 : ir->op1;
  lj_assertA(IR(ir->op1)->o != IR_CARG, "malformed CALL arg tree");
}

/* Reconstruct CCallInfo flags for CALLX*. */
static uint32_t asm_callx_flags(ASMState *as, IRIns *ir)
{
  uint32_t nargs = 0;
  if (ir->op1 != REF_NIL) {  /* Count number of arguments first. */
    IRIns *ira = IR(ir->op1);
    nargs++;
    while (ira->o == IR_CARG) { nargs++; ira = IR(ira->op1); }
  }
#if LJ_HASFFI
  if (IR(ir->op2)->o == IR_CARG) {  /* Copy calling convention info. */
    CTypeID id = (CTypeID)IR(IR(ir->op2)->op2)->i;
    CTState *cts = ctype_ctsG(J2G(as->J));
    CTInfo info;
    CTSize size;
    int ok = lj_ctype_info_predefined(cts, id, &info, &size, NULL, NULL);
    if (ok <= 0)
      ok = lj_ctype_info_snapshot(cts, id, &info, &size, NULL, NULL);
    if (ok < 0)
      lj_trace_err(as->J, LJ_TRERR_CTBUSY);
    if (!ok)
      lj_trace_err(as->J, LJ_TRERR_BADTYPE);
    nargs |= ((info & CTF_VARARG) ? CCI_VARARG : 0);
#if LJ_TARGET_X86
    nargs |= (ctype_cconv(info) << CCI_CC_SHIFT);
#endif
  }
#endif
  return (nargs | (ir->t.irt << CCI_OTSHIFT));
}

static void asm_callid(ASMState *as, IRIns *ir, IRCallID id)
{
  const CCallInfo *ci = &lj_ir_callinfo[id];
  IRRef args[2];
  args[0] = ir->op1;
  args[1] = ir->op2;
  asm_setupresult(as, ir, ci);
  asm_gencall(as, ci, args);
}

static void asm_call(ASMState *as, IRIns *ir)
{
  IRRef args[CCI_NARGS_MAX];
  const CCallInfo *ci = &lj_ir_callinfo[ir->op2];
  asm_collectargs(as, ir, ci, args);
  asm_setupresult(as, ir, ci);
  asm_gencall(as, ci, args);
}

/* -- PHI and loop handling ----------------------------------------------- */

/* Break a PHI cycle by renaming to a free register (evict if needed). */
static void asm_phi_break(ASMState *as, RegSet blocked, RegSet blockedby,
			  RegSet allow)
{
  RegSet candidates = blocked & allow;
  if (candidates) {  /* If this register file has candidates. */
    /* Note: the set for ra_pick cannot be empty, since each register file
    ** has some registers never allocated to PHIs.
    */
    Reg down, up = ra_pick(as, ~blocked & allow);  /* Get a free register. */
    if (candidates & ~blockedby)  /* Optimize shifts, else it's a cycle. */
      candidates = candidates & ~blockedby;
    down = rset_picktop(candidates);  /* Pick candidate PHI register. */
    ra_rename(as, down, up);  /* And rename it to the free register. */
  }
}

/* PHI register shuffling.
**
** The allocator tries hard to preserve PHI register assignments across
** the loop body. Most of the time this loop does nothing, since there
** are no register mismatches.
**
** If a register mismatch is detected and ...
** - the register is currently free: rename it.
** - the register is blocked by an invariant: restore/remat and rename it.
** - Otherwise the register is used by another PHI, so mark it as blocked.
**
** The renames are order-sensitive, so just retry the loop if a register
** is marked as blocked, but has been freed in the meantime. A cycle is
** detected if all of the blocked registers are allocated. To break the
** cycle rename one of them to a free register and retry.
**
** Note that PHI spill slots are kept in sync and don't need to be shuffled.
*/
static void asm_phi_shuffle(ASMState *as)
{
  RegSet work;

  /* Find and resolve PHI register mismatches. */
  for (;;) {
    RegSet blocked = RSET_EMPTY;
    RegSet blockedby = RSET_EMPTY;
    RegSet phiset = as->phiset;
    while (phiset) {  /* Check all left PHI operand registers. */
      Reg r = rset_pickbot(phiset);
      IRIns *irl = IR(as->phireg[r]);
      Reg left = irl->r;
      if (r != left) {  /* Mismatch? */
	if (!rset_test(as->freeset, r)) {  /* PHI register blocked? */
	  IRRef ref = regcost_ref(as->cost[r]);
	  /* Blocked by other PHI (w/reg)? */
	  if (!ra_iskref(ref) && irt_ismarked(IR(ref)->t)) {
	    rset_set(blocked, r);
	    if (ra_hasreg(left))
	      rset_set(blockedby, left);
	    left = RID_NONE;
	  } else {  /* Otherwise grab register from invariant. */
	    ra_restore(as, ref);
	    checkmclim(as);
	  }
	}
	if (ra_hasreg(left)) {
	  ra_rename(as, left, r);
	  checkmclim(as);
	}
      }
      rset_clear(phiset, r);
    }
    if (!blocked) break;  /* Finished. */
    if (!(as->freeset & blocked)) {  /* Break cycles if none are free. */
      asm_phi_break(as, blocked, blockedby, RSET_GPR);
      if (!LJ_SOFTFP) asm_phi_break(as, blocked, blockedby, RSET_FPR);
      checkmclim(as);
    }  /* Else retry some more renames. */
  }

  /* Restore/remat invariants whose registers are modified inside the loop. */
#if !LJ_SOFTFP
  work = as->modset & ~(as->freeset | as->phiset) & RSET_FPR;
  while (work) {
    Reg r = rset_pickbot(work);
    ra_restore(as, regcost_ref(as->cost[r]));
    rset_clear(work, r);
    checkmclim(as);
  }
#endif
  work = as->modset & ~(as->freeset | as->phiset);
  while (work) {
    Reg r = rset_pickbot(work);
    ra_restore(as, regcost_ref(as->cost[r]));
    rset_clear(work, r);
    checkmclim(as);
  }

  /* Allocate and save all unsaved PHI regs and clear marks. */
  work = as->phiset;
  while (work) {
    Reg r = rset_picktop(work);
    IRRef lref = as->phireg[r];
    IRIns *ir = IR(lref);
    if (ra_hasspill(ir->s)) {  /* Left PHI gained a spill slot? */
      irt_clearmark(ir->t);  /* Handled here, so clear marker now. */
      ra_alloc1(as, lref, RID2RSET(r));
      ra_save(as, ir, r);  /* Save to spill slot inside the loop. */
      checkmclim(as);
    }
    rset_clear(work, r);
  }
}

/* Copy unsynced left/right PHI spill slots. Rarely needed. */
static void asm_phi_copyspill(ASMState *as)
{
  int need = 0;
  IRIns *ir;
  for (ir = IR(as->orignins-1); ir->o == IR_PHI; ir--)
    if (ra_hasspill(ir->s) && ra_hasspill(IR(ir->op1)->s))
      need |= irt_isfp(ir->t) ? 2 : 1;  /* Unsynced spill slot? */
  if ((need & 1)) {  /* Copy integer spill slots. */
#if !LJ_TARGET_X86ORX64
    Reg r = RID_TMP;
#else
    Reg r = RID_RET;
    if ((as->freeset & RSET_GPR))
      r = rset_pickbot((as->freeset & RSET_GPR));
    else
      emit_spload(as, IR(regcost_ref(as->cost[r])), r, SPOFS_TMP);
#endif
    for (ir = IR(as->orignins-1); ir->o == IR_PHI; ir--) {
      if (ra_hasspill(ir->s)) {
	IRIns *irl = IR(ir->op1);
	if (ra_hasspill(irl->s) && !irt_isfp(ir->t)) {
	  emit_spstore(as, irl, r, sps_scale(irl->s));
	  emit_spload(as, ir, r, sps_scale(ir->s));
	  checkmclim(as);
	}
      }
    }
#if LJ_TARGET_X86ORX64
    if (!rset_test(as->freeset, r))
      emit_spstore(as, IR(regcost_ref(as->cost[r])), r, SPOFS_TMP);
#endif
  }
#if !LJ_SOFTFP
  if ((need & 2)) {  /* Copy FP spill slots. */
#if LJ_TARGET_X86
    Reg r = RID_XMM0;
#else
    Reg r = RID_FPRET;
#endif
    if ((as->freeset & RSET_FPR))
      r = rset_pickbot((as->freeset & RSET_FPR));
    if (!rset_test(as->freeset, r))
      emit_spload(as, IR(regcost_ref(as->cost[r])), r, SPOFS_TMP);
    for (ir = IR(as->orignins-1); ir->o == IR_PHI; ir--) {
      if (ra_hasspill(ir->s)) {
	IRIns *irl = IR(ir->op1);
	if (ra_hasspill(irl->s) && irt_isfp(ir->t)) {
	  emit_spstore(as, irl, r, sps_scale(irl->s));
	  emit_spload(as, ir, r, sps_scale(ir->s));
	  checkmclim(as);
	}
      }
    }
    if (!rset_test(as->freeset, r))
      emit_spstore(as, IR(regcost_ref(as->cost[r])), r, SPOFS_TMP);
  }
#endif
}

/* Emit renames for left PHIs which are only spilled outside the loop. */
static void asm_phi_fixup(ASMState *as)
{
  RegSet work = as->phiset;
  while (work) {
    Reg r = rset_picktop(work);
    IRRef lref = as->phireg[r];
    IRIns *ir = IR(lref);
    if (irt_ismarked(ir->t)) {
      irt_clearmark(ir->t);
      /* Left PHI gained a spill slot before the loop? */
      if (ra_hasspill(ir->s)) {
	ra_addrename(as, r, lref, as->loopsnapno);
      }
    }
    rset_clear(work, r);
  }
}

/* Setup right PHI reference. */
static void asm_phi(ASMState *as, IRIns *ir)
{
  RegSet allow = ((!LJ_SOFTFP && irt_isfp(ir->t)) ? RSET_FPR : RSET_GPR) &
		 ~as->phiset;
  RegSet afree = (as->freeset & allow);
  IRIns *irl = IR(ir->op1);
  IRIns *irr = IR(ir->op2);
  if (ir->r == RID_SINK)  /* Sink PHI. */
    return;
  /* Spill slot shuffling is not implemented yet (but rarely needed). */
  if (ra_hasspill(irl->s) || ra_hasspill(irr->s))
    lj_trace_err(as->J, LJ_TRERR_NYIPHI);
  /* Leave at least one register free for non-PHIs (and PHI cycle breaking). */
  if ((afree & (afree-1))) {  /* Two or more free registers? */
    Reg r;
    if (ra_noreg(irr->r)) {  /* Get a register for the right PHI. */
      r = ra_allocref(as, ir->op2, allow);
    } else {  /* Duplicate right PHI, need a copy (rare). */
      r = ra_scratch(as, allow);
      emit_movrr(as, irr, r, irr->r);
    }
    ir->r = (uint8_t)r;
    rset_set(as->phiset, r);
    as->phireg[r] = (IRRef1)ir->op1;
    irt_setmark(irl->t);  /* Marks left PHIs _with_ register. */
    if (ra_noreg(irl->r))
      ra_sethint(irl->r, r); /* Set register hint for left PHI. */
  } else {  /* Otherwise allocate a spill slot. */
    /* This is overly restrictive, but it triggers only on synthetic code. */
    if (ra_hasreg(irl->r) || ra_hasreg(irr->r))
      lj_trace_err(as->J, LJ_TRERR_NYIPHI);
    ra_spill(as, ir);
    irr->s = ir->s;  /* Set right PHI spill slot. Sync left slot later. */
  }
}

static void asm_loop_fixup(ASMState *as);
#if LJ_TARGET_X86ORX64 || LJ_TARGET_ARM64
static void asm_xpoll(ASMState *as, IRIns *ir);
#else
#define asm_xpoll(as, ir)	((void)0)
#endif

/* Middle part of a loop. */
static void asm_loop(ASMState *as)
{
  MCode *mcspill;
  /* LOOP is a guard, so the snapno is up to date. */
  as->loopsnapno = as->snapno;
  if (as->gcsteps)
    asm_gc_check(as);
  /* LOOP marks the transition from the variant to the invariant part. */
  as->flagmcp = as->invmcp = NULL;
  as->sectref = 0;
  if (!neverfuse(as)) as->fuseref = 0;
  asm_phi_shuffle(as);
  mcspill = as->mcp;
  asm_phi_copyspill(as);
  asm_loop_fixup(as);
  as->mcloop = as->mcp;
  RA_DBGX((as, "===== LOOP ====="));
  if (!as->realign) RA_DBG_FLUSH();
  if (as->mcp != mcspill)
    emit_jmp(as, mcspill);
}

/* -- Target-specific assembler ------------------------------------------- */

#if LJ_TARGET_ARM64 && defined(LJ_TRACE_TEST_HELPERS)
static uint32_t asm_test_exitstub_mcode_retry_count;

void lj_asm_arm64_test_force_exitstub_mcode_retry(uint32_t count)
{
  la_store32_rel(&asm_test_exitstub_mcode_retry_count, count);
}

static int asm_test_exitstub_mcode_retry_consume(void)
{
  uint32_t count = la_load32_acq(&asm_test_exitstub_mcode_retry_count);
  while (count != 0) {
    uint32_t expect = count;
    if (la_cas32(&asm_test_exitstub_mcode_retry_count, &expect, count-1u,
			 LA_ACQ_REL, LA_ACQ))
      return 1;
    count = expect;
  }
  return 0;
}
#endif

#if LJ_TARGET_X86ORX64
#include "lj_asm_x86.h"
#elif LJ_TARGET_ARM
#include "lj_asm_arm.h"
#elif LJ_TARGET_ARM64
#include "lj_asm_arm64.h"
#elif LJ_TARGET_PPC
#include "lj_asm_ppc.h"
#elif LJ_TARGET_MIPS
#include "lj_asm_mips.h"
#else
#error "Missing assembler for target CPU"
#endif

/* -- Common instruction helpers ------------------------------------------ */

#if !LJ_SOFTFP32
#if !LJ_TARGET_X86ORX64
#define asm_ldexp(as, ir)	asm_callid(as, ir, IRCALL_ldexp)
#endif

static void asm_pow(ASMState *as, IRIns *ir)
{
#if LJ_64 && LJ_HASFFI
  if (!irt_isnum(ir->t))
    asm_callid(as, ir, irt_isi64(ir->t) ? IRCALL_lj_carith_powi64 :
					  IRCALL_lj_carith_powu64);
  else
#endif
  asm_callid(as, ir, IRCALL_pow);
}

static void asm_div(ASMState *as, IRIns *ir)
{
#if LJ_64 && LJ_HASFFI
  if (!irt_isnum(ir->t))
    asm_callid(as, ir, irt_isi64(ir->t) ? IRCALL_lj_carith_divi64 :
					  IRCALL_lj_carith_divu64);
  else
#endif
    asm_fpdiv(as, ir);
}
#endif

static void asm_mod(ASMState *as, IRIns *ir)
{
#if LJ_64 && LJ_HASFFI
  if (!irt_isint(ir->t))
    asm_callid(as, ir, irt_isi64(ir->t) ? IRCALL_lj_carith_modi64 :
					  IRCALL_lj_carith_modu64);
  else
#endif
    asm_callid(as, ir, IRCALL_lj_vm_modi);
}

static void asm_fuseequal(ASMState *as, IRIns *ir)
{
  /* Fuse HREF + EQ/NE. */
  if ((ir-1)->o == IR_HREF && ir->op1 == as->curins-1) {
    as->curins--;
    asm_href(as, ir-1, (IROp)ir->o);
  } else {
    asm_equal(as, ir);
  }
}

static void asm_alen(ASMState *as, IRIns *ir)
{
  asm_callid(as, ir, ir->op2 == REF_NIL ? IRCALL_lj_tab_len :
					  IRCALL_lj_tab_len_hint);
}

/* -- Instruction dispatch ------------------------------------------------ */

static void asm_xsave(ASMState *as);

/* Assemble a single instruction. */
static void asm_ir(ASMState *as, IRIns *ir)
{
  switch ((IROp)ir->o) {
  /* Miscellaneous ops. */
  case IR_LOOP: asm_loop(as); break;
  case IR_XPOLL: asm_xpoll(as, ir); break;
  case IR_XSAVE: asm_xsave(as); break;
  case IR_NOP: case IR_XBAR:
    lj_assertA(!ra_used(ir),
	       "IR %04d not unused", (int)(ir - as->ir) - REF_BIAS);
    break;
  case IR_USE:
    ra_alloc1(as, ir->op1, irt_isfp(ir->t) ? RSET_FPR : RSET_GPR); break;
  case IR_PHI: asm_phi(as, ir); break;
  case IR_HIOP: asm_hiop(as, ir); break;
  case IR_GCSTEP: asm_gcstep(as, ir); break;
  case IR_PROF: asm_prof(as, ir); break;

  /* Guarded assertions. */
  case IR_LT: case IR_GE: case IR_LE: case IR_GT:
  case IR_ULT: case IR_UGE: case IR_ULE: case IR_UGT:
  case IR_ABC:
    asm_comp(as, ir);
    break;
  case IR_EQ: case IR_NE: asm_fuseequal(as, ir); break;

  case IR_RETF: asm_retf(as, ir); break;

  /* Bit ops. */
  case IR_BNOT: asm_bnot(as, ir); break;
  case IR_BSWAP: asm_bswap(as, ir); break;
  case IR_BAND: asm_band(as, ir); break;
  case IR_BOR: asm_bor(as, ir); break;
  case IR_BXOR: asm_bxor(as, ir); break;
  case IR_BSHL: asm_bshl(as, ir); break;
  case IR_BSHR: asm_bshr(as, ir); break;
  case IR_BSAR: asm_bsar(as, ir); break;
  case IR_BROL: asm_brol(as, ir); break;
  case IR_BROR: asm_bror(as, ir); break;

  /* Arithmetic ops. */
  case IR_ADD: asm_add(as, ir); break;
  case IR_SUB: asm_sub(as, ir); break;
  case IR_MUL: asm_mul(as, ir); break;
  case IR_MOD: asm_mod(as, ir); break;
  case IR_NEG: asm_neg(as, ir); break;
#if LJ_SOFTFP32
  case IR_DIV: case IR_POW: case IR_ABS:
  case IR_LDEXP: case IR_FPMATH: case IR_TOBIT:
    /* Unused for LJ_SOFTFP32. */
    lj_assertA(0, "IR %04d with unused op %d",
		  (int)(ir - as->ir) - REF_BIAS, ir->o);
    break;
#else
  case IR_DIV: asm_div(as, ir); break;
  case IR_POW: asm_pow(as, ir); break;
  case IR_ABS: asm_abs(as, ir); break;
  case IR_LDEXP: asm_ldexp(as, ir); break;
  case IR_FPMATH: asm_fpmath(as, ir); break;
  case IR_TOBIT: asm_tobit(as, ir); break;
#endif
  case IR_MIN: asm_min(as, ir); break;
  case IR_MAX: asm_max(as, ir); break;

  /* Overflow-checking arithmetic ops. */
  case IR_ADDOV: asm_addov(as, ir); break;
  case IR_SUBOV: asm_subov(as, ir); break;
  case IR_MULOV: asm_mulov(as, ir); break;

  /* Memory references. */
  case IR_AREF: asm_aref(as, ir); break;
  case IR_HREF: asm_href(as, ir, 0); break;
  case IR_HREFK: asm_hrefk(as, ir); break;
  case IR_NEWREF: asm_newref(as, ir); break;
  case IR_UREFO: case IR_UREFC: asm_uref(as, ir); break;
  case IR_FREF: asm_fref(as, ir); break;
  case IR_TMPREF: asm_tmpref(as, ir); break;
  case IR_STRREF: asm_strref(as, ir); break;
  case IR_LREF: asm_lref(as, ir); break;

  /* Loads and stores. */
  case IR_ALOAD: case IR_HLOAD: case IR_ULOAD: case IR_VLOAD:
    asm_ahuvload(as, ir);
    break;
  case IR_FLOAD: asm_fload(as, ir); break;
  case IR_XLOAD: asm_xload(as, ir); break;
  case IR_SLOAD: asm_sload(as, ir); break;
  case IR_ALEN: asm_alen(as, ir); break;

  case IR_ASTORE: case IR_HSTORE: case IR_USTORE: asm_ahustore(as, ir); break;
  case IR_FSTORE: asm_fstore(as, ir); break;
  case IR_XSTORE: asm_xstore(as, ir); break;

  /* Allocations. */
  case IR_SNEW: case IR_XSNEW: asm_snew(as, ir); break;
  case IR_TNEW: asm_tnew(as, ir); break;
  case IR_TDUP: asm_tdup(as, ir); break;
  case IR_CNEW: case IR_CNEWI:
#if LJ_HASFFI
    asm_cnew(as, ir);
#else
    lj_assertA(0, "IR %04d with unused op %d",
		  (int)(ir - as->ir) - REF_BIAS, ir->o);
#endif
    break;

  /* Buffer operations. */
  case IR_BUFHDR: asm_bufhdr(as, ir); break;
  case IR_BUFPUT: asm_bufput(as, ir); break;
  case IR_BUFSTR: asm_bufstr(as, ir); break;

  /* Write barriers. */
  case IR_TBAR: asm_tbar(as, ir); break;
  case IR_OBAR: asm_obar(as, ir); break;

  /* Type conversions. */
  case IR_CONV: asm_conv(as, ir); break;
  case IR_TOSTR: asm_tostr(as, ir); break;
  case IR_STRTO: asm_strto(as, ir); break;

  /* Calls. */
  case IR_CALLA:
    as->gcsteps++;
    /* fallthrough */
  case IR_CALLN: case IR_CALLL: case IR_CALLS:
#if LJ_TARGET_X86ORX64
    if (asm_call_inline_x86(as, ir))
      break;
#endif
    asm_call(as, ir);
    break;
  case IR_CALLXS: asm_callx(as, ir); break;
  case IR_CARG: break;

  default:
    setintV(&as->J->errinfo, ir->o);
    lj_trace_err_info(as->J, LJ_TRERR_NYIIR);
    break;
  }
}

/* -- Head of trace ------------------------------------------------------- */

/* Head of a root trace. */
static void asm_head_root(ASMState *as)
{
  int32_t spadj;
  asm_head_root_base(as);
  emit_setvmstate_root(as, (int32_t)as->T->traceno);
  spadj = asm_stack_adjust(as);
  as->T->spadjust = (uint16_t)spadj;
  emit_spsub(as, spadj);
  /* Root traces assume a checked stack for the starting proto. */
  as->T->topslot = trace_startpt_acq(as->T)->framesize;
}

/* Head of a side trace.
**
** The current simplistic algorithm requires that all slots inherited
** from the parent are live in a register between pass 2 and pass 3. This
** avoids the complexity of stack slot shuffling. But of course this may
** overflow the register set in some cases and cause the dreaded error:
** "NYI: register coalescing too complex". A refined algorithm is needed.
*/
static void asm_head_side(ASMState *as)
{
  IRRef1 sloadins[RID_MAX];
  RegSet allow = RSET_ALL;  /* Inverse of all coalesced registers. */
  RegSet live = RSET_EMPTY;  /* Live parent registers. */
  RegSet pallow = RSET_GPR;  /* Registers needed by the parent stack check. */
  Reg pbase;
  IRIns *irp = &trace_ir_acq(as->parent)[REF_BASE];  /* Parent base. */
  MSize parent_topslot = trace_topslot_acq(as->parent);
  int32_t parent_spadjust = (int32_t)trace_spadjust_acq(as->parent);
  int32_t spadj, spdelta;
  int pass2 = 0;
  int pass3 = 0;
  IRRef i;

  if (as->snapno && as->topslot > parent_topslot) {
    /* Force snap #0 alloc to prevent register overwrite in stack check. */
    asm_snap_alloc(as, 0);
  }
  pbase = asm_head_side_base(as, irp);
  if (pbase != RID_NONE) {
    rset_clear(allow, pbase);
    rset_clear(pallow, pbase);
  }

  /* Scan all parent SLOADs and collect register dependencies. */
  for (i = as->stopins; i > REF_BASE; i--) {
    IRIns *ir = IR(i);
    RegSP rs;
    lj_assertA((ir->o == IR_SLOAD && (ir->op2 & IRSLOAD_PARENT)) ||
	       (LJ_SOFTFP && ir->o == IR_HIOP) || ir->o == IR_PVAL,
	       "IR %04d has bad parent op %d",
	       (int)(ir - as->ir) - REF_BIAS, ir->o);
    rs = as->parentmap[i - REF_FIRST];
    if (ra_hasreg(ir->r)) {
      rset_clear(allow, ir->r);
      if (ra_hasspill(ir->s)) {
	ra_save(as, ir, ir->r);
	checkmclim(as);
      }
    } else if (ra_hasspill(ir->s)) {
      irt_setmark(ir->t);
      pass2 = 1;
    }
    if (ir->r == rs) {  /* Coalesce matching registers right now. */
      ra_free(as, ir->r);
    } else if (ra_hasspill(regsp_spill(rs))) {
      if (ra_hasreg(ir->r))
	pass3 = 1;
    } else if (ra_used(ir)) {
      sloadins[rs] = (IRRef1)i;
      rset_set(live, rs);  /* Block live parent register. */
    }
    if (!ra_hasspill(regsp_spill(rs))) rset_clear(pallow, regsp_reg(rs));
  }

  /* Calculate stack frame adjustment. */
  spadj = asm_stack_adjust(as);
  spdelta = spadj - parent_spadjust;
  if (spdelta < 0) {  /* Don't shrink the stack frame. */
    spadj = parent_spadjust;
    spdelta = 0;
  }
  as->T->spadjust = (uint16_t)spadj;

  /* Reload spilled target registers. */
  if (pass2) {
    for (i = as->stopins; i > REF_BASE; i--) {
      IRIns *ir = IR(i);
      if (irt_ismarked(ir->t)) {
	RegSet mask;
	Reg r;
	RegSP rs;
	irt_clearmark(ir->t);
	rs = as->parentmap[i - REF_FIRST];
	if (!ra_hasspill(regsp_spill(rs)))
	  ra_sethint(ir->r, rs);  /* Hint may be gone, set it again. */
	else if (sps_scale(regsp_spill(rs))+spdelta == sps_scale(ir->s))
	  continue;  /* Same spill slot, do nothing. */
	mask = ((!LJ_SOFTFP && irt_isfp(ir->t)) ? RSET_FPR : RSET_GPR) & allow;
	if (mask == RSET_EMPTY)
	  lj_trace_err(as->J, LJ_TRERR_NYICOAL);
	r = ra_allocref(as, i, mask);
	ra_save(as, ir, r);
	rset_clear(allow, r);
	if (r == rs) {  /* Coalesce matching registers right now. */
	  ra_free(as, r);
	  rset_clear(live, r);
	} else if (ra_hasspill(regsp_spill(rs))) {
	  pass3 = 1;
	}
	checkmclim(as);
      }
    }
  }

  /* Store trace number and adjust stack frame relative to the parent. */
  emit_setvmstate(as, (int32_t)as->T->traceno);
  emit_spsub(as, spdelta);
  checkmclim(as);

#if !LJ_TARGET_X86ORX64
  /* Restore BASE register from parent spill slot. */
  if (ra_hasspill(irp->s))
    emit_spload(as, IR(REF_BASE), IR(REF_BASE)->r, sps_scale(irp->s));
#endif

  /* Restore target registers from parent spill slots. */
  if (pass3) {
    RegSet work = ~as->freeset & RSET_ALL;
    while (work) {
      Reg r = rset_pickbot(work);
      IRRef ref = regcost_ref(as->cost[r]);
      RegSP rs = as->parentmap[ref - REF_FIRST];
      rset_clear(work, r);
      if (ra_hasspill(regsp_spill(rs))) {
	int32_t ofs = sps_scale(regsp_spill(rs));
	ra_free(as, r);
	emit_spload(as, IR(ref), r, ofs);
	checkmclim(as);
      }
    }
  }

  /* Shuffle registers to match up target regs with parent regs. */
  for (;;) {
    RegSet work;

    /* Repeatedly coalesce free live registers by moving to their target. */
    while ((work = as->freeset & live) != RSET_EMPTY) {
      Reg rp = rset_pickbot(work);
      IRIns *ir = IR(sloadins[rp]);
      rset_clear(live, rp);
      rset_clear(allow, rp);
      ra_free(as, ir->r);
      emit_movrr(as, ir, ir->r, rp);
      checkmclim(as);
    }

    /* We're done if no live registers remain. */
    if (live == RSET_EMPTY)
      break;

    /* Break cycles by renaming one target to a temp. register. */
    checkmclim(as);
    if (live & RSET_GPR) {
      RegSet tmpset = as->freeset & ~live & allow & RSET_GPR;
      if (tmpset == RSET_EMPTY)
	lj_trace_err(as->J, LJ_TRERR_NYICOAL);
      ra_rename(as, rset_pickbot(live & RSET_GPR), rset_pickbot(tmpset));
      checkmclim(as);
    }
    if (!LJ_SOFTFP && (live & RSET_FPR)) {
      RegSet tmpset = as->freeset & ~live & allow & RSET_FPR;
      if (tmpset == RSET_EMPTY)
	lj_trace_err(as->J, LJ_TRERR_NYICOAL);
      checkmclim(as);
      ra_rename(as, rset_pickbot(live & RSET_FPR), rset_pickbot(tmpset));
      checkmclim(as);
    }
    /* Continue with coalescing to fix up the broken cycle(s). */
  }

  /* Inherit top stack slot already checked by parent trace. */
  as->T->topslot = (uint8_t)parent_topslot;
  if (as->topslot > as->T->topslot) {  /* Need to check for higher slot? */
#ifdef EXITSTATE_CHECKEXIT
    /* Highest exit + 1 indicates stack check. */
    ExitNo exitno = as->T->nsnap;
#else
    /* Reuse the parent exit in the context of the parent trace. */
    ExitNo exitno = as->J->exitno;
#endif
    as->T->topslot = (uint8_t)as->topslot;  /* Remember for child traces. */
    checkmclim(as);
    asm_stack_check(as, as->topslot, irp, pallow, exitno);
    checkmclim(as);
  }
}

/* -- Tail of trace ------------------------------------------------------- */

/* Get base slot for a snapshot. */
static BCReg asm_baseslot(ASMState *as, SnapShot *snap, int *gotframe)
{
  SnapEntry *map = &as->T->snapmap[snap->mapofs];
  MSize n;
  for (n = snap->nent; n > 0; n--) {
    SnapEntry sn = map[n-1];
    if ((sn & SNAP_FRAME)) {
      *gotframe = 1;
      return snap_slot(sn) - LJ_FR2;
    }
  }
  return 0;
}

/* Restore and stage the exact stack root for a future native publication.
** Snapshot numbers are intentionally derived from the backwards assembler
** cursor: LOOP unrolling copies/reindexes snapshots and makes a recorded
** literal snapshot number stale.
*/
static void asm_xsave(ASMState *as)
{
#if LJ_TARGET_X64
  SnapShot *snap;
  BCReg baseslot;
  int gotframe = 0;
  lj_assertA(as->snapno < as->T->nsnap,
	     "XSAVE IR %04d has no active snapshot",
	     as->curins - REF_BIAS);
  if (LJ_UNLIKELY(as->snapno >= as->T->nsnap))
    lj_trace_err(as->J, LJ_TRERR_BADRA);
  snap = &as->T->snap[as->snapno];
  lj_assertA(snap->ref == as->curins,
	     "XSAVE IR %04d mismatches snapshot %d ref %04d",
	     as->curins - REF_BIAS, as->snapno, snap->ref - REF_BIAS);
  lj_assertA(snap->nslots >= 1 + LJ_FR2, "XSAVE has short stack extent");
  if (LJ_UNLIKELY(snap->ref != as->curins ||
		  snap->nslots < 1 + LJ_FR2))
    lj_trace_err(as->J, LJ_TRERR_BADRA);
  asm_snap_prep(as);
  baseslot = asm_baseslot(as, snap, &gotframe);
  lj_assertA(gotframe || baseslot == 0, "XSAVE has inconsistent frame base");
  lj_assertA(baseslot <= snap->nslots - 1 - LJ_FR2,
	     "XSAVE frame base exceeds stack extent");
  if (LJ_UNLIKELY((!gotframe && baseslot != 0) ||
		  baseslot > snap->nslots - 1 - LJ_FR2))
    lj_trace_err(as->J, LJ_TRERR_BADRA);
  asm_xsave_restore_publish(as, snap, baseslot);
#else
  setintV(&as->J->errinfo, IR_XSAVE);
  lj_trace_err_info(as->J, LJ_TRERR_NYIIR);
#endif
}

static GCtrace *asm_traceref_live(ASMState *as, TraceNo traceno)
{
  jit_State *J = as->J;
  global_State *g = J2G(J);
  GCtrace *T;
  /* Assembly is speculative. Abort this turn instead of waiting while holding
  ** the recorder token if an exclusive trace-body reclaimer won admission. */
  if (LJ_UNLIKELY(!lj_gc2_smr_read_try(g)))
    lj_trace_err(as->J, LJ_TRERR_SMRRETRY);
  T = traceref_safe(J, traceno);
  if (LJ_UNLIKELY(!trace_runnable_acq(T, traceno))) {
    lj_gc2_smr_read_leave(g);
    lj_trace_err(as->J, LJ_TRERR_RETRY);
  }
  lj_gc2_smr_read_leave(g);
  return T;
}

/* Link to another trace. */
static void asm_tail_link(ASMState *as)
{
  SnapNo snapno = as->T->nsnap-1;  /* Last snapshot. */
  SnapShot *snap = &as->T->snap[snapno];
  int gotframe = 0;
  BCReg baseslot = asm_baseslot(as, snap, &gotframe);

  as->topslot = snap->topslot;
  checkmclim(as);
  ra_allocref(as, REF_BASE, RID2RSET(RID_BASE));
  checkmclim(as);

  if (as->T->link == 0) {
    /* Setup fixed registers for exit to interpreter. */
    const BCIns *pc = snap_pc(&as->T->snapmap[snap->mapofs + snap->nent]);
    int32_t mres;
    if (bc_op(*pc) == BC_JLOOP) {  /* NYI: find a better way to do this. */
      TraceNo targetno = bc_d(*pc);
      global_State *g = J2G(as->J);
      GCtrace *target;
      /* The target-return shortcut is optional. Keep the snapshot PC when a
      ** concurrent exclusive reclaimer closes one-shot SMR admission. */
      if (lj_gc2_smr_read_try(g)) {
	target = traceref_safe(as->J, targetno);
	if (trace_runnable_acq(target, targetno)) {
	  BCIns *retpc = &target->startins;
	  if (bc_isret(bc_op(*retpc)))
	    pc = retpc;
	}
	lj_gc2_smr_read_leave(g);
      }
    }
#if LJ_GC64
    emit_loadu64(as, RID_LPC, u64ptr(pc));
#else
    ra_allockreg(as, i32ptr(J2GG(as->J)->dispatch), RID_DISPATCH);
    ra_allockreg(as, i32ptr(pc), RID_LPC);
#endif
    mres = (int32_t)(snap->nslots - baseslot - LJ_FR2);
    switch (bc_op(*pc)) {
    case BC_CALLM: case BC_CALLMT:
      mres -= (int32_t)(1 + LJ_FR2 + bc_a(*pc) + bc_c(*pc)); break;
    case BC_RETM: mres -= (int32_t)(bc_a(*pc) + bc_d(*pc)); break;
    case BC_TSETM: mres -= (int32_t)bc_a(*pc); break;
    default: if (!bc_isfunc_or_ff(bc_op(*pc))) mres = 0; break;
    }
    ra_allockreg(as, mres, RID_RET);  /* Return MULTRES or 0. */
    checkmclim(as);
  } else if (baseslot) {
    /* Save modified BASE for linking to trace with higher start frame. */
    emit_settg(as, RID_BASE, jit_base);
    checkmclim(as);
  }
  emit_addptr(as, RID_BASE, 8*(int32_t)baseslot);
  checkmclim(as);

  /* Sync the interpreter state with the on-trace state. */
  checkmclim(as);
  asm_stack_restore(as, snap);

  /* Root traces that add frames need to check the stack at the end. */
  if (!as->parent && gotframe)
    asm_stack_check(as, as->topslot, NULL, as->freeset & RSET_GPR, snapno);
}

/* Patch the exact-body constant in every assembly attempt. Loop traces skip
** asm_tail_link(), and an IR-growth retry allocates a fresh curfinal copy, so
** neither tail linking nor a one-time patch is sufficient. Assembly proceeds
** backwards after this point and therefore observes the final KGC value when
** lowering native-entry arguments. */
static void asm_patch_ktrace(ASMState *as)
{
  if (as->J->ktrace) {
    IRIns *ir = IR(as->J->ktrace);
    lj_assertA(ir->o == IR_KNUM || ir->o == IR_KNULL || ir->o == IR_KGC,
	       "bad exact trace constant op %d", ir->o);
    ir_kgc_store_rel(ir, obj2gco(as->J->curfinal));
    la_store8_rel((uint8_t *)&ir->o, IR_KGC);  /* Trace-owned GC root. */
  }
}

/* -- Trace setup --------------------------------------------------------- */

/* Clear reg/sp for all instructions and add register hints. */
static void asm_setup_regsp(ASMState *as)
{
  GCtrace *T = as->T;
  int sink = T->sinktags;
  IRRef nins = T->nins;
  IRIns *ir, *lastir;
  int inloop;
#if LJ_TARGET_ARM
  uint32_t rload = 0xa6402a64;
#endif

  ra_setup(as);
#if LJ_TARGET_ARM64
  ra_setkref(as, RID_GL, (intptr_t)J2G(as->J));
#endif

  /* Clear reg/sp for constants. */
  for (ir = IR(T->nk), lastir = IR(REF_BASE); ir < lastir; ir++) {
    ir->prev = REGSP_INIT;
    if (irt_is64(ir->t) && ir->o != IR_KNULL) {
#if LJ_GC64
      /* The false-positive of irt_is64() for ASMREF_L (REF_NIL) is OK here. */
      ir->i = 0;  /* Will become non-zero only for RIP-relative addresses. */
#else
      /* Make life easier for backends by putting address of constant in i. */
      ir->i = (int32_t)(intptr_t)(ir+1);
#endif
      ir++;
    }
  }

  /* REF_BASE is used for implicit references to the BASE register. */
  lastir->prev = REGSP_HINT(RID_BASE);

  as->snaprename = nins;
  as->snapref = nins;
  as->snapno = T->nsnap;
  as->snapalloc = 0;

  as->stopins = REF_BASE;
  as->orignins = nins;
  as->curins = nins;

  /* Setup register hints for parent link instructions. */
  ir = IR(REF_FIRST);
  if (as->parent) {
    uint16_t *p;
    lastir = lj_snap_regspmap(as->J, as->parent, as->J->exitno, ir);
    if (lastir - ir > LJ_MAX_JSLOTS)
      lj_trace_err(as->J, LJ_TRERR_NYICOAL);
    as->stopins = (IRRef)((lastir-1) - as->ir);
    for (p = as->parentmap; ir < lastir; ir++) {
      RegSP rs = ir->prev;
      *p++ = (uint16_t)rs;  /* Copy original parent RegSP to parentmap. */
      if (!ra_hasspill(regsp_spill(rs)))
	ir->prev = (uint16_t)REGSP_HINT(regsp_reg(rs));
      else
	ir->prev = REGSP_INIT;
    }
  }

  inloop = 0;
  as->evenspill = SPS_FIRST;
  for (lastir = IR(nins); ir < lastir; ir++) {
    if (sink) {
      if (ir->r == RID_SINK)
	continue;
      if (ir->r == RID_SUNK) {  /* Revert after ASM restart. */
	ir->r = RID_SINK;
	continue;
      }
    }
    switch (ir->o) {
    case IR_LOOP:
      inloop = 1;
      break;
#if LJ_TARGET_ARM
    case IR_SLOAD:
      if (!((ir->op2 & IRSLOAD_TYPECHECK) || (ir+1)->o == IR_HIOP))
	break;
      /* fallthrough */
    case IR_ALOAD: case IR_HLOAD: case IR_ULOAD: case IR_VLOAD:
      if (!LJ_SOFTFP && irt_isnum(ir->t)) break;
      ir->prev = (uint16_t)REGSP_HINT((rload & 15));
      rload = lj_ror(rload, 4);
      continue;
    case IR_TMPREF:
      if ((ir->op2 & IRTMPREF_OUT2) && as->evenspill < 4)
	as->evenspill = 4;  /* TMPREF OUT2 needs two TValues on the stack. */
      break;
#endif
    case IR_CALLXS: {
      CCallInfo ci;
      ci.flags = asm_callx_flags(as, ir);
      ir->prev = asm_setup_call_slots(as, ir, &ci);
      if (inloop)
	as->modset |= RSET_SCRATCH;
      continue;
      }
    case IR_CALLL:
      /* lj_vm_next needs two TValues on the stack. */
#if LJ_TARGET_X64 && LJ_ABI_WIN
      if (ir->op2 == IRCALL_lj_vm_next && as->evenspill < SPS_FIRST + 4)
	as->evenspill = SPS_FIRST + 4;
#else
      if (SPS_FIRST < 4 && ir->op2 == IRCALL_lj_vm_next && as->evenspill < 4)
	as->evenspill = 4;
#endif
      /* fallthrough */
    case IR_CALLN: case IR_CALLA: case IR_CALLS: {
      const CCallInfo *ci = &lj_ir_callinfo[ir->op2];
      ir->prev = asm_setup_call_slots(as, ir, ci);
      if (inloop)
	as->modset |= (ci->flags & CCI_NOFPRCLOBBER) ?
		      (RSET_SCRATCH & ~RSET_FPR) : RSET_SCRATCH;
      continue;
      }
    case IR_HIOP:
      switch ((ir-1)->o) {
#if LJ_SOFTFP && LJ_TARGET_ARM
      case IR_SLOAD: case IR_ALOAD: case IR_HLOAD: case IR_ULOAD: case IR_VLOAD:
	if (ra_hashint((ir-1)->r)) {
	  ir->prev = (ir-1)->prev + 1;
	  continue;
	}
	break;
#endif
#if !LJ_SOFTFP && LJ_NEED_FP64 && LJ_32 && LJ_HASFFI
      case IR_CONV:
	if (irt_isfp((ir-1)->t)) {
	  ir->prev = REGSP_HINT(RID_FPRET);
	  continue;
	}
#endif
      /* fallthrough */
      case IR_CALLN: case IR_CALLL: case IR_CALLS: case IR_CALLXS:
#if LJ_SOFTFP
      case IR_MIN: case IR_MAX:
#endif
	(ir-1)->prev = REGSP_HINT(RID_RETLO);
	ir->prev = REGSP_HINT(RID_RETHI);
	continue;
      default:
	break;
      }
      break;
#if LJ_SOFTFP
    case IR_MIN: case IR_MAX:
      if ((ir+1)->o != IR_HIOP) break;
#endif
    /* fallthrough */
    /* C calls evict all scratch regs and return results in RID_RET. */
    case IR_SNEW: case IR_XSNEW: case IR_NEWREF: case IR_BUFPUT:
      if (REGARG_NUMGPR < 3 && as->evenspill < 3)
	as->evenspill = 3;  /* lj_str_new and lj_tab_newkey need 3 args. */
#if LJ_TARGET_X86 && LJ_HASFFI
      if (0) {
    case IR_CNEW:
	if (ir->op2 != REF_NIL && as->evenspill < 4)
	  as->evenspill = 4;  /* lj_cdata_newv needs 4 args. */
	else if (as->evenspill < 3)
	  as->evenspill = 3;  /* lj_cdata_new_forjit needs 3 args. */
      }
      /* fallthrough */
#else
      /* fallthrough */
    case IR_CNEW:
#endif
      /* fallthrough */
    case IR_TNEW: case IR_TDUP: case IR_CNEWI: case IR_TOSTR:
    case IR_BUFSTR:
      ir->prev = REGSP_HINT(RID_RET);
      if (inloop)
	as->modset = RSET_SCRATCH;
      continue;
    case IR_STRTO: case IR_TBAR: case IR_OBAR:
      if (inloop)
	as->modset = RSET_SCRATCH;
      break;
#if !LJ_SOFTFP
#if !LJ_TARGET_X86ORX64
    case IR_LDEXP:
#endif
#endif
      /* fallthrough */
    case IR_POW:
      if (!LJ_SOFTFP && irt_isnum(ir->t)) {
	if (inloop)
	  as->modset |= RSET_SCRATCH;
#if LJ_TARGET_X86
	if (irt_isnum(IR(ir->op2)->t)) {
	  if (as->evenspill < 4)  /* Leave room to call pow(). */
	    as->evenspill = 4;
	}
	break;
#else
	ir->prev = REGSP_HINT(RID_FPRET);
	continue;
#endif
      }
      /* fallthrough */ /* for integer POW */
    case IR_DIV: case IR_MOD:
      if ((LJ_64 && LJ_SOFTFP) || !irt_isnum(ir->t)) {
	ir->prev = REGSP_HINT(RID_RET);
	if (inloop)
	  as->modset |= (RSET_SCRATCH & RSET_GPR);
	continue;
      }
      break;
#if LJ_64 && LJ_SOFTFP
    case IR_ADD: case IR_SUB: case IR_MUL:
      if (irt_isnum(ir->t)) {
	ir->prev = REGSP_HINT(RID_RET);
	if (inloop)
	  as->modset |= (RSET_SCRATCH & RSET_GPR);
	continue;
      }
      break;
#endif
    case IR_FPMATH:
#if LJ_TARGET_X86ORX64
      if (ir->op2 <= IRFPM_TRUNC) {
	if (!(as->flags & JIT_F_SSE4_1)) {
	  ir->prev = REGSP_HINT(RID_XMM0);
	  if (inloop)
	    as->modset |= RSET_RANGE(RID_XMM0, RID_XMM3+1)|RID2RSET(RID_EAX);
	  continue;
	}
	break;
      }
#endif
      if (inloop)
	as->modset |= RSET_SCRATCH;
#if LJ_TARGET_X86
      break;
#else
      ir->prev = REGSP_HINT(RID_FPRET);
      continue;
#endif
#if LJ_TARGET_X86ORX64
    /* Non-constant shift counts need to be in RID_ECX on x86/x64. */
    case IR_BSHL: case IR_BSHR: case IR_BSAR:
      if ((as->flags & JIT_F_BMI2))  /* Except if BMI2 is available. */
	break;
      /* fallthrough */
    case IR_BROL: case IR_BROR:
      if (!irref_isk(ir->op2) && !ra_hashint(IR(ir->op2)->r)) {
	IR(ir->op2)->r = REGSP_HINT(RID_ECX);
	if (inloop)
	  rset_set(as->modset, RID_ECX);
      }
      break;
#endif
    /* Do not propagate hints across type conversions or loads. */
    case IR_TOBIT:
    case IR_XLOAD:
#if !LJ_TARGET_ARM
    case IR_ALOAD: case IR_HLOAD: case IR_ULOAD: case IR_VLOAD:
#endif
      break;
    case IR_CONV:
      if (irt_isfp(ir->t) || (ir->op2 & IRCONV_SRCMASK) == IRT_NUM ||
	  (ir->op2 & IRCONV_SRCMASK) == IRT_FLOAT)
	break;
      /* fallthrough */
    default:
      /* Propagate hints across likely 'op reg, imm' or 'op reg'. */
      if (irref_isk(ir->op2) && !irref_isk(ir->op1) &&
	  ra_hashint(regsp_reg(IR(ir->op1)->prev))) {
	ir->prev = IR(ir->op1)->prev;
	continue;
      }
      break;
    }
    ir->prev = REGSP_INIT;
  }
  if ((as->evenspill & 1))
    as->oddspill = as->evenspill++;
  else
    as->oddspill = 0;
}

/* -- Assembler core ------------------------------------------------------ */

/* Assemble a trace. */
void lj_asm_trace(jit_State *J, GCtrace *T)
{
  ASMState as_;
  ASMState *as = &as_;
#if LJ_TARGET_ARM64
  IRRef arm64_semantic_nins;
#endif

  /* Remove nops/renames left over from ASM restart due to LJ_TRERR_MCODELM. */
  {
    IRRef nins = T->nins;
    IRIns *ir = &T->ir[nins-1];
    if (ir->o == IR_NOP || ir->o == IR_RENAME) {
      do { ir--; nins--; } while (ir->o == IR_NOP || ir->o == IR_RENAME);
      T->nins = nins;
    }
  }

#if LJ_TARGET_ARM64
  {
    LJArm64IRReject reject;
    if (LJ_UNLIKELY(!lj_asm_arm64_ir_admit(J, T, &reject))) {
      /* Use the established deterministic assembler error. The exact rejected
      ** opcode remains available through errinfo; CALL helper IDs and shape
      ** details are exposed by the pure admission result used by diagnostics. */
      setintV(&J->errinfo, (int32_t)reject.op);
      lj_trace_err_info(J, LJ_TRERR_NYIIR);
    }
  }
  arm64_semantic_nins = T->nins;
#endif

  /* Ensure an initialized instruction beyond the last one for HIOP checks. */
  /* This also allows one RENAME to be added without reallocating curfinal. */
  as->orignins = lj_ir_nextins(J);
  lj_ir_nop(&J->cur.ir[as->orignins]);

  /* Setup initial state. Copy some fields to reduce indirections. */
  as->J = J;
  as->T = T;
  J->curfinal = lj_trace_alloc(J->L, T);  /* This copies the IR, too. */
  as->flags = jit_flags_acq(J);
  as->loopref = J->loopref;
  as->realign = NULL;
  as->loopinv = 0;
  as->parent = J->parent ? asm_traceref_live(as, J->parent) : NULL;
#ifdef LUAJIT_RANDOM_RA
  (void)lj_prng_u64(&J2TG(J)->prng);  /* Ensure PRNG step between traces. */
#endif

  /* Reserve MCode memory. */
  as->mctop = as->mctoporig = lj_mcode_reserve(J, &as->mcbot);
  as->mcp = as->mctop;
  as->mclim = as->mcbot + MCLIM_REDZONE;
  asm_setup_target(as);

  /*
  ** This is a loop, because the MCode may have to be (re-)assembled
  ** multiple times:
  **
  ** 1. as->realign is set (and the assembly aborted), if the arch-specific
  **    backend wants the MCode to be aligned differently.
  **
  **    This is currently only the case on x86/x64, where small loops get
  **    an aligned loop body plus a short branch. Not much effort is wasted,
  **    because the abort happens very quickly and only once.
  **
  ** 2. The IR is immovable, since the MCode embeds pointers to various
  **    constants inside the IR. But RENAMEs may need to be added to the IR
  **    during assembly, which might grow and reallocate the IR. We check
  **    at the end if the IR (in J->cur.ir) has actually grown, resize the
  **    copy (in J->curfinal.ir) and try again.
  **
  **    95% of all traces have zero RENAMEs, 3% have one RENAME, 1.5% have
  **    2 RENAMEs and only 0.5% have more than that. That's why we opt to
  **    always have one spare slot in the IR (see above), which means we
  **    have to redo the assembly for only ~2% of all traces.
  **
  **    Very, very rarely, this needs to be done repeatedly, since the
  **    location of constants inside the IR (actually, reachability from
  **    a global pointer) may affect register allocation and thus the
  **    number of RENAMEs.
  */
  for (;;) {
    as->mcp = as->mctop;
#ifdef LUA_USE_ASSERT
    as->mcp_prev = as->mcp;
#endif
    as->ir = J->curfinal->ir;  /* Use the copied IR. */
    asm_patch_ktrace(as);
    as->curins = J->cur.nins = as->orignins;
#ifdef LUAJIT_RANDOM_RA
    as->prngstate = J2TG(J)->prng;  /* Must (re)start from identical state. */
    as->prngbits = 0;
#endif

    RA_DBG_START();
    RA_DBGX((as, "===== STOP ====="));

    /* General trace setup. Emit tail of trace. */
    asm_tail_prep(as, T->link);
    as->mcloop = NULL;
    as->flagmcp = NULL;
    as->topslot = 0;
    as->gcsteps = 0;
    as->sectref = as->loopref;
    as->fuseref = (as->flags & JIT_F_OPT_FUSE) ? as->loopref : FUSE_DISABLED;
    asm_setup_regsp(as);
    if (!as->loopref)
      asm_tail_link(as);

    /* Assemble a trace in linear backwards order. */
    for (as->curins--; as->curins > as->stopins; as->curins--) {
      IRIns *ir = IR(as->curins);
      /* 64 bit types handled by SPLIT for 32 bit archs. */
      lj_assertA(!(LJ_32 && irt_isint64(ir->t)),
		 "IR %04d has unsplit 64 bit type",
		 (int)(ir - as->ir) - REF_BIAS);
      asm_snap_prev(as);
      if (!ra_used(ir) && !ir_sideeff(ir) && (as->flags & JIT_F_OPT_DCE))
	continue;  /* Dead-code elimination can be soooo easy. */
      if (irt_isguard(ir->t))
	asm_snap_prep(as);
      RA_DBG_REF();
      checkmclim(as);
      asm_ir(as, ir);
    }

    if (as->realign && J->curfinal->nins >= T->nins)
      continue;  /* Retry in case only the MCode needs to be realigned. */

    /* Emit head of trace. */
    RA_DBG_REF();
    checkmclim(as);
    if (as->gcsteps > 0) {
      as->curins = as->T->snap[0].ref;
      asm_snap_prep(as);  /* The GC check is a guard. */
      checkmclim(as);  /* M6: split trace-head GC check after snapshot prep. */
      asm_gc_check(as);
      as->curins = as->stopins;
    }
    ra_evictk(as);
    if (as->parent)
      asm_head_side(as);
    else
      asm_head_root(as);
#if LJ_ABI_BRANCH_TRACK
    emit_branch_track(as);
#endif
    asm_phi_fixup(as);

    if (J->curfinal->nins >= T->nins) {  /* IR didn't grow? */
      lj_assertA(J->curfinal->nk == T->nk, "unexpected IR constant growth");
      memcpy(J->curfinal->ir + as->orignins, T->ir + as->orignins,
	     (T->nins - as->orignins) * sizeof(IRIns));  /* Copy RENAMEs. */
      T->nins = J->curfinal->nins;
      /* Fill mcofs of any unprocessed snapshots. */
      as->curins = REF_FIRST;
      asm_snap_prev(as);
      break;  /* Done. */
    }

    /* Otherwise try again with a bigger IR. */
    {
      GCtrace *scratch = J->curfinal;
      /* Close the token-private pointer before the exact raw retire-list
      ** publication. A pre-clear observer remains covered by its epoch. */
      J->curfinal = NULL;  /* Also protects an OOM in the replacement alloc. */
      lj_trace_free_unpublished(J2G(J), scratch);
    }
    J->curfinal = lj_trace_alloc(J->L, T);
    as->realign = NULL;
  }

  RA_DBGX((as, "===== START ===="));
  RA_DBG_FLUSH();
  if (as->freeset != RSET_ALL)
    lj_trace_err(as->J, LJ_TRERR_BADRA);  /* Ouch! Should never happen. */

#if LJ_TARGET_ARM64
  {
    GCtrace finalview = *T;
    LJArm64PostRAView postraview;
    LJArm64IRReject reject;
    IRIns *finalir = J->curfinal->ir;
    IRRef finalnins = T->nins;
    IRRef validated_semantic_nins;

    /* Re-run the complete semantic policy against the register-allocated
    ** compact IR. Header and snapshot authority still comes from the current
    ** recorder trace; RENAME/NOP capacity lies beyond this semantic view. */
    finalview.ir = finalir;
    finalview.nins = arm64_semantic_nins;
    if (LJ_UNLIKELY(!lj_asm_arm64_ir_admit(J, &finalview, &reject))) {
      setintV(&J->errinfo, (int32_t)reject.op);
      lj_trace_err_info(J, LJ_TRERR_NYIIR);
    }

    postraview.ir = finalir;
    postraview.snap = T->snap;
    postraview.snapmap = T->snapmap;
    postraview.proto_bc = proto_bc(J->pt);
    postraview.nins = finalnins;
    postraview.nk = T->nk;
    postraview.nsnap = T->nsnap;
    postraview.nsnapmap = T->nsnapmap;
    postraview.spadjust = T->spadjust;
    postraview.proto_sizebc = J->pt->sizebc;
    postraview.root_topslot = T->topslot;
    postraview.startins = T->startins;
    postraview.base_delta = (uint8_t)(J->baseslot-2u);
    if (LJ_UNLIKELY(!lj_asm_arm64_postra_admit(
	  &postraview, &validated_semantic_nins) ||
	validated_semantic_nins != arm64_semantic_nins)) {
      setintV(&J->errinfo, (int32_t)IR_RENAME);
      lj_trace_err_info(J, LJ_TRERR_NYIIR);
    }
    if (bc_op(T->startins) == BC_FORL)
      T->unused1 |= TRACE_ARM64_INT_FORL_ADMITTED;
    else if (bc_op(T->startins) == BC_FUNCF)
      T->unused1 |= TRACE_ARM64_TRUE_FUNCF_ADMITTED;
    else
      T->unused1 |= TRACE_ARM64_INT_LOOP_ADMITTED;
  }
#endif

  /* Set trace entry point before fixing up tail to allow link to self. */
  T->mcode = as->mcp;
  T->mcloop = as->mcloop ? (MSize)((char *)as->mcloop - (char *)as->mcp) : 0;
  if (as->loopref)
    asm_loop_tail_fixup(as);
  else
    asm_tail_fixup(as, T->link);  /* Note: this may change as->mctop! */
  T->szmcode = (MSize)((char *)as->mctop - (char *)as->mcp);
  asm_snap_fixup_mcofs(as);
#if LJ_TARGET_MCODE_FIXUP
  asm_mcode_fixup(T->mcode, T->szmcode);
#endif
  lj_mcode_sync(T->mcode, as->mctoporig);
}

#if LJ_TARGET_ARM64 && defined(LJ_ARM64_EMIT_TEST_HELPERS)
MSize lj_asm_arm64_emit_test(jit_State *J, MCode *buf, MSize cap,
			     LJArm64EmitTestOp op, int32_t state)
{
  ASMState as_;
  ASMState *as = &as_;
  MCode *end;
  MSize n;

  if (J == NULL || buf == NULL || cap < 8)
    return 0;
  memset(as, 0, sizeof(*as));
  as->J = J;
  as->mcp = end = buf + cap;
  switch (op) {
  case LJ_ARM64_EMIT_TEST_GET_CUR_L:
    emit_gettg(as, RID_X0, cur_L);
    break;
  case LJ_ARM64_EMIT_TEST_GET_JIT_BASE:
    emit_gettg(as, RID_X1, jit_base);
    break;
  case LJ_ARM64_EMIT_TEST_SET_JIT_BASE:
    emit_settg(as, RID_X2, jit_base);
    break;
  case LJ_ARM64_EMIT_TEST_SETVMSTATE:
    emit_setvmstate(as, state);
    break;
  case LJ_ARM64_EMIT_TEST_SETVMSTATE_ROOT:
    emit_setvmstate_root(as, state);
    break;
  case LJ_ARM64_EMIT_TEST_GET_POLL:
    emit_gettg32(as, RID_X3, poll);
    break;
  case LJ_ARM64_EMIT_TEST_GET_PROFILE_REQUEST:
    emit_gettg32(as, RID_X4, profile_request);
    break;
  case LJ_ARM64_EMIT_TEST_GET_JIT_GATE:
    emit_getgl32acq(as, RID_X5, gc2.jit_phase_gate);
    break;
  default:
    return 0;
  }
  n = (MSize)(end - as->mcp);
  memmove(buf, as->mcp, n * sizeof(MCode));
  return n;
}
#endif

#if LJ_TARGET_ARM64 && defined(LJ_ARM64_EXIT_TEST_HELPERS)
int lj_asm_arm64_exitstub_layout_test(uintptr_t mctop, ExitNo nexits,
	MSize *needp)
{
  MSize need;
  if (nexits == 0 || needp == NULL)
    return 0;
  need = asm_exitstub_need(mctop, nexits);
  *needp = need;
  return need < 0x10000u;
}

MSize lj_asm_arm64_exitstub_test(jit_State *J, MCode *buf, MSize cap,
				 TraceNo traceno, ExitNo nexits, MCode **slots)
{
  ASMState as_;
  ASMState *as = &as_;
  GCtrace T;
  MSize need;

  if (J == NULL || buf == NULL || slots == NULL || traceno == 0 ||
      nexits == 0 || nexits-1u > UINT16_MAX ||
      ((uintptr_t)(void *)buf & 7u) != 0)
    return 0;
  need = ARM64_EXIT_FALLBACK_WORDS +
	 ARM64_EXIT_GATE_WORDS * (MSize)nexits;
  if (cap < need)
    return 0;
  memset(as, 0, sizeof(*as));
  memset(&T, 0, sizeof(T));
  T.traceno = traceno;
  T.nsnap = (SnapNo)nexits;
  T.exittab = slots;
  as->J = J;
  as->T = &T;
  as->mctop = buf + need;
  asm_exitstub_write(as, nexits);
  lj_assertA(as->mctop == buf, "bad synthetic exit-stub size");
  return need;
}
#endif

#undef IR

#endif
