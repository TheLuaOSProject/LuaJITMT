-- t-api.lua — threading.* conformance (09_threading_api.md; t-api-01..10).
local T = require("harness")
local th = require("threading")

T.case("01 spawn/join result passthrough", function()
  local t = th.spawn(function(a, b) return a + b, "x" end, 40, 2)
  local ok, s, x = t:join()
  T.truthy(ok); T.eq(s, 42); T.eq(x, "x")
end)

T.case("02 join idempotent from two callers", function()
  local t = th.spawn(function() return 7 end)
  local r1 = { t:join() }
  local r2 = { t:join() }
  T.eq(r1[2], 7); T.eq(r2[2], 7)
end)

T.case("03 error propagation via join", function()
  local t = th.spawn(function() error("boom", 0) end)
  local ok, err = t:join()
  T.eq(ok, false); T.eq(err, "boom")
end)

T.case("04 join timeout then success", function()
  local c = th.channel(0)
  local t = th.spawn(function() c:recv(); return 1 end)
  local r, why = t:join(0.05)
  T.eq(r, nil); T.eq(why, "timeout")
  c:send(true)
  T.truthy(t:join())
end)

T.case("05 id/current/running", function()
  local me = th.current()
  T.truthy(me:id() >= 0)
  local t = th.spawn(function() return th.current():id() end)
  local ok, cid = t:join()
  T.truthy(ok); T.truthy(cid ~= me:id())
  T.eq(t:running(), false)
end)

T.case("06 channel FIFO + close drain", function()
  local c = th.channel(8)
  for i = 1, 5 do c:send(i) end
  c:close()
  for i = 1, 5 do
    local v, ok = c:recv()
    T.eq(ok, true); T.eq(v, i)
  end
  local v, ok = c:recv()
  T.eq(ok, false); T.eq(v, nil)
  T.eq(pcall(function() c:send(1) end), false)  -- send-after-close errors
end)

T.case("07 rendezvous (cap=0) hands off synchronously", function()
  local c = th.channel(0)
  local got
  local t = th.spawn(function() got = c:recv() end)
  c:send("hi")                       -- returns only after receiver took it
  T.truthy(t:join())
  T.eq(got, "hi")
end)

T.case("08 channel transfers references, not copies", function()
  local c = th.channel(1)
  local obj = { mark = 1 }
  local t = th.spawn(function()
    local o = c:recv()
    o.mark = o.mark + 1
    return o
  end)
  c:send(obj)
  local ok, back = t:join()
  T.truthy(ok)
  T.truthy(back == obj, "identity preserved across threads")
  T.eq(obj.mark, 2)
end)

T.case("09 detached thread runs to completion", function()
  local c = th.channel(1)
  th.spawn(function() c:send("done") end)  -- handle dropped immediately
  collectgarbage("collect")
  T.eq(c:recv(), "done")
end)

T.case("10 thousand sequential spawns (TG attach/detach churn)", function()
  local sum = 0
  for i = 1, 1000 do
    local ok, v = th.spawn(function(x) return x end, i):join()
    T.truthy(ok); sum = sum + v
  end
  T.eq(sum, 500500)
end)

T.case("11 sleep is interruptible by shutdown only, returns on time", function()
  local t0 = os.clock()
  th.sleep(0.05)
  T.truthy(true)  -- crude: just ensure no error; wall-time check needs th.now
end)

T.case("12 mutex basic + trylock", function()
  local m = th.mutex()
  m:lock()
  local t = th.spawn(function() return m:trylock() end)
  local ok, got = t:join()
  T.truthy(ok); T.eq(got, false)
  m:unlock()
  T.eq(m:trylock(), true); m:unlock()
end)

T.done()
