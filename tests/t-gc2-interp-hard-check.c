/*
** Focused test for x64 interpreter GC2 hard-threshold checks.
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

static void load_lua(lua_State *L, const char *src)
{
  if (luaL_loadstring(L, src) != 0) {
    fputs(lua_tostring(L, -1), stderr);
    fputc('\n', stderr);
    assert(0);
  }
}

static void call_lua(lua_State *L)
{
  if (lua_pcall(L, 0, 0, 0) != 0) {
    fputs(lua_tostring(L, -1), stderr);
    fputc('\n', stderr);
    assert(0);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  uint64_t interp_checks0, assist_runs0;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  assert(g != NULL);
  assert(la_load64_acq(&g->gc2.interp_hard_checks) == 0);

  run_lua(L, "if jit then jit.off() end\n");
  load_lua(L,
    "local x = {}\n"
    "assert(type(x) == 'table')\n");

  lj_gc_threshold_store(g, LJ_MAX_MEM);
  lj_gc2_legacy_mark_begin(g);
  la_store64_rel(&g->gc2.hard_bytes, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  la_store64_rel(&g->gc2.alloc_since_trigger, 2);
  interp_checks0 = la_load64_acq(&g->gc2.interp_hard_checks);
  assist_runs0 = la_load64_acq(&g->gc2.assist_runs);

  lj_gc_threshold_store(g, g->gc.total);
  call_lua(L);

  if (la_load64_acq(&g->gc2.interp_hard_checks) <= interp_checks0) {
    fputs("interpreted TNEW did not enter the GC2 hard check\n", stderr);
    assert(0);
  }
  if (la_load64_acq(&g->gc2.assist_runs) <= assist_runs0) {
    fputs("interpreted TNEW did not run the GC2 hard assist\n", stderr);
    assert(0);
  }

  lj_gc2_legacy_cycle_end(g);
  g->gc.state = GCSpause;
  lj_gc_threshold_store(g, g->gc.total + 4u * LJ_GC2_ACCT_FLUSH);
  lua_close(L);
  puts("t-gc2-interp-hard-check OK: interpreted TNEW enters GC2 hard assist");
  return 0;
}
