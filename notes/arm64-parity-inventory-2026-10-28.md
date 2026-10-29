# Cross-architecture JIT workload inventory

`bench/jit-parity.lua` runs identical workloads against independently loaded,
recursively JIT-disabled interpreter oracles. An attached peer remains live
throughout each run. Each workload has twelve calls with alternating bounds
and one call after full GC. The original eighteen workloads produce 234 result
checks; the current twenty-workload harness adds two dynamic-string rows for
260 checks.

The current harness disables the loader's entire prototype tree before
creating oracle closures. Earlier revisions disabled only the returned
runner, which missed captured sibling helper prototypes. Historical native
candidate witnesses remain valid, but those older reference calls were not
guaranteed wholly interpreted. The corrected `d9352591a` runtime comparison
below supersedes that oracle claim.

The `texit` observer matches the exiting trace number against the selected
runner's currently patched JFORL/JLOOP/JITERL bytecode while the event is being
delivered. This avoids attributing a retired trace number to a later trace.
It excludes separately compiled nested callees from the selected-root count.
No TRACE observer is installed, since that suppresses some ARM64 side
publication. Instrumentation changes execution and timing; these results are
not performance measurements.

Run from a build's root:

```sh
LUA_PATH='src/?.lua;;' src/luajit bench/jit-parity.lua
```

An optional case name selects one workload. Rows report architecture, result
check count, live selected-root trace count at the end, and observed exits from
selected roots during the row. A positive exit count proves some native root
execution; it does not prove that the complete workload body was compiled or
that every call used generated code. Zero means no witness in this sample,
not proof that the architecture cannot execute the workload natively.

## Initial observations

The ordinary ARM64 assertion executable from `d0b88101` and a freshly built,
ad-hoc-signed x64/Rosetta assertion executable from the same source each pass
all 234 result comparisons. ARM64 has selected-root exit witnesses in seven
rows; x64 has witnesses in sixteen. The pending primitive-leaf change adds an
eighth ARM64 witnessed row with all result checks still passing.

| Workload | ARM64 baseline | ARM64 primitive leaves | x64 baseline |
| --- | --- | --- | --- |
| Numeric for/while, arithmetic leaf, table read/write, upvalue read, bit rotate | witnessed | witnessed | witnessed |
| Primitive leaf | no witness | witnessed | witnessed |
| Branching/nested leaf, varargs, multiple results | no witness | no witness | witnessed |
| Math floor/sqrt, string length/byte | no witness | no witness | witnessed |
| Table iterator, I64 arithmetic | no witness | no witness | no witness |

The last row is inconclusive on both architectures; it does not establish
equivalence or remove iterator/I64 coverage from the full parity goal. The
other differences identify concrete workloads for subsequent native tests.
General calls, trace topology, effects, ABI coverage, concurrency, and unwind
still need their independent contracts and broader comparisons.

The original string rows use literals and can fold away the string operation.
In particular, the x64 recorder folds `string.byte('abc', 2)` to `98`; its root
exit witness does not establish a native byte load. Two additional rows now
pass the string as a runner argument: `string-length-dynamic` and
`string-byte-dynamic`. Each passes thirteen interpreter comparisons and has a
selected-root exit witness on the unchanged baseline x64 executable. The
focused operation fixtures separately require the actual string IR and native
entry counters; the inventory remains a workload observation, not an operation
certificate. The original rows are retained for historical comparisons.

Evidence is under `/Users/frityet/Scratch/ljmt-parity-inventory-20261028/`:
`arm64-baseline.tsv`, `x64-baseline.tsv`, `arm64-primitives.tsv`, and
`x64-build.log`. Full x86_64 parity is not established by this inventory.

## Integrated checkpoint

At `14619e46`, ordinary ARM64 and signed x64 each pass all 260 result checks.
The original eighteen rows now have eleven ARM64 native-root witnesses versus
sixteen x64 witnesses. The two new dynamic rows add string length on ARM64 and
both string operations on x64, for twelve of twenty versus eighteen of twenty.
The independent main ARM64 rebuild reproduces the same row outcomes. The
remaining differences and evidence locations are recorded in the
[integrated checkpoint](arm64-parity-checkpoint-2026-10-28.md).

At production source `a4acb620`, integrated through `8e2679b0`, ordinary ARM64
adds a native-root witness for dynamic string byte: thirteen of twenty rows
versus eighteen on signed x64. All 260 result checks pass on each architecture,
and the independent main rebuild reproduces ARM64's rows. The literal byte and
early-return branch samples remain unwitnessed; the latter is outside the
new common-join branch capability. See the
[math/byte/branch checkpoint](arm64-math-byte-branch-checkpoint-2026-10-28.md).

The folded-literal byte follow-up at `cdc2b7e2`, integrated with test cleanup
through `98e158b8`, raises ARM64 to fourteen witnessed rows. All 260 result
checks pass, and main independently reproduces this inventory. The original
eighteen rows now have twelve witnesses versus sixteen on x64; both added
dynamic-string rows are witnessed on both targets. The four remaining sampled
differences are early-return branching, nested calls, varargs and multiple
results. Iterator and I64 rows remain inconclusive on both architectures.
See the [folded-byte evidence](arm64-string-byte-contract-2026-10-28.md).

## Port-branch follow-up

At `60d304989`, ordinary ARM64 and signed x64 each pass all 260 comparisons.
Early-return branching adds one ARM64 selected-root witness, giving fifteen
of twenty rows versus eighteen on x64. Captured nested calls, Lua varargs and
multiple results remain the sampled differences. Iterator and I64 remain
inconclusive on both architectures. Main has not been updated to this source;
the [checkpoint](arm64-math-dynamic-early-checkpoint-2026-10-28.md) records
validation and the unresolved harness crash separately.

At `f040711ab`, both architectures still pass all 260 comparisons. Captured
nested calls add an ARM64 selected-root witness, giving sixteen of twenty
rows versus eighteen on x64. Lua varargs and multiple results remain the
sampled differences. The [captured-call checkpoint](arm64-captured-nested-checkpoint-2026-10-28.md)
separates the focused four-profile contracts and stock corpus results from
the earlier complete gate and unresolved TDUP failure.

At `3e3f3432a`, both targets pass all 260 comparisons and the fixed multiple-
result leaf adds an ARM64 selected-root witness: seventeen of twenty versus
eighteen on x64. Varargs is the remaining sampled difference. Iterator and
I64 rows remain inconclusive on both targets. The complete experimental gate,
all four combined M7 FFI profiles, ordinary companions and both stock corpora
pass at this source; see the
[combined checkpoint](arm64-captured-multires-checkpoint-2026-10-28.md).

At runtime source `d9352591a`, the corrected oracle harness again passes all
260 comparisons on each target, with seventeen ARM64 selected-root witnessed
rows versus eighteen on x64. Varargs remains the sampled difference;
iterator/I64 remain inconclusive. The scalar FUNCF extension is measured
separately by the topology probe. See the
[integration and oracle correction](arm64-funcf-integrated-checkpoint-2026-10-28.md).

At runtime source `cbb707874`, both targets pass all 260 comparisons and both
have selected-root witnesses in 18 of 20 rows. Fixed Lua varargs supply the new
ARM64 witness. Iterator and I64 remain unwitnessed on both targets; this does
not establish full parity. Both ordinary builds also pass the 509 source and
509 imported-bytecode stock corpora. The combined Lua-call contract passes all
four ARM64/ARM64e assertion/release profiles on the integrated source.
Evidence: `/Users/frityet/Scratch/ljmt-varargs-integrated-20261029/`.

The integrated topology run missed the scalar FUNCF root once. Three isolated
reruns with that same immutable executable each publish it and observe 81
type-mismatch exits; three reruns with the earlier `d9352591a` executable do
the same. Those reruns are under
`/Users/frityet/Scratch/ljmt-funcf-regression-20261029/`. This variation is not
a demonstrated lost capability, but the original missing observation is
retained. FORL side traces remain a measured difference, while integer and
fractional while-loop samples have root-linked sides on both targets.
