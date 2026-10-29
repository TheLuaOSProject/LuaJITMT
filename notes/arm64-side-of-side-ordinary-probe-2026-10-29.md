# Ordinary side-of-side trace probe

`bench/jit-side-of-side-probe.lua` compares a candidate loop against an
independently interpreted copy through three phases: the root guard is warm,
the first side is recorded, then an independent guard inside that first side
is heated. It checks result equality, collects before each phase boundary,
counts exact `texit` trace/exit pairs, and reads `IR_BASE` to identify the
immediate recording parent. `traceinfo.link` and `linktype` are reported only
as final-link metadata and are not used as parentage evidence.

On x86_64/Rosetta, phase 2 produced root trace 1 and first side trace 4 with
`IR_BASE 1:1`; phase 3 produced trace 6 with `IR_BASE 4:1`, plus exits `4:1=1`
and `6:1=5`. On ARM64, phase 2 produced first side trace 2 with `IR_BASE 1:1`;
phase 3 produced no native child of trace 2 and observed `2:1=4747` exits.
Candidate and interpreted results matched in every phase on both targets.

The run used the checked-in probe with `LUA_PATH='src/?.lua;;'` and an
assert-enabled ordinary AppleClang build. Logs:

- `/Users/frityet/Scratch/ljmt-side-of-side-20261029/checked-in-probe/x64.tsv`
- `/Users/frityet/Scratch/ljmt-side-of-side-20261029/checked-in-probe/arm64.tsv`

This is a reproducible baseline witness, not evidence that ARM64 currently
supports side-of-side publication. The implementation must validate the
immediate parent from `IR_BASE` independently of the root-owned child list.

## Current implementation checkpoint

The bounded implementation now validates the immediate recorder parent from
the child `IR_BASE` while keeping the canonical root as the final native link
target and allocation-list owner. Root-list traversal accepts the supported
one-level nested topology only after checking each nested child's immediate
parent is itself a direct root child. The nested parent's selected exit must
still contain its exact authenticated fallback target before append; root
generation, parent certificate generation, source snapshot and the child
allocation identity are revalidated before publication. Retirement walks the
same flattened root list and validates each edge against its immediate parent.
Root exit maps use their existing `nexits == nsnap` geometry; nested side-parent
exit maps use the recorder's `nexits == nsnap + 1` geometry.

An assert-enabled AppleClang ARM64 rebuild and the checked-in ordinary probe
now pass without diagnostics. Phase 3 observes nested trace 3 with `IR_BASE
2:1`, a root final link to trace 1, one exact parent exit, and matching
candidate/interpreter results after collections. Evidence:

- `/Users/frityet/Scratch/ljmt-side-of-side-20261029/build-clean6.log`
- `/Users/frityet/Scratch/ljmt-side-of-side-20261029/checked-in-probe/arm64-clean.log`

This checkpoint covers a single nested level. It does not establish arbitrary
depth, side-of-side recording, or concurrent publication/retirement races.

Final focused validation on the cleaned source passed the complete first-side
contract with one run per profile: no-helper ARM64 smoke, ARM64 helper
`gc-claim`/`scoped`/`full-flush`, and arm64e helper `gc-claim`/`scoped`/
`full-flush`. The helper nested-side lifecycle fixture runs against a fresh
trace generation before the unrelated lifecycle fixtures, so prior DONE
hotcounts cannot mask its selected parent exit. Log:

- `/Users/frityet/Scratch/ljmt-side-of-side-20261029/first-side-contract-final.log`

The ordinary probe also passed after temporary diagnostics were removed. It
reported root 1, first side 2, nested child 3; the child has `IR_BASE 2:1` and
the canonical root final link to trace 1, with matching candidate/oracle
results after each collection:

- `/Users/frityet/Scratch/ljmt-side-of-side-20261029/ordinary-probe-final.log`

## Root-entry revalidation

Root entry also had to understand that the root's side list is flattened:
nested trace 3 is listed beside direct side 2, but `IR_BASE` names side 2.
Entry now checks each nested member against its immediate parent's sealed
certificate and selected exit, while direct-side checks allow only the exact
published nested mcode target for that exit. The root head/count and child
slot identities are reacquired through the existing jit-base lease; no new
locking, allocation, or blocking path was added.

The production fixture now repeats root entry after full GC and requires the
exact nested terminal exit (`trace 3, exit 1`) for the `(outer=2, inner=2)`
case. After scoped retirement of trace 3 and another collection, it requires
the surviving direct parent to report the same interpreter exit
(`trace 2, exit 1`) and confirms the root still enters natively. The focused
first-side contract passed no-helper ARM64 smoke, ARM64 helper lifecycle modes,
and arm64e helper lifecycle modes. The fixture's oracle comparisons and exact
trace/exit counters remain strict.

## Bounded certificate traversal review

A follow-up review moved the immediate parent's direct-root certificate checks
before recursive seal validation. This enforces the one-level ancestry bound
before following a certificate, including malformed cyclic views. The root
list walkers also bound IR extents before reading IR_BASE and check runnable
state before the entry walk dereferences a member. These checks add no lock,
allocation, or SMR reader to native entry. Focused root-entry and first-side
validation for this follow-up is recorded separately below when complete.

The certificate mutation fixture now also constructs a private copied trace,
private copied trace vector, and a self-parent certificate and requires
rejection without publishing any copied slot. The old branch's focused entry
run passed its root-entry C fixture but stopped in the older adaptation
fixture, whose lookup still insists that the root has exactly one child.
The current integration branch already has a multiple-child-aware lookup.
Validation will continue on that integration base, keeping this failure log
at `/Users/frityet/Scratch/ljmt-nested-reentry-integration-20261029/root-entry.log`.

## Active-peer cell-operation admission follow-up

The strengthened ordinary probe initially failed only while its live MT peer
was attached. A temporary trace diagnostic showed the nested parent was a
cell-operation prototype (`proto_cellops=1`) with active shared runtime
(`mtshared=1`), and `trace_hotside` was dropping every such nested attempt
before the nested snapshot proof. Removing that blanket veto produced nested
trace 3 from direct side 2 on ARM64, with the canonical root link still trace
1; 32 exact nested native exits were observed after full collection and
re-entry. The candidate uses ordinary local cell operations but has no
captured upvalues. A separate captured-local getter probe remained
unpublished, so captured-cell parity is not claimed.

The replacement is bounded to nested snapshots whose source prototype uses
cell operations. The exact parent map is checked for bounds and supported
NORESTORE forms; mapped `IRT_P32` SLOADs are refused as cell identities. The
existing source, semantic, and post-RA checks continue to bind CGET/CSET to
their rooted helpers. The focused production contract exercises a real
parent map plus independent private NORESTORE and mapped P32-source
mutations. The P32-source check first accepts a one-entry private view using
the original real map slot/ref and copied source IR, then changes that mapped
IR to `SLOAD/P32` and requires the full snapshot validator to reject it. The
contract requires bitmask 15; the ordinary peer fixture itself has no
captured cell. An earlier test-hook version copied the full IR and map before
mutating them; direct lifecycle runs sometimes segfaulted, while debugger
runs did not. That failure was not localized. The current test uses a bounded
private one-entry map plus copied source IR and passes the complete focused
contract.

Validation evidence:

- `/Users/frityet/Scratch/ljmt-nested-reentry-integration-20261029/ordinary/arm64-peer-final.tsv`
- `/Users/frityet/Scratch/ljmt-nested-reentry-integration-20261029/ordinary/first-side-cell-proof-contract-final.log`
- Earlier full-copy test-hook diagnostics are retained in `first-side-cell-proof-contract5.log` through `first-side-cell-proof-contract12.log` and `first-side-cell-proof-lldb*.log`; the full-copy failure remains unexplained.
- `/Users/frityet/Scratch/ljmt-nested-reentry-integration-20261029/ordinary/x64-peer-final-build.log`
- Current non-ARM compatibility build: AppleClang `-arch x86_64`, `-DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT`, built successfully after guarding the ARM64 certificate traversal.
- Current x86_64/Rosetta ordinary peer probe: `/Users/frityet/Scratch/ljmt-nested-reentry-integration-20261029/ordinary/x64-peer-final.tsv`; it observed nested trace 5 with immediate parent trace 3 and 32 native nested exits after GC. The probe rechecks candidate JFORL ownership, each parent/link relation, and exact mcode addresses after full collections and native re-entry.

The nested retirement walk uses depth-ordered certificate validation only on
ARM64, where `GCtrace` carries the ARM64 side certificate. Non-ARM targets
retain the prior generic one-pass scoped retirement behavior. This change
does not add locks or weaken the nested source/map proof.
