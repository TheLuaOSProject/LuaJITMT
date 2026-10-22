/* Successfully preserved SWEEP leaves must not create graph work. */
#ifndef LJ_GC2_TEST_HELPERS
#error "t-gc2-sweep-leaf-publication requires LJ_GC2_TEST_HELPERS"
#endif

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_tab.h"
#include "lj_tg.h"
#if LJ_HASFFI
#include "lj_cdata.h"
#endif

typedef struct Fixture {
  lua_State *L;
  global_State *g;
  TGState *tg;
  TValue leaf[7];
  uint32_t count;
  LJGC2ActivationSnap sweep;
} Fixture;

static uint32_t finalized;

#if LJ_HASFFI
static int leaf_finalizer(lua_State *L)
{
  UNUSED(L);
  finalized++;
  return 0;
}
#endif

static void drain(Fixture *f)
{
  uint32_t turn;
  (void)lj_gc2_flush_ssb(f->g, f->tg);
  for (turn = 0; turn < 64u && !lj_gc2_test_ssb_empty(f->g); turn++)
    (void)lj_gc2_test_ssb_drain(f->g);
  assert(lj_gc2_test_ssb_empty(f->g));
  assert(gc2_recovery_items_acq(f->g) == 0);
  assert(gc2_recovery_failed_acq(f->g) == 0);
}

static Fixture fixture_open(void)
{
  Fixture f;
  char *large;
  size_t len = LJ_HUGE_THRESHOLD + 4096u;
  uint32_t i;
  memset(&f, 0, sizeof(f));
  finalized = 0;
  f.L = luaL_newstate();
  assert(f.L != NULL);
  f.g = G(f.L);
  f.tg = G2TG(f.g);
  assert(f.tg != NULL);
  lua_gc(f.L, LUA_GCSTOP, 0);
  lua_pushliteral(f.L, "gc2 SWEEP small leaf publication");
  large = (char *)malloc(len);
  assert(large != NULL);
  memset(large, 's', len);
  lua_pushlstring(f.L, large, len);
  free(large);
  f.count = 2;
#if LJ_HASFFI
  luaL_openlibs(f.L);
  lua_pushcfunction(f.L, leaf_finalizer);
  lua_setglobal(f.L, "sweep_leaf_finalizer");
  assert(luaL_dostring(f.L,
    "local ffi = require('ffi'); return ffi.new('int64_t'), "
    "ffi.new('uint8_t[?]', 96), ffi.new('uint8_t[?]', 32768), "
    "ffi.new('struct { char __attribute__((aligned(8192))) a; }'), "
    "ffi.gc(ffi.new('int[1]'), sweep_leaf_finalizer)") == LUA_OK);
  f.count = 7;
#endif
  assert(lua_gettop(f.L) == (int)f.count);
  for (i = 0; i < f.count; i++)
    copyTV(f.L, &f.leaf[i], f.L->base + i);
  return f;
}

static void enter_sweep(Fixture *f)
{
  LJGC2ActivationSnap mark, weak;
  /* Remove constructor/startup SSB work while all fixture objects are rooted.
  ** The new synthetic cycle leaves them white without a broad stack scan. */
  assert(lua_gc(f->L, LUA_GCCOLLECT, 0) == 0);
  assert(lua_gc(f->L, LUA_GCSTOP, 0) == 0);
  lj_gc2_mark_begin(f->g);
  drain(f);
  mark = lj_gc2_activation_snapshot(&f->g->gc2.activation);
  assert(mark.state == LJ_GC2_ACT_MARK);
  assert(lj_gc2_activation_try_transition(&f->g->gc2.activation, &mark,
    mark.mark_epoch, LJ_GC2_ACT_WEAK, &weak) == LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(f->g, LJ_GC2_WEAK);
  assert(lj_gc2_activation_try_transition(&f->g->gc2.activation, &weak,
    mark.mark_epoch, LJ_GC2_ACT_SWEEP_OPEN, &f->sweep) ==
    LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(f->g, LJ_GC2_SWEEP);
  gc2_jit_phase_gate_rel(f->g, 0);
}

static void fixture_close(Fixture *f)
{
  lj_gc2_cycle_to_idle(f->g);
  lua_close(f->L);
#if LJ_HASFFI
  assert(finalized == 1u);
#endif
}

static void assert_leaf_mark(Fixture *f, cTValue *tv, int marked)
{
  GCobj *o = gcV(tv);
  void *base = o;
  GCArena *a;
#if LJ_HASFFI
  if (tviscdata(tv)) {
    GCSize size;
    assert(lj_cdata_validate(f->g, cdataV(tv), &base, &size));
  }
#endif
  assert(lj_gc2_ismarked(f->g, o) == marked);
  a = lj_arena_of(base);
  if (lj_arena_ishuge(a)) {
    LJHugeInfo hi;
    assert(lj_arena_hugetab_lookup(&f->tg->huge, base, &hi));
    assert(hi.readers == 0);
    assert(((hi.flags & LJ_HUGEF_MARK) != 0) == marked);
    assert(hi.flags & LJ_HUGEF_TRAVERSABLE);
  } else {
    assert(lj_arena_flags_acq(a) & LJ_AF_TRAVERSABLE);
    assert((int)lj_arena_bm_get(a->mark, lj_arena_cellof(base)) == marked);
    assert((lj_arena_remote_active_acq(a) & LJ_ARENA_REMOTE_COUNT_MASK) == 0);
  }
}

static void test_public_leaves(uint32_t api)
{
  Fixture f = fixture_open();
  GCRef *next;
  uint64_t grey, published;
  uint32_t i, round;
  enter_sweep(&f);
  for (i = 0; i < f.count; i++)
    assert_leaf_mark(&f, &f.leaf[i], 0);
  next = lj_tg_ssb_next_acq(f.tg);
  grey = gc2_grey_pushed_acq(f.g);
  published = gc2_ssb_items_published_acq(f.g);
  /* Repeat beyond active-buffer capacity. An already marked leaf must not
  ** eventually rotate an SSB or overflow into an allocation recovery lane. */
  for (round = 0; round <= TG_GC2_SSB_SLOTS; round++) {
    for (i = 0; i < f.count; i++) {
      TValue *tv = &f.leaf[i];
      uint32_t queued = 0;
      if (api == 0)
        queued = lj_gc2_trace_sweep_root(f.g, gcV(tv));
      else if (api == 1)
        lj_gc2_barrier_tv_g(f.g, tv);
      else if (api == 2)
        lj_gc_pubroot(f.L, tv);
      else {
        LJGC2Lease held = { 0 };
        assert(lj_gc2_obj_lease_acquire(f.g, gcV(tv),
          (uint32_t)~itype(tv), NULL, &held) >= 0);
        lj_gc2_lease_release(&held);
      }
      assert_leaf_mark(&f, tv, 1);
      if (queued != 0 || lj_tg_ssb_next_acq(f.tg) != next)
        fprintf(stderr, "marked SWEEP leaf created graph work: "
                "api=%u leaf=%u round=%u queued=%u ssb_changed=%d\n",
                api, i, round, queued, lj_tg_ssb_next_acq(f.tg) != next);
      assert(queued == 0);
      assert(lj_tg_ssb_next_acq(f.tg) == next);
      assert(gc2_grey_pushed_acq(f.g) == grey);
      assert(gc2_ssb_items_published_acq(f.g) == published);
      assert(gc2_recovery_items_acq(f.g) == 0);
      assert(gc2_recovery_huge_items_acq(f.g) == 0);
      assert(gc2_recovery_failed_acq(f.g) == 0);
      assert(gc2_smr_readers_acq(f.g) == 0);
    }
  }
  assert(!lj_gc2_activation_reclaim_veto(f.g));
  fixture_close(&f);
}

static void test_graph_control(uint32_t kind)
{
  Fixture f = fixture_open();
  GCtab *parent, *child;
  GCobj *root;
  uint32_t i;
  int parent_index;
  lua_createtable(f.L, (int)f.count + 1, 0);
  parent = tabV(f.L->top - 1);
  parent_index = lua_gettop(f.L);
  for (i = 0; i < f.count; i++)
    copyTVrel(f.L, &lj_tab_array_acq(parent)[i], &f.leaf[i]);
  lua_createtable(f.L, 1, 0);
  child = tabV(f.L->top - 1);
  copyTVrel(f.L, &lj_tab_array_acq(parent)[f.count], f.L->top - 1);
  copyTVrel(f.L, &lj_tab_array_acq(child)[0], f.L->base + parent_index - 1);
  lua_pop(f.L, 1);
  root = obj2gco(parent);
  if (kind) {
    (void)lua_newuserdata(f.L, kind == 1 ? 8 : LJ_HUGE_THRESHOLD + 4096u);
    root = gcV(f.L->top - 1);
    lua_pushvalue(f.L, parent_index);
    assert(lua_setmetatable(f.L, -2));
  }
  enter_sweep(&f);
  assert(lj_gc2_ismarked(f.g, root) == 0);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)) == 0);
  assert(lj_gc2_trace_sweep_root(f.g, root) == 1u);
  assert(!lj_gc2_test_ssb_empty(f.g));
  drain(&f);
  assert(lj_gc2_ismarked(f.g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)) == 1);
  for (i = 0; i < f.count; i++)
    assert_leaf_mark(&f, &f.leaf[i], 1);
  assert(!lj_gc2_activation_reclaim_veto(f.g));
  fixture_close(&f);
}

static void test_transient_leaf(void)
{
  Fixture f = fixture_open();
  GCobj *o = gcV(&f.leaf[0]);
  GCArena *a = lj_arena_of(o);
  uint32_t cell = lj_arena_cellof(o);
  enter_sweep(&f);
  assert(lj_gc2_ismarked(f.g, o) == 0);
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_LIVE,
                                   LJ_ARENA_LIFETIME_MUTATING));
  lj_gc2_barrier_tv_g(f.g, &f.leaf[0]);
  /* Generic MUTATING has no recovery reservation. Failed leaf admission must
  ** still publish the fail-closed veto rather than being consumed as a leaf. */
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_failed_acq(f.g) == 1u);
  assert(lj_gc2_activation_reclaim_veto(f.g));
  assert(lj_arena_lifetime_state_cas(a, cell, LJ_ARENA_LIFETIME_MUTATING,
                                   LJ_ARENA_LIFETIME_LIVE));
  assert(lj_gc2_ismarked(f.g, o) == 0);
  fixture_close(&f);
}

int main(int argc, char **argv)
{
  uint32_t kind;
  int all = argc == 1;
  assert(argc <= 2);
  assert(all || strcmp(argv[1], "leaf") == 0 ||
         strcmp(argv[1], "graph") == 0 || strcmp(argv[1], "retry") == 0);
  if (all || strcmp(argv[1], "leaf") == 0)
    for (kind = 0; kind < 4u; kind++)
      test_public_leaves(kind);
  if (all || strcmp(argv[1], "graph") == 0)
    for (kind = 0; kind < 3u; kind++)
      test_graph_control(kind);
  if (all || strcmp(argv[1], "retry") == 0)
    test_transient_leaf();
  puts("t-gc2-sweep-leaf-publication OK: leaves mark without graph work; "
       "containers and transient edges retain their obligations");
  return 0;
}
