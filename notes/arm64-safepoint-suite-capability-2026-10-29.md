# Safepoint suite capability detection — 2026-10-29

The complete gate at `6cce85d6b` exited zero, but its final
`m5_arm64_jit_fail_closed_safepoint_runtime` reported SKIP. This was an erroneous
harness skip: `tools/test.lua` disables its own JIT to keep control-plane work
interpreted, while the suite used `jit.status() == true` as a build-capability
check. It also assumed trace 1 must be absent in the runner.

The suite now detects the compiled JIT API (`jit.opt.start`) on native macOS
ARM64. Each linked C fixture creates its own VM. Enabling the suite exposed a
second stale harness dependency: recorder completion checks now need
`LJ_GC2_TEST_HELPERS`, but the final gate profile lacked it. The suite compile
flags and the gate's final-runtime profile now include that helper. Earlier
bound gate profiles are unchanged. No production runtime code changed.

A focused replay of the gate's final build and suite passes without SKIP:

- `arm64_vm_safepoint_contract OK`;
- `jit_recorder_safepoint_contract OK`;
- `t-vm-safepoint OK: loop, entry, return and unwind polls acked`;
- `t-jit-recorder-safepoint OK: gated root/side admission and cleanup verified`;
- `ok m5_arm64_jit_fail_closed_safepoint_runtime`.

The outer command exited zero and restored ordinary assertion ARM64 without
helper defines. Logs are retained in
`/Users/frityet/Scratch/ljmt-callm-integrated-20261029/`:
`safepoint-capability-runtime.log` preserves the initial missing-helper compile
failure, and `safepoint-capability-with-hooks.log` is the passing replay.

The earlier full gate remains evidence for the contracts it actually executed.
These two final runtime fixtures have separate passing evidence now; the
entire gate has not been rerun with this harness correction at this checkpoint.
