# FFI Layout Raw-Child Helper Loads

`ffi_layout_rawchild()` now uses helper-backed `CType.info` snapshots when
deriving a child ctype id and when stripping attributes from the snapshot
walker.

This keeps the `ffi.sizeof()` / `ffi.offsetof()` layout snapshot path aligned
with the M7 rule that shared ctype payloads are read through helper APIs.

Guardrail:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh` rejects raw `->info` / `->size` reads
  in `ffi_layout_rawchild()`.

Validation:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
