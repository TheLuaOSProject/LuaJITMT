# FFI WaitForMultipleObjectsEx-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `uint32_t(uint32_t, void *, int32_t, uint32_t, int32_t)`;
- runtime helper `lj_ccall_jit_u32_u32_ptr_i32_u32_i32()`;
- recorder matcher `crec_call_jit_u32_u32_ptr_i32_u32_i32()`;
- IR call metadata `IRCALL_lj_ccall_jit_u32_u32_ptr_i32_u32_i32`.

This covers the Windows WaitForMultipleObjectsEx-shaped
count/handle-array/wait-all/timeout/alertable ABI class without enabling
generic `IR_CALLXS`. The matcher requires a fixed non-vararg prototype, an
unsigned 32-bit return, an exact unsigned 32-bit count, a pointer handle-array
argument, exact signed 32-bit boolean flags, and an exact unsigned 32-bit
timeout. The helper remains on the audited native-state path and converts the
DWORD result to a Lua number so high-bit results are preserved.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
