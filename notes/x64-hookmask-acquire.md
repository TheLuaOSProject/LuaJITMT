# X64 Hookmask Acquire Loads

The global hook mask is release-published through `hookmask_*` helpers in C.
Generated x86-64 VM code also reads the same byte in hook dispatch and pcall
setup paths, so those loads are now named through `x64_vm_hookmask_acq()`.

The macro still emits a plain `movzx` byte load. On x86-64 this is the intended
TSO acquire-compatible load, matching the existing table/header acquire macros
in `vm_x64.dasc`, while keeping the generated instruction sequence unchanged.

Coverage: `m5_hookmask_atomic` and `m5_concurrent_objects` own the observable
hook-mask behavior. Raw `byte GL:*->hookmask` loads in `vm_x64.dasc` must stay
behind the documented macro instead of source-text matching.

Validation:
- `tools/ci/lua_test.sh m5_hookmask_atomic` passed.
- `tools/ci/lua_test.sh m5_concurrent_objects` passed.
- `git diff --check` passed before staging.
