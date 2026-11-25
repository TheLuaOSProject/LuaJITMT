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
  local new, tonumber = ffi.new, tonumber

  local function make(n)
    local sum, last = 0
    for i = 1, n do
      last = new(struct_t, i, i + 0.25)
      sum = sum + tonumber(new(int64_t, i))
    end
    return sum, last
  end

  local sum, last
  for _ = 1, 30 do
    sum, last = make(80)
    assert(sum == 3240 and last.x == 80 and last.y == 80.25)
  end

  local traces = trace_count(64)
  assert(traces > 0)
  collectgarbage("collect")
  collectgarbage("collect")
  assert(last.x == 80 and last.y == 80.25)
  return traces
end)

local ok, traces = worker:join(30)
assert(ok == true, tostring(traces))
assert(type(traces) == "number" and traces > 0)

print(("t-ffi-jit-cnew-alloc OK: %d traces"):format(traces))
