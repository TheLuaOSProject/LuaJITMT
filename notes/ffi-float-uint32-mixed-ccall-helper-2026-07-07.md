# FFI Float UInt32 Mixed C-Call Helpers

`lj_ccall_jit_flt_flt_u32()` and `lj_ccall_jit_flt_u32_flt()` extend the
helper-backed x64 ordinary FFI ccall recorder to exact
`float(float, uint32_t)` and `float(uint32_t, float)` calls.

The recorder requires exact `float` and unsigned 32-bit ctype slots, routes the
unsigned argument through the shared high-bit-preserving conversion helper, and
widens the helper's IR `FLOAT` result to Lua number after the call. The actual
foreign call remains inside the same `CCallNativeState`
save/enter/leave/checkstop protocol as the interpreted ccall path.

Focused coverage traces both argument orders with high-bit `uint32_t` cdata
values in `tests/t-ffi-ccall-native.lua`.
