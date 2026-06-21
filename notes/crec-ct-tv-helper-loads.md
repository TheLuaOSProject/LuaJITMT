# JIT TValue-to-C Store Helper Loads

`crec_ct_tv()` now snapshots destination and source `CType.info` / `CType.size`
through acquire helpers instead of reading shared ctype payload fields directly.

The destination info snapshot covers string-to-enum matching, string-to-array
rejection, and final enum child resolution before dispatching to `crec_ct_ct()`.
The source info snapshots are refreshed after cdata source normalization,
function-to-pointer interning, reference child resolution, and enum child
resolution so unboxing and conversion planning observe helper-backed payloads.
The enum string fallback now reads matched constant value metadata with
`ctype_info_acq()` / `ctype_size_acq()`.

Guardrail:

- `tools/ci/m7_ffi_jit_cnew.sh` rejects raw `->info` / `->size` reads in
  `crec_ct_tv()`.

Validation:

- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
