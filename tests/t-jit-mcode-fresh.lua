local th = require"threading"
local harness = require"thread_harness"

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "sizemcode=4", "maxmcode=2048")

local nthreads = tonumber(os.getenv("LJ_M6_MCODE_FRESH_THREADS") or "4")
local rounds = tonumber(os.getenv("LJ_M6_MCODE_FRESH_ROUNDS") or "24")

local function hot(n)
  local s = 0
  for i = 1, n do
    s = s + i
  end
  return s
end

for _ = 1, 20 do
  assert(hot(240) == 28920)
end

local ready, start = harness.channels(nthreads)
local done = th.channel(nthreads)
local workers = {}

for id = 1, nthreads do
  workers[id] = th.spawn(function(ready_ch, start_ch, done_ch, worker, count)
    jit.opt.start("hotloop=1", "hotexit=1", "sizemcode=4", "maxmcode=2048")

    local function make_trace_func(seed)
      local src = ("return function(n) local s=%d; for i=1,n do s=s+i end return s end"):format(seed)
      return assert(loadstring(src))()
    end

    ready_ch:send(worker)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    for r = 1, count do
      local seed = worker * 100000 + r
      local f = make_trace_func(seed)
      for _ = 1, 8 do
	assert(f(48) == seed + 1176)
      end
      if r % 8 == 0 then
	collectgarbage("step", 20)
      end
    end

    done_ch:send(worker)
    return worker
  end, ready, start, done, id, rounds)
end

harness.wait_ready(ready, nthreads)
harness.release_start(start, nthreads)

local finished = 0
while finished < nthreads do
  local _, ok = done:recv(0.001)
  if ok == true then
    finished = finished + 1
  else
    assert(hot(240) == 28920)
  end
end

harness.join_each(workers, function(worker, id)
  assert(worker == id)
end)

print(("t-jit-mcode-reuse OK: %d TGs published traces while main entered shared mcode"):format(nthreads))
