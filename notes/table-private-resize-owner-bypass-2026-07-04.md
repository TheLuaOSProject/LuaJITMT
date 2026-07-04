# Table Private Resize Owner Bypass

2026-07-04

- `lj_tab_resize()` now skips `GCtab.struct_owner` in the same private mutation
  window used by direct table insertion: no active/entering MT, no GC2 workers,
  and no active marking.
- The shared path is unchanged. Once another Lua thread can attach/observe the
  table, workers are parked, or active marking needs the publication protocol,
  resize still acquires the per-table structural owner before publishing a new
  array/hash generation.
- This removes the owner CAS from single-mutator resize/compaction without
  weakening racy table semantics. The resize still uses the existing forwarding,
  migration, and retire machinery; only the unnecessary private owner claim is
  skipped.
- Verification:
  - `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m5_tab_struct_owner m5_table_insert_entering m5_tab_resize_stress m5_tab_capi_resize_stress m5_tab_colocated_resize`
  - `LJ_BENCH_STOCK_FILTERS='tab_insert_newkey alloc_tables' LJ_BENCH_STOCK_SCALE=0.1 LJ_BENCH_STOCK_MAX=10 LJ_BENCH_STOCK_TIMEOUT=120s tools/ci/lua_test.sh m9_bench_stock_compare`
