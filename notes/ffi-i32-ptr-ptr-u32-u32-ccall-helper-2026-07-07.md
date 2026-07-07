# FFI SleepConditionVariableSRW-shaped C-call helper

Added the exact signed 32-bit result
`int32_t(void *, void *, uint32_t, uint32_t)` FFI C-call trace slice:

- runtime helper `lj_ccall_jit_i32_ptr_ptr_u32_u32()`;
- recorder matcher `crec_call_jit_i32_ptr_ptr_u32_u32()`;
- IR call metadata `IRCALL_lj_ccall_jit_i32_ptr_ptr_u32_u32`.

This covers SleepConditionVariableSRW-shaped condition-variable/lock/timeout/
flags ABI classes without widening either DWORD argument. The recorder preserves
both unsigned 32-bit arguments through `crec_call_jit_u32_arg()` and returns the
signed 32-bit result through the native-state helper path.

The `m7_ffi_ccall_native` shared-library gate now includes a hot loop with
high-bit timeout and flags values.
