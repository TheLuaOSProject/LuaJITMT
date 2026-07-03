CConv C-to-TValue helper loads

- Routed `lj_cconv_tv_ct_l()` through `ctype_info_acq()` and
  `ctype_size_acq()` before choosing numeric, bool, refarray/struct, and
  copy-out conversion paths.
- Reused the acquired size for integer copy-out, bool normalization, and cdata
  allocation size decisions.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_cdata_set_l` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_carith_l.sh
- git diff --check
