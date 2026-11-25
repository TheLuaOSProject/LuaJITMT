local threading = require"threading"

if jit then jit.off() end

local done = { false }
local ready = threading.channel(1)
local held = ("held-string-"):rep(200)
local reader = assert(threading.spawn(function(done, ready, held)
  assert(ready:send(true, 10))
  local sum = 0
  while not done[1] do
    sum = sum + #held
    if sum > 1000000000 then sum = 0 end
  end
  return #held
end, done, ready, held))
local started, received = ready:recv(10)
assert(received and started)

local sweeps = threading.gcstats().sweep_live_updates
for round = 1, 6 do
  for i = 1, 800 do
    local s = ("%d-%d:"):format(round, i):rep(20)
    assert(#s >= 80)
  end
  collectgarbage("collect")
end
local stats = threading.gcstats()
assert(stats.sweep_live_updates > sweeps, "no sweep completed while the reader ran")
assert(stats.phase == 0, "collector did not finish while the reader ran")
assert(held == ("held-string-"):rep(200))

done[1] = true
local joined, n = reader:join(10)
assert(joined and n == #held)
print("t-gc2-sweep-active-reader OK")
