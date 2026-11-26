local buffer = require("string.buffer")
local work = arg[1] or "append"
local size = tonumber(arg[2]) or 1
local count = tonumber(arg[3]) or 1000000
local rounds = tonumber(arg[4]) or 1
local capacity = tonumber(arg[5]) or 0
local dumpfile = arg[6]
jit.opt.start("hotloop=20", "hotexit=100")
local function make()
  local payload = ("x"):rep(size)
  local b = buffer.new(capacity)
  local function append(n)
    for _ = 1, n do b:put(payload) end
  end
  local function resetget(n)
    for _ = 1, n do
      b:reset():put(payload)
      assert(b:get() == payload)
    end
  end
  local function putget(n)
    for _ = 1, n do
      b:put(payload)
      assert(b:get() == payload)
    end
  end
  local function resetonly(n)
    for _ = 1, n do b:reset() end
  end
  local fn = assert(({append=append,resetget=resetget,putget=putget,resetonly=resetonly})[work])
  return b, payload, fn
end
local b, payload, fn = make()
if dumpfile then require("jit.dump").start("tbirsmT", dumpfile) end
fn(1000)
if work == "append" then assert(b:get() == payload:rep(1000)) end
collectgarbage()
local start = os.clock()
for _ = 1, rounds do
  b:reset()
  fn(count)
  if work == "append" then
    assert(#b == size*count)
    assert(b:get(1) == "x")
    b:skip(size*count-2)
    assert(b:get() == "x")
  else
    assert(#b == 0)
  end
end
local elapsed = os.clock()-start
if dumpfile then require("jit.dump").off() end
print(work,size,count,rounds,capacity,elapsed,elapsed*1e9/(count*rounds))
