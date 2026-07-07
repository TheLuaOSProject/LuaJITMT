# FFI CreateThread-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `void *(void *, uint64_t, void *, void *, uint32_t, void *)`;
- runtime helper `lj_ccall_jit_ptr_ptr_u64_ptr_ptr_u32_ptr()`;
- recorder matcher `crec_call_jit_ptr_ptr_u64_ptr_ptr_u32_ptr()`;
- IR call metadata `IRCALL_lj_ccall_jit_ptr_ptr_u64_ptr_ptr_u32_ptr`.

This covers the Windows CreateThread-shaped
security/stack-size/start-address/parameter/flags/thread-id ABI class without
enabling generic `IR_CALLXS`. The matcher requires a fixed non-vararg
prototype, a pointer return, pointer arguments at the security/start/parameter
and thread-id positions, an unsigned 64-bit stack-size argument, and a
high-bit-preserving unsigned 32-bit flags argument. The helper remains on the
audited native-state path and boxes the returned handle pointer as cdata.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
