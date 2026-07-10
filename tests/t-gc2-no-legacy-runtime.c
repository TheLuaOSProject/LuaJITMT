/*
** GC2-only runtime and shutdown regression.
**
** The old color marker and sweeper entry points are physically absent. This
** ordinary-build workload keeps broad GC/JIT/threading coverage around that
** invariant, including lua_close().
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

  printf("t-gc2-no-legacy-runtime OK; GC2 cycles=%" PRIu64
	 ", workers and sticky trace survived, lua_close completed\n",
	 cycle1 - cycle0);
  return 0;
}
