# Negative dynamic string.byte range ends

This slice extends the range-end proof for `string.byte(s, 2, last)` when
the recorded `last` is a negative integer or a negative fractional number.
Semantic and post-allocation validators independently find the same GCstr
length, a dominating `index < 0` guard, `len + index + 1`, and a later guarded
checked subtraction/equality proving a positive result width. `LJ_MAX_STR <
INT32_MAX` and the negative index exclude overflow in both additions. Thus
the constant byte offset is below the normalized end and string length.
The existing positive-end path reuses only its checked-width relation.

The generic bounds predicate proves memory safety from IR; it does not
attempt universal bytecode/IR arithmetic equivalence. Numeric conversion
admission separately binds the exact converted value to this relation.
An independent review found no out-of-bounds path; its proposed extra source
argument binding was not needed for this memory-safety proof. No locks or
new emitted instructions are introduced.

The string-byte contract passed ARM64 and ARM64e assertion/release profiles.
Both `last=-2` and `last=-2.9` select runnable native roots, match interpreted
results/errors for type, empty/short/long string and numeric boundary inputs,
and retain identical code after full GC. Each rejects nine independent
semantic and post-allocation corruptions: missing sign/overflow/width guards,
wrong width source or STRREF offset, zero width, wrong normalization input,
missing final increment, and a sign guard on the wrong value.

The first fixture used a 264-byte warm string, requesting 262 byte results;
the stock recorder stopped with `trace too deep` before reaching STRREF.
The final fixture warms a nine-byte string and keeps large strings in its
cold fallback checks. The failed run is retained rather than counted as
passing validation.

Ordinary assertion ARM64 passed all 509 stock source and 509 original v2
bytecode cases. A matched ordinary probe records eight selected root exits
before and after full GC for both negative ends on ARM64 and the preserved
strict x86_64 `4b8c2f880` build. Independently dynamic starts/ends still expose
an ARM64 native coverage gap. Full x86_64 parity is not claimed.

Evidence: `/Users/frityet/Scratch/ljmt-negative-range-byte-20261030/` contains
`four-profiles-bounded-width.log`, the earlier `four-profiles.log` failure,
`long-string-dump.log`, `stock-source.log`, `stock-bytecode.log`,
`ordinary-probe.lua`, and `{arm64,x86_64}-ordinary-probe.log`.
The final four-profile wrapper restored ordinary assertion ARM64 profile
`a9657995db8e88cfb4637fdee125a380def4216e646d93a323b2b38d6d5aaa12`.
Production/test evidence uses base `481959ce6` plus this slice; the final
comment clarification and this note do not affect executable behavior.
