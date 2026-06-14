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

static int publish_alloc_white_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  publish_manual(g, tg, LJ_GC2_HS_ALLOC_WHITE);
  return 0;
}

static int publish_stopreq_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  publish_manual(g, tg, LJ_GC2_HS_STOPREQ);
  return 0;
}

static int assert_acked_alloc_white_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  assert(g->gc2.hs_pending == 0);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);
  assert(tg->alloc.alloc_black == 0);
  return 0;
}

static int clear_stopreq_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  uint8_t flags;
  assert(tg != NULL);
  assert(g->gc2.hs_pending == 0);
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == g->gc2.hs_epoch);
  flags = la_load8_acq(&tg->tg_flags);
  assert((flags & TGF_STOPREQ) != 0);
  la_store8_rel(&tg->tg_flags, (uint8_t)(flags & ~TGF_STOPREQ));
  return 0;
}

static int assert_not_native_c(lua_State *L)
{
  global_State *g = G(L);
  TGState *tg = G2TG(g);
  assert(tg != NULL);
  assert(tg->in_native == 0);
  return 0;
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

static int tg_list_contains(TGState *tg, TGState *needle)
{
  while (tg) {
    if (tg == needle)
      return 1;
    tg = tg->next_tg;
  }
  return 0;
}

static void assert_attach_phase(lua_State *L, global_State *g, TGState *main_tg,
				uint32_t phase, uint32_t mark_active,
				uint32_t alloc_black)
{
  TGState phase_tg;
  uint32_t oldphase = la_load32_acq(&g->gc2.phase);

  lj_tg_init_thread(g, &phase_tg, NULL, 0);
  phase_tg.tid = main_tg->tid + 2000u + phase;
  phase_tg.alloc.owner_tid = phase_tg.tid;
  phase_tg.cur_L = L;
  la_store32_rel(&g->gc2.phase, phase);
  lj_tg_attach(g, &phase_tg);
  assert(g->gc2.n_threads == 2);
  assert(tg_list_contains(g->gc2.tg_list, &phase_tg));
  assert(phase_tg.mark_active == mark_active);
  assert(phase_tg.alloc.alloc_black == alloc_black);
  assert(phase_tg.hs_epoch_ack == g->gc2.hs_epoch);
  lj_tg_detach(g, &phase_tg);
  assert(g->gc2.n_threads == 1);
  assert(phase_tg.tg_flags & TGF_DEAD);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(!tg_list_contains(g->gc2.tg_list, &phase_tg));
  la_store32_rel(&g->gc2.phase, oldphase);
  lj_tg_fini_thread(g, &phase_tg);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  TGState extra_tg;
  TGState arena_tg;
  GCtab *root_tab, *native_tab;
  void *plain_reset, *trav_reset;
  void *transfer_small, *transfer_huge;
  size_t transfer_huge_size = LJ_HUGE_THRESHOLD + 8192u;
  GCArena *plain_reset_a, *trav_reset_a;
  LJHugeInfo hi;
  uint32_t i, ssb_published0, ssb_drained0;
  uint64_t ssb_items_published0, ssb_items_drained0;
  uint64_t epoch0;
  uint32_t actions;
  ASMFunction saved_dispatch;

  assert(L != NULL);
  luaL_openlibs(L);
  lua_pushcfunction(L, publish_alloc_white_c);
  lua_setglobal(L, "publish_alloc_white");
  lua_pushcfunction(L, publish_stopreq_c);
  lua_setglobal(L, "publish_stopreq");
  lua_pushcfunction(L, assert_acked_alloc_white_c);
  lua_setglobal(L, "assert_acked_alloc_white");
  lua_pushcfunction(L, clear_stopreq_c);
  lua_setglobal(L, "clear_stopreq");
  lua_pushcfunction(L, assert_not_native_c);
  lua_setglobal(L, "assert_not_native");
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert(g->gc2.tg_list == tg);
  assert(g->gc2.n_threads == 1);
  assert(g->gc2.hs_epoch == 0);
  assert(g->gc2.hs_pending == 0);
  assert(g->gc2.ssb_head == NULL);
  assert(la_load32_acq(&g->gc2.ssb_published) == 0);
  assert(la_load64_acq(&g->gc2.ssb_items_published) == 0);
  assert(lj_gc2_ssb_empty(g));
  assert(tg->poll == 0);
  assert(tg->reqmask == 0);
  assert(tg->hs_epoch_ack == 0);
  assert(tg->ssb_active == &tg->ssb_node[0]);
  assert(tg->ssb_free == &tg->ssb_node[1]);
  assert(tg->ssb_base == tg->ssb_node[0].slot);
  assert(tg->ssb_next == tg->ssb_base);
  assert(tg->ssb_end == tg->ssb_base + TG_GC2_SSB_SLOTS);

  assert_attach_phase(L, g, tg, LJ_GC2_IDLE, 0, 0);
  assert_attach_phase(L, g, tg, LJ_GC2_MARK, 1, 1);
  assert_attach_phase(L, g, tg, LJ_GC2_WEAK, 1, 1);
  assert_attach_phase(L, g, tg, LJ_GC2_SWEEP, 0, 1);

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
  assert(la_load64_acq(&g->gc2.marks_this_round) > 0);
  assert(!lj_gc2_ssb_empty(g));
  assert(lj_gc2_flush_ssb(g, tg) > 0);
  assert(lj_gc2_drain_ssb(g) > 0);
  assert(lj_gc2_ssb_empty(g));
  ssb_published0 = la_load32_acq(&g->gc2.ssb_published);
  ssb_drained0 = la_load32_acq(&g->gc2.ssb_drained);
  ssb_items_published0 = la_load64_acq(&g->gc2.ssb_items_published);
  ssb_items_drained0 = la_load64_acq(&g->gc2.ssb_items_drained);
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
  assert(la_load32_acq(&g->gc2.ssb_published) == ssb_published0 + 1u);
  assert(la_load64_acq(&g->gc2.ssb_items_published) ==
	 ssb_items_published0 + 2u);
  assert(lj_gc2_drain_ssb(g) == 2);
  assert(g->gc2.ssb_head == NULL);
  assert(lj_gc2_ssb_empty(g));
  assert(la_load32_acq(&g->gc2.ssb_drained) == ssb_drained0 + 1u);
  assert(la_load64_acq(&g->gc2.ssb_items_drained) ==
	 ssb_items_drained0 + 2u);
  ssb_drained0 = la_load32_acq(&g->gc2.ssb_drained);
  ssb_items_drained0 = la_load64_acq(&g->gc2.ssb_items_drained);
  for (i = 0; i < TG_GC2_SSB_SLOTS; i++)
    assert(lj_gc2_ssb_push(g, obj2gco(root_tab)) == 1);
  assert(tg->ssb_next == tg->ssb_end);
  ssb_published0 = la_load32_acq(&g->gc2.ssb_published);
  assert(lj_gc2_ssb_push(g, obj2gco(root_tab)) == 1);
  assert(la_load32_acq(&g->gc2.ssb_published) == ssb_published0 + 1u);
  assert(tg->ssb_next == tg->ssb_base + 1);
  assert(lj_gc2_drain_ssb(g) == TG_GC2_SSB_SLOTS);
  assert(g->gc2.ssb_head == NULL);
  assert(la_load32_acq(&g->gc2.ssb_drained) == ssb_drained0 + 1u);
  assert(la_load64_acq(&g->gc2.ssb_items_drained) ==
	 ssb_items_drained0 + TG_GC2_SSB_SLOTS);
  ssb_drained0 = la_load32_acq(&g->gc2.ssb_drained);
  ssb_items_drained0 = la_load64_acq(&g->gc2.ssb_items_drained);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(tg->ssb_next == tg->ssb_base);
  assert(!lj_gc2_ssb_empty(g));
  assert(lj_gc2_drain_ssb(g) == 1);
  assert(lj_gc2_ssb_empty(g));
  assert(la_load32_acq(&g->gc2.ssb_drained) == ssb_drained0 + 1u);
  assert(la_load64_acq(&g->gc2.ssb_items_drained) ==
	 ssb_items_drained0 + 1u);
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

  assert(luaL_dostring(L,
    "local p = os.tmpname()\n"
    "local q = p .. '.renamed'\n"
    "local f = assert(io.open(p, 'w'))\n"
    "f:write('x')\n"
    "f:close()\n"
    "publish_alloc_white()\n"
    "assert(os.rename(p, q))\n"
    "assert_acked_alloc_white()\n"
    "publish_alloc_white()\n"
    "assert(os.remove(q))\n"
    "assert_acked_alloc_white()\n"
    "publish_alloc_white()\n"
    "local r = os.tmpname()\n"
    "assert_acked_alloc_white()\n"
    "os.remove(r)\n") == LUA_OK);

  assert(luaL_dostring(L,
    "local function expect_stopreq(fn)\n"
    "  publish_stopreq()\n"
    "  local ok, err = pcall(fn)\n"
    "  assert(not ok)\n"
    "  assert(tostring(err):find('thread interrupted: VM shutdown', 1, true))\n"
    "  clear_stopreq()\n"
    "end\n"
    "local p = os.tmpname()\n"
    "local q = p .. '.stopreq'\n"
    "local f = assert(io.open(p, 'w'))\n"
    "f:write('x')\n"
    "f:close()\n"
    "expect_stopreq(function() return os.rename(p, q) end)\n"
    "os.remove(p)\n"
    "f = assert(io.open(q, 'w'))\n"
    "f:write('y')\n"
    "f:close()\n"
    "expect_stopreq(function() return os.remove(q) end)\n"
    "os.remove(q)\n"
    "expect_stopreq(function() return os.execute(':') end)\n"
    "expect_stopreq(function() return os.tmpname() end)\n"
    "expect_stopreq(function() return io.tmpfile() end)\n"
    "p = os.tmpname()\n"
    "f = assert(io.open(p, 'w'))\n"
    "f:write('z')\n"
    "expect_stopreq(function() return f:flush() end)\n"
    "expect_stopreq(function() return f:seek('set', 0) end)\n"
    "f:close()\n"
    "os.remove(p)\n") == LUA_OK);

#if LJ_HASFFI
  assert(luaL_dostring(L,
    "local ffi = require('ffi')\n"
    "ffi.cdef[[\n"
    "int getpid(void);\n"
    "typedef int (*cmp_t)(const void *, const void *);\n"
    "void qsort(void *base, unsigned long nmemb, unsigned long size,\n"
    "           cmp_t compar);\n"
    "]]\n"
    "publish_alloc_white()\n"
    "ffi.C.getpid()\n"
    "assert_acked_alloc_white()\n"
    "local arr = ffi.new('int[2]', {2, 1})\n"
    "local cmp\n"
    "cmp = ffi.cast('cmp_t', function(a, b)\n"
    "  assert_not_native()\n"
    "  local ia = ffi.cast('const int *', a)[0]\n"
    "  local ib = ffi.cast('const int *', b)[0]\n"
    "  return ia - ib\n"
    "end)\n"
    "publish_alloc_white()\n"
    "ffi.C.qsort(arr, 2, ffi.sizeof('int'), cmp)\n"
    "assert_acked_alloc_white()\n"
    "cmp:free()\n"
    "assert(arr[0] == 1 and arr[1] == 2)\n") == LUA_OK);
#endif

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

  lj_tg_init_thread(g, &extra_tg, NULL, 0);
  extra_tg.cur_L = L;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);
  assert(tg_list_contains(g->gc2.tg_list, tg));
  assert(tg_list_contains(g->gc2.tg_list, &extra_tg));
  assert(!(extra_tg.tg_flags & TGF_DEAD));
  assert(extra_tg.hs_epoch_ack == g->gc2.hs_epoch);

  epoch0 = g->gc2.hs_epoch;
  actions = LJ_GC2_HS_ENABLE_BARRIER|LJ_GC2_HS_ALLOC_BLACK;
  assert(lj_gc2_handshake(g, actions) == 2);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(extra_tg.poll == 0);
  assert(extra_tg.reqmask == 0);
  assert(extra_tg.hs_epoch_ack == g->gc2.hs_epoch);
  assert(extra_tg.mark_active == 1);
  assert(extra_tg.alloc.alloc_black == 1);
  assert(extra_tg.in_native == 1);

  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(extra_tg.tg_flags & TGF_DEAD);
  assert(extra_tg.in_native == 0);
  epoch0 = g->gc2.hs_epoch;
  actions = LJ_GC2_HS_DISABLE_BARRIER|LJ_GC2_HS_ALLOC_WHITE;
  assert(lj_gc2_handshake(g, actions) == 1);
  assert(g->gc2.hs_epoch == epoch0 + 1u);
  assert(g->gc2.hs_pending == 0);
  assert(extra_tg.hs_epoch_ack == epoch0);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(!tg_list_contains(g->gc2.tg_list, &extra_tg));
  lj_tg_fini_thread(g, &extra_tg);

  lj_tg_init_thread(g, &arena_tg, NULL, 1);
  arena_tg.tid = tg->tid + 1000u;
  arena_tg.alloc.owner_tid = arena_tg.tid;
  transfer_small = lj_arena_allocf(&arena_tg.allocd, NULL, 0, 64);
  transfer_huge = lj_arena_allocf(&arena_tg.allocd, NULL, 0,
				  transfer_huge_size);
  assert(transfer_small != NULL);
  assert(transfer_huge != NULL);
  assert(lj_arena_of(transfer_small)->hdr.owner_tid == arena_tg.tid);
  assert(lj_arena_of(transfer_huge)->hdr.owner_tid == arena_tg.tid);
  assert(lj_arena_hugetab_lookup(&arena_tg.huge, transfer_huge, &hi) == 1);
  assert(hi.size == transfer_huge_size);
  lj_tg_attach(g, &arena_tg);
  assert(g->gc2.n_threads == 2);
  assert(tg_list_contains(g->gc2.tg_list, &arena_tg));
  lj_tg_detach(g, &arena_tg);
  assert(g->gc2.n_threads == 1);
  assert(arena_tg.tg_flags & TGF_DEAD);
  assert(lj_tg_reclaim_dead(g) == 1u);
  assert(!tg_list_contains(g->gc2.tg_list, &arena_tg));
  assert((arena_tg.tg_flags & TGF_ARENA_INTERNAL) == 0);
  assert((arena_tg.tg_flags & TGF_HUGETAB) == 0);
  assert(arena_tg.huge.h == NULL);
  assert(lj_arena_of(transfer_small)->hdr.owner_tid == tg->alloc.owner_tid);
  assert(lj_arena_of(transfer_huge)->hdr.owner_tid == tg->alloc.owner_tid);
  assert(lj_arena_hugetab_lookup(&tg->huge, transfer_huge, &hi) == 1);
  assert(hi.size == transfer_huge_size);
  assert(lj_arena_allocf(&tg->allocd, transfer_small, 64, 0) == NULL);
  assert(lj_arena_allocf(&tg->allocd, transfer_huge, transfer_huge_size, 0) ==
	 NULL);
  lj_tg_fini_thread(g, &arena_tg);

  lua_close(L);

  printf("t-safepoint-handshake OK: C soft handshakes verified\n");
  return 0;
}
