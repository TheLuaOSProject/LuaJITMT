# ARM64 negative-first `string.sub` slice

The matched ordinary baseline at `9cc11ead869ba237b1fcddba33e6b4d63704cf77`
published the `sub(s, -4)` and `sub(s, -4, -1)` roots on x86_64, but not on
ARM64. The baseline bundles and probe output are preserved under
`/Users/frityet/Scratch/ljmt-zero-substring-identity-ordinary-20261030/` and
`/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/`.

This slice admits a fixed negative KSHORT first index, with either the omitted
end (Lua's `-1`) or explicit literal `-1`. It proves the `IR_ADD` index uses
the length field of the same guarded source string, and requires the dominating
`index >= 0` guard. The allocation width must be the guarded `IR_SUBOV`
`length - index`, followed by `width >= 0`. Semantic and post-RA admission
check these relationships independently. The bytecode decoder binds the exact
one-argument `string.sub` CALL, setup copies, endpoint arity/literal, frame
slots, and rejects control-flow edges that bypass the setup.

Inputs shorter than four bytes take the normal guard exit and resume the stock
interpreter's negative-index clamping behavior. Empty, short, NUL-containing,
and high-byte inputs are covered. Dynamic negative first/end values and other
endpoint literals remain outside this slice.

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
- The native fixture includes independent semantic and post-RA mutations for
  the lower-bound guard, in addition to the existing source, map, footer,
  allocation, and location checks.
