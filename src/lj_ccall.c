/*
** FFI C call handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <string.h>
#include <stdlib.h>

#include "lj_obj.h"

#include <errno.h>
#if LJ_TARGET_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if LJ_HASFFI

#include "lj_gc.h"
#include "lj_err.h"
#include "lj_tab.h"
#include "lj_ctype.h"
#include "lj_cconv.h"
#include "lj_cdata.h"
#include "lj_ccall.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_safepoint.h"
#include "lj_state.h"
#include "lj_thr.h"
#include "lj_trace.h"
#include "lj_tg.h"

typedef struct CCallErrorState {
  int errnum;
  uint32_t winerr;
} CCallErrorState;

static LJ_AINLINE void ccall_error_save(CCallErrorState *err)
{
#if LJ_TARGET_WINDOWS
  err->winerr = (uint32_t)GetLastError();
#else
  err->winerr = 0;
#endif
  err->errnum = errno;
}

static LJ_AINLINE void ccall_error_restore(const CCallErrorState *err)
{
  errno = err->errnum;
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)err->winerr);
#endif
}

static LJ_AINLINE void ccall_error_from_native(CCallErrorState *err,
					       const CCallNativeState *st)
{
  err->errnum = st->post_errno;
  err->winerr = st->post_winerr;
}

static LJ_AINLINE void ccall_error_publish(CCallNativeState *st,
					   const CCallErrorState *err)
{
  st->post_errno = err->errnum;
  st->post_winerr = err->winerr;
}

static void ccall_ctype_copy(CType *out, CType *ct)
{
  GCobj *name;
  out->info = ctype_info_acq(ct);
  out->size = ctype_size_acq(ct);
  out->sib = (CTypeID1)ctype_sib_acq(ct);
  out->next = (CTypeID1)ctype_next_acq(ct);
  name = ctype_nameobj_acq(ct);
  setgcrefp(out->name, name);
}

static CType *ccall_ctype_snapshot_wait(lua_State *L, CTState *cts,
					 CTypeID id, CType *out)
{
  if (id <= CTID_CTYPEID) {
    if (id == 0)
      lj_err_caller(L, LJ_ERR_FFI_INVTYPE);
    ccall_ctype_copy(out, ctype_get(cts, id));
    if (!ctype_isabandoned(ctype_info_acq(out)))
      return out;
    lj_err_caller(L, LJ_ERR_FFI_INVTYPE);
  }
  for (;;) {
    int ok = lj_ctype_snapshot(cts, id, out);
    if (ok > 0)
      return out;
    if (ok == 0)
      lj_err_caller(L, LJ_ERR_FFI_INVTYPE);
    lj_ctype_parse_wait(cts, L, ctype_parse_token_acq(cts));
  }
}

static CType *ccall_rawid_wait(lua_State *L, CTState *cts, CTypeID id,
			       CTypeID *ridp, CType *out)
{
  for (;;) {
    CTInfo info;
    ccall_ctype_snapshot_wait(L, cts, id, out);
    info = ctype_info_acq(out);
    if (!ctype_isattrib(info)) {
      if (ridp) *ridp = id;
      return out;
    }
    id = ctype_cid(info);
  }
}

static CType *ccall_rawchild_wait(lua_State *L, CTState *cts, CType *ct,
				  CTypeID *ridp, CType *out)
{
  CTypeID id = ctype_childid(cts, ct);
  return ccall_rawid_wait(L, cts, id, ridp, out);
}

/* ccall_set_args() pre-grows enough Lua stack space for every ABI temporary.
** Publish pass-by-reference cdata there immediately after construction. These
** roots then survive CType waits, callback-capable native execution and the
** post-return result snapshot; normal/error frame unwinding owns cleanup. */
static LJ_AINLINE void ccall_push_cdata_root(lua_State *L, GCcdata *cd)
{
  setcdataV(L, L->top, cd);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
}

#if (LJ_TARGET_X64 && LJ_ABI_WIN) || LJ_TARGET_ARM64 || LJ_TARGET_PPC
#define CCALL_ARG_ROOT_BOUND \
  (CCALL_NUM_STACK + CCALL_NUM_GPR + 1u)
static LJ_AINLINE void *ccall_new_temp_root(lua_State *L, CTState *cts,
					     CTypeID id, CTSize sz)
{
  GCcdata *cd = lj_cdata_new_l(L, cts, id, sz);
  void *p = cdataptr(cd);
  ccall_push_cdata_root(L, cd);  /* No poll/throw after READY and before root. */
  return p;
}
#else
/* These targets pass aggregate arguments directly in CCallState storage. */
#define CCALL_ARG_ROOT_BOUND 0u
#endif

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
    rp = ccall_new_temp_root(L, cts, did, sz); \
    sz = CTSIZE_PTR;  /* Pass all other structs by reference. */ \
  }

#define CCALL_HANDLE_COMPLEXARG \
  /* Pass complex float in a GPR and complex double by reference. */ \
  if (sz != 2*sizeof(float)) { \
    rp = ccall_new_temp_root(L, cts, did, sz); \
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
  if (ccall_classify_struct(L, cts, ctr, rcl, 0)) { \
    cc->retref = 1;  /* Return struct by reference. */ \
    cc->gpr[ngpr++] = (GPRArg)dp; \
  } else { \
    cc->retref = 0;  /* Return small structs in registers. */ \
  }

#define CCALL_HANDLE_STRUCTRET2 \
  int rcl[2]; rcl[0] = rcl[1] = 0; \
  ccall_classify_struct(L, cts, ctr, rcl, 0); \
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
  if (!ccall_classify_struct(L, cts, d, rcl, 0)) { \
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
    rp = ccall_new_temp_root(L, cts, did, sz); \
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
  rp = ccall_new_temp_root(L, cts, did, sz); \
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
  memcpy(dp, sp, ctype_size_acq(ctr));  /* Copy struct return value from GPRs. */
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

static int ccall_classify_struct(lua_State *L, CTState *cts, CType *ct,
				 int *rcl, CTSize ofs);

/* Classify a C type. */
static void ccall_classify_ct(lua_State *L, CTState *cts, CType *ct,
			      int *rcl, CTSize ofs)
{
  CTInfo info = ctype_info_acq(ct);
  CTSize size = ctype_size_acq(ct);
  if (ctype_isarray(info)) {
    CType cctsnap, *cct = ccall_rawchild_wait(L, cts, ct, NULL, &cctsnap);
    CTSize eofs, esz = ctype_size_acq(cct), asz = size;
    for (eofs = 0; eofs < asz; eofs += esz)
      ccall_classify_ct(L, cts, cct, rcl, ofs+eofs);
  } else if (ctype_isstruct(info)) {
    ccall_classify_struct(L, cts, ct, rcl, ofs);
  } else {
    int cl = ctype_isfp(info) ? CCALL_RCL_SSE : CCALL_RCL_INT;
    lj_assertCTS(ctype_hassize(info),
		 "classify ctype %08x without size", info);
    if ((ofs & (size-1))) cl = CCALL_RCL_MEM;  /* Unaligned. */
    rcl[(ofs >= 8)] |= cl;
  }
}

/* Recursively classify a struct based on its fields. */
static int ccall_classify_struct(lua_State *L, CTState *cts, CType *ct,
				 int *rcl, CTSize ofs)
{
  CTypeID fid;
  if (ctype_size_acq(ct) > 16) return CCALL_RCL_MEM;  /* Too big. */
  for (fid = ctype_sib_acq(ct); fid; ) {
    CType fcopy;
    CType *field;
    CTInfo info;
    CTSize fofs;
    field = ccall_ctype_snapshot_wait(L, cts, fid, &fcopy);
    info = ctype_info_acq(field);
    fid = ctype_sib_acq(field);
    fofs = ofs+ctype_size_acq(field);
    if (ctype_isfield(info)) {
      CType child;
      ccall_classify_ct(L, cts,
			ccall_rawchild_wait(L, cts, field, NULL, &child),
			rcl, fofs);
    } else if (ctype_isbitfield(info) && ctype_bitbsz(info)) {
      rcl[(fofs >= 8)] |= CCALL_RCL_INT;  /* NYI: unaligned bitfields? */
    } else if (ctype_isxattrib(info, CTA_SUBTYPE)) {
      CType child;
      ccall_classify_struct(L, cts,
			    ccall_rawchild_wait(L, cts, field, NULL, &child),
			    rcl, fofs);
    }
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
    CType ssnap, *s = ccall_ctype_snapshot_wait(L, cts, id, &ssnap);
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
			  CCallState *cc, int *has_result_root)
{
  int gcsteps = 0;
  TValue *o, *top = L->top;
  CTypeID fid;
  CTInfo info = ctype_info_acq(ct);  /* Vararg inference may invalidate ct. */
  CType ctrsnap, *ctr;
  CTInfo result_info;
  CTSize result_size;
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
  if (lj_ctype_info_wait(L, cts, ctype_cid(info), &result_info,
			 &result_size, NULL, &ctrsnap) <= 0)
    lj_err_caller(L, LJ_ERR_FFI_INVTYPE);
  ctr = &ctrsnap;
  {
    CTInfo rinfo = ctype_info_acq(ctr);
    CTSize rsize = result_size;
    if (ctype_isvector(rinfo)) {
      if (!(CCALL_VECTOR_REG && (rsize == 8 || rsize == 16)))
	goto err_nyi;
    } else if (ctype_iscomplex(rinfo) || ctype_isstruct(rinfo)) {
      /* Preallocate cdata object and anchor it after arguments. */
      CTSize sz = rsize;
      GCcdata *cd = lj_cdata_newx_l(L, cts, ctype_cid(info), sz,
				     result_info);
      void *dp = cdataptr(cd);
      ccall_push_cdata_root(L, cd);
      *has_result_root = 1;
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
    CType ctfcopy, *ctf = ccall_ctype_snapshot_wait(L, cts, fid, &ctfcopy);
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
    CType dsnap;

    if (fid) {  /* Get argument type from field. */
      CType ctfcopy, *ctf = ccall_ctype_snapshot_wait(L, cts, fid, &ctfcopy);
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
    d = ccall_rawid_wait(L, cts, did, NULL, &dsnap);
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
  CTypeID rid;
  CType ctrsnap, *ctr = ccall_rawid_wait(L, cts, ctype_cid(info),
					 &rid, &ctrsnap);
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

static void ffi_native_frame_copy(LJFFINativeFrame *dst,
				  const LJFFINativeFrame *src)
{
  lj_ffi_native_frame_trace_rel(dst, lj_ffi_native_frame_trace_acq(src));
  lj_ffi_native_frame_L_rel(dst, lj_ffi_native_frame_L_acq(src));
  lj_ffi_native_frame_func_rel(dst, lj_ffi_native_frame_func_acq(src));
  lj_ffi_native_frame_result_root_rel(dst,
	lj_ffi_native_frame_result_root_acq(src));
  lj_ffi_native_frame_old_func_rel(dst,
	lj_ffi_native_frame_old_func_acq(src));
  lj_ffi_native_frame_root_offset_rel(dst,
	lj_ffi_native_frame_root_offset_acq(src));
  lj_ffi_native_frame_base_offset_rel(dst,
	lj_ffi_native_frame_base_offset_acq(src));
  lj_ffi_native_frame_top_offset_rel(dst,
	lj_ffi_native_frame_top_offset_acq(src));
  lj_ffi_native_frame_jit_base_offset_rel(dst,
	lj_ffi_native_frame_jit_base_offset_acq(src));
  lj_ffi_native_frame_entry_exit_epoch_rel(dst,
	lj_ffi_native_frame_entry_exit_epoch_acq(src));
  lj_ffi_native_frame_trace_no_rel(dst,
	lj_ffi_native_frame_trace_no_acq(src));
  lj_ffi_native_frame_old_callback_slot_rel(dst,
	lj_ffi_native_frame_old_callback_slot_acq(src));
  lj_ffi_native_frame_flags_rel(dst, lj_ffi_native_frame_flags_acq(src));
  lj_ffi_native_frame_old_stopreq_rel(dst,
	lj_ffi_native_frame_old_stopreq_acq(src));
  lj_ffi_native_frame_had_stopreq_rel(dst,
	lj_ffi_native_frame_had_stopreq_acq(src));
}

static void ffi_native_frame_clear(LJFFINativeFrame *frame)
{
  lj_ffi_native_frame_trace_rel(frame, NULL);
  lj_ffi_native_frame_L_rel(frame, NULL);
  lj_ffi_native_frame_func_rel(frame, NULL);
  lj_ffi_native_frame_result_root_rel(frame, NULL);
  lj_ffi_native_frame_old_func_rel(frame, NULL);
  lj_ffi_native_frame_root_offset_rel(frame, 0);
  lj_ffi_native_frame_base_offset_rel(frame, 0);
  lj_ffi_native_frame_top_offset_rel(frame, 0);
  lj_ffi_native_frame_jit_base_offset_rel(frame, 0);
  lj_ffi_native_frame_entry_exit_epoch_rel(frame, 0);
  lj_ffi_native_frame_trace_no_rel(frame, 0);
  lj_ffi_native_frame_old_callback_slot_rel(frame, 0);
  lj_ffi_native_frame_flags_rel(frame, 0);
  lj_ffi_native_frame_old_stopreq_rel(frame, 0);
  lj_ffi_native_frame_had_stopreq_rel(frame, 0);
}

static int ffi_native_frame_empty(const LJFFINativeFrame *frame)
{
  return lj_ffi_native_frame_trace_acq(frame) == NULL &&
    lj_ffi_native_frame_L_acq(frame) == NULL &&
    lj_ffi_native_frame_func_acq(frame) == NULL &&
    lj_ffi_native_frame_result_root_acq(frame) == NULL &&
    lj_ffi_native_frame_old_func_acq(frame) == NULL &&
    lj_ffi_native_frame_root_offset_acq(frame) == 0 &&
    lj_ffi_native_frame_base_offset_acq(frame) == 0 &&
    lj_ffi_native_frame_top_offset_acq(frame) == 0 &&
    lj_ffi_native_frame_jit_base_offset_acq(frame) == 0 &&
    lj_ffi_native_frame_entry_exit_epoch_acq(frame) == 0 &&
    lj_ffi_native_frame_trace_no_acq(frame) == 0 &&
    lj_ffi_native_frame_old_callback_slot_acq(frame) == 0 &&
    lj_ffi_native_frame_flags_acq(frame) == 0 &&
    lj_ffi_native_frame_old_stopreq_acq(frame) == 0 &&
    lj_ffi_native_frame_had_stopreq_acq(frame) == 0;
}

static int ffi_native_frame_valid(const LJFFINativeFrame *frame)
{
  uint64_t root = lj_ffi_native_frame_root_offset_acq(frame);
  uint64_t base = lj_ffi_native_frame_base_offset_acq(frame);
  uint64_t top = lj_ffi_native_frame_top_offset_acq(frame);
  uint64_t jitbase = lj_ffi_native_frame_jit_base_offset_acq(frame);
  uint32_t flags = lj_ffi_native_frame_flags_acq(frame);
  uint32_t state = flags & (LJ_FFI_NATIVE_FRAME_F_ACTIVE |
			    LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
			    LJ_FFI_NATIVE_FRAME_F_POSTCALL);
  return lj_ffi_native_frame_trace_acq(frame) != NULL &&
    lj_ffi_native_frame_L_acq(frame) != NULL &&
    lj_ffi_native_frame_func_acq(frame) != NULL &&
    lj_ffi_native_frame_trace_no_acq(frame) != 0 &&
    (flags & LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED) != 0 &&
    (state == LJ_FFI_NATIVE_FRAME_F_ACTIVE ||
     state == LJ_FFI_NATIVE_FRAME_F_SUSPENDED ||
     state == LJ_FFI_NATIVE_FRAME_F_POSTCALL) &&
    (flags & ~LJ_FFI_NATIVE_FRAME_F_MASK) == 0 &&
    root <= base && base <= top && jitbase <= top &&
    lj_ffi_native_frame_old_stopreq_acq(frame) <= 1 &&
    lj_ffi_native_frame_had_stopreq_acq(frame) <= 1;
}

#if defined(LJ_FFI_NATIVE_FRAME_TEST_HELPERS)
static LJFFINativeFrameSnapshotHook ffi_native_frame_snapshot_hook;

void lj_ffi_native_frame_test_set_snapshot_hook(
  LJFFINativeFrameSnapshotHook hook)
{
  la_storefunc_rel(&ffi_native_frame_snapshot_hook, hook);
}
#endif

uint64_t lj_ffi_native_frame_sequence_acq(const TGState *tg)
{
  return tg ? la_load64_acq(&tg->ffi_native_seq) : 0;
}

uint32_t lj_ffi_native_frame_depth_acq(const TGState *tg)
{
  return tg ? la_load32_acq(&tg->ffi_native_depth) : 0;
}

void lj_ffi_native_frame_init(TGState *tg)
{
  uint32_t i;
  if (LJ_UNLIKELY(tg == NULL))
    abort();
  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++)
    ffi_native_frame_clear(&tg->ffi_native_frame[i]);
  la_store32_rlx(&tg->ffi_native_depth, 0);
  la_store64_rlx(&tg->ffi_native_seq, 0);
}

void lj_ffi_native_frame_fini(const TGState *tg)
{
  uint32_t i;
  uint64_t seq;
  if (LJ_UNLIKELY(tg == NULL))
    abort();
  seq = lj_ffi_native_frame_sequence_acq(tg);
  if (LJ_UNLIKELY((seq & 1u) != 0 ||
		  lj_ffi_native_frame_depth_acq(tg) != 0))
    abort();
  for (i = 0; i < LJ_FFI_NATIVE_FRAME_MAX; i++)
    if (LJ_UNLIKELY(!ffi_native_frame_empty(&tg->ffi_native_frame[i])))
      abort();
  /* Close a final-reader race in debug/lifecycle callers: a lawful owner is
  ** excluded here, so any sequence change is internal lifecycle corruption. */
  if (LJ_UNLIKELY(lj_ffi_native_frame_sequence_acq(tg) != seq))
    abort();
}

int lj_ffi_native_frame_push(TGState *tg, const LJFFINativeFrame *frame)
{
  LJFFINativeFrame *dst;
  uint64_t seq;
  uint32_t depth, flags;
  if (LJ_UNLIKELY(tg == NULL || frame == NULL ||
		  !ffi_native_frame_valid(frame)))
    abort();
  flags = lj_ffi_native_frame_flags_acq(frame);
  /* POSTCALL is produced only by the in-place owner finish transaction. A
  ** generic push always begins a new parked ACTIVE lifetime. */
  if (LJ_UNLIKELY((flags & (LJ_FFI_NATIVE_FRAME_F_ACTIVE |
			    LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
			    LJ_FFI_NATIVE_FRAME_F_POSTCALL)) !=
		  LJ_FFI_NATIVE_FRAME_F_ACTIVE))
    abort();
  seq = lj_ffi_native_frame_sequence_acq(tg);
  depth = lj_ffi_native_frame_depth_acq(tg);
  if (LJ_UNLIKELY((seq & 1u) != 0 || depth > LJ_FFI_NATIVE_FRAME_MAX))
    abort();
  if (depth == LJ_FFI_NATIVE_FRAME_MAX)
    return 0;  /* Future lowering takes a pre-call side exit. */
  if (LJ_UNLIKELY(seq > UINT64_MAX - 2u)) {
    la_store64_rel(&tg->ffi_native_seq, UINT64_MAX);
    abort();  /* Poison: never allow a remotely visible seqlock ABA. */
  }
  dst = &tg->ffi_native_frame[depth];
  if (LJ_UNLIKELY(!ffi_native_frame_empty(dst)))
    abort();
  la_store64_rel(&tg->ffi_native_seq, seq + 1u);
  /* An odd word must become observable before any following payload change.
  ** Atomic release orders prior accesses; this explicit writer barrier also
  ** prevents the compiler from sinking the transition behind frame stores. */
  la_fence_rel();
  ffi_native_frame_copy(dst, frame);
  la_store32_rel(&tg->ffi_native_depth, depth + 1u);
  la_store64_rel(&tg->ffi_native_seq, seq + 2u);
  return 1;
}

void lj_ffi_native_frame_pop(TGState *tg, LJFFINativeFrame *frame)
{
  LJFFINativeFrame *src;
  uint64_t seq;
  uint32_t depth;
  if (LJ_UNLIKELY(tg == NULL))
    abort();
  seq = lj_ffi_native_frame_sequence_acq(tg);
  depth = lj_ffi_native_frame_depth_acq(tg);
  if (LJ_UNLIKELY((seq & 1u) != 0 || depth == 0 ||
		  depth > LJ_FFI_NATIVE_FRAME_MAX))
    abort();
  if (LJ_UNLIKELY(seq > UINT64_MAX - 2u)) {
    la_store64_rel(&tg->ffi_native_seq, UINT64_MAX);
    abort();  /* Poison: never allow a remotely visible seqlock ABA. */
  }
  src = &tg->ffi_native_frame[depth - 1u];
  if (LJ_UNLIKELY(!ffi_native_frame_valid(src)))
    abort();
  la_store64_rel(&tg->ffi_native_seq, seq + 1u);
  la_fence_rel();
  if (frame)
    ffi_native_frame_copy(frame, src);
  ffi_native_frame_clear(src);
  la_store32_rel(&tg->ffi_native_depth, depth - 1u);
  la_store64_rel(&tg->ffi_native_seq, seq + 2u);
}

#if LJ_HASJIT
/* Begin the owner's final ACTIVE-frame transition. The odd sequence precedes
** the last callback/epoch decision, so a future remote flush admission can
** never certify the old even frame while the owner independently decides to
** drop its pin. No payload is changed until the odd word is visible. */
static LJFFINativeFrame *ffi_native_frame_finish_begin(
  TGState *tg, const LJFFINativeFrame *expected, uint64_t *seqp,
  uint32_t *depthp)
{
  LJFFINativeFrame *src;
  uint64_t seq;
  uint32_t depth, flags;
  if (LJ_UNLIKELY(tg == NULL || expected == NULL || seqp == NULL ||
		  depthp == NULL))
    abort();
  seq = lj_ffi_native_frame_sequence_acq(tg);
  depth = lj_ffi_native_frame_depth_acq(tg);
  if (LJ_UNLIKELY((seq & 1u) != 0 || depth == 0 ||
		  depth > LJ_FFI_NATIVE_FRAME_MAX ||
		  seq > UINT64_MAX - 2u))
    abort();
  src = &tg->ffi_native_frame[depth - 1u];
  flags = lj_ffi_native_frame_flags_acq(src);
  if (LJ_UNLIKELY(!ffi_native_frame_valid(src) ||
	  (flags & (LJ_FFI_NATIVE_FRAME_F_ACTIVE |
		    LJ_FFI_NATIVE_FRAME_F_SUSPENDED |
		    LJ_FFI_NATIVE_FRAME_F_POSTCALL)) !=
	    LJ_FFI_NATIVE_FRAME_F_ACTIVE ||
	  lj_ffi_native_frame_trace_acq(src) !=
	    lj_ffi_native_frame_trace_acq(expected) ||
	  lj_ffi_native_frame_L_acq(src) !=
	    lj_ffi_native_frame_L_acq(expected) ||
	  lj_ffi_native_frame_trace_no_acq(src) !=
	    lj_ffi_native_frame_trace_no_acq(expected)))
    abort();
  la_store64_rel(&tg->ffi_native_seq, seq + 1u);
  /* The following force decision performs acquire loads. A full fence is
  ** required here: release alone does not order this store before later loads
  ** on x86 or in the C memory model. */
  la_fence_seq();
  *seqp = seq;
  *depthp = depth;
  return src;
}

static void ffi_native_frame_finish_end(TGState *tg, LJFFINativeFrame *src,
					uint64_t seq, uint32_t depth,
					int retain, int callback_seen)
{
  if (LJ_UNLIKELY(tg == NULL || src == NULL || (seq & 1u) != 0 ||
		  lj_ffi_native_frame_sequence_acq(tg) != seq + 1u ||
		  lj_ffi_native_frame_depth_acq(tg) != depth))
    abort();
  if (retain) {
    uint32_t flags = LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED |
	LJ_FFI_NATIVE_FRAME_F_POSTCALL;
    if (callback_seen)
      flags |= LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN;
    lj_ffi_native_frame_flags_rel(src, flags);
  } else {
    ffi_native_frame_clear(src);
    la_store32_rel(&tg->ffi_native_depth, depth - 1u);
  }
  la_store64_rel(&tg->ffi_native_seq, seq + 2u);
}
#endif

LJFFINativeFrameSnapshotResult lj_ffi_native_frame_snapshot(
  const TGState *tg, LJFFINativeFrameSnapshot *snapshot)
{
  LJFFINativeFrameSnapshot copy;
  uint64_t seq0, seq1;
  uint32_t depth, i;
  if (LJ_UNLIKELY(tg == NULL))
    return LJ_FFI_NATIVE_FRAME_SNAPSHOT_INVALID;
  seq0 = lj_ffi_native_frame_sequence_acq(tg);
  if (seq0 & 1u)
    return LJ_FFI_NATIVE_FRAME_SNAPSHOT_RETRY;
  depth = lj_ffi_native_frame_depth_acq(tg);
  if (depth > LJ_FFI_NATIVE_FRAME_MAX) {
    seq1 = lj_ffi_native_frame_sequence_acq(tg);
    return seq1 != seq0 || (seq1 & 1u) ?
      LJ_FFI_NATIVE_FRAME_SNAPSHOT_RETRY :
      LJ_FFI_NATIVE_FRAME_SNAPSHOT_INVALID;
  }
  memset(&copy, 0, sizeof(copy));
  for (i = 0; i < depth; i++)
    ffi_native_frame_copy(&copy.frame[i], &tg->ffi_native_frame[i]);
#if defined(LJ_FFI_NATIVE_FRAME_TEST_HELPERS)
  {
    LJFFINativeFrameSnapshotHook hook =
      la_loadfunc_acq(&ffi_native_frame_snapshot_hook);
    if (hook)
      hook((TGState *)tg);
  }
#endif
  seq1 = lj_ffi_native_frame_sequence_acq(tg);
  if (seq1 != seq0 || (seq1 & 1u))
    return LJ_FFI_NATIVE_FRAME_SNAPSHOT_RETRY;
  copy.sequence = seq0;
  copy.depth = depth;
  for (i = 0; i < depth; i++)
    if (LJ_UNLIKELY(!ffi_native_frame_valid(&copy.frame[i])))
      return LJ_FFI_NATIVE_FRAME_SNAPSHOT_INVALID;
  if (snapshot)
    *snapshot = copy;
  return depth == 0 ? LJ_FFI_NATIVE_FRAME_SNAPSHOT_EMPTY :
    LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE;
}

#if LJ_HASJIT
typedef struct CCallJITGeometry {
  uint64_t root_offset;
  uint64_t base_offset;
  uint64_t top_offset;
  uint64_t jitbase_offset;
} CCallJITGeometry;

LJ_STATIC_ASSERT((LJ_FFI_NATIVE_LEAVE_FORCE_EXIT & 0x0000ffffu) == 0);

#if defined(LJ_XSAVE_TEST_HELPERS)
static LJFFINativeTraceFinishHook ffi_native_trace_finish_hook;
static LJFFINativeTraceLeaveHook ffi_native_trace_leave_hook;

void lj_ffi_native_trace_test_set_finish_hook(
  LJFFINativeTraceFinishHook hook)
{
  la_storefunc_rel(&ffi_native_trace_finish_hook, hook);
}

void lj_ffi_native_trace_test_set_leave_hook(
  LJFFINativeTraceLeaveHook hook)
{
  la_storefunc_rel(&ffi_native_trace_leave_hook, hook);
}
#endif

/* Validate owner-private staging entirely through integer byte geometry. This
** avoids undefined pointer arithmetic on corrupt pending state and keeps every
** ordinary failure before trace pinning or publication. */
static int ccall_jit_geometry(lua_State *L, TGState *tg,
			      CCallJITGeometry *geo)
{
  TValue *stack, *maxstack, *root, *jitbase;
  uintptr_t stackp, maxstackp, rootp, jitbasep;
  uint64_t extent, root_extent, base_index, top_index;
  uint64_t prefix = (uint64_t)(1 + LJ_FR2) * sizeof(TValue);
  uint32_t baseslot, nslots, tid;
  global_State *g;
  if (!L || !tg || !geo || lj_tg_load_cur_L(tg) != L)
    return 0;
  g = G(L);
  tid = lj_tg_tid_acq(tg);
  if (!g || !lj_thr_id_is_owner(tid) || !lj_tg_owns_state_acq(tg, L) ||
      mref(L->glref, global_State) != g)
    return 0;
  stack = tvref(L->stack);
  maxstack = tvref(L->maxstack);
  root = (TValue *)la_loadptr_acq((void *const *)&tg->ffi_xsave_root);
  baseslot = la_load32_acq(&tg->ffi_xsave_baseslot);
  nslots = la_load32_acq(&tg->ffi_xsave_nslots);
  jitbase = lj_tg_load_jit_base(tg);
  if (!stack || !maxstack || !root || !jitbase ||
      nslots < (uint32_t)(1 + LJ_FR2))
    return 0;
  stackp = (uintptr_t)(void *)stack;
  maxstackp = (uintptr_t)(void *)maxstack;
  rootp = (uintptr_t)(void *)root;
  jitbasep = (uintptr_t)(void *)jitbase;
  if (maxstackp < stackp || rootp < stackp || rootp >= maxstackp ||
      jitbasep < stackp || jitbasep >= maxstackp)
    return 0;
  extent = (uint64_t)(maxstackp - stackp);
  root_extent = (uint64_t)(maxstackp - rootp);
  if ((extent % sizeof(TValue)) != 0 ||
      ((uint64_t)(rootp - stackp) % sizeof(TValue)) != 0 ||
      ((uint64_t)(jitbasep - stackp) % sizeof(TValue)) != 0 ||
      (uint64_t)(rootp - stackp) < prefix ||
      (uint64_t)(jitbasep - stackp) < prefix ||
      (uint64_t)nslots > root_extent / sizeof(TValue))
    return 0;
  top_index = (uint64_t)nslots - 1u - LJ_FR2;
  base_index = (uint64_t)baseslot;
  if (base_index > top_index)
    return 0;
  geo->root_offset = (uint64_t)(rootp - stackp);
  geo->base_offset = geo->root_offset + base_index * sizeof(TValue);
  geo->top_offset = geo->root_offset + top_index * sizeof(TValue);
  geo->jitbase_offset = (uint64_t)(jitbasep - stackp);
  if (geo->root_offset > geo->base_offset ||
      geo->base_offset > geo->top_offset ||
      geo->top_offset > extent || geo->jitbase_offset > geo->top_offset)
    return 0;
  return 1;
}

/* XSAVE staging is a single generated-entry attempt, not persistent carrier
** state. Consume it after successful publication or before any ordinary
** rejection returns. This avoids leaving raw stack geometry live while the
** pre-call exit resumes in the interpreter. */
static void ccall_jit_xsave_clear(TGState *tg)
{
  la_storeptr_rel((void **)&tg->ffi_xsave_root, NULL);
  la_store32_rel(&tg->ffi_xsave_baseslot, 0);
  la_store32_rel(&tg->ffi_xsave_nslots, 0);
}

#define LJ_FFI_NATIVE_FRAME_F_STATE \
  (LJ_FFI_NATIVE_FRAME_F_ACTIVE | LJ_FFI_NATIVE_FRAME_F_SUSPENDED | \
   LJ_FFI_NATIVE_FRAME_F_POSTCALL)

/* Generated x64 code publishes its trace number in the carrier-local
** vmstate. Win64 additionally maintains the legacy global mirror because its
** trace code does the same; SysV x64 deliberately leaves that shared mirror
** out of the trace fast path. */
static void ffi_native_trace_vmstate_store(global_State *g, TGState *tg,
					   int32_t vmstate)
{
  lj_tg_vmstate_store_rel(tg, vmstate);
#if LJ_TARGET_X64 && LJ_ABI_WIN
  vmstate_store_rel(g, vmstate);
#else
  UNUSED(g);
#endif
}

static int ffi_native_frame_jitbase_ptr(lua_State *L,
					const LJFFINativeFrame *frame,
					TValue **basep)
{
  TValue *stack, *maxstack;
  uintptr_t stackp, maxstackp;
  uint64_t offset;
  if (!L || !frame || !basep)
    return 0;
  stack = mref_acq(L->stack, TValue);
  maxstack = mref_acq(L->maxstack, TValue);
  offset = lj_ffi_native_frame_jit_base_offset_acq(frame);
  stackp = (uintptr_t)(void *)stack;
  maxstackp = (uintptr_t)(void *)maxstack;
  if (!stack || !maxstack || maxstackp <= stackp ||
      offset >= (uint64_t)(maxstackp - stackp) ||
      (offset % sizeof(TValue)) != 0)
    return 0;
  *basep = (TValue *)(void *)(stackp + (uintptr_t)offset);
  return 1;
}

static int ffi_native_frame_offsets_valid(lua_State *L,
					 const LJFFINativeFrame *frame)
{
  TValue *stack, *maxstack;
  uintptr_t stackp, maxstackp;
  uint64_t extent, root, base, top, jitbase;
  uint64_t prefix = (uint64_t)(1 + LJ_FR2) * sizeof(TValue);
  if (!L || !frame)
    return 0;
  stack = mref_acq(L->stack, TValue);
  maxstack = mref_acq(L->maxstack, TValue);
  stackp = (uintptr_t)(void *)stack;
  maxstackp = (uintptr_t)(void *)maxstack;
  if (!stack || !maxstack || maxstackp <= stackp)
    return 0;
  extent = (uint64_t)(maxstackp - stackp);
  root = lj_ffi_native_frame_root_offset_acq(frame);
  base = lj_ffi_native_frame_base_offset_acq(frame);
  top = lj_ffi_native_frame_top_offset_acq(frame);
  jitbase = lj_ffi_native_frame_jit_base_offset_acq(frame);
  return (extent % sizeof(TValue)) == 0 && root >= prefix &&
    jitbase >= prefix && root <= base && base <= top && jitbase <= top &&
    root < extent && base < extent && top <= extent && jitbase < extent &&
    (root % sizeof(TValue)) == 0 && (base % sizeof(TValue)) == 0 &&
    (top % sizeof(TValue)) == 0 && (jitbase % sizeof(TValue)) == 0;
}

static LJFFINativeFrame *ffi_native_frame_transition_begin(
  TGState *tg, lua_State *L, uint32_t from, uint32_t expected_depth,
  uint64_t *seqp, uint32_t *depthp)
{
  LJFFINativeFrame *frame;
  uint64_t seq;
  uint32_t depth, flags;
  if (LJ_UNLIKELY(!tg || !L || !seqp || !depthp))
    abort();
  seq = lj_ffi_native_frame_sequence_acq(tg);
  depth = lj_ffi_native_frame_depth_acq(tg);
  if (LJ_UNLIKELY((seq & 1u) != 0 || depth == 0 ||
		  depth > LJ_FFI_NATIVE_FRAME_MAX ||
		  (expected_depth != 0 && depth != expected_depth) ||
		  seq > UINT64_MAX - 2u))
    abort();
  frame = &tg->ffi_native_frame[depth - 1u];
  flags = lj_ffi_native_frame_flags_acq(frame);
  if (LJ_UNLIKELY(!ffi_native_frame_valid(frame) ||
		  (flags & LJ_FFI_NATIVE_FRAME_F_STATE) != from ||
		  lj_ffi_native_frame_L_acq(frame) != L))
    abort();
  la_store64_rel(&tg->ffi_native_seq, seq + 1u);
  la_fence_seq();
  *seqp = seq;
  *depthp = depth;
  return frame;
}

static void ffi_native_frame_transition_end(TGState *tg,
					    LJFFINativeFrame *frame,
					    uint64_t seq, uint32_t depth,
					    uint32_t state)
{
  uint32_t flags;
  if (LJ_UNLIKELY(!tg || !frame || (seq & 1u) != 0 ||
		  lj_ffi_native_frame_sequence_acq(tg) != seq + 1u ||
		  lj_ffi_native_frame_depth_acq(tg) != depth ||
		  (state & LJ_FFI_NATIVE_FRAME_F_STATE) != state ||
		  state == 0 || (state & (state - 1u)) != 0))
    abort();
  flags = lj_ffi_native_frame_flags_acq(frame);
  flags &= ~LJ_FFI_NATIVE_FRAME_F_STATE;
  flags |= LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED | state;
  lj_ffi_native_frame_flags_rel(frame, flags);
  la_store64_rel(&tg->ffi_native_seq, seq + 2u);
}

/* A nested generated call is legal only while every older generated call is
** suspended beneath the current callback's Lua execution. */
static int ffi_native_frame_nested_push_allowed(TGState *tg, lua_State *L)
{
  LJFFINativeFrameSnapshot snapshot;
  uint32_t i;
  if (lj_ffi_native_frame_snapshot(tg, &snapshot) !=
      LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE)
    return 0;
  if (snapshot.depth == 0 || snapshot.depth >= LJ_FFI_NATIVE_FRAME_MAX)
    return 0;
  for (i = 0; i < snapshot.depth; i++) {
    const LJFFINativeFrame *frame = &snapshot.frame[i];
    uint32_t flags = lj_ffi_native_frame_flags_acq(frame);
    if (lj_ffi_native_frame_L_acq(frame) != L ||
	(flags & LJ_FFI_NATIVE_FRAME_F_STATE) !=
	  LJ_FFI_NATIVE_FRAME_F_SUSPENDED)
      return 0;
  }
  return lj_ffi_native_frame_sequence_acq(tg) == snapshot.sequence;
}

uint32_t lj_ffi_native_trace_callback_suspend(lua_State *L)
{
  LJFFINativeFrame *frame;
  global_State *g;
  TGState *tg;
  TValue *jitbase;
  uint64_t seq;
  uint32_t depth, flags;
  if (!L || !(g = G(L)) || !(tg = G2TG(g)) ||
      lj_tg_load_cur_L(tg) != L)
    return 0;
  depth = lj_ffi_native_frame_depth_acq(tg);
  if (depth == 0)
    return 0;
  if (LJ_UNLIKELY(depth > LJ_FFI_NATIVE_FRAME_MAX ||
		  (lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0))
    abort();
  frame = &tg->ffi_native_frame[depth - 1u];
  flags = lj_ffi_native_frame_flags_acq(frame);
  /* An interpreted call made by an already-active callback leaves the older
  ** generated continuation suspended and owns no additional trace frame. */
  if ((flags & LJ_FFI_NATIVE_FRAME_F_STATE) ==
      LJ_FFI_NATIVE_FRAME_F_SUSPENDED)
    return 0;
  if (LJ_UNLIKELY((flags & LJ_FFI_NATIVE_FRAME_F_STATE) !=
		  LJ_FFI_NATIVE_FRAME_F_ACTIVE ||
		  lj_ffi_native_frame_func_acq(frame) !=
		    lj_tg_ffi_call_func_acq(tg) ||
		  lj_tg_in_native_acq(tg) != 1u ||
		  lj_tg_vmstate_load_acq(tg) !=
		    (int32_t)lj_ffi_native_frame_trace_no_acq(frame) ||
		  !ffi_native_frame_jitbase_ptr(L, frame, &jitbase) ||
		  lj_tg_load_jit_base(tg) != jitbase))
    abort();
  frame = ffi_native_frame_transition_begin(tg, L,
    LJ_FFI_NATIVE_FRAME_F_ACTIVE, depth, &seq, &depth);
  flags = lj_ffi_native_frame_flags_acq(frame) |
    LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN;
  lj_ffi_native_frame_flags_rel(frame, flags);
  lj_tg_store_jit_base(tg, NULL);
  ffi_native_trace_vmstate_store(g, tg, ~LJ_VMST_INTERP);
  ffi_native_frame_transition_end(tg, frame, seq, depth,
    LJ_FFI_NATIVE_FRAME_F_SUSPENDED);
  return depth;
}

int lj_ffi_native_trace_callback_resume(lua_State *L, uint32_t frame_depth)
{
  LJFFINativeFrame *frame;
  global_State *g;
  TGState *tg;
  TValue *jitbase;
  uint64_t seq;
  uint32_t depth;
  if (!L || !(g = G(L)) || !(tg = G2TG(g)) ||
      lj_tg_load_cur_L(tg) != L)
    return 0;
  /* Drain any request published while Lua owned the callback stack. A request
  ** racing the ACTIVE publication below is instead remotely acknowledged from
  ** the exact pinned frame. */
  (void)lj_safepoint_poll(L);
  if (LJ_UNLIKELY(lj_tg_in_native_acq(tg) != 0 ||
		  lj_tg_load_jit_base(tg) != NULL))
    abort();
  frame = ffi_native_frame_transition_begin(tg, L,
    LJ_FFI_NATIVE_FRAME_F_SUSPENDED, frame_depth, &seq, &depth);
  if (LJ_UNLIKELY(!ffi_native_frame_jitbase_ptr(L, frame, &jitbase)))
    abort();
  lj_tg_store_jit_base(tg, jitbase);
  ffi_native_trace_vmstate_store(g, tg,
    (int32_t)lj_ffi_native_frame_trace_no_acq(frame));
  ffi_native_frame_transition_end(tg, frame, seq, depth,
    LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  /* Remote acknowledgement may use the exact even ACTIVE frame only after
  ** every frame and JIT-base publication is complete. */
  lj_tg_in_native_rel(tg, 1u);
  return 1;
}

int lj_ffi_native_trace_callback_unwind(lua_State *L, uint32_t frame_depth)
{
  LJFFINativeFrame *frame;
  global_State *g;
  TGState *tg;
  TValue *jitbase;
  uint64_t seq;
  uint32_t depth;
  if (!L || !(g = G(L)) || !(tg = G2TG(g)) ||
      lj_tg_load_cur_L(tg) != L)
    return 0;
  if (LJ_UNLIKELY(lj_tg_in_native_acq(tg) != 0 ||
		  lj_tg_load_jit_base(tg) != NULL))
    abort();
  frame = ffi_native_frame_transition_begin(tg, L,
    LJ_FFI_NATIVE_FRAME_F_SUSPENDED, frame_depth, &seq, &depth);
  if (LJ_UNLIKELY(!ffi_native_frame_jitbase_ptr(L, frame, &jitbase)))
    abort();
  /* External unwind still crosses the exact generated body. Republish its JIT
  ** base and transfer the pin to ordinary trace-exit cleanup. */
  lj_tg_store_jit_base(tg, jitbase);
  ffi_native_trace_vmstate_store(g, tg,
    (int32_t)lj_ffi_native_frame_trace_no_acq(frame));
  ffi_native_frame_transition_end(tg, frame, seq, depth,
    LJ_FFI_NATIVE_FRAME_F_POSTCALL);
  ccallback_slot_rel(&tg->cb,
    lj_ffi_native_frame_old_callback_slot_acq(frame));
  lj_tg_ffi_call_func_rel(tg, lj_ffi_native_frame_old_func_acq(frame));
  ccallback_native_had_stopreq_rel(
    &tg->cb, lj_ffi_native_frame_old_stopreq_acq(frame));
  return 1;
}

/* Copy and validate the non-dereferencing portion of the generated native
** certificate. require_native is used before request consumption; afterwards
** the owner may already have closed native state and be parked on the still-
** held poll without changing the exact even frame. */
static int ffi_native_trace_remote_snapshot(
  TGState *tg, LJFFINativeFrameSnapshot *snapshot, int require_native)
{
  LJFFINativeFrameSnapshot copy;
  lua_State *L;
  TValue *jitbase;
  uint32_t i;
  if (!tg || (require_native ? lj_tg_in_native_acq(tg) != 1u :
			       lj_tg_in_native_acq(tg) > 1u) ||
      lj_ffi_native_frame_snapshot(tg, &copy) !=
        LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE ||
      copy.depth == 0)
    return 0;
  L = lj_tg_load_cur_L(tg);
  if (!L || mref_acq(L->glref, global_State) != tg->gl ||
      !lj_tg_owns_state_acq(tg, L))
    return 0;
  for (i = 0; i < copy.depth; i++) {
    const LJFFINativeFrame *frame = &copy.frame[i];
    uint32_t state = lj_ffi_native_frame_flags_acq(frame) &
      LJ_FFI_NATIVE_FRAME_F_STATE;
    uint32_t expected = i + 1u == copy.depth ?
      LJ_FFI_NATIVE_FRAME_F_ACTIVE : LJ_FFI_NATIVE_FRAME_F_SUSPENDED;
    if (state != expected || lj_ffi_native_frame_L_acq(frame) != L ||
	!ffi_native_frame_offsets_valid(L, frame))
      return 0;
  }
  if (!ffi_native_frame_jitbase_ptr(L,
      &copy.frame[copy.depth - 1u], &jitbase) ||
      jitbase != lj_tg_load_jit_base(tg) ||
      lj_tg_vmstate_load_acq(tg) !=
        (int32_t)lj_ffi_native_frame_trace_no_acq(
          &copy.frame[copy.depth - 1u]) ||
      lj_tg_ffi_call_func_acq(tg) !=
        lj_ffi_native_frame_func_acq(&copy.frame[copy.depth - 1u]))
    return 0;
  la_fence_acq();
  if ((require_native ? lj_tg_in_native_acq(tg) != 1u :
			lj_tg_in_native_acq(tg) > 1u) ||
      lj_ffi_native_frame_sequence_acq(tg) != copy.sequence)
    return 0;
  if (snapshot)
    *snapshot = copy;
  return 1;
}

/* Non-dereferencing prefilter only. A remote trace action must still prove
** TraceVec identity and a nonzero exact pin after consuming the request. */
int lj_ffi_native_trace_remote_shape_allowed(TGState *tg)
{
  return ffi_native_trace_remote_snapshot(tg, NULL, 1);
}

/* An ACTIVE generated frame which has closed native state is the one trace-exit
** case that must wait on a remotely consumed poll. Ordinary exit C frames have
** no exact native-frame stack and retain the historical deadlock-avoidance
** bypass. */
int lj_ffi_native_trace_consumed_poll_wait_required(TGState *tg)
{
  return ffi_native_trace_remote_snapshot(tg, NULL, 0);
}

/* Exact post-consumption proof. Slot lookup precedes every trace-body access,
** so a poisoned frame pointer is compared but never dereferenced. Retirement
** may have cleared T->traceno while retaining this reserved TraceVec slot. */
int lj_ffi_native_trace_remote_certify(TGState *tg, uint64_t *sequencep)
{
  LJFFINativeFrameSnapshot snapshot;
  global_State *g;
  jit_State *J;
  uint32_t i;
  int certified = 0;
  if (!tg || !(g = tg->gl) || lj_tg_poll_acq(tg) == 0 ||
      lj_tg_reqmask_acq(tg) != 0 ||
      !ffi_native_trace_remote_snapshot(tg, &snapshot, 0) ||
      !lj_gc2_smr_read_try(g))
    return 0;
  J = G2J(g);
  for (i = 0; i < snapshot.depth; i++) {
    const LJFFINativeFrame *frame = &snapshot.frame[i];
    uint32_t raw = lj_ffi_native_frame_trace_no_acq(frame);
    uint32_t required_pins = 1u;
    uint32_t j;
    TraceNo traceno = (TraceNo)raw;
    GCtrace *trace = lj_ffi_native_frame_trace_acq(frame);
    GCtrace *slot;
    if ((uint32_t)traceno != raw)
      goto out;
    slot = traceref_safe(J, traceno);
    if (!slot || slot != trace)
      goto out;
    /* Recursive callback reentry may put the same exact trace in this frame
    ** stack more than once. Require one live lease for every local occurrence,
    ** not merely one lease for the shared body. */
    for (j = 0; j < i; j++) {
      const LJFFINativeFrame *older = &snapshot.frame[j];
      if (lj_ffi_native_frame_trace_acq(older) == trace &&
	  lj_ffi_native_frame_trace_no_acq(older) == raw)
	required_pins++;
    }
    if (trace_native_pins_acq(slot) < required_pins)
      goto out;
  }
  la_fence_acq();
  if (lj_tg_poll_acq(tg) == 0 ||
      lj_ffi_native_frame_sequence_acq(tg) != snapshot.sequence)
    goto out;
  certified = 1;
  if (sequencep)
    *sequencep = snapshot.sequence;
out:
  lj_gc2_smr_read_leave(g);
  return certified;
}

/* The executing trace's published jit_base is the independent lifetime proof
** required before the first T read. SMR then validates the exact public slot
** while native-pin admission races retirement without waiting. */
static int ccall_jit_trace_pin(global_State *g, GCtrace *T,
			       TraceNo *tracenop)
{
  jit_State *J = G2J(g);
  TraceNo traceno;
  GCtrace *slot;
  int pinned = 0;
  if (!T || !tracenop)
    return 0;
  traceno = trace_traceno_acq(T);
  if (traceno == 0 || !lj_gc2_smr_read_try(g))
    return 0;
  slot = traceref_safe(J, traceno);
  if (slot == T && trace_traceno_acq(slot) == traceno &&
      lj_trace_native_pin(slot)) {
    /* Retirement may close admission and clear T->traceno after the pin. Its
    ** slot must nevertheless remain reserved by exact body identity. */
    slot = traceref_safe(J, traceno);
    if (slot == T && trace_native_pins_acq(slot) != 0) {
      *tracenop = traceno;
      pinned = 1;
    } else {
      lj_trace_native_unpin(g, T);
    }
  }
  lj_gc2_smr_read_leave(g);
  return pinned;
}

int lj_ffi_native_trace_enter(lua_State *L, GCtrace *T, void *func,
			      GCcdata *result_root)
{
  CCallErrorState err;
  CCallJITGeometry geo;
  LJFFINativeFrame frame;
  global_State *g = NULL;
  TGState *tg = NULL;
  TGState *current = NULL;
  CCallbackRuntime *cb;
  TraceNo traceno;
  uint32_t depth;
  uint8_t had_stopreq;
  int ok = 0;
  /* This must remain the first operation: entry validation and SMR admission
  ** must not replace the errno/LastError pair observed by the foreign call. */
  ccall_error_save(&err);
  if (!L)
    goto out;
  g = G(L);
  if (!g)
    goto out;
  /* IR_XSAVE writes through RID_DISPATCH, which the x64 VM loads directly
  ** from L->tg_hint. Require that staged/dispatch carrier to be the current
  ** TLS carrier before reading or clearing any owner-private word. */
  tg = L->tg_hint;
  current = G2TG(g);
  if (!tg || tg != current || lj_tg_load_cur_L(tg) != L)
    goto out;
  if (!T || !func)
    goto out;
  /* Capacity is the expected nonexceptional failure. Check it before the raw
  ** trace constant so a full stack always side-exits without touching it. */
  depth = lj_ffi_native_frame_depth_acq(tg);
  if (LJ_UNLIKELY(depth > LJ_FFI_NATIVE_FRAME_MAX ||
      (lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0))
    abort();
  /* Older generated calls may remain pinned beneath callback Lua execution,
  ** but every such frame must be SUSPENDED before a nested CALLXS can publish
  ** a new ACTIVE top. */
  if (lj_tg_in_native_acq(tg) != 0 ||
      (depth != 0 && !ffi_native_frame_nested_push_allowed(tg, L)))
    goto out;
  if (!ccall_jit_geometry(L, tg, &geo) ||
      !ccall_jit_trace_pin(g, T, &traceno))
    goto out;
  /* The frame trace identity is also the exact positive vmstate restored after
  ** callback Lua returns. Never publish a frame if generated entry and the
  ** pinned TraceVec slot disagree about that identity. */
  if (LJ_UNLIKELY(lj_tg_vmstate_load_acq(tg) != (int32_t)traceno)) {
    lj_trace_native_unpin(g, T);
    goto out;
  }

  cb = &tg->cb;
  had_stopreq = (uint8_t)(lj_safepoint_had_stopreq(L) != 0);
  memset(&frame, 0, sizeof(frame));
  lj_ffi_native_frame_trace_rel(&frame, T);
  lj_ffi_native_frame_L_rel(&frame, L);
  lj_ffi_native_frame_func_rel(&frame, func);
  lj_ffi_native_frame_result_root_rel(&frame, result_root);
  lj_ffi_native_frame_old_func_rel(&frame, lj_tg_ffi_call_func_acq(tg));
  lj_ffi_native_frame_root_offset_rel(&frame, geo.root_offset);
  lj_ffi_native_frame_base_offset_rel(&frame, geo.base_offset);
  lj_ffi_native_frame_top_offset_rel(&frame, geo.top_offset);
  lj_ffi_native_frame_jit_base_offset_rel(&frame, geo.jitbase_offset);
  lj_ffi_native_frame_entry_exit_epoch_rel(
    &frame, lj_tg_hs_epoch_ack_acq(tg));
  lj_ffi_native_frame_trace_no_rel(&frame, (uint32_t)traceno);
  lj_ffi_native_frame_old_callback_slot_rel(
    &frame, ccallback_slot_acq(cb));
  lj_ffi_native_frame_flags_rel(&frame,
    LJ_FFI_NATIVE_FRAME_F_SYNCHRONIZED | LJ_FFI_NATIVE_FRAME_F_ACTIVE);
  lj_ffi_native_frame_old_stopreq_rel(
    &frame, ccallback_native_had_stopreq_acq(cb));
  lj_ffi_native_frame_had_stopreq_rel(&frame, had_stopreq);

  if (!lj_ffi_native_frame_push(tg, &frame)) {
    lj_trace_native_unpin(g, T);
    goto out;
  }
  /* The exact even frame precedes every remotely acknowledged native state.
  ** Successful entry consumes staging here after publication; every ordinary
  ** earlier rejection consumes it at the common return. */
  ccall_jit_xsave_clear(tg);
  ccallback_slot_rel(cb, ~0u);
  lj_tg_ffi_call_func_rel(tg, func);
  ccallback_native_had_stopreq_rel(cb, had_stopreq);
  lj_native_enter(tg);
  ok = 1;
out:
  /* A valid current-carrier route owns the staged words even when admission
  ** rejects before geometry, pinning or frame publication. The equality checks
  ** above prevent a stale hint from mutating a different carrier. */
  if (!ok && tg != NULL && tg == current &&
      lj_tg_load_cur_L(tg) == L)
    ccall_jit_xsave_clear(tg);
  ccall_error_restore(&err);
  return ok;
}

uint32_t lj_ffi_native_trace_leave(lua_State *L)
{
  CCallErrorState err;
  LJFFINativeFrame frame;
  global_State *g;
  TGState *tg, *current;
  CCallbackRuntime *cb;
  LJFFINativeFrame *src;
  GCtrace *T;
  uint64_t seq;
  uint32_t actions, depth, finish_depth;
  MSize callback_slot;
  uint8_t had_stopreq;
  int callback_seen, force, retain, stop;
  /* This must remain the first operation after the foreign return. */
  ccall_error_save(&err);
  if (LJ_UNLIKELY(L == NULL))
    abort();
  g = G(L);
  tg = L->tg_hint;
  current = g ? G2TG(g) : NULL;
  if (LJ_UNLIKELY(tg == NULL || tg != current || g == NULL ||
      lj_tg_load_cur_L(tg) != L))
    abort();
  depth = lj_ffi_native_frame_depth_acq(tg);
  if (LJ_UNLIKELY(depth == 0 || depth > LJ_FFI_NATIVE_FRAME_MAX ||
      (lj_ffi_native_frame_sequence_acq(tg) & 1u) != 0))
    abort();
  ffi_native_frame_copy(&frame, &tg->ffi_native_frame[depth - 1u]);
  if (LJ_UNLIKELY(!ffi_native_frame_valid(&frame) ||
      lj_ffi_native_frame_L_acq(&frame) != L))
    abort();
  if (LJ_UNLIKELY(lj_tg_in_native_acq(tg) != 1u))
    abort();  /* Nested leave lacks the consumed-poll stability certificate. */
  T = lj_ffi_native_frame_trace_acq(&frame);
  had_stopreq = lj_ffi_native_frame_had_stopreq_acq(&frame);

  /* The certified scanner may be reading this same-even frame while a remote
  ** leader owns its consumed poll. Do not change any payload or pin until
  ** native_leave has closed native state and returned from that wait. */
  actions = lj_native_leave(L);
#if defined(LJ_XSAVE_TEST_HELPERS)
  {
    /* Deterministic injection point after the native poll and before the
    ** fresh-STOPREQ decision. Production builds contain no hook load/call. */
    LJFFINativeTraceLeaveHook hook =
      la_loadfunc_acq(&ffi_native_trace_leave_hook);
    if (hook)
      hook(tg);
  }
#endif
  /* This predicate may perform one final owner poll, but never throws. If it
  ** reports a fresh STOPREQ, no generated post-call guard can be relied upon:
  ** the frame and pin must be released before checkstop unwinds the trace. */
  stop = lj_safepoint_fresh_stopreq(L, actions, had_stopreq);
  src = ffi_native_frame_finish_begin(tg, &frame, &seq, &finish_depth);
#if defined(LJ_XSAVE_TEST_HELPERS)
  {
    LJFFINativeTraceFinishHook hook =
      la_loadfunc_acq(&ffi_native_trace_finish_hook);
    if (hook)
      hook(tg);
  }
#endif
  cb = &tg->cb;
  callback_slot = ccallback_slot_acq(cb);
  callback_seen = callback_slot != (MSize)~0u ||
    (lj_ffi_native_frame_flags_acq(&frame) &
      LJ_FFI_NATIVE_FRAME_F_CALLBACK_SEEN) != 0;
  /* The sequence is odd before this final decision. A remote reader therefore
  ** cannot certify the old ACTIVE frame and race a zero-pin ordinary finish. */
  force = actions != 0 || callback_seen || lj_tg_poll_acq(tg) != 0 ||
    lj_tg_reqmask_acq(tg) != 0 ||
    lj_tg_hs_epoch_ack_acq(tg) !=
      lj_ffi_native_frame_entry_exit_epoch_acq(&frame);
  retain = force && !stop;

  ccallback_slot_rel(cb,
    lj_ffi_native_frame_old_callback_slot_acq(&frame));
  lj_tg_ffi_call_func_rel(tg, lj_ffi_native_frame_old_func_acq(&frame));
  ccallback_native_had_stopreq_rel(
    cb, lj_ffi_native_frame_old_stopreq_acq(&frame));
  ffi_native_frame_finish_end(tg, src, seq, finish_depth, retain,
			      callback_seen);
  if (!retain) {
    /* No stable frame names T after the final even publication. jit_base is
    ** still the conservative independent trace lifetime proof. */
    lj_trace_native_unpin(g, T);
  }

  ccall_error_restore(&err);
  if (stop)
    lj_safepoint_checkstop(L, actions | LJ_GC2_HS_STOPREQ);
  ccall_error_restore(&err);
  return actions | (retain ? LJ_FFI_NATIVE_LEAVE_FORCE_EXIT : 0);
}

/* Release the exact POSTCALL lifetime transferred to an unconditional trace
** exit. This is called after protected snapshot restore on both its success and
** error paths. Absence is ordinary: almost every trace exit is unrelated to a
** generic foreign call. A present POSTCALL frame must match exactly. */
int lj_ffi_native_trace_exit_cleanup(lua_State *L, GCtrace *T,
				     uint32_t traceno)
{
  CCallErrorState err;
  LJFFINativeFrame frame;
  global_State *g;
  TGState *tg;
  CCallbackRuntime *cb;
  uint64_t seq;
  uint32_t depth, flags;
  int cleaned = 0;
  ccall_error_save(&err);
  if (LJ_UNLIKELY(L == NULL || T == NULL || traceno == 0))
    goto out;
  g = G(L);
  if (LJ_UNLIKELY(g == NULL))
    abort();
  /* Trace exit runs on the current carrier. L->tg_hint may describe an older
  ** carrier after migration, so use the same TLS/fallback route as unwind. */
  tg = G2TG(g);
  if (LJ_UNLIKELY(tg == NULL || lj_tg_load_cur_L(tg) != L))
    abort();
  seq = lj_ffi_native_frame_sequence_acq(tg);
  depth = lj_ffi_native_frame_depth_acq(tg);
  if (LJ_UNLIKELY((seq & 1u) != 0 ||
		  depth > LJ_FFI_NATIVE_FRAME_MAX))
    abort();
  if (depth == 0)
    goto out;
  ffi_native_frame_copy(&frame, &tg->ffi_native_frame[depth - 1u]);
  if (LJ_UNLIKELY(!ffi_native_frame_valid(&frame)))
    abort();
  flags = lj_ffi_native_frame_flags_acq(&frame);
  if ((flags & LJ_FFI_NATIVE_FRAME_F_STATE) !=
      LJ_FFI_NATIVE_FRAME_F_POSTCALL)
    goto out;
  if (LJ_UNLIKELY(lj_ffi_native_frame_L_acq(&frame) != L ||
	  lj_ffi_native_frame_trace_acq(&frame) != T ||
	  lj_ffi_native_frame_trace_no_acq(&frame) != traceno ||
	  lj_tg_in_native_acq(tg) != 0))
    abort();
  /* Normal leave already restored these mirrors. Callback error unwind bypasses
  ** leave, so exact cleanup performs the same idempotent restoration. */
  cb = &tg->cb;
  ccallback_slot_rel(cb,
    lj_ffi_native_frame_old_callback_slot_acq(&frame));
  lj_tg_ffi_call_func_rel(tg, lj_ffi_native_frame_old_func_acq(&frame));
  ccallback_native_had_stopreq_rel(
    cb, lj_ffi_native_frame_old_stopreq_acq(&frame));
  lj_ffi_native_frame_pop(tg, NULL);
  lj_trace_native_unpin(g, T);
  cleaned = 1;
out:
  ccall_error_restore(&err);
  return cleaned;
}
#endif

void lj_ccall_native_save(lua_State *L, CCallNativeState *st)
{
  CCallErrorState err;
  TGState *tg;
  CCallbackRuntime *cb;
  /* This is deliberately first: interpreted argument conversion may allocate
  ** before native_enter(), but the foreign function must still observe the
  ** caller's errno/LastError pair. The same fields are replaced by the
  ** immediate post-call snapshot in native_leave(). */
  ccall_error_save(&err);
  tg = L2TG(L);
  cb = &tg->cb;
  st->tg = tg;
  st->old_ffi_call_func = lj_tg_ffi_call_func_acq(tg);
  st->old_callback_slot = ccallback_slot_acq(cb);
  st->old_native_had_stopreq = ccallback_native_had_stopreq_acq(cb);
  st->had_stopreq = 0;
  ccall_error_publish(st, &err);
}

void lj_ccall_native_enter(lua_State *L, CCallNativeState *st, void *func)
{
  CCallErrorState err;
  TGState *tg;
  CCallbackRuntime *cb;
  int had_stopreq;
  /* native_save() precedes interpreted argument conversion. Restore that
  ** exact caller state only after all entry bookkeeping is complete. */
  ccall_error_from_native(&err, st);
  tg = st->tg;
  if (LJ_UNLIKELY(tg == NULL || L2TG(L) != tg))
    abort();  /* Corrupt internal native frame; no recoverable call state. */
  cb = &tg->cb;
  ccallback_slot_rel(cb, ~0u);
  lj_tg_ffi_call_func_rel(tg, func);
  had_stopreq = lj_safepoint_had_stopreq(L);
  st->had_stopreq = had_stopreq;
  ccallback_native_had_stopreq_rel(cb, (uint8_t)had_stopreq);
  lj_native_enter(tg);
  ccall_error_restore(&err);
}

uint32_t lj_ccall_native_leave(lua_State *L, CTState *cts,
			       CCallNativeState *st, void *func)
{
  CCallErrorState err;
  uint32_t actions;
  TGState *tg;
  CCallbackRuntime *cb;
  MSize callback_slot;
  /* First operation after the foreign return. Publish it to the stack-local
  ** call frame before leave/blacklist code can poll, yield or throw. */
  ccall_error_save(&err);
  ccall_error_publish(st, &err);
  tg = st->tg;
  if (LJ_UNLIKELY(tg == NULL || L2TG(L) != tg))
    abort();  /* Corrupt internal native frame; no recoverable call state. */
  actions = lj_native_leave(L);
  cb = &tg->cb;
  callback_slot = ccallback_slot_acq(cb);
  /* Restore every surrounding-call mirror before callback-blacklist retry
  ** paths can yield and service a safepoint. Nested calls therefore never
  ** expose the just-finished frame as the current one. */
  ccallback_slot_rel(cb, st->old_callback_slot);
  lj_tg_ffi_call_func_rel(tg, st->old_ffi_call_func);
  ccallback_native_had_stopreq_rel(cb, st->old_native_had_stopreq);
  ccall_error_restore(&err);
  /* Blacklist function that called a callback. The table's bounded CAS retry
  ** may enter native state and poll, so bracket it with the foreign result. */
  if (callback_slot != (MSize)~0u)
    lj_ctype_cb_blacklist(L, cts, func);
  ccall_error_restore(&err);
  return actions;
}

void lj_ccall_native_checkstop(lua_State *L, uint32_t actions,
			       const CCallNativeState *st)
{
  CCallErrorState err;
  ccall_error_from_native(&err, st);
  ccall_error_restore(&err);
  lj_safepoint_checkstop_fresh(L, actions, st->had_stopreq);
  ccall_error_restore(&err);
}

void *lj_ffi_jit_memcpy(lua_State *L, void *dp, const void *sp, CTSize len)
{
  uint32_t actions;
  int had_stopreq = lj_safepoint_had_stopreq(L);
  lj_native_enter(L2TG(L));
  memcpy(dp, sp, (size_t)len);
  actions = lj_native_leave(L);
  lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
  return dp;
}

void *lj_ffi_jit_memset(lua_State *L, void *dp, int32_t fill, CTSize len)
{
  uint32_t actions;
  int had_stopreq = lj_safepoint_had_stopreq(L);
  lj_native_enter(L2TG(L));
  memset(dp, fill, (size_t)len);
  actions = lj_native_leave(L);
  lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
  return dp;
}

size_t lj_ffi_jit_strlen(lua_State *L, const char *p)
{
  uint32_t actions;
  int had_stopreq = lj_safepoint_had_stopreq(L);
  size_t len;
  lj_native_enter(L2TG(L));
  len = strlen(p);
  actions = lj_native_leave(L);
  lj_safepoint_checkstop_fresh(L, actions, had_stopreq);
  return len;
}

/* Call C function. */
int lj_ccall_func(lua_State *L, GCcdata *cd)
{
  CCallNativeState native;
  CTState *cts;
  CTypeID id;
  CType ctsnap, *ct;
  CTInfo info;
  CTSize sz = CTSIZE_PTR;
  /* Preserve caller error state before any snapshot retry can yield. */
  lj_ccall_native_save(L, &native);
  cts = ctype_cts(L);
  ct = ccall_rawid_wait(L, cts, cd->ctypeid, &id, &ctsnap);
  info = ctype_info_acq(ct);
  if (ctype_isptr(info)) {
    sz = ctype_size_acq(ct);
    ct = ccall_rawid_wait(L, cts, ctype_cid(info), &id, &ctsnap);
    info = ctype_info_acq(ct);
  }
  if (ctype_isfunc(info)) {
    CCallState cc;
    uint32_t actions;
    ptrdiff_t calltop;
    MSize nroot;
    int gcsteps, ret, has_result_root = 0;
    void *func;
    /* Every pass-by-reference temporary consumes one GPR or one stack slot;
    ** FPR-only arguments never create one. Thus CCALL_NUM_GPR plus
    ** CCALL_NUM_STACK bounds all argument roots on every supported ABI. The
    ** first extra slot is the optional aggregate-result root. Another argument
    ** slot can hold the single over-limit temporary created before ABI
    ** placement detects CCALL_SIZE_STACK exhaustion and throws. Targets which
    ** pass aggregates directly in CCallState have a compile-time zero argument
    ** bound. Cap the nonzero ABI bound by the actual argument count so an
    ** ordinary small call does not reserve the maximum call-state footprint. */
    nroot = (MSize)(L->top - (L->base+1));
    if (nroot > CCALL_ARG_ROOT_BOUND)
      nroot = CCALL_ARG_ROOT_BOUND;
    lj_state_checkstack(L, nroot + 1u);
    calltop = savestack(L, L->top);
    cc.func = (void (*)(void))cdata_getptr(cdataptr(cd), sz);
    func = (void *)cc.func;
    gcsteps = ccall_set_args(L, cts, ct, &cc, &has_result_root);
    lj_ccall_native_enter(L, &native, func);
    lj_vm_ffi_call(&cc);
    actions = lj_ccall_native_leave(L, cts, &native, func);
    ct = ccall_ctype_snapshot_wait(L, cts, id, &ctsnap);
    /* Drop argument temporaries only after the foreign call and the final
    ** throwing type snapshot. Keep an aggregate result as the sole appended
    ** slot so ccall_get_results() retains its established top-1 contract. */
    L->top = restorestack(L, calltop) + has_result_root;
    gcsteps += ccall_get_results(L, cts, ct, &cc, &ret);
    /* Scalar cdata results are constructed directly into top-1. Publish the
    ** final result uniformly before STOPREQ handling or GC accounting can
    ** poll, throw or start another cycle. */
    if (ret)
      lj_state_stack_pubtv(L, L, L->top-1);
#if LJ_TARGET_X86 && LJ_ABI_WIN
    /* Automatically detect __stdcall and fix up C function declaration. */
    {
      CType *livect = ctype_get(cts, id);
      info = ctype_info_acq(livect);
      if (cc.spadj && ctype_cconv(info) == CTCC_CDECL) {
	CTF_INSERT(livect->info, CCONV, CTCC_STDCALL);
	lj_trace_abort(G(L));
      }
    }
#endif
    lj_ccall_native_checkstop(L, actions, &native);
    while (gcsteps-- > 0)
      lj_gc_check(L);
    {
      CCallErrorState err;
      ccall_error_from_native(&err, &native);
      ccall_error_restore(&err);
    }
    return ret;
  }
  {
    CCallErrorState err;
    ccall_error_from_native(&err, &native);
    ccall_error_restore(&err);
  }
  return -1;  /* Not a function. */
}

#endif
