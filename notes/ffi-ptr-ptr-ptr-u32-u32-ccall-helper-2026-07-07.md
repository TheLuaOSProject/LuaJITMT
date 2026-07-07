# FFI CreateEventEx-shaped C-call helper

Added the exact pointer result
`void *(void *, void *, uint32_t, uint32_t)` FFI C-call trace slice:

- runtime helper `lj_ccall_jit_ptr_ptr_ptr_u32_u32()`;
- recorder matcher `crec_call_jit_ptr_ptr_ptr_u32_u32()`;
- IR call metadata `IRCALL_lj_ccall_jit_ptr_ptr_ptr_u32_u32`.

This covers CreateEventEx/CreateMutexEx-shaped security/name/flags/access ABI
classes without reusing the neighboring size-shaped helper. Both DWORD
arguments are preserved through `crec_call_jit_u32_arg()`, and the raw pointer
result is boxed with the existing `IR_CNEWI` pointer-result pattern.

The `m7_ffi_ccall_native` shared-library gate now includes a hot loop with
high-bit flags/access values and reads through the returned pointer to prove
the call records and boxes correctly.
