/*
** Focused guard for nested FFI callback native-state restoration.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_ctype.h"
#include "lj_tg.h"

#include "lib/lua_fixture_helpers.h"

typedef void (*NestedCallback)(void);
typedef int (*IntCallback)(void);

static TGState *test_tg;
static int outer_checked;
static int outer_saw_native;

static void inner_call(NestedCallback cb)
{
  cb();
}

static void error_call(NestedCallback cb)
{
  cb();
}

static void dead_call(NestedCallback cb)
{
  cb();
}

static int int_call(IntCallback cb)
{
  return cb();
}

static void outer_call(NestedCallback cb)
{
  cb();
  outer_checked++;
  outer_saw_native = test_tg != NULL && lj_tg_in_native_acq(test_tg) != 0;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  test_tg = L2TG(L);
  assert(test_tg != NULL);

  lua_pushlightuserdata(L, (void *)outer_call);
  lua_setglobal(L, "lj_m7_outer_call");
  lua_pushlightuserdata(L, (void *)inner_call);
  lua_setglobal(L, "lj_m7_inner_call");
  lua_pushlightuserdata(L, (void *)error_call);
  lua_setglobal(L, "lj_m7_error_call");
  lua_pushlightuserdata(L, (void *)dead_call);
  lua_setglobal(L, "lj_m7_dead_call");
  lua_pushlightuserdata(L, (void *)int_call);
  lua_setglobal(L, "lj_m7_int_call");

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef void (*lj_m7_nested_cb_t)(void);\n"
    "typedef int (*lj_m7_int_cb_t)(void);\n"
    "typedef void (*lj_m7_call_cb_t)(lj_m7_nested_cb_t);\n"
    "typedef int (*lj_m7_call_int_cb_t)(lj_m7_int_cb_t);\n"
    "]]\n"
    "local outer = ffi.cast('lj_m7_call_cb_t', lj_m7_outer_call)\n"
    "local inner = ffi.cast('lj_m7_call_cb_t', lj_m7_inner_call)\n"
    "local seen = 0\n"
    "local cb2 = ffi.cast('lj_m7_nested_cb_t', function()\n"
    "  seen = seen + 1\n"
    "end)\n"
    "local cb1 = ffi.cast('lj_m7_nested_cb_t', function()\n"
    "  inner(cb2)\n"
    "end)\n"
    "outer(cb1)\n"
    "cb1:free()\n"
    "cb2:free()\n"
    "assert(seen == 1, seen)\n");

  assert(outer_checked == 1);
  assert(outer_saw_native == 1);
  assert(ccallback_depth_acq(&test_tg->cb) == 0);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local call_bad = ffi.cast('lj_m7_call_cb_t', lj_m7_error_call)\n"
    "local bad_body = ffi.cast('lj_m7_nested_cb_t', function()\n"
    "  error('nested callback body error')\n"
    "end)\n"
    "local ok = pcall(function() call_bad(bad_body) end)\n"
    "bad_body:free()\n"
    "assert(not ok)\n");

  assert(ccallback_depth_acq(&test_tg->cb) == 0);
  assert(lj_tg_in_native_acq(test_tg) == 0);
  assert(lj_ctype_cb_isblacklisted(ctype_cts(L), (void *)error_call));

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local call_dead = ffi.cast('lj_m7_call_cb_t', lj_m7_dead_call)\n"
    "local dead = ffi.cast('lj_m7_nested_cb_t', function() end)\n"
    "dead:free()\n"
    "local ok = pcall(function() call_dead(dead) end)\n"
    "assert(not ok)\n");

  assert(ccallback_depth_acq(&test_tg->cb) == 0);
  assert(lj_tg_in_native_acq(test_tg) == 0);
  assert(lj_ctype_cb_isblacklisted(ctype_cts(L), (void *)dead_call));

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local call_bad = ffi.cast('lj_m7_call_int_cb_t', lj_m7_int_call)\n"
    "local bad_result = ffi.cast('lj_m7_int_cb_t', function()\n"
    "  return {}\n"
    "end)\n"
    "local ok = pcall(function() return call_bad(bad_result) end)\n"
    "bad_result:free()\n"
    "assert(not ok)\n");

  assert(ccallback_depth_acq(&test_tg->cb) == 0);
  assert(lj_tg_in_native_acq(test_tg) == 0);
  assert(lj_ctype_cb_isblacklisted(ctype_cts(L), (void *)int_call));

  lua_close(L);
  printf("t-ffi-callback-nested-native OK: nested callbacks restore native state\n");
  return 0;
}
