/*
** Focused regression test for the x64 interpreter empty-table TNEW inline bump path.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_state.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_tg.h"

#include "lib/lua_fixture_helpers.h"

/* Built by the M6 harness with LJ_TAB_TEST_HELPERS enabled. */

#define TNEW_EMPTY_SIZE		((GCSize)sizeof(GCtab))
#define TNEW_EMPTY_NCELLS	((uint32_t)((sizeof(GCtab) + LJ_CELL_SIZE-1u) >> LJ_CELL_SHIFT))

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
typedef struct TestAllocCtx {
  lua_Alloc oldf;
  void *oldud;
  uint32_t calls;
} TestAllocCtx;
#endif

typedef struct ReusableRun {
  GCArena *a;
  uint32_t cell;
} ReusableRun;

static void prime_traversable_bump_window(TGState *tg)
{
  TGAlloc *alloc = &tg->alloc;
  GCArena *a = lj_arena_map(&tg->prng, LJ_AF_TRAVERSABLE);
  assert(a != NULL);
  lj_arena_owner_rel(a, lj_arena_alloc_owner_acq(alloc));
  lj_arena_next_rel(a, alloc->owned[LJ_ARENAK_TRAVERSABLE]);
  alloc->owned[LJ_ARENAK_TRAVERSABLE] = a;
  alloc->bump[LJ_ARENAK_TRAVERSABLE].a = a;
  alloc->bump[LJ_ARENAK_TRAVERSABLE].cell = LJ_AFIRST_CELL;
  alloc->bump[LJ_ARENAK_TRAVERSABLE].end = LJ_ARENA_CELLS;
  assert(lj_arena_alloc_register_existing(alloc));
}

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
static void *counting_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  TestAllocCtx *ctx = (TestAllocCtx *)ud;
  if (!ptr && nsize == sizeof(GCtab))
    ctx->calls++;
  return ctx->oldf(ctx->oldud, ptr, osize, nsize);
}
#endif

static void load_empty_table_chunk(lua_State *L)
{
  ljt_lua_loadstring(L, "return {}\n");
}

static void load_grown_table_chunk(lua_State *L)
{
  ljt_lua_loadstring(L,
    "local t = {}\n"
    "for i = 1, 40 do t[i] = i end\n"
    "t.k = 'v'\n"
    "return t\n");
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

static uint32_t ptr_state(void *p)
{
  GCArena *a = lj_arena_of(p);
  uint32_t cell = lj_arena_cellof(p);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  return lj_arena_state(a, cell);
}

static void assert_empty_table_body(global_State *g, GCtab *t)
{
  Node *node;
  assert(t != NULL);
  node = lj_tab_node_acq(t);
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
  assert(lj_tab_weak_cycle_acq(t) == 0);
  assert(lj_tab_gc2_rescan_state_acq(t) == LJ_TAB_RESCAN_NONE);
  assert(node == &g->nilnode);
  assert(getfreetop(t, node) == &g->nilnode);
  if (g->allocf == lj_arena_allocf) {
    GCArena *a = lj_arena_of(t);
    uint32_t cell = lj_arena_cellof(t);
    assert(lj_arena_bm_get(a->block, cell));
    assert(lj_arena_ready_get(a, cell));
  }
}

static void poison_recycled_empty_table_fields(GCArena *a, uint32_t cell)
{
  GCtab *stale = (GCtab *)lj_arena_cellptr(a, cell);
  /* A private FREE bump cell may contain bytes from an older incarnation.
  ** Poison only the fields under test, without creating allocator-visible
  ** free-run metadata, so the inline constructor must overwrite both. */
  assert(lj_arena_state(a, cell) == 0);
  lj_tab_weak_cycle_store_rlx(stale, UINT32_C(0xa5a5a5a5));
  lj_tab_gc2_rescan_state_store_rlx(stale, LJ_TAB_RESCAN_CANCELLED);
  assert(lj_tab_weak_cycle_acq(stale) == UINT32_C(0xa5a5a5a5));
  assert(lj_tab_gc2_rescan_state_acq(stale) == LJ_TAB_RESCAN_CANCELLED);
}

static ReusableRun create_reusable_empty_table_run(TGState *tg)
{
  void *p = lj_arena_alloc(&tg->alloc, &tg->prng, TNEW_EMPTY_SIZE,
			   LJ_AF_TRAVERSABLE);
  ReusableRun r;
  assert(p != NULL);
  r.a = lj_arena_of(p);
  r.cell = lj_arena_cellof(p);
  lj_arena_free(&tg->alloc, p, TNEW_EMPTY_SIZE);
  assert(lj_arena_state(r.a, r.cell) == 1);
  assert(lj_arena_alloc_has_run_ge(&tg->alloc, LJ_ARENAK_TRAVERSABLE,
				    TNEW_EMPTY_NCELLS));
  return r;
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
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(t));
  assert(lj_obj_gcw_acq(obj2gco(t)) == pending0);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_chain_contains(g, obj2gco(t)));
  lua_pop(L, 1);
}

static void test_inline_empty_tnew_clears_recycled_fields(lua_State *L,
						   global_State *g,
						   TGState *tg)
{
  LJArenaBump *b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  GCArena *a0;
  uint32_t cell0, calls0;
  GCtab *t;

  load_empty_table_chunk(L);
  a0 = b->a;
  cell0 = b->cell;
  calls0 = lj_tab_test_new0_calls();
  assert(a0 != NULL);
  assert(cell0 + TNEW_EMPTY_NCELLS <= b->end);
  assert(lj_tg_local_total_acq(tg) <
	 LJ_GC2_ACCT_FLUSH - TNEW_EMPTY_SIZE);
  poison_recycled_empty_table_fields(a0, cell0);

  ljt_lua_pcall(L, 0, 1, "recycled-field empty TNEW inline pcall");
  t = tabV(L->top - 1);

  assert(lj_tab_test_new0_calls() == calls0);
  assert((void *)t == lj_arena_cellptr(a0, cell0));
  assert_empty_table_body(g, t);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(root_chain_contains(g, obj2gco(t)));
  lua_pop(L, 1);
}

static void test_plain_new0_inline_empty(lua_State *L, global_State *g,
					 TGState *tg)
{
  LJArenaBump *b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  GCArena *a0;
  GCobj *pending0;
  GCSize total0;
  uint64_t local0;
  uint32_t cell0, black, calls0, i;
  GCtab *t;

  a0 = b->a;
  cell0 = b->cell;
  black = lj_arena_alloc_black_acq(&tg->alloc);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  total0 = lj_gc_total_load(g);
  local0 = lj_tg_local_total_acq(tg);
  calls0 = lj_tab_test_new0_calls();

  assert(a0 != NULL);
  assert(cell0 + TNEW_EMPTY_NCELLS <= b->end);
  assert(local0 < LJ_GC2_ACCT_FLUSH - TNEW_EMPTY_SIZE);

  t = lj_tab_new0(L);

  assert(lj_tab_test_new0_calls() == calls0 + 1u);
  assert_empty_table_body(g, t);
  assert((void *)t == lj_arena_cellptr(a0, cell0));
  assert(b->cell == cell0 + TNEW_EMPTY_NCELLS);
  assert(lj_arena_state(a0, cell0) == (black ? 3u : 2u));
  for (i = 1; i < TNEW_EMPTY_NCELLS; i++)
    assert(lj_arena_state(a0, cell0 + i) == 0);
  assert(lj_gc_total_load(g) == total0 + TNEW_EMPTY_SIZE);
  assert(lj_tg_local_total_acq(tg) == local0 + TNEW_EMPTY_SIZE);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(t));
  assert(lj_obj_gcw_acq(obj2gco(t)) == pending0);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_chain_contains(g, obj2gco(t)));
}

static void test_plain_new_inline_empty(lua_State *L, global_State *g,
					TGState *tg)
{
  LJArenaBump *b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  GCArena *a0;
  GCobj *pending0;
  GCSize total0;
  uint64_t local0;
  uint32_t cell0, black, calls0, i;
  GCtab *t;

  a0 = b->a;
  cell0 = b->cell;
  black = lj_arena_alloc_black_acq(&tg->alloc);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  total0 = lj_gc_total_load(g);
  local0 = lj_tg_local_total_acq(tg);
  calls0 = lj_tab_test_new0_calls();

  assert(a0 != NULL);
  assert(cell0 + TNEW_EMPTY_NCELLS <= b->end);
  assert(local0 < LJ_GC2_ACCT_FLUSH - TNEW_EMPTY_SIZE);

  t = lj_tab_new(L, 0, 0);

  assert(lj_tab_test_new0_calls() == calls0 + 1u);
  assert_empty_table_body(g, t);
  assert((void *)t == lj_arena_cellptr(a0, cell0));
  assert(b->cell == cell0 + TNEW_EMPTY_NCELLS);
  assert(lj_arena_state(a0, cell0) == (black ? 3u : 2u));
  for (i = 1; i < TNEW_EMPTY_NCELLS; i++)
    assert(lj_arena_state(a0, cell0 + i) == 0);
  assert(lj_gc_total_load(g) == total0 + TNEW_EMPTY_SIZE);
  assert(lj_tg_local_total_acq(tg) == local0 + TNEW_EMPTY_SIZE);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(t));
  assert(lj_obj_gcw_acq(obj2gco(t)) == pending0);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_chain_contains(g, obj2gco(t)));
}

static void test_capi_newtable_rooted_empty(lua_State *L, global_State *g,
					    TGState *tg)
{
  LJArenaBump *b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  GCArena *a0;
  GCobj *pending0;
  GCSize total0;
  uint64_t local0;
  uint32_t cell0, black, calls0, i;
  GCtab *t;

  a0 = b->a;
  cell0 = b->cell;
  black = lj_arena_alloc_black_acq(&tg->alloc);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  total0 = lj_gc_total_load(g);
  local0 = lj_tg_local_total_acq(tg);
  calls0 = lj_tab_test_new0_calls();

  assert(a0 != NULL);
  assert(cell0 + TNEW_EMPTY_NCELLS <= b->end);
  assert(local0 < LJ_GC2_ACCT_FLUSH - TNEW_EMPTY_SIZE);

  lua_newtable(L);
  t = tabV(L->top - 1);

  /* The public API uses the explicitly rooted constructor while its target
  ** state claim is released, so it must bypass the unrooted new0 helper. */
  assert(lj_tab_test_new0_calls() == calls0);
  assert_empty_table_body(g, t);
  assert((void *)t == lj_arena_cellptr(a0, cell0));
  assert(b->cell == cell0 + TNEW_EMPTY_NCELLS);
  assert(lj_arena_state(a0, cell0) == (black ? 3u : 2u));
  for (i = 1; i < TNEW_EMPTY_NCELLS; i++)
    assert(lj_arena_state(a0, cell0 + i) == 0);
  assert(lj_gc_total_load(g) == total0 + TNEW_EMPTY_SIZE);
  assert(lj_tg_local_total_acq(tg) == local0 + TNEW_EMPTY_SIZE);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(t));
  assert(lj_obj_gcw_acq(obj2gco(t)) == pending0);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_chain_contains(g, obj2gco(t)));
  lua_pop(L, 1);
}

static void test_active_black_empty_tables_publish_exact(lua_State *L,
							 global_State *g,
							 TGState *tg)
{
  LJArenaBump *b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  GCArena *a0;
  GCobj *pending0;
  GCSize total0;
  uint64_t local0;
  uint32_t old_mark_active, cell0, calls0, i;
  uint8_t old_alloc_black;
  GCtab *t_inline, *t_helper;

  load_empty_table_chunk(L);
  (void)lj_gc_flush_root_pending(g);
  lj_gcroot_pending_hint_rel(g, 0);

  a0 = b->a;
  cell0 = b->cell;
  pending0 = lj_tg_gcroot_pending_acq(tg);
  total0 = lj_gc_total_load(g);
  local0 = lj_tg_local_total_acq(tg);
  calls0 = lj_tab_test_new0_calls();
  old_mark_active = lj_tg_mark_active_acq(tg);
  old_alloc_black = lj_tg_alloc_black_acq(tg);

  assert(a0 != NULL);
  assert(cell0 + 2u * TNEW_EMPTY_NCELLS <= b->end);
  assert(pending0 == NULL);
  assert(local0 < LJ_GC2_ACCT_FLUSH - 2u * TNEW_EMPTY_SIZE);

  lj_tg_mark_active_rel(tg, 1);
  lj_tg_alloc_black_rel(tg, 1);

  ljt_lua_pcall(L, 0, 1, "active-black empty TNEW pcall");
  t_inline = tabV(L->top - 1);
  t_helper = lj_tab_new0(L);

  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);

  assert(lj_tab_test_new0_calls() == calls0 + 1u);
  assert_empty_table_body(g, t_inline);
  assert_empty_table_body(g, t_helper);
  assert((void *)t_inline == lj_arena_cellptr(a0, cell0));
  assert((void *)t_helper ==
	 lj_arena_cellptr(a0, cell0 + TNEW_EMPTY_NCELLS));
  assert(b->cell == cell0 + 2u * TNEW_EMPTY_NCELLS);
  for (i = 0; i < 2u * TNEW_EMPTY_NCELLS; i += TNEW_EMPTY_NCELLS)
    assert(lj_arena_state(a0, cell0 + i) == 3u);
  assert(lj_gc_total_load(g) == total0 + 2u * TNEW_EMPTY_SIZE);
  assert(lj_tg_local_total_acq(tg) == local0 + 2u * TNEW_EMPTY_SIZE);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(t_helper));
  assert(lj_obj_gcw_acq(obj2gco(t_helper)) == obj2gco(t_inline));
  assert(lj_obj_gcw_acq(obj2gco(t_inline)) == pending0);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_gc2_ismarked(g, obj2gco(t_inline)) > 0);
  assert(lj_gc2_ismarked(g, obj2gco(t_helper)) > 0);
  assert(lj_gc_flush_root_pending(g) >= 2u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_chain_contains(g, obj2gco(t_inline)));
  assert(root_chain_contains(g, obj2gco(t_helper)));
  assert(lj_arena_bm_get(a0->mark, cell0));
  assert(lj_arena_bm_get(a0->mark, cell0 + TNEW_EMPTY_NCELLS));
  lua_pop(L, 1);
}

static void test_active_black_grown_table_publishes_exact(lua_State *L,
							  global_State *g,
							  TGState *tg)
{
  LJArenaBump *b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  GCArena *a0;
  GCobj *pending0, *tobj;
  TValue *array;
  Node *node;
  void *arraymem, *nodemem;
  uint32_t old_mark_active, cell0;
  uint8_t old_alloc_black;
  GCtab *t;

  load_grown_table_chunk(L);
  (void)lj_gc_flush_root_pending(g);
  lj_gcroot_pending_hint_rel(g, 0);
  (void)lj_tg_local_total_xchg_acqrel(tg, 0);

  a0 = b->a;
  cell0 = b->cell;
  pending0 = lj_tg_gcroot_pending_acq(tg);
  old_mark_active = lj_tg_mark_active_acq(tg);
  old_alloc_black = lj_tg_alloc_black_acq(tg);

  assert(a0 != NULL);
  assert(cell0 + TNEW_EMPTY_NCELLS <= b->end);
  assert(pending0 == NULL);

  lj_tg_mark_active_rel(tg, 1);
  lj_tg_alloc_black_rel(tg, 1);

  ljt_lua_pcall(L, 0, 1, "active-black grown TNEW pcall");
  t = tabV(L->top - 1);
  tobj = obj2gco(t);

  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);

  assert(t->colo == 0);
  assert((void *)t == lj_arena_cellptr(a0, cell0));
  assert(lj_tg_gcroot_pending_acq(tg) == tobj);
  assert(lj_obj_gcw_acq(tobj) == pending0);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_gc2_ismarked(g, tobj) > 0);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_chain_contains(g, tobj));

  array = lj_tab_array_acq(t);
  node = lj_tab_node_acq(t);
  assert(array != NULL);
  assert(!lj_tab_array_is_colocated(t, array));
  assert(lj_tab_asize_acq(t) >= 40u);
  assert(lj_tab_acap_acq(t) >= lj_tab_asize_acq(t));
  assert(node != &g->nilnode);
  assert(lj_tab_hmask_acq(t) > 0);
  arraymem = lj_tab_array_hdrw(array);
  nodemem = lj_tab_node_hdrw(node);
  assert((ptr_state(arraymem) & 2u) != 0);
  assert((ptr_state(nodemem) & 2u) != 0);
  lua_pop(L, 1);
}

#if LJ_HASJIT
static void test_jit_helper_inline_empty_tnew(lua_State *L, global_State *g,
					      TGState *tg)
{
  LJArenaBump *b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  GCArena *a0;
  GCobj *pending0;
  GCSize total0;
  uint64_t local0;
  uint32_t cell0, black, calls0, i;
  GCtab *t;

  a0 = b->a;
  cell0 = b->cell;
  black = lj_arena_alloc_black_acq(&tg->alloc);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  total0 = lj_gc_total_load(g);
  local0 = lj_tg_local_total_acq(tg);
  calls0 = lj_tab_test_new0_calls();

  assert(a0 != NULL);
  assert(cell0 + TNEW_EMPTY_NCELLS <= b->end);
  assert(local0 < LJ_GC2_ACCT_FLUSH - TNEW_EMPTY_SIZE);

  t = lj_tab_new0_forjit(L);

  assert(lj_tab_test_new0_calls() == calls0);
  assert_empty_table_body(g, t);
  assert((void *)t == lj_arena_cellptr(a0, cell0));
  assert(b->cell == cell0 + TNEW_EMPTY_NCELLS);
  assert(lj_arena_state(a0, cell0) == (black ? 3u : 2u));
  for (i = 1; i < TNEW_EMPTY_NCELLS; i++)
    assert(lj_arena_state(a0, cell0 + i) == 0);
  assert(lj_gc_total_load(g) == total0 + TNEW_EMPTY_SIZE);
  assert(lj_tg_local_total_acq(tg) == local0 + TNEW_EMPTY_SIZE);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(t));
  assert(lj_obj_gcw_acq(obj2gco(t)) == pending0);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_chain_contains(g, obj2gco(t)));
}

static void test_jit_helper_entering_fallback(lua_State *L, global_State *g)
{
  uint32_t calls0;
  GCtab *t;

  lj_tab_test_reset_new0_calls();
  calls0 = lj_tab_test_new0_calls();

  assert(mt_entering_add_rlx(g, 1) == 0);
  t = lj_tab_new0_forjit(L);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, 0x7fffffff);

  assert(lj_tab_test_new0_calls() == calls0 + 1u);
  assert_empty_table_body(g, t);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(root_chain_contains(g, obj2gco(t)));
}
#endif

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
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(b->cell >= cell0);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
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

#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
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
#endif

static void test_inline_empty_tnew_uses_bump_with_free_run(lua_State *L,
							   global_State *g,
							   TGState *tg)
{
  LJArenaBump *b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  GCArena *a0;
  GCobj *pending0;
  GCSize total0;
  uint64_t local0;
  uint32_t cell0, black, calls0, i;
  ReusableRun run;
  GCtab *t;

  load_empty_table_chunk(L);
  run = create_reusable_empty_table_run(tg);
  a0 = b->a;
  cell0 = b->cell;
  black = lj_arena_alloc_black_acq(&tg->alloc);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  total0 = lj_gc_total_load(g);
  local0 = lj_tg_local_total_acq(tg);
  calls0 = lj_tab_test_new0_calls();

  assert(a0 != NULL);
  assert(cell0 + TNEW_EMPTY_NCELLS <= b->end);
  assert(local0 < LJ_GC2_ACCT_FLUSH - TNEW_EMPTY_SIZE);

  ljt_lua_pcall(L, 0, 1, "empty TNEW inline free-run pcall");
  t = tabV(L->top - 1);

  assert(lj_tab_test_new0_calls() == calls0);
  assert_empty_table_body(g, t);
  assert((void *)t == lj_arena_cellptr(a0, cell0));
  assert((void *)t != lj_arena_cellptr(run.a, run.cell));
  assert(b->cell == cell0 + TNEW_EMPTY_NCELLS);
  assert(lj_arena_state(run.a, run.cell) == 1);
  assert(lj_arena_state(a0, cell0) == (black ? 3u : 2u));
  for (i = 1; i < TNEW_EMPTY_NCELLS; i++)
    assert(lj_arena_state(a0, cell0 + i) == 0);
  assert(lj_gc_total_load(g) == total0 + TNEW_EMPTY_SIZE);
  assert(lj_tg_local_total_acq(tg) == local0 + TNEW_EMPTY_SIZE);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(t));
  assert(lj_obj_gcw_acq(obj2gco(t)) == pending0);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_chain_contains(g, obj2gco(t)));
  lua_pop(L, 1);
}

static void test_plain_new0_uses_bump_with_free_run(lua_State *L,
						    global_State *g,
						    TGState *tg)
{
  LJArenaBump *b = &tg->alloc.bump[LJ_ARENAK_TRAVERSABLE];
  GCArena *a0;
  GCobj *pending0;
  GCSize total0;
  uint64_t local0;
  uint32_t cell0, black, calls0, i;
  ReusableRun run;
  GCtab *t;

  run = create_reusable_empty_table_run(tg);
  a0 = b->a;
  cell0 = b->cell;
  black = lj_arena_alloc_black_acq(&tg->alloc);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  total0 = lj_gc_total_load(g);
  local0 = lj_tg_local_total_acq(tg);
  calls0 = lj_tab_test_new0_calls();

  assert(a0 != NULL);
  assert(cell0 + TNEW_EMPTY_NCELLS <= b->end);
  assert(local0 < LJ_GC2_ACCT_FLUSH - TNEW_EMPTY_SIZE);

  t = lj_tab_new0(L);

  assert(lj_tab_test_new0_calls() == calls0 + 1u);
  assert_empty_table_body(g, t);
  assert((void *)t == lj_arena_cellptr(a0, cell0));
  assert((void *)t != lj_arena_cellptr(run.a, run.cell));
  assert(b->cell == cell0 + TNEW_EMPTY_NCELLS);
  assert(lj_arena_state(run.a, run.cell) == 1);
  assert(lj_arena_state(a0, cell0) == (black ? 3u : 2u));
  for (i = 1; i < TNEW_EMPTY_NCELLS; i++)
    assert(lj_arena_state(a0, cell0 + i) == 0);
  assert(lj_gc_total_load(g) == total0 + TNEW_EMPTY_SIZE);
  assert(lj_tg_local_total_acq(tg) == local0 + TNEW_EMPTY_SIZE);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(t));
  assert(lj_obj_gcw_acq(obj2gco(t)) == pending0);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  assert(root_chain_contains(g, obj2gco(t)));
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
  if (tg->alloc.bump[LJ_ARENAK_TRAVERSABLE].a == NULL)
    prime_traversable_bump_window(tg);

  test_inline_empty_tnew(L, g, tg);
  test_inline_empty_tnew_clears_recycled_fields(L, g, tg);
  test_plain_new0_inline_empty(L, g, tg);
  test_plain_new_inline_empty(L, g, tg);
  test_capi_newtable_rooted_empty(L, g, tg);
  test_active_black_empty_tables_publish_exact(L, g, tg);
  test_active_black_grown_table_publishes_exact(L, g, tg);
#if LJ_HASJIT
  test_jit_helper_inline_empty_tnew(L, g, tg);
  test_jit_helper_entering_fallback(L, g);
#endif
  test_entering_uses_helper(L, g, tg);
  test_local_accounting_fallback(L, g, tg);
#if !LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  test_custom_allocator_fallback(L, g);
#endif
  test_inline_empty_tnew_uses_bump_with_free_run(L, g, tg);
  test_plain_new0_uses_bump_with_free_run(L, g, tg);

  lua_close(L);
  puts("t-x64-tnew-empty-inline OK: interpreter empty TNEW inline/fallback paths verified");
  return 0;
}
