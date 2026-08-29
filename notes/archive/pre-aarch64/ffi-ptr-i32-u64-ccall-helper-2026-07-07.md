2026-07-07 pointer/int32/uint64 ccall helper note
==================================================

The native-state ccall helper matrix now records exact three-argument
`void *, int32_t, uint64_t` ABI shapes:

- `int32_t(void *, int32_t, uint64_t)` through
  `lj_ccall_jit_i32_ptr_i32_u64()`;
- `uint32_t(void *, int32_t, uint64_t)` through
  `lj_ccall_jit_u32_ptr_i32_u64()`;
- `int64_t(void *, int32_t, uint64_t)` through
  `lj_ccall_jit_i64_ptr_i32_u64()`;
- `uint64_t(void *, int32_t, uint64_t)` through
  `lj_ccall_jit_u64_ptr_i32_u64()`;
- `void(void *, int32_t, uint64_t)` through
  `lj_ccall_jit_void_ptr_i32_u64()`;
- `void *(void *, int32_t, uint64_t)` through
  `lj_ccall_jit_ptr_ptr_i32_u64()`.

This covers memset/memchr-style x64 ABI classes without enabling broad direct
`IR_CALLXS`: the middle argument remains an exact signed 32-bit integer, the
final size remains an exact unsigned 64-bit argument, and the helper enters and
leaves native state around the foreign call before checking fresh STOPREQ.

The shared-library fixture drives high-bit `uint64_t` size values through
signed and unsigned 32-bit returns, boxed signed and unsigned 64-bit returns,
pointer returns, and void side effects. Each loop requires at least one trace,
so these signatures cannot silently fall back to the interpreted native ccall
path.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `src/luajit -e 'assert(loadfile("tests/t-ffi-ccall-native.lua"))'`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
