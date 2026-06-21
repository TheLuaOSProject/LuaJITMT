lib_ffi entry validation helper loads

- Routed `ffi_metatype()` through `ctype_info_acq()` before checking struct,
  complex, and vector eligibility for a metatype.
- Routed `ffi_gc()` through `ctype_info_acq()` before checking that a cdata
  value can carry a finalizer.
- Routed `ffi_blocking()` through `ctype_info_acq()`/`ctype_size_acq()` before
  pointer unwrapping, function validation, and callback-blacklist marking.
- Extended the metatype, finreg, and blocking gates to reject raw
  `CType.info`/`CType.size` reads in the corresponding FFI entry bodies.

Verification:

- `tools/ci/m7_ffi_metatype.sh`
- `tools/ci/m7_ffi_finreg.sh`
- `tools/ci/m7_ffi_blocking.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
