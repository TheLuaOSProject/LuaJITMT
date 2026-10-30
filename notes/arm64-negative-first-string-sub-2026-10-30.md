# ARM64 negative-first `string.sub` slice

The matched ordinary baseline at `9cc11ead869ba237b1fcddba33e6b4d63704cf77`
published the `sub(s, -4)` and `sub(s, -4, -1)` roots on x86_64, but not on
ARM64. The baseline bundles and probe output are preserved under
`/Users/frityet/Scratch/ljmt-zero-substring-identity-ordinary-20261030/` and
`/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/`.

This slice admits a fixed negative KSHORT first index, with either the omitted
end (Lua's `-1`) or explicit literal `-1`. The regression matrix covers first
indices `-1`, `-4`, and `-32768`, both endpoint arities, and both FORL and while
loops. It proves the `IR_ADD` index uses
the length field of the same guarded source string, and requires the dominating
`index >= 0` guard. The allocation width must be the guarded `IR_SUBOV`
`length - index`, followed by `width >= 0`. Semantic and post-RA admission
check these relationships independently. The bytecode decoder binds the exact
source `string.sub` CALL, setup copies, endpoint arity/literal, frame
slots, and rejects control-flow edges that bypass the setup.

Inputs shorter than the absolute first index take the normal guard exit and
resume the stock interpreter's negative-index clamping behavior. Empty, short,
NUL-containing, and high-byte inputs are covered. Boundary lengths are derived from each
negative first index and include one below, exact, and one above the threshold.
Fresh strings vary the returned suffix so the publication witness cannot be
satisfied by an already-interned result. Dynamic negative first/end values and
other endpoint literals remain outside this slice.

For first index `-32768`, successful results use the large-arena allocation
path. The fixture permits that path only for this tested case; its ordinary
small-cell readiness assertion remains active for smaller results. For huge
strings it verifies the matching HugeTab entry is traversable and has the
published `LJ_HUGEF_READY` bit. The
single-byte result at first index `-1` may already be interned, so a fresh
publication is required for while forms except that one-byte case. All forms
still require native match and selected-root exits.

Validation:

- `tools/ci/arm64_jit_string_sub_contract.sh` passed all four ARM64/arm64e
  assert/release profiles. Full output:
  `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/final-four-profile.log`.
- Matched ordinary live-peer probes used the preserved 9cc source/runtime
  bundles. x86_64 published the omitted and explicit forms, observed selected
  root exits before and after peer GC, and retained code identity; ARM64 had no
  root before this change. Logs:
  `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/ordinary-x86_64.log`
  and
  `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/ordinary-arm64.log`.
- Candidate ordinary binaries were built from commit `56d60a3a9b4f2763d9a16d90798bb7c97049b9b0`
  with no test-helper flags, then preserved with their Lua modules and hashes
  under
  `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/candidate-ordinary/`.
  The live-peer probe exercised all 12 index/arity/loop combinations on both
  architectures, verified short-string results, observed selected-root exits
  after peer GC, and checked machine-code identity:
  `candidate-ordinary/arm64-probe.log` and `candidate-ordinary/x86_64-probe.log`.
- Expanded four-profile output, including the three index values and fresh
  publication checks:
  `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/final-expanded-four-profile.log`.
- After correcting named-loop selection for the omitted-end cases and checking
  HugeTab readiness, the final four-profile rerun passed at:
  `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/final-corrections-four-profile.log`.
- The native fixture includes independent semantic and post-RA mutations for
  the lower-bound guard and a different same-typed string length source, in
  addition to source literal/arity, map, footer, allocation, and location
  checks. Explicit endpoint mutations verify the `-1` literal is part of the
  accepted source shape.
