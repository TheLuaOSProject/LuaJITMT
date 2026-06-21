# Recorder Cdata Index Helper Loads

`recff_cdata_index()` now avoids direct shared `CType.info` / `CType.size`
payload reads while recording cdata indexing and field access.

The helper snapshots ctype metadata through `ctype_info_acq()` /
`ctype_size_acq()` for:

- pointer and reference base resolution;
- numeric index element sizing and complex-index masking;
- cdata integer index width/sign handling;
- string field lookup fallback metadata and constant fields;
- pointer-to-struct auto-deref decisions;
- reference-field and attribute stripping before delegating to load/store
  conversion helpers.

Guardrail:

- `tools/ci/m7_ffi_cdata_get_l.sh` rejects raw `->info` / `->size` reads in
  `recff_cdata_index()`.

Validation:

- `tools/ci/m7_ffi_cdata_get_l.sh`
- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
