local th = require"threading"
local harness = require"thread_harness"
local trace_count = require"jit_harness".trace_count
local util = require"jit.util"
local jit_flush = jit.flush
local jit_opt_start = jit.opt.start

local nthreads = harness.env_number("LJ_M6_JIT_FLUSH_THREAD_THREADS", 3)
local rounds = harness.env_number("LJ_M6_JIT_FLUSH_THREAD_ROUNDS", 16)
local churn = harness.env_number("LJ_M6_JIT_FLUSH_THREAD_CHURN", rounds * 2)
local trace_limit = harness.env_number("LJ_M6_JIT_FLUSH_THREAD_TRACE_LIMIT", 128)
local ready_timeout = harness.env_number("LJ_M6_JIT_FLUSH_THREAD_READY_TIMEOUT", 20)
local join_timeout = harness.env_number("LJ_M6_JIT_FLUSH_THREAD_JOIN_TIMEOUT", 40)

local function expected(seed, flag)
  local extra = flag and (17 + 34 + 51 + 68) * 6 or 0
  return seed + 3240 + extra
end
jit.off(expected, true)

local function hot_branch(n, flag, seed)
  local s = seed
  for i = 1, n do
    if flag and i % 17 == 0 then
      s = s + i * 7
    else
      s = s + i
    end
  end
  return s
end

local function heat_pair(seed)
  for _ = 1, 12 do
    local got = hot_branch(80, false, seed)
    local want = expected(seed, false)
    assert(got == want, "cold branch mismatch seed=" .. tostring(seed) ..
	   " got=" .. tostring(got) .. " want=" .. tostring(want))
  end
  for _ = 1, 12 do
    local got = hot_branch(80, true, seed)
    local want = expected(seed, true)
    assert(got == want, "hot branch mismatch seed=" .. tostring(seed) ..
	   " got=" .. tostring(got) .. " want=" .. tostring(want))
  end
end
jit.off(heat_pair, true)

local function live_trace_count()
  return trace_count(trace_limit)
end
jit.off(live_trace_count, true)

local function flush_one_live()
  for tr = trace_limit, 1, -1 do
    if util.traceinfo(tr) then
      local ok, err = pcall(jit_flush, tr)
      assert(ok, tostring(err))
      return true
    end
  end
  return false
end
jit.off(flush_one_live, true)

jit_flush()
jit_opt_start("hotloop=1", "hotexit=1", "minstitch=1")
heat_pair(1000)
assert(live_trace_count() >= 2, "pre-thread root/side trace pair did not form")

local ready, start = harness.channels(nthreads)
local workers = {}

for id = 1, nthreads do
  workers[id] = th.spawn(function(ready_ch, start_ch, worker, count)
    assert(type(worker) == "number", "missing worker id")
    assert(type(count) == "number", "missing worker round count")
    heat_pair(worker * 100000)
    assert(live_trace_count() > 0,
	   "worker did not publish traces before flush race")
    assert(ready_ch:send(worker, ready_timeout) == true)
    local token, ok = start_ch:recv(ready_timeout)
    assert(ok == true and token == "go")

    local observed = 0
    for r = 1, count do
      local seed = worker * 1000000 + r
      heat_pair(seed)
      observed = observed + live_trace_count()
      if r % 2 == 0 then flush_one_live() end
      if r % 3 == 0 then jit_flush(1) end
      if r % 7 == 0 then jit_flush() end
      if r % 5 == 0 then collectgarbage("step", 20) end
    end

    return observed
  end, ready, start, id, rounds)
end

harness.wait_ready(ready, nthreads, ready_timeout, "jit flush thread stress")
harness.release_start(start, nthreads, ready_timeout)

local short_lived = 0
for r = 1, churn do
  local seed = 5000000 + r
  local worker = th.spawn(function(worker_seed)
    assert(type(worker_seed) == "number", "missing churn seed")
    heat_pair(worker_seed)
    if worker_seed % 3 == 0 then flush_one_live() end
    if worker_seed % 5 == 0 then jit_flush() end
    return expected(worker_seed, true)
  end, seed)
  if r % 2 == 0 then
    flush_one_live()
  else
    jit_flush()
  end
  local ok, result = worker:join(join_timeout)
  assert(ok == true, tostring(result))
  assert(result == expected(seed, true))
  short_lived = short_lived + 1
end

local observed = harness.join_count(workers, join_timeout)
assert(observed > 0, "workers did not observe live traces during flush race")

jit_flush()
collectgarbage("collect")
collectgarbage("collect")
assert(live_trace_count() == 0, "full flush left visible live traces")

print("t-jit-flush-thread-stress OK: " .. tostring(nthreads) ..
      " workers, " .. tostring(rounds) .. " rounds, " ..
      tostring(short_lived) .. " short-lived threads")
