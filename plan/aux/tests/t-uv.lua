-- t-uv.lua — upvalue cell model conformance (06 §6.4, 10 §10.3; t-uv-01..07).
-- These must pass single-threaded too: cell semantics == Lua 5.1 semantics.
local T = require("harness")
local th = require("threading")

T.case("01 sibling closures share one cell", function()
  local function mk()
    local x = 0
    return function() x = x + 1 end, function() return x end
  end
  local inc, get = mk()
  inc(); inc()
  T.eq(get(), 2)
end)

T.case("02 parent frame sees child mutation (and vice versa)", function()
  local x = 1
  local function set(v) x = v end
  set(10)
  T.eq(x, 10)
  x = 20
  local function get() return x end
  T.eq(get(), 20)
end)

T.case("03 cells survive parent return (closure lifetime)", function()
  local function counter()
    local n = 0
    return function() n = n + 1; return n end
  end
  local c1, c2 = counter(), counter()
  T.eq(c1(), 1); T.eq(c1(), 2); T.eq(c2(), 1)  -- independent cells
end)

T.case("04 cross-thread shared mutable upvalue (requirement 3)", function()
  local total = 0
  local done = th.channel(4)
  local function add(n) total = total + n end   -- cell captured 4 ways
  local ts = {}
  for i = 1, 4 do
    ts[i] = th.spawn(function(k)
      -- data race on purpose for the increment; synchronize via channel
      -- per-thread distinct bits so lost updates are detectable but the
      -- *visibility* (some write of each thread lands) is what we assert
      add(2 ^ k)
      done:send(true)
    end, i)
  end
  for i = 1, 4 do done:recv() end
  for i = 1, 4 do assert(ts[i]:join()) end
  -- with unsynchronized add() lost updates are legal (02 M-2); use a
  -- mutex-protected variant for the exact-sum assertion:
  local m, exact = th.mutex(), 0
  local function addx(n) m:lock(); exact = exact + n; m:unlock() end
  local ts2 = {}
  for i = 1, 4 do ts2[i] = th.spawn(addx, i) end
  for i = 1, 4 do assert(ts2[i]:join()) end
  T.eq(exact, 10)
  T.truthy(total > 0, "at least one unsynchronized write visible")
end)

T.case("05 cell identity preserved through tables and channels", function()
  local x = 0
  local fns = { set = function(v) x = v end, get = function() return x end }
  local c = th.channel(1)
  local t = th.spawn(function()
    local f = c:recv()
    f.set(99)
    return f.get()
  end)
  c:send(fns)
  local ok, seen = t:join()
  T.truthy(ok); T.eq(seen, 99); T.eq(x, 99)
end)

T.case("06 debug.getlocal unwraps cell slots (06 §6.4.2 shim)", function()
  local function f()
    local captured = 123
    local function g() return captured end
    local name, val
    for i = 1, 200 do
      local n, v = debug.getlocal(1, i)
      if n == nil then break end
      if n == "captured" then name, val = n, v end
    end
    return name, val, g
  end
  local n, v = f()
  T.eq(n, "captured"); T.eq(v, 123)  -- not a GCupval object leaking out
end)

T.case("07 loop variable capture is per-iteration (Lua 5.1)", function()
  local fs = {}
  for i = 1, 3 do fs[i] = function() return i end end
  T.eq(fs[1](), 1); T.eq(fs[2](), 2); T.eq(fs[3](), 3)
  local gs = {}
  for _, v in ipairs({ 10, 20 }) do gs[#gs+1] = function() return v end end
  T.eq(gs[1](), 10); T.eq(gs[2](), 20)
end)

T.case("08 immutable-upvalue JIT path still correct (const cell)", function()
  local K = 7
  local function f() return K end       -- never mutated: UREFC const path
  local s = 0
  for i = 1, 1e5 do s = s + f() end     -- let it trace
  T.eq(s, 7e5)
end)

T.done()
