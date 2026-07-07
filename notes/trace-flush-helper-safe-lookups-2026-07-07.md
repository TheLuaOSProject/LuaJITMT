# Trace Flush Helper Safe Lookups

Recent flush slices moved root, side, and scoped slot retirement callers under
GC2 SMR read sections. A few helper-internal trace chain walks still used raw
`traceref()` while following prototype root chains, parent exits, or side-trace
links.

This slice switches those caller-protected helper lookups to `traceref_safe()`
in `trace_unpatch()`, `trace_flushroot()`, `trace_flushside()`, and
`trace_scope_clear_slot()`.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m0_matrix`
