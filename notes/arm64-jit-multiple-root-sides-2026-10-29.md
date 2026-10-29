# Multiple root-linked side traces on ARM64

The bounded scalar-side recorder now supports a root loop with more than one
root-linked side trace. Each child keeps its own sealed allocation identity,
root generation, selected parent exit, and authenticated native target. The
root-owned sibling list is mutable topology, so it is checked separately from
the immutable child certificate.

Root view and native-entry validation walk exactly the advertised child count,
bound it by the trace vector, reject repeated/invalid IDs and nonterminal tails,
and validate every child's certificate and `IR_BASE` parent/exit tuple. A
selected exit must still target its matching child. Side-of-side traces remain
outside this extension. Publication prepends a new child and updates the count
and head before making its parent exit runnable. Retirement first redirects
the selected parent exit to its authenticated fallback, then removes that
child from the root list and clears its sibling link. Readers reject the
temporary count/list mismatch during either transaction.

The ordinary x64 witness is
`/Users/frityet/Scratch/ljmt-multiside-20261029/two-independent-guards-x64-final.log`.
It heats two distinct parent exits from independent guards and observes two
children linked directly to the same loop root. On ARM64, the production
smoke confirms one root with two direct children; the helper contract removes
the head and then the tail in separate recreated cases, checks that the other
child and its exit target remain live, forces full GC, re-enters the retained
child, and finally flushes both roots.

Retirement planning requires the requested child to occur exactly once in the
validated chain. A test-only negative detaches the target head while retaining
the other sibling as a valid one-element chain, verifies the retirement plan
rejects the absent target, and restores the original topology. The two-child
entry check also runs after full GC and requires successful root entry before
either child is retired.

A separate three-guard loop creates three children at distinct root exits. The
helper retires the middle child, checks the predecessor and successor links
and both native exit targets, forces full GC, and natively re-enters both
survivors before flushing the root.

Validation command:

```sh
LJ_ARM64_FIRST_SIDE_PRODUCTION_RUNS=1 \
LJ_ARM64_FIRST_SIDE_PRODUCTION_PAUTH_RUNS=1 \
JOBS=2 MAKE_JOBS=2 tools/ci/arm64_jit_first_side_production_contract.sh
```

The contract passed the ARM64 production smoke, ARM64 helper GC-claim/scoped/
full-flush cases, ARM64e helper GC-claim/scoped/full-flush cases, and restored
ARM64 helper profile with both the absent-child negative and three-child
middle-retirement fixture. Output is preserved at
`/Users/frityet/Scratch/ljmt-multiside-20261029/three-sibling-middle-retire-contract.log`.
The list proof is count-bounded and does not impose a two- or three-child
production limit.
