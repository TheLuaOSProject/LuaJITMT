/*
** Focused test for x64 trace-side GC2 hard-threshold checks.
*/

#include <assert.h>
#include <stdio.h>

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
  uint64_t jit_checks0, assist_runs0;
  uint8_t classic_state0;

  lj_gc_threshold_store(g, LJ_MAX_MEM);
  la_store64_rel(&g->gc2.hard_bytes, 1);
  lj_gc2_hard_check_store(g, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  la_store64_rel(&g->gc2.alloc_since_trigger, 2);
  classic_state0 = g->gc.state;
  jit_checks0 = gc2_jit_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);

  ljt_lua_dostring(L, src);

  if (gc2_jit_hard_checks_acq(g) <= jit_checks0) {
    fprintf(stderr, "%s did not enter the x64 GC2 hard check\n", name);
    assert(0);
  }
  if (gc2_assist_runs_acq(g) <= assist_runs0) {
    fprintf(stderr, "%s did not run the GC2 hard assist\n", name);
    assert(0);
  }
  if (g->gc.state != classic_state0) {
    fprintf(stderr, "%s moved classic GC state %u -> %u\n",
	    name, (unsigned)classic_state0, (unsigned)g->gc.state);
    assert(0);
  }
}

static void run_idle_stopped_hard_check_trace(lua_State *L, global_State *g)
{
  uint64_t jit_checks0, assist_runs0, cycle_requests0, cycle_starts0;
  uint64_t major_roots0, minor_roots0;
  uint8_t classic_state0;

  lj_gc2_cycle_to_idle(g);
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  la_store64_rel(&g->gc2.hard_bytes, 1);
  lj_gc2_hard_check_store(g, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  la_store64_rel(&g->gc2.alloc_since_trigger, 2);
  classic_state0 = g->gc.state;
  jit_checks0 = gc2_jit_hard_checks_acq(g);
  assist_runs0 = gc2_assist_runs_acq(g);
  cycle_requests0 = gc2_cycle_requests_acq(g);
  cycle_starts0 = gc2_cycle_starts_acq(g);
  major_roots0 = gc2_major_root_scans_acq(g);
  minor_roots0 = gc2_minor_root_scans_acq(g);

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local x\n"
    "local i = 1\n"
    "while i <= 200 do x = {}; i = i + 1 end\n"
    "assert(type(x) == 'table')\n");

  if (gc2_jit_hard_checks_acq(g) <= jit_checks0) {
    fputs("IDLE_STOPPED did not enter the x64 GC2 hard check\n", stderr);
    assert(0);
  }
  assert(gc2_assist_runs_acq(g) == assist_runs0);
  assert(gc2_cycle_requests_acq(g) == cycle_requests0);
  assert(gc2_cycle_starts_acq(g) == cycle_starts0);
  assert(gc2_major_root_scans_acq(g) == major_roots0);
  assert(gc2_minor_root_scans_acq(g) == minor_roots0);
  assert(g->gc.state == classic_state0);
  lj_gc_threshold_store(g, g->gc.total + 4u * LJ_GC2_ACCT_FLUSH);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  GCtab *parent;

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

  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);

  run_hard_alloc_trace(L, g, "TNEW",
    "jit.flush()\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local x\n"
    "local i = 1\n"
    "while i <= 200 do x = {}; i = i + 1 end\n"
    "assert(type(x) == 'table')\n");

  run_hard_alloc_trace(L, g, "CNEW",
    "jit.flush()\n"
    "local ffi = require('ffi')\n"
    "ffi.cdef('typedef struct { int x; } lj_gc2_hard_cnew_t;')\n"
    "local ct = ffi.typeof('lj_gc2_hard_cnew_t')\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local x\n"
    "local i = 1\n"
    "while i <= 200 do x = ct(i); i = i + 1 end\n"
    "assert(x.x == 200)\n");

  run_hard_alloc_trace(L, g, "CELL_CNEW",
    "jit.flush()\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local keep\n"
    "local i = 1\n"
    "while i <= 200 do\n"
    "  local function f() return f end\n"
    "  keep = f\n"
    "  i = i + 1\n"
    "end\n"
    "assert(type(keep) == 'function' and keep() == keep)\n");

  run_hard_alloc_trace(L, g, "SNEW",
    "jit.flush()\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local s = 'abcdef'\n"
    "local x\n"
    "local i = 1\n"
    "while i <= 200 do x = string.sub(s, 1, 3); i = i + 1 end\n"
    "assert(x == 'abc')\n");

  lj_gc_threshold_store(g, g->gc.total + 4u * LJ_GC2_ACCT_FLUSH);
  lj_gc2_cycle_to_idle(g);
  run_idle_stopped_hard_check_trace(L, g);
  lj_gc2_cycle_to_idle(g);
  lua_close(L);
  puts("t-gc2-jit-hard-check OK: idle-stopped hard check is passive and "
       "TNEW/CNEW/CELL_CNEW/SNEW x64 GC checks enter GC2 hard assist");
  return 0;
}
