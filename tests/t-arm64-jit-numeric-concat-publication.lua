local jit = require('jit')
local util = require('jit.util')
local bit = require('bit')
local vmdef = require('jit.vmdef')
local threading = require('threading')
jit.off(true, true)

local ready, stop = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(r, s)
  jit.off(true, true)
  assert(r:send(true, 5))
  local v, ok = s:recv(60)
  assert(ok and v)
  return true
end, ready, stop))
assert(select(2, ready:recv(5)))

local function make()
  return function(n, x)
    local out = ''
    for i = 1, n do out = '' .. (x + i*.25) end
    return out
  end
end
local chunk = assert(loadstring('return function(n,x) local out=""; for i=1,n do out=""..(x+i*.25) end; return out end',
                                '@numeric-concat-fresh-publish'))
jit.off(chunk, true)
local candidate = chunk()
jit.off(candidate, true)
local oracle = make()
jit.off(oracle, true)
jit.on(candidate, true)
jit.flush()
jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=32')
for i = 1, 8 do assert(candidate(80, 4.75) == oracle(80, 4.75)) end

local root, rootpc
for pc = 0, util.funcinfo(candidate).bytecodes-1 do
  local ins = util.funcbc(candidate, pc)
  if ins then
    local off = bit.band(ins, 255)*6
    local op = vmdef.bcnames:sub(off+1, off+6)
    if op == 'JFORL ' then root, rootpc = bit.rshift(ins, 16), pc end
  end
end
assert(root and util.traceinfo(root))
local rootins = util.funcbc(candidate, rootpc)
assert(rootins and vmdef.bcnames:sub(bit.band(rootins, 255)*6+1,
                                   bit.band(rootins, 255)*6+6) == 'JFORL ')
local code, addr = util.tracemc(root)
local exits, loop_exits = 0, 0
local function exit(t, exitno)
  if t == root then
    exits = exits + 1
    local snap, pc = util.tracesnap(t, exitno, true)
    if snap and pc == rootpc+1 then loop_exits = loop_exits + 1 end
  end
end
jit.off(exit, true)
jit.attach(exit, 'texit')

collectgarbage('collect')
assert(candidate(80, 4.75) == oracle(80, 4.75))
native_begin_numeric()
local result = candidate(80, 100000.125)
local publications = native_finish()
jit.attach(exit)
assert(result == oracle(80, 100000.125))
assert(publications > 0 and exits > 0 and loop_exits > 0)
assert(util.tracemc(root) == code and select(2, util.tracemc(root)) == addr)
collectgarbage('collect')
assert(result == oracle(80, 100000.125))
assert(util.tracemc(root) == code and select(2, util.tracemc(root)) == addr)
print('NUMERIC CONCAT publication root='..root..' under-lease publications='..publications..' terminal loop exits='..loop_exits..' GC/code identity passed')
assert(stop:send(true, 5))
assert(peer:join(5))
