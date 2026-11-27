/* Exercise detached worker reclamation driven entirely by Lua allocations. */
#include <stdio.h>
#include <unistd.h>

#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"

static int worker_census(lua_State *L)
{
  global_State *g = G(L);
  TGState *main_tg = g->main_tg, *tg;
  uint32_t dead = 0, pending = 0;
  uint64_t epoch, completed, peer_ack = 0;
  if (!lj_gc2_smr_read_try(g))
    return 0;
  for (tg = gc2_tg_list_acq(g); tg; tg = lj_tg_next_acq(tg)) {
    if (lj_tg_flags_all_acq(tg, TGF_DEAD|TGF_LUA_ALLOC))
      dead++;
    else if (lj_tg_flags_test_acq(tg, TGF_LUA_ALLOC))
      peer_ack = la_load64_acq(&tg->dead_tg_epoch);
  }
  for (tg = main_tg->dead_tg_head; tg; tg = lj_tg_worker_retire_next_acq(tg))
    pending++;
  epoch = la_load64_acq(&main_tg->dead_tg_epoch);
  completed = gc2_sweep_to_idle_acq(g);
  lj_gc2_smr_read_leave(g);
  lua_pushnumber(L, dead);
  lua_pushnumber(L, (lua_Number)epoch);
  lua_pushnumber(L, (lua_Number)completed);
  lua_pushnumber(L, pending);
  lua_pushnumber(L, (lua_Number)peer_ack);
  lua_pushnumber(L, gc2_phase_acq(g));
  return 6;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  alarm(60);
  lua_register(L, "worker_census", worker_census);
  ljt_lua_assert_ok(L, luaL_loadstring(L,
    "local th, buffer = require('threading'), require('string.buffer')\n"
    "th.gcworkers(tonumber(os.getenv('POOL')) or 0)\n"
    "if os.getenv('JIT') == 'off' then jit.off() end\n"
    "local ready, stop = th.channel(1), th.channel(1)\n"
    "local sentinel = assert(th.spawn(function()\n"
    "  assert(ready:send(true, 10))\n"
    "  assert(stop:recv(50))\n"
    "end))\n"
    "assert(ready:recv(10))\n"
    "local function census()\n"
    "  for _ = 1, 1000 do\n"
    "    local dead, epoch, completed, pending, ack, phase = worker_census()\n"
    "    if dead then return dead, epoch, completed, pending, ack, phase end\n"
    "    th.sleep(0)\n"
    "  end\n"
    "  error('worker census admission did not settle')\n"
    "end\n"
    "local payload = ('kept'):rep(256)\n"
    "local function churn()\n"
    "  for i = 1, 1024 do\n"
    "    local row = {i, tostring(i), {answer = 42}}\n"
    "    assert(row[3].answer == 42)\n"
    "  end\n"
    "end\n"
    "jit.off(churn)\n"
    "local function worker()\n"
    "  local result = buffer.new(1024)\n"
    "  for _ = 1, 200 do\n"
    "    assert(result:reset():put(payload):get() == payload)\n"
    "  end\n"
    "  return result:put(payload)\n"
    "end\n"
    "local function run_wave(keep_first)\n"
    "  local jobs = {}\n"
    "  local kept\n"
    "  for i = 1, 4 do jobs[i] = assert(th.spawn(worker)) end\n"
    "  for i, job in ipairs(jobs) do\n"
    "    local ok, result = job:join(10)\n"
    "    assert(ok and result:tostring() == payload)\n"
    "    if keep_first and i == 1 then\n"
    "      kept = {job, result}\n"
    "    else\n"
    "      assert(result:get() == payload)\n"
    "      result:free()\n"
    "    end\n"
    "  end\n"
    "  return kept\n"
    "end\n"
    "local _, _, initial_cycles = census()\n"
    "local retained = run_wave(true)\n"
    "for _ = 1, 35 do\n"
    "  run_wave(false)\n"
    "  for _ = 1, 3 do churn() end\n"
    "end\n"
    "for _ = 1, 12 do th.sleep(0.002); churn() end\n"
    "local joined, original = retained[1]:join(0)\n"
    "assert(joined and original == retained[2] and original:get() == payload)\n"
    "assert(sentinel:running())\n"
    "local dead, epoch, completed, pending, ack, phase = census()\n"
    "local stats = ('retained %d of 144 TGs (%d pending), epoch %d, peer %d, swept %d, phase %d')\n"
    "  :format(dead + pending, pending, epoch, ack, completed - initial_cycles, phase)\n"
    "assert(completed > initial_cycles and epoch > 0 and dead + pending <= 48, stats)\n"
    "original:free()\n"
    "assert(stop:send(true, 10) and sentinel:join(10))\n"
    "th.gcworkers(0)\n"), "load automatic worker reclamation test");
  ljt_lua_pcall(L, 0, 0, "automatic worker reclamation");
  lua_close(L);
  puts("automatic collection reclaimed detached workers with a live sentinel");
  return 0;
}
