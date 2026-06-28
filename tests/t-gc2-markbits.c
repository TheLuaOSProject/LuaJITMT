/*
** Focused test for GC2 arena and HugeTab mark-bit helpers.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_tg.h"

static uint32_t ptr_state(void *p)
{
  GCArena *a = lj_arena_of(p);
  uint32_t cell = lj_arena_cellof(p);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  return lj_arena_state(a, cell);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *tab;
  void *trav, *plain, *huge;
  const size_t trav_size = 64;
  const size_t plain_size = 96;
  const size_t huge_size = LJ_HUGE_THRESHOLD + 113u;
  LJHugeInfo hi;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  assert((tg->tg_flags & TGF_HUGETAB) != 0);

  trav = lj_arena_alloc(&tg->alloc, &tg->prng, trav_size, LJ_AF_TRAVERSABLE);
  plain = lj_arena_alloc(&tg->alloc, &tg->prng, plain_size, 0);
  huge = lj_arena_allocd_alloc(&tg->allocd, huge_size, LJ_AF_TRAVERSABLE);
  assert(trav != NULL);
  assert(plain != NULL);
  assert(huge != NULL);
  assert(ptr_state(trav) == 2);
  assert(ptr_state(plain) == 2);
  assert(lj_arena_hugetab_lookup(&tg->huge, huge, &hi) == 1);
  assert(hi.size == huge_size);
  assert(hi.flags == LJ_HUGEF_TRAVERSABLE);

  lua_newtable(L);
  tab = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(la_load64_acq(&g->gc2.marks_this_round) == 0);
  assert(lj_gc2_markobj(g, NULL) == 0);
  assert(lj_gc2_test_ssb_empty(g));

  assert(lj_gc2_markmem(g, trav) == 1);
  assert(ptr_state(trav) == 3);
  assert(lj_gc2_test_ssb_empty(g));
  assert(lj_gc2_markmem(g, trav) == 0);
  assert(la_load64_acq(&g->gc2.marks_this_round) == 1);

  assert(lj_gc2_markmem(g, plain) == 1);
  assert(ptr_state(plain) == 3);
  assert(lj_gc2_test_ssb_empty(g));
  assert(lj_gc2_markmem(g, plain) == 0);
  assert(la_load64_acq(&g->gc2.marks_this_round) == 2);

  assert(lj_gc2_markmem(g, huge) == 1);
  assert(lj_arena_hugetab_lookup(&tg->huge, huge, &hi) == 1);
  assert(hi.flags == (LJ_HUGEF_TRAVERSABLE|LJ_HUGEF_MARK));
  assert(lj_gc2_test_ssb_empty(g));
  assert(lj_gc2_markmem(g, huge) == 0);
  assert(la_load64_acq(&g->gc2.marks_this_round) == 3);

  assert(lj_gc2_ismarked(g, obj2gco(tab)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(tab)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(tab)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  assert(lj_gc2_markobj(g, obj2gco(tab)) == 0);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(lj_gc2_test_ssb_drain(g) == 1);
  assert(lj_gc2_test_ssb_empty(g));
  assert(la_load64_acq(&g->gc2.marks_this_round) == 4);

  lj_gc2_cycle_to_idle(g);
  lj_arena_free(&tg->alloc, trav, trav_size);
  lj_arena_free(&tg->alloc, plain, plain_size);
  lj_arena_allocf(&tg->allocd, huge, huge_size, 0);
  lua_pop(L, 1);
  lua_close(L);

  printf("t-gc2-markbits OK: mark helpers are idempotent\n");
  return 0;
}
