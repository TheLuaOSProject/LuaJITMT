/* Read-only counters around ordinary public string, thread and GC calls.
** No collector/test hooks, forced phases, or reclamation admission changes.
*/
#include <assert.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_str.h"
#include "lj_tg.h"

enum { ROUNDS = 6, BATCH = 4096, LIVE = 32, PAYLOAD = 64 };
static const char *live_identity[LIVE];
static uint32_t peer_credit, peer_checks;
static uint64_t automatic_target;
static int with_peer, nworkers, explicit_collect, engine_on, automatic_completed;

typedef struct Snapshot {
  uint64_t bytes, cycles, major, completed, unlinked, reclaimed, strings;
  uint64_t worker_async, worker_runs;
  uint32_t phase, raw_count, local_credit, remote_credit, mask, peers, workers;
} Snapshot;

static void make_bytes(char *buf, int live, unsigned round, unsigned slot)
{
  int n = snprintf(buf, PAYLOAD + 1, "lj-retention-%c:%08x:%08x:",
                   live ? 'L' : 'G', round, slot);
  assert(n > 0 && n < PAYLOAD);
  memset(buf + n, 'a' + (slot % 26), PAYLOAD - (unsigned)n);
  buf[PAYLOAD] = 0;
}

static void check_live(lua_State *L, int index)
{
  unsigned i;
  assert(lua_istable(L, index));
  for (i = 0; i < LIVE; i++) {
    char buf[PAYLOAD + 1];
    size_t size;
    const char *p;
    make_bytes(buf, 1, 0, i);
    lua_rawgeti(L, index, (int)i + 1);
    p = lua_tolstring(L, -1, &size);
    assert(p == live_identity[i] && size == PAYLOAD);
    assert(memcmp(p, buf, PAYLOAD) == 0);
    lua_pushlstring(L, buf, PAYLOAD);
    assert(lua_tolstring(L, -1, NULL) == p);
    assert(lua_rawequal(L, -1, -2));
    lua_pop(L, 2);
  }
}

static int check_live_lua(lua_State *L)
{
  check_live(L, 1);
  if (L2TG(L) != G(L)->main_tg) {
    /* The peer executes only existing-string lookups and numeric channel
    ** traffic after this publication. No new string intern follows it until
    ** the next check, which again interns only the rooted canonical strings.
    ** Worker TGs never intern: they have no Lua stack and run GC work only.
    ** Thus this credit stays constant between main-owner measurements.
    */
    la_store32_rel(&peer_credit, L2TG(L)->strnum_credit);
    (void)la_add32_acqrel(&peer_checks, 1);
  }
  return 0;
}

static void call(lua_State *L, int fn, int argument, int nargs)
{
  lua_rawgeti(L, LUA_REGISTRYINDEX, fn);
  if (nargs) lua_pushinteger(L, argument);
  if (lua_pcall(L, nargs, 0, 0)) {
    fprintf(stderr, "Lua call failed: %s\n", lua_tostring(L, -1));
    abort();
  }
}

static Snapshot snapshot(lua_State *L, const char *stage, unsigned round,
                         uint64_t filler)
{
  global_State *g = G(L);
  Snapshot s;
  s.bytes = lj_gc_total_load(g);
  s.cycles = gc2_cycle_starts_acq(g);
  s.major = gc2_major_cycle_starts_acq(g);
  s.completed = gc2_sweep_to_idle_acq(g);
  s.phase = gc2_phase_acq(g);
  s.unlinked = la_load64_acq(&g->str.sweep_unlinked);
  s.reclaimed = la_load64_acq(&g->str.sweep_reclaimed);
  s.raw_count = lj_str_num_acq(g);
  s.local_credit = L2TG(L)->strnum_credit;
  s.remote_credit = la_load32_acq(&peer_credit);
  assert(s.raw_count >= s.local_credit + s.remote_credit);
  s.strings = s.raw_count - s.local_credit - s.remote_credit;
  s.mask = lj_str_mask_acq(g);
  s.peers = mt_live_acq(g);
  s.workers = gc2_n_workers_acq(g);
  s.worker_async = gc2_worker_async_progress_acq(g);
  s.worker_runs = gc2_worker_runs_acq(g);
  printf("{\"stage\":\"%s\",\"round\":%u,\"explicit\":%d,"
         "\"peer\":%d,\"configured_workers\":%d,\"strings\":%" PRIu64
         ",\"raw_count\":%u,\"local_credit\":%u,\"peer_credit\":%u,"
         "\"bytes\":%" PRIu64 ",\"mask\":%u,\"phase\":%u,"
         "\"cycles\":%" PRIu64 ",\"major\":%" PRIu64
         ",\"completed\":%" PRIu64 ",\"unlinked\":%" PRIu64
         ",\"reclaimed\":%" PRIu64 ",\"mt_live\":%u,\"n_workers\":%u,"
         "\"worker_async\":%" PRIu64 ",\"worker_runs\":%" PRIu64
         ",\"peer_checks\":%u,\"filler_tables\":%" PRIu64 "}\n",
         stage, round, explicit_collect, with_peer, nworkers, s.strings,
         s.raw_count, s.local_credit, s.remote_credit, s.bytes, s.mask,
         s.phase, s.cycles, s.major, s.completed, s.unlinked, s.reclaimed,
         s.peers, s.workers, s.worker_async, s.worker_runs,
         la_load32_acq(&peer_checks), filler);
  printf("{\"diagnostic\":\"%s\",\"round\":%u,\"threshold\":%" PRIu64
         ",\"mt_threshold\":%" PRIu64 ",\"alloc_since\":%" PRIu64
         ",\"trigger\":%" PRIu64 ",\"hard\":%" PRIu64
         ",\"hard_check\":%" PRIu64 ",\"local_total\":%" PRIu64
         ",\"cycle_leader\":%u,\"worker_active\":%u,\"worker_busy\":%" PRIu64
         ",\"jit_gate\":%u,\"mark_resume\":%u,\"mark_close_intent\":%u"
         ",\"interp_hard\":%" PRIu64 ",\"assist_runs\":%" PRIu64
         ",\"cycle_requests\":%" PRIu64 ",\"major_roots\":%" PRIu64
         ",\"worker_started\":%u,\"worker_exited\":%u,\"worker_stop\":%u}\n",
         stage, round, (uint64_t)lj_gc_threshold_load(g),
         (uint64_t)lj_gc_mt_threshold_load(g), lj_gc2_alloc_since_load(g),
         lj_gc2_trigger_load(g), lj_gc2_hard_load(g), lj_gc2_hard_check_load(g),
         lj_tg_local_total_acq(L2TG(L)), gc2_cycle_leader_acq(g),
         gc2_worker_active_acq(g), gc2_worker_busy_retries_acq(g),
         gc2_jit_phase_gate_acq(g), gc2_jit_mark_resume_acq(g),
         gc2_mark_close_intent_acq(g), gc2_interp_hard_checks_acq(g),
         gc2_assist_runs_acq(g), gc2_cycle_requests_acq(g),
         gc2_major_root_scans_acq(g), gc2_worker_started_acq(g),
         gc2_worker_exited_acq(g), gc2_worker_stop_acq(g));
  printf("{\"execution\":\"%s\",\"round\":%u,\"jit_enabled\":%d,"
         "\"jit_hard_checks\":%" PRIu64 "}\n", stage, round, engine_on,
         gc2_jit_hard_checks_acq(g));
  return s;
}

static void full_cycles(lua_State *L)
{
  global_State *g = G(L);
  uint64_t target = gc2_sweep_to_idle_acq(g) + 2;
  unsigned i;
  for (i = 0; i < 16; i++) {
    (void)lua_gc(L, LUA_GCCOLLECT, 0);
    if (gc2_sweep_to_idle_acq(g) >= target &&
        gc2_phase_acq(g) == LJ_GC2_IDLE) return;
  }
  assert(0 && "explicit calls did not complete two real GC cycles");
}

static int automatic_done(lua_State *L)
{
  global_State *g = G(L);
  lua_pushboolean(L, gc2_sweep_to_idle_acq(g) >= automatic_target &&
                     gc2_phase_acq(g) == LJ_GC2_IDLE);
  return 1;
}

static uint64_t automatic_cycles(lua_State *L, int fn)
{
  global_State *g = G(L);
  uint64_t n;
  automatic_target = gc2_sweep_to_idle_acq(g) + 3;
  assert(lua_gc(L, LUA_GCISRUNNING, 0));
  assert(lj_str_reclaim_requested_acq(g) == 0);
  lua_rawgeti(L, LUA_REGISTRYINDEX, fn);
  if (lua_pcall(L, 0, 2, 0)) {
    fprintf(stderr, "Automatic allocation failed: %s\n", lua_tostring(L, -1));
    (void)snapshot(L, "automatic_failed", 0, 262144);
    abort();
  }
  n = (uint64_t)lua_tointeger(L, -2);
  automatic_completed = lua_toboolean(L, -1);
  lua_pop(L, 2);
  assert(n && n <= 262144);
  assert(!automatic_completed || gc2_sweep_to_idle_acq(g) >= automatic_target);
  return n;
}

int main(int argc, char **argv)
{
  lua_State *L;
  global_State *g;
  Snapshot base, peak, settled, recovered;
  int start, ping, stop, workers, automatic, live;
  unsigned i, round, rounds_done = 0;
  uint64_t filler_total = 0;
  assert(argc == 5);
  explicit_collect = atoi(argv[1]);
  with_peer = atoi(argv[2]);
  nworkers = atoi(argv[3]);
  engine_on = getenv("RETENTION_JIT") && strcmp(getenv("RETENTION_JIT"), "1") == 0;
  assert((explicit_collect == 0 || explicit_collect == 1) &&
         (with_peer == 0 || with_peer == 1) &&
         (nworkers == 0 || nworkers == 2));
  setvbuf(stdout, NULL, _IOLBF, 0);
  alarm(45);
  L = luaL_newstate();
  assert(L);
  g = G(L);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE |
                          (engine_on ? LUAJIT_MODE_ON : LUAJIT_MODE_OFF)));
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
  if (luaL_loadfile(L, argv[4]) || lua_pcall(L, 0, 5, 0)) {
    fprintf(stderr, "Lua setup failed: %s\n", lua_tostring(L, -1));
    abort();
  }
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
         base.workers == (uint32_t)nworkers && lj_str_qcount_acq(g) == 0);
  for (round = 1; round <= ROUNDS; round++) {
    for (i = 0; i < BATCH; i++) {
      char buf[PAYLOAD + 1];
      make_bytes(buf, 0, round, i);
      lua_pushlstring(L, buf, PAYLOAD);
      lua_pop(L, 1);  /* No Lua edge retains any churn string. */
    }
    peak = snapshot(L, "after_churn", round, filler_total);
    if (explicit_collect) full_cycles(L);
    else filler_total += automatic_cycles(L, automatic);
    lua_rawgeti(L, LUA_REGISTRYINDEX, live);
    check_live(L, lua_gettop(L));
    lua_pop(L, 1);
    call(L, ping, (int)round, 1);
    settled = snapshot(L, (!explicit_collect && !automatic_completed) ?
                         "automatic_progress_missing" : "settled", round, filler_total);
    rounds_done = round;
    assert(settled.peers == (uint32_t)with_peer &&
           settled.workers == (uint32_t)nworkers);
    if (!explicit_collect && !automatic_completed) {
      assert(settled.strings == base.strings + (uint64_t)round * BATCH);
      assert(settled.reclaimed == base.reclaimed && settled.unlinked == base.unlinked);
      break;  /* Preserve this failed progress gate; no explicit substitute. */
    }
    assert(settled.phase == LJ_GC2_IDLE);
    assert(settled.completed >= peak.completed + (explicit_collect ? 2 : 3));
    if (explicit_collect && !with_peer && !nworkers) {
      assert(peak.strings == base.strings + BATCH);
      assert(settled.strings == base.strings);
      assert(settled.reclaimed - base.reclaimed == (uint64_t)round * BATCH);
      assert(settled.unlinked == settled.reclaimed);
    } else {
      assert(settled.strings == base.strings + (uint64_t)round * BATCH);
      assert(settled.reclaimed == base.reclaimed);
      assert(settled.unlinked == base.unlinked);
    }
  }
  call(L, workers, 0, 1);
  call(L, stop, 0, 0);
  la_store32_rel(&peer_credit, 0);  /* Detach already returned unused credits. */
  assert(mt_live_acq(g) == 0 && gc2_n_workers_acq(g) == 0);
  full_cycles(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, live);
  check_live(L, lua_gettop(L));
  lua_pop(L, 1);
  recovered = snapshot(L, "sole_main_recovered", rounds_done, filler_total);
  assert(recovered.strings <= base.strings);
  assert(recovered.reclaimed - base.reclaimed >= (uint64_t)rounds_done * BATCH);
  assert(recovered.unlinked == recovered.reclaimed);
  assert(lj_str_reclaim_exclusive_acq(g) == 0 &&
         lj_str_sweep_batch_acq(g) == NULL &&
         lj_str_retired_batch_head_acq(g) == NULL);
  if (with_peer) assert(la_load32_acq(&peer_checks) == rounds_done + 2);
  printf("%s rounds=%u batch=%d payload=%d body_bytes=%zu live=%d "
         "created_body_bytes=%zu filler_tables=%" PRIu64 "\n",
         (!explicit_collect && !automatic_completed) ? "INCOMPLETE_AUTO" : "PASS",
         rounds_done, BATCH, PAYLOAD, (size_t)lj_str_size(PAYLOAD), LIVE,
         (size_t)rounds_done * BATCH * lj_str_size(PAYLOAD), filler_total);
  lua_close(L);
  return (!explicit_collect && !automatic_completed) ? 2 : 0;
}
