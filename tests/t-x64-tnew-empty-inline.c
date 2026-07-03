/*
** Focused guard for the x64 interpreter empty-table TNEW inline bump path.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_tg.h"

#include "lib/lua_fixture_helpers.h"

#ifndef LJ_TAB_TEST_HELPERS
#error "t-x64-tnew-empty-inline requires LJ_TAB_TEST_HELPERS"
#endif

#define TNEW_EMPTY_SIZE		((GCSize)sizeof(GCtab))
#define TNEW_EMPTY_NCELLS	((uint32_t)((sizeof(GCtab) + LJ_CELL_SIZE-1u) >> LJ_CELL_SHIFT))

typedef struct TestAllocCtx {
  lua_Alloc oldf;
  void *oldud;
  uint32_t calls;
} TestAllocCtx;

static void *counting_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  TestAllocCtx *ctx = (TestAllocCtx *)ud;
  if (!ptr && nsize == sizeof(GCtab))
    ctx->calls++;
  return ctx->oldf(ctx->oldud, ptr, osize, nsize);
}

static void load_empty_table_chunk(lua_State *L)
{
  ljt_lua_loadstring(L, "return {}\n");
}

static int root_chain_contains(global_State *g, GCobj *needle)
{
  GCobj *o;
  uint32_t n = 0;
  for (o = lj_gc_root_acq(g); o != NULL; o = lj_obj_gcw_acq(o)) {
    if (o == needle)
      return 1;
    if (++n > 1000000u)
      return 0;
  }
  return 0;
}

static void assert_empty_table_body(global_State *g, GCtab *t)
{
  assert(t != NULL);
  assert(t->gct == (uint8_t)~LJ_TTAB);
  assert((t->marked & LJ_GC_WHITES) == (g->gc.currentwhite & LJ_GC_WHITES));
  assert(t->nomm == (uint8_t)~0u);
  assert(t->colo == 0);
  assert(mref(t->array, TValue) == NULL);
  assert(gcref_acq(t->metatable) == NULL);
  assert(t->asize == 0);
  assert(t->hmask == 0);
  assert(t->acap == 0);
  assert(t->struct_owner == 0);
  assert(mref(t->node, Node) == &g->nilnode);
#if LJ_GC64
  assert(mref(t->freetop, Node) == &g->nilnode);
#endif
}

static void test_inline_empty_tnew(lua_State *L, global_State *g, TGState *tg)
{
  LJArenaBump *b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  GCArena *a0;
  GCobj *pending0;
  GCSize total0;
  uint64_t local0;
  uint32_t cell0, black, i;
  GCtab *t;

  load_empty_table_chunk(L);
  a0 = b->a;
  cell0 = b->cell;
  black = lj_arena_alloc_black_acq(&tg->alloc);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  total0 = lj_gc_total_load(g);
  local0 = lj_tg_local_total_acq(tg);

  assert(a0 != NULL);
  assert(cell0 + TNEW_EMPTY_NCELLS <= b->end);
  for (i = 4; i < LJ_ALLOC_NBINS; i++)
    assert(tg->alloc.bins[LJ_ARENAK_TRAVERSABLE][i] == NULL);
  assert(local0 < LJ_GC2_ACCT_FLUSH - TNEW_EMPTY_SIZE);

  ljt_lua_pcall(L, 0, 1, "empty TNEW inline pcall");
  t = tabV(L->top - 1);

  assert_empty_table_body(g, t);
  assert((void *)t == lj_arena_cellptr(a0, cell0));
  assert(b->cell == cell0 + TNEW_EMPTY_NCELLS);
  assert(lj_arena_state(a0, cell0) == (black ? 3u : 2u));
  for (i = 1; i < TNEW_EMPTY_NCELLS; i++)
    assert(lj_arena_state(a0, cell0 + i) == 0);
  assert(lj_gc_total_load(g) == total0 + TNEW_EMPTY_SIZE);
  assert(lj_tg_local_total_acq(tg) == local0 + TNEW_EMPTY_SIZE);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(t));
  assert(lj_obj_gcw_acq(obj2gco(t)) == pending0);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(root_chain_contains(g, obj2gco(t)));
  lua_pop(L, 1);
}

static void test_entering_uses_helper(lua_State *L, global_State *g,
				      TGState *tg)
{
  LJArenaBump *b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  GCobj *pending0;
  uint32_t cell0;
  uint32_t calls0;
  GCtab *t;

  load_empty_table_chunk(L);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  cell0 = b->cell;
  lj_tab_test_reset_new0_calls();
  calls0 = lj_tab_test_new0_calls();

  assert(mt_entering_add_rlx(g, 1) == 0);
  ljt_lua_pcall(L, 0, 1, "entering empty TNEW helper pcall");
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, 0x7fffffff);

  t = tabV(L->top - 1);
  assert(lj_tab_test_new0_calls() == calls0 + 1u);
  assert_empty_table_body(g, t);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(t));
  assert(lj_obj_gcw_acq(obj2gco(t)) == pending0);
  assert(b->cell >= cell0);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(root_chain_contains(g, obj2gco(t)));
  lua_pop(L, 1);
}

static void test_local_accounting_fallback(lua_State *L, global_State *g,
					   TGState *tg)
{
  uint64_t since0;

  load_empty_table_chunk(L);
  since0 = lj_gc2_alloc_since_load(g);
  la_store64_rel(&tg->local_total, LJ_GC2_ACCT_FLUSH - TNEW_EMPTY_SIZE);
  lj_gc_threshold_store(g, lj_gc_total_load(g) + 4u * LJ_GC2_ACCT_FLUSH);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);

  ljt_lua_pcall(L, 0, 1, "empty TNEW accounting fallback pcall");
  assert(lj_tg_local_total_acq(tg) == 0);
  assert(lj_gc2_alloc_since_load(g) >=
	 since0 + LJ_GC2_ACCT_FLUSH);
  assert_empty_table_body(g, tabV(L->top - 1));
  lua_pop(L, 1);
}

static void test_custom_allocator_fallback(lua_State *L, global_State *g)
{
  TestAllocCtx ctx;

  load_empty_table_chunk(L);
  ctx.oldf = lua_getallocf(L, &ctx.oldud);
  ctx.calls = 0;
  lua_setallocf(L, counting_alloc, &ctx);
  assert(g->allocf_arena == 0);

  ljt_lua_pcall(L, 0, 1, "empty TNEW custom allocator fallback pcall");
  assert(ctx.calls > 0);
  assert_empty_table_body(g, tabV(L->top - 1));
  lua_pop(L, 1);

  lua_setallocf(L, ctx.oldf, ctx.oldud);
  assert(g->allocf_arena == 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = L2TG(L);
  assert(g != NULL && tg != NULL);
  assert(g->allocf_arena == 1);
  assert(tg == g->main_tg);
  assert(lj_tg_flags_test_acq(tg, TGF_ARENA_INTERNAL));
  ljt_lua_dostring(L, "if jit then jit.off(true, true) end\n");

  test_inline_empty_tnew(L, g, tg);
  test_entering_uses_helper(L, g, tg);
  test_local_accounting_fallback(L, g, tg);
  test_custom_allocator_fallback(L, g);

  lua_close(L);
  puts("t-x64-tnew-empty-inline OK: interpreter empty TNEW inline/fallback paths verified");
  return 0;
}
