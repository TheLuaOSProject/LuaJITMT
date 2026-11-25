/*
** A native side exit publishes restored closure roots during active SWEEP.
*/
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lj_snap.h"
#include "lj_trace.h"

static unsigned callback_entries, fresh_entries, snapshots;
static GCfunc *callback, *fresh;

static void call(lua_State *L, int args, int results)
{
  int status = lua_pcall(L, args, results, 0);
  if (status)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == LUA_OK);
}

static unsigned queued(TGState *tg, GCobj *target)
{
  GCRef *entry = lj_tg_ssb_base_acq(tg);
  GCRef *end = lj_tg_ssb_next_acq(tg);
  unsigned count = 0;
  for (; entry != end; entry++)
    count += gcref_acq(*entry) == target;
  return count;
}

static void observe_roots(lua_State *L, int published)
{
  TGState *tg = L2TG(L);
  global_State *g = G(L);
  assert(lj_trace_test_exit_calls() != 0);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  assert(tvisfunc(L->base + 1) && funcV(L->base + 1) == callback);
  assert(tvisfunc(L->base + 2) && funcV(L->base + 2) == fresh);
  if (published) {
    assert(queued(tg, obj2gco(callback)) == callback_entries);
    assert(queued(tg, obj2gco(fresh)) > fresh_entries);
    snapshots++;
  } else {
    callback_entries = queued(tg, obj2gco(callback));
    fresh_entries = queued(tg, obj2gco(fresh));
  }
}

int main(void)
{
  static const char script[] =
    "jit.opt.start('hotloop=1', 'hotexit=100000')\n"
    "local function run(marker, callback, fresh)\n"
    "  local sum = 0\n"
    "  for i=1,64 do\n"
    "    if i == marker then return callback(sum), fresh end\n"
    "    sum = sum + i\n"
    "  end\n"
    "  return sum\n"
    "end\n"
    "return run, function(x) return x + 7 end,\n"
    "       function(x) return function() return x.bytes end end\n";
  lua_State *L = luaL_newstate();
  global_State *g;
  unsigned i;

  assert(L);
  g = G(L);
  luaL_openlibs(L);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(luaL_loadstring(L, script) == LUA_OK);
  call(L, 0, 3);
  assert(lua_gettop(L) == 3);
  callback = funcV(L->top - 2);

  lj_trace_test_reset_exit_stats();
  for (i = 0; i < 20; i++) {
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 2);
    call(L, 3, 1);
    assert(lua_tointeger(L, -1) == 2080);
    lua_pop(L, 1);
  }
  assert(lj_trace_test_exit_calls() != 0);

  for (i = 0; i < 40000; i++) {
    lua_newtable(L);
    lua_pop(L, 1);
  }
  for (i = 0; i < 10000; i++) {
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP &&
        gc2_sweep_bridge_ready_acq(g) && lj_gc2_jit_entry_open(g))
      break;
    lua_gc(L, LUA_GCSTEP, 1);
  }
  assert(i < 10000);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(la_load32_acq(&callback->l.gc2_scan_cycle) == gc2_cycle_acq(g));

  lua_pushvalue(L, 3);
  lua_newtable(L);
  lua_pushliteral(L, "capture surviving a native side exit");
  lua_setfield(L, -2, "bytes");
  call(L, 1, 1);
  fresh = funcV(L->top - 1);
  assert(la_load32_acq(&fresh->l.gc2_scan_cycle) == 0);

  lua_pushvalue(L, 1);
  lua_pushinteger(L, 23);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 4);
  lua_remove(L, 4);
  (void)lj_gc2_flush_ssb(g, L2TG(L));
  lj_trace_test_reset_exit_stats();
  lj_snap_test_roots_hook(observe_roots);
  call(L, 3, 2);
  lj_snap_test_roots_hook(NULL);
  assert(lj_trace_test_exit_calls() != 0);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  assert(lua_tointeger(L, -2) == 260);
  assert(funcV(L->top - 1) == fresh);
  assert(snapshots == 1);

  lua_gc(L, LUA_GCCOLLECT, 0);
  call(L, 0, 1);
  assert(lua_type(L, -1) == LUA_TSTRING);
  assert(strcmp(lua_tostring(L, -1), "capture surviving a native side exit") == 0);
  lua_close(L);
  puts("t-jit-snapshot-roots OK");
  return 0;
}
