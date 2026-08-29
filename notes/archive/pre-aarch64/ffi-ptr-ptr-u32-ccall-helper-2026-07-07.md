2026-07-07 pointer/pointer/uint32 ccall helper note
====================================================

The native-state ccall helper matrix now records exact three-argument
`void *, void *, uint32_t` ABI shapes for the return families that already had
size and signed-length coverage:

- `int32_t(void *, void *, uint32_t)` through
  `lj_ccall_jit_i32_ptr_ptr_u32()`;
- `uint32_t(void *, void *, uint32_t)` through
  `lj_ccall_jit_u32_ptr_ptr_u32()`;
- `int64_t(void *, void *, uint32_t)` through
  `lj_ccall_jit_i64_ptr_ptr_u32()`;
- `uint64_t(void *, void *, uint32_t)` through
  `lj_ccall_jit_u64_ptr_ptr_u32()`;
- `void(void *, void *, uint32_t)` through
  `lj_ccall_jit_void_ptr_ptr_u32()`;
- `void *(void *, void *, uint32_t)` through
  `lj_ccall_jit_ptr_ptr_ptr_u32()`.

This is intentionally separate from the existing `uint64_t` size helpers:
Windows/DWORD-style counts and other unsigned 32-bit lengths keep the third
argument in the 32-bit integer ABI class, including high-bit values such as
`0xf00000f2`. The recorder accepts only fixed three-argument calls whose first
two arguments are pointers and whose third argument is an unsigned 32-bit
integer, converts that argument with the regular unsigned-32 ccall conversion
path, and emits side-effecting `IRCALL` helpers that enter and leave native
state around the final C call.

The focused shared-library fixture checks signed and unsigned 32-bit returns,
boxed signed and unsigned 64-bit returns, pointer returns, and void side
effects. Each loop asserts that recording produced at least one trace so these
prototypes cannot silently fall back to interpretation.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `src/luajit -e 'assert(loadfile("tests/t-ffi-ccall-native.lua"))'`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
