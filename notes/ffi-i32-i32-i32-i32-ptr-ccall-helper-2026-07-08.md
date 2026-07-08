2026-07-08 int32 int/int/int/pointer ccall helper note
=======================================================

The native-state ccall helper matrix now records exact four-argument
`int32_t(int32_t, int32_t, int32_t, void *)` ABI shapes through
`lj_ccall_jit_i32_i32_i32_i32_ptr()`.

This covers epoll_ctl-style descriptor, operation, target descriptor, and
event pointer signatures. The recorder accepts only fixed four-argument calls
with a signed 32-bit result, three signed 32-bit integer arguments, and a
pointer fourth argument.

The focused shared-library fixture drives a mutable event pointer through a
hot loop, mutates the event buffer through the pointer argument, and asserts
that recording produced at least one trace.
