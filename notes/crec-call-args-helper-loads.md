# Recorder C-call Argument Helper Loads

`crec_call_args()` now snapshots function and argument ctype metadata through
`ctype_info_acq()`, `ctype_size_acq()`, and `ctype_sib_acq()` instead of reading
shared `CType` payload fields directly.

The helper uses those snapshots for initial parameter attribute skipping,
argument field traversal, vararg inferred argument type validation, integer
promotion and sign-extension decisions, soft-float split checks, and ABI
register accounting. Argument metadata is snapped before `crec_ct_tv()` so
later conversion and vararg inference cannot invalidate a `CType *` and then
leave subsequent recorder decisions reading the stale payload directly.

Guardrail:

- `tools/ci/m7_ffi_cdata_set_l.sh` rejects raw `->info` / `->size` / `->sib`
  reads in `crec_call_args()`.

Validation:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m7_ffi_callback_runtime.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
