/*
** Process locale admission for Lua heaps.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_LOCALE_H
#define _LJ_LOCALE_H

#include "lj_obj.h"

#if !LJ_TARGET_PSVITA
LJ_FUNC void lj_locale_heap_register(global_State *g);
LJ_FUNC void lj_locale_heap_unregister(global_State *g);
LJ_FUNC void lj_locale_wait_attach(void);
#else
#define lj_locale_heap_register(g) UNUSED(g)
#define lj_locale_heap_unregister(g) UNUSED(g)
#define lj_locale_wait_attach() ((void)0)
#endif

#endif
