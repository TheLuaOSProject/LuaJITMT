/*
** M8 finalizer callback state ownership coverage.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_state.h"
#include "lib/lua_fixture_helpers.h"

enum { MAIN_UDATA, MAIN_CDATA, CO_UDATA, ERROR_CDATA, NUM_CASES };
static lua_State *expected_L;
static int expected_slot;
static int finalized[NUM_CASES], error_events;
static ptrdiff_t argument_slot, error_slot;
static const void *error_object;

static int state_finalizer(lua_State *L)
{
  assert(L == expected_L);
  assert(lua_gettop(L) == 1);
  argument_slot = savestack(L, L->base);
  assert(lua_checkstack(L, 2048));
  finalized[expected_slot]++;
  if (expected_slot == ERROR_CDATA) {
    error_object = lua_topointer(L, 1);
    lua_pushvalue(L, 1);
    return lua_error(L);
  }
  return 0;
}

static int error_finalizer_event(lua_State *L)
{
  assert(L == expected_L && lua_gettop(L) == 1);
  assert(lua_topointer(L, 1) == error_object);
  error_slot = savestack(L, L->base);
  error_events++;
  /* An event handler may keep the finalized object alive. */
  lua_pushvalue(L, 1);
  lua_setfield(L, LUA_REGISTRYINDEX, "resurrected");
  return 0;
}

static void check_released_frame(lua_State *L, ptrdiff_t argument)
{
  TValue *arg = restorestack(L, argument);
  TValue *slot = arg - (1+LJ_FR2);
  assert(slot >= L->top && arg < tvref(L->maxstack));
  for (; slot <= arg; slot++)
    assert(tvisnil(slot));
}

static void drive_full_gc(lua_State *L)
{
  int i;
  lua_pushinteger(L, 173);
  for (i = 0; i < 4 && !finalized[expected_slot]; i++)
    lua_gc(L, LUA_GCCOLLECT, 0);
  assert(finalized[expected_slot] == 1);
  /* Read before another API call can reuse the returned frame. A parked
  ** native root scan may see both the original call and the ERRFIN call. */
  check_released_frame(L, argument_slot);
  if (expected_slot == ERROR_CDATA) {
    assert(error_events == 1);
    check_released_frame(L, error_slot);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_getfield(L, LUA_REGISTRYINDEX, "resurrected");
    assert(lua_topointer(L, -1) == error_object);
    lua_pop(L, 1);
  }
  assert(lua_gettop(L) == 1 && lua_tointeger(L, 1) == 173);
  lua_pop(L, 1);
  for (i = 0; i < 3; i++)
    lua_gc(L, LUA_GCCOLLECT, 0);
  assert(finalized[expected_slot] == 1);
}

static void push_state_udata(lua_State *L)
{
  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, state_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
}

static void check_udata_on_state(lua_State *L, int slot)
{
  expected_L = L;
  expected_slot = slot;
  lua_gc(L, LUA_GCSTOP, 0);
  push_state_udata(L);
  drive_full_gc(L);
}

static void check_cdata_on_state(lua_State *L, int slot)
{
  static const char script[] =
    "local ffi = require('ffi')\n"
    "ffi.gc(ffi.new('int32_t[1]', 29), m8_state_finalizer)\n";
  expected_L = L;
  expected_slot = slot;
  lua_gc(L, LUA_GCSTOP, 0);
  lua_pushcfunction(L, state_finalizer);
  lua_setglobal(L, "m8_state_finalizer");
  ljt_lua_dostring(L, script);
  drive_full_gc(L);
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  lua_State *co;

  check_udata_on_state(L, MAIN_UDATA);
  check_cdata_on_state(L, MAIN_CDATA);
  lua_register(L, "m8_error_event", error_finalizer_event);
  ljt_lua_dostring(L, "jit.attach(m8_error_event, 'errfin')");
  check_cdata_on_state(L, ERROR_CDATA);

  co = lua_newthread(L);
  assert(co != NULL);
  check_udata_on_state(co, CO_UDATA);
  lua_pop(L, 1);

  lua_close(L);
  assert(error_events == 1 && finalized[ERROR_CDATA] == 1);
  puts("t-m8-finalizer-state OK: caller state, released frames and resurrection");
  return 0;
}
