local th = require"threading"
local harness = require"thread_harness"
local util = require"jit.util"

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "sizemcode=4", "maxmcode=2048")

local rounds = harness.env_number("LJ_M6_JIT_UTIL_FLUSH_RACE_ROUNDS", 48)
local ready, start = harness.channels(1)
local done = th.channel(1)

local function make_hot(seed)
  assert(type(seed) == "number")
  local src = "return function(n) local s=" .. seed ..
    "; for i=1,n do s=s+i end return s end"
  return assert(loadstring(src))()
end

local function publish_trace(seed)
  local f = make_hot(seed)
  for _ = 1, 12 do
    assert(f(64) == seed + 2080)
  end
end

local function call_ok(fn)
  local ok, err = pcall(fn)
  assert(ok, err)
end

local function probe_trace(tr)
  local info = util.traceinfo(tr)
  if not info then
    call_ok(function() util.traceir(tr, 1) end)
    call_ok(function() util.tracek(tr, -1) end)
    call_ok(function() util.tracesnap(tr, 0) end)
    call_ok(function() util.tracemc(tr) end)
    call_ok(function() util.traceexitstub(tr, 0) end)
    return 0
  end

  assert(type(info.nins) == "number")
  assert(type(info.nk) == "number")
  assert(type(info.nexit) == "number")
  assert(type(info.linktype) == "string")

  util.traceir(tr, 1)
  util.traceir(tr, info.nins)
  util.tracek(tr, -1)
  if info.nk > 0 then util.tracek(tr, -info.nk) end
  util.tracesnap(tr, 0)
  if info.nexit > 0 then util.tracesnap(tr, info.nexit - 1) end
  util.tracemc(tr)
  util.traceexitstub(tr, 0)
  return 1
end
jit.off(probe_trace, true)

local worker = th.spawn(function(ready_ch, start_ch, done_ch, count)
  jit.opt.start("hotloop=1", "hotexit=1", "sizemcode=4", "maxmcode=2048")
  ready_ch:send("ready")
  local token, ok = start_ch:recv(10)
  assert(ok == true and token == "go")
  for r = 1, count do
    jit.flush()
    publish_trace(r * 10000)
    if r % 8 == 0 then collectgarbage("step", 20) end
  end
  done_ch:send("done")
  return true
end, ready, start, done, rounds)

harness.wait_ready(ready, 1)
harness.release_start(start, 1)

local probes = 0
local live_seen = 0
local finished = false
while not finished do
  local token, ok = done:recv(0.001)
  if ok == true then
    assert(token == "done")
    finished = true
  end
  publish_trace(900000 + probes)
  for tr = 1, 96 do
    live_seen = live_seen + probe_trace(tr)
  end
  probes = probes + 1
end

local ok, result = worker:join(20)
assert(ok == true and result == true)
assert(probes > 0)
assert(live_seen > 0)

print(("t-jit-util-flush-race OK: %d probe rounds, %d live snapshots"):format(
  probes, live_seen))
