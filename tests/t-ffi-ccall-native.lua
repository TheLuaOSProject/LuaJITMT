local ffi = require"ffi"
local th = require"threading"
local trace_count = require"jit_harness".trace_count

ffi.cdef[[
int abs(int);
int getpid(void);
int poll(void *fds, unsigned long nfds, int timeout);
int lj_m7_ccall_jit_sleep_i32(int);
int lj_m7_ccall_jit_add2_i32(int, int);
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
    local function run_add2(n)
      local r = 0
      for i = 1, n do
	r = r + add2_i32(i, 2)
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
