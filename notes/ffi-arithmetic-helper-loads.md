FFI arithmetic helper loads

- Routed `carith_checkarg()` through `ctype_info_acq()` and `ctype_size_acq()`
  while normalizing cdata, pointer/reference/function, enum, and enum-string
  operands.
- Routed `carith_ptr()` through helper-backed payload reads for pointer/refarray
  classification, pointer differences, pointer-plus-index allocation, and
  swapped pointer/index operands.
- Routed `carith_int64()`, `lj_carith_meta()`, and `lj_carith_check64()`
  through helper-backed payload reads for numeric rank selection, metamethod
  lookup/error classification, and bit-library cdata operands.
- Extended `tools/ci/m7_ffi_carith_l.sh` to reject raw `CType.info` and
  `CType.size` reads in `src/lj_carith.c`.

Verification:

- tools/ci/m7_ffi_carith_l.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m0_source_guard.sh
- git diff --check
