# AArch64 exact first-side production canary (2026-08-28)

## Scope

The ordinary macOS AArch64 experimental build now records, publishes, enters
and retires the existing exact first-level side-trace grammar without either
test seam. This is the first production side path in the port.

The opening is deliberately granular:

- `LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED` remains `1`;
- ordinary arm64 and arm64e builds set
  `LJ_ARM64_JIT_FIRST_SIDE_RECORDER_FAIL_CLOSED` to `0`;
- the assembler-only and one-shot publication fixtures set the granular gate
  back to `1`; and
- GDBJIT and PERFTOOLS builds also keep it at `1`, because the finite
  transaction does not yet integrate deferred registration callbacks.

Unsupported first-side shapes, side-of-side recording, TRACE callbacks and
the generic ARM64 side publication path remain closed.

## Admitted generation

The canary reuses the already-proved certificate rather than adding another
grammar. Trace numbers are dynamic, but the generation itself is still exact:

- a runnable admitted integer `BC_LOOP` root with no existing child;
- selected parent exit 2, continuation bytecode offset 13 and local slot 4;
- a two-parameter, frame-size-5, 19-bytecode prototype;
- the exact inherited integer `CGET`/add/limit/guard/XPOLL child IR;
- five child snapshots, 17 map entries and the existing fixed register map;
- the authenticated parent fallback word and child entry; and
- a first child whose root/link are the parent, with no sibling or descendant.

The root metadata and continuation are captured twice under a nonwaiting SMR
reader. The same certificate is checked at three temporal boundaries:

1. `IDLE`, before any selected snapshot-count mutation;
2. `CLAIM`, after the exact TG acquires the recorder token but before owner and
   selector publication or the terminal count CAS; and
3. `OWNER`, on every START/RECORD dispatch through `lj_trace_ins`.

A failed checkpoint releases the token before the SMR lease and leaves the
parent fallback, topology and terminal state untouched. A focused STOPREQ
injection after token acquisition proves that cleanup dynamically, including
unchanged snapshot count and zero leaked token, owner, reader or parent
certificate.

## Publication and execution

Every ARM64 `BC_JMP` trace stop now has exactly two outcomes: the complete
certificate enters `trace_stop_arm64_first_side`, or recording raises RETRY.
The generic side publisher is unreachable on ARM64.

The finite transaction retains the earlier callback-absence reservation,
compact-body construction, sealed GC-root publication and nonwaiting SMR
checks. Its irreversible suffix publishes the child slot, GC checkpoint,
parent topslot/topology, terminal snapshot count and finally the authenticated
parent-to-child edge. It now also performs the ordinary completed-trace GC
pressure bookkeeping.

The production fixture warms a decoy root, discovers two independent
root/child pairs at dynamic trace numbers, enters both children natively and
checks that each exits as the child identity. It compares raw authenticated
exit-table representations with `memcpy`-to-`uintptr_t`, verifies BTI and the
body-discriminated `mcauth` entry on arm64e, and proves that repeated child
exits do not record side-of-side traces.

An otherwise similar loop whose taken branch adds 2 is rejected as an
unsupported first side: its root remains runnable, the selected edge remains
fallback, and no child/topology publication occurs.

## Retirement coverage

The same ordinary production children are exercised through all existing
exact inverse routes:

- GC claim, including idempotent repeated claim;
- scoped child flush, preserving the runnable parent; and
- full flush of the decoy, both parent/child pairs and the unsupported root.

Each route restores the authenticated parent fallback, collapses topology,
preserves the required retired-body identity through the grace interval and
leaves the recorder token and SMR reader count at zero.

## Validation

`tools/ci/arm64_jit_first_side_production_contract.sh` passed on this Apple
Silicon host with:

- a no-helper ordinary arm64 build and embedded-Lua smoke test;
- helper arm64 GC-claim, scoped-flush and full-flush runs;
- helper arm64e/BTI/PAUTH versions of the same three runs;
- strict fixture compilation with `-Wall -Wextra -Werror`;
- arm64 and arm64e preprocessing proofs that GDBJIT and PERFTOOLS close the
  granular gate; and
- restoration of the ordinary experimental helper build afterward.

The metadata contract additionally covers valid and rejected CLAIM states and
pins SMR/IDLE/count/token/reload/CLAIM/terminal-count/owner ordering. The
recorder safepoint contract independently pins the same mutation boundaries
and cleanup order. The only focused build diagnostic was the pre-existing
unused `ccall_rawchild_wait` warning.

## Next step

Generalize one certificate dimension at a time while retaining the same
three-stage admission and finite publish/retire transactions. A safe order is
dynamic selected exit, prototype/frame/slot shape, child snapshots/constants,
finite integer operations, register allocation, multiple inherited values and
spills. Siblings and side-of-side should remain closed until nested snapshot
and topology transactions have equivalent execution, race, flush and arm64e
coverage.
