local buffer = require("string.buffer")
local work, size, count, capacity = arg[1], tonumber(arg[2]), tonumber(arg[3]), tonumber(arg[4])
local b = buffer.new(capacity)
local payload = ("x"):rep(size)
local function loop(n)
  if work == "append" then
    for _ = 1, n do b:put(payload) end
  elseif work == "resetget" then
    for _ = 1, n do b:reset():put(payload); assert(b:get() == payload) end
  elseif work == "putget" then
    for _ = 1, n do b:put(payload); assert(b:get() == payload) end
  elseif work == "resetonly" then
    for _ = 1, n do b:reset() end
  end
end
jit.opt.start("hotloop=20", "hotexit=100")
loop(1000)
if work == "append" then assert(b:get() == payload:rep(1000)) end
collectgarbage()
local function run() loop(count) end
local function verify()
  if work == "append" then assert(b:get() == payload:rep(count)) else assert(#b == 0) end
end
return run, verify
