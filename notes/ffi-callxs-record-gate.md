# FFI CALLXS native-state boundary

Broad ordinary FFI C calls keep falling back to the interpreted
`lj_ccall_func()` path, which enters native state around `lj_vm_ffi_call()` and
performs fresh STOPREQ handling. Generic x64 `IR_CALLXS` lowering remains
unsupported until it can preserve ABI results and run the same native
enter/leave protocol. This is documented in `lj_crecord.c`; active tests cover
the fallback behavior instead of enforcing a compile-time implementation
inventory.

The narrow integer/pointer GPR trampoline family in
`lj_ccall_jit_{void,i32,ptr}_gpr()` is a separate helper-call bridge for 0, 1,
or 2 exact signed 32-bit integer and pointer arguments, plus exact
`pointer,int64_t` and `pointer,uint64_t` span-style argument pairs, with
zero-result void, signed 32-bit integer, or pointer returns. It traces through
`IRCALL`, not `IR_CALLXS`, and leaves unmatched shapes interpreted.
The sibling `lj_ccall_jit_i64_gpr()` helper traces exact zero-argument signed
64-bit integer returns, preserving stock boxed int64 cdata results. Signed
64-bit returns with exact signed 32-bit integer or pointer arguments trace
through `lj_ccall_jit_i64_ret_gpr()` and keep the same boxed int64 cdata
result. Exact `int64_t(int64_t)` calls now reuse `lj_ccall_jit_i64_gpr()` with
the exact 64-bit signature and preserve boxed int64 cdata results. Exact
`pointer,int64_t` and `pointer,uint64_t` argument pairs also trace for boxed
signed 64-bit returns. Other signed 64-bit argument combinations remain
interpreted until the recorder can produce the exact ABI value without widening
the semantics.
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
preserve boxed uint64 cdata results. Exact `pointer,int64_t` and
`pointer,uint64_t` argument pairs also trace for boxed unsigned 64-bit returns.
Other unsigned 64-bit argument combinations remain interpreted.
The separate `lj_ccall_jit_{num,flt}_fpr()` helpers trace exact double or float
returns with 0, 1, or 2 same-kind exact FP arguments through the same
native-state bridge.
`lj_ccall_jit_num_gpr()` traces exact double returns with the same one- or
two-argument GPR signature matrix used by the integer and pointer-result helper
families, including high-bit unsigned 32-bit and boxed 64-bit cdata arguments.
It remains a side-effecting native-state `IRCALL` helper rather than direct
`IR_CALLXS` lowering.
`lj_ccall_jit_flt_gpr()` traces exact float returns with the same one- or
two-argument GPR signature matrix and widens the helper result to Lua number
in the recorder.
`lj_ccall_jit_num_i32()`, `lj_ccall_jit_num_ptr()`,
`lj_ccall_jit_num_flt()`, `lj_ccall_jit_i32_num()`,
`lj_ccall_jit_i32_flt()`, `lj_ccall_jit_i32_i8()`, `lj_ccall_jit_ptr_num()`,
`lj_ccall_jit_void_num()`, `lj_ccall_jit_void_flt()`, and
`lj_ccall_jit_flt_num()` trace the first mixed one-argument slice: exact
`double(int32_t)`, `double(pointer)`, `double(float)`, `int32_t(double)`,
`int32_t(float)`, `int32_t(int8_t)`, `pointer(double)`, `void(double)`,
`void(float)`, and `float(double)`, while other mixed shapes remain
interpreted.
`lj_ccall_jit_num_num_i32()` and `lj_ccall_jit_num_i32_num()` trace the first
mixed two-argument double-return slice: exact `double(double, int32_t)` and
`double(int32_t, double)`. These also stay on the side-effecting `IRCALL`
helper path and leave the generic `IR_CALLXS` backend path disabled.
`lj_ccall_jit_num_num_u32()` and `lj_ccall_jit_num_u32_num()` extend that
double-returning mixed slice to exact `double(double, uint32_t)` and
`double(uint32_t, double)` calls, preserving high-bit unsigned arguments
through the recorder's unsigned conversion path.
`lj_ccall_jit_num_flt_i32()` / `lj_ccall_jit_num_i32_flt()` and
`lj_ccall_jit_num_flt_u32()` / `lj_ccall_jit_num_u32_flt()` cover the
cross-precision double-returning 32-bit mixed slice: exact
`double(float, int32_t)`, `double(int32_t, float)`,
`double(float, uint32_t)`, and `double(uint32_t, float)`. These helpers keep
the exact float C argument prototype instead of widening the foreign call.
`lj_ccall_jit_num_num_i64()` / `lj_ccall_jit_num_i64_num()` and
`lj_ccall_jit_num_num_u64()` / `lj_ccall_jit_num_u64_num()` extend the same
mixed double-returning path to exact signed and unsigned 64-bit cdata
arguments, preserving signedness in the selected helper.
`lj_ccall_jit_num_flt_i64()` / `lj_ccall_jit_num_i64_flt()` and
`lj_ccall_jit_num_flt_u64()` / `lj_ccall_jit_num_u64_flt()` extend the
cross-precision double-returning path to exact signed and unsigned 64-bit
cdata arguments, again preserving the exact float C argument prototype.
`lj_ccall_jit_flt_flt_i32()` and `lj_ccall_jit_flt_i32_flt()` extend that
mixed two-argument slice to exact `float(float, int32_t)` and
`float(int32_t, float)` calls, with the recorder widening the float result to
Lua number after the helper call.
`lj_ccall_jit_flt_flt_u32()` and `lj_ccall_jit_flt_u32_flt()` do the same for
exact `float(float, uint32_t)` and `float(uint32_t, float)` calls while
preserving high-bit unsigned arguments.
`lj_ccall_jit_flt_num_i32()` / `lj_ccall_jit_flt_i32_num()` and
`lj_ccall_jit_flt_num_u32()` / `lj_ccall_jit_flt_u32_num()` cover the
cross-precision float-returning 32-bit mixed slice: exact
`float(double, int32_t)`, `float(int32_t, double)`,
`float(double, uint32_t)`, and `float(uint32_t, double)`, with the recorder
widening the helper's float result to Lua number.
`lj_ccall_jit_flt_flt_i64()` / `lj_ccall_jit_flt_i64_flt()` and
`lj_ccall_jit_flt_flt_u64()` / `lj_ccall_jit_flt_u64_flt()` extend exact
float-returning mixed calls to signed and unsigned 64-bit cdata arguments.
`lj_ccall_jit_flt_num_i64()` / `lj_ccall_jit_flt_i64_num()` and
`lj_ccall_jit_flt_num_u64()` / `lj_ccall_jit_flt_u64_num()` extend the
cross-precision float-returning path to exact signed and unsigned 64-bit cdata
arguments, with the recorder widening the helper's float result to Lua number.
`lj_ccall_jit_i32_ptr_ulong_i32()` traces the exact poll-shaped
`int32_t(void *, unsigned long, int32_t)` family and normalizes the
`unsigned long` argument through the regular FFI conversion rules before the
helper casts to the host ABI's actual `unsigned long` width. The focused test
checks both `poll(nil, 0, 0)` and a shared-library signed-narrow-to-unsigned
conversion probe. The int/pointer/size/signed-offset slice traces exact
`int64_t(int32_t, void *, uint64_t, int64_t)` calls for pread/pwrite-style ABI
classes. The pointer/pointer/signed-length family also traces exact
`uint32_t(void *, void *, int32_t)`, `uint64_t(void *, void *, int32_t)`,
`void(void *, void *, int32_t)`, and `void *(void *, void *, int32_t)` calls
through side-effecting native-state helpers. The pointer/pointer/unsigned-count
family also traces exact `int32_t(void *, void *, uint32_t)`,
`uint32_t(void *, void *, uint32_t)`, `int64_t(void *, void *, uint32_t)`,
`uint64_t(void *, void *, uint32_t)`, `void(void *, void *, uint32_t)`, and
`void *(void *, void *, uint32_t)` calls without reusing the uint64-size ABI
helpers. The exact ReadFile/WriteFile-shaped slice traces
`int32_t(void *, void *, uint32_t, void *, void *)` calls. The exact
DeviceIoControl-shaped slice traces
`int32_t(void *, uint32_t, void *, uint32_t, void *, uint32_t, void *, void *)`
calls. The exact
GetOverlappedResult-shaped slice traces
`int32_t(void *, void *, void *, int32_t)` calls. The
GetOverlappedResultEx-shaped slice traces
`int32_t(void *, void *, void *, uint32_t, int32_t)` calls. The
pointer/signed-int/size family traces exact
`int32_t(void *, int32_t, uint64_t)`, `uint32_t(void *, int32_t, uint64_t)`,
`int64_t(void *, int32_t, uint64_t)`, `uint64_t(void *, int32_t, uint64_t)`,
`void(void *, int32_t, uint64_t)`, and `void *(void *, int32_t, uint64_t)`
calls for memset/memchr-style ABI classes. The pointer/signed-int/unsigned-int
family traces exact `int32_t(void *, int32_t, uint32_t)`,
`uint32_t(void *, int32_t, uint32_t)`, `int64_t(void *, int32_t, uint32_t)`,
`uint64_t(void *, int32_t, uint32_t)`, `void(void *, int32_t, uint32_t)`, and
`void *(void *, int32_t, uint32_t)` calls for open/shm_open/sem_init-style ABI
classes. The pointer/size/signed-int family
traces exact `int32_t(void *, uint64_t, int32_t)`,
`uint32_t(void *, uint64_t, int32_t)`, `int64_t(void *, uint64_t, int32_t)`,
`uint64_t(void *, uint64_t, int32_t)`, `void(void *, uint64_t, int32_t)`, and
`void *(void *, uint64_t, int32_t)` calls for mprotect/madvise-style ABI
classes. The exact pointer/size/unsigned-int slice traces
`int32_t(void *, uint64_t, uint32_t)` calls for VirtualFree-style ABI classes.
The pointer/size/pointer family traces exact
`int32_t(void *, uint64_t, void *)`, `uint32_t(void *, uint64_t, void *)`,
`int64_t(void *, uint64_t, void *)`, `uint64_t(void *, uint64_t, void *)`,
`void(void *, uint64_t, void *)`, and `void *(void *, uint64_t, void *)` calls
for mincore-style ABI classes. The exact CreateEventEx/CreateMutexEx-shaped
slice traces `void *(void *, void *, uint32_t, uint32_t)` calls. The exact
pointer/size/unsigned-int/pointer
slice traces `int32_t(void *, uint64_t, uint32_t, void *)` calls for
VirtualProtect-style ABI classes. The exact
CreateIoCompletionPort-shaped slice traces
`void *(void *, void *, uint64_t, uint32_t)` calls. The exact
PostQueuedCompletionStatus-shaped slice traces
`int32_t(void *, uint32_t, uint64_t, void *)` calls. The exact
GetQueuedCompletionStatus-shaped slice traces
`int32_t(void *, void *, void *, void *, uint32_t)` calls. The exact
GetQueuedCompletionStatusEx-shaped slice traces
`int32_t(void *, void *, uint32_t, void *, uint32_t, int32_t)` calls. The exact
pointer/size/unsigned-int/unsigned-int slice traces
`void *(void *, uint64_t, uint32_t, uint32_t)` calls for VirtualAlloc-style ABI
classes. The exact mmap-shaped slice traces
`void *(void *, uint64_t, int32_t, int32_t, int32_t, int64_t)` calls. The exact
mremap-shaped slice traces `void *(void *, uint64_t, uint64_t, int32_t)`
calls. The exact MapViewOfFile-shaped slice traces
`void *(void *, uint32_t, uint32_t, uint32_t, uint64_t)` calls. The exact
MapViewOfFileEx-shaped slice traces
`void *(void *, uint32_t, uint32_t, uint32_t, uint64_t, void *)` calls. The
exact CreateFile-shaped slice traces
`void *(void *, uint32_t, uint32_t, void *, uint32_t, uint32_t, void *)` calls.
The exact CreateFileMapping-shaped slice traces
`void *(void *, void *, uint32_t, uint32_t, uint32_t, void *)` calls.
The exact CreateThread-shaped slice traces
`void *(void *, uint64_t, void *, void *, uint32_t, void *)` calls.
The exact CreateEvent/CreateSemaphore-shaped slice traces
`void *(void *, int32_t, int32_t, void *)` calls.
The exact CreateMutex/CreateWaitableTimer-shaped slice traces
`void *(void *, int32_t, void *)` calls.
The exact OpenEvent/OpenFileMapping-shaped slice traces
`void *(uint32_t, int32_t, void *)` calls.
The exact SetHandleInformation-shaped slice traces
`int32_t(void *, uint32_t, uint32_t)` calls.
The exact WaitForSingleObject-shaped slice traces
`uint32_t(void *, uint32_t)` calls.
The exact WaitForMultipleObjects-shaped slice traces
`uint32_t(uint32_t, void *, int32_t, uint32_t)` calls.
The exact WaitForMultipleObjectsEx-shaped slice traces
`uint32_t(uint32_t, void *, int32_t, uint32_t, int32_t)` calls.
The exact SignalObjectAndWait-shaped slice traces
`uint32_t(void *, void *, uint32_t, int32_t)` calls.
The exact DuplicateHandle-shaped slice traces
`int32_t(void *, void *, void *, void *, uint32_t, int32_t, uint32_t)` calls.
Other multi-argument pointer/size shapes remain interpreted.
The shared GPR helper matrix separately covers exact two-argument
`pointer,int64_t` and `pointer,uint64_t` span shapes; broader pointer/size
families remain interpreted.

Validation:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
