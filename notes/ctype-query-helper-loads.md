CType query helper loads

- Routed `lj_ctype_getfieldq()` through `ctype_info_acq()` and
  `ctype_size_acq()` for direct field offsets and subtype walks.
- Routed `lj_ctype_size()`, `lj_ctype_vlsize()`, `lj_ctype_info()`, and
  `lj_ctype_info_raw()` through helper-backed payload reads while walking ctype
  IDs and attribute/reference chains.
- Routed `lj_ctype_meta()` and `lj_ctype_metatv()` through helper-backed info
  loads before checking pointer/function metatable special cases.
- Routed `ctype_repr()` through helper-backed info/size loads, including array
  child-size formatting.
- Extended `tools/ci/m7_ffi_typeinfo_snapshot.sh` to reject raw
  `CType.info`/`CType.size` reads inside these ctype query bodies.

Verification:

- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m7_ffi_metatype.sh
- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_carith_l.sh
- tools/ci/m7_ffi_jit_cnew.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m0_source_guard.sh
- git diff --check
