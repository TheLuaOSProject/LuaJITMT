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
double lj_m7_ccall_jit_num0(void);
double lj_m7_ccall_jit_num1(double);
double lj_m7_ccall_jit_num2(double, double);
float lj_m7_ccall_jit_flt0(void);
float lj_m7_ccall_jit_flt1(float);
float lj_m7_ccall_jit_flt2(float, float);
int lj_m7_ccall_jit_void_count_i32(void);
void lj_m7_ccall_jit_void0(void);
void lj_m7_ccall_jit_store_i32(int *, int);
unsigned int lj_m7_ccall_jit_u32(unsigned int);
uint32_t lj_m7_ccall_jit_u32_0(void);
uint64_t lj_m7_ccall_jit_u64(uint64_t);
uint64_t lj_m7_ccall_jit_u64_0(void);
int *lj_m7_ccall_jit_ptr0(void);
int lj_m7_ccall_jit_ptr_read_i32(int *);
int lj_m7_ccall_jit_ptr_sum_i32(int *, int *);
int lj_m7_ccall_jit_i32_ptr_read_i32(int, int *);
int *lj_m7_ccall_jit_ptr_add_i32(int *, int);
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
  assert(trace_count() == 0, "unsupported multi-arg FFI call must stay off trace")
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

  expect_no_trace("dynamic-length ffi.copy", function(n)
    for i = 1, n do
      ffi.copy(dst, src, (i % 64) + 1)
    end
  end)

  expect_no_trace("dynamic-length ffi.fill", function(n)
    for i = 1, n do
      ffi.fill(dst, (i % 64) + 1, i)
    end
  end)

  expect_no_trace("unbounded ffi.string", function(n)
    local total = 0
    for _ = 1, n do
      total = total + #ffi.string(cstr)
    end
    assert(total == n * 6)
  end)

  expect_no_trace("dynamic-length ffi.string", function(n)
    local total = 0
    for i = 1, n do
      total = total + #ffi.string(cstr, (i % 6) + 1)
    end
    assert(total > n)
  end)
end

do
  local so = os.getenv("LJ_M7_FFI_CCALL_JIT_SO")
  if so then
    local lib = ffi.load(so)
    local sleep_i32 = lib.lj_m7_ccall_jit_sleep_i32
    local add2_i32 = lib.lj_m7_ccall_jit_add2_i32
    local i64_0 = lib.lj_m7_ccall_jit_i64_0
    local num0 = lib.lj_m7_ccall_jit_num0
    local num1 = lib.lj_m7_ccall_jit_num1
    local num2 = lib.lj_m7_ccall_jit_num2
    local flt0 = lib.lj_m7_ccall_jit_flt0
    local flt1 = lib.lj_m7_ccall_jit_flt1
    local flt2 = lib.lj_m7_ccall_jit_flt2
    local void_count_i32 = lib.lj_m7_ccall_jit_void_count_i32
    local void0 = lib.lj_m7_ccall_jit_void0
    local store_i32 = lib.lj_m7_ccall_jit_store_i32
    local u32 = lib.lj_m7_ccall_jit_u32
    local u32_0 = lib.lj_m7_ccall_jit_u32_0
    local u64 = lib.lj_m7_ccall_jit_u64
    local u64_0 = lib.lj_m7_ccall_jit_u64_0
    local ptr0 = lib.lj_m7_ccall_jit_ptr0
    local ptr_read_i32 = lib.lj_m7_ccall_jit_ptr_read_i32
    local ptr_sum_i32 = lib.lj_m7_ccall_jit_ptr_sum_i32
    local i32_ptr_read_i32 = lib.lj_m7_ccall_jit_i32_ptr_read_i32
    local ptr_add_i32 = lib.lj_m7_ccall_jit_ptr_add_i32
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
    local function run_num0(n)
      local r = 0
      for _ = 1, n do
	r = r + num0()
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
    local function run_void0(n)
      local before = void_count_i32()
      for _ = 1, n do
	assert(void0() == nil)
      end
      return void_count_i32() - before
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
    assert(run_num0(80) == 120)
    assert(trace_count() > 0, "shared void->double FFI call loop should trace")

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
    assert(run_void0(80) == 80)
    assert(trace_count() > 0, "shared void->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_void_store(80) == 89)
    assert(trace_count() > 0, "shared ptr,int->void FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u32(80) == 80 * 8)
    assert(trace_count() == 0, "unsigned int FFI calls must keep stock semantics off trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u32_0(80) == 80 * 0xf0000001)
    assert(trace_count() > 0, "shared void->uint32_t FFI call loop should trace")

    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    assert(run_u64(80))
    assert(trace_count() == 0, "uint64_t FFI calls with arguments must stay off trace")

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
