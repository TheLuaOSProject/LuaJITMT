lj_ccallback runtime helper loads

- Routed `callback_conv_args()` through helper-backed ctype payload snapshots
  while loading the callback signature, walking argument fields, choosing
  register/stack argument storage, and preserving x86 stack-adjust metadata.
- Routed `callback_conv_result()` through helper-backed return ctype snapshots
  for result destination selection and integer/FP return extension.
- Extended `tools/ci/m7_ffi_callback_runtime.sh` to reject raw `CType.info`,
  `CType.size`, and `CType.sib` reads in the callback runtime conversion
  helpers.

Verification:

- `tools/ci/m7_ffi_callback_runtime.sh`
- `tools/ci/m7_ffi_callback_install.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`
