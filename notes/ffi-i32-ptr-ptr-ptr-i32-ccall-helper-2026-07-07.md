# FFI GetOverlappedResult-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `int32_t(void *, void *, void *, int32_t)`;
- runtime helper `lj_ccall_jit_i32_ptr_ptr_ptr_i32()`;
- recorder matcher `crec_call_jit_i32_ptr_ptr_ptr_i32()`;
- IR call metadata `IRCALL_lj_ccall_jit_i32_ptr_ptr_ptr_i32`.

This covers the Windows GetOverlappedResult-shaped
handle/overlapped/output/wait ABI class without enabling generic `IR_CALLXS`.
The matcher requires a fixed non-vararg prototype, a signed 32-bit integer
return, three pointer arguments, and a final signed 32-bit wait flag. The
helper stays on the audited side-effecting native-state path.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
