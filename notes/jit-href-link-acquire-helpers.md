2026-06-20

- Added `asm_href_node_next_acq()` and `asm_href_tab_node_acq()` in the
  x86/x64 assembler backend.
- Routed regular `IR_HREF` hash-chain and table-node loads through those
  helpers. On x86-64 the emitted instruction remains a plain `mov`, but the
  backend source now names the acquire-load contract shared with the C table
  helpers.
- Extended `tools/ci/m5_jit_href_node_order.sh` with a static source guard
  rejecting open-coded `offsetof(Node, next)` and `offsetof(GCtab, node)`
  HREF table-link loads outside the helper bodies.
- Validation: `tools/ci/m5_jit_href_node_order.sh` and
  `tools/ci/lua_test.sh m6_jit_href_nodehdr m6_jit_hrefk_nodehdr`.
