lj_ccall vararg helper loads

- Routed `lj_ccall_ctid_vararg()` through `ctype_info_acq()` and
  `ctype_size_acq()` when inferring vararg destination ctypes for cdata values.
- Covered cdata float-to-double vararg promotion and array-to-pointer vararg
  inference with a small `snprintf()` call in `tests/t-ffi-cdata-set-l.lua`.
- Documented the invariant formerly checked by `m7_ffi_cdata_set_l`: raw `CType.info` and
  `CType.size` reads in `lj_ccall_ctid_vararg()`.

Verification:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_cdata_get_l.sh`
- `git diff --check`
