# Hookmask Atomic Helpers

`global_State.hookmask` remains a byte so the x86-64 VM and recorder offset
assumptions stay intact, but C-side runtime access now goes through
`hookmask_*` helpers in `lj_obj.h`.

- `hookmask_load()`/`hookmask_store()` provide acquire/release whole-mask
  access for the profiler paths that already coordinate dispatch updates.
- `hookmask_update()` handles bit clear/set transitions for hook enter/leave,
  `HOOK_GC`, `HOOK_VMEVENT`, `HOOK_PROFILE`, and `lua_sethook()` event bits
  with a CAS loop.
- `hookmask_restore_()` preserves the current event bits while restoring saved
  non-event bits, matching the old `hook_restore()` shape without raw stores.
- `hookmask_set_if_clear()` lets the profiler timer publish `HOOK_PROFILE`
  only when profile/vmevent/GC bits are still clear.

The remaining raw `&J2G(J)->hookmask` in `lj_record.c` is intentional: trace
IR records a volatile memory load of the hookmask byte for generated code.
`lj_asm_*` hookmask references are likewise backend memory operands for
generated traces. VM/dasc hookmask byte loads remain a separate generated-code
follow-up.

Guard: `m5_hookmask_atomic` invariant: raw C-side `->hookmask` access
outside `lj_obj.h`, the recorder field-address exception, and backend
generated-code operands, then runs the `m5_hookmask_atomic` Lua smoke.

Validation:

- `tools/ci/m5_hookmask_atomic.sh`: passed.
- `tools/ci/m5_state_owner.sh`: passed.
-: passed.
- `tools/ci/m5_concurrent_objects.sh`: passed.
- `git diff --check`: passed.
