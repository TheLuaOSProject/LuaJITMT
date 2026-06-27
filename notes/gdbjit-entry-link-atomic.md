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
- Added `tools/ci/m6_jit_gdbjit_publish.sh` to reject raw descriptor-link
  access and compile a clean `LUAJIT_USE_GDBJIT` build.
- Follow-up: the script now cleans `src` after the opt-in build as well, so
  incremental default-build launchers do not inherit GDBJIT-compiled objects.
- Follow-up descriptor-lock wait slice: the opt-in GDBJIT descriptor lock still
  uses `la_cas32()`/`la_store32_rel()` for ownership, but a contended acquire
  now waits through `lj_thr_sleep_ns(NULL, 1000000)` instead of spinning on
  `la_cpu_pause()`. The CI wrapper rejects reintroducing the pause loop.

## Validation

- `tools/ci/m6_jit_gdbjit_publish.sh`
- `tools/ci/m6_jit_alloc_account.sh` immediately after the GDBJIT script

## Scope notes

- GDBJIT remains opt-in and is not part of the normal build unless
  `-DLUAJIT_USE_GDBJIT` is supplied.
- This check validates source shape and compilation; it does not attach GDB or
  exercise debugger-side descriptor consumption.
