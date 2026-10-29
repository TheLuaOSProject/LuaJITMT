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

## Root-entry fixture capacity repair

The full gate reached `t-arm64-jit-native-forl.c` and failed its full-word
race's assumption that the previously saved root remained live in slot 1.
A diagnostic linked to the preserved archive found slot 1 empty, proto root
list zero, and the source word correctly restored to FORL. The fixture uses
`maxtrace=2` and `hotexit=1`; newly admitted child recording exhausts that tiny
cache during its intentional accumulator-overflow exits and causes ordinary
whole-cache retirement. The stale fixture pointer was not a retained lease.

A diagnostic with only `maxtrace` raised from 2 to 128 passes every original
assertion against the same saved archive, retaining `hotexit=1`. This isolates
the capacity effect. The fixture now uses that larger capacity and keeps side
recording plus its exact slot-1 generation race and explicit flush/reuse
assertions. Native child publication and overflow replay also retain their
separate dedicated contracts. Production code is unchanged by this repair.
The original log is `full-gate.log`. Preserved source/runtime/archive, original
fixture/script, diagnostic source/executable and output are in
`native-forl-failure/` under the integration evidence directory. The original
contract executable was already removed by cleanup; the diagnostic executable
was relinked against the saved archive. A focused rerun and continuation are
required before this checkpoint can claim a complete gate pass.

The repaired native-FORL contract passed its complete ARM64/arm64e runs,
including the ordinary and MCODE placements, varied-loop companion, generation
races and signed-step edges. The final log is
`native-forl-capacity-repaired.log`. `native-forl-repaired.log` preserves an
earlier alternative that suppressed side recording; the committed correction
instead retains `hotexit=1` and gives the test sufficient trace capacity.

The continuation at `a5513ad15` completed successfully. `resume-gate.log` ends
with the gate's final OK marker; `resume-gate-source.txt` records its source.
Together, the original prefix, focused repaired native-FORL contract, and the
continuation cover the complete integrated gate. This is a resumed run after
a test-only repair, not an uninterrupted pass. Production source remained
`5cec5847d` throughout. The continuation includes the new six-range FORL
boundary-side contract, the later lifecycle contracts, M7, and the final
runtime/safepoint checks. No whole-fork parity claim is implied.
