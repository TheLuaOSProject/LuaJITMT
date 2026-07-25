/*
** SIMD vector semantics (interpreter reference implementation).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** This file defines the *meaning* of every vector operation. The JIT backend
** must produce bit-identical results; the tests in test/simd compare both.
**
** All integer lane arithmetic is done in unsigned types and wraps around,
** which avoids C undefined behaviour and matches the x86 packed instructions.
** Note that "unsigned lane type" is not enough on its own: anything narrower
** than int is promoted *to int* by the usual arithmetic conversions, so e.g.
** 65535 * 65535 on uint16_t lanes would overflow a signed int. The operands
** are therefore widened to an explicit unsigned type first.
*/

#define lj_simd_c
#define LUA_CORE

#include "lj_obj.h"

#if LJ_HASFFI

#include <math.h>
#include <string.h>

#include "lj_ctype.h"
#include "lj_simd.h"

/* -- Helpers ------------------------------------------------------------- */

/* Iterate over all lanes of a binary op, computing in type OTY. */
#define VEC_BINO(TY, OTY, EXPR) \
  { TY *d = (TY *)dp; const TY *a = (const TY *)ap; const TY *b = (const TY *)bp; \
    uint32_t i, n = vi->lanes; \
    for (i = 0; i < n; i++) { OTY x = a[i], y = b[i]; d[i] = (TY)(EXPR); } \
    return 1; }

#define VEC_BIN(TY, EXPR)	VEC_BINO(TY, TY, EXPR)

/* Iterate over all lanes of a unary op, computing in type OTY. */
#define VEC_UNO(TY, OTY, EXPR) \
  { TY *d = (TY *)dp; const TY *a = (const TY *)ap; \
    uint32_t i, n = vi->lanes; \
    for (i = 0; i < n; i++) { OTY x = a[i]; d[i] = (TY)(EXPR); } \
    return 1; }

#define VEC_UN(TY, EXPR)	VEC_UNO(TY, TY, EXPR)

/* Dispatch over the integer lane widths, computing in an unsigned type that
** is never promoted to a signed int.
*/
#define VEC_BIN_UINT(EXPR) \
  switch (vi->esize) { \
  case 1: VEC_BINO(uint8_t, uint32_t, EXPR) \
  case 2: VEC_BINO(uint16_t, uint32_t, EXPR) \
  case 4: VEC_BINO(uint32_t, uint32_t, EXPR) \
  default: VEC_BINO(uint64_t, uint64_t, EXPR) \
  }

/* Dispatch over the signed integer lane widths. */
#define VEC_BIN_SINT(EXPR) \
  switch (vi->esize) { \
  case 1: VEC_BIN(int8_t, EXPR) \
  case 2: VEC_BIN(int16_t, EXPR) \
  case 4: VEC_BIN(int32_t, EXPR) \
  default: VEC_BIN(int64_t, EXPR) \
  }

/* Reapply the sign of s to the magnitude m (handles -0.0 correctly). */
static double vec_copysign(double m, double s)
{
  union { double d; uint64_t u; } um, us;
  um.d = m; us.d = s;
  um.u = (um.u & U64x(7fffffff,ffffffff)) | (us.u & U64x(80000000,00000000));
  return um.d;
}

/* Round to nearest, ties to even. Matches ROUNDPS/ROUNDPD mode 0. */
static double vec_roundeven(double x)
{
  double a = fabs(x);
  if (a < 4503599627370496.0) {  /* 2^52: everything above is an integer. */
    double m = 4503599627370496.0;
    /* The default FP rounding mode is nearest-even, so this rounds correctly. */
    double y = (a + m) - m;
    return vec_copysign(y, x);
  }
  return x;
}

static double vec_round1(double x, uint32_t mode)
{
  switch (mode) {
  case VRND_FLOOR: return floor(x);
  case VRND_CEIL: return ceil(x);
  case VRND_TRUNC: return vec_copysign(floor(fabs(x)), x);
  default: return vec_roundeven(x);
  }
}

/* -- Binary element-wise operations -------------------------------------- */

int lj_simd_binop(void *dp, const void *ap, const void *bp,
		  const CTVecInfo *vi, uint32_t op)
{
  uint32_t k = vi->kind;
  switch (op) {
  case VOP_ADD:
    if (k == VECK_F32) VEC_BIN(float, x + y)
    if (k == VECK_F64) VEC_BIN(double, x + y)
    VEC_BIN_UINT(x + y)
  case VOP_SUB:
    if (k == VECK_F32) VEC_BIN(float, x - y)
    if (k == VECK_F64) VEC_BIN(double, x - y)
    VEC_BIN_UINT(x - y)
  case VOP_MUL:
    if (k == VECK_F32) VEC_BIN(float, x * y)
    if (k == VECK_F64) VEC_BIN(double, x * y)
    VEC_BIN_UINT(x * y)
  case VOP_DIV:
    if (k == VECK_F32) VEC_BIN(float, x / y)
    if (k == VECK_F64) VEC_BIN(double, x / y)
    return 0;  /* Integer vector division has no packed semantics. */
  case VOP_AND: VEC_BIN_UINT(x & y)
  case VOP_OR: VEC_BIN_UINT(x | y)
  case VOP_XOR: VEC_BIN_UINT(x ^ y)
  case VOP_ANDN: VEC_BIN_UINT(~x & y)
  case VOP_MIN:
    /* Defined as (x < y ? x : y), which is exactly what MINPS/MINPD do. */
    if (k == VECK_F32) VEC_BIN(float, x < y ? x : y)
    if (k == VECK_F64) VEC_BIN(double, x < y ? x : y)
    if (veck_isunsigned(k)) VEC_BIN_UINT(x < y ? x : y)
    VEC_BIN_SINT(x < y ? x : y)
  case VOP_MAX:
    if (k == VECK_F32) VEC_BIN(float, x > y ? x : y)
    if (k == VECK_F64) VEC_BIN(double, x > y ? x : y)
    if (veck_isunsigned(k)) VEC_BIN_UINT(x > y ? x : y)
    VEC_BIN_SINT(x > y ? x : y)
  case VOP_ADDS:
    switch (k) {
    case VECK_I8:
      VEC_BINO(int8_t, int32_t, x+y < -128 ? -128 : x+y > 127 ? 127 : x+y)
    case VECK_U8: VEC_BINO(uint8_t, uint32_t, x+y > 255 ? 255 : x+y)
    case VECK_I16:
      VEC_BINO(int16_t, int32_t,
	       x+y < -32768 ? -32768 : x+y > 32767 ? 32767 : x+y)
    case VECK_U16: VEC_BINO(uint16_t, uint32_t, x+y > 65535 ? 65535 : x+y)
    default: return 0;
    }
  case VOP_SUBS:
    switch (k) {
    case VECK_I8:
      VEC_BINO(int8_t, int32_t, x-y < -128 ? -128 : x-y > 127 ? 127 : x-y)
    case VECK_U8: VEC_BINO(uint8_t, uint32_t, x < y ? 0 : x-y)
    case VECK_I16:
      VEC_BINO(int16_t, int32_t,
	       x-y < -32768 ? -32768 : x-y > 32767 ? 32767 : x-y)
    case VECK_U16: VEC_BINO(uint16_t, uint32_t, x < y ? 0 : x-y)
    default: return 0;
    }
  default: return 0;
  }
}

/* -- Unary element-wise operations --------------------------------------- */

int lj_simd_unop(void *dp, const void *ap, const CTVecInfo *vi, uint32_t op)
{
  uint32_t k = vi->kind;
  switch (op) {
  case VUN_NEG:
    if (k == VECK_F32) VEC_UN(float, -x)
    if (k == VECK_F64) VEC_UN(double, -x)
    switch (vi->esize) {
    case 1: VEC_UNO(uint8_t, uint32_t, 0u - x)
    case 2: VEC_UNO(uint16_t, uint32_t, 0u - x)
    case 4: VEC_UNO(uint32_t, uint32_t, 0u - x)
    default: VEC_UNO(uint64_t, uint64_t, 0u - x)
    }
  case VUN_NOT:
    switch (vi->esize) {
    case 1: VEC_UNO(uint8_t, uint32_t, ~x)
    case 2: VEC_UNO(uint16_t, uint32_t, ~x)
    case 4: VEC_UNO(uint32_t, uint32_t, ~x)
    default: VEC_UNO(uint64_t, uint64_t, ~x)
    }
  case VUN_ABS:
    /* FP: clear the sign bit (so |NaN| keeps its payload, |-0| is +0). */
    if (k == VECK_F32) {
      uint32_t *d = (uint32_t *)dp; const uint32_t *a = (const uint32_t *)ap;
      uint32_t i, n = vi->lanes;
      for (i = 0; i < n; i++) d[i] = a[i] & 0x7fffffffu;
      return 1;
    }
    if (k == VECK_F64) {
      uint64_t *d = (uint64_t *)dp; const uint64_t *a = (const uint64_t *)ap;
      uint32_t i, n = vi->lanes;
      for (i = 0; i < n; i++) d[i] = a[i] & U64x(7fffffff,ffffffff);
      return 1;
    }
    if (veck_isunsigned(k)) {  /* Unsigned absolute value is the identity. */
      memcpy(dp, ap, (size_t)vi->esize * vi->lanes);
      return 1;
    }
    /* Signed: wraps around for the most negative value, just like PABSB etc. */
    switch (vi->esize) {
    case 1: VEC_UN(int8_t, x < 0 ? (int8_t)(0u - (uint8_t)x) : x)
    case 2: VEC_UN(int16_t, x < 0 ? (int16_t)(0u - (uint16_t)x) : x)
    case 4: VEC_UN(int32_t, x < 0 ? (int32_t)(0u - (uint32_t)x) : x)
    default: VEC_UN(int64_t, x < 0 ? (int64_t)(0u - (uint64_t)x) : x)
    }
  case VUN_SQRT:
    if (k == VECK_F32) VEC_UN(float, (float)sqrt((double)x))
    if (k == VECK_F64) VEC_UN(double, sqrt(x))
    return 0;
  default: return 0;
  }
}

/*
** Rounding. ROUNDPS/ROUNDPD return a NaN source operand *quieted*, so the
** reference implementation has to do the same, or the two disagree on the
** payload of a signalling NaN. The libm floor()/ceil() do not necessarily
** quiet, so NaN lanes are handled here by hand.
*/
int lj_simd_round(void *dp, const void *ap, const CTVecInfo *vi, uint32_t mode)
{
  uint32_t i, n = vi->lanes;
  if (mode >= VRND__MAX) return 0;
  if (vi->kind == VECK_F32) {
    uint32_t *d = (uint32_t *)dp;
    const uint32_t *a = (const uint32_t *)ap;
    for (i = 0; i < n; i++) {
      uint32_t b = a[i];
      if ((b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu)) {
	d[i] = b | 0x00400000u;  /* NaN: set the quiet bit. */
      } else {
	float f;
	memcpy(&f, &b, 4);
	f = (float)vec_round1((double)f, mode);
	memcpy(&d[i], &f, 4);
      }
    }
    return 1;
  }
  if (vi->kind == VECK_F64) {
    uint64_t *d = (uint64_t *)dp;
    const uint64_t *a = (const uint64_t *)ap;
    for (i = 0; i < n; i++) {
      uint64_t b = a[i];
      if ((b & U64x(7ff00000,00000000)) == U64x(7ff00000,00000000) &&
	  (b & U64x(000fffff,ffffffff))) {
	d[i] = b | U64x(00080000,00000000);  /* NaN: set the quiet bit. */
      } else {
	double f;
	memcpy(&f, &b, 8);
	f = vec_round1(f, mode);
	memcpy(&d[i], &f, 8);
      }
    }
    return 1;
  }
  return 0;
}

/* -- Shifts -------------------------------------------------------------- */

/*
** The shift count is treated as unsigned, just like the 64-bit count that the
** x86 packed shift instructions read from an XMM register: an out-of-range
** count yields zero (logical shifts) or a full sign fill (arithmetic shift).
*/
int lj_simd_shift(void *dp, const void *ap, const CTVecInfo *vi,
		  uint32_t op, int32_t n)
{
  uint32_t bits = (uint32_t)vi->esize * 8;
  uint32_t sh = (uint32_t)n;
  if (veck_isfp(vi->kind)) return 0;
  if (op == VSH_SAR) {
    if (sh >= bits) sh = bits - 1;
    switch (vi->esize) {
    case 1: VEC_UN(int8_t, x >> sh)
    case 2: VEC_UN(int16_t, x >> sh)
    case 4: VEC_UN(int32_t, x >> sh)
    default: VEC_UN(int64_t, x >> sh)
    }
  }
  if (sh >= bits) {  /* Logical shifts flush to zero. */
    memset(dp, 0, (size_t)vi->esize * vi->lanes);
    return 1;
  }
  if (op == VSH_SHL) {
    switch (vi->esize) {
    case 1: VEC_UNO(uint8_t, uint32_t, x << sh)
    case 2: VEC_UNO(uint16_t, uint32_t, x << sh)
    case 4: VEC_UNO(uint32_t, uint32_t, x << sh)
    default: VEC_UNO(uint64_t, uint64_t, x << sh)
    }
  } else if (op == VSH_SHR) {
    switch (vi->esize) {
    case 1: VEC_UNO(uint8_t, uint32_t, x >> sh)
    case 2: VEC_UNO(uint16_t, uint32_t, x >> sh)
    case 4: VEC_UNO(uint32_t, uint32_t, x >> sh)
    default: VEC_UNO(uint64_t, uint64_t, x >> sh)
    }
  }
  return 0;
}

/* -- Comparisons --------------------------------------------------------- */

/* Element-wise comparison. Writes an all-ones/all-zero mask of the same width. */
#define VEC_CMP(TY, MTY, EXPR) \
  { MTY *d = (MTY *)dp; \
    const TY *a = (const TY *)ap; const TY *b = (const TY *)bp; \
    uint32_t i, n = vi->lanes; \
    for (i = 0; i < n; i++) { TY x = a[i], y = b[i]; d[i] = (EXPR) ? (MTY)~(MTY)0 : 0; } \
    return; }

#define VEC_CMP_INT(EXPR) \
  if (veck_isunsigned(vi->kind)) { \
    switch (vi->esize) { \
    case 1: VEC_CMP(uint8_t, uint8_t, EXPR) \
    case 2: VEC_CMP(uint16_t, uint16_t, EXPR) \
    case 4: VEC_CMP(uint32_t, uint32_t, EXPR) \
    default: VEC_CMP(uint64_t, uint64_t, EXPR) \
    } \
  } else { \
    switch (vi->esize) { \
    case 1: VEC_CMP(int8_t, uint8_t, EXPR) \
    case 2: VEC_CMP(int16_t, uint16_t, EXPR) \
    case 4: VEC_CMP(int32_t, uint32_t, EXPR) \
    default: VEC_CMP(int64_t, uint64_t, EXPR) \
    } \
  }

void lj_simd_cmp(void *dp, const void *ap, const void *bp,
		 const CTVecInfo *vi, uint32_t op)
{
  uint32_t k = vi->kind;
  switch (op) {
  case VCMP_EQ:
    if (k == VECK_F32) VEC_CMP(float, uint32_t, x == y)
    if (k == VECK_F64) VEC_CMP(double, uint64_t, x == y)
    VEC_CMP_INT(x == y)
  case VCMP_NE:
    /* Unordered: NaN != anything is true, matching CMPNEQPS. */
    if (k == VECK_F32) VEC_CMP(float, uint32_t, !(x == y))
    if (k == VECK_F64) VEC_CMP(double, uint64_t, !(x == y))
    VEC_CMP_INT(x != y)
  case VCMP_LT:
    if (k == VECK_F32) VEC_CMP(float, uint32_t, x < y)
    if (k == VECK_F64) VEC_CMP(double, uint64_t, x < y)
    VEC_CMP_INT(x < y)
  case VCMP_LE:
    if (k == VECK_F32) VEC_CMP(float, uint32_t, x <= y)
    if (k == VECK_F64) VEC_CMP(double, uint64_t, x <= y)
    VEC_CMP_INT(x <= y)
  case VCMP_GT:
    if (k == VECK_F32) VEC_CMP(float, uint32_t, x > y)
    if (k == VECK_F64) VEC_CMP(double, uint64_t, x > y)
    VEC_CMP_INT(x > y)
  default:
    if (k == VECK_F32) VEC_CMP(float, uint32_t, x >= y)
    if (k == VECK_F64) VEC_CMP(double, uint64_t, x >= y)
    VEC_CMP_INT(x >= y)
  }
}

/* Whole-vector equality, used by the '==' operator. */
int lj_simd_equal(const void *ap, const void *bp, const CTVecInfo *vi)
{
  uint32_t i, n = vi->lanes;
  if (vi->kind == VECK_F32) {
    const float *a = (const float *)ap, *b = (const float *)bp;
    for (i = 0; i < n; i++) if (!(a[i] == b[i])) return 0;
    return 1;
  } else if (vi->kind == VECK_F64) {
    const double *a = (const double *)ap, *b = (const double *)bp;
    for (i = 0; i < n; i++) if (!(a[i] == b[i])) return 0;
    return 1;
  }
  return memcmp(ap, bp, (size_t)vi->esize * n) == 0;
}

uint32_t lj_simd_movemask(const void *ap, const CTVecInfo *vi)
{
  uint32_t i, n = vi->lanes, m = 0;
  const uint8_t *a = (const uint8_t *)ap;
  uint32_t sh = (uint32_t)vi->esize * 8 - 1;
  for (i = 0; i < n; i++) {
    const uint8_t *p = a + i*vi->esize;
    uint32_t bit;
    switch (vi->esize) {
    case 1: bit = (uint32_t)(*(const uint8_t *)p) >> sh; break;
    case 2: { uint16_t v; memcpy(&v, p, 2); bit = (uint32_t)v >> sh; break; }
    case 4: { uint32_t v; memcpy(&v, p, 4); bit = v >> sh; break; }
    default: { uint64_t v; memcpy(&v, p, 8); bit = (uint32_t)(v >> sh); break; }
    }
    m |= bit << i;
  }
  return m;
}

/* -- Horizontal reductions ----------------------------------------------- */

/*
** Reductions use a fixed pairwise halving tree, *not* a left-to-right scan:
**
**   n = lanes; while (n > 1) { n /= 2; t[i] = op(t[i], t[i+n]); }
**
** That is exactly the shuffle-and-combine sequence the x86 backend emits, so
** interpreter and JIT agree bit for bit even for non-associative float adds
** and for the asymmetric NaN behaviour of MINPS/MAXPS.
*/
#define VEC_REDO(TY, OTY, EXPR) \
  { TY t[LJ_VEC_MAXSIZE/sizeof(TY)]; \
    uint32_t i, n = vi->lanes; \
    memcpy(t, ap, (size_t)n*sizeof(TY)); \
    while (n > 1) { \
      n >>= 1; \
      for (i = 0; i < n; i++) { OTY x = t[i], y = t[i+n]; t[i] = (TY)(EXPR); } \
    } \
    *(TY *)dp = t[0]; return 1; }

#define VEC_RED(TY, EXPR)	VEC_REDO(TY, TY, EXPR)

#define VEC_RED_UINT(EXPR) \
  switch (vi->esize) { \
  case 1: VEC_REDO(uint8_t, uint32_t, EXPR) \
  case 2: VEC_REDO(uint16_t, uint32_t, EXPR) \
  case 4: VEC_REDO(uint32_t, uint32_t, EXPR) \
  default: VEC_REDO(uint64_t, uint64_t, EXPR) \
  }

#define VEC_RED_SINT(EXPR) \
  switch (vi->esize) { \
  case 1: VEC_RED(int8_t, EXPR) \
  case 2: VEC_RED(int16_t, EXPR) \
  case 4: VEC_RED(int32_t, EXPR) \
  default: VEC_RED(int64_t, EXPR) \
  }

/* Reduce a vector to a single element, stored in *dp with the element type. */
int lj_simd_reduce(void *dp, const void *ap, const CTVecInfo *vi, uint32_t op)
{
  uint32_t k = vi->kind;
  switch (op) {
  case VRD_SUM:
    if (k == VECK_F32) VEC_RED(float, x + y)
    if (k == VECK_F64) VEC_RED(double, x + y)
    VEC_RED_UINT(x + y)
  case VRD_MIN:
    if (k == VECK_F32) VEC_RED(float, x < y ? x : y)
    if (k == VECK_F64) VEC_RED(double, x < y ? x : y)
    if (veck_isunsigned(k)) VEC_RED_UINT(x < y ? x : y)
    VEC_RED_SINT(x < y ? x : y)
  case VRD_MAX:
    if (k == VECK_F32) VEC_RED(float, x > y ? x : y)
    if (k == VECK_F64) VEC_RED(double, x > y ? x : y)
    if (veck_isunsigned(k)) VEC_RED_UINT(x > y ? x : y)
    VEC_RED_SINT(x > y ? x : y)
  default: return 0;
  }
}

/* -- Shuffles, selects and splats ---------------------------------------- */

/*
** Lane permute. idx[i] selects lane idx[i] of the concatenation of a and b.
** Indices are pre-validated by the caller.
*/
void lj_simd_shuffle(void *dp, const void *ap, const void *bp,
		     const CTVecInfo *vi, const uint8_t *idx)
{
  uint8_t tmp[LJ_VEC_MAXSIZE];
  uint32_t i, n = vi->lanes, esz = vi->esize;
  for (i = 0; i < n; i++) {
    uint32_t j = idx[i];
    const uint8_t *src = (const uint8_t *)(j < n ? ap : bp);
    memcpy(tmp + i*esz, src + (j & (n-1))*esz, esz);
  }
  memcpy(dp, tmp, (size_t)n*esz);
}

/* Bitwise select: dp = (mask & a) | (~mask & b). */
void lj_simd_select(void *dp, const void *mp, const void *ap, const void *bp,
		    CTSize size)
{
  uint32_t i, n = size >> 2;
  uint32_t *d = (uint32_t *)dp;
  const uint32_t *m = (const uint32_t *)mp;
  const uint32_t *a = (const uint32_t *)ap;
  const uint32_t *b = (const uint32_t *)bp;
  for (i = 0; i < n; i++) d[i] = (m[i] & a[i]) | (~m[i] & b[i]);
}

/* Replicate one element to all lanes. */
void lj_simd_splat(void *dp, const void *ep, const CTVecInfo *vi)
{
  uint32_t i, n = vi->lanes, esz = vi->esize;
  uint8_t *d = (uint8_t *)dp;
  for (i = 0; i < n; i++) memcpy(d + i*esz, ep, esz);
}

/* -- Lane conversion ----------------------------------------------------- */

/* Read lane i of a vector as a double. */
static double vec_getlane_num(const void *ap, const CTVecInfo *vi, uint32_t i)
{
  const uint8_t *p = (const uint8_t *)ap + i*vi->esize;
  switch (vi->kind) {
  case VECK_I8: return (double)*(const int8_t *)p;
  case VECK_U8: return (double)*(const uint8_t *)p;
  case VECK_I16: { int16_t v; memcpy(&v, p, 2); return (double)v; }
  case VECK_U16: { uint16_t v; memcpy(&v, p, 2); return (double)v; }
  case VECK_I32: { int32_t v; memcpy(&v, p, 4); return (double)v; }
  case VECK_U32: { uint32_t v; memcpy(&v, p, 4); return (double)v; }
  case VECK_I64: { int64_t v; memcpy(&v, p, 8); return (double)v; }
  case VECK_U64: { uint64_t v; memcpy(&v, p, 8); return (double)v; }
  case VECK_F32: { float v; memcpy(&v, p, 4); return (double)v; }
  default: { double v; memcpy(&v, p, 8); return v; }
  }
}

/*
** Truncate a double toward zero into a signed integer of the given width.
** Matches the packed x86 conversions (CVTTPS2DQ and friends): a NaN or a
** value outside the *signed* range of the destination yields the "integer
** indefinite" value, which is the minimum signed value of that width. There
** is no packed instruction that converts to unsigned, so unsigned lanes get
** the same signed truncation and the same indefinite value.
*/
static int64_t vec_toint(double x, uint32_t esize)
{
  int64_t lo, hi;
  switch (esize) {
  case 1: lo = -128; hi = 127; break;
  case 2: lo = -32768; hi = 32767; break;
  case 4: lo = -2147483647-1; hi = 2147483647; break;
  default:
    if (!(x >= -9223372036854775808.0) || x >= 9223372036854775808.0)
      return (int64_t)(-9223372036854775807LL - 1);  /* NaN or out of range. */
    return (int64_t)x;
  }
  if (!(x >= (double)lo) || x > (double)hi) return lo;  /* NaN or out of range. */
  return (int64_t)x;
}

/* Write a double to lane i of a vector. */
static void vec_setlane_num(void *dp, const CTVecInfo *vi, uint32_t i, double x)
{
  uint8_t *p = (uint8_t *)dp + i*vi->esize;
  switch (vi->kind) {
  case VECK_F32: { float v = (float)x; memcpy(p, &v, 4); break; }
  case VECK_F64: memcpy(p, &x, 8); break;
  default: {
    int64_t v = vec_toint(x, vi->esize);
    memcpy(p, &v, vi->esize);  /* Little-endian truncate to the lane width. */
    break;
    }
  }
}

/*
** Numeric lane conversion. Only conversions with a direct packed lowering are
** allowed; everything else is rejected by the caller-visible return value.
**
** Supported: equal lane count with any element kinds where at least one side
** is 32/64 bit, plus integer widen/narrow between adjacent widths with an
** equal *total* size relationship handled by the lane count check below.
*/
int lj_simd_convert(void *dp, const CTVecInfo *dvi,
		    const void *sp, const CTVecInfo *svi)
{
  uint8_t tmp[LJ_VEC_MAXSIZE];
  uint32_t i, n = svi->lanes < dvi->lanes ? svi->lanes : dvi->lanes;
  if (svi->lanes != dvi->lanes) return 0;
  memset(tmp, 0, sizeof(tmp));
  if (veck_isfp(svi->kind) || veck_isfp(dvi->kind)) {
    /* At least one FP side: go through double, which is exact for all
    ** supported lane types except 64-bit integers with more than 53 bits.
    ** Those go through the integer path below instead. */
    if (svi->esize == 8 && !veck_isfp(svi->kind)) {
      for (i = 0; i < n; i++) {
	const uint8_t *p = (const uint8_t *)sp + i*8;
	if (svi->kind == VECK_I64) {
	  int64_t v; memcpy(&v, p, 8);
	  vec_setlane_num(tmp, dvi, i, (double)v);
	} else {
	  uint64_t v; memcpy(&v, p, 8);
	  vec_setlane_num(tmp, dvi, i, (double)v);
	}
      }
    } else if (dvi->esize == 8 && !veck_isfp(dvi->kind)) {
      for (i = 0; i < n; i++) {
	double x = vec_getlane_num(sp, svi, i);
	uint8_t *p = tmp + i*8;
	if (dvi->kind == VECK_I64) {
	  int64_t v = (int64_t)x; memcpy(p, &v, 8);
	} else {
	  uint64_t v = (uint64_t)x; memcpy(p, &v, 8);
	}
      }
    } else {
      for (i = 0; i < n; i++)
	vec_setlane_num(tmp, dvi, i, vec_getlane_num(sp, svi, i));
    }
  } else {
    /* Integer -> integer: sign/zero extend or truncate, exactly like C. */
    for (i = 0; i < n; i++) {
      const uint8_t *p = (const uint8_t *)sp + i*svi->esize;
      uint64_t v;
      switch (svi->kind) {
      case VECK_I8: v = (uint64_t)(int64_t)*(const int8_t *)p; break;
      case VECK_U8: v = (uint64_t)*(const uint8_t *)p; break;
      case VECK_I16: { int16_t t; memcpy(&t, p, 2); v = (uint64_t)(int64_t)t; break; }
      case VECK_U16: { uint16_t t; memcpy(&t, p, 2); v = (uint64_t)t; break; }
      case VECK_I32: { int32_t t; memcpy(&t, p, 4); v = (uint64_t)(int64_t)t; break; }
      case VECK_U32: { uint32_t t; memcpy(&t, p, 4); v = (uint64_t)t; break; }
      default: memcpy(&v, p, 8); break;
      }
      memcpy(tmp + i*dvi->esize, &v, dvi->esize);  /* Little-endian truncate. */
    }
  }
  memcpy(dp, tmp, (size_t)dvi->esize * dvi->lanes);
  return 1;
}

/* -- Mask ctypes --------------------------------------------------------- */

/* Signed integer vector ctype of the same shape, used for comparison masks. */
CTypeID lj_simd_masktype(CTState *cts, const CTVecInfo *vi)
{
  CTypeID eid;
  CTSize size = (CTSize)vi->esize * vi->lanes;
  uint32_t align;
  switch (vi->esize) {
  case 1: eid = CTID_INT8; break;
  case 2: eid = CTID_INT16; break;
  case 4: eid = CTID_INT32; break;
  default: eid = CTID_INT64; break;
  }
  align = lj_fls(size);
  if (align > 4) align = 4;
  return lj_ctype_intern(cts, CTINFO(CT_ARRAY, CTF_VECTOR+CTALIGN(align)+eid),
			 size);
}

#endif
