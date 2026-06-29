# Table colocated resize freeze

When a table with a colocated inline array grows into a detached array, the
resize path now freezes every copied old inline slot with a `FORWARD` sentinel
before publishing the new array. This includes nil inline slots. Stale writers
that still hold an old inline slot therefore route through the forwarded table
path instead of successfully CAS-storing into memory that is no longer the live
array.

`lj_tab_forwarded_array_slot()`, array-slot lookup, `next()`, and the JIT helper
array store path now treat a still-colocated old array as a transient resize
state and retry or forward through the table. The `LJ_TAB_TEST_HELPERS` hook
`lj_tab_test_set_resize_colocated_after_freeze_hook()` lets the C fixture inject
stores after the freeze and before resize publication.

Validation:

- `tools/ci/lua_test.sh m5_tab_colocated_resize`
- `tools/ci/lua_test.sh m5_tab_finreg_newkey_stale m5_tab_keylock_lookup`
- `LJ_M5_TAB_RESIZE_STRESS_CASES=tablelib LJ_M5_TAB_RESIZE_STRESS_REPS=192 LJ_M5_TAB_RESIZE_STRESS_THREADS=2 tools/ci/lua_test.sh m5_tab_resize_stress`
- `tools/ci/lua_test.sh m5_tab_cas_store m7_ffi_finreg`
- `tools/ci/lua_test.sh m9_gc_stats m10_generational`
