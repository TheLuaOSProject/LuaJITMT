# ARM64 string-input `tostring` identity path

The ordinary x86-64 topology probe has a native `loop-tostring` root for a
string argument. ARM64 previously refused this source shape. LuaJIT's
`recff_tostring` returns an already-string argument unchanged; it does not
invoke `__tostring` for strings. This checkpoint admits only that one-argument,
one-result fast-function case in a `LOOP` or `FORL` root.

The recorder preflight binds the current caller frame to the exact `CALL` and
requires one supplied argument and one result. Semantic admission and post-RA
admission independently verify the guarded fast-function identity, the source
`CALL` snapshot's function and string argument slots, and the result path:
the `CALL` is followed immediately by the unique `CSET` into the cell read by
the terminal `CGET`, and that `CGET` is immediately returned by `RET1`. The
source scan rejects other calls, additional writes to that cell, other
argument/result arities, and non-string argument loads. The runtime type and
function guards continue to exit to the interpreter for numbers, booleans,
tables, replacements, and errors.

The C fixture keeps a live peer, checks exact string bytes including embedded
NUL and high-byte data, verifies selected-root exits for type and function
changes, and checks code identity across full GC. Private clones verify that
both semantic and post-RA admission reject replacing the guarded builtin with
`string.byte` and changing the `CSET` source register. The ordinary Lua
companion checks the same results and metamethod/error behavior.

Focused validation used `tools/ci/arm64_jit_tostring_contract.sh` with an
explicit `LJ_TEST_ROOT` and isolated `TMPDIR`. All four profile builds and C
and Lua fixtures passed on 2026-10-30:

- arm64 assert: `2d01e6466bc401434875aa15dd91b014e3f11142ec9bd9683c2da8d3e7c489b6`
- arm64e assert: `f429a01519e42be0b6c48a77a95e41a482a00277ce55b2395be4f9c34791036c`
- arm64 release: `9b163e25ce11459dd04e88d926da184567b7efb3808bf6f6762fe7d1f0bc5ea6`
- arm64e release: `8757dbf4445f60d1cd0c2417330a314b3ef0cc7e569ea44b1aeef79d96c9d4b1`

Numeric formatting, `__tostring` metamethod execution, tail calls, and any
other `tostring` call shape remain interpreted.
