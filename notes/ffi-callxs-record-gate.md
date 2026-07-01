# FFI CALLXS record gate

`m7_ffi_ccall_native` now attempts an opt-in build with
`XCFLAGS=-DLJ_FFI_RECORD_CALLS=1` and requires the compile-time
`IR_CALLXS native-state protocol` error.

This pins the direct-`IR_CALLXS` safety boundary: broad ordinary FFI C calls
must keep falling back to the interpreted `lj_ccall_func()` path, which enters
native state around `lj_vm_ffi_call()` and performs fresh STOPREQ handling,
until x64 `IR_CALLXS` lowering can preserve ABI results and run the same native
enter/leave protocol.

The narrow integer/pointer GPR trampoline family in
`lj_ccall_jit_{void,i32,ptr}_gpr()` is a separate helper-call bridge for 0, 1,
or 2 exact signed 32-bit integer and pointer arguments, with zero-result void,
signed 32-bit integer, or pointer returns. It traces through `IRCALL`, not
`IR_CALLXS`, and keeps the compile-time `LJ_FFI_RECORD_CALLS` gate intact.
The sibling `lj_ccall_jit_i64_gpr()` helper traces exact zero-argument signed
64-bit integer returns, preserving stock boxed int64 cdata results. Signed
64-bit returns with exact signed 32-bit integer or pointer arguments trace
through `lj_ccall_jit_i64_ret_gpr()` and keep the same boxed int64 cdata
result. Exact `int64_t(int64_t)` calls now reuse `lj_ccall_jit_i64_gpr()` with
the exact 64-bit signature and preserve boxed int64 cdata results. Other signed
64-bit argument combinations remain interpreted until the recorder can produce
the exact ABI value without widening the semantics.
`lj_ccall_jit_narrow_0()` traces exact zero-argument signed/unsigned 8-bit and
16-bit integer returns as Lua numbers, after calling the exact C return type.
`lj_ccall_jit_narrow_gpr()` extends those exact narrow returns to signed
32-bit integer or pointer arguments. Narrow integer argument conversion remains
interpreted except for the exact `int32_t(int8_t)` slice traced through
`lj_ccall_jit_i32_i8()`.
`lj_ccall_jit_u32_0()` traces exact zero-argument unsigned 32-bit returns as
Lua numbers, preserving high-bit values without signed truncation. Unsigned
integer returns with exact signed 32-bit integer or pointer arguments trace
through `lj_ccall_jit_u32_gpr()` and use the same high-bit-safe Lua number
result conversion. `lj_ccall_jit_u32_u32()` traces the exact
`uint32_t(uint32_t)`/`unsigned int(unsigned int)` shape, including high-bit
argument values. Broader unsigned integer argument combinations remain
interpreted.
`lj_ccall_jit_u64_0()` traces exact zero-argument unsigned 64-bit returns,
preserving stock boxed uint64 cdata results. Unsigned 64-bit returns with exact
signed 32-bit integer or pointer arguments trace through `lj_ccall_jit_u64_gpr()`.
Exact `uint64_t(uint64_t)` calls trace through `lj_ccall_jit_u64_u64()` and
preserve boxed uint64 cdata results. Other unsigned 64-bit argument combinations
remain interpreted.
The separate `lj_ccall_jit_{num,flt}_fpr()` helpers trace exact double or float
returns with 0, 1, or 2 same-kind exact FP arguments through the same
native-state bridge.
`lj_ccall_jit_num_i32()`, `lj_ccall_jit_num_ptr()`,
`lj_ccall_jit_num_flt()`, `lj_ccall_jit_i32_num()`,
`lj_ccall_jit_i32_flt()`, `lj_ccall_jit_i32_i8()`, `lj_ccall_jit_ptr_num()`,
`lj_ccall_jit_void_num()`, `lj_ccall_jit_void_flt()`, and
`lj_ccall_jit_flt_num()` trace the first mixed one-argument slice: exact
`double(int32_t)`, `double(pointer)`, `double(float)`, `int32_t(double)`,
`int32_t(float)`, `int32_t(int8_t)`, `pointer(double)`, `void(double)`,
`void(float)`, and `float(double)`, while other mixed shapes remain
interpreted.

Validation:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
