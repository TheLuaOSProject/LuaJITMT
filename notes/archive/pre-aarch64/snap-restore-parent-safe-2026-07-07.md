# Snapshot Restore Parent Lookup

The legacy snapshot restore entry point can enter `snap_restore()` without a
pre-resolved trace body and falls back to `J->parent`. The exit path already
holds a GC2 SMR read section while restoring the snapshot, so the fallback must
use the same trace-slot validation as other retired-body readers.

This slice changes the fallback from raw `traceref()` to `traceref_safe()` and
asserts that a live body was found before reading trace IR and snapshot arrays.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m0_matrix`
