# Complete byte bounds, numeric integer bodies and variadic prefixes checkpoint

The full `tools/ci/arm64_jit_fail_closed_gate.sh` passes at frozen revision
`ff86407a1b656471d9523c477c443eeb44b5ba4c`. Production code matches
`4b8c2f880`; the later test correction updates the old unsupported variadic
aggregate-prefix expectation to the newly supported exact ABI layout.
This adds guarded integer bodies in NUM FORL roots, constant negative byte
indices, dynamic positive byte ends, and named aggregate prefixes in variadic
aggregate-return FFI calls to the previous complete `89226814d` checkpoint.
No runtime locks are added.

The complete run includes all four assertion/release ARM64/ARM64e FFI
profiles and finishes with actual VM and recorder safepoint runtime tests,
upvalue identity, source/import loop-cell and native-coverage checks. The
outer runner returned `gate exit=0; restore exit=0`. The gate checkout was
restored to ordinary assertion ARM64 without helper defines, profile
`96299af6fef18d9e9072bf43bf2f734cf8631f81e94d992055d64a2267bab9b5`.

Evidence is in
`/Users/frityet/Scratch/ljmt-byte-num-prefix-integration-20261030/`:
`gate-rerun-source.txt`, `complete-gate-rerun.log`, and
`gate-rerun-restoration.log`. The initial `complete-gate.log` is retained:
it stopped at the stale aggregate-prefix test expectation. The corrected
aggregate/vararg contracts passed all four profiles before the full rerun.

The ordinary matrix at `4b8c2f880` passes all 509 stock source cases and all
509 original v2-bytecode cases on both ARM64 and strict `-Werror` x86_64.
Each architecture passes 260 workload semantic checks with 18 of 20 sampled
rows providing root-exit witnesses; the two rows without witnesses establish
no native-execution claim. The topology and focused numeric loop, CALLM,
and string-byte probes are retained alongside the immutable ordinary binaries,
archives and matching JIT modules. Source and CI equality from that ordinary
revision to the full-gate revision was checked; only the ABI fixture and notes
differ. The initial scratch matrix restoration failed because its runner
unset a required shell variable. `ordinary-arm64-restoration-retry.log`
records the corrected successful restoration. This was a runner failure,
not a successful initial whole-script result.

Promotion is limited to this frozen revision. Wider forwarding, numeric and
negative byte ends, integer byte indices, captured FFI prefixes and folded
64-bit literals on the later integration branch are not covered by this full
gate. Numeric loop children, general calls/returns, recursion, protected
calls, remaining FFI forms and broader topology still prevent full parity.
