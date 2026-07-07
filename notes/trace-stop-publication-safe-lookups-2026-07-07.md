# Trace Stop Publication Lookups

`trace_stop()` publishes side and stitched traces while the active recorder owns
the JIT token. The parent/root traces are public trace-slot entries, so the
publication path should still validate slot contents before copying or mutating
their metadata.

This slice replaces the remaining raw `traceref()` calls in `trace_stop()` with
short GC2 SMR read sections:

- Before `trace_save()`, side traces copy the current root side-chain head from
  a validated root body.
- After `trace_save()`, side-trace parent/root metadata is revalidated before
  updating snapshots, side links, child counts, and the parent exit target.
- Stitched trace links revalidate the parent trace before writing `link`.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m0_matrix`
