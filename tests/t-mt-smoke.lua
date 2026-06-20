local th = require"threading"
local harness = require"thread_harness"

local max_threads = tonumber(os.getenv("LJ_M4_MT_SMOKE_THREADS") or "")
if not max_threads then
  max_threads = th.cpucount()
  if max_threads > 8 then max_threads = 8 end
end
if max_threads < 1 then max_threads = 1 end

local iters = tonumber(os.getenv("LJ_M4_MT_SMOKE_ITERS") or "50000")

local function compute(worker, n)
  local acc = worker * 17 + 3
  for i = 1, n do
    acc = (acc + ((i + worker) * (worker + 11)) % 9973) % 1000003
  end
  return acc
end

for nthreads = 1, max_threads do
  local threads = {}
  local expect = {}
  for i = 1, nthreads do
    expect[i] = compute(i, iters)
    threads[i] = th.spawn(function(worker, n)
      return worker, compute(worker, n)
    end, i, iters)
  end
  harness.join_each(threads, function(worker, i, result)
    assert(worker == i)
    assert(result == expect[i])
    assert(threads[i]:running() == false)
  end)
  harness.fullgc(1)
end

print(("t-mt-smoke OK: 1..%d pure-compute thread sets"):format(max_threads))
