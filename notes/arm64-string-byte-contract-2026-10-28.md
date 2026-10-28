# ARM64 `string.byte` bounded native contract

The experimental ARM64 recorder path now admits a generic, bounded constant
index `string.byte` read. The root fast-function preflight accepts only the
exact `FF_string_byte` identity in a Lua `LOOP` or `FORL`, with the recorded
two-argument, one-result call shape. It verifies the pending caller call and
frame geometry before the operation reaches the independent IR validators.

The semantic-IR and post-RA checks independently require a readonly `U8`
`XLOAD` sourced from `STRREF`, with a nonnegative constant byte offset and an
earlier unsigned guard proving `string.len` for that same guarded string is at
least `offset + 1`. The current STRREF address form is restricted to its
unsigned immediate range. Dynamic and negative indices remain refused; this
does not rely on the trailing NUL byte or Lua's negative-index handling.
`LDRB` zero-extends the byte, so the certified U8 value may feed the Lua
integer/numeric accumulator paths. U8 is not admitted directly into snapshots
because it has no TValue restoration encoding in this slice.

The x86-64 reference trace for a dynamic string uses this path:

```text
SLOAD string (type guard)
FLOAD string.len
UGE string.len, offset + 1 (guard)
STRREF string, offset
XLOAD U8 readonly
```

The ARM64 C fixture checks that recorded source and bound identities, checks
the source remains mapped in snapshots while STRREF/XLOAD do not escape, and
mutates load type, volatility, pointer source, offset, length source, guard,
and snapshot mapping independently through semantic and post-RA admission.
It keeps another TG alive while collecting and re-entering native code. A
`while` trace starts before its body, allowing too-short, empty, and wrong-type
arguments to enter the published trace and exercise native guard exits before
their interpreter errors. Valid dynamic, embedded-NUL, and long strings each
require a fresh native-entry delta after full GC.

`tests/t-arm64-jit-string-byte.lua` independently checks Lua results, IR
shape, GC re-entry, empty/short nil results, and invalid-type errors. It is a
semantic/IR companion; it makes no native-counter claim.

The profile-bound `tools/ci/arm64_jit_string_byte_contract.sh` passed all four
ARM64 and arm64e assertion/release profiles on 2026-10-28. The suite is also
registered in `m6_jit` and the ARM64 fail-closed gate.
