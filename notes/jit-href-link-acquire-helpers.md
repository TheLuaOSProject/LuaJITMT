2026-06-20

- Added `asm_href_node_next_acq()` and `asm_href_tab_node_acq()` in the
  x86/x64 assembler backend.
- Routed regular `IR_HREF` hash-chain and table-node loads through those
  helpers. On x86-64 the emitted instruction remains a plain `mov`, but the
  backend source now names the acquire-load contract shared with the C table
  helpers.
- Added `asm_href_tab_node_flags_test_acq()` plus
  `asm_href_tab_node_hmask_{load,and,cmpi}_acq()` so x64 HREF/HREFK
  node-header flag and mask reads are named at the backend helper boundary.
- Documented that HREF table-link and node-header reads should go through the
  named acquire helpers. The old static source-text check for open-coded offsets is
  obsolete; generated/backend behavior is covered by
  `tools/ci/lua_test.sh m6_jit_href_nodehdr m6_jit_hrefk_nodehdr`.
