/* Reuse the exact real SWEEP setup and handshake-entry observation pause. */
#define main stop_fixture_main
#include "t-worker-bridge-stop.c"
#undef main
#include <pthread.h>
#include "lj_arena.h"

static TGState *detach_tg;
static uint32_t detach_paused, detach_release, action_paused, action_release;
static uint32_t remote_calls, overlapped, detach_native;

extern uint32_t __real_lj_gc2_flush_ssb_detach(global_State *, TGState *);
uint32_t __wrap_lj_gc2_flush_ssb_detach(global_State *g, TGState *tg)
{
  if (g == probe_g && tg != g->main_tg &&
      lj_tg_tid_acq(tg) != worker_tid && tg == lj_thr_get_tg()) {
    LJTGSlotSnap snap;
    assert(gc2_worker_stop_acq(g) == 1);
    assert(lj_tg_reqmask_acq(tg) == 0 && lj_tg_poll_acq(tg) == 0);
    assert(lj_tgregistry_key_snapshot(&tg->registry_key, &snap) == LJ_TGSLOT_OK);
    assert(snap.state == LJ_TGSLOT_DETACHING);
    detach_native = lj_tg_in_native_acq(tg);
    la_storeptr_rel((void **)&detach_tg, tg);
    la_store32_rel(&detach_paused, 1);
    wait_value(&detach_release, 1);
  }
  return __real_lj_gc2_flush_ssb_detach(g, tg);
}

static void pause_remote_action(global_State *g, TGState *target)
{
  TGState *self = lj_thr_get_tg();
  LJTGSlotSnap snap;
  assert(target && target != self);
  assert(gc2_hs_leader_acq(g) == worker_tid);
  assert(self && lj_tg_tid_acq(self) == worker_tid);
  assert(lj_tg_reqmask_acq(target) == 0);
  assert(lj_tg_poll_acq(target) == 1);
  assert(lj_tg_in_native_acq(target) == 1);
  assert(gc2_hs_pending_acq(g) != 0);
  assert(lj_tgregistry_key_snapshot(&target->registry_key, &snap) == LJ_TGSLOT_OK);
  assert(snap.state == LJ_TGSLOT_DETACHING);
  (void)la_add32_rlx(&remote_calls, 1);
  assert(la_load32_acq(&remote_calls) == 1);
  la_store32_rel(&action_paused, 1);
  wait_value(&action_release, 1);
}

extern int __real_lj_gc2_scan_cycle_owner_tg_roots_native_parked(global_State *, TGState *);
int __wrap_lj_gc2_scan_cycle_owner_tg_roots_native_parked(global_State *g, TGState *tg)
{
  TGState *target = (TGState *)la_loadptr_acq((void *const *)&detach_tg);
  if (g == probe_g && tg == target && wanted == LJ_GC2_HS_SCAN_ROOTS)
    pause_remote_action(g, tg);
  return __real_lj_gc2_scan_cycle_owner_tg_roots_native_parked(g, tg);
}

extern int __real_lj_arena_alloc_prepare_sweep_kind(TGAlloc *, uint32_t);
int __wrap_lj_arena_alloc_prepare_sweep_kind(TGAlloc *alloc, uint32_t kind)
{
  TGState *target = (TGState *)la_loadptr_acq((void *const *)&detach_tg);
  if (target && alloc == &target->alloc &&
      wanted == LJ_GC2_HS_RESET_ALLOC && kind == LJ_ARENAK_TRAVERSABLE)
    pause_remote_action(probe_g, target);
  return __real_lj_arena_alloc_prepare_sweep_kind(alloc, kind);
}

static void *observe_detach(void *unused)
{
  global_State *g = probe_g;
  TGState *target;
  LJTGSlotSnap snap;
  uint64_t start;
  UNUSED(unused);
  wait_value(&g->gc2.worker_stop, 1);
  wait_value(&detach_paused, 1);
  target = (TGState *)la_loadptr_acq((void *const *)&detach_tg);
  assert(target != NULL && !lj_tg_flags_test_acq(target, TGF_DEAD));
  printf("{\"stage\":\"detach_after_own_poll\",\"tid\":%u,"
         "\"native\":%u,\"reqmask\":%u,\"poll\":%u}\n",
         lj_tg_tid_acq(target), detach_native,
         lj_tg_reqmask_acq(target), lj_tg_poll_acq(target));
  la_store32_rel(&release_worker, 1);
  if (detach_native != 0) {
    wait_value(&action_paused, 1);
    printf("{\"stage\":\"remote_private_action_admitted\",\"wanted\":%u,"
           "\"tid\":%u,\"native\":%u,\"reqmask\":%u,\"poll\":%u,"
           "\"leader\":%u,\"epoch\":%" PRIu64 "}\n",
           wanted, lj_tg_tid_acq(target), lj_tg_in_native_acq(target),
           lj_tg_reqmask_acq(target), lj_tg_poll_acq(target),
           gc2_hs_leader_acq(g), gc2_hs_epoch_acq(g));
    la_store32_rel(&detach_release, 1);
    start = lj_thr_now_ns();
    while (!lj_tg_flags_test_acq(target, TGF_DEAD)) {
      assert(lj_thr_now_ns() - start < UINT64_C(10000000000));
      la_cpu_pause();
    }
    assert(lj_tgregistry_key_snapshot(&target->registry_key, &snap) == LJ_TGSLOT_OK);
    assert(snap.state == LJ_TGSLOT_RETIRED);
    assert(gc2_hs_leader_acq(g) == worker_tid);
    assert(gc2_hs_pending_acq(g) != 0);
    assert(la_load32_acq(&action_release) == 0);
    printf("{\"stage\":\"retired_before_remote_action_return\",\"wanted\":%u,"
           "\"tid\":%u,\"native\":%u,\"reqmask\":%u,\"poll\":%u,"
           "\"registry_state\":%u,\"actor\":%" PRIu64 "}\n",
           wanted, lj_tg_tid_acq(target), lj_tg_in_native_acq(target),
           lj_tg_reqmask_acq(target), lj_tg_poll_acq(target), (uint32_t)snap.state,
           (uint64_t)lj_tg_actor_acq(target));
    la_store32_rel(&overlapped, 1);
    la_store32_rel(&action_release, 1);
  } else {
    /* A repaired owner closes native before touching detach-private state.
    ** The actual request must stay owner-pending until detach retires it. */
    start = lj_thr_now_ns();
    while ((lj_tg_reqmask_acq(target) & wanted) == 0) {
      assert(lj_thr_now_ns() - start < UINT64_C(10000000000));
      la_cpu_pause();
    }
    assert(lj_tg_poll_acq(target) == 1);
    assert(la_load32_acq(&action_paused) == 0);
    assert(la_load32_acq(&remote_calls) == 0);
    printf("{\"stage\":\"remote_action_refused_owner_pending\","
           "\"wanted\":%u,\"tid\":%u,\"native\":%u,\"reqmask\":%u}\n",
           wanted, lj_tg_tid_acq(target), lj_tg_in_native_acq(target),
           lj_tg_reqmask_acq(target));
    la_store32_rel(&detach_release, 1);
  }
  return NULL;
}

int main(int argc, char **argv)
{
  lua_State *L;
  global_State *g;
  uint32_t actions = 0;
  uint64_t start;
  pthread_t observer;
  assert(argc == 2);
  wanted = atoi(argv[1]) ? LJ_GC2_HS_SCAN_ROOTS : LJ_GC2_HS_RESET_ALLOC;
  setvbuf(stdout, NULL, _IOLBF, 0);
  alarm(30);
  L = luaL_newstate(); assert(L);
  luaL_openlibs(L);
  assert(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE|LUAJIT_MODE_OFF));
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_gc(L, LUA_GCSTOP, 0);
  g = G(L);
  enter_real_sweep(L);
  probe_g = g;
  la_store32_rel(&armed, 1);
  assert(lj_gc2_workers_set_l(L, 2, &actions) == 1);
  start = lj_thr_now_ns();
  while (la_load32_acq(&paused) == 0) {
    assert(lj_thr_now_ns() - start < UINT64_C(10000000000));
    actions |= lj_safepoint_ack(L);
    la_cpu_pause();
  }
  assert((actions & LJ_GC2_HS_STOPREQ) == 0);
  assert(pthread_create(&observer, NULL, observe_detach, NULL) == 0);
  actions = 0;
  assert(lj_gc2_workers_set_l(L, 0, &actions) == 1);
  assert(pthread_join(observer, NULL) == 0);
  assert(gc2_n_workers_acq(g) == 0 && gc2_worker_exited_acq(g) == 2);
  assert(gc2_hs_leader_acq(g) == 0 && gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  assert(actions == 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_close(L);
  printf("{\"stage\":\"joined\",\"overlapped\":%u}\n", overlapped);
  return overlapped ? 2 : 0;
}
