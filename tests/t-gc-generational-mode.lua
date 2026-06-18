local function stats_mode()
  local stats = collectgarbage("stats")
  assert(type(stats) == "table")
  assert(type(stats.generational) == "number")
  assert(type(stats.cycle_minor_requested) == "number")
  assert(type(stats.cycle_sweep_minor) == "number")
  assert(type(stats.minor_sweep_enabled) == "number")
  assert(type(stats.major_cycle_starts) == "number")
  assert(type(stats.minor_cycle_requests) == "number")
  assert(type(stats.minor_sweep_deferred) == "number")
  assert(type(stats.minor_sweep_arenas) == "number")
  assert(type(stats.remembered_barriers) == "number")
  assert(type(stats.remembered_pushed) == "number")
  assert(type(stats.remembered_overflows) == "number")
  assert(type(stats.remembered_drained) == "number")
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
assert(after_collect.cycle_sweep_minor == 0)
assert(after_collect.major_cycle_starts >= before_collect.major_cycle_starts)
assert(after_collect.minor_cycle_requests == before_collect.minor_cycle_requests)

local _, before_remember = stats_mode()
local holder = {}
for i = 1, 64 do
  holder[i] = {i}
end
local _, after_remember = stats_mode()
assert(after_remember.remembered_barriers > before_remember.remembered_barriers)
assert(after_remember.remembered_pushed > before_remember.remembered_pushed)
assert(after_remember.remembered_overflows >= before_remember.remembered_overflows)
assert(after_remember.remembered_drained >= before_remember.remembered_drained)
assert(after_remember.minor_sweep_deferred >= before_remember.minor_sweep_deferred)
assert(after_remember.minor_sweep_arenas >= before_remember.minor_sweep_arenas)

assert(collectgarbage("incremental") == "generational")
assert(stats_mode() == 0)

assert(collectgarbage("incremental") == "incremental")
assert(stats_mode() == 0)

print("t-gc-generational-mode OK")
