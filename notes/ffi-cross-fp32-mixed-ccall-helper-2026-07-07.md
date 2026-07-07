# FFI cross-precision 32-bit mixed ccall helper

The traced native-state ccall helper family now covers exact cross-precision
two-argument FP/GPR calls with 32-bit integer arguments:

- `double(float, int32_t)` / `double(int32_t, float)`
- `double(float, uint32_t)` / `double(uint32_t, float)`
- `float(double, int32_t)` / `float(int32_t, double)`
- `float(double, uint32_t)` / `float(uint32_t, double)`

These stay on side-effecting `IRCALL` helpers rather than direct `IR_CALLXS`.
Each helper calls through the exact C prototype so float and double arguments
keep their ABI width, wraps the foreign call with `CCallNativeState`, and uses
the existing unsigned 32-bit recorder conversion to preserve high-bit `uint32_t`
arguments. Float-returning helpers are widened back to Lua numbers after the
helper call, matching the earlier float ccall slices.

Validation for this slice:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `src/luajit -e 'assert(loadfile("tests/t-ffi-ccall-native.lua"))'`
