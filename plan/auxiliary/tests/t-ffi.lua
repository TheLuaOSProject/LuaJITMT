-- t-ffi.lua — FFI under threads (11 §11.8; t-ffi-01..06).
local T = require("harness")
local th = require("threading")
local ffi = require("ffi")
local NT = tonumber(arg and arg[1]) or 4

T.case("01 concurrent cdef of distinct types", function()
  local done = th.channel(NT)
  for id = 1, NT do
    th.spawn(function()
      ffi.cdef(("typedef struct { int a%d; double b%d; } tdistinct%d;")
               :format(id, id, id))
      local x = ffi.new("tdistinct" .. id)
      x["a" .. id] = id
      assert(x["a" .. id] == id)
      done:send(true)
    end)
  end
  for i = 1, NT do done:recv() end
end)

T.case("02 same-struct cdef race yields one usable type", function()
  local done = th.channel(NT)
  for id = 1, NT do
    th.spawn(function()
      pcall(ffi.cdef, "typedef struct { int x, y; } tsame_race;")
      local p = ffi.new("tsame_race", id, id * 2)
      assert(p.x == id and p.y == id * 2)
      done:send(true)
    end)
  end
  for i = 1, NT do done:recv() end
  local a = ffi.new("tsame_race"); a.x = 7
  T.eq(a.x, 7)
end)

T.case("03 cross-thread cdata sharing + ffi.gc fires once", function()
  ffi.cdef("typedef struct { int n; } tfin;")
  local fired = 0
  local c = th.channel(0)
  do
    local cd = ffi.gc(ffi.new("tfin", 5), function() fired = fired + 1 end)
    local t = th.spawn(function()
      local o = c:recv()
      o.n = o.n + 1
      return o.n
    end)
    c:send(cd)
    local ok, n = t:join()
    T.truthy(ok); T.eq(n, 6); T.eq(cd.n, 6)
  end
  collectgarbage("collect"); collectgarbage("collect")
  for i = 1, 100 do if fired > 0 then break end th.sleep(0.01) end
  T.eq(fired, 1)
end)

T.case("04 callback invoked from a spawned thread", function()
  ffi.cdef("typedef int (*cbfn)(int);")
  local cb = ffi.cast("cbfn", function(x) return x * 2 end)
  local t = th.spawn(function()
    local r = 0
    for i = 1, 1000 do r = cb(i) end
    return r
  end)
  local ok, r = t:join()
  T.truthy(ok); T.eq(r, 2000)
  cb:free()
end)

T.case("05 blocking C call doesn't stall other threads' GC (11 §11.5)", function()
  ffi.cdef("unsigned int sleep(unsigned int);")
  local sleeper = th.spawn(function()
    ffi.C.sleep(1)                       -- native state for ~1s
    return true
  end)
  local t0 = os.time()
  collectgarbage("collect")              -- must finish despite the sleeper
  T.truthy(os.time() - t0 < 1, "GC cycle blocked on native-state thread")
  assert(sleeper:join())
end)

T.case("06 struct field hammering: VM-safe under user races", function()
  ffi.cdef("typedef struct { int a; int b; } thammer;")
  local p = ffi.new("thammer")
  local done = th.channel(NT)
  for id = 1, NT do
    th.spawn(function()
      for i = 1, 50000 do
        p.a = id
        local v = p.a
        assert(type(v) == "number")      -- M-5: VM safe; value is racy
      end
      done:send(true)
    end)
  end
  for i = 1, NT do done:recv() end
end)

T.done()
