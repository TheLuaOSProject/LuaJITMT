CConv C-to-C helper loads

- Routed `lj_cconv_ct_ct_l()` initial destination/source `info` and `size`
  snapshots through `ctype_info_acq()` and `ctype_size_acq()`.
- Follow-up operand lifetime cleanup copies both destination and source `CType`
  operands into local snapshots at `lj_cconv_ct_ct_l()` entry before any
  wait-capable raw-child lookup. This covers the vector splat path where
  destination element resolution can wait before recursive conversion would
  otherwise reuse the caller's original source table-slot pointer.
- Reused helper-backed snapshots when resolving complex source/destination
  children, complex real/imaginary offsets, vector splat element sizes, and
  array/struct VLA or invalid-size checks.
- The typeinfo snapshot suite guards that those operand copies precede the
  wait-capable ctype helpers in the raw C-to-C conversion body.

Verification:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_cdata_get_l m7_ffi_cdata_set_l m7_ffi_carith_l m7_ffi_ccall_native`
- `git diff --check`
