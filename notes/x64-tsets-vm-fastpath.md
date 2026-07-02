# x64 TSETS Existing-Slot Fast Path

- Restored a bounded x64 VM fast path for `BC_TSETS_Z` existing string-key
  stores. The path probes the current hash generation with acquire loads and
  only handles matching keys whose value is non-nil and not `FORWARD`.
- The actual publication still goes through
  `lj_tab_storetv_forvm_strhash()`, which uses keyed CAS/revalidation and weak
  write coordination. This deliberately avoids restoring stock LuaJIT's raw
  `mov [slot], value` store, because concurrent resize can stale or forward the
  slot.
- Misses, nil slots, retired node generations, forwarded values, non-table
  operands, and metatable-sensitive cases still fall back to `vmeta_tsets` and
  `lj_meta_tsettv_pair()`.
- `BC_GSET` shares `BC_TSETS_Z`; after the helper call it must reload the
  parent table from the function environment, while normal `BC_TSETS` reloads
  the table operand from `PC_RB`. Stock `lang/assignment.lua` caught the first
  version of this bug.
- Verification:
  - `make -C src -j2`
  - direct `-joff` global and table assignment smokes
  - `tools/ci/lua_test.sh m5_x64_tset_nil_snapshot m5_tab_resize_stress m6_jit_table_store_helper`
  - `tools/ci/lua_test.sh run_stock_tests -- --quiet lang/assignment.lua`
  - `tools/ci/lua_test.sh run_stock_tests -- --quiet lang/table.lua`
