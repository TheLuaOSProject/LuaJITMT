/*
** Function handling (prototypes, functions and upvalues).
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_FUNC_H
#define _LJ_FUNC_H

#include "lj_obj.h"

/* Prototypes. */
LJ_FUNC void LJ_FASTCALL lj_func_freeproto(global_State *g, GCproto *pt);

/* Upvalues. */
LJ_FUNCA GCupval *LJ_FASTCALL lj_func_newuvcell(lua_State *L);
LJ_FUNCA void LJ_FASTCALL lj_func_closeuv(lua_State *L, TValue *level);
LJ_FUNC void LJ_FASTCALL lj_func_freeuv(global_State *g, GCupval *uv);

/* Functions (closures). */
LJ_FUNC GCfunc *lj_func_newC(lua_State *L, MSize nelems, GCtab *env,
			      uint32_t *anchoridx);
/* Adopt a caller-published top anchor containing env, then replace that exact
** slot with the complete closure. OOM pops the adopted slot before raising. */
LJ_FUNC GCfunc *lj_func_newC_envrooted(lua_State *L, MSize nelems,
				       GCtab *env, uint32_t anchoridx);
LJ_FUNC GCfunc *lj_func_newL_empty(lua_State *L, GCproto *pt, GCtab *env,
				   uint32_t anchoridx);
LJ_FUNCA GCfunc *lj_func_newL_gc(lua_State *L, GCproto *pt, GCfuncL *parent);
LJ_FUNCA void lj_func_syncslot_forjit(lua_State *L, TValue *base,
				      int32_t slot, const TValue *tv);
LJ_FUNCA void lj_func_storeuv_pub(lua_State *L, TValue *tv,
				  const TValue *src);
LJ_FUNCA void lj_func_storeuvstr_pub(lua_State *L, TValue *tv, GCstr *str);
LJ_FUNCA void lj_func_storeuvnum_pub(lua_State *L, TValue *tv,
				     const lua_Number *np);
LJ_FUNCA void lj_func_storeuvpri_pub(lua_State *L, TValue *tv,
				     uint32_t pri);
LJ_FUNCA void lj_func_storeuv_forjit(lua_State *L, TValue *tv,
				     const TValue *src);
LJ_FUNCA GCupval *lj_func_promoteuv_forjit(lua_State *L, TValue *base,
					   int32_t slot, const TValue *tv);
LJ_FUNCA GCupval *lj_func_newuvcell_forjit(lua_State *L, TValue *base,
					   int32_t slot);
LJ_FUNCA GCfunc *lj_func_newL_gc_forjit(lua_State *L, TValue *base,
					GCproto *pt, GCfuncL *parent);
LJ_FUNCA GCfunc *lj_func_newL_gc1num_forjit(lua_State *L, TValue *base,
					    GCproto *pt, GCfuncL *parent,
					    int32_t slot, lua_Number n);
#ifdef LJ_FUNC_TEST_HELPERS
LJ_FUNC uint32_t lj_func_test_gc1num_bump_fast_calls(void);
LJ_FUNC void lj_func_test_reset_gc1num_bump_fast_calls(void);
LJ_FUNC uint32_t lj_func_test_gc1num_bump_fallback_calls(void);
LJ_FUNC void lj_func_test_reset_gc1num_bump_fallback_calls(void);
LJ_FUNC uint32_t lj_func_test_gc1num_bump_interp_calls(void);
LJ_FUNC void lj_func_test_reset_gc1num_bump_interp_calls(void);
LJ_FUNC uint32_t lj_func_test_gc1uv_chain_calls(void);
LJ_FUNC void lj_func_test_reset_gc1uv_chain_calls(void);
LJ_FUNC uint32_t lj_func_test_uv_afterfn_calls(void);
LJ_FUNC void lj_func_test_reset_uv_afterfn_calls(void);
LJ_FUNC uint32_t lj_func_test_gc0_bump_interp_calls(void);
LJ_FUNC void lj_func_test_reset_gc0_bump_interp_calls(void);
LJ_FUNC uint32_t lj_func_test_gc0_bump_trace_calls(void);
LJ_FUNC void lj_func_test_reset_gc0_bump_trace_calls(void);
LJ_FUNC uint32_t lj_func_test_uvcell_bump_calls(void);
LJ_FUNC void lj_func_test_reset_uvcell_bump_calls(void);
LJ_FUNC void lj_func_test_fail_empty_uv_after(uint32_t nth);
LJ_FUNC uint32_t lj_func_test_empty_uv_fail_remaining(void);
LJ_FUNC void lj_func_test_fail_finduv_after(uint32_t nth);
LJ_FUNC uint32_t lj_func_test_finduv_fail_remaining(void);
LJ_FUNC void lj_func_test_collect_after_finduv(uint32_t nth);
#endif
LJ_FUNC void LJ_FASTCALL lj_func_free(global_State *g, GCfunc *c);

#endif
