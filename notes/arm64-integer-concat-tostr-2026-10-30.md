# ARM64 integer TOSTR admission

This slice admits `IR_TOSTR` with `IRTOSTR_INT` when its input is a proven
`IRT_INT` value. The stock ARM64 assembler already lowers this mode through
`lj_strfmt_int`; no emitter or formatter change was needed. The semantic
walker, recursive string-reference proof, post-RA opcode walk, post-RA string
reference proof, and generic post-RA value whitelist now all recognize the
integer mode explicitly. The numeric path remains type-bound to `IRT_NUM`.
`IRTOSTR_CHAR` and generic buffer operations remain closed.

`tests/t-arm64-jit-scalar-capability.c` supplies independent semantic and
post-RA mutations for mode, input type/reference, forward references, PHI
incoming values, and malformed string/buffer snapshots. The Lua cases cover
FORL integer indices, stable integer inputs at `INT32_MIN` and `INT32_MAX`, and
an addition that crosses `INT32_MAX` while comparing JIT-enabled output with
the interpreter result. The overflow case is an output-equivalence check; the
native selected-root proof covers the non-overflow integer cases.

The publication fixture starts a live peer before candidate compilation,
requires a selected FORL root and terminal loop exits, and checks that the
exact fresh integer-formatted output is interned while `jit_base` is active.
The expected digits are produced by the C fixture so the expected string is
not already interned from a Lua literal. It also checks traversable/READY
publication state, full-GC reuse, and code identity. The numeric publication
case uses the same C-only expected-text setup.

Validation on the integrated base `68c122f2a8772c1fbdca44007ff30271b4bd0f0d`
plus the source diff recorded in the companion Scratch evidence:

- `tools/ci/arm64_jit_numeric_tostr_contract.sh` passed all four ARM64 and
  arm64e assertion/release profiles. Log:
  `/Users/frityet/Scratch/ljmt-integer-concat-probe-20261030/four-profile.log`.
- Ordinary ARM64 runtime and JIT modules matched the integer probe with
  selected roots/exits: index `1/13`, index-plus-scalar `2/13`, wide index
  `1/13`; the numeric control was `2/13`. Bundle and output:
  `/Users/frityet/Scratch/ljmt-integer-concat-probe-20261030/ordinary-arm64-final/`.
- Strict Darwin x86_64 assertion build (`-arch x86_64 -mcx16`,
  `XCFLAGS='-DLUA_USE_ASSERT -Werror'`) passed. The same live-peer probe had
  selected roots/exits: index `1/6`, index-plus-scalar `3/6`, wide index
  `1/6`; numeric control was `3/6`. Bundle and build log:
  `/Users/frityet/Scratch/ljmt-integer-concat-probe-20261030/ordinary-x86_64-final/`
  and `x64-build.log`.

This does not claim broader integer overflow arithmetic support: only the
existing scalar admission accepts the operations around the integer TOSTR.
