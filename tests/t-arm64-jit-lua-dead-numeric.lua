local util, bit, vm = require('jit.util'), require('bit'), require('jit.vmdef')
local threading = require('threading')

jit.off(true, true)
local ready, stop = threading.channel(1), threading.channel(1)
local peer = threading.spawn(function(r, s)
  assert(r:send(true, 5))
  assert(select(2, s:recv(120000)))
  return true
end, ready, stop)
assert(select(2, ready:recv(5)))

local function root(fn)
  for pc = 1, util.funcinfo(fn).bytecodes - 1 do
    local ins = util.funcbc(fn, pc)
    local op = bit.band(ins, 255)
    if vm.bcnames:sub(op * 6 + 1, op * 6 + 6) == 'JFORL ' then
      local id = bit.rshift(ins, 16)
      if util.traceinfo(id) then return id end
    end
  end
end

local cases = {
  { 'dead-terminal', 'return x,.25,.5' },
  { 'dead-chain', 'return (x*.25+1)*.5,.25,.5' },
  { 'dead-division', 'return .5/x,.25,.5' },
}
for _, case in ipairs(cases) do
  local psrc = 'return function(x) ' .. case[2] .. ' end'
  local rsrc = 'return function(fn,n) local add=function() return 1.25 end; '
    .. 'local sum=.5; for i=1,n do sum=sum+add(fn(i+.5)) end; return sum end'
  local candidate = assert(loadstring(psrc, '@dead-numeric-candidate'))()
  local oracle = assert(loadstring(psrc, '@dead-numeric-oracle'))()
  local run = assert(loadstring(rsrc, '@dead-numeric-root'))()
  local oracle_run = assert(loadstring(rsrc, '@dead-numeric-oracle-root'))()
  jit.off(oracle, true)
  jit.off(oracle_run, true)
  jit.flush()
  jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=64')
  jit.on(candidate, true)
  jit.on(run, true)
  jit.on()
  local expected = oracle_run(oracle, 96)
  for i = 1, 16 do assert(run(candidate, 96) == expected) end
  local id = root(run)
  assert(id, case[1] .. ': selected root was not published')
  local exits = 0
  local function observe(trace)
    if trace == id then exits = exits + 1 end
  end
  jit.off(observe, true)
  jit.attach(observe, 'texit')
  collectgarbage('collect')
  assert(run(candidate, 64) == oracle_run(oracle, 64))
  jit.attach(observe)
  assert(exits > 0, case[1] .. ': selected root did not execute natively')
  print(case[1], jit.arch, 'root=' .. id, 'selected-exits=' .. exits)
end
assert(stop:send(true, 5))
local ok, result = peer:join(5)
assert(ok and result)
