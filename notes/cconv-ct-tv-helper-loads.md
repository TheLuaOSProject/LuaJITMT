CConv TValue-to-C helper loads

- Routed `lj_cconv_ct_tv_l()` destination decisions through
  `ctype_info_acq()` and `ctype_size_acq()`.
- Routed cdata source reference/function/enum handling through helper-backed
  source info/size snapshots, refreshing the destination snapshot after
  function-pointer interning may reallocate the ctype table.
- Routed enum string fallback constants and string-to-array child checks
  through helper-backed `CType.info`/`CType.size` reads.
- `src/lj_cconv.c` now has no raw `CType.info` or `CType.size` reads; the
  cdata-set guard covers all converted conversion helper bodies.

Verification:

- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_carith_l.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m0_source_guard.sh
- git diff --check
