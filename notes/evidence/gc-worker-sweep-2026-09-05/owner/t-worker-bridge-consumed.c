#define main stop_fixture_main
#include "t-worker-bridge-stop.c"
#undef main
#include <pthread.h>
#include "lj_arena.h"

static TGState *held_tg;
static uint32_t action_paused, action_release, leave_entered, detach_entered;
static uint32_t native_depth_at_leave;

static void hold_remote_action(global_State *g, TGState *target)
{
  TGState *self = lj_thr_get_tg();
  assert(target && target != self && target != g->main_tg);
  assert(lj_tg_load_cur_L(target) == NULL && lj_tg_load_thread_L(target) == NULL);
  assert(lj_tg_tid_acq(self) == worker_tid);
  assert(gc2_hs_leader_acq(g) == worker_tid);
  assert(lj_tg_reqmask_acq(target) == 0 && lj_tg_poll_acq(target) == 1);
  assert(lj_tg_in_native_acq(target) == 1);
  assert(!lj_tg_flags_test_acq(target, TGF_DEAD));
  la_storeptr_rel((void **)&held_tg, target);
  la_store32_rel(&action_paused, 1);
  wait_value(&action_release, 1);
}

extern int __real_lj_gc2_scan_cycle_owner_tg_roots_native_parked(global_State *, TGState *);
int __wrap_lj_gc2_scan_cycle_owner_tg_roots_native_parked(global_State *g, TGState *tg)
{
  if (g == probe_g && wanted == LJ_GC2_HS_SCAN_ROOTS &&
      (tg == gc2_worker_tg_acq(g, 0) || tg == gc2_worker_tg_acq(g, 1)) &&
      tg != lj_thr_get_tg())
    hold_remote_action(g, tg);
  return __real_lj_gc2_scan_cycle_owner_tg_roots_native_parked(g, tg);
}

extern int __real_lj_arena_alloc_prepare_sweep_kind(TGAlloc *, uint32_t);
int __wrap_lj_arena_alloc_prepare_sweep_kind(TGAlloc *alloc, uint32_t kind)
{
  global_State *g = probe_g;
  TGState *self = lj_thr_get_tg();
  unsigned i;
  if (g && wanted == LJ_GC2_HS_RESET_ALLOC &&
      kind == LJ_ARENAK_TRAVERSABLE && self != g->main_tg) {
    for (i = 0; i < 2; i++) {
      TGState *tg = gc2_worker_tg_acq(g, i);
      if (tg && tg != self && alloc == &tg->alloc) {
        hold_remote_action(g, tg);
        break;
      }
    }
  }
  return __real_lj_arena_alloc_prepare_sweep_kind(alloc, kind);
}

extern uint32_t __real_lj_native_leave_tg(TGState *);
uint32_t __wrap_lj_native_leave_tg(TGState *tg)
{
  TGState *target = (TGState *)la_loadptr_acq((void *const *)&held_tg);
  if (tg == target && target) {
    assert(tg == lj_thr_get_tg());
    native_depth_at_leave = lj_tg_in_native_acq(tg);
    assert(native_depth_at_leave == 1);
    la_store32_rel(&leave_entered, 1);
  }
  return __real_lj_native_leave_tg(tg);
}

extern void __real_lj_tg_detach(global_State *, TGState *);
void __wrap_lj_tg_detach(global_State *g, TGState *tg)
{
  TGState *target = (TGState *)la_loadptr_acq((void *const *)&held_tg);
  if (tg == target && target) {
    la_store32_rel(&detach_entered, 1);
    assert(la_load32_acq(&action_release) == 1);
    assert(la_load32_acq(&leave_entered) == 1);
    assert(lj_tg_in_native_acq(tg) == 0);
    assert(lj_tg_poll_acq(tg) == 0);
  }
  __real_lj_tg_detach(g, tg);
}

static void *observe_hold(void *unused)
{
  global_State *g = probe_g;
  TGState *target;
  uint64_t start, epoch;
  UNUSED(unused);
  wait_value(&g->gc2.worker_stop, 1);
  wait_value(&leave_entered, 1);
  target = (TGState *)la_loadptr_acq((void *const *)&held_tg);
  start = lj_thr_now_ns();
  while (lj_tg_in_native_acq(target) != 0) {
    assert(lj_thr_now_ns() - start < UINT64_C(10000000000));
    la_cpu_pause();
  }
  epoch = gc2_hs_epoch_acq(g);
  /* Keep the actual remote action paused while its target must remain behind
  ** the consumed poll. This is a real owner close, not an injected native bit. */
  usleep(20000);
  assert(lj_tg_reqmask_acq(target) == 0 && lj_tg_poll_acq(target) == 1);
  assert(!lj_tg_flags_test_acq(target, TGF_DEAD));
  assert(gc2_hs_leader_acq(g) == worker_tid && gc2_hs_pending_acq(g) != 0);
  assert(la_load32_acq(&detach_entered) == 0);
  if (wanted == LJ_GC2_HS_RESET_ALLOC)
    assert(lj_tg_hs_epoch_ack_acq(target) == epoch);
  else
    assert(lj_tg_hs_epoch_ack_acq(target) != epoch);
  printf("{\"stage\":\"native_leave_holds_consumed_action\",\"wanted\":%u,"
         "\"tid\":%u,\"depth_before_leave\":%u,\"native\":%u,"
         "\"poll\":%u,\"reqmask\":%u,\"epoch_claimed\":%u,"
         "\"detach_entered\":%u}\n",
         wanted, lj_tg_tid_acq(target), native_depth_at_leave,
         lj_tg_in_native_acq(target), lj_tg_poll_acq(target),
         lj_tg_reqmask_acq(target), lj_tg_hs_epoch_ack_acq(target) == epoch,
         la_load32_acq(&detach_entered));
  la_store32_rel(&action_release, 1);
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
  la_store32_rel(&release_worker, 1);
  wait_value(&action_paused, 1);
  assert(pthread_create(&observer, NULL, observe_hold, NULL) == 0);
  actions = 0;
  assert(lj_gc2_workers_set_l(L, 0, &actions) == 1);
  assert(pthread_join(observer, NULL) == 0);
  assert(la_load32_acq(&detach_entered) == 1);
  assert(gc2_n_workers_acq(g) == 0 && gc2_worker_exited_acq(g) == 2);
  assert(gc2_hs_leader_acq(g) == 0 && gc2_hs_pending_acq(g) == 0);
  assert(lj_tg_in_native_acq(L2TG(L)) == 0);
  assert(actions == 0);
  lua_gc(L, LUA_GCCOLLECT, 0);
  lua_close(L);
  puts("PASS consumed remote action retains worker before private detach");
  return 0;
}
