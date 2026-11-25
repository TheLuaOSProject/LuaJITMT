/*
** Focused regression test for lock-free callback set/free type snapshots.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_tg.h"

#include "lib/ctype_parse_fixture_helpers.h"
#include "lib/lua_fixture_helpers.h"

static uint32_t result_seq;

static int hold_parse_token_for_callback_result(lua_State *L)
{
  assert(result_seq == 0);
  result_seq = ljt_ctype_hold_parse_token(ctype_ctsG(G(L)));
  return 0;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  CTState *cts;
  uint32_t seq0, seq1;

  lua_pushcfunction(L, hold_parse_token_for_callback_result);
  lua_setglobal(L, "lj_m7_callback_snapshot_hold_result");

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef int (*lj_m7_callback_snapshot_t)(int);\n"
    "typedef enum { LJ_M7_CALLBACK_SNAPSHOT_ZERO = 0 } "
    "lj_m7_callback_snapshot_enum_t;\n"
    "typedef lj_m7_callback_snapshot_enum_t "
    "(*lj_m7_callback_snapshot_result_t)(int);\n"
    "]]\n"
    "lj_m7_callback_snapshot_cb = "
    "ffi.cast('lj_m7_callback_snapshot_t', function(x) return x + 1 end)\n"
    "lj_m7_callback_snapshot_free = "
    "ffi.cast('lj_m7_callback_snapshot_t', function(x) return x + 4 end)\n"
    "lj_m7_callback_snapshot_result = "
    "ffi.cast('lj_m7_callback_snapshot_result_t', function(x)\n"
    "  lj_m7_callback_snapshot_hold_result()\n"
    "  return x + 2\n"
    "end)\n");

  cts = ctype_ctsG(G(L));
  assert(cts != NULL);
  seq0 = ljt_ctype_parse_seq(cts);

  ljt_lua_dostring(L,
    "lj_m7_callback_snapshot_cb:set(function(x) return x + 2 end)\n"
    "assert(lj_m7_callback_snapshot_cb(40) == 42)\n"
    "assert(lj_m7_callback_snapshot_free(38) == 42)\n");
  seq1 = ljt_ctype_parse_seq(cts);
  assert(seq1 == seq0);

  ljt_ctype_read_committed(L, cts,
    "lj_m7_callback_snapshot_cb:set(function(x) return x + 3 end)\n"
    "assert(lj_m7_callback_snapshot_cb(39) == 42)\n");

  ljt_ctype_read_committed(L, cts,
    "assert(lj_m7_callback_snapshot_cb(39) == 42)\n");

  ljt_lua_dostring(L,
    "assert(lj_m7_callback_snapshot_result(40) == 42)\n");
  assert(result_seq && ctype_parse_token_acq(cts) == result_seq-1);
  ljt_ctype_release_parse_token(cts, result_seq);
  result_seq = 0;

  ljt_ctype_read_committed(L, cts,
    "lj_m7_callback_snapshot_free:free()\n"
    "assert(not pcall(lj_m7_callback_snapshot_free))\n"
    "lj_m7_callback_snapshot_free = nil\n");

  ljt_lua_dostring(L,
    "lj_m7_callback_snapshot_result:free()\n"
    "lj_m7_callback_snapshot_result = nil\n"
    "lj_m7_callback_snapshot_cb:free()\n"
    "lj_m7_callback_snapshot_cb = nil\n"
    "collectgarbage('collect')\n");

  lua_close(L);
  printf("t-ffi-callback-snapshot OK: callback install/runtime uses committed ctypes\n");
  return 0;
}
