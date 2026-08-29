2026-07-08 int32 int/pointer/pointer ccall helper note
=======================================================

The native-state ccall helper matrix now records exact three-argument
`int32_t(int32_t, void *, void *)` ABI shapes through
`lj_ccall_jit_i32_i32_ptr_ptr()`.

This covers accept/getsockname/getpeername-style descriptor, address pointer,
and length pointer signatures. The recorder accepts only fixed three-argument
calls with a signed 32-bit result, signed 32-bit descriptor, and two pointer
arguments, then returns the helper's plain signed 32-bit result.

The focused shared-library fixture drives both pointer arguments through a hot
loop, mutates the address buffer via the first pointer, and asserts that
recording produced at least one trace.
