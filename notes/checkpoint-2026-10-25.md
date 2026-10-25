# Post-refresh ARM64 validation (2026-10-25)

The unmodified full bootstrap gate passed on frozen `608e5d7e`. The gate used
`MACOSX_DEPLOYMENT_TARGET=13.0 JOBS=4` and the assertion-enabled ARM64
interpreter profile. No architecture branch or runtime fixture was omitted.

It exercised the original-bytecode C fixture, all 387 applicable stock cases,
TG-local root/safepoint/tmpbuf and C API checks, Darwin native-root completion
and hold tests, threading/hooks/coroutines, shared upvalues/cells, four-thread
callback rounds, all six native and arm64e/BTI callback lifecycle fixtures,
aggregate/variadic FFI, metadata waits, automatic-GC errno preservation, and
ARM64/arm64e/x86-64 OS-error unwinding. Both ARM64 interpreter GC2 hard-assist
branches ran. The x86-64 unwind executable ran under Rosetta with an explicit
local ad hoc signature.

The snapshot includes these coherent implementation checkpoints:

- `8a2bd465`: assertion-enabled callback lifecycle and C++ exception coverage.
- `46565580`: Apple variadic stack slots and internal VM errno preservation.
- `752328aa`: terminal sweep-close versus late table-rescan publication.
- `608e5d7e`: explicit local signing for Rosetta CI fixtures.

The experimental gate on that same frozen source passed its initial native
emitter, FFI, scalar, spill and lifecycle contracts, then stopped at a source
assertion in the side-ingress audit. That audit expected the child-side marker
inside the parent-root validator, which checks the exact integer-loop marker.
The audit now checks `v->admission != TRACE_ARM64_INT_LOOP_ADMITTED` explicitly;
its ARM64 runtime contract and arm64e compilation both pass. The existing
runtime fixture separately rejects the side marker and mixed admission flags.
A complete rerun remains required; the partial run is not a full experimental
pass.

A diagnostic continuation passed B26 and sealed-root publication, then exposed
another stale static audit in `arm64_jit_side_asm_consumption_contract.sh`.
That audit searched `trace_save()` for debugger/perf publication hooks which
the source places in `trace_stop()`. It now retains the complete compaction,
root, slot, and GC-publication ordering checks inside `trace_save()`, and
separately requires its call in `trace_stop()` to precede both debugger/perf
hooks. The corrected complete focused contract passed both ARM64 and arm64e
branches and restored the ordinary ARM64 build. No runtime change was needed.

Temporary full bootstrap log:
`/tmp/luajitmt-20261025-bootstrap-final.log`.
Partial experimental log:
`/tmp/luajitmt-20261025-experimental-baseline.log`.
Corrected side-ingress contract log:
`/tmp/luajitmt-20261025-side-ingress-recheck.log`.

Details: [FFI varargs/errno](arm64-ffi-varargs-errno-2026-10-25.md),
[GC2 table-rescan close](gc2-idle-table-rescan-close-2026-10-25.md),
[callback lifecycle](arm64-ffi-callback-lifecycle-2026-10-25.md), and
[Rosetta local signing](rosetta-validation-signing-2026-10-25.md).
