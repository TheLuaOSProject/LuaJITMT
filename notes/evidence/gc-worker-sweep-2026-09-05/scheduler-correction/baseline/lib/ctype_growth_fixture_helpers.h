/*
** Shared helpers for C fixtures that need CTState table growth.
*/

#ifndef TESTS_LIB_CTYPE_GROWTH_FIXTURE_HELPERS_H
#define TESTS_LIB_CTYPE_GROWTH_FIXTURE_HELPERS_H

#include "lua.h"

#include "lj_ctype.h"

#include "lua_fixture_helpers.h"

static inline void ljt_ctype_force_table_growth(lua_State *L, CTState *cts,
						const char *prefix)
{
  MSize count = ctype_tab_sizetab_acq(ctype_tabh_acq(cts)) + 64u;
  lua_pushinteger(L, (lua_Integer)count);
  lua_setglobal(L, "lj_m7_ctype_growth_count");
  lua_pushstring(L, prefix);
  lua_setglobal(L, "lj_m7_ctype_growth_prefix");
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local count = assert(lj_m7_ctype_growth_count)\n"
    "local prefix = assert(lj_m7_ctype_growth_prefix)\n"
    "local parts = {}\n"
    "for i = 1, count do\n"
    "  parts[#parts + 1] =\n"
    "    ('typedef struct { int x; } %s_%d_t;\\n'):format(prefix, i)\n"
    "end\n"
    "ffi.cdef(table.concat(parts))\n"
    "lj_m7_ctype_growth_count = nil\n"
    "lj_m7_ctype_growth_prefix = nil\n");
}

#endif
