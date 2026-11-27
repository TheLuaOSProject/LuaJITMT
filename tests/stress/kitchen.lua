-- Seeded shared-heap soak. Arguments are batches, workers, collector workers
-- and seed. LJ_MT_SOAK_SECONDS runs whole batches for a wall-clock interval.
-- LJ_MT_SOAK_GC=continuous adds a mutator requesting major collections.
local th = require("threading")
local ffi = require("ffi")
ffi.cdef("int abs(int);")
local batches = tonumber(arg[1]) or 8
local count = tonumber(arg[2]) or 4
local collectors = tonumber(arg[3]) or 2
local seed = tonumber(arg[4]) or 19
local seconds = tonumber(os.getenv("LJ_MT_SOAK_SECONDS")) or 0
local steps, modulus = 96, 2147483647
assert(batches >= 1 and count >= 1 and collectors >= 0 and seed > 0)
local function advance(n) return n * 48271 % modulus end
local function inspect(r)
  assert(r.value == r.box[0] and r.text == "s" .. r.value)
end

local function batch(id, tick, state, shared)
  local sum = 0
  for i = 1, steps do
    state = advance(state)
    local n, op = state % 100000, state % 6
    if op == 0 then
      local t = {}
      for k = 1, 24 do t["k" .. k] = n + k end
      for k = 2, 24, 2 do t["k" .. k] = nil end
      assert(t.k1 == n + 1 and t.k24 == nil)
    elseif op == 1 then
      local co = coroutine.create(function(value)
        return coroutine.yield(value) + value
      end)
      local ok, v = coroutine.resume(co, n)
      assert(ok and v == n)
      ok, v = coroutine.resume(co, 9)
      assert(ok and v == n + 9)
    elseif op == 2 then
      local value = n
      local function bump() value = value + 1; return value end
      assert(bump() == n + 1 and bump() == n + 2)
    elseif op == 3 then
      assert(ffi.C.abs(-n) == n)
    elseif op == 4 then
      collectgarbage("step", 1)
    else
      local v = { id, tick, n }
      local w = setmetatable({v}, {__mode="v"})
      assert(w[1] == v and v[3] == n)
    end
    if i % 8 == 0 then
      shared[id] = { value=n, text="s" .. n, box=ffi.new("int32_t[1]", n) }
    end
    local peer = shared[state % count + 1]
    if peer then inspect(peer) end
    sum = (sum + n) % modulus
  end
  local receipt = { calls=0 }
  local cd = ffi.gc(ffi.new("int32_t[1]", tick), function(x)
    assert(x[0] == tick)
    receipt.calls = receipt.calls + 1
  end)
  assert(cd[0] == tick)
  return { id=id, sum=sum, state=state, record=shared[id], receipt=receipt }
end

local function work(id, state, shared, inbox, replies)
  while true do
    local tick, received = inbox:recv()
    assert(received)
    if tick == false then return end
    local ok, result = pcall(batch, id, tick, state, shared)
    if not ok then replies:send({id=id, error=result}); return end
    state = result.state
    assert(replies:send(result))
  end
end

local function check_finalizers(receipts)
  local deadline = th.now() + 20
  repeat
    collectgarbage()
    local done = true
    for id = 1, count do
      assert(receipts[id].calls <= 1, "duplicate finalizer for worker " .. id)
      if receipts[id].calls == 0 then done = false end
    end
    if done then return end
    th.sleep(0)
  until th.now() >= deadline
  for id = 1, count do
    assert(receipts[id].calls == 1, "pending finalizer for worker " .. id)
  end
end

th.gcworkers(collectors)
local shared, workers, inboxes, states = {}, {}, {}, {}
local replies = th.channel(count)
for id = 1, count do
  states[id] = (seed + 7919 * id) % modulus
  inboxes[id] = th.channel(1)
  workers[id] = th.spawn(work, id, states[id], shared, inboxes[id], replies)
end
local pressure, stop_pressure = nil, false
if os.getenv("LJ_MT_SOAK_GC") == "continuous" then
  local ready = th.channel(1)
  pressure = th.spawn(function()
    local calls = 0
    repeat
      collectgarbage("collect")
      calls = calls + 1
      if calls == 1 then assert(ready:send(true)) end
      th.sleep(0)
    until stop_pressure
    return calls
  end)
  assert(ready:recv(30) == true, "collector did not start")
end
local deadline = th.now() + seconds
local tick = 0
local previous
repeat
  tick = tick + 1
  for id = 1, count do assert(inboxes[id]:send(tick)) end
  local receipts = {}
  for _ = 1, count do
    local r, received = replies:recv(30)
    assert(received == true, "worker response timed out in batch " .. tick)
    assert(not r.error, r.error)
    assert(not receipts[r.id])
    local state, sum = states[r.id], 0
    for i = 1, steps do
      state = advance(state)
      sum = (sum + state % 100000) % modulus
    end
    assert(r.state == state and r.sum == sum)
    states[r.id], receipts[r.id] = state, r.receipt
    inspect(r.record)
  end
  -- A parked worker can still retain its latest FFI result. Its next batch
  -- has replaced that frame while the previous receipt remains observable.
  if previous then check_finalizers(previous) end
  previous = receipts
until (seconds > 0 and th.now() >= deadline) or (seconds <= 0 and tick >= batches)
for id = 1, count do assert(inboxes[id]:send(false)) end
for id = 1, count do assert(workers[id]:join()) end
if pressure then
  stop_pressure = true
  local ok, calls = pressure:join(30)
  assert(ok and calls > 0, "collector did not stop")
  io.write(("background full collection requests: %d\n"):format(calls))
end
check_finalizers(previous)
th.gcworkers(0)
print(("shared soak passed %d batches, %d workers, %d collectors, seed %d"):
  format(tick, count, collectors, seed))
