TG FFI call-function helper surface
===================================

Status: implemented and guarded.

Changes:

- Added `lj_tg_ffi_call_func_*` helpers around `TGState.ffi_call_func`.
- Preserved the nested-callback protocol with release publication around FFI
  native calls and acquire reads on callback entry.
- Routed FFI call setup/restore, callback unwind clear, nested callback
  blacklist reads, and TG detach clear through the helper layer.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_callback_runtime` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m7_ffi_callback_runtime.sh`
