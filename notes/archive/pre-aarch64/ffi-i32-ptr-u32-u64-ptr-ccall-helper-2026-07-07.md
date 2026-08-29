# FFI PostQueuedCompletionStatus-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `int32_t(void *, uint32_t, uint64_t, void *)`;
- runtime helper `lj_ccall_jit_i32_ptr_u32_u64_ptr()`;
- recorder matcher `crec_call_jit_i32_ptr_u32_u64_ptr()`;
- IR call metadata `IRCALL_lj_ccall_jit_i32_ptr_u32_u64_ptr`.

This covers the Windows PostQueuedCompletionStatus-shaped
completion-port/byte-count/completion-key/overlapped-pointer ABI class without
enabling generic `IR_CALLXS`. The matcher requires a fixed non-vararg
prototype, a signed 32-bit result, a pointer completion-port argument, an exact
unsigned 32-bit transferred byte count, an exact unsigned 64-bit completion
key, and a pointer overlapped argument. The helper remains on the audited
native-state path.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
