local function stats_mode()
  local stats = collectgarbage("stats")
  assert(type(stats) == "table")
  assert(type(stats.generational) == "number")
  assert(type(stats.cycle_minor_requested) == "number")
  assert(type(stats.major_cycle_starts) == "number")
  assert(type(stats.minor_cycle_requests) == "number")
  return stats.generational, stats
end

local prev = collectgarbage("incremental")
assert(prev == "incremental" or prev == "generational")
assert(stats_mode() == 0)

assert(collectgarbage("generational") == "incremental")
assert(stats_mode() == 1)

assert(collectgarbage("generational") == "generational")
assert(stats_mode() == 1)

local _, before_collect = stats_mode()
collectgarbage("collect")
local _, after_collect = stats_mode()
assert(after_collect.cycle_minor_requested == 0)
assert(after_collect.major_cycle_starts >= before_collect.major_cycle_starts)
assert(after_collect.minor_cycle_requests == before_collect.minor_cycle_requests)

assert(collectgarbage("incremental") == "generational")
assert(stats_mode() == 0)

assert(collectgarbage("incremental") == "incremental")
assert(stats_mode() == 0)

print("t-gc-generational-mode OK")
