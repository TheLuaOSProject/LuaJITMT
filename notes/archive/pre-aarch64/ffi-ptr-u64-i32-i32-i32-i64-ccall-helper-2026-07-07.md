# FFI mmap-shaped C-call helper

Added a narrow helper-backed recorder slice for exact
`void *(void *, uint64_t, int32_t, int32_t, int32_t, int64_t)` FFI C calls.
This covers the POSIX/macOS mmap-shaped pointer/size/prot/flags/fd/offset ABI
class without enabling generic `IR_CALLXS`.

The helper:

- preserves the pointer address argument;
- preserves the unsigned 64-bit size argument;
- preserves signed 32-bit protection, mapping flag, and descriptor arguments;
- preserves the signed 64-bit offset argument as int64 cdata input;
- boxes the pointer result through the existing `IR_CNEWI` path;
- uses the existing `CCallNativeState` save/enter/leave/checkstop discipline;
- rejects callbacks through the existing callback blacklist check.

Validation plan for this slice:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `src/luajit -e 'assert(loadfile("tests/t-ffi-ccall-native.lua"))'`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
- `git diff --check`
