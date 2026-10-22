-- Full-recorder reproducer for PHI spill copies crossing the loop-body entry.
-- The current ARM64 production policy rejects NUM spills, so use the isolated
-- generated-code contract until capability admission covers this trace.
local jit = require('jit')
local util = require('jit.util')
if jit.arch ~= 'arm64' then
  print('t-arm64-jit-phi-pressure SKIP: requires ARM64')
  return
end
jit.off()
local names, initial, body = {}, {}, {}
for i = 1, 36 do
  names[i] = 'x' .. i
  initial[i] = ('a+%.3f'):format(i / 100)
  body[i] = ('x%d=x%d*1.0001+0.125;'):format(i, i)
end
local source = 'return function(n,a) local ' .. table.concat(names, ',') ..
  '=' .. table.concat(initial, ',') .. '; local i=0; while i<n do ' ..
  table.concat(body, ' ') .. ' i=i+1; end return ' ..
  table.concat(names, ',') .. ' end'
local f = assert(loadstring(source, 'arm64-phi-pressure'))()
local expected = { f(20, 0.5) }
jit.on()
jit.flush()
jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=1', '-fma')
local actual = { f(20, 0.5) }
local info = util.traceinfo(1)
if not info then
  print('t-arm64-jit-phi-pressure SKIP: NUM-spill trace admission is closed')
  return
end
assert(info.link == 1 and info.linktype == 'loop')
jit.off()
for i = 1, #expected do
  assert(actual[i] == expected[i], ('warm accumulator %d: %.17g ~= %.17g'):
    format(i, actual[i], expected[i]))
end
jit.on()
actual = { f(20, 0.5) }
jit.off()
for i = 1, #expected do
  assert(actual[i] == expected[i], ('native accumulator %d: %.17g ~= %.17g'):
    format(i, actual[i], expected[i]))
end
print('t-arm64-jit-phi-pressure OK: 36 live numeric recurrences')
