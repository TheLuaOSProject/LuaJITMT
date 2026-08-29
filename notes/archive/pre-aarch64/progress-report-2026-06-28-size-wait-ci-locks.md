# Progress Report - 2026-06-28 Size Waits And Coordination

Policy: preserve Lua/FFI semantics across parser rollback and native waits,
even when it costs short-term performance.

## Landed In This Area

- Interpreted numeric cdata element-size readers and pointer add/sub/diff avoid
  parser-token fallback where a sequence-checked size snapshot is sufficient.
- `lj_ctype_size_wait()` retries size snapshots and parks in native time while
  another parser owns the token.
- `lj_cdata_index_l()` and `carith_ptr()` refetch type metadata after native
  waits so stale `CType *` pointers are not used on meta/error paths.
- `t-ffi-element-size-snapshot.c` covers cdata indexing, pointer add, and
  pointer diff while another thread owns the parser token.

## Still Remaining

- FFI string-key field fallback and layout/string parsing paths still need
  careful snapshot/refetch conversions.
- GC2 weak/finalizer bridge behavior still needs more semantic coverage before
  simplification.
- Release confidence still needs long stress/soak, sanitizer-style runs where
  practical, and benchmark review after semantic closure.

## Verification At The Time

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m7_ffi_carith_l m7_ffi_cdata_get_l m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi_cdef_token m7_ffi_finreg`
- `tools/ci/lua_test.sh m7_ffi`
- `git diff --check`
