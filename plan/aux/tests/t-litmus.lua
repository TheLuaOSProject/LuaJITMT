-- t-litmus.lua — memory-model litmus tests (13 §13.3, model in 02).
-- Each scenario repeats REPS times; ANY violation fails the case.
local T = require("harness")
local th = require("threading")
local REPS = tonumber(arg and arg[1]) or 200

T.case("L1 message passing (send rel -> recv acq)", function()
  T.reps(REPS, function()
    local x, c = {}, th.channel(1)
    local t = th.spawn(function()
      c:recv()
      assert(x.v == 42, "L1: stale read after recv")
    end)
    x.v = 42
    c:send(1)
    assert(t:join())
  end)
end)

T.case("L2 join happens-before", function()
  T.reps(REPS, function()
    local t = th.spawn(function()
      local u = {}
      for i = 1, 1000 do u[i] = i end
      return u
    end)
    local ok, u = t:join()
    assert(ok)
    for i = 1, 1000 do assert(u[i] == i, "L2: missing write after join") end
  end)
end)

T.case("L3 fence gives store/load order (Dekker)", function()
  T.reps(REPS, function()
    local f = { a = 0, b = 0 }
    local ra, rb
    local t1 = th.spawn(function()
      f.a = 1; th.fence(); ra = f.b
    end)
    local t2 = th.spawn(function()
      f.b = 1; th.fence(); rb = f.a
    end)
    t1:join(); t2:join()
    assert(not (ra == 0 and rb == 0), "L3: SC violated through fences")
  end)
end)

T.case("L4 per-producer FIFO through one channel", function()
  local c = th.channel(64)
  local NP, N = 2, 5000
  local ps = {}
  for p = 1, NP do
    ps[p] = th.spawn(function(tag)
      for i = 1, N do c:send(tag * 1000000 + i) end
    end, p)
  end
  local last = { 0, 0 }
  local cons = th.spawn(function()
    for _ = 1, NP * N do
      local v = c:recv()
      local tag, i = math.floor(v / 1000000), v % 1000000
      assert(i == last[tag] + 1, "L4: per-producer order broken")
      last[tag] = i
    end
    return last[1], last[2]
  end)
  for p = 1, NP do assert(ps[p]:join()) end
  local ok, a, b = cons:join()
  T.truthy(ok); T.eq(a, N); T.eq(b, N)
end)

T.case("L5 upvalue cell visibility via channel edge", function()
  T.reps(REPS, function()
    local counter = 0
    local c = th.channel(0)
    local t = th.spawn(function()
      counter = counter + 1          -- CSET on the shared cell
      c:send(true)
    end)
    c:recv()
    assert(counter == 1, "L5: cell write invisible after recv")
    assert(t:join())
  end)
end)

T.case("L6 no torn values: every read is some written reference", function()
  local A, B = { tag = "A" }, { tag = "B" }
  local t1 = th.spawn(function(sh)
    for i = 1, 200000 do sh.k = (i % 2 == 0) and A or B end
    sh.stop = true
  end, _G.__l6 or (function() _G.__l6 = { k = A }; return _G.__l6 end)())
  local sh = _G.__l6
  local t2 = th.spawn(function(s)
    local n = 0
    while not s.stop do
      local v = s.k
      assert(v == A or v == B, "L6: torn/foreign value read")
      n = n + 1
    end
    return n
  end, sh)
  assert(t1:join()); local ok, reads = t2:join()
  T.truthy(ok); T.truthy(reads > 0)
  _G.__l6 = nil
end)

T.case("L7 structure publication via channel (interior covered)", function()
  T.reps(REPS, function()
    local c = th.channel(1)
    local t = th.spawn(function()
      local u = c:recv()
      for i = 1, 100 do assert(u["k"..i] == i, "L7: interior missing") end
    end)
    local u = {}
    for i = 1, 100 do u["k"..i] = i end
    c:send(u)
    assert(t:join())
  end)
end)

T.done()
