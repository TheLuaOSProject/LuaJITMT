#include <assert.h>
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_func.h"
#include "lj_tg.h"

static void fill(TGState *tg, GCobj *o)
{
  GCRef *next = lj_tg_ssb_next_acq(tg), *end = lj_tg_ssb_end_acq(tg);
  while (next < end) {
    setgcrefrel(next[0], o);
    lj_tg_ssb_next_rel(tg, ++next);
  }
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCfunc *fn;
  GCproto *pt;
  GCtab *env;
  GC2SSBNode *oldactive, *oldpublished;
  GCobj *published_first;
  uint32_t oldcycle, published_count;
  assert(L);
  luaL_openlibs(L);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  assert(luaL_loadstring(L, "return 1") == 0);
  g = G(L); tg = L2TG(L);
  fn = funcV(L->top-1); pt = funcproto(fn);
  env = lj_funcL_env_acq(&fn->l);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  if (lj_tg_ssb_free_acq(tg) != NULL) {
    fill(tg, obj2gco(pt));
    assert(lj_gc2_flush_ssb(g, tg) == TG_GC2_SSB_SLOTS);
  }
  oldpublished = gc2_ssb_head_acq(g);
  assert(oldpublished && lj_gc2_ssb_owner_acq(oldpublished) == tg);
  published_count = lj_gc2_ssb_count_acq(oldpublished);
  published_first = gcref_acq(oldpublished->slot[0]);
  assert(published_count > 0 && published_first != NULL);
  assert(lj_tg_ssb_free_acq(tg) == NULL);
  fill(tg, obj2gco(env));
  oldactive = lj_tg_ssb_active_acq(tg);
  oldcycle = gc2_cycle_acq(g);
  assert(oldactive != oldpublished);
  assert(!lj_gc2_fnew_certify_pair_nodrain(g, tg, pt, env));
  lj_gc2_preserve_abort_to_idle(g);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lj_tg_ssb_active_acq(tg) == oldactive);
  assert(lj_tg_ssb_next_acq(tg) == lj_tg_ssb_end_acq(tg));
  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(gc2_cycle_acq(g) != oldcycle);
  assert(gc2_jit_phase_gate_acq(g) != 0);
  assert(lj_tg_ssb_active_acq(tg) == oldactive);
  assert(lj_tg_ssb_next_acq(tg) == lj_tg_ssb_end_acq(tg));
  assert(lj_gc2_ssb_count_acq(oldpublished) == published_count);
  assert(gcref_acq(oldpublished->slot[0]) == published_first);
  assert(gcref_acq(oldactive->slot[0]) == obj2gco(env));
  assert(!lj_gc2_fnew_certify_pair_nodrain(g, tg, pt, env));
  puts("open MARK validly retained full active SSB and published predecessor");
  /* Outside the cycle-start worker token, the normal mutator flush may recycle
  ** a published node. This is an explicit capacity operation, not an abort or
  ** next-cycle guarantee, and it must preserve the full active node. */
  assert(lj_gc2_flush_ssb(g, tg) == TG_GC2_SSB_SLOTS);
  assert(lj_tg_ssb_active_acq(tg) != oldactive);
  assert(lj_tg_ssb_next_acq(tg) == lj_tg_ssb_base_acq(tg));
  assert(lj_gc2_ssb_count_acq(oldactive) == TG_GC2_SSB_SLOTS);
  assert(gcref_acq(oldactive->slot[0]) == obj2gco(env));
  assert(lj_gc2_fnew_certify_pair_nodrain(g, tg, pt, env));
  assert(lj_tg_ssb_next_acq(tg) == lj_tg_ssb_base_acq(tg) + 2);
  puts("ordinary owner flush preserved full node and restored pair capacity");
  lj_gc2_preserve_abort_to_idle(g);
  lua_close(L);
  return 0;
}
