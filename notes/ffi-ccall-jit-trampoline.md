# FFI C-Call JIT Trampoline

The first traced ordinary FFI C-call slice now records exact void, signed
32-bit integer, pointer-returning, zero-argument narrow integer, zero-argument
unsigned 32-bit/signed 64-bit/unsigned 64-bit integer, and FP-returning
function cdata on x64. The narrow GPR subset accepts 0, 1, or 2 signed 32-bit
integer/pointer arguments, plus exact zero-argument signed/unsigned 8-bit,
signed/unsigned 16-bit, unsigned 32-bit, signed 64-bit, and unsigned 64-bit
integer returns. Narrow integer returns, unsigned 32-bit returns, and
signed/unsigned 64-bit integer returns may also use the same signed 32-bit
integer/pointer GPR argument subset. The first unsigned-argument slice accepts
exact `uint32_t(uint32_t)` / `unsigned int(unsigned int)` calls. The FPR subset
accepts 0, 1, or 2 same-kind exact float or double arguments. The first mixed
one-argument subset
accepts exact `double(int32_t)`, `double(pointer)`, `double(float)`,
`int32_t(double)`, `int32_t(float)`, `pointer(double)`, `void(double)`,
`void(float)`, and `float(double)`. The recorder emits
`lj_ccall_jit_void_gpr()`, `lj_ccall_jit_i32_gpr()`,
`lj_ccall_jit_i64_gpr()`, or `lj_ccall_jit_ptr_gpr()` plus a tiny signature
code for the GPR argument shape, `lj_ccall_jit_u32_0()` /
`lj_ccall_jit_u32_gpr()` for high-bit-safe unsigned 32-bit results,
`lj_ccall_jit_u32_u32()` for the exact unsigned 32-bit argument/result shape,
`lj_ccall_jit_i64_gpr()` / `lj_ccall_jit_i64_ret_gpr()` for boxed int64
results, `lj_ccall_jit_u64_0()` / `lj_ccall_jit_u64_gpr()` for boxed uint64
results, `lj_ccall_jit_narrow_0()` / `lj_ccall_jit_narrow_gpr()` for narrow
integer results, `lj_ccall_jit_num_i32()`, `lj_ccall_jit_num_ptr()`,
`lj_ccall_jit_num_flt()`, `lj_ccall_jit_i32_num()`,
`lj_ccall_jit_i32_flt()`, `lj_ccall_jit_ptr_num()`,
`lj_ccall_jit_void_num()`, `lj_ccall_jit_void_flt()`, and
`lj_ccall_jit_flt_num()` for the mixed one-argument shapes, or
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
- zero-argument exact signed/unsigned 8-bit or 16-bit integer returns;
- exact signed/unsigned 8-bit or 16-bit integer returns with signed 32-bit
  integer/pointer arguments;
- zero-argument exact unsigned 32-bit integer returns as Lua numbers;
- exact unsigned 32-bit integer returns with signed 32-bit integer/pointer
  arguments;
- exact one-argument `uint32_t(uint32_t)` / `unsigned int(unsigned int)` calls;
- zero-argument exact signed 64-bit integer returns;
- zero-argument exact unsigned 64-bit integer returns;
- exact signed/unsigned 64-bit integer returns with signed 32-bit
  integer/pointer arguments;
- same-kind exact float/double arguments and exact float/double returns;
- exact one-argument `double(int32_t)`, `double(pointer)`, `double(float)`,
  `int32_t(double)`, `int32_t(float)`, `pointer(double)`, `void(double)`,
  `void(float)`, and `float(double)` mixed calls;
- x64 only;
- callback-blacklisted functions still abort recording;
- all other ordinary FFI calls continue to fall back to the interpreted native
  ccall path.

This gives hot `ffi.C.getpid()`, `ffi.C.abs(i)`, small
`int add(int,int)`-style loops, simple pointer-return/pointer-argument loops,
side-effecting `void f(...)` loops, zero-argument high-bit uint32 result loops,
signed-int/pointer to high-bit uint32 result loops, high-bit
uint32-argument/result loops, zero-argument narrow integer result loops,
signed-int/pointer to narrow integer result loops,
signed-int/pointer to int64/uint64 cdata-result loops, zero-argument signed
int64 and unsigned uint64 cdata-result loops, FP-only numeric call loops, and
mixed float/double one-argument calls a traced, nonblocking native-state path,
without risking the direct backend `IR_CALLXS` register/result ordering. The
full direct bridge still needs x64 lowering that brackets the foreign ABI call
without clobbering argument or result registers.

`tests/t-ffi-ccall-stopreq.c` also heats the shared `sleep_i32` trampoline until
a trace exists, starts the STOPREQ publisher only after that warmup, and catches
the shutdown error from a traced native sleep through `pcall()`. This pins the
`CCI_T`/snapshot side of the helper-call bridge.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `LUA=luajit LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
