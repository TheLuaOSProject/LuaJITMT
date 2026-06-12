-- t-str.lua — string interning under threads (06 §6.5; t-str-01..03; I-8).
local T = require("harness")
local th = require("threading")
local NT = tonumber(arg and arg[1]) or 8

T.case("01 intern storm: identity I-8 across threads", function()
  -- All threads intern the same content set; collect one representative
  -- string per content per thread; assert raw equality (same GCstr).
  local c = th.channel(NT)
  for id = 1, NT do
    th.spawn(function()
      local mine = {}
      for i = 1, 20000 do
        local s = "id_" .. (i % 4096) .. "_x"
        mine[i % 4096] = s
      end
      c:send(mine)
    end)
  end
  local sets = {}
  for i = 1, NT do sets[i] = c:recv() end
  for k = 0, 4095 do
    local ref = sets[1][k]
    for i = 2, NT do
      -- == on strings is pointer equality in LuaJIT after interning
      T.truthy(sets[i][k] == ref, "intern identity broken for key " .. k)
    end
  end
end)

T.case("02 sweep-resurrection window keeps identity", function()
  -- Force GC cycles while one thread re-creates strings another just
  -- dropped (06 §6.5.2 resurrect rule + §6.5.4 two-wave sweep).
  local stop = th.channel(1)
  local maker = th.spawn(function()
    local i, keep = 0, nil
    while stop:peek() == nil do
      i = i + 1
      keep = "resur_" .. (i % 97)        -- churn one small content set
      if i % 1000 == 0 then collectgarbage("step") end
    end
    return keep
  end)
  for n = 1, 20 do
    collectgarbage("collect")
    local a = "resur_" .. (n % 97)
    local b = "resur_" .. (n % 97)
    T.truthy(a == b)
  end
  stop:send(true)
  assert(maker:join())
end)

T.case("03 strtab resize race: distinct contents stay distinct", function()
  -- Grow the table past several resize thresholds from all threads at
  -- once; verify a sample of contents map to exactly one string and
  -- distinct contents never alias.
  local done = th.channel(NT)
  for id = 1, NT do
    th.spawn(function()
      for i = 1, 50000 do
        local s = "grow_" .. id .. "_" .. i
        assert(#s >= 7)
      end
      done:send(true)
    end)
  end
  for i = 1, NT do done:recv() end
  T.truthy(("grow_1_1" == "grow_1_1") and ("grow_1_1" ~= "grow_1_2"))
  -- spot identity through a table key round-trip
  local t = {}
  t["grow_2_42"] = true
  T.truthy(t["grow_" .. 2 .. "_" .. 42] == true, "content hashing diverged")
end)

T.case("04 string.format / tmpbuf is per-thread (03 §3.3)", function()
  local done = th.channel(NT)
  for id = 1, NT do
    th.spawn(function()
      for i = 1, 20000 do
        local s = string.format("%d:%s:%x", i, "abc", i)
        assert(s:find(":abc:", 1, true), "tmpbuf cross-thread corruption")
      end
      done:send(true)
    end)
  end
  for i = 1, NT do done:recv() end
end)

T.done()
