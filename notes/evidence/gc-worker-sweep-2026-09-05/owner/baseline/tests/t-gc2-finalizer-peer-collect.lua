local threading = require("threading")

-- A finalizer owns GC2's finalizer_active guard for the complete callback.
-- Release a pre-existing peer into an explicit full collection, then join it
-- from the callback. Peer collection must defer instead of waiting for the
-- callback guard: otherwise the peer waits for this callback while the callback
-- waits for the peer.
local ready = threading.channel(1)
local start = threading.channel(1)
local entered = threading.channel(1)
local callback_join_timeout =
  tonumber(os.getenv("LJ_GC2_FINALIZER_PEER_JOIN_TIMEOUT")) or 3

local worker = threading.spawn(function(ready_ch, start_ch, entered_ch)
  assert(ready_ch:send("ready", 5) == true)
  local command, ok = start_ch:recv(5)
  assert(ok == true and command == "collect")
  assert(entered_ch:send("entered", 5) == true)
  collectgarbage("collect")
  return "peer collect returned"
end, ready, start, entered)

do
  local message, ok = ready:recv(5)
  assert(ok == true and message == "ready")
end

local finalizer_ran = false
local callback_joined
local callback_result

do
  local object = newproxy(true)
  getmetatable(object).__gc = function()
    finalizer_ran = true
    assert(start:send("collect", 5) == true)
    local message, ok = entered:recv(5)
    assert(ok == true and message == "entered")

    -- Keep the reducer bounded so a broken runtime reports the dependency
    -- instead of leaving an unkillable child. This succeeds only if the peer's
    -- full collection returns while this callback still owns finalizer_active.
    callback_joined, callback_result = worker:join(callback_join_timeout)
  end
  object = nil
end

collectgarbage("collect")

-- Always drain the worker after the callback releases finalizer_active. This
-- keeps the failing path clean and makes the first join result the sole oracle.
local joined, result = worker:join(5)
assert(joined == true and result == "peer collect returned", tostring(result))
assert(finalizer_ran == true, "finalizer did not run")
assert(callback_joined == true,
       "peer full collection waited for its joiner's finalizer callback: " ..
       tostring(callback_result))
assert(callback_result == "peer collect returned", tostring(callback_result))
collectgarbage("collect")
assert(threading.gcstats().phase == 0,
       "deferred peer collection left GC2 outside IDLE")

print("t-gc2-finalizer-peer-collect OK: peer full GC deferred during finalizer join")
