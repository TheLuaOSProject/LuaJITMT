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
`pointer,uint64_t` span-style calls and exact `int32_t,int64_t` /
`int32_t,uint64_t` calls for those same return families. Exact
`uint32_t,int64_t`, `uint32_t,uint64_t`, `int64_t,int32_t`,
`int64_t,uint32_t`, `uint64_t,int32_t`, `uint64_t,uint32_t`, `int64_t,pointer`,
`uint64_t,pointer`, and all `int64_t`/`uint64_t` two-argument pairs are
covered too. Exact double-returning calls now also accept the same one- or
two-argument GPR signature matrix, and exact float-returning calls accept that
same GPR matrix with the result widened to Lua number after the helper returns.
The FPR subset accepts 0, 1, or 2 same-kind exact float or double
arguments. The first mixed two-argument FP/GPR subset accepts exact
`double(double, int32_t)`, `double(int32_t, double)`,
`double(double, uint32_t)`, `double(uint32_t, double)`,
`double(double, int64_t)`, `double(int64_t, double)`,
`double(double, uint64_t)`, `double(uint64_t, double)`,
`double(float, int32_t)`, `double(int32_t, float)`,
`double(float, uint32_t)`, `double(uint32_t, float)`,
`double(float, int64_t)`, `double(int64_t, float)`,
`double(float, uint64_t)`, `double(uint64_t, float)`,
`float(float, int32_t)`, `float(int32_t, float)`,
`float(float, uint32_t)`, `float(uint32_t, float)`,
`float(double, int32_t)`, `float(int32_t, double)`,
`float(double, uint32_t)`, `float(uint32_t, double)`,
`float(double, int64_t)`, `float(int64_t, double)`,
`float(double, uint64_t)`, `float(uint64_t, double)`,
`float(float, int64_t)`, `float(int64_t, float)`,
`float(float, uint64_t)`, and `float(uint64_t, float)`. The first mixed
one-argument subset accepts exact `double(int32_t)`, `double(pointer)`,
`double(float)`,
`int32_t(double)`, `int32_t(float)`, `int32_t(int8_t)`, `pointer(double)`,
`void(double)`, `void(float)`, and `float(double)`. The first exact
three-argument subset accepts `int32_t(void *, unsigned long, int32_t)` calls,
including the common `poll(nil, 0, 0)` shape, plus the POSIX-shaped
`int64_t(int32_t, pointer, uint64_t)` class used by calls such as
`ssize_t write(int, void *, size_t)`, the POSIX-shaped
`int64_t(int32_t, pointer, uint64_t, int64_t)` class used by calls such as
`ssize_t pread(int, void *, size_t, off_t)`, and the UCRT-shaped
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
integer results, `lj_ccall_jit_num_gpr()`, `lj_ccall_jit_num_i32()`,
`lj_ccall_jit_flt_gpr()`, `lj_ccall_jit_num_num_i32()`,
`lj_ccall_jit_num_i32_num()`, `lj_ccall_jit_num_num_u32()` /
`lj_ccall_jit_num_u32_num()`, `lj_ccall_jit_num_num_i64()` /
`lj_ccall_jit_num_i64_num()` / `lj_ccall_jit_num_num_u64()` /
`lj_ccall_jit_num_u64_num()`, `lj_ccall_jit_num_ptr()`,
`lj_ccall_jit_num_flt()`, `lj_ccall_jit_i32_num()`,
`lj_ccall_jit_i32_flt()`, `lj_ccall_jit_i32_i8()`,
`lj_ccall_jit_ptr_num()`,
`lj_ccall_jit_void_num()`, `lj_ccall_jit_void_flt()`, and
`lj_ccall_jit_flt_num()` for the mixed one- and two-argument shapes,
`lj_ccall_jit_flt_flt_i32()` / `lj_ccall_jit_flt_i32_flt()` for the exact
mixed signed-int float-returning two-argument shapes,
`lj_ccall_jit_flt_flt_u32()` / `lj_ccall_jit_flt_u32_flt()` for the exact
mixed unsigned-int float-returning two-argument shapes,
`lj_ccall_jit_num_flt_i32()` / `lj_ccall_jit_num_i32_flt()` /
`lj_ccall_jit_num_flt_u32()` / `lj_ccall_jit_num_u32_flt()` and
`lj_ccall_jit_flt_num_i32()` / `lj_ccall_jit_flt_i32_num()` /
`lj_ccall_jit_flt_num_u32()` / `lj_ccall_jit_flt_u32_num()` for the exact
cross-precision 32-bit mixed two-argument shapes,
`lj_ccall_jit_num_flt_i64()` / `lj_ccall_jit_num_i64_flt()` /
`lj_ccall_jit_num_flt_u64()` / `lj_ccall_jit_num_u64_flt()` and
`lj_ccall_jit_flt_num_i64()` / `lj_ccall_jit_flt_i64_num()` /
`lj_ccall_jit_flt_num_u64()` / `lj_ccall_jit_flt_u64_num()` for the exact
cross-precision 64-bit mixed two-argument shapes,
`lj_ccall_jit_flt_flt_i64()` / `lj_ccall_jit_flt_i64_flt()` /
`lj_ccall_jit_flt_flt_u64()` / `lj_ccall_jit_flt_u64_flt()` for the exact
mixed 64-bit float-returning two-argument shapes,
`lj_ccall_jit_i64_i32_ptr_u64()` for the exact int/pointer/size argument
shape,
`lj_ccall_jit_i64_i32_ptr_u64_i64()` for the exact
int/pointer/size/signed-offset argument shape,
`lj_ccall_jit_i64_i32_i64_i32()` for the exact int/signed-offset/int argument
shape,
`lj_ccall_jit_i32_i32_ptr_u32()` for the exact int/pointer/unsigned-int
argument shape,
`lj_ccall_jit_u32_u32_ptr_i32_u32()` for the exact unsigned 32-bit-result
unsigned-int/pointer/signed-int/unsigned-int argument shape,
`lj_ccall_jit_u32_u32_ptr_i32_u32_i32()` for the exact unsigned 32-bit-result
unsigned-int/pointer/signed-int/unsigned-int/signed-int argument shape,
`lj_ccall_jit_i32_ptr_i32_u64()` / `lj_ccall_jit_u32_ptr_i32_u64()` /
`lj_ccall_jit_i64_ptr_i32_u64()` / `lj_ccall_jit_u64_ptr_i32_u64()` /
`lj_ccall_jit_void_ptr_i32_u64()` / `lj_ccall_jit_ptr_ptr_i32_u64()` for exact
pointer/signed-int/size argument shapes,
`lj_ccall_jit_i32_ptr_i32_u32()` / `lj_ccall_jit_u32_ptr_i32_u32()` /
`lj_ccall_jit_i64_ptr_i32_u32()` / `lj_ccall_jit_u64_ptr_i32_u32()` /
`lj_ccall_jit_void_ptr_i32_u32()` / `lj_ccall_jit_ptr_ptr_i32_u32()` for exact
pointer/signed-int/unsigned-int argument shapes,
`lj_ccall_jit_i32_ptr_u64_i32()` / `lj_ccall_jit_u32_ptr_u64_i32()` /
`lj_ccall_jit_i64_ptr_u64_i32()` / `lj_ccall_jit_u64_ptr_u64_i32()` /
`lj_ccall_jit_void_ptr_u64_i32()` / `lj_ccall_jit_ptr_ptr_u64_i32()` for exact
pointer/size/signed-int argument shapes,
`lj_ccall_jit_i32_ptr_u64_u32()` for the exact signed-int-result
pointer/size/unsigned-int argument shape,
`lj_ccall_jit_i32_ptr_u64_ptr()` / `lj_ccall_jit_u32_ptr_u64_ptr()` /
`lj_ccall_jit_i64_ptr_u64_ptr()` / `lj_ccall_jit_u64_ptr_u64_ptr()` /
`lj_ccall_jit_void_ptr_u64_ptr()` / `lj_ccall_jit_ptr_ptr_u64_ptr()` for exact
pointer/size/pointer argument shapes,
`lj_ccall_jit_i32_ptr_u64_u32_ptr()` for the exact
pointer/size/unsigned-int/pointer signed-int-result argument shape,
`lj_ccall_jit_i32_ptr_u32_u64_ptr()` for the exact signed-int-result
pointer/unsigned-int/size/pointer argument shape,
`lj_ccall_jit_ptr_ptr_ptr_u64_u32()` for the exact pointer-returning
pointer/pointer/size/unsigned-int argument shape,
`lj_ccall_jit_ptr_ptr_u64_u32_u32()` for the exact pointer-returning
pointer/size/unsigned-int/unsigned-int argument shape,
`lj_ccall_jit_ptr_ptr_u64_i32_i32_i32_i64()` for the exact pointer-returning
pointer/size/int/int/int/signed-offset argument shape,
`lj_ccall_jit_ptr_ptr_u64_u64_i32()` for the exact pointer-returning
pointer/size/size/int argument shape,
`lj_ccall_jit_ptr_ptr_u32_u32_u32_u64()` for the exact pointer-returning
pointer/unsigned-int/unsigned-int/unsigned-int/size argument shape,
`lj_ccall_jit_ptr_ptr_u32_u32_u32_u64_ptr()` for the exact pointer-returning
pointer/unsigned-int/unsigned-int/unsigned-int/size/pointer argument shape,
`lj_ccall_jit_ptr_ptr_u32_u32_ptr_u32_u32_ptr()` for the exact
pointer-returning pointer/unsigned-int/unsigned-int/pointer/unsigned-int/
unsigned-int/pointer argument shape,
`lj_ccall_jit_ptr_ptr_ptr_u32_u32_u32_ptr()` for the exact pointer-returning
pointer/pointer/unsigned-int/unsigned-int/unsigned-int/pointer argument shape,
`lj_ccall_jit_ptr_ptr_u64_ptr_ptr_u32_ptr()` for the exact pointer-returning
pointer/size/pointer/pointer/unsigned-int/pointer argument shape,
`lj_ccall_jit_ptr_ptr_i32_i32_ptr()` for the exact pointer-returning
pointer/signed-int/signed-int/pointer argument shape,
`lj_ccall_jit_ptr_ptr_i32_ptr()` for the exact pointer-returning
pointer/signed-int/pointer argument shape,
`lj_ccall_jit_ptr_u32_i32_ptr()` for the exact pointer-returning
unsigned-int/signed-int/pointer argument shape,
`lj_ccall_jit_i32_ptr_u32_u32()` for the exact signed-int-result
pointer/unsigned-int/unsigned-int argument shape,
`lj_ccall_jit_u32_ptr_u32()` for the exact unsigned 32-bit-result
pointer/unsigned-int argument shape,
`lj_ccall_jit_i32_ptr_ptr_ptr_ptr_u32()` for the exact signed-int-result
pointer/pointer/pointer/pointer/unsigned-int argument shape,
`lj_ccall_jit_i32_ptr_ptr_ptr_ptr_u32_i32_u32()` for the exact signed-int-result
pointer/pointer/pointer/pointer/unsigned-int/signed-int/unsigned-int argument
shape,
`lj_ccall_jit_i32_ptr_ptr_u64()` for the exact pointer/pointer/size argument
shape,
`lj_ccall_jit_i32_ptr_ptr_u32()` / `lj_ccall_jit_u32_ptr_ptr_u32()` for exact
signed/unsigned 32-bit-result pointer/pointer/unsigned-count argument shapes,
`lj_ccall_jit_u32_ptr_ptr_u32_i32()` for the exact unsigned 32-bit-result
pointer/pointer/unsigned-int/signed-int argument shape,
`lj_ccall_jit_i32_ptr_ptr_u32_ptr_ptr()` for the exact signed-int-result
pointer/pointer/unsigned-count/pointer/pointer argument shape,
`lj_ccall_jit_i32_ptr_ptr_u32_ptr_u32_i32()` for the exact signed-int-result
pointer/pointer/unsigned-count/pointer/unsigned-int/signed-int argument shape,
`lj_ccall_jit_i32_ptr_u32_ptr_u32_ptr_u32_ptr_ptr()` for the exact
signed-int-result pointer/unsigned-int/pointer/unsigned-int/pointer/
unsigned-int/pointer/pointer argument shape,
`lj_ccall_jit_i32_ptr_ptr_ptr_i32()` for the exact signed-int-result
pointer/pointer/pointer/signed-int argument shape,
`lj_ccall_jit_i32_ptr_ptr_ptr_u32_i32()` for the exact signed-int-result
pointer/pointer/pointer/unsigned-int/signed-int argument shape,
`lj_ccall_jit_u32_ptr_ptr_i32()` for the exact unsigned 32-bit-result
pointer/pointer/signed-length argument shape,
`lj_ccall_jit_i64_ptr_ptr_u32()` / `lj_ccall_jit_u64_ptr_ptr_u32()` for exact
signed/unsigned 64-bit-result pointer/pointer/unsigned-count argument shapes,
`lj_ccall_jit_i64_ptr_ptr_u64()` / `lj_ccall_jit_u64_ptr_ptr_u64()` for exact
signed/unsigned 64-bit-result pointer/pointer/size argument shapes,
`lj_ccall_jit_u64_ptr_ptr_i32()` for the exact unsigned 64-bit-result
pointer/pointer/signed-length argument shape,
`lj_ccall_jit_void_ptr_ptr_i32()` / `lj_ccall_jit_void_ptr_ptr_u32()` /
`lj_ccall_jit_void_ptr_ptr_u64()` for exact void-returning pointer/pointer
signed-length, unsigned-count, and size argument shapes,
`lj_ccall_jit_ptr_ptr_ptr_i32()` / `lj_ccall_jit_ptr_ptr_ptr_u32()` /
`lj_ccall_jit_ptr_ptr_ptr_u64()` for exact pointer-returning pointer/pointer
signed-length, unsigned-count, and size argument shapes,
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
- exactly 0, 1, or 2 Lua arguments except for audited three- and four-argument shapes
  listed below;
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
- exact `int64_t(int32_t, pointer, uint64_t, int64_t)` calls for
  pread/pwrite-shaped APIs, preserving the size and signed offset arguments;
- exact `int64_t(int32_t, int64_t, int32_t)` calls for seek-shaped APIs, with
  the signed 64-bit offset preserved as an int64 cdata argument;
- exact `int32_t(int32_t, pointer, uint32_t)` calls, with high-bit unsigned
  32-bit count arguments preserved before the helper casts to the exact
  unsigned 32-bit ABI width;
- exact `int32_t(pointer, int32_t, uint64_t)` and
  `uint32_t(pointer, int32_t, uint64_t)` calls, with the signed int argument
  and final size argument preserved for memset/memchr-style ABI classes;
- exact signed/unsigned 64-bit cdata-result `int64_t(pointer, int32_t,
  uint64_t)` and `uint64_t(pointer, int32_t, uint64_t)` calls, with the signed
  int argument and final size argument preserved;
- exact `void(pointer, int32_t, uint64_t)` and
  `pointer(pointer, int32_t, uint64_t)` calls, preserving side effects and
  pointer results for pointer/int/size ABI classes;
- exact `int32_t(pointer, int32_t, uint32_t)` and
  `uint32_t(pointer, int32_t, uint32_t)` calls, preserving high-bit unsigned
  mode/value arguments for open/shm_open/sem_init-style ABI classes;
- exact signed/unsigned 64-bit cdata-result `int64_t(pointer, int32_t,
  uint32_t)` and `uint64_t(pointer, int32_t, uint32_t)` calls, preserving
  boxed results and high-bit unsigned 32-bit arguments;
- exact `void(pointer, int32_t, uint32_t)` and
  `pointer(pointer, int32_t, uint32_t)` calls, preserving side effects and
  pointer results for pointer/int/unsigned-int ABI classes;
- exact `int32_t(pointer, uint64_t, int32_t)` and
  `uint32_t(pointer, uint64_t, int32_t)` calls, preserving the size argument and
  trailing signed int for mprotect/madvise-style ABI classes;
- exact signed/unsigned 64-bit cdata-result `int64_t(pointer, uint64_t,
  int32_t)` and `uint64_t(pointer, uint64_t, int32_t)` calls, preserving boxed
  results for pointer/size/int ABI classes;
- exact `void(pointer, uint64_t, int32_t)` and
  `pointer(pointer, uint64_t, int32_t)` calls, preserving side effects and
  pointer results for pointer/size/int ABI classes;
- exact `int32_t(pointer, uint64_t, uint32_t)` calls, preserving the size
  argument and high-bit unsigned flags for VirtualFree-style
  pointer/size/flags ABI classes;
- exact `int32_t(pointer, uint64_t, pointer)` and
  `uint32_t(pointer, uint64_t, pointer)` calls, preserving the middle size
  argument for mincore-style pointer/size/output-buffer ABI classes;
- exact signed/unsigned 64-bit cdata-result `int64_t(pointer, uint64_t,
  pointer)` and `uint64_t(pointer, uint64_t, pointer)` calls, preserving boxed
  results for pointer/size/pointer ABI classes;
- exact `void(pointer, uint64_t, pointer)` and
  `pointer(pointer, uint64_t, pointer)` calls, preserving side effects and
  pointer results for pointer/size/pointer ABI classes;
- exact `int32_t(pointer, uint64_t, uint32_t, pointer)` calls, preserving the
  size argument and high-bit unsigned flags for VirtualProtect-style
  pointer/size/flags/output-pointer ABI classes;
- exact `pointer(pointer, pointer, uint64_t, uint32_t)` calls, preserving file
  handle, existing completion port, completion key, and thread count for
  CreateIoCompletionPort-style ABI classes;
- exact `int32_t(pointer, uint32_t, uint64_t, pointer)` calls, preserving
  completion port, transferred byte count, completion key, and overlapped
  pointer for PostQueuedCompletionStatus-style ABI classes;
- exact `int32_t(pointer, pointer, pointer, pointer, uint32_t)` calls,
  preserving completion port, byte-count output pointer, completion-key output
  pointer, overlapped output pointer, and timeout for
  GetQueuedCompletionStatus-style ABI classes;
- exact `int32_t(pointer, pointer, uint32_t, pointer, uint32_t, int32_t)`
  calls, preserving completion port, overlapped-entry buffer, entry count,
  removed-count output pointer, timeout, and alertable flag for
  GetQueuedCompletionStatusEx-style ABI classes;
- exact `pointer(pointer, uint64_t, uint32_t, uint32_t)` calls, preserving the
  size argument and high-bit unsigned flag arguments for VirtualAlloc-style
  pointer/size/allocation-flags/protection-flags ABI classes;
- exact `pointer(pointer, uint64_t, int32_t, int32_t, int32_t, int64_t)` calls,
  preserving the size, protection flags, mapping flags, descriptor, and signed
  offset for mmap-style ABI classes;
- exact `pointer(pointer, uint64_t, uint64_t, int32_t)` calls, preserving old
  size, new size, and signed flags for mremap-style ABI classes;
- exact `pointer(pointer, uint32_t, uint32_t, uint32_t, uint64_t)` calls,
  preserving access flags, split 32-bit file offset, and byte count for
  MapViewOfFile-style ABI classes;
- exact `pointer(pointer, uint32_t, uint32_t, uint32_t, uint64_t, pointer)`
  calls, preserving access flags, split 32-bit file offset, byte count, and
  desired base pointer for MapViewOfFileEx-style ABI classes;
- exact `pointer(pointer, uint32_t, uint32_t, pointer, uint32_t, uint32_t,
  pointer)` calls, preserving desired access, share mode, security attributes,
  creation disposition, flags, and template handle for CreateFile-style ABI
  classes;
- exact `pointer(pointer, pointer, uint32_t, uint32_t, uint32_t, pointer)`
  calls, preserving handle, security attributes, protection flags, maximum size
  high/low halves, and name for CreateFileMapping-style ABI classes;
- exact `pointer(pointer, uint64_t, pointer, pointer, uint32_t, pointer)`
  calls, preserving security attributes, stack size, start address, parameter,
  creation flags, and thread-id pointer for CreateThread-style ABI classes;
- exact `pointer(pointer, int32_t, int32_t, pointer)` calls, preserving
  security attributes, boolean/count fields, and name pointer for
  CreateEvent/CreateSemaphore-style ABI classes;
- exact `pointer(pointer, int32_t, pointer)` calls, preserving security
  attributes, inherit/manual-reset-style flags, and name pointer for
  CreateMutex/CreateWaitableTimer-style ABI classes;
- exact `pointer(uint32_t, int32_t, pointer)` calls, preserving desired access,
  inherit flag, and name pointer for OpenEvent/OpenMutex/OpenSemaphore and
  OpenFileMapping-style ABI classes;
- exact `int32_t(pointer, uint32_t, uint32_t)` calls, preserving handle, mask,
  and flags for SetHandleInformation-style ABI classes;
- exact `uint32_t(pointer, uint32_t)` calls, preserving handle and timeout for
  WaitForSingleObject-style ABI classes;
- exact `uint32_t(uint32_t, pointer, int32_t, uint32_t)` calls, preserving
  handle count, handle-array pointer, wait-all flag, timeout, and high-bit
  results for WaitForMultipleObjects-style ABI classes;
- exact `uint32_t(uint32_t, pointer, int32_t, uint32_t, int32_t)` calls,
  preserving handle count, handle-array pointer, wait-all flag, timeout,
  alertable flag, and high-bit results for WaitForMultipleObjectsEx-style ABI
  classes;
- exact `uint32_t(pointer, pointer, uint32_t, int32_t)` calls, preserving
  signal handle, wait handle, timeout, alertable flag, and high-bit results
  for SignalObjectAndWait-style ABI classes;
- exact `int32_t(pointer, pointer, pointer, pointer, uint32_t, int32_t,
  uint32_t)` calls, preserving source/target process and handle pointers,
  desired access, inherit flag, and options for DuplicateHandle-style ABI
  classes;
- exact `int32_t(pointer, pointer, uint64_t)` calls, with the final size
  argument preserved before the helper casts to the exact unsigned 64-bit ABI
  width;
- exact `int32_t(pointer, pointer, uint32_t)` and
  `uint32_t(pointer, pointer, uint32_t)` calls, with high-bit unsigned count
  arguments preserved before the helper casts to the exact unsigned 32-bit ABI
  width;
- exact `int32_t(pointer, pointer, uint32_t, pointer, pointer)` calls,
  preserving high-bit unsigned byte counts and both output/overlapped pointer
  arguments for ReadFile/WriteFile-style ABI classes;
- exact `int32_t(pointer, uint32_t, pointer, uint32_t, pointer, uint32_t,
  pointer, pointer)` calls, preserving all three DWORD arguments and all
  pointer buffers for DeviceIoControl-style ABI classes;
- exact `int32_t(pointer, pointer, pointer, int32_t)` calls, preserving the
  overlapped pointer, output pointer, and signed wait flag for
  GetOverlappedResult-style ABI classes;
- exact `int32_t(pointer, pointer, pointer, uint32_t, int32_t)` calls,
  preserving the overlapped pointer, output pointer, timeout, and alertable
  flag for GetOverlappedResultEx-style ABI classes;
- exact signed/unsigned 64-bit cdata-result `int64_t(pointer, pointer,
  uint64_t)` and `uint64_t(pointer, pointer, uint64_t)` calls, with the final
  size argument preserved before the helper casts to the exact unsigned 64-bit
  ABI width;
- exact signed/unsigned 64-bit cdata-result `int64_t(pointer, pointer,
  uint32_t)` and `uint64_t(pointer, pointer, uint32_t)` calls, with high-bit
  unsigned count arguments preserved before the helper casts to the exact
  unsigned 32-bit ABI width;
- exact `void(pointer, pointer, uint64_t)` calls, with the final size argument
  preserved before the helper casts to the exact unsigned 64-bit ABI width;
- exact `pointer(pointer, pointer, uint64_t)` calls, with the final size
  argument preserved before the helper casts to the exact unsigned 64-bit ABI
  width;
- exact `void(pointer, pointer, uint32_t)` and
  `pointer(pointer, pointer, uint32_t)` calls, with high-bit unsigned count
  arguments preserved;
- exact `uint32_t(pointer, pointer, int32_t)`,
  `uint64_t(pointer, pointer, int32_t)`, `void(pointer, pointer, int32_t)`,
  and `pointer(pointer, pointer, int32_t)` calls, with the signed 32-bit length
  preserved;
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
- exact two-argument `int32_t,uint64_t` calls for the same return families;
- exact two-argument `uint32_t,int64_t` and `uint32_t,uint64_t` calls for the
  same return families;
- exact two-argument `int64_t,int32_t` and `int64_t,uint32_t` calls for the
  same return families;
- exact two-argument `uint64_t,int32_t` and `uint64_t,uint32_t` calls for the
  same return families;
- exact two-argument `int64_t,pointer` and `uint64_t,pointer` calls for the
  same return families;
- exact two-argument `int64_t`/`uint64_t` all-64 pairs for void, signed
  32-bit, unsigned 32-bit, narrow integer, and pointer return families;
- exact double-returning calls with the one- or two-argument GPR signature
  matrix;
- exact float-returning calls with the one- or two-argument GPR signature
  matrix, widened to Lua numbers after the helper call;
- same-kind exact float/double arguments and exact float/double returns;
- exact two-argument `double(double, int32_t)` and
  `double(int32_t, double)` mixed calls;
- exact two-argument `double(double, uint32_t)` and
  `double(uint32_t, double)` mixed calls, preserving high-bit unsigned
  arguments;
- exact two-argument `double(float, int32_t)`, `double(int32_t, float)`,
  `double(float, uint32_t)`, and `double(uint32_t, float)` cross-precision
  mixed calls, preserving high-bit unsigned arguments;
- exact two-argument `double(double, int64_t)`, `double(int64_t, double)`,
  `double(double, uint64_t)`, and `double(uint64_t, double)` mixed calls,
  preserving boxed 64-bit cdata argument signedness;
- exact two-argument `double(float, int64_t)`, `double(int64_t, float)`,
  `double(float, uint64_t)`, and `double(uint64_t, float)` cross-precision
  mixed calls, preserving boxed 64-bit cdata argument signedness;
- exact two-argument `float(float, int32_t)` and `float(int32_t, float)` mixed
  calls, widened to Lua numbers after the helper call;
- exact two-argument `float(float, uint32_t)` and `float(uint32_t, float)`
  mixed calls, preserving high-bit unsigned arguments and widening the result
  to Lua number after the helper call;
- exact two-argument `float(double, int32_t)`, `float(int32_t, double)`,
  `float(double, uint32_t)`, and `float(uint32_t, double)` cross-precision
  mixed calls, preserving high-bit unsigned arguments and widening the result
  to Lua number after the helper call;
- exact two-argument `float(float, int64_t)`, `float(int64_t, float)`,
  `float(float, uint64_t)`, and `float(uint64_t, float)` mixed calls,
  preserving boxed 64-bit cdata argument signedness and widening the result to
  Lua number after the helper call;
- exact two-argument `float(double, int64_t)`, `float(int64_t, double)`,
  `float(double, uint64_t)`, and `float(uint64_t, double)` cross-precision
  mixed calls, preserving boxed 64-bit cdata argument signedness and widening
  the result to Lua number after the helper call;
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
families, traced int/signed-offset and int/unsigned-size loops for the same
return families, traced unsigned-count pointer/pointer loops,
traced pointer/int/size, pointer/int/unsigned-int, pointer/size/int,
pointer/size/flags, and pointer/size/pointer loops,
traced pointer/size/flags/output-pointer loops,
traced CreateIoCompletionPort-shaped pointer/pointer/uint64/uint32
pointer-returning loops,
traced PostQueuedCompletionStatus-shaped pointer/uint32/uint64/pointer
signed-int-result loops,
traced GetQueuedCompletionStatus-shaped pointer/pointer/pointer/pointer/uint32
signed-int-result loops,
traced GetQueuedCompletionStatusEx-shaped pointer/pointer/uint32/pointer/
uint32/int32 signed-int-result loops,
traced pointer/size/allocation-flags/protection-flags pointer-returning loops,
traced mmap-shaped pointer/size/int/int/int/signed-offset pointer-returning
loops,
traced mremap-shaped pointer/size/size/int pointer-returning loops,
traced MapViewOfFile-shaped pointer/uint32/uint32/uint32/size pointer-returning
loops,
traced MapViewOfFileEx-shaped pointer/uint32/uint32/uint32/size/pointer
pointer-returning loops,
traced CreateFile-shaped pointer/uint32/uint32/pointer/uint32/uint32/pointer
pointer-returning loops,
traced CreateFileMapping-shaped pointer/pointer/uint32/uint32/uint32/pointer
pointer-returning loops,
traced CreateThread-shaped pointer/uint64/ptr/ptr/uint32/ptr
pointer-returning loops,
traced CreateEvent/CreateSemaphore-shaped pointer/int32/int32/pointer
pointer-returning loops,
traced CreateMutex/CreateWaitableTimer-shaped pointer/int32/pointer
pointer-returning loops,
traced OpenEvent/OpenFileMapping-shaped uint32/int32/pointer pointer-returning
loops,
traced SetHandleInformation-shaped pointer/uint32/uint32 signed-int-result
loops,
traced WaitForSingleObject-shaped pointer/uint32 unsigned-result loops,
traced WaitForMultipleObjects-shaped uint32/pointer/int32/uint32
unsigned-result loops,
traced WaitForMultipleObjectsEx-shaped uint32/pointer/int32/uint32/int32
unsigned-result loops,
traced SignalObjectAndWait-shaped pointer/pointer/uint32/int32
unsigned-result loops,
traced DuplicateHandle-shaped pointer/pointer/pointer/pointer/uint32/int32/
uint32 signed-int-result loops,
traced ReadFile/WriteFile-shaped pointer/pointer/uint32/pointer/pointer
signed-int-result loops,
traced DeviceIoControl-shaped pointer/uint32/pointer/uint32/pointer/uint32/
pointer/pointer signed-int-result loops,
traced GetOverlappedResult-shaped pointer/pointer/pointer/int32
signed-int-result loops,
traced GetOverlappedResultEx-shaped pointer/pointer/pointer/uint32/int32
signed-int-result loops,
traced `poll(nil, 0, 0)`-style loops,
POSIX `write`-shaped int/pointer/size loops, POSIX `pread`/`pwrite`-shaped
int/pointer/size/signed-offset loops, UCRT `_write`-shaped
int/pointer/unsigned-int loops, `lseek`/`_lseeki64`-shaped int/signed-offset/int
loops, signed/unsigned 64-bit-result pointer/pointer/size loops,
void-returning pointer/pointer/size loops, pointer-returning pointer/pointer
unsigned-count loops, double- and float-returning
GPR-matrix loops, FP-only numeric call loops,
signed-narrow-to-`unsigned long` conversion probes, and
mixed float/double one-argument calls, plus exact double/int, double/uint,
double/64-bit, float/int, float/uint, float/64-bit, and cross-precision
float/double with 32-/64-bit int/uint two-argument calls, a
traced, nonblocking native-state path, without risking the direct backend
`IR_CALLXS` register/result ordering. The full direct bridge still needs x64
lowering that brackets the foreign ABI call without clobbering argument or
result registers.

`tests/t-ffi-ccall-stopreq.c` also heats the shared `sleep_i32` trampoline until
a trace exists, starts the STOPREQ publisher only after that warmup, and catches
the shutdown error from a traced native sleep through `pcall()`. This pins the
`CCI_T`/snapshot side of the helper-call bridge.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_ccall_native`
- `LUA=$PWD/src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet lib/ffi`
