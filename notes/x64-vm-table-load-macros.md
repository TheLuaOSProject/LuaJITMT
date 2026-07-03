2026-06-20

- Added x64 DASC helper macros for table pointer/size reads:
  `x64_vm_tab_node_acq`, `x64_vm_tab_array_acq`,
  `x64_vm_tab_asize_acq`, `x64_vm_tab_array_asize_acq`,
  `x64_vm_tab_node_flags_acq`, `x64_vm_tab_node_hmask_acq`, and
  `x64_vm_node_next_acq`.
- Routed x64 VM fast-path table array/node and hash-chain link loads through
  those macros. On x86-64 this still lowers to the same `mov`, but the source
  now names the acquire-load contract used by the C helpers.
- Documented that x64 VM table-link/size loads should go through the helper
  macros. The helper comment and behavior coverage own the invariant.
- The remaining x64 metatable/gcroot GCRef loads/stores are a separate
  follow-up with a different helper vocabulary.
- Follow-up: legacy `GCtab.asize` fallback loads in x64 table fast paths now
  use `x64_vm_tab_asize_acq`; the helper macro names the acquire-load
  contract.
- Follow-up: x64 table fast paths now load `TabArrayHdr.asize` and
  `TabNodeHdr.flags/hmask` through named acquire helpers.
- Validation:
  `tools/ci/m5_x64_table_next_snapshot.sh`,
  `tools/ci/lua_test.sh m5_x64_tget_array_header m5_x64_tgets_node_order m5_x64_getmetatable_node_order m5_x64_ipairs_snapshot m5_x64_itern_snapshot`,
  and `tools/ci/m5_x64_tset_nil_snapshot.sh`.
