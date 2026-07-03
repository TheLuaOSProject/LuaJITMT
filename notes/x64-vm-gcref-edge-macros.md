2026-06-20

- Added x64 DASC helpers for GCRef edges:
  `x64_vm_gcref_acq`, `x64_vm_gcref_cmpzero`, `x64_vm_gcref_rel`,
  `x64_vm_gcroot_acq`, `x64_vm_gcroot_cmpzero`, and `x64_vm_basemt_acq`.
- Routed x64 VM metatable loads/stores, base-metatable root loads, the
  `__metatable` name root load, and function environment loads through the
  helpers. On x86-64 these still lower to ordinary `mov` instructions, but the
  source now names the acquire/release contract for shared GCRef edges.
- Extended `tools/ci/m5_x64_getmetatable_node_order.sh` with a documented invariant
  rejecting raw `->metatable`, `->env`, and `gcroot` access in `vm_x64.dasc`
  outside the helper calls.
- Expanded the x64 getmetatable smoke to cover the setmetatable fast path,
  global env get/set, and number `tostring()` base-metatable root check.
- Validation:
  `tools/ci/m5_x64_getmetatable_node_order.sh`,
  `tools/ci/lua_test.sh m5_x64_tgets_node_order m5_x64_tset_nil_snapshot`,
  `../../../src/luajit test.lua --quiet lang/meta/arith.lua` from
  `tests/stock/test`, and
  `tools/ci/run_stock_tests.sh ./src/luajit --quiet lang/meta`.
