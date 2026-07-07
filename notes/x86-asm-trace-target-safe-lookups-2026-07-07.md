# x86 Assembler Trace Target Lookups

The x86/x64 assembler had two target-trace reads that still decoded public
trace slots directly:

- `asm_tail_link()` checked a `BC_JLOOP` target before using the target
  trace's `startins` pseudo-PC for interpreter-tail setup.
- `asm_tail_fixup()` loaded the final linked trace mcode pointer.

This slice validates those target slots with `traceref_safe()` under short GC2
SMR read sections before copying the needed target metadata. The existing
`&target->startins` embedded pseudo-PC remains a separate lifetime-design issue;
this change only removes the stale-slot read.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m0_matrix`
