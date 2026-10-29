# Complete ancestry, CALLM and composite FFI checkpoint

The uninterrupted `tools/ci/arm64_jit_fail_closed_gate.sh` run passed at
production source `0641fba1e` in the pinned `ir-admission` checkout. The outer
command exited zero after rebuilding ordinary assertion ARM64 without test
helpers. This source was promoted to `codex/arm64-parity` through `f2464d183`.

The gate log is
`/Users/frityet/Scratch/ljmt-side-ancestry-20261029/allocator-complete-gate.log`.
It ends with `arm64_jit_fail_closed_gate OK`. Its final runtime suite actually
executes `t-vm-safepoint` and `t-jit-recorder-safepoint`, followed by
`ok m5_arm64_jit_fail_closed_safepoint_runtime`; it does not repeat the skipped
runtime suite from the older `6cce85d6b` gate. All four combined foreign-call
profiles also finish in this uninterrupted run.

The restoration log is `allocator-complete-restoration.log` in the same
directory. Its ordinary ARM64 profile is
`105d1c6fd99b267168d73e104ac6d2c5a56f0490924e7b1216f974a768163383`, with only
bootstrap, experimental JIT and assertion flags. The pinned source checkout
was clean after the run.

The promoted `arm64-parity` checkout was independently rebuilt as ordinary
assertion ARM64, profile
`0d3175d4222ee7b3eb9f761cca7692268deb503e5c7dff6dbcc9eff339acc820`. It passes
all 509 source tests and all 509 original-v2 bytecode tests, the ordinary
depth-three side probe (including GC and native reentry), and the CALLM
consumer-exit interpreter companion. Evidence files in the same directory
are `main-promotion-build.log`, `main-promotion-stock-source.log`,
`main-promotion-stock-bytecode.log`, `main-promotion-depth.log` and
`main-promotion-callm.log`. The interpreter companion is not a native-entry
proof; the native contracts are covered by the gate above.

This checkpoint includes CALLM consumer arity normalization and captured
consumer replay, zero-formal consumers and one/two-result zero-formal
producers, bounded side ancestry for integer FORL and numeric while roots,
middle/root ancestor teardown, bounded MAP_JIT range recovery, 17–32-byte
non-HFA integer composite FFI results, and pointer fields in direct small
composite results. The four-profile side matrix, actual range-extension
evidence and ordinary ARM64/x86_64 source/bytecode corpora are documented in
`arm64-captured-ancestry-integration-2026-10-29.md` and its linked notes.

Discarded numeric CALLM DAGs, caller-side iterator early break, the transient
three-result zero-formal producer window and pointer-containing by-value FFI
arguments are later work. None is claimed covered by this gate. Full x86_64
parity remains incomplete.
