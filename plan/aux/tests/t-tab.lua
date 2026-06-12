-- t-tab.lua — concurrent table hammers (06 §6.2–6.3; t-tab-01..08).
-- Mirrors aux/nbtab_model.c at the Lua level; run under torture (05 §5.13).
local T = require("harness")
local th = require("threading")
local NT = tonumber(arg and arg[1]) or 8
local OPS = tonumber(arg and arg[2]) or 20000

local function spawn_all(n, fn, ...)
  local ts = {}
  for i = 1, n do ts[i] = th.spawn(fn, i, ...) end
  local rs = {}
  for i = 1, n do
    local ok, r = ts[i]:join()
    assert(ok, r)
    rs[i] = r
  end
  return rs
end

T.case("01 disjoint-key inserts all land (resize storm)", function()
  local t = {}
  spawn_all(NT, function(id)
    for i = 1, OPS do t[id * 1000000 + i] = i end
    return 0
  end)
  for id = 1, NT do
    for i = 1, OPS do assert(t[id * 1000000 + i] == i, "lost insert") end
  end
end)

T.case("02 shared hot keys: values always come from the written set", function()
  local t = {}
  spawn_all(NT, function(id)
    for i = 1, OPS do
      local k = "k" .. (i % 256)
      t[k] = id * 1000000 + i
      local v = t[k]
      if v ~= nil then
        assert(type(v) == "number" and v >= 1000001, "foreign value")
      end
    end
    return 0
  end)
end)

T.case("03 delete/reinsert churn never corrupts type", function()
  local t = {}
  spawn_all(NT, function(id)
    for i = 1, OPS do
      local k = i % 128
      if i % 3 == 0 then t[k] = nil
      elseif i % 3 == 1 then t[k] = { id, i }
      else
        local v = t[k]
        assert(v == nil or (type(v) == "table" and type(v[1]) == "number"),
               "type confusion")
      end
    end
    return 0
  end)
end)

T.case("04 array part: concurrent grow + read returns sane values", function()
  local t = {}
  spawn_all(NT, function(id)
    for i = 1, OPS do
      local j = (i * NT + id) % 4096 + 1
      t[j] = j * 10
      local v = t[j]
      assert(v == nil or v % 10 == 0, "torn array slot")
    end
    return 0
  end)
end)

T.case("05 pairs during mutation: no crash, no duplicate key per pass", function()
  local t = {}
  for i = 1, 512 do t["s"..i] = i end
  local stop = th.channel(1)
  local mut = th.spawn(function()
    local i = 0
    while stop:peek() == nil do
      i = i + 1
      t["m"..(i % 64)] = i
      if i % 7 == 0 then t["m"..(i % 64)] = nil end
    end
    return i
  end)
  for pass = 1, 50 do
    local seen = {}
    for k, v in pairs(t) do
      assert(seen[k] == nil, "duplicate key in one pairs() pass")
      seen[k] = true
      assert(v ~= nil)
    end
    for i = 1, 512 do assert(seen["s"..i], "stable key missed") end
  end
  stop:send(true)
  assert(mut:join())
end)

T.case("06 # returns a border under racing array writes", function()
  local t = {}
  for i = 1, 100 do t[i] = i end
  spawn_all(2, function(id)
    for i = 1, OPS do
      if id == 1 then
        t[100 + (i % 50) + 1] = i
      else
        local n = #t
        assert(n >= 0, "negative length")
        assert(t[n] ~= nil or n == 0, "len not a border")
      end
    end
    return 0
  end)
end)

T.case("07 metatable swap race keeps dispatch sane", function()
  local mtA = { __index = function() return "A" end }
  local mtB = { __index = function() return "B" end }
  local t = setmetatable({}, mtA)
  spawn_all(NT, function(id)
    for i = 1, OPS do
      if id == 1 then
        setmetatable(t, (i % 2 == 0) and mtA or mtB)
      else
        local v = t.missing
        assert(v == "A" or v == "B", "metamethod dispatch corrupted")
      end
    end
    return 0
  end)
end)

T.case("08 single-writer key: my own write is immediately visible", function()
  local t = {}
  spawn_all(NT, function(id)
    local k = "own" .. id
    for i = 1, OPS do
      t[k] = i
      assert(t[k] == i, "lost my own write")
    end
    return 0
  end)
end)

T.done()
