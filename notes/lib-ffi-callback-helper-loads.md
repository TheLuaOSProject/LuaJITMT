lib_ffi callback helper loads

- Routed `ffi_callback_set()` through `ctype_info_acq()` and
  `ctype_size_acq()` before checking that the cdata value is a pointer-sized
  callback function pointer.
- Preserved the existing fixed callback slot lookup and publish/free ordering;
  this slice only replaces raw ctype payload reads with helper-backed
  snapshots.
- Extended `tools/ci/m7_ffi_callback_install.sh` to reject raw `CType.info`
  and `CType.size` reads in the callback install helper body.

Verification:

- tools/ci/m7_ffi_callback_install.sh
- tools/ci/m7_ffi_callback_runtime.sh
- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m0_source_guard.sh
- git diff --check
