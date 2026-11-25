local th = require"threading"
local detached_park = jit.arch == "x64" and jit.os == "Linux"

local function make_trace(n)
  local sum = 0
  for i = 1, n do sum = sum + i end
  return sum
end

local function trigger_trace()
  for _ = 1, 40 do assert(make_trace(80) == 3240) end
end

local function channel_park_from_trace_event(event, nested)
  local entered = th.channel(1)
  local release = th.channel(1)
  local armed = true
  local received = false

  local function trace_hook(ev)
    if armed and (event == "record" or ev == "start") then
      armed = false
      assert(entered:send("entered", 1) == true)
      local function wait()
        return release:recv(detached_park and 2 or 0.05)
      end
      local token, status
      if nested then
        token, status = coroutine.wrap(wait)()
      else
        token, status = wait()
      end
      received = token == "release" and status == true
    end
  end
  jit.off(trace_hook, true)

  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")
  jit.attach(trace_hook, event)
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
  assert(received == detached_park, "channel park did not have expected progress")
end

local function join_park_from_trace_event()
  local entered = th.channel(1)
  local armed = true
  local finished = false
  local peer

  local function trace_hook(ev)
    if armed and ev == "start" then
      armed = false
      assert(entered:send("entered", 1) == true)
      local joined, result = peer:join(detached_park and 2 or 0.05)
      finished = joined == true and result == true
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
  assert(finished == detached_park, "join park did not have expected progress")
end

channel_park_from_trace_event("trace")
if detached_park then channel_park_from_trace_event("record", true) end
join_park_from_trace_event()
print("t-jit-park-vmevent-reentrant OK")
