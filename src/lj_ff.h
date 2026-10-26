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

/* Pure scalar bit-library recorders. Their cdata/string variants still need
** separate IR capabilities; recognizing the builtin alone admits no effect. */
static LJ_AINLINE int lj_ff_is_bit_scalar(uint32_t ffid)
{
  switch (ffid) {
  case FF_bit_tobit: case FF_bit_bnot: case FF_bit_bswap:
  case FF_bit_band: case FF_bit_bor: case FF_bit_bxor:
  case FF_bit_lshift: case FF_bit_rshift: case FF_bit_arshift:
  case FF_bit_rol: case FF_bit_ror:
    return 1;
  default:
    return 0;
  }
}

#endif
