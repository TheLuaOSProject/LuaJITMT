# FFI TrySubmitThreadpoolCallback-shaped C-call helper

The x64 FFI recorder now accepts exact
`int32_t(void *, void *, void *)` calls through:

- runtime helper `lj_ccall_jit_i32_ptr_ptr_ptr()`;
- recorder matcher `crec_call_jit_i32_ptr_ptr_ptr()`;
- IR call metadata `IRCALL_lj_ccall_jit_i32_ptr_ptr_ptr`.

This covers TrySubmitThreadpoolCallback-shaped callback/context/environment
ABI classes while preserving the signed 32-bit result.

`tests/t-ffi-ccall-native.lua` exercises the shape with hotloop=1 against the
m7 shared-library fixture and asserts that a trace is produced.
