/*
** Userdata handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_UDATA_H
#define _LJ_UDATA_H

#include "lj_obj.h"

/*
** A constructor root closes the gap between a userdata becoming READY and its
** first ordinary Lua/native semantic root.  The slot is reserved before the
** allocation, so installing the pending identity cannot allocate or throw.
*/
typedef struct LJUdataRoot {
  TGState *tg;
  uint32_t idx;
} LJUdataRoot;

LJ_FUNC GCudata *lj_udata_new(lua_State *L, MSize sz, GCtab *env);
LJ_FUNC GCudata *lj_udata_newrooted(lua_State *L, MSize sz, GCtab *env,
				    LJUdataRoot *root);
LJ_FUNC GCudata *lj_udata_newrooted_envrooted(lua_State *L, MSize sz,
					      GCtab *env,
					      LJUdataRoot *root);
LJ_FUNC void lj_udata_root_release(LJUdataRoot *root);
LJ_FUNC void lj_udata_pushrooted(lua_State *L, GCudata *ud,
				 LJUdataRoot *root);
LJ_FUNC void lj_udata_finreg_mt_rooted(lua_State *L, GCudata *ud, GCtab *mt,
				       LJUdataRoot *root);
LJ_FUNC void lj_udata_specialize(lua_State *L, GCudata *ud, uint8_t udtype);
LJ_FUNC void lj_udata_rescan(lua_State *L, GCudata *ud);
LJ_FUNC void LJ_FASTCALL lj_udata_free(global_State *g, GCudata *ud);
#if defined(LJ_UDATA_TEST_HELPERS)
LJ_FUNC void lj_udata_test_fail_finreg_after(uint32_t nth);
#endif
#if LJ_64
LJ_FUNC void * LJ_FASTCALL lj_lightud_intern(lua_State *L, void *p);
#endif

#endif
