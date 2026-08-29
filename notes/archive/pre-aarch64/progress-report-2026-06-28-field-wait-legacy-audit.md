# Progress Report - 2026-06-28 FFI Field Waits

Policy: preserve Lua/FFI language behavior across parser rollback, native waits,
and concurrent type-table growth.

## Landed In This Area

- `lj_cdata_index_l()` string-key field lookup moved off parser-token fallback
  for stable struct fields, constructor constants, and pointer auto-deref.
- Field readers use ID-rooted wait/refetch helpers so table-owned `CType *`
  pointers are not retained across native waits.
- Field snapshot qualifier handling avoids publishing partial qualifier state
  after a failed or retried attempt.
- `t-ffi-field-snapshot.c` covers field get/set, pointer auto-deref, misses,
  metatype dispatch, and constructor constants while another thread owns the
  parser token.
- Rollback coverage confirms failed cdefs do not leak constructor constants or
  constructor fields.

## Remaining Work

- `lib_ffi.c` still has parser-token fallbacks for string parsing and layout
  queries involving VLA/VLS sizes and error construction.
- `lj_clib.c` namespace lookup still has conservative parser fallback behavior.
- Finalizer/weak GC2 bridge names and behavior still need semantic coverage
  before simplification.

## Verification At The Time

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback m7_ffi_cdata_get_l m7_ffi_carith_l`
- `tools/ci/lua_test.sh m7_ffi`
