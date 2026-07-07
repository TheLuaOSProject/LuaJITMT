# FFI ReadFile/WriteFile-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `int32_t(void *, void *, uint32_t, void *, void *)`;
- runtime helper `lj_ccall_jit_i32_ptr_ptr_u32_ptr_ptr()`;
- recorder matcher `crec_call_jit_i32_ptr_ptr_u32_ptr_ptr()`;
- IR call metadata `IRCALL_lj_ccall_jit_i32_ptr_ptr_u32_ptr_ptr`.

This covers the Windows ReadFile/WriteFile-shaped
handle/buffer/byte-count/output/overlapped ABI class without enabling generic
`IR_CALLXS`. The matcher requires a fixed non-vararg prototype, a signed
32-bit integer return, two leading pointer arguments, a high-bit-preserving
unsigned 32-bit byte-count argument, and two trailing pointer arguments. The
helper stays on the audited side-effecting native-state path.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
