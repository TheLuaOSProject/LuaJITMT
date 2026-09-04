#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_tab.h"
static int snapshot(lua_State *L)
{
  global_State *g = G(L);
  unsigned asize = 0, hmask = 0;
  int i = (int)lua_tointeger(L, 1);
  if (lua_istable(L, 2)) {
    GCtab *t = tabV(L->base + 1);
    asize = lj_tab_asize_acq(t);
    hmask = lj_tab_hmask_acq(t);
  }
  fprintf(stderr, "%d,cpu=%.6f,phase=%u,cycle=%u,grey=%" PRIu64 ",ssb_pub=%" PRIu64 ",ssb_drain=%" PRIu64 ",assist=%" PRIu64 ",assist_grey=%" PRIu64 ",assist_ssb=%" PRIu64 ",marks=%" PRIu64 ",recovery=%" PRIu64 ",asize=%u,hmask=%u\n",
          i, (double)clock()/CLOCKS_PER_SEC, gc2_phase_acq(g), gc2_cycle_acq(g),
          gc2_grey_top_acq(g)-gc2_grey_bottom_acq(g),
          gc2_ssb_items_published_acq(g),gc2_ssb_items_drained_acq(g),
          gc2_assist_runs_acq(g),gc2_assist_grey_drained_acq(g),
          gc2_assist_ssb_converted_acq(g),gc2_marks_this_round_acq(g),
          gc2_recovery_items_acq(g),asize,hmask);
  return 0;
}
int luaopen_gcprobe(lua_State *L) { lua_pushcfunction(L,snapshot); return 1; }
