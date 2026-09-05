/* Public-call scheduling controls. Counter reads and native-entry wrapping
** observe real runtime behavior; no GC/test hook installs a phase or request.
*/
#define main retention_main
#include "t-string-retention.c"
#undef main

static TGState *observed_tg;
static uint32_t native_entries;
extern void __real_lj_native_enter(TGState *tg);
void __wrap_lj_native_enter(TGState *tg)
{
  __real_lj_native_enter(tg);
  if (tg == observed_tg) {
    assert(lj_tg_in_native_acq(tg) != 0);
    native_entries++;
  }
}

static void named(lua_State *L, const char *name)
{
  lua_getglobal(L, name);
  assert(lua_isfunction(L, -1));
  if (lua_pcall(L, 0, 0, 0)) {
    fprintf(stderr, "%s failed: %s\n", name, lua_tostring(L, -1));
    abort();
  }
}

static uint32_t publish_by_allocating(lua_State *L)
{
  global_State *g = G(L);
  uint64_t starts = gc2_cycle_starts_acq(g);
  uint32_t i;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(gc2_cycle_leader_acq(g) == 0);
  for (i = 0; i < 8192; i++) {
    char buf[PAYLOAD + 1];
    make_bytes(buf, 0, 71, i);
    lua_pushlstring(L, buf, PAYLOAD);
    lua_pop(L, 1);
    if (gc2_cycle_leader_acq(g) != 0) break;
  }
  assert(i < 8192);
  assert(gc2_cycle_starts_acq(g) == starts);
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  assert(lj_tg_local_total_acq(L2TG(L)) < LJ_GC2_ACCT_FLUSH);
  assert(lj_gc2_alloc_since_load(g) <= lj_gc2_hard_load(g));
  return i + 1;
}

int main(int argc, char **argv)
{
  static const char *const boundaries[] = {
    "control_tnew", "control_tdup", "control_fastfunc", "control_cfunc",
    NULL, "control_fnew"
  };
  lua_State *L;
  global_State *g;
  Snapshot base, published, observed, cleanup;
  uint64_t starts, completed, requests, filler = 0;
  uint32_t leader, nstrings, native0;
  int start, ping, stop, workers, automatic, live, mode, boundary, result = 0;
  unsigned i;
  assert(argc == 7);
  mode = atoi(argv[1]);  /* 0 boundary, 1 stop/restart, 2 last/first attach, 3 native. */
  boundary = atoi(argv[2]);
  with_peer = atoi(argv[3]);
  nworkers = atoi(argv[4]);
  assert(mode >= 0 && mode <= 3 && boundary >= 0 && boundary < 6);
  assert(mode != 2 || with_peer == 1);
  setvbuf(stdout, NULL, _IOLBF, 0);
  alarm(45);
  L = luaL_newstate();
  assert(L);
  g = G(L);
  observed_tg = L2TG(L);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF));
  lua_pushcfunction(L, check_live_lua);
  lua_setglobal(L, "baseline_check_live");
  lua_pushcfunction(L, automatic_done);
  lua_setglobal(L, "baseline_auto_done");
  lua_createtable(L, LIVE, 0);
  for (i = 0; i < LIVE; i++) {
    char buf[PAYLOAD + 1];
    make_bytes(buf, 1, 0, i);
    lua_pushlstring(L, buf, PAYLOAD);
    live_identity[i] = lua_tolstring(L, -1, NULL);
    lua_rawseti(L, -2, (int)i + 1);
  }
  lua_pushvalue(L, -1);
  live = luaL_ref(L, LUA_REGISTRYINDEX);
  lua_setglobal(L, "baseline_live");
  lua_pushnumber(L, 12345.125);
  assert(lua_tolstring(L, -1, NULL) != NULL);
  lua_setglobal(L, "control_number_string");
  if (luaL_loadfile(L, argv[6]) || lua_pcall(L, 0, 0, 0)) abort();
  if (luaL_loadfile(L, argv[5]) || lua_pcall(L, 0, 5, 0)) abort();
  automatic = luaL_ref(L, LUA_REGISTRYINDEX);
  workers = luaL_ref(L, LUA_REGISTRYINDEX);
  stop = luaL_ref(L, LUA_REGISTRYINDEX);
  ping = luaL_ref(L, LUA_REGISTRYINDEX);
  start = luaL_ref(L, LUA_REGISTRYINDEX);
  full_cycles(L);
  call(L, start, with_peer, 1);
  call(L, workers, nworkers, 1);
  full_cycles(L);
  base = snapshot(L, "baseline", 0, 0);
  assert(base.phase == LJ_GC2_IDLE && base.peers == (uint32_t)with_peer &&
         base.workers == (uint32_t)nworkers);
  nstrings = publish_by_allocating(L);
  published = snapshot(L, "published", 0, 0);
  starts = published.cycles;
  completed = published.completed;
  requests = gc2_cycle_requests_acq(g);
  leader = gc2_cycle_leader_acq(g);
  if (with_peer) assert(lj_gc_threshold_load(g) == LJ_MAX_MEM);
  assert(lua_gc(L, LUA_GCISRUNNING, 0));
  if (mode == 1 || mode == 2) {
    assert(lua_gc(L, LUA_GCSTOP, 0) == 0);
    if (mode == 2) {
      call(L, stop, 0, 0);
      la_store32_rel(&peer_credit, 0);
      assert(mt_live_acq(g) == 0 && lj_gc_threshold_load(g) == LJ_MAX_MEM);
      snapshot(L, "last_detach_stopped", 0, 0);
      call(L, start, 1, 1);
      assert(mt_live_acq(g) == 1 && lj_gc_threshold_load(g) == LJ_MAX_MEM);
      assert(lj_gc_mt_threshold_load(g) == LJ_MAX_MEM);
      snapshot(L, "first_attach_stopped", 0, 0);
    }
    named(L, "control_stopped_alloc");
    named(L, "control_fastfunc");
    named(L, "control_cfunc");
    assert(!lua_gc(L, LUA_GCISRUNNING, 0));
    assert(gc2_cycle_starts_acq(g) == starts && gc2_sweep_to_idle_acq(g) == completed);
    assert(gc2_cycle_requests_acq(g) == requests && gc2_cycle_leader_acq(g) == leader);
    assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
    snapshot(L, "stopped_request_preserved", 0, 256);
    assert(lua_gc(L, LUA_GCRESTART, 0) == 0);
    assert(lua_gc(L, LUA_GCISRUNNING, 0));
  }
  if (mode == 3) {
    native0 = native_entries;
    named(L, "control_native");
    assert(native_entries > native0 && lj_tg_in_native_acq(L2TG(L)) == 0);
    assert(gc2_cycle_starts_acq(g) == starts && gc2_cycle_leader_acq(g) == leader);
    snapshot(L, "native_return_request_pending", 0, 0);
  }
  if (boundaries[boundary]) named(L, boundaries[boundary]);
  else {
    lua_pushnumber(L, 12345.125);
    assert(strcmp(lua_tolstring(L, -1, NULL), "12345.125") == 0);
    lua_pop(L, 1);
  }
  observed = snapshot(L, "after_one_boundary", 0, 0);
  assert(observed.peers == (uint32_t)with_peer && observed.workers == (uint32_t)nworkers);
  if (observed.cycles <= starts) result = 3;  /* Preserve missed admission. */
  else {
    filler = automatic_cycles(L, automatic);
    snapshot(L, automatic_completed ? "automatic_completed" : "automatic_progress_missing", 0, filler);
    if (!automatic_completed) result = 2;
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, live);
  check_live(L, lua_gettop(L));
  lua_pop(L, 1);
  call(L, ping, 1, 1);
  call(L, workers, 0, 1);
  call(L, stop, 0, 0);
  la_store32_rel(&peer_credit, 0);
  assert(mt_live_acq(g) == 0 && gc2_n_workers_acq(g) == 0);
  full_cycles(L);
  cleanup = snapshot(L, "sole_main_recovered", 0, filler);
  assert(cleanup.strings <= base.strings && cleanup.reclaimed >= base.reclaimed + nstrings);
  assert(lj_str_reclaim_exclusive_acq(g) == 0 && lj_str_sweep_batch_acq(g) == NULL);
  printf("CONTROL result=%d mode=%d boundary=%d strings=%u native_entries=%u\n",
         result, mode, boundary, nstrings, native_entries);
  lua_close(L);
  return result;
}
