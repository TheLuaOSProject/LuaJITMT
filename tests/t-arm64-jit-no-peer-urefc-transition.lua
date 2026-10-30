local jit, util = require('jit'), require('jit.util')
local vmdef, bit = require('jit.vmdef'), require('bit')
local threading = require('threading')
assert(jit.arch == 'arm64' and jit.status())

local function make_candidate()
  local sub = string.sub
  return function(s, n)
    local i, result = 0, s
    while i < n do
      result = sub(s, -4, -5)
      i = i + 1
    end
    return result
  end
end

local f = make_candidate()
local sample = 'a\0\255bcdef'
local expected = ''
jit.flush()
jit.on(f, true)
jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=16')

local function root_id()
  for pc = 0, util.funcinfo(f).bytecodes-1 do
    local ins = util.funcbc(f, pc)
    if ins then
      local op = bit.band(ins, 0xff)
      local name = vmdef.bcnames:sub(op*6+1, op*6+6)
      if name == 'JFORL ' or name == 'JLOOP ' then
        return bit.rshift(ins, 16), name
      end
    end
  end
end

local function check_root(label)
  local no, loopop = root_id()
  assert(no, label..': missing loop root')
  local info = util.traceinfo(no)
  assert(info and info.linktype == 'loop' and info.link == no,
         label..': loop root is not published')
  local code, address, offset = util.tracemc(no)
  assert(code and #code > 0 and address and offset,
         label..': loop root has no native code')
  local rooted, raw = 0, 0
  for ref = 1, info.nins do
    local _, op, _, id = util.traceir(no, ref)
    local opname = op and vmdef.irnames:sub(bit.rshift(op, 8)*6+1,
                                             bit.rshift(op, 8)*6+6)
    if opname == 'CALLS ' and vmdef.ircall[id] == 'lj_func_getuv_rooted_try' then
      rooted = rooted + 1
    elseif opname == 'UREFC ' then
      raw = raw + 1
    end
  end
  assert(rooted > 0 and raw == 0,
         label..': expected rooted helper, got helper/raw='..rooted..'/'..raw)
  return no, loopop, code, address, offset
end

local function unchanged(no, code, address, offset)
  local current, at, loop = util.tracemc(no)
  assert(current == code and at == address and loop == offset,
         'loop machine code changed across GC')
end
jit.off(root_id, true)
jit.off(check_root, true)
jit.off(unchanged, true)

local function run_many(value, expected_value, n)
  for _ = 1, 40 do assert(f(value, n) == expected_value) end
end

run_many(sample, expected, 80)
local first, firstop, firstcode, firstaddr, firstoffset = check_root('no-peer')
local exits = 0
local function onexit(no)
  if no == first then exits = exits + 1 end
end
jit.off(onexit, true)
jit.attach(onexit, 'texit')
collectgarbage('collect')
run_many(sample, expected, 80)
jit.attach(onexit)
assert(exits > 0, 'no-peer root did not execute after GC')
unchanged(first, firstcode, firstaddr, firstoffset)

-- The helper reloads the exact closed cell. Changing it between entries must
-- produce the new function's result, and restoring it must restore string.sub.
assert(debug.getupvalue(f, 1) == 'sub')
local replacement = function() return 'changed' end
debug.setupvalue(f, 1, replacement)
run_many(sample, 'changed', 80)
debug.setupvalue(f, 1, string.sub)
run_many(sample, expected, 80)
collectgarbage('collect')
run_many(sample, expected, 80)

-- The first peer activation retires no-peer traces before shared recording.
-- Bind a currently published root after the callee-mutation roundtrip, so an
-- earlier invalidation cannot make this retirement check pass vacuously.
local retiring = check_root('before-peer-start')
local ready, release = threading.channel(1), threading.channel(1)
assert(root_id() == retiring and util.traceinfo(retiring),
       'no-peer root missing immediately before peer activation')
local peer = assert(threading.spawn(function(rdy, rel)
  assert(rdy:send(true, 5))
  assert(select(2, rel:recv(120)))
  return true
end, ready, release))
assert(select(2, ready:recv(5)))
assert(util.traceinfo(retiring) == nil,
       'no-peer trace remained published after first peer activation')
run_many(sample, expected, 80)
local second, _, secondcode, secondaddr, secondoffset = check_root('active-peer')
local peer_exits = 0
local function on_peer_exit(no)
  if no == second then peer_exits = peer_exits + 1 end
end
jit.off(on_peer_exit, true)
jit.attach(on_peer_exit, 'texit')
collectgarbage('collect')
run_many(sample, expected, 80)
jit.attach(on_peer_exit)
assert(peer_exits > 0, 'rooted peer trace did not execute after GC')
unchanged(second, secondcode, secondaddr, secondoffset)
assert(release:send(true, 5))
local ok, result = peer:join(5)
assert(ok and result)
print(string.format('no-peer UREFC normalization: %s exits=%d peer-exits=%d',
                    firstop, exits, peer_exits))
