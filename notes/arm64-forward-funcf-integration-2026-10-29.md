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
The full gate for these newer Lua changes remains pending. The separate full
gate at preceding sparse/FFI source `0dc7716c8` has passed; it must not be
attributed to this integration.

## Ordinary combined validation

Source `9812cc401` also contains the binary math implementation and expanded
contract from `daed68c5c`. Ordinary assertion ARM64 and strict (`-Werror`)
x86_64 builds each pass all 509 source cases and all 509 original v2-bytecode
cases. The binary math probe observes the selected root exit eight times for
eight calls, before and after full GC, for atan2, pow and ldexp on both
architectures. Logs share the `forward-funcf-math-` prefix in the evidence
directory above. The final ordinary ARM64 restoration has profile
`1b8b3a36075430136d2c8a4f0be5f14819e352bde5b122aa9e12c5d24eeb55dd`.

`434257da7` removes an unused J parameter from the static CALLM result-count
helper and its sole call. The strict IR contract passes on that source,
including its arm64e BTI compilation with extra warnings treated as errors.
See `forward-funcf-math-strict-build.log` and
`forward-funcf-math-strict-contract.log`; bound ordinary ARM64 profile
`05e40702f944e194e7b5058ea92aeacf020eeb4f6de9282f00d807bcba258041`.

The later merge `b1a8d3270` adds fixed forwarding arities one through three
and variadic indirect FFI results. Those features passed their independent
four-profile contracts. At documentation checkpoint `6c46f9bff`, ordinary
assertion ARM64 and strict assertion x86_64 each pass all 509 source and all
509 original v2-bytecode cases. The `forward-math-sret-{arm64,x64}-` build,
source and bytecode logs record these runs. The outer command exits zero
and restores ordinary assertion ARM64 without test helpers, profile
`880de035709cb8f99f56b6b6ec581edf64f3e34afb69a3515657ed49d0f690bd`.
The complete gate is running separately at frozen `6c46f9bff`.
None of these bounded results establishes general x86_64 parity.
