# ARM64 deeper side-trace depth audit

## Question and probe

This audit asks whether ordinary LuaJIT records a chain deeper than the
currently supported root → direct side → nested side. It changes no production
code. `bench/jit-side-depth-probe.lua` uses four independent numeric conditions
inside a numeric loop. The phases flip one condition at a time:

1. `(a,b,c,d)=(1,1,1,1)` warms the candidate root.
2. `(2,1,1,1)` selects the root's cold `a` path.
3. `(2,2,1,1)` selects the next cold `b` path.
4. `(2,2,2,1)` selects the next cold `c` path.
5. After full GC, `(2,2,2,2)` changes the deepest `d` guard to force a cold
   exit from the deepest retained trace.

The oracle is a separate interpreted copy. A channel-waiting MT peer is live
from before warmup through the final checks. The probe discovers the exact root
from the candidate's `JFORL`, reads each trace's `IR_BASE` `(parent, exit)` to
construct ancestry, and independently checks the final `linktype/link`. It
captures `util.tracemc` addresses and verifies the same root and child code
identities after collections and post-GC execution. Once the deepest graph is
present it raises `hotexit` to avoid recording additional traces while testing
the deepest cold edge; this changes recording frequency only.

## Evidence

The assert-enabled x86_64/Rosetta ordinary build produced a three-side chain:
It was built with `TARGET_FLAGS=-arch x86_64`,
`XCFLAGS='-DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'`, and
`MACOSX_DEPLOYMENT_TARGET=13.0`.

```text
root 1, loop link 1
trace 3: IR_BASE=(1, exit 1), final root link 1
trace 5: IR_BASE=(3, exit 1), final root link 1
trace 7: IR_BASE=(5, exit 1), final root link 1
```

All four phase results matched the interpreted oracle. After full GC, the
unchanged mcode identities remained reachable and the selected deepest cold
condition produced 1,504 `texit` events from trace 7, exit 1. The peer remained
attached through this run. Full run:

- `/Users/frityet/Scratch/ljmt-side-depth-audit-20261029/x64-final.log`

The assert-enabled ARM64 ordinary build produced two side levels:
It was built with `TARGET_FLAGS=-arch arm64`,
`XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL
-DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'`, and deployment target 13.0.

```text
root 1, loop link 1
trace 2: IR_BASE=(1, exit 1), final root link 1
trace 3: IR_BASE=(2, exit 1), final root link 1
```

Trace 3's code identity survived full GC and the peer remained attached. Under
`(2,2,2,1)`, no child of trace 3 appeared; that phase accumulated 4,700 exits
from trace 3, exit 1. A post-GC selected-path call and final reentry accumulated
47 and 1,457 more exits from that same trace/exit respectively, while results
still matched the interpreted oracle. Thus this source shape shows an actual
one-level discrepancy in maximum native ancestry (three side levels on x64,
two on ARM64), rather than merely a difference in aggregate exit counts.

- `/Users/frityet/Scratch/ljmt-side-depth-audit-20261029/arm64-run1.log`

Both logs print process-local mcode addresses. The assertions compare those
addresses within each process across GC; addresses should not be compared
between processes. The probe reports ordinary interpreter-equivalent behavior
and sampled trace topology, not unrestricted side-recorder parity.

## Implementation boundary and ranked generalization plan

The current C path keeps nested descendants in the root-owned flattened
`nextside` list. Each descendant's `IR_BASE` records its immediate parent and
selected parent exit, while the trace's final link remains the canonical root.
The root-entry walker bounds the list by `nchild`, detects malformed list
termination, and checks each member's certificate. Its ancestry checks accept
direct children and one nested level: a nested trace must point to a direct
root child, and that intermediate child's own `IR_BASE` must point to the root.
The nested-side source view likewise requires its immediate parent to point
directly to the root. The sealed certificate stores one parent pointer/number,
one parent generation, and one selected exit; validation branches between a
root parent and a single side parent. These are deliberate fail-closed depth
bounds visible in `trace_arm64_root_direct_side_present`,
`trace_arm64_first_side_view_acq_mode`, `trace_arm64_nested_side_view_acq`,
and `trace_arm64_side_certificate_sealed_valid_held`.

Ranked plan before any production widening:

1. **Generalize the parent-chain verifier, not admission.** Starting from each
   list member's exact `IR_BASE`, iteratively follow `(parent, exit)` through
   sealed certificates until the unique root is reached. Bound path length by
   the validated root `nchild`; reject repeated trace numbers, self-edges,
   missing/non-runnable parents, mismatched `IR_BASE.op2`, and any chain member
   that is not owned by the same root list. Recheck exact parent generation,
   source continuation, selected snapshot, exit target and mcode identity at
   every edge. Keep semantic/post-RA source admission unchanged.
2. **Preserve root ownership and make retirement ancestry-aware.** Descendants
   should continue to append to one root-owned list; do not give a side trace a
   separate mutable child list. Retirement must prove exact-once membership
   and preserve predecessor/successor, reject cycles/duplicate exits, and
   retire or reject descendants before retiring an ancestor whose certificate
   they reference. Exercise middle-of-list and ancestor retirement with
   survivors at multiple depths.
3. **Measure a bounded entry algorithm before selecting its representation.**
   Root entry currently validates the flattened list on entry. A naive
   per-member ancestry walk plus repeated list-membership scans can grow toward
   quadratic or cubic work as both depth and child count grow. First measure
   realistic `maxtrace`/list sizes. Prefer a bounded iterative walk with no
   allocation, lock, recursion proportional to untrusted metadata, or new
   mutable shared cache. If efficient exact-once cycle detection cannot be
   shown with existing root-generation/list invariants, retain a documented
   depth cap and fail closed rather than opening arbitrary cycles.

Any implementation must add private mutations for a skipped/altered ancestor,
wrong ancestor generation, wrong parent exit, duplicate/cycle, missing list
member, stale mcode identity, and retirement of an ancestor with live
descendants. Repeat the ordinary peer/GC/native-edge witness before claiming
parity beyond this sampled depth.
