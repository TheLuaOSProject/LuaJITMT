# X64 Hookmask Acquire Loads

The global hook mask is release-published through `hookmask_*` helpers in C.
Generated x86-64 VM code also reads the same byte in hook dispatch and pcall
setup paths, so those loads are now named through `x64_vm_hookmask_acq()`.

The macro still emits a plain `movzx` byte load. On x86-64 this is the intended
TSO acquire-compatible load, matching the existing table/header acquire macros
in `vm_x64.dasc`, while keeping the generated instruction sequence unchanged.

Coverage: `tools/ci/m5_hookmask_atomic.sh` now requires the macro and documents why raw
`byte GL:*->hookmask` loads in `src/vm_x64.dasc`.

Validation:
- `tools/ci/m5_hookmask_atomic.sh` passed.
- `tools/ci/m5_concurrent_objects.sh` passed.
- passed.
- `git diff --check` passed before staging.
