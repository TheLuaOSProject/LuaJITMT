2026-07-07 pointer/int32/uint32 ccall helper note
==================================================

The native-state ccall helper matrix now records exact three-argument
`void *, int32_t, uint32_t` ABI shapes:

- `int32_t(void *, int32_t, uint32_t)` through
  `lj_ccall_jit_i32_ptr_i32_u32()`;
- `uint32_t(void *, int32_t, uint32_t)` through
  `lj_ccall_jit_u32_ptr_i32_u32()`;
- `int64_t(void *, int32_t, uint32_t)` through
  `lj_ccall_jit_i64_ptr_i32_u32()`;
- `uint64_t(void *, int32_t, uint32_t)` through
  `lj_ccall_jit_u64_ptr_i32_u32()`;
- `void(void *, int32_t, uint32_t)` through
  `lj_ccall_jit_void_ptr_i32_u32()`;
- `void *(void *, int32_t, uint32_t)` through
  `lj_ccall_jit_ptr_ptr_i32_u32()`.

This covers open/shm_open/sem_init-style x64 ABI classes without enabling
generic direct `IR_CALLXS`: the pointer, signed 32-bit flags, and unsigned
32-bit mode/value argument are converted by the recorder up front and then
passed to an exact C function pointer prototype inside a native-state helper.

The shared-library fixture drives high-bit `uint32_t` values through signed and
unsigned 32-bit returns, boxed signed and unsigned 64-bit returns, pointer
returns, and void side effects. Each loop requires at least one trace, so these
signatures cannot silently fall back to the interpreted native ccall path.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `src/luajit -e 'assert(loadfile("tests/t-ffi-ccall-native.lua"))'`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
