# JIT pre-MT HREF retiring guard elision, 2026-07-03

Pre-MT x64 `HREF` and `HREFK` traces now omit the retiring-node-generation
guard. Active-MT and entering-MT traces still emit the guard before using a
loaded hash node generation.

This uses the same boundary as pre-MT direct array reads:
`mt_entering || mt_active`. Before that boundary, no secondary Lua thread can
resize a table concurrently with traced code. The first threading activation
sets `mt_entering`, flushes existing traces, and only then allows worker Lua
code to run, so old pre-MT hash-read traces do not survive into the shared-table
phase.

The remaining runtime coverage is behavior-based: HREF/HREFK traceability,
resize-after-trace reads, table forwarding fixtures, active-MT helper routing,
and stock-suite semantics. The assembler comment carries the implementation
reason.

Local validation:

- `tools/ci/lua_test.sh m6_jit_hrefk_nodehdr m6_jit_href_nodehdr
  m6_jit_mt_activation_flush m6_jit_aref_pair_boundary m6_jit_table_store_helper`
- `tools/ci/lua_test.sh m6_jit_threading_nyi_boundary`
- `tools/ci/lua_test.sh run_stock_tests -- --quiet`
- A tight prebuilt-key HREF microbench was effectively flat in this container;
  a constant-key HREFK probe improved from roughly 0.57 ns/op to 0.48 ns/op.
