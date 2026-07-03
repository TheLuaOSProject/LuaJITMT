# FFI Layout Size Helper Loads

`ffi_layout_vlsize()` now snapshots local layout `CType` copies through helper
loads while scanning struct fields for variable-length array tails, checking the
raw VLA array type, reading element size metadata, and computing the final size.

`ffi_layout_sizeof_snapshot()` now snapshots the raw type's `CType.info` /
`CType.size` values before deciding whether `ffi.sizeof()` needs an element
count or can return a direct size.

Coverage:

- `m7_ffi_typeinfo_snapshot` exercises the layout size snapshot path through
  `ffi.sizeof()` behavior and active-parser snapshot fixtures.
- Local layout `CType` copies in this path must use the documented helper-load
  surface; that rule lives here and beside the helpers instead of in a
  source-text predicate.

Validation:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `git diff --check`
