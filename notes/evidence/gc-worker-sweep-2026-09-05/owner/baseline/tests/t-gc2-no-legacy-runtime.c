/*
** GC2-only runtime and shutdown regression.
**
** The old color marker and sweeper entry points are physically absent. This
** ordinary-build workload keeps broad GC/JIT/threading coverage around that
** invariant, including lua_close() from both IDLE and an open active SWEEP.
*/

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_jit.h"
#include "lj_tg.h"
#include "lj_trace.h"

static const char gc2_only_workload[] =
  "local keep = {}\n"
  "local weak = setmetatable({}, { __mode = 'kv' })\n"
  "if jit then jit.opt.start('hotloop=1', 'hotexit=1') end\n"
  "for round = 1, 12 do\n"
  "  for i = 1, 2500 do\n"
  "    local payload = { round, i, tostring(i), child = { i + 1 } }\n"
  "    payload.call = function(x) return payload[2] + x end\n"
  "    weak[payload] = payload.call\n"
  "    keep[(i % 96) + 1] = payload\n"
  "  end\n"
  "  collectgarbage('collect')\n"
  "end\n"
  "for i = 1, #keep do assert(keep[i].call(1) == keep[i][2] + 1) end\n"
  "if jit then jit.flush() end\n"
  "collectgarbage('collect')\n"
  "local threading = require('threading')\n"
  "assert(type(threading.gcworkers(2)) == 'number')\n"
  "local close_worker = assert(threading.spawn(function() return true end))\n"
  "local joined, result = close_worker:join(10)\n"
  "assert(joined == true and result == true)\n"
  "keep.close_worker = close_worker\n"
  "if jit then\n"
  "  jit.opt.start('hotloop=1', 'hotexit=1')\n"
  "  local function close_trace(n)\n"
  "    local s = 0; for i = 1, n do s = s + i end; return s\n"
  "  end\n"
  "  for _ = 1, 80 do assert(close_trace(100) == 5050) end\n"
  "  keep.close_trace = close_trace\n"
  "  local util = require('jit.util')\n"
  "  local sticky_trace\n"
  "  for tr = 1, 128 do\n"
  "    if util.traceinfo(tr) then sticky_trace = tr; break end\n"
  "  end\n"
  "  assert(sticky_trace, 'close workload did not record a trace')\n"
  "  collectgarbage('collect')\n"
  "  assert(util.traceinfo(sticky_trace), 'live trace retired by GC2')\n"
  "  assert(close_trace(100) == 5050)\n"
  "end\n"
  "collectgarbage('collect')\n"
  "local joined_again, result_again = close_worker:join(0)\n"
  "assert(joined_again == true and result_again == true)\n"
  "assert(close_worker:running() == false)\n";

static void run_chunk(lua_State *L, const char *src)
{
  int status = luaL_loadstring(L, src);
  if (status == 0)
    status = lua_pcall(L, 0, 0, 0);
  if (status != 0) {
    fprintf(stderr, "GC2-only workload failed: %s\n", lua_tostring(L, -1));
    abort();
  }
}

static int trace_is_runnable(global_State *g, TraceNo traceno)
{
  GCtrace *T = traceref_safe(G2J(g), traceno);
  return trace_runnable_acq(T, traceno) && trace_mcode_acq(T) != NULL;
}

static int main_tg_has_traversable_sweep_work(global_State *g)
{
  TGState *tg = G2TG(g);
  return tg != NULL &&
    (tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL ||
     tg->alloc.quarantine[LJ_ARENAK_TRAVERSABLE] != NULL);
}

static void drive_to_pending_open_sweep(lua_State *L, global_State *g)
{
  uint32_t i;

  (void)lua_gc(L, LUA_GCRESTART, 0);
  for (i = 0; i < 1000000u; i++) {
    (void)lua_gc(L, LUA_GCSTEP, 1);
    if (gc2_phase_acq(g) == LJ_GC2_SWEEP &&
	gc2_sweep_bridge_ready_acq(g) != 0 &&
	gc2_jit_phase_gate_acq(g) != 0 &&
	main_tg_has_traversable_sweep_work(g))
      return;
  }
  fputs("GC2 did not reach an open SWEEP with physical work pending\n",
	stderr);
  assert(0);
}

static void run_active_sweep_close(void)
{
  static const char active_sweep_close_workload[] =
    "collectgarbage('stop')\n"
    "assert(require('threading').gcworkers(0) == 0)\n"
    "jit.flush()\n"
    "jit.on()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "function __gc2_active_close_trace(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 40 do\n"
    "  assert(__gc2_active_close_trace(100) == 5050)\n"
    "end\n"
    "local util = require('jit.util')\n"
    "for tr = 1, 128 do\n"
    "  if util.traceinfo(tr) then\n"
    "    __gc2_active_close_traceno = tr\n"
    "    break\n"
    "  end\n"
    "end\n"
    "assert(__gc2_active_close_traceno, 'active-close loop did not trace')\n"
    /* Leave substantially more than one bounded SWEEP batch of unreachable
    ** arena work, so the first coherent open bridge cannot also finish
    ** physical reclamation. */
    "for round = 1, 40 do\n"
    "  local dead = {}\n"
    "  for i = 1, 2500 do\n"
    "    dead[i] = { round, i, 'active-close-dead-' .. i }\n"
    "  end\n"
    "end\n";
  lua_State *L = luaL_newstate();
  global_State *g;
  TraceNo traceno;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  assert(g != NULL);

  run_chunk(L, active_sweep_close_workload);
  assert(gc2_n_workers_acq(g) == 0);
  lua_getglobal(L, "__gc2_active_close_traceno");
  assert(lua_isnumber(L, -1));
  traceno = (TraceNo)lua_tointeger(L, -1);
  lua_pop(L, 1);
  assert(traceno > 0);
  assert(trace_is_runnable(g, traceno));

  drive_to_pending_open_sweep(L, g);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  assert(gc2_sweep_bridge_ready_acq(g) != 0);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(main_tg_has_traversable_sweep_work(g));
  assert(trace_is_runnable(g, traceno));

  lua_close(L);
}

int main(void)
{
  lua_State *L;
  global_State *g;
  uint64_t cycle0, cycle1;

  L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  cycle0 = gc2_cycle_acq(g);

  run_chunk(L, gc2_only_workload);
  cycle1 = gc2_cycle_acq(g);
  assert(cycle1 > cycle0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_n_workers_acq(g) == 2);
  assert(gc2_worker_thread_acq(g, 0) != NULL);
  assert(gc2_worker_thread_acq(g, 1) != NULL);

  lua_close(L);

  run_active_sweep_close();

  printf("t-gc2-no-legacy-runtime OK; GC2 cycles=%" PRIu64
	 ", IDLE and active-SWEEP lua_close completed\n",
	 cycle1 - cycle0);
  return 0;
}
