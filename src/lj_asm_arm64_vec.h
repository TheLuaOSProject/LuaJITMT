/*
** ARM64 Advanced SIMD vector backend (128 bits).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** The vector IR deliberately retains SSE lane/shuffle semantics. In
** particular, MIN/MAX choose the second operand on unordered or equal
** comparisons, PSHUFB ignores bits 4..6, and invalid FP conversions return
** the signed indefinite integer. Do not substitute superficially similar
** NEON instructions without preserving these rules.
*/

#if LJ_HASFFI

#define VEC3(ai, d, n, m) \
  emit_dnm(as, (A64Ins)(ai), (d)&31, (n)&31, (m)&31)
#define VEC2(ai, d, n) emit_dn(as, (A64Ins)(ai), (d)&31, (n)&31)
#define VECMOV(d, n) VEC3(0x4ea01c00, d, n, n)
#define VECZERO(d) VEC3(0x6e201c00, d, d, d)

/* Log2(element size), shared by integer opcode and lane encodings. */
static unsigned asm_veclogsz(IRType t)
{
  switch (t & IRT_TYPE) {
  case IRT_V16I8: return 0;
  case IRT_V8I16: return 1;
  case IRT_V4I32: case IRT_V4F32: return 2;
  default: return 3;
  }
}

/* Allocate all inputs before scratch registers. Keep a multi-instruction
** result separate from its inputs, including the two-source shuffle inputs.
*/
typedef struct ARM64VecRegs {
  Reg d, n, m;
  RegSet allow;
} ARM64VecRegs;

static ARM64VecRegs asm_vecregs(ASMState *as, IRIns *ir, IRRef right)
{
  ARM64VecRegs r;
  r.d = ra_dest(as, ir, RSET_FPR);
  r.allow = rset_exclude(RSET_FPR, r.d);
  r.n = ra_alloc1(as, ir->op1, r.allow);
  r.allow = rset_exclude(r.allow, r.n);
  r.m = right ? (right == ir->op1 ? r.n :
                 ra_alloc1(as, right, r.allow)) : RID_NONE;
  if (right) r.allow = rset_exclude(r.allow, r.m);
  return r;
}

static Reg asm_vectmp(ASMState *as, ARM64VecRegs *r)
{
  Reg t = ra_scratch(as, r->allow);
  r->allow = rset_exclude(r->allow, t);
  return t;
}

static void emit_vecins(ASMState *as, Reg d, unsigned di, Reg n,
                        unsigned ni, unsigned sz)
{
  VEC2(0x6e000400 | (((di*2+1)<<sz)<<16) | ((ni<<sz)<<11), d, n);
}

static void emit_vecinsg(ASMState *as, Reg d, unsigned lane, Reg n,
                         unsigned sz)
{
  VEC2(0x4e001c00 | (((lane*2+1)<<sz)<<16), d, n);
}

static void emit_vecget(ASMState *as, Reg d, Reg n, unsigned lane,
                       unsigned sz)
{
  VEC2((sz == 3 ? 0x4e003c00 : 0x0e003c00) |
       (((lane*2+1)<<sz)<<16), d, n);
}

/* Emit a lane-sized immediate shift; counts outside the lane saturate. */
static void emit_vecshift(ASMState *as, IROp op, Reg d, Reg n,
                         unsigned sz, uint32_t count)
{
  unsigned bits = 8u<<sz;
  if (count == 0) {
    VECMOV(d, n);
  } else if (count >= bits && op != IR_VSAR) {
    VECZERO(d);
  } else if (op == IR_VSHL) {
    VEC2(0x4f005400 | ((bits+count)<<16), d, n);
  } else {
    if (count > bits) count = bits;
    VEC2((op == IR_VSAR ? 0x4f000400 : 0x6f000400) |
         ((2*bits-count)<<16), d, n);
  }
}

static void asm_vecsplat(ASMState *as, IRIns *ir)
{
  unsigned sz = asm_veclogsz(irt_type(ir->t));
  int fp = irt_isvecfp(ir->t);
  Reg d = ra_dest(as, ir, RSET_FPR);
  Reg n = ra_alloc1(as, ir->op1, fp ? rset_exclude(RSET_FPR, d) : RSET_GPR);
  if (ir->op2 == IRVSPLAT_SEED) {
    /* MOVD seeds 32 integer bits even for byte/word vector types. */
    if (fp) {
      VEC2(sz == 3 ? 0x1e604000 : 0x1e204000, d, n);
    } else {
      VEC2(sz == 3 ? 0x9e670000 : 0x1e270000, d, n);
    }
  } else {
    VEC2((fp ? 0x4e000400 : 0x4e000c00) | ((1u<<sz)<<16), d, n);
  }
}

/* Core three-register operations. All encodings have zeroed register fields. */
static uint32_t asm_vecopcode(IROp op, unsigned sz, int fp)
{
  uint32_t s = sz<<22, f = sz == 3 ? 0x00400000 : 0;
  switch (op) {
  case IR_VADD: return fp ? 0x4e20d400|f : 0x4e208400|s;
  case IR_VSUB: return fp ? 0x4ea0d400|f : 0x6e208400|s;
  case IR_VMUL: return fp ? 0x6e20dc00|f : 0x4e209c00|s;
  case IR_VDIV: return 0x6e20fc00|f;
  case IR_VAND: return 0x4e201c00;
  case IR_VOR: return 0x4ea01c00;
  case IR_VXOR: return 0x6e201c00;
  case IR_VANDN: return 0x4e601c00; /* Reverse n,m at the call site. */
  case IR_VUNPKL: return 0x4e003800|s;
  case IR_VUNPKH: return 0x4e007800|s;
  case IR_VCMPEQ: return fp ? 0x4e20e400|f : 0x6e208c00|s;
  case IR_VCMPGT: return fp ? 0x6ea0e400|f : 0x4e203400|s;
  case IR_VCMPGE: return fp ? 0x6e20e400|f : 0x4e203c00|s;
  case IR_VADDS: return 0x4e200c00|s;
  case IR_VADDSU: return 0x6e200c00|s;
  case IR_VSUBS: return 0x4e202c00|s;
  case IR_VSUBSU: return 0x6e202c00|s;
  default: return 0;
  }
}

/* Integer qword products: A64 scalar multiplication has the exact low/high
** semantics that SIMD lacks. No calls, memory temporaries, or FP rounding.
*/
static void asm_vecmul64(ASMState *as, IRIns *ir)
{
  ARM64VecRegs r = asm_vecregs(as, ir, ir->op2);
  Reg a = ra_scratch(as, RSET_GPR);
  Reg b = ra_scratch(as, rset_exclude(RSET_GPR, a));
  uint32_t ai = ir->o == IR_VMUL ? 0x9b007c00 :
                ir->o == IR_VMULHI ? 0x9b407c00 : 0x9bc07c00;
  int i;
  for (i = 1; i >= 0; i--) {
    emit_vecinsg(as, r.d, i, a, 3);
    emit_dnm(as, (A64Ins)ai, a, a, b);
    emit_vecget(as, b, r.m, i, 3);
    emit_vecget(as, a, r.n, i, 3);
  }
}

static void asm_vecmulhi(ASMState *as, IRIns *ir)
{
  unsigned sz = asm_veclogsz(irt_type(ir->t));
  ARM64VecRegs r;
  Reg hi, lo;
  uint32_t ai;
  if (sz == 3) { asm_vecmul64(as, ir); return; }
  r = asm_vecregs(as, ir, ir->op2);
  hi = asm_vectmp(as, &r); lo = asm_vectmp(as, &r);
  ai = (ir->o == IR_VMULHI ? 0x0e20c000 : 0x2e20c000) | (sz<<22);
  VEC2(0x4f008400 | ((8u<<sz)<<16), r.d, hi); /* SHRN2 */
  VEC2(0x0f008400 | ((8u<<sz)<<16), r.d, lo); /* SHRN */
  VEC3(ai|0x40000000, hi, r.n, r.m);
  VEC3(ai, lo, r.n, r.m);
}

static void asm_vecminmax(ASMState *as, IRIns *ir)
{
  ARM64VecRegs r = asm_vecregs(as, ir, ir->op2);
  unsigned sz = asm_veclogsz(irt_type(ir->t));
  int max = ir->o == IR_VMAX || ir->o == IR_VMAXU;
  int uns = ir->o == IR_VMINU || ir->o == IR_VMAXU;
  if (irt_isvecfp(ir->t) || sz == 3) {
    uint32_t cmp = irt_isvecfp(ir->t) ?
      (0x6ea0e400 | (sz == 3 ? 0x400000 : 0)) :
      ((uns ? 0x6e203400 : 0x4e203400) | (sz<<22));
    VEC3(0x6e601c00, r.d, r.n, r.m);
    if (max) VEC3(cmp, r.d, r.n, r.m);
    else VEC3(cmp, r.d, r.m, r.n);
  } else {
    VEC3((max ? 0x4e206400 : 0x4e206c00) |
         (uns ? 0x20000000 : 0) | (sz<<22), r.d, r.n, r.m);
  }
}

static void asm_vecsad(ASMState *as, IRIns *ir)
{
  ARM64VecRegs r = asm_vecregs(as, ir, ir->op2);
  VEC2(0x6ea02800, r.d, r.d); /* UADDLP 4S -> 2D */
  VEC2(0x6e602800, r.d, r.d); /* UADDLP 8H -> 4S */
  VEC2(0x6e202800, r.d, r.d); /* UADDLP 16B -> 8H */
  VEC3(0x6e207400, r.d, r.n, r.m);
}

static void asm_vecpmadd(ASMState *as, IRIns *ir)
{
  ARM64VecRegs r = asm_vecregs(as, ir, ir->op2);
  Reg hi = asm_vectmp(as, &r);
  VEC3(0x4ea0bc00, r.d, r.d, hi);
  VEC3(0x4e60c000, hi, r.n, r.m);
  VEC3(0x0e60c000, r.d, r.n, r.m);
}

static void asm_vecsign(ASMState *as, IRIns *ir)
{
  ARM64VecRegs r = asm_vecregs(as, ir, ir->op2);
  Reg t = asm_vectmp(as, &r), z = asm_vectmp(as, &r);
  unsigned s = asm_veclogsz(irt_type(ir->t))<<22;
  /* (b>0 ? a : 0) - (b<0 ? a : 0), including INT_MIN wraparound. */
  VEC3(0x6e208400|s, r.d, r.d, t);
  VEC3(0x4e201c00, t, t, r.n);
  VEC3(0x4e201c00, r.d, r.d, r.n);
  VEC3(0x4e203400|s, t, z, r.m);
  VEC3(0x4e203400|s, r.d, r.m, z);
  VECZERO(z);
}

static void asm_vecshifts(ASMState *as, IRIns *ir, int perlane)
{
  IROp op = (IROp)ir->o;
  unsigned sz = asm_veclogsz(irt_type(ir->t)), bits = 8u<<sz;
  int left = op == IR_VSHL || op == IR_VSHLV;
  int arith = op == IR_VSAR || op == IR_VSARV;
  ARM64VecRegs r;
  if (!perlane && irref_isk(ir->op2)) {
    r = asm_vecregs(as, ir, 0);
    emit_vecshift(as, op, r.d, r.n, sz, (uint32_t)IR(ir->op2)->i);
  } else {
    Reg cnt, lim;
    Reg g = RID_NONE, h = RID_NONE;
    r = asm_vecregs(as, ir, perlane ? ir->op2 : 0);
    if (!perlane) {
      g = ra_alloc1(as, ir->op2, RSET_GPR);
      h = ra_scratch(as, rset_exclude(RSET_GPR, g));
    }
    cnt = asm_vectmp(as, &r); lim = asm_vectmp(as, &r);
    if (perlane) g = ra_scratch(as, RSET_GPR);
    VEC3((arith ? 0x4e204400 : 0x6e204400)|(sz<<22), r.d, r.n, cnt);
    if (!left) VEC2(0x6e20b800|(sz<<22), cnt, cnt);
    /* Clamp the full unsigned count, since SSHL examines only its low byte. */
    if (perlane) {
      VEC3(0x6e601c00, cnt, r.m, lim);
      VEC3(0x6e203400|(sz<<22), cnt, lim, r.m);
    } else {
      /* Scalar counts are clamped as unsigned words before broadcasting. */
      VEC2(0x4e000c00|((1u<<sz)<<16), cnt, h);
      emit_dnm(as, (A64Ins)(A64I_CSELw | (3u<<12)), h, g, h); /* LO */
      emit_nm(as, A64I_CMPw, g, h);
      emit_loadi(as, h, bits);
      return;
    }
    VEC2(0x4e000c00|((1u<<sz)<<16), lim, g);
    emit_loadi(as, g, bits);
  }
}

static void asm_vecfma(ASMState *as, IRIns *ir)
{
  IRIns *arg = IR(ir->op2);
  ARM64VecRegs r = asm_vecregs(as, ir, arg->op1);
  Reg c = ra_alloc1(as, arg->op2, r.allow);
  VEC3(0x4e20cc00 | (irt_type(ir->t) == IRT_V2F64 ? 0x400000 : 0),
       r.d, r.n, r.m);
  VECMOV(r.d, c);
}

static void asm_vecunary(ASMState *as, IRIns *ir)
{
  ARM64VecRegs r = asm_vecregs(as, ir, 0);
  unsigned sz = asm_veclogsz(irt_type(ir->t));
  uint32_t ai;
  if (ir->o == IR_VABS) ai = irt_isvecfp(ir->t) ?
    (0x4ea0f800 | (sz == 3 ? 0x400000 : 0)) : (0x4e20b800|(sz<<22));
  else if (ir->o == IR_VSQRT) ai = 0x6ea1f800 | (sz == 3 ? 0x400000 : 0);
  else {
    static const uint32_t roundai[4] = {
      0x4e218800, 0x4e219800, 0x4ea18800, 0x4ea19800
    };
    ai = roundai[ir->op2 & 3] | (sz == 3 ? 0x400000 : 0);
  }
  VEC2(ai, r.d, r.n);
}

static void asm_vecshuf(ASMState *as, IRIns *ir)
{
  ARM64VecRegs r = asm_vecregs(as, ir, 0);
  unsigned mode = ir->op2>>8, imm = ir->op2&255;
  int i;
  if (mode == IRVSHUF_PSRLDQ) {
    if (imm >= 16) VECZERO(r.d);
    else if (imm == 0) VECMOV(r.d, r.n);
    else {
      Reg z = asm_vectmp(as, &r);
      VEC3(0x6e000000|(imm<<11), r.d, r.n, z);
      VECZERO(z);
    }
  } else if (mode == IRVSHUF_PSHUFD) {
    for (i = 3; i >= 0; i--)
      emit_vecins(as, r.d, i, r.n, (imm>>(2*i))&3, 2);
  } else {
    unsigned first = mode == IRVSHUF_PSHUFLW ? 0 : 4;
    if (mode != IRVSHUF_PSHUFLW && mode != IRVSHUF_PSHUFHW)
      lj_trace_err(as->J, LJ_TRERR_NYIVEC);
    for (i = 3; i >= 0; i--)
      emit_vecins(as, r.d, first+i, r.n, first+((imm>>(2*i))&3), 1);
    VECMOV(r.d, r.n);
  }
}

static void asm_vecshuf2(ASMState *as, IRIns *ir)
{
  IRIns *arg = IR(ir->op2);
  uint32_t lit = (uint32_t)IR(arg->op2)->i;
  unsigned mode = lit>>8, imm = lit&255;
  ARM64VecRegs r;
  int i;
  if (mode == IRVSHUF2_INSERT) {
    unsigned sz = asm_veclogsz(irt_type(ir->t));
    int fp = irt_isvecfp(ir->t);
    Reg n;
    r = asm_vecregs(as, ir, 0);
    n = ra_alloc1(as, arg->op1, fp ? r.allow : RSET_GPR);
    if (fp) emit_vecins(as, r.d, imm, n, 0, sz);
    else emit_vecinsg(as, r.d, imm, n, sz);
    VECMOV(r.d, r.n);
    return;
  }
  r = asm_vecregs(as, ir, arg->op1);
  if (mode == IRVSHUF2_ALIGNR) {
    VEC3(0x6e000000|(imm<<11), r.d, r.m, r.n);
  } else if (mode == IRVSHUF2_SHUFPS) {
    for (i = 3; i >= 0; i--)
      emit_vecins(as, r.d, i, i<2 ? r.n : r.m, (imm>>(2*i))&3, 2);
  } else if (mode == IRVSHUF2_SHUFPD) {
    emit_vecins(as, r.d, 1, r.m, (imm>>1)&1, 3);
    emit_vecins(as, r.d, 0, r.n, imm&1, 3);
  } else {
    if (mode != IRVSHUF2_PBLENDW) lj_trace_err(as->J, LJ_TRERR_NYIVEC);
    for (i = 7; i >= 0; i--)
      emit_vecins(as, r.d, i, (imm&(1u<<i)) ? r.m : r.n, i, 1);
  }
}

static void asm_vecshufb(ASMState *as, IRIns *ir)
{
  ARM64VecRegs r = asm_vecregs(as, ir, ir->op2);
  Reg mask = asm_vectmp(as, &r);
  /* TBL zeroes any index >=16. Preserve bit7, discard bits4..6. */
  VEC3(0x4e000000, r.d, r.n, mask);
  VEC3(0x4e201c00, mask, r.m, mask);
  VEC2(0x4f00e400 | ((0x8fu&0xe0)<<11) | ((0x8fu&31)<<5), mask, 0);
}

/* Scalar gathering is deliberately independent of lane signedness. UMOV
** zero-extends, and one UBFM extracts exactly the sign bit of each lane.
*/
static void emit_vecmovmsk(ASMState *as, Reg d, Reg n, unsigned sz, Reg tmp)
{
  int i, lanes = 16>>sz;
  unsigned bit = (8u<<sz)-1;
  for (i = lanes-1; i >= 0; i--) {
    emit_dnm(as, (A64Ins)(A64I_ORRw | ((uint32_t)i<<10)), d, d, tmp);
    emit_dn(as, (A64Ins)((sz == 3 ? A64I_UBFMx : A64I_UBFMw) |
                        (bit<<16) | (bit<<10)), tmp, tmp);
    emit_vecget(as, tmp, n, i, sz);
    checkmclim(as);
  }
  emit_loadi(as, d, 0);
}

static void asm_vecmovmsk(ASMState *as, IRIns *ir)
{
  Reg d = ra_dest(as, ir, RSET_GPR);
  Reg n = ra_alloc1(as, ir->op1, RSET_FPR);
  Reg tmp = ra_scratch(as, rset_exclude(RSET_GPR, d));
  emit_vecmovmsk(as, d, n, asm_veclogsz(irvsrc_type(ir->op2)), tmp);
}

static void asm_vechminpos(ASMState *as, IRIns *ir)
{
  ARM64VecRegs r = asm_vecregs(as, ir, 0);
  Reg mask = asm_vectmp(as, &r);
  Reg g = ra_scratch(as, RSET_GPR);
  Reg tmp = ra_scratch(as, rset_exclude(RSET_GPR, g));
  emit_vecinsg(as, r.d, 1, g, 1);
  emit_dn(as, (A64Ins)0x5ac01000, g, g); /* CLZ */
  emit_dn(as, (A64Ins)0x5ac00000, g, g); /* RBIT */
  emit_vecmovmsk(as, g, mask, 1, tmp);
  VEC3(0x6e608c00, mask, mask, r.n);
  VEC2(0x4e020400, mask, r.d);
  VEC2(0x6e71a800, r.d, r.n); /* UMINV writes min, zeroes other lanes. */
}

static void asm_vecextract(ASMState *as, IRIns *ir)
{
  unsigned lane = irvsrc_imm(ir->op2);
  unsigned sz = asm_veclogsz(irvsrc_type(ir->op2));
  Reg d = ra_dest(as, ir, irt_isfp(ir->t) ? RSET_FPR : RSET_GPR);
  Reg n = ra_alloc1(as, ir->op1, RSET_FPR);
  if (irt_isfp(ir->t)) {
    /* Scalar DUP (Q=0) zeroes the unused destination bits. */
    VEC2(0x5e000400 | (((lane*2+1)<<sz)<<16), d, n);
  } else {
    /* The recorder applies byte/word sign extension to the low dword. */
    emit_vecget(as, d, n, lane, sz < 2 ? 2 : sz);
  }
}

static void asm_vecconv(ASMState *as, IRIns *ir)
{
  unsigned dk = irvconv_dst(ir->op2), sk = irvconv_src(ir->op2);
  ARM64VecRegs r = asm_vecregs(as, ir, 0);
  unsigned sz = veck_size(sk) == 8 ? 3 : 2;
  uint32_t f = sz == 3 ? 0x400000 : 0;
  if (veck_size(dk) != veck_size(sk)) lj_trace_err(as->J, LJ_TRERR_NYIVEC);
  if (!veck_isfp(dk) && veck_isfp(sk)) {
    Reg bound = asm_vectmp(as, &r), value = asm_vectmp(as, &r);
    Reg g = ra_scratch(as, RSET_GPR);
    /* FCVTZS saturates/returns zero on invalid inputs, whereas SSE returns
    ** INT_MIN. Select the indefinite value for abs(x)>=2^(bits-1) or NaN.
    ** The lower endpoint also selects INT_MIN, which is already correct.
    */
    VEC3(0x6e601c00, r.d, value, bound);
    VEC2(0x4e000c00 | ((1u<<sz)<<16), bound, g);
    emit_loadu64(as, g, sz == 3 ? U64x(80000000,00000000) : 0x80000000u);
    VEC2(0x4ea1b800|f, value, r.n);
    VEC3(0x6ea0e400|f, r.d, bound, value);
    VEC2(0x4ea0f800|f, value, r.n);
    VEC2(0x4e000c00 | ((1u<<sz)<<16), bound, g);
    emit_loadu64(as, g, sz == 3 ? U64x(43e00000,00000000) : 0x4f000000u);
  } else if (veck_isfp(dk) && !veck_isfp(sk)) {
    VEC2((veck_isunsigned(sk) ? 0x6e21d800 : 0x4e21d800)|f, r.d, r.n);
  } else {
    VECMOV(r.d, r.n);
  }
}

static void asm_vec(ASMState *as, IRIns *ir)
{
  IROp op = (IROp)ir->o;
  unsigned sz = asm_veclogsz(irt_type(ir->t));
  uint32_t ai;
  if (irt_isvec256(ir->t)) lj_trace_err(as->J, LJ_TRERR_NYIVEC);
  switch (op) {
  case IR_VSPLAT: asm_vecsplat(as, ir); return;
  case IR_VMUL:
    if (sz == 3 && !irt_isvecfp(ir->t)) { asm_vecmul64(as, ir); return; }
    break;
  case IR_VMULHI: case IR_VMULHIU: asm_vecmulhi(as, ir); return;
  case IR_VMIN: case IR_VMAX: case IR_VMINU: case IR_VMAXU:
    asm_vecminmax(as, ir); return;
  case IR_VSADU8: asm_vecsad(as, ir); return;
  case IR_VPMADW: asm_vecpmadd(as, ir); return;
  case IR_VHMINPOSU16: asm_vechminpos(as, ir); return;
  case IR_VSIGN: asm_vecsign(as, ir); return;
  case IR_VSHL: case IR_VSHR: case IR_VSAR: asm_vecshifts(as, ir, 0); return;
  case IR_VSHLV: case IR_VSHRV: case IR_VSARV: asm_vecshifts(as, ir, 1); return;
  case IR_VFMA: asm_vecfma(as, ir); return;
  case IR_VSQRT: case IR_VABS: case IR_VROUND: asm_vecunary(as, ir); return;
  case IR_VSHUF: asm_vecshuf(as, ir); return;
  case IR_VSHUF2: asm_vecshuf2(as, ir); return;
  case IR_VSHUFB: asm_vecshufb(as, ir); return;
  case IR_VMOVMSK: asm_vecmovmsk(as, ir); return;
  case IR_VEXTRACT: asm_vecextract(as, ir); return;
  case IR_VCONV: asm_vecconv(as, ir); return;
  default: break;
  }
  ai = asm_vecopcode(op, sz, irt_isvecfp(ir->t));
  if (!ai) lj_trace_err(as->J, LJ_TRERR_NYIVEC);
  {
    ARM64VecRegs r = asm_vecregs(as, ir, ir->op2);
    if (op == IR_VANDN) VEC3(ai, r.d, r.m, r.n);
    else VEC3(ai, r.d, r.n, r.m);
  }
}

#undef VEC3
#undef VEC2
#undef VECMOV
#undef VECZERO
#endif
