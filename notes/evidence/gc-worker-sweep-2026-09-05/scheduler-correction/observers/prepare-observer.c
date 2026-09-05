#include <stdint.h>
#include <stdio.h>
#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc2.h"
#include "lj_safepoint.h"
#include "lj_tg.h"
typedef struct DiagPrepareState {
  uint32_t phase, cycle, leader, worker, ready, scanned;
  uint32_t main_prepare, workers_prepare[LJ_GC2_WORKER_MAX];
} DiagPrepareState;
typedef struct DiagPrepareCall {
  global_State *g;
  void *return_address;
  uint32_t actions, returned;
  DiagPrepareState before, after;
} DiagPrepareCall;
DiagPrepareCall diag_prepare_calls[64];
uint32_t diag_prepare_count;
static void diag_prepare_state(global_State *g, DiagPrepareState *s)
{
  uint32_t i;
  s->phase = gc2_phase_acq(g);
  s->cycle = gc2_cycle_acq(g);
  s->leader = gc2_cycle_leader_acq(g);
  s->worker = gc2_worker_active_acq(g);
  s->ready = gc2_sweep_bridge_ready_acq(g);
  s->scanned = gc2_sweep_root_scanned_acq(g);
  s->main_prepare = g->main_tg->alloc.prepare_epoch;
  for (i = 0; i < LJ_GC2_WORKER_MAX; i++) {
    TGState *tg = gc2_worker_tg_acq(g, i);
    s->workers_prepare[i] = tg ? tg->alloc.prepare_epoch : UINT32_MAX;
  }
}
extern uint32_t __real_lj_safepoint_handshake(global_State *g, uint32_t actions);
uint32_t __wrap_lj_safepoint_handshake(global_State *g, uint32_t actions)
{
  uint32_t result, i;
  DiagPrepareCall *d;
  if ((actions & LJ_GC2_HS_RESET_ALLOC) == 0)
    return __real_lj_safepoint_handshake(g, actions);
  i = __atomic_fetch_add(&diag_prepare_count, 1, __ATOMIC_RELAXED);
  d = &diag_prepare_calls[i % 64u];
  d->g = g;
  d->return_address = __builtin_return_address(0);
  d->actions = actions;
  d->returned = 0;
  diag_prepare_state(g, &d->before);
  result = __real_lj_safepoint_handshake(g, actions);
  diag_prepare_state(g, &d->after);
  d->returned = 1;
  return result;
}
__attribute__((destructor)) static void diag_prepare_report(void)
{
  uint32_t i;
  fprintf(stderr, "DIAG_PREPARE_CALLS %u\n", diag_prepare_count);
  for (i = 0; i < diag_prepare_count && i < 64u; i++) {
    DiagPrepareCall *d = &diag_prepare_calls[i];
    fprintf(stderr, "PREPARE %u caller=%p actions=%u cycle=%u phase=%u ready=%u scanned=%u leader=%u worker=%u main=%u->%u worker0=%u->%u worker1=%u->%u returned=%u\n", i, d->return_address, d->actions, d->before.cycle, d->before.phase, d->before.ready, d->before.scanned, d->before.leader, d->before.worker, d->before.main_prepare, d->after.main_prepare, d->before.workers_prepare[0], d->after.workers_prepare[0], d->before.workers_prepare[1], d->after.workers_prepare[1], d->returned);
  }
}
