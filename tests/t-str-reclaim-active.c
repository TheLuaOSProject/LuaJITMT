/*
** Public string reclamation with an attached Lua worker.
*/
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_str.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

static char held[1024];
static const char *borrowed;
static uint32_t stop_requested;
static uint32_t callback_ready, callback_release;

static void run_callback(void (*fn)(void))
{
  fn();
}

static int callback_park(lua_State *L)
{
  if (lua_gettop(L) == 0) {
    LJNativeFrame f;
    lj_native_enter_l(L, &f);
    while (!la_load32_acq(&callback_ready))
      la_cpu_pause();
    (void)lj_native_leave_l(L, &f);
  } else if (lua_toboolean(L, 1)) {
    la_store32_rel(&callback_release, 1);
  } else {
    char expected[512];
    const char *bytes;
    size_t len;
    LJNativeFrame f;
    memset(expected, 'B', sizeof(expected));
    lua_pushlstring(L, expected, sizeof(expected));
    bytes = lua_tolstring(L, -1, &len);
    assert(len == sizeof(expected));
    lj_native_enter_l(L, &f);
    la_store32_rel(&callback_ready, 1);
    while (!la_load32_acq(&callback_release))
      la_cpu_pause();
    (void)lj_native_leave_l(L, &f);
    assert(memcmp(bytes, expected, len) == 0);
    lua_pushlstring(L, expected, sizeof(expected));
    assert(lua_tolstring(L, -1, NULL) == bytes);
  }
  return 0;
}

static int stop_worker(lua_State *L)
{
  if (lua_gettop(L)) {
    la_store32_rel(&stop_requested, 1);
    return 0;
  }
  lua_pushboolean(L, la_load32_acq(&stop_requested) != 0);
  return 1;
}

static int check_reclamation(lua_State *L)
{
  global_State *g = G(L);
  assert(mt_live_acq(g) == 1);
  assert(memcmp(borrowed, held, sizeof(held)) == 0);
  lua_pushlstring(L, held, sizeof(held)-1);
  assert(lua_tolstring(L, -1, NULL) == borrowed);
  lua_pushnumber(L, (lua_Number)la_load64_acq(&g->str.sweep_reclaimed));
  return 1;
}

/* Sample the attached peer's actual native execution, not trace metadata. */
static int worker_running_trace(lua_State *L)
{
  global_State *g = G(L);
  TGState *peer = gc2_tg_list_acq(g);
  assert(peer && peer != g->main_tg && mt_live_acq(g) == 1);
  lua_pushboolean(L, lj_tg_vmstate_load_acq(peer) > 0);
  return 1;
}

static void run_reclamation(int use_jit)
{
  static const char script[] =
    "local stats, held, stop, park, invoke, use_jit, running_trace = ...\n"
    "local threading = require('threading')\n"
    "if use_jit then jit.opt.start('hotloop=8')\n"
    "elseif jit then jit.off() end\n"
    "local ready, start = threading.channel(1), threading.channel(1)\n"
    "local have_ffi, ffi = pcall(require, 'ffi')\n"
    "if not have_ffi then ffi = nil end\n"
    "local peer = assert(threading.spawn(function(stop, ready, start, held, park, invoke, ffi, use_jit)\n"
    "  assert(ready:send(true, 10))\n"
    "  assert(select(2, start:recv(20)))\n"
    "  assert(ready:send(true, 10))\n"
    "  if ffi then\n"
    "    local cb = ffi.cast('void (*)(void)', function() park(false) end)\n"
    "    ffi.cast('void (*)(void (*)(void))', invoke)(cb)\n"
    "    cb:free()\n"
    "  end\n"
    "  local sum = 0\n"
    "  local iterations = use_jit and 3000 or 200000\n"
    "  repeat\n"
    "    for i = 1, iterations do\n"
    "      sum = sum + #held\n"
    "      if sum > 1000000000 then sum = 0 end\n"
    "    end\n"
    "  until stop()\n"
    "  return #held\n"
    "end, stop, ready, start, held, park, invoke, ffi, use_jit))\n"
    "local function churn(round)\n"
    "  for i = 1, 600 do\n"
    "    local s = ('%d:%d:'):format(round, i):rep(20)\n"
    "    assert(#s >= 80)\n"
    "  end\n"
    "end\n"
    "assert(select(2, ready:recv(10)))\n"
    "local before = stats()\n"
    "churn(0)\n"
    "collectgarbage('collect')\n"
    "assert(stats() == before, 'native ACK certified string acquisition')\n"
    "assert(start:send(true, 10))\n"
    "assert(select(2, ready:recv(10)))\n"
    "if ffi then\n"
    "  park()\n"
    "  churn(12)\n"
    "  collectgarbage('collect')\n"
    "  assert(stats() == before, 'FFI callback certified string acquisition')\n"
    "  park(true)\n"
    "end\n"
    "if use_jit then\n"
    "  local running\n"
    "  for i = 1, 1000 do\n"
    "    running = running_trace()\n"
    "    if running then break end\n"
    "    threading.sleep(0.001)\n"
    "  end\n"
    "  assert(running, 'worker did not execute a native trace')\n"
    "end\n"
    "local weak = setmetatable({('weak-value-'):rep(16)}, {__mode = 'v'})\n"
    "local finalized = 0\n"
    "do\n"
    "  local p = newproxy(true)\n"
    "  getmetatable(p).__gc = function() finalized = finalized + 1 end\n"
    "end\n"
    "local settled\n"
    "for r = 1, 6 do\n"
    "  local prior = stats()\n"
    "  churn(r)\n"
    "  for attempt = 1, use_jit and 400 or 8 do\n"
    "    collectgarbage('collect')\n"
    "    if stats() - prior >= 600 then break end\n"
    "    if use_jit then threading.sleep(0.001) end\n"
    "  end\n"
    "  assert(stats() - prior >= 600, 'cohort did not retire while active')\n"
    "  local memory = collectgarbage('count')\n"
    "  if r == 3 then settled = memory end\n"
    "  if r > 3 then assert(memory < settled + 384, 'unbounded churn') end\n"
    "  assert(#weak[1] == 176)\n"
    "end\n"
    "assert(stats() - before > 1500, 'no active string body reclamation')\n"
    "assert(finalized == 1)\n"
    "stop(true)\n"
    "local joined, n = peer:join(10)\n"
    "assert(joined and n == #held)\n";
  lua_State *L = luaL_newstate();
  int status;
  assert(L);
  stop_requested = callback_ready = callback_release = 0;
  luaL_openlibs(L);
  memset(held, 'H', sizeof(held)-1);
  lua_pushlstring(L, held, sizeof(held)-1);
  borrowed = lua_tolstring(L, -1, NULL);
  status = luaL_loadbuffer(L, script, sizeof(script)-1, "=active string GC");
  assert(status == LUA_OK);
  lua_pushcfunction(L, check_reclamation);
  lua_pushvalue(L, 1);
  lua_pushcfunction(L, stop_worker);
  lua_pushcfunction(L, callback_park);
  lua_pushlightuserdata(L, (void *)(uintptr_t)run_callback);
  lua_pushboolean(L, use_jit);
  lua_pushcfunction(L, worker_running_trace);
  status = lua_pcall(L, 7, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == LUA_OK);
  assert(lua_tolstring(L, 1, NULL) == borrowed);
  lua_close(L);
}

static int request_pending(lua_State *L)
{
  lua_pushboolean(L, lj_str_reclaim_requested_acq(G(L)) != 0);
  return 1;
}

static void check_request_ownership(void)
{
  static const char script[] =
    "jit.off()\n"
    "local threading = require('threading')\n"
    "local ready, run, done = threading.channel(1), threading.channel(1), threading.channel(1)\n"
    "local peer = assert(threading.spawn(function(ready, run, done)\n"
    "  assert(ready:send(true, 10))\n"
    "  assert(select(2, run:recv(10)))\n"
    "  assert(collectgarbage('collect') == 0)\n"
    "  assert(done:send(true, 10))\n"
    "  assert(select(2, run:recv(10)))\n"
    "end, ready, run, done))\n"
    "assert(select(2, ready:recv(10)))\n"
    "collectgarbage('stop')\n"
    "local result\n"
    "do\n"
    "  local proxy = newproxy(true)\n"
    "  getmetatable(proxy).__gc = function()\n"
    "    local before = request_pending()\n"
    "    collectgarbage('collect')\n"
    "    local after_nested = request_pending()\n"
    "    assert(run:send(true, 10))\n"
    "    assert(select(2, done:recv(10)))\n"
    "    result = { before, after_nested, request_pending() }\n"
    "  end\n"
    "end\n"
    "assert(collectgarbage('collect') == 0)\n"
    "assert(run:send(true, 10))\n"
    "assert(peer:join(10))\n"
    "assert(result and result[1], 'outer request missing in finalizer')\n"
    "assert(result[2], 'nested collect canceled outer request')\n"
    "assert(result[3], 'peer collect canceled outer request')\n"
    "assert(not request_pending(), 'outer request escaped its scope')\n";
  lua_State *L = luaL_newstate();
  int status;
  assert(L);
  luaL_openlibs(L);
  lua_pushcfunction(L, request_pending);
  lua_setglobal(L, "request_pending");
  status = luaL_dostring(L, script);
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == LUA_OK);
  lua_close(L);
}

int main(void)
{
  check_request_ownership();
  run_reclamation(0);
#if LJ_HASJIT && LJ_TARGET_X64
  run_reclamation(1);
#endif
  puts("t-str-reclaim-active OK");
  return 0;
}
