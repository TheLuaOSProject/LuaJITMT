/*
** Focused test for the GC2 legacy phase scaffold.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tg.h"

static int arena_list_contains(GCArena *a, GCArena *needle)
{
  while (a) {
    if (a == needle)
      return 1;
    a = a->hdr.next;
  }
  return 0;
}

static void assert_idle(global_State *g, TGState *tg)
{
  assert(g->gc2.phase == LJ_GC2_IDLE);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);
}

static int finalizer_churn(lua_State *L)
{
  int status = luaL_dostring(L,
    "local hold = {}\n"
    "for i = 1, 96 do\n"
    "  local t = {i, i+1, i+2}\n"
    "  for j = 4, 48 do t[j] = j end\n"
    "  hold[i] = t\n"
    "end\n");
  if (status != LUA_OK)
    lua_pop(L, 1);
  return 0;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *phase_tab;
  uint32_t cycle0;
  uint32_t ssb_published0;
  void *phase_plain, *phase_trav;
  GCArena *phase_plain_a, *phase_trav_a;
  int i, done = 0, saw_mark = 0, saw_sweep = 0;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert(g->gc2.tg_list == tg);
  assert(g->gc2.n_threads == 1);
  assert_idle(g, tg);

  lua_newtable(L);
  phase_tab = tabV(L->top - 1);
  cycle0 = g->gc2.cycle;
  lj_gc2_legacy_mark_begin(g);
  assert(g->gc2.phase == LJ_GC2_MARK);
  assert(g->gc2.cycle == cycle0 + 1u);
  assert(g->gc2.marks_this_round == 0);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);
  assert(lj_gc2_ismarked(g, obj2gco(phase_tab)) == 0);
  assert(lj_gc2_ssb_push(g, obj2gco(phase_tab)) == 1);
  ssb_published0 = g->gc2.ssb_published;
  phase_plain = lj_arena_alloc(&tg->alloc, &tg->prng, 64, 0);
  phase_trav = lj_arena_alloc(&tg->alloc, &tg->prng, 64,
			      LJ_AF_TRAVERSABLE);
  assert(phase_plain != NULL);
  assert(phase_trav != NULL);
  phase_plain_a = lj_arena_of(phase_plain);
  phase_trav_a = lj_arena_of(phase_trav);
  lj_gc2_legacy_sweep_begin(g);
  assert(g->gc2.phase == LJ_GC2_SWEEP);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 1);
  assert(g->gc2.ssb_published == ssb_published0 + 1u);
  assert(tg->ssb_next == tg->ssb_base);
  assert(lj_gc2_drain_ssb(g) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(phase_tab)) == 1);
  assert(tg->alloc.bump[LJ_ARENAK_PLAIN].a == NULL);
  assert(tg->alloc.bump[LJ_ARENAK_TRAVERSABLE].a == NULL);
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_PLAIN],
			     phase_plain_a));
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     phase_trav_a));
  assert((phase_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
  assert((phase_trav_a->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
  lj_arena_alloc_restore_sweep_kind(&tg->alloc, LJ_ARENAK_TRAVERSABLE);
  lj_arena_alloc_restore_sweep_kind(&tg->alloc, LJ_ARENAK_PLAIN);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  lj_gc2_legacy_cycle_end(g);
  assert_idle(g, tg);
  lua_pop(L, 1);
  lj_arena_free(&tg->alloc, phase_plain, 64);
  lj_arena_free(&tg->alloc, phase_trav, 64);
  lj_gc2_legacy_mark_begin(g);
  assert(g->gc2.phase == LJ_GC2_MARK);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);
  lj_gc2_legacy_preserve_abort(g);
  assert_idle(g, tg);

  assert(luaL_dostring(L,
    "hold = {}\n"
    "for i = 1, 2600 do\n"
    "  local t = {}\n"
    "  for j = 1, 18 do t[j] = i + j end\n"
    "  hold[i] = t\n"
    "end\n") == LUA_OK);
  lj_gc_fullgc(L);
  assert_idle(g, tg);

  g->gc.stepmul = 1;
  g->gc.threshold = 0;
  assert(lj_gc_step(L) <= 0);
  assert(g->gc2.phase != LJ_GC2_IDLE);
  lj_gc_fullgc(L);
  assert_idle(g, tg);

  g->gc.stepmul = 1;
  g->gc.threshold = 0;
  for (i = 0; i < 100000; i++) {
    int rc = lj_gc_step(L);
    if (g->gc2.phase == LJ_GC2_MARK)
      saw_mark = 1;
    if (g->gc2.phase == LJ_GC2_SWEEP)
      saw_sweep = 1;
    if (rc > 0) {
      done = 1;
      break;
    }
  }
  assert(done);
  assert(saw_mark);
  assert(saw_sweep);
  assert_idle(g, tg);

  lua_pushnil(L);
  lua_setglobal(L, "hold");
  lj_gc_fullgc(L);
  assert_idle(g, tg);

  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, finalizer_churn);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
  lj_gc_fullgc(L);
  assert_idle(g, tg);

  lua_close(L);
  printf("t-gc2-phase OK: legacy GC2 phases and mirrors verified\n");
  return 0;
}
