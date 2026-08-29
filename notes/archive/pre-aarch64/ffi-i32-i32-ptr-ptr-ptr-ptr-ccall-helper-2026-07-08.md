2026-07-08 int32 int/pointer/pointer/pointer/pointer ccall helper note
========================================================================

The native-state ccall helper matrix now records exact five-argument
`int32_t(int32_t, void *, void *, void *, void *)` ABI shapes through
`lj_ccall_jit_i32_i32_ptr_ptr_ptr_ptr()`.

This covers select-style nfds, read fd-set pointer, write fd-set pointer,
exception fd-set pointer, and timeout pointer signatures. The recorder accepts
only fixed five-argument calls with a signed 32-bit result, one signed 32-bit
integer argument, and four pointer arguments.

The focused shared-library fixture drives all pointer arguments through a hot
loop, mutates the read-fd buffer through the pointer argument, and asserts that
recording produced at least one trace.
