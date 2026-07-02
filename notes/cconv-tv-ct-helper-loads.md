CConv C-to-TValue helper loads

- Routed `lj_cconv_tv_ct_l()` through `ctype_info_acq()` and
  `ctype_size_acq()` before choosing numeric, bool, refarray/struct, and
  copy-out conversion paths.
- Reused the acquired size for integer copy-out, bool normalization, and cdata
  allocation size decisions.
- Documented the invariant formerly checked by `m7_ffi_cdata_set_l`: raw `CType.info` and
  `CType.size` reads in this guarded conversion helper body.

Verification:

- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_carith_l.sh
- git diff --check
