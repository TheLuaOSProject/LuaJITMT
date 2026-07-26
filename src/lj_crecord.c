/*
** Trace recorder for C data operations.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_ffrecord_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASJIT && LJ_HASFFI

#include "lj_err.h"
#include "lj_tab.h"
#include "lj_frame.h"
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lj_cparse.h"
#include "lj_cconv.h"
#include "lj_carith.h"
#include "lj_simd.h"
#include "lj_clib.h"
#include "lj_ccall.h"
#include "lj_ff.h"
#include "lj_ir.h"
#include "lj_jit.h"
#include "lj_ircall.h"
#include "lj_iropt.h"
#include "lj_trace.h"
#include "lj_record.h"
#include "lj_ffrecord.h"
#include "lj_snap.h"
#include "lj_crecord.h"
#include "lj_dispatch.h"
#include "lj_strfmt.h"
#include "lj_strscan.h"

/* Some local macros to save typing. Undef'd at the end. */
#define IR(ref)			(&J->cur.ir[(ref)])

/* Pass IR on to next optimization in chain (FOLD). */
#define emitir(ot, a, b)	(lj_ir_set(J, (ot), (a), (b)), lj_opt_fold(J))

#define emitconv(a, dt, st, flags) \
  emitir(IRT(IR_CONV, (dt)), (a), (st)|((dt) << 5)|(flags))

/* -- C type checks ------------------------------------------------------- */

static GCcdata *argv2cdata(jit_State *J, TRef tr, cTValue *o)
{
  GCcdata *cd;
  TRef trtypeid;
  if (!tref_iscdata(tr))
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  cd = cdataV(o);
  /* Specialize to the CTypeID. */
  trtypeid = emitir(IRT(IR_FLOAD, IRT_U16), tr, IRFL_CDATA_CTYPEID);
  emitir(IRTG(IR_EQ, IRT_INT), trtypeid, lj_ir_kint(J, (int32_t)cd->ctypeid));
  return cd;
}

/* Specialize to the CTypeID held by a cdata constructor. */
static CTypeID crec_constructor(jit_State *J, GCcdata *cd, TRef tr)
{
  CTypeID id;
  lj_assertJ(tref_iscdata(tr) && cd->ctypeid == CTID_CTYPEID,
	     "expected CTypeID cdata");
  id = *(CTypeID *)cdataptr(cd);
  tr = emitir(IRT(IR_FLOAD, IRT_INT), tr, IRFL_CDATA_INT);
  emitir(IRTG(IR_EQ, IRT_INT), tr, lj_ir_kint(J, (int32_t)id));
  return id;
}

static CTypeID argv2ctype(jit_State *J, TRef tr, cTValue *o)
{
  if (tref_isstr(tr)) {
    GCstr *s = strV(o);
    CPState cp;
    CTypeID oldtop;
    /* Specialize to the string containing the C type declaration. */
    emitir(IRTG(IR_EQ, IRT_STR), tr, lj_ir_kstr(J, s));
    cp.L = J->L;
    cp.cts = ctype_cts(J->L);
    oldtop = cp.cts->top;
    cp.srcname = strdata(s);
    cp.p = strdata(s);
    cp.param = NULL;
    cp.mode = CPARSE_MODE_ABSTRACT|CPARSE_MODE_NOIMPLICIT;
    if (lj_cparse(&cp) || cp.cts->top > oldtop)  /* Avoid new struct defs. */
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    return cp.val.id;
  } else {
    GCcdata *cd = argv2cdata(J, tr, o);
    return cd->ctypeid == CTID_CTYPEID ? crec_constructor(J, cd, tr) :
					cd->ctypeid;
  }
}

/* Convert CType to IRType (if possible). */
/* Map a vector ctype to its IR type, or IRT_NIL if the JIT cannot handle it. */
static IRType crec_vec2irt(CTState *cts, CType *ct)
{
#if LJ_SIMD_JITSIZE >= 16
  CTVecInfo vi;
  if (lj_ctype_vecinfo(cts, ct, &vi)) {
    CTSize size = (CTSize)vi.esize * vi.lanes;
    IRType width;
    if (size != 16 && size != 32) return IRT_NIL;
    if (size == 32 && !(L2J(cts->L)->flags & JIT_F_AVX2))
      return IRT_NIL;
    width = size == 32 ? IRT_VEC256 : 0;
    if (vi.kind == VECK_F32) return (IRType)(IRT_V4F32 | width);
    if (vi.kind == VECK_F64) return (IRType)(IRT_V2F64 | width);
    switch (vi.esize) {
    case 1: return (IRType)(IRT_V16I8 | width);
    case 2: return (IRType)(IRT_V8I16 | width);
    case 4: return (IRType)(IRT_V4I32 | width);
    default: return (IRType)(IRT_V2I64 | width);
    }
  }
#else
  UNUSED(cts); UNUSED(ct);
#endif
  return IRT_NIL;
}

static IRType crec_ct2irt(CTState *cts, CType *ct)
{
  if (ctype_isenum(ct->info)) ct = ctype_child(cts, ct);
  if (LJ_LIKELY(ctype_isnum(ct->info))) {
    if ((ct->info & CTF_FP)) {
      if (ct->size == sizeof(double))
	return IRT_NUM;
      else if (ct->size == sizeof(float))
	return IRT_FLOAT;
    } else {
      uint32_t b = lj_fls(ct->size);
      if (b <= 3)
	return IRT_I8 + 2*b + ((ct->info & CTF_UNSIGNED) ? 1 : 0);
    }
  } else if (ctype_isptr(ct->info)) {
    return (LJ_64 && ct->size == 8) ? IRT_P64 : IRT_P32;
  } else if (ctype_iscomplex(ct->info)) {
    if (ct->size == 2*sizeof(double))
      return IRT_NUM;
    else if (ct->size == 2*sizeof(float))
      return IRT_FLOAT;
  }
  return IRT_CDATA;
}

/* -- Optimized memory fill and copy -------------------------------------- */

/* Maximum length and unroll of inlined copy/fill. */
#define CREC_COPY_MAXUNROLL		16
#define CREC_COPY_MAXLEN		128

#define CREC_FILL_MAXUNROLL		16

/* Number of windowed registers used for optimized memory copy. */
#if LJ_TARGET_X86
#define CREC_COPY_REGWIN		2
#elif LJ_TARGET_PPC || LJ_TARGET_MIPS
#define CREC_COPY_REGWIN		8
#else
#define CREC_COPY_REGWIN		4
#endif

/* List of memory offsets for copy/fill. */
typedef struct CRecMemList {
  CTSize ofs;		/* Offset in bytes. */
  IRType tp;		/* Type of load/store. */
  TRef trofs;		/* TRef of interned offset. */
  TRef trval;		/* TRef of load value. */
} CRecMemList;

/* Generate copy list for element-wise struct copy. */
static MSize crec_copy_struct(CRecMemList *ml, CTState *cts, CType *ct)
{
  CTypeID fid = ct->sib;
  MSize mlp = 0;
  while (fid) {
    CType *df = ctype_get(cts, fid);
    fid = df->sib;
    if (ctype_isfield(df->info)) {
      CType *cct;
      IRType tp;
      if (!gcref(df->name)) continue;  /* Ignore unnamed fields. */
      cct = ctype_rawchild(cts, df);  /* Field type. */
      tp = crec_ct2irt(cts, cct);
      if (tp == IRT_CDATA) return 0;  /* NYI: aggregates. */
      if (mlp >= CREC_COPY_MAXUNROLL) return 0;
      ml[mlp].ofs = df->size;
      ml[mlp].tp = tp;
      mlp++;
      if (ctype_iscomplex(cct->info)) {
	if (mlp >= CREC_COPY_MAXUNROLL) return 0;
	ml[mlp].ofs = df->size + (cct->size >> 1);
	ml[mlp].tp = tp;
	mlp++;
      }
    } else if (!ctype_isconstval(df->info)) {
      /* NYI: bitfields and sub-structures. */
      return 0;
    }
  }
  return mlp;
}

/* Generate unrolled copy list, from highest to lowest step size/alignment. */
static MSize crec_copy_unroll(CRecMemList *ml, CTSize len, CTSize step,
			      IRType tp)
{
  CTSize ofs = 0;
  MSize mlp = 0;
  if (tp == IRT_CDATA) tp = IRT_U8 + 2*lj_fls(step);
  do {
    while (ofs + step <= len) {
      if (mlp >= CREC_COPY_MAXUNROLL) return 0;
      ml[mlp].ofs = ofs;
      ml[mlp].tp = tp;
      mlp++;
      ofs += step;
    }
    step >>= 1;
    tp -= 2;
  } while (ofs < len);
  return mlp;
}

/*
** Emit copy list with windowed loads/stores.
** LJ_TARGET_UNALIGNED: may emit unaligned loads/stores (not marked as such).
*/
static void crec_copy_emit(jit_State *J, CRecMemList *ml, MSize mlp,
			   TRef trdst, TRef trsrc)
{
  MSize i, j, rwin = 0;
  for (i = 0, j = 0; i < mlp; ) {
    TRef trofs = lj_ir_kintp(J, ml[i].ofs);
    TRef trsptr = emitir(IRT(IR_ADD, IRT_PTR), trsrc, trofs);
    ml[i].trval = emitir(IRT(IR_XLOAD, ml[i].tp), trsptr, 0);
    ml[i].trofs = trofs;
    i++;
    rwin += (LJ_SOFTFP32 && ml[i].tp == IRT_NUM) ? 2 : 1;
    if (rwin >= CREC_COPY_REGWIN || i >= mlp) {  /* Flush buffered stores. */
      rwin = 0;
      for ( ; j < i; j++) {
	TRef trdptr = emitir(IRT(IR_ADD, IRT_PTR), trdst, ml[j].trofs);
	emitir(IRT(IR_XSTORE, ml[j].tp), trdptr, ml[j].trval);
      }
    }
  }
}

/* Optimized memory copy. */
static void crec_copy(jit_State *J, TRef trdst, TRef trsrc, TRef trlen,
		      CType *ct)
{
  if (tref_isk(trlen)) {  /* Length must be constant. */
    CRecMemList ml[CREC_COPY_MAXUNROLL];
    MSize mlp = 0;
    CTSize step = 1, len = (CTSize)IR(tref_ref(trlen))->i;
    IRType tp = IRT_CDATA;
    int needxbar = 0;
    if (len == 0) return;  /* Shortcut. */
    if (len > CREC_COPY_MAXLEN) goto fallback;
    if (ct) {
      CTState *cts = ctype_ctsG(J2G(J));
      lj_assertJ(ctype_isarray(ct->info) || ctype_isstruct(ct->info),
		 "copy of non-aggregate");
      if (ctype_isarray(ct->info)) {
	CType *cct = ctype_rawchild(cts, ct);
	tp = crec_ct2irt(cts, cct);
	if (tp == IRT_CDATA) goto rawcopy;
	step = lj_ir_type_size[tp];
	lj_assertJ((len & (step-1)) == 0, "copy of fractional size");
      } else if ((ct->info & CTF_UNION)) {
	step = (1u << ctype_align(ct->info));
	goto rawcopy;
      } else {
	mlp = crec_copy_struct(ml, cts, ct);
	goto emitcopy;
      }
    } else {
    rawcopy:
      needxbar = 1;
      if (LJ_TARGET_UNALIGNED || step >= CTSIZE_PTR)
	step = CTSIZE_PTR;
    }
    mlp = crec_copy_unroll(ml, len, step, tp);
  emitcopy:
    if (mlp) {
      crec_copy_emit(J, ml, mlp, trdst, trsrc);
      if (needxbar)
	emitir(IRT(IR_XBAR, IRT_NIL), 0, 0);
      return;
    }
  }
fallback:
  /* Call memcpy. Always needs a barrier to disable alias analysis. */
  lj_ir_call(J, IRCALL_memcpy, trdst, trsrc, trlen);
  emitir(IRT(IR_XBAR, IRT_NIL), 0, 0);
}

/* Generate unrolled fill list, from highest to lowest step size/alignment. */
static MSize crec_fill_unroll(CRecMemList *ml, CTSize len, CTSize step)
{
  CTSize ofs = 0;
  MSize mlp = 0;
  IRType tp = IRT_U8 + 2*lj_fls(step);
  do {
    while (ofs + step <= len) {
      if (mlp >= CREC_COPY_MAXUNROLL) return 0;
      ml[mlp].ofs = ofs;
      ml[mlp].tp = tp;
      mlp++;
      ofs += step;
    }
    step >>= 1;
    tp -= 2;
  } while (ofs < len);
  return mlp;
}

/*
** Emit stores for fill list.
** LJ_TARGET_UNALIGNED: may emit unaligned stores (not marked as such).
*/
static void crec_fill_emit(jit_State *J, CRecMemList *ml, MSize mlp,
			   TRef trdst, TRef trfill)
{
  MSize i;
  for (i = 0; i < mlp; i++) {
    TRef trofs = lj_ir_kintp(J, ml[i].ofs);
    TRef trdptr = emitir(IRT(IR_ADD, IRT_PTR), trdst, trofs);
    emitir(IRT(IR_XSTORE, ml[i].tp), trdptr, trfill);
  }
}

/* Optimized memory fill. */
static void crec_fill(jit_State *J, TRef trdst, TRef trlen, TRef trfill,
		      CTSize step)
{
  if (tref_isk(trlen)) {  /* Length must be constant. */
    CRecMemList ml[CREC_FILL_MAXUNROLL];
    MSize mlp;
    CTSize len = (CTSize)IR(tref_ref(trlen))->i;
    if (len == 0) return;  /* Shortcut. */
    if (LJ_TARGET_UNALIGNED || step >= CTSIZE_PTR)
      step = CTSIZE_PTR;
    if (step * CREC_FILL_MAXUNROLL < len) goto fallback;
    mlp = crec_fill_unroll(ml, len, step);
    if (!mlp) goto fallback;
    if (tref_isk(trfill) || ml[0].tp != IRT_U8)
      trfill = emitconv(trfill, IRT_INT, IRT_U8, 0);
    if (ml[0].tp != IRT_U8) {  /* Scatter U8 to U16/U32/U64. */
      if (CTSIZE_PTR == 8 && ml[0].tp == IRT_U64) {
	if (tref_isk(trfill))  /* Pointless on x64 with zero-extended regs. */
	  trfill = emitconv(trfill, IRT_U64, IRT_U32, 0);
	trfill = emitir(IRT(IR_MUL, IRT_U64), trfill,
			lj_ir_kint64(J, U64x(01010101,01010101)));
      } else {
	trfill = emitir(IRTI(IR_MUL), trfill,
		   lj_ir_kint(J, ml[0].tp == IRT_U16 ? 0x0101 : 0x01010101));
      }
    }
    crec_fill_emit(J, ml, mlp, trdst, trfill);
  } else {
fallback:
    /* Call memset. Always needs a barrier to disable alias analysis. */
    lj_ir_call(J, IRCALL_memset, trdst, trfill, trlen);  /* Note: arg order! */
  }
  emitir(IRT(IR_XBAR, IRT_NIL), 0, 0);
}

/* -- Convert C type to C type -------------------------------------------- */

/*
** This code mirrors the code in lj_cconv.c. It performs the same steps
** for the trace recorder that lj_cconv.c does for the interpreter.
**
** One major difference is that we can get away with much fewer checks
** here. E.g. checks for casts, constness or correct types can often be
** omitted, even if they might fail. The interpreter subsequently throws
** an error, which aborts the trace.
**
** All operations are specialized to their C types, so the on-trace
** outcome must be the same as the outcome in the interpreter. If the
** interpreter doesn't throw an error, then the trace is correct, too.
** Care must be taken not to generate invalid (temporary) IR or to
** trigger asserts.
*/

/* Determine whether a passed number or cdata number is non-zero. */
static int crec_isnonzero(CType *s, void *p)
{
  if (p == (void *)0)
    return 0;
  if (p == (void *)1)
    return 1;
  if ((s->info & CTF_FP)) {
    if (s->size == sizeof(float))
      return (*(float *)p != 0);
    else
      return (*(double *)p != 0);
  } else {
    if (s->size == 1)
      return (*(uint8_t *)p != 0);
    else if (s->size == 2)
      return (*(uint16_t *)p != 0);
    else if (s->size == 4)
      return (*(uint32_t *)p != 0);
    else
      return (*(uint64_t *)p != 0);
  }
}

static TRef crec_vec_splat(jit_State *J, CTState *cts, IRType vt,
			   const CTVecInfo *vi, TRef sp, CType *sct,
			   cTValue *sval);
static TRef crec_vec_box(jit_State *J, TRef val, IRType vt, CTypeID id);

static TRef crec_ct_ct(jit_State *J, CType *d, CType *s, TRef dp, TRef sp,
		       void *svisnz)
{
  IRType dt = crec_ct2irt(ctype_ctsG(J2G(J)), d);
  IRType st = crec_ct2irt(ctype_ctsG(J2G(J)), s);
  CTSize dsize = d->size, ssize = s->size;
  CTInfo dinfo = d->info, sinfo = s->info;

  if (ctype_type(dinfo) > CT_MAYCONVERT || ctype_type(sinfo) > CT_MAYCONVERT)
    goto err_conv;

  /*
  ** Note: Unlike lj_cconv_ct_ct(), sp holds the _value_ of pointers and
  ** numbers up to 8 bytes. Otherwise sp holds a pointer.
  */

  switch (cconv_idx2(dinfo, sinfo)) {
  /* Destination is a bool. */
  case CCX(B, B):
    goto xstore;  /* Source operand is already normalized. */
  case CCX(B, I):
  case CCX(B, F):
    if (st != IRT_CDATA) {
      /* Specialize to the result of a comparison against 0. */
      TRef zero = (st == IRT_NUM  || st == IRT_FLOAT) ? lj_ir_knum(J, 0) :
		  (st == IRT_I64 || st == IRT_U64) ? lj_ir_kint64(J, 0) :
		  lj_ir_kint(J, 0);
      int isnz = crec_isnonzero(s, svisnz);
      emitir(IRTG(isnz ? IR_NE : IR_EQ, st), sp, zero);
      sp = lj_ir_kint(J, isnz);
      goto xstore;
    }
    goto err_nyi;

  /* Destination is an integer. */
  case CCX(I, B):
  case CCX(I, I):
  conv_I_I:
    if (dt == IRT_CDATA || st == IRT_CDATA) goto err_nyi;
    /* Extend 32 to 64 bit integer. */
    if (dsize == 8 && ssize < 8 && !(LJ_64 && (sinfo & CTF_UNSIGNED)))
      sp = emitconv(sp, dt, ssize < 4 ? IRT_INT : st,
		    (sinfo & CTF_UNSIGNED) ? 0 : IRCONV_SEXT);
    else if (dsize < 8 && ssize == 8)  /* Truncate from 64 bit integer. */
      sp = emitconv(sp, dsize < 4 ? IRT_INT : dt, st, 0);
    else if (st == IRT_INT)
      sp = lj_opt_narrow_toint(J, sp);
  xstore:
    if (dt == IRT_I64 || dt == IRT_U64) lj_needsplit(J);
    if (dp == 0) return sp;
    emitir(IRT(IR_XSTORE, dt), dp, sp);
    break;
  case CCX(I, C):
    sp = emitir(IRT(IR_XLOAD, st), sp, 0);  /* Load re. */
    /* fallthrough */
  case CCX(I, F):
    if (dt == IRT_CDATA || st == IRT_CDATA) goto err_nyi;
  conv_I_F:
#if LJ_SOFTFP || LJ_32
    if (st == IRT_FLOAT) {  /* Uncommon. Simplify split backends. */
      sp = emitconv(sp, IRT_NUM, IRT_FLOAT, 0);
      st = IRT_NUM;
    }
#endif
    if (dsize < 8) {
      lj_needsplit(J);
      sp = emitconv(sp, IRT_I64, st, IRCONV_ANY);
      sp = emitconv(sp, dsize < 4 ? IRT_INT : dt, IRT_I64, 0);
    } else {
      sp = emitconv(sp, dt, st, IRCONV_ANY);
    }
    goto xstore;
  case CCX(I, P):
  case CCX(I, A):
    sinfo = CTINFO(CT_NUM, CTF_UNSIGNED);
    ssize = CTSIZE_PTR;
    st = IRT_UINTP;
    if (((dsize ^ ssize) & 8) == 0) {  /* Must insert no-op type conversion. */
      sp = emitconv(sp, dsize < 4 ? IRT_INT : dt, IRT_PTR, 0);
      goto xstore;
    }
    goto conv_I_I;

  /* Destination is a floating-point number. */
  case CCX(F, B):
  case CCX(F, I):
  conv_F_I:
    if (dt == IRT_CDATA || st == IRT_CDATA) goto err_nyi;
    sp = emitconv(sp, dt, ssize < 4 ? IRT_INT : st, 0);
    goto xstore;
  case CCX(F, C):
    sp = emitir(IRT(IR_XLOAD, st), sp, 0);  /* Load re. */
    /* fallthrough */
  case CCX(F, F):
  conv_F_F:
    if (dt == IRT_CDATA || st == IRT_CDATA) goto err_nyi;
    if (dt != st) sp = emitconv(sp, dt, st, 0);
    goto xstore;

  /* Destination is a complex number. */
  case CCX(C, I):
  case CCX(C, F):
    {  /* Clear im. */
      TRef ptr = emitir(IRT(IR_ADD, IRT_PTR), dp, lj_ir_kintp(J, (dsize >> 1)));
      emitir(IRT(IR_XSTORE, dt), ptr, lj_ir_knum(J, 0));
    }
    /* Convert to re. */
    if ((sinfo & CTF_FP)) goto conv_F_F; else goto conv_F_I;

  case CCX(C, C):
    if (dt == IRT_CDATA || st == IRT_CDATA) goto err_nyi;
    {
      TRef re, im, ptr;
      re = emitir(IRT(IR_XLOAD, st), sp, 0);
      ptr = emitir(IRT(IR_ADD, IRT_PTR), sp, lj_ir_kintp(J, (ssize >> 1)));
      im = emitir(IRT(IR_XLOAD, st), ptr, 0);
      if (dt != st) {
	re = emitconv(re, dt, st, 0);
	im = emitconv(im, dt, st, 0);
      }
      emitir(IRT(IR_XSTORE, dt), dp, re);
      ptr = emitir(IRT(IR_ADD, IRT_PTR), dp, lj_ir_kintp(J, (dsize >> 1)));
      emitir(IRT(IR_XSTORE, dt), ptr, im);
    }
    break;

  /* Destination is a vector. */
  case CCX(V, I):
  case CCX(V, F): {
    /* Constructing a vector from one scalar splats it across all lanes. */
    CTState *cts2 = ctype_ctsG(J2G(J));
    CTVecInfo vi;
    IRType vt;
    if (dp == 0 || !lj_ctype_vecinfo(cts2, d, &vi)) goto err_nyi;
    vt = crec_vec2irt(cts2, d);
    if (vt == IRT_NIL) goto err_nyi;
    sp = crec_vec_splat(J, cts2, vt, &vi, sp, s, NULL);
    emitir(IRT(IR_XSTORE, vt), dp, sp);
    break;
    }
  case CCX(V, V): {
    /* Same-sized vectors are copied bit for bit, like lj_cconv_ct_ct(). */
    CTState *cts2 = ctype_ctsG(J2G(J));
    IRType vt;
    if (dp == 0 || dsize != ssize) goto err_nyi;
    vt = crec_vec2irt(cts2, d);
    if (vt == IRT_NIL) vt = crec_vec2irt(cts2, s);
    if (vt == IRT_NIL) goto err_nyi;
    emitir(IRT(IR_XSTORE, vt), dp, emitir(IRT(IR_XLOAD, vt), sp, 0));
    break;
    }
  case CCX(V, C):
    goto err_nyi;

  /* Destination is a pointer. */
  case CCX(P, P):
  case CCX(P, A):
  case CCX(P, S):
    /* There are only 32 bit pointers/addresses on 32 bit machines.
    ** Also ok on x64, since all 32 bit ops clear the upper part of the reg.
    */
    goto xstore;
  case CCX(P, I):
    if (st == IRT_CDATA) goto err_nyi;
    if (!LJ_64 && ssize == 8)  /* Truncate from 64 bit integer. */
      sp = emitconv(sp, IRT_U32, st, 0);
    goto xstore;
  case CCX(P, F):
    if (st == IRT_CDATA) goto err_nyi;
    /* The signed 64 bit conversion is cheaper. */
    dt = (LJ_64 && dsize == 8) ? IRT_I64 : IRT_U32;
    goto conv_I_F;

  /* Destination is an array. */
  case CCX(A, A):
  /* Destination is a struct/union. */
  case CCX(S, S):
    if (dp == 0) goto err_conv;
    crec_copy(J, dp, sp, lj_ir_kint(J, dsize), d);
    break;

  default:
  err_conv:
  err_nyi:
    lj_trace_err(J, LJ_TRERR_NYICONV);
    break;
  }
  return 0;
}

/* -- Convert C type to TValue (load) ------------------------------------- */

static TRef crec_tv_ct(jit_State *J, CType *s, CTypeID sid, TRef sp)
{
  CTState *cts = ctype_ctsG(J2G(J));
  IRType t = crec_ct2irt(cts, s);
  CTInfo sinfo = s->info;
  if (ctype_isnum(sinfo)) {
    TRef tr;
    if (t == IRT_CDATA)
      goto err_nyi;  /* NYI: copyval of >64 bit integers. */
    tr = emitir(IRT(IR_XLOAD, t), sp, 0);
    if (t == IRT_FLOAT || t == IRT_U32) {  /* Keep uint32_t/float as numbers. */
      return emitconv(tr, IRT_NUM, t, 0);
    } else if (t == IRT_I64 || t == IRT_U64) {  /* Box 64 bit integer. */
      sp = tr;
      lj_needsplit(J);
    } else if ((sinfo & CTF_BOOL)) {
      /* Assume not equal to zero. Fixup and emit pending guard later. */
      lj_ir_set(J, IRTGI(IR_NE), tr, lj_ir_kint(J, 0));
      J->postproc = LJ_POST_FIXGUARD;
      return TREF_TRUE;
    } else {
      return tr;
    }
  } else if (ctype_isptr(sinfo) || ctype_isenum(sinfo)) {
    sp = emitir(IRT(IR_XLOAD, t), sp, 0);  /* Box pointers and enums. */
  } else if (ctype_isrefarray(sinfo) || ctype_isstruct(sinfo)) {
    cts->L = J->L;
    sid = lj_ctype_intern(cts, CTINFO_REF(sid), CTSIZE_PTR);  /* Create ref. */
  } else if (ctype_iscomplex(sinfo)) {  /* Unbox/box complex. */
    ptrdiff_t esz = (ptrdiff_t)(s->size >> 1);
    TRef ptr, tr1, tr2, dp;
    dp = emitir(IRTG(IR_CNEW, IRT_CDATA), lj_ir_kint(J, sid), TREF_NIL);
    tr1 = emitir(IRT(IR_XLOAD, t), sp, 0);
    ptr = emitir(IRT(IR_ADD, IRT_PTR), sp, lj_ir_kintp(J, esz));
    tr2 = emitir(IRT(IR_XLOAD, t), ptr, 0);
    ptr = emitir(IRT(IR_ADD, IRT_PTR), dp, lj_ir_kintp(J, sizeof(GCcdata)));
    emitir(IRT(IR_XSTORE, t), ptr, tr1);
    ptr = emitir(IRT(IR_ADD, IRT_PTR), dp, lj_ir_kintp(J, sizeof(GCcdata)+esz));
    emitir(IRT(IR_XSTORE, t), ptr, tr2);
    return dp;
  } else if (ctype_isvector(sinfo)) {  /* Box a vector value. */
    IRType vt = crec_vec2irt(cts, s);
    if (vt == IRT_NIL) goto err_nyi;
    return crec_vec_box(J, emitir(IRT(IR_XLOAD, vt), sp, 0), vt, sid);
  } else {
  err_nyi:
    lj_trace_err(J, LJ_TRERR_NYICONV);
  }
  /* Box pointer, ref, enum or 64 bit integer. */
  return emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, sid), sp);
}

/* -- Convert TValue to C type (store) ------------------------------------ */

static TRef crec_ct_tv(jit_State *J, CType *d, TRef dp, TRef sp, cTValue *sval)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTypeID sid = CTID_P_VOID;
  void *svisnz = 0;
  CType *s;
  if (LJ_LIKELY(tref_isinteger(sp))) {
    sid = CTID_INT32;
    svisnz = (void *)(intptr_t)(tvisint(sval)?(intV(sval)!=0):!tviszero(sval));
  } else if (tref_isnum(sp)) {
    sid = CTID_DOUBLE;
    svisnz = (void *)(intptr_t)(tvisint(sval)?(intV(sval)!=0):!tviszero(sval));
  } else if (tref_isbool(sp)) {
    sp = lj_ir_kint(J, tref_istrue(sp) ? 1 : 0);
    sid = CTID_BOOL;
  } else if (tref_isnil(sp)) {
    sp = lj_ir_kptr(J, NULL);
  } else if (tref_isudata(sp)) {
    GCudata *ud = udataV(sval);
    if (ud->udtype == UDTYPE_IO_FILE || ud->udtype == UDTYPE_BUFFER) {
      TRef tr = emitir(IRT(IR_FLOAD, IRT_U8), sp, IRFL_UDATA_UDTYPE);
      emitir(IRTGI(IR_EQ), tr, lj_ir_kint(J, ud->udtype));
      sp = emitir(IRT(IR_FLOAD, IRT_PTR), sp,
		  ud->udtype == UDTYPE_IO_FILE ? IRFL_UDATA_FILE :
						 IRFL_SBUF_R);
    } else {
      sp = emitir(IRT(IR_ADD, IRT_PTR), sp, lj_ir_kintp(J, sizeof(GCudata)));
    }
  } else if (tref_isstr(sp)) {
    if (ctype_isenum(d->info)) {  /* Match string against enum constant. */
      GCstr *str = strV(sval);
      CTSize ofs;
      CType *cct = lj_ctype_getfield(cts, d, str, &ofs);
      /* Specialize to the name of the enum constant. */
      emitir(IRTG(IR_EQ, IRT_STR), sp, lj_ir_kstr(J, str));
      if (cct && ctype_isconstval(cct->info)) {
	lj_assertJ(ctype_child(cts, cct)->size == 4,
		   "only 32 bit const supported");  /* NYI */
	svisnz = (void *)(intptr_t)(ofs != 0);
	sp = lj_ir_kint(J, (int32_t)ofs);
	sid = ctype_cid(cct->info);
      }  /* else: interpreter will throw. */
    } else if (ctype_isrefarray(d->info)) {  /* Copy string to array. */
      lj_trace_err(J, LJ_TRERR_BADTYPE);  /* NYI */
    } else {  /* Otherwise pass the string data as a const char[]. */
      /* Don't use STRREF. It folds with SNEW, which loses the trailing NUL. */
      sp = emitir(IRT(IR_ADD, IRT_PTR), sp, lj_ir_kintp(J, sizeof(GCstr)));
      sid = CTID_A_CCHAR;
    }
  } else if (tref_islightud(sp)) {
#if LJ_64
    lj_trace_err(J, LJ_TRERR_NYICONV);
#endif
  } else {  /* NYI: tref_istab(sp). */
    IRType t;
    sid = argv2cdata(J, sp, sval)->ctypeid;
    s = ctype_raw(cts, sid);
    svisnz = cdataptr(cdataV(sval));
    if (ctype_isfunc(s->info)) {
      sid = lj_ctype_intern(cts, CTINFO(CT_PTR, CTALIGN_PTR|sid), CTSIZE_PTR);
      s = ctype_get(cts, sid);
      t = IRT_PTR;
    } else {
      t = crec_ct2irt(cts, s);
    }
    if (ctype_isptr(s->info)) {
      sp = emitir(IRT(IR_FLOAD, t), sp, IRFL_CDATA_PTR);
      if (ctype_isref(s->info)) {
	svisnz = *(void **)svisnz;
	s = ctype_rawchild(cts, s);
	if (ctype_isenum(s->info)) s = ctype_child(cts, s);
	t = crec_ct2irt(cts, s);
      } else {
	goto doconv;
      }
    } else if (t == IRT_I64 || t == IRT_U64) {
      sp = emitir(IRT(IR_FLOAD, t), sp, IRFL_CDATA_INT64);
      lj_needsplit(J);
      goto doconv;
    } else if (t == IRT_INT || t == IRT_U32) {
      if (ctype_isenum(s->info)) s = ctype_child(cts, s);
      sp = emitir(IRT(IR_FLOAD, t), sp, IRFL_CDATA_INT);
      goto doconv;
    } else {
      sp = emitir(IRT(IR_ADD, IRT_PTR), sp, lj_ir_kintp(J, sizeof(GCcdata)));
    }
    if (ctype_isnum(s->info) && t != IRT_CDATA)
      sp = emitir(IRT(IR_XLOAD, t), sp, 0);  /* Load number value. */
    goto doconv;
  }
  s = ctype_get(cts, sid);
doconv:
  if (ctype_isenum(d->info)) d = ctype_child(cts, d);
  return crec_ct_ct(J, d, s, dp, sp, svisnz);
}

/* -- C data metamethods -------------------------------------------------- */

/* This would be rather difficult in FOLD, so do it here:
** (base+k)+(idx*sz)+ofs ==> (base+idx*sz)+(ofs+k)
** (base+(idx+k)*sz)+ofs ==> (base+idx*sz)+(ofs+k*sz)
*/
static TRef crec_reassoc_ofs(jit_State *J, TRef tr, ptrdiff_t *ofsp, MSize sz)
{
  IRIns *ir = IR(tref_ref(tr));
  if (LJ_LIKELY(J->flags & JIT_F_OPT_FOLD) && irref_isk(ir->op2) &&
      (ir->o == IR_ADD || ir->o == IR_ADDOV || ir->o == IR_SUBOV)) {
    IRIns *irk = IR(ir->op2);
    ptrdiff_t k;
    if (LJ_64 && irk->o == IR_KINT64)
      k = (ptrdiff_t)ir_kint64(irk)->u64 * sz;
    else
      k = (ptrdiff_t)irk->i * sz;
    if (ir->o == IR_SUBOV) *ofsp -= k; else *ofsp += k;
    tr = ir->op1;  /* Not a TRef, but the caller doesn't care. */
  }
  return tr;
}

/* Tailcall to function. */
static void crec_tailcall(jit_State *J, RecordFFData *rd, cTValue *tv)
{
  TRef kfunc = lj_ir_kfunc(J, funcV(tv));
#if LJ_FR2
  J->base[-2] = kfunc;
  J->base[-1] = TREF_FRAME;
#else
  J->base[-1] = kfunc | TREF_FRAME;
#endif
  rd->nres = -1;  /* Pending tailcall. */
}

/* Record ctype __index/__newindex metamethods. */
static void crec_index_meta(jit_State *J, CTState *cts, CType *ct,
			    RecordFFData *rd)
{
  CTypeID id = ctype_typeid(cts, ct);
  cTValue *tv = lj_ctype_meta(cts, id, rd->data ? MM_newindex : MM_index);
  if (!tv)
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  if (tvisfunc(tv)) {
    crec_tailcall(J, rd, tv);
  } else if (rd->data == 0 && tvistab(tv) && tref_isstr(J->base[1])) {
    /* Specialize to result of __index lookup. */
    cTValue *o = lj_tab_get(J->L, tabV(tv), &rd->argv[1]);
    J->base[0] = lj_record_constify(J, o);
    if (!J->base[0])
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    /* Always specialize to the key. */
    emitir(IRTG(IR_EQ, IRT_STR), J->base[1], lj_ir_kstr(J, strV(&rd->argv[1])));
  } else {
    /* NYI: resolving of non-function metamethods. */
    /* NYI: non-string keys for __index table. */
    /* NYI: stores to __newindex table. */
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  }
}

/* Record bitfield load/store. */
static void crec_index_bf(jit_State *J, RecordFFData *rd, TRef ptr, CTInfo info)
{
  IRType t = IRT_I8 + 2*lj_fls(ctype_bitcsz(info)) + ((info&CTF_UNSIGNED)?1:0);
  TRef tr = emitir(IRT(IR_XLOAD, t), ptr, 0);
  CTSize pos = ctype_bitpos(info), bsz = ctype_bitbsz(info), shift = 32 - bsz;
  lj_assertJ(t <= IRT_U32, "only 32 bit bitfields supported");  /* NYI */
  if (rd->data == 0) {  /* __index metamethod. */
    if ((info & CTF_BOOL)) {
      tr = emitir(IRTI(IR_BAND), tr, lj_ir_kint(J, (int32_t)((1u << pos))));
      /* Assume not equal to zero. Fixup and emit pending guard later. */
      lj_ir_set(J, IRTGI(IR_NE), tr, lj_ir_kint(J, 0));
      J->postproc = LJ_POST_FIXGUARD;
      tr = TREF_TRUE;
    } else if (!(info & CTF_UNSIGNED)) {
      tr = emitir(IRTI(IR_BSHL), tr, lj_ir_kint(J, shift - pos));
      tr = emitir(IRTI(IR_BSAR), tr, lj_ir_kint(J, shift));
    } else {
      lj_assertJ(bsz < 32, "unexpected full bitfield index");
      tr = emitir(IRTI(IR_BSHR), tr, lj_ir_kint(J, pos));
      tr = emitir(IRTI(IR_BAND), tr, lj_ir_kint(J, (int32_t)((1u << bsz)-1)));
      /* We can omit the U32 to NUM conversion, since bsz < 32. */
    }
    J->base[0] = tr;
  } else {  /* __newindex metamethod. */
    CTState *cts = ctype_ctsG(J2G(J));
    CType *ct = ctype_get(cts,
			  (info & CTF_BOOL) ? CTID_BOOL :
			  (info & CTF_UNSIGNED) ? CTID_UINT32 : CTID_INT32);
    int32_t mask = (int32_t)(((1u << bsz)-1) << pos);
    TRef sp = crec_ct_tv(J, ct, 0, J->base[2], &rd->argv[2]);
    sp = emitir(IRTI(IR_BSHL), sp, lj_ir_kint(J, pos));
    /* Use of the target type avoids forwarding conversions. */
    sp = emitir(IRT(IR_BAND, t), sp, lj_ir_kint(J, mask));
    tr = emitir(IRT(IR_BAND, t), tr, lj_ir_kint(J, (int32_t)~mask));
    tr = emitir(IRT(IR_BOR, t), tr, sp);
    emitir(IRT(IR_XSTORE, t), ptr, tr);
    rd->nres = 0;
    J->needsnap = 1;
  }
}

void LJ_FASTCALL recff_cdata_index(jit_State *J, RecordFFData *rd)
{
  TRef idx, ptr = J->base[0];
  ptrdiff_t ofs = sizeof(GCcdata);
  GCcdata *cd = argv2cdata(J, ptr, &rd->argv[0]);
  CTState *cts = ctype_ctsG(J2G(J));
  CType *ct = ctype_raw(cts, cd->ctypeid);
  CTypeID sid = 0;

  /* Resolve pointer or reference for cdata object. */
  if (ctype_isptr(ct->info)) {
    IRType t = (LJ_64 && ct->size == 8) ? IRT_P64 : IRT_P32;
    if (ctype_isref(ct->info)) ct = ctype_rawchild(cts, ct);
    ptr = emitir(IRT(IR_FLOAD, t), ptr, IRFL_CDATA_PTR);
    ofs = 0;
    ptr = crec_reassoc_ofs(J, ptr, &ofs, 1);
  }

again:
  idx = J->base[1];
  if (tref_isnumber(idx)) {
    idx = lj_opt_narrow_cindex(J, idx);
    if (ctype_ispointer(ct->info)) {
      CTSize sz;
  integer_key:
      if ((ct->info & CTF_COMPLEX))
	idx = emitir(IRT(IR_BAND, IRT_INTP), idx, lj_ir_kintp(J, 1));
      sz = lj_ctype_size(cts, (sid = ctype_cid(ct->info)));
      idx = crec_reassoc_ofs(J, idx, &ofs, sz);
#if LJ_TARGET_ARM || LJ_TARGET_PPC
      /* Hoist base add to allow fusion of index/shift into operands. */
      if (LJ_LIKELY(J->flags & JIT_F_OPT_LOOP) && ofs
#if LJ_TARGET_ARM
	  && (sz == 1 || sz == 4)
#endif
	  ) {
	ptr = emitir(IRT(IR_ADD, IRT_PTR), ptr, lj_ir_kintp(J, ofs));
	ofs = 0;
      }
#endif
      idx = emitir(IRT(IR_MUL, IRT_INTP), idx, lj_ir_kintp(J, sz));
      ptr = emitir(IRT(IR_ADD, IRT_PTR), idx, ptr);
    }
  } else if (tref_iscdata(idx)) {
    GCcdata *cdk = cdataV(&rd->argv[1]);
    CType *ctk = ctype_raw(cts, cdk->ctypeid);
    IRType t = crec_ct2irt(cts, ctk);
    if (ctype_ispointer(ct->info) && t >= IRT_I8 && t <= IRT_U64) {
      if (ctk->size == 8) {
	idx = emitir(IRT(IR_FLOAD, t), idx, IRFL_CDATA_INT64);
      } else if (ctk->size == 4) {
	idx = emitir(IRT(IR_FLOAD, t), idx, IRFL_CDATA_INT);
      } else {
	idx = emitir(IRT(IR_ADD, IRT_PTR), idx,
		     lj_ir_kintp(J, sizeof(GCcdata)));
	idx = emitir(IRT(IR_XLOAD, t), idx, 0);
      }
      if (LJ_64 && ctk->size < sizeof(intptr_t) && !(ctk->info & CTF_UNSIGNED))
	idx = emitconv(idx, IRT_INTP, IRT_INT, IRCONV_SEXT);
      if (!LJ_64 && ctk->size > sizeof(intptr_t)) {
	idx = emitconv(idx, IRT_INTP, t, 0);
	lj_needsplit(J);
      }
      goto integer_key;
    }
  } else if (tref_isstr(idx)) {
    GCstr *name = strV(&rd->argv[1]);
    if (cd && cd->ctypeid == CTID_CTYPEID)
      ct = ctype_raw(cts, crec_constructor(J, cd, ptr));
    if (ctype_isstruct(ct->info)) {
      CTSize fofs;
      CType *fct;
      fct = lj_ctype_getfield(cts, ct, name, &fofs);
      if (fct) {
	ofs += (ptrdiff_t)fofs;
	/* Always specialize to the field name. */
	emitir(IRTG(IR_EQ, IRT_STR), idx, lj_ir_kstr(J, name));
	if (ctype_isconstval(fct->info)) {
	  if (fct->size >= 0x80000000u &&
	      (ctype_child(cts, fct)->info & CTF_UNSIGNED)) {
	    J->base[0] = lj_ir_knum(J, (lua_Number)(uint32_t)fct->size);
	    return;
	  }
	  J->base[0] = lj_ir_kint(J, (int32_t)fct->size);
	  return;  /* Interpreter will throw for newindex. */
	} else if (cd && cd->ctypeid == CTID_CTYPEID) {
	  /* Only resolve constants and metamethods for constructors. */
	} else if (ctype_isbitfield(fct->info)) {
	  if (ofs)
	    ptr = emitir(IRT(IR_ADD, IRT_PTR), ptr, lj_ir_kintp(J, ofs));
	  crec_index_bf(J, rd, ptr, fct->info);
	  return;
	} else {
	  lj_assertJ(ctype_isfield(fct->info), "field expected");
	  sid = ctype_cid(fct->info);
	}
      }
    } else if (ctype_iscomplex(ct->info)) {
      if (name->len == 2 &&
	  ((strdata(name)[0] == 'r' && strdata(name)[1] == 'e') ||
	   (strdata(name)[0] == 'i' && strdata(name)[1] == 'm'))) {
	/* Always specialize to the field name. */
	emitir(IRTG(IR_EQ, IRT_STR), idx, lj_ir_kstr(J, name));
	if (strdata(name)[0] == 'i') ofs += (ct->size >> 1);
	sid = ctype_cid(ct->info);
      }
    }
  }
  if (!sid) {
    if (ctype_isptr(ct->info)) {  /* Automatically perform '->'. */
      CType *cct = ctype_rawchild(cts, ct);
      if (ctype_isstruct(cct->info)) {
	ct = cct;
	cd = NULL;
	if (tref_isstr(idx)) goto again;
      }
    }
    crec_index_meta(J, cts, ct, rd);
    return;
  }

  if (ofs)
    ptr = emitir(IRT(IR_ADD, IRT_PTR), ptr, lj_ir_kintp(J, ofs));

  /* Resolve reference for field. */
  ct = ctype_get(cts, sid);
  if (ctype_isref(ct->info)) {
    ptr = emitir(IRT(IR_XLOAD, IRT_PTR), ptr, 0);
    sid = ctype_cid(ct->info);
    ct = ctype_get(cts, sid);
  }

  while (ctype_isattrib(ct->info))
    ct = ctype_child(cts, ct);  /* Skip attributes. */

  if (rd->data == 0) {  /* __index metamethod. */
    J->base[0] = crec_tv_ct(J, ct, sid, ptr);
  } else {  /* __newindex metamethod. */
    rd->nres = 0;
    J->needsnap = 1;
    crec_ct_tv(J, ct, ptr, J->base[2], &rd->argv[2]);
  }
}

/* Record setting a finalizer. */
static void crec_finalizer(jit_State *J, TRef trcd, TRef trfin, cTValue *fin)
{
  if (tvisgcv(fin)) {
    if (!trfin) trfin = lj_ir_kptr(J, gcval(fin));
  } else if (tvisnil(fin)) {
    trfin = lj_ir_kptr(J, NULL);
  } else {
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  }
  lj_ir_call(J, IRCALL_lj_cdata_setfin, trcd,
	     trfin, lj_ir_kint(J, (int32_t)itype(fin)));
  J->needsnap = 1;
}

/* Record cdata allocation. */
static void crec_alloc(jit_State *J, RecordFFData *rd, CTypeID id)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTSize sz;
  CTInfo info = lj_ctype_info(cts, id, &sz);
  CType *d = ctype_raw(cts, id);
  TRef trcd, trid = lj_ir_kint(J, id);
  cTValue *fin;
  /* Use special instruction to box pointer or 32/64 bit integer. */
  if (ctype_isptr(info) || (ctype_isinteger(info) && (sz == 4 || sz == 8))) {
    TRef sp = J->base[1] ? crec_ct_tv(J, d, 0, J->base[1], &rd->argv[1]) :
	      ctype_isptr(info) ? lj_ir_kptr(J, NULL) :
	      sz == 4 ? lj_ir_kint(J, 0) :
	      (lj_needsplit(J), lj_ir_kint64(J, 0));
    J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), trid, sp);
    return;
  } else {
    TRef trsz = TREF_NIL;
    if ((info & CTF_VLA)) {  /* Calculate VLA/VLS size at runtime. */
      CTSize sz0, sz1;
      if (!J->base[1] || J->base[2])
	lj_trace_err(J, LJ_TRERR_NYICONV);  /* NYI: init VLA/VLS. */
      trsz = crec_ct_tv(J, ctype_get(cts, CTID_INT32), 0,
			J->base[1], &rd->argv[1]);
      sz0 = lj_ctype_vlsize(cts, d, 0);
      sz1 = lj_ctype_vlsize(cts, d, 1);
      trsz = emitir(IRTGI(IR_MULOV), trsz, lj_ir_kint(J, (int32_t)(sz1-sz0)));
      trsz = emitir(IRTGI(IR_ADDOV), trsz, lj_ir_kint(J, (int32_t)sz0));
      J->base[1] = 0;  /* Simplify logic below. */
    } else if (ctype_align(info) > CT_MEMALIGN) {
      trsz = lj_ir_kint(J, sz);
    }
    trcd = emitir(IRTG(IR_CNEW, IRT_CDATA), trid, trsz);
    if (sz > 128 || (info & CTF_VLA)) {
      TRef dp;
      CTSize align;
    special:  /* Only handle bulk zero-fill for large/VLA/VLS types. */
      if (J->base[1])
	lj_trace_err(J, LJ_TRERR_NYICONV);  /* NYI: init large/VLA/VLS types. */
      dp = emitir(IRT(IR_ADD, IRT_PTR), trcd, lj_ir_kintp(J, sizeof(GCcdata)));
      if (trsz == TREF_NIL) trsz = lj_ir_kint(J, sz);
      align = ctype_align(info);
      if (align < CT_MEMALIGN) align = CT_MEMALIGN;
      crec_fill(J, dp, trsz, lj_ir_kint(J, 0), (1u << align));
    } else if (J->base[1] && !J->base[2] &&
	!lj_cconv_multi_init(cts, d, &rd->argv[1])) {
      goto single_init;
    } else if (ctype_isarray(d->info)) {
      CType *dc = ctype_rawchild(cts, d);  /* Array element type. */
      CTSize ofs, esize = dc->size;
      TRef sp = 0;
      TValue tv;
      TValue *sval = &tv;
      MSize i;
      tv.u64 = 0;
      if (!(ctype_isnum(dc->info) || ctype_isptr(dc->info)) ||
	  esize * CREC_FILL_MAXUNROLL < sz)
	goto special;
      for (i = 1, ofs = 0; ofs < sz; ofs += esize) {
	TRef dp = emitir(IRT(IR_ADD, IRT_PTR), trcd,
			 lj_ir_kintp(J, ofs + sizeof(GCcdata)));
	if (J->base[i]) {
	  sp = J->base[i];
	  sval = &rd->argv[i];
	  i++;
	} else if (i != 2) {
	  sp = ctype_isnum(dc->info) ? lj_ir_kint(J, 0) : TREF_NIL;
	}
	crec_ct_tv(J, dc, dp, sp, sval);
      }
    } else if (ctype_isstruct(d->info)) {
      CTypeID fid;
      MSize i = 1;
      if (!J->base[1]) {  /* Handle zero-fill of struct-of-NYI. */
	fid = d->sib;
	while (fid) {
	  CType *df = ctype_get(cts, fid);
	  fid = df->sib;
	  if (ctype_isfield(df->info)) {
	    CType *dc;
	    if (!gcref(df->name)) continue;  /* Ignore unnamed fields. */
	    dc = ctype_rawchild(cts, df);  /* Field type. */
	    if (!(ctype_isnum(dc->info) || ctype_isptr(dc->info) ||
		  ctype_isenum(dc->info)))
	      goto special;
	  } else if (!ctype_isconstval(df->info)) {
	    goto special;
	  }
	}
      }
      fid = d->sib;
      while (fid) {
	CType *df = ctype_get(cts, fid);
	fid = df->sib;
	if (ctype_isfield(df->info)) {
	  CType *dc;
	  TRef sp, dp;
	  TValue tv;
	  TValue *sval = &tv;
	  setintV(&tv, 0);
	  if (!gcref(df->name)) continue;  /* Ignore unnamed fields. */
	  dc = ctype_rawchild(cts, df);  /* Field type. */
	  if (!(ctype_isnum(dc->info) || ctype_isptr(dc->info) ||
		ctype_isenum(dc->info)))
	    lj_trace_err(J, LJ_TRERR_NYICONV);  /* NYI: init aggregates. */
	  if (J->base[i]) {
	    sp = J->base[i];
	    sval = &rd->argv[i];
	    i++;
	  } else {
	    sp = ctype_isptr(dc->info) ? TREF_NIL : lj_ir_kint(J, 0);
	  }
	  dp = emitir(IRT(IR_ADD, IRT_PTR), trcd,
		      lj_ir_kintp(J, df->size + sizeof(GCcdata)));
	  crec_ct_tv(J, dc, dp, sp, sval);
	  if ((d->info & CTF_UNION)) {
	    if (d->size != dc->size)  /* NYI: partial init of union. */
	      lj_trace_err(J, LJ_TRERR_NYICONV);
	    break;
	  }
	} else if (!ctype_isconstval(df->info)) {
	  /* NYI: init bitfields and sub-structures. */
	  lj_trace_err(J, LJ_TRERR_NYICONV);
	}
      }
    } else {
      TRef dp;
    single_init:
      dp = emitir(IRT(IR_ADD, IRT_PTR), trcd, lj_ir_kintp(J, sizeof(GCcdata)));
      if (J->base[1]) {
	crec_ct_tv(J, d, dp, J->base[1], &rd->argv[1]);
      } else {
	TValue tv;
	tv.u64 = 0;
	crec_ct_tv(J, d, dp, lj_ir_kint(J, 0), &tv);
      }
    }
  }
  J->base[0] = trcd;
  /* Handle __gc metamethod. */
  fin = lj_ctype_meta(cts, id, MM_gc);
  if (fin)
    crec_finalizer(J, trcd, 0, fin);
}

/* Record argument conversions.
** Note: may reallocate cts->tab and invalidate CType pointers.
*/
static TRef crec_call_args(jit_State *J, RecordFFData *rd,
			   CTState *cts, CType *ct)
{
  TRef args[CCI_NARGS_MAX];
  CTypeID fid;
  CTInfo info = ct->info;  /* lj_ccall_ctid_vararg may invalidate ct pointer. */
  MSize i, n;
  TRef tr, *base;
  cTValue *o;
#if LJ_TARGET_X86
#if LJ_ABI_WIN
  TRef *arg0 = NULL, *arg1 = NULL;
#endif
  int ngpr = 0;
  if (ctype_cconv(info) == CTCC_THISCALL)
    ngpr = 1;
  else if (ctype_cconv(info) == CTCC_FASTCALL)
    ngpr = 2;
#elif LJ_TARGET_ARM64 && LJ_TARGET_OSX
  int ngpr = CCALL_NARG_GPR;
#endif

  /* Skip initial attributes. */
  fid = ct->sib;
  while (fid) {
    CType *ctf = ctype_get(cts, fid);
    if (!ctype_isattrib(ctf->info)) break;
    fid = ctf->sib;
  }
  args[0] = TREF_NIL;
  for (n = 0, base = J->base+1, o = rd->argv+1; *base; n++, base++, o++) {
    CTypeID did;
    CType *d;

    if (n >= CCI_NARGS_MAX)
      lj_trace_err(J, LJ_TRERR_NYICALL);

    if (fid) {  /* Get argument type from field. */
      CType *ctf = ctype_get(cts, fid);
      fid = ctf->sib;
      lj_assertJ(ctype_isfield(ctf->info), "field expected");
      did = ctype_cid(ctf->info);
    } else {
      if (!(info & CTF_VARARG))
	lj_trace_err(J, LJ_TRERR_NYICALL);  /* Too many arguments. */
#if LJ_TARGET_ARM64 && LJ_TARGET_OSX
      if (ngpr >= 0) {
	ngpr = -1;
	args[n++] = TREF_NIL;  /* Marker for start of varargs. */
	if (n >= CCI_NARGS_MAX)
	  lj_trace_err(J, LJ_TRERR_NYICALL);
      }
#endif
      did = lj_ccall_ctid_vararg(cts, o);  /* Infer vararg type. */
    }
    d = ctype_raw(cts, did);
    if (!(ctype_isnum(d->info) || ctype_isptr(d->info) ||
	  ctype_isenum(d->info)))
      lj_trace_err(J, LJ_TRERR_NYICALL);
    tr = crec_ct_tv(J, d, 0, *base, o);
    if (ctype_isinteger_or_bool(d->info)) {
#if LJ_TARGET_ARM64 && LJ_TARGET_OSX
      if (!ngpr) {
	/* Fixed args passed on the stack use their unpromoted size. */
	if (d->size != lj_ir_type_size[tref_type(tr)]) {
	  lj_assertJ(d->size == 1 || d->size==2, "unexpected size %d", d->size);
	  tr = emitconv(tr, d->size==1 ? IRT_U8 : IRT_U16, tref_type(tr), 0);
	}
      } else
#endif
      if (d->size < 4) {
	if ((d->info & CTF_UNSIGNED))
	  tr = emitconv(tr, IRT_INT, d->size==1 ? IRT_U8 : IRT_U16, 0);
	else
	  tr = emitconv(tr, IRT_INT, d->size==1 ? IRT_I8 : IRT_I16,IRCONV_SEXT);
      }
    } else if (LJ_SOFTFP32 && ctype_isfp(d->info) && d->size > 4) {
      lj_needsplit(J);
    }
#if LJ_TARGET_X86
    /* 64 bit args must not end up in registers for fastcall/thiscall. */
#if LJ_ABI_WIN
    if (!ctype_isfp(d->info)) {
      /* Sigh, the Windows/x86 ABI allows reordering across 64 bit args. */
      if (tref_typerange(tr, IRT_I64, IRT_U64)) {
	if (ngpr) {
	  arg0 = &args[n]; args[n++] = TREF_NIL; ngpr--;
	  if (ngpr) {
	    arg1 = &args[n]; args[n++] = TREF_NIL; ngpr--;
	  }
	}
      } else {
	if (arg0) { *arg0 = tr; arg0 = NULL; n--; continue; }
	if (arg1) { *arg1 = tr; arg1 = NULL; n--; continue; }
	if (ngpr) ngpr--;
      }
    }
#else
    if (!ctype_isfp(d->info) && ngpr) {
      if (tref_typerange(tr, IRT_I64, IRT_U64)) {
	/* No reordering for other x86 ABIs. Simply add alignment args. */
	do { args[n++] = TREF_NIL; } while (--ngpr);
      } else {
	ngpr--;
      }
    }
#endif
#elif LJ_TARGET_ARM64 && LJ_TARGET_OSX
    if (!ctype_isfp(d->info) && ngpr) {
      ngpr--;
    }
#endif
    args[n] = tr;
  }
  tr = args[0];
  for (i = 1; i < n; i++)
    tr = emitir(IRT(IR_CARG, IRT_NIL), tr, args[i]);
  return tr;
}

/* Create a snapshot for the caller, simulating a 'false' return value. */
static void crec_snap_caller(jit_State *J)
{
  lua_State *L = J->L;
  TValue *base = L->base, *top = L->top;
  const BCIns *pc = J->pc;
  TRef ftr = J->base[-1-LJ_FR2];
  ptrdiff_t delta;
  if (!frame_islua(base-1) || J->framedepth <= 0)
    lj_trace_err(J, LJ_TRERR_NYICALL);
  J->pc = frame_pc(base-1); delta = 1+LJ_FR2+bc_a(J->pc[-1]);
  L->top = base; L->base = base - delta;
  J->base[-1-LJ_FR2] = TREF_FALSE;
  J->base -= delta; J->baseslot -= (BCReg)delta;
  J->maxslot = (BCReg)delta-LJ_FR2; J->framedepth--;
  lj_snap_add(J);
  L->base = base; L->top = top;
  J->framedepth++; J->maxslot = 1;
  J->base += delta; J->baseslot += (BCReg)delta;
  J->base[-1-LJ_FR2] = ftr; J->pc = pc;
}

/* Record function call. */
static int crec_call(jit_State *J, RecordFFData *rd, GCcdata *cd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CType *ct = ctype_raw(cts, cd->ctypeid);
  CTInfo info;
  IRType tp = IRT_PTR;
  if (ctype_isptr(ct->info)) {
    tp = (LJ_64 && ct->size == 8) ? IRT_P64 : IRT_P32;
    ct = ctype_rawchild(cts, ct);
  }
  info = ct->info;  /* crec_call_args may invalidate ct pointer. */
  if (ctype_isfunc(info)) {
    TRef func = emitir(IRT(IR_FLOAD, tp), J->base[0], IRFL_CDATA_PTR);
    CType *ctr = ctype_rawchild(cts, ct);
    CTInfo ctr_info = ctr->info;  /* crec_call_args may invalidate ctr. */
    IRType t = crec_ct2irt(cts, ctr);
    TRef tr;
    TValue tv;
    /* Check for blacklisted C functions that might call a callback. */
    tv.u64 = ((uintptr_t)cdata_getptr(cdataptr(cd), (LJ_64 && tp == IRT_P64) ? 8 : 4) >> 2) | U64x(800000000, 00000000);
    if (tvistrue(lj_tab_get(J->L, cts->miscmap, &tv)))
      lj_trace_err(J, LJ_TRERR_BLACKL);
    if (ctype_isvoid(ctr_info)) {
      t = IRT_NIL;
      rd->nres = 0;
    } else if (!(ctype_isnum(ctr_info) || ctype_isptr(ctr_info) ||
		 ctype_isenum(ctr_info)) || t == IRT_CDATA) {
      lj_trace_err(J, LJ_TRERR_NYICALL);
    }
    if ((info & CTF_VARARG)
#if LJ_TARGET_X86
	|| ctype_cconv(info) != CTCC_CDECL
#endif
	)
      func = emitir(IRT(IR_CARG, IRT_NIL), func,
		    lj_ir_kint(J, ctype_typeid(cts, ct)));
    tr = emitir(IRT(IR_CALLXS, t), crec_call_args(J, rd, cts, ct), func);
    if (ctype_isbool(ctr_info)) {
      if (frame_islua(J->L->base-1) && bc_b(frame_pc(J->L->base-1)[-1]) == 1) {
	/* Don't check result if ignored. */
	tr = TREF_NIL;
      } else {
	crec_snap_caller(J);
#if LJ_TARGET_X86ORX64
	/* Note: only the x86/x64 backend supports U8 and only for EQ(tr, 0). */
	lj_ir_set(J, IRTG(IR_NE, IRT_U8), tr, lj_ir_kint(J, 0));
#else
	lj_ir_set(J, IRTGI(IR_NE), tr, lj_ir_kint(J, 0));
#endif
	J->postproc = LJ_POST_FIXGUARDSNAP;
	tr = TREF_TRUE;
      }
    } else if (t == IRT_PTR || (LJ_64 && t == IRT_P32) ||
	       t == IRT_I64 || t == IRT_U64 || ctype_isenum(ctr_info)) {
      TRef trid = lj_ir_kint(J, ctype_cid(info));
      tr = emitir(IRTG(IR_CNEWI, IRT_CDATA), trid, tr);
      if (t == IRT_I64 || t == IRT_U64) lj_needsplit(J);
    } else if (t == IRT_FLOAT || t == IRT_U32) {
      tr = emitconv(tr, IRT_NUM, t, 0);
    } else if (t == IRT_I8 || t == IRT_I16) {
      tr = emitconv(tr, IRT_INT, t, IRCONV_SEXT);
    } else if (t == IRT_U8 || t == IRT_U16) {
      tr = emitconv(tr, IRT_INT, t, 0);
    }
    J->base[0] = tr;
    J->needsnap = 1;
    return 1;
  }
  return 0;
}

void LJ_FASTCALL recff_cdata_call(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  GCcdata *cd = argv2cdata(J, J->base[0], &rd->argv[0]);
  CTypeID id = cd->ctypeid;
  CType *ct;
  cTValue *tv;
  MMS mm = MM_call;
  if (id == CTID_CTYPEID) {
    id = crec_constructor(J, cd, J->base[0]);
    mm = MM_new;
  } else if (crec_call(J, rd, cd)) {
    return;
  }
  /* Record ctype __call/__new metamethod. */
  ct = ctype_raw(cts, id);
  tv = lj_ctype_meta(cts, ctype_isptr(ct->info) ? ctype_cid(ct->info) : id, mm);
  if (tv) {
    if (tvisfunc(tv)) {
      crec_tailcall(J, rd, tv);
      return;
    }
  } else if (mm == MM_new) {
    crec_alloc(J, rd, id);
    return;
  }
  /* No metamethod or NYI: non-function metamethods. */
  lj_trace_err(J, LJ_TRERR_BADTYPE);
}

/* -- Vector arithmetic --------------------------------------------------- */

/* Box a raw vector value into a fresh cdata object. */
static TRef crec_vec_box(jit_State *J, TRef val, IRType vt, CTypeID id)
{
  TRef dp = emitir(IRTG(IR_CNEW, IRT_CDATA), lj_ir_kint(J, (int32_t)id),
		   TREF_NIL);
  TRef ptr = emitir(IRT(IR_ADD, IRT_PTR), dp, lj_ir_kintp(J, sizeof(GCcdata)));
  emitir(IRT(IR_XSTORE, vt), ptr, val);
  return dp;
}

/*
** Turn a scalar operand into a vector by splatting it across all lanes.
** A constant scalar is folded into a vector constant, which is both exact
** (the interpreter does the conversion) and the best possible code.
*/
static TRef crec_vec_splat(jit_State *J, CTState *cts, IRType vt,
			   const CTVecInfo *vi, TRef sp, CType *sct,
			   cTValue *sval)
{
  CType *ect = ctype_get(cts, vi->eid);
  if (tref_isk(sp)) {
    /* Constant: convert and splat with the very same code the VM uses. */
    TValue tv;
    cTValue *kv = NULL;
    IRIns *irk = IR(tref_ref(sp));
    if (sval && !tviscdata(sval)) {
      kv = sval;
    } else if (irk->o == IR_KINT) {
      setintV(&tv, irk->i); kv = &tv;
    } else if (irk->o == IR_KNUM) {
      setnumV(&tv, ir_knum(irk)->n); kv = &tv;
    }
    if (kv) {
      uint8_t ebuf[8], vbuf[LJ_VEC_MAXSIZE];
      lj_cconv_ct_tv(cts, ect, ebuf, (TValue *)kv, 0);
      lj_simd_splat(vbuf, ebuf, vi);
      return lj_ir_kvec(J, vt, vbuf);
    }
  }
  /* Otherwise convert with the standard FFI rules and broadcast at runtime. */
  sp = crec_ct_ct(J, ect, sct, 0, sp, NULL);
  return emitir(IRT(IR_VSPLAT, vt), sp, 0);
}

static TRef crec_simd_k16(jit_State *J, IRType vt, uint16_t v);

/* Packed byte multiplication through the two word products x86 provides. */
static TRef crec_simd_mul_i8(jit_State *J, IRType vt, TRef a, TRef b)
{
  IRType wvt = (IRType)(IRT_V8I16 | (vt & IRT_VEC256));
  TRef even, odd;
  even = emitir(IRT(IR_VMUL, wvt), a, b);
  even = emitir(IRT(IR_VAND, wvt), even,
		crec_simd_k16(J, wvt, 0x00ff));
  odd = emitir(IRT(IR_VMUL, wvt),
	       emitir(IRT(IR_VSHR, wvt), a, lj_ir_kint(J, 8)),
	       emitir(IRT(IR_VSHR, wvt), b, lj_ir_kint(J, 8)));
  return emitir(IRT(IR_VOR, vt), even,
		emitir(IRT(IR_VSHL, wvt), odd, lj_ir_kint(J, 8)));
}

/* Recover the two original operands from crec_simd_mul_i8()'s packed IR.
** This lets a following byte hsum bypass the materialised byte products while
** keeping the expansion visible for ordinary multiply CSE.
*/
static int crec_simd_match_mul_i8(jit_State *J, TRef tr,
				  IRRef *pa, IRRef *pb)
{
  IRIns *top = IR(tref_ref(tr)), *band, *shl, *even, *odd, *sa, *sb, *mask;
  IRRef er, kr;
  uint32_t i, size;
  const uint8_t *kp;
  if (top->o != IR_VOR || irt_type(top->t) != IRT_V16I8) return 0;
  band = IR(top->op1);
  shl = IR(top->op2);
  if (band->o != IR_VAND || shl->o != IR_VSHL) {
    band = IR(top->op2);
    shl = IR(top->op1);
    if (band->o != IR_VAND || shl->o != IR_VSHL) return 0;
  }
  if (irt_type(band->t) != IRT_V8I16 ||
      irt_type(shl->t) != IRT_V8I16 ||
      irt_isvec256(band->t) != irt_isvec256(top->t) ||
      irt_isvec256(shl->t) != irt_isvec256(top->t))
    return 0;
  if (IR(shl->op2)->o != IR_KINT || IR(shl->op2)->i != 8) return 0;
  if (IR(band->op1)->o == IR_VMUL && IR(band->op2)->o == IR_KVEC) {
    er = band->op1; kr = band->op2;
  } else if (IR(band->op2)->o == IR_VMUL &&
	     IR(band->op1)->o == IR_KVEC) {
    er = band->op2; kr = band->op1;
  } else {
    return 0;
  }
  even = IR(er);
  mask = IR(kr);
  if (irt_type(even->t) != IRT_V8I16 ||
      irt_type(mask->t) != IRT_V8I16 ||
      irt_isvec256(even->t) != irt_isvec256(top->t) ||
      irt_isvec256(mask->t) != irt_isvec256(top->t))
    return 0;
  size = irt_isvec256(top->t) ? 32 : 16;
  kp = ir_kvec(mask);
  for (i = 0; i < size; i += 2)
    if (kp[i] != 0xff || kp[i+1] != 0) return 0;
  odd = IR(shl->op1);
  if (odd->o != IR_VMUL || irt_type(odd->t) != IRT_V8I16 ||
      irt_isvec256(odd->t) != irt_isvec256(top->t))
    return 0;
  sa = IR(odd->op1);
  sb = IR(odd->op2);
  if (sa->o != IR_VSHR || sb->o != IR_VSHR ||
      irt_type(sa->t) != IRT_V8I16 ||
      irt_type(sb->t) != IRT_V8I16 ||
      irt_isvec256(sa->t) != irt_isvec256(top->t) ||
      irt_isvec256(sb->t) != irt_isvec256(top->t) ||
      IR(sa->op2)->o != IR_KINT || IR(sa->op2)->i != 8 ||
      IR(sb->op2)->o != IR_KINT || IR(sb->op2)->i != 8)
    return 0;
  if (!((even->op1 == sa->op1 && even->op2 == sb->op1) ||
	(even->op1 == sb->op1 && even->op2 == sa->op1)))
    return 0;
  *pa = even->op1;
  *pb = even->op2;
  return 1;
}

/* Record arithmetic on vector cdata. Returns 0 if this is not vector math. */
static TRef crec_arith_vec(jit_State *J, TRef *sp, CType **s, MMS mm,
			   RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTVecInfo vi;
  CType *vct;
  IRType vt;
  IROp op;
  TRef tra, trb;
  CTypeID id;
  int isv0 = s[0] && ctype_isvector(s[0]->info);
  int isv1 = s[1] && ctype_isvector(s[1]->info);
  if (!(isv0 || isv1)) return 0;
  if (isv0 && isv1) {
    if (s[0] != s[1]) return 0;  /* Interned, so identical types are equal. */
    vct = s[0];
    if (!lj_ctype_vecinfo(cts, vct, &vi)) return 0;
    vt = crec_vec2irt(cts, vct);
    if (vt == IRT_NIL) lj_trace_err(J, LJ_TRERR_NYIVEC);
    tra = sp[0]; trb = sp[1];
  } else {
    int vn = isv0 ? 0 : 1;
    CType *sct = s[1-vn];
    TRef trs;
    vct = s[vn];
    if (!sct || !ctype_isnum(sct->info) || (sct->info & CTF_BOOL)) return 0;
    if (!lj_ctype_vecinfo(cts, vct, &vi)) return 0;
    vt = crec_vec2irt(cts, vct);
    if (vt == IRT_NIL) lj_trace_err(J, LJ_TRERR_NYIVEC);
    trs = crec_vec_splat(J, cts, vt, &vi, sp[1-vn], sct, &rd->argv[1-vn]);
    tra = isv0 ? sp[0] : trs;
    trb = isv0 ? trs : sp[1];
  }
  switch (mm) {
  case MM_add: op = IR_VADD; break;
  case MM_sub: op = IR_VSUB; break;
  case MM_mul: op = IR_VMUL; break;
  case MM_div:
    if (!veck_isfp(vi.kind)) return 0;
    op = IR_VDIV;
    break;
  case MM_eq: {
    /* Whole-vector equality: compare, gather the lane sign bits, then guard.
    ** Integer lanes compare byte-wise, which is the same answer and works on
    ** every lane width; FP lanes need the real per-lane comparison so that a
    ** NaN lane is never equal.
    */
    TRef mask, mm2;
    uint32_t all;
    if (veck_isfp(vi.kind)) {
      mask = emitir(IRT(IR_VCMPEQ, vt), tra, trb);
      all = (1u << vi.lanes) - 1;
    } else {
      IRType bt = (IRType)(IRT_V16I8 | (vt & IRT_VEC256));
      mask = emitir(IRT(IR_VCMPEQ, bt), tra, trb);
      all = (vt & IRT_VEC256) ? ~(uint32_t)0 : 0xffffu;
    }
    mm2 = emitir(IRTI(IR_VMOVMSK), mask,
		 IRVSRC(veck_isfp(vi.kind) ? vt :
			 (IRT_V16I8 | (vt & IRT_VEC256)), 0));
    /* Assume true comparison. Fixup and emit the pending guard later. */
    lj_ir_set(J, IRTGI(IR_EQ), mm2, lj_ir_kint(J, (int32_t)all));
    J->postproc = LJ_POST_FIXGUARD;
    return TREF_TRUE;
    }
  case MM_unm:
    /* Negation: 0 - v for integers, flip the sign bit for floats. */
    if (veck_isfp(vi.kind)) {
      uint8_t vbuf[LJ_VEC_MAXSIZE];
      uint8_t ebuf[8];
      memset(ebuf, 0, sizeof(ebuf));
      ebuf[vi.esize-1] = 0x80;  /* Little-endian sign bit of one lane. */
      lj_simd_splat(vbuf, ebuf, &vi);
      tra = emitir(IRT(IR_VXOR, vt), sp[0], lj_ir_kvec(J, vt, vbuf));
    } else {
      uint8_t vbuf[LJ_VEC_MAXSIZE];
      memset(vbuf, 0, sizeof(vbuf));
      tra = emitir(IRT(IR_VSUB, vt), lj_ir_kvec(J, vt, vbuf), sp[0]);
    }
    goto box;
  default:
    return 0;
  }
  if (op == IR_VMUL && !veck_isfp(vi.kind) && vi.esize == 1) {
    tra = crec_simd_mul_i8(J, vt, tra, trb);
    goto box;
  }
  tra = emitir(IRT(op, vt), tra, trb);
box:
  id = ctype_typeid(cts, vct);
  return crec_vec_box(J, tra, vt, id);
}

/* -- ffi.simd recording -------------------------------------------------- */

/* Splat one byte pattern across all lanes and intern it as a constant. */
static TRef crec_vec_kmask(jit_State *J, IRType vt, const CTVecInfo *vi,
			   const uint8_t *elem)
{
  uint8_t vbuf[LJ_VEC_MAXSIZE];
  lj_simd_splat(vbuf, elem, vi);
  return lj_ir_kvec(J, vt, vbuf);
}

/* All-ones constant, used to invert a mask. */
static TRef crec_vec_kones(jit_State *J, IRType vt)
{
  uint8_t vbuf[LJ_VEC_MAXSIZE];
  memset(vbuf, 0xff, sizeof(vbuf));
  return lj_ir_kvec(J, vt, vbuf);
}

/* Load the vector value of argument n, with a guard on its ctype. */
static TRef crec_simd_arg(jit_State *J, RecordFFData *rd, int n,
			  CTVecInfo *vi, CTypeID *pid, IRType *pvt)
{
  CTState *cts = ctype_ctsG(J2G(J));
  TRef tr = J->base[n];
  CType *ct;
  CTypeID id;
  IRType vt;
  if (!tr || !tref_iscdata(tr)) lj_trace_err(J, LJ_TRERR_BADTYPE);
  id = argv2cdata(J, tr, &rd->argv[n])->ctypeid;
  ct = ctype_raw(cts, id);
  if (!lj_ctype_vecinfo(cts, ct, vi)) lj_trace_err(J, LJ_TRERR_BADTYPE);
  vt = crec_vec2irt(cts, ct);
  if (vt == IRT_NIL) lj_trace_err(J, LJ_TRERR_NYIVEC);
  if (pid) *pid = id;
  if (pvt) *pvt = vt;
  tr = emitir(IRT(IR_ADD, IRT_PTR), tr, lj_ir_kintp(J, sizeof(GCcdata)));
  return emitir(IRT(IR_XLOAD, vt), tr, 0);
}

/*
** Second operand of a binary ffi.simd call: a matching vector, or a Lua
** number that is converted and splatted. A scalar cdata operand is left to
** the interpreter.
*/
static TRef crec_simd_arg2(jit_State *J, RecordFFData *rd, int n,
			   const CTVecInfo *vi, CTypeID id, IRType vt)
{
  CTState *cts = ctype_ctsG(J2G(J));
  TRef tr = J->base[n];
  if (!tr) lj_trace_err(J, LJ_TRERR_BADTYPE);
  if (tref_iscdata(tr)) {
    CTypeID id2 = argv2cdata(J, tr, &rd->argv[n])->ctypeid;
    CType *ct = ctype_raw(cts, id2);
    CTVecInfo vi2;
    if (lj_ctype_vecinfo(cts, ct, &vi2)) {
      if (ctype_raw(cts, id) != ct) lj_trace_err(J, LJ_TRERR_NYIVEC);
      tr = emitir(IRT(IR_ADD, IRT_PTR), tr, lj_ir_kintp(J, sizeof(GCcdata)));
      return emitir(IRT(IR_XLOAD, vt), tr, 0);
    }
    /* A scalar cdata is unboxed, converted and splatted like a Lua number. */
    if (!ctype_isnum(ct->info) || (ct->info & CTF_BOOL))
      lj_trace_err(J, LJ_TRERR_NYIVEC);
    tr = crec_ct_tv(J, ctype_get(cts, vi->eid), 0, tr, &rd->argv[n]);
    return emitir(IRT(IR_VSPLAT, vt), tr, 0);
  } else if (tref_isnumber(tr)) {
    CType *sct = ctype_get(cts, tref_isinteger(tr) ? CTID_INT32 : CTID_DOUBLE);
    return crec_vec_splat(J, cts, vt, vi, tr, sct, &rd->argv[n]);
  }
  lj_trace_err(J, LJ_TRERR_BADTYPE);
  return 0;
}

/* Abort unless the backend has a packed lowering for this combination. */
static void crec_simd_need(jit_State *J, int ok)
{
  if (!ok) lj_trace_err(J, LJ_TRERR_NYIVEC);
}

/* A vector constant built from a repeating 16 bit pattern. */
static TRef crec_simd_k16(jit_State *J, IRType vt, uint16_t v)
{
  uint8_t vbuf[LJ_VEC_MAXSIZE];
  uint32_t i;
  for (i = 0; i < sizeof(vbuf); i += 2) memcpy(vbuf+i, &v, 2);
  return lj_ir_kvec(J, vt, vbuf);
}

/* A vector constant built from a repeating 32 bit pattern. */
static TRef crec_simd_k32(jit_State *J, IRType vt, uint32_t v)
{
  uint8_t vbuf[LJ_VEC_MAXSIZE];
  uint32_t i;
  for (i = 0; i < sizeof(vbuf); i += 4) memcpy(vbuf+i, &v, 4);
  return lj_ir_kvec(J, vt, vbuf);
}

/* A vector constant built from a repeating 64 bit pattern. */
static TRef crec_simd_k64(jit_State *J, IRType vt, uint64_t v)
{
  uint8_t vbuf[LJ_VEC_MAXSIZE];
  uint32_t i;
  for (i = 0; i < sizeof(vbuf); i += 8) memcpy(vbuf+i, &v, 8);
  return lj_ir_kvec(J, vt, vbuf);
}

/*
** Broadcast the low byte of every 16 bit lane into both halves of that lane.
** Multiplying by 0x0101 is v | (v << 8) for v <= 0xff, cannot carry, and
** needs only SSE2. Used to turn a 16 bit lane mask into a byte lane mask.
*/
static TRef crec_simd_bcastbyte(jit_State *J, TRef m)
{
  IRType vt = tref_vtype(m);
  return emitir(IRT(IR_VMUL, vt), m, crec_simd_k16(J, vt, 0x0101));
}

/*
** Clamp a shift count to [0, lim] with *unsigned* saturation, branchlessly,
** so no guard is needed and the trace stays valid for every count. lim+1
** must be a power of two. d = n & ~lim is non-zero exactly when n is
** negative or greater than lim, and (d | -d) >>a 31 turns that into an
** all-ones mask. This matches the interpreter, which reads the count as
** uint32_t and treats anything from lim+1 upwards as a full shift.
*/
static TRef crec_simd_clampcnt(jit_State *J, TRef n, int32_t lim)
{
  TRef d = emitir(IRTI(IR_BAND), n, lj_ir_kint(J, (int32_t)~(uint32_t)lim));
  TRef m = emitir(IRTI(IR_BOR), d, emitir(IRTI(IR_SUB), lj_ir_kint(J, 0), d));
  m = emitir(IRTI(IR_BSAR), m, lj_ir_kint(J, 31));
  return emitir(IRTI(IR_BAND), emitir(IRTI(IR_BOR), n, m),
		lj_ir_kint(J, lim));
}

static TRef crec_simd_ubias(jit_State *J, IRType vt, const CTVecInfo *vi,
			    TRef tr);

/*
** AVX2 has variable shifts only for 32 and 64 bit lanes. Split every dword
** into two words or four bytes, shift those independently with VPSLLVD,
** VPSRLVD or VPSRAVD, then put the pieces back in place. Masking after a
** logical shift makes counts from the narrow lane width through 31 flush to
** zero; the hardware already flushes at 32. Arithmetic input pieces are
** sign-extended first, so every count at or above the narrow width produces
** the required full sign fill.
*/
static TRef crec_simd_shiftv_narrow(jit_State *J, IRType vt, uint32_t esize,
				    TRef a, TRef nv, IROp op)
{
  IRType wvt = (IRType)(IRT_V4I32 | (vt & IRT_VEC256));
  uint32_t bits = esize * 8, parts = 4 / esize, i;
  TRef mask = crec_simd_k32(J, wvt, bits == 8 ? 0xffu : 0xffffu);
  TRef r = 0;
  for (i = 0; i < parts; i++) {
    uint32_t ofs = i * bits;
    TRef cnt = nv, v, p;
    if (ofs)
      cnt = emitir(IRT(IR_VSHR, wvt), cnt, lj_ir_kint(J, (int32_t)ofs));
    cnt = emitir(IRT(IR_VAND, wvt), cnt, mask);
    if (op == IR_VSARV) {
      uint32_t lsh = 32 - bits - ofs;
      v = lsh ? emitir(IRT(IR_VSHL, wvt), a,
			lj_ir_kint(J, (int32_t)lsh)) : a;
      v = emitir(IRT(IR_VSAR, wvt), v,
		 lj_ir_kint(J, (int32_t)(32-bits)));
    } else {
      v = ofs ? emitir(IRT(IR_VSHR, wvt), a,
			lj_ir_kint(J, (int32_t)ofs)) : a;
      v = emitir(IRT(IR_VAND, wvt), v, mask);
    }
    p = emitir(IRT(op, wvt), v, cnt);
    p = emitir(IRT(IR_VAND, wvt), p, mask);
    if (ofs)
      p = emitir(IRT(IR_VSHL, wvt), p, lj_ir_kint(J, (int32_t)ofs));
    if (!r) {
      r = p;
    } else {
      r = emitir(IRT(IR_VOR, i == parts-1 ? vt : wvt), r, p);
    }
  }
  return r;
}

/*
** A variable word left shift is multiplication modulo 2^16. Build both word
** factors with the available dword variable shift, pack them, then multiply
** both data words at once. Counts at or above 16 lose every low-word bit.
*/
static TRef crec_simd_shiftv_i16_left(jit_State *J, IRType vt, TRef a, TRef nv)
{
  IRType dvt = (IRType)(IRT_V4I32 | (vt & IRT_VEC256));
  TRef one = crec_simd_k32(J, dvt, 1);
  TRef mask = crec_simd_k32(J, dvt, 0xffff);
  TRef lo, hi;
  lo = emitir(IRT(IR_VSHLV, dvt), one,
	      emitir(IRT(IR_VAND, dvt), nv, mask));
  lo = emitir(IRT(IR_VAND, dvt), lo, mask);
  hi = emitir(IRT(IR_VSHLV, dvt), one,
	      emitir(IRT(IR_VSHR, dvt), nv, lj_ir_kint(J, 16)));
  hi = emitir(IRT(IR_VSHL, dvt), hi, lj_ir_kint(J, 16));
  return emitir(IRT(IR_VMUL, vt), a, emitir(IRT(IR_VOR, vt), lo, hi));
}

/*
** For counts from one through 16, multiplying by 2^(16-count) and taking the
** high word is a right shift. A zero count needs the original word because
** 2^16 does not fit in the factor lane. For arithmetic shifts, clamp to 16
** and use signed MULHI. Its factor for count one is the signed value -32768:
** adding the input corrects floor(-x/2) to floor(x/2). The same addition
** handles count zero, where MULHI receives a zero factor.
*/
static TRef crec_simd_shiftv_i16_right(jit_State *J, IRType vt, TRef a,
				       TRef nv, int sar)
{
  IRType dvt = (IRType)(IRT_V4I32 | (vt & IRT_VEC256));
  TRef base = crec_simd_k32(J, dvt, 0x10000);
  TRef mask = crec_simd_k32(J, dvt, 0xffff);
  TRef cnt = nv, lo, hi, factor, correction, r;
  if (sar)
    cnt = emitir(IRT(IR_VMINU, vt), nv, crec_simd_k16(J, vt, 16));
  lo = emitir(IRT(IR_VSHRV, dvt), base,
	      emitir(IRT(IR_VAND, dvt), cnt, mask));
  lo = emitir(IRT(IR_VAND, dvt), lo, mask);
  hi = emitir(IRT(IR_VSHRV, dvt), base,
	      emitir(IRT(IR_VSHR, dvt), cnt, lj_ir_kint(J, 16)));
  hi = emitir(IRT(IR_VSHL, dvt), hi, lj_ir_kint(J, 16));
  factor = emitir(IRT(IR_VOR, vt), lo, hi);
  correction = sar ?
    emitir(IRT(IR_VCMPGT, vt), crec_simd_k16(J, vt, 2), cnt) :
    emitir(IRT(IR_VCMPEQ, vt), cnt, crec_simd_k16(J, vt, 0));
  r = emitir(IRT(sar ? IR_VMULHI : IR_VMULHIU, vt), a, factor);
  return emitir(IRT(sar ? IR_VADD : IR_VOR, vt), r,
		emitir(IRT(IR_VAND, vt), a, correction));
}

/*
** A variable byte left shift is multiplication modulo 256 by 2^count.
** PSHUFB obtains that multiplier from an eight-entry table. Saturating 0x78+n
** maps valid counts 0..7 to table slots 8..15 and sets the control byte's high
** bit for every larger unsigned count, which makes PSHUFB return zero.
*/
static TRef crec_simd_shiftv_i8_left(jit_State *J, IRType vt, TRef a, TRef nv)
{
  uint8_t table[LJ_VEC_MAXSIZE];
  uint32_t i;
  TRef ctrl, factor;
  for (i = 0; i < sizeof(table); i++) {
    uint32_t n = i & 15;
    table[i] = (uint8_t)(n >= 8 ? 1u << (n-8) : 0);
  }
  ctrl = emitir(IRT(IR_VADDSU, vt), nv,
		crec_simd_k16(J, vt, 0x7878));
  factor = emitir(IRT(IR_VSHUFB, vt), lj_ir_kvec(J, vt, table), ctrl);
  return crec_simd_mul_i8(J, vt, a, factor);
}

/*
** Logical byte right shift through (x * 2^(7-count)) >> 7. The factor lookup
** zeroes out-of-range counts just like the left-shift lookup above. Isolate
** even and odd bytes before multiplying so adjacent byte products cannot mix.
*/
static TRef crec_simd_shiftv_i8_right(jit_State *J, IRType vt, TRef a, TRef nv)
{
  IRType wvt = (IRType)(IRT_V8I16 | (vt & IRT_VEC256));
  uint8_t table[LJ_VEC_MAXSIZE];
  uint32_t i;
  TRef ctrl, factor, mask, even, odd;
  for (i = 0; i < sizeof(table); i++) {
    uint32_t n = i & 15;
    table[i] = (uint8_t)(n >= 8 ? 0x80u >> (n-8) : 0);
  }
  ctrl = emitir(IRT(IR_VADDSU, vt), nv,
		crec_simd_k16(J, vt, 0x7878));
  factor = emitir(IRT(IR_VSHUFB, vt), lj_ir_kvec(J, vt, table), ctrl);
  mask = crec_simd_k16(J, wvt, 0x00ff);
  even = emitir(IRT(IR_VMUL, wvt),
		emitir(IRT(IR_VAND, wvt), a, mask),
		emitir(IRT(IR_VAND, wvt), factor, mask));
  even = emitir(IRT(IR_VSHR, wvt), even, lj_ir_kint(J, 7));
  odd = emitir(IRT(IR_VMUL, wvt),
	       emitir(IRT(IR_VSHR, wvt), a, lj_ir_kint(J, 8)),
	       emitir(IRT(IR_VSHR, wvt), factor, lj_ir_kint(J, 8)));
  odd = emitir(IRT(IR_VSHR, wvt), odd, lj_ir_kint(J, 7));
  return emitir(IRT(IR_VOR, vt), even,
		emitir(IRT(IR_VSHL, wvt), odd, lj_ir_kint(J, 8)));
}

/*
** Arithmetic byte right shift uses the same factors after clamping the count
** to seven. Sign-extend each byte to a word before multiplication; shifting
** the signed product by seven then gives floor(x / 2^count). A factor of one
** at the clamped limit naturally produces the required full sign fill.
*/
static TRef crec_simd_shiftv_i8_sar(jit_State *J, IRType vt, TRef a, TRef nv)
{
  IRType wvt = (IRType)(IRT_V8I16 | (vt & IRT_VEC256));
  uint8_t table[LJ_VEC_MAXSIZE];
  uint32_t i;
  TRef ctrl, factor, mask, even, odd;
  for (i = 0; i < sizeof(table); i++) {
    uint32_t n = i & 15;
    table[i] = (uint8_t)(n < 8 ? 0x80u >> n : 0);
  }
  ctrl = emitir(IRT(IR_VMINU, vt), nv, crec_simd_k16(J, vt, 0x0707));
  factor = emitir(IRT(IR_VSHUFB, vt), lj_ir_kvec(J, vt, table), ctrl);
  mask = crec_simd_k16(J, wvt, 0x00ff);
  even = emitir(IRT(IR_VSAR, wvt),
		emitir(IRT(IR_VSHL, wvt), a, lj_ir_kint(J, 8)),
		lj_ir_kint(J, 8));
  even = emitir(IRT(IR_VMUL, wvt), even,
		emitir(IRT(IR_VAND, wvt), factor, mask));
  even = emitir(IRT(IR_VSAR, wvt), even, lj_ir_kint(J, 7));
  even = emitir(IRT(IR_VAND, wvt), even, mask);
  odd = emitir(IRT(IR_VMUL, wvt),
	       emitir(IRT(IR_VSAR, wvt), a, lj_ir_kint(J, 8)),
	       emitir(IRT(IR_VSHR, wvt), factor, lj_ir_kint(J, 8)));
  odd = emitir(IRT(IR_VSAR, wvt), odd, lj_ir_kint(J, 7));
  return emitir(IRT(IR_VOR, vt), even,
		emitir(IRT(IR_VSHL, wvt), odd, lj_ir_kint(J, 8)));
}

/*
** 64 bit lane min/max. There is no instruction for it before AVX-512, so
** compare and blend instead. That is still fully packed, three instructions
** plus the compare, and it needs PCMPGTQ from SSE4.2.
*/
static TRef crec_simd_minmax64(jit_State *J, IRType vt, const CTVecInfo *vi,
			       TRef a, TRef b, int ismax)
{
  TRef ca = a, cb = b, m;
  crec_simd_need(J, (J->flags & JIT_F_SSE4_2) != 0);
  if (veck_isunsigned(vi->kind)) {  /* Bias, so the signed compare answers. */
    ca = crec_simd_ubias(J, vt, vi, a);
    cb = crec_simd_ubias(J, vt, vi, b);
  }
  m = emitir(IRT(IR_VCMPGT, vt), ca, cb);
  /* max picks a where a > b, min picks b. Equal lanes are bit-identical, so
  ** it does not matter which side an a == b lane takes.
  */
  return emitir(IRT(IR_VOR, vt),
		emitir(IRT(IR_VAND, vt), m, ismax ? a : b),
		emitir(IRT(IR_VANDN, vt), m, ismax ? b : a));
}

/*
** Fold a byte-aligned rotate idiom into one PSHUFB:
**
**   (x << n) | (x >> (bits-n))
**
** The shifts have already been recorded by the time bor() sees them. Leaving
** them behind is harmless: DCE removes both once this packed shuffle becomes
** their only consumer. The control bytes are lane-local, so the same mask
** works for XMM and for both 128 bit halves of YMM.
*/
static TRef crec_simd_rotbytes(jit_State *J, IRType vt,
			       const CTVecInfo *vi, TRef a, TRef b)
{
  IRIns *ls = IR(tref_ref(a)), *rs = IR(tref_ref(b));
  uint32_t bits = (uint32_t)vi->esize * 8;
  uint32_t lsh, rsh, bytes, i;
  uint8_t mask[LJ_VEC_MAXSIZE];
  IRRef src;

  if (veck_isfp(vi->kind) || vi->esize < 2 ||
      !(J->flags & JIT_F_SSSE3))
    return 0;
  if (ls->o == IR_VSHR && rs->o == IR_VSHL) {
    IRIns *tmp = ls; ls = rs; rs = tmp;
  }
  if (ls->o != IR_VSHL || rs->o != IR_VSHR || ls->op1 != rs->op1 ||
      IR(ls->op2)->o != IR_KINT || IR(rs->op2)->o != IR_KINT)
    return 0;
  lsh = (uint32_t)IR(ls->op2)->i;
  rsh = (uint32_t)IR(rs->op2)->i;
  if (lsh == 0 || lsh >= bits || rsh >= bits ||
      lsh + rsh != bits || (lsh & 7))
    return 0;

  bytes = (uint32_t)vi->esize * vi->lanes;
  for (i = 0; i < bytes; i++) {
    uint32_t lane = i & ~((uint32_t)vi->esize - 1);
    uint32_t pos = i & ((uint32_t)vi->esize - 1);
    mask[i] = (uint8_t)(lane +
      ((pos + (uint32_t)vi->esize - (lsh >> 3)) &
       ((uint32_t)vi->esize - 1)));
  }
  src = ls->op1;
  return emitir(IRT(IR_VSHUFB, vt),
		TREF(src, irt_t(IR(src)->t)), lj_ir_kvec(J, vt, mask));
}

void LJ_FASTCALL recff_simd_binop(jit_State *J, RecordFFData *rd)
{
  CTVecInfo vi; CTypeID id; IRType vt;
  TRef a = crec_simd_arg(J, rd, 0, &vi, &id, &vt);
  TRef b = crec_simd_arg2(J, rd, 1, &vi, id, vt);
  int uns = veck_isunsigned(vi.kind);
  int sse41 = (J->flags & JIT_F_SSE4_1) != 0;
  IROp op;
  switch (rd->data) {
  case VOP_AND: op = IR_VAND; break;
  case VOP_OR: case VOP_XOR: {
    TRef rot = crec_simd_rotbytes(J, vt, &vi, a, b);
    if (rot) {
      J->base[0] = crec_vec_box(J, rot, vt, id);
      return;
    }
    op = rd->data == VOP_OR ? IR_VOR : IR_VXOR;
    break;
    }
  case VOP_ANDN: op = IR_VANDN; break;
  case VOP_MIN: case VOP_MAX:
    if (veck_isfp(vi.kind)) {
      op = rd->data == VOP_MIN ? IR_VMIN : IR_VMAX;
    } else if (vi.esize == 8) {
      J->base[0] = crec_vec_box(J,
	crec_simd_minmax64(J, vt, &vi, a, b, rd->data == VOP_MAX), vt, id);
      return;
    } else {
      crec_simd_need(J, vi.esize == 2 ? (uns ? sse41 : 1) :
			vi.esize == 1 ? (uns ? 1 : sse41) : sse41);
      op = rd->data == VOP_MIN ? (uns ? IR_VMINU : IR_VMIN)
			       : (uns ? IR_VMAXU : IR_VMAX);
    }
    break;
  case VOP_ADDS: case VOP_SUBS:
    crec_simd_need(J, vi.esize <= 2 && !veck_isfp(vi.kind));
    op = rd->data == VOP_ADDS ? (uns ? IR_VADDSU : IR_VADDS)
			      : (uns ? IR_VSUBSU : IR_VSUBS);
    break;
  default:
    lj_trace_err(J, LJ_TRERR_NYIVEC);
    return;
  }
  J->base[0] = crec_vec_box(J, emitir(IRT(op, vt), a, b), vt, id);
}

/* Packed absolute value for a signed integer vector. The 64 bit fallback
** uses SSE2-class sign/shuffle operations at XMM width (their AVX2 forms at
** YMM width); narrower lanes use the SSSE3 PABS family.
*/
static TRef crec_simd_iabs(jit_State *J, IRType vt, const CTVecInfo *vi,
			   TRef a)
{
  if (vi->esize == 8) {
    /* Broadcast the sign of each 64 bit lane, then (v^m)-m. */
    IRType wvt = (IRType)(IRT_V4I32 | (vt & IRT_VEC256));
    TRef sh = emitir(IRT(IR_VSAR, wvt), a, lj_ir_kint(J, 31));
    TRef m = emitir(IRT(IR_VSHUF, vt), sh,
		    IRVSHUF(IRVSHUF_PSHUFD, 0xf5));
    return emitir(IRT(IR_VSUB, vt), emitir(IRT(IR_VXOR, vt), a, m), m);
  }
  return emitir(IRT(IR_VABS, vt), a, 0);
}

void LJ_FASTCALL recff_simd_unop(jit_State *J, RecordFFData *rd)
{
  CTVecInfo vi; CTypeID id; IRType vt;
  TRef a = crec_simd_arg(J, rd, 0, &vi, &id, &vt);
  TRef r;
  switch (rd->data) {
  case VUN_NOT:
    r = emitir(IRT(IR_VXOR, vt), a, crec_vec_kones(J, vt));
    break;
  case VUN_SQRT:
    crec_simd_need(J, veck_isfp(vi.kind));
    r = emitir(IRT(IR_VSQRT, vt), a, 0);
    break;
  default: {  /* VUN_ABS */
    if (veck_isfp(vi.kind)) {
      uint8_t ebuf[8];
      memset(ebuf, 0xff, sizeof(ebuf));
      ebuf[vi.esize-1] = 0x7f;  /* Clear the sign bit of every lane. */
      r = emitir(IRT(IR_VAND, vt), a, crec_vec_kmask(J, vt, &vi, ebuf));
    } else if (veck_isunsigned(vi.kind)) {
      r = a;
    } else {
      crec_simd_need(J, vi.esize == 8 || (J->flags & JIT_F_SSSE3));
      r = crec_simd_iabs(J, vt, &vi, a);
    }
    break;
    }
  }
  J->base[0] = crec_vec_box(J, r, vt, id);
}

void LJ_FASTCALL recff_simd_round(jit_State *J, RecordFFData *rd)
{
  CTVecInfo vi; CTypeID id; IRType vt;
  TRef a = crec_simd_arg(J, rd, 0, &vi, &id, &vt);
  crec_simd_need(J, veck_isfp(vi.kind) && (J->flags & JIT_F_SSE4_1));
  J->base[0] = crec_vec_box(J, emitir(IRT(IR_VROUND, vt), a, rd->data), vt, id);
}

/* Bias both operands by the sign bit, so an unsigned compare becomes signed. */
static TRef crec_simd_ubias(jit_State *J, IRType vt, const CTVecInfo *vi,
			    TRef tr)
{
  uint8_t ebuf[8];
  memset(ebuf, 0, sizeof(ebuf));
  ebuf[vi->esize-1] = 0x80;
  return emitir(IRT(IR_VXOR, vt), tr, crec_vec_kmask(J, vt, vi, ebuf));
}

void LJ_FASTCALL recff_simd_cmp(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTVecInfo vi; CTypeID id; IRType vt;
  TRef a = crec_simd_arg(J, rd, 0, &vi, &id, &vt);
  TRef b;
  uint32_t op = rd->data;
  int isfp = veck_isfp(vi.kind);
  int inv = 0;
  TRef r;
  CTypeID mid;
  b = crec_simd_arg2(J, rd, 1, &vi, id, vt);
  if (op == VCMP_NE) { op = VCMP_EQ; inv = 1; }
  else if (op == VCMP_LE) { op = VCMP_GE; { TRef t = a; a = b; b = t; } }
  else if (op == VCMP_LT) { op = VCMP_GT; { TRef t = a; a = b; b = t; } }
  if (op == VCMP_GE && !isfp) {  /* Integer: a >= b is !(b > a). */
    TRef t = a; a = b; b = t;
    op = VCMP_GT;
    inv ^= 1;
  }
  if (op == VCMP_EQ) {
    crec_simd_need(J, isfp || vi.esize != 8 || (J->flags & JIT_F_SSE4_1) ||
		      1);  /* V2I64 has an SSE2 fallback in the backend. */
    r = emitir(IRT(IR_VCMPEQ, vt), a, b);
  } else if (op == VCMP_GT) {
    if (!isfp) {
      if (veck_isunsigned(vi.kind)) {
	a = crec_simd_ubias(J, vt, &vi, a);
	b = crec_simd_ubias(J, vt, &vi, b);
      }
      crec_simd_need(J, vi.esize != 8 || (J->flags & JIT_F_SSE4_2));
    }
    r = emitir(IRT(IR_VCMPGT, vt), a, b);
  } else {
    crec_simd_need(J, isfp);
    r = emitir(IRT(IR_VCMPGE, vt), a, b);
  }
  if (inv) r = emitir(IRT(IR_VXOR, vt), r, crec_vec_kones(J, vt));
  mid = lj_simd_masktype(cts, &vi);
  /* The mask ctype is the signed integer vector of the same shape, so box it
  ** with that lane type: the next load of this cdata uses it, and a store and
  ** a load with different lane types cannot be forwarded.
  */
  J->base[0] = crec_vec_box(J, r, isfp ?
    ((vi.esize == 4 ? IRT_V4I32 : IRT_V2I64) | (vt & IRT_VEC256)) : vt,
    mid);
}

void LJ_FASTCALL recff_simd_shift(jit_State *J, RecordFFData *rd)
{
  CTVecInfo vi; CTypeID id; IRType vt;
  TRef a = crec_simd_arg(J, rd, 0, &vi, &id, &vt);
  TRef n = J->base[1];
  uint32_t op = rd->data;
  IROp irop = op == VSH_SHL ? IR_VSHL : op == VSH_SHR ? IR_VSHR : IR_VSAR;
  uint32_t bits = (uint32_t)vi.esize * 8;
  TRef r;
  crec_simd_need(J, !veck_isfp(vi.kind));
  if (n && tref_iscdata(n)) {
    /* A vector count shifts each lane by its own amount. */
    CTVecInfo nvi; CTypeID nid; IRType nvt;
    TRef nv = crec_simd_arg(J, rd, 1, &nvi, &nid, &nvt);
    IROp vop = op == VSH_SHL ? IR_VSHLV : op == VSH_SHR ? IR_VSHRV : IR_VSARV;
    crec_simd_need(J, (J->flags & JIT_F_AVX2) != 0 &&
		      !veck_isfp(nvi.kind) && nvi.esize == vi.esize &&
		      nvi.lanes == vi.lanes);
    if (vi.esize == 1 && op == VSH_SHL) {
      r = crec_simd_shiftv_i8_left(J, vt, a, nv);
      J->base[0] = crec_vec_box(J, r, vt, id);
      return;
    }
    if (vi.esize == 2 && op == VSH_SHL) {
      r = crec_simd_shiftv_i16_left(J, vt, a, nv);
      J->base[0] = crec_vec_box(J, r, vt, id);
      return;
    }
    if (vi.esize == 2 && op == VSH_SHR) {
      r = crec_simd_shiftv_i16_right(J, vt, a, nv, 0);
      J->base[0] = crec_vec_box(J, r, vt, id);
      return;
    }
    /*
    ** The signed high-product path removes YMM register pressure, but the
    ** two-dword arithmetic-shift path is faster for streaming XMM chains.
    */
    if (vi.esize == 2 && op == VSH_SAR && (vt & IRT_VEC256)) {
      r = crec_simd_shiftv_i16_right(J, vt, a, nv, 1);
      J->base[0] = crec_vec_box(J, r, vt, id);
      return;
    }
    if (vi.esize == 1 && op == VSH_SHR) {
      r = crec_simd_shiftv_i8_right(J, vt, a, nv);
      J->base[0] = crec_vec_box(J, r, vt, id);
      return;
    }
    if (vi.esize == 1 && op == VSH_SAR) {
      r = crec_simd_shiftv_i8_sar(J, vt, a, nv);
      J->base[0] = crec_vec_box(J, r, vt, id);
      return;
    }
    if (vi.esize < 4) {
      r = crec_simd_shiftv_narrow(J, vt, vi.esize, a, nv, vop);
      J->base[0] = crec_vec_box(J, r, vt, id);
      return;
    }
    if (vi.esize == 8 && op == VSH_SAR) {
      /*
      ** No VPSRAVQ. Use ((v >>u n) ^ m) - m with m = (1<<63) >>u n, which is
      ** an arithmetic shift for any n <= 63. The count has to be clamped
      ** first: VPSRLVQ flushes to zero past the lane width, which would take
      ** the sign bias with it and lose the sign fill.
      */
      uint8_t ebuf[8], zbuf[LJ_VEC_MAXSIZE];
      TRef k63, khi, kz, n6, hi, lt64, nc, ks;
      uint64_t s;
      memset(zbuf, 0, sizeof(zbuf));
      kz = lj_ir_kvec(J, nvt, zbuf);
      s = 63; memcpy(ebuf, &s, 8);
      k63 = crec_vec_kmask(J, nvt, &nvi, ebuf);
      s = ~(uint64_t)63; memcpy(ebuf, &s, 8);
      khi = crec_vec_kmask(J, nvt, &nvi, ebuf);
      n6 = emitir(IRT(IR_VAND, nvt), nv, k63);
      hi = emitir(IRT(IR_VAND, nvt), nv, khi);
      lt64 = emitir(IRT(IR_VCMPEQ, nvt), hi, kz);  /* All ones where n < 64. */
      nc = emitir(IRT(IR_VOR, nvt), n6,
		  emitir(IRT(IR_VANDN, nvt), lt64, k63));
      s = (uint64_t)1 << 63; memcpy(ebuf, &s, 8);
      ks = emitir(IRT(IR_VSHRV, vt), crec_vec_kmask(J, vt, &vi, ebuf), nc);
      r = emitir(IRT(IR_VSHRV, vt), a, nc);
      r = emitir(IRT(IR_VSUB, vt), emitir(IRT(IR_VXOR, vt), r, ks), ks);
    } else {
      r = emitir(IRT(vop, vt), a, nv);
    }
    J->base[0] = crec_vec_box(J, r, vt, id);
    return;
  }
  if (!n || !tref_isinteger(n)) {
    if (n && tref_isnum(n)) n = lj_opt_narrow_toint(J, n);
    else lj_trace_err(J, LJ_TRERR_BADTYPE);
  }
  if (vi.esize == 1 || (vi.esize == 8 && op == VSH_SAR)) {
    /* No instruction for these: rewrite with wider shifts plus masking.
    ** A constant count folds the masks away; a variable count builds them
    ** at runtime for a few extra packed instructions.
    */
    uint32_t sh;
    uint8_t ebuf[8];
    IRType wvt = (IRType)(IRT_V8I16 | (vt & IRT_VEC256));
    if (!tref_isk(n)) {
      if (vi.esize == 1) {
	/* Shift 16 bit lanes and clear the bits that crossed the byte
	** boundary. The mask is (0xff << n) & 0xff resp. 0xff >> n, which
	** is zero for an out of range count, exactly like the interpreter.
	*/
	TRef m, nc = op == VSH_SAR ? crec_simd_clampcnt(J, n, 7) : n;
	if (op == VSH_SHL) {
	  m = emitir(IRT(IR_VSHL, wvt),
		     crec_simd_k16(J, wvt, 0x00ff), nc);
	  m = emitir(IRT(IR_VAND, wvt), m,
		     crec_simd_k16(J, wvt, 0x00ff));
	  r = emitir(IRT(IR_VSHL, wvt), a, nc);
	} else {
	  m = emitir(IRT(IR_VSHR, wvt),
		     crec_simd_k16(J, wvt, 0x00ff), nc);
	  r = emitir(IRT(IR_VSHR, wvt), a, nc);
	}
	r = emitir(IRT(IR_VAND, vt), r, crec_simd_bcastbyte(J, m));
	if (op == VSH_SAR) {  /* (x ^ s) - s with s = 0x80 >> nc. */
	  TRef s = crec_simd_bcastbyte(J,
	    emitir(IRT(IR_VSHR, wvt), crec_simd_k16(J, wvt, 0x0080), nc));
	  r = emitir(IRT(IR_VSUB, vt), emitir(IRT(IR_VXOR, vt), r, s), s);
	}
      } else {
	/* 64 bit arithmetic shift right, same identity as below but with the
	** bias vector built at runtime. Clamping the count matters here: an
	** unclamped PSRLQ would flush the bias to zero and lose the sign.
	*/
	TRef ks, nc = crec_simd_clampcnt(J, n, 63);
	uint64_t s = (uint64_t)1 << 63;
	memcpy(ebuf, &s, 8);
	ks = emitir(IRT(IR_VSHR, vt), crec_vec_kmask(J, vt, &vi, ebuf), nc);
	r = emitir(IRT(IR_VSHR, vt), a, nc);
	r = emitir(IRT(IR_VSUB, vt), emitir(IRT(IR_VXOR, vt), r, ks), ks);
      }
      J->base[0] = crec_vec_box(J, r, vt, id);
      return;
    }
    sh = (uint32_t)IR(tref_ref(n))->i;
    if (vi.esize == 1) {
      if (sh >= 8) {  /* Flushes to zero, or to a full sign fill. */
	if (op == VSH_SAR) sh = 7; else {
	  uint8_t zero[LJ_VEC_MAXSIZE];
	  memset(zero, 0, sizeof(zero));
	  J->base[0] = crec_vec_box(J, lj_ir_kvec(J, vt, zero), vt, id);
	  return;
	}
      }
      if (op == VSH_SHL) {
	r = emitir(IRT(IR_VSHL, wvt), a, lj_ir_kint(J, (int32_t)sh));
	memset(ebuf, (int)((0xffu << sh) & 0xff), sizeof(ebuf));
	r = emitir(IRT(IR_VAND, vt), r, crec_vec_kmask(J, vt, &vi, ebuf));
      } else {
	r = emitir(IRT(IR_VSHR, wvt), a, lj_ir_kint(J, (int32_t)sh));
	memset(ebuf, (int)((0xffu >> sh) & 0xff), sizeof(ebuf));
	r = emitir(IRT(IR_VAND, vt), r, crec_vec_kmask(J, vt, &vi, ebuf));
	if (op == VSH_SAR) {  /* (x ^ s) - s with s = 0x80 >> sh. */
	  TRef ks;
	  memset(ebuf, (int)(0x80u >> sh), sizeof(ebuf));
	  ks = crec_vec_kmask(J, vt, &vi, ebuf);
	  r = emitir(IRT(IR_VSUB, vt), emitir(IRT(IR_VXOR, vt), r, ks), ks);
	}
      }
    } else {
      /* 64 bit arithmetic shift right, which has no instruction before
      ** AVX-512: sar(x,n) == ((x >>u n) ^ m) - m with m = (1<<63) >>u n.
      */
      TRef ks;
      uint64_t m;
      if (sh >= 64) sh = 63;
      m = ((uint64_t)1 << 63) >> sh;
      memcpy(ebuf, &m, 8);
      ks = crec_vec_kmask(J, vt, &vi, ebuf);
      r = emitir(IRT(IR_VSHR, vt), a, lj_ir_kint(J, (int32_t)sh));
      r = emitir(IRT(IR_VSUB, vt), emitir(IRT(IR_VXOR, vt), r, ks), ks);
    }
  } else {
    if (tref_isk(n)) {
      uint32_t sh = (uint32_t)IR(tref_ref(n))->i;
      if (op == VSH_SAR && sh >= bits) sh = bits - 1;
      n = lj_ir_kint(J, (int32_t)sh);
    }
    /* No clamping is needed for a variable count: the arithmetic shifts fill
    ** with the sign bit and the logical shifts flush to zero when the 64 bit
    ** count is out of range, which is the interpreter's definition too.
    */
    r = emitir(IRT(irop, vt), a, n);
  }
  J->base[0] = crec_vec_box(J, r, vt, id);
}

void LJ_FASTCALL recff_simd_movemask(jit_State *J, RecordFFData *rd)
{
  CTVecInfo vi; CTypeID id; IRType vt;
  TRef a = crec_simd_arg(J, rd, 0, &vi, &id, &vt);
  J->base[0] = emitir(IRTI(IR_VMOVMSK), a, IRVSRC(vt, 0));
}

void LJ_FASTCALL recff_ffi_simd_mulhi(jit_State *J, RecordFFData *rd)
{
  CTVecInfo vi; CTypeID id; IRType vt;
  TRef a = crec_simd_arg(J, rd, 0, &vi, &id, &vt);
  TRef b;
  b = crec_simd_arg2(J, rd, 1, &vi, id, vt);
  if (!veck_isfp(vi.kind) && vi.esize == 1) {
    IRType wvt = (IRType)(IRT_V8I16 | (vt & IRT_VEC256));
    TRef lo = crec_simd_k16(J, wvt, 0x00ff);
    TRef hi = crec_simd_k16(J, wvt, 0xff00);
    TRef even, odd;
    if (a == b) {
      TRef sq = veck_isunsigned(vi.kind) ? a :
	emitir(IRT(IR_VABS, vt), a, 0);
      /* A byte square is unsigned and fits in a word. Multiply the absolute,
      ** zero-extended even and odd bytes, then take bits 8..15.
      */
      even = emitir(IRT(IR_VAND, wvt), sq, lo);
      odd = emitir(IRT(IR_VSHR, wvt), sq, lj_ir_kint(J, 8));
      even = emitir(IRT(IR_VMUL, wvt), even, even);
      even = emitir(IRT(IR_VSHR, wvt), even, lj_ir_kint(J, 8));
      odd = emitir(IRT(IR_VMUL, wvt), odd, odd);
      odd = emitir(IRT(IR_VAND, wvt), odd, hi);
      J->base[0] = crec_vec_box(J, emitir(IRT(IR_VOR, vt), even, odd), vt, id);
      return;
    }
    if (veck_isunsigned(vi.kind)) {
      /* Scale one byte operand by 2^8, then PMULHUW gives bits 8..15
      ** of the original byte product in the low byte of each word.
      */
      even = emitir(IRT(IR_VMULHIU, wvt),
		    emitir(IRT(IR_VAND, wvt), a, lo),
		    emitir(IRT(IR_VSHL, wvt), b, lj_ir_kint(J, 8)));
      odd = emitir(IRT(IR_VMULHIU, wvt),
		   emitir(IRT(IR_VSHR, wvt), a, lj_ir_kint(J, 8)),
		   emitir(IRT(IR_VAND, wvt), b, hi));
    } else {
      /* Sign-extend one byte and keep the other scaled by 2^8. PMULHW
      ** then performs the required signed arithmetic shift by eight.
      */
      even = emitir(IRT(IR_VMULHI, wvt),
		    emitir(IRT(IR_VSAR, wvt),
		      emitir(IRT(IR_VSHL, wvt), a, lj_ir_kint(J, 8)),
		      lj_ir_kint(J, 8)),
		    emitir(IRT(IR_VSHL, wvt), b, lj_ir_kint(J, 8)));
      odd = emitir(IRT(IR_VMULHI, wvt),
		   emitir(IRT(IR_VSAR, wvt), a, lj_ir_kint(J, 8)),
		   emitir(IRT(IR_VAND, wvt), b, hi));
      even = emitir(IRT(IR_VAND, wvt), even, lo);
    }
    J->base[0] = crec_vec_box(J,
      emitir(IRT(IR_VOR, vt), even,
	emitir(IRT(IR_VSHL, wvt), odd, lj_ir_kint(J, 8))), vt, id);
    return;
  }
  /* PMULHW/PMULHUW cover 16 bit lanes directly. For 32 bit lanes the backend
  ** multiplies even and odd dwords separately and blends their high halves.
  */
  crec_simd_need(J, !veck_isfp(vi.kind) &&
		    (vi.esize == 2 ||
		     vi.esize == 8 ||
		     (vi.esize == 4 && (J->flags & JIT_F_SSE4_1))));
  J->base[0] = crec_vec_box(J,
    emitir(IRT(veck_isunsigned(vi.kind) ? IR_VMULHIU : IR_VMULHI, vt), a, b),
    vt, id);
}

void LJ_FASTCALL recff_ffi_simd_fma(jit_State *J, RecordFFData *rd)
{
  CTVecInfo vi; CTypeID id; IRType vt;
  TRef a = crec_simd_arg(J, rd, 0, &vi, &id, &vt);
  TRef b, c;
  b = crec_simd_arg2(J, rd, 1, &vi, id, vt);
  c = crec_simd_arg2(J, rd, 2, &vi, id, vt);
  /* Only lower this where the hardware fuses too. Without FMA the trace
  ** aborts and the interpreter's fma() still gives the single-rounded
  ** result, so the two never disagree.
  */
  crec_simd_need(J, veck_isfp(vi.kind) && (J->flags & JIT_F_FMA) != 0);
  /* Three operands do not fit in one IR instruction; the second and third
  ** travel in a CARG pair, which asm_ir() treats as a no-op.
  */
  J->base[0] = crec_vec_box(J,
    emitir(IRT(IR_VFMA, vt), a, emitir(IRT(IR_CARG, IRT_NIL), b, c)), vt, id);
}

static int crec_simd_iskfill(jit_State *J, IRRef ref, uint32_t size,
			     uint8_t fill)
{
  IRIns *k = IR(ref);
  const uint8_t *p;
  uint32_t i;
  if (k->o != IR_KVEC) return 0;
  p = ir_kvec(k);
  for (i = 0; i < size; i++)
    if (p[i] != fill) return 0;
  return 1;
}

void LJ_FASTCALL recff_ffi_simd_select(jit_State *J, RecordFFData *rd)
{
  CTVecInfo mvi, vi; CTypeID mid, id; IRType mvt, vt;
  TRef m = crec_simd_arg(J, rd, 0, &mvi, &mid, &mvt);
  TRef a = crec_simd_arg(J, rd, 1, &vi, &id, &vt);
  TRef b = crec_simd_arg2(J, rd, 2, &vi, id, vt);
  TRef r;
  IRIns *mc = IR(tref_ref(m));
  IRRef ca, cb;
  int directmm, inv = 0;
  uint32_t size = (uint32_t)vi.esize * vi.lanes;
  crec_simd_need(J, (CTSize)mvi.esize * mvi.lanes ==
		    (CTSize)vi.esize * vi.lanes);
  /*
  ** A comparison selecting the same operands is exactly min/max. Keep the
  ** operand order: x86 FP min/max returns its second operand for unordered
  ** inputs and equal signed zeros, just as the false arm of select does.
  */
  directmm = veck_isfp(vi.kind) ||
    (vi.esize != 8 &&
     (veck_isunsigned(vi.kind) ?
      (vi.esize == 1 || (J->flags & JIT_F_SSE4_1)) :
      (vi.esize == 2 || (J->flags & JIT_F_SSE4_1))));
  if (mc->o == IR_VXOR && !veck_isfp(vi.kind)) {
    IRIns *x1 = IR(mc->op1), *x2 = IR(mc->op2);
    if (x1->o == IR_VCMPGT &&
	crec_simd_iskfill(J, mc->op2, size, 0xff)) {
      mc = x1; inv = 1;
    } else if (x2->o == IR_VCMPGT &&
	       crec_simd_iskfill(J, mc->op1, size, 0xff)) {
      mc = x2; inv = 1;
    }
  }
  ca = mc->op1;
  cb = mc->op2;
  /*
  ** select(x > 0, x, -x), and its <, >= and <= equivalents, are signed
  ** integer absolute value. Match only a literal zero and the exact negated
  ** selected operand. This preserves wraparound at INT_MIN and avoids
  ** changing the subtler FP NaN and signed-zero rules.
  */
  if (!veck_isfp(vi.kind) && !veck_isunsigned(vi.kind) &&
      mc->o == IR_VCMPGT && irt_vtype(mc->t) == vt &&
      mvi.esize == vi.esize && mvi.lanes == vi.lanes &&
      (vi.esize == 8 || (J->flags & JIT_F_SSSE3))) {
    TRef x = 0;
    int truthpos = 0, maskpos = 0, haszero = 0;
    IRIns *na = IR(tref_ref(a)), *nb = IR(tref_ref(b));
    if (nb->o == IR_VSUB && irt_vtype(nb->t) == vt &&
	nb->op2 == tref_ref(a) &&
	crec_simd_iskfill(J, nb->op1, size, 0)) {
      x = a; truthpos = 1;
    } else if (na->o == IR_VSUB && irt_vtype(na->t) == vt &&
	       na->op2 == tref_ref(b) &&
	       crec_simd_iskfill(J, na->op1, size, 0)) {
      x = b; truthpos = 0;
    }
    if (x) {
      if (ca == tref_ref(x) && crec_simd_iskfill(J, cb, size, 0)) {
	maskpos = 1 ^ inv; haszero = 1;
      } else if (cb == tref_ref(x) &&
		 crec_simd_iskfill(J, ca, size, 0)) {
	maskpos = inv; haszero = 1;
      }
      if (haszero && truthpos == maskpos) {
	r = crec_simd_iabs(J, vt, &vi, x);
	J->base[0] = crec_vec_box(J, r, vt, id);
	return;
      }
    }
  }
  if (mc->o == IR_VCMPGT && directmm &&
      irt_isvecfp(mc->t) == veck_isfp(vi.kind) &&
      irt_vecesz(mc->t) == vi.esize &&
      mvi.esize == vi.esize && mvi.lanes == vi.lanes) {
    if (veck_isunsigned(vi.kind)) {
      uint8_t ebuf[8];
      IRRef kref;
      int oka = 0, okb = 0;
      memset(ebuf, 0, sizeof(ebuf));
      ebuf[vi.esize-1] = 0x80;
      kref = tref_ref(crec_vec_kmask(J, vt, &vi, ebuf));
      if (IR(ca)->o == IR_VXOR) {
	IRIns *x = IR(ca);
	if (x->op1 == kref) { ca = x->op2; oka = 1; }
	else if (x->op2 == kref) { ca = x->op1; oka = 1; }
      }
      if (IR(cb)->o == IR_VXOR) {
	IRIns *x = IR(cb);
	if (x->op1 == kref) { cb = x->op2; okb = 1; }
	else if (x->op2 == kref) { cb = x->op1; okb = 1; }
      }
      if (!oka || !okb) ca = cb = REF_DROP;
    }
    if ((ca == tref_ref(a) && cb == tref_ref(b)) ||
	(ca == tref_ref(b) && cb == tref_ref(a))) {
      int normal = ca == tref_ref(a);
      int ismax = inv ? !normal : normal;
      IROp op = ismax ?
	(veck_isunsigned(vi.kind) ? IR_VMAXU : IR_VMAX) :
	(veck_isunsigned(vi.kind) ? IR_VMINU : IR_VMIN);
      r = emitir(IRT(op, vt), a, b);
      J->base[0] = crec_vec_box(J, r, vt, id);
      return;
    }
  }
  /* (mask & a) | (~mask & b), which is what SSE2 needs anyway. */
  r = emitir(IRT(IR_VOR, vt),
	     emitir(IRT(IR_VAND, vt), m, a),
	     emitir(IRT(IR_VANDN, vt), m, b));
  J->base[0] = crec_vec_box(J, r, vt, id);
}

void LJ_FASTCALL recff_ffi_simd_bitcast(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTVecInfo svi, dvi; CTypeID sid; IRType svt, dvt;
  CTypeID did;
  TRef trct = J->base[0];
  TRef a;
  CType *dct;
  if (!trct || !tref_iscdata(trct)) lj_trace_err(J, LJ_TRERR_BADTYPE);
  {
    GCcdata *cd = argv2cdata(J, trct, &rd->argv[0]);
    did = cd->ctypeid == CTID_CTYPEID ? *(CTypeID *)cdataptr(cd) : cd->ctypeid;
  }
  a = crec_simd_arg(J, rd, 1, &svi, &sid, &svt);
  dct = ctype_raw(cts, did);
  if (!lj_ctype_vecinfo(cts, dct, &dvi)) lj_trace_err(J, LJ_TRERR_BADTYPE);
  dvt = crec_vec2irt(cts, dct);
  crec_simd_need(J, dvt != IRT_NIL &&
		    (CTSize)dvi.esize * dvi.lanes ==
		    (CTSize)svi.esize * svi.lanes);
  /* A bitcast changes no bits: box the same value under the new ctype, with
  ** the destination lane type so that reloading it can forward.
  */
  J->base[0] = crec_vec_box(J, a, dvt, did);
}

/*
** Convert packed u32 lanes to float without AVX-512. Each source lane is
** split into two exactly representable 16 bit pieces encoded directly as
** floats; their biases cancel before the one rounding addition.
*/
static TRef crec_simd_u32tof32(jit_State *J, IRType svt, IRType dvt, TRef a)
{
  TRef lo = emitir(IRT(IR_VAND, svt), a,
		   crec_simd_k32(J, svt, 0x0000ffffu));
  TRef hi = emitir(IRT(IR_VSHR, svt), a, lj_ir_kint(J, 16));
  lo = emitir(IRT(IR_VOR, svt), lo,
	      crec_simd_k32(J, svt, 0x4b000000u));
  hi = emitir(IRT(IR_VOR, svt), hi,
	      crec_simd_k32(J, svt, 0x53000000u));
  hi = emitir(IRT(IR_VSUB, dvt), hi,
	      crec_simd_k32(J, dvt, 0x53000080u));
  return emitir(IRT(IR_VADD, dvt), lo, hi);
}

/*
** Convert packed 64 bit integers to double from exact 32 bit pieces.
** ORing the pieces into binary64 mantissas makes the integer arithmetic
** implicit; the constants remove the exponents and leave one final rounded
** add. For signed input, bias the high dword by 2^31 first.
*/
static TRef crec_simd_i64tof64(jit_State *J, IRType svt, IRType dvt, TRef a,
			       int uns)
{
  uint64_t bias = U64x(45300000,00100000);
  TRef lo = emitir(IRT(IR_VAND, svt), a,
		   crec_simd_k64(J, svt, U64x(00000000,ffffffff)));
  TRef hi = emitir(IRT(IR_VSHR, svt), a, lj_ir_kint(J, 32));
  if (!uns) {
    hi = emitir(IRT(IR_VXOR, svt), hi,
		crec_simd_k64(J, svt, U64x(00000000,80000000)));
    bias = U64x(45300000,80100000);
  }
  lo = emitir(IRT(IR_VOR, svt), lo,
	      crec_simd_k64(J, svt, U64x(43300000,00000000)));
  hi = emitir(IRT(IR_VOR, svt), hi,
	      crec_simd_k64(J, svt, U64x(45300000,00000000)));
  hi = emitir(IRT(IR_VSUB, dvt), hi, crec_simd_k64(J, dvt, bias));
  return emitir(IRT(IR_VADD, dvt), lo, hi);
}

/*
** Narrow integer lanes from one YMM register to one XMM register, keeping the
** low half of every source lane. VPSHUFB compacts the wanted bytes into the
** low qword of each 128 bit half; the VCONV backend completes the operation
** with one VPERMQ that places those two qwords next to each other.
*/
static TRef crec_simd_narrow_int(jit_State *J, IRType svt, IRType dvt, TRef a,
				 uint32_t dk, uint32_t sk,
				 uint32_t desz, uint32_t sesz)
{
  uint8_t mask[LJ_VEC_MAXSIZE];
  uint32_t half, lane, byte, nph = 16 / sesz;
  memset(mask, 0x80, sizeof(mask));
  for (half = 0; half < 2; half++)
    for (lane = 0; lane < nph; lane++)
      for (byte = 0; byte < desz; byte++)
	mask[half*16 + lane*desz + byte] = (uint8_t)(lane*sesz + byte);
  a = emitir(IRT(IR_VSHUFB, svt), a, lj_ir_kvec(J, svt, mask));
  return emitir(IRT(IR_VCONV, dvt), a, IRVCONV(dk, sk));
}

/* Convert four unsigned dwords to four doubles exactly. Zero extension puts
** every value into the mantissa of 2^52+x, so one packed subtraction removes
** the exponent bias. This is three AVX2 instructions and rounds no bits.
*/
static TRef crec_simd_u32tof64(jit_State *J, IRType dvt, TRef a)
{
  IRType qvt = (IRType)(IRT_V2I64 | IRT_VEC256);
  TRef bias = crec_simd_k64(J, qvt, U64x(43300000,00000000));
  TRef w = emitir(IRT(IR_VCONV, qvt), a, IRVCONV(VECK_U64, VECK_U32));
  w = emitir(IRT(IR_VOR, qvt), w, bias);
  return emitir(IRT(IR_VSUB, dvt), w, bias);
}

/* Widen signed/unsigned words to dwords before the direct packed dword to
** float conversion. Both source ranges fit in a signed dword.
*/
static TRef crec_simd_i16tof32(jit_State *J, IRType dvt, TRef a, uint32_t sk)
{
  IRType ivt = (IRType)(IRT_V4I32 | IRT_VEC256);
  TRef w = emitir(IRT(IR_VCONV, ivt), a, IRVCONV(VECK_I32, sk));
  return emitir(IRT(IR_VCONV, dvt), w, IRVCONV(VECK_F32, VECK_I32));
}

/*
** Float-to-word conversion has narrower indefinite bounds than CVTTPS2DQ.
** Check the original floats against [-32768,32767], substitute -32768 for
** every unordered/out-of-range lane, then truncate the valid lanes and use
** the ordinary integer narrowing path.
*/
static TRef crec_simd_f32toi16(jit_State *J, IRType svt, IRType dvt, TRef a,
			       uint32_t dk)
{
  IRType ivt = (IRType)(IRT_V4I32 | IRT_VEC256);
  TRef kmin = crec_simd_k32(J, svt, 0xc7000000u);  /* -32768.0f. */
  TRef kmax = crec_simd_k32(J, svt, 0x46fffe00u);  /*  32767.0f. */
  TRef valid = emitir(IRT(IR_VAND, svt),
    emitir(IRT(IR_VCMPGE, svt), a, kmin),
    emitir(IRT(IR_VCMPGE, svt), kmax, a));
  TRef q = emitir(IRT(IR_VCONV, ivt), a,
		  IRVCONV(VECK_I32, VECK_F32));
  TRef bad = crec_simd_k32(J, ivt, 0xffff8000u);
  q = emitir(IRT(IR_VOR, ivt),
	     emitir(IRT(IR_VAND, ivt), valid, q),
	     emitir(IRT(IR_VANDN, ivt), valid, bad));
  return crec_simd_narrow_int(J, ivt, dvt, q, dk, VECK_I32, 2, 4);
}

void LJ_FASTCALL recff_ffi_simd_convert(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTVecInfo svi, dvi; CTypeID sid; IRType svt, dvt;
  CTypeID did;
  TRef trct = J->base[0];
  TRef a, r;
  CType *dct;
  if (!trct || !tref_iscdata(trct)) lj_trace_err(J, LJ_TRERR_BADTYPE);
  {
    GCcdata *cd = argv2cdata(J, trct, &rd->argv[0]);
    did = cd->ctypeid == CTID_CTYPEID ? *(CTypeID *)cdataptr(cd) : cd->ctypeid;
  }
  a = crec_simd_arg(J, rd, 1, &svi, &sid, &svt);
  dct = ctype_raw(cts, did);
  if (!lj_ctype_vecinfo(cts, dct, &dvi)) lj_trace_err(J, LJ_TRERR_BADTYPE);
  dvt = crec_vec2irt(cts, dct);
  crec_simd_need(J, dvt != IRT_NIL && dvi.lanes == svi.lanes);
  if (dvi.kind == svi.kind) {
    J->base[0] = crec_vec_box(J, a, dvt, did);
    return;
  }
  if (dvi.esize != svi.esize) {
    /*
    ** Equal-lane cross-width conversions are exactly the 16 <-> 32 byte
    ** shapes on the native JIT surface. The 32 byte side already guarantees
    ** AVX2 through crec_vec2irt().
    */
    if (!veck_isfp(dvi.kind) && !veck_isfp(svi.kind)) {
      r = dvi.esize > svi.esize ?
	  emitir(IRT(IR_VCONV, dvt), a, IRVCONV(dvi.kind, svi.kind)) :
	  crec_simd_narrow_int(J, svt, dvt, a, dvi.kind, svi.kind,
			       dvi.esize, svi.esize);
    } else if (dvi.kind == VECK_F64) {
      if (svi.kind == VECK_U32)
	r = crec_simd_u32tof64(J, dvt, a);
      else {
	crec_simd_need(J, svi.kind == VECK_F32 || svi.kind == VECK_I32);
	r = emitir(IRT(IR_VCONV, dvt), a,
		   IRVCONV(dvi.kind, svi.kind));
      }
    } else if (dvi.kind == VECK_F32) {
      if (svi.kind == VECK_I16 || svi.kind == VECK_U16)
	r = crec_simd_i16tof32(J, dvt, a, svi.kind);
      else {
	crec_simd_need(J, svi.kind == VECK_F64 ||
			 svi.kind == VECK_I64 || svi.kind == VECK_U64);
	r = emitir(IRT(IR_VCONV, dvt), a,
		   IRVCONV(dvi.kind, svi.kind));
      }
    } else if (svi.kind == VECK_F32 &&
	       (dvi.kind == VECK_I64 || dvi.kind == VECK_U64)) {
      IRType f64vt = (IRType)(IRT_V2F64 | IRT_VEC256);
      TRef w = emitir(IRT(IR_VCONV, f64vt), a,
		      IRVCONV(VECK_F64, VECK_F32));
      r = emitir(IRT(IR_VCONV, dvt), w,
		 IRVCONV(dvi.kind, VECK_F64));
    } else if (svi.kind == VECK_F32 &&
	       (dvi.kind == VECK_I16 || dvi.kind == VECK_U16)) {
      r = crec_simd_f32toi16(J, svt, dvt, a, dvi.kind);
    } else {
      crec_simd_need(J, svi.kind == VECK_F64 &&
			    (dvi.kind == VECK_I32 || dvi.kind == VECK_U32));
      r = emitir(IRT(IR_VCONV, dvt), a,
		 IRVCONV(dvi.kind, svi.kind));
    }
    J->base[0] = crec_vec_box(J, r, dvt, did);
    return;
  }
  if (dvi.kind == VECK_F32 && svi.kind == VECK_U32) {
    r = crec_simd_u32tof32(J, svt, dvt, a);
    J->base[0] = crec_vec_box(J, r, dvt, did);
    return;
  }
  if (dvi.kind == VECK_F64 &&
      (svi.kind == VECK_I64 || svi.kind == VECK_U64)) {
    r = crec_simd_i64tof64(J, svt, dvt, a, svi.kind == VECK_U64);
    J->base[0] = crec_vec_box(J, r, dvt, did);
    return;
  }
  /* Only the conversions with a direct packed instruction are compiled. */
  crec_simd_need(J,
    (dvi.kind == VECK_F32 && (svi.kind == VECK_I32)) ||
    (svi.kind == VECK_F32 && (dvi.kind == VECK_I32 || dvi.kind == VECK_U32)) ||
    (svi.kind == VECK_F64 && (dvi.kind == VECK_I64 || dvi.kind == VECK_U64)) ||
    (!veck_isfp(dvi.kind) && !veck_isfp(svi.kind) && dvi.esize == svi.esize));
  J->base[0] = crec_vec_box(J,
    emitir(IRT(IR_VCONV, dvt), a, IRVCONV(dvi.kind, svi.kind)), dvt, did);
}

/* Build a PSHUFB byte-permute mask for a lane shuffle. */
static void crec_simd_shufmask(uint8_t *mask, const CTVecInfo *vi,
			       const uint8_t *idx, uint32_t base)
{
  uint32_t i, j, n = vi->lanes, esz = vi->esize;
  for (i = 0; i < n; i++) {
    uint32_t src = idx[i];
    for (j = 0; j < esz; j++) {
      /* A byte index with bit 7 set makes PSHUFB store zero. */
      mask[i*esz+j] = (src >= base && src < base + n) ?
			(uint8_t)((src - base)*esz + j) : 0x80;
    }
  }
}

/* Collect and validate constant shuffle indices. */
static void crec_simd_idx(jit_State *J, RecordFFData *rd, int narg,
			  uint32_t lanes, uint32_t range, uint8_t *idx)
{
  uint32_t i;
  for (i = 0; i < lanes; i++) {
    TRef tr = J->base[narg + i];
    int32_t v;
    if (!tr || !tref_isk(tr) || !tref_isinteger(tr))
      lj_trace_err(J, LJ_TRERR_NYIVEC);  /* Needs a constant lane index. */
    v = IR(tref_ref(tr))->i;
    if ((uint32_t)v >= range) lj_trace_err(J, LJ_TRERR_BADTYPE);
    idx[i] = (uint8_t)v;
  }
  UNUSED(rd);
}

/* An immediate PSHUFD can express every XMM 32/64 bit lane shuffle and a
** YMM lane-local shuffle when both 128 bit halves use the same pattern.
** Unlike PSHUFB, it needs no mask vector and therefore no extra register or
** constant-pool load.
*/
static int crec_simd_shufd(const CTVecInfo *vi, const uint8_t *idx,
			   uint32_t *imm)
{
  uint32_t dpl, h, i, j, nph, nh, ctl = 0;
  if (vi->esize != 4 && vi->esize != 8) return 0;
  dpl = vi->esize >> 2;
  nph = 16 / vi->esize;
  nh = vi->lanes / nph;
  for (i = 0; i < nph; i++) {
    uint32_t src = idx[i];
    if (src >= nph) return 0;
    for (h = 1; h < nh; h++)
      if (idx[h*nph+i] != h*nph+src) return 0;
    for (j = 0; j < dpl; j++) {
      uint32_t outd = i*dpl+j;
      uint32_t srcd = src*dpl+j;
      ctl |= srcd << (outd*2);
    }
  }
  *imm = ctl;
  return 1;
}

/* Recognise the lane-local low/high interleaves implemented directly by the
** PUNPCK family. AVX/YMM unpacking repeats independently in each 128 bit
** half, so account for that layout rather than treating YMM as one lane.
*/
static int crec_simd_unpk(const CTVecInfo *vi, const uint8_t *idx,
			  IROp *op, int *swap)
{
  uint32_t high, sw, i, n = vi->lanes, nph = 16 / vi->esize;
  for (high = 0; high < 2; high++) {
    for (sw = 0; sw < 2; sw++) {
      for (i = 0; i < n; i++) {
	uint32_t h = i / nph, out = i % nph;
	uint32_t lane = h*nph + (out >> 1) + high*(nph >> 1);
	uint32_t fromb = (out & 1) ^ sw;
	uint32_t want = lane + (fromb ? n : 0);
	if (idx[i] != want) break;
      }
      if (i == n) {
	*op = high ? IR_VUNPKH : IR_VUNPKL;
	*swap = (int)sw;
	return 1;
      }
    }
  }
  return 0;
}

static TRef crec_simd_shuf2_emit(jit_State *J, IRType vt, TRef a, TRef b,
				 uint32_t mode, uint32_t imm)
{
  TRef ctl = lj_ir_kint(J, (int32_t)IRVSHUF2(mode, imm));
  TRef args = emitir(IRT(IR_CARG, IRT_NIL), b, ctl);
  return emitir(IRT(IR_VSHUF2, vt), a, args);
}

/* Match the two-input immediate permutes that x86 implements directly. */
static TRef crec_simd_shuf2_direct(jit_State *J, IRType vt,
				   const CTVecInfo *vi, TRef a, TRef b,
				   const uint8_t *idx)
{
  uint32_t n = vi->lanes, nph = 16 / vi->esize, i;
  if (vt & IRT_VEC256) {
    uint32_t imm = 0;
    for (i = 0; i < 2; i++) {
      uint32_t j, base = idx[i*nph];
      if ((base % nph) != 0) break;
      for (j = 1; j < nph && idx[i*nph+j] == base+j; j++) {}
      if (j != nph) break;
      imm |= (base / nph) << (i*4);
    }
    if (i == 2)
      return crec_simd_shuf2_emit(J, vt, a, b,
				  IRVSHUF2_PERM2I128, imm);
  }
  if (J->flags & JIT_F_SSE4_1) {
    int8_t wsel[16];
    uint32_t nw = ((uint32_t)vi->esize * n) >> 1, imm = 0;
    memset(wsel, -1, sizeof(wsel));
    for (i = 0; i < n; i++) {
      uint32_t v = idx[i], fromb, first, count, w;
      if (v == i) fromb = 0;
      else if (v == n+i) fromb = 1;
      else break;
      first = (i*(uint32_t)vi->esize) >> 1;
      count = vi->esize == 1 ? 1 : (uint32_t)vi->esize >> 1;
      for (w = first; w < first+count; w++) {
	if (wsel[w] >= 0 && wsel[w] != (int8_t)fromb) break;
	wsel[w] = (int8_t)fromb;
      }
      if (w != first+count) break;
    }
    if (i == n) {
      for (i = 0; i < 8; i++) imm |= (uint32_t)wsel[i] << i;
      for (i = 8; i < nw && wsel[i] == wsel[i-8]; i++) {}
      if (i == nw)
	return crec_simd_shuf2_emit(J, vt, a, b,
				    IRVSHUF2_PBLENDW, imm);
    }
  }
  if ((vt & IRT_VEC256) && (J->flags & JIT_F_AVX2) &&
      (vi->esize == 4 || vi->esize == 8)) {
    uint32_t imm = 0, nd = vi->esize >> 2;
    for (i = 0; i < n; i++) {
      uint32_t v = idx[i], fromb, d;
      if (v == i) fromb = 0;
      else if (v == n+i) fromb = 1;
      else break;
      for (d = 0; d < nd; d++)
	imm |= fromb << (i*nd+d);
    }
    if (i == n)
      return crec_simd_shuf2_emit(J, vt, a, b,
				  IRVSHUF2_PBLENDD, imm);
  }
  if (vi->esize == 4) {
    uint32_t sw;
    for (sw = 0; sw < 2; sw++) {
      uint32_t h, imm = 0, nh = n / nph;
      for (h = 0; h < nh; h++) {
	uint32_t out;
	for (out = 0; out < nph; out++) {
	  uint32_t v = idx[h*nph+out], fromb = v >= n;
	  uint32_t lane = v - (fromb ? n : 0);
	  uint32_t wantb = (out >= 2) ^ sw;
	  uint32_t local = lane % nph;
	  if (fromb != wantb || lane / nph != h ||
	      (h && local != ((imm >> (out*2)) & 3)))
	    break;
	  if (!h) imm |= local << (out*2);
	}
	if (out != nph) break;
      }
      if (h == nh)
	return crec_simd_shuf2_emit(J, vt, sw ? b : a, sw ? a : b,
				    IRVSHUF2_SHUFPS, imm);
    }
  } else if (vi->esize == 8) {
    uint32_t sw;
    for (sw = 0; sw < 2; sw++) {
      uint32_t h, imm = 0, nh = n / nph;
      for (h = 0; h < nh; h++) {
	uint32_t out;
	for (out = 0; out < nph; out++) {
	  uint32_t v = idx[h*nph+out], fromb = v >= n;
	  uint32_t lane = v - (fromb ? n : 0);
	  uint32_t wantb = (out != 0) ^ sw;
	  if (fromb != wantb || lane / nph != h) break;
	  imm |= (lane % nph) << (h*nph+out);
	}
	if (out != nph) break;
      }
      if (h == nh)
	return crec_simd_shuf2_emit(J, vt, sw ? b : a, sw ? a : b,
				    IRVSHUF2_SHUFPD, imm);
    }
  }
  if (J->flags & JIT_F_SSSE3) {
    uint32_t sw, shift, nh = n / nph;
    for (sw = 0; sw < 2; sw++) {
      uint32_t xbase = sw ? n : 0, ybase = sw ? 0 : n;
      for (shift = 1; shift < nph; shift++) {
	uint32_t h;
	for (h = 0; h < nh; h++) {
	  uint32_t out;
	  for (out = 0; out < nph; out++) {
	    uint32_t src = out + shift;
	    uint32_t want = h*nph +
	      (src < nph ? ybase + src : xbase + src-nph);
	    if (idx[h*nph+out] != want) break;
	  }
	  if (out != nph) break;
	}
	if (h == nh)
	  return crec_simd_shuf2_emit(J, vt, sw ? b : a, sw ? a : b,
				      IRVSHUF2_ALIGNR, shift*vi->esize);
      }
    }
  }
  if (vt & IRT_VEC256) {
    uint32_t sw, shift;
    for (sw = 0; sw < 2; sw++) {
      uint32_t xbase = sw ? n : 0, ybase = sw ? 0 : n;
      for (shift = 1; shift < n; shift++) {
	if (shift == nph) continue;  /* One VPERM2I128, matched above. */
	for (i = 0; i < n; i++) {
	  uint32_t src = i + shift;
	  uint32_t want = src < n ? xbase + src : ybase + src-n;
	  if (idx[i] != want) break;
	}
	if (i == n)
	  return crec_simd_shuf2_emit(J, vt, sw ? b : a, sw ? a : b,
				      IRVSHUF2_ALIGNR256,
				      shift*vi->esize);
      }
    }
  }
  return 0;
}

/* A 128 bit permute is one PSHUFB. A 256 bit permute also shuffles a copy
** with its 128 bit halves swapped, then selects the matching bytes.
*/
static TRef crec_simd_shuf1(jit_State *J, IRType vt, const CTVecInfo *vi,
			    TRef a, const uint8_t *idx, uint32_t base)
{
  IRType bvt = (IRType)(IRT_V16I8 | (vt & IRT_VEC256));
  uint8_t mask[LJ_VEC_MAXSIZE];
  if (!(vt & IRT_VEC256)) {
    crec_simd_shufmask(mask, vi, idx, base);
    return emitir(IRT(IR_VSHUFB, vt), a, lj_ir_kvec(J, bvt, mask));
  } else {
    uint8_t cross[LJ_VEC_MAXSIZE];
    uint32_t i, j, n = vi->lanes, esz = vi->esize;
    int has_same = 0, has_cross = 0;
    TRef same, other, sw;
    memset(mask, 0x80, sizeof(mask));
    memset(cross, 0x80, sizeof(cross));
    for (i = 0; i < n; i++) {
      uint32_t src = idx[i];
      if (src >= base && src < base + n) {
	uint32_t lane = src - base;
	int is_cross = (((i*esz) ^ (lane*esz)) & 16) != 0;
	uint8_t *m = is_cross ? cross : mask;
	if (is_cross) has_cross = 1; else has_same = 1;
	for (j = 0; j < esz; j++)
	  m[i*esz+j] = (uint8_t)((lane*esz+j) & 15);
      }
    }
    if (!has_cross)
      return emitir(IRT(IR_VSHUFB, vt), a, lj_ir_kvec(J, bvt, mask));
    sw = emitir(IRT(IR_VSHUF, vt), a, IRVSHUF(IRVSHUF_SWAP128, 0));
    other = emitir(IRT(IR_VSHUFB, vt), sw, lj_ir_kvec(J, bvt, cross));
    if (!has_same) return other;
    same = emitir(IRT(IR_VSHUFB, vt), a, lj_ir_kvec(J, bvt, mask));
    return emitir(IRT(IR_VOR, vt), same, other);
  }
}

/* Lower a validated constant permutation of one source. Shared by shuffle
** and by shuffle2 patterns that do not actually use both inputs.
*/
static TRef crec_simd_constshuffle(jit_State *J, IRType vt,
				   const CTVecInfo *vi, TRef a,
				   const uint8_t *idx)
{
  uint32_t i;
  for (i = 0; i < vi->lanes && idx[i] == i; i++) {}
  if (i == vi->lanes) return a;
  if (vt & IRT_VEC256) {
    uint32_t nph = 16 / vi->esize;
    for (i = 0; i < vi->lanes &&
	 idx[i] == (uint8_t)((i+nph) % vi->lanes); i++) {}
    if (i == vi->lanes)
      return emitir(IRT(IR_VSHUF, vt), a,
		    IRVSHUF(IRVSHUF_SWAP128, 0));
  }
  {
    uint32_t imm;
    if (crec_simd_shufd(vi, idx, &imm))
      return emitir(IRT(IR_VSHUF, vt), a,
		    IRVSHUF(IRVSHUF_PSHUFD, imm));
  }
  if ((vt & IRT_VEC256) && vi->esize == 4) {
    uint32_t ctl[8], routes = 0;
    for (i = 0; i < vi->lanes; i++) {
      ctl[i] = idx[i];
      routes |= 1u << ((((i*4) ^ ((uint32_t)idx[i]*4)) & 16) != 0);
    }
    /* A same-half constant is one low-latency VPSHUFB. Once any lane crosses
    ** a half, one VPERMD beats the dependent half-swap plus byte shuffle.
    */
    return routes == 1 ?
      crec_simd_shuf1(J, vt, vi, a, idx, 0) :
      emitir(IRT(IR_VPERMD, vt), a,
	     lj_ir_kvec(J, (IRType)(IRT_V4I32|IRT_VEC256), ctl));
  } else if ((vt & IRT_VEC256) && vi->esize == 8) {
    uint32_t routes = 0;
    for (i = 0; i < vi->lanes; i++)
      routes |= 1u << ((((i*8) ^ ((uint32_t)idx[i]*8)) & 16) != 0);
    /* As above: retain one VPSHUFB for a purely same-half constant. */
    if (routes != 1) {
      uint32_t imm = (uint32_t)idx[0] | ((uint32_t)idx[1] << 2) |
		     ((uint32_t)idx[2] << 4) | ((uint32_t)idx[3] << 6);
      return emitir(IRT(IR_VSHUF, vt), a, IRVSHUF(IRVSHUF_PERMQ, imm));
    }
  }
  return crec_simd_shuf1(J, vt, vi, a, idx, 0);
}

/*
** Runtime lane permute: turn a vector of lane indices into the byte control
** vector PSHUFB wants, then do the permute. PSHUFB is byte granular, so a
** lane index has to be scaled to a byte offset and replicated across the
** bytes of its lane, with 0..esize-1 added back.
**
** Masking the index with lanes-1 first keeps every scaled offset within the
** vector. For XMM the offset is below 16; for YMM bit 4 selects the source
** half. The control byte's high bit is always clear, so PSHUFB never takes
** its "write zero" path. Every index is therefore defined without a guard,
** matching lj_simd_permute().
*/
static TRef crec_simd_permute(jit_State *J, IRType vt, const CTVecInfo *vi,
			      TRef a, TRef ix, IRType ivt)
{
  uint8_t rep[LJ_VEC_MAXSIZE], off[LJ_VEC_MAXSIZE];
  uint32_t i, k, esz = vi->esize, n = vi->lanes;
  IRType bvt = (IRType)(IRT_V16I8 | (vt & IRT_VEC256));
  TRef ctrl;
  {  /* Reduce the index modulo the lane count. */
    uint8_t kbuf[LJ_VEC_MAXSIZE];
    uint8_t elem[8];
    uint64_t m = n - 1;
    memset(elem, 0, sizeof(elem));
    memcpy(elem, &m, esz < 8 ? esz : 8);
    lj_simd_splat(kbuf, elem, vi);
    ctrl = emitir(IRT(IR_VAND, ivt), ix, lj_ir_kvec(J, ivt, kbuf));
  }
  if (esz != 1) {
    /* Scale to a byte offset, then spread that byte over the whole lane. */
    ctrl = emitir(IRT(IR_VSHL, ivt), ctrl,
		  lj_ir_kint(J, (int32_t)lj_fls(esz)));
    for (i = 0; i < n; i++)
      for (k = 0; k < esz; k++) {
	rep[i*esz + k] = (uint8_t)(i*esz);  /* Low byte of lane i. */
	off[i*esz + k] = (uint8_t)k;
      }
    ctrl = emitir(IRT(IR_VSHUFB, bvt), ctrl, lj_ir_kvec(J, bvt, rep));
    ctrl = emitir(IRT(IR_VADD, bvt), ctrl, lj_ir_kvec(J, bvt, off));
  }
  if (!(vt & IRT_VEC256))
    return emitir(IRT(IR_VSHUFB, vt), a, ctrl);
  else {
    uint8_t hbuf[LJ_VEC_MAXSIZE], obuf[LJ_VEC_MAXSIZE];
    TRef hbit, out, same, lo, hi, sw;
    memset(hbuf, 0x10, sizeof(hbuf));
    memset(obuf, 0, 16);
    memset(obuf+16, 0x10, 16);
    hbit = emitir(IRT(IR_VAND, bvt), ctrl, lj_ir_kvec(J, bvt, hbuf));
    out = lj_ir_kvec(J, bvt, obuf);
    same = emitir(IRT(IR_VCMPEQ, bvt), hbit, out);
    lo = emitir(IRT(IR_VSHUFB, vt), a, ctrl);
    sw = emitir(IRT(IR_VSHUF, vt), a, IRVSHUF(IRVSHUF_SWAP128, 0));
    hi = emitir(IRT(IR_VSHUFB, vt), sw, ctrl);
    return emitir(IRT(IR_VOR, vt),
		  emitir(IRT(IR_VAND, vt), same, lo),
		  emitir(IRT(IR_VANDN, vt), same, hi));
  }
}

void LJ_FASTCALL recff_ffi_simd_shuffle(jit_State *J, RecordFFData *rd)
{
  CTVecInfo vi; CTypeID id; IRType vt;
  TRef a = crec_simd_arg(J, rd, 0, &vi, &id, &vt);
  uint8_t idx[LJ_VEC_MAXSIZE];
  crec_simd_need(J, (J->flags & JIT_F_SSSE3) != 0);
  if (J->base[1] && tref_iscdata(J->base[1])) {
    CTVecInfo ivi; CTypeID iid; IRType ivt;
    TRef ix = crec_simd_arg(J, rd, 1, &ivi, &iid, &ivt);
    crec_simd_need(J, !veck_isfp(ivi.kind) &&
		      ivi.esize == vi.esize && ivi.lanes == vi.lanes);
    J->base[0] = crec_vec_box(J,
      (vt & IRT_VEC256) && vi.esize == 4 ?
	emitir(IRT(IR_VPERMD, vt), a, ix) :
	crec_simd_permute(J, vt, &vi, a, ix, ivt), vt, id);
    return;
  }
  crec_simd_idx(J, rd, 1, vi.lanes, vi.lanes, idx);
  J->base[0] = crec_vec_box(J,
    crec_simd_constshuffle(J, vt, &vi, a, idx), vt, id);
}

void LJ_FASTCALL recff_ffi_simd_shuffle2(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTVecInfo vi, vi2; CTypeID id, id2; IRType vt, vt2;
  TRef a = crec_simd_arg(J, rd, 0, &vi, &id, &vt);
  TRef b;
  uint8_t idx[LJ_VEC_MAXSIZE];
  IROp op;
  int swap;
  TRef ra, rb;
  b = crec_simd_arg(J, rd, 1, &vi2, &id2, &vt2);
  UNUSED(vi2);
  /* Compare the raw ctypes: an arithmetic result carries the raw id, while a
  ** value built from a typedef carries the typedef id.
  */
  crec_simd_need(J, (J->flags & JIT_F_SSSE3) != 0 && vt == vt2 &&
		    ctype_raw(cts, id) == ctype_raw(cts, id2));
  crec_simd_idx(J, rd, 2, vi.lanes, 2*(uint32_t)vi.lanes, idx);
  if (crec_simd_unpk(&vi, idx, &op, &swap)) {
    J->base[0] = crec_vec_box(J,
      emitir(IRT(op, vt), swap ? b : a, swap ? a : b), vt, id);
    return;
  }
  {
    uint32_t i, n = vi.lanes, use = 0;
    TRef one = 0;
    if (tref_ref(a) == tref_ref(b)) {
      one = a;
      for (i = 0; i < n; i++) idx[i] = (uint8_t)(idx[i] % n);
    } else {
      for (i = 0; i < n; i++) use |= idx[i] < n ? 1u : 2u;
      if (use != 3) {
	one = use == 1 ? a : b;
	if (use == 2)
	  for (i = 0; i < n; i++) idx[i] = (uint8_t)(idx[i] - n);
      }
    }
    if (one) {
      J->base[0] = crec_vec_box(J,
	crec_simd_constshuffle(J, vt, &vi, one, idx), vt, id);
      return;
    }
  }
  ra = crec_simd_shuf2_direct(J, vt, &vi, a, b, idx);
  if (ra) {
    J->base[0] = crec_vec_box(J, ra, vt, id);
    return;
  }
  /* Two permutes, each zeroing the lanes taken from the other vector. */
  ra = crec_simd_shuf1(J, vt, &vi, a, idx, 0);
  rb = crec_simd_shuf1(J, vt, &vi, b, idx, vi.lanes);
  J->base[0] = crec_vec_box(J, emitir(IRT(IR_VOR, vt), ra, rb), vt, id);
}

void LJ_FASTCALL recff_ffi_simd_insert(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTVecInfo vi; CTypeID id; IRType vt;
  TRef a = crec_simd_arg(J, rd, 0, &vi, &id, &vt);
  TRef trlane = J->base[1], trval = J->base[2];
  uint8_t kbuf[LJ_VEC_MAXSIZE];
  int32_t lane;
  TRef splat, kmask, ra;
  CType *sct;
  if (!trlane) lj_trace_err(J, LJ_TRERR_NYIVEC);
  if (!tref_isinteger(trlane)) {
    if (tref_isnum(trlane)) trlane = lj_opt_narrow_toint(J, trlane);
    else lj_trace_err(J, LJ_TRERR_NYIVEC);
  }
  if (!trval || !tref_isnumber(trval)) lj_trace_err(J, LJ_TRERR_NYIVEC);
  sct = ctype_get(cts, tref_isinteger(trval) ? CTID_INT32 : CTID_DOUBLE);
  splat = crec_vec_splat(J, cts, vt, &vi, trval, sct, &rd->argv[2]);
  if (tref_isk(trlane)) {
    uint32_t i;
    lane = IR(tref_ref(trlane))->i;
    if ((uint32_t)lane >= (uint32_t)vi.lanes) lj_trace_err(J, LJ_TRERR_BADTYPE);
    if (!(vt & IRT_VEC256) && IR(tref_ref(splat))->o == IR_VSPLAT &&
	(veck_isfp(vi.kind) ?
	 (vi.esize == 8 || (J->flags & JIT_F_SSE4_1)) :
	 (vi.esize == 2 || ((J->flags & JIT_F_SSE4_1) &&
			    (vi.esize != 8 || LJ_64))))) {
      IRRef sr = IR(tref_ref(splat))->op1;
      ra = crec_simd_shuf2_emit(J, vt, a, TREF(sr, irt_t(IR(sr)->t)),
				IRVSHUF2_INSERT, (uint32_t)lane);
      J->base[0] = crec_vec_box(J, ra, vt, id);
      return;
    }
    /*
    ** An immutable constant-lane insert is a two-input blend: every lane
    ** comes from the original vector except one from the splat. Prefer the
    ** immediate blend forms when the ISA can express that mask directly.
    */
    for (i = 0; i < vi.lanes; i++) kbuf[i] = (uint8_t)i;
    kbuf[lane] = (uint8_t)(vi.lanes + lane);
    ra = crec_simd_shuf2_direct(J, vt, &vi, a, splat, kbuf);
    if (ra) {
      J->base[0] = crec_vec_box(J, ra, vt, id);
      return;
    }
    memset(kbuf, 0, sizeof(kbuf));
    memset(kbuf + (uint32_t)lane * vi.esize, 0xff, vi.esize);
    kmask = lj_ir_kvec(J, vt, kbuf);
  } else {
    /* Variable index: guard the range, then build the lane mask by comparing
    ** a constant vector of lane numbers against the splatted index. Still
    ** fully packed, no memory round trip.
    */
    IRType ivt = (IRType)((vi.esize == 1 ? IRT_V16I8 :
			   vi.esize == 2 ? IRT_V8I16 :
			   vi.esize == 4 ? IRT_V4I32 : IRT_V2I64) |
			  (vt & IRT_VEC256));
    TRef tridx, kidx;
    uint32_t i;
    emitir(IRTGI(IR_ULT), trlane, lj_ir_kint(J, (int32_t)vi.lanes));
    memset(kbuf, 0, sizeof(kbuf));
    for (i = 0; i < vi.lanes; i++) kbuf[i*vi.esize] = (uint8_t)i;
    kidx = lj_ir_kvec(J, ivt, kbuf);
    tridx = vi.esize == 8 ?
	      emitconv(trlane, IRT_I64, IRT_INT, IRCONV_SEXT) : trlane;
    tridx = emitir(IRT(IR_VSPLAT, ivt), tridx, 0);
    kmask = emitir(IRT(IR_VCMPEQ, ivt), kidx, tridx);
  }
  J->base[0] = crec_vec_box(J,
    emitir(IRT(IR_VOR, vt),
	   emitir(IRT(IR_VAND, vt), kmask, splat),
	   emitir(IRT(IR_VANDN, vt), kmask, a)),
    vt, id);
}

void LJ_FASTCALL recff_simd_reduce(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTVecInfo vi; CTypeID id; IRType vt;
  TRef a = crec_simd_arg(J, rd, 0, &vi, &id, &vt);
  uint32_t n = vi.lanes, sz = (uint32_t)vi.esize * vi.lanes;
  IRType rvt = vt;
  int uns = veck_isunsigned(vi.kind);
  int sse41 = (J->flags & JIT_F_SSE4_1) != 0;
  IROp op;
  int mm64 = 0;  /* 64 bit lane min/max needs the compare and blend form. */
  int32_t minposxor = 0;
  TRef r = a;
  switch (rd->data) {
  case VRD_SUM: op = IR_VADD; break;
  case VRD_MIN: case VRD_MAX:
    if (veck_isfp(vi.kind)) {
      op = rd->data == VRD_MIN ? IR_VMIN : IR_VMAX;
    } else if (vi.esize == 8) {
      mm64 = 1;
      op = IR_VMIN;  /* Unused, see the loop below. */
    } else {
      crec_simd_need(J, vi.esize == 2 ? (uns ? sse41 : 1) :
			vi.esize == 1 ? (uns ? 1 : sse41) : sse41);
      op = rd->data == VRD_MIN ? (uns ? IR_VMINU : IR_VMIN)
			       : (uns ? IR_VMAXU : IR_VMAX);
    }
    break;
  default:
    lj_trace_err(J, LJ_TRERR_NYIVEC);
    return;
  }
  if (rd->data == VRD_SUM && vi.esize == 1) {
    IRRef ma, mb;
    if (crec_simd_match_mul_i8(J, a, &ma, &mb)) {
      /*
      ** Pair-dot the even bytes as their containing words, then do the same
      ** after exposing odd bytes in the low half of each word. Every omitted
      ** term is a multiple of 256, so the extracted byte is unchanged.
      */
      IRType wvt = (IRType)(IRT_V8I16 | (vt & IRT_VEC256));
      IRType dvt = (IRType)(IRT_V4I32 | (vt & IRT_VEC256));
      TRef ah = emitir(IRT(IR_VSHR, wvt), ma, lj_ir_kint(J, 8));
      TRef bh = emitir(IRT(IR_VSHR, wvt), mb, lj_ir_kint(J, 8));
      TRef even = emitir(IRT(IR_VPMADW, dvt), ma, mb);
      TRef odd = emitir(IRT(IR_VPMADW, dvt), ah, bh);
      r = emitir(IRT(IR_VADD, dvt), even, odd);
      rvt = dvt;
      n = sz >> 2;
    } else {
      /*
      ** PSADBW against zero sums each group of eight byte bit-patterns into a
      ** qword. The final lane-width narrowing makes this identical for signed
      ** and unsigned byte sums, including wraparound.
      */
      IRType qvt = (IRType)(IRT_V2I64 | (vt & IRT_VEC256));
      uint8_t zbuf[LJ_VEC_MAXSIZE];
      memset(zbuf, 0, sizeof(zbuf));
      r = emitir(IRT(IR_VSADU8, qvt), a, lj_ir_kvec(J, vt, zbuf));
      rvt = qvt;
      n = sz >> 3;
    }
  } else if (rd->data == VRD_SUM && vi.esize == 2 &&
	     IR(tref_ref(a))->o == IR_VMUL) {
    /*
    ** Fuse hsum(a*b) into PMADDWD pair sums. Signed interpretation is also
    ** valid for unsigned inputs: every discrepancy is a multiple of 65536
    ** and disappears when the extracted result is narrowed back to a word.
    */
    IRIns *mul = IR(tref_ref(a));
    IRType dvt = (IRType)(IRT_V4I32 | (vt & IRT_VEC256));
    r = emitir(IRT(IR_VPMADW, dvt), mul->op1, mul->op2);
    rvt = dvt;
    n = sz >> 2;
  } else if ((rd->data == VRD_MIN || rd->data == VRD_MAX) &&
	     vi.esize == 1 && sse41) {
    IRType wvt = (IRType)(IRT_V8I16 | (vt & IRT_VEC256));
    uint8_t zero[LJ_VEC_MAXSIZE], mask[LJ_VEC_MAXSIZE];
    uint8_t xmask = (uint8_t)(uns ?
      (rd->data == VRD_MAX ? 0xffu : 0) :
      (rd->data == VRD_MAX ? 0x7fu : 0x80u));
    TRef z, lo, hi, half;
    if (xmask) {
      memset(mask, xmask, sizeof(mask));
      r = emitir(IRT(IR_VXOR, vt), r, lj_ir_kvec(J, vt, mask));
      minposxor = (int32_t)xmask;
    }
    memset(zero, 0, sizeof(zero));
    z = lj_ir_kvec(J, vt, zero);
    lo = emitir(IRT(IR_VUNPKL, vt), r, z);
    hi = emitir(IRT(IR_VUNPKH, vt), r, z);
    r = emitir(IRT(IR_VMINU, wvt), lo, hi);
    if (vt & IRT_VEC256) {
      half = emitir(IRT(IR_VSHUF, wvt), r,
		    IRVSHUF(IRVSHUF_SWAP128, 0));
      r = emitir(IRT(IR_VMINU, wvt), r, half);
    }
    r = emitir(IRT(IR_VHMINPOSU16, wvt), r, 0);
    rvt = wvt;
    n = 1;
  } else if ((rd->data == VRD_MIN || rd->data == VRD_MAX) &&
	     vi.esize == 2 && sse41) {
    TRef half;
    uint16_t xmask = uns ? (rd->data == VRD_MAX ? 0xffffu : 0) :
			   (rd->data == VRD_MAX ? 0x7fffu : 0x8000u);
    if (xmask) {
      uint8_t mask[LJ_VEC_MAXSIZE];
      uint32_t i;
      for (i = 0; i < sizeof(mask); i += 2) {
	mask[i] = (uint8_t)xmask;
	mask[i+1] = (uint8_t)(xmask >> 8);
      }
      r = emitir(IRT(IR_VXOR, vt), r, lj_ir_kvec(J, vt, mask));
      minposxor = (int32_t)xmask;
    }
    if (vt & IRT_VEC256) {
      half = emitir(IRT(IR_VSHUF, vt), r,
		    IRVSHUF(IRVSHUF_SWAP128, 0));
      r = emitir(IRT(IR_VMINU, vt), r, half);
    }
    r = emitir(IRT(IR_VHMINPOSU16, vt), r, 0);
    n = 1;
  }
  /* The same pairwise halving tree the interpreter uses: shift the vector
  ** right by half its remaining width and combine.
  */
  while (n > 1) {
    TRef half;
    n >>= 1;
    sz >>= 1;
    half = emitir(IRT(IR_VSHUF, rvt), r,
      IRVSHUF((rvt & IRT_VEC256) && sz == 16 ?
	      IRVSHUF_SWAP128 : IRVSHUF_PSRLDQ, sz));
    r = mm64 ? crec_simd_minmax64(J, vt, &vi, r, half, rd->data == VRD_MAX)
	     : emitir(IRT(op, rvt), r, half);
  }
  {
    IRType et = veck_isfp(vi.kind) ?
		  (vi.kind == VECK_F32 ? IRT_FLOAT : IRT_NUM) :
		  (vi.esize == 8 ? (uns ? IRT_U64 : IRT_I64) : IRT_INT);
    TRef res = emitir(IRT(IR_VEXTRACT, et), r, IRVSRC(vt, 0));
    CType *ect = ctype_get(cts, vi.eid);
    if (minposxor)
      res = emitir(IRT(IR_BXOR, IRT_INT), res,
		   lj_ir_kint(J, minposxor));
    if (et == IRT_FLOAT) {
      J->base[0] = emitconv(res, IRT_NUM, IRT_FLOAT, 0);
    } else if (et == IRT_NUM) {
      J->base[0] = res;
    } else if (et == IRT_INT) {
      IRType st = crec_ct2irt(cts, ect);
      if (st == IRT_U32) {
	J->base[0] = emitconv(res, IRT_NUM, IRT_U32, 0);
      } else if (st != IRT_INT) {  /* Narrow to the lane width. */
	J->base[0] = emitconv(res, IRT_INT, st, 0);
      } else {
	J->base[0] = res;
      }
    } else {
      lj_needsplit(J);
      J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA),
			  lj_ir_kint(J, (int32_t)vi.eid), res);
    }
  }
}

static TRef crec_arith_int64(jit_State *J, TRef *sp, CType **s, MMS mm)
{
  if (sp[0] && sp[1] && ctype_isnum(s[0]->info) && ctype_isnum(s[1]->info)) {
    IRType dt;
    CTypeID id;
    TRef tr;
    MSize i;
    IROp op;
    lj_needsplit(J);
    if (((s[0]->info & CTF_UNSIGNED) && s[0]->size == 8) ||
	((s[1]->info & CTF_UNSIGNED) && s[1]->size == 8)) {
      dt = IRT_U64; id = CTID_UINT64;
    } else {
      dt = IRT_I64; id = CTID_INT64;
      if (mm < MM_add &&
	  !((s[0]->info | s[1]->info) & CTF_FP) &&
	  s[0]->size == 4 && s[1]->size == 4) {  /* Try to narrow comparison. */
	if (!((s[0]->info ^ s[1]->info) & CTF_UNSIGNED) ||
	    (tref_isk(sp[1]) && IR(tref_ref(sp[1]))->i >= 0)) {
	  dt = (s[0]->info & CTF_UNSIGNED) ? IRT_U32 : IRT_INT;
	  goto comp;
	} else if (tref_isk(sp[0]) && IR(tref_ref(sp[0]))->i >= 0) {
	  dt = (s[1]->info & CTF_UNSIGNED) ? IRT_U32 : IRT_INT;
	  goto comp;
	}
      }
    }
    for (i = 0; i < 2; i++) {
      IRType st = tref_type(sp[i]);
      if (st == IRT_NUM || st == IRT_FLOAT)
	sp[i] = emitconv(sp[i], dt, st, IRCONV_ANY);
      else if (!(st == IRT_I64 || st == IRT_U64))
	sp[i] = emitconv(sp[i], dt, IRT_INT,
			 ((st - IRT_I8) & 1) ? 0 : IRCONV_SEXT);
    }
    if (mm < MM_add) {
    comp:
      /* Assume true comparison. Fixup and emit pending guard later. */
      if (mm == MM_eq) {
	op = IR_EQ;
      } else {
	op = mm == MM_lt ? IR_LT : IR_LE;
	if (dt == IRT_U32 || dt == IRT_U64)
	  op += (IR_ULT-IR_LT);
      }
      lj_ir_set(J, IRTG(op, dt), sp[0], sp[1]);
      J->postproc = LJ_POST_FIXGUARD;
      return TREF_TRUE;
    } else {
      tr = emitir(IRT(mm+(int)IR_ADD-(int)MM_add, dt), sp[0], sp[1]);
    }
    return emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, id), tr);
  }
  return 0;
}

static TRef crec_arith_ptr(jit_State *J, TRef *sp, CType **s, MMS mm)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CType *ctp = s[0];
  if (!(sp[0] && sp[1])) return 0;
  if (ctype_isptr(ctp->info) || ctype_isrefarray(ctp->info)) {
    if ((mm == MM_sub || mm == MM_eq || mm == MM_lt || mm == MM_le) &&
	(ctype_isptr(s[1]->info) || ctype_isrefarray(s[1]->info))) {
      if (mm == MM_sub) {  /* Pointer difference. */
	TRef tr;
	CTSize sz = lj_ctype_size(cts, ctype_cid(ctp->info));
	if (sz == 0 || (sz & (sz-1)) != 0)
	  return 0;  /* NYI: integer division. */
	tr = emitir(IRT(IR_SUB, IRT_INTP), sp[0], sp[1]);
	tr = emitir(IRT(IR_BSAR, IRT_INTP), tr, lj_ir_kint(J, lj_fls(sz)));
#if LJ_64
	tr = emitconv(tr, IRT_NUM, IRT_INTP, 0);
#endif
	return tr;
      } else {  /* Pointer comparison (unsigned). */
	/* Assume true comparison. Fixup and emit pending guard later. */
	IROp op = mm == MM_eq ? IR_EQ : mm == MM_lt ? IR_ULT : IR_ULE;
	lj_ir_set(J, IRTG(op, IRT_PTR), sp[0], sp[1]);
	J->postproc = LJ_POST_FIXGUARD;
	return TREF_TRUE;
      }
    }
    if (!((mm == MM_add || mm == MM_sub) && ctype_isnum(s[1]->info)))
      return 0;
  } else if (mm == MM_add && ctype_isnum(ctp->info) &&
	     (ctype_isptr(s[1]->info) || ctype_isrefarray(s[1]->info))) {
    TRef tr = sp[0]; sp[0] = sp[1]; sp[1] = tr;  /* Swap pointer and index. */
    ctp = s[1];
  } else {
    return 0;
  }
  {
    TRef tr = sp[1];
    IRType t = tref_type(tr);
    CTSize sz = lj_ctype_size(cts, ctype_cid(ctp->info));
    CTypeID id;
#if LJ_64
    if (t == IRT_NUM || t == IRT_FLOAT)
      tr = emitconv(tr, IRT_INTP, t, IRCONV_ANY);
    else if (!(t == IRT_I64 || t == IRT_U64))
      tr = emitconv(tr, IRT_INTP, IRT_INT,
		    ((t - IRT_I8) & 1) ? 0 : IRCONV_SEXT);
#else
    if (!tref_typerange(sp[1], IRT_I8, IRT_U32)) {
      tr = emitconv(tr, IRT_INTP, t,
		    (t == IRT_NUM || t == IRT_FLOAT) ? IRCONV_ANY : 0);
    }
#endif
    tr = emitir(IRT(IR_MUL, IRT_INTP), tr, lj_ir_kintp(J, sz));
    tr = emitir(IRT(mm+(int)IR_ADD-(int)MM_add, IRT_PTR), sp[0], tr);
    id = lj_ctype_intern(cts, CTINFO(CT_PTR, CTALIGN_PTR|ctype_cid(ctp->info)),
			 CTSIZE_PTR);
    return emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, id), tr);
  }
}

/* Record ctype arithmetic metamethods. */
static TRef crec_arith_meta(jit_State *J, TRef *sp, CType **s, CTState *cts,
			    RecordFFData *rd)
{
  cTValue *tv = NULL;
  if (J->base[0]) {
    if (tviscdata(&rd->argv[0])) {
      CTypeID id = argv2cdata(J, J->base[0], &rd->argv[0])->ctypeid;
      CType *ct = ctype_raw(cts, id);
      if (ctype_isptr(ct->info)) id = ctype_cid(ct->info);
      tv = lj_ctype_meta(cts, id, (MMS)rd->data);
    }
    if (!tv && J->base[1] && tviscdata(&rd->argv[1])) {
      CTypeID id = argv2cdata(J, J->base[1], &rd->argv[1])->ctypeid;
      CType *ct = ctype_raw(cts, id);
      if (ctype_isptr(ct->info)) id = ctype_cid(ct->info);
      tv = lj_ctype_meta(cts, id, (MMS)rd->data);
    }
  }
  if (tv) {
    if (tvisfunc(tv)) {
      crec_tailcall(J, rd, tv);
      return 0;
    }  /* NYI: non-function metamethods. */
  } else if ((MMS)rd->data == MM_eq) {  /* Fallback cdata pointer comparison. */
    if (sp[0] && sp[1] && ctype_isnum(s[0]->info) == ctype_isnum(s[1]->info)) {
      /* Assume true comparison. Fixup and emit pending guard later. */
      lj_ir_set(J, IRTG(IR_EQ, IRT_PTR), sp[0], sp[1]);
      J->postproc = LJ_POST_FIXGUARD;
      return TREF_TRUE;
    } else {
      return TREF_FALSE;
    }
  }
  lj_trace_err(J, LJ_TRERR_BADTYPE);
  return 0;
}

void LJ_FASTCALL recff_cdata_arith(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_cts(J->L);
  MMS mm = (MMS)rd->data;
  TRef sp[2];
  CType *s[2];
  MSize i;
  for (i = 0; i < 2; i++) {
    TRef tr = J->base[i];
    CType *ct = ctype_get(cts, CTID_DOUBLE);
    if (!tr) {
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    } else if (tref_iscdata(tr)) {
      CTypeID id = argv2cdata(J, tr, &rd->argv[i])->ctypeid;
      IRType t;
      ct = ctype_raw(cts, id);
      t = crec_ct2irt(cts, ct);
      if (ctype_isptr(ct->info)) {  /* Resolve pointer or reference. */
	tr = emitir(IRT(IR_FLOAD, t), tr, IRFL_CDATA_PTR);
	if (ctype_isref(ct->info)) {
	  ct = ctype_rawchild(cts, ct);
	  t = crec_ct2irt(cts, ct);
	}
      } else if (t == IRT_I64 || t == IRT_U64) {
	tr = emitir(IRT(IR_FLOAD, t), tr, IRFL_CDATA_INT64);
	lj_needsplit(J);
	goto ok;
      } else if (t == IRT_INT || t == IRT_U32) {
	tr = emitir(IRT(IR_FLOAD, t), tr, IRFL_CDATA_INT);
	if (ctype_isenum(ct->info)) ct = ctype_child(cts, ct);
	goto ok;
      } else if (ctype_isfunc(ct->info)) {
	CTypeID id0 = i ? ctype_typeid(cts, s[0]) : 0;
	tr = emitir(IRT(IR_FLOAD, IRT_PTR), tr, IRFL_CDATA_PTR);
	ct = ctype_get(cts,
	  lj_ctype_intern(cts, CTINFO(CT_PTR, CTALIGN_PTR|id), CTSIZE_PTR));
	if (i) {
	  s[0] = ctype_get(cts, id0);  /* cts->tab may have been reallocated. */
	}
	goto ok;
      } else if (ctype_isvector(ct->info)) {
	IRType vt = crec_vec2irt(cts, ct);
	if (vt == IRT_NIL) lj_trace_err(J, LJ_TRERR_NYIVEC);
	tr = emitir(IRT(IR_ADD, IRT_PTR), tr, lj_ir_kintp(J, sizeof(GCcdata)));
	tr = emitir(IRT(IR_XLOAD, vt), tr, 0);
	goto ok;
      } else {
	tr = emitir(IRT(IR_ADD, IRT_PTR), tr, lj_ir_kintp(J, sizeof(GCcdata)));
      }
      if (ctype_isenum(ct->info)) ct = ctype_child(cts, ct);
      if (ctype_isnum(ct->info)) {
	if (t == IRT_CDATA) {
	  tr = 0;
	} else {
	  if (t == IRT_I64 || t == IRT_U64) lj_needsplit(J);
	  tr = emitir(IRT(IR_XLOAD, t), tr, 0);
	}
      }
    } else if (tref_isnil(tr)) {
      if (!(mm == MM_len || mm == MM_eq || mm == MM_lt || mm == MM_le))
	lj_trace_err(J, LJ_TRERR_BADTYPE);
      tr = lj_ir_kptr(J, NULL);
      ct = ctype_get(cts, CTID_P_VOID);
    } else if (tref_isinteger(tr)) {
      ct = ctype_get(cts, CTID_INT32);
    } else if (tref_isstr(tr)) {
      TRef tr2 = J->base[1-i];
      CTypeID id = argv2cdata(J, tr2, &rd->argv[1-i])->ctypeid;
      ct = ctype_raw(cts, id);
      if (ctype_isenum(ct->info)) {  /* Match string against enum constant. */
	GCstr *str = strV(&rd->argv[i]);
	CTSize ofs;
	CType *cct = lj_ctype_getfield(cts, ct, str, &ofs);
	if (cct && ctype_isconstval(cct->info)) {
	  /* Specialize to the name of the enum constant. */
	  emitir(IRTG(IR_EQ, IRT_STR), tr, lj_ir_kstr(J, str));
	  ct = ctype_child(cts, cct);
	  tr = lj_ir_kint(J, (int32_t)ofs);
	} else {  /* Interpreter will throw or return false. */
	  lj_trace_err(J, LJ_TRERR_BADTYPE);
	}
      } else if (ctype_isptr(ct->info)) {
	tr = emitir(IRT(IR_ADD, IRT_PTR), tr, lj_ir_kintp(J, sizeof(GCstr)));
      } else {
	lj_trace_err(J, LJ_TRERR_BADTYPE);
      }
    } else if (!tref_isnum(tr)) {
      tr = 0;
      ct = ctype_get(cts, CTID_P_VOID);
    }
  ok:
    s[i] = ct;
    sp[i] = tr;
  }
  {
    TRef tr;
    if ((mm == MM_len || mm == MM_concat ||
	 (!(tr = crec_arith_vec(J, sp, s, mm, rd)) &&
	  !(tr = crec_arith_int64(J, sp, s, mm)) &&
	  !(tr = crec_arith_ptr(J, sp, s, mm)))) &&
	!(tr = crec_arith_meta(J, sp, s, cts, rd)))
      return;
    J->base[0] = tr;
    /* Fixup cdata comparisons, too. Avoids some cdata escapes. */
    if (J->postproc == LJ_POST_FIXGUARD && frame_iscont(J->L->base-1) &&
	!irt_isguard(J->guardemit)) {
      const BCIns *pc = frame_contpc(J->L->base-1) - 1;
      if (bc_op(*pc) <= BC_ISNEP) {
	J2G(J)->tmptv.u64 = (uint64_t)(uintptr_t)pc;
	J->postproc = LJ_POST_FIXCOMP;
      }
    }
  }
}

/* -- C library namespace metamethods ------------------------------------- */

void LJ_FASTCALL recff_clib_index(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  if (tref_isudata(J->base[0]) && tref_isstr(J->base[1]) &&
      udataV(&rd->argv[0])->udtype == UDTYPE_FFI_CLIB) {
    CLibrary *cl = (CLibrary *)uddata(udataV(&rd->argv[0]));
    GCstr *name = strV(&rd->argv[1]);
    CType *ct;
    CTypeID id = lj_ctype_getname(cts, &ct, name, CLNS_INDEX);
    cTValue *tv = lj_tab_getstr(cl->cache, name);
    rd->nres = rd->data;
    if (id && tv && !tvisnil(tv)) {
      /* Specialize to the symbol name and make the result a constant. */
      emitir(IRTG(IR_EQ, IRT_STR), J->base[1], lj_ir_kstr(J, name));
      if (ctype_isconstval(ct->info)) {
	if (ct->size >= 0x80000000u &&
	    (ctype_child(cts, ct)->info & CTF_UNSIGNED))
	  J->base[0] = lj_ir_knum(J, (lua_Number)(uint32_t)ct->size);
	else
	  J->base[0] = lj_ir_kint(J, (int32_t)ct->size);
      } else if (ctype_isextern(ct->info)) {
	CTypeID sid = ctype_cid(ct->info);
	void *sp = *(void **)cdataptr(cdataV(tv));
	TRef ptr;
	ct = ctype_raw(cts, sid);
	if (LJ_64 && !checkptr32(sp))
	  ptr = lj_ir_kintp(J, (uintptr_t)sp);
	else
	  ptr = lj_ir_kptr(J, sp);
	if (rd->data) {
	  J->base[0] = crec_tv_ct(J, ct, sid, ptr);
	} else {
	  J->needsnap = 1;
	  crec_ct_tv(J, ct, ptr, J->base[2], &rd->argv[2]);
	}
      } else {
	J->base[0] = lj_ir_kgc(J, obj2gco(cdataV(tv)), IRT_CDATA);
      }
    } else {
      lj_trace_err(J, LJ_TRERR_NOCACHE);
    }
  }  /* else: interpreter will throw. */
}

/* -- FFI library functions ----------------------------------------------- */

static TRef crec_toint(jit_State *J, CTState *cts, TRef sp, TValue *sval)
{
  return crec_ct_tv(J, ctype_get(cts, CTID_INT32), 0, sp, sval);
}

void LJ_FASTCALL recff_ffi_new(jit_State *J, RecordFFData *rd)
{
  crec_alloc(J, rd, argv2ctype(J, J->base[0], &rd->argv[0]));
}

void LJ_FASTCALL recff_ffi_errno(jit_State *J, RecordFFData *rd)
{
  UNUSED(rd);
  if (J->base[0])
    lj_trace_err(J, LJ_TRERR_NYICALL);
  J->base[0] = lj_ir_call(J, IRCALL_lj_vm_errno);
}

void LJ_FASTCALL recff_ffi_string(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  TRef tr = J->base[0];
  if (tr) {
    TRef trlen = J->base[1];
    if (!tref_isnil(trlen)) {
      trlen = crec_toint(J, cts, trlen, &rd->argv[1]);
      tr = crec_ct_tv(J, ctype_get(cts, CTID_P_CVOID), 0, tr, &rd->argv[0]);
    } else {
      tr = crec_ct_tv(J, ctype_get(cts, CTID_P_CCHAR), 0, tr, &rd->argv[0]);
      trlen = lj_ir_call(J, IRCALL_strlen, tr);
    }
    J->base[0] = emitir(IRT(IR_XSNEW, IRT_STR), tr, trlen);
  }  /* else: interpreter will throw. */
}

void LJ_FASTCALL recff_ffi_copy(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  TRef trdst = J->base[0], trsrc = J->base[1], trlen = J->base[2];
  if (trdst && trsrc && (trlen || tref_isstr(trsrc))) {
    trdst = crec_ct_tv(J, ctype_get(cts, CTID_P_VOID), 0, trdst, &rd->argv[0]);
    trsrc = crec_ct_tv(J, ctype_get(cts, CTID_P_CVOID), 0, trsrc, &rd->argv[1]);
    if (trlen) {
      trlen = crec_toint(J, cts, trlen, &rd->argv[2]);
    } else {
      trlen = emitir(IRTI(IR_FLOAD), J->base[1], IRFL_STR_LEN);
      trlen = emitir(IRTI(IR_ADD), trlen, lj_ir_kint(J, 1));
    }
    rd->nres = 0;
    crec_copy(J, trdst, trsrc, trlen, NULL);
  }  /* else: interpreter will throw. */
}

void LJ_FASTCALL recff_ffi_fill(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  TRef trdst = J->base[0], trlen = J->base[1], trfill = J->base[2];
  if (trdst && trlen) {
    CTSize step = 1;
    if (tviscdata(&rd->argv[0])) {  /* Get alignment of original destination. */
      CTSize sz;
      CType *ct = ctype_raw(cts, cdataV(&rd->argv[0])->ctypeid);
      if (ctype_isptr(ct->info))
	ct = ctype_rawchild(cts, ct);
      step = (1u<<ctype_align(lj_ctype_info(cts, ctype_typeid(cts, ct), &sz)));
    }
    trdst = crec_ct_tv(J, ctype_get(cts, CTID_P_VOID), 0, trdst, &rd->argv[0]);
    trlen = crec_toint(J, cts, trlen, &rd->argv[1]);
    if (trfill)
      trfill = crec_toint(J, cts, trfill, &rd->argv[2]);
    else
      trfill = lj_ir_kint(J, 0);
    rd->nres = 0;
    crec_fill(J, trdst, trlen, trfill, step);
  }  /* else: interpreter will throw. */
}

void LJ_FASTCALL recff_ffi_typeof(jit_State *J, RecordFFData *rd)
{
  if (tref_iscdata(J->base[0])) {
    TRef trid = lj_ir_kint(J, argv2ctype(J, J->base[0], &rd->argv[0]));
    J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA),
			lj_ir_kint(J, CTID_CTYPEID), trid);
  } else {
    setfuncV(J->L, &J->errinfo, J->fn);
    lj_trace_err_info(J, LJ_TRERR_NYIFFU);
  }
}

void LJ_FASTCALL recff_ffi_istype(jit_State *J, RecordFFData *rd)
{
  argv2ctype(J, J->base[0], &rd->argv[0]);
  if (tref_iscdata(J->base[1])) {
    argv2ctype(J, J->base[1], &rd->argv[1]);
    J->postproc = LJ_POST_FIXBOOL;
    J->base[0] = TREF_TRUE;
  } else {
    J->base[0] = TREF_FALSE;
  }
}

void LJ_FASTCALL recff_ffi_abi(jit_State *J, RecordFFData *rd)
{
  if (tref_isstr(J->base[0])) {
    /* Specialize to the ABI string to make the boolean result a constant. */
    emitir(IRTG(IR_EQ, IRT_STR), J->base[0], lj_ir_kstr(J, strV(&rd->argv[0])));
    J->postproc = LJ_POST_FIXBOOL;
    J->base[0] = TREF_TRUE;
  } else {
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  }
}

/* Record ffi.sizeof(), ffi.alignof(), ffi.offsetof(). */
void LJ_FASTCALL recff_ffi_xof(jit_State *J, RecordFFData *rd)
{
  CTypeID id = argv2ctype(J, J->base[0], &rd->argv[0]);
  if (rd->data == FF_ffi_sizeof) {
    CType *ct = lj_ctype_rawref(ctype_ctsG(J2G(J)), id);
    if (ctype_isvltype(ct->info))
      lj_trace_err(J, LJ_TRERR_BADTYPE);
  } else if (rd->data == FF_ffi_offsetof) {  /* Specialize to the field name. */
    if (!tref_isstr(J->base[1]))
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    emitir(IRTG(IR_EQ, IRT_STR), J->base[1], lj_ir_kstr(J, strV(&rd->argv[1])));
    rd->nres = 3;  /* Just in case. */
  }
  J->postproc = LJ_POST_FIXCONST;
  J->base[0] = J->base[1] = J->base[2] = TREF_NIL;
}

void LJ_FASTCALL recff_ffi_gc(jit_State *J, RecordFFData *rd)
{
  argv2cdata(J, J->base[0], &rd->argv[0]);
  if (!J->base[1])
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  crec_finalizer(J, J->base[0], J->base[1], &rd->argv[1]);
}

/* -- 64 bit bit.* library functions -------------------------------------- */

/* Determine bit operation type from argument type. */
static CTypeID crec_bit64_type(CTState *cts, cTValue *tv)
{
  if (tviscdata(tv)) {
    CType *ct = lj_ctype_rawref(cts, cdataV(tv)->ctypeid);
    if (ctype_isenum(ct->info)) ct = ctype_child(cts, ct);
    if ((ct->info & (CTMASK_NUM|CTF_BOOL|CTF_FP|CTF_UNSIGNED)) ==
	CTINFO(CT_NUM, CTF_UNSIGNED) && ct->size == 8)
      return CTID_UINT64;  /* Use uint64_t, since it has the highest rank. */
    return CTID_INT64;  /* Otherwise use int64_t. */
  }
  return 0;  /* Use regular 32 bit ops. */
}

static TRef crec_bit64_arg(jit_State *J, CType *d, TRef sp, TValue *sval)
{
  if (LJ_UNLIKELY(tref_isstr(sp))) {
    if (lj_strscan_num(strV(sval), sval)) {
      sp = emitir(IRTG(IR_STRTO, IRT_NUM), sp, 0);
    }  /* else: interpreter will throw. */
  }
  return crec_ct_tv(J, d, 0, sp, sval);
}

void LJ_FASTCALL recff_bit64_tobit(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  TRef tr = crec_bit64_arg(J, ctype_get(cts, CTID_INT64),
			   J->base[0], &rd->argv[0]);
  if (!tref_isinteger(tr))
    tr = emitconv(tr, IRT_INT, tref_type(tr), 0);
  J->base[0] = tr;
}

int LJ_FASTCALL recff_bit64_unary(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTypeID id = crec_bit64_type(cts, &rd->argv[0]);
  if (id) {
    TRef tr = crec_bit64_arg(J, ctype_get(cts, id), J->base[0], &rd->argv[0]);
    tr = emitir(IRT(rd->data, id-CTID_INT64+IRT_I64), tr, 0);
    J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, id), tr);
    return 1;
  }
  return 0;
}

int LJ_FASTCALL recff_bit64_nary(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTypeID id = 0;
  MSize i;
  for (i = 0; J->base[i] != 0; i++) {
    CTypeID aid = crec_bit64_type(cts, &rd->argv[i]);
    if (id < aid) id = aid;  /* Determine highest type rank of all arguments. */
  }
  if (id) {
    CType *ct = ctype_get(cts, id);
    uint32_t ot = IRT(rd->data, id-CTID_INT64+IRT_I64);
    TRef tr = crec_bit64_arg(J, ct, J->base[0], &rd->argv[0]);
    for (i = 1; J->base[i] != 0; i++) {
      TRef tr2 = crec_bit64_arg(J, ct, J->base[i], &rd->argv[i]);
      tr = emitir(ot, tr, tr2);
    }
    J->base[0] = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, id), tr);
    return 1;
  }
  return 0;
}

int recff_bit64_shift(jit_State *J, TRef *rb, TRef *rc,
		      TValue *rbv, TValue *rcv, IROp op)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTypeID id;
  TRef tsh = 0;
  if (*rb && tref_iscdata(*rc)) {
    tsh = crec_bit64_arg(J, ctype_get(cts, CTID_INT64), *rc, rcv);
    if (LJ_32 && !tref_isinteger(tsh))
      tsh = emitconv(tsh, IRT_INT, tref_type(tsh), 0);
    *rc = tsh;
  }
  id = crec_bit64_type(cts, rbv);
  if (id) {
    TRef tr = crec_bit64_arg(J, ctype_get(cts, id), *rb, rbv);
    IRType t;
    if (!tsh) tsh = lj_opt_narrow_tobit(J, *rc);
    t = tref_isinteger(tsh) ? IRT_INT : tref_type(tsh);
    if (!(op < IR_BROL ? LJ_TARGET_MASKSHIFT : LJ_TARGET_MASKROT) &&
	!tref_isk(tsh))
      tsh = emitir(IRT(IR_BAND, t), tsh, lj_ir_kint(J, 63));
#ifdef LJ_TARGET_UNIFYROT
    if (op == (LJ_TARGET_UNIFYROT == 1 ? IR_BROR : IR_BROL)) {
      op = LJ_TARGET_UNIFYROT == 1 ? IR_BROL : IR_BROR;
      tsh = emitir(IRT(IR_NEG, t), tsh, tsh);
    }
#endif
    tr = emitir(IRT(op, id-CTID_INT64+IRT_I64), tr, tsh);
    *rb = emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, id), tr);
    return 1;
  }
  return 0;
}

TRef recff_bit64_tohex(jit_State *J, RecordFFData *rd, TRef hdr)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTypeID id = crec_bit64_type(cts, &rd->argv[0]);
  TRef tr, trsf = J->base[1];
  SFormat sf = (STRFMT_UINT|STRFMT_T_HEX);
  int32_t n;
  if (trsf) {
    CTypeID id2 = 0;
    n = (int32_t)lj_carith_check64(J->L, 2, &id2);
    if (id2)
      trsf = crec_bit64_arg(J, ctype_get(cts, CTID_INT32), trsf, &rd->argv[1]);
    else
      trsf = lj_opt_narrow_tobit(J, trsf);
    emitir(IRTGI(IR_EQ), trsf, lj_ir_kint(J, n));  /* Specialize to n. */
  } else {
    n = id ? 16 : 8;
  }
  if (n < 0) { n = (int32_t)(~n+1u); sf |= STRFMT_F_UPPER; }
  if ((uint32_t)n > 254) n = 254;
  sf |= ((SFormat)((n+1)&255) << STRFMT_SH_PREC);
  if (id) {
    tr = crec_bit64_arg(J, ctype_get(cts, id), J->base[0], &rd->argv[0]);
    if (n < 16)
      tr = emitir(IRT(IR_BAND, IRT_U64), tr,
		  lj_ir_kint64(J, ((uint64_t)1 << 4*n)-1));
  } else {
    tr = lj_opt_narrow_tobit(J, J->base[0]);
    if (n < 8)
      tr = emitir(IRTI(IR_BAND), tr, lj_ir_kint(J, (int32_t)((1u << 4*n)-1)));
    tr = emitconv(tr, IRT_U64, IRT_INT, 0);  /* No sign-extension. */
    lj_needsplit(J);
  }
  return lj_ir_call(J, IRCALL_lj_strfmt_putfxint, hdr, lj_ir_kint(J, sf), tr);
}

TRef recff_bit64_bitop(jit_State *J, TRef rb, TRef rc,
		       TValue *rbv, TValue *rcv, IROp op)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CTypeID id = crec_bit64_type(cts, rbv);
  CTypeID id2 = rcv ? crec_bit64_type(cts, rcv) : 0;
  CType *ct;
  TRef tr, tr2;
  if (id < id2) id = id2;
  ct = ctype_get(cts, id);
  tr = crec_bit64_arg(J, ct, rb, rbv);
  tr2 = rcv ? crec_bit64_arg(J, ct, rc, rcv) : 0;
  tr = emitir(IRT(op, id-CTID_INT64+IRT_I64), tr, tr2);
  return emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, id), tr);
}

/* -- Miscellaneous library functions ------------------------------------- */

void LJ_FASTCALL lj_crecord_tonumber(jit_State *J, RecordFFData *rd)
{
  CTState *cts = ctype_ctsG(J2G(J));
  CType *d, *ct = lj_ctype_rawref(cts, cdataV(&rd->argv[0])->ctypeid);
  if (ctype_isenum(ct->info)) ct = ctype_child(cts, ct);
  if (ctype_isnum(ct->info) || ctype_iscomplex(ct->info)) {
    if (ctype_isinteger_or_bool(ct->info) && ct->size <= 4 &&
	!(ct->size == 4 && (ct->info & CTF_UNSIGNED)))
      d = ctype_get(cts, CTID_INT32);
    else
      d = ctype_get(cts, CTID_DOUBLE);
    J->base[0] = crec_ct_tv(J, d, 0, J->base[0], &rd->argv[0]);
  } else {
    /* Specialize to the ctype that couldn't be converted. */
    argv2cdata(J, J->base[0], &rd->argv[0]);
    J->base[0] = TREF_NIL;
  }
}

TRef lj_crecord_loadiu64(jit_State *J, TRef tr, cTValue *o)
{
  CTypeID id = argv2cdata(J, tr, o)->ctypeid;
  if (!(id == CTID_INT64 || id == CTID_UINT64))
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  lj_needsplit(J);
  return emitir(IRT(IR_FLOAD, id == CTID_INT64 ? IRT_I64 : IRT_U64), tr,
		IRFL_CDATA_INT64);
}

#if LJ_HASBUFFER
TRef lj_crecord_topcvoid(jit_State *J, TRef tr, cTValue *o)
{
  CTState *cts = ctype_ctsG(J2G(J));
  if (!tref_iscdata(tr)) lj_trace_err(J, LJ_TRERR_BADTYPE);
  return crec_ct_tv(J, ctype_get(cts, CTID_P_CVOID), 0, tr, o);
}

TRef lj_crecord_topuint8(jit_State *J, TRef tr)
{
  return emitir(IRTG(IR_CNEWI, IRT_CDATA), lj_ir_kint(J, CTID_P_UINT8), tr);
}
#endif

#undef IR
#undef emitir
#undef emitconv

#endif
