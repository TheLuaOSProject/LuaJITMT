/*
** SIMD vector semantics (interpreter reference implementation).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_SIMD_H
#define _LJ_SIMD_H

#include "lj_obj.h"

#if LJ_HASFFI

#include "lj_ctype.h"

/* Maximum vector width in bytes that the JIT backend compiles natively. */
#if LJ_HASJIT && LJ_TARGET_X64
#define LJ_SIMD_JITSIZE		32
#else
#define LJ_SIMD_JITSIZE		0
#endif

/* Binary element-wise operations. ORDER VOP */
typedef enum {
  VOP_ADD, VOP_SUB, VOP_MUL, VOP_DIV,
  VOP_AND, VOP_OR, VOP_XOR, VOP_ANDN,
  VOP_MIN, VOP_MAX,
  VOP_ADDS, VOP_SUBS,
  VOP__MAX
} VecBinOp;

/* Element-wise comparisons. ORDER VCMP */
typedef enum {
  VCMP_EQ, VCMP_NE, VCMP_LT, VCMP_LE, VCMP_GT, VCMP_GE,
  VCMP__MAX
} VecCmpOp;

/* Unary element-wise operations. ORDER VUN */
typedef enum {
  VUN_NEG, VUN_NOT, VUN_ABS, VUN_SQRT,
  VUN__MAX
} VecUnOp;

/* Element-wise shifts. ORDER VSH */
typedef enum {
  VSH_SHL, VSH_SHR, VSH_SAR,
  VSH__MAX
} VecShiftOp;

/* Horizontal reductions. ORDER VRD */
typedef enum {
  VRD_SUM, VRD_MIN, VRD_MAX,
  VRD__MAX
} VecReduceOp;

/* Rounding modes, values match the SSE4.1 ROUNDPS immediate. ORDER VRND */
typedef enum {
  VRND_NEAREST, VRND_FLOOR, VRND_CEIL, VRND_TRUNC,
  VRND__MAX
} VecRoundMode;

/* Returns 0 if the operation is not defined for this element kind. */
LJ_FUNC int lj_simd_binop(void *dp, const void *ap, const void *bp,
			  const CTVecInfo *vi, uint32_t op);
LJ_FUNC int lj_simd_unop(void *dp, const void *ap, const CTVecInfo *vi,
			 uint32_t op);
LJ_FUNC int lj_simd_shift(void *dp, const void *ap, const CTVecInfo *vi,
			  uint32_t op, int32_t n);
LJ_FUNC void lj_simd_cmp(void *dp, const void *ap, const void *bp,
			 const CTVecInfo *vi, uint32_t op);
LJ_FUNC int lj_simd_equal(const void *ap, const void *bp, const CTVecInfo *vi);
LJ_FUNC int lj_simd_round(void *dp, const void *ap, const CTVecInfo *vi,
			  uint32_t mode);
LJ_FUNC uint32_t lj_simd_movemask(const void *ap, const CTVecInfo *vi);
LJ_FUNC int lj_simd_reduce(void *dp, const void *ap, const CTVecInfo *vi,
			   uint32_t op);
LJ_FUNC void lj_simd_shuffle(void *dp, const void *ap, const void *bp,
			     const CTVecInfo *vi, const uint8_t *idx);
LJ_FUNC void lj_simd_permute(void *dp, const void *ap, const void *ip,
			     const CTVecInfo *vi);
LJ_FUNC int lj_simd_shiftv(void *dp, const void *ap, const void *np,
			   const CTVecInfo *vi, uint32_t op);
LJ_FUNC int lj_simd_fma(void *dp, const void *ap, const void *bp,
			const void *cp, const CTVecInfo *vi);
LJ_FUNC int lj_simd_mulhi(void *dp, const void *ap, const void *bp,
			  const CTVecInfo *vi);
LJ_FUNC void lj_simd_select(void *dp, const void *mp, const void *ap,
			    const void *bp, CTSize size);
LJ_FUNC void lj_simd_splat(void *dp, const void *ep, const CTVecInfo *vi);
LJ_FUNC int lj_simd_convert(void *dp, const CTVecInfo *dvi,
			    const void *sp, const CTVecInfo *svi);

/* Signed integer element ctype ID for a mask of the same lane width. */
LJ_FUNC CTypeID lj_simd_masktype(CTState *cts, const CTVecInfo *vi);

#endif

#endif
