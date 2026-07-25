/*
** Fast function IDs.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_FF_H
#define _LJ_FF_H

/* Fast function ID. */
typedef enum {
  FF_LUA_ = FF_LUA,	/* Lua function (must be 0). */
  FF_C_ = FF_C,		/* Regular C function (must be 1). */
#define FFDEF(name)	FF_##name,
#include "lj_ffdef.h"
  FF__MAX
} FastFunc;

/*
** GCfunc.ffid is a uint8_t, so a library that pushes the total past 256 IDs
** silently truncates the IDs of the last few functions. Those then collide
** with FF_LUA or FF_C, which makes isluafunc()/iscfunc() lie about them and
** makes the trace recorder pick the wrong handler. Fail the build instead.
*/
LJ_STATIC_ASSERT(FF__MAX <= 256);

#endif
