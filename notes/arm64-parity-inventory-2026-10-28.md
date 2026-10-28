# Cross-architecture JIT workload inventory

`bench/jit-parity.lua` runs identical workloads against independently loaded,
recursively JIT-disabled interpreter oracles. An attached peer remains live
throughout each run. Each workload has twelve calls with alternating bounds
and one call after full GC. The original eighteen workloads produce 234 result
checks; the current twenty-workload harness adds two dynamic-string rows for
260 checks.

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
