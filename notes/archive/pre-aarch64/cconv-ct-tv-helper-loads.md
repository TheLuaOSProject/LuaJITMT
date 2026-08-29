CConv TValue-to-C helper loads

- Routed `lj_cconv_ct_tv_l()` destination decisions through
  `ctype_info_acq()` and `ctype_size_acq()`.
- Follow-up destination lifetime cleanup copies the destination `CType` into a
  local snapshot at `lj_cconv_ct_tv_l()` entry, before any cdata source,
  enum-string, string-array child, or default source lookup can wait on the
  parser token. This keeps raw destination table-slot pointers from surviving
  across wait-capable conversion paths.
- Added `lj_cconv_ct_tv_id_l()` for callers that have a destination `CTypeID`.
  It resolves the raw destination through the helper-backed snapshot/wait path
  before entering `lj_cconv_ct_tv_l()`, so library call sites no longer need to
  pass live `ctype_get()` table-slot pointers for predefined conversions.
- Routed predefined destination conversions in `ffi.string()`, `ffi.copy()`,
  `ffi.fill()`, `ffi.errno()`, `tonumber(cdata)`, string-buffer
  `set(cdata, len)` / `putcdata()`, and bitfield scalar writes through the new
  ID-based helper.
- Routed the predefined int32/double C-to-Lua number conversions in
  `lj_cconv_tv_ct_l()` through a local ID-based snapshot helper too.
- Routed cdata source reference/function/enum handling through helper-backed
  source info/size snapshots, refreshing the destination snapshot after
  function-pointer interning may reallocate the ctype table.
- Routed enum string fallback constants and string-to-array child checks
  through helper-backed `CType.info`/`CType.size` reads.
- `src/lj_cconv.c` now has no raw `CType.info` or `CType.size` reads; the
  cdata-set coverage covers all converted conversion helper bodies.

Verification:

- `make -C src -j2`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_cdata_get_l`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_cdata_set_l`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m5_buffer_publish`
- `git diff --check`
