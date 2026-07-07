# FFI RegisterWaitForSingleObject-shaped C-call helper

The x64 FFI recorder now accepts exact
`int32_t(void *, void *, void *, void *, uint32_t, uint32_t)` calls through:

- runtime helper `lj_ccall_jit_i32_ptr_ptr_ptr_ptr_u32_u32()`;
- recorder matcher `crec_call_jit_i32_ptr_ptr_ptr_ptr_u32_u32()`;
- IR call metadata `IRCALL_lj_ccall_jit_i32_ptr_ptr_ptr_ptr_u32_u32`.

This covers RegisterWaitForSingleObject-shaped
wait-output/object/callback/context/timeout/flags ABI classes while preserving
both high-bit unsigned 32-bit fields and the signed 32-bit result.

`tests/t-ffi-ccall-native.lua` exercises the shape with hotloop=1 against the
m7 shared-library fixture and asserts that a trace is produced.
