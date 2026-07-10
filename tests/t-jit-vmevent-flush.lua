local th = require("threading")
local trace_count = require("jit_harness").trace_count

local function exercise(expect_seen)
  local seen = 0
  local function hook(ev)
    assert(ev == "flush", ev)
    seen = seen + 1
    local s = 0
    for i = 1, 8 do s = s + i end
    assert(s == 36)
  end
  jit.attach(hook, "trace")
  jit.flush()
  jit.attach(hook)
  assert(seen == expect_seen, seen)
end

exercise(1)

local worker = th.spawn(function()
  -- The safepoint leader flushes eventlessly, then the initiating state emits
  -- the public TRACE "flush" event after it regains ownership while retaining
  -- the recorder token through the callback.
  exercise(1)
  return true
end)

assert(worker:join(20) == true)

-- Hold a flush callback open while a peer tries to start recording. Before the
-- flush callback retained the recorder token, the peer could enter the recorder
-- and publish J->L; lj_vmevent_call() then restored the flushing state over that
-- live owner. No trace may publish until the callback releases the token; the
-- same peer must record normally immediately afterward.
local flush_entered = th.channel(1)
local flush_release = th.channel(1)
local recorder_first_done = th.channel(1)
local recorder_retry = th.channel(1)

local function overlap_hook(ev)
  if ev == "flush" then
    assert(flush_entered:send(true, 5) == true)
    local token, ok = flush_release:recv(5)
    assert(ok == true and token == "go")
  end
end
jit.off(overlap_hook, true)

local function record_after_flush()
  local total = 0
  for round = 1, 40 do
    for i = 1, 80 do total = total + i end
  end
  return total
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.attach(overlap_hook, "trace")
local flusher = th.spawn(function()
  jit.flush()
  return true
end)
assert(select(2, flush_entered:recv(5)) == true)
local recorder = th.spawn(function(first_done, retry)
  local first = record_after_flush()
  assert(first_done:send(true, 5) == true)
  local token, ok = retry:recv(5)
  assert(ok == true and token == "go")
  return first + record_after_flush()
end, recorder_first_done, recorder_retry)
assert(select(2, recorder_first_done:recv(5)) == true)
assert(trace_count(64) == 0,
       "peer recorder published while TRACE flush callback held the JIT token")
assert(flush_release:send("go", 5) == true)
assert(recorder_retry:send("go", 5) == true)
assert(flusher:join(20) == true)
local ok, result = recorder:join(20)
assert(ok == true, result)
assert(result == 2 * 40 * 3240, result)
assert(trace_count(64) > 0, "peer did not record after flush callback released")
jit.attach(overlap_hook)

-- A recorder TRACE callback and a peer native trace exit used to share the
-- universe vmthread stack. Keep the recorder callback open, drive many TEXIT
-- opportunities on another TG, then prove both states and the event registry
-- remain usable. VM events now build arguments on their initiating L and a
-- single nonwaiting callback owner suppresses a colliding observational event.
jit.flush()
jit.opt.start("hotloop=1", "hotexit=100000")

local function exit_target(n, flip)
  local s = 0
  for i = 1, n do s = s + i end
  if flip then s = s + 1 end
  return s
end
for _ = 1, 30 do assert(exit_target(80, false) == 3240) end
assert(trace_count(64) > 0, "expected precompiled TEXIT target")

local event_entered = th.channel(1)
local event_release = th.channel(1)
local event_armed = true
local texit_seen = 0

local function serialized_trace_hook(ev)
  if event_armed and ev == "start" then
    event_armed = false
    assert(event_entered:send(true, 5) == true)
    local token, ok = event_release:recv(5)
    assert(ok == true and token == "go")
  end
end
local function serialized_texit_hook()
  texit_seen = texit_seen + 1
end
jit.off(serialized_trace_hook, true)
jit.off(serialized_texit_hook, true)
jit.attach(serialized_trace_hook, "trace")
jit.attach(serialized_texit_hook, "texit")

local recorder_event = th.spawn(function()
  local function new_hot_loop(n)
    local s = 0
    for i = 1, n do s = s + i end
    return s
  end
  for _ = 1, 30 do assert(new_hot_loop(80) == 3240) end
  return true
end)
assert(select(2, event_entered:recv(5)) == true)

local exiting_peer = th.spawn(function()
  local total = 0
  for _ = 1, 1000 do total = total + exit_target(80, true) end
  return total
end)
local exit_ok, exit_total = exiting_peer:join(20)
assert(exit_ok == true, exit_total)
assert(exit_total == 1000 * 3241, exit_total)
assert(texit_seen == 0,
       "peer TEXIT callback overlapped the active TRACE event owner")

assert(event_release:send("go", 5) == true)
assert(recorder_event:join(20) == true)
assert(exit_target(80, true) == 3241)
assert(texit_seen > 0, "TEXIT handler did not recover after owner release")
jit.attach(serialized_trace_hook)
jit.attach(serialized_texit_hook)

print("t-jit-vmevent-flush OK: VM events keep initiating stacks and JIT ownership")
