# FFI Typecmp Walker Helper Loads

`ffi_typecmp_rawid()`, `ffi_typecmp_rawrefid()`, and
`ffi_typecmp_childqual()` now use helper-backed `CType.info` / `CType.size`
snapshots while walking type comparison records for `ffi.istype()`.

The covered decisions include attribute stripping, reference stripping, child
id selection, and qualifier accumulation during sequence-checked ctype
comparison snapshots.

Guardrail:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh` rejects raw `->info` / `->size` reads
  in these `ffi.istype()` typecmp walker helpers.

Validation:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- stock `lib/ffi/istype.lua`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
