2026-07-07 uint64 pointer/size/size/pointer ccall helper note
==============================================================

The native-state ccall helper matrix now records exact four-argument
`uint64_t(void *, uint64_t, uint64_t, void *)` ABI shapes through
`lj_ccall_jit_u64_ptr_u64_u64_ptr()`.

This covers fread/fwrite-style buffer, element-size, element-count, and stream
pointer signatures on x86_64 targets where `size_t` is an unsigned 64-bit
integer. The recorder accepts only fixed four-argument calls with an unsigned
64-bit result, pointer first argument, two unsigned 64-bit size arguments, and
a pointer fourth argument, then boxes the unsigned 64-bit result as cdata.

The focused shared-library fixture drives high-bit unsigned 64-bit size/count
arguments through a hot loop and asserts that recording produced at least one
trace.
