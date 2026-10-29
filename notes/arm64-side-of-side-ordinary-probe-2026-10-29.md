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
