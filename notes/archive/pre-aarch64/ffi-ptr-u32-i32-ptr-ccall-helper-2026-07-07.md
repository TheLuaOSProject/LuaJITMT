# FFI OpenEvent/OpenFileMapping-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `void *(uint32_t, int32_t, void *)`;
- runtime helper `lj_ccall_jit_ptr_u32_i32_ptr()`;
- recorder matcher `crec_call_jit_ptr_u32_i32_ptr()`;
- IR call metadata `IRCALL_lj_ccall_jit_ptr_u32_i32_ptr`.

This covers Windows OpenEvent/OpenMutex/OpenSemaphore/OpenFileMapping-shaped
desired-access/inherit/name ABI classes without enabling generic `IR_CALLXS`.
The matcher requires a fixed non-vararg prototype, a pointer return, an exact
unsigned 32-bit desired-access argument, an exact signed 32-bit inherit flag,
and a pointer name argument. The helper remains on the audited native-state
path and boxes the returned handle pointer as cdata.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
