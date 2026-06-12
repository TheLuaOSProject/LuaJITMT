local th = require"threading"

assert(type(th) == "table")
assert(type(th.cpucount) == "function")
assert(type(th.fence) == "function")
assert(type(th.sleep) == "function")
assert(type(th.channel) == "function")

local n = th.cpucount()
assert(type(n) == "number" and n >= 1)

assert(th.fence() == nil)
assert(th.sleep(0) == nil)

local ch = th.channel(2)
assert(type(ch) == "userdata")

local v, ok = ch:peek()
assert(v == nil and ok == "empty")

assert(ch:send("x") == true)
v, ok = ch:peek()
assert(v == "x" and ok == true)
v, ok = ch:recv()
assert(v == "x" and ok == true)

local keepalive = th.channel(1)
local t = {marker = 42}
assert(keepalive:send(t) == true)
t = nil
collectgarbage()
collectgarbage()
v, ok = keepalive:recv()
assert(ok == true and v.marker == 42)

ch:close()
v, ok = ch:recv()
assert(v == nil and ok == false)
local sent, err = pcall(function() ch:send("y") end)
assert(sent == false and tostring(err):match("closed channel"))

local made, rangeerr = pcall(function() th.channel(-1) end)
assert(made == false and tostring(rangeerr):match("out of range"))

local th2 = require"threading"
assert(th2 == th)

print("t-threading-api OK: cpucount, fence, sleep, channel registered")
