2026-07-07 void pointer/int32/pointer ccall helper note
========================================================

The native-state ccall helper matrix now records exact three-argument
`void *, int32_t, void *` ABI shapes for void-returning side-effect calls
through `lj_ccall_jit_void_ptr_i32_ptr()`.

This covers CloseThreadpoolCleanupGroupMembers-style cleanup group,
cancel-pending flag, and context/reserved pointer signatures without enabling
generic direct `IR_CALLXS`. The recorder accepts only fixed three-argument
calls with a void result, pointer first argument, signed 32-bit integer second
argument, and pointer third argument, then emits a side-effecting `IRCALL`
helper that enters and leaves native state around the exact C prototype.

The focused shared-library fixture drives a negative signed 32-bit flag and
two independent pointer arguments through a hot loop. The loop asserts that
the helper's side effects are visible and that recording produced at least one
trace, so this shape cannot silently fall back to interpretation.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN) TARGET_STRIP=:`
- `src/luajit -e 'assert(loadfile("tests/t-ffi-ccall-native.lua"))'`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
