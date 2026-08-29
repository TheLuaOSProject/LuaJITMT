# FFI SetWaitableTimer-shaped C-call helper

The x64 FFI recorder now accepts exact
`int32_t(void *, void *, int32_t, void *, void *, int32_t)` calls through:

- runtime helper `lj_ccall_jit_i32_ptr_ptr_i32_ptr_ptr_i32()`;
- recorder matcher `crec_call_jit_i32_ptr_ptr_i32_ptr_ptr_i32()`;
- IR call metadata `IRCALL_lj_ccall_jit_i32_ptr_ptr_i32_ptr_ptr_i32`.

This covers SetWaitableTimer-shaped handle/due-time/period/callback/argument/
resume ABI classes while preserving signed period and resume fields and the
signed 32-bit result.

`tests/t-ffi-ccall-native.lua` exercises the shape with hotloop=1 against the
m7 shared-library fixture and asserts that a trace is produced.
