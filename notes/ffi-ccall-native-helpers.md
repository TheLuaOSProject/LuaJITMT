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
  callback state before result conversion, including the callback slot marker
  that is temporarily reused to detect whether the C call reached a callback;
- convert results before checking fresh STOPREQ.

The helpers now live in `lj_ccall.h` so a future x64 `IR_CALLXS` bridge can use
the same callback blacklist, `ffi_call_func`, and STOPREQ-freshness protocol as
the interpreted path instead of duplicating private bookkeeping.

`tests/t-ffi-ccall-native-helpers.c` verifies the exported helper ABI directly:
saved TG callback state is restored after leave, a callback-observed foreign
function is blacklisted without losing the prior callback slot, and a fresh
`LJ_GC2_HS_STOPREQ` published while the TG is native is reported only by the
delayed helper check.

This remains infrastructure for a future direct `IR_CALLXS` bridge. The narrow
`lj_ccall_jit_{void,i32,ptr}_gpr()` trampoline family now traces exact void,
signed 32-bit integer, and pointer-returning calls with 0, 1, or 2
integer/pointer arguments through `IRCALL` helpers using this same native-state
protocol. `lj_ccall_jit_i64_gpr()` handles exact zero-argument signed 64-bit
integer returns and preserves boxed int64 cdata results.
`lj_ccall_jit_u32_0()` handles exact zero-argument unsigned 32-bit integer
returns as Lua numbers without high-bit truncation. The sibling
`lj_ccall_jit_{num,flt}_fpr()` helpers trace exact double or float returns with
0, 1, or 2 same-kind exact FP arguments. Broad traced ordinary FFI C calls
remain disabled by `LJ_FFI_RECORD_CALLS=0` because x64 `IR_CALLXS` lowering
still needs explicit result preservation and carefully ordered native entry
relative to ABI argument setup before direct mcode calls are safe.
