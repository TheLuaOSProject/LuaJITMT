-- Exercise the standalone frontend's protected script entry.
local th = require("threading")
local mode = assert(arg[1])
assert(th.gcmode(mode))
assert(th.gcworkers(2) == 0)
jit.opt.start("hotloop=4", "hotexit=3")

local marker = { value = ("kept on the worker"):rep(1024), tag = 173 }
local weak = setmetatable({marker}, {__mode = "v"})
local state, ready = {false}, th.channel(1)
local peer = th.spawn(function(state, ready, marker)
  local function sum_window()
    local sum = 0
    for i = 1, 2048 do sum = sum + i end
    return sum
  end
  local windows = 0
  for _ = 1, 10 do assert(sum_window() == 2098176) end
  assert(ready:send(true, 10))
  repeat
    assert(sum_window() == 2098176)
    assert(#marker.value == 18432 and marker.tag == 173)
    windows = windows + 1
  until state[1]
  return windows
end, state, ready, marker)
assert(select(2, ready:recv(10)))

local ring, high = {}, 0
local before = th.gcstats()
for round = 1, 64 do
  for i = 1, 1800 do
    ring[(i-1) % 32 + 1] = ("%d/%d cli-auto:"):format(round, i):rep(252)
  end
  assert(#ring == 32 and weak[1] == marker and marker.tag == 173)
  assert(ring[8] == ("%d/1800 cli-auto:"):format(round):rep(252))
  assert(peer:running() and th.gcworkers() == 2)
  if round > 8 then
    local bytes = th.gcstats().total_bytes
    high = bytes >= 96 * 1024 * 1024 and high + 1 or 0
    assert(high < 8, ("CLI automatic heap stayed high at round %d (%d bytes)"):format(
      round, bytes))
  end
end
local after = th.gcstats()
assert(collectgarbage("isrunning"))
assert(after.generational == (mode == "generational" and 1 or 0))
assert(after.sweep_live_updates > before.sweep_live_updates)
assert(after.worker_async_progress > before.worker_async_progress)
state[1] = true
local joined, windows = peer:join(10)
assert(joined and windows > 0, tostring(windows))
print(("t-str-reclaim-auto-cli %s OK: %d bytes, %d collections, %d peer windows"):format(
  mode, after.total_bytes, after.sweep_live_updates - before.sweep_live_updates, windows))
