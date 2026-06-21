# Recorder Cdata Allocation Helper Loads

`crec_alloc()` now uses helper-backed ctype metadata snapshots while recording
trace-side cdata allocation and aggregate initialization.

The helper already starts from `lj_ctype_info_snapshot()` for the root ctype.
This slice removes the remaining direct payload reads in the array and struct
initializer paths by using `ctype_info_acq()`, `ctype_size_acq()`, and
`ctype_sib_acq()` for element types, field lists, named-field checks, field
offsets, union size decisions, and aggregate fallback selection.

Guardrail:

- `tools/ci/m7_ffi_jit_cnew.sh` rejects raw `->info` / `->size` / `->sib`
  reads in `crec_alloc()`.

Validation:

- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m7_ffi_cdata_alloc.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
