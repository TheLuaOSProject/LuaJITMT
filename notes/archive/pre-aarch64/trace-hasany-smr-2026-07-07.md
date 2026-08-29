# Trace Hasany SMR

`lj_trace_hasany()` is used by API/threading boundaries to decide whether a
flush boundary has work to do. Its idle-state path scanned trace slots with raw
`traceref()` and then read `traceno` from the trace body without an SMR reader.

This slice wraps the slot scan in a GC2 SMR read section and loads each trace
body with `traceref_safe()` before checking the slot identity.

Validation exposed a second trace-reader gap: `jit.util.trace*` APIs resolved a
trace body with `jit_checktrace()` and then read IR/snapshot/mcode fields while
another thread could flush and retire the same body. Those readers now perform
argument checks before taking a non-blocking JIT token read lock and entering
SMR, then either copy the trace metadata while protected or keep the protection
active while materializing data that must be copied from trace-owned memory.

Repeated `m6_jit_util_flush_race` runs still expose an existing intermittent
failure on clean `HEAD` (worker userdata method lookup can fail on the second
repeat). Single-pass validation remains useful for this slice, but repeat
failures are tracked as pre-existing rather than introduced here.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m6_jit_util_flush_race`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m0_matrix`
