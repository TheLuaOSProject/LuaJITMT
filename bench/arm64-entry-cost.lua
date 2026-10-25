-- Entry-dominated and sustained scalar loops. Run with the same build/profile.
-- Usage: luajit bench/arm64-entry-cost.lua on|off iterations calls [initial_sum] [hotexit]
local jit = require('jit')
local util = require('jit.util')
local mode = assert(arg[1])
local n = assert(tonumber(arg[2]))
local calls = assert(tonumber(arg[3]))
local initial = tonumber(arg[4] or '0.5')
local hotexit = assert(tonumber(arg[5] or '1000'))
assert((mode == 'on' or mode == 'off') and initial)
assert(n >= 1 and n % 1 == 0 and calls >= 1 and calls % 1 == 0)
assert(hotexit >= 1 and hotexit <= 2147483647 and hotexit % 1 == 0)

-- A literal preserves the INT-versus-NUM specialization being measured.
local kernel = assert(loadstring(string.format([[
return function(a, count)
  local i, sum = 1, %.17g
  while i <= count do
    sum = sum + a
    i = i + 1
  end
  return sum
end
]], initial)))()
local function driver()
  local total = 0
  for j = 1, calls do total = total + kernel(1.25, n) end
  assert(total == calls * (n * 1.25 + initial))
end
jit.off(driver)
jit.flush()
jit.opt.start('hotloop=1', 'hotexit=' .. hotexit)
if mode == 'off' then jit.off(kernel) end

local exits = 0
local function onexit() exits = exits + 1 end
jit.attach(onexit, 'texit')
for k = 1, 20 do assert(kernel(1.25, n) == n * 1.25 + initial) end
jit.attach(onexit)
local traces = 0
for k = 1, 100 do if util.traceinfo(k) then traces = traces + 1 end end
if mode == 'on' then assert(traces > 0 and exits > 0) end
collectgarbage('collect')

local samples = {}
for k = 1, 5 do
  local start = os.clock()
  driver()
  samples[k] = os.clock() - start
end
table.sort(samples)
print(string.format(
  'mode=%s n=%d calls=%d initial=%g hotexit=%d median_cpu_s=%.6f min=%.6f max=%.6f traces=%d warmup_exits=%d',
  mode, n, calls, initial, hotexit, samples[3], samples[1], samples[5], traces, exits))
