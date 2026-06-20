/*
** Focused test for the runtime traversable arena sweep bridge.
*/

#include <assert.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"

static uint32_t ptr_state(void *p)
{
  GCArena *a = lj_arena_of(p);
  uint32_t cell = lj_arena_cellof(p);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  return lj_arena_state(a, cell);
}

static int arena_list_contains(GCArena *a, GCArena *needle)
{
  while (a) {
    if (a == needle)
      return 1;
    a = lj_arena_next_acq(a);
  }
  return 0;
}

static int noop_finalizer(lua_State *L)
{
  (void)L;
  return 0;
}

static void seed_traversable_needsweep(TGState *tg, uint32_t n)
{
  uint32_t i;
  for (i = 0; i < n; i++) {
    GCArena *a = lj_arena_map(&tg->prng, LJ_AF_TRAVERSABLE);
    assert(a != NULL);
    a->hdr.owner_tid = tg->alloc.owner_tid;
    a->hdr.flags |= LJ_AF_NEEDSWEEP;
    lj_arena_next_rel(a, tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE]);
    tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] = a;
  }
}

static void test_worker_owned_sweep_direct(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg, extra_tg;
  uint64_t worker_runs0, arenas0, idle0;
  uint32_t sweep_cycle;
  void *extra_plain, *extra_trav;
  GCArena *extra_plain_a, *extra_trav_a, *swept_a;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  lua_gc(L, LUA_GCCOLLECT, 0);

  lj_tg_init_thread(g, &extra_tg, NULL, 1);
  extra_tg.tid = tg->tid + 3000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  extra_tg.cur_L = L;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);

  extra_plain = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64, 0);
  extra_trav = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			      LJ_AF_TRAVERSABLE);
  assert(extra_plain != NULL);
  assert(extra_trav != NULL);
  extra_plain_a = lj_arena_of(extra_plain);
  extra_trav_a = lj_arena_of(extra_trav);

  g->gc2.cycle++;
  sweep_cycle = g->gc2.cycle;
  g->gc2.phase = LJ_GC2_SWEEP;
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN);
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN],
			     extra_plain_a));
  assert(arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     extra_trav_a));
  swept_a = extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE];
  assert(swept_a != NULL);
  assert(lj_gc2_sweep_tg_ready(&extra_tg));
  assert(lj_gc2_sweep_needs_prepare(g));
  assert(lj_gc2_sweep_pending(g));

  worker_runs0 = la_load64_acq(&g->gc2.worker_runs);
  arenas0 = la_load64_acq(&g->gc2.sweep_owner_arenas);
  assert(lj_gc2_worker_drain(g, 1) == 1u);
  assert(la_load64_acq(&g->gc2.worker_runs) == worker_runs0 + 1u);
  assert(la_load64_acq(&g->gc2.sweep_owner_arenas) == arenas0 + 1u);
  assert(la_load32_acq(&g->gc2.worker_active) == 0);
  assert(!arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			      swept_a));
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_TRAVERSABLE],
			     swept_a));
  assert((extra_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((swept_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert(swept_a->hdr.sweep_epoch == sweep_cycle);
  assert(!lj_gc2_sweep_pending(g));
  idle0 = la_load64_acq(&g->gc2.worker_idle_declares);
  assert(lj_gc2_worker_drain(g, 1) == 0);
  assert(la_load64_acq(&g->gc2.worker_idle_declares) == idle0 + 1u);
  assert(la_load32_acq(&g->gc2.worker_active) == 0);

  lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  lj_gc2_legacy_cycle_end(g);
  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
  lua_close(L);
}

static void test_minor_sweep_identity_direct(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg, extra_tg;
  uint64_t minor_arenas0;
  uint32_t sweep_cycle;
  void *dead, *live;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  lua_gc(L, LUA_GCCOLLECT, 0);

  lj_tg_init_thread(g, &extra_tg, NULL, 1);
  extra_tg.tid = tg->tid + 3500u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  extra_tg.cur_L = L;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);

  extra_tg.alloc.alloc_black = 0;
  dead = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			LJ_AF_TRAVERSABLE);
  extra_tg.alloc.alloc_black = 1;
  live = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			LJ_AF_TRAVERSABLE);
  extra_tg.alloc.alloc_black = 0;
  assert(dead != NULL);
  assert(live != NULL);
  assert(lj_arena_of(dead) == lj_arena_of(live));
  assert(ptr_state(dead) == 2);
  assert(ptr_state(live) == 3);

  g->gc2.cycle++;
  sweep_cycle = g->gc2.cycle;
  g->gc2.phase = LJ_GC2_SWEEP;
  la_store32_rel(&g->gc2.cycle_minor_requested, 1);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  la_store32_rel(&g->gc2.cycle_sweep_minor, 1);
  lj_arena_alloc_prepare_sweep_kind(&extra_tg.alloc, LJ_ARENAK_TRAVERSABLE);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL);
  minor_arenas0 = la_load64_acq(&g->gc2.minor_sweep_arenas);
  assert(lj_gc2_sweep_owner_progress(g, &extra_tg, 1) == 1u);
  assert(la_load64_acq(&g->gc2.minor_sweep_arenas) == minor_arenas0 + 1u);
  assert(ptr_state(dead) == 1);
  assert(ptr_state(live) == 3);
  assert(lj_arena_of(live)->hdr.sweep_epoch == sweep_cycle);

  lj_gc2_legacy_cycle_end(g);
  la_store32_rel(&g->gc2.cycle_minor_requested, 0);
  la_store32_rel(&g->gc2.cycle_sweep_minor, 0);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 0);
  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
  lua_close(L);
}

static void test_boundary_lazy_sweep(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCRef empty;
  MSize oldstepmul;
  uint64_t arenas0, delta;
  uint32_t oldcycle, i;
  const uint32_t seeded = LJ_GC2_SWEEP_BATCH + 3u;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  lua_gc(L, LUA_GCCOLLECT, 0);

  oldcycle = g->gc2.cycle;
  g->gc2.cycle = oldcycle + 1u;
  g->gc2.phase = LJ_GC2_SWEEP;
  tg->alloc.sweep_epoch = g->gc2.cycle;  /* Simulate prepared boundary. */
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);

  seed_traversable_needsweep(tg, seeded);
  assert(lj_gc2_sweep_pending(g));

  setgcrefnull(empty);
  setmref(g->gc.sweep, &empty);
  g->gc.state = GCSsweep;
  oldstepmul = g->gc.stepmul;
  g->gc.stepmul = 1;
  arenas0 = la_load64_acq(&g->gc2.sweep_owner_arenas);

  (void)lj_gc_step(L);
  delta = la_load64_acq(&g->gc2.sweep_owner_arenas) - arenas0;
  assert(delta > 0);
  assert(delta <= LJ_GC2_SWEEP_BATCH);
  assert(g->gc.state == GCSsweep);
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(lj_gc2_sweep_pending(g));

  for (i = 0; i < seeded + 4u && g->gc.state != GCSpause; i++)
    (void)lj_gc_step(L);
  assert(g->gc.state == GCSpause);
  assert(tg->alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(tg->alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(!lj_gc2_sweep_pending(g));
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.stepmul = oldstepmul;
  lua_close(L);
}

static void test_boundary_lazy_sweep_extra_tg(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg, extra_tg;
  GCRef empty;
  MSize oldstepmul;
  uint64_t arenas0, delta;
  uint32_t oldcycle, sweep_cycle, i;
  void *extra_plain, *extra_trav;
  GCArena *extra_plain_a, *extra_trav_a;
  const uint32_t seeded = LJ_GC2_SWEEP_BATCH + 2u;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  lua_gc(L, LUA_GCCOLLECT, 0);

  lj_tg_init_thread(g, &extra_tg, NULL, 1);
  extra_tg.tid = tg->tid + 2000u;
  extra_tg.alloc.owner_tid = extra_tg.tid;
  extra_tg.cur_L = L;
  lj_native_enter(&extra_tg);
  lj_tg_attach(g, &extra_tg);
  assert(g->gc2.n_threads == 2);

  extra_plain = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64, 0);
  extra_trav = lj_arena_alloc(&extra_tg.alloc, &extra_tg.prng, 64,
			      LJ_AF_TRAVERSABLE);
  assert(extra_plain != NULL);
  assert(extra_trav != NULL);
  extra_plain_a = lj_arena_of(extra_plain);
  extra_trav_a = lj_arena_of(extra_trav);
  assert(extra_plain_a->hdr.owner_tid == extra_tg.alloc.owner_tid);
  assert(extra_trav_a->hdr.owner_tid == extra_tg.alloc.owner_tid);

  oldcycle = g->gc2.cycle;
  g->gc2.cycle = oldcycle + 1u;
  sweep_cycle = g->gc2.cycle;
  g->gc2.phase = LJ_GC2_SWEEP;
  assert(lj_gc2_handshake(g, LJ_GC2_HS_RESET_ALLOC) == 2);
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN],
			     extra_plain_a));
  assert(arena_list_contains(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE],
			     extra_trav_a));
  assert((extra_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((extra_trav_a->hdr.flags & LJ_AF_NEEDSWEEP) != 0);
  seed_traversable_needsweep(&extra_tg, seeded);
  assert(!lj_gc2_sweep_needs_prepare(g));
  assert(lj_gc2_sweep_pending(g));

  setgcrefnull(empty);
  setmref(g->gc.sweep, &empty);
  g->gc.state = GCSsweep;
  oldstepmul = g->gc.stepmul;
  g->gc.stepmul = 1;
  arenas0 = la_load64_acq(&g->gc2.sweep_owner_arenas);

  (void)lj_gc_step(L);
  delta = la_load64_acq(&g->gc2.sweep_owner_arenas) - arenas0;
  assert(delta > 0);
  assert(delta <= LJ_GC2_SWEEP_BATCH);
  assert(g->gc.state == GCSsweep);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL);
  assert(lj_gc2_sweep_pending(g));

  for (i = 0; i < seeded + 4u && g->gc.state != GCSpause; i++)
    (void)lj_gc_step(L);
  assert(g->gc.state == GCSpause);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE] == NULL);
  assert(extra_tg.alloc.needsweep[LJ_ARENAK_PLAIN] == NULL);
  assert(!lj_gc2_sweep_needs_prepare(g));
  assert(!lj_gc2_sweep_pending(g));
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN],
			     extra_plain_a));
  assert(arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_TRAVERSABLE],
			     extra_trav_a));
  assert((extra_plain_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert((extra_trav_a->hdr.flags & LJ_AF_NEEDSWEEP) == 0);
  assert(extra_trav_a->hdr.sweep_epoch == sweep_cycle);
  assert(extra_tg.in_native == 1);

  g->gc.stepmul = oldstepmul;
  lj_tg_detach(g, &extra_tg);
  assert(g->gc2.n_threads == 1);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, &extra_tg);
  lua_close(L);
}

static void test_sweep_to_idle_worker_active(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  GCRef empty;
  uint64_t sweep_to_idle0;

  assert(L != NULL);
  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  lua_gc(L, LUA_GCCOLLECT, 0);

  g->gc2.cycle++;
  g->gc2.phase = LJ_GC2_SWEEP;
  tg->alloc.sweep_epoch = g->gc2.cycle;
  setgcrefnull(empty);
  setmref(g->gc.sweep, &empty);
  g->gc.state = GCSsweep;
  sweep_to_idle0 = la_load64_acq(&g->gc2.sweep_to_idle);

  la_store32_rel(&g->gc2.worker_active, 1);
  (void)lj_gc_step(L);
  assert(g->gc.state == GCSsweep);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_SWEEP);
  assert(la_load64_acq(&g->gc2.sweep_to_idle) == sweep_to_idle0);

  la_store32_rel(&g->gc2.worker_active, 0);
  (void)lj_gc_step(L);
  assert(g->gc.state == GCSpause);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_IDLE);
  assert(la_load64_acq(&g->gc2.sweep_to_idle) == sweep_to_idle0 + 1u);

  setmref(g->gc.sweep, &g->gc.root);
  lua_close(L);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  TValue *tv;
  GCtab *keep, *arrtab, *deadtab, *deadcolo, *deadsplit;
  GCfunc *fn, *deadchunk, *deadfn, *livefn, *deadcf, *finchunk, *finfn;
  GCfunc *bcfn, *hugefn;
  GCproto *deadpt, *deadfnpt;
  GCproto *bcpt, *hugept, *finpt;
  GCSize before_tab, deadtab_size, deadarr_size, deadnode_size;
  GCSize before_colo, deadcolo_size;
  GCSize before_split, deadsplit_size, splitarr_size, splitnode_size;
  GCSize before_fn, deadfn_size, before_drop, deadpt_size, deadchunk_size;
  GCSize before_raw, before_cf, deadcf_size;
  GCSize before_bc, bcfn_size, bcpt_size, before_huge, hugefn_size;
  GCSize hugept_size;
  GCSize before_fin, finpt_size, finchunk_size, finfn_size;
  uint64_t sweep_owner_runs0, sweep_owner_arenas0, sweep_owner_live0;
  uint64_t huge_live_bytes;
  uint32_t sweep_epoch0;
  void *raw, *deadarr, *deadnode, *splitarr, *splitnode;
  LJHugeInfo hugehi;
  GCArena *fna, *arra;

  assert(L != NULL);
  luaL_openlibs(L);
  assert(luaL_dostring(L,
    "keep = {}\n"
    "keep.f = function(x) return x + 1 end\n"
    "keep.dead = loadstring('return 42')\n"
    "keep.parent = loadstring('return function(x) return x + 7 end')\n"
    "keep.deadfn = keep.parent()\n"
    "keep.livefn = keep.parent()\n"
    "keep.arr = {}\n"
    "for i = 1, 300 do keep.arr[i] = i end\n"
    "keep.deadtab = {}\n"
    "for i = 1, 300 do keep.deadtab[i] = i end\n"
    "for i = 1, 80 do keep.deadtab['k'..i] = i end\n"
    "keep.deadcolo = {10, 20, 30}\n"
    "keep.deadsplit = {1, 2, 3}\n"
    "for i = 4, 80 do keep.deadsplit[i] = i end\n"
    "collectgarbage('collect')\n") == LUA_OK);

  g = G(L);
  tg = G2TG(g);
  assert(tg != NULL);
  assert((tg->tg_flags & TGF_ARENA_INTERNAL) != 0);
  assert(tg->mark_active == 0);
  assert(tg->alloc.alloc_black == 0);
  assert(tg->alloc.sweep_epoch != 0);

  lua_getglobal(L, "keep");
  tv = L->top - 1;
  assert(tvistab(tv));
  keep = tabV(tv);
  assert((lj_arena_of(keep)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(keep) == 2);
  assert(lj_arena_of(keep)->hdr.sweep_epoch == tg->alloc.sweep_epoch);

  lua_getfield(L, -1, "f");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  fna = lj_arena_of(fn);
  assert((fna->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(fn) == 2);
  assert(fna->hdr.sweep_epoch == tg->alloc.sweep_epoch);
  L->top--;

  lua_getfield(L, -1, "dead");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  assert(isluafunc(fn));
  deadchunk = fn;
  deadchunk_size = sizeLfunc((MSize)deadchunk->l.nupvalues);
  deadpt = funcproto(fn);
  deadpt_size = deadpt->sizept;
  assert(ptr_state(deadchunk) == 2);
  assert(ptr_state(deadpt) == 2);
  L->top--;

  lua_getfield(L, -1, "deadfn");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  deadfn = funcV(tv);
  assert(isluafunc(deadfn));
  deadfn_size = sizeLfunc((MSize)deadfn->l.nupvalues);
  deadfnpt = funcproto(deadfn);
  assert(ptr_state(deadfn) == 2);
  assert(ptr_state(deadfnpt) == 2);
  L->top--;

  lua_getfield(L, -1, "livefn");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  livefn = funcV(tv);
  assert(isluafunc(livefn));
  assert(funcproto(livefn) == deadfnpt);
  assert(ptr_state(livefn) == 2);
  L->top--;

  lua_getfield(L, -1, "arr");
  tv = L->top - 1;
  assert(tvistab(tv));
  arrtab = tabV(tv);
  assert(arrtab->asize > 0);
  assert(arrtab->colo <= 0);
  arra = lj_arena_of(lj_tab_array_hdrw(lj_tab_array_acq(arrtab)));
  assert((arra->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert(arra->hdr.sweep_epoch == 0);
  assert(ptr_state(lj_tab_array_hdrw(lj_tab_array_acq(arrtab))) == 3);
  L->top--;

  lua_getfield(L, -1, "deadtab");
  tv = L->top - 1;
  assert(tvistab(tv));
  deadtab = tabV(tv);
  assert(deadtab->asize > 0);
  assert(deadtab->hmask > 0);
  assert(deadtab->colo <= 0);
  deadtab_size = sizeof(GCtab);
  deadarr = lj_tab_array_hdrw(lj_tab_array_acq(deadtab));
  deadarr_size = lj_tab_array_bytes(deadtab->acap);
  deadnode = lj_tab_node_hdrw(lj_tab_node_acq(deadtab));
  deadnode_size = lj_tab_node_bytes(deadtab->hmask);
  assert((lj_arena_of(deadtab)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert((lj_arena_of(deadarr)->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert((lj_arena_of(deadnode)->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert(ptr_state(deadtab) == 2);
  assert(ptr_state(deadarr) == 3);
  assert(ptr_state(deadnode) == 3);
  L->top--;

  sweep_owner_runs0 = la_load64_acq(&g->gc2.sweep_owner_runs);
  sweep_owner_arenas0 = la_load64_acq(&g->gc2.sweep_owner_arenas);
  sweep_owner_live0 = la_load64_acq(&g->gc2.sweep_owner_live_cells);
  sweep_epoch0 = tg->alloc.sweep_epoch;
  before_tab = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadtab");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(la_load64_acq(&g->gc2.sweep_owner_runs) > sweep_owner_runs0);
  assert(la_load64_acq(&g->gc2.sweep_owner_arenas) > sweep_owner_arenas0);
  assert(la_load64_acq(&g->gc2.sweep_owner_live_cells) >= sweep_owner_live0);
  assert(tg->alloc.sweep_epoch > sweep_epoch0);
  assert(g->gc.total <=
	 before_tab - deadtab_size - deadarr_size - deadnode_size);
  assert((ptr_state(deadtab) & 2u) == 0);
  assert((ptr_state(deadarr) & 2u) == 0);
  assert((ptr_state(deadnode) & 2u) == 0);

  lua_getfield(L, -1, "deadcolo");
  tv = L->top - 1;
  assert(tvistab(tv));
  deadcolo = tabV(tv);
  assert(deadcolo->asize > 0);
  assert(deadcolo->hmask == 0);
  assert(deadcolo->colo > 0);
  deadcolo_size = sizetabcolo((uint32_t)deadcolo->colo & 0x7f);
  assert((lj_arena_of(deadcolo)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(deadcolo) == 2);
  L->top--;

  before_colo = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadcolo");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_colo - deadcolo_size);
  assert((ptr_state(deadcolo) & 2u) == 0);

  lua_getfield(L, -1, "deadsplit");
  tv = L->top - 1;
  assert(tvistab(tv));
  deadsplit = tabV(tv);
  assert(deadsplit->asize > 0);
  assert(deadsplit->colo < 0);
  assert(deadsplit->asize > ((uint32_t)deadsplit->colo & 0x7f));
  deadsplit_size = sizetabcolo((uint32_t)deadsplit->colo & 0x7f);
  splitarr = lj_tab_array_hdrw(lj_tab_array_acq(deadsplit));
  splitarr_size = lj_tab_array_bytes(deadsplit->acap);
  splitnode_size = deadsplit->hmask > 0 ?
		   lj_tab_node_bytes(deadsplit->hmask) : 0;
  splitnode = deadsplit->hmask > 0 ?
	      (void *)lj_tab_node_hdrw(lj_tab_node_acq(deadsplit)) : NULL;
  assert((lj_arena_of(deadsplit)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert((lj_arena_of(splitarr)->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
  assert(ptr_state(deadsplit) == 2);
  assert(ptr_state(splitarr) == 3);
  if (splitnode) {
    assert((lj_arena_of(splitnode)->hdr.flags & LJ_AF_TRAVERSABLE) == 0);
    assert(ptr_state(splitnode) == 3);
  }
  L->top--;

  before_split = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadsplit");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <=
	 before_split - deadsplit_size - splitarr_size - splitnode_size);
  assert((ptr_state(deadsplit) & 2u) == 0);
  assert((ptr_state(splitarr) & 2u) == 0);
  if (splitnode)
    assert((ptr_state(splitnode) & 2u) == 0);

  before_fn = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadfn");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_fn - deadfn_size);
  assert((ptr_state(deadfn) & 2u) == 0);
  assert(ptr_state(deadfnpt) == 2);
  assert(ptr_state(livefn) == 2);

  before_raw = g->gc.total;
  raw = lj_mem_newgco_raw(L, 64, LJ_AF_TRAVERSABLE);
  assert(g->gc.total == before_raw + 64);
  assert(ptr_state(raw) == 2);
  assert(lj_mem_freegco_defer(g, raw, 64) == 1);
  assert(g->gc.total == before_raw);
  assert(ptr_state(raw) == 2);

  before_drop = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "dead");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_drop - deadpt_size - deadchunk_size);
  assert((ptr_state(deadchunk) & 2u) == 0);
  assert((ptr_state(deadpt) & 2u) == 0);
  assert(ptr_state(raw) == 1);

  lua_getfield(L, -1, "arr");
  lua_pushcclosure(L, noop_finalizer, 1);
  tv = L->top - 1;
  assert(tvisfunc(tv));
  deadcf = funcV(tv);
  assert(!isluafunc(deadcf));
  assert(deadcf->c.nupvalues == 1);
  deadcf_size = sizeCfunc((MSize)deadcf->c.nupvalues);
  assert(tvistab(&deadcf->c.upvalue[0]));
  assert(tabV(&deadcf->c.upvalue[0]) == arrtab);
  assert((lj_arena_of(deadcf)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(deadcf) == 2);
  lua_setfield(L, -2, "deadcf");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(ptr_state(deadcf) == 2);

  before_cf = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadcf");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_cf - deadcf_size);
  assert((ptr_state(deadcf) & 2u) == 0);
  assert(ptr_state(arrtab) == 2);

  assert(luaL_dostring(L,
    "do\n"
    "  local f = assert(loadstring('return function(y) return y * 9 end'))()\n"
    "  keep.bcblob = string.dump(f)\n"
    "  keep.bcdead = assert(loadstring(keep.bcblob))\n"
    "end\n"
    "collectgarbage('collect')\n") == LUA_OK);
  lua_getfield(L, -1, "bcdead");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  bcfn = funcV(tv);
  assert(isluafunc(bcfn));
  bcfn_size = sizeLfunc((MSize)bcfn->l.nupvalues);
  bcpt = funcproto(bcfn);
  bcpt_size = bcpt->sizept;
  assert((lj_arena_of(bcpt)->hdr.flags & LJ_AF_TRAVERSABLE) != 0);
  assert(ptr_state(bcfn) == 2);
  assert(ptr_state(bcpt) == 2);
  L->top--;

  before_bc = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "bcdead");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_bc - bcpt_size - bcfn_size);
  assert((ptr_state(bcfn) & 2u) == 0);
  assert((ptr_state(bcpt) & 2u) == 0);

  assert(luaL_dostring(L,
    "do\n"
    "  local t = {'return function() local x = 0\\n'}\n"
    "  for i = 1, 6000 do t[#t+1] = 'x = x + 1\\n' end\n"
    "  t[#t+1] = 'return x end'\n"
    "  keep.huge = assert(loadstring(table.concat(t)))()\n"
    "end\n"
    "collectgarbage('collect')\n") == LUA_OK);
  lua_getfield(L, -1, "huge");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  hugefn = funcV(tv);
  assert(isluafunc(hugefn));
  hugefn_size = sizeLfunc((MSize)hugefn->l.nupvalues);
  hugept = funcproto(hugefn);
  hugept_size = hugept->sizept;
  assert(hugept_size > LJ_HUGE_THRESHOLD);
  assert(lj_arena_ishuge(lj_arena_of(hugept)));
  assert(lj_arena_hugetab_lookup(&tg->huge, hugept, &hugehi) == 1);
  assert(hugehi.size == hugept_size);
  assert((hugehi.flags & LJ_HUGEF_TRAVERSABLE) != 0);
  assert((hugehi.flags & LJ_HUGEF_MARK) != 0);
  huge_live_bytes = la_load64_acq(&g->gc2.sweep_live_huge_bytes);
  assert(huge_live_bytes >= hugept_size);
  assert(la_load64_acq(&g->gc2.live_estimate) >= huge_live_bytes);
  assert(ptr_state(hugefn) == 2);
  L->top--;

  before_huge = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "huge");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_huge - hugept_size - hugefn_size);
  assert((ptr_state(hugefn) & 2u) == 0);
  assert(lj_arena_hugetab_lookup(&tg->huge, hugept, NULL) == 0);

  assert(luaL_dostring(L,
    "keep.deadfin = loadstring('return 43')\n"
    "keep.deadfinfn = keep.parent()\n") ==
	 LUA_OK);
  lua_getfield(L, -1, "deadfin");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  fn = funcV(tv);
  assert(isluafunc(fn));
  finchunk = fn;
  finchunk_size = sizeLfunc((MSize)finchunk->l.nupvalues);
  finpt = funcproto(fn);
  finpt_size = finpt->sizept;
  assert(ptr_state(finchunk) == 2);
  assert(ptr_state(finpt) == 2);
  L->top--;

  lua_getfield(L, -1, "deadfinfn");
  tv = L->top - 1;
  assert(tvisfunc(tv));
  finfn = funcV(tv);
  assert(isluafunc(finfn));
  finfn_size = sizeLfunc((MSize)finfn->l.nupvalues);
  assert(funcproto(finfn) == deadfnpt);
  assert(ptr_state(finfn) == 2);
  L->top--;

  lua_newuserdata(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, noop_finalizer);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_setfield(L, -2, "ud");

  before_fin = g->gc.total;
  lua_pushnil(L);
  lua_setfield(L, -2, "deadfin");
  lua_pushnil(L);
  lua_setfield(L, -2, "deadfinfn");
  lua_pushnil(L);
  lua_setfield(L, -2, "ud");
  lua_gc(L, LUA_GCCOLLECT, 0);
  assert(g->gc.total <= before_fin - finpt_size - finchunk_size - finfn_size);
  assert((ptr_state(finchunk) & 2u) == 0);
  assert((ptr_state(finfn) & 2u) == 0);
  assert((ptr_state(finpt) & 2u) == 0);

  lua_close(L);
  test_sweep_to_idle_worker_active();
  test_worker_owned_sweep_direct();
  test_minor_sweep_identity_direct();
  test_boundary_lazy_sweep();
  test_boundary_lazy_sweep_extra_tg();
  printf("t-arena-gcsweep OK: traversable runtime sweep bridge verified\n");
  return 0;
}
