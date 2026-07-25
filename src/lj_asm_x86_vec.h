/*
** x86-64 SIMD vector backend.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Included from lj_asm_x86.h. Every vector value is 128 bits wide and lives
** in an XMM register. Memory operands are never fused into a packed
** instruction, because a vector cdata payload is only 8 byte aligned and a
** vector may also be loaded through a pointer to arbitrary memory: everything
** goes through MOVUPS.
*/

#if LJ_HASFFI

/* Shift a vector by an immediate: the group number goes in the ModRM reg. */
#define emit_vshifti(as, xo, xg, r, i) \
  (emit_i8(as, (i)), emit_rr(as, (xo), (Reg)(xg), (r)))

/* Emit a packed instruction with an immediate operand. */
#define emit_vrri(as, xo, r1, r2, i) \
  (emit_i8(as, (i)), emit_rr(as, (xo), (r1), (r2)))

/* Emit an SSSE3/SSE4 three byte opcode, which needs an explicit 66 prefix. */
static void emit_vrr66(ASMState *as, x86Op xo, Reg r1, Reg r2)
{
  emit_rr(as, xo, r1, r2);
  emit_prefix66(as, xo);
}

/* -- Opcode selection ---------------------------------------------------- */

/* Packed opcode for a lane-type dependent operation, or 0 if there is none. */
static x86Op asm_vecxo(IROp op, IRType t)
{
  switch (op) {
  case IR_VADD:
    switch (t) {
    case IRT_V16I8: return XO_PADDB;
    case IRT_V8I16: return XO_PADDW;
    case IRT_V4I32: return XO_PADDD;
    case IRT_V2I64: return XO_PADDQ;
    case IRT_V4F32: return XO_ADDPS;
    default: return XO_ADDPD;
    }
  case IR_VSUB:
    switch (t) {
    case IRT_V16I8: return XO_PSUBB;
    case IRT_V8I16: return XO_PSUBW;
    case IRT_V4I32: return XO_PSUBD;
    case IRT_V2I64: return XO_PSUBQ;
    case IRT_V4F32: return XO_SUBPS;
    default: return XO_SUBPD;
    }
  case IR_VMUL:
    switch (t) {
    case IRT_V8I16: return XO_PMULLW;
    case IRT_V4F32: return XO_MULPS;
    case IRT_V2F64: return XO_MULPD;
    default: return (x86Op)0;  /* Needs an emulation sequence. */
    }
  case IR_VDIV:
    return t == IRT_V4F32 ? XO_DIVPS : XO_DIVPD;
  case IR_VAND:
    return t == IRT_V4F32 ? XO_ANDPS : t == IRT_V2F64 ? XO_ANDPD : XO_PAND;
  case IR_VOR:
    return t == IRT_V4F32 ? XO_ORPS : t == IRT_V2F64 ? XO_ORPD : XO_POR;
  case IR_VXOR:
    return t == IRT_V4F32 ? XO_XORPS : t == IRT_V2F64 ? XO_XORPD : XO_PXOR;
  case IR_VANDN:
    return t == IRT_V4F32 ? XO_ANDNPS : t == IRT_V2F64 ? XO_ANDNPD : XO_PANDN;
  case IR_VCMPEQ:
    switch (t) {
    case IRT_V16I8: return XO_PCMPEQB;
    case IRT_V8I16: return XO_PCMPEQW;
    case IRT_V4I32: return XO_PCMPEQD;
    default: return (x86Op)0;  /* V2I64 needs SSE4.1, FP uses CMPPS/CMPPD. */
    }
  case IR_VUNPKL:
    switch (t) {
    case IRT_V16I8: return XO_PUNPCKLBW;
    case IRT_V8I16: return XO_PUNPCKLWD;
    case IRT_V4I32: return XO_PUNPCKLDQ;
    case IRT_V2I64: return XO_PUNPCKLQDQ;
    case IRT_V4F32: return XO_UNPCKLPS;
    default: return XO_UNPCKLPD;
    }
  case IR_VUNPKH:
    switch (t) {
    case IRT_V16I8: return XO_PUNPCKHBW;
    case IRT_V8I16: return XO_PUNPCKHWD;
    case IRT_V4I32: return XO_PUNPCKHDQ;
    case IRT_V2I64: return XO_PUNPCKHQDQ;
    case IRT_V4F32: return XO_UNPCKHPS;
    default: return XO_UNPCKHPD;
    }
  case IR_VADDS:
    return t == IRT_V16I8 ? XO_PADDSB : XO_PADDSW;
  case IR_VSUBS:
    return t == IRT_V16I8 ? XO_PSUBSB : XO_PSUBSW;
  case IR_VADDSU:
    return t == IRT_V16I8 ? XO_PADDUSB : XO_PADDUSW;
  case IR_VSUBSU:
    return t == IRT_V16I8 ? XO_PSUBUSB : XO_PSUBUSW;
  default:
    return (x86Op)0;
  }
}

/* -- Generic two-operand lowering ---------------------------------------- */

/*
** dest = op1 <xo> op2, using the two operand x86 form. The left operand is
** moved into dest, so dest must never alias the right operand.
*/
static void asm_vecbin(ASMState *as, IRIns *ir, x86Op xo, int is3byte)
{
  IRRef lref = ir->op1, rref = ir->op2;
  RegSet allow = RSET_FPR;
  Reg dest, right = IR(rref)->r;
  if (ra_hasreg(right)) {
    rset_clear(allow, right);
    ra_noweak(as, right);
  }
  dest = ra_dest(as, ir, allow);
  if (lref == rref) {
    right = dest;
  } else if (ra_noreg(right)) {
    right = ra_alloc1(as, rref, rset_clear(allow, dest));
  }
  if (is3byte)
    emit_vrr66(as, xo, dest, right);
  else
    emit_rr(as, xo, dest, right);
  ra_left(as, dest, lref);
}

/* -- Multiplication ------------------------------------------------------ */

/* 32 bit lane multiply without SSE4.1: two PMULUDQ halves, then interleave. */
static void asm_vmul_i32_sse2(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_FPR);
  RegSet allow = rset_exclude(RSET_FPR, dest);
  Reg left = ra_alloc1(as, ir->op1, allow);
  Reg right, t1, t2;
  allow = rset_exclude(allow, left);
  right = ir->op1 == ir->op2 ? left : ra_alloc1(as, ir->op2, allow);
  allow = rset_exclude(allow, right);
  t1 = ra_scratch(as, allow);
  allow = rset_exclude(allow, t1);
  t2 = ra_scratch(as, allow);
  /* Forward order:
  **   movaps dest,left; pmuludq dest,right     ; [a0*b0, a2*b2]
  **   movaps t1,left;   psrlq t1,32
  **   movaps t2,right;  psrlq t2,32
  **   pmuludq t1,t2                            ; [a1*b1, a3*b3]
  **   pshufd dest,dest,0x08; pshufd t1,t1,0x08 ; compact the low dwords
  **   punpckldq dest,t1                        ; interleave
  */
  emit_rr(as, XO_PUNPCKLDQ, dest, t1);
  emit_vrri(as, XO_PSHUFD, t1, t1, 0x08);
  emit_vrri(as, XO_PSHUFD, dest, dest, 0x08);
  checkmclim(as);
  emit_rr(as, XO_PMULUDQ, t1, t2);
  emit_vshifti(as, XO_PSHIFTQ, XOg_PSRL, t2, 32);
  emit_rr(as, XO_MOVAPS, t2, right);
  emit_vshifti(as, XO_PSHIFTQ, XOg_PSRL, t1, 32);
  emit_rr(as, XO_MOVAPS, t1, left);
  checkmclim(as);
  emit_rr(as, XO_PMULUDQ, dest, right);
  emit_rr(as, XO_MOVAPS, dest, left);
}

/* 8 bit lane multiply: separate the even and odd byte products. */
static void asm_vmul_i8(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_FPR);
  RegSet allow = rset_exclude(RSET_FPR, dest);
  Reg left = ra_alloc1(as, ir->op1, allow);
  Reg right, t1, t2;
  allow = rset_exclude(allow, left);
  right = ir->op1 == ir->op2 ? left : ra_alloc1(as, ir->op2, allow);
  allow = rset_exclude(allow, right);
  t1 = ra_scratch(as, allow);
  allow = rset_exclude(allow, t1);
  t2 = ra_scratch(as, allow);
  /* Forward order:
  **   movaps t1,left;  psrlw t1,8         ; odd bytes of a
  **   movaps t2,right; psrlw t2,8         ; odd bytes of b
  **   pmullw t1,t2;    psllw t1,8         ; odd products, back in place
  **   movaps dest,left; pmullw dest,right ; even products in the low bytes
  **   psllw dest,8;    psrlw dest,8       ; mask off the high bytes
  **   por dest,t1
  */
  emit_rr(as, XO_POR, dest, t1);
  emit_vshifti(as, XO_PSHIFTW, XOg_PSRL, dest, 8);
  emit_vshifti(as, XO_PSHIFTW, XOg_PSLL, dest, 8);
  emit_rr(as, XO_PMULLW, dest, right);
  emit_rr(as, XO_MOVAPS, dest, left);
  checkmclim(as);
  emit_vshifti(as, XO_PSHIFTW, XOg_PSLL, t1, 8);
  emit_rr(as, XO_PMULLW, t1, t2);
  emit_vshifti(as, XO_PSHIFTW, XOg_PSRL, t2, 8);
  emit_rr(as, XO_MOVAPS, t2, right);
  emit_vshifti(as, XO_PSHIFTW, XOg_PSRL, t1, 8);
  emit_rr(as, XO_MOVAPS, t1, left);
  checkmclim(as);
}

/* 64 bit lane multiply: lo*lo + ((hi*lo + lo*hi) << 32). */
static void asm_vmul_i64(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_FPR);
  RegSet allow = rset_exclude(RSET_FPR, dest);
  Reg left = ra_alloc1(as, ir->op1, allow);
  Reg right, t1, t2;
  allow = rset_exclude(allow, left);
  right = ir->op1 == ir->op2 ? left : ra_alloc1(as, ir->op2, allow);
  allow = rset_exclude(allow, right);
  t1 = ra_scratch(as, allow);
  allow = rset_exclude(allow, t1);
  t2 = ra_scratch(as, allow);
  emit_rr(as, XO_PADDQ, dest, t1);
  emit_rr(as, XO_PMULUDQ, dest, right);
  emit_rr(as, XO_MOVAPS, dest, left);
  checkmclim(as);
  emit_vshifti(as, XO_PSHIFTQ, XOg_PSLL, t1, 32);
  emit_rr(as, XO_PADDQ, t1, t2);
  emit_rr(as, XO_PMULUDQ, t2, left);
  emit_vshifti(as, XO_PSHIFTQ, XOg_PSRL, t2, 32);
  emit_rr(as, XO_MOVAPS, t2, right);
  emit_rr(as, XO_PMULUDQ, t1, right);
  emit_vshifti(as, XO_PSHIFTQ, XOg_PSRL, t1, 32);
  emit_rr(as, XO_MOVAPS, t1, left);
  checkmclim(as);
}

static void asm_vmul(ASMState *as, IRIns *ir)
{
  x86Op xo = asm_vecxo(IR_VMUL, irt_type(ir->t));
  if (xo) {
    asm_vecbin(as, ir, xo, 0);
  } else if (irt_type(ir->t) == IRT_V4I32) {
    if ((as->flags & JIT_F_SSE4_1))
      asm_vecbin(as, ir, XO_PMULLD, 1);
    else
      asm_vmul_i32_sse2(as, ir);
  } else if (irt_type(ir->t) == IRT_V16I8) {
    asm_vmul_i8(as, ir);
  } else {
    asm_vmul_i64(as, ir);
  }
}

/* -- Splat --------------------------------------------------------------- */

/* Broadcast a scalar to all lanes. */
static void asm_vsplat(ASMState *as, IRIns *ir)
{
  IRType t = irt_type(ir->t);
  Reg dest = ra_dest(as, ir, RSET_FPR);
  if (t == IRT_V4F32) {
    emit_vrri(as, XO_SHUFPS, dest, dest, 0);
    ra_left(as, dest, ir->op1);
  } else if (t == IRT_V2F64) {
    emit_rr(as, XO_UNPCKLPD, dest, dest);
    ra_left(as, dest, ir->op1);
  } else {
    /* Integer lanes: move the scalar over from a GPR, then broadcast. */
    Reg src = ra_alloc1(as, ir->op1, RSET_GPR);
    switch (t) {
    case IRT_V2I64:
      emit_vrri(as, XO_PSHUFD, dest, dest, 0x44);
      emit_rr(as, XO_MOVD, dest|REX_64, src|REX_64);
      break;
    case IRT_V4I32:
      emit_vrri(as, XO_PSHUFD, dest, dest, 0x00);
      emit_rr(as, XO_MOVD, dest, src);
      break;
    case IRT_V8I16:
      emit_vrri(as, XO_PSHUFD, dest, dest, 0x00);
      emit_vrri(as, XO_PSHUFLW, dest, dest, 0x00);
      emit_rr(as, XO_MOVD, dest, src);
      break;
    default:  /* IRT_V16I8 */
      emit_vrri(as, XO_PSHUFD, dest, dest, 0x00);
      emit_vrri(as, XO_PSHUFLW, dest, dest, 0x00);
      emit_rr(as, XO_PUNPCKLBW, dest, dest);
      emit_rr(as, XO_MOVD, dest, src);
      break;
    }
  }
}

/* -- Dispatch ------------------------------------------------------------ */

static void asm_vec(ASMState *as, IRIns *ir)
{
  IRType t = irt_type(ir->t);
  x86Op xo;
  switch ((IROp)ir->o) {
  case IR_VSPLAT: asm_vsplat(as, ir); return;
  case IR_VMUL: asm_vmul(as, ir); return;
  case IR_VADD: case IR_VSUB: case IR_VDIV:
  case IR_VAND: case IR_VOR: case IR_VXOR:
  case IR_VUNPKL: case IR_VUNPKH:
  case IR_VADDS: case IR_VSUBS: case IR_VADDSU: case IR_VSUBSU:
    xo = asm_vecxo((IROp)ir->o, t);
    lj_assertA(xo != 0, "no packed opcode for IR op %d type %d", ir->o, t);
    asm_vecbin(as, ir, xo, 0);
    return;
  case IR_VANDN:
    /* PANDN computes ~dest & src, so the destination holds the *first*
    ** operand, which is exactly the IR_VANDN operand order.
    */
    asm_vecbin(as, ir, asm_vecxo(IR_VANDN, t), 0);
    return;
  default:
    lj_assertA(0, "unhandled vector IR op %d", ir->o);
    return;
  }
}

#endif
