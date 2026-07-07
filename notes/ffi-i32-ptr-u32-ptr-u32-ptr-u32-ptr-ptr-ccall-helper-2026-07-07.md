# FFI DeviceIoControl-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `int32_t(void *, uint32_t, void *, uint32_t, void *, uint32_t, void *, void *)`;
- runtime helper `lj_ccall_jit_i32_ptr_u32_ptr_u32_ptr_u32_ptr_ptr()`;
- recorder matcher `crec_call_jit_i32_ptr_u32_ptr_u32_ptr_u32_ptr_ptr()`;
- IR call metadata `IRCALL_lj_ccall_jit_i32_ptr_u32_ptr_u32_ptr_u32_ptr_ptr`.

This covers the Windows DeviceIoControl-shaped
handle/control-code/input/input-size/output/output-size/bytes-returned/
overlapped ABI class without enabling generic `IR_CALLXS`. The matcher
requires a fixed non-vararg prototype, a signed 32-bit integer return, pointer
arguments at every buffer/overlapped position, and high-bit-preserving unsigned
32-bit control-code and byte-count arguments. The helper stays on the audited
side-effecting native-state path.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
