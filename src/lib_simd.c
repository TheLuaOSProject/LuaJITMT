/*
** SIMD library. Reached via require("ffi.simd").
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** This module only exports operations that ordinary Lua operators on FFI
** vector cdata cannot express. Plain arithmetic (+ - * / unary minus) and
** whole-vector equality are handled by the operators themselves.
*/

#define lib_simd_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"

#if LJ_HASFFI

#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_ctype.h"
#include "lj_cconv.h"
#include "lj_cdata.h"
#include "lj_simd.h"
#include "lj_jit.h"
#include "lj_dispatch.h"
#include "lj_lib.h"

/* -- Argument checking --------------------------------------------------- */

#define LJLIB_MODULE_ffi_simd

/* Check for a vector cdata argument and return a pointer to its payload. */
static uint8_t *simd_checkvec(lua_State *L, CTState *cts, int narg,
			      CTVecInfo *vi, CTypeID *pid)
{
  TValue *o = L->base + (narg-1);
  if (o < L->top && tviscdata(o)) {
    GCcdata *cd = cdataV(o);
    CType *ct = ctype_raw(cts, cd->ctypeid);
    if (lj_ctype_vecinfo(cts, ct, vi)) {
      if (pid) *pid = cd->ctypeid;
      return (uint8_t *)cdataptr(cd);
    }
  }
  lj_err_argtype(L, narg, "vector");
  return NULL;
}

/*
** Check for the second operand of a binary op: either a vector of exactly the
** same ctype, or a scalar which is converted to the element type and splatted.
** The splatted value is written to buf.
*/
static const uint8_t *simd_checkvec2(lua_State *L, CTState *cts, int narg,
				     const CTVecInfo *vi, CTypeID id,
				     uint8_t *buf)
{
  TValue *o = L->base + (narg-1);
  if (o < L->top) {
    if (tviscdata(o)) {
      GCcdata *cd = cdataV(o);
      CTVecInfo vi2;
      CType *ct = ctype_raw(cts, cd->ctypeid);
      if (lj_ctype_vecinfo(cts, ct, &vi2)) {
	if (ctype_raw(cts, id) != ct)
	  lj_err_argtype(L, narg, "matching vector");
	return (const uint8_t *)cdataptr(cd);
      }
    }
    if (tviscdata(o) || tvisnumber(o)) {
      uint8_t ebuf[8];
      lj_cconv_ct_tv(cts, ctype_get(cts, vi->eid), ebuf, o, 0);
      lj_simd_splat(buf, ebuf, vi);
      return buf;
    }
  }
  lj_err_argtype(L, narg, "matching vector or number");
  return NULL;
}

/* Push a new vector cdata and return a pointer to its payload. */
static void *simd_newvec(lua_State *L, CTState *cts, CTypeID id, CTSize size)
{
  GCcdata *cd = lj_cdata_new(cts, id, size);
  TValue *o = L->top++;
  setcdataV(L, o, cd);
  return cdataptr(cd);
}

/* Check for a ctype argument: either a ctype object or an existing cdata. */
static CTypeID simd_checkctypeid(lua_State *L, CTState *cts, int narg)
{
  TValue *o = L->base + (narg-1);
  if (o < L->top && tviscdata(o)) {
    GCcdata *cd = cdataV(o);
    if (cd->ctypeid == CTID_CTYPEID) return *(CTypeID *)cdataptr(cd);
    return cd->ctypeid;
  }
  lj_err_argtype(L, narg, "C type");
  return 0;
}

/* Common prologue for a binary vector operation. */
#define SIMD_BINPRE(vi, ap, bp, id) \
  CTState *cts = ctype_cts(L); \
  CTVecInfo vi; CTypeID id; uint8_t sbuf[LJ_VEC_MAXSIZE]; \
  const uint8_t *ap = simd_checkvec(L, cts, 1, &vi, &id); \
  const uint8_t *bp = simd_checkvec2(L, cts, 2, &vi, id, sbuf);

static int simd_binop(lua_State *L, uint32_t op)
{
  SIMD_BINPRE(vi, ap, bp, id)
  {
    uint8_t rbuf[LJ_VEC_MAXSIZE];
    CTSize size = (CTSize)vi.esize * vi.lanes;
    if (!lj_simd_binop(rbuf, ap, bp, &vi, op))
      lj_err_callermsg(L, "operation not supported for this element type");
    memcpy(simd_newvec(L, cts, id, size), rbuf, size);
    lj_gc_check(L);
    return 1;
  }
}

static int simd_unop(lua_State *L, uint32_t op)
{
  CTState *cts = ctype_cts(L);
  CTVecInfo vi; CTypeID id;
  const uint8_t *ap = simd_checkvec(L, cts, 1, &vi, &id);
  uint8_t rbuf[LJ_VEC_MAXSIZE];
  CTSize size = (CTSize)vi.esize * vi.lanes;
  if (!lj_simd_unop(rbuf, ap, &vi, op))
    lj_err_callermsg(L, "operation not supported for this element type");
  memcpy(simd_newvec(L, cts, id, size), rbuf, size);
  lj_gc_check(L);
  return 1;
}

static int simd_cmpop(lua_State *L, uint32_t op)
{
  SIMD_BINPRE(vi, ap, bp, id)
  {
    uint8_t rbuf[LJ_VEC_MAXSIZE];
    CTSize size = (CTSize)vi.esize * vi.lanes;
    CTypeID mid;
    lj_simd_cmp(rbuf, ap, bp, &vi, op);
    mid = lj_simd_masktype(cts, &vi);  /* May reallocate cts->tab. */
    memcpy(simd_newvec(L, cts, mid, size), rbuf, size);
    lj_gc_check(L);
    return 1;
  }
}

static int simd_shiftop(lua_State *L, uint32_t op)
{
  CTState *cts = ctype_cts(L);
  CTVecInfo vi; CTypeID id;
  const uint8_t *ap = simd_checkvec(L, cts, 1, &vi, &id);
  int32_t n = lj_lib_checkint(L, 2);
  uint8_t rbuf[LJ_VEC_MAXSIZE];
  CTSize size = (CTSize)vi.esize * vi.lanes;
  if (!lj_simd_shift(rbuf, ap, &vi, op, n))
    lj_err_callermsg(L, "shift not supported for this element type");
  memcpy(simd_newvec(L, cts, id, size), rbuf, size);
  lj_gc_check(L);
  return 1;
}

static int simd_roundop(lua_State *L, uint32_t mode)
{
  CTState *cts = ctype_cts(L);
  CTVecInfo vi; CTypeID id;
  const uint8_t *ap = simd_checkvec(L, cts, 1, &vi, &id);
  uint8_t rbuf[LJ_VEC_MAXSIZE];
  CTSize size = (CTSize)vi.esize * vi.lanes;
  if (!lj_simd_round(rbuf, ap, &vi, mode))
    lj_err_callermsg(L, "rounding requires a floating-point vector");
  memcpy(simd_newvec(L, cts, id, size), rbuf, size);
  lj_gc_check(L);
  return 1;
}

/* Push a single lane value with the element ctype. */
static int simd_pushelem(lua_State *L, CTState *cts, const CTVecInfo *vi,
			 const void *ep)
{
  CType *ct = ctype_get(cts, vi->eid);
  TValue *o = L->top++;
  lj_cconv_tv_ct(cts, ct, vi->eid, o, (uint8_t *)ep);
  lj_gc_check(L);
  return 1;
}

/* -- Bitwise operations -------------------------------------------------- */

LJLIB_CF(ffi_simd_band)		LJLIB_REC(simd_binop VOP_AND)
{
  return simd_binop(L, VOP_AND);
}

LJLIB_CF(ffi_simd_bor)		LJLIB_REC(simd_binop VOP_OR)
{
  return simd_binop(L, VOP_OR);
}

LJLIB_CF(ffi_simd_bxor)		LJLIB_REC(simd_binop VOP_XOR)
{
  return simd_binop(L, VOP_XOR);
}

LJLIB_CF(ffi_simd_bandn)		LJLIB_REC(simd_binop VOP_ANDN)
{
  return simd_binop(L, VOP_ANDN);
}

LJLIB_CF(ffi_simd_bnot)		LJLIB_REC(simd_unop VUN_NOT)
{
  return simd_unop(L, VUN_NOT);
}

/* -- Min/max, saturating arithmetic and math ----------------------------- */

LJLIB_CF(ffi_simd_min)		LJLIB_REC(simd_binop VOP_MIN)
{
  return simd_binop(L, VOP_MIN);
}

LJLIB_CF(ffi_simd_max)		LJLIB_REC(simd_binop VOP_MAX)
{
  return simd_binop(L, VOP_MAX);
}

LJLIB_CF(ffi_simd_adds)		LJLIB_REC(simd_binop VOP_ADDS)
{
  return simd_binop(L, VOP_ADDS);
}

LJLIB_CF(ffi_simd_subs)		LJLIB_REC(simd_binop VOP_SUBS)
{
  return simd_binop(L, VOP_SUBS);
}

LJLIB_CF(ffi_simd_abs)		LJLIB_REC(simd_unop VUN_ABS)
{
  return simd_unop(L, VUN_ABS);
}

LJLIB_CF(ffi_simd_sqrt)		LJLIB_REC(simd_unop VUN_SQRT)
{
  return simd_unop(L, VUN_SQRT);
}

LJLIB_CF(ffi_simd_floor)		LJLIB_REC(simd_round VRND_FLOOR)
{
  return simd_roundop(L, VRND_FLOOR);
}

LJLIB_CF(ffi_simd_ceil)		LJLIB_REC(simd_round VRND_CEIL)
{
  return simd_roundop(L, VRND_CEIL);
}

LJLIB_CF(ffi_simd_trunc)		LJLIB_REC(simd_round VRND_TRUNC)
{
  return simd_roundop(L, VRND_TRUNC);
}

LJLIB_CF(ffi_simd_round)		LJLIB_REC(simd_round VRND_NEAREST)
{
  return simd_roundop(L, VRND_NEAREST);
}

/* -- Shifts -------------------------------------------------------------- */

LJLIB_CF(ffi_simd_shl)		LJLIB_REC(simd_shift VSH_SHL)
{
  return simd_shiftop(L, VSH_SHL);
}

LJLIB_CF(ffi_simd_shr)		LJLIB_REC(simd_shift VSH_SHR)
{
  return simd_shiftop(L, VSH_SHR);
}

LJLIB_CF(ffi_simd_sar)		LJLIB_REC(simd_shift VSH_SAR)
{
  return simd_shiftop(L, VSH_SAR);
}

/* -- Lane-wise comparisons ----------------------------------------------- */

LJLIB_CF(ffi_simd_eq)		LJLIB_REC(simd_cmp VCMP_EQ)
{
  return simd_cmpop(L, VCMP_EQ);
}

LJLIB_CF(ffi_simd_gt)		LJLIB_REC(simd_cmp VCMP_GT)
{
  return simd_cmpop(L, VCMP_GT);
}

LJLIB_CF(ffi_simd_ge)		LJLIB_REC(simd_cmp VCMP_GE)
{
  return simd_cmpop(L, VCMP_GE);
}

/* -- Masks --------------------------------------------------------------- */

LJLIB_CF(ffi_simd_select)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  CTVecInfo mvi, vi;
  CTypeID mid, id;
  uint8_t sbuf[LJ_VEC_MAXSIZE], rbuf[LJ_VEC_MAXSIZE];
  const uint8_t *mp = simd_checkvec(L, cts, 1, &mvi, &mid);
  const uint8_t *ap = simd_checkvec(L, cts, 2, &vi, &id);
  const uint8_t *bp;
  CTSize size = (CTSize)vi.esize * vi.lanes;
  if ((CTSize)mvi.esize * mvi.lanes != size)
    lj_err_argtype(L, 1, "mask vector of matching size");
  bp = simd_checkvec2(L, cts, 3, &vi, id, sbuf);
  lj_simd_select(rbuf, mp, ap, bp, size);
  memcpy(simd_newvec(L, cts, id, size), rbuf, size);
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_simd_movemask)	LJLIB_REC(simd_movemask)
{
  CTState *cts = ctype_cts(L);
  CTVecInfo vi;
  const uint8_t *ap = simd_checkvec(L, cts, 1, &vi, NULL);
  setintV(L->top++, (int32_t)lj_simd_movemask(ap, &vi));
  return 1;
}

LJLIB_CF(ffi_simd_allof)		LJLIB_REC(simd_maskcmp 0)
{
  CTState *cts = ctype_cts(L);
  CTVecInfo vi;
  const uint8_t *ap = simd_checkvec(L, cts, 1, &vi, NULL);
  uint32_t all = vi.lanes == 32 ? 0xffffffffu : (1u << vi.lanes) - 1;
  int res = lj_simd_movemask(ap, &vi) == all;
  setboolV(L->top++, res);
  setboolV(&G(L)->tmptv2, res);  /* Remember for the trace recorder. */
  return 1;
}

LJLIB_CF(ffi_simd_anyof)		LJLIB_REC(simd_maskcmp 1)
{
  CTState *cts = ctype_cts(L);
  CTVecInfo vi;
  const uint8_t *ap = simd_checkvec(L, cts, 1, &vi, NULL);
  {
    int res = lj_simd_movemask(ap, &vi) != 0;
    setboolV(L->top++, res);
    setboolV(&G(L)->tmptv2, res);  /* Remember for the trace recorder. */
  }
  return 1;
}

/* -- Horizontal reductions ----------------------------------------------- */

static int simd_reduceop(lua_State *L, uint32_t op)
{
  CTState *cts = ctype_cts(L);
  CTVecInfo vi;
  const uint8_t *ap = simd_checkvec(L, cts, 1, &vi, NULL);
  uint8_t rbuf[8];
  if (!lj_simd_reduce(rbuf, ap, &vi, op))
    lj_err_callermsg(L, "reduction not supported for this element type");
  return simd_pushelem(L, cts, &vi, rbuf);
}

LJLIB_CF(ffi_simd_hsum)		LJLIB_REC(simd_reduce VRD_SUM)
{
  return simd_reduceop(L, VRD_SUM);
}

LJLIB_CF(ffi_simd_hmin)		LJLIB_REC(simd_reduce VRD_MIN)
{
  return simd_reduceop(L, VRD_MIN);
}

LJLIB_CF(ffi_simd_hmax)		LJLIB_REC(simd_reduce VRD_MAX)
{
  return simd_reduceop(L, VRD_MAX);
}

/* -- Lane insertion and shuffles ----------------------------------------- */

LJLIB_CF(ffi_simd_insert)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  CTVecInfo vi;
  CTypeID id;
  const uint8_t *ap = simd_checkvec(L, cts, 1, &vi, &id);
  int32_t lane = lj_lib_checkint(L, 2);
  CTSize size = (CTSize)vi.esize * vi.lanes;
  uint8_t rbuf[LJ_VEC_MAXSIZE];
  TValue *o = lj_lib_checkany(L, 3);
  if ((uint32_t)lane >= (uint32_t)vi.lanes)
    lj_err_arg(L, 2, LJ_ERR_IDXRNG);
  memcpy(rbuf, ap, size);
  lj_cconv_ct_tv(cts, ctype_get(cts, vi.eid), rbuf + lane*vi.esize, o, 0);
  memcpy(simd_newvec(L, cts, id, size), rbuf, size);
  lj_gc_check(L);
  return 1;
}

/* Collect and validate shuffle indices from the argument list. */
static void simd_checkidx(lua_State *L, int narg, uint32_t lanes,
			  uint32_t range, uint8_t *idx)
{
  uint32_t i;
  for (i = 0; i < lanes; i++) {
    int32_t v = lj_lib_checkint(L, narg + (int)i);
    if ((uint32_t)v >= range) lj_err_arg(L, narg + (int)i, LJ_ERR_IDXRNG);
    idx[i] = (uint8_t)v;
  }
}

LJLIB_CF(ffi_simd_shuffle)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  CTVecInfo vi;
  CTypeID id;
  const uint8_t *ap = simd_checkvec(L, cts, 1, &vi, &id);
  CTSize size = (CTSize)vi.esize * vi.lanes;
  uint8_t idx[LJ_VEC_MAXSIZE], rbuf[LJ_VEC_MAXSIZE];
  simd_checkidx(L, 2, vi.lanes, vi.lanes, idx);
  lj_simd_shuffle(rbuf, ap, ap, &vi, idx);
  memcpy(simd_newvec(L, cts, id, size), rbuf, size);
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_simd_shuffle2)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  CTVecInfo vi, vi2;
  CTypeID id, id2;
  const uint8_t *ap = simd_checkvec(L, cts, 1, &vi, &id);
  const uint8_t *bp = simd_checkvec(L, cts, 2, &vi2, &id2);
  CTSize size = (CTSize)vi.esize * vi.lanes;
  uint8_t idx[LJ_VEC_MAXSIZE], rbuf[LJ_VEC_MAXSIZE];
  if (ctype_raw(cts, id) != ctype_raw(cts, id2))
    lj_err_argtype(L, 2, "matching vector");
  simd_checkidx(L, 3, vi.lanes, 2*(uint32_t)vi.lanes, idx);
  lj_simd_shuffle(rbuf, ap, bp, &vi, idx);
  memcpy(simd_newvec(L, cts, id, size), rbuf, size);
  lj_gc_check(L);
  return 1;
}

/* -- Conversions --------------------------------------------------------- */

LJLIB_CF(ffi_simd_bitcast)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  CTypeID did = simd_checkctypeid(L, cts, 1);
  CTVecInfo svi, dvi;
  const uint8_t *sp = simd_checkvec(L, cts, 2, &svi, NULL);
  CType *dct = ctype_raw(cts, did);
  if (!lj_ctype_vecinfo(cts, dct, &dvi))
    lj_err_argtype(L, 1, "vector C type");
  if ((CTSize)dvi.esize * dvi.lanes != (CTSize)svi.esize * svi.lanes)
    lj_err_callermsg(L, "bitcast between vectors of different size");
  {
    CTSize size = (CTSize)dvi.esize * dvi.lanes;
    uint8_t rbuf[LJ_VEC_MAXSIZE];
    memcpy(rbuf, sp, size);
    memcpy(simd_newvec(L, cts, did, size), rbuf, size);
  }
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_simd_convert)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  CTypeID did = simd_checkctypeid(L, cts, 1);
  CTVecInfo svi, dvi;
  const uint8_t *sp = simd_checkvec(L, cts, 2, &svi, NULL);
  CType *dct = ctype_raw(cts, did);
  uint8_t rbuf[LJ_VEC_MAXSIZE];
  if (!lj_ctype_vecinfo(cts, dct, &dvi))
    lj_err_argtype(L, 1, "vector C type");
  if (!lj_simd_convert(rbuf, &dvi, sp, &svi))
    lj_err_callermsg(L, "unsupported vector conversion");
  {
    CTSize size = (CTSize)dvi.esize * dvi.lanes;
    memcpy(simd_newvec(L, cts, did, size), rbuf, size);
  }
  lj_gc_check(L);
  return 1;
}

/* -- Introspection ------------------------------------------------------- */

static int simd_cf_isvector(lua_State *L)
{
  CTState *cts = ctype_cts(L);
  TValue *o = L->base;
  CTVecInfo vi;
  int res = 0;
  if (o < L->top && tviscdata(o)) {
    GCcdata *cd = cdataV(o);
    CTypeID id = cd->ctypeid == CTID_CTYPEID ? *(CTypeID *)cdataptr(cd) :
					       cd->ctypeid;
    res = lj_ctype_vecinfo(cts, ctype_raw(cts, id), &vi);
  }
  setboolV(L->top++, res);
  return 1;
}

static int simd_cf_lanes(lua_State *L)
{
  CTState *cts = ctype_cts(L);
  CTypeID id = simd_checkctypeid(L, cts, 1);
  CTVecInfo vi;
  if (!lj_ctype_vecinfo(cts, ctype_raw(cts, id), &vi))
    lj_err_argtype(L, 1, "vector");
  setintV(L->top++, (int32_t)vi.lanes);
  return 1;
}

static int simd_cf_elementtype(lua_State *L)
{
  CTState *cts = ctype_cts(L);
  CTypeID id = simd_checkctypeid(L, cts, 1);
  CTVecInfo vi;
  GCcdata *cd;
  if (!lj_ctype_vecinfo(cts, ctype_raw(cts, id), &vi))
    lj_err_argtype(L, 1, "vector");
  cd = lj_cdata_new(cts, CTID_CTYPEID, 4);
  *(CTypeID *)cdataptr(cd) = vi.eid;
  setcdataV(L, L->top++, cd);
  lj_gc_check(L);
  return 1;
}

static int simd_cf_features(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, 4);
  settabV(L, L->top++, t);
#if LJ_HASJIT
  {
    jit_State *J = L2J(L);
    uint32_t f = J->flags;
#define SETFEAT(name, cond) \
    setboolV(lj_tab_setstr(L, t, lj_str_newlit(L, name)), (cond) != 0)
#if LJ_TARGET_X86ORX64
    SETFEAT("sse2", 1);
    SETFEAT("sse3", f & JIT_F_SSE3);
    SETFEAT("ssse3", f & JIT_F_SSSE3);
    SETFEAT("sse4_1", f & JIT_F_SSE4_1);
    SETFEAT("sse4_2", f & JIT_F_SSE4_2);
    SETFEAT("avx", f & JIT_F_AVX);
    SETFEAT("avx2", f & JIT_F_AVX2);
#else
    UNUSED(f);
#endif
#undef SETFEAT
  }
#endif
  setintV(lj_tab_setstr(L, t, lj_str_newlit(L, "vecsize")), LJ_SIMD_JITSIZE);
  lj_gc_check(L);
  return 1;
}

#include "lj_libdef.h"

/*
** Lua source for the handful of functions that are exact rewrites of others.
** They inline into a trace just as well as a fast function would, and they
** keep the module inside LuaJIT's budget of 255 fast function IDs.
*/
static const char simd_luasrc[] =
  "local simd = ...\n"
  "local eq, gt, ge, bnot = simd.eq, simd.gt, simd.ge, simd.bnot\n"
  "function simd.ne(a, b) return bnot(eq(a, b)) end\n"
  "function simd.lt(a, b) return gt(b, a) end\n"
  "function simd.le(a, b) return ge(b, a) end\n";

static const luaL_Reg simd_plaincf[] = {
  { "isvector",    simd_cf_isvector },
  { "lanes",       simd_cf_lanes },
  { "elementtype", simd_cf_elementtype },
  { "features",    simd_cf_features },
  { NULL, NULL }
};

LUALIB_API int luaopen_ffi_simd(lua_State *L)
{
  const luaL_Reg *r;
  LJ_LIB_REG(L, NULL, ffi_simd);
  for (r = simd_plaincf; r->name; r++) {
    lua_pushcfunction(L, r->func);
    lua_setfield(L, -2, r->name);
  }
  if (luaL_loadbuffer(L, simd_luasrc, sizeof(simd_luasrc)-1, "=ffi.simd") != 0)
    lua_error(L);
  lua_pushvalue(L, -2);
  lua_call(L, 1, 0);
  return 1;
}

#endif
