# FFI pointer/size/flags/output-pointer C-call helper

Added a narrow helper-backed recorder slice for exact
`int32_t(void *, uint64_t, uint32_t, void *)` FFI C calls. This covers the
VirtualProtect-style pointer/size/flags/output-pointer ABI class without
reenabling generic `IR_CALLXS`.

The helper:

- preserves the unsigned 64-bit size argument;
- preserves high-bit unsigned 32-bit flags through `crec_call_jit_u32_arg()`;
- preserves the trailing output pointer argument;
- uses the existing `CCallNativeState` save/enter/leave/checkstop discipline;
- rejects callbacks through the existing callback blacklist check.

Validation plan for this slice:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `src/luajit -e 'assert(loadfile("tests/t-ffi-ccall-native.lua"))'`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
- `git diff --check`
