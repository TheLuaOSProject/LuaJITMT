local th = require "threading"
local ready, requests, replies = th.channel(1), th.channel(1), th.channel(1)
local ring = {}
for i = 1, 32 do ring[i] = { i } end
local child

local function start(enabled)
  if enabled == 0 then return end
  child = th.spawn(function(rdy, req, ack, live)
    baseline_check_live(live)
    assert(rdy:send(1, 10) == true)
    while true do
      local value, ok = req:recv(30)
      assert(ok == true)
      if value == 0 then break end
      baseline_check_live(live)
      assert(ack:send(value, 10) == true)
    end
    baseline_check_live(live)
    return true
  end, ready, requests, replies, baseline_live)
  local value, ok = ready:recv(10)
  assert(ok == true and value == 1)
end

local function ping(round)
  if not child then return end
  assert(requests:send(round, 10) == true)
  local value, ok = replies:recv(10)
  assert(ok == true and value == round)
end

local function stop()
  if not child then return end
  assert(requests:send(0, 10) == true)
  local ok, result = child:join(10)
  assert(ok == true and result == true)
  child = nil
end

local function workers(count)
  th.gcworkers(count)
  assert(th.gcworkers() == count)
end

local function automatic()
  local n = 0
  for burst = 1, 64 do
    for i = 1, 4096 do
      n = n + 1
      ring[i % 32 + 1] = { n } -- Escaping objects retain a fixed live set.
    end
    if baseline_auto_done() then return n, true end
    -- Real native waits give configured GC workers time to drain each burst.
    -- No explicit collection/step or internal collector function is called.
    for wait = 1, 8 do
      th.sleep(0.002)
      if baseline_auto_done() then return n, true end
    end
  end
  return n, false
end

return start, ping, stop, workers, automatic
