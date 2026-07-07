# FFI CreateSemaphoreEx-shaped C-call helper

Added the exact pointer result
`void *(void *, int32_t, int32_t, void *, uint32_t, uint32_t)` FFI C-call trace
slice:

- runtime helper `lj_ccall_jit_ptr_ptr_i32_i32_ptr_u32_u32()`;
- recorder matcher `crec_call_jit_ptr_ptr_i32_i32_ptr_u32_u32()`;
- IR call metadata `IRCALL_lj_ccall_jit_ptr_ptr_i32_i32_ptr_u32_u32`.

This covers CreateSemaphoreEx-shaped security/counts/name/flags/access ABI
classes without broadening the generic C-call recorder. The recorder keeps the
count fields signed, preserves the flag/access DWORDs through
`crec_call_jit_u32_arg()`, and boxes the raw pointer result with the existing
`IR_CNEWI` pointer-result pattern.

The `m7_ffi_ccall_native` shared-library gate now includes a hot loop with
negative and positive signed counts plus high-bit flag/access values.
