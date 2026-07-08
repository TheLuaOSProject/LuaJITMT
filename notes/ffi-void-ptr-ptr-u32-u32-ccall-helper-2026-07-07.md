# FFI SetThreadpoolTimer-shaped void C-call helper

The x64 FFI recorder now accepts exact
`void(void *, void *, uint32_t, uint32_t)` calls through:

- runtime helper `lj_ccall_jit_void_ptr_ptr_u32_u32()`;
- recorder matcher `crec_call_jit_void_ptr_ptr_u32_u32()`;
- IR call metadata `IRCALL_lj_ccall_jit_void_ptr_ptr_u32_u32`.

This covers SetThreadpoolTimer-shaped timer/due-time/period/window ABI classes
while preserving side effects and high-bit unsigned 32-bit arguments.

`tests/t-ffi-ccall-native.lua` exercises the shape with hotloop=1 against the
m7 shared-library fixture and asserts that a trace is produced.
