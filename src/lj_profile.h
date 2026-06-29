/*
** Low-overhead profiling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_PROFILE_H
#define _LJ_PROFILE_H

#include "lj_obj.h"

#if LJ_HASPROFILE

#define LJ_PROFILE_TGLOCAL	(LJ_PROFILE_SIGPROF && LJ_TARGET_X86ORX64)

LJ_FUNC void LJ_FASTCALL lj_profile_interpreter(lua_State *L);
LJ_FUNC int lj_profile_pending(lua_State *L);
LJ_FUNC int lj_profile_active(lua_State *L);
LJ_FUNC uint32_t lj_profile_stop_hs(lua_State *L);
#if !LJ_PROFILE_SIGPROF
LJ_FUNC void LJ_FASTCALL lj_profile_hook_enter(global_State *g);
LJ_FUNC void LJ_FASTCALL lj_profile_hook_leave(global_State *g);
#endif

#else

#define LJ_PROFILE_TGLOCAL	0

#endif

#endif
