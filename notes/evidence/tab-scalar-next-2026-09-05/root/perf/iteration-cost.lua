local workload = assert(arg[1])
local rounds = assert(tonumber(arg[2]))
local jit, util = require('jit'), require('jit.util')
local t = {}
for i = 1, 32 do t[i] = i end
local function ordinary_next(n)
  local sum = 0
  for _ = 1, n do
    local key = nil
    while true do
      local value
      key, value = next(t, key)
      if key == nil then break end
      sum = sum + value
    end
  end
  return sum
end
local function itern(n)
  local sum = 0
  for _ = 1, n do
    for _, value in next, t do sum = sum + value end
  end
  return sum
end
local fn = workload == 'next' and ordinary_next or itern
assert(workload == 'next' or workload == 'itern')
if jit.status() then jit.opt.start('hotloop=1', 'hotexit=1') end
assert(fn(200) == 200 * 528)
collectgarbage('collect')
for sample = 1, 5 do
  local start = os.clock()
  local sum = fn(rounds)
  local seconds = os.clock() - start
  assert(sum == rounds * 528)
  io.write(string.format('{"sample":%d,"visits":%d,"seconds":%.9f,"ns":%.9f,"checksum":%.0f}\n',
    sample, rounds * 32, seconds, seconds * 1e9 / (rounds * 32), sum))
end
local traces = 0
for i = 1, 1000 do if util.traceinfo(i) then traces = traces + 1 end end
io.write(string.format('{"jit_enabled":%s,"trace_count":%d}\n',tostring(jit.status()),traces))
