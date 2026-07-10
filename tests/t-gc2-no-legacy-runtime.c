/*
** GC2-only runtime tripwire.
**
** The old color marker and sweeper must never run, including lua_close().
*/

#ifndef LJ_GC2_TEST_HELPERS
#error "t-gc2-no-legacy-runtime requires -DLJ_GC2_TEST_HELPERS"
#endif

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
  "end\n";

static void dump_stats(const char *where, const GCLegacyEntryStats *s)
{
  fprintf(stderr,
    "%s: runtime markobj=%" PRIu64 " markobj_deep=%" PRIu64
    " mark=%" PRIu64 " propagate=%" PRIu64 " propagatemark=%" PRIu64
    " sweep=%" PRIu64 " sweepstr=%" PRIu64
    "; shutdown markobj=%" PRIu64 " markobj_deep=%" PRIu64
    " mark=%" PRIu64 " propagate=%" PRIu64 " propagatemark=%" PRIu64
    " sweep=%" PRIu64 " sweepstr=%" PRIu64 "\n",
    where,
    s->runtime_markobj, s->runtime_markobj_deep,
    s->runtime_mark, s->runtime_propagate, s->runtime_propagatemark,
    s->runtime_sweep, s->runtime_sweepstr,
    s->shutdown_markobj, s->shutdown_markobj_deep,
    s->shutdown_mark, s->shutdown_propagate, s->shutdown_propagatemark,
    s->shutdown_sweep, s->shutdown_sweepstr);
}

static void require_zero_runtime(const char *where,
                                 const GCLegacyEntryStats *s)
{
  if (s->runtime_markobj != 0 || s->runtime_markobj_deep != 0 ||
      s->runtime_mark != 0 || s->runtime_propagate != 0 ||
      s->runtime_propagatemark != 0 ||
      s->runtime_sweep != 0 || s->runtime_sweepstr != 0) {
    dump_stats(where, s);
    fputs("live-state execution entered the retired color collector\n", stderr);
    abort();
  }
}

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
  GCLegacyEntryStats live, closed;
  lua_State *L;
  global_State *g;
  uint64_t cycle0;

  lj_gc_test_legacy_entries_reset();
  L = luaL_newstate();
  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  cycle0 = gc2_cycle_acq(g);

  run_chunk(L, gc2_only_workload);
  assert(gc2_cycle_acq(g) > cycle0);

  lj_gc_test_legacy_entries_snapshot(&live);
  require_zero_runtime("before lua_close", &live);
  if (live.shutdown_markobj != 0 || live.shutdown_markobj_deep != 0 ||
      live.shutdown_mark != 0 || live.shutdown_propagate != 0 ||
      live.shutdown_propagatemark != 0 ||
      live.shutdown_sweep != 0 || live.shutdown_sweepstr != 0) {
    dump_stats("before lua_close", &live);
    fputs("shutdown-classified legacy work ran before lua_close\n", stderr);
    abort();
  }

  lua_close(L);

  lj_gc_test_legacy_entries_snapshot(&closed);
  require_zero_runtime("after lua_close", &closed);
  if (closed.shutdown_markobj != 0 || closed.shutdown_markobj_deep != 0 ||
      closed.shutdown_mark != 0 || closed.shutdown_propagate != 0 ||
      closed.shutdown_propagatemark != 0) {
    dump_stats("after lua_close", &closed);
    fputs("lua_close entered the retired color marker\n", stderr);
    abort();
  }
  if (closed.shutdown_sweep != 0 || closed.shutdown_sweepstr != 0) {
    dump_stats("after lua_close", &closed);
    fputs("lua_close entered the retired color sweeper\n", stderr);
    abort();
  }

  puts("t-gc2-no-legacy-runtime OK; runtime and shutdown legacy entries=0");
  return 0;
}
