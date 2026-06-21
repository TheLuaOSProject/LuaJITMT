CConv compatibility helper loads

- Routed `cconv_err_conv_l()` through `ctype_info_acq()` when naming TValue
  source types for conversion errors.
- Routed `cconv_childqual()` through `ctype_info_acq()` and `ctype_size_acq()`
  while walking child attributes and enum aliases.
- Routed `lj_cconv_compatptr()` through helper-backed info/size loads for
  source struct checks, void/numeric/pointer/struct/function compatibility, and
  equal-size checks.
- Extended `tools/ci/m7_ffi_cdata_set_l.sh` to reject raw `CType.info` and
  `CType.size` reads in those conversion compatibility helper bodies.

Verification:

- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_carith_l.sh
- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m0_source_guard.sh
- git diff --check
