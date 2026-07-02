# FFI Layout Field Helper Loads

`ffi_layout_getfield()` now uses helper-backed local snapshots for field type
metadata, field offsets, and sibling traversal while recursively resolving
anonymous subtype fields for `ffi.offsetof()`.

The `ffi_layout_offsetof_snapshot()` caller also snapshots the raw type's
`CType.info` / `CType.size` values before deciding whether the field walker can
run.

Guardrail:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh` rejects direct local `ct.info`,
  `ct.size`, `ct.sib`, `cct.info`, `cct.size`, or `cct.sib` reads in the field
  layout snapshot path.

Validation:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `git diff --check`
