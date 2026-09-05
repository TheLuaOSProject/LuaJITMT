local ffi, th = require('ffi'), require('threading')
local util, vmdef, bit = require('jit.util'), require('jit.vmdef'), require('bit')
local n = tonumber(arg[1]) or 2000000
local expected_helper = arg[2] == 'helper'
ffi.cdef('int abs(int);')
local lib = ffi.C
assert(lib.abs)
local index = debug.getmetatable(lib).__index
local ready, stop = th.channel(1), th.channel(1)
local peer = th.spawn(function(ready, stop)
  assert(ready:send(true))
  local value, ok = stop:recv(60)
  assert(ok and value)
  return true
end, ready, stop)
assert(select(2, ready:recv(10)))
local function run(fn, receiver, count)
  local sum = 0
  for i = 1, count do
    if fn(receiver, 'abs') then sum = sum + 1 end
  end
  return sum
end
local exits = {}
local function onexit(tr) exits[tr] = (exits[tr] or 0) + 1 end
local function main()
  collectgarbage('collect')
  collectgarbage('collect')
  jit.flush()
  jit.opt.start('hotloop=1', 'hotexit=1000')
  jit.attach(onexit, 'texit')
  assert(run(index, lib, 80) == 80)
  local root, helpers, calls = nil, 0, {}
  for id, name in pairs(vmdef.ircall) do
    if name == 'lj_tab_gettv_rooted_hit_try' then calls[id] = true end
  end
  for tr = 1, 200 do
    local ti = util.traceinfo(tr)
    if ti and ti.link == tr and ti.linktype == 'loop' then root = tr; break end
  end
  assert(root and (exits[root] or 0) > 0, 'actual shared native root')
  local ti = util.traceinfo(root)
  for ref = 1, ti.nins do
    local _, ot, a, b = util.traceir(root, ref)
    if ot then
      local op = vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6)
      if op == 'CALLS ' and calls[b] then helpers = helpers + 1 end
    end
  end
  if expected_helper then assert(helpers > 0, 'actual shared hit_try call') end
  print('shape', root, ti.nins, #util.tracemc(root), helpers, exits[root])
  jit.attach(onexit)
  local best = math.huge
  for pass = 1, 5 do
    local start = os.clock()
    assert(run(index, lib, n) == n)
    local seconds = os.clock() - start
    print('sample', pass, string.format('%.9f', seconds))
    best = math.min(best, seconds)
  end
  print('result', 'shared', n, string.format('%.9f', best))
end
jit.off(onexit, true)
jit.off(main, true)
local ok, err = pcall(main)
jit.attach(onexit)
assert(stop:send(true, 10))
local joined, result = peer:join(10)
assert(joined and result)
assert(ok, err)
