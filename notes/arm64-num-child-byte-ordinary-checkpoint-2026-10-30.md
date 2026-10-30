# Ordinary NUM loop, child-call and byte-range checkpoint

Frozen source `89226814d` combines the prior complete `6c46f9bff` checkpoint
with integer-literal CALLM replay, CALLXS integer accumulator PHIs, NUM FORL
direction admission, guarded FUNCF producer-child exits, default string.byte
indices and fixed positive end/range arguments. The complete gate is running
separately on this frozen source. This note records the completed ordinary
matrix, not a complete-gate result or full x86_64 parity.

Ordinary assertion ARM64 and x86_64/Rosetta each pass all 509 stock source
cases and all 509 cases imported as genuine original LuaJIT v2 bytecode.
The x86_64 build uses `-Werror`. Neither runtime contains test helpers.
Both pass the existing twenty-row parity inventory's 260 result comparisons,
with eighteen selected-root witness rows each; iterator-table and I64 rows
remain unwitnessed on both. The expanded topology probe also completes all
result/error comparisons, including custom ITERL roots and Lua return/call
forms. These are workload observations, not exhaustive native coverage.

The focused ordinary comparisons show:

- Literal, dynamic and descending fractional FORL samples now publish roots
  on ARM64 and x86_64. The observer probe sees eight selected-root exits for
  eight calls before collection. After collection it sees seven on ARM64 and
  eight on x86_64, including the preexisting integer-control row. This count
  difference is preserved; it is not treated as proof that every call entered
  native code. The separate native C contracts test post-GC entry directly.
- All six mixed CALLM producer cases publish roots on both targets, including
  `return x+1,.25` and integer literals in either result position. Each has
  eight selected identity exits for eight calls before and after collection.
- Positive exact-end, wider-range and nil-end string.byte cases have eight
  selected exits before and after collection on both targets. Constant
  negative indices remain a measured ARM64 gap and are being handled later.

Evidence is under
`/Users/frityet/Scratch/ljmt-num-child-byte-integration-20261030`:

- `{arm64,x86_64}-{build,source,bytecode}.log`.
- `{arm64,x86_64}-{fractional-forl,mixed-callm,byte-ranges}.log`.
- `{arm64,x86_64}-{parity,topology}.tsv`.
- `{arm64,x86_64}-ordinary/`: preserved executable, archive, JIT modules,
  source revision and SHA-256 manifest for each exact build.
- `ordinary-arm64-restoration.log`: successful ordinary ARM64 restoration,
  profile `143207c9d19e152d98e49f35f0d489011b362dc977269f8d11fda07e3d7dba9d`.

Initial build profiles are
`3d6739dfba34c84924169e1660fe6382bfcff6b7c3557eb4f7f80a10178695d0`
(ARM64) and
`03ac7a9109f7702daffcd8627e9f1683eb56edae2168982b7e4e006c18768fd2`
(x86_64). The main parity branch remains on the earlier complete checkpoint
until the frozen combined gate is resolved. Aggregate-prefix variadic FFI,
integer bodies inside NUM FORL and constant negative bytes are separate
follow-up work.
