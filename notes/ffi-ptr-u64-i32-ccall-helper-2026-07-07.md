2026-07-07 pointer/uint64/int32 ccall helper note
==================================================

The native-state ccall helper matrix now records exact three-argument
`void *, uint64_t, int32_t` ABI shapes:

- `int32_t(void *, uint64_t, int32_t)` through
  `lj_ccall_jit_i32_ptr_u64_i32()`;
- `uint32_t(void *, uint64_t, int32_t)` through
  `lj_ccall_jit_u32_ptr_u64_i32()`;
- `int64_t(void *, uint64_t, int32_t)` through
  `lj_ccall_jit_i64_ptr_u64_i32()`;
- `uint64_t(void *, uint64_t, int32_t)` through
  `lj_ccall_jit_u64_ptr_u64_i32()`;
- `void(void *, uint64_t, int32_t)` through
  `lj_ccall_jit_void_ptr_u64_i32()`;
- `void *(void *, uint64_t, int32_t)` through
  `lj_ccall_jit_ptr_ptr_u64_i32()`.

This covers mprotect/madvise-style x64 ABI classes without enabling generic
direct `IR_CALLXS`: the pointer, unsigned 64-bit size, and trailing signed
32-bit argument are converted by the recorder up front and then passed to an
exact C function pointer prototype inside a native-state helper.

The shared-library fixture drives high-bit `uint64_t` size values and signed
`int32_t` flag values through signed and unsigned 32-bit returns, boxed signed
and unsigned 64-bit returns, pointer returns, and void side effects. Each loop
requires at least one trace, so these signatures cannot silently fall back to
the interpreted native ccall path.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `src/luajit -e 'assert(loadfile("tests/t-ffi-ccall-native.lua"))'`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
