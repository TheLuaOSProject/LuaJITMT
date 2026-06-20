local ffi = require"ffi"
local trace_count = require"jit_harness".trace_count

ffi.cdef[[
int abs(int);
int getpid(void);
]]

local abs = ffi.C.abs

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
assert(trace_count() > 0, "baseline FFI call loop should record")

assert(ffi.blocking(abs) == abs)
assert(trace_count() == 0, "ffi.blocking must flush existing traces")
assert(run_abs(100) == 5050)
assert(trace_count() == 0, "ffi.blocking function must stay off trace")

assert(ffi.blocking(abs)(-9) == 9)
assert(pcall(ffi.blocking, ffi.new("int[1]")) == false)
assert(pcall(ffi.blocking, function() end) == false)

local getpid = ffi.blocking(ffi.C.getpid)
assert(getpid() > 0)

print("t-ffi-blocking OK")
