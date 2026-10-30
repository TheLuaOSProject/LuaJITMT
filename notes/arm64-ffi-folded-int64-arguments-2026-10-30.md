# Folded fixed 64-bit integer FFI arguments

An ordinary matched probe at `ec98dbf5a` found that a Lua literal passed to a
fixed `uint64_t` parameter had an x86_64 native root but no ARM64 root.
`lj_ir_k64` interns every `KINT64` bit pattern with `IRT_I64`, including
unsigned conversions. ARM64's fixed argument classifier incorrectly demanded
`IRT_U64` for that constant. It now accepts the canonical `KINT64/IRT_I64`
carrier for fixed unsigned 64-bit arguments. Dynamic typed values retain
their prior checks; native code generation is unchanged.

Compact stack signatures also need exact source binding. Both independent
passes now match the original `KINT/IRT_INT` snapshot argument to a folded
`KINT64` only when its complete payload equals the signed INT extension.
This covers negative Lua integers converted to `uint64_t` as well as signed
64-bit parameters. It does not admit arbitrary NUM constants or unrelated
constant payloads.

All four ARM64/ARM64e assertion/release profiles pass the fixed-integer
runtime and admission contracts. Runtime coverage now includes 72 native
shapes, 528 runs and 88 forced POSTCALL exits per profile. The 24 new literal
shapes cover signed and unsigned register/stack arguments for -1, -129,
INT32_MIN, INT32_MAX, zero and one, exact captured C argument bits against
an interpreted oracle, errno, no replay, and reuse after full GC. Admission
coverage is 88 semantic positives/520 negatives and 88 post-allocation
positives/628 negatives, including sixteen literal cases, wrong carrier
types, floating/boolean substitutions, and changed full-width stack payloads.

An initial stack-literal test exposed the missing folded source relation;
that relation is included above. An early private test mutated unused
constant header bits, which did not change the accepted register argument.
It was replaced with a meaningful floating-constant substitution; no
production check was weakened to satisfy the test.

The final wrapper restored ordinary assertion ARM64 without test helpers,
profile `8e555692a5ed333eefc3c3818bcd834204f59f3c96337872bd748addcf43629b`.
It passes all 509 stock source and all 509 original v2 bytecode cases. The
ordinary literal probe has eight selected root exits before and after GC,
matching the frozen strict x86_64 `ec98dbf5a` build. Direct FORL-index
arguments whose sign-extension was removed by optimization remain a separate
ARM64 gap: INT typing alone is insufficient to prove a nonnegative range.

Evidence is in `/Users/frityet/Scratch/ljmt-ffi-int-u64-20261030/`:
`{arm64,x86_64}-probe.log`, `arm64-dump-clean.log`,
`literal-four-profiles-typed.log`, `stock-source.log`, `stock-bytecode.log`,
`final-arm64-probe.log`, and the earlier diagnostic/failed contract logs.
Validation used base `ec98dbf5a` plus this production/test slice.
