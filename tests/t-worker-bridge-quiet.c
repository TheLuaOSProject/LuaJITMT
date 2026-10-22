#define main stop_fixture_main
#include "t-worker-bridge-stop.c"
#undef main

static uint32_t prune_armed, prune_paused, prune_release;
static uint32_t prune_calls, cursor_only, eof_advances;
static uint64_t unlinked_total;

extern uint32_t __real_lj_gc_sweep_gc2_unmarked(global_State *);
uint32_t __wrap_lj_gc_sweep_gc2_unmarked(global_State *g)
{
  TGState *self = lj_thr_get_tg();
  GCRef *before, *after;
  uint32_t done, n, cycle, expect = 1;
  if (g != probe_g || !self || self == g->main_tg)
    return __real_lj_gc_sweep_gc2_unmarked(g);
  assert(gc2_worker_active_acq(g) == 1);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  if (la_cas32(&prune_armed, &expect, 0, LA_ACQ_REL, LA_ACQ)) {
    assert(gc2_sweep_root_scanned_acq(g) == 1);
    assert(lj_gc2_sweep_needs_prepare(g) == 0);
    la_store32_rel(&prune_paused, 1);
    wait_value(&prune_release, 1);
  }
  cycle = gc2_cycle_acq(g);
  done = gc2_sweep_root_done_acq(g);
  before = gc2_sweep_root_cursor_acq(g);
  if (!before) before = lj_gc_root_ref(g);
  n = __real_lj_gc_sweep_gc2_unmarked(g);
  after = gc2_sweep_root_cursor_acq(g);
  if (!after) after = lj_gc_root_ref(g);
  assert(gc2_worker_active_acq(g) == 1);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP && gc2_cycle_acq(g) == cycle);
  (void)la_add32_rlx(&prune_calls, 1);
  (void)la_add64_rlx(&unlinked_total, n);
  if (n == 0 && before != after)
    (void)la_add32_rlx(&cursor_only, 1);
  if (!done && gc2_sweep_root_done_acq(g))
    (void)la_add32_rlx(&eof_advances, 1);
  return n;
}

int main(int argc, char **argv)
{
  lua_State *L;
  global_State *g;
  TGState *tg;
  GCRef *cursor;
  GCobj *pending;
  uint32_t actions = 0, workers, cycle;
  uint64_t start, elapsed, parks0, parks, progress0, completed;
  unsigned i;
  assert(argc == 2);
  workers = (uint32_t)atoi(argv[1]);
  assert(workers == 1 || workers == 2);
  setvbuf(stdout, NULL, _IOLBF, 0);
  alarm(30);
  L = luaL_newstate(); assert(L);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  lua_createtable(L, 32, 0);  /* A fixed 32-table live ring. */
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L); tg = L2TG(L);
  enter_real_sweep(L);
  probe_g = g;
  completed = gc2_sweep_to_idle_acq(g);
  cycle = gc2_cycle_acq(g);
  la_store32_rel(&prune_armed, 1);
  assert(lj_gc2_workers_set_l(L, workers, &actions) == 1);
  start = lj_thr_now_ns();
  while (la_load32_acq(&prune_paused) == 0) {
    assert(lj_thr_now_ns() - start < UINT64_C(10000000000));
    actions |= lj_safepoint_ack(L);
    la_cpu_pause();
  }
  assert((actions & LJ_GC2_HS_STOPREQ) == 0);
  /* The real reset is complete and a real bridge owner is paused. Publish a
  ** fixed number of new-generation tables, then do no more Lua allocation or
  ** mutator collection until the same cycle completes in background workers. */
  for (i = 1; i <= 1024; i++) {
    lua_createtable(L, 1, 0);
    lua_pushinteger(L, i);
    lua_rawseti(L, -2, 1);
    lua_rawseti(L, 1, (int)((i - 1) % 32 + 1));
  }
  lj_native_enter(tg);
  cursor = gc2_sweep_root_cursor_acq(g);
  pending = lj_tg_gcroot_pending_acq(tg);
  assert(pending != NULL);
  progress0 = gc2_worker_async_progress_acq(g);
  parks0 = gc2_worker_parks_acq(g);
  start = lj_thr_now_ns();
  usleep(20000);
  elapsed = lj_thr_now_ns() - start;
  parks = gc2_worker_parks_acq(g) - parks0;
  assert(gc2_worker_active_acq(g) == 1);
  assert(gc2_sweep_root_cursor_acq(g) == cursor);
  assert(lj_tg_gcroot_pending_acq(tg) == pending);
  assert(gc2_worker_async_progress_acq(g) == progress0);
  assert(gc2_sweep_to_idle_acq(g) == completed);
  assert(parks <= elapsed / UINT64_C(1000000) + 8);
  if (workers == 2) assert(parks != 0);
  printf("{\"stage\":\"unchanged_claim_frontier\",\"workers\":%u,"
         "\"cycle\":%u,\"cursor\":\"%p\",\"pending\":\"%p\","
         "\"ns\":%" PRIu64 ",\"parks\":%" PRIu64 ",\"fake_progress\":0}\n",
         workers, cycle, (void *)cursor, (void *)pending, elapsed, parks);
  la_store32_rel(&prune_release, 1);
  start = lj_thr_now_ns();
  while (gc2_sweep_to_idle_acq(g) == completed) {
    assert(lj_thr_now_ns() - start < UINT64_C(10000000000));
    usleep(1000);
  }
  assert(gc2_sweep_to_idle_acq(g) == completed + 1);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE && gc2_cycle_acq(g) == cycle);
  assert(la_load32_acq(&prune_calls) >= 4);
  assert(la_load32_acq(&cursor_only) != 0);
  assert(la_load32_acq(&eof_advances) == 1);
  printf("{\"stage\":\"quiet_native_completed\",\"workers\":%u,"
         "\"cycle\":%u,\"completed_delta\":1,\"ns\":%" PRIu64 ","
         "\"prune_calls\":%u,\"cursor_only\":%u,\"unlinked\":%" PRIu64 "}\n",
         workers, cycle, lj_thr_now_ns() - start,
         la_load32_acq(&prune_calls), la_load32_acq(&cursor_only),
         la_load64_acq(&unlinked_total));
  actions = lj_native_leave(L);
  assert((actions & LJ_GC2_HS_STOPREQ) == 0);
  assert(lj_gc2_workers_set_l(L, 0, &actions) == 1);
  for (i = 1; i <= 32; i++) {
    lua_rawgeti(L, 1, (int)i);
    assert(lua_istable(L, -1));
    lua_rawgeti(L, -1, 1);
    assert(lua_tointeger(L, -1) == (lua_Integer)(992 + i));
    lua_pop(L, 2);
  }
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_close(L);
  puts("PASS real bridge frontier backoff and quiet native completion");
  return 0;
}
