# FFI Layout Raw-Child Helper Loads

`ffi_layout_rawchild()` now uses helper-backed `CType.info` snapshots when
deriving a child ctype id and when stripping attributes from the snapshot
walker.

This keeps the `ffi.sizeof()` / `ffi.offsetof()` layout snapshot path aligned
with the M7 rule that shared ctype payloads are read through helper APIs.

Coverage model:

- Active coverage stays in `m7_ffi_typeinfo_snapshot` behavior/counter fixtures and code-adjacent helper docs. Direct helper/backend sites are documented by the implementation; raw-field source inventories are not pass/fail contracts.

Validation:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `git diff --check`
