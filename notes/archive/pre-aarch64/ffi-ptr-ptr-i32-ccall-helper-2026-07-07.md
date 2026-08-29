# FFI pointer-pointer signed-length ccall helper

The traced native-state ccall helper family now covers more exact
`void *, void *, int32_t` ABI shapes:

- `uint32_t(void *, void *, int32_t)`
- `uint64_t(void *, void *, int32_t)`
- `void(void *, void *, int32_t)`
- `void *(void *, void *, int32_t)`

The existing signed `int32_t` and signed `int64_t` return helpers now select
their unsigned siblings when the return ctype is unsigned. The new void and
pointer helpers keep side effects and pointer results on the side-effecting
`IRCALL` path with the same `CCallNativeState` enter/leave/checkstop protocol
as interpreted FFI calls. The recorder only accepts exact fixed-argument
function cdata with pointer, pointer, signed 32-bit integer arguments.

Validation for this slice:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `src/luajit -e 'assert(loadfile("tests/t-ffi-ccall-native.lua"))'`
