/*
** Focused guard for nested FFI callback native-state restoration.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_tg.h"

typedef void (*NestedCallback)(void);

static TGState *test_tg;
static int outer_checked;
static int outer_saw_native;

static void inner_call(NestedCallback cb)
{
  cb();
}

static void outer_call(NestedCallback cb)
{
  cb();
  outer_checked++;
  outer_saw_native = test_tg != NULL && test_tg->in_native != 0;
}

static void dostring(lua_State *L, const char *src)
{
  if (luaL_dostring(L, src) != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    fprintf(stderr, "lua error: %s\n", err ? err : "(non-string)");
    assert(0);
  }
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

  dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "typedef void (*lj_m7_nested_cb_t)(void);\n"
    "typedef int (*lj_m7_int_cb_t)(void);\n"
    "typedef void (*lj_m7_call_cb_t)(lj_m7_nested_cb_t);\n"
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
  assert(test_tg->cb.depth == 0);

  dostring(L,
    "local ffi = require('ffi')\n"
    "local bad_body = ffi.cast('lj_m7_nested_cb_t', function()\n"
    "  error('nested callback body error')\n"
    "end)\n"
    "local ok = pcall(function() bad_body() end)\n"
    "bad_body:free()\n"
    "assert(not ok)\n");

  assert(test_tg->cb.depth == 0);
  assert(test_tg->in_native == 0);

  dostring(L,
    "local ffi = require('ffi')\n"
    "local inner = ffi.cast('lj_m7_call_cb_t', lj_m7_inner_call)\n"
    "local dead = ffi.cast('lj_m7_nested_cb_t', function() end)\n"
    "dead:free()\n"
    "local ok = pcall(function() inner(dead) end)\n"
    "assert(not ok)\n");

  assert(test_tg->cb.depth == 0);
  assert(test_tg->in_native == 0);

  dostring(L,
    "local ffi = require('ffi')\n"
    "local bad_result = ffi.cast('lj_m7_int_cb_t', function()\n"
    "  return {}\n"
    "end)\n"
    "local ok = pcall(function() return bad_result() end)\n"
    "bad_result:free()\n"
    "assert(not ok)\n");

  assert(test_tg->cb.depth == 0);
  assert(test_tg->in_native == 0);

  lua_close(L);
  printf("t-ffi-callback-nested-native OK: nested callbacks restore native state\n");
  return 0;
}
