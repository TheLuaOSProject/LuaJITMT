/*
** Allocation-only string reclamation in the public C API and Lua VM.
*/
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_str.h"
#include "lj_tg.h"
#include "lj_thr.h"

static char expected[LJ_HUGE_THRESHOLD + 1024];
static const char *borrowed;
static uint32_t stop_requested, worker_progress;
static uint32_t borrow_active;

static uint64_t reclaimed(lua_State *L)
{
  return la_load64_acq(&G(L)->str.sweep_reclaimed);
}

static void check_bytes(const char *p)
{
  assert(memcmp(p, expected, sizeof(expected)-1) == 0);
  assert(p[sizeof(expected)-1] == '\0');
}

static lua_State *new_owner(int libs)
{
  lua_State *L = luaL_newstate();
  assert(L);
  if (libs)
    luaL_openlibs(L);
  lua_pushlstring(L, expected, sizeof(expected)-1);
  borrowed = lua_tolstring(L, 1, NULL);
  check_bytes(borrowed);
  return L;
}

static void churn_c(lua_State *L, int round, int count)
{
  char payload[4096];
  int i;
  memset(payload, 'C', sizeof(payload));
  memcpy(payload, &round, sizeof(round));
  for (i = 0; i < count; i++) {
    memcpy(payload + sizeof(round), &i, sizeof(i));
    lua_pushlstring(L, payload, sizeof(payload));
    assert(lua_objlen(L, 2) == sizeof(payload));
    lua_pop(L, 1);
  }
}

static uintptr_t probe_address;
static uint32_t probe_unmapped;

extern int __real_munmap(void *addr, size_t size);

int __wrap_munmap(void *addr, size_t size)
{
  uintptr_t start = (uintptr_t)addr;
  uintptr_t probe = la_loaduptr_acq(&probe_address);
  int status = __real_munmap(addr, size);
  /* The kernel may reuse the address before the Lua observer runs. Record
  ** a successful physical unmap while its exact range is still known. */
  if (status == 0 && probe != 0 && probe >= start && probe - start < size) {
    TGState *tg = lj_thr_get_tg();
    uint32_t unseen = 0;
    uint32_t actor = tg && lj_tg_flags_test_acq(tg, TGF_LUA_ALLOC) ? 2 : 1;
    (void)la_cas32(&probe_unmapped, &unseen, actor, LA_ACQ_REL, LA_ACQ);
  }
  return status;
}

static void allocate_huge_probe(lua_State *L, int round)
{
  char payload[LJ_HUGE_THRESHOLD * 2u + 17u];
  GCstr *s;
  LJHugeInfo info;
  memset(payload, 'p', sizeof(payload));
  memcpy(payload, &round, sizeof(round));
  lua_pushlstring(L, payload, sizeof(payload));
  s = strV(L->top - 1);
  assert(lj_arena_hugetab_lookup(&L2TG(L)->huge, s, &info) == 1);
  la_store32_rel(&probe_unmapped, 0);
  la_storeuptr_rel(&probe_address, (uintptr_t)s);
  lua_pop(L, 1);
}

static int huge_probe_live(void)
{
  return !la_load32_acq(&probe_unmapped);
}

static void check_c_api(void)
{
  lua_State *L = new_owner(0);
  int round, settled = 0;
  uint64_t stopped = reclaimed(L);

  lua_gc(L, LUA_GCSTOP, 0);
  allocate_huge_probe(L, -1);
  churn_c(L, 0, 1800);
  assert(huge_probe_live() && reclaimed(L) == stopped);
  lua_gc(L, LUA_GCRESTART, 0);
  for (round = 1; round <= 8; round++) {
    uint64_t before = reclaimed(L);
    churn_c(L, round, 1800);
    check_bytes(borrowed);
    lua_pushlstring(L, borrowed, sizeof(expected)-1);
    assert(lua_tolstring(L, 2, NULL) == borrowed);
    lua_pop(L, 1);
    assert(lua_pushfstring(L, "%s", borrowed) == borrowed);
    lua_pop(L, 1);
    if (round >= 3)
      assert(reclaimed(L) - before >= 1000);
    if (round == 3)
      settled = lua_gc(L, LUA_GCCOUNT, 0);
    if (round > 3)
      assert(lua_gc(L, LUA_GCCOUNT, 0) <= settled + 2048);
  }
  assert(!huge_probe_live());
  assert(reclaimed(L) > 10000);
  lua_close(L);
}

static int snapshot(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg;
  int native = 0;
  if (lj_gc2_smr_read_try(g)) {
    for (tg = gc2_tg_list_acq(g); tg; tg = lj_tg_next_acq(tg))
      if (tg != g->main_tg && lj_tg_flags_test_acq(tg, TGF_LUA_ALLOC) &&
          lj_tg_vmstate_load_acq(tg) > 0)
        native = 1;
    lj_gc2_smr_read_leave(g);
  }
  check_bytes(borrowed);
  assert((uint32_t)(lj_str_reclaim_scope_acq(g) >> 32) !=
         lj_tg_tid_acq(L2TG(L)));
  lua_pushnumber(L, (lua_Number)reclaimed(L));
  lua_pushnumber(L, (lua_Number)la_load32_acq(&worker_progress));
  lua_pushnumber(L, (lua_Number)mt_live_acq(g));
  lua_pushboolean(L, native);
  return 4;
}

static int probe(lua_State *L)
{
  if (lua_gettop(L)) {
    allocate_huge_probe(L, (int)luaL_checkinteger(L, 1));
    return 0;
  }
  lua_pushboolean(L, huge_probe_live());
  lua_pushboolean(L, la_load32_acq(&probe_unmapped) == 2);
  return 2;
}

static int tick(lua_State *L)
{
  if (lua_gettop(L)) {
    la_store32_rel(&stop_requested, 1);
    return 0;
  }
  (void)la_add32_rlx(&worker_progress, 1);
  lua_pushboolean(L, la_load32_acq(&stop_requested) != 0);
  return 1;
}

static void invoke_borrowed(const char *p, void (*callback)(void))
{
  assert(p == borrowed);
  check_bytes(p);
  la_store32_rel(&borrow_active, 1);
  callback();
  la_store32_rel(&borrow_active, 0);
  check_bytes(p);
}

static void check_vm(int use_jit, int workers)
{
  static const char chunk[] =
    "local held, stats, tick, invoke, use_jit, workers, probe = ...\n"
    "collectgarbage('stop')\n"
    "if use_jit then jit.opt.start('hotloop=4', 'hotexit=3')\n"
    "else jit.off() end\n"
    "local function churn(sink, round, n)\n"
    "  local tally = 0\n"
    "  for i = 1, n do\n"
    "    tally = tally + i\n"
    "    local s = ('%d/%d automatic:'):format(round, i):rep(252)\n"
    "    if #s < 3000 then error('allocation result changed') end\n"
    "    sink[1] = s\n"
    "  end\n"
    "  assert(tally == n*(n+1)/2, tostring(tally))\n"
    "end\n"
    "local ffi = require('ffi')\n"
    "local callback = ffi.cast('void (*)(void)', function()\n"
    "  churn({false}, 0, 800)\n"
    "  assert(#held > 10000)\n"
    "  stats()\n"
    "end)\n"
    "ffi.cast('void (*)(const char *, void (*)(void))', invoke)(held, callback)\n"
    "callback:free()\n"
    "local threading = require('threading')\n"
    "assert(threading.gcworkers(workers) == 0)\n"
    "local ready = threading.channel(1)\n"
    "local peer = assert(threading.spawn(function(ready, tick, held)\n"
    "  assert(ready:send(true, 10))\n"
    "  local total = 0\n"
    "  repeat\n"
    "    for i = 1, 2048 do\n"
    "      total = total + #held\n"
    "      if total > 1000000000 then total = 0 end\n"
    "    end\n"
    "  until tick()\n"
    "  return #held\n"
    "end, ready, tick, held))\n"
    "assert(select(2, ready:recv(10)) == true)\n"
    "if use_jit then\n"
    "  local deadline = threading.now() + 5\n"
    "  local seen\n"
    "  repeat\n"
    "    seen = select(4, stats())\n"
    "    if not seen then threading.sleep(0.001) end\n"
    "  until seen or threading.now() >= deadline\n"
    "  assert(seen, 'managed peer did not execute native trace')\n"
    "end\n"
    "probe(1)\n"
    "assert(probe())\n"
    "local sink = {false}\n"
    "local stopped = stats()\n"
    "churn(sink, -1, 1800)\n"
    "assert(probe() and stats() == stopped, 'collection continued after stop')\n"
    "collectgarbage('restart')\n"
    "local reclaimed0, progress0, unmapped = 0, 0, false\n"
    "for round = 1, 64 do\n"
    "  churn(sink, round, 1800)\n"
    "  assert(sink[1]:sub(1, #tostring(round) + 5) == tostring(round)..'/1800')\n"
    "  local reclaimed, progress, active = stats()\n"
    "  assert(active == 1, 'managed worker ended during allocation')\n"
    "  unmapped = unmapped or not probe()\n"
    "  if round == 16 then reclaimed0, progress0 = reclaimed, progress end\n"
    "  if round > 16 then\n"
    "    assert(collectgarbage('count') < 65536, 'automatic string heap did not settle')\n"
    "  end\n"
    "  if round == 64 then\n"
    "    assert(reclaimed - reclaimed0 > 80000, 'no sustained automatic reclamation')\n"
    "    assert(unmapped, 'automatic sweep did not unmap a huge string')\n"
    "    assert(progress > progress0, 'managed worker did not progress')\n"
    "  end\n"
    "end\n"
    "tick(true)\n"
    "local joined, n = peer:join(10)\n"
    "assert(joined and n == #held and threading.gcworkers() == workers)\n";
  lua_State *L = new_owner(1);
  int err;
  stop_requested = worker_progress = 0;
  assert(luaL_loadbuffer(L, chunk, sizeof(chunk)-1, "=automatic string GC") ==
	 LUA_OK);
  lua_pushvalue(L, 1);
  lua_pushcfunction(L, snapshot);
  lua_pushcfunction(L, tick);
  lua_pushlightuserdata(L, (void *)(uintptr_t)invoke_borrowed);
  lua_pushboolean(L, use_jit);
  lua_pushinteger(L, workers);
  lua_pushcfunction(L, probe);
  err = lua_pcall(L, 7, 0, 0);
  if (err != LUA_OK)
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(err == LUA_OK);
  assert(lua_tolstring(L, 1, NULL) == borrowed);
  check_bytes(borrowed);
  lua_close(L);
}

static int managed_snapshot(lua_State *L)
{
  TGState *main = G(L)->main_tg;
  check_bytes(borrowed);
  lua_pushnumber(L, (lua_Number)reclaimed(L));
  lua_pushboolean(L, lj_tg_vmstate_load_acq(main) > 0);
  lua_pushboolean(L, lj_tg_in_native_acq(main));
  lua_pushnumber(L, (lua_Number)la_load64_acq(&main->str_vm_root_epoch));
  lua_pushboolean(L, la_load32_acq(&borrow_active));
  assert(!gc2_recovery_failed_acq(G(L)));
  return 5;
}

static void check_managed(int joining, int workers)
{
  static const char chunk[] =
    "local held, stats, tick, invoke, joining, workers, probe = ...\n"
    "local th, ffi = require('threading'), require('ffi')\n"
    "jit.opt.start('hotloop=4', 'hotexit=3')\n"
    "assert(th.gcworkers(workers) == 0)\n"
    "local ready, done = th.channel(1), th.channel(1)\n"
    "local function main_window(n, s)\n"
    "  local sum = 0; for i=1,n do sum = sum + #s end\n"
    "  assert(sum == n * #s)\n"
    "end\n"
    "local peer = th.spawn(function(ready, done, held, stats, tick, probe, joining)\n"
    "  assert(select(2, ready:recv(90)) == true)\n"
    "  local sink = {false}\n"
    "  local function churn(round, n)\n"
    "    local tally = 0\n"
    "    for i=1,n do\n"
    "      local s = ('%d/%d worker auto:'):format(round, i):rep(252)\n"
    "      if #s <= 3000 then error('short string') end\n"
    "      tally = tally + i; sink[1] = s\n"
    "    end\n"
    "    assert(tally == n*(n+1)/2 and #held > 10000)\n"
    "  end\n"
    "  assert(select(5, stats())); probe(1); churn(-1, 1200)\n"
    "  assert(select(5, stats()) and probe(), 'string retired during a foreign native borrow')\n"
    "  assert(done:send(true, 90) == true)\n"
    "  local native, waiting = false, false\n"
    "  if not joining then\n"
    "    local deadline = th.now() + 10\n"
    "    repeat native = select(2, stats()) until native or th.now() >= deadline\n"
    "    assert(native, 'main Lua state did not execute native code')\n"
    "  end\n"
    "  local before, high = stats(), 0\n"
    "  for round=1,(joining and 16 or 64) do\n"
    "    churn(round, 1800)\n"
    "    local count, running, parked = stats()\n"
    "    native, waiting = native or running, waiting or parked\n"
    "    if round == 4 then before = count end\n"
    "    if round > 4 then\n"
    "      local kb = collectgarbage('count')\n"
    "      high = kb >= 65536 and high + 1 or 0\n"
    "      assert(high < 4, ('managed heap stayed high at round %d (%d KiB)'):format(round, kb))\n"
    "    end\n"
    "  end\n"
    "  local count, _, _, epoch = stats()\n"
    "  local live, byworker = probe()\n"
    "  assert(not live and (not joining or byworker), 'huge string still mapped')\n"
    "  assert(count - before > (joining and 20000 or 80000), 'no sustained frees')\n"
    "  assert(epoch > 0 and (joining and waiting or not joining and native))\n"
    "  tick(true); return #held\n"
    "end, ready, done, held, stats, tick, probe, joining)\n"
    "if not joining then for i=1,20 do main_window(4096, held) end end\n"
    "local cb = ffi.cast('void (*)(void)', function()\n"
    "  assert(ready:send(true, 90) == true); assert(select(2, done:recv(90)) == true)\n"
    "end)\n"
    "ffi.cast('void (*)(const char *, void (*)(void))', invoke)(held, cb)\n"
    "cb:free()\n"
    "if not joining then\n"
    "  local windows = 0\n"
    "  repeat main_window(2048, held); windows = windows + 1\n"
    "  until tick() or (windows % 128 == 0 and not peer:running())\n"
    "end\n"
    "local ok, n = peer:join(); assert(ok and n == #held, tostring(n))\n";
  lua_State *L = new_owner(1);
  int status;
  stop_requested = worker_progress = 0;
  assert(luaL_loadbuffer(L, chunk, sizeof(chunk)-1, "=managed automatic string GC") == LUA_OK);
  lua_pushvalue(L, 1);
  lua_pushcfunction(L, managed_snapshot);
  lua_pushcfunction(L, tick);
  lua_pushlightuserdata(L, (void *)(uintptr_t)invoke_borrowed);
  lua_pushboolean(L, joining);
  lua_pushinteger(L, workers);
  lua_pushcfunction(L, probe);
  status = lua_pcall(L, 7, 0, 0);
  if (status != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(L, -1));
  assert(status == LUA_OK && lua_tolstring(L, 1, NULL) == borrowed);
  check_bytes(borrowed);
  lua_close(L);
}

int main(int argc, char **argv)
{
  memset(expected, 'H', sizeof(expected)-1);
  if (argc == 1 || strcmp(argv[1], "c") == 0)
    check_c_api();
  if (argc == 1 || strcmp(argv[1], "interp") == 0)
    check_vm(0, 0);
  if (LJ_HASJIT && (argc == 1 || strcmp(argv[1], "jit") == 0))
    check_vm(1, 0);
  if (LJ_HASJIT && (argc == 1 || strcmp(argv[1], "pool") == 0))
    check_vm(1, 2);
  if (LJ_HASJIT && (argc == 1 || strcmp(argv[1], "managed-join") == 0))
    check_managed(1, 0);
  if (LJ_HASJIT && (argc == 1 || strcmp(argv[1], "managed-join-pool") == 0))
    check_managed(1, 2);
  if (LJ_HASJIT && (argc == 1 || strcmp(argv[1], "managed-jit") == 0))
    check_managed(0, 0);
  if (LJ_HASJIT && (argc == 1 || strcmp(argv[1], "managed-pool") == 0))
    check_managed(0, 2);
  puts("t-str-reclaim-auto OK");
  return 0;
}
