/*
** Safepoint and soft-handshake scaffold.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_SAFEPOINT_H
#define _LJ_SAFEPOINT_H

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_tg.h"

LJ_FUNCA uint32_t lj_safepoint_ack(lua_State *L);
LJ_FUNCA uint32_t lj_safepoint_ack_check(lua_State *L);
LJ_FUNCA uint32_t lj_safepoint_poll(lua_State *L);
LJ_FUNCA void lj_safepoint_checkstop(lua_State *L, uint32_t actions);
LJ_FUNC uint32_t lj_safepoint_handshake(global_State *g, uint32_t actions);
LJ_FUNC void lj_safepoint_apply_tg(global_State *g, TGState *tg,
				   uint32_t actions);
LJ_FUNC uint32_t lj_safepoint_retire_dead_tg(global_State *g, TGState *tg);
LJ_FUNCA void lj_native_enter(TGState *tg);
LJ_FUNCA uint32_t lj_native_leave(lua_State *L);

static LJ_AINLINE int lj_safepoint_fresh_stopreq(lua_State *L,
						 uint32_t actions,
						 int had_stopreq)
{
  TGState *tg = L ? L2TG(L) : NULL;
  return (actions & LJ_GC2_HS_STOPREQ) ||
    (tg && lj_tg_flags_all_acq(tg, TGF_STOPREQ|TGF_STOPREQ_FRESH)) ||
    (!had_stopreq && tg && lj_tg_flags_test_acq(tg, TGF_STOPREQ));
}

#endif
