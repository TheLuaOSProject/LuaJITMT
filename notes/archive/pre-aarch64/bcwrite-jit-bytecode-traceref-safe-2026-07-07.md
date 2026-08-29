# Bytecode Writer JIT Slot Lookup

`bcwrite_unpatch_jitins()` decodes patched JIT bytecode while dumping a
prototype. A concurrent trace flush can leave retired bodies in public trace
slots until GC2 SMR release, so raw slot reads are not safe for this diagnostic
unpatch path.

This slice wraps the JIT-bytecode trace lookup in a short GC2 SMR read section
and uses `traceref_safe()` before checking `trace_runnable_acq()` and copying
the trace `startins` value.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `src/luajit -e '... compiled loop string.dump smoke ...'`
- `tools/ci/lua_test.sh m0_matrix`
