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
