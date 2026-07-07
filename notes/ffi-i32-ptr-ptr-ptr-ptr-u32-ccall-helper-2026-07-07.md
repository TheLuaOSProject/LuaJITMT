# FFI GetQueuedCompletionStatus-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `int32_t(void *, void *, void *, void *, uint32_t)`;
- runtime helper `lj_ccall_jit_i32_ptr_ptr_ptr_ptr_u32()`;
- recorder matcher `crec_call_jit_i32_ptr_ptr_ptr_ptr_u32()`;
- IR call metadata `IRCALL_lj_ccall_jit_i32_ptr_ptr_ptr_ptr_u32`.

This covers the Windows GetQueuedCompletionStatus-shaped
completion-port/byte-count-output/completion-key-output/overlapped-output/
timeout ABI class without enabling generic `IR_CALLXS`. The matcher requires a
fixed non-vararg prototype, a signed 32-bit result, four pointer arguments, and
an exact unsigned 32-bit timeout. The helper remains on the audited
native-state path.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
