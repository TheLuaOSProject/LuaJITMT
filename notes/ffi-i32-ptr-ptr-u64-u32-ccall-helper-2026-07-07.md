# FFI WaitOnAddress-shaped C-call helper

Added the exact signed 32-bit result
`int32_t(void *, void *, uint64_t, uint32_t)` FFI C-call trace slice:

- runtime helper `lj_ccall_jit_i32_ptr_ptr_u64_u32()`;
- recorder matcher `crec_call_jit_i32_ptr_ptr_u64_u32()`;
- IR call metadata `IRCALL_lj_ccall_jit_i32_ptr_ptr_u64_u32`.

This covers WaitOnAddress-shaped address/compare/size/timeout ABI classes
without reusing pointer-result size helpers. The recorder preserves the size
argument as unsigned 64-bit and the timeout as unsigned 32-bit before calling
through the native-state helper path.

The `m7_ffi_ccall_native` shared-library gate now includes a hot loop with a
high-bit timeout and high-bit unsigned size value.
