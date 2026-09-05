local th = require("threading")

local workers = tonumber(os.getenv("LJ_M4_HOOK_WORKERS") or "4")
local ready = th.channel(workers)
local start = th.channel(workers)
local clear = th.channel(workers)
local done = th.channel(workers)
local hits = th.channel(workers * 4096)

local main_id = th.current():id()

local function busy(id, rounds, inner)
  local sum = 0
  for round = 1, rounds do
    for i = 1, inner do
      sum = sum + i + id + round
    end
    th.sleep(0)
  end
  return sum
end

local function worker(id, ready_ch, start_ch, clear_ch, done_ch)
  local worker_id = th.current():id()
  assert(ready_ch:send(worker_id, 10) == true)
  local token, ok = start_ch:recv(10)
  assert(ok == true and token == "go")

  local sum = 0
  while true do
    sum = sum + busy(id, 2, 400)
    local clear_token, clear_ok = clear_ch:recv(0)
    if clear_ok == true then
      assert(clear_token == "cleared")
      break
    end
  end

  sum = sum + busy(id, 120, 700)
  assert(done_ch:send({ worker_id, sum }, 10) == true)
end

local function hook(ev)
  if ev == "count" then
    local current = th.current()
    hits:send(current and current:id() or -1, 0)
  end
end

local threads = {}
local expected = {}
local expected_n = 0
for i = 1, workers do
  threads[i] = th.spawn(worker, i, ready, start, clear, done)
end

for _ = 1, workers do
  local id, ok = ready:recv(10)
  assert(ok == true and type(id) == "number")
  expected[id] = true
  expected_n = expected_n + 1
end

debug.sethook(hook, "", 100)
do
  local fn, mask, count = debug.gethook()
  assert(fn == hook and mask == "" and count == 100)
end

for _ = 1, workers do
  assert(start:send("go", 10) == true)
end

local seen = {}
local seen_n = 0
local deadline = th.now() + 10
while seen_n < expected_n and th.now() < deadline do
  local id, ok = hits:recv(0.1)
  if ok == true and expected[id] and not seen[id] then
    seen[id] = true
    seen_n = seen_n + 1
  end
end
assert(seen_n == expected_n, "hook did not reach every live worker")

debug.sethook()
do
  local fn, mask, count = debug.gethook()
  assert(fn == nil and mask == "" and count == 0)
end

th.sleep(0)
while true do
  local _, ok = hits:recv(0)
  if ok ~= true then break end
end

for _ = 1, workers do
  assert(clear:send("cleared", 10) == true)
end

for _ = 1, workers do
  local result, ok = done:recv(10)
  assert(ok == true and expected[result[1]] and result[2] > 0)
end

local after_clear, after_ok = hits:recv(0)
assert(after_clear == nil and after_ok == "timeout")

for i = 1, workers do
  assert(threads[i]:join())
end

assert(not seen[main_id], "worker hook accounting included main thread")
print("t-threading-hooks OK: debug hook redispatch across OS threads")
