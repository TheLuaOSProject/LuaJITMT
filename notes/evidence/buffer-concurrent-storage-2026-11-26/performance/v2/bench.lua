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
  return tonumber(stamp[0].tv_sec) + tonumber(stamp[0].tv_nsec) * 1e-9
end

local variant, sample, order = arg[1], tonumber(arg[2]), tonumber(arg[3])
local enabled = jit.status()
local mode = enabled and "jit" or "joff"
if enabled then jit.opt.start("hotloop=20", "hotexit=10") end

local function traces()
  local n = 0
  for tr = 1, 10000 do
    if util.traceinfo(tr) then n = n + 1 end
  end
  return n
end
jit.off(traces)

local cases = {}
local function add(name, size, count, streams, prepare, run, verify)
  cases[#cases+1] = {
    name=name, size=size, count=count, streams=streams,
    prepare=prepare, run=run, verify=verify,
  }
end

for _, row in ipairs({{64, 100000}, {1024, 20000}}) do
  local size, count = row[1], row[2]
  local b, payload = buffer.new(), ("Q"):rep(size)
  local function run()
    for _ = 1, count do
      b:reset():put(payload)
      assert(b:get() == payload)
    end
    return count * size
  end
  add("reset_put_get", size, count, 1, function() b:reset() end,
      run, function(sum) assert(sum == count*size and #b == 0) end)
end

for _, row in ipairs({{1, 200000}, {16, 50000}}) do
  local size, count = row[1], row[2]
  local b, payload = buffer.new(), ("a"):rep(size)
  local expected = payload:rep(count)
  local function run()
    for _ = 1, count do b:put(payload) end
    return #b
  end
  add("small_append", size, count, 1, function() b:reset() end,
      run, function(sum)
        assert(sum == #expected and b:get() == expected and #b == 0)
      end)
end

for _, row in ipairs({{1, 200000}, {32, 100000}}) do
  local size, count = row[1], row[2]
  local b, payload = buffer.new(), ("g"):rep(size)
  local input = payload:rep(count)
  local function run()
    for _ = 1, count do assert(b:get(size) == payload) end
    return #b
  end
  add("fifo_get", size, count, 1, function() b:reset():put(input) end,
      run, function(left) assert(left == 0 and #b == 0) end)
end

for _, row in ipairs({{64, 100000}, {1024, 20000}}) do
  local size, count = row[1], row[2]
  local b, payload = buffer.new(), ("s"):rep(size)
  local function run()
    for _ = 1, count do
      b:set(payload)
      assert(b:get() == payload)
    end
    return count * size
  end
  add("set_get", size, count, 1, function() b:reset() end,
      run, function(sum) assert(sum == count*size and #b == 0) end)
end

local encoded = buffer.encode(41)
assert(#encoded == 9 and buffer.decode(encoded) == 41)
for _, count in ipairs({1000, 10000, 100000}) do
  local b, streams = buffer.new(), 200000/count
  local input = encoded:rep(count)
  local function run()
    local sum = 0
    for _ = 1, streams do
      b:set(input)
      for _ = 1, count do
        local value = b:decode()
        assert(value == 41)
        sum = sum + value
      end
      assert(#b == 0)
    end
    return sum
  end
  add("stream_decode", #encoded, count, streams, function() b:reset() end,
      run, function(sum) assert(sum == 41*count*streams and #b == 0) end)
end

for _, c in ipairs(cases) do
  jit.flush()
  for _ = 1, 2 do
    c.prepare()
    c.verify(c.run())
  end
  collectgarbage()
  c.prepare()
  local wallstart, cpustart = wallclock(), os.clock()
  local checksum = c.run()
  local cpu, wall = os.clock()-cpustart, wallclock()-wallstart
  c.verify(checksum)
  local ops = c.count*c.streams
  io.write(string.format("%s,%s,%s,%d,%d,%d,%d,%d,%d,%d,%.9f,%.9f,%g,%d\n",
      variant, mode, c.name, sample, order, c.count, c.streams, c.size,
      ops, ops*c.size, cpu, wall, checksum, traces()))
  io.flush()
end
