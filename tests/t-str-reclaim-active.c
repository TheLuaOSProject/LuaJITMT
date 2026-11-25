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

static char held[LJ_HUGE_THRESHOLD + 1024];
static const char *borrowed;
static GCstr *huge_probe;
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
    char expected[LJ_HUGE_THRESHOLD + 512];
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

/* Keep only the mapping address in C. Look it up without accessing the
** old bytes after the allocating call has returned to Lua. */
static int probe_huge_mapping(lua_State *L)
{
  LJHugeInfo info;
  if (lua_gettop(L)) {
    char payload[LJ_HUGE_THRESHOLD * 2 + 17];
    int round = (int)luaL_checkinteger(L, 1);
    memset(payload, '0' + round, sizeof(payload));
    lua_pushlstring(L, payload, sizeof(payload));
    huge_probe = strV(L->top - 1);
    assert(lj_arena_hugetab_lookup(&L2TG(L)->huge, huge_probe, &info));
    lua_pop(L, 1);
    lua_pushnil(L);
    return 0;
  }
  lua_pushboolean(L, lj_arena_hugetab_lookup(&L2TG(L)->huge, huge_probe,
					     &info) != 0);
  return 1;
}

/* Sample the attached peer's actual native execution, not trace metadata. */
static int worker_running_trace(lua_State *L)
{
  global_State *g = G(L);
  TGState *peer = gc2_tg_list_acq(g);
  while (peer && !lj_tg_flags_test_acq(peer, TGF_LUA_ALLOC))
    peer = lj_tg_next_acq(peer);
  assert(peer && mt_live_acq(g) == 1);
  lua_pushboolean(L, lj_tg_vmstate_load_acq(peer) > 0);
  return 1;
}

static void run_reclamation(int use_jit, int pool)
{
  static const char script[] =
    "local stats, held, stop, park, invoke, use_jit, running_trace, probe, pool = ...\n"
    "local threading = require('threading')\n"
    "if use_jit then jit.opt.start('hotloop=8')\n"
    "elseif jit then jit.off() end\n"
    "assert(threading.gcworkers(pool) == 0)\n"
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
    "  probe(r)\n"
    "  for attempt = 1, use_jit and 400 or (pool > 0 and 32 or 8) do\n"
    "    collectgarbage('collect')\n"
    "    if stats() - prior >= 600 and not probe() then break end\n"
    "    if use_jit then threading.sleep(0.001) end\n"
    "  end\n"
    "  assert(stats() - prior >= 600, 'cohort did not retire while active')\n"
    "  assert(not probe(), 'unreachable huge string mapping survived')\n"
    "  local memory = collectgarbage('count')\n"
    "  if r == 3 then settled = memory end\n"
    "  if r > 3 then assert(memory < settled + 384, 'unbounded churn') end\n"
    "  assert(#weak[1] == 176)\n"
    "end\n"
    "assert(stats() - before > 1500, 'no active string body reclamation')\n"
    "assert(finalized == 1)\n"
    "stop(true)\n"
    "local joined, n = peer:join(10)\n"
    "assert(joined and n == #held)\n"
    "assert(threading.gcworkers(0) == pool)\n";
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
  lua_pushcfunction(L, probe_huge_mapping);
  lua_pushinteger(L, pool);
  status = lua_pcall(L, 9, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "jit=%d pool=%d: %s\n", use_jit, pool, lua_tostring(L, -1));
  assert(status == LUA_OK);
  assert(lua_tolstring(L, 1, NULL) == borrowed);
  lua_close(L);
}

static void check_pool_only_reclamation(void)
{
  static const char script[] =
    "if jit then jit.off() end\n"
    "local threading = require('threading')\n"
    "assert(threading.gcworkers(2) == 0)\n"
    "local keep = ('pool-root:'):rep(40)\n"
    "local settled\n"
    "for round = 1, 8 do\n"
    "  collectgarbage('stop')\n"
    "  for i = 1, 1000 do\n"
    "    assert(#('%d/%d pool data:'):format(round, i):rep(30) >= 300)\n"
    "  end\n"
    "  local peak = collectgarbage('count')\n"
    "  local memory\n"
    "  for pass = 1, 48 do\n"
    "    collectgarbage('collect')\n"
    "    memory = collectgarbage('count')\n"
    "    if peak - memory >= 250 then break end\n"
    "  end\n"
    "  assert(peak - memory >= 250, ('pool did not reclaim small string bytes: %g -> %g at %d'):format(peak, memory, round))\n"
    "  if round == 3 then settled = memory end\n"
    "  if round > 3 then assert(memory < settled + 512, 'pool heap grew') end\n"
    "  assert(keep == ('pool-root:'):rep(40))\n"
    "end\n"
    "assert(threading.gcworkers() == 2)\n";
  lua_State *L = luaL_newstate();
  int status;
  assert(L);
  luaL_openlibs(L);
  status = luaL_dostring(L, script);
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == LUA_OK);
  /* Exercise joined teardown with the public pool still running. */
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
    "    assert(collectgarbage('step', 1) == false)\n"
    "    assert(request_pending())\n"
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

static int check_step_scope(lua_State *L)
{
  uint32_t request = lua_toboolean(L, 1) ?
    LJ_STR_RECLAIM_STEP : LJ_STR_RECLAIM_NONE;
  assert(lj_str_reclaim_request_acq(G(L)) == request);
  assert(lj_str_reclaim_exclusive_acq(G(L)) == 0);
  return 0;
}

static void step_borrow(const char *bytes, void (*callback)(void))
{
  assert(memcmp(bytes, held, sizeof(held)) == 0);
  callback();
  assert(memcmp(bytes, held, sizeof(held)) == 0);
}

static void check_step_reclamation(int workers)
{
  static const char script[] =
    "local keep, check_scope, borrow, workers, probe = ...\n"
    "if jit then jit.off() end\n"
    "local threading = require('threading')\n"
    "assert(threading.gcworkers(workers) == 0)\n"
    "local cycles = 0\n"
    "local mode = table.concat({'__', 'mode'})\n"
    "local weak = setmetatable({}, {[mode] = 'v'})\n"
    "local function step()\n"
    "  if collectgarbage('step', 1) then cycles = cycles + 1 end\n"
    "  check_scope()\n"
    "end\n"
    "local ok, ffi = pcall(require, 'ffi')\n"
    "if ok then\n"
    "  local callback = ffi.cast('void (*)(void)', function()\n"
    "    for i = 1, 40 do step() end\n"
    "  end)\n"
    "  ffi.cast('void (*)(const char *, void (*)(void))', borrow)(keep, callback)\n"
    "  callback:free()\n"
    "end\n"
    "local settled\n"
    "for round = 1, 5 do\n"
    "  collectgarbage('stop')\n"
    "  for i = 1, 1200 do\n"
    "    assert(#('%d/%d step data:'):format(round, i):rep(30) >= 300)\n"
    "  end\n"
    "  probe(round)\n"
    "  local peak, memory = collectgarbage('count')\n"
    "  local deadline = threading.now() + 15\n"
    "  repeat\n"
    "    for i = 1, 32 do step() end\n"
    /* Background workers can complete the cycle between public steps. */
    "    if workers > 0 then threading.sleep(0.001) end\n"
    "    memory = collectgarbage('count')\n"
    "    assert(getmetatable(weak)[mode] == 'v', 'step lost rooted __mode')\n"
    "    if peak - memory > 450 and (not settled or memory < settled + 256)\n"
    "       and not probe() then break end\n"
    "  until threading.now() >= deadline\n"
    "  assert(peak - memory > 450, ('step heap %.0f -> %.0f'):format(peak, memory))\n"
    "  assert(not probe(), 'step retained huge string')\n"
    "  if round == 2 then settled = memory end\n"
    "  if round > 2 then assert(memory < settled + 256, 'step heap grew') end\n"
    "  assert(keep == ('S'):rep(#keep))\n"
    "end\n"
    "assert(workers > 0 or cycles > 0)\n"
    "assert(threading.gcworkers() == workers)\n";
  lua_State *L = luaL_newstate();
  int status;
  assert(L);
  luaL_openlibs(L);
  memset(held, 'S', sizeof(held)-1);
  held[sizeof(held)-1] = 0;
  lua_pushlstring(L, held, sizeof(held)-1);
  borrowed = lua_tolstring(L, 1, NULL);
  assert(luaL_loadbuffer(L, script, sizeof(script)-1, "=bounded string step") == 0);
  lua_pushvalue(L, 1);
  lua_pushcfunction(L, check_step_scope);
  lua_pushlightuserdata(L, (void *)(uintptr_t)step_borrow);
  lua_pushinteger(L, workers);
  lua_pushcfunction(L, probe_huge_mapping);
  status = lua_pcall(L, 5, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "workers=%d: %s\n", workers, lua_tostring(L, -1));
  assert(status == LUA_OK);
  assert(lua_tolstring(L, 1, NULL) == borrowed);
  assert(memcmp(borrowed, held, sizeof(held)) == 0);
  lua_close(L);
}

static void check_nested_step_scope(void)
{
  static const char script[] =
    "if jit then jit.off() end\n"
    "collectgarbage('stop')\n"
    "local finalized = false\n"
    "do\n"
    "  local proxy = newproxy(true)\n"
    "  getmetatable(proxy).__gc = function()\n"
    "    check_step_scope(true)\n"
    "    assert(collectgarbage('step', 1) == false)\n"
    "    assert(collectgarbage('collect') == 0)\n"
    "    check_step_scope(true)\n"
    "    finalized = true\n"
    "    error('expected bounded step finalizer error')\n"
    "  end\n"
    "end\n"
    "for i = 1, 500 do\n"
    "  collectgarbage('step', 1)\n"
    "  check_step_scope()\n"
    "  if finalized then break end\n"
    "end\n"
    "assert(finalized)\n";
  lua_State *L = luaL_newstate();
  int status;
  assert(L);
  luaL_openlibs(L);
  lua_pushcfunction(L, check_step_scope);
  lua_setglobal(L, "check_step_scope");
  status = luaL_dostring(L, script);
  if (status != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == LUA_OK);
  lua_close(L);
}

static uint32_t worker_sampled_main_trace;

static int main_running_trace(lua_State *L)
{
  TGState *main = G(L)->main_tg;
  if (L2TG(L) == main)
    lua_pushboolean(L, la_load32_acq(&worker_sampled_main_trace) != 0);
  else if (lua_gettop(L)) {
    la_store32_rel(&worker_sampled_main_trace, 1);
    return 0;
  } else {
    lua_pushboolean(L, lj_tg_vmstate_load_acq(main) > 0);
  }
  return 1;
}

static void check_worker_collection(int use_jit, int pool)
{
  static const char script[] =
    "local stats, stop, park, invoke, probe, native, use_jit, pool = ...\n"
    "if use_jit then jit.opt.start('hotloop=8')\n"
    "elseif jit then jit.off() end\n"
    "local threading = require('threading')\n"
    "assert(threading.gcworkers(pool) == 0)\n"
    "local have_ffi, ffi = pcall(require, 'ffi')\n"
    "local keeper = ('main Lua dispatch value:'):rep(300)\n"
    "local peer = assert(threading.spawn(function(stats, stop, park, probe, have_ffi, native, use_jit)\n"
    "  local prior = stats()\n"
    "  if have_ffi then\n"
    "    park()\n"
    "    probe(7)\n"
    "    collectgarbage('collect')\n"
    "    local blocked = stats() == prior and probe()\n"
    "    park(true)\n"
    "    assert(blocked, 'main callback certified reclamation')\n"
    "  end\n"
    "  for i = 1, 600 do\n"
    "    assert(#('worker body:%d:'):format(i):rep(30) >= 420)\n"
    "  end\n"
    "  probe(8)\n"
    "  if use_jit then\n"
    "    local running\n"
    "    for i = 1, 1000 do\n"
    "      running = native()\n"
    "      if running then break end\n"
    "      threading.sleep(0.001)\n"
    "    end\n"
    "    assert(running, 'main did not execute a native trace')\n"
    "    collectgarbage('collect')\n"
    "    native(true)\n"
    "  end\n"
    "  for i = 1, 400 do\n"
    "    collectgarbage('collect')\n"
    "    if stats() - prior >= 600 and not probe() then break end\n"
    "    threading.sleep(0.001)\n"
    "  end\n"
    "  local retired = stats() - prior\n"
    "  local mapping_retired = not probe()\n"
    "  stop(true)\n"
    "  return retired, mapping_retired\n"
    "end, stats, stop, park, probe, have_ffi, native, use_jit))\n"
    "if have_ffi then\n"
    "  local cb = ffi.cast('void (*)(void)', function() park(false) end)\n"
    "  ffi.cast('void (*)(void (*)(void))', invoke)(cb)\n"
    "  cb:free()\n"
    "end\n"
    "local function dispatch_window(s, n)\n"
    "  local sum = 0\n"
    "  for i = 1, n do sum = sum + #s end\n"
    "  return sum\n"
    "end\n"
    "if jit then jit.off(dispatch_window, true) end\n"
    "local len, count = #keeper, 0\n"
    "local deadline = threading.now() + 12\n"
    "while not stop() and threading.now() < deadline do\n"
    "  if use_jit and not native() then\n"
    "    for i = 1, 20000 do count = count + #keeper - len end\n"
    "  else\n"
    "    assert(dispatch_window(keeper, 100000) == len * 100000)\n"
    "  end\n"
    "end\n"
    "assert(count == 0 and keeper == ('main Lua dispatch value:'):rep(300))\n"
    "local joined, retired, mapping_retired = peer:join(10)\n"
    "assert(joined and retired >= 600, 'worker did not retire string bodies')\n"
    "assert(mapping_retired, 'worker did not unmap an unreachable string')\n"
    "assert(threading.gcworkers(0) == pool)\n";
  lua_State *L = luaL_newstate();
  int status;
  assert(L);
  stop_requested = callback_ready = callback_release = 0;
  worker_sampled_main_trace = 0;
  luaL_openlibs(L);
  memset(held, 'H', sizeof(held)-1);
  lua_pushlstring(L, held, sizeof(held)-1);
  borrowed = lua_tolstring(L, -1, NULL);
  assert(luaL_loadbuffer(L, script, sizeof(script)-1, "=worker string GC") ==
	 LUA_OK);
  lua_pushcfunction(L, check_reclamation);
  lua_pushcfunction(L, stop_worker);
  lua_pushcfunction(L, callback_park);
  lua_pushlightuserdata(L, (void *)(uintptr_t)run_callback);
  lua_pushcfunction(L, probe_huge_mapping);
  lua_pushcfunction(L, main_running_trace);
  lua_pushboolean(L, use_jit);
  lua_pushinteger(L, pool);
  status = lua_pcall(L, 8, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "worker full GC jit=%d pool=%d: %s\n",
	    use_jit, pool, lua_tostring(L, -1));
  assert(status == LUA_OK);
  assert(lua_tolstring(L, 1, NULL) == borrowed);
  assert(memcmp(borrowed, held, sizeof(held)) == 0);
  lua_close(L);
}

int main(void)
{
  check_request_ownership();
  check_worker_collection(0, 0);
  check_worker_collection(0, 2);
#if LJ_HASJIT
  check_worker_collection(1, 0);
  check_worker_collection(1, 2);
#endif
  check_nested_step_scope();
  check_step_reclamation(0);
  check_step_reclamation(2);
  run_reclamation(0, 0);
  run_reclamation(0, 2);
  check_pool_only_reclamation();
#if LJ_HASJIT && LJ_TARGET_X64
  run_reclamation(1, 0);
  run_reclamation(1, 2);
#endif
  puts("t-str-reclaim-active OK");
  return 0;
}
