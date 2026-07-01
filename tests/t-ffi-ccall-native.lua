local ffi = require"ffi"
local th = require"threading"
local trace_count = require"jit_harness".trace_count

ffi.cdef[[
int abs(int);
int getpid(void);
int poll(void *fds, unsigned long nfds, int timeout);
int lj_m7_ccall_jit_sleep_i32(int);
int lj_m7_ccall_jit_add2_i32(int, int);
int64_t lj_m7_ccall_jit_i64_0(void);
int64_t lj_m7_ccall_jit_i64_i32(int32_t);
int64_t lj_m7_ccall_jit_i64_ptr(int *);
int64_t lj_m7_ccall_jit_i64_i32_ptr(int32_t, int *);
int64_t lj_m7_ccall_jit_i64_i64(int64_t);
int64_t lj_m7_ccall_jit_i64_i64_i64(int64_t, int64_t);
int64_t lj_m7_ccall_jit_i64_u64(uint64_t);
int64_t lj_m7_ccall_jit_i64_i64_u64(int64_t, uint64_t);
int64_t lj_m7_ccall_jit_i64_u64_i64(uint64_t, int64_t);
int64_t lj_m7_ccall_jit_i64_i32_ptr_u64(int32_t, int *, uint64_t);
int64_t lj_m7_ccall_jit_i64_i32_i64_i32(int32_t, int64_t, int32_t);
int32_t lj_m7_ccall_jit_i32_i32_ptr_u32(int32_t, int *, uint32_t);
uint32_t lj_m7_ccall_jit_u32_i32_ptr_u32(int32_t, int *, uint32_t);
int32_t lj_m7_ccall_jit_i32_ptr_ptr_u64(int *, int *, uint64_t);
uint32_t lj_m7_ccall_jit_u32_ptr_ptr_u64(int *, int *, uint64_t);
int32_t lj_m7_ccall_jit_i32_ptr_ptr_i32(int *, int *, int32_t);
int64_t lj_m7_ccall_jit_i64_ptr_ptr_u64(int *, int *, uint64_t);
uint64_t lj_m7_ccall_jit_u64_ptr_ptr_u64(int *, int *, uint64_t);
int *lj_m7_ccall_jit_ptr_ptr_ptr_u64(int *, int *, uint64_t);
void lj_m7_ccall_jit_void_ptr_ptr_u64(int *, int *, uint64_t);
int8_t lj_m7_ccall_jit_i8_0(void);
int8_t lj_m7_ccall_jit_i8_i32(int32_t);
uint8_t lj_m7_ccall_jit_u8_0(void);
uint8_t lj_m7_ccall_jit_u8_ptr(int *);
int16_t lj_m7_ccall_jit_i16_0(void);
int16_t lj_m7_ccall_jit_i16_i32_ptr(int32_t, int *);
uint16_t lj_m7_ccall_jit_u16_0(void);
uint16_t lj_m7_ccall_jit_u16_i32(int32_t);
int lj_m7_ccall_jit_i8_arg_i32(int8_t);
int32_t lj_m7_ccall_jit_i32_u32(uint32_t);
int32_t lj_m7_ccall_jit_i32_u32_ptr(uint32_t, int *);
int32_t lj_m7_ccall_jit_i32_i64arg(int64_t);
int32_t lj_m7_ccall_jit_i32_u64arg(uint64_t);
uint8_t lj_m7_ccall_jit_u8_u32(uint32_t);
uint8_t lj_m7_ccall_jit_u8_i64arg(int64_t);
uint8_t lj_m7_ccall_jit_u8_ptr_u64(int *, uint64_t);
uint8_t lj_m7_ccall_jit_u8_ptr_i64(int *, int64_t);
uint8_t lj_m7_ccall_jit_u8_i32_i64(int32_t, int64_t);
uint8_t lj_m7_ccall_jit_u8_i32_u64(int32_t, uint64_t);
uint8_t lj_m7_ccall_jit_u8_u32_i64(uint32_t, int64_t);
uint8_t lj_m7_ccall_jit_u8_u32_u64(uint32_t, uint64_t);
void lj_m7_ccall_jit_void_u32(uint32_t);
void lj_m7_ccall_jit_void_i64arg(int64_t);
void lj_m7_ccall_jit_void_u64arg(uint64_t);
void lj_m7_ccall_jit_void_ptr_u64(int *, uint64_t);
void lj_m7_ccall_jit_void_ptr_i64(int *, int64_t);
void lj_m7_ccall_jit_void_i32_i64(int32_t, int64_t);
void lj_m7_ccall_jit_void_i32_u64(int32_t, uint64_t);
void lj_m7_ccall_jit_void_u32_i64(uint32_t, int64_t);
void lj_m7_ccall_jit_void_u32_u64(uint32_t, uint64_t);
uint64_t lj_m7_ccall_jit_u64_u32arg(uint32_t);
uint32_t lj_m7_ccall_jit_u32_u64arg(uint64_t);
int32_t lj_m7_ccall_jit_i32_ptr_u64(int *, uint64_t);
int32_t lj_m7_ccall_jit_i32_ptr_i64(int *, int64_t);
int32_t lj_m7_ccall_jit_i32_i32_i64(int32_t, int64_t);
int32_t lj_m7_ccall_jit_i32_i32_u64(int32_t, uint64_t);
int32_t lj_m7_ccall_jit_i32_u32_i64(uint32_t, int64_t);
int32_t lj_m7_ccall_jit_i32_u32_u64(uint32_t, uint64_t);
uint32_t lj_m7_ccall_jit_u32_ptr_u64(int *, uint64_t);
uint32_t lj_m7_ccall_jit_u32_ptr_i64(int *, int64_t);
uint32_t lj_m7_ccall_jit_u32_i32_i64(int32_t, int64_t);
uint32_t lj_m7_ccall_jit_u32_i32_u64(int32_t, uint64_t);
uint32_t lj_m7_ccall_jit_u32_u32_i64(uint32_t, int64_t);
uint32_t lj_m7_ccall_jit_u32_u32_u64(uint32_t, uint64_t);
int *lj_m7_ccall_jit_ptr_u32(uint32_t);
int *lj_m7_ccall_jit_ptr_u64arg(uint64_t);
int *lj_m7_ccall_jit_ptr_ptr_u64(int *, uint64_t);
int *lj_m7_ccall_jit_ptr_ptr_i64(int *, int64_t);
int *lj_m7_ccall_jit_ptr_i32_i64(int32_t, int64_t);
int *lj_m7_ccall_jit_ptr_i32_u64(int32_t, uint64_t);
int *lj_m7_ccall_jit_ptr_u32_i64(uint32_t, int64_t);
int *lj_m7_ccall_jit_ptr_u32_u64(uint32_t, uint64_t);
double lj_m7_ccall_jit_num0(void);
double lj_m7_ccall_jit_num_i32(int32_t);
double lj_m7_ccall_jit_num_num_i32(double, int32_t);
double lj_m7_ccall_jit_num_ptr(int *);
double lj_m7_ccall_jit_num_flt(float);
double lj_m7_ccall_jit_num1(double);
double lj_m7_ccall_jit_num2(double, double);
float lj_m7_ccall_jit_flt0(void);
float lj_m7_ccall_jit_flt1(float);
float lj_m7_ccall_jit_flt2(float, float);
int lj_m7_ccall_jit_void_count_i32(void);
int32_t lj_m7_ccall_jit_i32_num(double);
int32_t lj_m7_ccall_jit_i32_flt(float);
void lj_m7_ccall_jit_void0(void);
void lj_m7_ccall_jit_void_num(double);
void lj_m7_ccall_jit_void_flt(float);
float lj_m7_ccall_jit_flt_num(double);
void lj_m7_ccall_jit_store_i32(int *, int);
unsigned int lj_m7_ccall_jit_u32(unsigned int);
uint32_t lj_m7_ccall_jit_u32_i32(int32_t);
uint32_t lj_m7_ccall_jit_u32_ptr(int *);
uint32_t lj_m7_ccall_jit_u32_i32_ptr(int32_t, int *);
uint32_t lj_m7_ccall_jit_u32_0(void);
uint64_t lj_m7_ccall_jit_u64(uint64_t);
uint64_t lj_m7_ccall_jit_u64_u64_u64(uint64_t, uint64_t);
uint64_t lj_m7_ccall_jit_u64_i64(int64_t);
uint64_t lj_m7_ccall_jit_u64_i64_u64(int64_t, uint64_t);
uint64_t lj_m7_ccall_jit_u64_u64_i64(uint64_t, int64_t);
uint64_t lj_m7_ccall_jit_u64_i32(int32_t);
uint64_t lj_m7_ccall_jit_u64_ptr(int *);
uint64_t lj_m7_ccall_jit_u64_i32_ptr(int32_t, int *);
int64_t lj_m7_ccall_jit_i64_ptr_u64(int *, uint64_t);
uint64_t lj_m7_ccall_jit_u64_ptr_u64(int *, uint64_t);
int64_t lj_m7_ccall_jit_i64_ptr_i64(int *, int64_t);
uint64_t lj_m7_ccall_jit_u64_ptr_i64(int *, int64_t);
int64_t lj_m7_ccall_jit_i64_i32_i64(int32_t, int64_t);
uint64_t lj_m7_ccall_jit_u64_i32_i64(int32_t, int64_t);
int64_t lj_m7_ccall_jit_i64_i32_u64(int32_t, uint64_t);
uint64_t lj_m7_ccall_jit_u64_i32_u64(int32_t, uint64_t);
int64_t lj_m7_ccall_jit_i64_u32_i64(uint32_t, int64_t);
int64_t lj_m7_ccall_jit_i64_u32_u64(uint32_t, uint64_t);
uint64_t lj_m7_ccall_jit_u64_u32_i64(uint32_t, int64_t);
uint64_t lj_m7_ccall_jit_u64_u32_u64(uint32_t, uint64_t);
uint8_t lj_m7_ccall_jit_u8_i64_i32(int64_t, int32_t);
uint8_t lj_m7_ccall_jit_u8_i64_u32(int64_t, uint32_t);
void lj_m7_ccall_jit_void_i64_i32(int64_t, int32_t);
void lj_m7_ccall_jit_void_i64_u32(int64_t, uint32_t);
int32_t lj_m7_ccall_jit_i32_i64_i32(int64_t, int32_t);
int32_t lj_m7_ccall_jit_i32_i64_u32(int64_t, uint32_t);
uint32_t lj_m7_ccall_jit_u32_i64_i32(int64_t, int32_t);
uint32_t lj_m7_ccall_jit_u32_i64_u32(int64_t, uint32_t);
int *lj_m7_ccall_jit_ptr_i64_i32(int64_t, int32_t);
int *lj_m7_ccall_jit_ptr_i64_u32(int64_t, uint32_t);
int64_t lj_m7_ccall_jit_i64_i64_i32(int64_t, int32_t);
int64_t lj_m7_ccall_jit_i64_i64_u32(int64_t, uint32_t);
uint64_t lj_m7_ccall_jit_u64_i64_i32(int64_t, int32_t);
uint64_t lj_m7_ccall_jit_u64_i64_u32(int64_t, uint32_t);
uint8_t lj_m7_ccall_jit_u8_u64_i32(uint64_t, int32_t);
uint8_t lj_m7_ccall_jit_u8_u64_u32(uint64_t, uint32_t);
void lj_m7_ccall_jit_void_u64_i32(uint64_t, int32_t);
void lj_m7_ccall_jit_void_u64_u32(uint64_t, uint32_t);
int32_t lj_m7_ccall_jit_i32_u64_i32(uint64_t, int32_t);
int32_t lj_m7_ccall_jit_i32_u64_u32(uint64_t, uint32_t);
uint32_t lj_m7_ccall_jit_u32_u64_i32(uint64_t, int32_t);
uint32_t lj_m7_ccall_jit_u32_u64_u32(uint64_t, uint32_t);
int *lj_m7_ccall_jit_ptr_u64_i32(uint64_t, int32_t);
int *lj_m7_ccall_jit_ptr_u64_u32(uint64_t, uint32_t);
int64_t lj_m7_ccall_jit_i64_u64_i32(uint64_t, int32_t);
int64_t lj_m7_ccall_jit_i64_u64_u32(uint64_t, uint32_t);
uint64_t lj_m7_ccall_jit_u64_u64_i32(uint64_t, int32_t);
uint64_t lj_m7_ccall_jit_u64_u64_u32(uint64_t, uint32_t);
uint8_t lj_m7_ccall_jit_u8_i64_ptr(int64_t, int *);
uint8_t lj_m7_ccall_jit_u8_u64_ptr(uint64_t, int *);
void lj_m7_ccall_jit_void_i64_ptr(int64_t, int *);
void lj_m7_ccall_jit_void_u64_ptr(uint64_t, int *);
int32_t lj_m7_ccall_jit_i32_i64_ptr(int64_t, int *);
int32_t lj_m7_ccall_jit_i32_u64_ptr(uint64_t, int *);
uint32_t lj_m7_ccall_jit_u32_i64_ptr(int64_t, int *);
uint32_t lj_m7_ccall_jit_u32_u64_ptr(uint64_t, int *);
int *lj_m7_ccall_jit_ptr_i64_ptr(int64_t, int *);
int *lj_m7_ccall_jit_ptr_u64_ptr(uint64_t, int *);
int64_t lj_m7_ccall_jit_i64_i64_ptr(int64_t, int *);
int64_t lj_m7_ccall_jit_i64_u64_ptr(uint64_t, int *);
uint64_t lj_m7_ccall_jit_u64_i64_ptr(int64_t, int *);
uint64_t lj_m7_ccall_jit_u64_u64_ptr(uint64_t, int *);
uint8_t lj_m7_ccall_jit_u8_i64_i64(int64_t, int64_t);
uint8_t lj_m7_ccall_jit_u8_i64_u64(int64_t, uint64_t);
uint8_t lj_m7_ccall_jit_u8_u64_i64(uint64_t, int64_t);
uint8_t lj_m7_ccall_jit_u8_u64_u64(uint64_t, uint64_t);
void lj_m7_ccall_jit_void_i64_i64(int64_t, int64_t);
void lj_m7_ccall_jit_void_i64_u64(int64_t, uint64_t);
void lj_m7_ccall_jit_void_u64_i64(uint64_t, int64_t);
void lj_m7_ccall_jit_void_u64_u64(uint64_t, uint64_t);
int32_t lj_m7_ccall_jit_i32_i64_i64(int64_t, int64_t);
int32_t lj_m7_ccall_jit_i32_i64_u64(int64_t, uint64_t);
int32_t lj_m7_ccall_jit_i32_u64_i64(uint64_t, int64_t);
int32_t lj_m7_ccall_jit_i32_u64_u64(uint64_t, uint64_t);
uint32_t lj_m7_ccall_jit_u32_i64_i64(int64_t, int64_t);
uint32_t lj_m7_ccall_jit_u32_i64_u64(int64_t, uint64_t);
uint32_t lj_m7_ccall_jit_u32_u64_i64(uint64_t, int64_t);
uint32_t lj_m7_ccall_jit_u32_u64_u64(uint64_t, uint64_t);
int *lj_m7_ccall_jit_ptr_i64_i64(int64_t, int64_t);
int *lj_m7_ccall_jit_ptr_i64_u64(int64_t, uint64_t);
int *lj_m7_ccall_jit_ptr_u64_i64(uint64_t, int64_t);
int *lj_m7_ccall_jit_ptr_u64_u64(uint64_t, uint64_t);
uint64_t lj_m7_ccall_jit_u64_0(void);
int *lj_m7_ccall_jit_ptr0(void);
int lj_m7_ccall_jit_ptr_read_i32(int *);
int lj_m7_ccall_jit_ptr_sum_i32(int *, int *);
int lj_m7_ccall_jit_i32_ptr_read_i32(int, int *);
int *lj_m7_ccall_jit_ptr_add_i32(int *, int);
int *lj_m7_ccall_jit_ptr_num(double);
int lj_m7_ccall_jit_i32_ptr_ulong_i32(int *, unsigned long, int);
]]

local abs = ffi.C.abs
local function now()
  return assert(th.now())
end

local function run_abs(n)
  local s = 0
  for i = 1, n do
    s = s + abs(-i)
  end
  return s
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
assert(run_abs(100) == 5050)
assert(trace_count() > 0, "supported int->int FFI call loop should trace")

assert(ffi.blocking == nil)
local getpid = ffi.C.getpid
assert(getpid() > 0)

do
  local function run_getpid(n)
    local pid = 0
    for _ = 1, n do
      pid = getpid()
    end
    return pid
  end
  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")
  assert(run_getpid(100) > 0)
  assert(trace_count() > 0, "supported void->int FFI call loop should trace")
end

do
  local poll = ffi.C.poll
  local function run_poll0(n)
    for _ = 1, n do
      assert(poll(nil, 0, 0) == 0)
    end
  end
  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")
  run_poll0(100)
  assert(trace_count() > 0, "ptr,unsigned long,int->int FFI call loop should trace")
end

do
  local src = ffi.new("uint8_t[128]")
  local dst = ffi.new("uint8_t[128]")
  local cstr = ffi.new("char[16]", "abcdef")

  local function heat(fn)
    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    fn(120)
    return trace_count()
  end

  local function expect_trace(label, fn)
    assert(heat(fn) > 0, label .. " should keep tracing")
  end

  local function expect_no_trace(label, fn)
    assert(heat(fn) == 0, label .. " must use the native interpreter path")
  end

  expect_trace("small constant ffi.copy/fill/string", function(n)
    local total = 0
    for i = 1, n do
      ffi.copy(dst, src, 8)
      ffi.fill(dst, 8, i)
      total = total + #ffi.string(cstr, 3)
    end
    assert(total == n * 3)
  end)

  expect_trace("dynamic-length ffi.copy", function(n)
    ffi.fill(dst, 128, 0)
    for i = 1, n do
      ffi.copy(dst, src, (i % 64) + 1)
    end
    assert(dst[0] == 0)
  end)

  expect_trace("dynamic-length ffi.fill", function(n)
    for i = 1, n do
      ffi.fill(dst, (i % 64) + 1, i)
    end
    assert(dst[0] == n % 256)
  end)

  expect_trace("unbounded ffi.string", function(n)
    local total = 0
    for _ = 1, n do
      total = total + #ffi.string(cstr)
    end
    assert(total == n * 6)
  end)

  expect_trace("dynamic-length ffi.string", function(n)
    local total = 0
    for i = 1, n do
      total = total + #ffi.string(cstr, (i % 6) + 1)
    end
    assert(total == n * 7 / 2)
  end)
end

do
  local so = os.getenv("LJ_M7_FFI_CCALL_JIT_SO")
  if so then
    local lib = ffi.load(so)
    local sleep_i32 = lib.lj_m7_ccall_jit_sleep_i32
    local add2_i32 = lib.lj_m7_ccall_jit_add2_i32
    local i64_0 = lib.lj_m7_ccall_jit_i64_0
    local i64_i32 = lib.lj_m7_ccall_jit_i64_i32
    local i64_ptr = lib.lj_m7_ccall_jit_i64_ptr
    local i64_i32_ptr = lib.lj_m7_ccall_jit_i64_i32_ptr
    local i64_i64 = lib.lj_m7_ccall_jit_i64_i64
    local i64_i64_i64 = lib.lj_m7_ccall_jit_i64_i64_i64
    local i64_u64 = lib.lj_m7_ccall_jit_i64_u64
    local i64_i64_u64 = lib.lj_m7_ccall_jit_i64_i64_u64
    local i64_u64_i64 = lib.lj_m7_ccall_jit_i64_u64_i64
    local i64_i32_ptr_u64 = lib.lj_m7_ccall_jit_i64_i32_ptr_u64
    local i32_i32_ptr_u32 = lib.lj_m7_ccall_jit_i32_i32_ptr_u32
    local i8_0 = lib.lj_m7_ccall_jit_i8_0
    local i8_i32 = lib.lj_m7_ccall_jit_i8_i32
    local u8_0 = lib.lj_m7_ccall_jit_u8_0
    local u8_ptr = lib.lj_m7_ccall_jit_u8_ptr
    local i16_0 = lib.lj_m7_ccall_jit_i16_0
    local i16_i32_ptr = lib.lj_m7_ccall_jit_i16_i32_ptr
    local u16_0 = lib.lj_m7_ccall_jit_u16_0
    local u16_i32 = lib.lj_m7_ccall_jit_u16_i32
    local i8_arg_i32 = lib.lj_m7_ccall_jit_i8_arg_i32
    local i32_u32 = lib.lj_m7_ccall_jit_i32_u32
    local i32_u32_ptr = lib.lj_m7_ccall_jit_i32_u32_ptr
    local i32_i64arg = lib.lj_m7_ccall_jit_i32_i64arg
    local i32_u64arg = lib.lj_m7_ccall_jit_i32_u64arg
    local u8_u32 = lib.lj_m7_ccall_jit_u8_u32
    local u8_i64arg = lib.lj_m7_ccall_jit_u8_i64arg
    local u8_ptr_u64 = lib.lj_m7_ccall_jit_u8_ptr_u64
    local u8_ptr_i64 = lib.lj_m7_ccall_jit_u8_ptr_i64
    local void_u32 = lib.lj_m7_ccall_jit_void_u32
    local void_i64arg = lib.lj_m7_ccall_jit_void_i64arg
    local void_u64arg = lib.lj_m7_ccall_jit_void_u64arg
    local void_ptr_u64 = lib.lj_m7_ccall_jit_void_ptr_u64
    local void_ptr_i64 = lib.lj_m7_ccall_jit_void_ptr_i64
    local u64_u32arg = lib.lj_m7_ccall_jit_u64_u32arg
    local u32_u64arg = lib.lj_m7_ccall_jit_u32_u64arg
    local i32_ptr_u64 = lib.lj_m7_ccall_jit_i32_ptr_u64
    local i32_ptr_i64 = lib.lj_m7_ccall_jit_i32_ptr_i64
    local u32_ptr_u64 = lib.lj_m7_ccall_jit_u32_ptr_u64
    local u32_ptr_i64 = lib.lj_m7_ccall_jit_u32_ptr_i64
    local ptr_u32 = lib.lj_m7_ccall_jit_ptr_u32
    local ptr_u64arg = lib.lj_m7_ccall_jit_ptr_u64arg
    local ptr_ptr_u64 = lib.lj_m7_ccall_jit_ptr_ptr_u64
    local ptr_ptr_i64 = lib.lj_m7_ccall_jit_ptr_ptr_i64
    local num0 = lib.lj_m7_ccall_jit_num0
    local num_i32 = lib.lj_m7_ccall_jit_num_i32
    local num_ptr = lib.lj_m7_ccall_jit_num_ptr
    local num_flt = lib.lj_m7_ccall_jit_num_flt
    local num1 = lib.lj_m7_ccall_jit_num1
    local num2 = lib.lj_m7_ccall_jit_num2
    local flt0 = lib.lj_m7_ccall_jit_flt0
    local flt1 = lib.lj_m7_ccall_jit_flt1
    local flt2 = lib.lj_m7_ccall_jit_flt2
    local void_count_i32 = lib.lj_m7_ccall_jit_void_count_i32
    local i32_num = lib.lj_m7_ccall_jit_i32_num
    local i32_flt = lib.lj_m7_ccall_jit_i32_flt
    local void0 = lib.lj_m7_ccall_jit_void0
    local void_num = lib.lj_m7_ccall_jit_void_num
    local void_flt = lib.lj_m7_ccall_jit_void_flt
    local flt_num = lib.lj_m7_ccall_jit_flt_num
    local store_i32 = lib.lj_m7_ccall_jit_store_i32
    local u32 = lib.lj_m7_ccall_jit_u32
    local u32_i32 = lib.lj_m7_ccall_jit_u32_i32
    local u32_ptr = lib.lj_m7_ccall_jit_u32_ptr
    local u32_i32_ptr = lib.lj_m7_ccall_jit_u32_i32_ptr
    local u32_0 = lib.lj_m7_ccall_jit_u32_0
    local u64 = lib.lj_m7_ccall_jit_u64
    local u64_u64_u64 = lib.lj_m7_ccall_jit_u64_u64_u64
    local u64_i64 = lib.lj_m7_ccall_jit_u64_i64
    local u64_i64_u64 = lib.lj_m7_ccall_jit_u64_i64_u64
    local u64_u64_i64 = lib.lj_m7_ccall_jit_u64_u64_i64
    local u64_i32 = lib.lj_m7_ccall_jit_u64_i32
    local u64_ptr = lib.lj_m7_ccall_jit_u64_ptr
    local u64_i32_ptr = lib.lj_m7_ccall_jit_u64_i32_ptr
    local i64_ptr_u64 = lib.lj_m7_ccall_jit_i64_ptr_u64
    local u64_ptr_u64 = lib.lj_m7_ccall_jit_u64_ptr_u64
    local i64_ptr_i64 = lib.lj_m7_ccall_jit_i64_ptr_i64
    local u64_ptr_i64 = lib.lj_m7_ccall_jit_u64_ptr_i64
    local u64_0 = lib.lj_m7_ccall_jit_u64_0
    local ptr0 = lib.lj_m7_ccall_jit_ptr0
    local ptr_read_i32 = lib.lj_m7_ccall_jit_ptr_read_i32
    local ptr_sum_i32 = lib.lj_m7_ccall_jit_ptr_sum_i32
    local i32_ptr_read_i32 = lib.lj_m7_ccall_jit_i32_ptr_read_i32
    local ptr_add_i32 = lib.lj_m7_ccall_jit_ptr_add_i32
    local ptr_num = lib.lj_m7_ccall_jit_ptr_num
    local i32_ptr_ulong_i32 = lib.lj_m7_ccall_jit_i32_ptr_ulong_i32
    local function run_add2(n)
      local r = 0
      for i = 1, n do
	r = r + add2_i32(i, 2)
      end
      return r
    end
    local function run_i64_0(n)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(i64_0())
      end
      return r
    end
    local function run_i64_i32(n)
      local r = 0
      for i = 1, n do
	r = r + tonumber(i64_i32(i))
      end
      return r
    end
    local function run_i64_ptr(n)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + tonumber(i64_ptr(p))
      end
      return r
    end
    local function run_i64_i32_ptr(n)
      local p = ptr0()
      local r = 0
      for i = 1, n do
	r = r + tonumber(i64_i32_ptr(i % 4, p))
      end
      return r
    end
    local function run_i64_i64(n)
      local arg = ffi.new("int64_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(i64_i64(arg))
      end
      return r
    end
    local function run_i64_i64_i64(n)
      local a = ffi.new("int64_t", 0x100000000)
      local b = ffi.new("int64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(i64_i64_i64(a, b))
      end
      return r
    end
    local function run_i64_u64(n)
      local arg = ffi.new("uint64_t", -2)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(i64_u64(arg))
      end
      return r
    end
    local function run_i64_i64_u64(n)
      local a = ffi.new("int64_t", 11)
      local b = ffi.new("uint64_t", -16)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(i64_i64_u64(a, b))
      end
      return r
    end
    local function run_i64_u64_i64(n)
      local a = ffi.new("uint64_t", -16)
      local b = ffi.new("int64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(i64_u64_i64(a, b))
      end
      return r
    end
    local function run_i64_i32_ptr_u64(n)
      local p = ptr0()
      local count = ffi.new("uint64_t", 0xfffffff2)
      local r = 0
      for i = 1, n do
	r = r + tonumber(i64_i32_ptr_u64(i, p, count))
      end
      return r
    end
    local function run_i32_i32_ptr_u32(n)
      local p = ptr0()
      local count = ffi.new("uint32_t", 0xfffffff2)
      local r = 0
      for i = 1, n do
	r = r + i32_i32_ptr_u32(i, p, count)
      end
      return r
    end
    local function run_i8_0(n)
      local r = 0
      for _ = 1, n do
	r = r + i8_0()
      end
      return r
    end
    local function run_i8_i32(n)
      local r = 0
      for i = 1, n do
	r = r + i8_i32(i)
      end
      return r
    end
    local function run_u8_0(n)
      local r = 0
      for _ = 1, n do
	r = r + u8_0()
      end
      return r
    end
    local function run_u8_ptr(n)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + u8_ptr(p)
      end
      return r
    end
    local function run_i16_0(n)
      local r = 0
      for _ = 1, n do
	r = r + i16_0()
      end
      return r
    end
    local function run_i16_i32_ptr(n)
      local p = ptr0()
      local r = 0
      for i = 1, n do
	r = r + i16_i32_ptr(i % 4, p)
      end
      return r
    end
    local function run_u16_0(n)
      local r = 0
      for _ = 1, n do
	r = r + u16_0()
      end
      return r
    end
    local function run_u16_i32(n)
      local r = 0
      for i = 1, n do
	r = r + u16_i32(i)
      end
      return r
    end
    local function run_i8_arg(n)
      local r = 0
      for _ = 1, n do
	r = r + i8_arg_i32(7)
      end
      return r
    end
    local function run_i8_arg_wrap(n)
      local arg = ffi.new("uint8_t", 255)
      local r = 0
      for _ = 1, n do
	r = r + i8_arg_i32(arg)
      end
      return r
    end
    local function run_i32_u32(n)
      local r = 0
      for i = 1, n do
	r = r + i32_u32(i)
      end
      return r
    end
    local function run_i32_u32_high(n)
      local arg = ffi.new("uint32_t", 0xfffffff0)
      local r = 0
      for _ = 1, n do
	r = r + i32_u32(arg)
      end
      return r
    end
    local function run_i32_u32_ptr_high(n)
      local p = ptr0()
      local arg = ffi.new("uint32_t", 0xfffffff1)
      local r = 0
      for _ = 1, n do
	r = r + i32_u32_ptr(arg, p)
      end
      return r
    end
    local function run_i32_i64arg(n)
      local arg = ffi.new("int64_t", 0x100000123)
      local r = 0
      for _ = 1, n do
	r = r + i32_i64arg(arg)
      end
      return r
    end
    local function run_i32_u64arg(n)
      local arg = ffi.new("uint64_t", -16)
      local r = 0
      for _ = 1, n do
	r = r + i32_u64arg(arg)
      end
      return r
    end
    local function run_u8_u32_high(n)
      local arg = ffi.new("uint32_t", 0xfffffffe)
      local r = 0
      for _ = 1, n do
	r = r + u8_u32(arg)
      end
      return r
    end
    local function run_u8_i64arg(n)
      local arg = ffi.new("int64_t", -4)
      local r = 0
      for _ = 1, n do
	r = r + u8_i64arg(arg)
      end
      return r
    end
    local function run_u8_ptr_u64(n)
      local p = ptr0()
      local count = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + u8_ptr_u64(p, count)
      end
      return r
    end
    local function run_u8_ptr_i64(n)
      local p = ptr0()
      local count = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + u8_ptr_i64(p, count)
      end
      return r
    end
    local function run_void_u32(n)
      local before = void_count_i32()
      for i = 1, n do
	assert(void_u32(i) == nil)
      end
      return void_count_i32() - before
    end
    local function run_void_i64arg(n)
      local arg = ffi.new("int64_t", 0x10000001f)
      local before = void_count_i32()
      for _ = 1, n do
	assert(void_i64arg(arg) == nil)
      end
      return void_count_i32() - before
    end
    local function run_void_u64arg(n)
      local arg = ffi.new("uint64_t", -2)
      local before = void_count_i32()
      for _ = 1, n do
	assert(void_u64arg(arg) == nil)
      end
      return void_count_i32() - before
    end
    local function run_void_ptr_u64(n)
      local p = ptr0()
      local count = ffi.new("uint64_t", -14)
      local before = void_count_i32()
      for _ = 1, n do
	assert(void_ptr_u64(p, count) == nil)
      end
      return void_count_i32() - before
    end
    local function run_void_ptr_i64(n)
      local p = ptr0()
      local count = ffi.new("int64_t", -14)
      local before = void_count_i32()
      for _ = 1, n do
	assert(void_ptr_i64(p, count) == nil)
      end
      return void_count_i32() - before
    end
    local function run_u64_u32arg_high(n)
      local arg = ffi.new("uint32_t", 0xfffffff0)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(u64_u32arg(arg))
      end
      return r
    end
    local function run_u32_u64arg(n)
      local arg = ffi.new("uint64_t", 0x1f)
      local r = 0
      for _ = 1, n do
	r = r + u32_u64arg(arg)
      end
      return r
    end
    local function run_i32_ptr_u64(n)
      local p = ptr0()
      local count = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + i32_ptr_u64(p, count)
      end
      return r
    end
    local function run_i32_ptr_i64(n)
      local p = ptr0()
      local count = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + i32_ptr_i64(p, count)
      end
      return r
    end
    local function run_u32_ptr_u64(n)
      local p = ptr0()
      local count = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + u32_ptr_u64(p, count)
      end
      return r
    end
    local function run_u32_ptr_i64(n)
      local p = ptr0()
      local count = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + u32_ptr_i64(p, count)
      end
      return r
    end
    local function run_ptr_u32(n)
      local r = 0
      for i = 1, n do
	r = r + ptr_u32(i)[0]
      end
      return r
    end
    local function run_ptr_ptr_u64(n)
      local p = ptr0()
      local count = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + ptr_ptr_u64(p, count)[0]
      end
      return r
    end
    local function run_ptr_ptr_i64(n)
      local p = ptr0()
      local count = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + ptr_ptr_i64(p, count)[0]
      end
      return r
    end
    local function run_ptr_u64arg(n)
      local arg = ffi.new("uint64_t", -15)
      local r = 0
      for _ = 1, n do
	r = r + ptr_u64arg(arg)[0]
      end
      return r
    end
    local function run_num0(n)
      local r = 0
      for _ = 1, n do
	r = r + num0()
      end
      return r
    end
    local function run_num_i32(n)
      local r = 0
      for i = 1, n do
	r = r + num_i32(i)
      end
      return r
    end
    local function run_num_ptr(n)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + num_ptr(p)
      end
      return r
    end
    local function run_num_flt(n)
      local r = 0
      for i = 1, n do
	r = r + num_flt(i + 0.25)
      end
      return r
    end
    local function run_num1(n)
      local r = 0
      for i = 1, n do
	r = r + num1(i)
      end
      return r
    end
    local function run_num2(n)
      local r = 0
      for i = 1, n do
	r = r + num2(i, 0.25)
      end
      return r
    end
    local function run_flt0(n)
      local r = 0
      for _ = 1, n do
	r = r + flt0()
      end
      return r
    end
    local function run_flt1(n)
      local r = 0
      for i = 1, n do
	r = r + flt1(i)
      end
      return r
    end
    local function run_flt2(n)
      local r = 0
      for i = 1, n do
	r = r + flt2(i, 0.25)
      end
      return r
    end
    local function run_i32_num(n)
      local r = 0
      for i = 1, n do
	r = r + i32_num(i + 0.25)
      end
      return r
    end
    local function run_i32_flt(n)
      local r = 0
      for i = 1, n do
	r = r + i32_flt(i + 0.75)
      end
      return r
    end
    local function run_void0(n)
      local before = void_count_i32()
      for _ = 1, n do
	assert(void0() == nil)
      end
      return void_count_i32() - before
    end
    local function run_void_num(n)
      local before = void_count_i32()
      for i = 1, n do
	assert(void_num(i + 0.25) == nil)
      end
      return void_count_i32() - before
    end
    local function run_void_flt(n)
      local before = void_count_i32()
      for i = 1, n do
	assert(void_flt(i + 0.25) == nil)
      end
      return void_count_i32() - before
    end
    local function run_flt_num(n)
      local r = 0
      for i = 1, n do
	r = r + flt_num(i + 0.25)
      end
      return r
    end
    local function run_void_store(n)
      local p = ffi.new("int[1]")
      local last
      for i = 1, n do
	last = store_i32(p, i)
	assert(last == nil)
      end
      return p[0]
    end
    local function run_u32(n)
      local r = 0
      for _ = 1, n do
	r = r + u32(7)
      end
      return r
    end
    local function run_u32_high(n)
      local arg = ffi.new("uint32_t", 0xf0000000)
      local r = 0
      for _ = 1, n do
	r = r + u32(arg)
      end
      return r
    end
    local function run_u32_i32(n)
      local r = 0
      for i = 1, n do
	r = r + u32_i32(i)
      end
      return r
    end
    local function run_u32_ptr(n)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + u32_ptr(p)
      end
      return r
    end
    local function run_u32_i32_ptr(n)
      local p = ptr0()
      local r = 0
      for i = 1, n do
	r = r + u32_i32_ptr(i % 4, p)
      end
      return r
    end
    local function run_u32_0(n)
      local r = 0
      for _ = 1, n do
	r = r + u32_0()
      end
      return r
    end
    local function run_u64(n)
      local arg = ffi.new("uint64_t", 7)
      local expected = ffi.new("uint64_t", 8)
      for _ = 1, n do
	assert(u64(arg) == expected)
      end
      return true
    end
    local function run_u64_u64_u64(n)
      local a = ffi.new("uint64_t", -16)
      local b = ffi.new("uint64_t", 9)
      local expected = ffi.new("uint64_t", -2)
      for _ = 1, n do
	assert(u64_u64_u64(a, b) == expected)
      end
      return true
    end
    local function run_u64_i64(n)
      local arg = ffi.new("int64_t", -7)
      local expected = ffi.new("uint64_t", -12)
      for _ = 1, n do
	assert(u64_i64(arg) == expected)
      end
      return true
    end
    local function run_u64_i64_u64(n)
      local a = ffi.new("int64_t", -32)
      local b = ffi.new("uint64_t", 20)
      local expected = ffi.new("uint64_t", -1)
      for _ = 1, n do
	assert(u64_i64_u64(a, b) == expected)
      end
      return true
    end
    local function run_u64_u64_i64(n)
      local a = ffi.new("uint64_t", 20)
      local b = ffi.new("int64_t", -34)
      local expected = ffi.new("uint64_t", -1)
      for _ = 1, n do
	assert(u64_u64_i64(a, b) == expected)
      end
      return true
    end
    local function run_u64_i32(n)
      local r = 0
      for i = 1, n do
	r = r + tonumber(u64_i32(i))
      end
      return r
    end
    local function run_u64_ptr(n)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + tonumber(u64_ptr(p))
      end
      return r
    end
    local function run_u64_i32_ptr(n)
      local p = ptr0()
      local r = 0
      for i = 1, n do
	r = r + tonumber(u64_i32_ptr(i % 4, p))
      end
      return r
    end
    local function run_i64_ptr_u64(n)
      local p = ptr0()
      local count = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(i64_ptr_u64(p, count))
      end
      return r
    end
    local function run_u64_ptr_u64(n)
      local p = ptr0()
      local count = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(u64_ptr_u64(p, count))
      end
      return r
    end
    local function run_i64_ptr_i64(n)
      local p = ptr0()
      local count = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(i64_ptr_i64(p, count))
      end
      return r
    end
    local function run_u64_ptr_i64(n)
      local p = ptr0()
      local count = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(u64_ptr_i64(p, count))
      end
      return r
    end
    local function run_u64_0(n)
      local expected = ffi.new("uint64_t", -1)
      local r
      for _ = 1, n do
	r = u64_0()
	assert(r == expected)
      end
      return r
    end
    local function run_ptr0(n)
      local p
      for _ = 1, n do
	p = ptr0()
      end
      return p[2]
    end
    local function run_ptr_read(n)
      local p = ptr0()
      local r = 0
      for i = 1, n do
	r = r + ptr_read_i32(p + (i % 4))
      end
      return r
    end
    local function run_ptr_sum(n)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + ptr_sum_i32(p, p + 1)
      end
      return r
    end
    local function run_i32_ptr_read(n)
      local p = ptr0()
      local r = 0
      for i = 1, n do
	r = r + i32_ptr_read_i32(i % 4, p)
      end
      return r
    end
    local function run_ptr_add(n)
      local p = ptr0()
      local r = 0
      for i = 1, n do
	r = r + ptr_add_i32(p, i % 4)[0]
      end
      return r
    end
    local function run_ptr_num(n)
      local r = 0
      for i = 1, n do
	r = r + ptr_num(i + 0.25)[0]
      end
      return r
    end
    local function run_ptr_ulong_i32(n)
      local p = ptr0()
      local count = ffi.new("int8_t", -1)
      local r = 0
      for i = 1, n do
	r = r + i32_ptr_ulong_i32(p, count, i % 4)
      end
      return r
    end
    local function run_sleep(n, ms)
      local r = 0
      for _ = 1, n do
	r = sleep_i32(ms)
      end
      return r
    end

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_add2(80) == (80 * 81) / 2 + 80 * 5)
    assert(trace_count() > 0, "shared int,int->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i64_0(80) == 80 * 17)
    assert(trace_count() > 0, "shared void->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i64_i32(80) == 80 * 4294967296 + (80 * 81) / 2)
    assert(trace_count() > 0, "shared int->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i64_ptr(80) == 80 * (4294967296 + 11))
    assert(trace_count() > 0, "shared ptr->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i64_i32_ptr(80) == 80 * 4294967296 + 80 * 27 + 40)
    assert(trace_count() > 0, "shared int,ptr->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i64_i64(80) == 640)
    assert(trace_count() > 0, "shared int64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i64_i64_i64(80) == 80 * (4294967296 + 9 + 3))
    assert(trace_count() > 0, "shared int64_t,int64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i64_u64(80) == -960)
    assert(trace_count() > 0, "shared uint64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i64_i64_u64(80) == 80 * 256)
    assert(trace_count() > 0, "shared int64_t,uint64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i64_u64_i64(80) == 80 * 252)
    assert(trace_count() > 0, "shared uint64_t,int64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i64_i32_ptr_u64(80) == 80 * (4294967296 + 1043) + (80 * 81) / 2)
    assert(trace_count() > 0, "shared int,ptr,uint64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local offset = ffi.new("int64_t", 0x100000010)
      local r = 0
      for i = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_i64_i32_i64_i32(i, offset, 3))
      end
      return r
    end)(80) == 80 * (4294967296 + 4294967312 + 3) + (80 * 81) / 2)
    assert(trace_count() > 0, "shared int,int64_t,int->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i32_i32_ptr_u32(80) == 80 * 1043 + (80 * 81) / 2)
    assert(trace_count() > 0, "shared int,ptr,uint32_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local p = ptr0()
      local count = ffi.new("uint32_t", 0xfffffff2)
      local r = 0
      for i = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_i32_ptr_u32(i, p, count)
      end
      return r
    end)(80) == 80 * (2147483648 + 1043) + (80 * 81) / 2)
    assert(trace_count() > 0, "shared int,ptr,uint32_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local p = ptr0()
      local count = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_ptr_ptr_u64(p, p, count)
      end
      return r
    end)(80) == 80 * 1087)
    assert(trace_count() > 0, "shared ptr,ptr,uint64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local p = ptr0()
      local count = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_ptr_ptr_u64(p, p, count)
      end
      return r
    end)(80) == 80 * (2147483648 + 1087))
    assert(trace_count() > 0, "shared ptr,ptr,uint64_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local p = ptr0()
      local len = ffi.new("int32_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_ptr_ptr_i32(p, p, len)
      end
      return r
    end)(80) == 80 * (33 + 44 - 14))
    assert(trace_count() > 0, "shared ptr,ptr,int32_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local p = ptr0()
      local count = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_i64_ptr_ptr_u64(p, p, count))
      end
      return r
    end)(80) == 80 * (4294967296 + 1087))
    assert(trace_count() > 0, "shared ptr,ptr,uint64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local p = ptr0()
      local count = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_u64_ptr_ptr_u64(p, p, count))
      end
      return r
    end)(80) == 80 * (4294967296 + 1087))
    assert(trace_count() > 0, "shared ptr,ptr,uint64_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local dst = ffi.new("int[4]")
      local src = ptr0()
      local count = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	local p = lib.lj_m7_ccall_jit_ptr_ptr_ptr_u64(dst, src, count)
	r = r + p[0]
      end
      assert(dst[2] == 46)
      return r
    end)(80) == 80 * 46)
    assert(trace_count() > 0, "shared ptr,ptr,uint64_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local dst = ffi.new("int[4]")
      local src = ptr0()
      local count = ffi.new("uint64_t", -14)
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_ptr_ptr_u64(dst, src, count) == nil)
      end
      return dst[2]
    end)(80) == 80 * 46)
    assert(trace_count() > 0, "shared ptr,ptr,uint64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i8_0(80) == -560)
    assert(trace_count() > 0, "shared void->int8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i8_i32(80) == 2600)
    assert(trace_count() > 0, "shared int->int8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u8_0(80) == 20000)
    assert(trace_count() > 0, "shared void->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u8_ptr(80) == 16880)
    assert(trace_count() > 0, "shared ptr->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i16_0(80) == -98720)
    assert(trace_count() > 0, "shared void->int16_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i16_i32_ptr(80) == -157800)
    assert(trace_count() > 0, "shared int,ptr->int16_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u16_0(80) == 4800000)
    assert(trace_count() > 0, "shared void->uint16_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u16_i32(80) == 4803240)
    assert(trace_count() > 0, "shared int->uint16_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i8_arg(80) == 960)
    assert(trace_count() > 0, "shared int8_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i8_arg_wrap(80) == 320)
    assert(trace_count() > 0, "shared wrapped uint8_t->int8_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i32_u32(80) == (80 * 81) / 2 + 80 * 5)
    assert(trace_count() > 0, "shared uint32_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i32_u32_high(80) == -880)
    assert(trace_count() > 0, "shared high-bit uint32_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i32_u32_ptr_high(80) == 82480)
    assert(trace_count() > 0, "shared high-bit uint32_t,ptr->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i32_i64arg(80) == 80 * 284)
    assert(trace_count() > 0, "shared int64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i32_u64arg(80) == 80 * 1017)
    assert(trace_count() > 0, "shared uint64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u8_u32_high(80) == 80)
    assert(trace_count() > 0, "shared high-bit uint32_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u8_i64arg(80) == 80)
    assert(trace_count() > 0, "shared int64_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u8_ptr_u64(80) == 1520)
    assert(trace_count() > 0, "shared ptr,uint64_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u8_ptr_i64(80) == 1520)
    assert(trace_count() > 0, "shared ptr,int64_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_i32_i64(7, arg)
      end
      return r
    end)(80) == 80 * 249)
    assert(trace_count() > 0, "shared int,int64_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_i32_u64(7, arg)
      end
      return r
    end)(80) == 80 * 249)
    assert(trace_count() > 0, "shared int,uint64_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_u32_i64(a, b)
      end
      return r
    end)(80) == 80 * 249)
    assert(trace_count() > 0, "shared uint32_t,int64_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_u32_u64(a, b)
      end
      return r
    end)(80) == 80 * 249)
    assert(trace_count() > 0, "shared uint32_t,uint64_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_void_u32(80) == 600)
    assert(trace_count() > 0, "shared uint32_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_void_i64arg(80) == 80 * 31)
    assert(trace_count() > 0, "shared int64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_void_u64arg(80) == 80 * 30)
    assert(trace_count() > 0, "shared uint64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_void_ptr_u64(80) == 80 * 35)
    assert(trace_count() > 0, "shared ptr,uint64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_void_ptr_i64(80) == 80 * 35)
    assert(trace_count() > 0, "shared ptr,int64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("int64_t", -14)
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_i32_i64(7, arg) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 9)
    assert(trace_count() > 0, "shared int,int64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("uint64_t", -14)
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_i32_u64(7, arg) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 9)
    assert(trace_count() > 0, "shared int,uint64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("int64_t", -14)
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_u32_i64(a, b) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 9)
    assert(trace_count() > 0, "shared uint32_t,int64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("uint64_t", -14)
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_u32_u64(a, b) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 9)
    assert(trace_count() > 0, "shared uint32_t,uint64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64_u32arg_high(80) == 80 * (4294967296 + 0xfffffff0))
    assert(trace_count() > 0, "shared high-bit uint32_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u32_u64arg(80) == 80 * (0xf0000000 + 0x1f))
    assert(trace_count() > 0, "shared uint64_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i32_ptr_u64(80) == 80 * 1043)
    assert(trace_count() > 0, "shared ptr,uint64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i32_ptr_i64(80) == 80 * 1043)
    assert(trace_count() > 0, "shared ptr,int64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_i32_i64(7, arg)
      end
      return r
    end)(80) == 80 * 1017)
    assert(trace_count() > 0, "shared int,int64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_i32_u64(7, arg)
      end
      return r
    end)(80) == 80 * 1017)
    assert(trace_count() > 0, "shared int,uint64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_u32_i64(a, b)
      end
      return r
    end)(80) == 80 * 1017)
    assert(trace_count() > 0, "shared uint32_t,int64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_u32_u64(a, b)
      end
      return r
    end)(80) == 80 * 1017)
    assert(trace_count() > 0, "shared uint32_t,uint64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u32_ptr_u64(80) == 80 * (0xf0000000 + 1043))
    assert(trace_count() > 0, "shared ptr,uint64_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u32_ptr_i64(80) == 80 * (0xf0000000 + 1043))
    assert(trace_count() > 0, "shared ptr,int64_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_i32_i64(7, arg)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1017))
    assert(trace_count() > 0, "shared int,int64_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_i32_u64(7, arg)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1017))
    assert(trace_count() > 0, "shared int,uint64_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_u32_i64(a, b)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1017))
    assert(trace_count() > 0, "shared uint32_t,int64_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_u32_u64(a, b)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1017))
    assert(trace_count() > 0, "shared uint32_t,uint64_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_ptr_u32(80) == 2200)
    assert(trace_count() > 0, "shared uint32_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_ptr_u64arg(80) == 80 * 22)
    assert(trace_count() > 0, "shared uint64_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_ptr_ptr_u64(80) == 80 * 33)
    assert(trace_count() > 0, "shared ptr,uint64_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_ptr_ptr_i64(80) == 80 * 33)
    assert(trace_count() > 0, "shared ptr,int64_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_i32_i64(7, arg)[0]
      end
      return r
    end)(80) == 80 * 22)
    assert(trace_count() > 0, "shared int,int64_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_i32_u64(7, arg)[0]
      end
      return r
    end)(80) == 80 * 22)
    assert(trace_count() > 0, "shared int,uint64_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_u32_i64(a, b)[0]
      end
      return r
    end)(80) == 80 * 22)
    assert(trace_count() > 0, "shared uint32_t,int64_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_u32_u64(a, b)[0]
      end
      return r
    end)(80) == 80 * 22)
    assert(trace_count() > 0, "shared uint32_t,uint64_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_num0(80) == 120)
    assert(trace_count() > 0, "shared void->double FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_num_i32(80) == (80 * 81) / 2 + 60)
    assert(trace_count() > 0, "shared int->double FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local r = 0
      for i = 1, n do
	r = r + lib.lj_m7_ccall_jit_num_num_i32(i + 0.25, 7)
      end
      return r
    end)(80) == (80 * 81) / 2 + 80 * (7 + 0.25 + 0.375))
    assert(trace_count() > 0, "shared double,int->double FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_num_ptr(80) == 900)
    assert(trace_count() > 0, "shared ptr->double FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_num_flt(80) == (80 * 81) / 2 + 30)
    assert(trace_count() > 0, "shared float->double FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_num1(80) == (80 * 81) / 2 + 40)
    assert(trace_count() > 0, "shared double->double FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_num2(80) == (80 * 81) / 2 + 40)
    assert(trace_count() > 0, "shared double,double->double FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_flt0(80) == 120)
    assert(trace_count() > 0, "shared void->float FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_flt1(80) == (80 * 81) / 2 + 40)
    assert(trace_count() > 0, "shared float->float FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_flt2(80) == (80 * 81) / 2 + 40)
    assert(trace_count() > 0, "shared float,float->float FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i32_num(80) == (80 * 81) / 2 + 240)
    assert(trace_count() > 0, "shared double->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i32_flt(80) == (80 * 81) / 2 + 320)
    assert(trace_count() > 0, "shared float->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_void0(80) == 80)
    assert(trace_count() > 0, "shared void->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_void_num(80) == (80 * 81) / 2)
    assert(trace_count() > 0, "shared double->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_void_flt(80) == (80 * 81) / 2)
    assert(trace_count() > 0, "shared float->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_flt_num(80) == (80 * 81) / 2 + 60)
    assert(trace_count() > 0, "shared double->float FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_void_store(80) == 89)
    assert(trace_count() > 0, "shared ptr,int->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u32(80) == 80 * 8)
    assert(trace_count() > 0, "shared uint32_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u32_high(80) == 80 * 0xf0000001)
    assert(trace_count() > 0, "shared high-bit uint32_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u32_i32(80) == 80 * 0xf0000000 + (80 * 81) / 2)
    assert(trace_count() > 0, "shared int->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u32_ptr(80) == 80 * (0xf0000000 + 11))
    assert(trace_count() > 0, "shared ptr->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u32_i32_ptr(80) == 80 * 0xf0000000 + 80 * 27 + 40)
    assert(trace_count() > 0, "shared int,ptr->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u32_0(80) == 80 * 0xf0000001)
    assert(trace_count() > 0, "shared void->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64(80))
    assert(trace_count() > 0, "shared uint64_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64_u64_u64(80))
    assert(trace_count() > 0, "shared uint64_t,uint64_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64_i64(80))
    assert(trace_count() > 0, "shared int64_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64_i64_u64(80))
    assert(trace_count() > 0, "shared int64_t,uint64_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64_u64_i64(80))
    assert(trace_count() > 0, "shared uint64_t,int64_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64_i32(80) == 80 * 4294967296 + (80 * 81) / 2)
    assert(trace_count() > 0, "shared int->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64_ptr(80) == 80 * (4294967296 + 11))
    assert(trace_count() > 0, "shared ptr->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64_i32_ptr(80) == 80 * 4294967296 + 80 * 27 + 40)
    assert(trace_count() > 0, "shared int,ptr->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i64_ptr_u64(80) == 80 * (4294967296 + 1043))
    assert(trace_count() > 0, "shared ptr,uint64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64_ptr_u64(80) == 80 * (4294967296 + 1043))
    assert(trace_count() > 0, "shared ptr,uint64_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i64_ptr_i64(80) == 80 * (4294967296 + 1043))
    assert(trace_count() > 0, "shared ptr,int64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64_ptr_i64(80) == 80 * (4294967296 + 1043))
    assert(trace_count() > 0, "shared ptr,int64_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_i64_i32_i64(7, arg))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared int,int64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_u64_i32_i64(7, arg))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared int,int64_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_i64_i32_u64(7, arg))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared int,uint64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local arg = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_u64_i32_u64(7, arg))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared int,uint64_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_i64_u32_i64(a, b))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared uint32_t,int64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_i64_u32_u64(a, b))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared uint32_t,uint64_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_u64_u32_i64(a, b))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared uint32_t,int64_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint32_t", 7)
      local b = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_u64_u32_u64(a, b))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared uint32_t,uint64_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_i64_i32(a, 7)
      end
      return r
    end)(80) == 80 * 249)
    assert(trace_count() > 0, "shared int64_t,int->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_i64_u32(a, b)
      end
      return r
    end)(80) == 80 * 249)
    assert(trace_count() > 0, "shared int64_t,uint32_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_i64_i32(a, 7) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 9)
    assert(trace_count() > 0, "shared int64_t,int->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_i64_u32(a, b) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 9)
    assert(trace_count() > 0, "shared int64_t,uint32_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_i64_i32(a, 7)
      end
      return r
    end)(80) == 80 * 1017)
    assert(trace_count() > 0, "shared int64_t,int->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_i64_u32(a, b)
      end
      return r
    end)(80) == 80 * 1017)
    assert(trace_count() > 0, "shared int64_t,uint32_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_i64_i32(a, 7)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1017))
    assert(trace_count() > 0, "shared int64_t,int->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_i64_u32(a, b)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1017))
    assert(trace_count() > 0, "shared int64_t,uint32_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_i64_i32(a, 7)[0]
      end
      return r
    end)(80) == 80 * 22)
    assert(trace_count() > 0, "shared int64_t,int->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_i64_u32(a, b)[0]
      end
      return r
    end)(80) == 80 * 22)
    assert(trace_count() > 0, "shared int64_t,uint32_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_i64_i64_i32(a, 7))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared int64_t,int->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_i64_i64_u32(a, b))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared int64_t,uint32_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_u64_i64_i32(a, 7))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared int64_t,int->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_u64_i64_u32(a, b))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared int64_t,uint32_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_u64_i32(a, 7)
      end
      return r
    end)(80) == 80 * 249)
    assert(trace_count() > 0, "shared uint64_t,int->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_u64_u32(a, b)
      end
      return r
    end)(80) == 80 * 249)
    assert(trace_count() > 0, "shared uint64_t,uint32_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_u64_i32(a, 7) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 9)
    assert(trace_count() > 0, "shared uint64_t,int->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_u64_u32(a, b) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 9)
    assert(trace_count() > 0, "shared uint64_t,uint32_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_u64_i32(a, 7)
      end
      return r
    end)(80) == 80 * 1017)
    assert(trace_count() > 0, "shared uint64_t,int->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_u64_u32(a, b)
      end
      return r
    end)(80) == 80 * 1017)
    assert(trace_count() > 0, "shared uint64_t,uint32_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_u64_i32(a, 7)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1017))
    assert(trace_count() > 0, "shared uint64_t,int->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_u64_u32(a, b)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1017))
    assert(trace_count() > 0, "shared uint64_t,uint32_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_u64_i32(a, 7)[0]
      end
      return r
    end)(80) == 80 * 22)
    assert(trace_count() > 0, "shared uint64_t,int->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_u64_u32(a, b)[0]
      end
      return r
    end)(80) == 80 * 22)
    assert(trace_count() > 0, "shared uint64_t,uint32_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_i64_u64_i32(a, 7))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared uint64_t,int->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_i64_u64_u32(a, b))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared uint64_t,uint32_t->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_u64_u64_i32(a, 7))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared uint64_t,int->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("uint32_t", 7)
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_u64_u64_u32(a, b))
      end
      return r
    end)(80) == 80 * (4294967296 + 1017))
    assert(trace_count() > 0, "shared uint64_t,uint32_t->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_i64_ptr(a, p)
      end
      return r
    end)(80) == 80 * 253)
    assert(trace_count() > 0, "shared int64_t,ptr->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_u64_ptr(a, p)
      end
      return r
    end)(80) == 80 * 253)
    assert(trace_count() > 0, "shared uint64_t,ptr->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local p = ptr0()
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_i64_ptr(a, p) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 13)
    assert(trace_count() > 0, "shared int64_t,ptr->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local p = ptr0()
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_u64_ptr(a, p) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 13)
    assert(trace_count() > 0, "shared uint64_t,ptr->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_i64_ptr(a, p)
      end
      return r
    end)(80) == 80 * 1021)
    assert(trace_count() > 0, "shared int64_t,ptr->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_u64_ptr(a, p)
      end
      return r
    end)(80) == 80 * 1021)
    assert(trace_count() > 0, "shared uint64_t,ptr->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_i64_ptr(a, p)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1021))
    assert(trace_count() > 0, "shared int64_t,ptr->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_u64_ptr(a, p)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1021))
    assert(trace_count() > 0, "shared uint64_t,ptr->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_i64_ptr(a, p)[0]
      end
      return r
    end)(80) == 80 * 22)
    assert(trace_count() > 0, "shared int64_t,ptr->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_u64_ptr(a, p)[0]
      end
      return r
    end)(80) == 80 * 22)
    assert(trace_count() > 0, "shared uint64_t,ptr->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_i64_i64_ptr(a, p))
      end
      return r
    end)(80) == 80 * (4294967296 + 1021))
    assert(trace_count() > 0, "shared int64_t,ptr->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_i64_u64_ptr(a, p))
      end
      return r
    end)(80) == 80 * (4294967296 + 1021))
    assert(trace_count() > 0, "shared uint64_t,ptr->int64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_u64_i64_ptr(a, p))
      end
      return r
    end)(80) == 80 * (4294967296 + 1021))
    assert(trace_count() > 0, "shared int64_t,ptr->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local p = ptr0()
      local r = 0
      for _ = 1, n do
	r = r + tonumber(lib.lj_m7_ccall_jit_u64_u64_ptr(a, p))
      end
      return r
    end)(80) == 80 * (4294967296 + 1021))
    assert(trace_count() > 0, "shared uint64_t,ptr->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("int64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_i64_i64(a, b)
      end
      return r
    end)(80) == 80 * 251)
    assert(trace_count() > 0, "shared int64_t,int64_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("uint64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_i64_u64(a, b)
      end
      return r
    end)(80) == 80 * 251)
    assert(trace_count() > 0, "shared int64_t,uint64_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("int64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_u64_i64(a, b)
      end
      return r
    end)(80) == 80 * 251)
    assert(trace_count() > 0, "shared uint64_t,int64_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("uint64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u8_u64_u64(a, b)
      end
      return r
    end)(80) == 80 * 251)
    assert(trace_count() > 0, "shared uint64_t,uint64_t->uint8_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("int64_t", 9)
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_i64_i64(a, b) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 11)
    assert(trace_count() > 0, "shared int64_t,int64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("uint64_t", 9)
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_i64_u64(a, b) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 11)
    assert(trace_count() > 0, "shared int64_t,uint64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("int64_t", 9)
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_u64_i64(a, b) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 11)
    assert(trace_count() > 0, "shared uint64_t,int64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("uint64_t", 9)
      local before = void_count_i32()
      for _ = 1, n do
	assert(lib.lj_m7_ccall_jit_void_u64_u64(a, b) == nil)
      end
      return void_count_i32() - before
    end)(80) == 80 * 11)
    assert(trace_count() > 0, "shared uint64_t,uint64_t->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("int64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_i64_i64(a, b)
      end
      return r
    end)(80) == 80 * 1019)
    assert(trace_count() > 0, "shared int64_t,int64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("uint64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_i64_u64(a, b)
      end
      return r
    end)(80) == 80 * 1019)
    assert(trace_count() > 0, "shared int64_t,uint64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("int64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_u64_i64(a, b)
      end
      return r
    end)(80) == 80 * 1019)
    assert(trace_count() > 0, "shared uint64_t,int64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("uint64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_i32_u64_u64(a, b)
      end
      return r
    end)(80) == 80 * 1019)
    assert(trace_count() > 0, "shared uint64_t,uint64_t->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("int64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_i64_i64(a, b)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1019))
    assert(trace_count() > 0, "shared int64_t,int64_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("uint64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_i64_u64(a, b)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1019))
    assert(trace_count() > 0, "shared int64_t,uint64_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("int64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_u64_i64(a, b)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1019))
    assert(trace_count() > 0, "shared uint64_t,int64_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("uint64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_u32_u64_u64(a, b)
      end
      return r
    end)(80) == 80 * (0xf0000000 + 1019))
    assert(trace_count() > 0, "shared uint64_t,uint64_t->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("int64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_i64_i64(a, b)[0]
      end
      return r
    end)(80) == 80 * 44)
    assert(trace_count() > 0, "shared int64_t,int64_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("int64_t", -14)
      local b = ffi.new("uint64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_i64_u64(a, b)[0]
      end
      return r
    end)(80) == 80 * 44)
    assert(trace_count() > 0, "shared int64_t,uint64_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("int64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_u64_i64(a, b)[0]
      end
      return r
    end)(80) == 80 * 44)
    assert(trace_count() > 0, "shared uint64_t,int64_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert((function(n)
      local a = ffi.new("uint64_t", -14)
      local b = ffi.new("uint64_t", 9)
      local r = 0
      for _ = 1, n do
	r = r + lib.lj_m7_ccall_jit_ptr_u64_u64(a, b)[0]
      end
      return r
    end)(80) == 80 * 44)
    assert(trace_count() > 0, "shared uint64_t,uint64_t->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64_0(80) == ffi.new("uint64_t", -1))
    assert(trace_count() > 0, "shared void->uint64_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_ptr0(80) == 33)
    assert(trace_count() > 0, "shared void->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_ptr_read(80) == 80 * 27 + 40)
    assert(trace_count() > 0, "shared ptr->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_ptr_sum(80) == 80 * 33)
    assert(trace_count() > 0, "shared ptr,ptr->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_i32_ptr_read(80) == 80 * 27 + 40)
    assert(trace_count() > 0, "shared int,ptr->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_ptr_add(80) == 80 * 27 + 40)
    assert(trace_count() > 0, "shared ptr,int->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_ptr_num(80) == 80 * 27 + 40)
    assert(trace_count() > 0, "shared double->ptr FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_ptr_ulong_i32(80) == 80 * 1023 + 80 * 27 + 40)
    assert(trace_count() > 0, "shared ptr,unsigned long,int->int FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_sleep(80, 0) == 7)
    assert(trace_count() > 0, "shared int->int FFI call loop should trace")

    local ready = th.channel(1)
    local done = th.channel(1)
    local sleep_ms = 350
    local worker = th.spawn(function(ready_ch, done_ch, so_path, timeout_ms)
      local ffi = require"ffi"
      ffi.cdef"int lj_m7_ccall_jit_sleep_i32(int);"
      local sleep_i32 = ffi.load(so_path).lj_m7_ccall_jit_sleep_i32
      local function run_sleep(n, ms)
	local r = 0
	for _ = 1, n do
	  r = sleep_i32(ms)
	end
	return r
      end
      jit.flush()
      jit.opt.start("hotloop=1", "hotexit=1")
      assert(run_sleep(80, 0) == 7)
      ready_ch:send("sleeping")
      assert(run_sleep(2, timeout_ms) == timeout_ms + 7)
      done_ch:send("done")
      return true
    end, ready, done, so, sleep_ms)

    local token, ok = ready:recv(10)
    assert(ok == true and token == "sleeping")
    th.sleep(0.05)
    local early, why = done:recv(0)
    assert(early == nil and why == "timeout",
	   "traced FFI worker finished before GC probe")

    local t0 = now()
    collectgarbage("collect")
    local elapsed = now() - t0
    assert(elapsed < 0.5,
	   ("collectgarbage blocked on traced FFI native call for %.3fs"):
	   format(elapsed))

    local msg, done_ok = done:recv(2)
    assert(done_ok == true and msg == "done")
    local joined, result = worker:join(2)
    assert(joined == true and result == true)
  end
end

do
  local ready = th.channel(1)
  local done = th.channel(1)
  local sleep_ms = 800
  local worker = th.spawn(function(ready_ch, done_ch, timeout_ms)
    local ffi = require"ffi"
    local poll = ffi.C.poll
    local function call_poll(timeout)
      return poll(nil, 0, timeout)
    end
    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    for _ = 1, 120 do
      assert(call_poll(0) == 0)
    end
    ready_ch:send("sleeping")
    assert(call_poll(timeout_ms) == 0)
    done_ch:send("done")
    return true
  end, ready, done, sleep_ms)

  local token, ok = ready:recv(10)
  assert(ok == true and token == "sleeping")
  th.sleep(0.05)
  local early, why = done:recv(0)
  assert(early == nil and why == "timeout",
         "blocking FFI worker finished before GC probe")

  local t0 = now()
  collectgarbage("collect")
  local elapsed = now() - t0
  assert(elapsed < 0.5,
         ("collectgarbage blocked on ordinary FFI native call for %.3fs"):
	 format(elapsed))

  local msg, done_ok = done:recv(2)
  assert(done_ok == true and msg == "done")
  local joined, result = worker:join(2)
  assert(joined == true and result == true)
end

print("t-ffi-ccall-native OK")
