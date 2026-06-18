local function stats_mode()
  local stats = collectgarbage("stats")
  assert(type(stats) == "table")
  assert(type(stats.generational) == "number")
  return stats.generational
end

local prev = collectgarbage("incremental")
assert(prev == "incremental" or prev == "generational")
assert(stats_mode() == 0)

assert(collectgarbage("generational") == "incremental")
assert(stats_mode() == 1)

assert(collectgarbage("generational") == "generational")
assert(stats_mode() == 1)

assert(collectgarbage("incremental") == "generational")
assert(stats_mode() == 0)

assert(collectgarbage("incremental") == "incremental")
assert(stats_mode() == 0)

print("t-gc-generational-mode OK")
