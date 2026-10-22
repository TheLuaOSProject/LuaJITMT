/* Target only safepoint calls; keep the real GC scanner and kernel wait. */
#define __ulock_wait lj_test_completion_ulock_wait
#define lj_gc2_scan_cycle_owner_tg_roots_native_parked \
  __wrap_lj_gc2_scan_cycle_owner_tg_roots_native_parked
#include "../../src/lj_safepoint.c"
