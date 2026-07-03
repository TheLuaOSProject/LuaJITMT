# Hook Function And Count Atomics

Global hooks remain universe-wide for v1, but the neighboring hook state now
matches the hookmask helper model:

- `hookf_load()`/`hookf_store()` acquire/release the global hook callback.
- `hookcount_load()`, `hookcstart_load()`, `hookcount_setstart()`, and
  `hookcount_reset()` acquire/release the shared count-hook state.
- `lua_sethook()` publishes the hook callback and count start before publishing
  the event mask bits, so a reader that observes a new mask can acquire the
  matching callback/count state.
- `lj_dispatch_ins()` reloads the count atomically and resets from the acquired
  start count. It treats `<= 0` as expired so racing count decrements cannot
  strand the shared counter below zero.
- x86-64 `vm_record` and `vm_inshook` now use `lock; dec` for the generated VM
  hookcount decrement. Hooks are already slow-path dispatch targets; this keeps
  the shared counter atomic without changing the field layout.

The non-x64 DASC paths remain out of scope for this x86-64 Linux pass.

Coverage: `m5_hook_state_atomic` invariant: raw C-side `hookf`/`hookcount`
field access outside the helper definitions, requires the x64 locked
decrements, and runs the `m5_hook_state_atomic` Lua smoke.

Validation:
- `tools/ci/m5_hook_state_atomic.sh` passed.
- `tools/ci/m5_hookmask_atomic.sh` passed.
- passed.
- `tools/ci/m5_concurrent_objects.sh` passed.
- `git diff --check` passed before staging.
