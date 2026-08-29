# FFI CreateFile-shaped C-call helper

Added exact traced native-state FFI C-call coverage for:

- `void *(void *, uint32_t, uint32_t, void *, uint32_t, uint32_t, void *)`;
- runtime helper `lj_ccall_jit_ptr_ptr_u32_u32_ptr_u32_u32_ptr()`;
- recorder matcher `crec_call_jit_ptr_ptr_u32_u32_ptr_u32_u32_ptr()`;
- IR call metadata `IRCALL_lj_ccall_jit_ptr_ptr_u32_u32_ptr_u32_u32_ptr`.

This covers the Windows CreateFile-shaped
path/desired-access/share/security/disposition/flags/template ABI class
without enabling generic `IR_CALLXS`. The matcher requires a fixed non-vararg
prototype, a pointer return, pointer arguments at the path/security/template
positions, and high-bit-preserving unsigned 32-bit DWORD arguments for desired
access, share mode, disposition, and flags. The helper stays on the audited
side-effecting native-state path and boxes the returned handle pointer as
cdata after return.

Validation plan:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
