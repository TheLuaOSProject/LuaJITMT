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
root number, and selected exit. Each copy also binds the immediate parent's
generation separately. Both admission passes check every row independently,
require unique ancestry IDs distinct from the child, and bind the final row's
parent snapshot count to the canonical root while retaining the final root
link. Independent private-copy mutations cover skipped ancestry, foreign
root, stale parent and row generations, child-ID/cycle reuse, and a wrong
final root snapshot count; all must fail both passes.

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

The local no-observer timing probe was run three times per revision on the
same host, with 2,000 calls per phase. At checkpoint `570d5b942`, medians were
5.525, 5.669, 5.668, and 5.740 microseconds per call for root, direct,
nested, and depth-3 phases. With the current wide-depth fallback and ancestry
checks, medians were 5.165, 5.471, 5.600, and 5.700 microseconds. These noisy
samples show no measured regression on this probe; they are not a general
performance guarantee.

Retirement now validates each root-owned side list and walks newest-first.
Publication prepends each child, so this is descendant-before-ancestor order.
The traversal saves the successor before unlinking the current node. Deep
graphs use a 64-member indexed ancestry proof. Larger roots retain direct and
one-nested-side topology: the fallback checks each nested child's exact direct
parent later in the prepend-ordered list. New publication at or above the
index cap also checks the *prospective* child depth, so an existing depth-two
graph cannot admit its first depth-three member as child 65. The cap policy
tests exercise prospective depths 1–4 at counts 63, 64, and 65.
This fallback uses bounded suffix scans for membership, so it has quadratic
worst-case work for a wide list containing many nested children; it is only
used beyond the indexed cap and remains a performance point to measure. Depth
greater than two remains refused beyond the index cap. Focused admission
and depth-3 peer/GC tests pass after this retirement rewrite. The private graph-view test first accepts the live
descendant-before-ancestor order, then reorders the immediate ancestor ahead
of its child and requires the path validator to reject it. A cap-policy unit
check covers counts 63, 64, and 65: it accepts depth-one/two graphs at and
beyond the limit and rejects depth greater than two at/above the limit. An
ordinary ARM64 fixture also constructs a real 67-member root-owned graph with
maximum depth two: it publishes a direct side first, appends 65 distinct root
exits, then publishes a new nested child after the list is already oversized.
It verifies interpreter-equivalent results, preserves the validated summary
across full GC, and full-flushes before retracing. This exercises depth-two
admission beyond the cap and wide-list retirement.

## Limits and remaining tests

This is bounded depth-3 support, not unrestricted side-recorder parity. The
cap policy and live 67-member depth-two graph are covered, but a live 64-member
deep graph followed by a refused depth-three append is not constructed. The
reorder negative mutates a private captured index, not the live list; live-list
corruption is rejected by the full graph validator but has not been exercised
through retirement. The focused contract does not claim adversarial
retirement-race coverage. Both strict ARM64 and arm64e builds and the focused
native contract pass; the full multi-profile integration remains for the
integrating branch.

The source keeps a bounded iterative ancestry walk, no allocation or new lock,
and a root-owned flattened list. The indexed path rejects malformed chains,
missing or foreign ancestors, cycles, stale generations, and invalid root
identity. The oversized depth-two fallback verifies direct-parent membership
in prepend order. Retirement correctness depends on exact-once membership and
preserving that order; the private reorder mutation checks the validator's
descendant-before-ancestor requirement.
