lj_ccall vararg helper loads

- Routed `lj_ccall_ctid_vararg()` through `ctype_info_acq()` and
  `ctype_size_acq()` when inferring vararg destination ctypes for cdata values.
- Covered cdata float-to-double vararg promotion and array-to-pointer vararg
  inference with a small `snprintf()` call in `tests/t-ffi-cdata-set-l.lua`.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_cdata_set_l` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_cdata_get_l.sh`
- `git diff --check`
