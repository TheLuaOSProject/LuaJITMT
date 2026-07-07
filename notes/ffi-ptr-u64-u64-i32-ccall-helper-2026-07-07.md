# FFI mremap-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `void *(void *, uint64_t, uint64_t, int32_t)`;
- runtime helper `lj_ccall_jit_ptr_ptr_u64_u64_i32()`;
- recorder matcher `crec_call_jit_ptr_ptr_u64_u64_i32()`;
- IR call metadata `IRCALL_lj_ccall_jit_ptr_ptr_u64_u64_i32`.

This covers the common fixed Linux mremap-shaped
pointer/old-size/new-size/flags ABI class without enabling generic
`IR_CALLXS`. The matcher requires a fixed non-vararg prototype, a pointer
return, pointer first argument, two unsigned 64-bit size arguments, and a
signed 32-bit flags argument. The helper stays on the audited side-effecting
native-state path and boxes the pointer result as cdata after return.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
