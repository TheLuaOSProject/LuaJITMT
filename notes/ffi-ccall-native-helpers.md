# FFI C-Call Native-State Helpers

Interpreted FFI C calls now factor their native-state bookkeeping through
`CCallNativeState` plus exported `lj_ccall_native_save()`,
`lj_ccall_native_enter()`, `lj_ccall_native_leave()`, and
`lj_ccall_native_checkstop()`.

The ordering intentionally matches the old inline sequence:

- save the surrounding TG/callback state before argument conversion;
- publish `ffi_call_func`, snapshot sticky STOPREQ, and enter native only after
  arguments are ready;
- leave native, blacklist callback-calling functions, and restore the saved TG
  callback state before result conversion, including the callback slot marker
  that is temporarily reused to detect whether the C call reached a callback;
- convert results before checking fresh STOPREQ.

The helpers now live in `lj_ccall.h` so a future x64 `IR_CALLXS` bridge can use
the same callback blacklist, `ffi_call_func`, and STOPREQ-freshness protocol as
the interpreted path instead of duplicating private bookkeeping.

`tests/t-ffi-ccall-native-helpers.c` verifies the exported helper ABI directly:
saved TG callback state is restored after leave, a callback-observed foreign
function is blacklisted without losing the prior callback slot, and a fresh
`LJ_GC2_HS_STOPREQ` published while the TG is native is reported only by the
delayed helper check. It also calls `lj_ccall_jit_num_gpr()` directly across
signed 32-bit, high-bit unsigned 32-bit, and mixed int64/uint32 signatures.

This remains infrastructure for a future direct `IR_CALLXS` bridge. The narrow
`lj_ccall_jit_{void,i32,ptr}_gpr()` trampoline family now traces exact void,
signed 32-bit integer, and pointer-returning calls with 0, 1, or 2
integer/pointer arguments, plus exact `pointer,int64_t` and `pointer,uint64_t`
argument pairs, through `IRCALL` helpers using this same native-state
protocol. `lj_ccall_jit_i64_gpr()` handles exact zero-argument signed 64-bit
integer returns and exact `int64_t(int64_t)` calls, preserving boxed int64 cdata
results.
`lj_ccall_jit_i64_ret_gpr()` extends boxed int64 returns to exact signed 32-bit
integer or pointer arguments, plus exact `pointer,int64_t` and
`pointer,uint64_t` pairs.
`lj_ccall_jit_narrow_0()` handles exact zero-argument signed/unsigned 8-bit and
16-bit integer returns as Lua numbers, after calling through the exact C return
type. `lj_ccall_jit_narrow_gpr()` extends those exact narrow returns to signed
32-bit integer or pointer arguments, plus exact `pointer,int64_t` and
`pointer,uint64_t` pairs.
`lj_ccall_jit_u32_0()` handles exact zero-argument unsigned 32-bit integer
returns as Lua numbers without high-bit truncation. `lj_ccall_jit_u32_gpr()`
extends that high-bit-safe result conversion to unsigned 32-bit returns with
exact signed 32-bit integer or pointer arguments, plus exact
`pointer,int64_t` and `pointer,uint64_t` pairs. `lj_ccall_jit_u32_u32()`
handles exact `uint32_t(uint32_t)` / `unsigned int(unsigned int)` calls with
the same native-state protocol and high-bit-safe Lua number result conversion.
`lj_ccall_jit_u64_0()` handles
exact zero-argument unsigned 64-bit integer returns and preserves boxed uint64
cdata results; `lj_ccall_jit_u64_gpr()` does the same for exact signed 32-bit
integer or pointer arguments, plus exact `pointer,int64_t` and
`pointer,uint64_t` pairs. `lj_ccall_jit_u64_u64()` handles exact
`uint64_t(uint64_t)` calls. The sibling
`lj_ccall_jit_{num,flt}_fpr()` helpers trace exact double or float returns with
0, 1, or 2 same-kind exact FP arguments. `lj_ccall_jit_num_gpr()` traces exact
double returns with the shared one- or two-argument GPR signature matrix,
including high-bit unsigned 32-bit and boxed 64-bit cdata arguments.
`lj_ccall_jit_flt_gpr()` traces exact float returns with the same GPR
signature matrix and widens the float result back to a Lua number in the
recorder.
`lj_ccall_jit_num_i32()`,
`lj_ccall_jit_num_ptr()`, `lj_ccall_jit_num_flt()`,
`lj_ccall_jit_i32_num()`, `lj_ccall_jit_i32_flt()`,
`lj_ccall_jit_i32_i8()`, `lj_ccall_jit_ptr_num()`,
`lj_ccall_jit_void_num()`,
`lj_ccall_jit_void_flt()`, and `lj_ccall_jit_flt_num()` cover the first exact
mixed one-argument calls, including float/double crossings and the first exact
signed narrow argument conversion. `lj_ccall_jit_flt_flt_i32()` and
`lj_ccall_jit_flt_i32_flt()` extend the mixed two-argument slice to exact
float-returning `float(float, int32_t)` and `float(int32_t, float)` calls.
`lj_ccall_jit_num_num_u32()` and `lj_ccall_jit_num_u32_num()` cover exact
double-returning `double(double, uint32_t)` and `double(uint32_t, double)`
calls without truncating high-bit unsigned arguments.
`lj_ccall_jit_num_flt_i32()` / `lj_ccall_jit_num_i32_flt()` and
`lj_ccall_jit_num_flt_u32()` / `lj_ccall_jit_num_u32_flt()` cover exact
cross-precision double-returning `double(float, int32_t)`,
`double(int32_t, float)`, `double(float, uint32_t)`, and
`double(uint32_t, float)` calls without widening the C prototype used for the
foreign call.
`lj_ccall_jit_num_num_i64()` / `lj_ccall_jit_num_i64_num()` and
`lj_ccall_jit_num_num_u64()` / `lj_ccall_jit_num_u64_num()` extend that mixed
double-returning coverage to exact signed and unsigned 64-bit cdata arguments
while preserving per-argument signedness.
`lj_ccall_jit_num_flt_i64()` / `lj_ccall_jit_num_i64_flt()` and
`lj_ccall_jit_num_flt_u64()` / `lj_ccall_jit_num_u64_flt()` extend the
cross-precision double-returning coverage to exact signed and unsigned 64-bit
cdata arguments while preserving the exact float C argument ABI.
`lj_ccall_jit_flt_flt_u32()` and `lj_ccall_jit_flt_u32_flt()` do the same for
exact float-returning `float(float, uint32_t)` and `float(uint32_t, float)`
calls, widening the helper result back to a Lua number in the recorder.
`lj_ccall_jit_flt_num_i32()` / `lj_ccall_jit_flt_i32_num()` and
`lj_ccall_jit_flt_num_u32()` / `lj_ccall_jit_flt_u32_num()` cover exact
cross-precision float-returning `float(double, int32_t)`,
`float(int32_t, double)`, `float(double, uint32_t)`, and
`float(uint32_t, double)` calls, preserving the exact double-argument ABI and
widening the float result back to Lua number in the recorder.
`lj_ccall_jit_flt_flt_i64()` / `lj_ccall_jit_flt_i64_flt()` and
`lj_ccall_jit_flt_flt_u64()` / `lj_ccall_jit_flt_u64_flt()` extend exact
float-returning mixed coverage to signed and unsigned 64-bit cdata arguments.
`lj_ccall_jit_flt_num_i64()` / `lj_ccall_jit_flt_i64_num()` and
`lj_ccall_jit_flt_num_u64()` / `lj_ccall_jit_flt_u64_num()` extend the
cross-precision float-returning coverage to exact signed and unsigned 64-bit
cdata arguments, preserving the exact double-argument ABI and widening the
float result back to Lua number in the recorder.
`lj_ccall_jit_i32_ptr_ulong_i32()` covers the first exact three-argument
pointer/size/int shape, including `ffi.C.poll(nil, 0, 0)`, while preserving the
host ABI's `unsigned long` width at the final C call; the shared-library test
also passes a signed narrow cdata value through the regular unsigned-long
conversion path. The int/pointer/size/signed-offset slice traces exact
`int64_t(int32_t, void *, uint64_t, int64_t)` calls for pread/pwrite-style ABI
classes while preserving the unsigned size and signed offset arguments. The
pointer/pointer/signed-length family also traces exact
`uint32_t(void *, void *, int32_t)`, `uint64_t(void *, void *, int32_t)`,
`void(void *, void *, int32_t)`, and `void *(void *, void *, int32_t)` calls,
preserving high-bit unsigned results, boxed uint64 results, side effects, and
pointer results through the same native-state helper protocol. The
pointer/pointer/unsigned-count family now traces exact
`int32_t(void *, void *, uint32_t)`, `uint32_t(void *, void *, uint32_t)`,
`int64_t(void *, void *, uint32_t)`, `uint64_t(void *, void *, uint32_t)`,
`void(void *, void *, uint32_t)`, and `void *(void *, void *, uint32_t)` calls,
preserving high-bit unsigned count arguments without widening them to a
different ABI shape. The pointer/signed-int/size family traces exact
`int32_t(void *, int32_t, uint64_t)`, `uint32_t(void *, int32_t, uint64_t)`,
`int64_t(void *, int32_t, uint64_t)`, `uint64_t(void *, int32_t, uint64_t)`,
`void(void *, int32_t, uint64_t)`, and `void *(void *, int32_t, uint64_t)`
calls for memset/memchr-style ABI classes while preserving the middle signed
int and final size argument. The pointer/signed-int/unsigned-int family traces
exact `int32_t(void *, int32_t, uint32_t)`,
`uint32_t(void *, int32_t, uint32_t)`, `int64_t(void *, int32_t, uint32_t)`,
`uint64_t(void *, int32_t, uint32_t)`, `void(void *, int32_t, uint32_t)`, and
`void *(void *, int32_t, uint32_t)` calls for open/shm_open/sem_init-style ABI
classes while preserving high-bit unsigned 32-bit mode/value arguments. The
pointer/size/signed-int family traces exact
`int32_t(void *, uint64_t, int32_t)`, `uint32_t(void *, uint64_t, int32_t)`,
`int64_t(void *, uint64_t, int32_t)`, `uint64_t(void *, uint64_t, int32_t)`,
`void(void *, uint64_t, int32_t)`, and `void *(void *, uint64_t, int32_t)`
calls for mprotect/madvise-style ABI classes while preserving the size argument
and trailing signed int. The exact pointer/size/unsigned-int slice traces
`int32_t(void *, uint64_t, uint32_t)` calls for VirtualFree-style ABI classes
while preserving high-bit unsigned flags. The pointer/size/pointer family traces exact
`int32_t(void *, uint64_t, void *)`, `uint32_t(void *, uint64_t, void *)`,
`int64_t(void *, uint64_t, void *)`, `uint64_t(void *, uint64_t, void *)`,
`void(void *, uint64_t, void *)`, and `void *(void *, uint64_t, void *)` calls
for mincore-style pointer/size/output-buffer ABI classes. The exact
pointer/size/unsigned-int/pointer slice traces
`int32_t(void *, uint64_t, uint32_t, void *)` calls for
VirtualProtect-style ABI classes while preserving high-bit unsigned flags and
the output pointer argument. The exact pointer/size/unsigned-int/unsigned-int
slice traces `void *(void *, uint64_t, uint32_t, uint32_t)` calls for
VirtualAlloc-style ABI classes while preserving both high-bit unsigned flag
arguments and pointer results. Exact
two-argument `pointer,int64_t` and `pointer,uint64_t` span-style shapes are
covered by the shared GPR helper matrix, while broader pointer/size families
still fall back. Broad traced ordinary FFI C calls remain interpreted because
x64 `IR_CALLXS` lowering still needs explicit result preservation and carefully
ordered native entry relative to ABI argument setup before direct mcode calls
are safe.
