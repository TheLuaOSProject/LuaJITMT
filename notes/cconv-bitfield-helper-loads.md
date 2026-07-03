CConv bitfield helper loads

- Routed `lj_cconv_tv_bf_l()` through `ctype_info_acq()` before decoding
  bitfield container size, position, signedness, and bool flags.
- Routed `lj_cconv_bf_tv_l()` through `ctype_info_acq()` before converting and
  masking bitfield writes.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_cdata_set_l` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_carith_l.sh
- git diff --check
