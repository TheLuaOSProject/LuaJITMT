# Post-refresh ARM64 validation (2026-10-25)

Latest complete validation is frozen
`d8f02a81081ac6f678279a2dd02e8a959f620ad2`: the full experimental gate, both
509-case native ARM64 stock source/import corpora, and then the full bootstrap
gate all passed sequentially. The bootstrap gate includes 387 stock cases.
No default architecture branch was omitted. This validates the rooted numeric
read from `0d9475d8`, the duplicate entry-scan removal from `61801fae`, and the
nested M6 caller-profile restoration fix in `d8f02a81` together.

Logs: `/tmp/luajitmt-20261025-reviewed-experimental.log`,
`/tmp/luajitmt-20261025-reviewed-stock-source.log`,
`/tmp/luajitmt-20261025-reviewed-stock-import.log`, and
`/tmp/luajitmt-20261025-reviewed-bootstrap.log`.

The implementation and architecture review, including measured entry cost and
remaining coverage limits, is in
[`implementation-review-2026-10-25.md`](implementation-review-2026-10-25.md).
The earlier checkpoints below retain their original evidence and limits.

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

The remaining diagnostic continuation then passed, including first-side
production, GDB-JIT preparation, VM iteration, callback-result lifetime,
numeric/FORL profiles, FFI CALLXS lifecycle, root entry, live flush/reuse, and
source/import loop cells. These segmented diagnostics identify no further
baseline failure; they do not replace the required single complete gate.

The rooted table-length capability is now integrated. Its dedicated native
ARM64 and arm64e/BTI contract and both registered M5/M6 cases pass against the
combined source. See
[`arm64-jit-rooted-tablen-capability.md`](arm64-jit-rooted-tablen-capability.md)
for the bounded admission and lifecycle evidence.

The x86-64 focused GC2 fixture exposed a separate nested-universe hotcount
handshake stall: the physical actor owned the second universe's main TG while
raw TLS intentionally retained the first universe's TG. Both reset-hotcount
owner checks now use the existing universe-aware, actor-checked TG lookup.
The regression passes on x86-64/Rosetta and ARM64 experimental builds, with
the safe interpreter fixture retained; the foreign-actor rejection still
passes. Both x86-64 509-case source/import suites also pass after the fix. See
[`x64-shared-runtime-regression-2026-10-25.md`](x64-shared-runtime-regression-2026-10-25.md).

The complete bootstrap gate passed again on frozen `a549d537`, including the
registered shared errno suite and all default architecture branches. The log
is `/tmp/luajitmt-20261025-bootstrap-current.log`. Both native ARM64 stock
corpora on frozen `1a67d2ea` passed all 509 cases after rooted-length admission
was added (`/tmp/luajitmt-20261025-arm64-stock-{source,import}.log`).

The complete experimental attempt on `1a67d2ea` exposed the static audit's
old blanket scalar-call prohibition. The updated audit retains rejection of
CALLN/CALLA/CALLL/CALLXS and separately verifies the exact rooted-length
CALLS/root/retry relationship in both passes. Its existing table-typed SLOAD
negative now expects rejection at the invalid numeric use; that SLOAD type
can validly occur only with the rooted helper graph. The focused IR contract
and the separate native ARM64/arm64e rooted-length contract both pass.

The single complete experimental gate subsequently passed on frozen
`d8f6ab685b697d8410dc36c7c4cd2ff29d90a6e4`, with its default ARM64,
arm64e/BTI, and x86-64 branches intact. Both 509-case native ARM64 stock
source/import corpora then passed on the same tree. This supersedes the
segmented experimental diagnostics above. Logs:

- `/tmp/luajitmt-20261025-experimental-current.log`
- `/tmp/luajitmt-20261025-arm64-current-stock-source.log`
- `/tmp/luajitmt-20261025-arm64-current-stock-import.log`

These results certify the currently admitted capabilities. General ARM64
JIT heap operations and unrestricted FFI signatures remain closed.

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
