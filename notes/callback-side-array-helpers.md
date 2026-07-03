Callback side-array helper slice

- Added `ctype_cb_*()` helpers for callback id, owner, function side-array
  pointer publication/acquire loads, callback table size publication, id-slot
  loads/stores, and owner-slot acquire/release/CAS operations.
- Routed callback slot initialization, slot claiming, callback free, runtime
  entry lookup, legacy GC/GC2 side-root scans, GC2 paranoia checks, and CTState
  teardown through the helper API.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_callback_install` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_callback_install.sh
- tools/ci/m7_ffi_callback_runtime.sh
- tools/ci/m8_weak.sh
- git diff --check
