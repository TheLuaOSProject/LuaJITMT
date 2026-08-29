lj_ccallback runtime helper loads

- Routed `callback_conv_args()` through helper-backed ctype payload snapshots
  while loading the callback signature, walking argument fields, choosing
  register/stack argument storage, and preserving x86 stack-adjust metadata.
- Routed `callback_conv_result()` through helper-backed return ctype snapshots
  for result destination selection and integer/FP return extension.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_callback_runtime` behavior/counter fixtures; the helper comments carry the ordering rationale.

Verification:

- `tools/ci/m7_ffi_callback_runtime.sh`
- `tools/ci/m7_ffi_callback_install.sh`
- `git diff --check`
