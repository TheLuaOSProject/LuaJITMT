Callback mcode helper slice

- Added `ctype_cb_mcode_acq()` and `ctype_cb_mcode_rel()` for the callback
  trampoline page pointer.
- Routed callback slot pointer conversion, callback pointer-to-slot lookup,
  mcode publication, and mcode free through the helper API.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_callback_install` behavior/counter fixtures and code-adjacent helper docs; raw-field source inventories are not pass/fail contracts.

Verification:

- tools/ci/m7_ffi_callback_install.sh
- tools/ci/m7_ffi_callback_runtime.sh
- git diff --check
