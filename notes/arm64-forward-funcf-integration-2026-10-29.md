# Vararg forwarding and fixed-root CALLM integration

Source `ceebbf287` combines the sparse/FFI integration with fixed-function
open-result CALLM (`1a0664bd`) and the three-argument vararg tail forwarder
(`6da9e48d6`). Integration cleanup replaces leftover forwarding diagnostic
macros with their unconditional returns and removes unused diagnostic includes
and an empty preprocessor block. No chunk-name diagnostics or conditional
diagnostic rejection paths remain.

Both features share source replay, function identity and frame/snapshot code,
so the combined source was validated before further integration. In
`/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029`:

- `forward-funcf-leaf-four-profile.log`: complete Lua leaf contract passes all
  four ARM64/arm64e assertion/release profiles, including fixed/nested calls,
  varargs, open CALLM vectors, child exits, normalized arguments, primitive and
  multi-result returns, sparse maps and ordinary publication/refusal cases.
- `forward-funcf-open-four-profile.log`: open-CALLM exit contract passes all
  four profiles, retaining current-iteration captured producer arguments,
  selected consumer identity exits, frame/MULTRES restoration, negative
  copies, live peer and exact code reuse across GC.
- `forward-funcf-root-assert.log` and `forward-funcf-root-release.log`: fixed
  root recording contracts pass ARM64 and arm64e in both build modes.

The whole sequence exits zero and restores ordinary assertion ARM64 without
test helpers, final profile
`0b34717ca5485f9d6b28cf1a10161f56285198b5d469e4bb185f6b79f2e7cce4`.
Ordinary combined stock corpora and the full gate for these newer Lua changes
remain pending. The separately running full gate is pinned to the preceding
sparse/FFI source and must not be attributed to this integration.
