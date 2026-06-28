# FFI C-Call Native-State Helpers

Interpreted FFI C calls now factor their native-state bookkeeping through
`CCallNativeState` plus exported `lj_ccall_native_save()`,
`lj_ccall_native_enter()`, `lj_ccall_native_leave()`, and
`lj_ccall_native_checkstop()`.

The ordering intentionally matches the old inline sequence:

- save the surrounding TG/callback state before argument conversion;
- publish `ffi_call_func`, snapshot sticky STOPREQ, and enter native only after
  arguments are ready;
- leave native, blacklist callback-calling functions, and restore the saved TG
  callback state before result conversion;
- convert results before checking fresh STOPREQ.

The helpers now live in `lj_ccall.h` so a future x64 `IR_CALLXS` bridge can use
the same callback blacklist, `ffi_call_func`, and STOPREQ-freshness protocol as
the interpreted path instead of duplicating private bookkeeping.

`tests/t-ffi-ccall-native-helpers.c` verifies the exported helper ABI directly:
saved TG callback state is restored after leave, a callback-observed foreign
function is blacklisted, and a fresh `LJ_GC2_HS_STOPREQ` published while the TG
is native is reported only by the delayed helper check.

This is still infrastructure for a future `IR_CALLXS` bridge. Traced ordinary
FFI C calls remain disabled by `LJ_FFI_RECORD_CALLS=0` because the x64 lowering
still needs explicit result preservation and carefully ordered native entry
relative to ABI argument setup before direct mcode calls are safe.
