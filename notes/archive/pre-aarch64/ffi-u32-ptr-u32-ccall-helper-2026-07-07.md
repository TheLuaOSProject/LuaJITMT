# FFI WaitForSingleObject-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `uint32_t(void *, uint32_t)`;
- runtime helper `lj_ccall_jit_u32_ptr_u32()`;
- recorder matcher `crec_call_jit_u32_ptr_u32()`;
- IR call metadata `IRCALL_lj_ccall_jit_u32_ptr_u32`.

This covers the Windows WaitForSingleObject-shaped handle/timeout ABI class
without enabling generic `IR_CALLXS`. The matcher requires a fixed non-vararg
prototype, an unsigned 32-bit return, a pointer handle argument, and an exact
unsigned 32-bit timeout argument. The helper remains on the audited
native-state path and converts the DWORD result to a Lua number so high-bit
results are preserved.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
