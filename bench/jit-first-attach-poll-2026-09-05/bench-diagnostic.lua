local jit = require('jit')
local util = require('jit.util')
local case = assert(arg[1])
local work, input, n, expected
local exits = 0
local function witness() exits = exits + 1 end
jit.off(witness)
jit.opt.start('hotloop=1', 'hotexit=255')
jit.attach(witness, 'texit')

if case == 'numeric' then
  n = 500000000
  work = function(count)
    local s = 0
    for i = 1, count do s = s + 1 end
    return s
  end
  expected = function(count) return count end
elseif case == 'ffi_struct' then
  input = require('ffi').new('struct { double x; double y; }', 0, 1)
  n = 50000000
  work = function(count, p)
    p.x = 0
    local s = 0
    for i = 1, count do p.x = p.x + 1; s = p.x + p.y end
    return s
  end
  expected = function(count) return count + 1 end
elseif case == 'table_read' then
  input = { 1, 2 }
  n = 100000000
  work = function(count, p)
    local s = 0
    for i = 1, count do s = s + p[1] + p[2] end
    return s
  end
  expected = function(count) return count * 3 end
else
  error('unknown case')
end

assert(work(1000, input) == expected(1000))
assert(exits > 0 and util.traceinfo(1), 'real native warm witness')
do
  local bit = require('bit')
  local names = require('jit.vmdef').irnames
  local info = assert(util.traceinfo(1))
  local polls = 0
  print('case', case, 'nins', info.nins)
  for ref = 1, info.nins do
    local mode, ot, a, b = util.traceir(1, ref)
    if ot then
      local offset = bit.rshift(ot, 8) * 6
      local op = names:sub(offset+1, offset+6)
      print(ref, op, a, b)
      if op:match('^XPOLL') then assert(a == 0); polls = polls + 1 end
    end
  end
  assert(polls > 0)
  local mcode, address, loop = util.tracemc(1)
  print('mcode', #mcode, 'loop-offset', loop)
  require('jit.dis_x64').disass(mcode, address)
  return
end
local samples, best = {}, math.huge
for i = 1, 5 do
  local start = os.clock()
  local result = work(n, input)
  local elapsed = os.clock() - start
  assert(result == expected(n))
  samples[i] = string.format('%.9f', elapsed)
  best = math.min(best, elapsed)
end
print(case, n, string.format('%.9f', best), table.concat(samples, ','), exits)
