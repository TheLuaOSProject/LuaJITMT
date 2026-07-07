# Debug Frame PC SMR Scan

`debug_jit_startpc()` repairs debug locations for frames whose PC points at a
trace `startins` pseudo-bytecode. It already validated that a current trace slot
still named the derived trace body before reading metadata, but the scan did
not hold a GC2 SMR read section while walking the trace vector.

This slice wraps the trace-vector scan in GC2 SMR and copies the bytecode
position before leaving the read section.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `src/luajit -e '... traced return debug.getinfo smoke ...'`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `tools/ci/lua_test.sh m0_matrix`
