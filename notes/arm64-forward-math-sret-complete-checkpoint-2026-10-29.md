# Complete Lua forwarding, binary math and variadic result checkpoint

The complete fail-closed gate passes at frozen source `6c46f9bff`. It combines
the verified sparse-frame/numeric-variadic checkpoint with fixed FUNCF open
CALLM, zero-formal CALLMT forwarders targeting one through three fixed
arguments, binary math helpers, and variadic indirect aggregate FFI results.

The uninterrupted gate exits zero, has no skipped checks, and passes all
four ARM64/arm64e assertion/release M7 FFI profiles. The final VM and recorder
safepoint fixtures actually run and pass, followed by upvalue identity,
JIT cell-loop and native-coverage tests. The outer command then successfully
restores ordinary assertion ARM64 without test helpers and exits zero.

Logs under `/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029`:

- `forward-math-sret-complete-gate.log`: complete gate, four M7 passes and
  actual final safepoint runtime checks.
- `forward-math-sret-complete-restoration.log`: ordinary restoration, profile
  `1d6f5c9ae6a780ce216e0a1838e6d61b6734e0446e053b9b19a50c163ff43765`.
- `forward-math-sret-{arm64,x64}-{build,source,bytecode}.log`: the same frozen
  source also passes all 509 source and all 509 original v2-bytecode cases
  in ordinary assertion ARM64 and strict assertion x86_64 builds.
- `forward-math-sret-main-build.log`: after promotion to `codex/arm64-parity`,
  production, tests and CI sources still match `6c46f9bff` exactly. The main
  parity checkout rebuilds successfully as ordinary assertion ARM64 without
  test helpers, profile
  `03430f9f9fd66629949671634e8996dcf06729e5312ad7babb41d1212f952e75`.

This gate does not include the later integer-literal replay fix, CALLXS
integer accumulator PHI, or default-index string.byte change. Those have
their own focused evidence and remain later integration work. Fractional
FORL, guarded child-call continuations and further FFI combinations are
still active. Full x86_64 behavior parity is not complete.
