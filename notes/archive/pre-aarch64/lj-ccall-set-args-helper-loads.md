lj_ccall argument setup helper loads

- Routed `ccall_set_args()` through helper-backed `CType.info`, `CType.size`,
  and `CType.sib` snapshots for function metadata, result preallocation,
  initial attribute skipping, field argument selection, pass-kind decisions,
  stack alignment, integer extension, and platform post-processing.
- Kept the existing mutable ABI pass-size variable separate from immutable
  source ctype snapshots so pointer-sized fallback arguments still lay out the
  same way while sign/zero extension decisions use the original ctype size.
- Follow-up lifetime cleanup adds ccall-local waitable snapshots for exact
  ctype records, raw child records, and raw argument records. `ccall_set_args()`
  now uses those snapshots for the function return child, initial attribute
  skip, fixed-argument fields, inferred vararg cdata source metadata, and each
  destination argument type so conversion helpers can wait on parser ownership
  without leaving later ABI decisions on stale table pointers.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_cdata_set_l` behavior/counter fixtures; the helper comments carry the ordering rationale.

Verification:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m7_ffi_ccall_native`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_cdata_get_l.sh`
- `tools/ci/m7_ffi_callback_runtime.sh`
- `git diff --check`
