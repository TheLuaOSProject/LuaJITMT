# Deep side lists and CALLM producer integration

Production source `bc0313318` combines the verified `8106cfe53` checkpoint
with deep side-list acquisition (`868ce2f1e`), captured open-result producer
replay with unchanged formal guards (`a9406664d`, `1ac324e85`), and the bounded
zero-formal three-result CALLM window (`7a557306b`, integration `fb7579006`).
The runtime producer-pair proof retains the existing input-span bound;
only the C1 result window receives the extra `LJ_FR2` extent. The separate
returned-call callback still refuses a result beyond the declared frame,
pending a direct witness. Snapshot restore is unchanged.

The combined C fixture retains the original eight formal-producer cases and
adds three separate zero-formal/captured-consumer cases. It preserves the
current-iteration producer guard checks, missing/excess formal normalization,
and dead-DAG/spill/RENAME negatives. The ordinary Lua companion now correctly
passes its fifth tuple field to create the zero-formal producer.

## Focused and ordinary validation

All logs below are under
`/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029/`.

- `leaf-four-profile.log`: the complete Lua leaf contract passes ARM64 and
  arm64e assertion/release profiles. Each C run reports 52 native entries,
  267 semantic negatives, 278 allocated negatives, and zero cold retries.
  The corrected ordinary zero-formal companion publishes and reuses its
  selected trace on the first attempt in all four profiles.
- `open-exits-four-profile.log`: the independent open-result CALLM exit
  contract passes all four profiles, including the previously regressed
  unchanged formal guard and captured current-iteration producer cases.
- `side-four-profile.log`: the side contract passes all four profiles,
  including depth 15 and full-capacity depth 64, exact parent-edge checks,
  copied admission negatives, GC/code-generation stability, and flush/reentry.
  The final ordinary assertion ARM64 profile is
  `d21be7b2e3cc0a7e9bbf83bd049fddb273a1c533212866a7abf5bcbd36e96194`.
- `arm64-stock-{source,bytecode}.log`: both ordinary ARM64 corpora pass 509
  cases. `arm64-depth65.log` observes all 65 bodies, depth 64, and 32 deepest
  exits over 32 calls after full GC. `arm64-leaf-ordinary.log` passes all 17
  publication/reuse cases and three refusal shapes; this companion alone is
  not a native-execution claim.
- `x64-build.log`: the same source builds ordinary assertion x86_64 with
  `-Werror`, profile
  `41cbcd6645edc81eede4a9d1a87bff46498689a801e8a223cd92f5a33c850833`.
  `x64-stock-{source,bytecode}.log` each passes 509 cases.
- `arm64-topology.log` and `x64-topology.log`: every result/error oracle
  comparison passes. The captured three-result producer row has selected
  root 1 on both targets. ARM64 observes 81 selected exits in each phase;
  x64 observes five then zero after publishing its return/interpreter traces.
  These counts do not establish identical IR or trace topology. The sampled
  protected-leaf row publishes no runner root on either target, so that row
  is not evidence of an architecture-specific native gap.

## Complete gate

The uninterrupted complete gate passed at pinned `bc0313318` in the
`ir-admission` checkout (`complete-gate.log`, outer command exit zero).
It includes all four M7 FFI profiles and actual final VM/recorder safepoint
runtime fixtures, followed by upvalue identity, JIT cell-loop and native
coverage fixtures. The final gate result is OK; no contract was skipped.
`complete-restoration.log` records the successful ordinary assertion ARM64
rebuild without test helpers, profile
`0f1d36d18ed37222162de13bad45465ea629d80e81cf0c1d06187b5bbd57a487`.
This is now the verified source checkpoint for the parity continuation.

Vararg-forwarded Lua tail calls, recursive traces and further FFI/source
restrictions remain under investigation. Full x86_64 parity is not complete.

## Independent continuation rebuild

After promotion, the main parity checkout was independently rebuilt as
ordinary assertion ARM64 without test helpers (`main-build.log`), profile
`1f8b5e602290bdc19ea6bee64e5b3b6815167848eafe5b2181e07dabc4bfdb53`.
Both its source and original-v2-bytecode stock corpora pass all 509 cases
(`main-stock-source.log`, `main-stock-bytecode.log`). The bytecode wrapper was
rerun from its required `tests/stock/test` directory after the initial command
failed to locate `test.lua`; that invocation was not a conformance failure.
