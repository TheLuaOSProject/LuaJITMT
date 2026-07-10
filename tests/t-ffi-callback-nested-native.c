/*
** Focused regression test for nested FFI callback native-state restoration.
*/

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#if LJ_TARGET_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include "lj_ctype.h"
#include "lj_tg.h"

#include "lib/lua_fixture_helpers.h"

typedef void (*NestedCallback)(void);
typedef int (*IntCallback)(void);

#define CALLBACK_ENTRY_WINERR UINT32_C(0x5a31)
#define CALLBACK_BODY_WINERR UINT32_C(0x6b42)

static TGState *test_tg;
static int outer_checked;
static int outer_saw_native;
static int outer_saw_errno;
static int inner_saw_errno;

static void assert_callback_frames_empty(TGState *tg)
{
  MSize i;
  assert(ccallback_depth_acq(&tg->cb) == 0);
  assert(ccallback_slot_acq(&tg->cb) == 0);
  assert(ccallback_native_had_stopreq_acq(&tg->cb) == 0);
  assert(ccallback_auto_detach_acq(&tg->cb) == 0);
  assert(lj_tg_ffi_call_func_acq(tg) == NULL);
  for (i = 0; i < CCALLBACK_MAX_NEST; i++) {
    assert(tg->cb.frame[i].L == NULL);
    assert(tg->cb.frame[i].cont == 0);
    assert(tg->cb.frame[i].native_depth == 0);
    assert(tg->cb.frame[i].auto_detach == 0);
    assert(tg->cb.frame[i].errphase == CCALLBACK_ERR_SETUP);
    assert(tg->cb.frame[i].errnum == 0);
    assert(tg->cb.frame[i].winerr == 0);
  }
}

static void set_error_pair(int32_t errnum, uint32_t winerr)
{
  errno = errnum;
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)winerr);
#else
  (void)winerr;
#endif
}

static uint32_t get_winerr(void)
{
#if LJ_TARGET_WINDOWS
  return (uint32_t)GetLastError();
#else
  return 0;
#endif
}

static void inner_call(NestedCallback cb)
{
  errno = ENOENT;
  cb();
  inner_saw_errno = errno;
}

static void error_call(NestedCallback cb)
{
  set_error_pair(EBUSY, CALLBACK_ENTRY_WINERR);
  cb();
}

static void dead_call(NestedCallback cb)
{
  cb();
}

static int int_call(IntCallback cb)
{
  errno = EAGAIN;
  return cb();
}

static void outer_call(NestedCallback cb)
{
  errno = EAGAIN;
  cb();
  outer_checked++;
  outer_saw_native = test_tg != NULL && lj_tg_in_native_acq(test_tg) != 0;
  outer_saw_errno = errno;
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
  lua_pushlightuserdata(L, (void *)set_error_pair);
  lua_setglobal(L, "lj_m7_set_error_pair");
  lua_pushlightuserdata(L, (void *)get_winerr);
  lua_setglobal(L, "lj_m7_get_winerr");
  lua_pushinteger(L, EAGAIN);
  lua_setglobal(L, "lj_m7_eagain");
  lua_pushinteger(L, EBUSY);
  lua_setglobal(L, "lj_m7_ebusy");
  lua_pushinteger(L, ENOENT);
  lua_setglobal(L, "lj_m7_enoent");
  lua_pushinteger(L, EDOM);
  lua_setglobal(L, "lj_m7_edom");
  lua_pushinteger(L, ERANGE);
  lua_setglobal(L, "lj_m7_erange");
  lua_pushinteger(L, (lua_Integer)(LJ_TARGET_WINDOWS ?
				 CALLBACK_ENTRY_WINERR : 0));
  lua_setglobal(L, "lj_m7_callback_entry_winerr");
  lua_pushinteger(L, (lua_Integer)(LJ_TARGET_WINDOWS ?
				 CALLBACK_BODY_WINERR : 0));
  lua_setglobal(L, "lj_m7_callback_body_winerr");

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
    "  assert(ffi.errno() == lj_m7_enoent, ffi.errno())\n"
    "  ffi.errno(lj_m7_erange)\n"
    "  seen = seen + 1\n"
    "end)\n"
    "local cb1 = ffi.cast('lj_m7_nested_cb_t', function()\n"
    "  assert(ffi.errno() == lj_m7_eagain, ffi.errno())\n"
    "  inner(cb2)\n"
    "  assert(ffi.errno() == lj_m7_erange, ffi.errno())\n"
    "  ffi.errno(lj_m7_edom)\n"
    "end)\n"
    "outer(cb1)\n"
    "assert(ffi.errno() == lj_m7_edom, ffi.errno())\n"
    "cb1:free()\n"
    "cb2:free()\n"
    "assert(seen == 1, seen)\n");

  assert(outer_checked == 1);
  assert(outer_saw_native == 1);
  assert(inner_saw_errno == ERANGE);
  assert(outer_saw_errno == EDOM);
  assert_callback_frames_empty(test_tg);

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local call_bad = ffi.cast('lj_m7_call_cb_t', lj_m7_error_call)\n"
    "local bad_body = ffi.cast('lj_m7_nested_cb_t', function()\n"
    "  error('nested callback body error')\n"
    "end)\n"
    "local ok = pcall(function() call_bad(bad_body) end)\n"
    "bad_body:free()\n"
    "assert(not ok)\n"
    "assert(ffi.errno() == lj_m7_ebusy, ffi.errno())\n"
    "local get_winerr = ffi.cast('uint32_t (*)(void)', lj_m7_get_winerr)\n"
    "assert(get_winerr() == lj_m7_callback_entry_winerr)\n");

  assert_callback_frames_empty(test_tg);
  assert(lj_tg_in_native_acq(test_tg) == 0);

  /* A body-owned error keeps explicit errno/LastError writes made by Lua.
  ** Setup and result-conversion errors still use their phase snapshots. */
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[typedef void (*lj_m7_set_error_pair_t)(int32_t, uint32_t);]]\n"
    "local call_bad = ffi.cast('lj_m7_call_cb_t', lj_m7_error_call)\n"
    "local set_error = ffi.cast('lj_m7_set_error_pair_t',\n"
    "                             lj_m7_set_error_pair)\n"
    "local get_winerr = ffi.cast('uint32_t (*)(void)', lj_m7_get_winerr)\n"
    "local bad_body = ffi.cast('lj_m7_nested_cb_t', function()\n"
    "  set_error(lj_m7_erange, lj_m7_callback_body_winerr)\n"
    "  error('callback body error after explicit OS-error write')\n"
    "end)\n"
    "local ok = pcall(function() call_bad(bad_body) end)\n"
    "bad_body:free()\n"
    "assert(not ok)\n"
    "assert(ffi.errno() == lj_m7_erange, ffi.errno())\n"
    "assert(get_winerr() == lj_m7_callback_body_winerr)\n");

  assert_callback_frames_empty(test_tg);
  assert(lj_tg_in_native_acq(test_tg) == 0);
  assert(lj_ctype_cb_isblacklisted(ctype_cts(L), (void *)error_call));

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local call_dead = ffi.cast('lj_m7_call_cb_t', lj_m7_dead_call)\n"
    "local dead = ffi.cast('lj_m7_nested_cb_t', function() end)\n"
    "dead:free()\n"
    "local ok = pcall(function() call_dead(dead) end)\n"
    "assert(not ok)\n");

  assert_callback_frames_empty(test_tg);
  assert(lj_tg_in_native_acq(test_tg) == 0);
  assert(lj_ctype_cb_isblacklisted(ctype_cts(L), (void *)dead_call));

  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local call_bad = ffi.cast('lj_m7_call_int_cb_t', lj_m7_int_call)\n"
    "local bad_result = ffi.cast('lj_m7_int_cb_t', function()\n"
    "  ffi.errno(lj_m7_erange)\n"
    "  return {}\n"
    "end)\n"
    "local ok = pcall(function() return call_bad(bad_result) end)\n"
    "bad_result:free()\n"
    "assert(not ok)\n"
    "assert(ffi.errno() == lj_m7_erange, ffi.errno())\n");

  assert_callback_frames_empty(test_tg);
  assert(lj_tg_in_native_acq(test_tg) == 0);
  assert(lj_ctype_cb_isblacklisted(ctype_cts(L), (void *)int_call));

  lua_close(L);
  printf("t-ffi-callback-nested-native OK: nested callbacks restore native/error state\n");
  return 0;
}
