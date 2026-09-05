#include <stdint.h>
#include "lj_obj.h"
#include "lj_gc2.h"
#include "lj_atomic.h"
typedef struct DiagReadyState {
  uint32_t phase, cycle, ready, scanned, jit_gate, leader, worker;
} DiagReadyState;
typedef struct DiagReadyCall {
  global_State *g;
  void *return_address;
  DiagReadyState before, after;
  uint32_t returned;
} DiagReadyCall;
DiagReadyCall diag_ready_calls[64];
uint32_t diag_ready_count;
static void diag_ready_state(global_State *g, DiagReadyState *s)
{
  s->phase = gc2_phase_acq(g);
  s->cycle = gc2_cycle_acq(g);
  s->ready = gc2_sweep_bridge_ready_acq(g);
  s->scanned = gc2_sweep_root_scanned_acq(g);
  s->jit_gate = gc2_jit_phase_gate_acq(g);
  s->leader = gc2_cycle_leader_acq(g);
  s->worker = gc2_worker_active_acq(g);
}
extern void __real_lj_gc2_sweep_bridge_ready(global_State *g);
void __wrap_lj_gc2_sweep_bridge_ready(global_State *g)
{
  uint32_t i = diag_ready_count++;
  DiagReadyCall *d = &diag_ready_calls[i % 64u];
  d->g = g;
  d->return_address = __builtin_return_address(0);
  d->returned = 0;
  diag_ready_state(g, &d->before);
  __real_lj_gc2_sweep_bridge_ready(g);
  diag_ready_state(g, &d->after);
  d->returned = 1;
}
