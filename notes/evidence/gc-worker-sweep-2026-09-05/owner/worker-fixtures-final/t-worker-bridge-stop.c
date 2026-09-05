#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
#include "lj_thr.h"

/* Only pause an actual worker's existing handshake entry. All GC phases,
** requests, owner/native words and allocator/root actions are production calls.
** The main actor then releases the worker and waits for its real counted
** request before invoking the ordinary pool stop/join API. */
static global_State *probe_g;
static uint32_t wanted, armed, paused, release_worker, worker_tid;
static uint64_t handshake_epoch;

static void wait_value(const uint32_t *p, uint32_t value)
{
  uint64_t start = lj_thr_now_ns();
  while (la_load32_acq(p) != value) {
    assert(lj_thr_now_ns() - start < UINT64_C(10000000000));
    la_cpu_pause();
  }
}

extern uint32_t __real_lj_safepoint_handshake(global_State *, uint32_t);
uint32_t __wrap_lj_safepoint_handshake(global_State *g, uint32_t actions)
{
  TGState *self = lj_thr_get_tg();
  uint32_t expect = 1;
  if (g == probe_g && (actions & wanted) != 0 && self &&
      self != g->main_tg && lj_tg_load_cur_L(self) == NULL &&
      la_cas32(&armed, &expect, 0, LA_ACQ_REL, LA_ACQ)) {
    assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
    assert(gc2_worker_active_acq(g) == 1);
    assert(lj_tg_in_native_acq(self) == 1);
    assert(lj_tg_load_thread_L(self) == NULL);
    worker_tid = lj_tg_tid_acq(self);
    la_store32_rel(&paused, 1);
    wait_value(&release_worker, 1);
  }
  return __real_lj_safepoint_handshake(g, actions);
}

static void enter_real_sweep(lua_State *L)
{
  global_State *g = G(L);
  unsigned i;
  assert(gc2_phase_acq(g) == LJ_GC2_IDLE);
  lj_gc2_force_major(g);
  lj_gc2_mark_begin(g);  /* Existing explicit start, without synthetic phase. */
  assert(gc2_phase_acq(g) == LJ_GC2_MARK);
  for (i = 0; i < 10000 && gc2_phase_acq(g) == LJ_GC2_MARK; i++) {
    (void)lj_gc2_worker_drain(g, 64);
    if (lj_gc2_mark_complete(g, L, 1, 64))
      lj_gc2_mark_to_weak(g);
  }
  assert(gc2_phase_acq(g) == LJ_GC2_WEAK);
  for (i = 0; i < 10000 && gc2_phase_acq(g) == LJ_GC2_WEAK; i++) {
    if (lj_gc2_weak_complete(g, L, NULL, 64))
      lj_gc2_weak_to_sweep(g, L);
  }
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP);
  assert(gc2_sweep_bridge_ready_acq(g) == 0);
  assert(gc2_sweep_root_scanned_acq(g) == 0);
  assert(gc2_sweep_root_done_acq(g) == 0);
  assert(gc2_worker_active_acq(g) == 0);
}

int main(int argc, char **argv)
{
  lua_State *L;
  global_State *g;
  TGState *tg;
  const char *identity;
  LJStateOwner owner;
  uint32_t actions = 0, workers, api, cycle;
  uint64_t start, elapsed;
  int ok;
  assert(argc == 4);
  wanted = atoi(argv[1]) ? LJ_GC2_HS_SCAN_ROOTS : LJ_GC2_HS_RESET_ALLOC;
  workers = (uint32_t)atoi(argv[2]);
  api = (uint32_t)atoi(argv[3]);
  assert((workers == 1 || workers == 2) && api <= 1);
  setvbuf(stdout, NULL, _IOLBF, 0);
  alarm(30);
  L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  lua_pushliteral(L, "worker bridge stop root remains canonical");
  identity = lua_tolstring(L, -1, NULL);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L); tg = L2TG(L); owner = lj_state_owner_word_acq(L);
  assert(tg == lj_thr_get_tg() && lj_tg_in_native_acq(tg) == 0);
  enter_real_sweep(L);
  cycle = gc2_cycle_acq(g);
  probe_g = g;
  la_store32_rel(&armed, 1);
  assert(lj_gc2_workers_set_l(L, workers, &actions) == 1);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert((actions & LJ_GC2_HS_STOPREQ) == 0);
  printf("{\"stage\":\"startup_return\",\"actions\":%u}\n", actions);
  actions = 0;  /* Keep subsequent shutdown actions a separate observation. */
  /* SCAN_ROOTS can be preceded by RESET_ALLOC after startup returned. Keep
  ** servicing those ordinary owner polls until the worker reaches the paused
  ** target entry. The target cannot publish before release_worker below. */
  start = lj_thr_now_ns();
  while (la_load32_acq(&paused) == 0) {
    assert(lj_thr_now_ns() - start < UINT64_C(10000000000));
    actions |= lj_safepoint_ack(L);
    la_cpu_pause();
  }
  printf("{\"stage\":\"before_target_release\",\"actions\":%u}\n", actions);
  assert((actions & LJ_GC2_HS_STOPREQ) == 0);
  actions = 0;
  la_store32_rel(&release_worker, 1);
  start = lj_thr_now_ns();
  while ((lj_tg_reqmask_acq(tg) & wanted) == 0) {
    assert(lj_thr_now_ns() - start < UINT64_C(10000000000));
    la_cpu_pause();
  }
  handshake_epoch = gc2_hs_epoch_acq(g);
  assert(gc2_hs_leader_acq(g) == worker_tid);
  assert(gc2_hs_pending_acq(g) != 0);
  assert(lj_tg_poll_acq(tg) == 1);
  assert(lj_tg_hs_epoch_ack_acq(tg) != handshake_epoch);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_state_owner_word_acq(L) == owner);
  assert(gc2_phase_acq(g) == LJ_GC2_SWEEP && gc2_cycle_acq(g) == cycle);
  printf("{\"stage\":\"pending_before_stop\",\"actions\":%u,"
         "\"workers\":%u,\"api\":%u,\"phase\":%u,\"cycle\":%u,"
         "\"leader\":%u,\"pending\":%u,\"main_native\":%u,"
         "\"epoch\":%" PRIu64 ",\"owner\":%" PRIu64 "}\n",
         wanted, workers, api, gc2_phase_acq(g), cycle, worker_tid,
         gc2_hs_pending_acq(g), lj_tg_in_native_acq(tg), handshake_epoch,
         (uint64_t)owner);
  start = lj_thr_now_ns();
  ok = api ? lj_gc2_workers_set(g, 0) : lj_gc2_workers_set_l(L, 0, &actions);
  elapsed = lj_thr_now_ns() - start;
  assert(ok == 1 && elapsed < UINT64_C(10000000000));
  assert(gc2_n_workers_acq(g) == 0 && gc2_worker_stop_acq(g) == 1);
  assert(gc2_worker_exited_acq(g) == workers);
  assert(gc2_hs_leader_acq(g) == 0 && gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_reqmask_acq(tg) == 0 && lj_tg_poll_acq(tg) == 0);
  assert(lj_tg_in_native_acq(tg) == 0);
  assert(lj_state_owner_word_acq(L) == owner);
  assert(lua_tolstring(L, -1, NULL) == identity);
  assert(actions == 0);
  printf("{\"stage\":\"joined\",\"actions\":%u,\"workers\":%u,"
         "\"api\":%u,\"ns\":%" PRIu64 ",\"phase\":%u,"
         "\"cycle\":%u,\"exited\":%u,\"canonical\":true}\n",
         wanted, workers, api, elapsed, gc2_phase_acq(g), gc2_cycle_acq(g),
         gc2_worker_exited_acq(g));
  lua_gc(L, LUA_GCCOLLECT, 0);  /* Teardown, not the shutdown oracle. */
  lua_close(L);
  puts("PASS worker bridge pending handshake stop/join");
  return 0;
}
