lib_ffi callback helper loads

- Routed `ffi_callback_set()` through `ctype_info_acq()` and
  `ctype_size_acq()` before checking that the cdata value is a pointer-sized
  callback function pointer.
- Preserved the existing fixed callback slot lookup and publish/free ordering;
  this slice only replaces raw ctype payload reads with helper-backed
  snapshots.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_callback_install` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_callback_install.sh
- tools/ci/m7_ffi_callback_runtime.sh
- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- git diff --check
