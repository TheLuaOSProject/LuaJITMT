# Small-aggregate native coverage comparison

`bench/jit-aggregate-probe.lua` uses the existing
`t-ffi-callxs-sysv-small-aggregate-lib.c` targets to compare identical programs
on ARM64 and x86_64. A live MT peer waits throughout. Each case makes 25
comparisons against a separately loaded, recursively JIT-disabled oracle,
including a full collection and reuse. Foreign-call counters require exactly
2,752 calls across candidate and oracle, detecting omitted or repeated effects.
The waiting peer does not access those counters.

Only the `texit` observer is installed. It matches the current runner's patched
root when each exit is delivered; a final trace read additionally counts
`CALLXS` operations in that root. An arbitrary published trace is not treated
as evidence that this workload ran natively.

## Observed gap

| shape | ARM64 | x86_64/Rosetta |
| --- | --- | --- |
| two uint32 fields passed by value, scalar result | no selected-root witness | one live loop root, two CALLXS instructions, 25 selected-root exits |
| two floats passed by value, scalar result | no selected-root witness | one live loop root, two CALLXS instructions, 25 selected-root exits |
| two uint32 fields returned by value | no selected-root witness | no selected-root witness |
| two floats returned by value | no selected-root witness | no selected-root witness |
| one uint32 field passed and returned by value | no selected-root witness | no selected-root witness |

All 125 oracle comparisons and 13,760 foreign-call counts pass on each target.
The argument rows establish actual x86_64 aggregate CALLXS traces and an
observed ARM64 coverage difference. The result rows do not establish an
architecture difference. The existing x64 small-aggregate test separately
passes its generic ABI/IR/effect checks, but it does not use this probe's live
peer and selected-root-exit measurement; keep those claims distinct.

Source agrees with the argument difference: x64's
`crec_call_sysv_aggregate_type()` admits recursively classified one-eightbyte
aggregates of exact size 1, 2, 4 or 8, and
`crec_call_sysv_aggregate_arg()` loads the matching raw scalar carrier. ARM64's
`lj_ccall_arm64_signature()` still classifies only scalar types, and the
aggregate argument branch in `crec_call_args_collect()` is x64-only. This
comparison does not justify reusing the SysV classifier for Darwin ARM64:
integer aggregates and homogeneous floating aggregates need their own target
classification and register/stack plan, followed by independent snapshot,
root, semantic and post-allocation checks.

## Reproduction and evidence

Initial runs used the immutable binaries built from `3e3f3432a` and matching
module bundles under `/Users/frityet/Scratch/ljmt-topology-probe-20261028/`.
Their source, flags and SHA-256 hashes are recorded in the topology-probe note.
The shared library was built from the existing C fixture with:

```sh
clang -O2 -Wall -Wextra -Werror -dynamiclib -arch arm64 -arch x86_64 \
  tests/t-ffi-callxs-sysv-small-aggregate-lib.c -o /tmp/small.dylib
LUA_PATH='src/?.lua;;' src/luajit bench/jit-aggregate-probe.lua /tmp/small.dylib
```

Both profiles were then rebuilt from `3e3f3432a` plus the shared snapshot-PC
fix in `2a79ec2c5`, with assertions enabled. Results are unchanged. The corrected
reflection reports completion snapshot 12 at bytecode position 14 (`CGET`),
IR reference 58 for the integer argument and 56 for the float argument. The
pre-fix reflection reported a wrong position 0; do not use that value as an
execution-location claim.

Evidence directory:
`/Users/frityet/Scratch/ljmt-aggregate-native-probe-20261028/`.
The initial rows are `{arm64,x64}-final.tsv`; the rebuilt rows are
`{arm64,x86_64}-fixed.tsv`, with matching build logs and profile hashes.
`x64-small-existing.log` records the existing ABI test. The exploratory larger
aggregate probes had no selected witnesses on either target and are not used
to establish a parity gap. No production aggregate admission changed here.

## Integer argument follow-up (2026-10-29)

At runtime source `cbb707874`, the identical probe passes all 125 oracle
comparisons and 13,760 foreign-call counts on both targets. The integer-pair
argument now has the same native witness on ARM64 and x86_64: one selected
loop root, two CALLXS instructions, and 25 selected-root exits. This closes
the measured integer-argument difference above. The two-float argument still
has a selected witness only on x86_64. Aggregate-result samples remain
unwitnessed on both targets, so they remain inconclusive.

The change admits direct integer-only structs of total width 1, 2, 4, or 8
bytes in one available Darwin argument register; it does not copy the SysV
classification. Its independent native/corruption contracts and precise
limits are in the
[integer-aggregate note](arm64-integer-aggregate-callxs-2026-10-29.md).
Evidence uses the immutable runtime/module bundles under
`/Users/frityet/Scratch/ljmt-varargs-integrated-20261029/`, with the universal
`small-aggregate.dylib` and `{arm64,x86_64}-aggregate.tsv`.

## Retained result follow-up (2026-10-29)

The probe now separates aggregate return lowering from field reads within the
recorded loop. Three added cases retain `last=fn(...)`, accumulate a scalar
loop sum, and return `last` to the JIT-disabled harness. The harness checks the
exact CType and every field against independently calculated C-fixture results,
as well as the separately interpreted candidate. It retains all 50 result
objects per case through a full collection and checks their fields again.
The original five rows remain, including their inconclusive combined
result-plus-field-access workloads.

With the same immutable `cbb707874` ARM64/x64 runtimes, all eight cases pass
200 oracle comparisons and exactly 22,016 foreign calls per architecture.
Each new x64 row has a selected loop root containing two CALLXS, two CNEW and
two XSTORE operations, and 25 selected-root exits. ARM64 has no selected-root
witness in those three rows. The return shapes are the two-uint32 struct, the
two-float struct, and the one-uint32 struct also passed as an argument. This
establishes a native result-lowering gap without relying on the earlier
unwitnessed workloads or arbitrary published traces.

The x64 result exits resume at `CGET`: bytecode position 16 for the integer
and float pairs (IR reference 58), and position 17 for the word (reference 66).
Evidence is under
`/Users/frityet/Scratch/ljmt-aggregate-result-probe-20261029/`:
`{arm64,x86_64}-expanded.tsv` records the committed eight-case probe; the
discard and retained exploratory programs/logs are preserved separately.
This follow-up changes only the probe and notes, not production admission.
