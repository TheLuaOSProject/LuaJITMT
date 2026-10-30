local jit = require("jit")
local util = require("jit.util")
local vmdef = require("jit.vmdef")
local bit = require("bit")
local threading = require("threading")

jit.off(true, true)
local source = [[return function(x,n)
  local result=.5
  for i=1,n do result=-(x+.25) end
  return result
end]]
local function compile(name) return assert(loadstring(source, name))() end
jit.off(compile, true)
local candidate, oracle = compile("@unary-leaf-candidate"), compile("@unary-leaf-oracle")
assert(candidate ~= oracle)
jit.off(oracle, true)

local function root_trace(fn)
  local info = util.funcinfo(fn)
  for pc=1,info.bytecodes-1 do
    local ins = util.funcbc(fn, pc)
    if ins then
      local op = bit.band(ins, 255)
      if vmdef.bcnames:sub(op*6+1, op*6+6) == "JFORL " then
        local no = bit.rshift(ins, 16)
        local ti = util.traceinfo(no)
        if ti and ti.linktype == "loop" and ti.link == no then return no end
      end
    end
  end
end
jit.off(root_trace, true)

local ready, stop = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(r, s)
  assert(r:send(true, 5))
  local _, ok = s:recv(120)
  assert(ok)
  return true
end, ready, stop))
assert(select(2, ready:recv(5)))
jit.on()
jit.opt.start("hotloop=1", "hotexit=1", "maxtrace=64")
assert(candidate(5, 96) == oracle(5, 96))
local root
for i=1,8 do root=root_trace(candidate); if root then break end end
assert(root, "unary leaf did not publish its selected loop root")

local events = {}
local function onexit(no, exitno) events[#events+1] = {no, exitno} end
jit.off(onexit, true)
jit.attach(onexit, "texit")
local inputs = {30, 15, -3, 5, -0.0, 0/0, math.huge, -math.huge, -2147483648}
local total_exits = 0
local require_exit = jit.arch == "arm64"
local function same(a, b)
  if a ~= a then return b ~= b end
  if a == 0 and b == 0 then return 1/a == 1/b end
  return a == b
end
for _, x in ipairs(inputs) do
  events = {}
  local got, want = candidate(x, 32), oracle(x, 32)
  assert(same(got, want), "unary result differs from interpreted oracle")
  local selected = 0
  for _, e in ipairs(events) do
    local ti = util.traceinfo(e[1])
    if e[1] == root or (ti and ti.linktype == "root" and ti.link == root) then
      selected = selected+1
    end
  end
  if require_exit then
    assert(selected > 0, "input did not execute/exit the selected native root")
  end
  total_exits = total_exits+selected
end
jit.attach(onexit)

local before = {}
for no=1,64 do
  local ti = util.traceinfo(no)
  if ti and (no == root or (ti.linktype == "root" and ti.link == root)) then
    local code, addr, loop = util.tracemc(no)
    assert(code and addr and loop, "related trace has no native code")
    before[no] = {code, addr, loop, ti.linktype, ti.link}
  end
end
collectgarbage("collect")
assert(same(candidate(5, 32), oracle(5, 32)))
for no, row in pairs(before) do
  local ti = util.traceinfo(no)
  local code, addr, loop = util.tracemc(no)
  assert(ti and ti.linktype == row[4] and ti.link == row[5] and
         code == row[1] and addr == row[2] and loop == row[3],
         "native trace/code identity changed across GC")
end
assert(next(before), "no selected native trace remained live across GC")
assert(stop:send(true, 5)); assert(peer:join(5))
io.write("unary leaf ordinary: root=", root, " selected-exits=", total_exits,
         " require-exit=", require_exit and "yes" or "no",
         " inputs=", #inputs, " GC/code-reuse=yes oracle=yes live-peer=yes\n")
