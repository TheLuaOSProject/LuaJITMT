local th = require"threading"

local function make_trace(n)
  local sum = 0
  for i = 1, n do sum = sum + i end
  return sum
end

local function trigger_trace()
  for _ = 1, 40 do assert(make_trace(80) == 3240) end
end

local function channel_park_from_trace_event()
  local entered = th.channel(1)
  local release = th.channel(1)
  local armed = true
  local saw_timeout = false

  local function trace_hook(ev)
    if armed and ev == "start" then
      armed = false
      assert(entered:send("entered", 1) == true)
      local token, status = release:recv(0.05)
      saw_timeout = token == nil and status == "timeout"
    end
  end
  jit.off(trace_hook, true)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")
  jit.attach(trace_hook, "trace")
  local peer = th.spawn(function(entered_ch, release_ch)
    local token, ok = entered_ch:recv(1)
    assert(ok == true and token == "entered")
    jit.flush()
    assert(release_ch:send("release", 1) == true)
    return true
  end, entered, release)

  trigger_trace()
  jit.attach(trace_hook)
  local joined, result = peer:join(5)
  assert(joined == true and result == true,
         "channel-event peer did not finish after recorder unwind")
  assert(saw_timeout,
         "channel event park unexpectedly crossed synchronous peer flush")
end

local function join_park_from_trace_event()
  local entered = th.channel(1)
  local armed = true
  local saw_timeout = false
  local peer

  local function trace_hook(ev)
    if armed and ev == "start" then
      armed = false
      assert(entered:send("entered", 1) == true)
      local joined, status = peer:join(0.05)
      saw_timeout = joined ~= true and status == "timeout"
    end
  end
  jit.off(trace_hook, true)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")
  jit.attach(trace_hook, "trace")
  peer = th.spawn(function(entered_ch)
    local token, ok = entered_ch:recv(1)
    assert(ok == true and token == "entered")
    jit.flush()
    return true
  end, entered)

  trigger_trace()
  jit.attach(trace_hook)
  local joined, result = peer:join(5)
  assert(joined == true and result == true,
         "join-event peer did not finish after recorder unwind")
  assert(saw_timeout,
         "join event park unexpectedly crossed synchronous peer flush")
end

channel_park_from_trace_event()
join_park_from_trace_event()
print("t-jit-park-vmevent-reentrant OK: parks defer owner teardown")
