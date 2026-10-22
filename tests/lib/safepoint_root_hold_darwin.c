/* Interpose observation points while executing the real scanner and wait. */
#define __ulock_wait lj_test_completion_ulock_wait
#define lj_gc2_scan_cycle_owner_tg_roots_native_parked \
  __wrap_lj_gc2_scan_cycle_owner_tg_roots_native_parked
#define lj_gc2_scan_cycle_global_roots __wrap_lj_gc2_scan_cycle_global_roots
#define lj_gc2_flush_ssb __wrap_lj_gc2_flush_ssb
#include "../../src/lj_safepoint.c"
