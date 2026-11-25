/*
** Focused regression test for ccall small-struct stack overflow ctype snapshots.
*/

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_arch.h"
#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_tg.h"

#include "lib/ctype_parse_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "jit.off()\n"
    "ffi.cdef[[\n"
    "typedef struct { int x; } lj_m7_ccall_struct_overflow_t;\n"
    "int lj_m7_ccall_struct_overflow(int, int, int, int, int, int,\n"
    "                                lj_m7_ccall_struct_overflow_t);\n"
    "]]\n"
    "lj_m7_ccall_struct_lib = ffi.load(\n"
    "  assert(os.getenv('LJ_M7_FFI_CCALL_STRUCT_SO')))\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);

#if LJ_TARGET_X64 && !LJ_ABI_WIN
  ljt_ctype_read_committed(L, cts,
    "local s = { x = 37 }\n"
    "local r = lj_m7_ccall_struct_lib.lj_m7_ccall_struct_overflow(\n"
    "  1, 2, 3, 4, 5, 6, s)\n"
    "assert(r == 58)\n");
#endif

  lua_close(L);
  printf("t-ffi-ccall-struct-overflow OK: ccall struct overflow conversion uses committed types\n");
  return 0;
}
