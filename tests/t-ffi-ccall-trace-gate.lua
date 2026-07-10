local ffi = require "ffi"
local util = require "jit.util"

ffi.cdef [[
int abs(int);
]]

local function trace_count()
  local n = 0
  for tr = 1, 128 do
    if util.traceinfo(tr) then n = n + 1 end
  end
  return n
end
jit.off(trace_count, true)

local function run(n)
  local sum = 0
  for i = 1, n do
    sum = sum + ffi.C.abs(-i)
  end
  return sum
end

jit.on()
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")

assert(run(400) == 400 * 401 / 2)
assert(trace_count() == 0,
       "ordinary FFI C calls must stay interpreted until XSAVE publication")

print("t-ffi-ccall-trace-gate OK: the generic CALLXS seam stayed gated")
