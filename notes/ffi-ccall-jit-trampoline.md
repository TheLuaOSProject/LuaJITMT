# FFI C-Call JIT Trampoline

The first traced ordinary FFI C-call slice now records exact void, signed
32-bit integer, pointer-returning, zero-argument narrow integer, zero-argument
unsigned 32-bit/signed 64-bit/unsigned 64-bit integer, and FP-returning
function cdata on x64. The narrow GPR subset accepts 0, 1, or 2 signed 32-bit,
unsigned 32-bit, or pointer arguments, plus exact zero-argument signed/unsigned
8-bit, signed/unsigned 16-bit, unsigned 32-bit, signed 64-bit, and unsigned
64-bit integer returns. Narrow integer returns, unsigned 32-bit returns, and
signed/unsigned 64-bit integer returns may also use the same signed
32-bit/unsigned 32-bit/pointer GPR argument subset. The exact 64-bit
cdata-result slices accept one or two `int64_t` and/or `uint64_t` arguments,
with the helper signature preserving signedness for each argument. The first
64-bit GPR argument slice also accepts exact one-argument `int64_t` or
`uint64_t` calls for void, signed 32-bit, unsigned 32-bit, narrow integer, and
pointer returns, plus exact two-argument `pointer,int64_t` and
`pointer,uint64_t` span-style calls and exact `int32_t,int64_t` calls for
those same return families. The FPR subset accepts 0, 1, or 2 same-kind exact
float or double arguments. The first mixed one-argument subset
accepts exact `double(int32_t)`, `double(pointer)`, `double(float)`,
`int32_t(double)`, `int32_t(float)`, `int32_t(int8_t)`, `pointer(double)`,
`void(double)`, `void(float)`, and `float(double)`. The first exact
three-argument subset accepts `int32_t(void *, unsigned long, int32_t)` calls,
including the common `poll(nil, 0, 0)` shape, plus the POSIX-shaped
`int64_t(int32_t, pointer, uint64_t)` class used by calls such as
`ssize_t write(int, void *, size_t)`, and the UCRT-shaped
`int32_t(int32_t, pointer, uint32_t)` class used by calls such as
`int _write(int, void *, unsigned int)`, plus the seek-shaped
`int64_t(int32_t, int64_t, int32_t)` class used by calls such as
`off_t lseek(int, off_t, int)` and `_lseeki64()`. The recorder emits
`lj_ccall_jit_void_gpr()`, `lj_ccall_jit_i32_gpr()`,
`lj_ccall_jit_i64_gpr()`, or `lj_ccall_jit_ptr_gpr()` plus a tiny signature
code for the GPR argument shape, `lj_ccall_jit_u32_0()` /
`lj_ccall_jit_u32_gpr()` for high-bit-safe unsigned 32-bit results,
`lj_ccall_jit_u32_u32()` for the exact unsigned 32-bit argument/result shape,
`lj_ccall_jit_i64_gpr()` / `lj_ccall_jit_i64_ret_gpr()` for boxed int64
results, `lj_ccall_jit_u64_0()` / `lj_ccall_jit_u64_gpr()` /
`lj_ccall_jit_u64_u64()` for boxed uint64 results,
`lj_ccall_jit_narrow_0()` / `lj_ccall_jit_narrow_gpr()` for narrow
integer results, `lj_ccall_jit_num_i32()`, `lj_ccall_jit_num_ptr()`,
`lj_ccall_jit_num_flt()`, `lj_ccall_jit_i32_num()`,
`lj_ccall_jit_i32_flt()`, `lj_ccall_jit_i32_i8()`,
`lj_ccall_jit_ptr_num()`,
`lj_ccall_jit_void_num()`, `lj_ccall_jit_void_flt()`, and
`lj_ccall_jit_flt_num()` for the mixed one-argument shapes,
`lj_ccall_jit_i64_i32_ptr_u64()` for the exact int/pointer/size argument
shape,
`lj_ccall_jit_i64_i32_i64_i32()` for the exact int/signed-offset/int argument
shape,
`lj_ccall_jit_i32_i32_ptr_u32()` for the exact int/pointer/unsigned-int
argument shape,
`lj_ccall_jit_i32_ptr_ulong_i32()` for the exact pointer/unsigned-long/int
shape, or
`lj_ccall_jit_num_fpr()` / `lj_ccall_jit_flt_fpr()` for the FP-only FPR shapes.

This does not enable the old direct `IR_CALLXS` path. Each helper is emitted as
a side-effecting `IRCALL` with an implicit `lua_State *`; the recorder converts
the supported integer/pointer arguments up front, and the helper then calls the
foreign function from C while using the same `CCallNativeState`
save/enter/leave/checkstop protocol as the interpreted `lj_ccall_func()` path.

The scope is deliberately narrow:

- fixed arguments only;
- exactly 0, 1, or 2 Lua arguments except for the audited
  `int32_t(void *, unsigned long, int32_t)` shape;
- exact signed 32-bit integer, unsigned 32-bit integer, or pointer argument
  types in the shared GPR helper matrix;
- void, exact signed 32-bit integer, or pointer return types;
- zero-argument exact signed/unsigned 8-bit or 16-bit integer returns;
- exact signed/unsigned 8-bit or 16-bit integer returns with signed 32-bit,
  unsigned 32-bit, or pointer arguments;
- zero-argument exact unsigned 32-bit integer returns as Lua numbers;
- exact unsigned 32-bit integer returns with signed 32-bit, unsigned 32-bit, or
  pointer arguments;
- exact one-argument `uint32_t(uint32_t)` / `unsigned int(unsigned int)` calls;
- exact `int32_t(void *, unsigned long, int32_t)` calls, with the
  `unsigned long` argument converted before the helper casts to the host ABI
  width;
- exact `int64_t(int32_t, pointer, uint64_t)` calls, with the final argument
  converted before the helper casts to the unsigned 64-bit ABI width;
- exact `int64_t(int32_t, int64_t, int32_t)` calls for seek-shaped APIs, with
  the signed 64-bit offset preserved as an int64 cdata argument;
- exact `int32_t(int32_t, pointer, uint32_t)` calls, with high-bit unsigned
  32-bit count arguments preserved before the helper casts to the exact
  unsigned 32-bit ABI width;
- zero-argument exact signed 64-bit integer returns;
- zero-argument exact unsigned 64-bit integer returns;
- exact signed/unsigned 64-bit integer returns with signed 32-bit, unsigned
  32-bit, or pointer arguments;
- exact one- or two-argument `int64_t`/`uint64_t` calls with per-argument
  signedness preserved;
- exact one-argument `int64_t` or `uint64_t` calls for void, signed 32-bit,
  unsigned 32-bit, narrow integer, and pointer return families;
- exact two-argument `pointer,int64_t` and `pointer,uint64_t` calls for void,
  signed 32-bit, unsigned 32-bit, narrow integer, signed/unsigned 64-bit cdata,
  and pointer return families;
- exact two-argument `int32_t,int64_t` calls for void, signed 32-bit,
  unsigned 32-bit, narrow integer, signed/unsigned 64-bit cdata, and pointer
  return families;
- same-kind exact float/double arguments and exact float/double returns;
- exact one-argument `double(int32_t)`, `double(pointer)`, `double(float)`,
  `int32_t(double)`, `int32_t(float)`, `int32_t(int8_t)`,
  `pointer(double)`, `void(double)`, `void(float)`, and `float(double)` mixed
  calls;
- x64 only;
- callback-blacklisted functions still abort recording;
- all other ordinary FFI calls continue to fall back to the interpreted native
  ccall path.

This gives hot `ffi.C.getpid()`, `ffi.C.abs(i)`, small
`int add(int,int)`-style loops, simple pointer-return/pointer-argument loops,
side-effecting `void f(...)` loops, zero-argument high-bit uint32 result loops,
signed-int/unsigned-int/pointer to high-bit uint32 result loops, high-bit
uint32-argument/result loops, zero-argument narrow integer result loops,
signed-int/unsigned-int/pointer to narrow integer result loops, exact
int8-to-int loops, signed-int/unsigned-int/pointer to int64/uint64 cdata-result
loops, zero-argument signed int64 and unsigned uint64 cdata-result loops, exact
one- and two-argument int64/uint64 mixed-signedness argument/result loops, traced
single-argument int64/uint64 calls returning void, int32, uint32, narrow
integers, or pointers, traced pointer/64-bit span loops for the same return
families, traced int/signed-offset loops for the same return families, traced
`poll(nil, 0, 0)`-style loops,
POSIX `write`-shaped int/pointer/size loops, UCRT `_write`-shaped
int/pointer/unsigned-int loops, `lseek`/`_lseeki64`-shaped int/signed-offset/int
loops, FP-only numeric
call loops, signed-narrow-to-`unsigned long` conversion probes, and
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
