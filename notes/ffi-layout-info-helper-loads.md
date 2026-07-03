# FFI Layout Info Helper Loads

`ffi_layout_info()` and `ffi_layout_info_raw()` now use helper-backed local
`CType.info` / `CType.size` snapshots while accumulating layout attributes and
returning size information for `ffi.sizeof()`, `ffi.alignof()`, and
`ffi.offsetof()`.

Coverage:

- `m7_ffi_typeinfo_snapshot` exercises these layout info helpers through
  `ffi.sizeof()`, `ffi.alignof()`, and `ffi.offsetof()` behavior.
- Local `CType.info` / `CType.size` reads in these helpers must use the
  documented helper-load surface instead of source-text enforcement.

Validation:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `git diff --check`
