local ffi = require"ffi"
local th = require"threading"
local trace_count = require"jit_harness".trace_count

ffi.cdef[[
int abs(int);
int getpid(void);
int poll(void *fds, unsigned long nfds, int timeout);
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
assert(trace_count() == 0, "ordinary FFI call loop must stay off trace")

assert(ffi.blocking(abs) == abs)
assert(trace_count() == 0, "ffi.blocking must keep FFI calls off trace")
assert(run_abs(100) == 5050)
assert(trace_count() == 0, "ffi.blocking function must stay off trace")

assert(ffi.blocking(abs)(-9) == 9)
assert(pcall(ffi.blocking, ffi.new("int[1]")) == false)
assert(pcall(ffi.blocking, function() end) == false)

local getpid = ffi.blocking(ffi.C.getpid)
assert(getpid() > 0)

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

print("t-ffi-blocking OK")
