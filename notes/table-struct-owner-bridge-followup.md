# table structural owner bridge follow-up

- `global_State.gc2.tab_struct_owner` has been replaced by
  `GCtab.struct_owner`. `lj_tab_struct_enter(L, t)` now CAS-claims only the
  table being structurally mutated, so unrelated table resize/clear/table.insert
  structural shifts no longer serialize on one global owner word.
- Same-table structural mutation is still serialized by the per-table owner.
  This preserves the current resize/copy safety bridge while narrowing the
  bottleneck to the table being changed.
- Same-table contenders now park on `GCtab.struct_owner` through the existing
  cross-platform `la_futex_wait()` / `la_futex_wake()` substrate instead of
  sleeping through the generic fixed 1 ms retry helper. Waiters enter native
  state around the timed futex wait so safepoint/STOPREQ visibility follows the
  existing blocking bridge convention.
- The earlier direct table-local prototype failed because active-MT shared
  `next()`/optimized `pairs()` tracing was still unsafe under concurrent
  resize/value churn. Current HEAD keeps those shared traversal paths
  interpreted under active MT; with that recorder fence restored, the per-table
  owner stress is stable.
- The fixed 1 ms wait helper remains a pending bridge gap for transient
  `KEYLOCK`, value-publication, and generic FORWARD/generation retry waits. The
  final design still needs per-generation resize ownership, bounded copy
  cursors, writer helping, and reader hop/retry.

Verification:

- `tools/ci/lua_test.sh m5_tab_struct_owner`
- `tools/ci/lua_test.sh m6_jit_token m6_jit_cell_ops m5_tab_next_snapshot m5_x64_table_next_snapshot`
- `LJ_M5_TAB_RESIZE_TRAVERSAL_MODES=pairs LJ_M5_TAB_RESIZE_STRESS_CASES=traversal,nextchurn tools/ci/lua_test.sh m5_tab_resize_stress`, 20/20 runs
- `tools/ci/lua_test.sh m5_tab_keylock_lookup m5_tab_next_snapshot m5_tab_colocated_resize m5_tab_capi_resize_stress m5_tab_resize_stress m5_tab_struct_owner`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m5_tab_struct_owner`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m5_tab_resize_stress`
