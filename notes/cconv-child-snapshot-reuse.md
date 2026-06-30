CConv child snapshot reuse

- Replaced live ctype-table refetches in `lj_cconv_ct_ct_l()` with local
  helper-backed snapshots for complex source/destination children, complex
  real/imaginary recursion, and vector splat element conversion.
- Reused the existing array element snapshot in table-to-array and array
  initializer loops instead of reacquiring `ctype_get(cts, dcid)` after each
  recursive TValue conversion.
- Routed `lj_cconv_ct_tv_l()` cdata source resolution through local snapshots
  for source cdata, reference unwrapping, enum child conversion, function
  pointer interning refresh, default TValue source ctypes, and enum
  destination stripping.
- Remaining `lj_cconv.c` live table reads are the immutable predefined copy
  fast path and the existing compatibility child walker.

Verification:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_cdata_set_l`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_cdata_get_l`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_carith_l`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m5_buffer_publish`
