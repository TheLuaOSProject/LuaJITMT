CConv bitfield helper loads

- Routed `lj_cconv_tv_bf_l()` through `ctype_info_acq()` before decoding
  bitfield container size, position, signedness, and bool flags.
- Routed `lj_cconv_bf_tv_l()` through `ctype_info_acq()` before converting and
  masking bitfield writes.
- Extended `tools/ci/m7_ffi_cdata_set_l.sh` to reject raw `CType.info` and
  `CType.size` reads in the guarded conversion helper bodies.

Verification:

- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_carith_l.sh
- tools/ci/m0_source_guard.sh
- git diff --check
