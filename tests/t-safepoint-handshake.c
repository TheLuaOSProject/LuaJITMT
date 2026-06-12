/*
** Focused test for the C-level soft-handshake scaffold.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc2.h"
#include "lj_dispatch.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

static void publish_manual(global_State *g, TGState *tg, uint32_t actions)
{
  uint64_t epoch = la_load64_rlx(&g->gc2.hs_epoch) + 1u;
  g->gc2.hs_actions = actions;
  la_store32_rel(&g->gc2.hs_pending, 1);  /* 05 section 5.4.2. */
  la_store64_rel(&g->gc2.hs_epoch, epoch);  /* 05 section 5.4.2. */
  la_store32_rel(&tg->reqmask, actions);  /* 05 section 5.4.2. */
  la_store32_rel(&tg->poll, 1);  /* 05 section 5.4.2 signal word. */
}

static int arena_list_contains(GCArena *a, GCArena *needle)
{
  while (a) {
    if (a == needle)
      return 1;
    a = a->hdr.next;
  }
  return 0;
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCtab *root_tab, *native_tab;
  void *plain_reset, *trav_reset;
  GCArena *plain_reset_a, *trav_reset_a;
  uint32_t i, ssb_published0, ssb_drained0;
  uint64_t ssb_items_published0, ssb_items_drained0;
  uint64_t epoch0;
  uint32_t actions;
  ASMFunction saved_dispatch;

  assert(L != NULL);
  luaL_openlibs(L);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert(g->gc2.tg_list == tg);
  assert(g->gc2.n_threads == 1);
  assert(g->gc2.hs_epoch == 0);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.ssb_head == NULL);
  assert(g->gc2.ssb_published == 0);
  assert(g->gc2.ssb_items_published == 0);
  assert(lj_gc2_ssb_empty(g));
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == 0);
  assert(tg->ssb_active == &tg->ssb_node[0]);
  assert(tg->ssb_free == &tg->ssb_node[1]);
  assert(tg->ssb_base == tg->ssb_node[0].slot);
  assert(tg->ssb_next == tg->ssb_base);
  assert(tg->ssb_end == tg->ssb_base + TG_GC2_SSB_SLOTS);

  epoch0 = g->gc2.hs_epoch;
  actions = LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);
  assert(tg->mark_active == 1);
  assert(tg->alloc.alloc_black == 1);

  actions = LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_pending == 0);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);

  saved_dispatch = tg->dispatch[BC_RET];
  assert(saved_dispatch == G2GG(g)->dispatch[BC_RET]);
  tg->dispatch[BC_RET] = NULL;
  assert(tg->dispatch[BC_RET] != G2GG(g)->dispatch[BC_RET]);
  actions = LJ_GC2_HS_REDISPATCH;
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert(tg->dispatch[BC_RET] == saved_dispatch);
  assert(tg->dispatch[BC_RET] == G2GG(g)->dispatch[BC_RET]);

  assert((tg->tg_flags & TGF_STOPREQ) == 0);
  actions = LJ_GC2_HS_STOPREQ;
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert((tg->tg_flags & TGF_STOPREQ) != 0);
  tg->tg_flags &= (uint8_t)~TGF_STOPREQ;

#if LJ_HASJIT
  assert(luaL_dostring(L,
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local s = 0\n"
    "for i = 1, 200 do s = s + i end\n"
    "return s\n") == LUA_OK);
  lua_pop(L, 1);
  assert(traceref(G2J(g), 1) != NULL || G2J(g)->freetrace > 0);
  epoch0 = g->gc2.hs_epoch;
  actions = LJ_GC2_HS_FLUSHJ;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert(G2J(g)->cur.traceno == 0);
  assert(G2J(g)->freetrace == 0);

  assert(G2J(g)->state == LJ_TRACE_IDLE);
  G2J(g)->state = LJ_TRACE_RECORD;
  epoch0 = g->gc2.hs_epoch;
  actions = LJ_GC2_HS_EXIT_TRACES;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert((G2J(g)->state & LJ_TRACE_ACTIVE) == 0);
  G2J(g)->state = LJ_TRACE_IDLE;
#endif

  lua_newtable(L);
  root_tab = tabV(L->top - 1);
  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(root_tab)) == 0);
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, LJ_GC2_HS_SCAN_ROOTS) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(lj_gc2_ismarked(g, obj2gco(root_tab)) == 1);
  assert(g->gc2.marks_this_round > 0);
  assert(!lj_gc2_ssb_empty(g));
  assert(lj_gc2_flush_ssb(g, tg) > 0);
  assert(lj_gc2_drain_ssb(g) > 0);
  assert(lj_gc2_ssb_empty(g));
  ssb_published0 = g->gc2.ssb_published;
  ssb_drained0 = g->gc2.ssb_drained;
  ssb_items_published0 = g->gc2.ssb_items_published;
  ssb_items_drained0 = g->gc2.ssb_items_drained;
  assert(lj_gc2_ssb_push(g, obj2gco(root_tab)) == 1);
  assert(lj_gc2_ssb_push(g, obj2gco(root_tab)) == 1);
  assert(!lj_gc2_ssb_empty(g));
  assert(tg->ssb_next == tg->ssb_base + 2);
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(tg->ssb_next == tg->ssb_base);
  assert(g->gc2.ssb_head != NULL);
  assert(!lj_gc2_ssb_empty(g));
  assert(g->gc2.ssb_published == ssb_published0 + 1u);
  assert(g->gc2.ssb_items_published == ssb_items_published0 + 2u);
  assert(lj_gc2_drain_ssb(g) == 2);
  assert(g->gc2.ssb_head == NULL);
  assert(lj_gc2_ssb_empty(g));
  assert(g->gc2.ssb_drained == ssb_drained0 + 1u);
  assert(g->gc2.ssb_items_drained == ssb_items_drained0 + 2u);
  ssb_drained0 = g->gc2.ssb_drained;
  ssb_items_drained0 = g->gc2.ssb_items_drained;
  for (i = 0; i < TG_GC2_SSB_SLOTS; i++)
    assert(lj_gc2_ssb_push(g, obj2gco(root_tab)) == 1);
  assert(tg->ssb_next == tg->ssb_end);
  ssb_published0 = g->gc2.ssb_published;
  assert(lj_gc2_ssb_push(g, obj2gco(root_tab)) == 1);
  assert(g->gc2.ssb_published == ssb_published0 + 1u);
  assert(tg->ssb_next == tg->ssb_base + 1);
  assert(lj_gc2_drain_ssb(g) == TG_GC2_SSB_SLOTS);
  assert(g->gc2.ssb_head == NULL);
  assert(g->gc2.ssb_drained == ssb_drained0 + 1u);
  assert(g->gc2.ssb_items_drained ==
	 ssb_items_drained0 + TG_GC2_SSB_SLOTS);
  ssb_drained0 = g->gc2.ssb_drained;
  ssb_items_drained0 = g->gc2.ssb_items_drained;
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(tg->ssb_next == tg->ssb_base);
  assert(!lj_gc2_ssb_empty(g));
  assert(lj_gc2_drain_ssb(g) == 1);
  assert(lj_gc2_ssb_empty(g));
  assert(g->gc2.ssb_drained == ssb_drained0 + 1u);
  assert(g->gc2.ssb_items_drained == ssb_items_drained0 + 1u);
  lua_pop(L, 1);
  lj_gc2_legacy_cycle_end(g);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);

  lua_newtable(L);
  native_tab = tabV(L->top - 1);
  lj_gc2_legacy_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(native_tab)) == 0);
  lj_native_enter(tg);
  assert(tg->in_native == 1);
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, LJ_GC2_HS_SCAN_ROOTS) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(tg->in_native == 1);
  assert(lj_gc2_ismarked(g, obj2gco(native_tab)) == 1);
  assert(lj_native_leave(L) == 0);
  assert(tg->in_native == 0);
  assert(lj_gc2_flush_ssb(g, tg) > 0);
  assert(lj_gc2_drain_ssb(g) > 0);
  assert(lj_gc2_ssb_empty(g));
  lua_pop(L, 1);
  lj_gc2_legacy_cycle_end(g);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);

  publish_manual(g, tg, LJ_GC2_HS_ENABLE_BARRIER);
  assert(lj_safepoint_poll(L) == LJ_GC2_HS_ENABLE_BARRIER);
  assert(g->gc2.hs_pending == 0);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->mark_active == 1);
  assert(lj_safepoint_poll(L) == 0);

  lj_native_enter(tg);
  assert(tg->in_native == 1);
  actions = LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_pending == 0);
  assert(tg->in_native == 1);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);
  assert(lj_native_leave(L) == 0);
  assert(tg->in_native == 0);

  lj_native_enter(tg);
  publish_manual(g, tg, LJ_GC2_HS_ALLOC_BLACK);
  assert(lj_native_leave(L) == LJ_GC2_HS_ALLOC_BLACK);
  assert(tg->in_native == 0);
  assert(g->gc2.hs_pending == 0);
  assert(tg->alloc.alloc_black == 1);

  assert(lj_gc2_handshake(g, 0) == 0);

  plain_reset = lj_arena_alloc(&tg->alloc, &tg->prng, 64, 0);
  trav_reset = lj_arena_alloc(&tg->alloc, &tg->prng, 64,
			      LJ_AF_TRAVERSABLE);
  assert(plain_reset != NULL);
  assert(trav_reset != NULL);
  plain_reset_a = lj_arena_of(plain_reset);
  trav_reset_a = lj_arena_of(trav_reset);
  actions = LJ_GC2_HS_RESET_ALLOC;
  epoch0 = g->gc2.hs_epoch;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.hs_actions == actions);
  assert(tg->alloc.bump[LJ_ARENAK_PLAIN].a == NULL);
  assert(tg->alloc.bump[LJ_ARENAK_TRAVERSABLE].a == NULL);
  assert(tg->alloc.owned[LJ_ARENAK_PLAIN] == NULL);
  assert(tg->alloc.owned[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_PLAIN],
			     plain_reset_a));
  assert(arena_list_contains(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     trav_reset_a));
  assert((plain_reset_a->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
  assert((trav_reset_a->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
  lj_arena_alloc_restore_sweep_kind(&tg->alloc, LJ_ARENAK_TRAVERSABLE);
  lj_arena_alloc_restore_sweep_kind(&tg->alloc, LJ_ARENAK_PLAIN);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  lj_arena_free(&tg->alloc, plain_reset, 64);
  lj_arena_free(&tg->alloc, trav_reset, 64);

  lua_close(L);

  printf("t-safepoint-handshake OK: C soft handshakes verified\n");
  return 0;
}
