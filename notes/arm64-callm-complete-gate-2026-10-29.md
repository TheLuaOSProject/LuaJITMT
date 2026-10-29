# Complete CALLM checkpoint gate — 2026-10-29

The uninterrupted `tools/ci/arm64_jit_fail_closed_gate.sh` run passed at
`6cce85d6b` in the `ir-admission` worktree. This includes the bounded matching
one/two/three-result Lua CALL-to-CALLM work and the exact consumer exit fixture.
It does not include subsequent arity normalization, deeper ancestry, captured
consumer replay, or indirect composite result work.

Evidence is retained outside the repository in
`/Users/frityet/Scratch/ljmt-callm-integrated-20261029/complete-gate.log`.
The final line reports `arm64_jit_fail_closed_gate OK`. Nested contracts
completed; deliberately injected protected VM failures are expected test output.
The final M5 safepoint runtime suite erroneously skipped because it checked the
runner's disabled engine instead of compiled JIT capability. The correction and
separate passing runtime replay are recorded in
`arm64-safepoint-suite-capability-2026-10-29.md`; the full gate result must not
be interpreted as having executed that skipped suite.
The final helper profile was
`0249d579aeb66578f14a74753ed012be14a5b01e5ae4728a0f3391a9ee20ccae`.

Afterward, the worktree was rebuilt as ordinary assertion ARM64 with bootstrap
and experimental JIT enabled and no helper defines. That build passed with
profile `11c4efdcdd078b0c2704600d1ec77a23d473dd50f683ffcc6eb165c9dd08f66a`;
its log is `complete-gate-restoration.log` in the same evidence directory.
The full outer command exited zero. The source tree remained clean.

The ordinary 509-case source and bytecode corpora and the focused native
identity-exit evidence at this production checkpoint are recorded in
`arm64-jit-open-result-callm-2026-10-29.md` and
`arm64-open-callm-consumer-exits-2026-10-29.md`. This passing bounded gate does
not establish full x86_64 parity; the remaining port goal is still active.
