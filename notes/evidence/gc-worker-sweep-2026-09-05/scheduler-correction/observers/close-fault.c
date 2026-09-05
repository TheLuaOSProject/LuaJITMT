#include "lj_obj.h"
#include "lj_gc2.h"
extern int __real_lj_gc2_sweep_to_idle(global_State *g);
int __wrap_lj_gc2_sweep_to_idle(global_State *g)
{
  if (gc2_n_workers_acq(g)==0 && gc2_phase_acq(g)==LJ_GC2_SWEEP) {
#if DIAG_CLOSE_FAULT == 1
    if (!gc2_sweep_bridge_ready_acq(g)) return 1;
#else
    if (gc2_sweep_bridge_ready_acq(g)) return 0;
#endif
  }
  return __real_lj_gc2_sweep_to_idle(g);
}
