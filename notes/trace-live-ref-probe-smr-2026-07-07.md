# Trace Live Reference Probe SMR

Recorder and assembler helpers that check whether an existing trace is still
live used raw `traceref()` and then read the trace body with
`trace_runnable_acq()`. A concurrent trace flush can retire the slot body while
those probes are racing the slot read.

This slice changes:

- `rec_traceref_live()`;
- `asm_traceref_live()`.

Both helpers now enter a GC2 SMR read section, load the slot with
`traceref_safe()`, validate runnability, and leave SMR before returning the
trace pointer. The returned pointer follows the existing JIT-token lifetime
contract; the SMR section covers the racy slot/body probe itself.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m6_jit_util_flush_race`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `tools/ci/lua_test.sh m0_matrix`
