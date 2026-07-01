/*
** FFI C call handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <string.h>

#include "lj_obj.h"

#if LJ_HASFFI

#include "lj_gc.h"
#include "lj_err.h"
#include "lj_tab.h"
#include "lj_ctype.h"
#include "lj_cconv.h"
#include "lj_cdata.h"
#include "lj_ccall.h"
#include "lj_safepoint.h"
#include "lj_trace.h"
#include "lj_tg.h"

/* Target-specific handling of register arguments. */
#if LJ_TARGET_X86
/* -- x86 calling conventions --------------------------------------------- */

#define CCALL_PUSH(arg) \
  *(GPRArg *)((uint8_t *)cc->stack + nsp) = (GPRArg)(arg), nsp += CTSIZE_PTR

#if LJ_ABI_WIN

#define CCALL_HANDLE_STRUCTRET \
  /* Return structs bigger than 8 by reference (on stack only). */ \
  cc->retref = (sz > 8); \
  if (cc->retref) CCALL_PUSH(dp);

#define CCALL_HANDLE_COMPLEXRET CCALL_HANDLE_STRUCTRET

#else

#if LJ_TARGET_OSX

#define CCALL_HANDLE_STRUCTRET \
  /* Return structs of size 1, 2, 4 or 8 in registers. */ \
  cc->retref = !(sz == 1 || sz == 2 || sz == 4 || sz == 8); \
  if (cc->retref) { \
    if (ngpr < maxgpr) \
      cc->gpr[ngpr++] = (GPRArg)dp; \
    else \
      CCALL_PUSH(dp); \
  } else {  /* Struct with single FP field ends up in FPR. */ \
    cc->resx87 = ccall_classify_struct(cts, ctr); \
  }

#define CCALL_HANDLE_STRUCTRET2 \
  if (cc->resx87) sp = (uint8_t *)&cc->fpr[0]; \
  memcpy(dp, sp, ctr->size);

#else

#define CCALL_HANDLE_STRUCTRET \
  cc->retref = 1;  /* Return all structs by reference (in reg or on stack). */ \
  if (ngpr < maxgpr) \
    cc->gpr[ngpr++] = (GPRArg)dp; \
  else \
    CCALL_PUSH(dp);

#endif

#define CCALL_HANDLE_COMPLEXRET \
  /* Return complex float in GPRs and complex double by reference. */ \
  cc->retref = (sz > 8); \
  if (cc->retref) { \
    if (ngpr < maxgpr) \
      cc->gpr[ngpr++] = (GPRArg)dp; \
    else \
      CCALL_PUSH(dp); \
  }

#endif

#define CCALL_HANDLE_COMPLEXRET2 \
  if (!cc->retref) \
    *(int64_t *)dp = *(int64_t *)sp;  /* Copy complex float from GPRs. */

#define CCALL_HANDLE_STRUCTARG \
  ngpr = maxgpr;  /* Pass all structs by value on the stack. */

#define CCALL_HANDLE_COMPLEXARG \
  isfp = 1;  /* Pass complex by value on stack. */

#define CCALL_HANDLE_REGARG \
  if (!isfp) {  /* Only non-FP values may be passed in registers. */ \
    if (n > 1) {  /* Anything > 32 bit is passed on the stack. */ \
      if (!LJ_ABI_WIN) ngpr = maxgpr;  /* Prevent reordering. */ \
    } else if (ngpr + 1 <= maxgpr) { \
      dp = &cc->gpr[ngpr]; \
      ngpr += n; \
      goto done; \
    } \
  }

#elif LJ_TARGET_X64 && LJ_ABI_WIN
/* -- Windows/x64 calling conventions ------------------------------------- */

#define CCALL_HANDLE_STRUCTRET \
  /* Return structs of size 1, 2, 4 or 8 in a GPR. */ \
  cc->retref = !(sz == 1 || sz == 2 || sz == 4 || sz == 8); \
  if (cc->retref) cc->gpr[ngpr++] = (GPRArg)dp;

#define CCALL_HANDLE_COMPLEXRET CCALL_HANDLE_STRUCTRET

#define CCALL_HANDLE_COMPLEXRET2 \
  if (!cc->retref) \
    *(int64_t *)dp = *(int64_t *)sp;  /* Copy complex float from GPRs. */

#define CCALL_HANDLE_STRUCTARG \
  /* Pass structs of size 1, 2, 4 or 8 in a GPR by value. */ \
  if (!(sz == 1 || sz == 2 || sz == 4 || sz == 8)) { \
    rp = cdataptr(lj_cdata_new_l(L, cts, did, sz)); \
    sz = CTSIZE_PTR;  /* Pass all other structs by reference. */ \
  }

#define CCALL_HANDLE_COMPLEXARG \
  /* Pass complex float in a GPR and complex double by reference. */ \
  if (sz != 2*sizeof(float)) { \
    rp = cdataptr(lj_cdata_new_l(L, cts, did, sz)); \
    sz = CTSIZE_PTR; \
  }

/* Windows/x64 argument registers are strictly positional (use ngpr). */
#define CCALL_HANDLE_REGARG \
  if (isfp) { \
    if (ngpr < maxgpr) { dp = &cc->fpr[ngpr++]; nfpr = ngpr; goto done; } \
  } else { \
    if (ngpr < maxgpr) { dp = &cc->gpr[ngpr++]; goto done; } \
  }

#elif LJ_TARGET_X64
/* -- POSIX/x64 calling conventions --------------------------------------- */

#define CCALL_HANDLE_STRUCTRET \
  int rcl[2]; rcl[0] = rcl[1] = 0; \
  if (ccall_classify_struct(cts, ctr, rcl, 0)) { \
    cc->retref = 1;  /* Return struct by reference. */ \
    cc->gpr[ngpr++] = (GPRArg)dp; \
  } else { \
    cc->retref = 0;  /* Return small structs in registers. */ \
  }

#define CCALL_HANDLE_STRUCTRET2 \
  int rcl[2]; rcl[0] = rcl[1] = 0; \
  ccall_classify_struct(cts, ctr, rcl, 0); \
  ccall_struct_ret(cc, rcl, dp, ctype_size_acq(ctr));

#define CCALL_HANDLE_COMPLEXRET \
  /* Complex values are returned in one or two FPRs. */ \
  cc->retref = 0;

#define CCALL_HANDLE_COMPLEXRET2 \
  if (ctype_size_acq(ctr) == 2*sizeof(float)) {  /* Copy complex float from FPR. */ \
    *(int64_t *)dp = cc->fpr[0].l[0]; \
  } else {  /* Copy non-contiguous complex double from FPRs. */ \
    ((int64_t *)dp)[0] = cc->fpr[0].l[0]; \
    ((int64_t *)dp)[1] = cc->fpr[1].l[0]; \
  }

#define CCALL_HANDLE_STRUCTARG \
  int rcl[2]; rcl[0] = rcl[1] = 0; \
  if (!ccall_classify_struct(cts, d, rcl, 0)) { \
    cc->nsp = nsp; cc->ngpr = ngpr; cc->nfpr = nfpr; \
    if (ccall_struct_arg(cc, L, cts, d, did, rcl, o, narg)) goto err_nyi; \
    nsp = cc->nsp; ngpr = cc->ngpr; nfpr = cc->nfpr; \
    continue; \
  } else {  /* Pass all other structs by value on stack. */ \
    onstack = 1; \
  }

#define CCALL_HANDLE_COMPLEXARG \
  isfp = 2;  /* Pass complex in FPRs or on stack. Needs postprocessing. */

#define CCALL_HANDLE_REGARG \
  if (isfp) {  /* Try to pass argument in FPRs. */ \
    int n2 = ctype_isvector(ctype_info_acq(d)) ? 1 : n; \
    if (nfpr + n2 <= CCALL_NARG_FPR) { \
      dp = &cc->fpr[nfpr]; \
      nfpr += n2; \
      goto done; \
    } \
  } else {  /* Try to pass argument in GPRs. */ \
    /* Note that reordering is explicitly allowed in the x64 ABI. */ \
    if (!onstack && n <= 2 && ngpr + n <= maxgpr) { \
      dp = &cc->gpr[ngpr]; \
      ngpr += n; \
      goto done; \
    } \
  }

#elif LJ_TARGET_ARM
/* -- ARM calling conventions --------------------------------------------- */

#if LJ_ABI_SOFTFP

#define CCALL_HANDLE_STRUCTRET \
  /* Return structs of size <= 4 in a GPR. */ \
  cc->retref = !(sz <= 4); \
  if (cc->retref) cc->gpr[ngpr++] = (GPRArg)dp;

#define CCALL_HANDLE_COMPLEXRET \
  cc->retref = 1;  /* Return all complex values by reference. */ \
  cc->gpr[ngpr++] = (GPRArg)dp;

#define CCALL_HANDLE_COMPLEXRET2 \
  UNUSED(dp); /* Nothing to do. */

#define CCALL_HANDLE_STRUCTARG \
  /* Pass all structs by value in registers and/or on the stack. */

#define CCALL_HANDLE_COMPLEXARG \
  /* Pass complex by value in 2 or 4 GPRs. */

#define CCALL_HANDLE_REGARG_FP1
#define CCALL_HANDLE_REGARG_FP2

#else

#define CCALL_HANDLE_STRUCTRET \
  cc->retref = !ccall_classify_struct(cts, ctr, ct); \
  if (cc->retref) cc->gpr[ngpr++] = (GPRArg)dp;

#define CCALL_HANDLE_STRUCTRET2 \
  if (ccall_classify_struct(cts, ctr, ct) > 1) sp = (uint8_t *)&cc->fpr[0]; \
  memcpy(dp, sp, ctr->size);

#define CCALL_HANDLE_COMPLEXRET \
  if (!(ct->info & CTF_VARARG)) cc->retref = 0;  /* Return complex in FPRs. */

#define CCALL_HANDLE_COMPLEXRET2 \
  if (!(ct->info & CTF_VARARG)) memcpy(dp, &cc->fpr[0], ctr->size);

#define CCALL_HANDLE_STRUCTARG \
  isfp = (ccall_classify_struct(cts, d, ct) > 1);
  /* Pass all structs by value in registers and/or on the stack. */

#define CCALL_HANDLE_COMPLEXARG \
  isfp = 1;  /* Pass complex by value in FPRs or on stack. */

#define CCALL_HANDLE_REGARG_FP1 \
  if (isfp && !(ct->info & CTF_VARARG)) { \
    if ((d->info & CTF_ALIGN) > CTALIGN_PTR) { \
      if (nfpr + (n >> 1) <= CCALL_NARG_FPR) { \
	dp = &cc->fpr[nfpr]; \
	nfpr += (n >> 1); \
	goto done; \
      } \
    } else { \
      if (sz > 1 && fprodd != nfpr) fprodd = 0; \
      if (fprodd) { \
	if (2*nfpr+n <= 2*CCALL_NARG_FPR+1) { \
	  dp = (void *)&cc->fpr[fprodd-1].f[1]; \
	  nfpr += (n >> 1); \
	  if ((n & 1)) fprodd = 0; else fprodd = nfpr-1; \
	  goto done; \
	} \
      } else { \
	if (2*nfpr+n <= 2*CCALL_NARG_FPR) { \
	  dp = (void *)&cc->fpr[nfpr]; \
	  nfpr += (n >> 1); \
	  if ((n & 1)) fprodd = ++nfpr; else fprodd = 0; \
	  goto done; \
	} \
      } \
    } \
    fprodd = 0;  /* No reordering after the first FP value is on stack. */ \
  } else {

#define CCALL_HANDLE_REGARG_FP2	}

#endif

#define CCALL_HANDLE_REGARG \
  CCALL_HANDLE_REGARG_FP1 \
  if ((d->info & CTF_ALIGN) > CTALIGN_PTR) { \
    if (ngpr < maxgpr) \
      ngpr = (ngpr + 1u) & ~1u;  /* Align to regpair. */ \
  } \
  if (ngpr < maxgpr) { \
    dp = &cc->gpr[ngpr]; \
    if (ngpr + n > maxgpr) { \
      nsp += (ngpr + n - maxgpr) * CTSIZE_PTR;  /* Assumes contiguous gpr/stack fields. */ \
      if (nsp > CCALL_SIZE_STACK) goto err_nyi;  /* Too many arguments. */ \
      ngpr = maxgpr; \
    } else { \
      ngpr += n; \
    } \
    goto done; \
  } CCALL_HANDLE_REGARG_FP2

#define CCALL_HANDLE_RET \
  if ((ct->info & CTF_VARARG)) sp = (uint8_t *)&cc->gpr[0];

#elif LJ_TARGET_ARM64
/* -- ARM64 calling conventions ------------------------------------------- */

#define CCALL_HANDLE_STRUCTRET \
  cc->retref = !ccall_classify_struct(cts, ctr); \
  if (cc->retref) cc->retp = dp;

#define CCALL_HANDLE_STRUCTRET2 \
  unsigned int cl = ccall_classify_struct(cts, ctr); \
  if ((cl & 4)) { /* Combine float HFA from separate registers. */ \
    CTSize i = (cl >> 8) - 1; \
    do { ((uint32_t *)dp)[i] = cc->fpr[i].lo; } while (i--); \
  } else { \
    if (cl > 1) sp = (uint8_t *)&cc->fpr[0]; \
    memcpy(dp, sp, ctr->size); \
  }

#define CCALL_HANDLE_COMPLEXRET \
  /* Complex values are returned in one or two FPRs. */ \
  cc->retref = 0;

#define CCALL_HANDLE_COMPLEXRET2 \
  if (ctr->size == 2*sizeof(float)) {  /* Copy complex float from FPRs. */ \
    ((float *)dp)[0] = cc->fpr[0].f; \
    ((float *)dp)[1] = cc->fpr[1].f; \
  } else {  /* Copy complex double from FPRs. */ \
    ((double *)dp)[0] = cc->fpr[0].d; \
    ((double *)dp)[1] = cc->fpr[1].d; \
  }

#define CCALL_HANDLE_STRUCTARG \
  unsigned int cl = ccall_classify_struct(cts, d); \
  if (cl == 0) {  /* Pass struct by reference. */ \
    rp = cdataptr(lj_cdata_new_l(L, cts, did, sz)); \
    sz = CTSIZE_PTR; \
  } else if (cl > 1) {  /* Pass struct in FPRs or on stack. */ \
    isfp = (cl & 4) ? 2 : 1; \
  }  /* else: Pass struct in GPRs or on stack. */

#define CCALL_HANDLE_COMPLEXARG \
  /* Pass complex by value in separate (!) FPRs or on stack. */ \
  isfp = sz == 2*sizeof(float) ? 2 : 1;

#define CCALL_HANDLE_REGARG \
  if (LJ_TARGET_OSX && isva) { \
    /* IOS: All variadic arguments are on the stack. */ \
  } else if (isfp) {  /* Try to pass argument in FPRs. */ \
    int n2 = ctype_isvector(d->info) ? 1 : \
	     isfp == 1 ? n : (d->size >> (4-isfp)); \
    if (nfpr + n2 <= CCALL_NARG_FPR) { \
      dp = &cc->fpr[nfpr]; \
      nfpr += n2; \
      goto done; \
    } else { \
      nfpr = CCALL_NARG_FPR;  /* Prevent reordering. */ \
    } \
  } else {  /* Try to pass argument in GPRs. */ \
    if (!LJ_TARGET_OSX && !rp && ccall_struct_align(cts, d, did) > CTALIGN_PTR) \
      ngpr = (ngpr + 1u) & ~1u;  /* Align to regpair. */ \
    if (ngpr + n <= maxgpr) { \
      dp = &cc->gpr[ngpr]; \
      ngpr += n; \
      goto done; \
    } else { \
      ngpr = maxgpr;  /* Prevent reordering. */ \
    } \
  }

#if LJ_BE
#define CCALL_HANDLE_RET \
  if (ctype_isfp(ctr->info) && ctr->size == sizeof(float)) \
    sp = (uint8_t *)&cc->fpr[0].f;
#endif


#elif LJ_TARGET_PPC
/* -- PPC calling conventions --------------------------------------------- */

#define CCALL_HANDLE_STRUCTRET \
  cc->retref = 1;  /* Return all structs by reference. */ \
  cc->gpr[ngpr++] = (GPRArg)dp;

#define CCALL_HANDLE_COMPLEXRET \
  /* Complex values are returned in 2 or 4 GPRs. */ \
  cc->retref = 0;

#define CCALL_HANDLE_COMPLEXRET2 \
  memcpy(dp, sp, ctr->size);  /* Copy complex from GPRs. */

#define CCALL_HANDLE_STRUCTARG \
  rp = cdataptr(lj_cdata_new_l(L, cts, did, sz)); \
  sz = CTSIZE_PTR;  /* Pass all structs by reference. */

#define CCALL_HANDLE_COMPLEXARG \
  /* Pass complex by value in 2 or 4 GPRs. */

#define CCALL_HANDLE_GPR \
  /* Try to pass argument in GPRs. */ \
  if (n > 1) { \
    /* int64_t or complex (float). */ \
    lj_assertL(n == 2 || n == 4, "bad GPR size %d", n); \
    if (ctype_isinteger(d->info) || ctype_isfp(d->info)) \
      ngpr = (ngpr + 1u) & ~1u;  /* Align int64_t to regpair. */ \
    else if (ngpr + n > maxgpr) \
      ngpr = maxgpr;  /* Prevent reordering. */ \
  } \
  if (ngpr + n <= maxgpr) { \
    dp = &cc->gpr[ngpr]; \
    ngpr += n; \
    goto done; \
  } \

#if LJ_ABI_SOFTFP
#define CCALL_HANDLE_REGARG  CCALL_HANDLE_GPR
#else
#define CCALL_HANDLE_REGARG \
  if (isfp) {  /* Try to pass argument in FPRs. */ \
    if (nfpr + 1 <= CCALL_NARG_FPR) { \
      dp = &cc->fpr[nfpr]; \
      nfpr += 1; \
      d = ctype_get(cts, CTID_DOUBLE);  /* FPRs always hold doubles. */ \
      goto done; \
    } \
  } else { \
    CCALL_HANDLE_GPR \
  }
#endif

#if !LJ_ABI_SOFTFP
#define CCALL_HANDLE_RET \
  if (ctype_isfp(ctr->info) && ctr->size == sizeof(float)) \
    ctr = ctype_get(cts, CTID_DOUBLE);  /* FPRs always hold doubles. */
#endif

#elif LJ_TARGET_MIPS32
/* -- MIPS o32 calling conventions ---------------------------------------- */

#define CCALL_HANDLE_STRUCTRET \
  cc->retref = 1;  /* Return all structs by reference. */ \
  cc->gpr[ngpr++] = (GPRArg)dp;

#define CCALL_HANDLE_COMPLEXRET \
  /* Complex values are returned in 1 or 2 FPRs. */ \
  cc->retref = 0;

#if LJ_ABI_SOFTFP
#define CCALL_HANDLE_COMPLEXRET2 \
  if (ctr->size == 2*sizeof(float)) {  /* Copy complex float from GPRs. */ \
    ((intptr_t *)dp)[0] = cc->gpr[0]; \
    ((intptr_t *)dp)[1] = cc->gpr[1]; \
  } else {  /* Copy complex double from GPRs. */ \
    ((intptr_t *)dp)[0] = cc->gpr[0]; \
    ((intptr_t *)dp)[1] = cc->gpr[1]; \
    ((intptr_t *)dp)[2] = cc->gpr[2]; \
    ((intptr_t *)dp)[3] = cc->gpr[3]; \
  }
#else
#define CCALL_HANDLE_COMPLEXRET2 \
  if (ctr->size == 2*sizeof(float)) {  /* Copy complex float from FPRs. */ \
    ((float *)dp)[0] = cc->fpr[0].f; \
    ((float *)dp)[1] = cc->fpr[1].f; \
  } else {  /* Copy complex double from FPRs. */ \
    ((double *)dp)[0] = cc->fpr[0].d; \
    ((double *)dp)[1] = cc->fpr[1].d; \
  }
#endif

#define CCALL_HANDLE_STRUCTARG \
  /* Pass all structs by value in registers and/or on the stack. */

#define CCALL_HANDLE_COMPLEXARG \
  /* Pass complex by value in 2 or 4 GPRs. */

#define CCALL_HANDLE_GPR \
  if ((d->info & CTF_ALIGN) > CTALIGN_PTR) \
    ngpr = (ngpr + 1u) & ~1u;  /* Align to regpair. */ \
  if (ngpr < maxgpr) { \
    dp = &cc->gpr[ngpr]; \
    if (ngpr + n > maxgpr) { \
     nsp += (ngpr + n - maxgpr) * CTSIZE_PTR;  /* Assumes contiguous gpr/stack fields. */ \
     if (nsp > CCALL_SIZE_STACK) goto err_nyi;  /* Too many arguments. */ \
     ngpr = maxgpr; \
    } else { \
     ngpr += n; \
    } \
    goto done; \
  }

#if !LJ_ABI_SOFTFP	/* MIPS32 hard-float */
#define CCALL_HANDLE_REGARG \
  if (isfp && nfpr < CCALL_NARG_FPR && !(ct->info & CTF_VARARG)) { \
    /* Try to pass argument in FPRs. */ \
    dp = n == 1 ? (void *)&cc->fpr[nfpr].f : (void *)&cc->fpr[nfpr].d; \
    nfpr++; ngpr += n; \
    goto done; \
  } else {  /* Try to pass argument in GPRs. */ \
    nfpr = CCALL_NARG_FPR; \
    CCALL_HANDLE_GPR \
  }
#else			/* MIPS32 soft-float */
#define CCALL_HANDLE_REGARG CCALL_HANDLE_GPR
#endif

#if !LJ_ABI_SOFTFP
/* On MIPS64 soft-float, position of float return values is endian-dependant. */
#define CCALL_HANDLE_RET \
  if (ctype_isfp(ctr->info) && ctr->size == sizeof(float)) \
    sp = (uint8_t *)&cc->fpr[0].f;
#endif

#elif LJ_TARGET_MIPS64
/* -- MIPS n64 calling conventions ---------------------------------------- */

#define CCALL_HANDLE_STRUCTRET \
  cc->retref = !(sz <= 16); \
  if (cc->retref) cc->gpr[ngpr++] = (GPRArg)dp;

#define CCALL_HANDLE_STRUCTRET2 \
  ccall_copy_struct(cc, ctr, dp, sp, ccall_classify_struct(cts, ctr, ct));

#define CCALL_HANDLE_COMPLEXRET \
  /* Complex values are returned in 1 or 2 FPRs. */ \
  cc->retref = 0;

#if LJ_ABI_SOFTFP	/* MIPS64 soft-float */

#define CCALL_HANDLE_COMPLEXRET2 \
  if (ctr->size == 2*sizeof(float)) {  /* Copy complex float from GPRs. */ \
    ((intptr_t *)dp)[0] = cc->gpr[0]; \
  } else {  /* Copy complex double from GPRs. */ \
    ((intptr_t *)dp)[0] = cc->gpr[0]; \
    ((intptr_t *)dp)[1] = cc->gpr[1]; \
  }

#define CCALL_HANDLE_COMPLEXARG \
  /* Pass complex by value in 2 or 4 GPRs. */

/* Position of soft-float 'float' return value depends on endianess.  */
#define CCALL_HANDLE_RET \
  if (ctype_isfp(ctr->info) && ctr->size == sizeof(float)) \
    sp = (uint8_t *)cc->gpr + LJ_ENDIAN_SELECT(0, 4);

#else			/* MIPS64 hard-float */

#define CCALL_HANDLE_COMPLEXRET2 \
  if (ctr->size == 2*sizeof(float)) {  /* Copy complex float from FPRs. */ \
    ((float *)dp)[0] = cc->fpr[0].f; \
    ((float *)dp)[1] = cc->fpr[1].f; \
  } else {  /* Copy complex double from FPRs. */ \
    ((double *)dp)[0] = cc->fpr[0].d; \
    ((double *)dp)[1] = cc->fpr[1].d; \
  }

#define CCALL_HANDLE_COMPLEXARG \
  if (sz == 2*sizeof(float)) { \
    isfp = 2; \
    if (ngpr < maxgpr) \
      sz *= 2; \
  }

#define CCALL_HANDLE_RET \
  if (ctype_isfp(ctr->info) && ctr->size == sizeof(float)) \
    sp = (uint8_t *)&cc->fpr[0].f;

#endif

#define CCALL_HANDLE_STRUCTARG \
  /* Pass all structs by value in registers and/or on the stack. */

#define CCALL_HANDLE_REGARG \
  if (ngpr < maxgpr) { \
    dp = &cc->gpr[ngpr]; \
    if (ngpr + n > maxgpr) { \
      nsp += (ngpr + n - maxgpr) * CTSIZE_PTR;  /* Assumes contiguous gpr/stack fields. */ \
      if (nsp > CCALL_SIZE_STACK) goto err_nyi;  /* Too many arguments. */ \
      ngpr = maxgpr; \
    } else { \
      ngpr += n; \
    } \
    goto done; \
  }

#else
#error "Missing calling convention definitions for this architecture"
#endif

#ifndef CCALL_HANDLE_STRUCTRET2
#define CCALL_HANDLE_STRUCTRET2 \
  memcpy(dp, sp, ctr->size);  /* Copy struct return value from GPRs. */
#endif

/* -- x86 OSX ABI struct classification ----------------------------------- */

#if LJ_TARGET_X86 && LJ_TARGET_OSX

/* Check for struct with single FP field. */
static int ccall_classify_struct(CTState *cts, CType *ct)
{
  CTSize sz = ct->size;
  if (!(sz == sizeof(float) || sz == sizeof(double))) return 0;
  if ((ct->info & CTF_UNION)) return 0;
  while (ct->sib) {
    ct = ctype_get(cts, ct->sib);
    if (ctype_isfield(ct->info)) {
      CType *sct = ctype_rawchild(cts, ct);
      if (ctype_isfp(sct->info)) {
	if (sct->size == sz)
	  return (sz >> 2);  /* Return 1 for float or 2 for double. */
      } else if (ctype_isstruct(sct->info)) {
	if (sct->size)
	  return ccall_classify_struct(cts, sct);
      } else {
	break;
      }
    } else if (ctype_isbitfield(ct->info)) {
      break;
    } else if (ctype_isxattrib(ct->info, CTA_SUBTYPE)) {
      CType *sct = ctype_rawchild(cts, ct);
      if (sct->size)
	return ccall_classify_struct(cts, sct);
    }
  }
  return 0;
}

#endif

/* -- x64 struct classification ------------------------------------------- */

#if LJ_TARGET_X64 && !LJ_ABI_WIN

/* Register classes for x64 struct classification. */
#define CCALL_RCL_INT	1
#define CCALL_RCL_SSE	2
#define CCALL_RCL_MEM	4
/* NYI: classify vectors. */

static int ccall_classify_struct(CTState *cts, CType *ct, int *rcl, CTSize ofs);

/* Classify a C type. */
static void ccall_classify_ct(CTState *cts, CType *ct, int *rcl, CTSize ofs)
{
  CTInfo info = ctype_info_acq(ct);
  CTSize size = ctype_size_acq(ct);
  if (ctype_isarray(info)) {
    CType *cct = ctype_rawchild(cts, ct);
    CTSize eofs, esz = ctype_size_acq(cct), asz = size;
    for (eofs = 0; eofs < asz; eofs += esz)
      ccall_classify_ct(cts, cct, rcl, ofs+eofs);
  } else if (ctype_isstruct(info)) {
    ccall_classify_struct(cts, ct, rcl, ofs);
  } else {
    int cl = ctype_isfp(info) ? CCALL_RCL_SSE : CCALL_RCL_INT;
    lj_assertCTS(ctype_hassize(info),
		 "classify ctype %08x without size", info);
    if ((ofs & (size-1))) cl = CCALL_RCL_MEM;  /* Unaligned. */
    rcl[(ofs >= 8)] |= cl;
  }
}

/* Recursively classify a struct based on its fields. */
static int ccall_classify_struct(CTState *cts, CType *ct, int *rcl, CTSize ofs)
{
  CTypeID fid;
  if (ctype_size_acq(ct) > 16) return CCALL_RCL_MEM;  /* Too big. */
  for (fid = ctype_sib_acq(ct); fid; ) {
    CTInfo info;
    CTSize fofs;
    ct = ctype_get(cts, fid);
    info = ctype_info_acq(ct);
    fid = ctype_sib_acq(ct);
    fofs = ofs+ctype_size_acq(ct);
    if (ctype_isfield(info))
      ccall_classify_ct(cts, ctype_rawchild(cts, ct), rcl, fofs);
    else if (ctype_isbitfield(info) && ctype_bitbsz(info))
      rcl[(fofs >= 8)] |= CCALL_RCL_INT;  /* NYI: unaligned bitfields? */
    else if (ctype_isxattrib(info, CTA_SUBTYPE))
      ccall_classify_struct(cts, ctype_rawchild(cts, ct), rcl, fofs);
  }
  return ((rcl[0]|rcl[1]) & CCALL_RCL_MEM);  /* Memory class? */
}

/* Try to split up a small struct into registers. */
static int ccall_struct_reg(CCallState *cc, CTState *cts, GPRArg *dp, int *rcl)
{
  MSize ngpr = cc->ngpr, nfpr = cc->nfpr;
  uint32_t i;
  UNUSED(cts);
  for (i = 0; i < 2; i++) {
    lj_assertCTS(!(rcl[i] & CCALL_RCL_MEM), "pass mem struct in reg");
    if ((rcl[i] & CCALL_RCL_INT)) {  /* Integer class takes precedence. */
      if (ngpr >= CCALL_NARG_GPR) return 1;  /* Register overflow. */
      cc->gpr[ngpr++] = dp[i];
    } else if ((rcl[i] & CCALL_RCL_SSE)) {
      if (nfpr >= CCALL_NARG_FPR) return 1;  /* Register overflow. */
      cc->fpr[nfpr++].l[0] = dp[i];
    }
  }
  cc->ngpr = ngpr; cc->nfpr = nfpr;
  return 0;  /* Ok. */
}

/* Pass a small struct argument. */
static int ccall_struct_arg(CCallState *cc, lua_State *L, CTState *cts,
			    CType *d, CTypeID did, int *rcl, TValue *o,
			    int narg)
{
  GPRArg dp[2];
  MSize align = (1u << ctype_align(ctype_info_acq(d))) - 1;
  dp[0] = dp[1] = 0;
  /* Convert to temp. struct. */
  lj_cconv_ct_tv_l(L, cts, d, did, (uint8_t *)dp, o, CCF_ARG(narg));
  if (ccall_struct_reg(cc, cts, dp, rcl)) {
    /* Register overflow? Pass on stack. */
    MSize nsp = cc->nsp, sz = rcl[1] ? 2*CTSIZE_PTR : CTSIZE_PTR;
    if (nsp + sz > CCALL_SIZE_STACK)
      return 1;  /* Too many arguments. */
    if (CCALL_ALIGN_STACKARG && align > CTSIZE_PTR-1)
      nsp = (nsp + align) & ~align;  /* Align argument on stack. */
    cc->nsp = nsp + sz;
    memcpy((uint8_t *)cc->stack + nsp, dp, sz);
  }
  return 0;  /* Ok. */
}

/* Combine returned small struct. */
static void ccall_struct_ret(CCallState *cc, int *rcl, uint8_t *dp, CTSize sz)
{
  GPRArg sp[2];
  MSize ngpr = 0, nfpr = 0;
  uint32_t i;
  for (i = 0; i < 2; i++) {
    if ((rcl[i] & CCALL_RCL_INT)) {  /* Integer class takes precedence. */
      sp[i] = cc->gpr[ngpr++];
    } else if ((rcl[i] & CCALL_RCL_SSE)) {
      sp[i] = cc->fpr[nfpr++].l[0];
    }
  }
  memcpy(dp, sp, sz);
}
#endif

/* -- ARM hard-float ABI struct classification ---------------------------- */

#if LJ_TARGET_ARM && !LJ_ABI_SOFTFP

/* Classify a struct based on its fields. */
static unsigned int ccall_classify_struct(CTState *cts, CType *ct, CType *ctf)
{
  CTSize sz = ct->size;
  unsigned int r = 0, n = 0, isu = (ct->info & CTF_UNION);
  if ((ctf->info & CTF_VARARG)) goto noth;
  while (ct->sib) {
    CType *sct;
    ct = ctype_get(cts, ct->sib);
    if (ctype_isfield(ct->info)) {
      sct = ctype_rawchild(cts, ct);
      if (ctype_isfp(sct->info)) {
	r |= sct->size;
	if (!isu) n++; else if (n == 0) n = 1;
      } else if (ctype_iscomplex(sct->info)) {
	r |= (sct->size >> 1);
	if (!isu) n += 2; else if (n < 2) n = 2;
      } else if (ctype_isstruct(sct->info)) {
	goto substruct;
      } else {
	goto noth;
      }
    } else if (ctype_isbitfield(ct->info)) {
      goto noth;
    } else if (ctype_isxattrib(ct->info, CTA_SUBTYPE)) {
      sct = ctype_rawchild(cts, ct);
    substruct:
      if (sct->size > 0) {
	unsigned int s = ccall_classify_struct(cts, sct, ctf);
	if (s <= 1) goto noth;
	r |= (s & 255);
	if (!isu) n += (s >> 8); else if (n < (s >>8)) n = (s >> 8);
      }
    }
  }
  if ((r == 4 || r == 8) && n <= 4)
    return r + (n << 8);
noth:  /* Not a homogeneous float/double aggregate. */
  return (sz <= 4);  /* Return structs of size <= 4 in a GPR. */
}

#endif

/* -- ARM64 ABI struct classification ------------------------------------- */

#if LJ_TARGET_ARM64

#if !LJ_TARGET_OSX
/* Alignment of pass-by-value structs: 8 or 16. */
static CTInfo ccall_struct_align_arm64(CTState *cts, CType *ct, CTypeID id)
{
  CTSize sz;
  if (ct->sib) {
    while (ct->sib) {
      ct = ctype_get(cts, ct->sib);
      if (ctype_isfield(ct->info)) {
	if ((ct->info & CTF_ALIGN) > CTALIGN_PTR) return CTALIGN(4);
      } else if (ctype_isxattrib(ct->info, CTA_SUBTYPE)) {
	CTypeID sid = ctype_rawchildid(cts, ct);
	CTInfo info = lj_ctype_info(cts, sid, &sz);
	if ((info & CTF_ALIGN) > CTALIGN_PTR) return CTALIGN(4);
      }
    }
  } else  {
    CTInfo info = lj_ctype_info(cts, ctype_rawid(cts, id), &sz);
    if ((info & CTF_ALIGN) > CTALIGN_PTR) return CTALIGN(4);
  }
  return CTALIGN_PTR;
}
#define ccall_struct_align(cts, ct, id) \
  ccall_struct_align_arm64((cts), (ct), (id))
#endif

/* Classify a struct based on its fields. */
static unsigned int ccall_classify_struct(CTState *cts, CType *ct)
{
  CTSize sz = ct->size;
  unsigned int r = 0, n = 0, isu = (ct->info & CTF_UNION);
  while (ct->sib && n <= 4) {
    unsigned int m = 1;
    CType *sct;
    ct = ctype_get(cts, ct->sib);
    if (ctype_isfield(ct->info)) {
      sct = ctype_rawchild(cts, ct);
      if (ctype_isarray(sct->info) && !sct->size) goto noth;
      while (ctype_isarray(sct->info)) {
	CType *cct = ctype_rawchild(cts, sct);
	m *= sct->size / cct->size;
	sct = cct;
      }
      if (ctype_isfp(sct->info)) {
	r |= sct->size;
	if (!isu) n += m; else if (n < m) n = m;
      } else if (ctype_iscomplex(sct->info)) {
	r |= (sct->size >> 1);
	if (!isu) n += 2*m; else if (n < 2*m) n = 2*m;
      } else if (ctype_isstruct(sct->info)) {
	goto substruct;
      } else {
	goto noth;
      }
    } else if (ctype_isbitfield(ct->info) && ctype_bitbsz(ct->info)) {
      goto noth;
    } else if (ctype_isxattrib(ct->info, CTA_SUBTYPE)) {
      sct = ctype_rawchild(cts, ct);
    substruct:
      if (sct->size > 0) {
	unsigned int s = ccall_classify_struct(cts, sct), sn;
	if (s <= 1) goto noth;
	r |= (s & 255);
	sn = (s >> 8) * m;
	if (!isu) n += sn; else if (n < sn) n = sn;
      }
    }
  }
  if ((r == 4 || r == 8) && n <= 4)
    return r + (n << 8);
noth:  /* Not a homogeneous float/double aggregate. */
  return (sz <= 16);  /* Return structs of size <= 16 in GPRs. */
}

#endif

/* -- MIPS64 ABI struct classification ---------------------------- */

#if LJ_TARGET_MIPS64

#define FTYPE_FLOAT	1
#define FTYPE_DOUBLE	2

/* Classify FP fields (max. 2) and their types. */
static unsigned int ccall_classify_struct(CTState *cts, CType *ct, CType *ctf)
{
  int n = 0, ft = 0;
  if ((ctf->info & CTF_VARARG) || (ct->info & CTF_UNION))
    goto noth;
  while (ct->sib) {
    CType *sct;
    ct = ctype_get(cts, ct->sib);
    if (n == 2) {
      goto noth;
    } else if (ctype_isfield(ct->info)) {
      sct = ctype_rawchild(cts, ct);
      if (ctype_isfp(sct->info)) {
	ft |= (sct->size == 4 ? FTYPE_FLOAT : FTYPE_DOUBLE) << 2*n;
	n++;
      } else {
	goto noth;
      }
    } else if (ctype_isbitfield(ct->info) ||
	       ctype_isxattrib(ct->info, CTA_SUBTYPE)) {
      goto noth;
    }
  }
  if (n <= 2)
    return ft;
noth:  /* Not a homogeneous float/double aggregate. */
  return 0;  /* Struct is in GPRs. */
}

static void ccall_copy_struct(CCallState *cc, CType *ctr, void *dp, void *sp,
			      int ft)
{
  if (LJ_ABI_SOFTFP ? ft :
      ((ft & 3) == FTYPE_FLOAT || (ft >> 2) == FTYPE_FLOAT)) {
    int i, ofs = 0;
    for (i = 0; ft != 0; i++, ft >>= 2) {
      if ((ft & 3) == FTYPE_FLOAT) {
#if LJ_ABI_SOFTFP
	/* The 2nd FP struct result is in CARG1 (gpr[2]) and not CRET2. */
	memcpy((uint8_t *)dp + ofs,
	       (uint8_t *)&cc->gpr[2*i] + LJ_ENDIAN_SELECT(0, 4), 4);
#else
	*(float *)((uint8_t *)dp + ofs) = cc->fpr[i].f;
#endif
	ofs += 4;
      } else {
	ofs = (ofs + 7) & ~7;  /* 64 bit alignment. */
#if LJ_ABI_SOFTFP
	*(intptr_t *)((uint8_t *)dp + ofs) = cc->gpr[2*i];
#else
	*(double *)((uint8_t *)dp + ofs) = cc->fpr[i].d;
#endif
	ofs += 8;
      }
    }
  } else {
#if !LJ_ABI_SOFTFP
    if (ft) sp = (uint8_t *)&cc->fpr[0];
#endif
    memcpy(dp, sp, ctr->size);
  }
}

#endif

#ifndef ccall_struct_align
/* Alignment of pass-by-value structs. */
#define ccall_struct_align(cts, ct, id)	(ctype_info_acq((ct)) & CTF_ALIGN)
#endif

/* -- Common C call handling ---------------------------------------------- */

/* Infer the destination CTypeID for a vararg argument.
** Note: may reallocate the C type table and invalidate CType pointers.
*/
CTypeID lj_ccall_ctid_vararg(lua_State *L, CTState *cts, cTValue *o)
{
  if (tvisnumber(o)) {
    return CTID_DOUBLE;
  } else if (tviscdata(o)) {
    CTypeID id = cdataV(o)->ctypeid;
    CType *s = ctype_get(cts, id);
    CTInfo sinfo = ctype_info_acq(s);
    CTSize ssize = ctype_size_acq(s);
    if (ctype_isrefarray(sinfo)) {
      return lj_ctype_intern_l(L, cts,
	       CTINFO(CT_PTR, CTALIGN_PTR|ctype_cid(sinfo)), CTSIZE_PTR);
    } else if (ctype_isstruct(sinfo) || ctype_isfunc(sinfo)) {
      /* NYI: how to pass a struct by value in a vararg argument? */
      return lj_ctype_intern_l(L, cts, CTINFO(CT_PTR, CTALIGN_PTR|id),
			       CTSIZE_PTR);
    } else if (ctype_isfp(sinfo) && ssize == sizeof(float)) {
      return CTID_DOUBLE;
    } else {
      return id;
    }
  } else if (tvisstr(o)) {
    return CTID_P_CCHAR;
  } else if (tvisbool(o)) {
    return CTID_BOOL;
  } else {
    return CTID_P_VOID;
  }
}

/* Setup arguments for C call.
** Note: may reallocate the C type table and invalidate CType pointers.
*/
static int ccall_set_args(lua_State *L, CTState *cts, CType *ct,
			  CCallState *cc)
{
  int gcsteps = 0;
  TValue *o, *top = L->top;
  CTypeID fid;
  CTInfo info = ctype_info_acq(ct);  /* Vararg inference may invalidate ct. */
  CType *ctr;
  MSize maxgpr, ngpr = 0, nsp = 0, narg;
#if CCALL_NARG_FPR
  MSize nfpr = 0;
#if LJ_TARGET_ARM
  MSize fprodd = 0;
#endif
#endif

  /* Clear unused regs to get some determinism in case of misdeclaration. */
  memset(cc->gpr, 0, sizeof(cc->gpr));
#if CCALL_NUM_FPR
  memset(cc->fpr, 0, sizeof(cc->fpr));
#endif

#if LJ_TARGET_X86
  /* x86 has several different calling conventions. */
  cc->resx87 = 0;
  switch (ctype_cconv(info)) {
  case CTCC_FASTCALL: maxgpr = 2; break;
  case CTCC_THISCALL: maxgpr = 1; break;
  default: maxgpr = 0; break;
  }
#else
  maxgpr = CCALL_NARG_GPR;
#endif

  /* Perform required setup for some result types. */
  ctr = ctype_rawchild(cts, ct);
  {
    CTInfo rinfo = ctype_info_acq(ctr);
    CTSize rsize = ctype_size_acq(ctr);
    if (ctype_isvector(rinfo)) {
      if (!(CCALL_VECTOR_REG && (rsize == 8 || rsize == 16)))
	goto err_nyi;
    } else if (ctype_iscomplex(rinfo) || ctype_isstruct(rinfo)) {
      /* Preallocate cdata object and anchor it after arguments. */
      CTSize sz = rsize;
      GCcdata *cd = lj_cdata_new_l(L, cts, ctype_cid(info), sz);
      void *dp = cdataptr(cd);
      setcdataV(L, L->top++, cd);
      if (ctype_isstruct(rinfo)) {
	CCALL_HANDLE_STRUCTRET
      } else {
	CCALL_HANDLE_COMPLEXRET
      }
#if LJ_TARGET_X86
    } else if (ctype_isfp(rinfo)) {
      cc->resx87 = rsize == sizeof(float) ? 1 : 2;
#endif
    }
  }

  /* Skip initial attributes. */
  fid = ctype_sib_acq(ct);
  while (fid) {
    CType *ctf = ctype_get(cts, fid);
    CTInfo finfo = ctype_info_acq(ctf);
    if (!ctype_isattrib(finfo)) break;
    fid = ctype_sib_acq(ctf);
  }

#if LJ_TARGET_ARM64 && LJ_ABI_WIN
  if ((info & CTF_VARARG)) {
    nsp -= maxgpr * CTSIZE_PTR;  /* May end up with negative nsp. */
    ngpr = maxgpr;
    nfpr = CCALL_NARG_FPR;
  }
#endif

  /* Walk through all passed arguments. */
  for (o = L->base+1, narg = 1; o < top; o++, narg++) {
    CTypeID did;
    CType *d;
    CTInfo dinfo;
    CTSize dsize, sz;
    MSize n, isfp = 0, isva = 0;
    void *dp, *rp = NULL;
#if LJ_TARGET_X64 && !LJ_ABI_WIN
    int onstack = 0;
#endif

    if (fid) {  /* Get argument type from field. */
      CType *ctf = ctype_get(cts, fid);
      CTInfo finfo = ctype_info_acq(ctf);
      fid = ctype_sib_acq(ctf);
      lj_assertL(ctype_isfield(finfo), "field expected");
      did = ctype_cid(finfo);
    } else {
      if (!(info & CTF_VARARG))
	lj_err_caller(L, LJ_ERR_FFI_NUMARG);  /* Too many arguments. */
      did = lj_ccall_ctid_vararg(L, cts, o);  /* Infer vararg type. */
      isva = 1;
    }
    d = ctype_raw(cts, did);
    dinfo = ctype_info_acq(d);
    dsize = ctype_size_acq(d);
    sz = dsize;

    /* Find out how (by value/ref) and where (GPR/FPR) to pass an argument. */
    if (ctype_isnum(dinfo)) {
      if (sz > 8) goto err_nyi;
      if ((dinfo & CTF_FP))
	isfp = 1;
    } else if (ctype_isvector(dinfo)) {
      if (CCALL_VECTOR_REG && (sz == 8 || sz == 16))
	isfp = 1;
      else
	goto err_nyi;
    } else if (ctype_isstruct(dinfo)) {
      CCALL_HANDLE_STRUCTARG
    } else if (ctype_iscomplex(dinfo)) {
      CCALL_HANDLE_COMPLEXARG
    } else if (!(CCALL_PACK_STACKARG && ctype_isenum(dinfo))) {
      sz = CTSIZE_PTR;
    }
    n = (sz + CTSIZE_PTR-1) / CTSIZE_PTR;  /* Number of GPRs or stack slots needed. */

    CCALL_HANDLE_REGARG  /* Handle register arguments. */

    /* Otherwise pass argument on stack. */
    if (CCALL_ALIGN_STACKARG) {  /* Align argument on stack. */
      MSize align = (1u << ctype_align(ccall_struct_align(cts, d, did))) - 1;
#if LJ_TARGET_ARM64 && LJ_TARGET_OSX
      isva = ctype_isstruct(dinfo);
#endif
      if (rp || (CCALL_PACK_STACKARG && isva && align < CTSIZE_PTR-1))
	align = CTSIZE_PTR-1;
      nsp = (nsp + align) & ~align;
    }
#if LJ_TARGET_ARM64 && LJ_ABI_WIN
    /* A negative nsp points into cc->gpr. Blame MS for their messy ABI. */
    dp = ((uint8_t *)cc->stack) + (int32_t)nsp;
#else
    dp = ((uint8_t *)cc->stack) + nsp;
#endif
    nsp += CCALL_PACK_STACKARG ? sz : n * CTSIZE_PTR;
    if ((int32_t)nsp > CCALL_SIZE_STACK) {  /* Too many arguments. */
    err_nyi:
      lj_err_caller(L, LJ_ERR_FFI_NYICALL);
    }
    isva = 0;

  done:
    if (rp) {  /* Pass by reference. */
      gcsteps++;
      *(void **)dp = rp;
      dp = rp;
    }
    lj_cconv_ct_tv_l(L, cts, d, did, (uint8_t *)dp, o, CCF_ARG(narg));
    /* Extend passed integers to 32 bits at least. */
    if (ctype_isinteger_or_bool(dinfo) && dsize < 4 &&
	(!CCALL_PACK_STACKARG || !((uintptr_t)dp & 3))) {  /* Assumes LJ_LE. */
      if (dinfo & CTF_UNSIGNED)
	*(uint32_t *)dp = dsize == 1 ? (uint32_t)*(uint8_t *)dp :
					  (uint32_t)*(uint16_t *)dp;
      else
	*(int32_t *)dp = dsize == 1 ? (int32_t)*(int8_t *)dp :
					 (int32_t)*(int16_t *)dp;
    }
#if LJ_TARGET_ARM64 && LJ_BE
    if (isfp && dsize == sizeof(float))
      ((float *)dp)[1] = ((float *)dp)[0];  /* Floats occupy high slot. */
#endif
#if LJ_TARGET_MIPS64 || (LJ_TARGET_ARM64 && LJ_BE)
    if ((ctype_isinteger_or_bool(dinfo) || ctype_isenum(dinfo)
#if LJ_TARGET_MIPS64
	 || (isfp && nsp == 0)
#endif
	 ) && dsize <= 4) {
      *(int64_t *)dp = (int64_t)*(int32_t *)dp;  /* Sign-extend to 64 bit. */
    }
#endif
#if LJ_TARGET_X64 && LJ_ABI_WIN
    if (isva) {  /* Windows/x64 mirrors varargs in both register sets. */
      if (nfpr == ngpr)
	cc->gpr[ngpr-1] = cc->fpr[ngpr-1].l[0];
      else
	cc->fpr[ngpr-1].l[0] = cc->gpr[ngpr-1];
    }
#else
    UNUSED(isva);
#endif
#if LJ_TARGET_X64 && !LJ_ABI_WIN
    if (isfp == 2 && n == 2 && (uint8_t *)dp == (uint8_t *)&cc->fpr[nfpr-2]) {
      cc->fpr[nfpr-1].d[0] = cc->fpr[nfpr-2].d[1];  /* Split complex double. */
      cc->fpr[nfpr-2].d[1] = 0;
    }
#elif LJ_TARGET_ARM64 || (LJ_TARGET_MIPS64 && !LJ_ABI_SOFTFP)
    if (isfp == 2 && (uint8_t *)dp < (uint8_t *)cc->stack) {
      /* Split float HFA or complex float into separate registers. */
      CTSize i = (sz >> 2) - 1;
      do { ((uint64_t *)dp)[i] = ((uint32_t *)dp)[i]; } while (i--);
    }
#else
    UNUSED(isfp);
#endif
  }
  if (fid) lj_err_caller(L, LJ_ERR_FFI_NUMARG);  /* Too few arguments. */
#if LJ_TARGET_ARM64 && LJ_ABI_WIN
  if ((int32_t)nsp < 0) nsp = 0;
#endif

#if LJ_TARGET_X64 || (LJ_TARGET_PPC && !LJ_ABI_SOFTFP)
  cc->nfpr = nfpr;  /* Required for vararg functions. */
#endif
  cc->nsp = (nsp + CTSIZE_PTR-1) & ~(CTSIZE_PTR-1);
  cc->spadj = (CCALL_SPS_FREE + CCALL_SPS_EXTRA) * CTSIZE_PTR;
  if (cc->nsp > CCALL_SPS_FREE * CTSIZE_PTR)
    cc->spadj += (((cc->nsp - CCALL_SPS_FREE * CTSIZE_PTR) + 15u) & ~15u);
  return gcsteps;
}

/* Get results from C call. */
static int ccall_get_results(lua_State *L, CTState *cts, CType *ct,
			     CCallState *cc, int *ret)
{
  CTInfo info = ctype_info_acq(ct);
  CTypeID rid = ctype_rawid(cts, ctype_cid(info));
  CType *ctr = ctype_get(cts, rid);
  CTInfo rinfo = ctype_info_acq(ctr);
  CTSize rsize = ctype_size_acq(ctr);
  uint8_t *sp = (uint8_t *)&cc->gpr[0];
  if (ctype_isvoid(rinfo)) {
    *ret = 0;  /* Zero results. */
    return 0;  /* No additional GC step. */
  }
  *ret = 1;  /* One result. */
  if (ctype_isstruct(rinfo)) {
    /* Return cdata object which is already on top of stack. */
    if (!cc->retref) {
      void *dp = cdataptr(cdataV(L->top-1));  /* Use preallocated object. */
      CCALL_HANDLE_STRUCTRET2
    }
    return 1;  /* One GC step. */
  }
  if (ctype_iscomplex(rinfo)) {
    /* Return cdata object which is already on top of stack. */
    void *dp = cdataptr(cdataV(L->top-1));  /* Use preallocated object. */
    CCALL_HANDLE_COMPLEXRET2
    return 1;  /* One GC step. */
  }
  if (LJ_BE && rsize < CTSIZE_PTR &&
      (ctype_isinteger_or_bool(rinfo) || ctype_isenum(rinfo)))
    sp += (CTSIZE_PTR - rsize);
#if CCALL_NUM_FPR
  if (ctype_isfp(rinfo) || ctype_isvector(rinfo))
    sp = (uint8_t *)&cc->fpr[0];
#endif
#ifdef CCALL_HANDLE_RET
  CCALL_HANDLE_RET
#endif
  lj_assertL(!(ctype_isrefarray(rinfo) || ctype_isstruct(rinfo)),
	     "unexpected reference ctype");
  return lj_cconv_tv_ct_l(L, cts, ctr, rid, L->top-1, sp);
}

static int ccall_had_stopreq(lua_State *L)
{
  TGState *tg = L2TG(L);
  return tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ);
}

static int ccall_fresh_stopreq(lua_State *L, uint32_t actions,
			       int had_stopreq)
{
  return lj_safepoint_fresh_stopreq(L, actions, had_stopreq);
}

static void ccall_checkstop_fresh(lua_State *L, uint32_t actions,
				  int had_stopreq)
{
  if (ccall_fresh_stopreq(L, actions, had_stopreq))
    lj_safepoint_checkstop(L, actions);
}

void lj_ccall_native_save(lua_State *L, CCallNativeState *st)
{
  TGState *tg = L2TG(L);
  CCallbackRuntime *cb = &tg->cb;
  st->tg = tg;
  st->cb = cb;
  st->old_ffi_call_func = lj_tg_ffi_call_func_acq(tg);
  st->old_callback_slot = ccallback_slot_acq(cb);
  st->old_native_had_stopreq = ccallback_native_had_stopreq_acq(cb);
  st->had_stopreq = 0;
}

void lj_ccall_native_enter(lua_State *L, CCallNativeState *st, void *func)
{
  TGState *tg = st->tg;
  CCallbackRuntime *cb = st->cb;
  int had_stopreq;
  ccallback_slot_rel(cb, ~0u);
  lj_tg_ffi_call_func_rel(tg, func);
  had_stopreq = ccall_had_stopreq(L);
  st->had_stopreq = had_stopreq;
  ccallback_native_had_stopreq_rel(cb, (uint8_t)had_stopreq);
  lj_native_enter(tg);
}

uint32_t lj_ccall_native_leave(lua_State *L, CTState *cts,
			       CCallNativeState *st, void *func)
{
  uint32_t actions = lj_native_leave(L);
  CCallbackRuntime *cb = st->cb;
  MSize callback_slot = ccallback_slot_acq(cb);
  /* Blacklist function that called a callback. */
  if (callback_slot != (MSize)~0u)
    lj_ctype_cb_blacklist(L, cts, func);
  ccallback_slot_rel(cb, st->old_callback_slot);
  lj_tg_ffi_call_func_rel(st->tg, st->old_ffi_call_func);
  ccallback_native_had_stopreq_rel(cb, st->old_native_had_stopreq);
  return actions;
}

void lj_ccall_native_checkstop(lua_State *L, uint32_t actions,
			       const CCallNativeState *st)
{
  ccall_checkstop_fresh(L, actions, st->had_stopreq);
}

void lj_ccall_jit_void_gpr(lua_State *L, void *func, uintptr_t a,
			   uintptr_t b, uint32_t sig)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  switch (sig) {
  case LJ_CCALL_JIT_SIG0:
    ((void (*)(void))(uintptr_t)func)();
    break;
  case LJ_CCALL_JIT_SIG_I32:
    ((void (*)(int32_t))(uintptr_t)func)((int32_t)a);
    break;
  case LJ_CCALL_JIT_SIG_U32:
    ((void (*)(uint32_t))(uintptr_t)func)((uint32_t)a);
    break;
  case LJ_CCALL_JIT_SIG_PTR:
    ((void (*)(void *))(uintptr_t)func)((void *)a);
    break;
  case LJ_CCALL_JIT_SIG_I32_I32:
    ((void (*)(int32_t, int32_t))(uintptr_t)func)((int32_t)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_I32_U32:
    ((void (*)(int32_t, uint32_t))(uintptr_t)func)
      ((int32_t)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_I32_PTR:
    ((void (*)(int32_t, void *))(uintptr_t)func)((int32_t)a, (void *)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_I32:
    ((void (*)(uint32_t, int32_t))(uintptr_t)func)
      ((uint32_t)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_U32:
    ((void (*)(uint32_t, uint32_t))(uintptr_t)func)
      ((uint32_t)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_PTR:
    ((void (*)(uint32_t, void *))(uintptr_t)func)
      ((uint32_t)a, (void *)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_I32:
    ((void (*)(void *, int32_t))(uintptr_t)func)((void *)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_U32:
    ((void (*)(void *, uint32_t))(uintptr_t)func)
      ((void *)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_PTR:
    ((void (*)(void *, void *))(uintptr_t)func)((void *)a, (void *)b);
    break;
  default:
    break;
  }
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
}

int32_t lj_ccall_jit_i32_gpr(lua_State *L, void *func, uintptr_t a,
			     uintptr_t b, uint32_t sig)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  int32_t ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  switch (sig) {
  case LJ_CCALL_JIT_SIG0:
    ret = ((int32_t (*)(void))(uintptr_t)func)();
    break;
  case LJ_CCALL_JIT_SIG_I32:
    ret = ((int32_t (*)(int32_t))(uintptr_t)func)((int32_t)a);
    break;
  case LJ_CCALL_JIT_SIG_U32:
    ret = ((int32_t (*)(uint32_t))(uintptr_t)func)((uint32_t)a);
    break;
  case LJ_CCALL_JIT_SIG_PTR:
    ret = ((int32_t (*)(void *))(uintptr_t)func)((void *)a);
    break;
  case LJ_CCALL_JIT_SIG_I32_I32:
    ret = ((int32_t (*)(int32_t, int32_t))(uintptr_t)func)
	    ((int32_t)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_I32_U32:
    ret = ((int32_t (*)(int32_t, uint32_t))(uintptr_t)func)
	    ((int32_t)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_I32_PTR:
    ret = ((int32_t (*)(int32_t, void *))(uintptr_t)func)
	    ((int32_t)a, (void *)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_I32:
    ret = ((int32_t (*)(uint32_t, int32_t))(uintptr_t)func)
	    ((uint32_t)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_U32:
    ret = ((int32_t (*)(uint32_t, uint32_t))(uintptr_t)func)
	    ((uint32_t)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_PTR:
    ret = ((int32_t (*)(uint32_t, void *))(uintptr_t)func)
	    ((uint32_t)a, (void *)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_I32:
    ret = ((int32_t (*)(void *, int32_t))(uintptr_t)func)
	    ((void *)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_U32:
    ret = ((int32_t (*)(void *, uint32_t))(uintptr_t)func)
	    ((void *)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_PTR:
    ret = ((int32_t (*)(void *, void *))(uintptr_t)func)
	    ((void *)a, (void *)b);
    break;
  default:
    ret = 0;
    break;
  }
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

int32_t lj_ccall_jit_i32_u32(lua_State *L, void *func, uintptr_t a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  int32_t ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((int32_t (*)(uint32_t))(uintptr_t)func)((uint32_t)a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

int32_t lj_ccall_jit_narrow_0(lua_State *L, void *func, uint32_t sig)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  int32_t ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  switch (sig) {
  case LJ_CCALL_JIT_NARROW_I8:
    ret = ((int8_t (*)(void))(uintptr_t)func)();
    break;
  case LJ_CCALL_JIT_NARROW_U8:
    ret = ((uint8_t (*)(void))(uintptr_t)func)();
    break;
  case LJ_CCALL_JIT_NARROW_I16:
    ret = ((int16_t (*)(void))(uintptr_t)func)();
    break;
  case LJ_CCALL_JIT_NARROW_U16:
    ret = ((uint16_t (*)(void))(uintptr_t)func)();
    break;
  default:
    ret = 0;
    break;
  }
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

int32_t lj_ccall_jit_narrow_gpr(lua_State *L, void *func, uintptr_t a,
				uintptr_t b, uint32_t sig)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions, argsig = sig & 0xffu, retsig = sig >> 8;
  int32_t ret;
#define LJ_CCALL_JIT_NARROW_DISPATCH(T) \
  switch (argsig) { \
  case LJ_CCALL_JIT_SIG0: \
    ret = (int32_t)((T (*)(void))(uintptr_t)func)(); \
    break; \
  case LJ_CCALL_JIT_SIG_I32: \
    ret = (int32_t)((T (*)(int32_t))(uintptr_t)func)((int32_t)a); \
    break; \
  case LJ_CCALL_JIT_SIG_U32: \
    ret = (int32_t)((T (*)(uint32_t))(uintptr_t)func)((uint32_t)a); \
    break; \
  case LJ_CCALL_JIT_SIG_PTR: \
    ret = (int32_t)((T (*)(void *))(uintptr_t)func)((void *)a); \
    break; \
  case LJ_CCALL_JIT_SIG_I32_I32: \
    ret = (int32_t)((T (*)(int32_t, int32_t))(uintptr_t)func) \
	    ((int32_t)a, (int32_t)b); \
    break; \
  case LJ_CCALL_JIT_SIG_I32_U32: \
    ret = (int32_t)((T (*)(int32_t, uint32_t))(uintptr_t)func) \
	    ((int32_t)a, (uint32_t)b); \
    break; \
  case LJ_CCALL_JIT_SIG_I32_PTR: \
    ret = (int32_t)((T (*)(int32_t, void *))(uintptr_t)func) \
	    ((int32_t)a, (void *)b); \
    break; \
  case LJ_CCALL_JIT_SIG_U32_I32: \
    ret = (int32_t)((T (*)(uint32_t, int32_t))(uintptr_t)func) \
	    ((uint32_t)a, (int32_t)b); \
    break; \
  case LJ_CCALL_JIT_SIG_U32_U32: \
    ret = (int32_t)((T (*)(uint32_t, uint32_t))(uintptr_t)func) \
	    ((uint32_t)a, (uint32_t)b); \
    break; \
  case LJ_CCALL_JIT_SIG_U32_PTR: \
    ret = (int32_t)((T (*)(uint32_t, void *))(uintptr_t)func) \
	    ((uint32_t)a, (void *)b); \
    break; \
  case LJ_CCALL_JIT_SIG_PTR_I32: \
    ret = (int32_t)((T (*)(void *, int32_t))(uintptr_t)func) \
	    ((void *)a, (int32_t)b); \
    break; \
  case LJ_CCALL_JIT_SIG_PTR_U32: \
    ret = (int32_t)((T (*)(void *, uint32_t))(uintptr_t)func) \
	    ((void *)a, (uint32_t)b); \
    break; \
  case LJ_CCALL_JIT_SIG_PTR_PTR: \
    ret = (int32_t)((T (*)(void *, void *))(uintptr_t)func) \
	    ((void *)a, (void *)b); \
    break; \
  default: \
    ret = 0; \
    break; \
  }
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  switch (retsig) {
  case LJ_CCALL_JIT_NARROW_I8:
    LJ_CCALL_JIT_NARROW_DISPATCH(int8_t);
    break;
  case LJ_CCALL_JIT_NARROW_U8:
    LJ_CCALL_JIT_NARROW_DISPATCH(uint8_t);
    break;
  case LJ_CCALL_JIT_NARROW_I16:
    LJ_CCALL_JIT_NARROW_DISPATCH(int16_t);
    break;
  case LJ_CCALL_JIT_NARROW_U16:
    LJ_CCALL_JIT_NARROW_DISPATCH(uint16_t);
    break;
  default:
    ret = 0;
    break;
  }
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
#undef LJ_CCALL_JIT_NARROW_DISPATCH
  return ret;
}

double lj_ccall_jit_u32_gpr(lua_State *L, void *func, uintptr_t a,
			    uintptr_t b, uint32_t sig)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions, ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  switch (sig) {
  case LJ_CCALL_JIT_SIG0:
    ret = ((uint32_t (*)(void))(uintptr_t)func)();
    break;
  case LJ_CCALL_JIT_SIG_I32:
    ret = ((uint32_t (*)(int32_t))(uintptr_t)func)((int32_t)a);
    break;
  case LJ_CCALL_JIT_SIG_U32:
    ret = ((uint32_t (*)(uint32_t))(uintptr_t)func)((uint32_t)a);
    break;
  case LJ_CCALL_JIT_SIG_PTR:
    ret = ((uint32_t (*)(void *))(uintptr_t)func)((void *)a);
    break;
  case LJ_CCALL_JIT_SIG_I32_I32:
    ret = ((uint32_t (*)(int32_t, int32_t))(uintptr_t)func)
	    ((int32_t)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_I32_U32:
    ret = ((uint32_t (*)(int32_t, uint32_t))(uintptr_t)func)
	    ((int32_t)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_I32_PTR:
    ret = ((uint32_t (*)(int32_t, void *))(uintptr_t)func)
	    ((int32_t)a, (void *)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_I32:
    ret = ((uint32_t (*)(uint32_t, int32_t))(uintptr_t)func)
	    ((uint32_t)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_U32:
    ret = ((uint32_t (*)(uint32_t, uint32_t))(uintptr_t)func)
	    ((uint32_t)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_PTR:
    ret = ((uint32_t (*)(uint32_t, void *))(uintptr_t)func)
	    ((uint32_t)a, (void *)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_I32:
    ret = ((uint32_t (*)(void *, int32_t))(uintptr_t)func)
	    ((void *)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_U32:
    ret = ((uint32_t (*)(void *, uint32_t))(uintptr_t)func)
	    ((void *)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_PTR:
    ret = ((uint32_t (*)(void *, void *))(uintptr_t)func)
	    ((void *)a, (void *)b);
    break;
  default:
    ret = 0;
    break;
  }
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return (double)ret;
}

double lj_ccall_jit_u32_u32(lua_State *L, void *func, uintptr_t a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions, ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((uint32_t (*)(uint32_t))(uintptr_t)func)((uint32_t)a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return (double)ret;
}

double lj_ccall_jit_u32_0(lua_State *L, void *func)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions, ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((uint32_t (*)(void))(uintptr_t)func)();
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return (double)ret;
}

uint64_t lj_ccall_jit_u64_0(lua_State *L, void *func)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  uint64_t ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((uint64_t (*)(void))(uintptr_t)func)();
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

uint64_t lj_ccall_jit_u64_u64(lua_State *L, void *func, uint64_t a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  uint64_t ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((uint64_t (*)(uint64_t))(uintptr_t)func)(a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

int64_t lj_ccall_jit_i64_gpr(lua_State *L, void *func, int64_t a,
			     int64_t b, uint32_t sig)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  int64_t ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  switch (sig) {
  case LJ_CCALL_JIT_NUM_SIG0:
    ret = ((int64_t (*)(void))(uintptr_t)func)();
    break;
  case LJ_CCALL_JIT_NUM_SIG_NUM:
    ret = ((int64_t (*)(int64_t))(uintptr_t)func)(a);
    break;
  case LJ_CCALL_JIT_NUM_SIG_NUM_NUM:
    ret = ((int64_t (*)(int64_t, int64_t))(uintptr_t)func)(a, b);
    break;
  default:
    ret = 0;
    break;
  }
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

int64_t lj_ccall_jit_i64_ret_gpr(lua_State *L, void *func, uintptr_t a,
				 uintptr_t b, uint32_t sig)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  int64_t ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  switch (sig) {
  case LJ_CCALL_JIT_SIG0:
    ret = ((int64_t (*)(void))(uintptr_t)func)();
    break;
  case LJ_CCALL_JIT_SIG_I32:
    ret = ((int64_t (*)(int32_t))(uintptr_t)func)((int32_t)a);
    break;
  case LJ_CCALL_JIT_SIG_U32:
    ret = ((int64_t (*)(uint32_t))(uintptr_t)func)((uint32_t)a);
    break;
  case LJ_CCALL_JIT_SIG_PTR:
    ret = ((int64_t (*)(void *))(uintptr_t)func)((void *)a);
    break;
  case LJ_CCALL_JIT_SIG_I32_I32:
    ret = ((int64_t (*)(int32_t, int32_t))(uintptr_t)func)
	    ((int32_t)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_I32_U32:
    ret = ((int64_t (*)(int32_t, uint32_t))(uintptr_t)func)
	    ((int32_t)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_I32_PTR:
    ret = ((int64_t (*)(int32_t, void *))(uintptr_t)func)
	    ((int32_t)a, (void *)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_I32:
    ret = ((int64_t (*)(uint32_t, int32_t))(uintptr_t)func)
	    ((uint32_t)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_U32:
    ret = ((int64_t (*)(uint32_t, uint32_t))(uintptr_t)func)
	    ((uint32_t)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_PTR:
    ret = ((int64_t (*)(uint32_t, void *))(uintptr_t)func)
	    ((uint32_t)a, (void *)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_I32:
    ret = ((int64_t (*)(void *, int32_t))(uintptr_t)func)
	    ((void *)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_U32:
    ret = ((int64_t (*)(void *, uint32_t))(uintptr_t)func)
	    ((void *)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_PTR:
    ret = ((int64_t (*)(void *, void *))(uintptr_t)func)
	    ((void *)a, (void *)b);
    break;
  default:
    ret = 0;
    break;
  }
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

uint64_t lj_ccall_jit_u64_gpr(lua_State *L, void *func, uintptr_t a,
			      uintptr_t b, uint32_t sig)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  uint64_t ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  switch (sig) {
  case LJ_CCALL_JIT_SIG0:
    ret = ((uint64_t (*)(void))(uintptr_t)func)();
    break;
  case LJ_CCALL_JIT_SIG_I32:
    ret = ((uint64_t (*)(int32_t))(uintptr_t)func)((int32_t)a);
    break;
  case LJ_CCALL_JIT_SIG_U32:
    ret = ((uint64_t (*)(uint32_t))(uintptr_t)func)((uint32_t)a);
    break;
  case LJ_CCALL_JIT_SIG_PTR:
    ret = ((uint64_t (*)(void *))(uintptr_t)func)((void *)a);
    break;
  case LJ_CCALL_JIT_SIG_I32_I32:
    ret = ((uint64_t (*)(int32_t, int32_t))(uintptr_t)func)
	    ((int32_t)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_I32_U32:
    ret = ((uint64_t (*)(int32_t, uint32_t))(uintptr_t)func)
	    ((int32_t)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_I32_PTR:
    ret = ((uint64_t (*)(int32_t, void *))(uintptr_t)func)
	    ((int32_t)a, (void *)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_I32:
    ret = ((uint64_t (*)(uint32_t, int32_t))(uintptr_t)func)
	    ((uint32_t)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_U32:
    ret = ((uint64_t (*)(uint32_t, uint32_t))(uintptr_t)func)
	    ((uint32_t)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_PTR:
    ret = ((uint64_t (*)(uint32_t, void *))(uintptr_t)func)
	    ((uint32_t)a, (void *)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_I32:
    ret = ((uint64_t (*)(void *, int32_t))(uintptr_t)func)
	    ((void *)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_U32:
    ret = ((uint64_t (*)(void *, uint32_t))(uintptr_t)func)
	    ((void *)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_PTR:
    ret = ((uint64_t (*)(void *, void *))(uintptr_t)func)
	    ((void *)a, (void *)b);
    break;
  default:
    ret = 0;
    break;
  }
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

void *lj_ccall_jit_ptr_gpr(lua_State *L, void *func, uintptr_t a,
			   uintptr_t b, uint32_t sig)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  void *ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  switch (sig) {
  case LJ_CCALL_JIT_SIG0:
    ret = ((void *(*)(void))(uintptr_t)func)();
    break;
  case LJ_CCALL_JIT_SIG_I32:
    ret = ((void *(*)(int32_t))(uintptr_t)func)((int32_t)a);
    break;
  case LJ_CCALL_JIT_SIG_U32:
    ret = ((void *(*)(uint32_t))(uintptr_t)func)((uint32_t)a);
    break;
  case LJ_CCALL_JIT_SIG_PTR:
    ret = ((void *(*)(void *))(uintptr_t)func)((void *)a);
    break;
  case LJ_CCALL_JIT_SIG_I32_I32:
    ret = ((void *(*)(int32_t, int32_t))(uintptr_t)func)
	    ((int32_t)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_I32_U32:
    ret = ((void *(*)(int32_t, uint32_t))(uintptr_t)func)
	    ((int32_t)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_I32_PTR:
    ret = ((void *(*)(int32_t, void *))(uintptr_t)func)
	    ((int32_t)a, (void *)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_I32:
    ret = ((void *(*)(uint32_t, int32_t))(uintptr_t)func)
	    ((uint32_t)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_U32:
    ret = ((void *(*)(uint32_t, uint32_t))(uintptr_t)func)
	    ((uint32_t)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_U32_PTR:
    ret = ((void *(*)(uint32_t, void *))(uintptr_t)func)
	    ((uint32_t)a, (void *)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_I32:
    ret = ((void *(*)(void *, int32_t))(uintptr_t)func)
	    ((void *)a, (int32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_U32:
    ret = ((void *(*)(void *, uint32_t))(uintptr_t)func)
	    ((void *)a, (uint32_t)b);
    break;
  case LJ_CCALL_JIT_SIG_PTR_PTR:
    ret = ((void *(*)(void *, void *))(uintptr_t)func)
	    ((void *)a, (void *)b);
    break;
  default:
    ret = NULL;
    break;
  }
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

void *lj_ffi_jit_memcpy(lua_State *L, void *dp, const void *sp, CTSize len)
{
  uint32_t actions;
  int had_stopreq = ccall_had_stopreq(L);
  lj_native_enter(L2TG(L));
  memcpy(dp, sp, (size_t)len);
  actions = lj_native_leave(L);
  ccall_checkstop_fresh(L, actions, had_stopreq);
  return dp;
}

void *lj_ffi_jit_memset(lua_State *L, void *dp, int32_t fill, CTSize len)
{
  uint32_t actions;
  int had_stopreq = ccall_had_stopreq(L);
  lj_native_enter(L2TG(L));
  memset(dp, fill, (size_t)len);
  actions = lj_native_leave(L);
  ccall_checkstop_fresh(L, actions, had_stopreq);
  return dp;
}

size_t lj_ffi_jit_strlen(lua_State *L, const char *p)
{
  uint32_t actions;
  int had_stopreq = ccall_had_stopreq(L);
  size_t len;
  lj_native_enter(L2TG(L));
  len = strlen(p);
  actions = lj_native_leave(L);
  ccall_checkstop_fresh(L, actions, had_stopreq);
  return len;
}

double lj_ccall_jit_num_i32(lua_State *L, void *func, int32_t a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  double ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((double (*)(int32_t))(uintptr_t)func)(a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

double lj_ccall_jit_num_ptr(lua_State *L, void *func, void *a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  double ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((double (*)(void *))(uintptr_t)func)(a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

double lj_ccall_jit_num_flt(lua_State *L, void *func, float a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  double ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((double (*)(float))(uintptr_t)func)(a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

int32_t lj_ccall_jit_i32_num(lua_State *L, void *func, double a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  int32_t ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((int32_t (*)(double))(uintptr_t)func)(a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

int32_t lj_ccall_jit_i32_flt(lua_State *L, void *func, float a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  int32_t ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((int32_t (*)(float))(uintptr_t)func)(a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

int32_t lj_ccall_jit_i32_i8(lua_State *L, void *func, uintptr_t a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  int32_t ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((int32_t (*)(int8_t))(uintptr_t)func)((int8_t)a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

int32_t lj_ccall_jit_i32_ptr_ulong_i32(lua_State *L, void *func, void *a,
				       uintptr_t b, int32_t c)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  int32_t ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((int32_t (*)(void *, unsigned long, int32_t))(uintptr_t)func)
	  (a, (unsigned long)b, c);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

void *lj_ccall_jit_ptr_num(lua_State *L, void *func, double a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  void *ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((void *(*)(double))(uintptr_t)func)(a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

void lj_ccall_jit_void_num(lua_State *L, void *func, double a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ((void (*)(double))(uintptr_t)func)(a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
}

void lj_ccall_jit_void_flt(lua_State *L, void *func, float a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ((void (*)(float))(uintptr_t)func)(a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
}

float lj_ccall_jit_flt_num(lua_State *L, void *func, double a)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  float ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  ret = ((float (*)(double))(uintptr_t)func)(a);
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

double lj_ccall_jit_num_fpr(lua_State *L, void *func, double a,
			    double b, uint32_t sig)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  double ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  switch (sig) {
  case LJ_CCALL_JIT_NUM_SIG0:
    ret = ((double (*)(void))(uintptr_t)func)();
    break;
  case LJ_CCALL_JIT_NUM_SIG_NUM:
    ret = ((double (*)(double))(uintptr_t)func)(a);
    break;
  case LJ_CCALL_JIT_NUM_SIG_NUM_NUM:
    ret = ((double (*)(double, double))(uintptr_t)func)(a, b);
    break;
  default:
    ret = 0;
    break;
  }
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

float lj_ccall_jit_flt_fpr(lua_State *L, void *func, float a,
			   float b, uint32_t sig)
{
  CTState *cts = ctype_cts(L);
  CCallNativeState native;
  uint32_t actions;
  float ret;
  lj_ccall_native_save(L, &native);
  lj_ccall_native_enter(L, &native, func);
  switch (sig) {
  case LJ_CCALL_JIT_NUM_SIG0:
    ret = ((float (*)(void))(uintptr_t)func)();
    break;
  case LJ_CCALL_JIT_NUM_SIG_NUM:
    ret = ((float (*)(float))(uintptr_t)func)(a);
    break;
  case LJ_CCALL_JIT_NUM_SIG_NUM_NUM:
    ret = ((float (*)(float, float))(uintptr_t)func)(a, b);
    break;
  default:
    ret = 0;
    break;
  }
  actions = lj_ccall_native_leave(L, cts, &native, func);
  lj_ccall_native_checkstop(L, actions, &native);
  return ret;
}

/* Call C function. */
int lj_ccall_func(lua_State *L, GCcdata *cd)
{
  CTState *cts = ctype_cts(L);
  CTypeID id = ctype_rawid(cts, cd->ctypeid);
  CType *ct = ctype_get(cts, id);
  CTInfo info = ctype_info_acq(ct);
  CTSize sz = CTSIZE_PTR;
  if (ctype_isptr(info)) {
    sz = ctype_size_acq(ct);
    id = ctype_rawid(cts, ctype_cid(info));
    ct = ctype_get(cts, id);
    info = ctype_info_acq(ct);
  }
  if (ctype_isfunc(info)) {
    CCallState cc;
    CCallNativeState native;
    uint32_t actions;
    int gcsteps, ret;
    void *func;
    lj_ccall_native_save(L, &native);
    cc.func = (void (*)(void))cdata_getptr(cdataptr(cd), sz);
    func = (void *)cc.func;
    gcsteps = ccall_set_args(L, cts, ct, &cc);
    lj_ccall_native_enter(L, &native, func);
    lj_vm_ffi_call(&cc);
    actions = lj_ccall_native_leave(L, cts, &native, func);
    ct = ctype_get(cts, id);  /* Table may have been reallocated. */
    gcsteps += ccall_get_results(L, cts, ct, &cc, &ret);
#if LJ_TARGET_X86 && LJ_ABI_WIN
    /* Automatically detect __stdcall and fix up C function declaration. */
    info = ctype_info_acq(ct);
    if (cc.spadj && ctype_cconv(info) == CTCC_CDECL) {
      CTF_INSERT(ct->info, CCONV, CTCC_STDCALL);
      lj_trace_abort(G(L));
    }
#endif
    lj_ccall_native_checkstop(L, actions, &native);
    while (gcsteps-- > 0)
      lj_gc_check(L);
    return ret;
  }
  return -1;  /* Not a function. */
}

#endif
