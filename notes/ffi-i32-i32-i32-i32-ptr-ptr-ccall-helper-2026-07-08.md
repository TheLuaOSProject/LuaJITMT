2026-07-08 int32 int/int/int/pointer/pointer ccall helper note
================================================================

The native-state ccall helper matrix now records exact five-argument
`int32_t(int32_t, int32_t, int32_t, void *, void *)` ABI shapes through
`lj_ccall_jit_i32_i32_i32_i32_ptr_ptr()`.

This covers getsockopt-style descriptor, level, option name, option-value
pointer, and option-length pointer signatures. The recorder accepts only fixed
five-argument calls with a signed 32-bit result, three signed 32-bit integer
arguments, and pointer fourth and fifth arguments.

The focused shared-library fixture drives a mutable option-length pointer
through a hot loop, mutates the option-value buffer through the pointer
argument, and asserts that recording produced at least one trace.
