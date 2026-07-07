# FFI MapViewOfFileEx-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `void *(void *, uint32_t, uint32_t, uint32_t, uint64_t, void *)`;
- runtime helper `lj_ccall_jit_ptr_ptr_u32_u32_u32_u64_ptr()`;
- recorder matcher `crec_call_jit_ptr_ptr_u32_u32_u32_u64_ptr()`;
- IR call metadata `IRCALL_lj_ccall_jit_ptr_ptr_u32_u32_u32_u64_ptr`.

This covers the Windows MapViewOfFileEx-shaped
handle/access/offset-high/offset-low/size/desired-base ABI class without
enabling generic `IR_CALLXS`. The matcher requires a fixed non-vararg
prototype, a pointer return, a pointer first argument, three unsigned 32-bit
arguments, a final unsigned 64-bit size argument, and a final desired-base
pointer argument. The helper stays on the audited side-effecting native-state
path and boxes the pointer result as cdata after return.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
