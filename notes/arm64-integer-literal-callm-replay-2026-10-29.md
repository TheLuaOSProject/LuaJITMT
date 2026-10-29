# Integer literals in Lua CALLM replay

Matched ordinary probes at `6c46f9bff` distinguish mixed return values from
integer literal replay. Both architectures already run `return x,.25` and
`return .25,x` producers natively. ARM64 refuses `return x+.5,1` and
`return 1,x+.5`, while x64 records and enters both. The roots call the producer,
forward its open results to a fixed consumer, and return one scalar value.

Six recorder-side literal lookups passed an instruction reference as the
`numonly` boolean. Every such nonzero reference forced a KNUM lookup, even
when the source uses a KSHORT or an integer KNUM literal. The independent
allocated-view replay already passes zero in the corresponding places.
The fix passes zero for those six source literal lookups. Existing explicit
integer-to-NUM conversion still passes one; negative-zero handling and exact
constant-value matching remain unchanged. No emitter or runtime path changes.

The ordinary no-helper probe then observes eight selected producer-identity
exits for eight calls, before and after full GC, for both formerly refused
literal cases. The direct mixed-value controls remain native. The separate
`return x+1,.25` arithmetic producer still refuses on ARM64 and is not claimed
fixed here.

The FUNCF fixture adds native producers using -17 (KSHORT) and 65537 (KNUM),
in opposite return positions. Each has an independent interpreted oracle,
live threading peer, exact native entry, GC/code reuse and producer identity
exit followed by reentry. Private copies changing the integer constant reject
independently in both semantic and allocated admission. Existing arity and
result-map corruptions remain in the same fixture.

Evidence directory:
`/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029`.

- `mixed-callm-probe.lua`, `mixed-callm-arm64-before.log`, and
  `mixed-callm-x64-before.log`: matching source/result/selected-exit probes.
  The preserved x64 control is from `bc0313318`, with the same source probe.
- `mixed-callm-literal-build.log`, `mixed-callm-literal-probe.log`: ordinary
  assertion ARM64 after the literal fix, including the remaining arithmetic
  refusal.
- `mixed-callm-literal-funcf-assert.log`: complete FUNCF recording contract
  passes assertion ARM64 and arm64e, including the two literal positives and
  two literal negatives per admission pass. The command exits zero and
  restores ordinary assertion ARM64 without helpers, profile
  `6702de125d40816264be34dddd2a1be12e91968dc30e60b9c2657cad7864212c`.

The production checkpoint is `fb5157fb9`. Independent read-only review found
no actionable defect in the literal selection or private mutation fixtures.
Subsequent validation passes:

- `mixed-callm-literal-funcf-release.log`: complete FUNCF recording contract
  on release ARM64 and arm64e. Together with the assertion run, all four
  profiles execute the new native positives and both literal mutations.
- `mixed-callm-literal-leaf-four-profile.log`: complete Lua-call contract on
  all four profiles, including nested calls, varargs, open result vectors,
  child exits, sparse maps, argument normalization and multiple returns.
- `mixed-callm-literal-open-four-profile.log`: complete open-CALLM exit
  contract on all four profiles, including current-iteration producer
  arguments, frame/MULTRES restoration, live peer and exact GC/code reuse.

The combined command exits zero and restores ordinary assertion ARM64 without
test helpers, profile
`3a2a22d783839ace0e1551282d1207ead2f27f4de513fe92a46cb88dfa8a006d`.

`mixed-callm-addint-{arm64,x64}-dump.log` records the remaining arithmetic
case: ARM64 records an integer ADDOV with a child-frame overflow snapshot;
the preserved x64 control records NUM arithmetic. That extra guarded child
continuation is outside the literal fix. Full combined-gate validation of
this later fix and general Lua/JIT parity remain incomplete.
