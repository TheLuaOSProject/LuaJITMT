2026-07-07 int32 int/pointer/int/int ccall helper note
=======================================================

The native-state ccall helper matrix now records exact four-argument
`int32_t(int32_t, void *, int32_t, int32_t)` ABI shapes through
`lj_ccall_jit_i32_i32_ptr_i32_i32()`.

This covers epoll_wait-style descriptor, event-buffer pointer, maximum-event
count, and timeout signatures. The recorder accepts only fixed four-argument
calls with a signed 32-bit result, signed 32-bit descriptor, pointer second
argument, and signed 32-bit count/timeout arguments, then returns the helper's
plain signed 32-bit result.

The focused shared-library fixture drives signed 32-bit count and negative
timeout arguments through a hot loop and asserts that recording produced at
least one trace.
