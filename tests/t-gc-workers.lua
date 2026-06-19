local function workers(...)
  return collectgarbage("workers", ...)
end

collectgarbage("workers", 0)
assert(workers() == 0)
assert(workers(0) == 0)
assert(workers() == 0)

local stats0 = collectgarbage("stats")
assert(type(stats0.worker_wakes) == "number")
assert(type(stats0.worker_async_progress) == "number")

assert(collectgarbage("workers", 1) == 0)
assert(workers() == 1)
assert(workers(2) == 1)
assert(workers() == 1)

collectgarbage("collect")
local stats1 = collectgarbage("stats")
assert(stats1.worker_wakes >= stats0.worker_wakes)
assert(stats1.worker_async_progress >= stats0.worker_async_progress)

assert(workers(0) == 1)
assert(workers() == 0)
assert(workers(0) == 0)

print("t-gc-workers OK")
