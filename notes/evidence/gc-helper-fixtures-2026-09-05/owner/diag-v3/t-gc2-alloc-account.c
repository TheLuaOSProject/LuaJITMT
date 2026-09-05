/*
** Focused test for GC2 allocation accounting bridge.
*/

#ifndef LJ_GC2_TEST_HELPERS
#define LJ_GC2_TEST_HELPERS
#endif

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_cdata.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_meta.h"
#include "lj_safepoint.h"
#include "lj_tab.h"
#include "lj_thr.h"
#include "lj_tg.h"
#if LJ_HASJIT
#include "lj_dispatch.h"
#include "lj_jit.h"
#endif


static void diagnose_phase(global_State *g, const char *name, unsigned line)
{
  TGState *tg=G2TG(g);
  fprintf(stderr,"snapshot %s line=%u phase=%u cycle=%u request=%u requests=%llu starts=%llu major=%llu force=%u threshold=%llu total=%llu debt=%llu hard=%llu checks=%llu assists=%llu sweep=%u/%u/%u mark_active=%u gen=%u minor=%u close_intent=%u worker=%u phase_gate=%u smr=%u filtered=%llu pushed=%llu\n",
    name,line,gc2_phase_acq(g),gc2_cycle_acq(g),gc2_cycle_leader_acq(g),
    (unsigned long long)gc2_cycle_requests_acq(g),(unsigned long long)gc2_cycle_starts_acq(g),
    (unsigned long long)gc2_major_cycle_starts_acq(g),la_load32_acq(&g->gc2.force_major),
    (unsigned long long)lj_gc_threshold_load(g),(unsigned long long)lj_gc_total_load(g),
    (unsigned long long)la_load64_acq(&g->gc2.alloc_since_trigger),(unsigned long long)la_load64_acq(&g->gc2.hard_bytes),
    (unsigned long long)gc2_interp_hard_checks_acq(g),(unsigned long long)gc2_assist_runs_acq(g),
    gc2_sweep_bridge_ready_acq(g),gc2_sweep_root_done_acq(g),gc2_sweep_grace_needed_acq(g),
    tg?lj_tg_mark_active_acq(tg):0,gc2_generational_acq(g),gc2_minor_sweep_enabled_acq(g),
    gc2_mark_close_intent_acq(g),gc2_worker_active_acq(g),gc2_jit_phase_gate_acq(g),gc2_smr_reclaiming_acq(g),
    (unsigned long long)gc2_remembered_filtered_acq(g),(unsigned long long)gc2_remembered_pushed_acq(g));
}

static void assert_late_attach_color(global_State *g, TGState *tg,
				     TGState *late_tg, uint32_t tid_offset,
				     uint32_t mark_active, uint8_t alloc_black)
{
  lj_tg_init_thread(g, late_tg, NULL, 0);
  late_tg->tid = tg->tid + tid_offset;
  if (late_tg->tid == 0 || late_tg->tid == LJ_THREAD_GCSCAN)
    late_tg->tid = tid_offset;
  late_tg->alloc.owner_tid = late_tg->tid;
  lj_tg_attach(g, late_tg);
  assert(la_load32_acq(&late_tg->mark_active) == mark_active);
  assert(la_load8_acq(&late_tg->alloc.alloc_black) == alloc_black);
  lj_tg_detach(g, late_tg);
  assert(lj_tg_reclaim_dead(g) == 1u);
  lj_tg_fini_thread(g, late_tg);
}

static void close_weak_for_sweep(lua_State *L, global_State *g)
{
  int complete = 0, i;
  for (i = 0; i < 128 && !complete; i++)
    complete = lj_gc2_weak_complete(g, L, NULL,
				    LJ_GC2_WEAK_DRAIN_BATCH);
  assert(complete);
}

static int root_contains(global_State *g, GCobj *target)
{
  GCobj *o;
  (void)lj_gc_flush_root_pending(g);
  for (o = gcref_acq(g->gc.root); o != NULL; o = lj_obj_gcw_acq(o))
    if (o == target)
      return 1;
  return 0;
}

static GCobj *active_ssb_last(TGState *tg)
{
  GCRef *base = (GCRef *)la_loadptr_acq((void *const *)&tg->ssb_base);
  GCRef *next = (GCRef *)la_loadptr_acq((void *const *)&tg->ssb_next);
  if (base == NULL || next == NULL || next <= base)
    return NULL;
  return gcref_acq(*(next - 1));
}

static void test_obj_valid_rejects_nonobject(global_State *g)
{
  GCobj *bad = (GCobj *)(uintptr_t)U64x(00004000,00000000);
  assert(lj_gc2_obj_valid(g, bad) == 0);
  assert(lj_gc2_obj_valid_queued(g, bad) == 0);
}

static void test_global_barrier_without_mark_active(lua_State *L,
					     global_State *g, TGState *tg)
{
  GCtab *parent, *mark_child, *weak_child;
  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  mark_child = tabV(L->top - 1);
  lua_newtable(L);
  weak_child = tabV(L->top - 1);

  lj_gc2_mark_begin(g);
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  assert(lj_gc2_ismarked(g, obj2gco(mark_child)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(weak_child)) == 0);
  lj_tg_mark_active_rel(tg, 0);

  lj_gc_barrierf(g, obj2gco(parent), obj2gco(mark_child));
  assert(lj_gc2_ismarked(g, obj2gco(mark_child)) == 1);

  lj_gc2_mark_to_weak(g);
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  lj_gc_barrierf(g, obj2gco(parent), obj2gco(weak_child));
  assert(lj_gc2_ismarked(g, obj2gco(weak_child)) == 1);

  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_finreg_entry_rejects_nonobject(lua_State *L,
						global_State *g)
{
  GCobj *bad = (GCobj *)(uintptr_t)U64x(00004000,00000000);
  TValue fin, out;
  uint64_t csets0 = gc2_finreg_cdata_sets_acq(g);
  uint64_t cclears0 = gc2_finreg_cdata_clears_acq(g);
  uint64_t cqueued0 = gc2_finreg_cdata_queued_acq(g);
  uint64_t cclaimed0 = gc2_finreg_cdata_pweak_claimed_acq(g);
  uint64_t cdispatched0 = gc2_finreg_cdata_preclaim_dispatched_acq(g);
  uint64_t usets0 = gc2_finreg_udata_sets_acq(g);
  uint64_t uclears0 = gc2_finreg_udata_clears_acq(g);
  uint64_t uqueued0 = gc2_finreg_udata_queued_acq(g);
  uint64_t uregistered0 = gc2_finreg_udata_registered_acq(g);
  uint64_t uforgets0 = gc2_finreg_udata_forgets_acq(g);
  setnilV(&fin);
  setnilV(&out);

  lj_gc2_finreg_cdata_set(g, bad, 1);
  lj_gc2_finreg_cdata_set(g, bad, 0);
  lj_gc2_test_finreg_cdata_finalizer_enqueue(g, bad);
  assert(lj_gc2_test_finreg_cdata_preclaim(L, g, bad, &fin) == 0);
  assert(lj_gc2_test_finreg_cdata_preclaim_take(L, g, bad, &out) == 0);
  assert(gc2_finreg_cdata_sets_acq(g) == csets0);
  assert(gc2_finreg_cdata_clears_acq(g) == cclears0);
  assert(gc2_finreg_cdata_queued_acq(g) == cqueued0);
  assert(gc2_finreg_cdata_pweak_claimed_acq(g) == cclaimed0);
  assert(gc2_finreg_cdata_preclaim_dispatched_acq(g) == cdispatched0);

  assert(lj_gc2_finreg_udata_set(g, bad, 1) == 0);
  assert(lj_gc2_finreg_udata_set(g, bad, 0) == 0);
  lj_gc2_finreg_udata_register(L, g, bad);
  lj_gc2_finreg_udata_forget(g, bad);
  lj_gc2_test_finreg_udata_queue(g, bad);
  assert(gc2_finreg_udata_sets_acq(g) == usets0);
  assert(gc2_finreg_udata_clears_acq(g) == uclears0);
  assert(gc2_finreg_udata_queued_acq(g) == uqueued0);
  assert(gc2_finreg_udata_registered_acq(g) == uregistered0);
  assert(gc2_finreg_udata_forgets_acq(g) == uforgets0);
}

static void test_pub_barrier_entry_rejects_nonobject(lua_State *L,
						     global_State *g)
{
  GCobj *bad = (GCobj *)(uintptr_t)U64x(00004000,00000000);
  TValue badtv;
  GCtab *parent;
  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  assert(parent != NULL);
  setgcVraw(&badtv, bad, LJ_TTAB);

  lj_gc_pubroot(L, &badtv);
  lj_gc_pubobjroot(L, bad);
  assert(root_contains(g, bad) == 0);

  lj_gc2_mark_begin(g);
  lj_gc2_barrier_tv_g(g, &badtv);
  lj_gc2_barrier_tv_pair_g(g, obj2gco(parent), &badtv);
  lj_gc2_barrier_tvn_pair_g(g, obj2gco(parent), &badtv, 1);
  lj_gc2_barrier_key_g(g, parent, &badtv);
  lj_gc_pubtabobj_vm(L, parent, bad);
  lj_gc_pubtabtvn_vm(L, parent, &badtv, 1);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);
}

static void test_obj_valid_accepts_variable_cdata(lua_State *L,
						  global_State *g)
{
#if LJ_HASFFI
  {
    static const char *chunks[] = {
      "local ffi = ffi\n"
      "local ct = ffi.typeof('double __attribute__((vector_size(16)))')\n"
      "local v = ct(1.5)\n"
      "return v",
      "local ffi = ffi; return ffi.new('uint8_t[?]', 32)",
      "local ffi = ffi; return ffi.new('uint8_t[?]', 70000)"
    };
    size_t i;
    int status;
    lua_pushcfunction(L, luaopen_ffi);
    lua_call(L, 0, 1);
    lua_setglobal(L, "ffi");
    for (i = 0; i < sizeof(chunks)/sizeof(chunks[0]); i++) {
      GCcdata *cd;
      GCArena *a;
      void *base;
      GCSize size;
      status = luaL_dostring(L, chunks[i]);
      if (status != LUA_OK)
	fprintf(stderr, "variable cdata chunk %u failed: %s\n", (unsigned)i,
		lua_tostring(L, -1));
      assert(status == LUA_OK);
      assert(tviscdata(L->top - 1));
      cd = cdataV(L->top - 1);
      assert(cdataisv(cd));
      assert(lj_cdata_validate(g, cd, &base, &size) == 1);
      assert(base != cd);
      a = lj_arena_of(base);
      if (size <= LJ_HUGE_THRESHOLD) {
	uint32_t start = lj_arena_cellof(base);
	assert(lj_arena_bm_get(a->block, start));
	assert(lj_arena_cdata_get(a, start));
      } else {
	LJHugeInfo hi;
	TGState *tg = L2TG(L);
	assert(lj_arena_hugetab_lookup(&tg->huge, base, &hi) == 1);
	assert(hi.size == size);
	assert((hi.flags & (LJ_HUGEF_TRAVERSABLE |
			    LJ_HUGEF_INTERIOR_CDATA)) ==
	       (LJ_HUGEF_TRAVERSABLE | LJ_HUGEF_INTERIOR_CDATA));
      }
      {
	int marked_before = lj_gc2_ismarked(g, obj2gco(cd));
        assert(lj_gc2_obj_valid(g, obj2gco(cd)) == 1);
        assert(lj_gc2_ismarked(g, obj2gco(cd)) == marked_before);
        assert(lj_gc2_obj_valid_queued(g, obj2gco(cd)) == 1);
        assert(lj_gc2_ismarked(g, obj2gco(cd)) == marked_before);
        assert(lj_gc2_markobj(g, obj2gco(cd)) == (marked_before == 0));
      }
      assert(lj_gc2_ismarked(g, obj2gco(cd)) == 1);
      setnilV(L->top - 1);
      lua_settop(L, 0);
    }
  }
#else
  UNUSED(L);
  UNUSED(g);
#endif
}

static void test_variable_cdata_reclamation(lua_State *L, global_State *g)
{
#if LJ_HASFFI
  static const char *chunks[] = {
    "local ffi = ffi; return ffi.new('uint8_t[?]', 32)",
    "local ffi = ffi; return ffi.new('uint8_t[?]', 70000)"
  };
  TGState *tg = L2TG(L);
  size_t i;
  for (i = 0; i < sizeof(chunks)/sizeof(chunks[0]); i++) {
    GCcdata *cd;
    void *base;
    GCSize size;
    int status = luaL_dostring(L, chunks[i]);
    int round;
    if (status != LUA_OK)
      fprintf(stderr, "variable cdata reclaim chunk %u failed: %s\n",
	      (unsigned)i, lua_tostring(L, -1));
    assert(status == LUA_OK);
    assert(tviscdata(L->top - 1));
    cd = cdataV(L->top - 1);
    assert(cdataisv(cd));
    assert(lj_cdata_validate(g, cd, &base, &size) == 1);

    /* A full collection while the stack still owns the cdata must preserve
    ** both its allocation metadata and its interior header. */
    (void)lua_gc(L, LUA_GCCOLLECT, 0);
    assert(lj_gc2_obj_valid(g, obj2gco(cd)) == 1);
    if (size <= LJ_HUGE_THRESHOLD) {
      GCArena *a = lj_arena_of(base);
      uint32_t start = lj_arena_cellof(base);
      assert(lj_arena_bm_get(a->block, start));
      assert(lj_arena_cdata_get(a, start));
      setnilV(L->top - 1);  /* Defeat deliberate full-stack conservative scan. */
      lua_settop(L, 0);
      for (round = 0; round < 16 &&
	   lj_arena_bm_get(a->block, start); round++)
	(void)lua_gc(L, LUA_GCCOLLECT, 0);
      assert(!lj_arena_bm_get(a->block, start));
      /* cdata[] is allocation coverage, intentionally retained until free-run
      ** selection/reuse; the cleared authoritative block start rejects this
      ** dead incarnation (see lj_arena_sweep_words()). */
    } else {
      LJHugeInfo hi;
      assert(lj_arena_hugetab_lookup(&tg->huge, base, &hi) == 1);
      assert((hi.flags & LJ_HUGEF_INTERIOR_CDATA) != 0);
      setnilV(L->top - 1);  /* Defeat deliberate full-stack conservative scan. */
      lua_settop(L, 0);
      for (round = 0; round < 16 &&
	   lj_arena_hugetab_lookup(&tg->huge, base, &hi) == 1; round++)
	(void)lua_gc(L, LUA_GCCOLLECT, 0);
      assert(lj_arena_hugetab_lookup(&tg->huge, base, &hi) == 0);
    }
  }
#else
  UNUSED(L);
  UNUSED(g);
#endif
}

static void test_public_minor_request_falls_back_major(lua_State *L,
						       global_State *g,
						       TGState *tg)
{
  uint64_t major0, minor_req0, minor_start0;

  UNUSED(tg);
  lua_settop(L, 0);
  lj_gc2_set_generational(g, 1);
  lua_gc(L, LUA_GCCOLLECT, 0);  /* Forced-major b1.2 fallback baseline. */
  assert(g->gc.state == GCSpause);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_IDLE);
  assert(la_load32_acq(&g->gc2.minor_sweep_enabled) == 0);
  assert(la_load32_acq(&g->gc2.minor_roots_enabled) == 0);
  assert(lj_gc2_test_ssb_empty(g));

  major0 = gc2_major_cycle_starts_acq(g);
  minor_req0 = gc2_minor_cycle_requests_acq(g);
  minor_start0 = gc2_minor_cycle_starts_acq(g);
  /*
  ** b1.2 accepts the request telemetry but runs the safe major root set.
  */
  lj_gc2_mark_begin(g);
  assert(gc2_major_cycle_starts_acq(g) == major0 + 1u);
  assert(gc2_minor_cycle_requests_acq(g) == minor_req0 + 1u);
  assert(gc2_minor_cycle_starts_acq(g) == minor_start0);
  assert(la_load32_acq(&g->gc2.cycle_roots_minor) == 0);
  lj_gc2_cycle_to_idle(g);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lj_gc2_set_generational(g, 0);
  lua_settop(L, 0);
}

static void test_vm_generational_table_store_remembered(lua_State *L,
							global_State *g,
							TGState *tg)
{
  GCtab *parent, *child;
  int status;
  uint64_t major_starts0, minor_requests0, minor_starts0;
  uint64_t remembered_barriers0, remembered_pushed0, remembered_filtered0;
  uint64_t remembered_drained0;

  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_IDLE);
  lua_settop(L, 0);
  status = luaL_dostring(L,
    "return function(t, v)\n"
    "  t[1] = v\n"
    "end\n");
  if (status != LUA_OK)
    fprintf(stderr, "vm generational setup failed: %s\n",
	    lua_tostring(L, -1));
  assert(status == LUA_OK);

  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lj_gc2_set_generational(g, 1);
  assert(la_load32_acq(&g->gc2.force_major) == 1);
  major_starts0 = gc2_major_cycle_starts_acq(g);
  minor_requests0 = gc2_minor_cycle_requests_acq(g);
  minor_starts0 = gc2_minor_cycle_starts_acq(g);
  lj_gc2_mark_begin(g);
  assert(gc2_major_cycle_starts_acq(g) == major_starts0 + 1u);
  assert(gc2_minor_cycle_requests_acq(g) == minor_requests0);
  assert(gc2_minor_cycle_starts_acq(g) == minor_starts0);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 0);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  (void)lj_gc2_flush_ssb(g, tg);
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  lj_gc2_cycle_to_idle(g);
  assert(la_load32_acq(&g->gc2.minor_sweep_enabled) == 0);
  assert(la_load32_acq(&g->gc2.minor_roots_enabled) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_test_ssb_empty(g));

  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  remembered_barriers0 = gc2_remembered_barriers_acq(g);
  remembered_pushed0 = gc2_remembered_pushed_acq(g);
  remembered_filtered0 = gc2_remembered_filtered_acq(g);
  lua_call(L, 2, 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(gc2_remembered_barriers_acq(g) >
	 remembered_barriers0);
  assert(gc2_remembered_pushed_acq(g) > remembered_pushed0);
  assert(gc2_remembered_filtered_acq(g) >= remembered_filtered0);
  assert(active_ssb_last(tg) == obj2gco(parent));
  lua_pop(L, 1);

  remembered_drained0 = gc2_remembered_drained_acq(g);
  major_starts0 = gc2_major_cycle_starts_acq(g);
  minor_requests0 = gc2_minor_cycle_requests_acq(g);
  minor_starts0 = gc2_minor_cycle_starts_acq(g);
  lj_gc2_mark_begin(g);
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_major_cycle_starts_acq(g) == major_starts0 + 1u);
  assert(gc2_minor_cycle_requests_acq(g) ==
	 minor_requests0 + 1u);
  assert(gc2_minor_cycle_starts_acq(g) == minor_starts0);
  assert(la_load32_acq(&g->gc2.cycle_roots_minor) == 0);
  assert(gc2_remembered_drained_acq(g) >=
	 remembered_drained0 + 1u);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_cycle_to_idle(g);

  la_store32_rel(&g->gc2.minor_sweep_enabled, 0);
  la_store32_rel(&g->gc2.minor_roots_enabled, 0);
  lj_gc2_set_generational(g, 0);
  lua_settop(L, 0);
}

#if LJ_HASJIT
static GCtrace *find_trace(global_State *g)
{
  jit_State *J = G2J(g);
  MSize i;
  for (i = 1; i < J->sizetrace; i++) {
    GCtrace *T = traceref(J, i);
    if (T != NULL)
      return T;
  }
  return NULL;
}

static void test_jit_generational_table_store_remembered(lua_State *L,
							 global_State *g,
							 TGState *tg)
{
  GCtab *parent, *child;
  int status;
  uint64_t major_starts0, minor_requests0, minor_starts0;
  uint64_t remembered_barriers0, remembered_pushed0, remembered_filtered0;
  uint64_t remembered_drained0;

  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_IDLE);
  lua_settop(L, 0);
  lua_pushcfunction(L, luaopen_jit);
  lua_pushliteral(L, LUA_JITLIBNAME);
  lua_call(L, 1, 0);
  status = luaL_dostring(L,
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "return function(t, v, n)\n"
    "  for i = 1, n do\n"
    "    if i > 1 then t[1] = v end\n"
    "  end\n"
    "end\n");
  if (status != LUA_OK)
    fprintf(stderr, "jit generational setup failed: %s\n",
	    lua_tostring(L, -1));
  assert(status == LUA_OK);

  lua_createtable(L, 1, 0);
  lua_newtable(L);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushinteger(L, 100);
  lua_call(L, 3, 0);
  assert(find_trace(g) != NULL);
  lua_pop(L, 2);

  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lj_gc2_set_generational(g, 1);
  assert(la_load32_acq(&g->gc2.force_major) == 1);
  major_starts0 = gc2_major_cycle_starts_acq(g);
  minor_requests0 = gc2_minor_cycle_requests_acq(g);
  minor_starts0 = gc2_minor_cycle_starts_acq(g);
  lj_gc2_mark_begin(g);
  assert(gc2_major_cycle_starts_acq(g) == major_starts0 + 1u);
  assert(gc2_minor_cycle_requests_acq(g) == minor_requests0);
  assert(gc2_minor_cycle_starts_acq(g) == minor_starts0);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 0);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  (void)lj_gc2_flush_ssb(g, tg);
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  lj_gc2_cycle_to_idle(g);
  assert(la_load32_acq(&g->gc2.minor_sweep_enabled) == 0);
  assert(la_load32_acq(&g->gc2.minor_roots_enabled) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_test_ssb_empty(g));

  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushinteger(L, 20);
  remembered_barriers0 = gc2_remembered_barriers_acq(g);
  remembered_pushed0 = gc2_remembered_pushed_acq(g);
  remembered_filtered0 = gc2_remembered_filtered_acq(g);
  lua_call(L, 3, 0);
  assert(find_trace(g) != NULL);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(gc2_remembered_barriers_acq(g) >
	 remembered_barriers0);
  assert(gc2_remembered_pushed_acq(g) > remembered_pushed0);
  assert(gc2_remembered_filtered_acq(g) >= remembered_filtered0);
  assert(active_ssb_last(tg) == obj2gco(parent));
  lua_pop(L, 1);

  remembered_drained0 = gc2_remembered_drained_acq(g);
  major_starts0 = gc2_major_cycle_starts_acq(g);
  minor_requests0 = gc2_minor_cycle_requests_acq(g);
  minor_starts0 = gc2_minor_cycle_starts_acq(g);
  lj_gc2_mark_begin(g);
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_major_cycle_starts_acq(g) == major_starts0 + 1u);
  assert(gc2_minor_cycle_requests_acq(g) ==
	 minor_requests0 + 1u);
  assert(gc2_minor_cycle_starts_acq(g) == minor_starts0);
  assert(la_load32_acq(&g->gc2.cycle_roots_minor) == 0);
  assert(gc2_remembered_drained_acq(g) >=
	 remembered_drained0 + 1u);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  lj_gc2_cycle_to_idle(g);

  la_store32_rel(&g->gc2.minor_sweep_enabled, 0);
  la_store32_rel(&g->gc2.minor_roots_enabled, 0);
  lj_gc2_set_generational(g, 0);
  lua_settop(L, 0);
}
#endif

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;
  TGState late_tg;
  void *p;
  uint64_t total, alloc_total0;
  uint64_t epoch0;
  uint64_t cycle_requests0, cycle_starts0;
  uint64_t major_starts0, minor_requests0, minor_starts0;
  uint64_t minor_deferred0, minor_roots_deferred0;
  uint64_t minor_survival_major0;
  uint64_t remembered_drained0;
  uint64_t remembered_pushed0, remembered_filtered0;
  uint64_t assist_runs0, assist_grey0, assist_ssb0;
  uint64_t assist_weak0;
  uint64_t weak_clear_tables0, weak_clear_cleared0;
  TValue vals[2];
  GCtab *parent, *child, *grandchild, *old_survivor, *active_child;
  GCtab *weak, *key, *val;

  assert(L != NULL);
  g = G(L);
  tg = L2TG(L);
  assert(g != NULL);
  assert(tg != NULL);
  test_obj_valid_rejects_nonobject(g);
  test_finreg_entry_rejects_nonobject(L, g);
  assert(gc2_gcpause_pct_acq(g) == lj_gc_pause_load(g));
  assert(la_load32_acq(&g->gc2.cycle_leader) == 0);
  assert(gc2_cycle_requests_acq(g) == 0);
  assert(gc2_cycle_starts_acq(g) == 0);
  assert(gc2_major_cycle_starts_acq(g) == 0);
  assert(gc2_minor_cycle_requests_acq(g) == 0);
  assert(gc2_minor_cycle_starts_acq(g) == 0);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 0);
  assert(la_load32_acq(&g->gc2.cycle_sweep_minor) == 0);
  assert(la_load32_acq(&g->gc2.minor_sweep_enabled) == 0);
  assert(la_load32_acq(&g->gc2.cycle_roots_minor) == 0);
  assert(la_load32_acq(&g->gc2.minor_roots_enabled) == 0);
  assert(gc2_minor_sweep_deferred_acq(g) == 0);
  assert(gc2_minor_sweep_arenas_acq(g) == 0);
  assert(gc2_minor_roots_deferred_acq(g) == 0);
  assert(gc2_minor_survival_base_live_acq(g) == 0);
  assert(gc2_minor_survival_bytes_acq(g) == 0);
  assert(gc2_minor_survival_pct_acq(g) == 0);
  assert(gc2_minor_survival_threshold_pct_acq(g) ==
	 LJ_GC2_MINOR_SURVIVAL_MAJOR_PCT);
  assert(gc2_minor_survival_major_requests_acq(g) == 0);
  assert(la_load32_acq(&g->gc2.force_major) == 0);
  assert(gc2_remembered_filtered_acq(g) == 0);
  assert(gc2_remembered_drained_acq(g) == 0);
  assert(la_load32_acq(&g->gc2.assist_shift) ==
	 lj_gc2_assist_shift_from_stepmul(g->gc.stepmul));
  assert(la_load64_acq(&g->gc2.trigger_bytes) >= LJ_GC2_TRIGGER_MIN);
  assert(la_load64_acq(&g->gc2.hard_bytes) ==
	 2u * la_load64_acq(&g->gc2.trigger_bytes));
  assert(gc2_alloc_total_bytes_acq(g) == 0);
  assert(la_load64_acq(&g->gc2.cycle_alloc_bytes) == 0);
  assert(gc2_assist_runs_acq(g) == 0);
  assert(gc2_assist_grey_drained_acq(g) == 0);
  assert(gc2_assist_ssb_converted_acq(g) == 0);
  assert(gc2_assist_weak_drained_acq(g) == 0);
  assert(gc2_jit_hard_checks_acq(g) == 0);
  assert(la_load32_acq(&g->gc2.assist_active) == 0);
  test_pub_barrier_entry_rejects_nonobject(L, g);
  test_global_barrier_without_mark_active(L, g, tg);

  (void)lua_gc(L, LUA_GCSETPAUSE, 150);
  assert(la_load32_acq(&g->gc2.gcpause_pct) == 150);
  assert(la_load64_acq(&g->gc2.trigger_bytes) >= LJ_GC2_TRIGGER_MIN);
  assert(la_load64_acq(&g->gc2.hard_bytes) ==
	 2u * la_load64_acq(&g->gc2.trigger_bytes));
  (void)lua_gc(L, LUA_GCSETSTEPMUL, 400);
  assert(la_load32_acq(&g->gc2.assist_shift) ==
	 lj_gc2_assist_shift_from_stepmul(400));

  (void)lj_gc2_flush_alloc(g, tg);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 0);
  lj_gc2_update_pacing(g);
  lj_gc2_publish_idle_threshold(g);
  assert(lj_gc_threshold_load(g) >= lj_gc_total_load(g));
  assert((uint64_t)(lj_gc_threshold_load(g) - lj_gc_total_load(g)) >=
	 LJ_GC2_TRIGGER_MIN);
  /*
  ** The raw accounting checks below intentionally stop short of cycle-request
  ** behavior. Keep the synthetic trigger above the local flush quantum; the
  ** request path has its own block with a one-byte trigger.
  */
  la_store64_rel(&g->gc2.trigger_bytes, 4u * LJ_GC2_ACCT_FLUSH);
  la_store64_rel(&g->gc2.hard_bytes, 8u * LJ_GC2_ACCT_FLUSH);

  p = lj_mem_realloc(L, NULL, 0, 128);
  assert(p != NULL);
  assert(la_load64_acq(&tg->local_total) == 128);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 0);
  alloc_total0 = gc2_alloc_total_bytes_acq(g);
  assert(lj_gc2_flush_alloc(g, tg) == 128);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 128);
  assert(gc2_alloc_total_bytes_acq(g) == alloc_total0 + 128);

  lj_mem_free(g, p, 128);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 128);

  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH - 1u);
  assert(la_load64_acq(&tg->local_total) == LJ_GC2_ACCT_FLUSH - 1u);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 128);
  lj_gc2_account_alloc(g, tg, 1);
  assert(la_load64_acq(&tg->local_total) == 0);
  total = la_load64_acq(&g->gc2.alloc_since_trigger);
  assert(total == 128 + LJ_GC2_ACCT_FLUSH);
  assert(gc2_alloc_total_bytes_acq(g) ==
	 alloc_total0 + 128 + LJ_GC2_ACCT_FLUSH);

  lj_gc2_account_alloc(g, tg, 7);
  assert(la_load64_acq(&tg->local_total) == 7);
  epoch0 = la_load64_acq(&g->gc2.hs_epoch);
  assert(lj_gc2_handshake(g, LJ_GC2_HS_REDISPATCH) == 1);
  assert(la_load64_acq(&g->gc2.hs_epoch) == epoch0 + 1u);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == total + 7);
  assert(gc2_alloc_total_bytes_acq(g) ==
	 alloc_total0 + 128 + LJ_GC2_ACCT_FLUSH + 7);
  assert(la_load32_acq(&g->gc2.cycle_leader) == 0);

  lj_gc_threshold_store(g, g->gc.total + 4u * LJ_GC2_ACCT_FLUSH);
  la_store64_rel(&g->gc2.trigger_bytes, 1);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  cycle_requests0 = gc2_cycle_requests_acq(g);
  cycle_starts0 = gc2_cycle_starts_acq(g);
  major_starts0 = gc2_major_cycle_starts_acq(g);
  minor_requests0 = gc2_minor_cycle_requests_acq(g);
  minor_starts0 = gc2_minor_cycle_starts_acq(g);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(lj_gc_threshold_load(g) == g->gc.total);
  assert(la_load32_acq(&g->gc2.cycle_leader) == tg->tid);
  assert(gc2_cycle_requests_acq(g) == cycle_requests0 + 1u);
  assert(gc2_cycle_starts_acq(g) == cycle_starts0);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH);
  assert(gc2_cycle_requests_acq(g) == cycle_requests0 + 1u);
  lj_gc2_mark_begin(g);
  assert(la_load32_acq(&g->gc2.cycle_leader) == 0);
  assert(gc2_cycle_starts_acq(g) == cycle_starts0 + 1u);
  assert(gc2_major_cycle_starts_acq(g) == major_starts0 + 1u);
  assert(gc2_minor_cycle_requests_acq(g) == minor_requests0);
  assert(gc2_minor_cycle_starts_acq(g) == minor_starts0);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 0);
  assert(la_load64_acq(&g->gc2.cycle_alloc_bytes) >=
	 2u * LJ_GC2_ACCT_FLUSH);
  assert(gc2_alloc_total_bytes_acq(g) >=
	 alloc_total0 + 128 + 3u * LJ_GC2_ACCT_FLUSH + 7);
  lj_gc2_cycle_to_idle(g);

  la_store32_rel(&g->gc2.generational, 1);
  major_starts0 = gc2_major_cycle_starts_acq(g);
  minor_requests0 = gc2_minor_cycle_requests_acq(g);
  minor_starts0 = gc2_minor_cycle_starts_acq(g);
  minor_deferred0 = gc2_minor_sweep_deferred_acq(g);
  minor_roots_deferred0 = gc2_minor_roots_deferred_acq(g);
  lj_gc2_mark_begin(g);
  assert(gc2_major_cycle_starts_acq(g) == major_starts0 + 1u);
  assert(gc2_minor_cycle_requests_acq(g) ==
	 minor_requests0 + 1u);
  assert(gc2_minor_cycle_starts_acq(g) == minor_starts0);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 1);
  assert(la_load32_acq(&g->gc2.cycle_sweep_minor) == 0);
  assert(tg->alloc.alloc_black == 1);
  assert(gc2_minor_sweep_deferred_acq(g) ==
	 minor_deferred0 + 1u);
  assert(la_load32_acq(&g->gc2.cycle_roots_minor) == 0);
  assert(gc2_minor_roots_deferred_acq(g) ==
	 minor_roots_deferred0 + 1u);
  lj_gc2_cycle_to_idle(g);

  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  la_store32_rel(&g->gc2.minor_roots_enabled, 1);
  major_starts0 = gc2_major_cycle_starts_acq(g);
  minor_requests0 = gc2_minor_cycle_requests_acq(g);
  minor_starts0 = gc2_minor_cycle_starts_acq(g);
  minor_deferred0 = gc2_minor_sweep_deferred_acq(g);
  minor_roots_deferred0 = gc2_minor_roots_deferred_acq(g);
  lj_gc2_mark_begin(g);
  assert(gc2_major_cycle_starts_acq(g) == major_starts0);
  assert(gc2_minor_cycle_requests_acq(g) ==
	 minor_requests0 + 1u);
  assert(gc2_minor_cycle_starts_acq(g) == minor_starts0 + 1u);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 1);
  assert(la_load32_acq(&g->gc2.cycle_sweep_minor) == 1);
  assert(la_load32_acq(&g->gc2.cycle_roots_minor) == 1);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_MARK);
  assert(tg->alloc.alloc_black == 1);
  assert_late_attach_color(g, tg, &late_tg, 7000u, 1, 1);
  lua_newtable(L);
  active_child = tabV(L->top - 1);
  assert(lj_gc2_ismarked(g, obj2gco(active_child)) == 1);
  lua_pop(L, 1);
  assert(gc2_minor_sweep_deferred_acq(g) == minor_deferred0);
  assert(gc2_minor_roots_deferred_acq(g) ==
	 minor_roots_deferred0);
  lj_gc2_cycle_to_idle(g);

  /* White-box legacy minor mechanics remain tested explicitly even though
  ** the b1.2 public close resets both gates to the safe major fallback. */
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  la_store32_rel(&g->gc2.minor_roots_enabled, 1);
  lj_gc2_mark_begin(g);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_MARK);
  assert(la_load32_acq(&g->gc2.cycle_sweep_minor) == 1);
  assert_late_attach_color(g, tg, &late_tg, 7001u, 1, 1);
  lj_gc2_mark_to_weak(g);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_WEAK);
  assert_late_attach_color(g, tg, &late_tg, 7002u, 1, 1);
  close_weak_for_sweep(L, g);
  lj_gc2_weak_to_sweep(g, L);
  assert(la_load32_acq(&g->gc2.phase) == LJ_GC2_SWEEP);
  assert(tg->alloc.alloc_black == 0);
  assert_late_attach_color(g, tg, &late_tg, 7003u, 0, 0);
  lj_gc2_cycle_to_idle(g);

  active_child = lj_tab_new(L, 0, 0);
  assert(active_child != NULL);
  assert(root_contains(g, obj2gco(active_child)));
  assert(lj_gc2_ismarked(g, obj2gco(active_child)) == 0);
  flipwhite(obj2gco(active_child));  /* Manual GC2 sweep setup mirrors color atomic. */
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  la_store32_rel(&g->gc2.minor_roots_enabled, 1);
  lj_gc2_mark_begin(g);
  assert(la_load32_acq(&g->gc2.cycle_sweep_minor) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(active_child)) == 0);
  lj_gc2_mark_to_weak(g);
  close_weak_for_sweep(L, g);
  lj_gc2_weak_to_sweep(g, L);
  assert(lj_gc2_test_sweep_owner_progress(g, tg, 64) == 0);
  /*
  ** Owner sweep starts only after the bridge boundary moves traversable owned
  ** arenas to the sweep list and then publishes bridge readiness.
  */
  lj_gc2_sweep_prepare_bridge_boundary(g, NULL);
  assert(lj_gc2_test_sweep_owner_progress(g, tg, 64) > 0);
  assert(!root_contains(g, obj2gco(active_child)));
  lj_gc2_cycle_to_idle(g);
  lua_gc(L, LUA_GCCOLLECT, 0);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 0);
  la_store32_rel(&g->gc2.minor_roots_enabled, 0);

  major_starts0 = gc2_major_cycle_starts_acq(g);
  minor_requests0 = gc2_minor_cycle_requests_acq(g);
  minor_starts0 = gc2_minor_cycle_starts_acq(g);
  lj_gc2_force_major(g);
  lj_gc2_mark_begin(g);
  assert(gc2_major_cycle_starts_acq(g) == major_starts0 + 1u);
  assert(tg->alloc.alloc_black == 1);
  assert(gc2_minor_cycle_requests_acq(g) == minor_requests0);
  assert(gc2_minor_cycle_starts_acq(g) == minor_starts0);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 0);
  assert(la_load32_acq(&g->gc2.force_major) == 0);
  lj_gc2_cycle_to_idle(g);
  lj_gc2_set_generational(g, 0);

  minor_survival_major0 =
    gc2_minor_survival_major_requests_acq(g);
  la_store32_rel(&g->gc2.generational, 1);
  la_store32_rel(&g->gc2.cycle_sweep_minor, 1);
  la_store64_rel(&g->gc2.cycle_alloc_bytes, 1000);
  gc2_minor_survival_base_live_rel(g, 10000);
  lj_gc2_test_update_minor_survival_policy(g, 10500);
  assert(gc2_minor_survival_bytes_acq(g) == 500);
  assert(gc2_minor_survival_pct_acq(g) == 50);
  assert(gc2_minor_survival_base_live_acq(g) == 10500);
  assert(la_load32_acq(&g->gc2.force_major) == 0);
  assert(gc2_minor_survival_major_requests_acq(g) ==
	 minor_survival_major0);
  la_store64_rel(&g->gc2.cycle_alloc_bytes, 1000);
  gc2_minor_survival_base_live_rel(g, 10000);
  lj_gc2_test_update_minor_survival_policy(g, 10800);
  assert(gc2_minor_survival_bytes_acq(g) == 800);
  assert(gc2_minor_survival_pct_acq(g) ==
	 LJ_GC2_MINOR_SURVIVAL_MAJOR_PCT);
  assert(gc2_minor_survival_base_live_acq(g) == 10800);
  assert(la_load32_acq(&g->gc2.force_major) == 1);
  assert(gc2_minor_survival_major_requests_acq(g) ==
	 minor_survival_major0 + 1u);
  la_store32_rel(&g->gc2.cycle_sweep_minor, 0);
  lj_gc2_set_generational(g, 0);
  assert(la_load32_acq(&g->gc2.force_major) == 0);
  assert(gc2_minor_survival_pct_acq(g) == 0);
  assert(gc2_minor_survival_bytes_acq(g) == 0);

  lua_settop(L, 0);
  lj_gc2_set_generational(g, 1);
  assert(la_load32_acq(&g->gc2.generational) == 1);
  assert(la_load32_acq(&g->gc2.force_major) == 1);
  assert(la_load32_acq(&g->gc2.minor_sweep_enabled) == 0);
  assert(la_load32_acq(&g->gc2.minor_roots_enabled) == 0);
  assert(tg->mark_active == 1);
  major_starts0 = gc2_major_cycle_starts_acq(g);
  minor_requests0 = gc2_minor_cycle_requests_acq(g);
  minor_starts0 = gc2_minor_cycle_starts_acq(g);
  lj_gc2_mark_begin(g);
  assert(gc2_major_cycle_starts_acq(g) == major_starts0 + 1u);
  assert(gc2_minor_cycle_requests_acq(g) == minor_requests0);
  assert(gc2_minor_cycle_starts_acq(g) == minor_starts0);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 0);
  assert(la_load32_acq(&g->gc2.force_major) == 0);
  lj_gc2_cycle_to_idle(g);
  assert(la_load32_acq(&g->gc2.minor_sweep_enabled) == 0);
  assert(la_load32_acq(&g->gc2.minor_roots_enabled) == 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  assert(lj_gc2_test_ssb_push(g, obj2gco(parent)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  remembered_drained0 = gc2_remembered_drained_acq(g);
  major_starts0 = gc2_major_cycle_starts_acq(g);
  minor_requests0 = gc2_minor_cycle_requests_acq(g);
  minor_starts0 = gc2_minor_cycle_starts_acq(g);
  lj_gc2_mark_begin(g);
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_major_cycle_starts_acq(g) == major_starts0 + 1u);
  assert(gc2_minor_cycle_requests_acq(g) ==
	 minor_requests0 + 1u);
  assert(gc2_minor_cycle_starts_acq(g) == minor_starts0);
  assert(la_load32_acq(&g->gc2.cycle_minor_requested) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_remembered_drained_acq(g) >=
	 remembered_drained0 + 1u);
  lj_gc2_cycle_to_idle(g);
  lj_gc2_set_generational(g, 0);
  assert(la_load32_acq(&g->gc2.minor_sweep_enabled) == 0);
  assert(la_load32_acq(&g->gc2.minor_roots_enabled) == 0);
  lua_settop(L, 0);

  lj_gc2_set_generational(g, 1);
  /* Internal remembered tests avoid allocation-triggered major cycles. */
  la_store32_rel(&g->gc2.force_major, 0);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  /* Meta-store resolution may allocate. Keep this white-box IDLE remembered
  ** sequence from running a complete automatic minor cycle between its exact
  ** filtering counter snapshots. */
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  lua_createtable(L, 1, 0);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_newtable(L);
  grandchild = tabV(L->top - 1);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(lj_gc2_markobj(g, obj2gco(child)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  remembered_pushed0 = gc2_remembered_pushed_acq(g);
  remembered_filtered0 = gc2_remembered_filtered_acq(g);
  settabV(L, &vals[0], grandchild);
  lj_gc2_barrier_tv_pair_g(g, obj2gco(parent), &vals[0]);
  assert(gc2_remembered_pushed_acq(g) == remembered_pushed0 + 1u);
  settabV(L, &vals[0], child);
  lj_gc2_barrier_tv_pair_g(g, obj2gco(parent), &vals[0]);
  assert(gc2_remembered_filtered_acq(g) ==
	 remembered_filtered0 + 1u);
  assert(gc2_remembered_pushed_acq(g) ==
	 remembered_pushed0 + 1u);
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_test_ssb_drain(g);
  remembered_pushed0 = gc2_remembered_pushed_acq(g);
  remembered_filtered0 = gc2_remembered_filtered_acq(g);
  setintV(&vals[0], 1);
  /*
  ** Direct meta stores use the same public table-store publication route as
  ** VM fallback stores. A missing-key resolution remembers the owner once and
  ** observes four already-old edges: the source temporary root, the direct
  ** receiver root, the metamethod lookup's receiver snapshot, and the final
  ** owner/value pair. The weak-value helper is WEAK-phase-only and contributes
  ** no IDLE remembered telemetry.
  */
  settabV(L, &vals[1], grandchild);
  diagnose_phase(g,"before-meta-store",__LINE__);
  assert(lj_meta_tsettv_pair(L, L->top - 3, &vals[0], &vals[1]) != NULL);
  diagnose_phase(g,"after-meta-store",__LINE__);
  assert(gc2_remembered_pushed_acq(g) == remembered_pushed0 + 1u);
  assert(gc2_remembered_filtered_acq(g) == remembered_filtered0 + 4u);
  assert(active_ssb_last(tg) == obj2gco(parent));
  /* Existing-key resolution additionally publishes the copied old value as a
  ** root and owner/value pair. Together with the source root, receiver root,
  ** and final pair this filters five more old edges; the copied-read and
  ** returned-owner routes remember the table twice. */
  settabV(L, &vals[1], child);
  diagnose_phase(g,"before-meta-store",__LINE__);
  assert(lj_meta_tsettv_pair(L, L->top - 3, &vals[0], &vals[1]) != NULL);
  diagnose_phase(g,"after-meta-store",__LINE__);
  assert(gc2_remembered_filtered_acq(g) ==
	 remembered_filtered0 + 9u);
  assert(gc2_remembered_pushed_acq(g) ==
	 remembered_pushed0 + 3u);
  assert(active_ssb_last(tg) == obj2gco(parent));
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_test_ssb_drain(g);
  grandchild = lj_tab_new(L, 0, 0);
  assert(grandchild != NULL);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  remembered_pushed0 = gc2_remembered_pushed_acq(g);
  remembered_filtered0 = gc2_remembered_filtered_acq(g);
  lj_gc2_barrier_obj_pair(L, obj2gco(parent), obj2gco(grandchild));
  assert(gc2_remembered_pushed_acq(g) == remembered_pushed0 + 1u);
  assert(gc2_remembered_filtered_acq(g) == remembered_filtered0);
  assert(active_ssb_last(tg) == obj2gco(parent));
  lj_gc2_barrier_obj_pair(L, obj2gco(parent), obj2gco(child));
  assert(gc2_remembered_filtered_acq(g) ==
	 remembered_filtered0 + 1u);
  assert(gc2_remembered_pushed_acq(g) ==
	 remembered_pushed0 + 1u);
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_test_ssb_drain(g);
  grandchild = lj_tab_new(L, 0, 0);
  assert(grandchild != NULL);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  remembered_pushed0 = gc2_remembered_pushed_acq(g);
  remembered_filtered0 = gc2_remembered_filtered_acq(g);
  settabV(L, &vals[0], grandchild);
  settabV(L, &vals[1], child);
  lj_gc2_barrier_tvn_pair_g(g, obj2gco(parent), vals, 2);
  assert(gc2_remembered_pushed_acq(g) == remembered_pushed0 + 1u);
  assert(gc2_remembered_filtered_acq(g) ==
	 remembered_filtered0 + 1u);
  assert(active_ssb_last(tg) == obj2gco(parent));
  (void)lj_gc2_handshake(g, LJ_GC2_HS_FLUSH_SSB);
  (void)lj_gc2_test_ssb_drain(g);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 0);
  lj_gc2_set_generational(g, 0);
  lua_settop(L, 0);

  lua_newtable(L);
  parent = tabV(L->top - 1);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  (void)lj_gc2_flush_ssb(g, tg);
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  lua_newtable(L);
  old_survivor = tabV(L->top - 1);
  assert(lj_gc2_markobj(g, obj2gco(old_survivor)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(old_survivor)) == 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -4, 1);  /* parent[1] = child. */
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);

  lj_gc2_set_generational(g, 1);
  la_store32_rel(&g->gc2.force_major, 0);  /* Internal minor test owns age setup. */
  la_store32_rel(&g->gc2.minor_sweep_enabled, 1);
  la_store32_rel(&g->gc2.minor_roots_enabled, 1);
  remembered_pushed0 = gc2_remembered_pushed_acq(g);
  remembered_drained0 = gc2_remembered_drained_acq(g);
  lj_gc2_barrier_obj_pair(L, obj2gco(parent), obj2gco(child));
  assert(gc2_remembered_pushed_acq(g) == remembered_pushed0 + 1u);
  major_starts0 = gc2_major_cycle_starts_acq(g);
  minor_requests0 = gc2_minor_cycle_requests_acq(g);
  minor_starts0 = gc2_minor_cycle_starts_acq(g);
  lj_gc2_mark_begin(g);
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
  assert(gc2_major_cycle_starts_acq(g) == major_starts0);
  assert(gc2_minor_cycle_requests_acq(g) ==
	 minor_requests0 + 1u);
  assert(gc2_minor_cycle_starts_acq(g) == minor_starts0 + 1u);
  assert(la_load32_acq(&g->gc2.cycle_roots_minor) == 1);
  assert(tg->alloc.alloc_black == 1);
  lua_newtable(L);
  active_child = tabV(L->top - 1);
  assert(lj_gc2_ismarked(g, obj2gco(active_child)) == 1);
  lj_gc2_barrier_obj_pair(L, obj2gco(parent), obj2gco(active_child));
  assert(lj_gc2_ismarked(g, obj2gco(active_child)) == 1);
  lua_pop(L, 1);
  assert(gc2_remembered_drained_acq(g) >=
	 remembered_drained0 + 1u);
  assert(lj_gc2_ismarked(g, obj2gco(old_survivor)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  la_store32_rel(&g->gc2.minor_sweep_enabled, 0);
  la_store32_rel(&g->gc2.minor_roots_enabled, 0);
  lj_gc2_set_generational(g, 0);
  lj_gc2_cycle_to_idle(g);
  lua_settop(L, 0);

  test_public_minor_request_falls_back_major(L, g, tg);

  test_vm_generational_table_store_remembered(L, g, tg);

#if LJ_HASJIT
  test_jit_generational_table_store_remembered(L, g, tg);
#endif

  lj_gc_threshold_store(g, LJ_MAX_MEM);
  lj_gc2_publish_idle_threshold(g);
  assert(lj_gc_threshold_load(g) == LJ_MAX_MEM);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  cycle_requests0 = gc2_cycle_requests_acq(g);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(lj_gc_threshold_load(g) == LJ_MAX_MEM);
  assert(la_load32_acq(&g->gc2.cycle_leader) == 0);
  assert(gc2_cycle_requests_acq(g) == cycle_requests0);
  lj_gc_threshold_store(g, g->gc.total + 4u * LJ_GC2_ACCT_FLUSH);
  lj_gc2_update_pacing(g);

  lj_gc2_account_alloc(g, tg, 99);
  assert(la_load64_acq(&tg->local_total) == 99);
  lj_gc2_mark_begin(g);
  assert(la_load64_acq(&tg->local_total) == 0);
  assert(la_load64_acq(&g->gc2.alloc_since_trigger) == 0);
  lj_gc2_cycle_to_idle(g);

  lj_gc2_mark_begin(g);
  la_store64_rel(&tg->local_total, 0);
  la_store64_rel(&g->gc2.hard_bytes, 2u * LJ_GC2_ACCT_FLUSH);
  la_store32_rel(&g->gc2.assist_shift, 0);
  la_store64_rel(&g->gc2.alloc_since_trigger, 2u * LJ_GC2_ACCT_FLUSH + 1u);
  lj_gc_threshold_store(g, LJ_MAX_MEM);
  assist_runs0 = gc2_assist_runs_acq(g);
  lj_gc_step_fixtop(L);
  assert(gc2_assist_runs_acq(g) == assist_runs0);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH - 1u);
  assert(gc2_assist_runs_acq(g) == assist_runs0);
  lj_gc2_account_alloc(g, tg, 1u);
  assert(gc2_assist_runs_acq(g) == assist_runs0 + 1u);
  lj_gc2_cycle_to_idle(g);

  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_newtable(L);
  grandchild = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);  /* child[1] = grandchild. */
  lua_pushvalue(L, -2);
  lua_rawseti(L, -4, 1);  /* parent[1] = child. */

  lj_gc2_mark_begin(g);
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 0);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  la_store64_rel(&g->gc2.hard_bytes, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  assist_runs0 = gc2_assist_runs_acq(g);
  assist_grey0 = gc2_assist_grey_drained_acq(g);
  assist_ssb0 = gc2_assist_ssb_converted_acq(g);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH);
  assert(tg->gc_assist == 0);
  assert(la_load32_acq(&g->gc2.assist_active) == 0);
  assert(gc2_assist_runs_acq(g) == assist_runs0 + 1u);
  assert(gc2_assist_grey_drained_acq(g) >= assist_grey0);
  assert(gc2_assist_ssb_converted_acq(g) >= assist_ssb0);
  assert((gc2_assist_grey_drained_acq(g) - assist_grey0) +
	 (gc2_assist_ssb_converted_acq(g) - assist_ssb0) >= 1u);
  /* The bounded assist may spend this quantum converting the SSB request.
  ** Consume one ordinary worker item before asserting its child frontier. */
  if (lj_gc2_ismarked(g, obj2gco(child)) == 0) {
    /* This direct ownership assertion is not a native scheduling fixture.
    ** Close the fresh cooperative MARK lease before demanding immediate work. */
    lj_gc2_jit_mark_request_exit(g);
    assert(gc2_jit_phase_gate_acq(g) == 0);
    assert(lj_gc2_worker_drain(g, 1) != 0);
  }
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(!lj_gc2_test_ssb_empty(g));
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  lj_gc2_cycle_to_idle(g);

  lua_settop(L, 0);
  lua_newtable(L);
  parent = tabV(L->top - 1);
  lua_newtable(L);
  child = tabV(L->top - 1);
  lua_newtable(L);
  grandchild = tabV(L->top - 1);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 1);  /* child[1] = grandchild. */
  lua_pushvalue(L, -2);
  lua_rawseti(L, -4, 1);  /* parent[1] = child. */

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(parent)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  assert(!lj_gc2_test_ssb_empty(g));
  la_store64_rel(&g->gc2.hard_bytes, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  assist_runs0 = gc2_assist_runs_acq(g);
  assist_grey0 = gc2_assist_grey_drained_acq(g);
  assist_ssb0 = gc2_assist_ssb_converted_acq(g);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH);
  assert(tg->gc_assist == 0);
  assert(la_load32_acq(&g->gc2.assist_active) == 0);
  assert(gc2_assist_runs_acq(g) == assist_runs0 + 1u);
  assert(gc2_assist_grey_drained_acq(g) >= assist_grey0);
  assert(gc2_assist_ssb_converted_acq(g) >= assist_ssb0);
  assert((gc2_assist_grey_drained_acq(g) - assist_grey0) +
	 (gc2_assist_ssb_converted_acq(g) - assist_ssb0) >= 1u);
  if (lj_gc2_ismarked(g, obj2gco(child)) == 0) {
    lj_gc2_jit_mark_request_exit(g);
    assert(gc2_jit_phase_gate_acq(g) == 0);
    assert(lj_gc2_worker_drain(g, 1) != 0);
  }
  assert(lj_gc2_ismarked(g, obj2gco(parent)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(child)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 0);
  assert(!lj_gc2_test_ssb_empty(g));
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_ismarked(g, obj2gco(grandchild)) == 1);
  lj_gc2_cycle_to_idle(g);

  lua_settop(L, 0);
  lua_newtable(L);
  weak = tabV(L->top - 1);
  lua_newtable(L);
  lua_pushliteral(L, "__mode");
  lua_pushliteral(L, "v");
  lua_settable(L, -3);
  lua_setmetatable(L, -2);
  lua_newtable(L);
  key = tabV(L->top - 1);
  lua_newtable(L);
  val = tabV(L->top - 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_settable(L, 1);

  lj_gc2_mark_begin(g);
  assert(lj_gc2_markobj(g, obj2gco(weak)) == 1);
  assert(lj_gc2_flush_ssb(g, tg) == 1);
  (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_weak_snapshot_count(g) == 1u);
  assert(lj_gc2_ismarked(g, obj2gco(key)) == 1);
  assert(lj_gc2_ismarked(g, obj2gco(val)) == 0);
  lj_gc2_mark_to_weak(g);
  /*
  ** Allocation assist drains weak snapshots only after weak mark closure. The
  ** completion bridge owns that closure in production; this white-box case
  ** sets the same precondition so the assist path can be tested directly.
  */
  gc2_weak_mark_closed_rel(g, 1);
  la_store64_rel(&g->gc2.hard_bytes, 1);
  la_store32_rel(&g->gc2.assist_shift, 0);
  (void)la_xchg64_acqrel(&g->gc2.alloc_since_trigger, 0);
  assist_runs0 = gc2_assist_runs_acq(g);
  assist_weak0 = gc2_assist_weak_drained_acq(g);
  weak_clear_tables0 = gc2_weak_clear_tables_acq(g);
  weak_clear_cleared0 = gc2_weak_clear_cleared_acq(g);
  lj_gc2_account_alloc(g, tg, LJ_GC2_ACCT_FLUSH);
  assert(tg->gc_assist == 0);
  assert(la_load32_acq(&g->gc2.assist_active) == 0);
  assert(gc2_assist_runs_acq(g) == assist_runs0 + 1u);
  assert(gc2_assist_weak_drained_acq(g) == assist_weak0 + 1u);
  assert(gc2_weak_clear_tables_acq(g) == weak_clear_tables0 + 1u);
  assert(gc2_weak_clear_cleared_acq(g) ==
	 weak_clear_cleared0 + 1u);
  lua_pushvalue(L, 2);
  lua_gettable(L, 1);
  assert(lua_isnil(L, -1));
  lua_pop(L, 1);
  lj_gc2_cycle_to_idle(g);

  test_obj_valid_accepts_variable_cdata(L, g);
  test_variable_cdata_reclamation(L, g);

  lua_close(L);
  puts("t-gc2-alloc-account OK: allocation accounting flushes by threshold and safepoint");
  return 0;
}
