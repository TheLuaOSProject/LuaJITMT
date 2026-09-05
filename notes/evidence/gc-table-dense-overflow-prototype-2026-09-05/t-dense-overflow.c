/* Isolated reserved dense-W study. No canonical registration. */
#define main original_coalescing_main
#include "coalescing-adapter.c"
#undef main
#include "lualib.h"

static uint32_t exact_scan_result;
static void *scan_exact(void *arg)
{
  Fixture *f = (Fixture *)arg;
  la_store32_rel(&exact_scan_result,
    (uint32_t)lj_gc2_test_table_token_scan_one(f->g, f->parent));
  return NULL;
}

static void test_old_scanner(int huge, int wide, int exact)
{
  Fixture f = open_fixture_phase(huge, 1, !exact);
  pthread_t scanner;
  uint64_t ticket = 0;
  dense_seed(f.parent, 17u, 1u, 0, wide);
  if (exact) {
    ticket = lj_gc2_test_table_token_request(f.g, f.parent);
    assert(ticket != 0);
  } else {
    assert(lj_gc2_test_ssb_push(f.g, obj2gco(f.parent)) == 1);
    assert(lj_gc2_flush_ssb(f.g, f.tg) == 1);
  }
  watch(&f, LJ_GC2_TABLE_COALESCE_TEST_PRE_PROOF);
  assert(pthread_create(&scanner, NULL,
    exact ? scan_exact : scan_to_proof, &f) == 0);
  wait_value(&paused, LJ_GC2_TABLE_COALESCE_TEST_PRE_PROOF);
  if (wide) {
    dense_seed(f.parent, 17u, UINT32_MAX, 0, 1);
  } else {
    /* The new W serial equals the old inline serial: checking low bits
    ** without checking the captured domain would accept a false proof. */
    dense_seed(f.parent, 17u, 0u, 0, 0);
    la_store64_rel(&f.stamp->state, UINT32_MAX - 1u);
  }
  store_child(&f);
  if (exact)
    lj_gc2_barrier_tab_g(f.g, f.parent);
  else
    publish(&f, 0);
  assert(!lj_gc2_activation_reclaim_veto(f.g));
  assert((uint32_t)la_load64_acq(&f.stamp->state) == UINT32_MAX);
  assert(dense_snapshot(f.parent).hi == (wide ? 18u : 17u));
  assert((uint32_t)dense_snapshot(f.parent).lo == 1u);
  la_store32_rel(&released, 1);
  assert(pthread_join(scanner, NULL) == 0);
  if (exact) {
    uint64_t control = la_load64_acq(&f.stamp->token.control);
    assert(exact_scan_result == 0);
    assert(lj_gc2_table_token_generation(control) == ticket);
    assert(lj_gc2_table_token_state(control) == LJ_GC2_TABLE_TOKEN_PENDING);
    assert(lj_gc2_test_table_token_scan_one(f.g, f.parent) == 1);
    assert(lj_gc2_table_token_state(la_load64_acq(&f.stamp->token.control)) ==
           LJ_GC2_TABLE_TOKEN_NONE);
  }
  drain(&f);
  assert_graph(&f);
  assert(scans >= 2);
  close_fixture(&f);
  printf("old scanner huge=%d wide=%d exact=%d rejected\n", huge, wide, exact);
}

static void *peer_promote(void *arg)
{
  Fixture *f = (Fixture *)arg;
  uint32_t stage;
  do { stage = la_load32_acq(&paused); } while (!stage);
  assert((uint32_t)(dense_snapshot(f->parent).lo >> 32) == 0);
  assert((uint32_t)la_load64_acq(&f->stamp->state) ==
    (stage == LJ_GC2_TABLE_COALESCE_TEST_PRE_MODE ?
     UINT32_MAX - 1u : UINT32_MAX));
  publish(f, 0);  /* Completes while the first publisher remains paused. */
  assert((uint32_t)la_load64_acq(&f->stamp->state) == UINT32_MAX);
  la_store32_rel(&released, 1);
  return NULL;
}

static void test_mode_pause(int huge, uint32_t stage)
{
  Fixture f = open_fixture(huge, 1);
  pthread_t peer;
  uint32_t cycle = gc2_cycle_acq(f.g);
  dense_seed(f.parent, 73u, 5u, cycle, 0);
  /* Simulate a reused cell's old current-cycle W, with a real completed
  ** semantic request waiting in the new incarnation's ordinary queue. */
  store_child(&f);
  publish(&f, 0);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == 1);
  la_store64_rel(&f.stamp->state, UINT32_MAX - 1u);
  watch(&f, stage);
  assert(pthread_create(&peer, NULL, peer_promote, &f) == 0);
  publish(&f, 0);
  assert(pthread_join(peer, NULL) == 0);
  drain(&f);
  assert_graph(&f);
  assert(!lj_gc2_activation_reclaim_veto(f.g));
  close_fixture(&f);
  printf("mode pause huge=%d stage=%u peer completed\n", huge, stage);
}

static void lua_ok(lua_State *L, const char *s)
{
  if (luaL_dostring(L, s)) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    abort();
  }
}

static void test_continued_collection(int huge)
{
  Fixture f;
  uint32_t round;
  memset(&f, 0, sizeof(f));
  f.L = luaL_newstate(); assert(f.L); luaL_openlibs(f.L);
  lua_gc(f.L, LUA_GCSTOP, 0);
  f.g = G(f.L); f.tg = G2TG(f.g);
  lua_newtable(f.L);
  f.parent = huge ? huge_table(&f) : tabV(f.L->top - 1);
  if (huge) settabV(f.L, f.L->top - 1, f.parent);
  lua_setglobal(f.L, "p");
  lua_ok(f.L, "weak = setmetatable({}, {__mode='v'})");
  f.stamp = lj_arena_gc2_stamp_acq(f.parent);
  for (round = 0; round < 12; round++) {
    uint64_t era;
    lua_ok(f.L, "p.keep = {n=2718, child={value=314}}; weak[1] = {}");
    era = dense_snapshot(f.parent).hi;
    dense_seed(f.parent, era, UINT32_MAX, 0, 1);
    lj_gc2_test_table_dirty_bump(f.g, f.parent);
    assert(dense_snapshot(f.parent).hi == era + 1);
    assert(!lj_gc2_activation_reclaim_veto(f.g));
    assert(lua_gc(f.L, LUA_GCCOLLECT, 0) == 0);
    assert(gc2_phase_acq(f.g) == LJ_GC2_IDLE);
    assert(gc2_recovery_items_acq(f.g) == 0);
    assert(!lj_gc2_activation_reclaim_veto(f.g));
    lua_ok(f.L, "assert(weak[1] == nil); assert(p.keep.n == 2718 and p.keep.child.value == 314)");
  }
  lua_ok(f.L, "p=nil; weak=nil");
  lua_settop(f.L, 0);
  close_fixture(&f);
  printf("huge=%d twelve rollover collections reached IDLE\n", huge);
}

#if defined(DENSE_WRAP_CALLOC)
extern void *__real_calloc(size_t, size_t);
static int deny_calloc;
static uint32_t denied_calloc_calls;
void *__wrap_calloc(size_t n, size_t size)
{
  if (deny_calloc) { denied_calloc_calls++; return NULL; }
  return __real_calloc(n, size);
}
#endif

static void test_reserved_failure(void)
{
  Fixture f = open_fixture(0, 0);
  GCArena *plain;
  lj_arena_test_gc2_sidecar_fail_alloc(1);
  assert(lj_arena_map(&f.tg->prng, LJ_AF_TRAVERSABLE) == NULL);
  assert(lj_arena_huge_map(&f.tg->prng, LJ_HUGE_THRESHOLD + 4096u,
                         LJ_AF_TRAVERSABLE) == NULL);
  plain = lj_arena_map(&f.tg->prng, 0); assert(plain);
  assert(lj_arena_gc2_tabstamp_acq(plain) == NULL);
  lj_arena_unmap(plain);
  lj_arena_test_gc2_sidecar_fail_alloc(0);
  dense_seed(f.parent, 0, 0, 0, 0);
  la_store64_rel(&f.stamp->state, UINT32_MAX - 1u);
  store_child(&f);
#if defined(DENSE_WRAP_CALLOC)
  deny_calloc = 1;
#endif
  lj_gc2_test_table_dirty_bump(f.g, f.parent);
#if defined(DENSE_WRAP_CALLOC)
  deny_calloc = 0;
  assert(denied_calloc_calls == 0);
#endif
  assert(dense_snapshot(f.parent).lo == 1);
  assert(!lj_gc2_activation_reclaim_veto(f.g));
  publish(&f, 0); drain(&f); assert_graph(&f);
  close_fixture(&f);
  puts("reservation failure remains private; post-store promotion allocates nothing");
}

int main(int argc, char **argv)
{
  const char *mode = argc > 1 ? argv[1] : "all";
  int huge, wide, exact;
  alarm(60);
  if (!strcmp(mode, "existing")) {
    char *args[] = { argv[0], "all", NULL };
    return original_coalescing_main(2, args);
  }
  if (!strcmp(mode, "all") || !strcmp(mode, "pause")) {
    for (huge=0; huge<2; huge++) for (wide=0; wide<2; wide++)
      for (exact=0; exact<2; exact++) test_old_scanner(huge, wide, exact);
    for (huge=0; huge<2; huge++) {
      test_mode_pause(huge, LJ_GC2_TABLE_COALESCE_TEST_PRE_MODE);
      test_mode_pause(huge, LJ_GC2_TABLE_COALESCE_TEST_POST_MODE);
    }
  }
  if (!strcmp(mode, "all") || !strcmp(mode, "collect")) {
    test_continued_collection(0); test_continued_collection(1);
  }
  if (!strcmp(mode, "all") || !strcmp(mode, "oom")) test_reserved_failure();
  puts("dense-W prototype controls passed");
  return 0;
}

