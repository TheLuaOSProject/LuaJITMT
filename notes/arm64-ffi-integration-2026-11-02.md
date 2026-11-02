# Pointer, large and fixed-array FFI result integration

`7f8d046ee` integrates the independently tested result work through
`db8fe11b8` onto `codex/arm64-parity-20261102`. The result classifier now
handles pointer leaves, larger non-HFA indirect results, and fixed arrays
within its existing 32-leaf/32-depth budget. The full extent uses `CTSize`;
checked field/array arithmetic rejects malformed extents before traversal.
HFA classification remains first. Runtime calling and allocation protocols
are unchanged, with indirect result storage passed in X8.

The component evidence is recorded in
[pointer results](arm64-ffi-pointer-sret-2026-11-02.md) and
[large and array results](arm64-ffi-large-indirect-results-2026-11-02.md).
It includes native ARM64/arm64e assertion/release execution, selected-root
exits after GC and different callees, complete returned-field comparisons,
exact effect counts, malformed-CType controls, and a signed x64 comparison.

## Integrated verification

All four combined `m7_ffi_callxs_arm64_scalar` profiles pass on `7f8d046ee`:
ARM64 assertion, arm64e assertion with BTI, ARM64 release, and arm64e release
with BTI. This covers the existing scalar/aggregate/vararg call admission and
native execution, callbacks, suspended native roots, remote flush, C++ unwind,
and shared-table argument/lifecycle fixtures together with the new results.
The profile loop was extracted directly from the umbrella gate, using its
shared profile/lock helpers and private fixture directories.

The run then restored an ordinary ARM64 assertion build with only bootstrap,
experimental JIT and assertions enabled. That runtime passed both 509-case
stock corpora and all 260 checks in `bench/jit-parity.lua`; 18 of the 20
sampled workloads produced selected-root exit witnesses. These counts remain
bounded observations, not a full-parity claim.

Evidence directory:
`/Users/frityet/Scratch/ljmt-parity-checkpoint-20261102/`.

- `integrated-m7-four-profile.log`: all four M7 success markers and ordinary
  restoration success.
- `integrated-arm64-stock-source.log` and `integrated-arm64-stock-v2.log`:
  509 passed each.
- `integrated-arm64-parity.tsv`: workload comparisons and native witnesses.

The complete umbrella gate passed the preceding production source at
`e0da9e64e`, as recorded in the
[complete checkpoint](arm64-complete-checkpoint-2026-11-02.md). It was not
rerun in full for this narrow FFI integration. The umbrella script now
restores an ordinary build on success and cleanup instead of retaining its
final fixture-helper profile; the same restore helper succeeded in the
integrated M7 run above.

The original `/Users/frityet/Projects/LuaJITMT` checkout's pre-existing source
edits and trace dump remain untouched. Integration is performed in
`/Users/frityet/Scratch/ljmt-arm64-parity-20261102` and pushed continuously.
