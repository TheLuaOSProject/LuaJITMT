# Trace Flush Slot Scan SMR

Bulk trace flush paths scan public trace slots while the JIT token is held or a
flush handshake is active. The token prevents new trace publication, but it does
not block GC2 SMR reclamation of older retired bodies that may still occupy a
public slot until release.

This slice wraps the bulk slot walks in `trace_exittab_resetroot()`,
`trace_flushscope_mark_deps()`, `lj_trace_flushscope_retire_hs()`, and
`trace_flushall_direct()` with GC2 SMR read sections and loads scanned slots via
`traceref_safe()`.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m0_matrix`
