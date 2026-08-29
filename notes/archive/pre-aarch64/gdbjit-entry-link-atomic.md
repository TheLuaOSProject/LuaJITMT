# GDBJIT entry-link atomic helpers

## 2026-06-20

- Added local acquire/release helpers in `src/lj_gdbjit.c` for:
  - `GDBJITentry.next_entry`
  - `GDBJITentry.prev_entry`
  - `__jit_debug_descriptor.first_entry`
  - `__jit_debug_descriptor.relevant_entry`
  - `__jit_debug_descriptor.action_flag`
- Routed opt-in GDB JIT add/delete publication through those helpers while
  preserving the existing `gdbjit_lock_acquire()` / `gdbjit_lock_release()`
  critical section and the standard `__jit_debug_register_code()` notification.
- Added GDBJIT opt-in build coverage so `LUAJIT_USE_GDBJIT` compiles cleanly
  and publishes/deletes trace descriptors under runtime trace churn.
- Follow-up: the suite case cleans `src` after the opt-in build as well, so
  incremental default-build launchers do not inherit GDBJIT-compiled objects.
- Follow-up descriptor-lock wait slice: the opt-in GDBJIT descriptor lock still
  uses `la_cas32()`/`la_store32_rel()` for debugger-required descriptor
  serialization, but a contended acquire now waits through
  `lj_thr_sleep_ns(L, 1000000)` when a Lua state is available. That marks the
  owning TG as native and acknowledges safepoint actions without throwing
  STOPREQ from inside trace add/delete cleanup.

## Validation

- `tools/ci/lua_test.sh m6_jit_gdbjit_publish`
- `tools/ci/lua_test.sh m6_jit_alloc_account` immediately after the GDBJIT case

## Scope notes

- GDBJIT remains opt-in and is not part of the normal build unless
  `-DLUAJIT_USE_GDBJIT` is supplied.
- This check validates compilation and runtime trace descriptor publication; it
  does not attach GDB or exercise debugger-side descriptor consumption.
