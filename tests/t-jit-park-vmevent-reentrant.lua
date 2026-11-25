local th = require"threading"

local function make_trace(n)
  local sum = 0
  for i = 1, n do sum = sum + i end
  return sum
end

local function trigger_trace()
  for _ = 1, 40 do assert(make_trace(80) == 3240) end
end

local function channel_park_from_trace_event(event, phase, nested)
  local entered = th.channel(1)
  local release = th.channel(1)
  local armed = true
  local received = false
  local terminal = phase == "stop" or phase == "abort"

  local function trace_hook(ev, _, fn)
    if armed and (event == "record" or ev == phase) and
       (not terminal or fn == make_trace) then
      armed = false
      assert(entered:send("entered", 1) == true)
      local function wait()
        return release:recv(2)
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
  if phase == "abort" then jit.opt.start("maxrecord=1") end
  jit.attach(trace_hook, event)
  local function flush_and_release(entered_ch, release_ch)
    local token, ok = entered_ch:recv(1)
    assert(ok == true and token == "entered")
    jit.flush()
    assert(release_ch:send("release", 1) == true)
    return true
  end

  if terminal then
    local recorder = th.spawn(trigger_trace)
    flush_and_release(entered, release)
    assert(recorder:join(5) == true, "recorder worker did not finish")
  else
    local peer = th.spawn(flush_and_release, entered, release)
    trigger_trace()
    local joined, result = peer:join(5)
    assert(joined == true and result == true,
           "channel-event peer did not finish after recorder unwind")
  end
  jit.attach(trace_hook)
  assert(received, (phase or event) .. " callback did not receive peer release")
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
      local joined, result = peer:join(2)
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
  assert(finished, "join did not finish inside the callback")
end

channel_park_from_trace_event("trace", "start")
channel_park_from_trace_event("record", nil, true)
join_park_from_trace_event()
channel_park_from_trace_event("trace", "stop")
-- Run last because the deliberately small recording budget persists.
channel_park_from_trace_event("trace", "abort")
print("t-jit-park-vmevent-reentrant OK")
