lj_ccall vararg helper loads

- Routed `lj_ccall_ctid_vararg()` through `ctype_info_acq()` and
  `ctype_size_acq()` when inferring vararg destination ctypes for cdata values.
- Covered cdata float-to-double vararg promotion and array-to-pointer vararg
  inference with a small `snprintf()` call in `tests/t-ffi-cdata-set-l.lua`.
- Extended `tools/ci/m7_ffi_cdata_set_l.sh` to reject raw `CType.info` and
  `CType.size` reads in `lj_ccall_ctid_vararg()`.

Verification:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_cdata_get_l.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
