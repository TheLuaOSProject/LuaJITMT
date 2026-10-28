# Scalar function-entry integration checkpoint

Production source `d9352591a` combines the captured/fixed-result Lua call
checkpoint with scalar FUNCF numeric literals and the shared tracesnap cell-PC
fix. Ordinary ARM64 and signed x86_64 assertion builds each pass both stock
corpora (509 source tests and 509 imported-bytecode tests), the strengthened
tracesnap getpos C fixture, the topology probe and all 260 inventory comparisons.
Standalone `lj_trace.c` and `lj_asm.c` builds also pass `-Wall -Wextra -Werror`
for ARM64 and arm64e with assertions, experimental JIT and GDBJIT enabled.

The FUNCF-specific assertion contract separately passes ARM64 and arm64e with
default and randomized machine-code placement, native execution, an attached
peer, GC/reuse and malformed semantic/allocated operands. See the
[literal proof and scope](arm64-funcf-numeric-literals-2026-10-28.md).
The last complete experimental gate and four combined M7 FFI profiles remain
at `3e3f3432a`; they have not been rerun on this newer source.

## Interpreter-oracle correction

The inventory and topology probes previously disabled only the returned
oracle runner and its nested prototypes. A captured helper created as the
loader's sibling prototype could still compile. This became visible when the
new scalar FUNCF support produced two child roots in the nonleaf topology row.
Both probes now disable the loader's complete prototype tree before creating
the oracle closures, and retain the returned-runner disable as well.

Both corrected probes pass on immutable ordinary `d9352591a` ARM64 and x86_64
executables with their matching module bundles. The inventory remains 17/20
ARM64 selected-root witnessed rows versus 18/20 x64, with 260 comparisons per
target. The ARM64 nonleaf topology row now has one child root and 40 child
exits, rather than two roots and 80 exits. Its selected outer function still
has no root. The hot scalar function has a patched FUNCF root and 81 observed
type-mismatch exits after collection. The branching-loop sample still shows
an x64 root-linked side and no ARM64 side in this run.

Prior selected-candidate-root witness counts remain evidence of native
execution: they matched the candidate's patched header at event delivery.
Prior claims that every reference helper was interpreted were too broad;
the corrected runs replace those claims for this checkpoint. None of these
samples establish full-body compilation or x86_64 parity.

## Evidence

`/Users/frityet/Scratch/ljmt-funcf-integrated-20261028/` contains:

- `check.sh`, successful `check.log`, `tested-source.txt`, build logs and
  both targets' source/import stock logs and getpos fixture logs.
- Immutable `arm64/` and `x86_64/` runtime/module bundles, with binary hashes.
- `*-topology-interpreted-oracle.tsv` and
  `*-inventory-interpreted-oracle.tsv`, from the corrected benchmark source.
- The four strict compiler objects, one per translation unit/architecture.

The initial driver stopped while packaging a nonexistent `src/threading.lua`;
`check-packaging-failure.log` retains that failed attempt. The corrected driver
uses `etc/threading.lua` and completed successfully. Main remains at
`49523abd` while the earlier intermittent host TDUP crash is unresolved.
