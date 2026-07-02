TG FFI call-function helper surface
===================================

Status: implemented and guarded.

Changes:

- Added `lj_tg_ffi_call_func_*` helpers around `TGState.ffi_call_func`.
- Preserved the nested-callback protocol with release publication around FFI
  native calls and acquire reads on callback entry.
- Routed FFI call setup/restore, callback unwind clear, nested callback
  blacklist reads, and TG detach clear through the helper layer.
- Documented the invariant formerly checked by `m7_ffi_callback_runtime`: raw production
  `ffi_call_func` access outside `src/lj_tg.h`.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m7_ffi_callback_runtime.sh`
