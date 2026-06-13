/*
** Safepoint and soft-handshake scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_SAFEPOINT_H
#define _LJ_SAFEPOINT_H

#include "lj_obj.h"
#include "lj_gc2.h"

LJ_FUNCA uint32_t lj_safepoint_ack(lua_State *L);
LJ_FUNCA uint32_t lj_safepoint_ack_check(lua_State *L);
LJ_FUNCA uint32_t lj_safepoint_poll(lua_State *L);
LJ_FUNC void lj_safepoint_checkstop(lua_State *L, uint32_t actions);
LJ_FUNC uint32_t lj_safepoint_handshake(global_State *g, uint32_t actions);
LJ_FUNC void lj_safepoint_apply_tg(global_State *g, TGState *tg,
				   uint32_t actions);
LJ_FUNC void lj_native_enter(TGState *tg);
LJ_FUNC uint32_t lj_native_leave(lua_State *L);

#endif
