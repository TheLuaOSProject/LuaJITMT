local util = require("jit.util")

local function trace_count(limit)
  local n = 0
  for tr = 1, limit do
    if util.traceinfo(tr) then n = n + 1 end
  end
  return n
end
jit.off(trace_count, true)

local function side_exit_sum(n, pivot, replacement)
  local sum = 0
  for i = 1, n do
    if i == pivot then
      sum = sum + replacement
    else
      sum = sum + i
    end
  end
  return sum
end

-- Re-record several roots so a successful result cannot depend on stale
-- jit_State recorder fields. x64 exits must restore from the L/parent/exit
-- values passed by the exit stub, including Win64's packed parent/exit pair.
for generation = 1, 3 do
  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=100000")
  for _ = 1, 40 do
    assert(side_exit_sum(96, 0, 0) == 4656)
  end
  assert(trace_count(256) > 0, "expected root trace")

  local exits = {}
  local function exit_hook(parent, exitno)
    assert(type(parent) == "number" and type(exitno) == "number")
    exits[exitno] = true
  end
  jit.off(exit_hook, true)
  jit.attach(exit_hook, "texit")

  for round = 1, 160 do
    local pivot = (round * 29) % 96 + 1
    local replacement = (generation * 101 + round * 17) % 211 - 100
    local expected = 4656 - pivot + replacement
    assert(side_exit_sum(96, pivot, replacement) == expected)
  end
  jit.attach(exit_hook)

  local distinct = 0
  for _ in pairs(exits) do distinct = distinct + 1 end
  assert(distinct >= 2,
         "expected both loop-completion and guarded branch side exits")
end

print("t-jit-explicit-exit OK: x64 exits restore from explicit state")
