# FFI C-Call JIT Trampoline

The first traced ordinary FFI C-call slice now records exact `int f(int)`
function cdata on x64 through `lj_ccall_jit_i32_i32()`.

This does not enable the old direct `IR_CALLXS` path. The helper is emitted as a
side-effecting `IRCALL` with an implicit `lua_State *`, converts the single
argument during recording, and then calls the foreign function from C while
using the same `CCallNativeState` save/enter/leave/checkstop protocol as the
interpreted `lj_ccall_func()` path.

The scope is deliberately narrow:

- fixed arguments only;
- exactly one Lua argument;
- exact signed 32-bit integer argument and return type;
- x64 only;
- callback-blacklisted functions still abort recording;
- all other ordinary FFI calls continue to fall back to the interpreted native
  ccall path.

This gives hot `ffi.C.abs(i)`-style loops a traced, nonblocking native-state
path without risking the direct backend `IR_CALLXS` register/result ordering.
The full direct bridge still needs x64 lowering that brackets the foreign ABI
call without clobbering argument or result registers.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `LUA=luajit LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
