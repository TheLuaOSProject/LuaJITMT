# FFI Typecmp Walker Helper Loads

`ffi_typecmp_rawid()`, `ffi_typecmp_rawrefid()`, and
`ffi_typecmp_childqual()` now use helper-backed `CType.info` / `CType.size`
snapshots while walking type comparison records for `ffi.istype()`.

The covered decisions include attribute stripping, reference stripping, child
id selection, and qualifier accumulation during sequence-checked ctype
comparison snapshots.

Coverage model:

- Active coverage stays in `m7_ffi_typeinfo_snapshot` behavior/counter fixtures and code-adjacent helper docs. Direct helper/backend sites are documented by the implementation; raw-field source inventories are not pass/fail contracts.

Validation:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- stock `lib/ffi/istype.lua`
- `git diff --check`
