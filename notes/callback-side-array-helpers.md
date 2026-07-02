Callback side-array helper slice

- Added `ctype_cb_*()` helpers for callback id, owner, function side-array
  pointer publication/acquire loads, callback table size publication, id-slot
  loads/stores, and owner-slot acquire/release/CAS operations.
- Routed callback slot initialization, slot claiming, callback free, runtime
  entry lookup, legacy GC/GC2 side-root scans, GC2 paranoia checks, and CTState
  teardown through the helper API.
- Documented the invariant formerly checked by `m7_ffi_callback_install`: raw
  implementation-side `cts->cb.cbid`, `cts->cb.owner`, `cts->cb.func`, and
  `cts->cb.sizeid` access outside the helper definitions.

Verification:

- tools/ci/m7_ffi_callback_install.sh
- tools/ci/m7_ffi_callback_runtime.sh
- tools/ci/m8_weak.sh
- git diff --check
