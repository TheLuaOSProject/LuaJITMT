local bit, util = require("bit"), require("jit.util")
local names = require("jit.vmdef").bcnames
local th = require("threading")

local function make(value)
  return function(n)
    local child
    for _ = 1, n do child = function() return value end end
    return child
  end
end

local function check(factory, expected)
  local pc
  for i = 1, util.funcinfo(factory).bytecodes - 1 do
    local op = bit.band(util.funcbc(factory, i), 255) * 6
    local name = names:sub(op+1, op+6)
    if name == "FORL  " or name == "JFORL " then pc = i end
  end
  assert(pc)
  local exited = false
  local function onexit(tr)
    local ins = util.funcbc(factory, pc)
    local op = bit.band(ins, 255) * 6
    if names:sub(op+1, op+6) == "JFORL " and bit.rshift(ins, 16) == tr then
      exited = true
    end
  end
  jit.off(onexit)
  jit.attach(onexit, "texit")
  local child
  for _ = 1, 128 do
    child = factory(64)
    assert(child() == expected)
    if exited then break end
    collectgarbage("step", 256)
    th.sleep(0.001)
  end
  jit.attach(onexit)
  assert(exited, "no native exit from the loop creating inherited closures")
  return child
end
jit.off(check)

local ready, done = th.channel(1), th.channel(1)
local peer = th.spawn(function()
  assert(ready:send(true))
  return done:recv(10)
end)
assert(ready:recv(10))
jit.opt.start("hotloop=1", "hotexit=100")
local first, second = {}, {}
local factory, donor = make(first), make(second)
local before = check(factory, first)
debug.upvaluejoin(factory, 1, donor, 1)
local after = check(factory, second)
collectgarbage()
assert(before() == first and after() == second)
assert(debug.upvalueid(before, 1) ~= debug.upvalueid(after, 1))
assert(done:send(true))
assert(peer:join())
print("t-jit-fnew-inherited OK")
