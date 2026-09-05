/* Isolated study fixture; no production registration. */
#define main original_coalescing_main
#include "coalescing-adapter.c"
#undef main
#include "lualib.h"

static uint64_t era_of(LJGC2TabStamp *s)
{
#ifdef LJ_GC2_WIDE_STAMP
  return lj_arena_gc2_stamp_snapshot(s).hi;
#else
  (void)s;
  return 0;
#endif
}

static void force_stamp(LJGC2TabStamp *s, uint64_t era, uint32_t serial)
{
#ifdef LJ_GC2_WIDE_STAMP
  la_u128 old = lj_arena_gc2_stamp_snapshot(s);
  la_u128 desired = { serial, era };
  assert(la_cas128(&s->proof, &old, desired));
#else
  (void)era;
  la_store64_rel(&s->state, serial);
#endif
}

static void test_old_scanner_renewal(int huge)
{
  Fixture f = open_fixture(huge, 1);
  pthread_t scanner;
  force_stamp(f.stamp, 17, 1);
  assert(lj_gc2_test_ssb_push(f.g, obj2gco(f.parent)) == 1);
  assert(lj_gc2_flush_ssb(f.g, f.tg) == 1);
  watch(&f, LJ_GC2_TABLE_COALESCE_TEST_PRE_PROOF);
  assert(pthread_create(&scanner, NULL, scan_to_proof, &f) == 0);
  wait_value(&paused, LJ_GC2_TABLE_COALESCE_TEST_PRE_PROOF);
  /* Compress the intervening low-word namespace, with the actual scanner
  ** stopped before proof. The following public barrier performs real rollover. */
  force_stamp(f.stamp, 17, UINT32_MAX);
  store_child(&f);
  publish(&f, 0);
  assert(!lj_gc2_activation_reclaim_veto(f.g));
  assert(era_of(f.stamp) == 18);
  assert((uint32_t)la_load64_acq(&f.stamp->state) == 1u);
  la_store32_rel(&released, 1);
  assert(pthread_join(scanner, NULL) == 0);
  drain(&f);
  assert_graph(&f);
  assert(scans >= 2u);
  close_fixture(&f);
}

static void lua_ok(lua_State *L, const char *source)
{
  if (luaL_dostring(L, source) != 0) {
    fprintf(stderr, "Lua failure: %s\n", lua_tostring(L, -1));
    abort();
  }
}

static void test_continued_collection(int huge)
{
  Fixture f;
  uint32_t round;
  uint64_t previous_era = 0;
  memset(&f, 0, sizeof(f));
  f.L = luaL_newstate(); assert(f.L);
  luaL_openlibs(f.L);
  lua_gc(f.L, LUA_GCSTOP, 0);
  f.g = G(f.L); f.tg = G2TG(f.g);
  lua_newtable(f.L);
  f.parent = huge ? huge_table(&f) : tabV(f.L->top - 1);
  if (huge) settabV(f.L, f.L->top - 1, f.parent);
  lua_setglobal(f.L, "p");
  lua_ok(f.L, "weak = setmetatable({}, {__mode='v'})");
  f.stamp = lj_arena_gc2_stamp_acq(f.parent); assert(f.stamp);
  for (round = 0; round < 12u; round++) {
    lua_ok(f.L, "p.keep = {n=2718, child={value=314}}; weak[1] = {}");
    previous_era = era_of(f.stamp);
    force_stamp(f.stamp, previous_era, UINT32_MAX - 1u);
    lj_gc2_test_table_dirty_bump(f.g, f.parent);
    lj_gc2_test_table_dirty_bump(f.g, f.parent);
    assert(!lj_gc2_activation_reclaim_veto(f.g));
    assert(era_of(f.stamp) == previous_era + 1u);
    assert(lua_gc(f.L, LUA_GCCOLLECT, 0) == 0);
    assert(gc2_phase_acq(f.g) == LJ_GC2_IDLE);
    assert(gc2_recovery_items_acq(f.g) == 0);
    assert(!lj_gc2_activation_reclaim_veto(f.g));
    lua_ok(f.L, "assert(weak[1] == nil); assert(p.keep.n == 2718 and p.keep.child.value == 314)");
  }
  assert(previous_era >= 11u);
  lua_ok(f.L, "p = nil; weak = nil");
  lua_settop(f.L, 0);
  close_fixture(&f);
}

int wide_main(int argc, char **argv)
{
  const char *mode = argc > 1 ? argv[1] : "all";
  alarm(40);
  if (!strcmp(mode, "existing")) {
    char *args[] = { argv[0], "all", NULL };
    return original_coalescing_main(2, args);
  }
  if (!strcmp(mode, "all") || !strcmp(mode, "pause")) {
    test_old_scanner_renewal(0);
    test_old_scanner_renewal(1);
  }
  if (!strcmp(mode, "all") || !strcmp(mode, "collect")) {
    test_continued_collection(0);
    test_continued_collection(1);
  }
  puts("wide-stamp prototype: old-era proof rejected; small/huge collection continues after rollover");
  return 0;
}

int main(int argc, char **argv)
{
 int huge; assert(argc == 3); huge = atoi(argv[2]); assert(huge == 0 || huge == 1); alarm(40);
 if (!strcmp(argv[1], "pause")) test_old_scanner_renewal(huge);
 else { assert(!strcmp(argv[1], "collect")); test_continued_collection(huge); }
 puts("wide-stamp focused control OK"); return 0;
}
