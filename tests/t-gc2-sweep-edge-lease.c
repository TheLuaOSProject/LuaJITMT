/* Linux linker-wrap checks for the production SWEEP TValue barrier. */
#ifndef LJ_GC2_TEST_HELPERS
#error "t-gc2-sweep-edge-lease requires LJ_GC2_TEST_HELPERS"
#endif

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_thr.h"
#if LJ_HASFFI
#include "lj_cdata.h"
#endif

typedef struct Fixture {
  lua_State *L;
  global_State *g;
  TGState *tg;
} Fixture;

typedef struct AdmissionWatch {
  GCArena *arena;
  GCobj *object;
  void *base;
  HugeTab *wrapper;
  uint32_t small, observed, marked_reader, marked, releases;
  uint32_t expected_readers;
  int admission, withdraw_wrapper, retire_attempt, external_free;
} AdmissionWatch;

static AdmissionWatch watch;

int __real_lj_arena_rescue_enter(GCArena *a);
int __wrap_lj_arena_rescue_enter(GCArena *a)
{
  int result = __real_lj_arena_rescue_enter(a);
  if (a == watch.arena) {
    watch.small++;
    watch.admission = result;
  }
  return result;
}

int __real_lj_arena_hugetab_rescue_enter(HugeTab *ht, GCArena *a, LJHugeInfo *hi);
int __wrap_lj_arena_hugetab_rescue_enter(HugeTab *ht, GCArena *a, LJHugeInfo *hi)
{
  int result = __real_lj_arena_hugetab_rescue_enter(ht, a, hi);
  if (a == watch.arena) {
    watch.small++;
    watch.admission = result;
  }
  return result;
}

int __real_lj_arena_hugetab_reader_cdata_range_acquire(
  HugeTab *ht, const void *p, void **basep, LJHugeReader *reader, LJHugeInfo *hi);
int __wrap_lj_arena_hugetab_reader_cdata_range_acquire(
  HugeTab *ht, const void *p, void **basep, LJHugeReader *reader, LJHugeInfo *hi)
{
  int result = __real_lj_arena_hugetab_reader_cdata_range_acquire(
    ht, p, basep, reader, hi);
  if (p == watch.object && result == LJ_ARENA_HUGE_READER_ACQUIRED) {
    watch.observed++;
    assert(reader->base == watch.base);
    assert(hi->readers == watch.expected_readers);
  }
  return result;
}

int __real_lj_arena_hugetab_mark_cdata_range_reader_acquire(
  HugeTab *ht, const void *p, void **basep, LJHugeReader *reader, LJHugeInfo *hi);
int __wrap_lj_arena_hugetab_mark_cdata_range_reader_acquire(
  HugeTab *ht, const void *p, void **basep, LJHugeReader *reader, LJHugeInfo *hi)
{
  if (p == watch.object)
    watch.marked_reader++;
  return __real_lj_arena_hugetab_mark_cdata_range_reader_acquire(
    ht, p, basep, reader, hi);
}

int __real_lj_arena_hugetab_mark(HugeTab *ht, const void *p, LJHugeInfo *hi);
int __wrap_lj_arena_hugetab_mark(HugeTab *ht, const void *p, LJHugeInfo *hi)
{
  LJHugeTabHdr *saved = NULL;
  int result;
  if (p == watch.base && watch.object) {
    LJHugeInfo before;
    watch.marked++;
    assert(lj_arena_hugetab_lookup(ht, p, &before));
    assert(before.readers == watch.expected_readers);
    assert(watch.object->gch.gct >= (uint8_t)~LJ_TSTR);
    if (watch.retire_attempt) {
      assert(before.flags & LJ_HUGEF_SWEEP_OLD);
      assert(lj_arena_hugetab_retire(ht, p, watch.object, 1, NULL) == 0);
      assert(lj_arena_hugetab_destruct_acquire(ht, p, NULL) ==
	     LJ_ARENA_DESTRUCT_LOST);
    }
    if (watch.external_free) {
      assert(lj_arena_hugetab_claim_external_free(ht, p, &before) == 0);
      assert(before.readers == 1 && (before.flags & LJ_HUGEF_DEFER_FREE));
      assert(!(before.flags & LJ_HUGEF_FREEING));
    }
    if (watch.withdraw_wrapper) {
      /* The counted header must remain sufficient after the former TG's
      ** embedded wrapper is overwritten. Restore the fixture's registry
      ** before queued work is consumed, as in t-arena-hugetab's lease model. */
      assert(watch.wrapper && watch.wrapper != ht);
      saved = watch.wrapper->h;
      assert(saved == ht->h);
      watch.wrapper->h = NULL;
    }
  }
  result = __real_lj_arena_hugetab_mark(ht, p, hi);
  if (saved)
    watch.wrapper->h = saved;
  return result;
}

int __real_lj_arena_hugetab_reader_release(LJHugeReader *reader, LJHugeInfo *hi);
int __wrap_lj_arena_hugetab_reader_release(LJHugeReader *reader, LJHugeInfo *hi)
{
  if (reader && reader->base == watch.base && watch.object)
    watch.releases++;
  return __real_lj_arena_hugetab_reader_release(reader, hi);
}

static Fixture fixture_open(void)
{
  Fixture f;
  memset(&watch, 0, sizeof(watch));
  f.L = luaL_newstate();
  assert(f.L);
  lua_gc(f.L, LUA_GCSTOP, 0);
  f.g = G(f.L);
  f.tg = G2TG(f.g);
  assert(f.tg);
  return f;
}

static void drain(Fixture *f)
{
  uint32_t i;
  (void)lj_gc2_flush_ssb(f->g, f->tg);
  for (i = 0; i < 4096u && !lj_gc2_test_ssb_empty(f->g); i++)
    (void)lj_gc2_test_ssb_drain(f->g);
  assert(lj_gc2_test_ssb_empty(f->g));
  assert(gc2_recovery_items_acq(f->g) == 0);
  assert(gc2_recovery_failed_acq(f->g) == 0);
}

static void enter_sweep(Fixture *f)
{
  LJGC2ActivationSnap mark, weak, sweep;
  /* Startup SSB entries are pending semantic work. Consume them in an
  ** explicitly closed MARK turn before starting the measured white cycle. */
  lj_gc2_mark_begin(f->g);
  lj_gc2_jit_mark_request_exit(f->g);
  gc2_jit_mark_resume_rel(f->g, 0);
  drain(f);
  lj_gc2_cycle_to_idle(f->g);
  lj_gc2_mark_begin(f->g);
  lj_gc2_jit_mark_request_exit(f->g);
  gc2_jit_mark_resume_rel(f->g, 0);
  /* Model the exact SWEEP publication boundary without scanning the C-held
  ** test objects first. Mark-begin clears their real allocator mark planes;
  ** the normal barrier and worker below own their only semantic discovery. */
  mark = lj_gc2_activation_snapshot(&f->g->gc2.activation);
  assert(mark.state == LJ_GC2_ACT_MARK);
  assert(lj_gc2_activation_try_transition(&f->g->gc2.activation, &mark,
	 mark.mark_epoch, LJ_GC2_ACT_WEAK, &weak) == LJ_GC2_TRANSITION_OK);
  assert(lj_gc2_activation_try_transition(&f->g->gc2.activation, &weak,
	 mark.mark_epoch, LJ_GC2_ACT_SWEEP_OPEN, &sweep) == LJ_GC2_TRANSITION_OK);
  gc2_phase_rel(f->g, LJ_GC2_SWEEP);
  gc2_sweep_root_scanned_rel(f->g, 1);
}

static void fixture_close(Fixture *f)
{
  memset(&watch, 0, sizeof(watch));
  drain(f);
  lj_gc2_cycle_to_idle(f->g);
  lua_close(f->L);
}

static void test_small_tag_and_direct_bodies(void)
{
  Fixture f = fixture_open();
  TValue good, wrong, stale;
  GCtab *parent, *child;
  TValue *array;
  Node *node;
  uint32_t cell;
  lua_createtable(f.L, 64, 32);
  parent = tabV(f.L->top - 1);
  lua_newtable(f.L);
  child = tabV(f.L->top - 1);
  lua_rawseti(f.L, -2, 1);  /* The parent is the child's only semantic root. */
  settabV(f.L, &good, parent);
  setgcVraw(&wrong, obj2gco(parent), LJ_TFUNC);
  setgcVraw(&stale, (GCobj *)(uintptr_t)0x10000u, LJ_TTAB);
  array = lj_tab_array_acq(parent);
  node = lj_tab_node_acq(parent);
  assert(array && !lj_tab_array_is_colocated(parent, array));
  assert(node != &f.g->nilnode);
  enter_sweep(&f);
  assert(!lj_gc2_ismarked(f.g, obj2gco(parent)));
  assert(!lj_gc2_ismarked(f.g, obj2gco(child)));
  watch.arena = lj_arena_of(parent);
  cell = lj_arena_cellof(parent);
  lj_gc2_barrier_tv_g(f.g, &wrong);
  lj_gc2_barrier_tv_g(f.g, &stale);
  assert(watch.small == 1);
  assert(!lj_arena_bm_get(watch.arena->mark, cell));
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_failed_acq(f.g) == 0);
  watch.small = 0;
  lj_gc2_barrier_tv_g(f.g, &good);
  assert(watch.small == 1);  /* Original source acquires this arena twice. */
  assert(lj_arena_bm_get(watch.arena->mark, cell));
  assert((lj_arena_remote_active_acq(watch.arena) &
	  LJ_ARENA_REMOTE_COUNT_MASK) == 0);
  assert(lj_gc2_ismarkedmem(f.g, lj_tab_array_hdrw(array)));
  assert(lj_gc2_ismarkedmem(f.g, lj_tab_node_hdrw(node)));
  memset(&watch, 0, sizeof(watch));
  drain(&f);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)));
  fixture_close(&f);
}

static void test_committed_mark_modes(uint32_t flags, uint64_t remote,
				      int expected_admission,
				      uint32_t expected_mark)
{
  Fixture f = fixture_open();
  GCArena *a;
  TValue tv;
  uint32_t cell, saved_flags;
  uint64_t saved_remote;
  lua_pushliteral(f.L, "committed sweep edge");
  copyTV(f.L, &tv, f.L->top - 1);
  a = lj_arena_of(gcV(&tv));
  cell = lj_arena_cellof(gcV(&tv));
  enter_sweep(&f);
  saved_flags = lj_arena_flags_acq(a);
  saved_remote = lj_arena_remote_active_acq(a);
  assert((saved_remote & LJ_ARENA_REMOTE_COUNT_MASK) == 0);
  assert(!lj_arena_bm_get(a->mark, cell));
  assert(lj_arena_sweep_state_acq(a, cell) == LJ_ARENA_SWEEP_WHITE);
  la_store32_rel(&a->hdr.flags,
	(saved_flags & ~(LJ_AF_PREPSWEEP|LJ_AF_QUARANTINE|LJ_AF_RECLAIMED)) | flags);
  la_store64_rel(&a->hdr.remote_active, remote);
  watch.arena = a;
  lj_gc2_barrier_tv_g(f.g, &tv);
  assert(watch.small == 1 && watch.admission == expected_admission);
  assert(lj_arena_bm_get(a->mark, cell) == expected_mark);
  assert((lj_arena_remote_active_acq(a) & LJ_ARENA_REMOTE_COUNT_MASK) == 0);
  la_store32_rel(&a->hdr.flags, saved_flags);
  la_store64_rel(&a->hdr.remote_active, saved_remote);
  fixture_close(&f);
}

typedef struct Transition {
  GCArena *arena;
  uint32_t cell;
  TGAlloc *free_alloc;
} Transition;

static void *destruct_between_mark_and_check(void *arg)
{
  Transition *t = (Transition *)arg;
  uint32_t i;
  for (i = 0; i < 1000000u && !lj_gc2_test_queue_post_admit_paused(); i++)
    (void)lj_thr_retry_yield(NULL);
  assert(i < 1000000u);
  assert(watch.small == 1);
  assert((lj_arena_remote_active_acq(t->arena) &
	  LJ_ARENA_REMOTE_COUNT_MASK) == 1);
  assert(lj_arena_destruct_acquire(lj_arena_cellptr(t->arena, t->cell),
	 sizeof(GCtab)) == LJ_ARENA_DESTRUCT_LOST);
  assert(lj_arena_lifetime_state_acq(t->arena, t->cell) ==
	 LJ_ARENA_LIFETIME_LIVE);
  if (t->free_alloc) {
    lj_arena_free(t->free_alloc, lj_arena_cellptr(t->arena, t->cell),
		  sizeof(GCtab));
    assert(lj_arena_late_get(t->arena, t->cell));
    assert(lj_arena_ready_get(t->arena, t->cell));
    lj_gc2_test_queue_post_admit_release();
    return NULL;
  }
  assert(lj_arena_lifetime_state_cas(t->arena, t->cell,
	 LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_DESTRUCT));
  lj_gc2_test_queue_post_admit_release();
  for (i = 0; i < 1000000u && !lj_gc2_test_queue_retry_witness_paused(); i++)
    (void)lj_thr_retry_yield(NULL);
  assert(i < 1000000u);
  assert(lj_arena_lifetime_state_cas(t->arena, t->cell,
	 LJ_ARENA_LIFETIME_DESTRUCT, LJ_ARENA_LIFETIME_LIVE));
  lj_gc2_test_queue_retry_witness_release();
  return NULL;
}

static void test_post_mark_destruct_retry(void)
{
  Fixture f = fixture_open();
  Transition transition;
  GCtab *parent, *child;
  TValue tv;
  pthread_t thread;
  lua_newtable(f.L);
  parent = tabV(f.L->top - 1);
  lua_newtable(f.L);
  child = tabV(f.L->top - 1);
  lua_rawseti(f.L, -2, 1);
  settabV(f.L, &tv, parent);
  enter_sweep(&f);
  assert(!lj_gc2_ismarked(f.g, obj2gco(parent)));
  assert(!lj_gc2_ismarked(f.g, obj2gco(child)));
  transition.arena = lj_arena_of(parent);
  transition.cell = lj_arena_cellof(parent);
  transition.free_alloc = NULL;
  watch.arena = transition.arena;
  lj_gc2_test_queue_post_admit_pause(obj2gco(parent));
  lj_gc2_test_queue_retry_witness_pause(obj2gco(parent));
  assert(pthread_create(&thread, NULL, destruct_between_mark_and_check,
			&transition) == 0);
  lj_gc2_barrier_tv_g(f.g, &tv);
  assert(pthread_join(thread, NULL) == 0);
  assert(lj_arena_bm_get(transition.arena->mark, transition.cell));
  assert(lj_arena_recovery_state_acq(transition.arena, transition.cell) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1);
  memset(&watch, 0, sizeof(watch));
  drain(&f);
  assert(lj_gc2_ismarked(f.g, obj2gco(child)));
  fixture_close(&f);
}

static void test_admitted_small_external_free(void)
{
  Fixture f = fixture_open();
  Transition transition;
  TValue tv;
  GCtab *t;
  pthread_t thread;
  lua_newtable(f.L);
  t = tabV(f.L->top - 1);
  copyTV(f.L, &tv, f.L->top - 1);
  assert(lj_gc_flush_root_pending(f.g));
  assert(lj_gc_unlink_root_obj(f.g, obj2gco(t)) == LJ_GC_ROOT_UNLINKED);
  lua_pop(f.L, 1);
  enter_sweep(&f);
  transition.arena = lj_arena_of(t);
  transition.cell = lj_arena_cellof(t);
  transition.free_alloc = &f.tg->alloc;
  assert(lj_arena_root_state_acq(transition.arena, transition.cell) ==
	 LJ_ARENA_ROOT_NONE);
  watch.arena = transition.arena;
  lj_gc2_test_queue_post_admit_pause(obj2gco(t));
  assert(pthread_create(&thread, NULL, destruct_between_mark_and_check,
			&transition) == 0);
  lj_gc2_barrier_tv_g(f.g, &tv);
  assert(pthread_join(thread, NULL) == 0);
  assert(lj_arena_late_get(transition.arena, transition.cell));
  assert((lj_arena_remote_active_acq(transition.arena) &
	  LJ_ARENA_REMOTE_COUNT_MASK) == 0);
  assert(lj_arena_recovery_state_acq(transition.arena, transition.cell) ==
	 LJ_ARENA_RECOVERY_PENDING);
  assert(gc2_recovery_items_acq(f.g) == 1);
  assert(gc2_recovery_failed_acq(f.g) == 0);
  fixture_close(&f);
}

static void assert_huge_edge(Fixture *f, TValue *tv, void *base,
			     int saturated, int withdraw_wrapper)
{
  LJHugeInfo hi;
  LJHugeReader *readers = NULL;
  uint32_t i, count = saturated ? LJ_ARENA_HUGE_READER_MAX - 1u : 0;
  HugeTab *ht = &f->tg->huge;
  assert(lj_arena_hugetab_lookup(ht, base, &hi) && !hi.readers);
  assert(!(hi.flags & LJ_HUGEF_MARK));
  if (count) {
    readers = (LJHugeReader *)calloc(count, sizeof(*readers));
    assert(readers);
    for (i = 0; i < count; i++)
      assert(lj_arena_hugetab_reader_acquire(ht, base, &readers[i], NULL) ==
	     LJ_ARENA_HUGE_READER_ACQUIRED);
  }
  watch.object = gcV(tv);
  watch.base = base;
  watch.wrapper = ht;
  watch.expected_readers = count + 1u;
  watch.withdraw_wrapper = withdraw_wrapper;
  watch.retire_attempt = 1;
  lj_gc2_barrier_tv_g(f->g, tv);
  assert(watch.observed == 1 && watch.marked_reader == 0);
  assert(watch.marked == 1 && watch.releases == 1);
  assert(lj_arena_hugetab_lookup(ht, base, &hi));
  assert(hi.readers == count && (hi.flags & LJ_HUGEF_MARK));
  assert(!(hi.flags & (LJ_HUGEF_FREEING|LJ_HUGEF_DEFER_FREE|LJ_HUGEF_TICKET)));
  assert(gc2_recovery_failed_acq(f->g) == 0);
  memset(&watch, 0, sizeof(watch));
  for (i = 0; i < count; i++)
    assert(lj_arena_hugetab_reader_release(&readers[i], NULL) ==
	   LJ_ARENA_HUGE_READER_RELEASED);
  free(readers);
  assert(lj_arena_hugetab_lookup(ht, base, &hi) && !hi.readers);
}

static void test_huge_edge(int saturated, int withdraw_wrapper)
{
  Fixture f = fixture_open();
  TValue tv, wrong;
  GCudata *ud;
  LJHugeInfo hi;
  (void)lua_newuserdata(f.L, LJ_HUGE_THRESHOLD + 1024u);
  ud = udataV(f.L->top - 1);
  copyTV(f.L, &tv, f.L->top - 1);
  setgcVraw(&wrong, obj2gco(ud), LJ_TFUNC);
  enter_sweep(&f);
  lj_arena_hugetab_prepare_sweep(&f.tg->huge);
  lj_gc2_barrier_tv_g(f.g, &wrong);
  assert(lj_arena_hugetab_lookup(&f.tg->huge, ud, &hi));
  assert(!(hi.flags & LJ_HUGEF_MARK) && !hi.readers);
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert_huge_edge(&f, &tv, ud, saturated, withdraw_wrapper);
  fixture_close(&f);
}

static void test_admitted_huge_external_free(void)
{
  Fixture f = fixture_open();
  TValue tv;
  GCudata *ud;
  LJHugeInfo hi;
  (void)lua_newuserdata(f.L, LJ_HUGE_THRESHOLD + 1024u);
  ud = udataV(f.L->top - 1);
  copyTV(f.L, &tv, f.L->top - 1);
  assert(lj_gc_flush_root_pending(f.g));
  assert(lj_gc_unlink_root_obj(f.g, obj2gco(ud)) == LJ_GC_ROOT_UNLINKED);
  lua_pop(f.L, 1);
  enter_sweep(&f);
  lj_arena_hugetab_prepare_sweep(&f.tg->huge);
  watch.object = obj2gco(ud);
  watch.base = ud;
  watch.expected_readers = 1;
  watch.external_free = 1;
  lj_gc2_barrier_tv_g(f.g, &tv);
  assert(watch.observed == 1 && watch.marked_reader == 0);
  assert(watch.marked == 1 && watch.releases == 1);
  assert(lj_arena_hugetab_lookup(&f.tg->huge, ud, &hi));
  assert(!hi.readers && (hi.flags & LJ_HUGEF_FREEING));
  assert(!(hi.flags & (LJ_HUGEF_MARK|LJ_HUGEF_DEFER_FREE)));
  assert(gc2_recovery_items_acq(f.g) == 0);
  assert(gc2_recovery_failed_acq(f.g) == 0);
  fixture_close(&f);
}

#if LJ_HASFFI
static void test_cdata_geometry(void)
{
  Fixture f = fixture_open();
  GCcdata *cd = lj_cdata_new_(f.L, CTID_INT8, 1);
  TValue tv;
  GCArena *a = lj_arena_of(cd);
  uint32_t cell = lj_arena_cellof(cd);
  setcdataV(f.L, &tv, cd);
  enter_sweep(&f);
  assert(ctype_ctsG(f.g) == NULL);
  cd->ctypeid = CTID_INT64;  /* Same cells, wrong exact byte-tail certificate. */
  lj_gc2_barrier_tv_g(f.g, &tv);
  assert(!lj_arena_bm_get(a->mark, cell));
  assert(gc2_recovery_items_acq(f.g) == 0);
  cd->ctypeid = CTID_INT8;
  watch.arena = a;
  lj_gc2_barrier_tv_g(f.g, &tv);
  assert(watch.small == 1 && lj_arena_bm_get(a->mark, cell));
  fixture_close(&f);

  f = fixture_open();
  luaL_openlibs(f.L);
  assert(luaL_dostring(f.L,
    "local ffi = require('ffi')\n"
    "return ffi.new('uint8_t[?]', 96), ffi.new('uint8_t[?]', 32768),\n"
    "  ffi.new('struct { char __attribute__((aligned(8192))) a; }')") == 0);
  enter_sweep(&f);
  lj_arena_hugetab_prepare_sweep(&f.tg->huge);
  {
    int i;
    for (i = 0; i < 3; i++) {
      void *base;
      GCSize size;
      cd = cdataV(f.L->top - 3 + i);
      assert(lj_cdata_validate(f.g, cd, &base, &size));
      copyTV(f.L, &tv, f.L->top - 3 + i);
      assert(cdataisv(cd) && base != cd);
      if (size > LJ_HUGE_THRESHOLD) {
	assert_huge_edge(&f, &tv, base, 0, 0);
      } else {
	a = lj_arena_of(base);
	cell = lj_arena_cellof(base);
	assert(!lj_arena_bm_get(a->mark, cell));
	watch.arena = a;
	lj_gc2_barrier_tv_g(f.g, &tv);
	assert(watch.small == 1 && lj_arena_bm_get(a->mark, cell));
	memset(&watch, 0, sizeof(watch));
      }
    }
  }
  fixture_close(&f);
}
#endif

int main(void)
{
  test_small_tag_and_direct_bodies();
  test_committed_mark_modes(LJ_AF_PREPSWEEP|LJ_AF_RECLAIMED,
    LJ_ARENA_REMOTE_SEALED, LJ_ARENA_RESCUE_COMMITTED, 1);
  test_committed_mark_modes(0, LJ_ARENA_REMOTE_SEALED,
    LJ_ARENA_RESCUE_COMMITTED, 1);
  test_committed_mark_modes(LJ_AF_QUARANTINE|LJ_AF_RECLAIMED,
    LJ_ARENA_REMOTE_SEALED, LJ_ARENA_RESCUE_COMMITTED, 0);
  test_committed_mark_modes(LJ_AF_RECLAIMED, LJ_ARENA_REMOTE_SEALED,
    LJ_ARENA_RESCUE_COMMITTED, 0);
  test_committed_mark_modes(0, LJ_ARENA_REMOTE_CLOSED|LJ_ARENA_REMOTE_SEALED,
    LJ_ARENA_RESCUE_BIT_ONLY, 1);
  test_post_mark_destruct_retry();
  test_admitted_small_external_free();
  test_huge_edge(0, 0);
  test_huge_edge(0, 1);
  test_huge_edge(1, 0);
  test_admitted_huge_external_free();
#if LJ_HASFFI
  test_cdata_geometry();
#endif
  puts("t-gc2-sweep-edge-lease OK: one admission, exact marking and retry");
  return 0;
}
