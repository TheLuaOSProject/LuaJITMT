# FFI CreateMutex-shaped C-call helper

Added the exact pointer result
`void *(void *, int32_t, void *)` FFI C-call trace slice:

- runtime helper `lj_ccall_jit_ptr_ptr_i32_ptr()`;
- recorder matcher `crec_call_jit_ptr_ptr_i32_ptr()`;
- IR call metadata `IRCALL_lj_ccall_jit_ptr_ptr_i32_ptr`.

This covers CreateMutex/CreateWaitableTimer-shaped
security/inherit-or-manual-reset/name ABI classes without broadening the
generic C-call recorder. The recorder keeps the middle flag on the signed
32-bit path and boxes the raw helper pointer result back into cdata with the
same `IR_CNEWI` pattern used by neighboring pointer-result helpers.

The `m7_ffi_ccall_native` shared-library gate now includes a hot loop with a
negative signed flag and reads through the returned pointer to prove the call
records and boxes correctly.
