2026-07-07 int64 int/pointer/size/int ccall helper note
========================================================

The native-state ccall helper matrix now records exact four-argument
`int64_t(int32_t, void *, uint64_t, int32_t)` ABI shapes through
`lj_ccall_jit_i64_i32_ptr_u64_i32()`.

This covers send/recv-style descriptor, buffer pointer, byte count, and flags
signatures on x86_64 targets where `ssize_t` is a signed 64-bit integer and
`size_t` is an unsigned 64-bit integer. The recorder accepts only fixed
four-argument calls with a signed 64-bit result, signed 32-bit descriptor,
pointer buffer, unsigned 64-bit byte count, and signed 32-bit flags argument,
then boxes the signed 64-bit result as cdata.

The focused shared-library fixture drives a high-bit unsigned 64-bit size and
a negative signed 32-bit flags value through a hot loop and asserts that
recording produced at least one trace.
