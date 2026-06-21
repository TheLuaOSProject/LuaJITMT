CConv C-to-C helper loads

- Routed `lj_cconv_ct_ct_l()` initial destination/source `info` and `size`
  snapshots through `ctype_info_acq()` and `ctype_size_acq()`.
- Reused helper-backed snapshots when resolving complex source/destination
  children, complex real/imaginary offsets, vector splat element sizes, and
  array/struct VLA or invalid-size checks.
- Extended `tools/ci/m7_ffi_cdata_set_l.sh` to reject raw `CType.info` and
  `CType.size` reads in the guarded raw C-to-C conversion helper body.

Verification:

- tools/ci/m7_ffi_cdata_set_l.sh
- tools/ci/m7_ffi_cdata_get_l.sh
- tools/ci/m7_ffi_carith_l.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m0_source_guard.sh
- git diff --check
