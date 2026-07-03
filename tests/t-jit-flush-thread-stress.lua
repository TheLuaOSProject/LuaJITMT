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
local progress = th.channel(nthreads + churn + 16)
local progress_log = {}
local progress_seen = 0
local started_at = th.now() or 0

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

local function elapsed()
  local now = th.now()
  if not now or started_at == 0 then return 0 end
  return now - started_at
end
jit.off(elapsed, true)

local function record_progress(ev)
  progress_seen = progress_seen + 1
  progress_log[#progress_log + 1] = ev
  if #progress_log > 24 then table.remove(progress_log, 1) end
end
jit.off(record_progress, true)

local function drain_progress()
  while true do
    local ev, ok = progress:peek()
    if ok ~= true then break end
    ev, ok = progress:recv(0)
    if ok ~= true then break end
    record_progress(ev)
  end
end
jit.off(drain_progress, true)

local function publish_progress(ch, ev)
  ev.t = elapsed()
  local ok = ch:send(ev, 0)
  return ok == true
end
jit.off(publish_progress, true)

local function progress_summary()
  drain_progress()
  local out = {
    "events=" .. tostring(progress_seen),
    "elapsed=" .. tostring(elapsed()),
    "live_traces=" .. tostring(live_trace_count())
  }
  local stats_ok, stats = pcall(th.gcstats)
  if stats_ok and type(stats) == "table" then
    out[#out + 1] = "gc_phase=" .. tostring(stats.phase)
    out[#out + 1] = "worker_runs=" .. tostring(stats.worker_runs)
    out[#out + 1] = "smr_reclaimed=" .. tostring(stats.smr_reclaimed)
  end
  for i = 1, #progress_log do
    local ev = progress_log[i]
    out[#out + 1] = string.format(
      "%s id=%s round=%s traces=%s churn=%s t=%.6f",
      tostring(ev.kind), tostring(ev.id), tostring(ev.round),
      tostring(ev.traces), tostring(ev.churn), tonumber(ev.t) or 0)
  end
  return table.concat(out, "\n")
end
jit.off(progress_summary, true)

local function join_checked(worker, label, timeout)
  drain_progress()
  local joined = { worker:join(timeout) }
  drain_progress()
  if joined[1] ~= true then
    error(label .. " join failed: " .. tostring(joined[2]) ..
	  "\n" .. progress_summary(), 0)
  end
  return unpack(joined, 2)
end
jit.off(join_checked, true)

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
  workers[id] = th.spawn(function(ready_ch, start_ch, progress_ch, worker, count)
    assert(type(worker) == "number", "missing worker id")
    assert(type(count) == "number", "missing worker round count")
    local preheat_ok, preheat_err = pcall(function()
      heat_pair(worker * 100000)
      local traces = live_trace_count()
      assert(traces > 0,
	     "worker did not publish traces before flush race")
      publish_progress(progress_ch, {
	kind = "preheat",
	id = worker,
	round = 0,
	traces = traces
      })
    end)
    assert(ready_ch:send(preheat_ok and worker or -worker, ready_timeout) == true)
    if not preheat_ok then error(preheat_err, 0) end
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
      if r == count or r % 8 == 0 then
	publish_progress(progress_ch, {
	  kind = "worker",
	  id = worker,
	  round = r,
	  traces = live_trace_count()
	})
      end
    end

    return observed
  end, ready, start, progress, id, rounds)
end

harness.wait_ready(ready, nthreads, ready_timeout, "jit flush thread stress")
harness.release_start(start, nthreads, ready_timeout)
drain_progress()

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
  local result = join_checked(worker, "churn " .. tostring(r), join_timeout)
  assert(result == expected(seed, true))
  short_lived = short_lived + 1
  if r == churn or r % 16 == 0 then
    record_progress({
      kind = "churn",
      churn = r,
      round = r,
      traces = live_trace_count(),
      t = elapsed()
    })
  end
end

local observed = 0
for i = 1, #workers do
  local result = join_checked(workers[i], "worker " .. tostring(i),
			      join_timeout)
  assert(type(result) == "number", "worker returned non-number observation")
  observed = observed + result
end
assert(observed > 0, "workers did not observe live traces during flush race")

jit_flush()
collectgarbage("collect")
collectgarbage("collect")
assert(live_trace_count() == 0, "full flush left visible live traces")

print("t-jit-flush-thread-stress OK: " .. tostring(nthreads) ..
      " workers, " .. tostring(rounds) .. " rounds, " ..
      tostring(short_lived) .. " short-lived threads")
