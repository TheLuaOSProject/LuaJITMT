# FFI int/pointer/size/offset C-call helper

Added a narrow helper-backed recorder slice for exact
`int64_t(int32_t, void *, uint64_t, int64_t)` FFI C calls. This covers the
pread/pwrite-style int/pointer/size/signed-offset ABI class without enabling
generic `IR_CALLXS`.

The helper:

- preserves the signed 32-bit descriptor argument;
- preserves the pointer argument;
- preserves the unsigned 64-bit size argument;
- preserves the signed 64-bit offset argument as int64 cdata input;
- boxes the signed 64-bit result through the existing `IR_CNEWI` path;
- uses the existing `CCallNativeState` save/enter/leave/checkstop discipline;
- rejects callbacks through the existing callback blacklist check.

Validation plan for this slice:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `src/luajit -e 'assert(loadfile("tests/t-ffi-ccall-native.lua"))'`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
- `git diff --check`
