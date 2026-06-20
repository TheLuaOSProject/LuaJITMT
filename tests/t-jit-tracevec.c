/*
** Focused guard for JIT trace-vector RCU growth and SMR retirement.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

static int retired_has(jit_State *J, TraceVec *needle)
{
  TraceVec *tv;
  for (tv = J->retiredtracev; tv != NULL; tv = tracevec_retired_next_acq(tv))
    if (tv == needle)
      return 1;
  return 0;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g;
  jit_State *J;
  TraceVec *oldtv, *newtv;
  uint64_t epoch;

  g = G(L);
  J = G2J(g);
  assert(J->tracev == NULL);
  assert(J->retiredtracev == NULL);

  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "local function tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 64 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(tracecount, true)\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1', 'maxtrace=2')\n"
    "local function make(k)\n"
    "  return function(n) local s = 0 for i = 1, n do s = s + i + k end return s end\n"
    "end\n"
    "local f1, f2 = make(1), make(2)\n"
    "for _ = 1, 20 do f1(80); f2(80) end\n"
    "assert(tracecount() >= 1, tracecount())\n");

  oldtv = tracevec_acq(J);
  assert(oldtv != NULL);
  assert(oldtv->sizetrace == 3);
  assert(trace_sizetrace_acq(J) == oldtv->sizetrace);
  assert(J->retiredtracev == NULL);

  ljt_lua_dostring(L,
    "local util = require'jit.util'\n"
    "local function tracecount()\n"
    "  local n = 0\n"
    "  for i = 1, 64 do if util.traceinfo(i) then n = n + 1 end end\n"
    "  return n\n"
    "end\n"
    "jit.off(tracecount, true)\n"
    "jit.opt.start('maxtrace=20')\n"
    "local function make(k)\n"
    "  return function(n) local s = 0 for i = 1, n do s = s + i + k end return s end\n"
    "end\n"
    "for k = 3, 12 do local f = make(k); for _ = 1, 20 do f(80) end end\n"
    "assert(tracecount() > 2, tracecount())\n");

  newtv = tracevec_acq(J);
  assert(newtv != NULL);
  assert(newtv != oldtv);
  assert(newtv->sizetrace == 21);
  assert(trace_sizetrace_acq(J) == newtv->sizetrace);
  assert(retired_has(J, oldtv));

  epoch = oldtv->retire_epoch;
  assert(epoch == g->gc2.hs_epoch);
  assert(lj_trace_reclaim_retired(g, epoch) == 0);
  assert(retired_has(J, oldtv));
  assert(lj_trace_reclaim_retired(g, epoch + 1u) >= 1);
  assert(J->retiredtracev == NULL);

  lua_close(L);
  printf("t-jit-tracevec OK: trace vector grows by RCU and retires by epoch\n");
  return 0;
}
