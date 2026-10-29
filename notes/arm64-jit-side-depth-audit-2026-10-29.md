# ARM64 deeper side-trace ancestry

## Probe and initial gap

`bench/jit-side-depth-probe.lua` uses four independent numeric conditions
inside a numeric loop. Its phases warm the root, then select three successive
cold paths. A separate interpreted copy is the oracle; a channel-waiting MT
peer remains attached. The probe identifies the candidate root from its
`JFORL`, follows each side's `IR_BASE` `(parent, exit)`, checks final
`linktype/link`, and records `util.tracemc` identities across full GC and
reentry.

The initial assert-enabled x86_64/Rosetta run produced three side levels while
the initial ARM64 run produced two. The original logs are
`/Users/frityet/Scratch/ljmt-side-depth-audit-20261029/x64-final.log` and
`/Users/frityet/Scratch/ljmt-side-depth-audit-20261029/arm64-run1.log`.
That initial difference was a real source/ancestry admission gap, not an
aggregate exit-count inference.

## Implementation and current evidence

The copied semantic and post-RA side views now carry an immutable ancestry
chain with each row binding the side generation, immediate parent generation
and snapshot count, exact `IR_BASE`, trace number, parent number, canonical
root number, and selected exit. Both admission passes check every row
independently while retaining the final canonical root link. Private mutations
for skipped ancestry, a foreign root, and a stale parent generation must fail
in both passes.

The ARM64 ordinary probe now reaches the same chain shape as x64:

```text
root 1
trace 2: IR_BASE=(1, exit 1)
trace 3: IR_BASE=(2, exit 1)
trace 4: IR_BASE=(3, exit 1)
```

The peer remained attached. All phase results matched the interpreted oracle;
trace/mcode identities remained stable through full GC; and the selected
deepest side produced native exits after GC and reentry. The probe's final
reentry observed 3,519 deepest-side native exits. The focused native contract
also passed:

```text
tools/ci/arm64_jit_side_ir_admission_contract.sh
ARM64 scalar side adaptation: PASS
arm64_jit_side_ir_admission_contract OK
```

The local no-observer timing sample was about 7.0, 6.4, 5.7, and 5.6
microseconds per call for root, direct, nested, and depth-3 phases. This is a
single-run sanity sample, not a comparative performance claim.

Retirement now validates each root-owned side list and walks newest-first.
Publication prepends each child, so this is descendant-before-ancestor order.
The traversal saves the successor before unlinking the current node. Deep
graphs are capped at 64 members; larger roots are accepted only when every
member is a direct child. At a 64-member list, only another direct child may
extend the graph; even a depth-2 graph refuses further extension so it cannot
cross into the oversized direct-only path. Focused admission and depth-3
peer/GC tests pass after this retirement rewrite. The private graph-view test first accepts the live
descendant-before-ancestor order, then reorders the immediate ancestor ahead
of its child and requires the path validator to reject it. A cap-policy unit
check covers counts 63, 64, and 65: it accepts shallow graphs at and beyond
the limit and rejects depth greater than two at/above the limit.

## Limits and remaining tests

This is bounded depth-3 support, not unrestricted side-recorder parity. The
cap test exercises the policy predicate rather than constructing a live
64-member trace graph; a real 64-to-65 append fixture and wide-shallow
lifecycle test remain outstanding. The reorder negative mutates a private
captured index, not the live list; live-list corruption is rejected by the
existing full graph validator but has not been exercised through retirement.
The focused contract does not claim adversarial retirement-race coverage. Full
multi-profile integration is also still required before integration.

The source keeps a bounded iterative ancestry walk, no allocation or new lock,
and a root-owned flattened list. It rejects malformed chains, missing or
foreign ancestors, cycles, stale generations, and invalid root identity.
Retirement correctness additionally depends on exact-once membership and
preserving the list's prepend order; tests for those limits remain to be
added before this checkpoint is ready to integrate.
