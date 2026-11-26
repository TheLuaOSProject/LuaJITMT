local buffer = require("string.buffer")
local ffi = require("ffi")
local util = require("jit.util")
ffi.cdef[[
struct timespec { long tv_sec; long tv_nsec; };
int clock_gettime(int clock_id, struct timespec *time);
]]
local stamp = ffi.new("struct timespec[1]")
local function wallclock()
  assert(ffi.C.clock_gettime(1, stamp) == 0)
  return tonumber(stamp[0].tv_sec) + tonumber(stamp[0].tv_nsec)*1e-9
end
local variant, sample, order = arg[1], tonumber(arg[2]), tonumber(arg[3])
assert(jit.status())
jit.opt.start("hotloop=20", "hotexit=10")
local function append(b, payload, count)
  for _ = 1, count do b:put(payload) end
end
local function traces()
  local n = 0
  for tr = 1, 10000 do if util.traceinfo(tr) then n = n+1 end end
  return n
end
jit.off(traces)
for _, size in ipairs({1, 16}) do
  local count, streams = 1000000, 10
  local b, payload = buffer.new(), ("a"):rep(size)
  local expected = payload:rep(count)
  jit.flush()
  for _ = 1, 2 do
    b:reset()
    append(b, payload, count)
    assert(#b == #expected and b:get() == expected and #b == 0)
  end
  collectgarbage()
  local cpu, wall = 0, 0
  for _ = 1, streams do
    b:reset()
    local wallstart, cpustart = wallclock(), os.clock()
    append(b, payload, count)
    cpu = cpu + os.clock()-cpustart
    wall = wall + wallclock()-wallstart
    assert(#b == #expected and b:get() == expected and #b == 0)
  end
  local ops = count*streams
  io.write(string.format("%s,jit,small_append_long,%d,%d,%d,%d,%d,%d,%d,%.9f,%.9f,%d,%d\n",
      variant,sample,order,count,streams,size,ops,ops*size,cpu,wall,ops*size,traces()))
  io.flush()
end
