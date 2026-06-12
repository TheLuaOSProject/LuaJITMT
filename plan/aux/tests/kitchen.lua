-- kitchen.lua — combined randomized soak (13 §13.4). Any error fails.
-- Usage: luajit kitchen.lua [threads] [seconds] [seed]
local th = require("threading")
local ffi = pcall(require, "ffi") and require("ffi") or nil

local NT   = tonumber(arg and arg[1]) or 8
local SECS = tonumber(arg and arg[2]) or 60
local SEED = tonumber(arg and arg[3]) or 12345

if ffi then ffi.cdef("typedef struct { double x, y; } kpoint;") end

-- shared world
local shared = {
  tab = {},                                  -- hot shared table
  cells = (function()
    local n = 0
    return { inc = function() n = n + 1 end, get = function() return n end }
  end)(),
  chan = th.channel(256),
  weak = setmetatable({}, { __mode = "v" }),
}
local stopch = th.channel(NT)

local function worker(id)
  local rng = SEED + id * 7919
  local function r(m) rng = (rng * 1103515245 + 12345) % 2^31; return rng % m end
  local co = coroutine.wrap(function()
    while true do coroutine.yield(coroutine.yield(1) or 1) end
  end)
  local keep = {}
  local deadline = os.time() + SECS
  local ops = 0
  while os.time() < deadline do
    ops = ops + 1
    local k = r(12)
    if k == 0 then                              -- shared table write
      shared.tab["k" .. r(512)] = { id, ops }
    elseif k == 1 then                          -- shared table read
      local v = shared.tab["k" .. r(512)]
      assert(v == nil or type(v) == "table")
    elseif k == 2 then                          -- delete
      shared.tab["k" .. r(512)] = nil
    elseif k == 3 then                          -- private alloc churn
      keep[r(200) + 1] = { ops, ("s"):rep(r(40)) }
    elseif k == 4 then                          -- string intern churn
      local s = "kit_" .. r(4096)
      assert(#s >= 5)
    elseif k == 5 then                          -- shared cell
      shared.cells.inc()
      assert(shared.cells.get() >= 1)
    elseif k == 6 then                          -- channel produce
      if not pcall(shared.chan.send, shared.chan, { id, ops }) then end
    elseif k == 7 then                          -- channel consume
      local v = shared.chan:peek()
      if v then shared.chan:recv() end
    elseif k == 8 then                          -- coroutine traffic
      assert(co() ~= nil)
    elseif k == 9 then                          -- weak table churn
      shared.weak[r(64)] = { ops }
    elseif k == 10 and ffi then                 -- ffi churn
      local p = ffi.new("kpoint", ops, id)
      p.x = p.x + 1
      assert(p.x == ops + 1)
    else                                        -- nested spawn (shallow)
      if ops % 997 == 0 then
        local s = th.spawn(function(a) return a * 2 end, ops)
        local ok, v = s:join()
        assert(ok and v == ops * 2)
      end
    end
    if ops % 5000 == 0 then collectgarbage("step") end
  end
  stopch:send(ops)
  return ops
end

print(("kitchen: %d threads, %ds, seed %d"):format(NT, SECS, SEED))
local ts = {}
for i = 1, NT do ts[i] = th.spawn(worker, i) end

-- main thread: periodic full collections + a torture window
local total = 0
local torture_done = false
for i = 1, NT do
  if not torture_done and i == 2 then
    collectgarbage("torture", 1)
    th.sleep(math.min(5, SECS / 4))
    collectgarbage("torture", 0)
    torture_done = true
  end
  total = total + stopch:recv()
end
for i = 1, NT do assert(ts[i]:join()) end
collectgarbage("collect")
print(("kitchen OK: %d total ops, cells=%d, heap=%.0f KB")
      :format(total, shared.cells.get(), collectgarbage("count")))
