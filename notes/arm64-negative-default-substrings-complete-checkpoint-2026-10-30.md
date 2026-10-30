# Negative and omitted-end substring complete checkpoint

Production source `52efb731ed68a8202f4246fe61c813d91cd39a43` passes the complete
ARM64 experimental gate. It extends the prior `503c8ba2f` checkpoint with
positive literal first indices and negative literal last indices, plus omitted
last arguments. The exact source CALL, same-string length, checked width,
nonnegative extent guards and snapshot arguments are independently validated
before and after register allocation. Every repeated function guard must retain
its exact pure CALL-suffix continuation and zero root-frame footer tag.
The existing lockless string allocator and ARM64 emitter are reused.

## Complete gate

The frozen gate checkout ran `tools/ci/arm64_jit_fail_closed_gate.sh` and
returned zero; its enclosing ordinary ARM64 restoration also returned zero.
Evidence is saved under
`/Users/frityet/Scratch/ljmt-negative-default-substring-complete-20261030/`:
`gate-source.txt`, `complete-gate.log` and `gate-restoration.log`.
All component contracts, all four M7 FFI profiles, and the actual VM/recorder
safepoint, upvalue identity, cell-loop and native-coverage runtimes pass.
The restored ordinary assertion ARM64 profile is
`14a6a27ad356108804b4d6f198eea6db5dcefa4fc9faa6bc20825381149d5268`.

## Ordinary cross-architecture evidence

Exact-source ARM64 and strict x86_64 ordinary builds pass all 509 source and
509 original v2-bytecode cases, all 260 inventory comparisons, and the previous
byte/FFI/string/tostring native matrix. Added cases include both loop forms,
four omitted-end rows, six extreme negative-end rows and six extreme
omitted-end rows (first indices 1, 2 and 32767). Selected-root exits, oracle
results, live peers and machine-code identity across full GC are checked.
Saved binaries, matching JIT modules, archives, hashes and logs are under
`/Users/frityet/Scratch/ljmt-default-substring-ordinary-20261030/`.
The focused four-profile integration evidence and earlier failed fixture logs
remain in `/Users/frityet/Scratch/ljmt-default-substring-integration-20261030/`.

This gate excludes the later sequential-leaf, numeric-identity, zero-first,
fixed-pair byte and negative-first substring work. General call/return and
trace topology restrictions remain; this is an incremental verified
checkpoint, not full x86_64 parity.
