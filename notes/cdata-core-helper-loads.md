CData core helper loads

- Routed `lj_cdata_free()` through `ctype_info_acq()` and `ctype_size_acq()`
  before computing fixed-size cdata free sizes.
- Routed `lj_cdata_index_l()` through helper-backed payload reads while
  resolving references, attributes, numeric indexing, string-field lookup,
  CTypeID constructor constants, complex fields, and pointer auto-deref.
- Routed `cdata_getconst()`, `lj_cdata_get_l()`, and `lj_cdata_set_l()` through
  helper-backed info/size loads for const/bitfield, child, ref, attribute, and
  write-const checks.
- Extended `tools/ci/m7_ffi_cdata_get_l.sh` to reject raw `CType.info` and
  `CType.size` reads inside the cdata core helper bodies.

Verification:

- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m7_ffi_carith_l.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m0_source_guard.sh
- git diff --check
