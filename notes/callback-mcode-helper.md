Callback mcode helper slice

- Added `ctype_cb_mcode_acq()` and `ctype_cb_mcode_rel()` for the callback
  trampoline page pointer.
- Routed callback slot pointer conversion, callback pointer-to-slot lookup,
  mcode publication, and mcode free through the helper API.
- Extended `tools/ci/m7_ffi_callback_install.sh` to reject raw
  implementation-side `cts->cb.mcode` access alongside the existing callback
  side arrays.

Verification:

- tools/ci/m7_ffi_callback_install.sh
- tools/ci/m7_ffi_callback_runtime.sh
- tools/ci/m0_source_guard.sh
- git diff --check
