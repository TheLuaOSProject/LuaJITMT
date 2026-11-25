/* Construct a committed struct while an unrelated IDLE metadata collector
** is parked. Subsequent metatype registration still applies to that ctype. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lib/lua_fixture_helpers.h"

static uint32_t stage;

uint32_t __real_lj_str_reclaim_retired(global_State *g, uint64_t epoch);
uint32_t __wrap_lj_str_reclaim_retired(global_State *g, uint64_t epoch)
{
  uint32_t expected = 1;
  if (la_cas32(&stage, &expected, 2, LA_ACQ_REL, LA_ACQ)) {
    assert(lj_gc2_reclaim_context_held(g));
    while (la_load32_acq(&stage) == 2)
      sched_yield();
  }
  return __real_lj_str_reclaim_retired(g, epoch);
}

static void *reclaim(void *arg)
{
  global_State *g = (global_State *)arg;
  (void)lj_gc2_reclaim_retired(g, lj_gc2_retire_epoch(g) + 1u);
  return NULL;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g = G(L);
  pthread_t worker;
  ljt_lua_dostring(L,
    "local ffi = require('ffi')\n"
    "local ct = ffi.typeof('struct { int x; }')\n"
    "local new, size, is, meta = ffi.new, ffi.sizeof, ffi.istype, ffi.metatype\n"
    "return function()\n"
    "  local a, b = new(ct, 23), new(ct, 19)\n"
    "  return is(ct, a) and is(ct, b), size(a), a, b\n"
    "end, function(a, b)\n"
    "  meta(ct, {__add = function(a, b) return a.x - b.x end})\n"
    "  return a + b, new(ct, 23) + ct(19)\n"
    "end\n");
  assert(lua_gettop(L) == 2);
  assert(lua_gc(L, LUA_GCCOLLECT, 0) == 0);
  lua_gc(L, LUA_GCSTOP, 0);
  la_store32_rel(&stage, 1);
  assert(pthread_create(&worker, NULL, reclaim, g) == 0);
  while (la_load32_acq(&stage) == 1)
    sched_yield();
  assert(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_META_EXCLUSIVE);
  lua_pushvalue(L, 1);
  ljt_lua_pcall(L, 0, 4, "construct while IDLE reclaimer is parked");
  assert(lua_toboolean(L, 3) && lua_tointeger(L, 4) == 4);
  assert(gc2_smr_reclaiming_acq(g) == LJ_GC2_SMR_META_EXCLUSIVE);
  la_store32_rel(&stage, 3);
  assert(pthread_join(worker, NULL) == 0);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 5);
  lua_pushvalue(L, 6);
  ljt_lua_pcall(L, 2, 2, "register metatype after construction");
  assert(lua_tointeger(L, -2) == 4 && lua_tointeger(L, -1) == 4);
  lua_close(L);
  puts("t-ffi-new-idle-reclaim OK");
  return 0;
}
