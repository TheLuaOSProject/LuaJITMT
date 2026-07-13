/*
** Focused test for x64 trace-side GC2 hard-threshold checks.
*/

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_gc2.h"

#include "lib/lua_fixture_helpers.h"

static void run_hard_alloc_trace(lua_State *L, global_State *g,
				 const char *name, const char *src)
{
  uint64_t jit_checks0, assist_runs0, hard_check0;
  uint64_t cycle_requests0, cycle_starts0;

  lua_settop(L, 0);
  lj_gc2_cycle_to_idle(g);
  g->gc.state = GCSpause;
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  la_store64_rel(&g->gc2.hard_bytes, LJ_MAX_MEM);
  lj_gc2_hard_check_store(g, LJ_MAX_MEM);
  la_store64_rel(&g->gc2.alloc_since_trigger, 0);

  /* Compile the allocation loop while JLOOP entry is open. GC2 mark start
  ** deliberately closes trace entry until the cycle returns to IDLE, so
  ** starting MARK before recording does not exercise a trace allocation
  ** check. */
  ljt_lua_dostring(L, src);
  assert(lua_isfunction(L, -1));

  la_store64_rel(&g->gc2.hard_bytes, LJ_GC2_ACCT_FLUSH);
  lj_gc2_hard_check_store(g, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  la_store64_rel(&g->gc2.alloc_since_trigger, LJ_GC2_ACCT_FLUSH + 1u);
  jit_checks0 = gc2_jit_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);
  hard_check0 = lj_gc2_hard_check_load(g);
  cycle_requests0 = gc2_cycle_requests_acq(g);
  cycle_starts0 = gc2_cycle_starts_acq(g);

  lua_pushinteger(L, 200);
  ljt_lua_pcall(L, 1, 1, name);
  lua_pop(L, 1);

  if (gc2_jit_hard_checks_acq(g) <= jit_checks0) {
    fprintf(stderr, "%s did not enter the x64 GC2 hard check\n", name);
    assert(0);
  }
  if (lj_gc2_hard_check_load(g) <= hard_check0) {
    fprintf(stderr, "%s did not advance the x64 GC2 hard-check cadence\n",
	    name);
    assert(0);
  }
  assert(gc2_assist_runs_acq(g) == assist_runs0);
  assert(gc2_cycle_requests_acq(g) == cycle_requests0);
  assert(gc2_cycle_starts_acq(g) == cycle_starts0);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
}

static void run_idle_stopped_hard_check_trace(lua_State *L, global_State *g)
{
  uint64_t jit_checks0, assist_runs0, cycle_requests0, cycle_starts0;
  uint64_t major_roots0, minor_roots0;

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local util = require('jit.util')\n"
    "function __gc2_idle_hard_run(n)\n"
    "  local x\n"
    "  local i = 1\n"
    "  while i <= n do x = {}; i = i + 1 end\n"
    "  return type(x)\n"
    "end\n"
    "assert(__gc2_idle_hard_run(200) == 'table')\n"
    "assert(util.traceinfo(1), 'IDLE_STOPPED loop did not trace')\n");
  lj_gc2_cycle_to_idle(g);
  g->gc.state = GCSpause;
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  la_store64_rel(&g->gc2.hard_bytes, 1);
  lj_gc2_hard_check_store(g, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  la_store64_rel(&g->gc2.alloc_since_trigger, 2);
  jit_checks0 = gc2_jit_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);
  cycle_requests0 = gc2_cycle_requests_acq(g);
  cycle_starts0 = gc2_cycle_starts_acq(g);
  major_roots0 = gc2_major_root_scans_acq(g);
  minor_roots0 = gc2_minor_root_scans_acq(g);

  lua_getglobal(L, "__gc2_idle_hard_run");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, 200);
  ljt_lua_pcall(L, 1, 1, "IDLE_STOPPED hard-check rerun");
  assert(lua_isstring(L, -1));
  assert(strcmp(lua_tostring(L, -1), "table") == 0);
  lua_pop(L, 1);

  if (gc2_jit_hard_checks_acq(g) <= jit_checks0) {
    fputs("IDLE_STOPPED did not enter the x64 GC2 hard check\n", stderr);
    assert(0);
  }
  assert(gc2_assist_runs_acq(g) == assist_runs0);
  assert(gc2_cycle_requests_acq(g) == cycle_requests0);
  assert(gc2_cycle_starts_acq(g) == cycle_starts0);
  assert(gc2_major_root_scans_acq(g) == major_roots0);
  assert(gc2_minor_root_scans_acq(g) == minor_roots0);
  lua_pushnil(L);
  lua_setglobal(L, "__gc2_idle_hard_run");
  lj_gc_threshold_store(g, g->gc.total + 4u * LJ_GC2_ACCT_FLUSH);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  assert(g != NULL);
  assert(gc2_jit_hard_checks_acq(g) == 0);

  ljt_lua_dostring(L,
    "assert(jit and jit.status())\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local x\n"
    "local i = 1\n"
    "while i <= 50 do x = {}; i = i + 1 end\n"
    "assert(type(x) == 'table')\n");

  run_hard_alloc_trace(L, g, "TNEW",
    "jit.flush()\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local util = require('jit.util')\n"
    "local function hard_tnew(n)\n"
    "  local x\n"
    "  local i = 1\n"
    "  while i <= n do x = {}; i = i + 1 end\n"
    "  return type(x)\n"
    "end\n"
    "assert(hard_tnew(200) == 'table')\n"
    "assert(util.traceinfo(1), 'TNEW hard-check loop did not trace')\n"
    "return hard_tnew\n");

  run_hard_alloc_trace(L, g, "CNEW",
    "jit.flush()\n"
    "local ffi = require('ffi')\n"
    "local util = require('jit.util')\n"
    "ffi.cdef('typedef struct { int x; } lj_gc2_hard_cnew_t;')\n"
    "local ct = ffi.typeof('lj_gc2_hard_cnew_t')\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local function hard_cnew(n)\n"
    "  local x\n"
    "  local i = 1\n"
    "  while i <= n do x = ct(i); i = i + 1 end\n"
    "  return x.x\n"
    "end\n"
    "assert(hard_cnew(200) == 200)\n"
    "assert(util.traceinfo(1), 'CNEW hard-check loop did not trace')\n"
    "return hard_cnew\n");

  run_hard_alloc_trace(L, g, "CELL_CNEW",
    "jit.flush()\n"
    "local util = require('jit.util')\n"
    "jit.opt.start('hotloop=1','hotexit=1')\n"
    "local function hard_cell_cnew(n)\n"
    "  local t = {}\n"
    "  for i = 1, n do\n"
    "    local x = i\n"
    "    t[i] = function()\n"
    "      x = x + 1\n"
    "      return x\n"
    "    end\n"
    "  end\n"
    "  return t\n"
    "end\n"
    "assert(type(hard_cell_cnew(200)) == 'table')\n"
    "assert(util.traceinfo(1), 'CELL_CNEW loop did not trace')\n"
    "return hard_cell_cnew\n"
    );

  run_hard_alloc_trace(L, g, "SNEW",
    "jit.flush()\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local util = require('jit.util')\n"
    "local s = 'abcdef'\n"
    "local function hard_snew(n)\n"
    "  local x\n"
    "  local i = 1\n"
    "  while i <= n do\n"
    "    local first = i % 3 + 1\n"
    "    x = string.sub(s, first, first + 2)\n"
    "    i = i + 1\n"
    "  end\n"
    "  return x\n"
    "end\n"
    "assert(#hard_snew(200) == 3)\n"
    "assert(util.traceinfo(1), 'SNEW hard-check loop did not trace')\n"
    "return hard_snew\n");

  lj_gc_threshold_store(g, g->gc.total + 4u * LJ_GC2_ACCT_FLUSH);
  lj_gc2_cycle_to_idle(g);
  run_idle_stopped_hard_check_trace(L, g);
  lj_gc2_cycle_to_idle(g);
  lua_close(L);
  puts("t-gc2-jit-hard-check OK: idle-stopped hard check is passive and "
       "TNEW/CNEW/CELL_CNEW/SNEW x64 GC checks advance hard cadence");
  return 0;
}
