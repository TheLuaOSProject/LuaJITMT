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

static char held[1024];
static const char *borrowed;
static uint32_t stop_requested;

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

int main(void)
{
  static const char script[] =
    "local stats, held, stop = ...\n"
    "local threading = require('threading')\n"
    "if jit then jit.off() end\n"
    "local ready, start = threading.channel(1), threading.channel(1)\n"
    "local peer = assert(threading.spawn(function(stop, ready, start, held)\n"
    "  assert(ready:send(true, 10))\n"
    "  assert(select(2, start:recv(20)))\n"
    "  assert(ready:send(true, 10))\n"
    "  local sum = 0\n"
    "  repeat\n"
    "    for i = 1, 200000 do\n"
    "      sum = sum + #held\n"
    "      if sum > 1000000000 then sum = 0 end\n"
    "    end\n"
    "  until stop()\n"
    "  return #held\n"
    "end, stop, ready, start, held))\n"
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
    "  for attempt = 1, 8 do\n"
    "    collectgarbage('collect')\n"
    "    if stats() - prior >= 600 then break end\n"
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
  luaL_openlibs(L);
  memset(held, 'H', sizeof(held)-1);
  lua_pushlstring(L, held, sizeof(held)-1);
  borrowed = lua_tolstring(L, -1, NULL);
  status = luaL_loadbuffer(L, script, sizeof(script)-1, "=active string GC");
  assert(status == LUA_OK);
  lua_pushcfunction(L, check_reclamation);
  lua_pushvalue(L, 1);
  lua_pushcfunction(L, stop_worker);
  status = lua_pcall(L, 3, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == LUA_OK);
  assert(lua_tolstring(L, 1, NULL) == borrowed);
  lua_close(L);
  puts("t-str-reclaim-active OK");
  return 0;
}
