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

local chunk = assert(loadstring(
  'local char=string.char; return function(n,x) local out=""; for i=1,n do out=char(i+x) end; return out end',
  '@char-tostr-fresh-publish'))
jit.off(chunk, true)
local candidate = chunk()
jit.off(candidate, true)
local oracle_chunk = assert(loadstring(
  'local char=string.char; return function(n,x) local out=""; for i=1,n do out=char(i+x) end; return out end',
  '@char-tostr-interpreted-oracle'))
jit.off(oracle_chunk, true)
local oracle = oracle_chunk()
jit.off(oracle, true)
jit.on(candidate, true)
jit.flush()
jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=32')
for i = 1, 8 do assert(candidate(48, 0) == oracle(48, 0)) end

local root, rootpc
for pc = 0, util.funcinfo(candidate).bytecodes-1 do
  local ins = util.funcbc(candidate, pc)
  if ins then
    local off = bit.band(ins, 255)*6
    if vmdef.bcnames:sub(off+1, off+6) == 'JFORL ' then
      root, rootpc = bit.rshift(ins, 16), pc
    end
  end
end
assert(root and util.traceinfo(root))
local ins = util.funcbc(candidate, rootpc)
assert(ins and vmdef.bcnames:sub(bit.band(ins, 255)*6+1,
                                 bit.band(ins, 255)*6+6) == 'JFORL ')
local code, addr = util.tracemc(root)
local exits, loop_exits = 0, 0
local function exit(trace, exitno)
  if trace == root then
    exits = exits + 1
    local _, pc = util.tracesnap(trace, exitno, true)
    if pc == rootpc+1 then loop_exits = loop_exits + 1 end
  end
end
jit.off(exit, true)
jit.attach(exit, 'texit')

collectgarbage('collect')
assert(candidate(48, 0) == oracle(48, 0))
native_begin_char(255)
local result = candidate(48, 207)
local publications = native_finish()
jit.attach(exit)
assert(result == oracle(48, 207) and #result == 1 and result:byte(1) == 255)
assert(publications > 0 and exits > 0 and loop_exits > 0)
assert(util.tracemc(root) == code and select(2, util.tracemc(root)) == addr)
collectgarbage('collect')
assert(candidate(48, 207) == oracle(48, 207))
assert(util.tracemc(root) == code and select(2, util.tracemc(root)) == addr)
local zero_byte = candidate(1, -1)
assert(zero_byte == oracle(1, -1) and #zero_byte == 1 and
       zero_byte:byte(1) == 0)
assert(util.tracemc(root) == code and select(2, util.tracemc(root)) == addr)
collectgarbage('collect')
assert(candidate(48, -1) == oracle(48, -1))
assert(util.tracemc(root) == code and select(2, util.tracemc(root)) == addr)
assert(candidate(1, 254) == oracle(1, 254))
assert(candidate(1, 0.5) == oracle(1, 0.5))
for _, x in ipairs({0, 255, 256, -2}) do
  local actual_ok, actual = pcall(candidate, 1, x)
  local oracle_ok, expected = pcall(oracle, 1, x)
  assert(actual_ok == oracle_ok)
  if actual_ok then assert(actual == expected) end
end
print('CHAR TOSTR fresh byte publication root='..root..' loop exits='..loop_exits..' GC/code identity passed')
assert(stop:send(true, 5))
assert(peer:join(5))
