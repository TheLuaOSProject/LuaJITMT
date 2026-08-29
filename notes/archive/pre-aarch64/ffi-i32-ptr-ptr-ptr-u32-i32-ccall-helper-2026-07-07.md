# FFI GetOverlappedResultEx-shaped C-call helper

Added the exact signed 32-bit result
`int32_t(void *, void *, void *, uint32_t, int32_t)` FFI C-call trace slice:

- runtime helper `lj_ccall_jit_i32_ptr_ptr_ptr_u32_i32()`;
- recorder matcher `crec_call_jit_i32_ptr_ptr_ptr_u32_i32()`;
- IR call metadata `IRCALL_lj_ccall_jit_i32_ptr_ptr_ptr_u32_i32`.

This covers the Windows GetOverlappedResultEx-shaped
handle/overlapped/output/timeout/alertable ABI class without falling back to
the interpreter for hot loops. The recorder keeps the timeout as an exact
unsigned 32-bit value through `crec_call_jit_u32_arg()` and keeps the alertable
flag on the signed 32-bit path.

The `m7_ffi_ccall_native` shared-library gate now runs a hot loop through the
new helper with a high-bit timeout and negative alertable flag, checks the
output pointer side effect, and asserts the loop traces.
