# FFI C-Call Native-State Helpers

Interpreted FFI C calls now factor their native-state bookkeeping through
`CCallNativeState` plus `ccall_native_save()`, `ccall_native_enter()`, and
`ccall_native_leave()`.

The ordering intentionally matches the old inline sequence:

- save the surrounding TG/callback state before argument conversion;
- publish `ffi_call_func`, snapshot sticky STOPREQ, and enter native only after
  arguments are ready;
- leave native, blacklist callback-calling functions, and restore the saved TG
  callback state before result conversion;
- convert results before checking fresh STOPREQ.

This is only infrastructure for a future `IR_CALLXS` bridge. Traced ordinary FFI
C calls remain disabled by `LJ_FFI_RECORD_CALLS=0` because the x64 lowering still
needs explicit result preservation, callback bookkeeping, and fresh STOPREQ
handling before direct mcode calls are safe.
