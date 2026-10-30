local jit, util = require('jit'), require('jit.util')
local vmdef, bit = require('jit.vmdef'), require('bit')
local threading = require('threading')
assert(jit.arch == 'arm64' and jit.status())
jit.off(true, true)

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

local function root_id(fn)
  for pc = 0, util.funcinfo(fn).bytecodes-1 do
    local ins = util.funcbc(fn, pc)
    if ins then
      local op = bit.band(ins, 0xff)
      local name = vmdef.bcnames:sub(op*6+1, op*6+6)
      if name == 'JFORL ' or name == 'JLOOP ' then
        return bit.rshift(ins, 16), name
      end
    end
  end
end

local function check_root(label, fn)
  local no, loopop = root_id(fn or f)
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

local function run_many(value, expected_value, n)
  for _ = 1, 40 do assert(f(value, n) == expected_value) end
end

run_many(sample, expected, 80)
local first, firstop, firstcode, firstaddr, firstoffset = check_root('no-peer', f)
local function make_numeric_reader()
  local value = 17
  local function read(n)
    local out = value
    for i = 1, n do out = value end
    return out
  end
  local function write(v) value = v end
  return read, write
end
local read_number, write_number = make_numeric_reader()
jit.on(read_number, true)
for _ = 1, 40 do assert(read_number(80) == 17) end
local number_root, _, numbercode, numberaddr, numberoffset =
  check_root('no-peer-numeric', read_number)
local number_info = assert(util.traceinfo(number_root))
local number_get = false
for ref = 1, number_info.nins do
  local _, op, _, id = util.traceir(number_root, ref)
  local opname = op and vmdef.irnames:sub(bit.rshift(op, 8)*6+1,
                                           bit.rshift(op, 8)*6+6)
  number_get = number_get or
    (opname == 'CALLS ' and vmdef.ircall[id] == 'lj_func_getuv_rooted_try')
end
assert(number_get, 'numeric no-peer root missing rooted helper')
local exits = 0
local number_exits = 0
local function onexit(no)
  if no == first then exits = exits + 1 end
  if no == number_root then number_exits = number_exits + 1 end
end
jit.off(onexit, true)
jit.attach(onexit, 'texit')
collectgarbage('collect')
run_many(sample, expected, 80)
for _ = 1, 40 do assert(read_number(80) == 17) end
jit.attach(onexit)
assert(exits > 0, 'no-peer root did not execute after GC')
assert(number_exits > 0, 'no-peer numeric root did not execute after GC')
unchanged(first, firstcode, firstaddr, firstoffset)
unchanged(number_root, numbercode, numberaddr, numberoffset)

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
write_number('changed-type')
for _ = 1, 8 do assert(read_number(80) == 'changed-type') end
write_number(17)
for _ = 1, 8 do assert(read_number(80) == 17) end

-- Bind current roots after mutation, so prior invalidation cannot make the
-- peer-retirement assertions pass vacuously.
local retiring = check_root('before-peer-start', f)
local retiring_number = check_root('numeric-before-peer-start', read_number)
-- The first peer activation retires no-peer traces before shared recording.
local ready, release = threading.channel(1), threading.channel(1)
assert(root_id(f) == retiring and util.traceinfo(retiring) and
       root_id(read_number) == retiring_number and util.traceinfo(retiring_number),
       'no-peer roots missing immediately before peer activation')
local peer = assert(threading.spawn(function(rdy, rel)
  assert(rdy:send(true, 5))
  assert(select(2, rel:recv(120)))
  return true
end, ready, release))
assert(select(2, ready:recv(5)))
assert(util.traceinfo(retiring) == nil,
       'no-peer trace remained published after first peer activation')
assert(util.traceinfo(retiring_number) == nil,
       'no-peer numeric trace remained published after peer activation')
run_many(sample, expected, 80)
for _ = 1, 40 do assert(read_number(80) == 17) end
local second, _, secondcode, secondaddr, secondoffset = check_root('active-peer', f)
local second_number, _, peernumcode, peernumaddr, peernumoffset =
  check_root('active-peer-numeric', read_number)
local peer_exits = 0
local peer_number_exits = 0
local function on_peer_exit(no)
  if no == second then peer_exits = peer_exits + 1 end
  if no == second_number then peer_number_exits = peer_number_exits + 1 end
end
jit.off(on_peer_exit, true)
jit.attach(on_peer_exit, 'texit')
collectgarbage('collect')
run_many(sample, expected, 80)
for _ = 1, 40 do assert(read_number(80) == 17) end
jit.attach(on_peer_exit)
assert(peer_exits > 0, 'rooted peer trace did not execute after GC')
assert(peer_number_exits > 0,
       'rooted peer numeric trace did not execute after GC')
unchanged(second, secondcode, secondaddr, secondoffset)
unchanged(second_number, peernumcode, peernumaddr, peernumoffset)
assert(release:send(true, 5))
local ok, result = peer:join(5)
assert(ok and result)
print(string.format('no-peer UREFC normalization: %s exits=%d peer-exits=%d',
                    firstop, exits, peer_exits))
