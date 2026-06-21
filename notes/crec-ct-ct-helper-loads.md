# JIT C-to-C Conversion Helper Loads

`crec_isnonzero()` now snapshots source `CType.info` and `CType.size` through
`ctype_info_acq()` / `ctype_size_acq()` before specializing constant
bool-conversion comparisons.

`crec_ct_ct()` now snapshots destination and source `CType.info` / `CType.size`
through the same acquire helpers before selecting conversion cases, integer
extension/truncation widths, complex half offsets, pointer sizing, and aggregate
copy lengths. This keeps the trace recorder from reading shared ctype payloads
directly while CTState publication continues moving toward lockless readers.

Guardrail:

- `tools/ci/m7_ffi_jit_cnew.sh` rejects raw `->info` / `->size` reads in
  `crec_isnonzero()` and `crec_ct_ct()`.

Validation:

- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m7_ffi_snap_restore_l.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
