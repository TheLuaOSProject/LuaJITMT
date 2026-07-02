CConv aggregate helper loads

- Routed table-to-array and array initializer helpers through
  `ctype_info_acq()` and `ctype_size_acq()` for child type IDs, destination
  sizes, and element sizes.
- Routed table/init sub-struct walks through helper-backed field info/offset
  snapshots and `ctype_sib_acq()` while preserving named-field, bitfield,
  subtype, and union behavior.
- Routed `lj_cconv_multi_init()` and `lj_cconv_ct_init_l()` aggregate decisions
  through helper-backed destination info loads.
- Documented the invariant formerly checked by `m7_ffi_cdata_set_l`: raw `CType.info` and
  `CType.size` reads in these guarded aggregate conversion helper bodies.

Verification:

- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_carith_l.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- git diff --check
