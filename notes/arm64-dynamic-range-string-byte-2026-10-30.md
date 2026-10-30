# Dynamic positive range ends for string.byte

`byte(s, 2, last)` with a positive integer `last` produced a native root on
x86_64 but was refused on ARM64. The recorder retains an unsigned string-length
guard followed by a checked `end - offset` subtraction and an equality guard
for the recorded result count. The previous constant-offset bounds proof only
recognized a literal length bound.

The semantic and allocated-code passes now independently accept that existing
guard relation. The same immutable string must supply the guarded length and
the STRREF base. A nonnegative constant offset, guarded subtraction, and
positive width equality prove `offset < end <= length`; all guards must precede
the address computation. This adds no emitted instructions or runtime locks.
The existing source-call, frame, snapshot, allocation, and byte-load checks
still apply. A separate read-only review found no soundness blocker in this
relation and its call sites.

## Validation

- `tools/ci/arm64_jit_string_byte_contract.sh` passes assertion and release
  profiles for ARM64 and ARM64e/BTI. The new fixture records both `last=2` and
  `last=4`, compares against a separately interpreted oracle, and checks native
  entry/exits, full-GC reuse of identical code, empty/short strings, NUL/high
  bytes, changed ends, integer extremes, and numeric type transitions.
- Six private mutations independently fail semantic and allocated admission:
  removing each of the three guards, disconnecting the subtraction's end,
  using the unproven end as the byte offset, and changing the width to zero.
- The restored ordinary ARM64 binary passes all 509 stock source tests and all
  509 stock v2 bytecode tests. Its profile is
  `b66f7e72318ed4ed768299b5b7cd105ef67d728f80192bb3eecb77991f99cb19`.
- Ordinary ARM64 and the frozen x86_64 binary from `89226814d` both select root
  1 and produce eight selected-root exits for eight calls, before and after
  full GC, for integer end values 2 and 4. Nil end retains its previous native
  behavior. Numeric non-integer, negative, and independently dynamic start/end
  recordings still expose separate ARM64 gaps.

Logs are in `/Users/frityet/Scratch/ljmt-dynamic-range-byte-20261030/`:
`four-profiles.log`, `stock-source.log`, `stock-bytecode.log`,
`arm64-ordinary-probe.log`, and `x86_64-baseline-probe.log`. The full combined
gate has not been run on this isolated change.
