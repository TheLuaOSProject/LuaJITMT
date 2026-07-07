# FFI cross-precision 64-bit mixed ccall helper

The traced native-state ccall helper family now covers exact cross-precision
two-argument FP/GPR calls with signed and unsigned 64-bit cdata arguments:

- `double(float, int64_t)` / `double(int64_t, float)`
- `double(float, uint64_t)` / `double(uint64_t, float)`
- `float(double, int64_t)` / `float(int64_t, double)`
- `float(double, uint64_t)` / `float(uint64_t, double)`

These stay on side-effecting `IRCALL` helpers and use the existing
`CCallNativeState` enter/leave/checkstop protocol. The recorder requires the
64-bit argument to be exact `int64_t` or `uint64_t` cdata, selects helper
signedness by ctype, and keeps the foreign C prototype exact so float and
double arguments are not widened or narrowed by the bridge. Float-returning
helpers are widened back to Lua numbers after the helper call.

Validation for this slice:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `src/luajit -e 'assert(loadfile("tests/t-ffi-ccall-native.lua"))'`
