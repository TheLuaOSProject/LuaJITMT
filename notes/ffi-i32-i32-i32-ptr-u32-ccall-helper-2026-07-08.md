2026-07-08 int32 int/int/int/pointer/uint32 ccall helper note
==============================================================

The native-state ccall helper matrix now records exact five-argument
`int32_t(int32_t, int32_t, int32_t, void *, uint32_t)` ABI shapes through
`lj_ccall_jit_i32_i32_i32_ptr_u32()`.

This covers setsockopt-style descriptor, level, option name, option-value
pointer, and option-length signatures. The recorder accepts only fixed
five-argument calls with a signed 32-bit result, three signed 32-bit integer
arguments, a pointer fourth argument, and an unsigned 32-bit final argument.

The focused shared-library fixture drives a high-bit unsigned 32-bit length
through a hot loop, mutates the option-value buffer through the pointer
argument, and asserts that recording produced at least one trace.
