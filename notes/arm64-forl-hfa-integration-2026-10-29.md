# FORL sibling and HFA integration checkpoint

Production source `5cec5847d` combines the independent and constant-stop FORL
children, two-result FUNCF roots, float/double HFA pairs, protected recorder
flush deferral, and corrected combined-mask test seams. All commits are pushed
on `codex/arm64-parity`. The preceding full gate passed at `42f751357`; this
new checkpoint has its own ordinary comparison below.

Ordinary builds without test helpers pass on ARM64 and x64/Rosetta:

- 509 stock source tests and the 509-case stock-bytecode import corpus.
- Snapshot-PC and vararg companion checks.
- The full topology probe, inventory and aggregate result/error comparisons.
- Strict ARM64/arm64e GDBJIT trace and assembler compilation.

The ARM64 independent two/three-root-side rows now publish native children
instead of leaving the root on every changed-branch iteration. The ordinary
probe still reports unsupported custom ITERL roots; it does not claim identical
trace coverage for every row. The constant-stop and six-range native/mutation
fixtures have separate five-profile evidence in the FORL note.

Runtime/archive/module/DSO bundles, source probes, source ID, hashes and logs
are in `/Users/frityet/Scratch/ljmt-forl-hfa-combined-20261029/ordinary/`.
`check.log` ends with `Combined ordinary matrix passed` and the matching
source manifest is `tested-source.txt`. A new full integrated gate is the next
validation step. Whole-fork x64 parity remains unfinished: custom iterators,
nested side recording, tail-return roots and larger FFI shapes are active work.
