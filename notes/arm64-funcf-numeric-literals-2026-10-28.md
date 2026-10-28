# ARM64 scalar function roots with numeric bytecode literals

This slice extends the existing fixed scalar `FUNCF` root proof to bind
`ADDVN`, `SUBVN`, `MULVN`, `DIVVN` and their `NV` forms to the prototype's
numeric constant pool. It does not open calls, branches, heap effects, varargs,
or general function bodies.

## Proof changes

- The source preflight and semantic replay receive the owning `GCproto`, load
  the exact encoded KNUM slot with the existing acquire helper, and require a
  numeric TValue. `numberVnum()` handles either numeric TValue tag without
  asserting on integer-tagged values.
- Both VN and NV encode the variable in bytecode B and the KNUM index in C.
  NV changes the operation's semantic operand order only. The semantic and
  post-RA bytecode replays use that distinction when matching the IR operation.
- The semantic and post-RA walks independently resolve the source literal to
  an `IR_KNUM` head by exact IEEE bits. Their pool scans skip KNUM, KGC and
  KKPTR payload words, and reject payload refs as operand heads. KNUM is only
  admitted for this `FUNCF` trace; other root classes keep the previous
  constant policy.
- Post-RA source replay independently binds the arithmetic result back to the
  bytecode result slot and validates allocated operands/results. No global
  builtin/source association or runtime lock is introduced.

## Validation

`tools/ci/arm64_jit_funcf_record_contract.sh` passed on ARM64 and arm64e,
compiled with `LUA_USE_ASSERT`, `LJ_TRACE_TEST_HELPERS` and
`LUAJIT_MCODE_TEST`; each profile ran default and randomized mcode placement.
The native fixture runs with a live `threading.spawn` peer attached. Positive
cases include `x*.5+1.25`, `x*.5+1`, `x+1.25`, `x-1.25`, `1.25-x`,
`x/1.25`, `1.25/x`, and `1.25+x`. The `x*.5+1` case exercises an integer-valued
source numeric literal through the KNUM path. Semantic and post-RA negatives
mutate exact KNUM bits, substitute its payload word as an operand, use
`REF_BASE`, use the actual dynamic reference `nk-1`, and use `REF_TRUE`; each is
rejected and restored positives are re-admitted.
The integer-valued mutation is covered by the native positive case on both
profiles.

The profile output is at
`/Users/frityet/Scratch/ljmt-funcf-vn-20261028/final-nv-peer-contract.log`.

For an ordinary non-test ARM64 build from this source, the topology probe used
the matching worktree Lua modules and a live MT peer. It published the hot
scalar `FUNCF` root as trace 1 (`nins=5`, `nk=7`); changing the input to a string
after a full collection produced 81 `texit` callbacks from root trace 1. The
non-leaf function row still had no root publication. Probe output:
`/Users/frityet/Scratch/ljmt-funcf-vn-20261028/current-arm64-worktree-probe.log`.
The ordinary ARM64 executable SHA-256 is
`c342ff88901b7b6ccb5e1b984a9a975edc024c5cb1aebdb96c4226dc7100d9ba`.
This is evidence for the scalar root shape only, not complete function-root
parity.
