/* V1 independent scalar iterator authority probes. All stage hooks below
** only change already owned words; they never allocate, throw, move a stack,
** wait, or call Lua. Real resize/GC takes place between helper attempts. */
#include <assert.h>
#include <math.h>
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
#include "lj_obj.h"
#include "lj_state.h"
#include "lj_tab.h"
#include "lj_tg.h"

#if !defined(LJ_TAB_TEST_HELPERS) || !defined(LJ_GC2_TEST_HELPERS) || !defined(LJ_ARENA_TEST_HELPERS)
#error "authority fixture requires table/GC2/arena helpers"
#endif

static GCArena *known_array_arena;
static uint32_t known_array_cell;

static lua_State *new_state(int separate)
{
  lua_State *L = luaL_newstate();
  unsigned i;
  assert(L); luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  assert(lua_checkstack(L, 128));
  lua_createtable(L, separate ? 40 : 8, 0);
  for (i = 0; i <= 3; i++) {
    lua_pushinteger(L, i ? (int)i * 11 : 7); lua_rawseti(L, 1, (int)i);
  }
  lua_pushnil(L); lua_pushinteger(L, 998); lua_pushinteger(L, 999);
  lua_pushvalue(L, 1); /* Persistent backup survives either aliased output. */
  lua_createtable(L, 64, 0); /* Existing replacement array root. */
  lua_pushinteger(L, 91); lua_rawseti(L, 6, 0);
  lua_createtable(L, 0, 4); /* Existing non-nil replacement node root. */
  lua_pushinteger(L, 93); lua_setfield(L, 7, "x");
  (void)lua_gc(L, LUA_GCCOLLECT, 0);
  assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE);
  (void)lua_gc(L, LUA_GCSTOP, 0);
  assert(lj_tab_node_acq(tabV(L->base)) == &G(L)->nilnode);
  {
    GCtab *t = tabV(L->base); TValue *a = lj_tab_array_acq(t);
    known_array_arena = NULL;
    if (!lj_tab_array_is_colocated(t, a)) {
      known_array_arena = lj_arena_of(lj_tab_array_hdr(a));
      known_array_cell = lj_arena_cellof(lj_tab_array_hdr(a));
    }
  }
  return L;
}

static int attempt(lua_State *L, cTValue *t, cTValue *k, TValue *ok, TValue *ov, uint32_t *idx)
{
  GCArena *a[2]; uint64_t marks[2]; uint32_t cells[2], n = 1, i;
  GCtab *rooted = tabV(L->base + 4);
  uint32_t readers = gc2_smr_readers_acq(G(L));
  uint32_t anchors = lj_tg_root_anchor_top_acq(L2TG(L));
  uint32_t waits = lj_tab_test_wait_l_calls(), no_l = lj_tab_test_wait_no_l_calls();
  uint64_t dirty = lj_tg_stack_dirty_epoch_acq(L2TG(L));
  GCSize total = lj_gc_total_load(G(L));
  TValue beforek = *ok, beforev = *ov;
  uint32_t beforeidx = idx ? *idx : 0;
  int result;
  a[0] = lj_arena_of(rooted); cells[0] = lj_arena_cellof(rooted);
  /* A malformed current pointer must never drive fixture header reads. */
  if (known_array_arena) {
    a[n] = known_array_arena; cells[n++] = known_array_cell;
  }
  for (i = 0; i < n; i++) marks[i] = la_load64_acq(&a[i]->mark[cells[i] >> 6]);
  result = lj_tab_test_nextscalar_rooted_try(L, t, k, ok, ov, idx);
  assert(gc2_smr_readers_acq(G(L)) == readers);
  assert(lj_tg_root_anchor_top_acq(L2TG(L)) == anchors);
  assert(lj_tab_test_wait_l_calls() == waits && lj_tab_test_wait_no_l_calls() == no_l);
  assert(lj_gc_total_load(G(L)) == total);
  for (i = 0; i < n; i++) {
    assert(lj_arena_remote_active_acq(a[i]) == 0);
    assert(marks[i] == la_load64_acq(&a[i]->mark[cells[i] >> 6]));
  }
  if (result != 1) {
    assert(tv_rawload(ok) == tv_rawload(&beforek));
    assert(tv_rawload(ov) == tv_rawload(&beforev));
    assert(!idx || *idx == beforeidx);
    assert(lj_tg_stack_dirty_epoch_acq(L2TG(L)) == dirty);
  }
  return result;
}

static int probe(lua_State *L, uint32_t *idx)
{
  return attempt(L, L->base, L->base + 1, L->base + 2, L->base + 3, idx);
}

static void basic_cases(int separate)
{
  lua_State *L = new_state(separate);
  GCtab *t = tabV(L->base); TValue *a = lj_tab_array_acq(t);
  uint32_t idx = 719, count = 0, i; double sum = 0;
  int status;
  while ((status = probe(L, &idx)) == 1) {
    assert((uint32_t)intV(L->base + 2) == count && idx == count + 1u);
    sum += numberVnum(L->base + 3); count++;
    * (L->base + 1) = *(L->base + 2);
  }
  assert(status == 0 && count == 4 && sum == 73);
  for (i = 0; i < lj_tab_asize_acq(t); i++) setnilV(&a[i]);
  setnilV(L->base + 1); assert(probe(L, &idx) == 0);
  setnumV(&a[0], NAN); assert(probe(L, &idx) == 1 && isnan(numV(L->base + 3)));
  setnumV(&a[0], INFINITY); assert(probe(L, &idx) == 1 && numV(L->base + 3) == INFINITY);
  setnumV(&a[0], -INFINITY); assert(probe(L, &idx) == 1 && numV(L->base + 3) == -INFINITY);
  setnumV(&a[0], -0.0); assert(probe(L, &idx) == 1 && signbit(numV(L->base + 3)));
  setboolV(&a[0], 0); assert(probe(L, &idx) == 1 && tvisfalse(L->base + 3));
  setboolV(&a[0], 1); assert(probe(L, &idx) == 1 && tvistrue(L->base + 3));
  assert(lj_arena_remote_active_acq(lj_arena_of(t)) == 0);
  if (!lj_tab_array_is_colocated(t, a))
    assert(lj_arena_remote_active_acq(lj_arena_of(lj_tab_array_hdr(a))) == 0);
  lua_close(L);
}

static void key_cases(void)
{
  lua_State *L = new_state(0); TValue *k = L->base + 1;
  uint32_t idx = 719; unsigned i;
  double bad[] = {-1.0, 0.5, -0.5, 1000.0, NAN, INFINITY, -INFINITY, 4294967296.0};
  setintV(k, 0); assert(probe(L, &idx) == 1 && intV(L->base + 2) == 1);
  setnumV(k, 0.0); assert(probe(L, &idx) == 1 && intV(L->base + 2) == 1);
  setnumV(k, -0.0); assert(probe(L, &idx) == 1 && intV(L->base + 2) == 1);
  setnumV(k, 2.0); assert(probe(L, &idx) == 1 && intV(L->base + 2) == 3);
  setintV(k, -1); assert(probe(L, &idx) == -2);
  setintV(k, INT32_MAX); assert(probe(L, &idx) == -2);
  for (i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) { setnumV(k, bad[i]); assert(probe(L, &idx) == -2); }
  k->u32.hi = LJ_KEYINDEX; k->u32.lo = 0;
  assert(probe(L, &idx) == 1 && intV(L->base + 2) == 0);
  k->u32.hi = LJ_KEYINDEX; k->u32.lo = UINT32_MAX; assert(probe(L, &idx) == 0);
  setnilV(k); lua_close(L);
}

static void alias_cases(void)
{
  lua_State *L = new_state(0); unsigned ki, vi;
  TValue localkey, localval;
  uint32_t idx;
  for (ki = 0; ki < 4; ki++) for (vi = 0; vi < 4; vi++) {
    if (ki == vi) continue;
    *L->base = *(L->base + 4); setnilV(L->base + 1);
    setintV(L->base + 2, 998); setintV(L->base + 3, 999); idx = 719;
    assert(attempt(L, L->base, L->base + 1, L->base + ki, L->base + vi, &idx) == 1);
    assert(intV(L->base + ki) == 0 && intV(L->base + vi) == 7 && idx == 1);
  }
  *L->base = *(L->base + 4); setnilV(L->base + 1);
  setintV(&localkey, 998); setintV(&localval, 999); idx = 719;
  assert(attempt(L, L->base, L->base + 1, &localkey, &localval, &idx) == 1);
  assert(intV(&localkey) == 0 && intV(&localval) == 7 && idx == 1);
  assert(attempt(L, L->base, L->base + 1, &localkey, &localkey, &idx) == -2);
  for (ki = 0; ki < 4; ki++) for (vi = 0; vi < 4; vi++) {
    if (ki == vi) continue;
    *L->base = *(L->base + 4); setintV(L->base + 1, 3);
    setintV(L->base + 2, 998); setintV(L->base + 3, 999);
    assert(attempt(L, L->base, L->base + 1, L->base + ki, L->base + vi, &idx) == 0);
    setintV(L->base + 1, -1);
    assert(attempt(L, L->base, L->base + 1, L->base + ki, L->base + vi, &idx) == -2);
  }
  *L->base = *(L->base + 4); lua_close(L);
}

static void opaque_cases(void)
{
  lua_State *L = new_state(0); TValue *a = lj_tab_array_acq(tabV(L->base));
  TValue saved = a[0]; uint32_t idx = 719;
  a[0] = *(L->base + 4); assert(probe(L, &idx) == -2); /* Must not skip first GC result. */
  setforwardV(&a[0]); assert(probe(L, &idx) == -2);
  setkeylockV(&a[0]); assert(probe(L, &idx) == -2);
  a[0].u32.hi = LJ_KEYINDEX; a[0].u32.lo = 0; assert(probe(L, &idx) == -2);
  a[0] = saved;
  setboolV(L->base + 1, 0); assert(probe(L, &idx) == -2);
  setkeylockV(L->base + 1); assert(probe(L, &idx) == -2);
  setnilV(L->base + 1);
  lj_tab_node_rel(tabV(L->base), lj_tab_node_acq(tabV(L->base + 6)));
  assert(probe(L, &idx) == -2);
  lj_tab_node_rel(tabV(L->base), &G(L)->nilnode);
  lua_close(L);
}

static void protected_cases(void)
{
  lua_State *L = new_state(1); GCtab *t = tabV(L->base);
  TValue savedt = *L->base, *a = lj_tab_array_acq(t), saved = a[0];
  uint32_t idx = 719;
  void *mapping = mmap(NULL, 3u*LJ_ARENA_SIZE, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  void *guard; GCArena *arena; LJHugeInfo hi;
  HugeTab *registry = (HugeTab *)gc2_small_arena_tab_acq(G(L));
  assert(mapping != MAP_FAILED);
  guard = (void *)((((uintptr_t)mapping + LJ_ARENA_SIZE-1u) & ~(uintptr_t)(LJ_ARENA_SIZE-1u)) + 32768u);
  setgcVraw(L->base, (GCobj *)guard, LJ_TTAB); assert(probe(L, &idx) == -2); *L->base = savedt;
  setgcVraw(L->base + 1, (GCobj *)guard, LJ_TSTR); assert(probe(L, &idx) == -2); setnilV(L->base + 1);
  setgcVraw(&a[0], (GCobj *)guard, LJ_TTAB); assert(probe(L, &idx) == -2); a[0] = saved;
  lj_tab_array_rel(t, (TValue *)((char *)guard + sizeof(TabArrayHdr)));
  assert(probe(L, &idx) == -2); lj_tab_array_rel(t, a);
  lj_tab_node_rel(t, (Node *)guard); assert(probe(L, &idx) == -2); lj_tab_node_rel(t, &G(L)->nilnode);
  arena = lj_arena_map(&L2TG(L)->prng, 0);
  assert(arena && registry && lj_arena_hugetab_insert(registry, arena, LJ_ARENA_SIZE, 0));
  assert(mprotect((char *)arena + 32768, 4096, PROT_NONE) == 0);
  lj_tab_array_rel(t, (TValue *)((char *)arena + 32768 + sizeof(TabArrayHdr)));
  assert(probe(L, &idx) == -2); lj_tab_array_rel(t, a);
  setgcVraw(L->base, (GCobj *)((char *)arena + 32768), LJ_TTAB);
  assert(probe(L, &idx) == -2); *L->base = savedt;
  assert(mprotect((char *)arena + 32768, 4096, PROT_READ|PROT_WRITE) == 0);
  assert(lj_arena_hugetab_delete(registry, arena, &hi)); lj_arena_unmap(arena);
  assert(munmap(mapping, 3u*LJ_ARENA_SIZE) == 0);
  lua_close(L);
}

static void bounds_cases(void)
{
  lua_State *L = new_state(1); GCtab *t = tabV(L->base);
  TValue *a = lj_tab_array_acq(t); TabArrayHdr *hdr = lj_tab_array_hdrw(a);
  MSize acap = hdr->acap, asize = hdr->asize, cached = lj_tab_asize_acq(t);
  uint32_t idx = 719;
  assert(!lj_tab_array_is_colocated(t, a));
  hdr->acap = LJ_MAX_ASIZE; assert(probe(L, &idx) == -2); hdr->acap = acap;
  hdr->asize = acap + 1u; assert(probe(L, &idx) == -2); hdr->asize = asize;
  hdr->acap |= TABARRAY_FLAG_RETIRING; assert(probe(L, &idx) == -2); hdr->acap = acap;
  lj_tab_array_rel(t, NULL); assert(probe(L, &idx) == -2);
  lj_tab_asize_rel(t, 0); assert(probe(L, &idx) == 0);
  lj_tab_array_rel(t, a); lj_tab_asize_rel(t, cached);
  lua_close(L);
  L = new_state(0); t = tabV(L->base); a = lj_tab_array_acq(t);
  assert(lj_tab_array_is_colocated(t, a));
  {
    int8_t colo = lj_tab_colo_acq(t);
    lj_tab_colo_rel(t, 0); assert(probe(L, &idx) == -2);
    lj_tab_colo_rel(t, -colo); assert(probe(L, &idx) == -2);
    lj_tab_colo_rel(t, 1); assert(probe(L, &idx) == -2);
    lj_tab_colo_rel(t, LJ_MAX_COLOSIZE + 1); assert(probe(L, &idx) == -2);
    lj_tab_colo_rel(t, colo);
  }
  lua_close(L);
}

static uint32_t stage_wanted, hook_kind, hook_hits;
static TValue *source_word, replacement;
static GCtab *hook_table, *alternate;
static LJStateOwner saved_owner;
static void word_hook(lua_State *L, GCtab *unused, uint32_t stage)
{
  UNUSED(unused);
  if (stage != stage_wanted) return;
  lj_tab_test_set_scalar_rooted_try_hook(NULL); hook_hits++;
  if (hook_kind == 0) copyTVrel(L, source_word, &replacement);
  else if (hook_kind == 1) lj_tab_array_rel(hook_table, lj_tab_array_acq(alternate));
  else if (hook_kind == 2) lj_tab_node_rel(hook_table, lj_tab_node_acq(alternate));
  else if (hook_kind == 3) lj_state_owner_word_rel(L, 0);
  else if (hook_kind == 4) lj_tab_asize_rel(hook_table, 1);
  else { assert(hook_kind == 5); lj_tab_colo_rel(hook_table, 1); }
}

static void hooks_cases(int end)
{
  lua_State *L = new_state(0); GCtab *t = tabV(L->base);
  TValue *a = lj_tab_array_acq(t), savedt = *L->base, savedkey;
  MSize asize = lj_tab_asize_acq(t); int8_t colo = lj_tab_colo_acq(t);
  uint32_t idx = 719, stage, kind;
  if (end) setintV(L->base + 1, 3);
  savedkey = *(L->base + 1); hook_table = t;
  for (stage = LJ_TAB_SCALAR_TEST_SOURCE; stage <= LJ_TAB_SCALAR_TEST_RESULT; stage++) {
    for (kind = 0; kind < 4; kind++) {
      *L->base = savedt; *(L->base + 1) = savedkey;
      hook_kind = kind == 3 ? 3 : 0; stage_wanted = stage; hook_hits = 0;
      source_word = kind == 1 ? L->base + 1 : L->base;
      if (kind == 0) setnilV(&replacement);
      else if (kind == 1) setintV(&replacement, 2);
      else replacement = *(L->base + 5); /* New valid table differs by identity. */
      saved_owner = lj_state_owner_word_acq(L);
      lj_tab_test_set_scalar_rooted_try_hook(word_hook);
      assert(probe(L, &idx) == -2 && hook_hits == 1);
      lj_state_owner_word_rel(L, saved_owner);
    }
  }
  *L->base = savedt; *(L->base + 1) = savedkey;
  for (stage = LJ_TAB_SCALAR_TEST_VECTORS; stage <= LJ_TAB_SCALAR_TEST_RESULT; stage++) {
    for (kind = 1; kind <= 2; kind++) {
      stage_wanted = stage; hook_kind = kind; hook_hits = 0;
      alternate = tabV(L->base + (kind == 1 ? 5 : 6));
      lj_tab_test_set_scalar_rooted_try_hook(word_hook);
      assert(probe(L, &idx) == -2 && hook_hits == 1);
      lj_tab_array_rel(t, a); lj_tab_node_rel(t, &G(L)->nilnode);
    }
  }
  for (kind = 4; kind <= 5; kind++) {
    stage_wanted = LJ_TAB_SCALAR_TEST_RESULT; hook_kind = kind; hook_hits = 0;
    lj_tab_test_set_scalar_rooted_try_hook(word_hook);
    assert(probe(L, &idx) == -2 && hook_hits == 1);
    lj_tab_asize_rel(t, asize); lj_tab_colo_rel(t, colo);
  }
  assert(probe(L, &idx) == (end ? 0 : 1));
  lua_close(L);
}

static void resize_cases(void)
{
  lua_State *L = new_state(0); GCtab *t = tabV(L->base); unsigned i;
  uint32_t idx = 719;
  for (i = 0; i < 8; i++) {
    assert(probe(L, &idx) == 1 && intV(L->base + 3) == 7);
    lj_tab_resize(L, t, 40u + i*17u, 0);
    (void)lua_gc(L, LUA_GCCOLLECT, 0);
    assert(gc2_phase_acq(G(L)) == LJ_GC2_IDLE);
    (void)lua_gc(L, LUA_GCSTOP, 0);
    known_array_arena = lj_arena_of(lj_tab_array_hdr(lj_tab_array_acq(t)));
    known_array_cell = lj_arena_cellof(lj_tab_array_hdr(lj_tab_array_acq(t)));
  }
  for (i = 0; i < LJ_TAB_RETIRE_EPOCHS+3u; i++) (void)lua_gc(L, LUA_GCCOLLECT, 0);
  assert(lj_tab_array_retired_head_acq(G(L)) == NULL);
  assert(lj_tab_node_retired_head_acq(G(L)) == NULL);
  assert(probe(L, &idx) == 1 && intV(L->base + 3) == 7);
  lj_tab_resize(L, t, LJ_HUGE_THRESHOLD/sizeof(TValue)+16u, 0);
  assert(probe(L, &idx) == -2);
  assert(lj_tab_next_rooted(L, L->base, L->base+1, L->base+2, L->base+3, &idx) == 1);
  assert(intV(L->base+2) == 0 && intV(L->base+3) == 7);
  lua_close(L);
}

typedef struct FreeCtx { TGAlloc *alloc; void *p; size_t size; } FreeCtx;
static void *free_main(void *arg)
{
  FreeCtx *ctx = (FreeCtx *)arg; lj_arena_free(ctx->alloc, ctx->p, ctx->size); return NULL;
}
static void plain_writer(void)
{
  lua_State *L = new_state(1); FreeCtx ctx; pthread_t worker;
  GCArena *a = lj_arena_of(lj_tab_array_hdr(lj_tab_array_acq(tabV(L->base))));
  struct timespec delay = {0, 1000000}; uint32_t i, idx = 719;
  ctx.alloc = &L2TG(L)->alloc; ctx.size = 64; ctx.p = lj_mem_new(L, ctx.size);
  assert(ctx.p && lj_arena_of(ctx.p) == a);
  lj_arena_test_plain_claim_pause(1);
  assert(pthread_create(&worker, NULL, free_main, &ctx) == 0);
  for (i = 0; i < 5000 && !lj_arena_test_plain_claim_paused(); i++) nanosleep(&delay, NULL);
  assert(lj_arena_test_plain_claim_paused());
  alarm(3); assert(probe(L, &idx) == -2); alarm(0);
  assert(lj_arena_remote_active_acq(a) == 0);
  lj_arena_test_plain_claim_pause(0); assert(pthread_join(worker, NULL) == 0);
  lj_gc_total_sub(G(L), (GCSize)ctx.size);
  assert(probe(L, &idx) == 1 && intV(L->base + 3) == 7);
  lua_close(L);
}

int main(int argc, char **argv)
{
  const char *mode; assert(argc == 2); mode = argv[1];
  if (!strcmp(mode, "basic-colo")) basic_cases(0);
  else if (!strcmp(mode, "basic-separate")) basic_cases(1);
  else if (!strcmp(mode, "keys")) key_cases();
  else if (!strcmp(mode, "aliases")) alias_cases();
  else if (!strcmp(mode, "opaque")) opaque_cases();
  else if (!strcmp(mode, "protected")) protected_cases();
  else if (!strcmp(mode, "bounds")) bounds_cases();
  else if (!strcmp(mode, "hooks-found")) hooks_cases(0);
  else if (!strcmp(mode, "hooks-end")) hooks_cases(1);
  else if (!strcmp(mode, "resize")) resize_cases();
  else { assert(!strcmp(mode, "plain")); plain_writer(); }
  printf("scalar next authority passed: %s\n", mode); return 0;
}
