# FFI CreateIoCompletionPort-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `void *(void *, void *, uint64_t, uint32_t)`;
- runtime helper `lj_ccall_jit_ptr_ptr_ptr_u64_u32()`;
- recorder matcher `crec_call_jit_ptr_ptr_ptr_u64_u32()`;
- IR call metadata `IRCALL_lj_ccall_jit_ptr_ptr_ptr_u64_u32`.

This covers the Windows CreateIoCompletionPort-shaped
file-handle/existing-completion-port/completion-key/thread-count ABI class
without enabling generic `IR_CALLXS`. The matcher requires a fixed non-vararg
prototype, a pointer result, pointer arguments for both handles, an exact
unsigned 64-bit completion key, and an exact unsigned 32-bit thread count. The
helper remains on the audited native-state path and boxes the returned pointer
with the original pointer ctype.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
