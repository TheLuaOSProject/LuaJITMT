2026-07-07 int32 int/int/int ccall helper note
===============================================

The native-state ccall helper matrix now records exact three-argument
`int32_t(int32_t, int32_t, int32_t)` ABI shapes through
`lj_ccall_jit_i32_i32_i32_i32()`.

This covers socket-style domain, type, and protocol signatures. The recorder
accepts only fixed three-argument calls with a signed 32-bit result and three
signed 32-bit arguments, then returns the helper's plain signed 32-bit result.

The focused shared-library fixture drives cdata signed 32-bit type/protocol
arguments through a hot loop and asserts that recording produced at least one
trace.
