# Integer-indexed dynamic `string.byte` on ARM64

The bounded dynamic `string.byte` proof previously recognized an index loaded
as a guarded NUM and converted to INT. It now also recognizes an index that is
already an `IRT_INT|IRT_GUARD` SLOAD. Both semantic IR and post-RA admission
require the exact typechecked load, prove that the source slot is the current
`string.byte` index argument, and bind the existing positive or negative range,
nonnegative-offset, checked-width, and same-string proofs to that source. The
two passes validate these relationships independently. No emitted code,
runtime metadata, or lock behavior changes.

The bounded scope is dynamic integer indices for the existing single-byte
positive and negative lowering. The change does not extend dynamic range-end
admission or permit untyped integer loads.

## Validation

- `tools/ci/arm64_jit_string_byte_contract.sh` passed arm64/arm64e assertion
  and release profiles. Positive and negative integer candidates each selected
  a native root, compared valid and invalid integer indices against a separate
  JIT-off oracle, exercised fractional-number type exits, repeated the checks
  after full GC, and verified that the same machine-code allocation remained
  live. Seven copied-source/bounds mutations were rejected by both semantic
  and post-RA validators for each direction.
- The ordinary companion checks both integer directions against separate
  interpreted copies. The stock source and genuine v2 bytecode corpora each
  passed all 509 tests on ARM64 and x86_64.
- The matched observer-free ordinary probe selected root 1 for positive and
  negative integer indices on both targets. Each case observed eight selected
  root exits before and after GC. Those counts are runtime witnesses, not a
  claim that the targets emit identical trace topologies.

Evidence is in `/Users/frityet/Scratch/ljmt-integer-index-byte-20261030/`:
`integer-index-final4profiles.log`, `ordinary-matrix-driver.log`,
`arm64-source.log`, `arm64-bytecode.log`, `x86_64-source.log`,
`x86_64-bytecode.log`, `arm64-integer-index-probe.log`, and
`x86_64-integer-index-probe.log`.
