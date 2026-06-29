# Colocated array resize freeze

- Growing a colocated array into a separated array now freezes the old inline
  slots to `FORWARD` before the new array is published.  The resize migrates
  every non-absent old value into the separated array after taking ownership of
  the old slot, instead of copying inline slots before publication.
- Array readers, traversal, absent-slot probes, and JIT/CAS forwarding helpers
  now treat `FORWARD` in a still-current colocated array as an in-progress
  split and wait/retry through the table's current array snapshot.
- Added `LJ_TAB_TEST_HELPERS` coverage in `tests/t-tab-colocated-resize.c`.
  The hook fires after the colocated slots are frozen but before publication
  and verifies keyed CAS stores into both live and nil old inline slots return
  `LJ_TAB_STORE_CAS_FORWARD`.

Validation:

- `tools/ci/lua_test.sh m5_tab_colocated_resize`
- `tools/ci/lua_test.sh m5_tab_array_publish`
- `tools/ci/lua_test.sh m5_tab_cas_store`
- `LJ_M5_TAB_RESIZE_STRESS_CASES=tablelib LJ_M5_TAB_RESIZE_STRESS_REPS=192 LJ_M5_TAB_RESIZE_STRESS_THREADS=2 tools/ci/lua_test.sh m5_tab_resize_stress`
