# Trace Unwind Exit Lookup SMR

`lj_trace_unwind()` can be entered by platform unwinders while the VM is
resolving a JIT frame back to a side-exit stub. That path was scanning the
public trace vector and then reading trace mcode/snapshot metadata without
holding an SMR read section, even though concurrent trace flush/reclaim can
retire the vector and the trace body.

This slice tightens the exit lookup path:

- `trace_exit_find()` now decodes slots with `traceref_safe()` and only matches
  trace bodies that still satisfy `trace_exit_body_match()`;
- the finder returns the matched body so the caller does not need to re-read the
  slot immediately after resolving the PC;
- `lj_trace_unwind()` holds an SMR reader while it scans trace slots, reads
  mcode bounds, searches snapshots, and derives the target exit stub;
- the trace-exit slow path uses safe trace-slot reads for the resolved parent,
  stack-check parent, and stale `JLOOP` target validation.

This keeps stale-but-public retired trace bodies available for exit restore and
unwind lookup until the reader leaves, matching the existing trace flush SMR
lifetime rule.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m6_jit_util_flush_race`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `LUA_PATH='tests/lib/?.lua;src/?.lua;src/jit/?.lua;;' src/luajit -e '...'`
- `tools/ci/lua_test.sh m0_matrix`
