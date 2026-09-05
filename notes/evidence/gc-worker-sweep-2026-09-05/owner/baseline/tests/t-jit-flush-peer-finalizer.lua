local th = require"threading"
local trace_count = require"jit_harness".trace_count

assert(type(newproxy) == "function", "newproxy support required")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")

local function hot_sum(n)
  local sum = 0
  for i = 1, n do sum = sum + i end
  return sum
end

for _ = 1, 32 do assert(hot_sum(200) == 20100) end
assert(trace_count(128) > 0, "failed to record peer-finalizer test trace")

local entered = th.channel(1)
local release = th.channel(1)
local worker = th.spawn(function(entered_ch, release_ch)
  local proxy = newproxy(true)
  getmetatable(proxy).__gc = function()
    assert(entered_ch:send("in-gc", 10) == true)
    -- The outer suite timeout must diagnose a peer-flush handshake stall; do
    -- not let this callback time out first and accidentally unblock it.
    local msg, ok = release_ch:recv(120)
    assert(ok == true and msg == "release")
  end
  proxy = nil
  collectgarbage("collect")
  collectgarbage("collect")
  return true
end, entered, release)

local msg, entered_ok = entered:recv(10)
assert(entered_ok == true and msg == "in-gc",
       "peer finalizer did not enter")

local flush_ok, flush_err = pcall(function()
  -- The old process-wide HOOK_GC check rejected every setmode operation on a
  -- peer, not only full flush. Exercise the engine modes before the handshake
  -- flush while the other TG still owns the callback.
  jit.off()
  jit.on()
  jit.flush()
end)
assert(release:send("release", 10) == true,
       "failed to release peer finalizer")
local joined, result = worker:join(20)
assert(joined == true and result == true, tostring(result))
assert(flush_ok == true, tostring(flush_err))
assert(trace_count(128) == 0,
       "peer-finalizer full flush left visible traces")

local local_rejected = false
local proxy = newproxy(true)
getmetatable(proxy).__gc = function()
  local ok, err = pcall(jit.flush)
  local_rejected = ok == false and
    tostring(err):find("bad action while in __gc metamethod", 1, true) ~= nil
end
proxy = nil
collectgarbage("collect")
collectgarbage("collect")
assert(local_rejected == true,
       "same-owner finalizer JIT flush was not rejected")

print("t-jit-flush-peer-finalizer OK: peer flush succeeds; owner is rejected")
