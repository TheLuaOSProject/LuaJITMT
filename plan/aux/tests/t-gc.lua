-- t-gc.lua — concurrent GC behavior (05; t-gc-01..06, t-weak basics).
local T = require("harness")
local th = require("threading")
local NT = tonumber(arg and arg[1]) or 4

T.case("01 cycles complete while mutators run (no STW dependence)", function()
  local stop = th.channel(1)
  local ws = {}
  for i = 1, NT do
    ws[i] = th.spawn(function()
      local keep, n = {}, 0
      while stop:peek() == nil do
        n = n + 1
        keep[n % 500 + 1] = { tostring(n), n }
      end
      return n
    end)
  end
  for c = 1, 5 do collectgarbage("collect") end   -- parks until cycle done (05 §5.10)
  stop:send(true)
  for i = 1, NT do assert(ws[i]:join()) end
  T.truthy(true)
end)

T.case("02 live data survives heavy parallel churn", function()
  local anchors = {}
  for i = 1, 1000 do anchors[i] = { id = i, payload = ("p"):rep(i % 64) } end
  local done = th.channel(NT)
  for id = 1, NT do
    th.spawn(function()
      for i = 1, 50000 do local _ = { i, { i } } end
      done:send(true)
    end)
  end
  for i = 1, NT do done:recv() end
  collectgarbage("collect")
  for i = 1, 1000 do
    T.eq(anchors[i].id, i, "live object corrupted/collected")
  end
end)

T.case("03 weak-value table drops garbage, keeps anchored", function()
  local wt = setmetatable({}, { __mode = "v" })
  local keep = { name = "kept" }
  wt.a = keep
  wt.b = { name = "dropme" }
  collectgarbage("collect"); collectgarbage("collect")
  T.truthy(wt.a == keep, "anchored value cleared")
  T.eq(wt.b, nil, "garbage value not cleared")
end)

T.case("04 weak write during mark can't resurrect stale (05 §5.8)", function()
  -- racy by nature; assert only safety: no crash, entries are either the
  -- written object or nil
  local wt = setmetatable({}, { __mode = "v" })
  local stop = th.channel(1)
  local w = th.spawn(function()
    local i = 0
    while stop:peek() == nil do
      i = i + 1
      wt[i % 64] = { i }
    end
    return i
  end)
  for c = 1, 10 do collectgarbage("collect") end
  stop:send(true); assert(w:join())
  for k, v in pairs(wt) do
    T.truthy(type(v) == "table", "weak slot holds non-table")
  end
end)

T.case("05 udata finalizer runs exactly once, after unreachable", function()
  local fired = 0
  do
    local u = newproxy and newproxy(true) or nil
    if u then
      getmetatable(u).__gc = function() fired = fired + 1 end
    else
      -- 5.1-style via ffi cdata finalizer
      local ffi = require("ffi")
      local cd = ffi.new("int[1]")
      ffi.gc(cd, function() fired = fired + 1 end)
    end
  end
  collectgarbage("collect"); collectgarbage("collect")
  -- finalizer thread is async (05 §5.8): give it bounded time
  for i = 1, 100 do
    if fired > 0 then break end
    th.sleep(0.01)
  end
  T.eq(fired, 1)
end)

T.case("06 torture mode end-to-end", function()
  collectgarbage("torture", 1)
  local done = th.channel(NT)
  for id = 1, NT do
    th.spawn(function()
      local t = {}
      for i = 1, 5000 do
        t["k"..(i % 97)] = { i, ("x"):rep(i % 32) }
      end
      done:send(true)
    end)
  end
  for i = 1, NT do done:recv() end
  collectgarbage("torture", 0)
  T.truthy(true)
end)

T.case("07 collectgarbage('count') is a sane estimate", function()
  local before = collectgarbage("count")
  local big = {}
  for i = 1, 10000 do big[i] = { i, i, i } end
  local after = collectgarbage("count")
  T.truthy(after > before, "count not increasing with live data")
  big = nil
  collectgarbage("collect")
end)

T.done()
