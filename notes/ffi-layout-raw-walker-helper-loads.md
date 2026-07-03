# FFI Layout Raw Walker Helper Loads

`ffi_layout_rawref()`, `ffi_layout_rawid()`, and `ffi_layout_raw()` now use
helper-backed `CType.info` snapshots while stripping references and attributes
from sequence-checked layout snapshots.

This extends the raw-child layout guard so the local raw layout walker group no
longer contains direct `->info` / `->size` reads.

Coverage model:

- Active coverage stays in `m7_ffi_typeinfo_snapshot` behavior/counter fixtures and code-adjacent helper docs. Direct helper/backend sites are documented by the implementation; raw-field source inventories are not pass/fail contracts.

Validation:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `git diff --check`
