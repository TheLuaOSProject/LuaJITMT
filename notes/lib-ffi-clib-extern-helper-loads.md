lib_ffi C library extern helper loads

- Routed `ffi_clib___index` through `ctype_info_acq()` before detecting extern
  cdata entries and resolving their child type.
- Routed `ffi_clib___newindex` through helper-backed info/size loads while
  detecting extern entries, stripping attributes, collecting qualifiers, and
  checking constness before conversion.
- Documented the invariant formerly checked by `m7_ffi_clib_cache`: raw `CType.info` and
  `CType.size` reads in these C library extern helper bodies.

Verification:

- tools/ci/m7_ffi_clib_cache.sh
- tools/ci/m7_ffi_clib_ldscript.sh
- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- git diff --check
