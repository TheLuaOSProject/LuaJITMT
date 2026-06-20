/*
** Focused guard for JIT mcode SMR retirement after trace flush.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_mcode.h"
#include "lj_trace.h"

#include "lib/lua_fixture_helpers.h"

static MCodeRetire *retired_find(jit_State *J, MCode *needle)
{
  MCodeRetire *ret;
  for (ret = J->retiredmcode; ret != NULL; ret = ret->next)
    if (ret->area == needle)
      return ret;
  return NULL;
}

int main(void)
{
  lua_State *L = ljt_lua_newstate_openlibs();
  global_State *g;
  jit_State *J;
  MCode *oldmc;
  MCodeRetire *ret;
  size_t szall;
  uint64_t epoch;

  g = G(L);
  J = G2J(g);
  assert(J->retiredmcode == NULL);

  ljt_lua_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local function f(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do s = s + i end\n"
    "  return s\n"
    "end\n"
    "for _ = 1, 40 do assert(f(120) == 7260) end\n");

  oldmc = J->mcarea;
  szall = J->szallmcarea;
  assert(oldmc != NULL);
  assert(szall != 0);
  assert(J->retiredmcode == NULL);

  assert(lj_trace_flushall(L) == 0);
  assert(J->mcarea == NULL);
  assert(J->mctop == NULL);
  assert(J->mcbot == NULL);
  assert(J->szmcarea == 0);
  assert(J->szallmcarea == szall);
  ret = retired_find(J, oldmc);
  assert(ret != NULL);

  epoch = ret->retire_epoch;
  assert(epoch == g->gc2.hs_epoch);
  assert(lj_mcode_reclaim_retired(g, epoch) == 0);
  ret = retired_find(J, oldmc);
  assert(ret != NULL);
  assert(J->szallmcarea == szall);
  assert(lj_mcode_reclaim_retired(g, epoch + 1u) == 0);
  assert(retired_find(J, oldmc) != NULL);
  assert(lj_mcode_reclaim_retired(g, epoch + LJ_FLUSH_EPOCHS) >= 1);
  assert(J->retiredmcode == NULL);
  assert(J->szallmcarea == 0);

  lua_close(L);
  printf("t-jit-mcode-retire OK: mcode flush retires by epoch\n");
  return 0;
}
