/*
** Buffer library.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lib_buffer_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"

#if LJ_HASBUFFER
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_udata.h"
#include "lj_meta.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#include "lj_cconv.h"
#endif
#include "lj_strfmt.h"
#include "lj_serialize.h"
#include "lj_lib.h"

/* -- Helper functions ---------------------------------------------------- */

/* Check that the first argument is a string buffer. */
static SBufExt *buffer_tobuf(lua_State *L)
{
  if (!(L->base < L->top && tvisbuf(L->base)))
    lj_err_argtype(L, 1, "buffer");
  return bufV(L->base);
}

/* Ditto, but for writers. */
static LJ_AINLINE SBufExt *buffer_tobufw(lua_State *L)
{
  SBufExt *sbx = buffer_tobuf(L);
  setsbufXL_(sbx, L);
  return sbx;
}

#define buffer_toudata(sbx)	((GCudata *)(sbx)-1)

/* -- Buffer methods ------------------------------------------------------ */

#define LJLIB_MODULE_buffer_method

LJLIB_CF(buffer_method_free)
{
  SBufExt *sbx = buffer_tobuf(L);
  lj_bufx_free(L, sbx);
  L->top = L->base+1;  /* Chain buffer object. */
  return 1;
}

LJLIB_CF(buffer_method_reset)		LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobuf(L);
  lj_bufx_reset(sbx);
  L->top = L->base+1;  /* Chain buffer object. */
  return 1;
}

LJLIB_CF(buffer_method_skip)		LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobuf(L);
  MSize n = (MSize)lj_lib_checkintrange(L, 2, 0, LJ_MAX_BUF);
  MSize len = sbufxlen(sbx);
  if (n < len) {
    lj_buf_rptr_rel(sbx, lj_buf_rptr_acq(sbx) + n);
  } else if (sbufiscow(sbx)) {
    lj_buf_rptr_rel(sbx, lj_buf_wptr_acq((SBuf *)sbx));
  } else {
    char *b = lj_buf_bptr_acq((SBuf *)sbx);
    lj_buf_rptr_rel(sbx, b);
    lj_buf_wptr_rel((SBuf *)sbx, b);
  }
  L->top = L->base+1;  /* Chain buffer object. */
  return 1;
}

LJLIB_CF(buffer_method_set)		LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobuf(L);
  GCobj *ref;
  const char *p;
  MSize len;
#if LJ_HASFFI
  if (tviscdata(L->base+1)) {
    CTState *cts = ctype_cts(L);
    lj_cconv_ct_tv_l(L, cts, ctype_get(cts, CTID_P_CVOID), CTID_P_CVOID,
		     (uint8_t *)&p, L->base+1, CCF_ARG(2));
    len = (MSize)lj_lib_checkintrange(L, 3, 0, LJ_MAX_BUF);
  } else
#endif
  {
    GCstr *str = lj_lib_checkstrx(L, 2);
    p = strdata(str);
    len = str->len;
  }
  lj_bufx_free(L, sbx);
  lj_bufx_set_cow(L, sbx, p, len);
  ref = gcV(L->base+1);
  setgcrefrel(sbx->cowref, ref);
  lj_gc_pubobjobj(L, buffer_toudata(sbx), ref);
  L->top = L->base+1;  /* Chain buffer object. */
  return 1;
}

LJLIB_CF(buffer_method_put)		LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobufw(L);
  ptrdiff_t arg, narg = L->top - L->base;
  for (arg = 1; arg < narg; arg++) {
    TValue motv;
    cTValue *o = &L->base[arg], *mo = NULL;
  retry:
    if (tvisstr(o)) {
      lj_buf_putstr((SBuf *)sbx, strV(o));
    } else if (tvisint(o)) {
      lj_strfmt_putint((SBuf *)sbx, intV(o));
    } else if (tvisnum(o)) {
      lj_strfmt_putfnum((SBuf *)sbx, STRFMT_G14, numV(o));
    } else if (tvisbuf(o)) {
      SBufExt *sbx2 = bufV(o);
      MSize len;
      const char *p;
      if (sbx2 == sbx) lj_err_arg(L, (int)(arg+1), LJ_ERR_BUFFER_SELF);
      p = lj_bufx_data_acq(sbx2, &len);
      lj_buf_putmem((SBuf *)sbx, p, len);
    } else if (!mo &&
	       !tvisnil(mo = lj_meta_lookuptv(L, &motv, o, MM_tostring))) {
      /* Call __tostring metamethod inline. */
      copyTV(L, L->top++, mo);
      copyTV(L, L->top++, o);
      lua_call(L, 1, 1);
      o = &L->base[arg];  /* The stack may have been reallocated. */
      copyTV(L, &L->base[arg], L->top-1);
      L->top = L->base + narg;
      goto retry;  /* Retry with the result. */
    } else {
      lj_err_argtype(L, (int)(arg+1), "string/number/__tostring");
    }
    /* Probably not useful to inline other __tostring MMs, e.g. FFI numbers. */
  }
  L->top = L->base+1;  /* Chain buffer object. */
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(buffer_method_putf)		LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobufw(L);
  lj_strfmt_putarg(L, (SBuf *)sbx, 2, 2);
  L->top = L->base+1;  /* Chain buffer object. */
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(buffer_method_get)		LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobuf(L);
  ptrdiff_t arg, narg = L->top - L->base;
  if (narg == 1) {
    narg++;
    setnilV(L->top++);  /* get() is the same as get(nil). */
  }
  for (arg = 1; arg < narg; arg++) {
    TValue *o = &L->base[arg];
    MSize n = tvisnil(o) ? LJ_MAX_BUF :
	      (MSize) lj_lib_checkintrange(L, (int)(arg+1), 0, LJ_MAX_BUF);
    MSize len;
    const char *p = lj_bufx_data_acq(sbx, &len);
    if (n > len) n = len;
    setstrV(L, o, lj_str_new(L, p, n));
    lj_buf_rptr_rel(sbx, (char *)p + n);
  }
  if (lj_buf_rptr_acq(sbx) == lj_buf_wptr_acq((SBuf *)sbx) &&
      !sbufiscow(sbx)) {
    char *b = lj_buf_bptr_acq((SBuf *)sbx);
    lj_buf_rptr_rel(sbx, b);
    lj_buf_wptr_rel((SBuf *)sbx, b);
  }
  lj_gc_check(L);
  return (int)(narg-1);
}

#if LJ_HASFFI
LJLIB_CF(buffer_method_putcdata)	LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobufw(L);
  const char *p;
  MSize len;
  if (tviscdata(L->base+1)) {
    CTState *cts = ctype_cts(L);
    lj_cconv_ct_tv_l(L, cts, ctype_get(cts, CTID_P_CVOID), CTID_P_CVOID,
		     (uint8_t *)&p, L->base+1, CCF_ARG(2));
  } else {
    lj_err_argtype(L, 2, "cdata");
  }
  len = (MSize)lj_lib_checkintrange(L, 3, 0, LJ_MAX_BUF);
  lj_buf_putmem((SBuf *)sbx, p, len);
  L->top = L->base+1;  /* Chain buffer object. */
  return 1;
}

LJLIB_CF(buffer_method_reserve)		LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobufw(L);
  MSize sz = (MSize)lj_lib_checkintrange(L, 2, 0, LJ_MAX_BUF);
  GCcdata *cd;
  lj_buf_more((SBuf *)sbx, sz);
  ctype_loadffi(L);
  cd = lj_cdata_new_(L, CTID_P_UINT8, CTSIZE_PTR);
  *(void **)cdataptr(cd) = lj_buf_wptr_acq((SBuf *)sbx);
  setcdataV(L, L->top++, cd);
  setintV(L->top++, sbufleft(sbx));
  return 2;
}

LJLIB_CF(buffer_method_commit)		LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobuf(L);
  MSize len = (MSize)lj_lib_checkintrange(L, 2, 0, LJ_MAX_BUF);
  if (len > sbufleft(sbx)) lj_err_arg(L, 2, LJ_ERR_NUMRNG);
  lj_buf_wptr_rel((SBuf *)sbx, lj_buf_wptr_acq((SBuf *)sbx) + len);
  L->top = L->base+1;  /* Chain buffer object. */
  return 1;
}

LJLIB_CF(buffer_method_ref)		LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobuf(L);
  GCcdata *cd;
  MSize len;
  const char *p = lj_bufx_data_acq(sbx, &len);
  ctype_loadffi(L);
  cd = lj_cdata_new_(L, CTID_P_UINT8, CTSIZE_PTR);
  *(void **)cdataptr(cd) = (void *)p;
  setcdataV(L, L->top++, cd);
  setintV(L->top++, len);
  return 2;
}
#endif

LJLIB_CF(buffer_method_encode)		LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobufw(L);
  cTValue *o = lj_lib_checkany(L, 2);
  lj_serialize_put(sbx, o);
  lj_gc_check(L);
  L->top = L->base+1;  /* Chain buffer object. */
  return 1;
}

LJLIB_CF(buffer_method_decode)		LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobufw(L);
  setnilV(L->top++);
  lj_buf_rptr_rel(sbx, lj_serialize_get(sbx, L->top-1));
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(buffer_method___gc)
{
  SBufExt *sbx = buffer_tobuf(L);
  lj_bufx_free(L, sbx);
  return 0;
}

LJLIB_CF(buffer_method___tostring)	LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobuf(L);
  MSize len;
  const char *p = lj_bufx_data_acq(sbx, &len);
  setstrV(L, L->top-1, lj_str_new(L, p, len));
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(buffer_method___len)		LJLIB_REC(.)
{
  SBufExt *sbx = buffer_tobuf(L);
  setintV(L->top-1, (int32_t)sbufxlen(sbx));
  return 1;
}

LJLIB_PUSH("buffer") LJLIB_SET(__metatable)
LJLIB_PUSH(top-1) LJLIB_SET(__index)

/* -- Buffer library functions -------------------------------------------- */

#define LJLIB_MODULE_buffer

LJLIB_PUSH(top-2) LJLIB_SET(!)  /* Set environment. */

LJLIB_CF(buffer_new)
{
  MSize sz = 0;
  int targ = 1;
  GCtab *env, *dict_str = NULL, *dict_mt = NULL;
  GCudata *ud;
  SBufExt *sbx;
  if (L->base < L->top && !tvistab(L->base)) {
    targ = 2;
    if (!tvisnil(L->base))
      sz = (MSize)lj_lib_checkintrange(L, 1, 0, LJ_MAX_BUF);
  }
  if (L->base+targ-1 < L->top) {
    GCtab *options = lj_lib_checktab(L, targ);
    cTValue *opt_dict, *opt_mt;
    TValue optv;
    opt_dict = lj_tab_getstr(options, lj_str_newlit(L, "dict"));
    if (opt_dict) {
      lj_tv_load_acq(&optv, opt_dict);
      if (tvistab(&optv)) {
	dict_str = tabV(&optv);
	lj_serialize_dict_prep_str(L, dict_str);
      }
    }
    opt_mt = lj_tab_getstr(options, lj_str_newlit(L, "metatable"));
    if (opt_mt) {
      lj_tv_load_acq(&optv, opt_mt);
      if (tvistab(&optv)) {
	dict_mt = tabV(&optv);
	lj_serialize_dict_prep_mt(L, dict_mt);
      }
    }
  }
  env = lj_func_env_acq(curr_func(L));
  ud = lj_udata_new(L, sizeof(SBufExt), env);
  lj_udata_metatable_rel(ud, env);
  lj_gc_pubobjobj(L, ud, env);
  lj_gc2_finreg_udata_register_mt(L, G(L), ud, env);
  setudataV(L, L->top++, ud);
  sbx = (SBufExt *)uddata(ud);
  lj_bufx_init(L, sbx);
  setgcrefrel(sbx->dict_str, obj2gco(dict_str));
  if (dict_str)
    lj_gc_pubobjobj(L, ud, dict_str);
  setgcrefrel(sbx->dict_mt, obj2gco(dict_mt));
  if (dict_mt)
    lj_gc_pubobjobj(L, ud, dict_mt);
  lj_udata_udtype_rel(ud, UDTYPE_BUFFER);
  if (sz > 0) lj_buf_need2((SBuf *)sbx, sz);
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(buffer_encode)			LJLIB_REC(.)
{
  cTValue *o = lj_lib_checkany(L, 1);
  setstrV(L, L->top++, lj_serialize_encode(L, o));
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(buffer_decode)			LJLIB_REC(.)
{
  GCstr *str = lj_lib_checkstrx(L, 1);
  setnilV(L->top++);
  lj_serialize_decode(L, L->top-1, str);
  lj_gc_check(L);
  return 1;
}

/* ------------------------------------------------------------------------ */

#include "lj_libdef.h"

int luaopen_string_buffer(lua_State *L)
{
  LJ_LIB_REG(L, NULL, buffer_method);
  lua_getfield(L, -1, "__tostring");
  lua_setfield(L, -2, "tostring");
  LJ_LIB_REG(L, NULL, buffer);
  return 1;
}

#endif
