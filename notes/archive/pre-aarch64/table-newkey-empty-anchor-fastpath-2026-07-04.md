# Table New-Key Empty-Anchor Fast Path

2026-07-04

- Added a private-mutator fast path for hash inserts whose primary anchor is
  empty, current-generation, and has no collision chain. In that state there is
  no duplicate key reachable from the anchor, so the generic duplicate scan and
  KEYLOCK handoff can be skipped for this one case.
- The path is intentionally narrow. It reloads the anchor after checking the
  private/current-generation predicates and falls back for active MT, GC worker
  participation, active marking, KEYLOCK claims, resize/retiring generations,
  tombstones, non-empty anchors, and any collision chain.
- Publication still uses `tab_storekeyrel()`, `lj_gc2_barrier_weak_key()`, and
  `lj_gc_pubtabkey()`. The optimization removes avoidable scan/locking overhead
  for private new-key inserts without bypassing weak-key or GC visibility
  requirements.
- Verification:
  - `make -C src -j$(nproc)`
  - `LJ_BENCH_STOCK_FILTERS='tab_insert_newkey tab_hash_write tab_store_existing string_intern' LJ_BENCH_STOCK_SCALE=0.2 LJ_BENCH_STOCK_MAX=10 LJ_BENCH_STOCK_TIMEOUT=120s tools/ci/lua_test.sh m9_bench_stock_compare`
  - `LJ_BENCH_STOCK_FILTERS='tab_insert_newkey' LJ_BENCH_STOCK_SCALE=0.05 LJ_BENCH_STOCK_MAX=10 LJ_BENCH_STOCK_TIMEOUT=120s tools/ci/lua_test.sh m9_bench_stock_compare`
  - `LJ_TEST_DISABLE_BUILD_CACHE=1 LJ_M5_TAB_RESIZE_STRESS_CASES=traversal,nextchurn LJ_M5_TAB_RESIZE_TRAVERSAL_MODES=pairs,next,ipairs LJ_M5_TAB_RESIZE_STRESS_REPS=1536 LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS=384 LJ_M5_TAB_RESIZE_STRESS_TIMEOUT=90s tools/ci/lua_test.sh m5_tab_resize_stress`
  - `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m5_tab_keylock_lookup m5_tab_finreg_newkey_stale m5_tab_resize_stress m5_tab_struct_owner m5_tab_capi_resize_stress m5_tab_colocated_resize m9_newkey_barrier_scope m6_jit_table_store_helper`
