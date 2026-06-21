# FFI Layout Raw Walker Helper Loads

`ffi_layout_rawref()`, `ffi_layout_rawid()`, and `ffi_layout_raw()` now use
helper-backed `CType.info` snapshots while stripping references and attributes
from sequence-checked layout snapshots.

This extends the raw-child layout guard so the local raw layout walker group no
longer contains direct `->info` / `->size` reads.

Guardrail:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh` rejects raw `->info` / `->size` reads
  in the FFI layout raw walker helpers.

Validation:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
