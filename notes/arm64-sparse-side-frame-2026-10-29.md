# Sparse parent snapshots and deeper side chains

The 133- and 260-branch probes failed before their first side trace on the
previous ARM64 checkpoint. Their parent snapshots reserve seven frame slots
but contain only six live slots. `topslot` describes the reserved Lua frame;
`nslots` describes its live prefix, including the frame header. They do not
have the ordering previously required by side admission. The recorder's
frame-link and stack-map construction, and stack restoration, agree on this
distinction. An independent source review found the same cause.

Both parent-snapshot and entry-SLOAD checks now bound the live prefix by the
reserved frame plus its header. They retain the exact root frame extent,
snapshot identity, live map-entry bounds and live entry-slot bounds. This
does not admit references to dead slots or change snapshot restoration.
A copied-parent negative expands the live prefix beyond the reserved frame
and must fail independently before and after register allocation.

The side lifecycle fixture now includes depths 132 and 260, proves that the
large source actually has a sparse parent snapshot, and exercises repeated
growth/reuse of the recorder-owned ancestry buffer. It retains native entry
and deepest-exit evidence, GC and code/generation stability, scoped leaf and
ancestor retirement, and full flush followed by native reentry on the same
last branch. Its explicit recording/constant/snapshot limits accommodate that
long root path: default limits also prevent the corresponding x64 root from
being recorded. Earlier runs using the shorter first branch for reentry are
superseded by the final same-branch run.

## Evidence

Logs are in `/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029`.

- `large-loop-snapshot-full-reentry-four-profile.log`: complete side contract
  passes ARM64 and arm64e assertion/release profiles, including depths 15,
  64, 69, 80, 132 and 260 and the copied malformed-live-extent negative.
  The command exits zero and restores ordinary assertion ARM64 without test
  helpers, profile
  `87aa3ffe423410e8b163f8c37fec4fa2132887c6e0c6b6d6641e1290ab81a822`.
- `arm64-capacity260-sparse-fixed.log`: the ordinary no-helper probe publishes
  260 native bodies at depth 259. It observes three selected deepest exits
  for three calls and eight for eight calls after full GC; every result
  matches the interpreter oracle. The x64 control also reaches depth 259
  (`x64-capacity260.log`). These are bounded execution witnesses, not a
  trace-topology or performance equivalence claim.
- `large-loop-snapshot-assert.log`: the initial fixture stopped at full-flush
  reentry under insufficient default recording limits. Matching root-only
  controls reproduced default-limit refusal on both architectures. Raising
  `maxrecord`, `maxirconst` and `maxsnap` permits both roots; the final fixture
  therefore keeps the original last branch for reentry.

This closes the measured sparse-parent refusal and extends the tested chain
depth. Full combined validation with numeric-cdata variadic FFI has since
passed at `0dc7716c8`; see
`arm64-sparse-ffi-complete-checkpoint-2026-10-29.md`. General Lua/JIT behavior
parity is not complete.
