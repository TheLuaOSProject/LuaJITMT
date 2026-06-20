/*
** FFI library.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lib_ffi_c
#define LUA_LIB

#include <errno.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"

#if LJ_HASFFI

#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_ctype.h"
#include "lj_cparse.h"
#include "lj_cdata.h"
#include "lj_cconv.h"
#include "lj_carith.h"
#include "lj_udata.h"
#include "lj_ccall.h"
#include "lj_ccallback.h"
#include "lj_clib.h"
#include "lj_strfmt.h"
#include "lj_ff.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lj_lib.h"

/* -- C type checks ------------------------------------------------------- */

static CTypeID ffi_parse_ctype_locked(lua_State *L, CTState *cts,
				      TValue *param, int *errcode)
{
  GCstr *s = strV(L->base);
  CPState cp;
  cp.L = L;
  cp.cts = cts;
  cp.srcname = strdata(s);
  cp.p = strdata(s);
  cp.param = param;
  cp.mode = CPARSE_MODE_ABSTRACT|CPARSE_MODE_NOIMPLICIT;
  *errcode = lj_cparse(&cp);
  return cp.val.id;
}

/* Check first argument for a C type and returns its ID. */
static CTypeID ffi_checkctype(lua_State *L, CTState *cts, TValue *param)
{
  TValue *o = L->base;
  if (!(o < L->top)) {
  err_argtype:
    lj_err_argtype(L, 1, "C type");
  }
  if (tvisstr(o)) {  /* Parse an abstract C type declaration. */
    int errcode;
    CTypeID id;
    lj_ctype_parse_lock(cts, L);
    id = ffi_parse_ctype_locked(L, cts, param, &errcode);
    lj_ctype_parse_unlock(cts);
    if (errcode) lj_err_throw(L, errcode);  /* Propagate errors. */
    return id;
  } else {
    GCcdata *cd;
    if (!tviscdata(o)) goto err_argtype;
    if (param && param < L->top) lj_err_arg(L, 1, LJ_ERR_FFI_NUMPARAM);
    cd = cdataV(o);
    return cd->ctypeid == CTID_CTYPEID ? *(CTypeID *)cdataptr(cd) : cd->ctypeid;
  }
}

static CTypeID ffi_checkctype_layout_lock(lua_State *L, CTState *cts,
					  TValue *param)
{
  TValue *o = L->base;
  int errcode = 0;
  CTypeID id;
  if (!(o < L->top)) {
  err_argtype:
    lj_err_argtype(L, 1, "C type");
  }
  if (!tvisstr(o)) {
    GCcdata *cd;
    if (!tviscdata(o)) goto err_argtype;
    if (param && param < L->top) lj_err_arg(L, 1, LJ_ERR_FFI_NUMPARAM);
    cd = cdataV(o);
    id = cd->ctypeid == CTID_CTYPEID ? *(CTypeID *)cdataptr(cd) : cd->ctypeid;
    lj_ctype_parse_lock(cts, L);
    return id;  /* 11.2: layout reader waits out parser rollback. */
  }
  lj_ctype_parse_lock(cts, L);
  id = ffi_parse_ctype_locked(L, cts, param, &errcode);
  if (errcode) {
    lj_ctype_parse_unlock(cts);
    lj_err_throw(L, errcode);  /* Propagate errors. */
  }
  return id;  /* 11.2: layout reader waits out parser rollback. */
}

static CTypeID ffi_checkctype_noparse(lua_State *L, TValue *param, int *isstr)
{
  TValue *o = L->base;
  if (!(o < L->top)) {
  err_argtype:
    lj_err_argtype(L, 1, "C type");
  }
  if (tvisstr(o)) {
    *isstr = 1;
    return 0;
  } else {
    GCcdata *cd;
    *isstr = 0;
    if (!tviscdata(o)) goto err_argtype;
    if (param && param < L->top) lj_err_arg(L, 1, LJ_ERR_FFI_NUMPARAM);
    cd = cdataV(o);
    return cd->ctypeid == CTID_CTYPEID ? *(CTypeID *)cdataptr(cd) : cd->ctypeid;
  }
}

static int ffi_new_layout_snapshot(CTState *cts, CTypeID id, CTSize nelem,
				   int hasnelem, CTypeID *ridp,
				   CTInfo *infop, CTSize *szp,
				   int *neednelem);

/* Check argument for C data and return it. */
static GCcdata *ffi_checkcdata(lua_State *L, int narg)
{
  TValue *o = L->base + narg-1;
  if (!(o < L->top && tviscdata(o)))
    lj_err_argt(L, narg, LUA_TCDATA);
  return cdataV(o);
}

/* Convert argument to C pointer. */
static void *ffi_checkptr(lua_State *L, int narg, CTypeID id)
{
  CTState *cts = ctype_cts(L);
  TValue *o = L->base + narg-1;
  void *p;
  if (o >= L->top)
    lj_err_arg(L, narg, LJ_ERR_NOVAL);
  lj_cconv_ct_tv_l(L, cts, ctype_get(cts, id), id, (uint8_t *)&p, o,
		   CCF_ARG(narg));
  return p;
}

/* Convert argument to int32_t. */
static int32_t ffi_checkint(lua_State *L, int narg)
{
  CTState *cts = ctype_cts(L);
  TValue *o = L->base + narg-1;
  int32_t i;
  if (o >= L->top)
    lj_err_arg(L, narg, LJ_ERR_NOVAL);
  lj_cconv_ct_tv_l(L, cts, ctype_get(cts, CTID_INT32), CTID_INT32,
		   (uint8_t *)&i, o, CCF_ARG(narg));
  return i;
}

/* -- C type metamethods -------------------------------------------------- */

#define LJLIB_MODULE_ffi_meta

/* Handle ctype __index/__newindex metamethods. */
static int ffi_index_meta(lua_State *L, CTState *cts, CTypeID id, MMS mm)
{
  TValue metatv;
  cTValue *tv = lj_ctype_metatv(cts, &metatv, id, mm);
  TValue *base = L->base;
  if (!tv) {
    const char *s;
  err_index:
    s = strdata(lj_ctype_repr(L, id, NULL));
    if (tvisstr(L->base+1)) {
      lj_err_callerv(L, LJ_ERR_FFI_BADMEMBER, s, strVdata(L->base+1));
    } else {
      const char *key = tviscdata(L->base+1) ?
	strdata(lj_ctype_repr(L, cdataV(L->base+1)->ctypeid, NULL)) :
	lj_typename(L->base+1);
      lj_err_callerv(L, LJ_ERR_FFI_BADIDXW, s, key);
    }
  }
  if (!tvisfunc(tv)) {
    if (mm == MM_index) {
      cTValue *o = lj_meta_tget(L, tv, base+1);
      if (o) {
	if (tvisnil(o)) goto err_index;
	copyTV(L, L->top-1, o);
	return 1;
      }
    } else {
      GCtab *owner;
      TValue *o = lj_meta_tset_owner(L, tv, base+1, &owner);
      if (o) {
	copyTVrel(L, o, base+2);
	lj_gc2_barrier_weak_write(L, owner, base+1, base+2);
	lj_gc2_barrier_tv_pair(L, obj2gco(owner), o);
	return 0;
      }
    }
    copyTV(L, base, L->top);
    tv = L->top-1-LJ_FR2;
  }
  return lj_meta_tailcall(L, tv);
}

LJLIB_CF(ffi_meta___index)	LJLIB_REC(cdata_index 0)
{
  CTState *cts = ctype_cts(L);
  CTInfo qual = 0;
  CTypeID id = 0;
  CType snap;
  CType *ct;
  uint8_t *p;
  TValue *o = L->base;
  if (!(o+1 < L->top && tviscdata(o)))  /* Also checks for presence of key. */
    lj_err_argt(L, 1, LUA_TCDATA);
  ct = lj_cdata_index_l(L, cts, cdataV(o), o+1, &p, &qual, &snap, &id);
  if ((qual & 1))
    return ffi_index_meta(L, cts, id, MM_index);
  if (lj_cdata_get_l(L, cts, ct, L->top-1, p))
    lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_meta___newindex)	LJLIB_REC(cdata_index 1)
{
  CTState *cts = ctype_cts(L);
  CTInfo qual = 0;
  CTypeID id = 0;
  CType snap;
  CType *ct;
  uint8_t *p;
  TValue *o = L->base;
  if (!(o+2 < L->top && tviscdata(o)))  /* Also checks for key and value. */
    lj_err_argt(L, 1, LUA_TCDATA);
  ct = lj_cdata_index_l(L, cts, cdataV(o), o+1, &p, &qual, &snap, &id);
  if ((qual & 1)) {
    if ((qual & CTF_CONST))
      lj_err_caller(L, LJ_ERR_FFI_WRCONST);
    return ffi_index_meta(L, cts, id, MM_newindex);
  }
  lj_cdata_set_l(L, cts, ct, id, p, o+2, qual);
  return 0;
}

/* Common handler for cdata arithmetic. */
static int ffi_arith(lua_State *L)
{
  MMS mm = (MMS)(curr_func(L)->c.ffid - (int)FF_ffi_meta___eq + (int)MM_eq);
  return lj_carith_op(L, mm);
}

/* The following functions must be in contiguous ORDER MM. */
LJLIB_CF(ffi_meta___eq)		LJLIB_REC(cdata_arith MM_eq)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___len)	LJLIB_REC(cdata_arith MM_len)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___lt)		LJLIB_REC(cdata_arith MM_lt)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___le)		LJLIB_REC(cdata_arith MM_le)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___concat)	LJLIB_REC(cdata_arith MM_concat)
{
  return ffi_arith(L);
}

/* Forward declaration. */
static int lj_cf_ffi_new(lua_State *L);

LJLIB_CF(ffi_meta___call)	LJLIB_REC(cdata_call)
{
  CTState *cts = ctype_cts(L);
  GCcdata *cd = ffi_checkcdata(L, 1);
  CTypeID id = cd->ctypeid;
  CType *ct;
  TValue metatv;
  cTValue *tv;
  MMS mm = MM_call;
  if (cd->ctypeid == CTID_CTYPEID) {
    id = *(CTypeID *)cdataptr(cd);
    mm = MM_new;
  } else {
    int ret = lj_ccall_func(L, cd);
    if (ret >= 0)
      return ret;
  }
  /* Handle ctype __call/__new metamethod. */
  ct = ctype_raw(cts, id);
  if (ctype_isptr(ct->info)) id = ctype_cid(ct->info);
  tv = lj_ctype_metatv(cts, &metatv, id, mm);
  if (tv)
    return lj_meta_tailcall(L, tv);
  else if (mm == MM_call)
    lj_err_callerv(L, LJ_ERR_FFI_BADCALL, strdata(lj_ctype_repr(L, id, NULL)));
  return lj_cf_ffi_new(L);
}

LJLIB_CF(ffi_meta___add)	LJLIB_REC(cdata_arith MM_add)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___sub)	LJLIB_REC(cdata_arith MM_sub)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___mul)	LJLIB_REC(cdata_arith MM_mul)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___div)	LJLIB_REC(cdata_arith MM_div)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___mod)	LJLIB_REC(cdata_arith MM_mod)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___pow)	LJLIB_REC(cdata_arith MM_pow)
{
  return ffi_arith(L);
}

LJLIB_CF(ffi_meta___unm)	LJLIB_REC(cdata_arith MM_unm)
{
  return ffi_arith(L);
}
/* End of contiguous ORDER MM. */

LJLIB_CF(ffi_meta___tostring)
{
  GCcdata *cd = ffi_checkcdata(L, 1);
  const char *msg = "cdata<%s>: %p";
  CTypeID id = cd->ctypeid;
  void *p = cdataptr(cd);
  if (id == CTID_CTYPEID) {
    msg = "ctype<%s>";
    id = *(CTypeID *)p;
  } else {
    CTState *cts = ctype_cts(L);
    CTypeID rid = ctype_rawid(cts, id);
    CType *ct = ctype_get(cts, rid);
    if (ctype_isref(ct->info)) {
      p = *(void **)p;
      rid = ctype_rawid(cts, ctype_cid(ct->info));
      ct = ctype_get(cts, rid);
    }
    if (ctype_iscomplex(ct->info)) {
      setstrV(L, L->top-1, lj_ctype_repr_complex(L, cdataptr(cd), ct->size));
      goto checkgc;
    } else if (ct->size == 8 && ctype_isinteger(ct->info)) {
      setstrV(L, L->top-1, lj_ctype_repr_int64(L, *(uint64_t *)cdataptr(cd),
					       (ct->info & CTF_UNSIGNED)));
      goto checkgc;
    } else if (ctype_isfunc(ct->info)) {
      p = *(void **)p;
    } else if (ctype_isenum(ct->info)) {
      msg = "cdata<%s>: %d";
      p = (void *)(uintptr_t)*(uint32_t *)p;
    } else {
      if (ctype_isptr(ct->info)) {
	p = cdata_getptr(p, ct->size);
	rid = ctype_rawid(cts, ctype_cid(ct->info));
	ct = ctype_get(cts, rid);
      }
      if (ctype_isstruct(ct->info) || ctype_isvector(ct->info)) {
	/* Handle ctype __tostring metamethod. */
	TValue metatv;
	cTValue *tv = lj_ctype_metatv(cts, &metatv, rid, MM_tostring);
	if (tv)
	  return lj_meta_tailcall(L, tv);
      }
    }
  }
  lj_strfmt_pushf(L, msg, strdata(lj_ctype_repr(L, id, NULL)), p);
checkgc:
  lj_gc_check(L);
  return 1;
}

static int ffi_pairs(lua_State *L, MMS mm)
{
  CTState *cts = ctype_cts(L);
  CTypeID id = ffi_checkcdata(L, 1)->ctypeid;
  CType *ct = ctype_raw(cts, id);
  TValue metatv;
  cTValue *tv;
  if (ctype_isptr(ct->info)) id = ctype_cid(ct->info);
  tv = lj_ctype_metatv(cts, &metatv, id, mm);
  if (!tv)
    lj_err_callerv(L, LJ_ERR_FFI_BADMM, strdata(lj_ctype_repr(L, id, NULL)),
		   strdata(mmname_str(G(L), mm)));
  return lj_meta_tailcall(L, tv);
}

LJLIB_CF(ffi_meta___pairs)
{
  return ffi_pairs(L, MM_pairs);
}

LJLIB_CF(ffi_meta___ipairs)
{
  return ffi_pairs(L, MM_ipairs);
}

LJLIB_PUSH("ffi") LJLIB_SET(__metatable)

#include "lj_libdef.h"

/* -- C library metamethods ----------------------------------------------- */

#define LJLIB_MODULE_ffi_clib

/* Index C library by a name. */
static TValue *ffi_clib_index(lua_State *L)
{
  TValue *o = L->base;
  CLibrary *cl;
  if (!(o < L->top && tvisudata(o) &&
	lj_udata_udtype_acq(udataV(o)) == UDTYPE_FFI_CLIB))
    lj_err_argt(L, 1, LUA_TUSERDATA);
  cl = (CLibrary *)uddata(udataV(o));
  if (!(o+1 < L->top && tvisstr(o+1)))
    lj_err_argt(L, 2, LUA_TSTRING);
  return lj_clib_index(L, cl, strV(o+1));
}

LJLIB_CF(ffi_clib___index)	LJLIB_REC(clib_index 1)
{
  TValue *tv = ffi_clib_index(L);
  if (tviscdata(tv)) {
    CTState *cts = ctype_cts(L);
    GCcdata *cd = cdataV(tv);
    CType *s = ctype_get(cts, cd->ctypeid);
    if (ctype_isextern(s->info)) {
      CTypeID sid = ctype_cid(s->info);
      void *sp = *(void **)cdataptr(cd);
      CType *ct = ctype_raw(cts, sid);
      if (lj_cconv_tv_ct_l(L, cts, ct, sid, L->top-1, sp))
	lj_gc_check(L);
      return 1;
    }
  }
  copyTV(L, L->top-1, tv);
  return 1;
}

LJLIB_CF(ffi_clib___newindex)	LJLIB_REC(clib_index 0)
{
  TValue *tv = ffi_clib_index(L);
  TValue *o = L->base+2;
  if (o < L->top && tviscdata(tv)) {
    CTState *cts = ctype_cts(L);
    GCcdata *cd = cdataV(tv);
    CTypeID did = cd->ctypeid;
    CType *d = ctype_get(cts, did);
    if (ctype_isextern(d->info)) {
      CTInfo qual = 0;
      for (;;) {  /* Skip attributes and collect qualifiers. */
	did = ctype_cid(d->info);
	d = ctype_get(cts, did);
	if (!ctype_isattrib(d->info)) break;
	if (ctype_attrib(d->info) == CTA_QUAL) qual |= d->size;
      }
      if (!((d->info|qual) & CTF_CONST)) {
	lj_cconv_ct_tv_l(L, cts, d, did, *(void **)cdataptr(cd), o, 0);
	return 0;
      }
    }
  }
  lj_err_caller(L, LJ_ERR_FFI_WRCONST);
  return 0;  /* unreachable */
}

LJLIB_CF(ffi_clib___gc)
{
  TValue *o = L->base;
  if (o < L->top && tvisudata(o) &&
      lj_udata_udtype_acq(udataV(o)) == UDTYPE_FFI_CLIB)
    lj_clib_unload(L, G(L), (CLibrary *)uddata(udataV(o)));
  return 0;
}

#include "lj_libdef.h"

/* -- Callback function metamethods --------------------------------------- */

#define LJLIB_MODULE_ffi_callback

static int ffi_callback_set(lua_State *L, GCfunc *fn)
{
  GCcdata *cd = ffi_checkcdata(L, 1);
  CTState *cts = ctype_cts(L);
  CType *ct = ctype_raw(cts, cd->ctypeid);
  if (ctype_isptr(ct->info) && (LJ_32 || ct->size == 8)) {
    MSize slot = lj_ccallback_ptr2slot(cts, *(void **)cdataptr(cd));
    CTypeID1 *cbid = NULL;
    lua_State **owner = NULL;
    if (slot < la_load32_acq(&cts->cb.sizeid) &&
	(cbid = (CTypeID1 *)la_loadptr_acq((void *const *)&cts->cb.cbid)) != NULL &&
	la_load16_acq(&cbid[slot]) != 0) {
      if (fn) {
	lj_ccallback_func_store_l(L, cts, slot, fn);
      } else {
	owner = (lua_State **)la_loadptr_acq((void *const *)&cts->cb.owner);
	if (owner && la_loadptr_acq((void *const *)&owner[slot]) == NULL) {
	  /* 11.5 disowned callback free: nil function before cbid release. */
	  lj_ccallback_func_clear(cts, slot);
	  la_store16_rel(&cbid[slot], 0);
	} else {
	  /* 11.5 owned callback free: cbid release before owner release. */
	  la_store16_rel(&cbid[slot], 0);
	  lj_ccallback_func_clear(cts, slot);
	  if (owner)
	    la_storeptr_rel((void **)&owner[slot], NULL);  /* 11.5 slot reusable. */
	}
      }
      return 0;
    }
  }
  lj_err_caller(L, LJ_ERR_FFI_BADCBACK);
  return 0;
}

LJLIB_CF(ffi_callback_free)
{
  return ffi_callback_set(L, NULL);
}

LJLIB_CF(ffi_callback_set)
{
  GCfunc *fn = lj_lib_checkfunc(L, 2);
  return ffi_callback_set(L, fn);
}

LJLIB_PUSH(top-1) LJLIB_SET(__index)

#include "lj_libdef.h"

/* -- ffi.pin() handle methods ------------------------------------------- */

#define LJLIB_MODULE_ffi_pin

static GCudata *ffi_pin_check(lua_State *L)
{
  TValue *o = L->base;
  if (!(o < L->top && tvisudata(o) &&
	lj_udata_udtype_acq(udataV(o)) == UDTYPE_FFI_PIN))
    lj_err_argtype(L, 1, "ffi.pin");
  return udataV(o);
}

static void ffi_pin_release_l(lua_State *L, GCudata *ud)
{
  TValue nilv;
  setnilV(&nilv);
  copyTVrel(L, (TValue *)uddata(ud), &nilv);
}

LJLIB_CF(ffi_pin_release)
{
  ffi_pin_release_l(L, ffi_pin_check(L));
  return 0;
}

LJLIB_CF(ffi_pin___gc)
{
  TValue *o = L->base;
  if (o < L->top && tvisudata(o) &&
      lj_udata_udtype_acq(udataV(o)) == UDTYPE_FFI_PIN)
    ffi_pin_release_l(L, udataV(o));
  return 0;
}

LJLIB_CF(ffi_pin___tostring)
{
  (void)ffi_pin_check(L);
  lua_pushliteral(L, "ffi.pin");
  return 1;
}

LJLIB_PUSH("ffi.pin") LJLIB_SET(__metatable)
LJLIB_PUSH(top-1) LJLIB_SET(__index)

#include "lj_libdef.h"

/* -- FFI library functions ----------------------------------------------- */

#define LJLIB_MODULE_ffi

LJLIB_CF(ffi_cdef)
{
  GCstr *s = lj_lib_checkstr(L, 1);
  CPState cp;
  int errcode;
  cp.L = L;
  cp.cts = ctype_cts(L);
  cp.srcname = strdata(s);
  cp.p = strdata(s);
  cp.param = L->base+1;
  cp.mode = CPARSE_MODE_MULTI|CPARSE_MODE_DIRECT;
  lj_ctype_parse_lock(cp.cts, L);
  errcode = lj_cparse(&cp);
  lj_ctype_parse_unlock(cp.cts);
  if (errcode) lj_err_throw(L, errcode);  /* Propagate errors. */
  lj_gc_check(L);
  return 0;
}

LJLIB_CF(ffi_new)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  CTypeID id, rid;
  CType *ct;
  CTSize sz = CTSIZE_INVALID;
  CTInfo info = 0;
  MSize ofs = 1;
  TValue *o;
  GCcdata *cd;
  int isstr, neednelem = 0;
  id = ffi_checkctype_noparse(L, NULL, &isstr);
  if (!isstr) {
    int ok = ffi_new_layout_snapshot(cts, id, 0, 0, &rid, &info, &sz,
				     &neednelem);
    if (ok > 0 && neednelem) {
      CTSize nelem = (CTSize)ffi_checkint(L, 2);
      ofs = 2;
      ok = ffi_new_layout_snapshot(cts, id, nelem, 1, &rid, &info, &sz,
				   &neednelem);
    }
    if (ok > 0)
      goto got_layout;
    if (ok == 0) {
      sz = CTSIZE_INVALID;
      goto got_layout;  /* Invalid/abandoned ID: report as invalid size. */
    }
  }
  id = ffi_checkctype_layout_lock(L, cts, NULL);
  rid = ctype_rawid(cts, id);
  ct = ctype_get(cts, rid);
  info = lj_ctype_info(cts, id, &sz);
  if ((info & CTF_VLA)) {
    CTSize nelem;
    lj_ctype_parse_unlock(cts);
    nelem = (CTSize)ffi_checkint(L, 2);
    id = ffi_checkctype_layout_lock(L, cts, NULL);
    rid = ctype_rawid(cts, id);
    ct = ctype_get(cts, rid);
    info = lj_ctype_info(cts, id, &sz);
    ofs = 2;
    sz = (info & CTF_VLA) ? lj_ctype_vlsize(cts, ct, nelem) : CTSIZE_INVALID;
  }
  if (sz == CTSIZE_INVALID) {
    lj_ctype_parse_unlock(cts);
    lj_err_arg(L, 1, LJ_ERR_FFI_INVSIZE);
  }
  lj_ctype_parse_unlock(cts);  /* 11.2: ffi.new waits out parser rollback. */
got_layout:
  if (sz == CTSIZE_INVALID)
    lj_err_arg(L, 1, LJ_ERR_FFI_INVSIZE);
  o = L->base + ofs;
  cd = lj_cdata_newx_l(L, cts, id, sz, info);
  setcdataV(L, o-1, cd);  /* Anchor the uninitialized cdata. */
  ct = ctype_get(cts, rid);  /* Table may have been reallocated. */
  lj_cconv_ct_init_l(L, cts, ct, rid, sz, cdataptr(cd),
		     o, (MSize)(L->top - o));  /* Initialize cdata. */
  if (ctype_isstruct(ct->info)) {
    /* Handle ctype __gc metamethod. Use the fast lookup here. */
    TValue gctv;
    cTValue *tv = lj_ctype_metatv(cts, &gctv, id, MM_gc);
    if (tv)
      lj_cdata_setfin(L, cd, gcV(tv), itype(tv));
  }
  L->top = o;  /* Only return the cdata itself. */
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_cast)	LJLIB_REC(ffi_new)
{
  CTState *cts = ctype_cts(L);
  CTypeID id = ffi_checkctype(L, cts, NULL);
  CType *d = ctype_raw(cts, id);
  TValue *o = lj_lib_checkany(L, 2);
  ptrdiff_t ofs = o - L->base;
  L->top = o+1;  /* Make sure this is the last item on the stack. */
  lj_state_checkstack(L, 1);
  o = L->base + ofs;
  if (!(ctype_isnum(d->info) || ctype_isptr(d->info) || ctype_isenum(d->info)))
    lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  if (!(tviscdata(o) && cdataV(o)->ctypeid == id)) {
    GCcdata *cd = lj_cdata_new_l(L, cts, id, d->size);
    setcdataV(L, L->top++, cd);  /* Anchor across callback allocation. */
    lj_cconv_ct_tv_l(L, cts, d, ctype_rawid(cts, id), cdataptr(cd), o,
		     CCF_CAST);
    L->top = o+1;
    setcdataV(L, o, cd);
    lj_gc_check(L);
  }
  return 1;
}

LJLIB_CF(ffi_typeof)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  CTypeID id = ffi_checkctype(L, cts, L->base+1);
  GCcdata *cd = lj_cdata_new_(L, CTID_CTYPEID, 4);
  *(CTypeID *)cdataptr(cd) = id;
  setcdataV(L, L->top-1, cd);
  lj_gc_check(L);
  return 1;
}

static void ffi_typeinfo_storeint(lua_State *L, GCtab *tab, GCstr *key,
				  int32_t val)
{
  TValue tv, *dst;
  setintV(&tv, val);
  for (;;) {
    dst = lj_tab_setstr(L, tab, key);
    if (lj_tab_trystoretv_cas(L, dst, &tv) == LJ_TAB_STORE_CAS_OK)
      return;
    la_cpu_pause();  /* FFI typeinfo int store saw FORWARD after lookup. */
  }
}

static void ffi_typeinfo_storestr(lua_State *L, GCtab *tab, GCstr *key,
				  GCstr *val)
{
  TValue tv, *dst;
  setstrV(L, &tv, val);
  for (;;) {
    dst = lj_tab_setstr(L, tab, key);
    if (lj_tab_trystoretv_cas(L, dst, &tv) == LJ_TAB_STORE_CAS_OK)
      return;
    la_cpu_pause();  /* FFI typeinfo string store saw FORWARD after lookup. */
  }
}

static int ffi_typeinfo_snapshot_locked(CTState *cts, CTypeID id, CType *out)
{
  CType *ct;
  GCobj *name;
  if (!(id > 0 && id < ctype_top_acq(cts)))
    return 0;
  ct = ctype_get(cts, id);
  out->info = la_load32_acq(&ct->info);
  out->size = la_load32_acq(&ct->size);
  out->sib = (CTypeID1)la_load16_acq(&ct->sib);
  out->next = (CTypeID1)la_load16_acq(&ct->next);
  name = gcref_acq(ct->name);
  setgcrefp(out->name, name);
  return !ctype_isabandoned(out->info);
}

/* Internal and unsupported API. */
LJLIB_CF(ffi_typeinfo)
{
  CTState *cts = ctype_cts(L);
  CTypeID id = (CTypeID)ffi_checkint(L, 1);
  CType snap;
  CTInfo info;
  CTSize size;
  CTypeID sib;
  GCstr *name;
  int ok = lj_ctype_snapshot(cts, id, &snap);
  if (ok < 0) {
    lj_ctype_parse_lock(cts, L);
    ok = ffi_typeinfo_snapshot_locked(cts, id, &snap);
    lj_ctype_parse_unlock(cts);
  }
  if (ok > 0) {
    GCtab *t;
    info = snap.info;
    size = snap.size;
    sib = snap.sib;
    name = ctype_name_acq(&snap);
    lua_createtable(L, 0, 4);  /* Increment hash size if fields are added. */
    t = tabV(L->top-1);
    ffi_typeinfo_storeint(L, t, lj_str_newlit(L, "info"), (int32_t)info);
    if (size != CTSIZE_INVALID)
      ffi_typeinfo_storeint(L, t, lj_str_newlit(L, "size"), (int32_t)size);
    if (sib)
      ffi_typeinfo_storeint(L, t, lj_str_newlit(L, "sib"), (int32_t)sib);
    if (name) {
      if (isdead(G(L), obj2gco(name))) flipwhite(obj2gco(name));
      ffi_typeinfo_storestr(L, t, lj_str_newlit(L, "name"), name);
    }
    lj_gc_pubtab(L, t);
    lj_gc_check(L);
    return 1;
  }
  return 0;
}

LJLIB_CF(ffi_istype)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  CTypeID id1 = ffi_checkctype(L, cts, NULL);
  TValue *o = lj_lib_checkany(L, 2);
  int b = 0;
  if (tviscdata(o)) {
    GCcdata *cd = cdataV(o);
    CTypeID id2 = cd->ctypeid == CTID_CTYPEID ? *(CTypeID *)cdataptr(cd) :
						cd->ctypeid;
    CTypeID rid1 = ctype_rawrefid(cts, id1);
    CTypeID rid2 = ctype_rawrefid(cts, id2);
    CType *ct1 = ctype_get(cts, rid1);
    CType *ct2 = ctype_get(cts, rid2);
    if (rid1 == rid2) {
      b = 1;
    } else if (ctype_type(ct1->info) == ctype_type(ct2->info) &&
	       ct1->size == ct2->size) {
      if (ctype_ispointer(ct1->info))
	b = lj_cconv_compatptr(cts, ct1, ct2, CCF_IGNQUAL);
      else if (ctype_isnum(ct1->info) || ctype_isvoid(ct1->info))
	b = (((ct1->info ^ ct2->info) & ~(CTF_QUAL|CTF_LONG)) == 0);
    } else if (ctype_isstruct(ct1->info) && ctype_isptr(ct2->info) &&
	       rid1 == ctype_rawid(cts, ctype_cid(ct2->info))) {
      b = 1;
    }
  }
  setboolV(L->top-1, b);
  setboolV(&L2TG(L)->tmptv2, b);  /* Remember for trace recorder. */
  return 1;
}

typedef struct FFILayoutSnap {
  CTState *cts;
  CTypeTab *tabh;
  CTypeID top;
  uint32_t seq;
  MSize budget;
} FFILayoutSnap;

static int ffi_layout_begin(CTState *cts, FFILayoutSnap *ls)
{
  uint32_t seq = la_load32_acq(&cts->parse_token);
  if (seq & 1u)
    return -1;
  ls->cts = cts;
  ls->top = ctype_top_acq(cts);
  ls->tabh = ctype_tabh_acq(cts);
  ls->seq = seq;
  ls->budget = ls->top ? (MSize)ls->top * 2u : 1u;
  return 1;
}

static int ffi_layout_end(FFILayoutSnap *ls)
{
  uint32_t seq = la_load32_acq(&ls->cts->parse_token);
  return (seq == ls->seq && !(seq & 1u)) ? 1 : -1;
}

static int ffi_layout_get(FFILayoutSnap *ls, CTypeID id, CType *out)
{
  CType *ct;
  GCobj *name;
  if (id == 0 || id >= ls->top || (MSize)id >= ls->tabh->sizetab)
    return 0;
  if (ls->budget-- == 0)
    return -1;
  ct = &ls->tabh->tab[id];
  out->info = la_load32_acq(&ct->info);
  out->size = la_load32_acq(&ct->size);
  out->sib = (CTypeID1)la_load16_acq(&ct->sib);
  out->next = (CTypeID1)la_load16_acq(&ct->next);
  name = gcref_acq(ct->name);
  setgcrefp(out->name, name);
  return ctype_isabandoned(out->info) ? 0 : 1;
}

static int ffi_layout_rawref(FFILayoutSnap *ls, CTypeID id, CType *out)
{
  int ok;
  for (;;) {
    ok = ffi_layout_get(ls, id, out);
    if (ok <= 0)
      return ok;
    if (!(ctype_isattrib(out->info) || ctype_isref(out->info)))
      return 1;
    id = ctype_cid(out->info);
  }
}

static int ffi_layout_rawid(FFILayoutSnap *ls, CTypeID id, CTypeID *ridp,
			    CType *out)
{
  int ok;
  for (;;) {
    ok = ffi_layout_get(ls, id, out);
    if (ok <= 0)
      return ok;
    if (!ctype_isattrib(out->info)) {
      *ridp = id;
      return 1;
    }
    id = ctype_cid(out->info);
  }
}

static int ffi_layout_raw(FFILayoutSnap *ls, CTypeID id, CType *out)
{
  int ok;
  for (;;) {
    ok = ffi_layout_get(ls, id, out);
    if (ok <= 0)
      return ok;
    if (!ctype_isattrib(out->info))
      return 1;
    id = ctype_cid(out->info);
  }
}

static int ffi_layout_rawchild(FFILayoutSnap *ls, const CType *ct, CType *out)
{
  CTypeID id = ctype_cid(ct->info);
  int ok;
  do {
    ok = ffi_layout_get(ls, id, out);
    if (ok <= 0)
      return ok;
    if (!ctype_isattrib(out->info))
      return 1;
    id = ctype_cid(out->info);
  } while (1);
}

static int ffi_layout_info(FFILayoutSnap *ls, CTypeID id,
			   CTInfo *infop, CTSize *szp)
{
  CTInfo qual = 0;
  CType ct;
  int ok = ffi_layout_get(ls, id, &ct);
  if (ok <= 0)
    return ok;
  for (;;) {
    CTInfo info = ct.info;
    if (ctype_isenum(info)) {
      /* Follow child. Need to look at its attributes, too. */
    } else if (ctype_isattrib(info)) {
      if (ctype_isxattrib(info, CTA_QUAL))
	qual |= ct.size;
      else if (ctype_isxattrib(info, CTA_ALIGN) && !(qual & CTFP_ALIGNED))
	qual |= CTFP_ALIGNED + CTALIGN(ct.size);
    } else {
      if (!(qual & CTFP_ALIGNED)) qual |= (info & CTF_ALIGN);
      qual |= (info & ~(CTF_ALIGN|CTMASK_CID));
      *infop = qual;
      *szp = ctype_isfunc(info) ? CTSIZE_INVALID : ct.size;
      return 1;
    }
    ok = ffi_layout_get(ls, ctype_cid(info), &ct);
    if (ok <= 0)
      return ok;
  }
}

static int ffi_layout_info_raw(FFILayoutSnap *ls, CTypeID id,
			       CTInfo *infop, CTSize *szp)
{
  CTInfo qual = 0;
  CType ct;
  int ok = ffi_layout_get(ls, id, &ct);
  if (ok <= 0)
    return ok;
  if (ctype_isref(ct.info)) {
    id = ctype_cid(ct.info);
    ok = ffi_layout_get(ls, id, &ct);
    if (ok <= 0)
      return ok;
  }
  for (;;) {
    CTInfo info = ct.info;
    if (ctype_isenum(info)) {
      /* Follow child. Need to look at its attributes, too. */
    } else if (ctype_isattrib(info)) {
      if (ctype_isxattrib(info, CTA_QUAL))
	qual |= ct.size;
      else if (ctype_isxattrib(info, CTA_ALIGN) && !(qual & CTFP_ALIGNED))
	qual |= CTFP_ALIGNED + CTALIGN(ct.size);
    } else {
      if (!(qual & CTFP_ALIGNED)) qual |= (info & CTF_ALIGN);
      qual |= (info & ~(CTF_ALIGN|CTMASK_CID));
      *infop = qual;
      *szp = ctype_isfunc(info) ? CTSIZE_INVALID : ct.size;
      return 1;
    }
    ok = ffi_layout_get(ls, ctype_cid(info), &ct);
    if (ok <= 0)
      return ok;
  }
}

static int ffi_layout_vlsize(FFILayoutSnap *ls, const CType *ct,
			     CTSize nelem, CTSize *szp)
{
  CType cur = *ct, elem;
  uint64_t xsz = 0;
  int ok;
  if (ctype_isstruct(cur.info)) {
    CTypeID arrid = 0, fid = cur.sib;
    xsz = cur.size;
    while (fid) {
      ok = ffi_layout_get(ls, fid, &cur);
      if (ok <= 0)
	return ok;
      if (ctype_type(cur.info) == CT_FIELD)
	arrid = ctype_cid(cur.info);
      fid = cur.sib;
    }
    if (arrid == 0)
      return 0;
    ok = ffi_layout_raw(ls, arrid, &cur);
    if (ok <= 0)
      return ok;
  }
  if (!ctype_isvlarray(cur.info))
    return 0;
  ok = ffi_layout_rawchild(ls, &cur, &elem);
  if (ok <= 0)
    return ok;
  if (!ctype_hassize(elem.info))
    return 0;
  xsz += (uint64_t)elem.size * nelem;
  *szp = xsz < 0x80000000u ? (CTSize)xsz : CTSIZE_INVALID;
  return 1;
}

static int ffi_new_layout_snapshot(CTState *cts, CTypeID id, CTSize nelem,
				   int hasnelem, CTypeID *ridp,
				   CTInfo *infop, CTSize *szp,
				   int *neednelem)
{
  FFILayoutSnap ls;
  CType raw;
  int ok = ffi_layout_begin(cts, &ls);
  if (ok < 0)
    return -1;
  ok = ffi_layout_rawid(&ls, id, ridp, &raw);
  if (ok > 0) {
    ok = ffi_layout_info(&ls, id, infop, szp);
    if (ok > 0) {
      if ((*infop & CTF_VLA)) {
	if (!hasnelem) {
	  *neednelem = 1;
	} else {
	  *neednelem = 0;
	  ok = ffi_layout_vlsize(&ls, &raw, nelem, szp);
	}
      } else {
	*neednelem = 0;
      }
    }
  }
  if (ok >= 0 && ffi_layout_end(&ls) < 0)
    return -1;
  return ok;
}

static int ffi_layout_sizeof_snapshot(CTState *cts, CTypeID id, CTSize nelem,
				      int hasnelem, CTSize *szp, int *neednelem)
{
  FFILayoutSnap ls;
  CType ct;
  int ok = ffi_layout_begin(cts, &ls);
  if (ok < 0)
    return -1;
  ok = ffi_layout_rawref(&ls, id, &ct);
  if (ok > 0) {
    if (ctype_isvltype(ct.info)) {
      if (!hasnelem) {
	*neednelem = 1;
      } else {
	ok = ffi_layout_vlsize(&ls, &ct, nelem, szp);
      }
    } else {
      *neednelem = 0;
      *szp = ctype_hassize(ct.info) ? ct.size : CTSIZE_INVALID;
    }
  }
  if (ok >= 0 && ffi_layout_end(&ls) < 0)
    return -1;
  return ok;
}

static int ffi_layout_alignof_snapshot(CTState *cts, CTypeID id, CTSize *alignp)
{
  FFILayoutSnap ls;
  CTInfo info;
  CTSize sz;
  int ok = ffi_layout_begin(cts, &ls);
  if (ok < 0)
    return -1;
  ok = ffi_layout_info_raw(&ls, id, &info, &sz);
  if (ok > 0)
    *alignp = (CTSize)1u << ctype_align(info);
  if (ok >= 0 && ffi_layout_end(&ls) < 0)
    return -1;
  return ok;
}

static int ffi_layout_getfield(FFILayoutSnap *ls, const CType *root,
			       GCstr *name, CTSize *ofs, CType *out)
{
  CType ct = *root;
  CTypeID sid = ct.sib;
  while (sid) {
    int ok = ffi_layout_get(ls, sid, &ct);
    if (ok <= 0)
      return ok;
    if (ctype_name_acq(&ct) == name) {
      *ofs = ct.size;
      *out = ct;
      return 1;
    }
    if (ctype_isxattrib(ct.info, CTA_SUBTYPE)) {
      CType cct, fct;
      CTSize subofs;
      ok = ffi_layout_get(ls, ctype_cid(ct.info), &cct);
      if (ok <= 0)
	return ok;
      while (ctype_isattrib(cct.info)) {
	ok = ffi_layout_get(ls, ctype_cid(cct.info), &cct);
	if (ok <= 0)
	  return ok;
      }
      ok = ffi_layout_getfield(ls, &cct, name, &subofs, &fct);
      if (ok != 0) {
	if (ok > 0) {
	  *ofs = subofs + ct.size;
	  *out = fct;
	}
	return ok;
      }
    }
    sid = ct.sib;
  }
  return 0;
}

static int ffi_layout_offsetof_snapshot(CTState *cts, CTypeID id, GCstr *name,
					CTSize *ofs, CType *out)
{
  FFILayoutSnap ls;
  CType ct;
  int ok = ffi_layout_begin(cts, &ls);
  if (ok < 0)
    return -1;
  ok = ffi_layout_rawref(&ls, id, &ct);
  if (ok > 0) {
    if (ctype_isstruct(ct.info) && ct.size != CTSIZE_INVALID)
      ok = ffi_layout_getfield(&ls, &ct, name, ofs, out);
    else
      ok = 0;
  }
  if (ok >= 0 && ffi_layout_end(&ls) < 0)
    return -1;
  return ok;
}

LJLIB_CF(ffi_sizeof)	LJLIB_REC(ffi_xof FF_ffi_sizeof)
{
  CTState *cts = ctype_cts(L);
  CTypeID id;
  CTSize sz;
  if (LJ_UNLIKELY(tviscdata(L->base) && cdataisv(cdataV(L->base)))) {
    sz = cdatavlen(cdataV(L->base));
  } else {
    int isstr, neednelem = 0;
    id = ffi_checkctype_noparse(L, NULL, &isstr);
    if (!isstr) {
      int ok = ffi_layout_sizeof_snapshot(cts, id, 0, 0, &sz, &neednelem);
      if (ok > 0 && neednelem) {
	CTSize nelem = (CTSize)ffi_checkint(L, 2);
	neednelem = 0;
	ok = ffi_layout_sizeof_snapshot(cts, id, nelem, 1, &sz, &neednelem);
      }
      if (ok > 0) {
	if (LJ_UNLIKELY(sz == CTSIZE_INVALID)) {
	  setnilV(L->top-1);
	  return 1;
	}
	goto got_size;
      }
      if (ok == 0) {
	setnilV(L->top-1);
	return 1;
      }
    }
    id = ffi_checkctype_layout_lock(L, cts, NULL);
    /* 11.2: keep layout reads atomic against failed parser rollback. */
    CType *ct = lj_ctype_rawref(cts, id);
    if (ctype_isvltype(ct->info)) {
      CTSize nelem;
      lj_ctype_parse_unlock(cts);
      nelem = (CTSize)ffi_checkint(L, 2);
      id = ffi_checkctype_layout_lock(L, cts, NULL);
      ct = lj_ctype_rawref(cts, id);
      sz = ctype_isvltype(ct->info) ?
	   lj_ctype_vlsize(cts, ct, nelem) : CTSIZE_INVALID;
    } else {
      sz = ctype_hassize(ct->info) ? ct->size : CTSIZE_INVALID;
    }
    lj_ctype_parse_unlock(cts);
    if (LJ_UNLIKELY(sz == CTSIZE_INVALID)) {
      setnilV(L->top-1);
      return 1;
    }
  }
got_size:
  setintV(L->top-1, (int32_t)sz);
  return 1;
}

LJLIB_CF(ffi_alignof)	LJLIB_REC(ffi_xof FF_ffi_alignof)
{
  CTState *cts = ctype_cts(L);
  CTypeID id;
  CTSize align;
  int isstr;
  id = ffi_checkctype_noparse(L, NULL, &isstr);
  if (!isstr) {
    int ok = ffi_layout_alignof_snapshot(cts, id, &align);
    if (ok > 0) {
      setintV(L->top-1, (int32_t)align);
      return 1;
    }
  }
  id = ffi_checkctype_layout_lock(L, cts, NULL);
  {
    CTSize sz = 0;
    CTInfo info = lj_ctype_info_raw(cts, id, &sz);
    lj_ctype_parse_unlock(cts);
    setintV(L->top-1, 1 << ctype_align(info));
  }
  return 1;
}

LJLIB_CF(ffi_offsetof)	LJLIB_REC(ffi_xof FF_ffi_offsetof)
{
  CTState *cts = ctype_cts(L);
  CTypeID id;
  GCstr *name;
  CType *ct;
  CTSize ofs;
  int isstr;
  id = ffi_checkctype_noparse(L, NULL, &isstr);
  if (isstr)
    id = ffi_checkctype(L, cts, NULL);
  name = lj_lib_checkstr(L, 2);
  if (!isstr) {
    CType snap;
    int ok = ffi_layout_offsetof_snapshot(cts, id, name, &ofs, &snap);
    if (ok > 0) {
      setintV(L->top-1, ofs);
      if (ctype_isfield(snap.info)) {
	return 1;
      } else if (ctype_isbitfield(snap.info)) {
	setintV(L->top++, ctype_bitpos(snap.info));
	setintV(L->top++, ctype_bitbsz(snap.info));
	return 3;
      }
    } else if (ok == 0) {
      return 0;
    }
  }
  lj_ctype_parse_lock(cts, L);
  ct = lj_ctype_rawref(cts, id);
  if (ctype_isstruct(ct->info) && ct->size != CTSIZE_INVALID) {
    CType *fct = lj_ctype_getfield(cts, ct, name, &ofs);
    if (fct) {
      setintV(L->top-1, ofs);
      if (ctype_isfield(fct->info)) {
	lj_ctype_parse_unlock(cts);
	return 1;
      } else if (ctype_isbitfield(fct->info)) {
	setintV(L->top++, ctype_bitpos(fct->info));
	setintV(L->top++, ctype_bitbsz(fct->info));
	lj_ctype_parse_unlock(cts);
	return 3;
      }
    }
  }
  lj_ctype_parse_unlock(cts);
  return 0;
}

LJLIB_CF(ffi_errno)	LJLIB_REC(.)
{
  int err = errno;
  if (L->top > L->base)
    errno = ffi_checkint(L, 1);
  setintV(L->top++, err);
  return 1;
}

LJLIB_CF(ffi_string)	LJLIB_REC(.)
{
  CTState *cts = ctype_cts(L);
  TValue *o = lj_lib_checkany(L, 1);
  const char *p;
  size_t len;
  if (o+1 < L->top && !tvisnil(o+1)) {
    len = (size_t)ffi_checkint(L, 2);
    lj_cconv_ct_tv_l(L, cts, ctype_get(cts, CTID_P_CVOID), CTID_P_CVOID,
		     (uint8_t *)&p, o, CCF_ARG(1));
  } else {
    lj_cconv_ct_tv_l(L, cts, ctype_get(cts, CTID_P_CCHAR), CTID_P_CCHAR,
		     (uint8_t *)&p, o, CCF_ARG(1));
    len = strlen(p);
  }
  L->top = o+1;  /* Make sure this is the last item on the stack. */
  setstrV(L, o, lj_str_new(L, p, len));
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_copy)	LJLIB_REC(.)
{
  void *dp = ffi_checkptr(L, 1, CTID_P_VOID);
  void *sp = ffi_checkptr(L, 2, CTID_P_CVOID);
  TValue *o = L->base+1;
  CTSize len;
  if (tvisstr(o) && o+1 >= L->top)
    len = strV(o)->len+1;  /* Copy Lua string including trailing '\0'. */
  else
    len = (CTSize)ffi_checkint(L, 3);
  memcpy(dp, sp, len);
  return 0;
}

LJLIB_CF(ffi_fill)	LJLIB_REC(.)
{
  void *dp = ffi_checkptr(L, 1, CTID_P_VOID);
  CTSize len = (CTSize)ffi_checkint(L, 2);
  int32_t fill = 0;
  if (L->base+2 < L->top && !tvisnil(L->base+2)) fill = ffi_checkint(L, 3);
  memset(dp, fill, len);
  return 0;
}

/* Test ABI string. */
LJLIB_CF(ffi_abi)	LJLIB_REC(.)
{
  GCstr *s = lj_lib_checkstr(L, 1);
  int b = lj_cparse_case(s,
#if LJ_64
    "\00564bit"
#else
    "\00532bit"
#endif
#if LJ_ARCH_HASFPU
    "\003fpu"
#endif
#if LJ_ABI_SOFTFP
    "\006softfp"
#else
    "\006hardfp"
#endif
#if LJ_ABI_EABI
    "\004eabi"
#endif
#if LJ_ABI_WIN
    "\003win"
#endif
#if LJ_ABI_PAUTH
    "\005pauth"
#endif
#if LJ_TARGET_UWP
    "\003uwp"
#endif
#if LJ_LE
    "\002le"
#else
    "\002be"
#endif
#if LJ_GC64
    "\004gc64"
#endif
#if LJ_DUALNUM
    "\007dualnum"
#endif
  ) >= 0;
  setboolV(L->top-1, b);
  setboolV(&L2TG(L)->tmptv2, b);  /* Remember for trace recorder. */
  return 1;
}

LJLIB_PUSH(top-7) LJLIB_SET(!)  /* Store reference to miscmap table. */

LJLIB_CF(ffi_metatype)
{
  CTState *cts = ctype_cts(L);
  CTypeID id = ffi_checkctype(L, cts, NULL);
  GCtab *mt = lj_lib_checktab(L, 2);
  CTypeID rid = ctype_rawid(cts, id);
  CType *ct = ctype_get(cts, rid);
  TValue tmp;
  GCcdata *cd;
  if (!(ctype_isstruct(ct->info) || ctype_iscomplex(ct->info) ||
	ctype_isvector(ct->info)))
    lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  if (!lj_ctype_setmeta(cts, rid, mt))
    lj_err_caller(L, LJ_ERR_PROTMT);
  settabV(L, &tmp, mt);
  lj_gc_barrierroot(L, &tmp);  /* 11.2 metatype side root. */
  cd = lj_cdata_new_(L, CTID_CTYPEID, 4);
  *(CTypeID *)cdataptr(cd) = id;
  setcdataV(L, L->top-1, cd);
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_gc)	LJLIB_REC(.)
{
  GCcdata *cd = ffi_checkcdata(L, 1);
  TValue *fin = lj_lib_checkany(L, 2);
  CTState *cts = ctype_cts(L);
  CType *ct = ctype_raw(cts, cd->ctypeid);
  if (!(ctype_isptr(ct->info) || ctype_isstruct(ct->info) ||
	ctype_isrefarray(ct->info)))
    lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  lj_cdata_setfin(L, cd, gcval(fin), itype(fin));
  L->top = L->base+1;  /* Pass through the cdata object. */
  return 1;
}

LJLIB_CF(ffi_pin)
{
  TValue *o = lj_lib_checkany(L, 1);
  CTState *cts = ctype_cts(L);
  GCtab *mt = cts->pinmt;
  GCudata *ud = lj_udata_new(L, sizeof(TValue), mt);
  setgcrefmt(ud->metatable, obj2gco(mt));
  lj_gc_pubobjobj(L, ud, mt);
  lj_gc2_finreg_udata_register_mt(L, G(L), ud, mt);
  copyTVrel(L, (TValue *)uddata(ud), o);
  lj_gc_pubobjtv(L, ud, (TValue *)uddata(ud));
  lj_udata_udtype_rel(ud, UDTYPE_FFI_PIN);
  setudataV(L, L->top++, ud);
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(ffi_blocking)
{
  GCcdata *cd = ffi_checkcdata(L, 1);
  CTState *cts = ctype_cts(L);
  CTypeID id = ctype_rawid(cts, cd->ctypeid);
  CType *ct = ctype_get(cts, id);
  CTSize sz = CTSIZE_PTR;
  if (ctype_isptr(ct->info)) {
    sz = ct->size;
    id = ctype_rawid(cts, ctype_cid(ct->info));
    ct = ctype_get(cts, id);
  }
  if (!ctype_isfunc(ct->info))
    lj_err_arg(L, 1, LJ_ERR_FFI_INVTYPE);
  lj_ctype_cb_blacklist(cts, cdata_getptr(cdataptr(cd), sz));
  (void)lj_trace_flushall_hs(L);
  L->top = L->base+1;  /* Pass through the function pointer. */
  return 1;
}

LJLIB_PUSH(top-5) LJLIB_SET(!)  /* Store clib metatable in func environment. */

LJLIB_CF(ffi_load)
{
  GCstr *name = lj_lib_checkstr(L, 1);
  int global = (L->base+1 < L->top && tvistruecond(L->base+1));
  lj_clib_load(L, tabref_acq(curr_func(L)->c.env), name, global);
  return 1;
}

LJLIB_PUSH(top-4) LJLIB_SET(C)
LJLIB_PUSH(top-3) LJLIB_SET(os)
LJLIB_PUSH(top-2) LJLIB_SET(arch)

#include "lj_libdef.h"

/* ------------------------------------------------------------------------ */

static TValue *ffi_loaded_store(lua_State *L, GCtab *t, GCstr *name,
				cTValue *src)
{
  TValue *dst;
  for (;;) {
    dst = lj_tab_setstr(L, t, name);
    if (lj_tab_trystoretv_cas(L, dst, src) == LJ_TAB_STORE_CAS_OK)
      return dst;
    la_cpu_pause();  /* FFI module registry saw FORWARD after lookup. */
  }
}

static TValue *ffi_miscmap_store(lua_State *L, CTState *cts, GCstr *key,
				 cTValue *src)
{
  TValue *dst;
  for (;;) {
    dst = lj_tab_setstr(L, cts->miscmap, key);
    if (lj_tab_trystoretv_cas(L, dst, src) == LJ_TAB_STORE_CAS_OK)
      return dst;
    la_cpu_pause();  /* FFI miscmap store saw FORWARD after lookup. */
  }
}

/* Register FFI module as loaded. */
static void ffi_register_module(lua_State *L)
{
  cTValue *tmp = lj_tab_getstr(tabV(registry(L)), lj_str_newlit(L, "_LOADED"));
  if (tmp && tvistab(tmp)) {
    GCtab *t = tabV(tmp);
    GCstr *name = lj_str_newlit(L, LUA_FFILIBNAME);
    TValue key;
    setstrV(L, &key, name);
    ffi_loaded_store(L, t, name, L->top-1);
    lj_gc2_barrier_weak_write(L, t, &key, L->top-1);
    lj_gc_pubtab(L, t);
  }
}

LUALIB_API int luaopen_ffi(lua_State *L)
{
  CTState *cts = lj_ctype_init(L);
  lj_ccallback_init_l(L, cts);
  settabV(L, L->top++,
	  (cts->miscmap = lj_tab_new(L, 0, 1)));
  LJ_LIB_REG(L, NULL, ffi_meta);
  /* NOBARRIER: basemt is a GC root. */
  setgcrefroot(basemt_it(G(L), LJ_TCDATA), obj2gco(tabV(L->top-1)));
  LJ_LIB_REG(L, NULL, ffi_clib);
  LJ_LIB_REG(L, NULL, ffi_callback);
  ffi_miscmap_store(L, cts, &cts->g->strempty, L->top-1);
  lj_gc_pubtabobj(L, cts->miscmap, tabV(L->top-1));
  L->top--;
  LJ_LIB_REG(L, NULL, ffi_pin);
  cts->pinmt = tabV(L->top-1);
  L->top--;
  lj_clib_default(L, tabV(L->top-1));  /* Create ffi.C default namespace. */
  lua_pushliteral(L, LJ_OS_NAME);
  lua_pushliteral(L, LJ_ARCH_NAME);
  LJ_LIB_REG(L, NULL, ffi);  /* Note: no global "ffi" created! */
  ffi_register_module(L);
  return 1;
}

#endif
