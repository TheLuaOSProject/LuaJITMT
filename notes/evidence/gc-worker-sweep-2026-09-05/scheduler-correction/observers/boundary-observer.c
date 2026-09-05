#include <stdint.h>
#include <stdio.h>
#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc2.h"
typedef struct DiagState { uint32_t phase, cycle, ready, scanned, jit_gate, leader, worker, nworkers, nthreads; } DiagState;
typedef struct DiagCall { const char *name; global_State *g; void *caller; DiagState before, after; int result; } DiagCall;
static DiagCall diag_calls[32];
static uint32_t diag_count;
static void diag_state(global_State *g, DiagState *s)
{
 s->phase=gc2_phase_acq(g);s->cycle=gc2_cycle_acq(g);s->ready=gc2_sweep_bridge_ready_acq(g);s->scanned=gc2_sweep_root_scanned_acq(g);s->jit_gate=gc2_jit_phase_gate_acq(g);s->leader=gc2_cycle_leader_acq(g);s->worker=gc2_worker_active_acq(g);s->nworkers=gc2_n_workers_acq(g);s->nthreads=gc2_n_threads_acq(g);
}
extern void __real_lj_gc2_sweep_bridge_ready(global_State *g);
extern int __real_lj_gc2_sweep_to_idle(global_State *g);
void __wrap_lj_gc2_sweep_bridge_ready(global_State *g)
{
 DiagCall *d=&diag_calls[diag_count++%32u];d->name="ready";d->g=g;d->caller=__builtin_return_address(0);diag_state(g,&d->before);__real_lj_gc2_sweep_bridge_ready(g);diag_state(g,&d->after);d->result=0;
}
int __wrap_lj_gc2_sweep_to_idle(global_State *g)
{
 DiagCall *d=&diag_calls[diag_count++%32u];d->name="close";d->g=g;d->caller=__builtin_return_address(0);diag_state(g,&d->before);d->result=__real_lj_gc2_sweep_to_idle(g);diag_state(g,&d->after);return d->result;
}
__attribute__((destructor)) static void diag_report(void)
{
 uint32_t i;fprintf(stderr,"DIAG_BOUNDARIES %u\n",diag_count);
 for(i=0;i<diag_count&&i<32u;i++){
 DiagCall *d=&diag_calls[i];fprintf(stderr,"BOUNDARY %u %s caller=%p result=%d phase=%u->%u cycle=%u ready=%u->%u scanned=%u->%u jit=%u->%u leader=%u->%u worker=%u->%u nworkers=%u nthreads=%u\n",i,d->name,d->caller,d->result,d->before.phase,d->after.phase,d->before.cycle,d->before.ready,d->after.ready,d->before.scanned,d->after.scanned,d->before.jit_gate,d->after.jit_gate,d->before.leader,d->after.leader,d->before.worker,d->after.worker,d->before.nworkers,d->before.nthreads);
 }
}
