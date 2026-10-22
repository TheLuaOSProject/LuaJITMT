/* Linux: scalar owner-root reads retain small bodies/vectors without SMR. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_arena.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_meta.h"
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"

#if !defined(LJ_TAB_TEST_HELPERS) || !defined(LJ_GC2_TEST_HELPERS) || \
    !defined(LJ_ARENA_TEST_HELPERS)
#error "scalar-hit fixture requires table, GC2 and arena helpers"
#endif

static lua_State *new_state(void)
{
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  return L;
}

static void table_roots(lua_State *L)
{
  lua_createtable(L, 16, 8);
  lua_pushinteger(L, 17);
  lua_setfield(L, -2, "offset");
  lua_pushinteger(L, 33);
  lua_rawseti(L, -2, 3);
  lua_pushboolean(L, 1); lua_pushinteger(L, 21); lua_rawset(L, -3);
  lua_pushnumber(L, 1.5); lua_pushinteger(L, 44); lua_rawset(L, -3);
  lua_pushliteral(L, "offset");
  lua_pushinteger(L, 999);
}

static int attempt(lua_State *L, cTValue *t, cTValue *k, TValue *out)
{
  uint32_t readers = gc2_smr_readers_acq(G(L));
  uint32_t anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  uint32_t waits = lj_tab_test_wait_l_calls();
  uint32_t no_l = lj_tab_test_wait_no_l_calls();
  int result = lj_tab_getscalar_rooted_try(L, t, k, out);
  assert(gc2_smr_readers_acq(G(L)) == readers);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == anchors);
  assert(lj_tab_test_wait_l_calls() == waits);
  assert(lj_tab_test_wait_no_l_calls() == no_l);
  return result;
}

static void simple_cases(void)
{
  lua_State *L = new_state();
  TValue *t, *k, *out, old, saved;
  GCtab *tab;
  TValue *slot;
  uint64_t marks[3];
  GCArena *arenas[3];
  uint32_t cells[3], i;
  GCSize total;
  table_roots(L);
  t = L->top - 3; k = L->top - 2; out = L->top - 1;
  tab = tabV(t);
  arenas[0] = lj_arena_of(tab); cells[0] = lj_arena_cellof(tab);
  arenas[1] = lj_arena_of(strV(k)); cells[1] = lj_arena_cellof(strV(k));
  arenas[2] = lj_arena_of(lj_tab_node_hdr(lj_tab_node_acq(tab)));
  cells[2] = lj_arena_cellof(lj_tab_node_hdr(lj_tab_node_acq(tab)));
  for (i = 0; i < 3; i++) marks[i] = la_load64_acq(&arenas[i]->mark[cells[i] >> 6]);
  total = lj_gc_total_load(G(L));
  assert(attempt(L, t, k, out) && numberVnum(out) == 17);
  assert(lj_gc_total_load(G(L)) == total);
  for (i = 0; i < 3; i++) {
    assert(marks[i] == la_load64_acq(&arenas[i]->mark[cells[i] >> 6]));
    assert(lj_arena_remote_active_acq(arenas[i]) == 0);
  }
  saved = *k;
  setintV(k, 3);
  assert(attempt(L, t, k, out) && numberVnum(out) == 33);
  setnumV(k, 3.0);
  assert(attempt(L, t, k, out) && numberVnum(out) == 33);
  setboolV(k, 1);
  assert(attempt(L, t, k, out) && numberVnum(out) == 21);
  setnumV(k, 1.5);
  assert(attempt(L, t, k, out) && numberVnum(out) == 44);
  *k = saved;
  assert(attempt(L, t, k, k) && numberVnum(k) == 17);
  *k = saved;
  old = *t;
  assert(attempt(L, t, k, t) && numberVnum(t) == 17);
  *t = old;

  slot = (TValue *)(void *)lj_tab_getstr(tab, strV(k));
  saved = *slot;
  setboolV(slot, 0);
  assert(attempt(L, t, k, out) && tvisfalse(out));
  setnilV(slot);
  setintV(out, 999);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  setforwardV(slot);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  copyTVrel(L, slot, t);  /* Unsupported GC result, already strongly rooted. */
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  *slot = saved;
  setnilV(k);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  setkeylockV(k);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  setintV(k, 700001);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  lua_close(L);
  puts("simple/nonmarking/aliases/misses passed");
}

static TValue *change_source;
static TValue replacement;
static uint32_t change_stage, change_kind, hook_hits;
static void change_hook(lua_State *L, GCtab *t, uint32_t stage)
{
  if (stage != change_stage) return;
  lj_tab_test_set_scalar_rooted_try_hook(NULL);
  hook_hits++;
  if (change_kind == 0)
    copyTVrel(L, change_source, &replacement);
  else if (change_kind == 1)
    lj_tab_resize(L, t, 128, 6);
  else {
    lj_tab_clear(L, t);
    if (change_kind == 3) {
      TValue *slot = lj_tab_setstr(L, t, strV(change_source));
      setintV(slot, 91);
    }
  }
}

static void source_and_resize(void)
{
  lua_State *L = new_state();
  TValue *t, *k, *out, original;
  GCtab *tab;
  unsigned stage;
  table_roots(L);
  t = L->top - 3; k = L->top - 2; out = L->top - 1;
  original = *t; tab = tabV(t);
  for (stage = LJ_TAB_SCALAR_TEST_SOURCE; stage <= LJ_TAB_SCALAR_TEST_RESULT; stage++) {
    change_source = t; setnilV(&replacement);
    change_stage = stage; change_kind = 0;
    hook_hits = 0;
    lj_tab_test_set_scalar_rooted_try_hook(change_hook);
    assert(!attempt(L, t, k, out) && numberVnum(out) == 999 && hook_hits == 1);
    *t = original;
  }
  original = *k;
  change_source = k; setintV(&replacement, 3);
  change_stage = LJ_TAB_SCALAR_TEST_SOURCE;
  lj_tab_test_set_scalar_rooted_try_hook(change_hook);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  *k = original;
  for (stage = LJ_TAB_SCALAR_TEST_VECTORS; stage <= LJ_TAB_SCALAR_TEST_RESULT; stage++) {
    lj_tab_resize(L, tab, 16, 3);
    change_stage = stage; change_kind = 1; hook_hits = 0;
    lj_tab_test_set_scalar_rooted_try_hook(change_hook);
    assert(!attempt(L, t, k, out) && numberVnum(out) == 999 && hook_hits == 1);
    assert(attempt(L, t, k, out) && numberVnum(out) == 17);
    setintV(out, 999);
  }
  for (stage = 0; stage < LJ_TAB_RETIRE_EPOCHS + 3u; stage++)
    lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE);
  assert(lj_tab_node_retired_head_acq(G(L)) == NULL);
  assert(lj_tab_array_retired_head_acq(G(L)) == NULL);
  assert(attempt(L, t, k, out) && numberVnum(out) == 17);
  change_stage = LJ_TAB_SCALAR_TEST_VALUE; change_kind = 2;
  setintV(out, 999);
  lj_tab_test_set_scalar_rooted_try_hook(change_hook);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  setintV(lj_tab_setstr(L, tab, strV(k)), 17);
  change_source = k; change_kind = 3;
  lj_tab_test_set_scalar_rooted_try_hook(change_hook);
  assert(attempt(L, t, k, out) && numberVnum(out) == 91);
  lua_close(L);
  puts("source replacement/real resize/retirement passed");
}

static void protected_candidates(void)
{
  lua_State *L = new_state();
  TValue *t, *k, *out, original;
  GCtab *tab;
  GCArena *a;
  HugeTab *registry = (HugeTab *)gc2_small_arena_tab_acq(G(L));
  LJHugeInfo hi;
  Node *node, *head, *spare;
  Node saved_head, saved_spare;
  void *mapping = mmap(NULL, 3u * LJ_ARENA_SIZE, PROT_NONE,
		     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  void *guard;
  uint32_t hmask, i;
  assert(mapping != MAP_FAILED);
  guard = (void *)((((uintptr_t)mapping + LJ_ARENA_SIZE - 1u) &
		   ~(uintptr_t)(LJ_ARENA_SIZE - 1u)) + 32768u);
  table_roots(L);
  t = L->top - 3; k = L->top - 2; out = L->top - 1;
  tab = tabV(t); original = *t;
  setgcVraw(t, (GCobj *)guard, LJ_TTAB);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  *t = original;
  original = *k;
  setgcVraw(k, (GCobj *)guard, LJ_TSTR);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  *k = original;
  /* A nonmatching collision key is never dereferenced, even if its bytes are
  ** inaccessible. The requested live string follows it in the actual chain. */
  node = lj_tab_node_acq(tab); hmask = lj_tab_node_hmask_acq(node);
  head = hashstr_node(node, hmask, strV(k)); spare = NULL;
  for (i = 0; i <= hmask; i++)
    if (&node[i] != head && lj_tv_isnil_acq(&node[i].key)) { spare = &node[i]; break; }
  assert(spare);
  saved_head = *head; saved_spare = *spare;
  copyTVrel(L, &spare->key, k); setintV(&spare->val, 17);
  lj_tab_nextnode_rel(spare, NULL);
  setgcVraw(&head->key, (GCobj *)guard, LJ_TSTR);
  lj_tab_nextnode_rel(head, spare);
  assert(attempt(L, t, k, out) && numberVnum(out) == 17);
  setintV(out, 999);
  setkeylockV(&head->key);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  *head = saved_head; *spare = saved_spare;
  /* Registry membership alone does not authorize a body. Protect a raw body
  ** page while allocation metadata says block=0, then use it as a vector. */
  a = lj_arena_map(&L2TG(L)->prng, 0);
  assert(a && registry && lj_arena_hugetab_insert(registry, a, LJ_ARENA_SIZE, 0));
  assert(mprotect((char *)a + 32768, 4096, PROT_NONE) == 0);
  lj_tab_node_rel(tab, (Node *)((char *)a + 32768 + sizeof(TabNodeHdr)));
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  lj_tab_node_rel(tab, node);
  assert(mprotect((char *)a + 32768, 4096, PROT_READ|PROT_WRITE) == 0);
  assert(lj_arena_hugetab_delete(registry, a, &hi));
  lj_arena_unmap(a);
  /* A malformed size cannot extend the admitted vector into another start. */
  lj_tab_node_hdrw(node)->hmask = ((MSize)1u << LJ_MAX_HBITS) - 1u;
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  lj_tab_node_hdrw(node)->hmask = hmask;
  lj_tab_resize(L, tab, 128, 3);
  {
    TValue *array = lj_tab_array_acq(tab);
    uint32_t acap = lj_tab_array_hdrw(array)->acap;
    assert(!lj_tab_array_is_colocated(tab, array));
    lj_tab_array_hdrw(array)->acap = LJ_MAX_ASIZE;
    assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
    lj_tab_array_hdrw(array)->acap = acap;
  }
  assert(munmap(mapping, 3u * LJ_ARENA_SIZE) == 0);
  lua_close(L);
  puts("protected candidates/collision keys/vector bounds passed");
}

static void *custom_alloc(void *ud, void *p, size_t osize, size_t nsize)
{
  (void)ud; (void)osize;
  if (nsize == 0) { free(p); return NULL; }
  return realloc(p, nsize);
}

static void unsupported_cases(void)
{
  lua_State *L = new_state();
  TValue *t, *k, *out, key;
  char *longkey = (char *)malloc(LJ_HUGE_THRESHOLD + 100u);
  assert(longkey);
  memset(longkey, 'z', LJ_HUGE_THRESHOLD + 100u);
  table_roots(L);
  lua_pushlstring(L, longkey, LJ_HUGE_THRESHOLD + 100u);
  free(longkey);
  t = L->top - 4; k = L->top - 3; out = L->top - 2;
  key = *k;
  copyTVrel(L, k, L->top - 1);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  *k = key;
  lj_tab_resize(L, tabV(t), LJ_HUGE_THRESHOLD / sizeof(TValue) + 16u, 3);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  /* The unchanged general implementation still owns this Huge-vector case. */
  assert(lj_meta_tgettv_rooted(L, t, k, out) == out && numberVnum(out) == 17);
  lua_close(L);
  L = lua_newstate(custom_alloc, NULL);
  assert(L);
  table_roots(L);
  t = L->top - 3; k = L->top - 2; out = L->top - 1;
#if LJ_GC2_INTERNAL_ALLOCATOR_ONLY
  /* Public callbacks are currently disabled. Exercise only the new helper's
  ** compatibility preflight; this does not claim custom-runtime validation. */
  assert(la_load32_acq(&G(L)->allocf_arena) == 1);
  la_store32_rel(&G(L)->allocf_arena, 0);
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  la_store32_rel(&G(L)->allocf_arena, 1);
#else
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
#endif
  assert(lj_meta_tgettv_rooted(L, t, k, out) == out && numberVnum(out) == 17);
  lua_close(L);
  puts("Huge fallback/custom allocator preflight passed");
}

typedef struct PauseCtx { global_State *g; uint32_t done; } PauseCtx;
static void *reclaim_main(void *arg)
{
  PauseCtx *ctx = (PauseCtx *)arg;
  (void)lj_gc2_reclaim_retired(ctx->g, lj_gc2_retire_epoch(ctx->g) + 1u);
  la_store32_rel(&ctx->done, 1);
  return NULL;
}

static void paused_reclaimer(void)
{
  lua_State *L = new_state();
  PauseCtx ctx = { G(L), 0 };
  pthread_t thread;
  struct timespec delay = { 0, 1000000 };
  uint32_t i, waits;
  assert(luaL_loadstring(L,
    "return function(t) local s=0 for i=1,50 do s=s+t.offset end return s end") == 0);
  assert(lua_pcall(L, 0, 1, 0) == 0);
  lua_createtable(L, 0, 2);
  lua_pushinteger(L, 17); lua_setfield(L, -2, "offset");
  assert(gc2_phase_acq(ctx.g) == LJ_GC2_IDLE);
  assert(gc2_smr_readers_acq(ctx.g) == 0);
  lj_gc2_test_idle_reclaim_pause_after_jit_quiescence();
  assert(pthread_create(&thread, NULL, reclaim_main, &ctx) == 0);
  for (i = 0; i < 5000 && !lj_gc2_test_idle_reclaim_paused(); i++) {
    assert(!la_load32_acq(&ctx.done)); nanosleep(&delay, NULL);
  }
  assert(lj_gc2_test_idle_reclaim_paused());
  assert(gc2_smr_reclaiming_acq(ctx.g) == LJ_GC2_SMR_META_EXCLUSIVE);
  waits = lj_tab_test_wait_l_calls();
  puts("real IDLE reclaimer paused; entering ordinary scalar field loop"); fflush(stdout);
  alarm(5);  /* The original source-SMR path cannot finish this schedule. */
  assert(lua_pcall(L, 1, 1, 0) == 0);
  alarm(0);
  assert(lua_tointeger(L, -1) == 850);
  assert(lj_tab_test_wait_l_calls() == waits && !la_load32_acq(&ctx.done));
  assert(lj_gc2_test_idle_reclaim_paused());
  lj_gc2_test_idle_reclaim_release();
  assert(pthread_join(thread, NULL) == 0 && la_load32_acq(&ctx.done));
  lua_close(L);
  puts("ordinary scalar field loop completed before reclaimer release");
}

typedef struct FreeCtx { TGAlloc *alloc; void *p; size_t size; } FreeCtx;
static void *plain_free_main(void *arg)
{
  FreeCtx *ctx = (FreeCtx *)arg;
  lj_arena_free(ctx->alloc, ctx->p, ctx->size);
  return NULL;
}
static void paused_plain_writer(void)
{
  lua_State *L = new_state();
  TValue *t, *k, *out;
  FreeCtx ctx;
  pthread_t thread;
  GCArena *a;
  struct timespec delay = { 0, 1000000 };
  uint32_t i;
  table_roots(L);
  t = L->top - 3; k = L->top - 2; out = L->top - 1;
  a = lj_arena_of(lj_tab_node_hdr(lj_tab_node_acq(tabV(t))));
  ctx.alloc = &L2TG(L)->alloc; ctx.size = 64;
  ctx.p = lj_mem_new(L, ctx.size);
  assert(ctx.p && lj_arena_of(ctx.p) == a);
  lj_arena_test_plain_claim_pause(1);
  assert(pthread_create(&thread, NULL, plain_free_main, &ctx) == 0);
  for (i = 0; i < 5000 && !lj_arena_test_plain_claim_paused(); i++) nanosleep(&delay, NULL);
  assert(lj_arena_test_plain_claim_paused());
  assert(!attempt(L, t, k, out) && numberVnum(out) == 999);
  lj_arena_test_plain_claim_pause(0);
  assert(pthread_join(thread, NULL) == 0);
  lj_gc_total_sub(G(L), (GCSize)ctx.size);  /* arena free owned the physical step. */
  assert(attempt(L, t, k, out) && numberVnum(out) == 17);
  lua_close(L);
  puts("paused unrelated plain-arena writer remains a bounded refusal");
}

int main(int argc, char **argv)
{
  if (argc > 1 && strcmp(argv[1], "paused-only") == 0) {
    paused_reclaimer();
    return 0;
  }
  simple_cases();
  source_and_resize();
  protected_candidates();
  unsupported_cases();
  paused_reclaimer();
  paused_plain_writer();
  puts("scalar table-hit tests passed");
  return 0;
}
