# CType Snapshot Helper Loads

The ctype snapshot helpers now use helper-backed loads for local copied
`CType` records while validating abandoned records, following anonymous subtype
field chains, resolving pointer-to-struct auto-deref targets, and accumulating
stable typeinfo/layout metadata.

Converted helpers:

- `lj_ctype_snapshot()`
- `ctype_snapshot_copy()`
- `ctype_getfieldq_snapshot_rec()`
- `lj_ctype_ptrstruct_snapshot()`
- `lj_ctype_info_snapshot()`

Coverage:

- `m7_ffi_typeinfo_snapshot` exercises the ctype snapshot helpers through FFI
  typeinfo, layout, pointer-struct, and rollback-reader paths.
- Local `CType` record access in these helpers must use the documented
  snapshot load helpers; that rule lives here and beside the helper surface
  instead of in a source-text predicate.

Validation:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `git diff --check`
