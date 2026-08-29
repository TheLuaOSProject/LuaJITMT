# Trace Recorder Helper Safe Lookups

The active recorder owns the JIT token, but a few helper paths still decoded
public trace slots directly while copying scalar metadata or checking whether a
pending bytecode patch could be published.

This slice uses short GC2 SMR read sections and `traceref_safe()` for:

- GDBJIT parent `spadjust` copying in `lj_gdbjit_addtrace()`.
- Abort self-link blacklisting in `trace_abort()`.
- Pending JIT bytecode patch checks in `trace_pendpatch()`.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `make -C src clean && make -C src -j$(nproc) TARGET_STRIP=: XCFLAGS=-DLUAJIT_USE_GDBJIT`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m0_matrix`
