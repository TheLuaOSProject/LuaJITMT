# FFI C-Call JIT Trampoline

The first traced ordinary FFI C-call slice now records exact void, signed
32-bit integer, pointer-returning, zero-argument unsigned 32-bit/signed
64-bit/unsigned 64-bit integer, and FP-returning function cdata on x64. The
narrow GPR subset accepts 0, 1, or 2 signed 32-bit integer/pointer arguments,
plus exact zero-argument unsigned 32-bit, signed 64-bit, and unsigned 64-bit
integer returns. The FPR subset accepts 0, 1, or 2 same-kind exact float or
double arguments. The recorder emits
`lj_ccall_jit_void_gpr()`, `lj_ccall_jit_i32_gpr()`,
`lj_ccall_jit_i64_gpr()`, or `lj_ccall_jit_ptr_gpr()` plus a tiny signature
code for the GPR argument shape, `lj_ccall_jit_u32_0()` for high-bit-safe
unsigned 32-bit results, `lj_ccall_jit_u64_0()` for boxed uint64 results, or
`lj_ccall_jit_num_fpr()` / `lj_ccall_jit_flt_fpr()` for the FP-only FPR shapes.

This does not enable the old direct `IR_CALLXS` path. Each helper is emitted as
a side-effecting `IRCALL` with an implicit `lua_State *`; the recorder converts
the supported integer/pointer arguments up front, and the helper then calls the
foreign function from C while using the same `CCallNativeState`
save/enter/leave/checkstop protocol as the interpreted `lj_ccall_func()` path.

The scope is deliberately narrow:

- fixed arguments only;
- exactly 0, 1, or 2 Lua arguments;
- exact signed 32-bit integer or pointer argument types;
- void, exact signed 32-bit integer, or pointer return types;
- zero-argument exact unsigned 32-bit integer returns as Lua numbers;
- zero-argument exact signed 64-bit integer returns;
- zero-argument exact unsigned 64-bit integer returns;
- same-kind exact float/double arguments and exact float/double returns;
- x64 only;
- callback-blacklisted functions still abort recording;
- all other ordinary FFI calls continue to fall back to the interpreted native
  ccall path.

This gives hot `ffi.C.getpid()`, `ffi.C.abs(i)`, small
`int add(int,int)`-style loops, simple pointer-return/pointer-argument loops,
side-effecting `void f(...)` loops, zero-argument high-bit uint32 result loops,
zero-argument signed int64 and unsigned uint64 cdata-result loops, and FP-only
numeric call loops a traced, nonblocking native-state path without risking the
direct backend `IR_CALLXS` register/result ordering. The full direct bridge
still needs x64 lowering that brackets the foreign ABI call without clobbering
argument or result registers.

`tests/t-ffi-ccall-stopreq.c` also heats the shared `sleep_i32` trampoline until
a trace exists, starts the STOPREQ publisher only after that warmup, and catches
the shutdown error from a traced native sleep through `pcall()`. This pins the
`CCI_T`/snapshot side of the helper-call bridge.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `LUA=luajit LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
