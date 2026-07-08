2026-07-08 int32 int/int/pointer/pointer ccall helper note
===========================================================

The native-state ccall helper matrix now records exact four-argument
`int32_t(int32_t, int32_t, void *, void *)` ABI shapes through
`lj_ccall_jit_i32_i32_i32_ptr_ptr()`.

This covers timerfd- and clock-style descriptor, flags, request pointer, and
output pointer signatures. The recorder accepts only fixed four-argument calls
with a signed 32-bit result, two signed 32-bit integer arguments, and pointer
third and fourth arguments.

The focused shared-library fixture drives both pointer arguments through a hot
loop, mutates the output buffer through the pointer argument, and asserts that
recording produced at least one trace.
