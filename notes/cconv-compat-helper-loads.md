CConv compatibility helper loads

- Routed `cconv_err_conv_l()` through `ctype_info_acq()` when naming TValue
  source types for conversion errors.
- Routed `cconv_childqual_l()` through ctype snapshots plus
  `ctype_info_acq()`/`ctype_size_acq()` while walking child attributes and enum
  aliases.
- Routed `lj_cconv_compatptr_l()` through helper-backed info/size loads for
  source struct checks, void/numeric/pointer/struct/function compatibility, and
  equal-size checks. Struct/union exact-type compatibility now compares the
  resolved CType IDs instead of raw `CType *` table-slot addresses, preserving
  stock struct-to-pointer conversions with local snapshots.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_cdata_set_l` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_carith_l.sh
- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- git diff --check
