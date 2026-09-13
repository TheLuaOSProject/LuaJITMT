-- t-co.lua — coroutine × OS-thread matrix (03 §3.7, 05 §5.7.2, 06 §6.7).
local T = require("harness")
local th = require("threading")

T.case("01 coroutines unchanged within one thread", function()
  local co = coroutine.wrap(function(a)
    local b = coroutine.yield(a + 1)
    return b * 2
  end)
  T.eq(co(1), 2)
  T.eq(co(10), 20)
end)

T.case("02 resume/yield storm while GC marks (GCSCAN claim spin)", function()
  local cos = {}
  for i = 1, 64 do
    cos[i] = coroutine.create(function()
      local n = 0
      while true do n = n + 1; coroutine.yield(n) end
    end)
  end
  local stop = th.channel(1)
  local gcw = th.spawn(function()
    while stop:peek() == nil do collectgarbage("step") end
    return true
  end)
  for round = 1, 2000 do
    local i = round % 64 + 1
    local ok, v = coroutine.resume(cos[i])
    T.truthy(ok); T.truthy(v >= 1)
  end
  stop:send(true); assert(gcw:join())
end)

T.case("03 same coroutine resumed on two threads sequentially via channel", function()
  -- handoff: thread A resumes, suspends, sends the coroutine to B; B
  -- resumes it further. Owner-claim transfers via the resume CAS (06 §6.7).
  local co = coroutine.create(function()
    local from_a = coroutine.yield("first")
    local from_b = coroutine.yield("second:" .. from_a)
    return "done:" .. from_b
  end)
  local pipe = th.channel(0)
  local A = th.spawn(function()
    local ok, v = coroutine.resume(co)
    assert(ok and v == "first")
    pipe:send(co)
    return true
  end)
  local B = th.spawn(function()
    local c = pipe:recv()
    local ok, v = coroutine.resume(c, "A")
    assert(ok and v == "second:A", v)
    local ok2, v2 = coroutine.resume(c, "B")
    assert(ok2 and v2 == "done:B", v2)
    return true
  end)
  assert(A:join()); assert(B:join())
end)

T.case("04 concurrent resume of one coroutine: exactly one wins", function()
  T.reps(100, function()
    local gate = th.channel(0)
    local co = coroutine.create(function()
      gate:recv()                      -- park inside the coroutine
      return "winner-result"
    end)
    local function racer()
      local ok, v = coroutine.resume(co)
      if ok then return "ran", v end
      return "refused", v              -- "cannot resume" / busy error
    end
    local t1 = th.spawn(racer)
    local t2 = th.spawn(racer)
    gate:send(true)                     -- release whichever got in
    local _, s1, v1 = t1:join()
    local _, s2, v2 = t2:join()
    local rans = (s1 == "ran" and 1 or 0) + (s2 == "ran" and 1 or 0)
    T.eq(rans, 1, "both/neither thread resumed the coroutine")
    local winv = (s1 == "ran") and v1 or v2
    T.eq(winv, "winner-result")
  end)
end)

T.case("05 dead/suspended coroutines are collected across threads", function()
  local wt = setmetatable({}, { __mode = "v" })
  do
    for i = 1, 100 do
      local co = coroutine.create(function() coroutine.yield() end)
      coroutine.resume(co)
      wt[i] = co
    end
  end
  collectgarbage("collect"); collectgarbage("collect")
  local left = 0
  for _ in pairs(wt) do left = left + 1 end
  T.eq(left, 0, "suspended unreferenced coroutines retained")
end)

T.done()
