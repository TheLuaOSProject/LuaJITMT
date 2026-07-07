# FFI Double UInt32 Mixed C-Call Helpers

`lj_ccall_jit_num_num_u32()` and `lj_ccall_jit_num_u32_num()` extend the
side-effecting x64 ordinary FFI ccall helper path to exact
`double(double, uint32_t)` and `double(uint32_t, double)` calls.

The recorder requires an exact unsigned 32-bit ctype for the integer slot and
uses the shared unsigned argument conversion helper before emitting the call.
This preserves high-bit `uint32_t` cdata arguments instead of routing through a
signed 32-bit helper. As with the rest of the helper-backed ccall surface, the
foreign call runs inside the `CCallNativeState` save/enter/leave/checkstop
protocol and callback-blacklisted functions abort recording.

Focused coverage in `tests/t-ffi-ccall-native.lua` traces both argument orders
with high-bit `uint32_t` values.
