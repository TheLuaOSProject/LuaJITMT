# Variadic indirect results with named aggregate prefixes

The ARM64 variadic CALLXS path now accepts a named, small non-HFA aggregate
when the ordinary GPR bank is full, including a 24-byte indirect result in X8
and a scalar variadic suffix. The indirect result pointer remains raw CARG
zero. Semantic and post-RA argument proofs remove it before matching visible
ABI indexes, and validate it against the exact guarded CNEW payload, CType,
GCcdata offset, and one-use boundary. The named aggregate is then matched to
its fixed CType argument and stack offset; the variadic marker and promoted
suffix keep their existing positions and stack planner.

The fixture covers two placements: a named `Pair8` aggregate within the GPR
bank, and eight explicit `uint64_t` runner parameters followed by `Pair8`, a
fixed integer, and `int`/`double` variadic values. The latter places the named
aggregate after the eight-register bank. Both functions return exact fields
in a 24-byte result and increment independent C counters. The live peer stays
attached across publication and forced full GC. The callback verifies the
selected native trace, result-root CType, and sret CALLXS/CNEW presence. After
GC, each case requires the same selected trace, native entries, exactly 24 C
calls, successful post-call exits, and a runnable trace. Private copies prove
semantic and post-RA acceptance and reject a substituted hidden pointer, a
moved separator, and a fixed aggregate CARG replaced by a variadic carrier.

The captured-prefix form is not part of this claim. An earlier draft captured
the eight values as upvalues and failed semantic admission; the validated case
passes them as explicit runner arguments, isolating named ABI placement from
upvalue carrier proof. Lua-integer `CONV` composition into the variadic suffix
also remains a separate measured refusal.

## Evidence

Focused ARM64 contract, with `LJ_TEST_ROOT` set to this checkout and
`LJ_ARM64_TABLE_CALL_CASES=ffi-varargs`, passed all four helper profiles:
ARM64 and ARM64e assertion and release. The run used the existing
`maxtrace=64` setting. Log:
`/Users/frityet/Scratch/ljmt-vararg-prefix-20261029/four-profile-final.log`.

The matched x86_64 probe used the frozen ordinary runtime
`/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029/x64-frozen/luajit`
(SHA-256 `66afc8970ea61714248f512067dfa449301197ad267cc2ca3ed697f6273fce9c`)
and a DSO compiled from the fixture source for x86_64 (SHA-256
`da11d6612d68a43dc694ad638705693ac2658b6306a2c34c4b14ca08bbc34a1d`). Both
runner roots stopped natively: trace 1 for the in-bank prefix and trace 2 for
the eight-register overflow prefix; each reported 16 exits, returned the
expected fields, and advanced its C counter by exactly 24. The trace-event
and counter log is
`/Users/frityet/Scratch/ljmt-vararg-prefix-20261029/x64/events-correct.log`.

The ordinary Lua companion remains labeled as publication/oracle evidence;
the native claims above come from the selected-trace C callback fixture.
