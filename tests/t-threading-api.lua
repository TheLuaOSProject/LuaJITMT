local th = require"threading"

assert(type(th) == "table")
assert(type(th.cpucount) == "function")
assert(type(th.fence) == "function")
assert(type(th.sleep) == "function")

local n = th.cpucount()
assert(type(n) == "number" and n >= 1)

assert(th.fence() == nil)
assert(th.sleep(0) == nil)

local th2 = require"threading"
assert(th2 == th)

print("t-threading-api OK: cpucount, fence, sleep registered")
