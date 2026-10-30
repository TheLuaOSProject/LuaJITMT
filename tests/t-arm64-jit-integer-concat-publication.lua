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
  local value, ok = s:recv(60)
  assert(ok and value)
  return true
end, ready, stop))
assert(select(2, ready:recv(5)))

local source = 'return function(n) local out=""; for i=1,n do out=""..i end; return out end'
local candidate_chunk = assert(loadstring(source, '@integer-concat-fresh-publish'))
jit.off(candidate_chunk, true)
local candidate = candidate_chunk()
local oracle_chunk = assert(loadstring(source, '@integer-concat-interpreter-oracle'))
jit.off(oracle_chunk, true)
local oracle = oracle_chunk()
jit.off(oracle, true)
jit.on(candidate, true)
jit.flush()
jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=32')
for i = 1, 8 do
  local n = i % 2 == 0 and 64 or 48
  assert(candidate(n) == oracle(n))
end

local root, rootpc
for pc = 0, util.funcinfo(candidate).bytecodes-1 do
  local ins = util.funcbc(candidate, pc)
  if ins then
    local op = vmdef.bcnames:sub(bit.band(ins, 255)*6+1,
                                 bit.band(ins, 255)*6+6)
    if op == 'JFORL ' then root, rootpc = bit.rshift(ins, 16), pc end
  end
end
assert(root and util.traceinfo(root), 'integer concat root was not published')
local code, addr = util.tracemc(root)
local exits, loop_exits = 0, 0
local function observe(no, exitno)
  if no == root then
    exits = exits + 1
    local _, pc = util.tracesnap(no, exitno, true)
    if pc == rootpc+1 then loop_exits = loop_exits + 1 end
  end
end
jit.off(observe, true)
jit.attach(observe, 'texit')

collectgarbage('collect')
assert(candidate(48) == oracle(48))
assert(util.tracemc(root) == code and select(2, util.tracemc(root)) == addr)
native_begin_int(97)
local result = candidate(97)
local publications = native_finish()
jit.attach(observe)
assert(result == oracle(97))
assert(publications > 0 and exits > 0 and loop_exits > 0)
assert(util.tracemc(root) == code and select(2, util.tracemc(root)) == addr)
collectgarbage('collect')
assert(result == oracle(97))
assert(util.tracemc(root) == code and select(2, util.tracemc(root)) == addr)
print('INTEGER CONCAT publication root='..root..' target=97 under-lease publications='..publications..' terminal loop exits='..loop_exits..' GC/code identity passed')
assert(stop:send(true, 5))
assert(peer:join(5))
