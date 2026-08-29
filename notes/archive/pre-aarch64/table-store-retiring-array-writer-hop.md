# Table store retiring-array writer hop

Store helpers can see a table while the old separated array is still the table
root but has `TABARRAY_FLAG_RETIRING` set and `next_gen` published. In that
window old slots may still contain ordinary non-`FORWARD` values. Treating the
old slot as current can write stale storage; falling back to `lj_tab_setinth()`
can move an array write into the hash part while resize publication is still in
progress.

Read-side forwarding still requires an observed `FORWARD` slot before following
`next_gen` from the current root. Store-side resolution is different: migration
uses `tab_migrate_store_if_absent`, so a writer that publishes into the
successor array cannot be overwritten by later migration. `lj_tab.c` now has a
writer-only array hop used by JIT/VM store-slot resolution and keyed CAS
validation. If a successor is not visible yet, the helper waits/retries instead
of falling through to integer-hash insertion.

Coverage:

- `tests/t-tab-cas-store.c` covers `lj_tab_storetv_forjit_array_nogc()` with a
  current-root retiring array and a stale non-forward old slot.
- `tests/t-x64-tset-forward.c` covers x64 `BC_TSETB`, `BC_TSETV`, `BC_TSETR`,
  and the VM helper over current-root retiring stale slots.

Validation:

- `make -C src -j$(nproc)`
- `LUA=src/luajit tools/ci/lua_test.sh m5_tab_cas_store m5_x64_tset_nil_snapshot`
- `LUA=src/luajit tools/ci/lua_test.sh m5_x64_getmetatable_node_order m5_x64_tget_array_header m5_x64_tgets_node_order m5_x64_ipairs_snapshot m5_x64_itern_snapshot m5_x64_table_next_snapshot m5_x64_tset_nil_snapshot`
- `LUA=src/luajit tools/ci/lua_test.sh m6_jit_table_store_helper`
- `LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet` (509 passed)
