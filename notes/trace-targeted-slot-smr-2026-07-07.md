# Trace Targeted Slot SMR

Single-trace flush helpers read a public trace slot before checking whether the
body is still live for that trace number. A public slot can still hold a retired
body until SMR release, and the JIT token does not block the GC2 retired-body
reclaimer.

This slice wraps targeted slot reads in `trace_scope_flushing()`,
`lj_trace_flush()`, `lj_trace_flush_unlink()`, and `lj_trace_flushproto()` with
GC2 SMR read sections and uses `traceref_safe()` before the live `traceno`
check.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m0_matrix`
