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

/* Packed min/max, or 0 if the lane type has no instruction. */
static x86Op asm_vecminmax(ASMState *as, IROp op, IRType t)
{
  int sse41 = (as->flags & JIT_F_SSE4_1) != 0;
  switch (op) {
  case IR_VMIN:
    switch (t) {
    case IRT_V4F32: return XO_MINPS;
    case IRT_V2F64: return XO_MINPD;
    case IRT_V16I8: return sse41 ? XO_PMINSB : (x86Op)0;
    case IRT_V8I16: return XO_PMINSW;
    case IRT_V4I32: return sse41 ? XO_PMINSD : (x86Op)0;
    default: return (x86Op)0;
    }
  case IR_VMAX:
    switch (t) {
    case IRT_V4F32: return XO_MAXPS;
    case IRT_V2F64: return XO_MAXPD;
    case IRT_V16I8: return sse41 ? XO_PMAXSB : (x86Op)0;
    case IRT_V8I16: return XO_PMAXSW;
    case IRT_V4I32: return sse41 ? XO_PMAXSD : (x86Op)0;
    default: return (x86Op)0;
    }
  case IR_VMINU:
    switch (t) {
    case IRT_V16I8: return XO_PMINUB;
    case IRT_V8I16: return sse41 ? XO_PMINUW : (x86Op)0;
    case IRT_V4I32: return sse41 ? XO_PMINUD : (x86Op)0;
    default: return (x86Op)0;
    }
  default:
    switch (t) {
    case IRT_V16I8: return XO_PMAXUB;
    case IRT_V8I16: return sse41 ? XO_PMAXUW : (x86Op)0;
    case IRT_V4I32: return sse41 ? XO_PMAXUD : (x86Op)0;
    default: return (x86Op)0;
    }
  }
}

/* Is this opcode one of the SSSE3/SSE4 three byte forms? */
static int asm_vec3byte(x86Op xo)
{
  return (xo & 0xff) == 0xfc && ((xo >> 8) & 0xff) == 0x0f;
}

/* -- Three operand emission ---------------------------------------------- */

/*
** dest = src1 <xo> src2.
**
** With AVX this is a single VEX-encoded instruction and dest may alias either
** source. Without it, the left operand is copied into dest first, so the
** caller must guarantee dest != src2 unless both sources are the same.
*/
static void emit_vrr3(ASMState *as, x86Op xo, Reg dest, Reg src1, Reg src2)
{
  if ((as->flags & JIT_F_AVX)) {
    emit_vexrr(as, xo, dest, src1, src2);
    return;
  }
  lj_assertA(dest != src2 || src1 == src2, "vector operand aliasing");
  if (asm_vec3byte(xo))
    emit_vrr66(as, xo, dest, src2);
  else
    emit_rr(as, xo, dest, src2);
  if (dest != src1) emit_rr(as, XO_MOVAPS, dest, src1);
}

/* dest = <xo> src, a two operand form. VEX.vvvv is unused. */
static void emit_vrr2(ASMState *as, x86Op xo, Reg dest, Reg src)
{
  if ((as->flags & JIT_F_AVX))
    emit_vexrr(as, xo, dest, VEXNOV, src);
  else if (asm_vec3byte(xo))
    emit_vrr66(as, xo, dest, src);
  else
    emit_rr(as, xo, dest, src);
}

/* dest = <xo> src, imm8. */
static void emit_vrr2i(ASMState *as, x86Op xo, Reg dest, Reg src, int32_t i)
{
  emit_i8(as, i);
  emit_vrr2(as, xo, dest, src);
}

/* Shift by an immediate: the group number goes in the ModRM reg field and the
** destination in VEX.vvvv.
*/
static void emit_vshift(ASMState *as, x86Op xg, uint32_t grp, Reg dest,
			Reg src, int32_t i)
{
  emit_i8(as, i);
  if ((as->flags & JIT_F_AVX)) {
    emit_vexrr(as, xg, (Reg)grp, dest, src);
    return;
  }
  emit_rr(as, xg, (Reg)grp, dest);
  if (dest != src) emit_rr(as, XO_MOVAPS, dest, src);
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
  Reg dest, right;
  UNUSED(is3byte);  /* The VEX map is derived from the opcode itself. */
  if ((as->flags & JIT_F_AVX)) {
    /* Three operands: the destination may alias either source. */
    Reg left;
    dest = ra_dest(as, ir, RSET_FPR);
    left = ra_alloc1(as, lref, RSET_FPR);
    right = lref == rref ? left :
	    ra_alloc1(as, rref, rset_exclude(RSET_FPR, left));
    emit_vexrr(as, xo, dest, left, right);
    return;
  }
  right = IR(rref)->r;
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
  if (asm_vec3byte(xo))
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
  **   dest = pmuludq(left, right)          ; [a0*b0, a2*b2]
  **   t1 = left >> 32; t2 = right >> 32
  **   t1 = pmuludq(t1, t2)                 ; [a1*b1, a3*b3]
  **   compact the low dwords of both, then interleave
  */
  emit_vrr3(as, XO_PUNPCKLDQ, dest, dest, t1);
  emit_vrr2i(as, XO_PSHUFD, t1, t1, 0x08);
  emit_vrr2i(as, XO_PSHUFD, dest, dest, 0x08);
  emit_vrr3(as, XO_PMULUDQ, t1, t1, t2);
  checkmclim(as);
  emit_vshift(as, XO_PSHIFTQ, XOg_PSRL, t2, right, 32);
  emit_vshift(as, XO_PSHIFTQ, XOg_PSRL, t1, left, 32);
  emit_vrr3(as, XO_PMULUDQ, dest, left, right);
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
  **   t1 = left >> 8; t2 = right >> 8      ; odd bytes
  **   t1 = pmullw(t1, t2) << 8             ; odd products, back in place
  **   dest = pmullw(left, right)           ; even products in the low bytes
  **   dest = (dest << 8) >> 8              ; mask off the high bytes
  **   dest = dest | t1
  */
  emit_vrr3(as, XO_POR, dest, dest, t1);
  emit_vshift(as, XO_PSHIFTW, XOg_PSRL, dest, dest, 8);
  emit_vshift(as, XO_PSHIFTW, XOg_PSLL, dest, dest, 8);
  emit_vrr3(as, XO_PMULLW, dest, left, right);
  checkmclim(as);
  emit_vshift(as, XO_PSHIFTW, XOg_PSLL, t1, t1, 8);
  emit_vrr3(as, XO_PMULLW, t1, t1, t2);
  emit_vshift(as, XO_PSHIFTW, XOg_PSRL, t2, right, 8);
  emit_vshift(as, XO_PSHIFTW, XOg_PSRL, t1, left, 8);
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
  emit_vrr3(as, XO_PADDQ, dest, dest, t1);
  emit_vrr3(as, XO_PMULUDQ, dest, left, right);
  emit_vshift(as, XO_PSHIFTQ, XOg_PSLL, t1, t1, 32);
  emit_vrr3(as, XO_PADDQ, t1, t1, t2);
  checkmclim(as);
  emit_vrr3(as, XO_PMULUDQ, t2, t2, left);
  emit_vshift(as, XO_PSHIFTQ, XOg_PSRL, t2, right, 32);
  emit_vrr3(as, XO_PMULUDQ, t1, t1, right);
  emit_vshift(as, XO_PSHIFTQ, XOg_PSRL, t1, left, 32);
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
    Reg left = ra_alloc1(as, ir->op1, RSET_FPR);
    emit_i8(as, 0);
    if ((as->flags & JIT_F_AVX)) emit_vexrr(as, XO_SHUFPS, dest, left, left);
    else { emit_rr(as, XO_SHUFPS, dest, dest); ra_left(as, dest, ir->op1); }
  } else if (t == IRT_V2F64) {
    Reg left = ra_alloc1(as, ir->op1, RSET_FPR);
    emit_vrr3(as, XO_UNPCKLPD, dest, left, left);
  } else {
    /* Integer lanes: move the scalar over from a GPR, then broadcast. */
    Reg src = ra_alloc1(as, ir->op1, RSET_GPR);
    switch (t) {
    case IRT_V2I64:
      emit_vrr2i(as, XO_PSHUFD, dest, dest, 0x44);
      emit_rr(as, XO_MOVD, dest|REX_64, src|REX_64);
      break;
    case IRT_V4I32:
      emit_vrr2i(as, XO_PSHUFD, dest, dest, 0x00);
      emit_rr(as, XO_MOVD, dest, src);
      break;
    case IRT_V8I16:
      emit_vrr2i(as, XO_PSHUFD, dest, dest, 0x00);
      emit_vrr2i(as, XO_PSHUFLW, dest, dest, 0x00);
      emit_rr(as, XO_MOVD, dest, src);
      break;
    default:  /* IRT_V16I8 */
      emit_vrr2i(as, XO_PSHUFD, dest, dest, 0x00);
      emit_vrr2i(as, XO_PSHUFLW, dest, dest, 0x00);
      emit_vrr3(as, XO_PUNPCKLBW, dest, dest, dest);
      emit_rr(as, XO_MOVD, dest, src);
      break;
    }
  }
}

/* -- Comparisons --------------------------------------------------------- */

/*
** dest = op1 <cmp> op2. Integer lanes use the PCMPEQ and PCMPGT forms,
** which have the
** same operand order as the IR. Floating-point lanes use CMPPS/CMPPD, whose
** only ordered greater-than form is a less-than with swapped operands, so
** the destination takes the right operand instead.
*/
static void asm_veccmp(ASMState *as, IRIns *ir)
{
  IRType t = irt_type(ir->t);
  IROp op = (IROp)ir->o;
  if (t == IRT_V4F32 || t == IRT_V2F64) {
    x86Op xo = t == IRT_V4F32 ? XO_CMPPS : XO_CMPPD;
    int32_t pred = op == IR_VCMPEQ ? 0 : op == IR_VCMPGT ? 1 : 2;
    IRRef lref = ir->op1, rref = ir->op2;
    RegSet allow = RSET_FPR;
    Reg dest, other;
    if (op != IR_VCMPEQ) { IRRef tmp = lref; lref = rref; rref = tmp; }
    other = IR(rref)->r;
    if (ra_hasreg(other)) { rset_clear(allow, other); ra_noweak(as, other); }
    dest = ra_dest(as, ir, allow);
    if (lref == rref) other = dest;
    else if (ra_noreg(other)) other = ra_alloc1(as, rref, rset_clear(allow, dest));
    if ((as->flags & JIT_F_AVX)) {
      Reg left = ra_alloc1(as, lref, RSET_FPR);
      emit_i8(as, pred);
      emit_vexrr(as, xo, dest, left, other == dest ? left : other);
      return;
    }
    emit_vrri(as, xo, dest, other, pred);
    ra_left(as, dest, lref);
    return;
  }
  if (op == IR_VCMPEQ && t == IRT_V2I64 && !(as->flags & JIT_F_SSE4_1)) {
    /* SSE2: compare the 32 bit halves and AND the two results together. */
    Reg dest = ra_dest(as, ir, RSET_FPR);
    RegSet allow = rset_exclude(RSET_FPR, dest);
    Reg left = ra_alloc1(as, ir->op1, allow);
    Reg right, t1;
    allow = rset_exclude(allow, left);
    right = ir->op1 == ir->op2 ? left : ra_alloc1(as, ir->op2, allow);
    t1 = ra_scratch(as, rset_exclude(allow, right));
    emit_vrr3(as, XO_PAND, dest, dest, t1);
    emit_vrr2i(as, XO_PSHUFD, t1, dest, 0xb1);  /* Swap the 32 bit halves. */
    emit_vrr3(as, XO_PCMPEQD, dest, left, right);
    return;
  }
  {
    x86Op xo;
    if (op == IR_VCMPEQ) {
      xo = t == IRT_V2I64 ? XO_PCMPEQQ : asm_vecxo(IR_VCMPEQ, t);
    } else {
      switch (t) {
      case IRT_V16I8: xo = XO_PCMPGTB; break;
      case IRT_V8I16: xo = XO_PCMPGTW; break;
      case IRT_V4I32: xo = XO_PCMPGTD; break;
      default: xo = XO_PCMPGTQ; break;
      }
    }
    asm_vecbin(as, ir, xo, asm_vec3byte(xo));
  }
}

/* -- Shifts -------------------------------------------------------------- */

/* Lane shifts. 8 bit lanes have no instruction and are handled by the
** recorder, which rewrites them into 16 bit shifts plus a mask.
*/
static void asm_vecshift(ASMState *as, IRIns *ir)
{
  IRType t = irt_type(ir->t);
  IROp op = (IROp)ir->o;
  x86Op xg, xr;
  uint32_t grp;
  switch (t) {
  case IRT_V8I16: xg = XO_PSHIFTW; break;
  case IRT_V4I32: xg = XO_PSHIFTD; break;
  default: xg = XO_PSHIFTQ; break;
  }
  grp = op == IR_VSHL ? XOg_PSLL : op == IR_VSHR ? XOg_PSRL : XOg_PSRA;
  if (op == IR_VSHL)
    xr = t == IRT_V8I16 ? XO_PSLLW_r : t == IRT_V4I32 ? XO_PSLLD_r : XO_PSLLQ_r;
  else if (op == IR_VSHR)
    xr = t == IRT_V8I16 ? XO_PSRLW_r : t == IRT_V4I32 ? XO_PSRLD_r : XO_PSRLQ_r;
  else
    xr = t == IRT_V8I16 ? XO_PSRAW_r : XO_PSRAD_r;
  if (irref_isk(ir->op2)) {
    Reg dest = ra_dest(as, ir, RSET_FPR);
    Reg left = ra_alloc1(as, ir->op1, RSET_FPR);
    int32_t n = IR(ir->op2)->i;
    if (n > 255) n = 255;  /* Saturate: the instruction flushes to 0 anyway. */
    emit_vshift(as, xg, grp, dest, left, n);
  } else {
    /* Variable count: the packed shifts read a 64 bit count from an XMM. */
    Reg dest = ra_dest(as, ir, RSET_FPR);
    RegSet allow = rset_exclude(RSET_FPR, dest);
    Reg left = ra_alloc1(as, ir->op1, allow);
    Reg tmp = ra_scratch(as, rset_exclude(allow, left));
    Reg cnt = ra_alloc1(as, ir->op2, RSET_GPR);
    emit_vrr3(as, xr, dest, left, tmp);
    emit_vrr2(as, XO_MOVD, tmp, cnt);
  }
}

/*
** Per-lane shift counts (AVX2). One instruction, and genuinely three operand,
** so no register copy is ever needed. 32 and 64 bit lanes only: there is no
** VPSLLVW before AVX-512 and no VPSRAVQ at all, so the recorder rejects the
** narrow lane widths and emulates the 64 bit arithmetic shift.
*/
static void asm_vecshiftv(ASMState *as, IRIns *ir)
{
  IROp op = (IROp)ir->o;
  x86Op xo = op == IR_VSHLV ? XO_VPSLLV :
	     op == IR_VSHRV ? XO_VPSRLV : XO_VPSRAV;
  int w = irt_type(ir->t) == IRT_V2I64;
  Reg dest = ra_dest(as, ir, RSET_FPR);
  Reg left = ra_alloc1(as, ir->op1, RSET_FPR);
  Reg cnt = ra_alloc1(as, ir->op2, rset_exclude(RSET_FPR, left));
  lj_assertA(op != IR_VSARV || !w, "no VPSRAVQ before AVX-512");
  emit_vexrrw(as, xo, dest, left, cnt, w);
}

/*
** Fused multiply-add. Three inputs do not fit in one IR instruction, so the
** second and third arrive through a CARG pair: VFMA(a, CARG(b, c)) is
** a*b + c. asm_ir() treats a CARG as a no-op, so the pair costs no code.
**
** VFMADD213PS/PD computes dest = vvvv * dest + rm, i.e. the destination is
** also the second multiplicand. b is therefore moved into dest by ra_left(),
** and a and c must be kept out of dest so that move cannot clobber them.
*/
static void asm_vecfma(ASMState *as, IRIns *ir)
{
  IRIns *arg = IR(ir->op2);
  int w = irt_type(ir->t) == IRT_V2F64;
  Reg dest = ra_dest(as, ir, RSET_FPR);
  RegSet allow = rset_exclude(RSET_FPR, dest);
  Reg ra = ra_alloc1(as, ir->op1, allow);
  Reg rc = ra_alloc1(as, arg->op2, rset_exclude(allow, ra));
  lj_assertA(arg->o == IR_CARG, "VFMA op2 is not a CARG pair");
  emit_vexrrw(as, XO_VFMADD213, dest, ra, rc, w);
  ra_left(as, dest, arg->op1);
}

/* -- Unary and shuffle operations ---------------------------------------- */

static void asm_vecsqrt(ASMState *as, IRIns *ir)
{
  Reg dest = ra_dest(as, ir, RSET_FPR);
  Reg left = ra_alloc1(as, ir->op1, RSET_FPR);
  emit_vrr2(as, irt_type(ir->t) == IRT_V4F32 ? XO_SQRTPS : XO_SQRTPD,
	    dest, left);
}

static void asm_vecabs(ASMState *as, IRIns *ir)
{
  x86Op xo;
  Reg dest, left;
  switch (irt_type(ir->t)) {
  case IRT_V16I8: xo = XO_PABSB; break;
  case IRT_V8I16: xo = XO_PABSW; break;
  default: xo = XO_PABSD; break;
  }
  dest = ra_dest(as, ir, RSET_FPR);
  left = ra_alloc1(as, ir->op1, RSET_FPR);
  emit_vrr2(as, xo, dest, left);
}

static void asm_vecround(ASMState *as, IRIns *ir)
{
  x86Op xo = irt_type(ir->t) == IRT_V4F32 ? XO_ROUNDPS : XO_ROUNDPD;
  Reg dest = ra_dest(as, ir, RSET_FPR);
  Reg left = ra_alloc1(as, ir->op1, RSET_FPR);
  emit_vrr2i(as, xo, dest, left, (int32_t)ir->op2);
}

static void asm_vecshuf(ASMState *as, IRIns *ir)
{
  uint32_t lit = (uint32_t)ir->op2;
  uint32_t mode = lit >> 8;
  int32_t imm = (int32_t)(lit & 255);
  Reg dest = ra_dest(as, ir, RSET_FPR);
  Reg left = ra_alloc1(as, ir->op1, RSET_FPR);
  if (mode == IRVSHUF_PSRLDQ) {
    emit_vshift(as, XO_PSHIFTQ, 3, dest, left, imm);  /* PSRLDQ is group 3. */
  } else {
    x86Op xo = mode == IRVSHUF_PSHUFD ? XO_PSHUFD :
	       mode == IRVSHUF_PSHUFLW ? XO_PSHUFLW : XO_PSHUFHW;
    emit_vrr2i(as, xo, dest, left, imm);
  }
}

static void asm_vecshufb(ASMState *as, IRIns *ir)
{
  asm_vecbin(as, ir, XO_PSHUFB, 1);
}

/* Sign bits of all lanes, gathered into an integer. */
static void asm_vecmovmsk(ASMState *as, IRIns *ir)
{
  IRType t = irvsrc_type(ir->op2);
  Reg dest = ra_dest(as, ir, RSET_GPR);
  Reg left = ra_alloc1(as, ir->op1, RSET_FPR);
  switch (t) {
  case IRT_V4F32: case IRT_V4I32:
    emit_vrr2(as, XO_MOVMSKPS, dest, left);
    break;
  case IRT_V2F64: case IRT_V2I64:
    emit_vrr2(as, XO_MOVMSKPD, dest, left);
    break;
  case IRT_V16I8:
    emit_vrr2(as, XO_PMOVMSKB, dest, left);
    break;
  default: {  /* 16 bit lanes: saturate down to bytes, then PMOVMSKB. */
    Reg tmp = ra_scratch(as, rset_exclude(RSET_FPR, left));
    emit_gri(as, XG_ARITHi(XOg_AND), dest, 0xff);
    emit_vrr2(as, XO_PMOVMSKB, dest, tmp);
    emit_vrr3(as, XO_PACKSSWB, tmp, left, left);
    break;
    }
  }
}

/* Extract lane 0. Other lanes are handled by an ordinary load from memory. */
static void asm_vecextract(ASMState *as, IRIns *ir)
{
  IRType st = irvsrc_type(ir->op2);
  if (irt_isfp(ir->t)) {
    Reg dest = ra_dest(as, ir, RSET_FPR);
    ra_left(as, dest, ir->op1);  /* Lane 0 is the low part of the register. */
  } else {
    Reg dest = ra_dest(as, ir, RSET_GPR);
    Reg left = ra_alloc1(as, ir->op1, RSET_FPR);
    if (st == IRT_V2I64)
      emit_rr(as, XO_MOVDto, left|REX_64, dest|REX_64);
    else
      emit_rr(as, XO_MOVDto, left, dest);
  }
}

/* Lane conversion. Only the pairs with a direct packed instruction get here. */
static void asm_vecconv(ASMState *as, IRIns *ir)
{
  uint32_t lit = (uint32_t)ir->op2;
  uint32_t dk = irvconv_dst(lit), sk = irvconv_src(lit);
  Reg dest = ra_dest(as, ir, RSET_FPR);
  Reg left = ra_alloc1(as, ir->op1, RSET_FPR);
  if (dk == VECK_F32)
    emit_vrr2(as, XO_CVTDQ2PS, dest, left);
  else if (sk == VECK_F32)
    emit_vrr2(as, XO_CVTTPS2DQ, dest, left);
  else
    emit_vrr2(as, XO_MOVAPS, dest, left);  /* Same-width reinterpretation. */
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
  case IR_VMIN: case IR_VMAX: case IR_VMINU: case IR_VMAXU:
    xo = asm_vecminmax(as, (IROp)ir->o, t);
    lj_assertA(xo != 0, "no packed min/max for IR op %d type %d", ir->o, t);
    asm_vecbin(as, ir, xo, asm_vec3byte(xo));
    return;
  case IR_VCMPEQ: case IR_VCMPGT: case IR_VCMPGE: asm_veccmp(as, ir); return;
  case IR_VSHL: case IR_VSHR: case IR_VSAR: asm_vecshift(as, ir); return;
  case IR_VSHLV: case IR_VSHRV: case IR_VSARV: asm_vecshiftv(as, ir); return;
  case IR_VFMA: asm_vecfma(as, ir); return;
  case IR_VSQRT: asm_vecsqrt(as, ir); return;
  case IR_VABS: asm_vecabs(as, ir); return;
  case IR_VROUND: asm_vecround(as, ir); return;
  case IR_VSHUF: asm_vecshuf(as, ir); return;
  case IR_VSHUFB: asm_vecshufb(as, ir); return;
  case IR_VMOVMSK: asm_vecmovmsk(as, ir); return;
  case IR_VEXTRACT: asm_vecextract(as, ir); return;
  case IR_VCONV: asm_vecconv(as, ir); return;
  default:
    lj_assertA(0, "unhandled vector IR op %d", ir->o);
    return;
  }
}

#endif
