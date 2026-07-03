# FFI Layout Field Helper Loads

`ffi_layout_getfield()` now uses helper-backed local snapshots for field type
metadata, field offsets, and sibling traversal while recursively resolving
anonymous subtype fields for `ffi.offsetof()`.

The `ffi_layout_offsetof_snapshot()` caller also snapshots the raw type's
`CType.info` / `CType.size` values before deciding whether the field walker can
run.

Coverage:

- `m7_ffi_typeinfo_snapshot` exercises the field layout snapshot path through
  `ffi.offsetof()` behavior and active-parser snapshot fixtures.
- Local field-layout `CType` copies must use the documented helper-load
  surface; that rule lives here and beside the helpers instead of in a
  source-text predicate.

Validation:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `git diff --check`
