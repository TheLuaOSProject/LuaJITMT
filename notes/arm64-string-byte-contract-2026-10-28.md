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

For a literal call such as `string.byte('abc', 2)`, the recorder folds the
read to integer constant `98` before admission, so that trace has no STRREF or
XLOAD to certify. The fold can leave an IR_KKPTR constant naming the literal's
interior byte address. The generic scalar root's semantic and post-RA constant
scans accept that residue only when neither its IRRef nor its paired GC64
payload IRRef occurs in any surviving IR operand or snapshot. The post-RA scan
also examines allocator-tail RENAME operands. Scalar constant resolvers skip
the paired payload and explicitly refuse to use the KKPTR header as a value.
If either word becomes live, admission rejects it; this does not admit pointer
arithmetic, loads, or pointer snapshot state. The runtime result remains the
ordinary integer constant produced by the recorder.

The native C fixture requires an entry-count increase for the literal loop,
checks the compiled root shape, and separately mutates the residue and paired
payload into IR operands, snapshots, and an allocator-tail RENAME. It also
mutates the function identity guard. The Lua companion compares literal calls
and first/last/out-of-range indices through ordinary Lua semantics. The
four-profile contract passed on arm64 and arm64e with assertions enabled and
disabled; the C fixture reported ten native entries and ten exits, including
the literal-root entry and bounds exits.

The ARM64 C fixture checks that recorded source and bound identities, checks
the source remains mapped in snapshots while STRREF/XLOAD do not escape, and
mutates load type, volatility, pointer source, offset, length source, guard,
and snapshot mapping independently through semantic and post-RA admission.
It also replaces STRREF with a generic PGC TMPREF, mutates pointer, result,
and base register classes, removes the string base location, and attempts to
feed the pointer into a loop PHI. It keeps another TG alive while collecting
and re-entering native code. The integer accumulator uses a `FORL` trace with
`sum=0`, matching the ordinary benchmark. A separate numeric `while` trace
starts before its body, allowing too-short, empty, and wrong-type arguments to
enter the published trace and exercise native guard exits before their
interpreter errors. Valid dynamic, embedded-NUL, and long strings each require
a fresh native-entry delta after full GC.

`tests/t-arm64-jit-string-byte.lua` independently checks Lua results, IR
shape, GC re-entry, empty/short nil results, and invalid-type errors. It is a
semantic/IR companion; it makes no native-counter claim.

The profile-bound `tools/ci/arm64_jit_string_byte_contract.sh` passed all four
ARM64 and arm64e assertion/release profiles after the post-RA pointer and
snapshot checks on 2026-10-28. The suite is also
registered in `m6_jit` and the ARM64 fail-closed gate.

## Positive dynamic index

The same bounded read also admits the recorder's positive, one-based dynamic
index form. Independent semantic and post-RA proofs require the exact numeric
SLOAD-to-`IRCONV_INT_NUM|IRCONV_ANY` conversion, the exact string SLOAD, an
unsigned `index <= string.len` guard, checked `index-1`, nonnegative offset,
checked subtraction, and an exact one-byte range before STRREF. The string and
index SLOAD slots must resolve from the ordered first and second arguments of
the function's fixed two-argument `CALL`; only straight-line MOV/CGET
definitions are followed, and cell-backed argument sources with any CSET in
the function are refused. The root preflight separately verifies the active
`FF_string_byte` identity and current caller frame/call shape. The proof does
not admit negative-index lowering or dynamic values directly into snapshots.

The numeric conversion follows ARM64 `FCVTZS` behavior measured on this
target: NaN converts to zero, positive overflow/infinity saturates to
`INT32_MAX`, and negative overflow/infinity saturates to `INT32_MIN`. The
checked nonnegative guard rejects NaN/zero and negative values. `LJ_MAX_STR` is
statically required to be below `INT32_MAX`, so a positive saturated index
fails the unsigned upper-bound check before STRREF. The test exercises
fractional positive index 2.9 (Lua truncates it to byte index 2), signed zero,
NaN, both infinities, very large magnitudes, negative, and out-of-range inputs.
Valid index execution requires native entry; invalid cases require native
entry and exit before the interpreter produces the expected result/error. The
fixture keeps a second TG attached through native calls and full GC/re-entry.

The updated profile-bound contract passed arm64 and arm64e assertion and
release profiles. The combined C fixture reported 101 native entries and 101
exits across the primary, bounds, and dynamic traces; the literal trace has a
separate native-entry assertion. The Lua companion verifies fractional
conversion and ordinary embedded-NUL/negative/out-of-range semantics without
claiming native coverage.
