lj_ccall argument setup helper loads

- Routed `ccall_set_args()` through helper-backed `CType.info`, `CType.size`,
  and `CType.sib` snapshots for function metadata, result preallocation,
  initial attribute skipping, field argument selection, pass-kind decisions,
  stack alignment, integer extension, and platform post-processing.
- Kept the existing mutable ABI pass-size variable separate from immutable
  source ctype snapshots so pointer-sized fallback arguments still lay out the
  same way while sign/zero extension decisions use the original ctype size.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_cdata_set_l` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_cdata_get_l.sh`
- `tools/ci/m7_ffi_callback_runtime.sh`
- `git diff --check`
