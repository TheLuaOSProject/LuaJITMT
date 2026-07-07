# FFI CreateThreadpool-style pointer C-call helper

The x64 FFI recorder now accepts exact
`void *(void *, void *, void *)` calls through:

- runtime helper `lj_ccall_jit_ptr_ptr_ptr_ptr()`;
- recorder matcher `crec_call_jit_ptr_ptr_ptr_ptr()`;
- IR call metadata `IRCALL_lj_ccall_jit_ptr_ptr_ptr_ptr`.

This covers CreateThreadpoolWait/Work/Timer-shaped callback/context/
environment ABI classes while preserving the pointer result.

`tests/t-ffi-ccall-native.lua` exercises the shape with hotloop=1 against the
m7 shared-library fixture and asserts that a trace is produced.
