# Complete byte results, numeric sides and wider FFI checkpoint

The complete `tools/ci/arm64_jit_fail_closed_gate.sh` passes at frozen source
`789c1e229262a6e16512951dce916faed51494dd`. This extends the previous
`ff86407a1` checkpoint with wider vararg forwarding, typed integer byte
indices, numeric and negative byte ends, fixed multiple byte results,
dynamic-end multiple results, root-linked NUM FORL children, bounded empty-arm
Lua leaves, captured variadic aggregate prefixes and folded fixed I64/U64
arguments. No runtime locks are added.

The full run includes all four ARM64/ARM64e assertion/release FFI profiles
and finishes with actual VM and recorder safepoint runtime tests, upvalue
identity, source/import loop-cell and native-coverage checks. The outer runner
returned `gate exit=0; restore exit=0`. Ordinary assertion ARM64 was restored
without test helpers, profile
`b875c2070dd756928f88b4ac421837f3d7bc5a3d72f1cc2073abd750add5e27c`.
Evidence is in
`/Users/frityet/Scratch/ljmt-byte-results-num-sides-complete-20261030/`:
`gate-source.txt`, `complete-gate.log`, and `gate-restoration.log`.

The earlier full gate at `d0ee5eb8c` failed to reach a cdata recorder-capture
test seam. The retained failure and three passing exact-profile replays did
not establish a successful full gate. Test-only counters subsequently showed
that a cold attempt could miss recorder entry; the corrected fixture uses
bounded fresh attempts and reports recorder/sample/consume progress, keeping
the original runtime assertions and deadline. Its four-profile contract and
this complete rerun pass. See
[`cdata-capture-race-fixture-2026-10-30.md`](cdata-capture-race-fixture-2026-10-30.md).

The ordinary matrix at `8b3cf5a57` covers the same production behavior before
the test-helper-only capture changes. ARM64 assertions and strict
bare-`-Werror` x86_64 assertions each pass 509 stock source and 509 original
v2-bytecode cases, 260 workload semantic checks, topology probes and fifty
fixed/dynamic-end multiple-byte rows with native witnesses. Eighteen of the
twenty workload rows have native root-exit witnesses. Immutable binaries,
matching JIT modules, archives and hashes are retained in
`/Users/frityet/Scratch/ljmt-byte-results-num-sides-ordinary-20261030/`.
The integration note records exact profile IDs and limits.

Promotion is limited to this frozen checkpoint. Dynamic-positive-first
multiple-byte results and nonnegative FORL I64/U64 argument widening are later
focused integrations. Negative dynamic multiple-byte ranges, raw byte
snapshot values and string-identity tostring remain under development.
General Lua calls/returns, recursion, protected calls, remaining FFI forms
and broader trace topology still prevent full x86_64 parity.

After promotion, the main `codex/arm64-parity` checkout independently rebuilt
ordinary assertion ARM64 without test helpers. Its successful build is recorded
as `main-ordinary-build.log` in the complete-gate evidence directory.
