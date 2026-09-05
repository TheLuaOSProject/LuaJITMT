local jit = require"jit"

if jit.arch ~= "x64" or (jit.os ~= "Linux" and jit.os ~= "OSX") then
  print("t-profile-single-trace SKIP: unsupported signal target")
  return
end

local ok, profile = pcall(require, "jit.profile")
if not ok then
  print("t-profile-single-trace SKIP: jit.profile unavailable")
  return
end

local util = require"jit.util"

-- Internal profiler policy retirement must be eventless. A user TRACE handler
-- may reenter profile.stop(); invoking it while state is STARTING/STOPPING
-- would self-wait forever. Public jit.flush() still delivers the event after
-- the profile lifecycle is stable.
do
  local flush_events = 0
  local function flush_hook(ev)
    if ev == "flush" then
      flush_events = flush_events + 1
      profile.stop()
    end
  end
  jit.attach(flush_hook, "trace")
  profile.start("i1000", function() end)
  assert(flush_events == 0)
  profile.stop()
  assert(flush_events == 0)
  jit.flush()
  assert(flush_events == 1)
  jit.attach(flush_hook)
end

local function live_trace()
  for tr = 1, 1000 do
    if util.traceinfo(tr) then return tr end
  end
end

local function exercise(optlevel, label)
  local done = false
  local callbacks = 0
  local n = 0

  jit.flush()
  jit.on()
  if optlevel == nil then
    jit.opt.start("hotloop=1")
  else
    jit.opt.start(optlevel, "hotloop=1")
  end
  profile.start("i20", function(thread, samples, vmstate)
    assert(type(thread) == "thread")
    assert(type(samples) == "number" and samples > 0)
    assert(type(vmstate) == "string" and #vmstate == 1)
    callbacks = callbacks + 1
    done = true
  end)

  -- This loop has no call/exit edge of its own after tracing. SIGPROF must make
  -- a qword XPOLL leave both optimized IR_LOOP code and an optimizer-disabled
  -- self-link so owner context can install and run the callback hook.
  while not done do
    n = n + 1
  end

  local trace = live_trace()
  profile.stop()
  assert(trace ~= nil, label .. ": callback fired before trace compilation")
  assert(callbacks > 0 and n > 0)
  return callbacks, n
end

local default_callbacks, default_n = exercise(nil, "default optimizer")
local opt0_callbacks, opt0_n = exercise(0, "optimizer level 0")

print(("t-profile-single-trace OK: default=%d/%d opt0=%d/%d"):format(
  default_callbacks, default_n, opt0_callbacks, opt0_n))
