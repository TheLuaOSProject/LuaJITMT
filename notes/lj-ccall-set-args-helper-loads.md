lj_ccall argument setup helper loads

- Routed `ccall_set_args()` through helper-backed `CType.info`, `CType.size`,
  and `CType.sib` snapshots for function metadata, result preallocation,
  initial attribute skipping, field argument selection, pass-kind decisions,
  stack alignment, integer extension, and platform post-processing.
- Kept the existing mutable ABI pass-size variable separate from immutable
  source ctype snapshots so pointer-sized fallback arguments still lay out the
  same way while sign/zero extension decisions use the original ctype size.
- Extended `tools/ci/m7_ffi_cdata_set_l.sh` to reject raw payload reads in the
  common C-call argument setup walk.

Verification:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_cdata_get_l.sh`
- `tools/ci/m7_ffi_callback_runtime.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
