# Table New-Key Private Chain Scan

`lj_tab_newkey()` now lets the private single-mutator path scan a stable
collision chain before entering the shared KEYLOCK/CAS lookup. This applies only
under `tab_private_mutation_allowed()`: no active/entering MT, no GC2 workers,
and no active marking on the current TG.

The path still reloads after the private predicate, preserves the chain-length
overflow rule, verifies the current hash generation before modifying the vector,
canonicalizes the key through `tab_storekeyrel()`, and keeps the weak-key and
GC publication barriers. Active MT and bridge-sensitive FINREG helpers still use
the shared KEYLOCK/CAS protocol.

Focused verification:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 LUA=luajit tools/ci/lua_test.sh m5_tab_keylock_lookup m5_tab_finreg_newkey_stale m5_tab_resize_stress m5_tab_struct_owner m5_tab_colocated_resize m9_newkey_barrier_scope`
- `LJ_BENCH_STOCK_FILTERS='tab_insert_newkey' LJ_BENCH_STOCK_SCALE=0.05 LJ_BENCH_STOCK_MAX=10 LJ_BENCH_STOCK_TIMEOUT=120s LUA=luajit tools/ci/lua_test.sh m9_bench_stock_compare`
