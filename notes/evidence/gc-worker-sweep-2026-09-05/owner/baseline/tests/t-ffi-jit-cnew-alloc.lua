local th = require"threading"
local ffi = require"ffi"

ffi.cdef[[
typedef struct { int x; double y; } lj_m7_jit_cnew_alloc_t;
]]

local worker = th.spawn(function()
  local ffi = require"ffi"
  local trace_count = require"jit_harness".trace_count

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1", "-sink")

  local struct_t = ffi.typeof("lj_m7_jit_cnew_alloc_t")
  local int64_t = ffi.typeof("int64_t")

  local function make(n)
    local sum = 0
    for i = 1, n do
      local obj = struct_t(i, i + 0.25)
      local i64 = int64_t(i)
      sum = sum + obj.x + tonumber(i64)
    end
    return sum
  end

  for _ = 1, 30 do
    assert(make(80) == 6480)
  end

  local traces = trace_count(64)
  assert(traces > 0)
  collectgarbage("collect")
  collectgarbage("collect")
  return traces
end)

local ok, traces = worker:join(30)
assert(ok == true, tostring(traces))
assert(type(traces) == "number" and traces > 0)

print(("t-ffi-jit-cnew-alloc OK: %d traces"):format(traces))
