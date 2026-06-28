/*
** Internal helpers for public API implementation/users.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_API_H
#define _LJ_API_H

#include "lj_obj.h"

LJ_FUNC int lj_api_equal(lua_State *L, int idx1, int idx2);
LJ_FUNC int lj_api_lessthan(lua_State *L, int idx1, int idx2);
LJ_FUNC size_t lj_api_objlen(lua_State *L, int idx);
LJ_FUNC int lj_api_cpcall(lua_State *L, lua_CFunction func, void *ud);

#endif
