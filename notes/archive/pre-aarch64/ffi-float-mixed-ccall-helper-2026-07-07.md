# FFI Float Mixed C-Call Helpers

`lj_ccall_jit_flt_flt_i32()` and `lj_ccall_jit_flt_i32_flt()` extend the
side-effecting x64 ordinary FFI ccall helper path to exact two-argument mixed
float/signed-int shapes: `float(float, int32_t)` and
`float(int32_t, float)`.

The recorder keeps the same constraints as the earlier mixed double-returning
helpers: fixed arguments only, exact `float` return, exact signed 32-bit integer
where requested, exact `float` where requested, callback blacklist checks, and
the shared `CCallNativeState` save/enter/leave/checkstop protocol in the helper.
The helpers return IR `FLOAT`; the recorder widens the result to Lua number
after the call.

Focused shared-library coverage traces both argument orders through
`tests/t-ffi-ccall-native.lua` without adding more main-scope locals to that
already limit-sized test file.
