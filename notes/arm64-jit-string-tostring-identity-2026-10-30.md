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

## Ordinary matched runtime confirmation

A second matched probe ran against the committed source without trace or mcode
observers. The ARM64 runtime was built from `49d879b6097395a5b8a9e6720d6d13564b1b42e9`
with `TARGET_FLAGS='-arch arm64'` and
`XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT'`.
The strict x86-64 runtime used the same source with
`TARGET_FLAGS='-arch x86_64'` and `XCFLAGS='-DLUA_USE_ASSERT -Werror'`.
Both binaries and matching `jit`/`etc` Lua modules, the probe, and output logs
are preserved under `/Users/frityet/Scratch/ljmt-tostring-ordinary-20261030/`.
The probe's `texit` callback classified exits against the selected runner's
encoded JLOOP/JFORL root IDs; it did not install a TRACE observer. A live
threading peer remained attached through both forms and was joined at the end.

Both architectures selected JLOOP root 1 (14 IR instructions) and JFORL root 2
(15 IR instructions). Exact-root exits occurred in each form: ARM64 recorded
166 and 159 selected-root exits; x86-64 recorded 168 and 160. Each run compared
the candidate with a separately compiled JIT-disabled oracle for embedded-NUL,
high-byte, empty, numeric, boolean, table `__tostring`, replacement-function,
and throwing `__tostring` inputs. A full GC preserved each selected trace and
its machine-code bytes. The captured results are in each architecture's
`probe.log`; binary hashes are in `runtime.sha256`.
