/*
 * Focused positive rooted-hit publication: real full SSB buffers and a full
 * embedded grey queue force normal nonthrowing queue growth under exact leases.
 * The observer uses an existing commit hook; it never attaches to the VM/TG.
 */
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "lua.h"
#include "lauxlib.h"
#include "lj_atomic.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"
#include "lj_vm.h"

static uint32_t watching, result_calls, filler_calls, blocking_smr_calls;
static uint32_t vm_reentry_calls;

static void forbid_vm_reentry(void)
{
  if (la_load32_acq(&watching)) {
    (void)la_add32_rlx(&vm_reentry_calls, 1);
    fprintf(stderr, "unexpected VM/protected-call entry during rooted hit\n");
    abort();
  }
}

void __real_lj_vm_call(lua_State *L, TValue *base, int nres1);
void __wrap_lj_vm_call(lua_State *L, TValue *base, int nres1)
{
  forbid_vm_reentry();
  __real_lj_vm_call(L, base, nres1);
}

int __real_lj_vm_pcall(lua_State *L, TValue *base, int nres1, ptrdiff_t ef);
int __wrap_lj_vm_pcall(lua_State *L, TValue *base, int nres1, ptrdiff_t ef)
{
  forbid_vm_reentry();
  return __real_lj_vm_pcall(L, base, nres1, ef);
}

int __real_lj_vm_cpcall(lua_State *L, lua_CFunction func, void *ud,
                       lua_CPFunction cp);
int __wrap_lj_vm_cpcall(lua_State *L, lua_CFunction func, void *ud,
                       lua_CPFunction cp)
{
  forbid_vm_reentry();
  return __real_lj_vm_cpcall(L, func, ud, cp);
}

int __real_lj_vm_resume(lua_State *L, TValue *base, int nres1, ptrdiff_t ef);
int __wrap_lj_vm_resume(lua_State *L, TValue *base, int nres1, ptrdiff_t ef)
{
  forbid_vm_reentry();
  return __real_lj_vm_resume(L, base, nres1, ef);
}

void __real_lj_gc2_smr_read_enter(global_State *g);
void __wrap_lj_gc2_smr_read_enter(global_State *g)
{
  if (la_load32_acq(&watching)) {
    (void)la_add32_rlx(&blocking_smr_calls, 1);
    fprintf(stderr, "unexpected blocking SMR entry during rooted hit\n");
    abort();
  }
  __real_lj_gc2_smr_read_enter(g);
}

typedef struct Probe {
  lua_State *L;
  global_State *g;
  TGState *tg;
  GCtab *table;
  GCstr *key;
  GCfunc *result, *filler;
  GCArena *table_arena, *key_arena, *result_arena;
  TValue *base, *top, *stack;
  uint64_t table_word, key_word, result_word, owner_word;
  uint32_t anchors, wait_no_l, wait_l, wait_store_l;
  MSize initial_cap;
  GCRef *initial_stack;
  GC2SSBNode *published, *active;
  uint64_t published_items, drained_items, cycle_starts;
  uint64_t total0;
  uint64_t pause_table_readers, pause_key_readers, pause_result_readers;
  uint64_t pause_total;
  uint32_t pause_cap, pause_observed, returned;
} Probe;

static int result_function(lua_State *L)
{
  assert(!la_load32_acq(&watching));
  (void)la_add32_rlx(&result_calls, 1);
  lua_pushinteger(L, 12345);
  return 1;
}

static int filler_function(lua_State *L)
{
  (void)L;
  (void)la_add32_rlx(&filler_calls, 1);
  return 0;
}

static double now(void)
{
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t readers(GCArena *a)
{
  return lj_arena_remote_active_acq(a) & LJ_ARENA_REMOTE_COUNT_MASK;
}

static void assert_gc_live(GCArena *a, GCobj *o)
{
  uint32_t cell = lj_arena_cellof(o);
  assert(lj_arena_bm_get(a->block, cell) == 1);
  assert(lj_arena_ready_get(a, cell) == 1);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_late_get(a, cell) == 0);
}

static void assert_roots(Probe *p, int found)
{
  assert(p->L->base == p->base && p->L->top == p->top);
  assert(tvref(p->L->stack) == p->stack);
  assert(lj_state_owner_word_acq(p->L) == p->owner_word);
  assert(tv_rawload_acq(p->base) == p->table_word);
  assert(tv_rawload_acq(p->base + 1) == p->key_word);
  if (found)
    assert(tv_rawload_acq(p->base + 3) == p->result_word);
  assert(lj_tg_root_anchor_top_acq(p->tg) == p->anchors);
  assert(lj_gc2_rootdesc_snapshot(&p->tg->root_desc, NULL) ==
         LJ_GC2_ROOTDESC_SNAPSHOT_IDLE);
  assert(gc2_cycle_starts_acq(p->g) == p->cycle_starts);
  assert(gc2_phase_acq(p->g) == LJ_GC2_MARK);
  assert(lj_tab_test_wait_no_l_calls() == p->wait_no_l);
  assert(lj_tab_test_wait_l_calls() == p->wait_l);
  assert(lj_tab_test_store_wait_l_calls() == p->wait_store_l);
  assert(la_load32_acq(&result_calls) == 0);
  assert(la_load32_acq(&filler_calls) == 0);
  assert(la_load32_acq(&blocking_smr_calls) == 0);
  assert(la_load32_acq(&vm_reentry_calls) == 0);
}

static void *observe_publication(void *arg)
{
  Probe *p = (Probe *)arg;
  double deadline = now() + 5.0;
  while (lj_gc2_test_recovery_paused() !=
         LJ_GC2_RECOVERY_TEST_SSB_COMMITTED) {
    if (la_load32_acq(&p->returned) || now() >= deadline) {
      fprintf(stderr, "rooted hit did not reach SSB commit pause in 5 seconds\n");
      abort();
    }
    la_cpu_pause();
  }
  /* The marker's temporary filler scope has been released. The table and
   * string key live in distinct arenas, so their remaining counts identify
   * their exact source leases. The result has its read and publication leases.
   */
  assert_roots(p, 1);
  p->pause_table_readers = readers(p->table_arena);
  p->pause_key_readers = readers(p->key_arena);
  p->pause_result_readers = readers(p->result_arena);
  assert(p->pause_table_readers == 1);
  assert(p->pause_key_readers == 1);
  assert(p->pause_result_readers == 2);
  assert_gc_live(p->table_arena, obj2gco(p->table));
  assert_gc_live(p->result_arena, obj2gco(p->result));
  assert(lj_arena_bm_get(p->key_arena->block, lj_arena_cellof(p->key)) == 1);
  assert(p->key->gct == (uint8_t)~LJ_TSTR);
  assert(gc2_smr_readers_acq(p->g) == 0);
  assert(gc2_worker_active_acq(p->g) == 1);
  assert(gc2_ssb_consumer_active_acq(p->g) == 1);
  assert(gc2_recovery_items_acq(p->g) == 0);
  assert(gc2_recovery_huge_items_acq(p->g) == 0);
  assert(lj_tg_ssb_active_acq(p->tg) == p->active);
  assert(lj_tg_ssb_free_acq(p->tg) == NULL);
  assert(lj_tg_ssb_next_acq(p->tg) == lj_tg_ssb_end_acq(p->tg));
  assert(lj_gc2_ssb_count_acq(p->published) == TG_GC2_SSB_SLOTS);
  assert(gcref_acq(p->published->slot[TG_GC2_SSB_SLOTS - 1]) ==
         obj2gco(p->filler));
  assert(gc2_ssb_items_drained_acq(p->g) == p->drained_items);
  p->pause_cap = gc2_grey_capacity_acq(p->g);
  p->pause_total = lj_gc_total_load(p->g);
  assert(p->pause_cap == 2u * p->initial_cap);
  assert(gc2_grey_stack_acq(p->g) != p->initial_stack);
  assert(gc2_grey_top_acq(p->g) == 0);
  assert(gc2_grey_bottom_acq(p->g) == p->initial_cap + 1u);
  assert(p->pause_total == p->total0 +
         (uint64_t)p->pause_cap * sizeof(GCRef));
  assert(lj_gc2_ismarked(p->g, obj2gco(p->result)) == 1);
  la_store32_rel(&p->pause_observed, 1);
  lj_gc2_test_recovery_release();
  return NULL;
}

static void drain_ssb(global_State *g, TGState *tg)
{
  unsigned i;
  (void)lj_gc2_flush_ssb(g, tg);
  for (i = 0; i < 64 && !lj_gc2_test_ssb_empty(g); i++)
    (void)lj_gc2_test_ssb_drain(g);
  assert(lj_gc2_test_ssb_empty(g));
}

static void fill_ssb(Probe *p)
{
  uint32_t n = 0;
  assert(lj_tg_ssb_next_acq(p->tg) == lj_tg_ssb_base_acq(p->tg));
  while (lj_tg_ssb_next_acq(p->tg) != lj_tg_ssb_end_acq(p->tg)) {
    assert(n++ < TG_GC2_SSB_SLOTS);
    assert(lj_gc2_test_ssb_push(p->g, obj2gco(p->filler)) == 1);
  }
  assert(n == TG_GC2_SSB_SLOTS);
}

int main(void)
{
  Probe p = {0};
  pthread_t observer;
  uint32_t pads, keypads, i;
  uint64_t final_cap;
  int hit;
  char keytext[80];
  setvbuf(stdout, NULL, _IONBF, 0);
  p.L = luaL_newstate();
  assert(p.L != NULL);
  p.g = G(p.L);
  p.tg = L2TG(p.L);
  assert(p.tg == G2TG(p.g));
  assert(gc2_n_workers_acq(p.g) == 0);
  /* Stop only for deterministic pre-MARK allocation geometry. */
  lua_gc(p.L, LUA_GCSTOP, 0);
  assert(lua_checkstack(p.L, 12));
  lua_createtable(p.L, 0, 1);             /* 1 source */
  p.table = tabV(p.L->top - 1);
  p.table_arena = lj_arena_of(p.table);
  /* Strings share traversable small allocation storage in this runtime.
   * Use ordinary distinct strings until the key has its own counted arena. */
  for (keypads = 0; keypads < 8192; keypads++) {
    (void)snprintf(keytext, sizeof(keytext),
                   "rooted hit queue growth key %u", keypads);
    lua_pushstring(p.L, keytext);          /* 2 key */
    p.key = strV(p.L->top - 1);
    p.key_arena = lj_arena_of(p.key);
    if (p.key_arena != p.table_arena)
      break;
    lua_pop(p.L, 1);
  }
  assert(keypads < 8192);
  lua_pushcfunction(p.L, filler_function); /* 3 filler */
  p.filler = funcV(p.L->top - 1);
  lua_pushnil(p.L);                        /* 4 output */
  /* Ordinary allocations, no allocator cursor mutation: place the result
   * in another traversable arena for independent retained-lease evidence. */
  for (pads = 0; pads < 8192; pads++) {
    lua_pushcfunction(p.L, result_function);
    p.result = funcV(p.L->top - 1);
    p.result_arena = lj_arena_of(p.result);
    if (p.result_arena != p.table_arena &&
        p.result_arena != p.key_arena)
      break;
    lua_pop(p.L, 1);
  }
  assert(pads < 8192);
  p.result_word = tv_rawload_acq(p.L->top - 1);
  lua_pushvalue(p.L, 2);
  lua_pushvalue(p.L, 5);
  lua_rawset(p.L, 1);
  lua_pop(p.L, 1);                         /* result now solely source-owned */
  assert(lua_gettop(p.L) == 4);
  assert(p.table_arena != p.key_arena);
  assert(p.result_arena != p.table_arena);
  printf("setup keypads=%u resultpads=%u source_arena=%p key_arena=%p "
         "result_arena=%p\n", keypads, pads,
         (void *)p.table_arena, (void *)p.key_arena,
         (void *)p.result_arena);
  /* Settle real public-store handoffs before the new MARK cycle. Draining
   * those handoffs after MARK begins would pre-mark the result and would
   * not exercise publication of a newly discovered GC result. */
  assert(lua_gc(p.L, LUA_GCCOLLECT, 0) == 0);
  assert(gc2_phase_acq(p.g) == LJ_GC2_IDLE);
  lj_gc2_mark_begin(p.g);
  drain_ssb(p.g, p.tg);
  assert(gc2_phase_acq(p.g) == LJ_GC2_MARK);
  assert(lj_gc2_ismarked(p.g, obj2gco(p.result)) == 0);
  assert(gc2_grey_top_acq(p.g) == gc2_grey_bottom_acq(p.g));
  assert(gc2_grey_stack_acq(p.g) == p.g->gc2.grey_embedded);
  /* Re-enable normal GC accounting/progress before the measured operation. */
  lua_gc(p.L, LUA_GCRESTART, -1);
  p.initial_cap = gc2_grey_capacity_acq(p.g);
  p.initial_stack = gc2_grey_stack_acq(p.g);
  assert(p.initial_cap == LJ_GC2_GREY_EMBEDDED);
  for (i = 0; i < p.initial_cap; i++)
    assert(lj_gc2_test_grey_push(p.g, obj2gco(p.filler)) == 1);
  assert(gc2_grey_capacity_acq(p.g) == p.initial_cap);
  fill_ssb(&p);
  assert(lj_gc2_flush_ssb(p.g, p.tg) == TG_GC2_SSB_SLOTS);
  p.published = gc2_ssb_head_acq(p.g);
  p.active = lj_tg_ssb_active_acq(p.tg);
  assert(p.published != NULL && p.published != p.active);
  assert(lj_tg_ssb_free_acq(p.tg) == NULL);
  fill_ssb(&p);
  assert(lj_gc2_ismarked(p.g, obj2gco(p.result)) == 0);
  assert(readers(p.table_arena) == 0 && readers(p.key_arena) == 0 &&
         readers(p.result_arena) == 0);
  assert(gc2_smr_readers_acq(p.g) == 0);
  p.base = p.L->base; p.top = p.L->top; p.stack = tvref(p.L->stack);
  p.table_word = tv_rawload_acq(p.base);
  p.key_word = tv_rawload_acq(p.base + 1);
  p.owner_word = lj_state_owner_word_acq(p.L);
  p.anchors = lj_tg_root_anchor_top_acq(p.tg);
  p.wait_no_l = lj_tab_test_wait_no_l_calls();
  p.wait_l = lj_tab_test_wait_l_calls();
  p.wait_store_l = lj_tab_test_store_wait_l_calls();
  p.published_items = gc2_ssb_items_published_acq(p.g);
  p.drained_items = gc2_ssb_items_drained_acq(p.g);
  p.cycle_starts = gc2_cycle_starts_acq(p.g);
  p.total0 = lj_gc_total_load(p.g);
  printf("before grey_capacity=%u grey_items=%" PRIu64
         " ssb_published=%u ssb_active=%u total=%" PRIu64 "\n",
         (unsigned)p.initial_cap,
         gc2_grey_bottom_acq(p.g) - gc2_grey_top_acq(p.g),
         lj_gc2_ssb_count_acq(p.published), (unsigned)TG_GC2_SSB_SLOTS,
         p.total0);
  lj_gc2_test_recovery_pause(LJ_GC2_RECOVERY_TEST_SSB_COMMITTED);
  assert(pthread_create(&observer, NULL, observe_publication, &p) == 0);
  la_store32_rel(&watching, 1);
  hit = lj_tab_gettv_rooted_hit_try(p.L, p.base, p.base + 1, p.base + 3);
  la_store32_rel(&watching, 0);
  la_store32_rel(&p.returned, 1);
  assert(pthread_join(observer, NULL) == 0);
  assert(hit == 1 && la_load32_acq(&p.pause_observed) == 1);
  assert_roots(&p, 1);
  assert(gc2_smr_readers_acq(p.g) == 0);
  assert(readers(p.table_arena) == 0 && readers(p.key_arena) == 0 &&
         readers(p.result_arena) == 0);
  assert(gc2_worker_active_acq(p.g) == 0);
  assert(gc2_ssb_consumer_active_acq(p.g) == 0);
  assert(gc2_recovery_items_acq(p.g) == 0 &&
         gc2_recovery_huge_items_acq(p.g) == 0);
  assert(gc2_ssb_items_drained_acq(p.g) ==
         p.drained_items + TG_GC2_SSB_SLOTS);
  assert(gc2_ssb_items_published_acq(p.g) ==
         p.published_items + TG_GC2_SSB_SLOTS);
  assert(gc2_grey_bottom_acq(p.g) - gc2_grey_top_acq(p.g) ==
         p.initial_cap + TG_GC2_SSB_SLOTS);
  assert(lj_tg_ssb_next_acq(p.tg) - lj_tg_ssb_base_acq(p.tg) == 1);
  assert(gcref_acq(*lj_tg_ssb_base_acq(p.tg)) == obj2gco(p.result));
  final_cap = gc2_grey_capacity_acq(p.g);
  assert(lj_gc_total_load(p.g) ==
         p.total0 + final_cap * sizeof(GCRef));
  printf("paused grey_capacity=%u total_delta=%" PRIu64
         " source/key/result_readers=%" PRIu64 "/%" PRIu64 "/%" PRIu64
         " smr_readers=0 worker=1 consumer=1\n",
         p.pause_cap, p.pause_total - p.total0, p.pause_table_readers,
         p.pause_key_readers, p.pause_result_readers);
  printf("returned hit=1 grey_capacity=%" PRIu64 " grey_items=%" PRIu64
         " total_delta=%" PRIu64 " result_ssb_items=1 all_leases_released=1\n",
         final_cap, gc2_grey_bottom_acq(p.g) - gc2_grey_top_acq(p.g),
         (uint64_t)lj_gc_total_load(p.g) - p.total0);
  /* Drop the source edge through the public API, preserving only output root.
   * Full GC must keep the returned object usable after all queue work settles. */
  lua_pushvalue(p.L, 2);
  lua_pushnil(p.L);
  lua_rawset(p.L, 1);
  lua_pushvalue(p.L, 2);
  lua_rawget(p.L, 1);
  assert(lua_isnil(p.L, -1));
  lua_pop(p.L, 1);
  assert(lua_gc(p.L, LUA_GCCOLLECT, 0) == 0);
  assert(gc2_phase_acq(p.g) == LJ_GC2_IDLE);
  assert(tv_rawload_acq(p.L->base + 3) == p.result_word);
  lua_pushvalue(p.L, 4);
  assert(lua_pcall(p.L, 0, 1, 0) == 0);
  assert(lua_tointeger(p.L, -1) == 12345);
  assert(la_load32_acq(&result_calls) == 1);
  assert(la_load32_acq(&filler_calls) == 0);
  lua_pop(p.L, 1);
  printf("survival source_edge=nil full_gc=IDLE returned_function=12345 "
         "callbacks_during_hit=0 table_waits=0 blocking_smr_calls=0 "
         "vm_reentry_calls=0\n");
  lua_close(p.L);
  puts("PASS");
  return 0;
}
