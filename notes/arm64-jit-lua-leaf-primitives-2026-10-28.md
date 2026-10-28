# Primitive literals in bounded ARM64 Lua leaf returns

## Scope

The fixed pure-Lua leaf certificate now admits `BC_KPRI` nil, false, and true
inside its bounded source body. Source recognition, semantic type propagation,
allocated type propagation, both independent prefix reconstructions, and both
independent liveness walks handle the literals explicitly. Prefix values use
the exact `REF_NIL`, `REF_FALSE`, or `REF_TRUE` constant; primitive values stay
nonnumeric in the semantic and allocated operand walks. Values still have to
match the exact child snapshot and caller assignment binding.

This change adds no recorder, assembler, VM, GC2, FFI, lock, or executable-memory
mechanism. General Lua call/return, effectful leaves, unrestricted snapshots,
and x86_64 parity remain outside the certificate.

## Focused evidence

`tests/t-arm64-jit-lua-return-exits.c` covers direct nil/false/true local
assignments and a source prefix with a true literal before dead arithmetic and
a parameter assignment. The current trace may remove that arithmetic, so this
case does not establish retained arithmetic or overflow-snapshot liveness.
Independent semantic and post-RA mutations substitute one valid primitive for
another, use an invalid primitive selector, or use a KPRI-produced value as an
arithmetic operand. The ordinary Lua companion adds the same three direct
literal cases and the prefix case.

The four-profile contract passed before the final valid-selector substitution
negative and allocated primitive type-enum correction. Its profile fingerprints
and output remain useful history, but do not validate the final source state.
After those edits, one ARM64 assertion profile built and the focused C fixture
and ordinary Lua companion passed: 84 cases, 336 native invocations, 84
controlled terminal exits, 252 same-trace reuses, 84 post-GC reuses, 1,320
semantic negatives, and 1,329 allocated negatives. The Lua companion reports
43 publications, four numeric ranges, and 94 exact-trace reuses; it makes no
native-execution claim. The later final-source four-profile run and complete
experimental gate both pass; see the
[integrated checkpoint](arm64-parity-checkpoint-2026-10-28.md).

| Profile | Fingerprint prefix |
| --- | --- |
| ARM64 assertions | `bbf70d8dcb4a` |
| ARM64e/BTI assertions | `cf6a2c413709` |
| ARM64 release | `f4c5f3209503` |
| ARM64e/BTI release | `42412d3f3c22` |

Earlier four-profile output:
`/Users/frityet/Scratch/ljmt-leaf-primitives-20261028/leaf-contract-final.log`.
Focused final-source output:
`/Users/frityet/Scratch/ljmt-leaf-primitives-20261028/leaf-return-focused.log`.

The user's wider x86_64 parity goal is not complete with this slice.
