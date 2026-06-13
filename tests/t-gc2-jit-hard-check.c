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

static void run_lua(lua_State *L, const char *src)
{
  if (luaL_loadstring(L, src) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
    fputs(lua_tostring(L, -1), stderr);
    fputc('\n', stderr);
    assert(0);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  uint64_t jit_checks0, assist_runs0;
  uint8_t legacy_state0;
  GCtab *parent;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  assert(g != NULL);
  assert(la_load64_acq(&g->gc2.jit_hard_checks) == 0);

  run_lua(L,
    "assert(jit and jit.status())\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local x\n"
    "for i=1,50 do x = {} end\n"
    "assert(type(x) == 'table')\n");

  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);

  lj_gc_threshold_store(g, LJ_MAX_MEM);
  la_store64_rel(&g->gc2.hard_bytes, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  la_store64_rel(&g->gc2.alloc_since_trigger, 2);
  legacy_state0 = g->gc.state;
  jit_checks0 = la_load64_acq(&g->gc2.jit_hard_checks);
  assist_runs0 = la_load64_acq(&g->gc2.assist_runs);

  run_lua(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1','hotexit=1','-sink')\n"
    "local x\n"
    "for i=1,200 do x = {} end\n"
    "assert(type(x) == 'table')\n");

  assert(la_load64_acq(&g->gc2.jit_hard_checks) > jit_checks0);
  assert(la_load64_acq(&g->gc2.assist_runs) > assist_runs0);
  assert(g->gc.state == legacy_state0);

  lj_gc_threshold_store(g, g->gc.total + 4u * LJ_GC2_ACCT_FLUSH);
  lj_gc2_legacy_cycle_end(g);
  lua_close(L);
  puts("t-gc2-jit-hard-check OK: x64 trace GC checks enter GC2 hard assist");
  return 0;
}
