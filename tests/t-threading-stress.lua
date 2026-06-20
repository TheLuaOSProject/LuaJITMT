local th = require"threading"
local harness = require"thread_harness"

local reps = harness.env_number("LJ_M4_THREAD_STRESS_REPS", 1000)

for i = 1, reps do
  local t = th.spawn(function(a, b)
    return a + b, th.current():id()
  end, i, 3)
  local ok, sum, tid = t:join()
  assert(ok == true)
  assert(sum == i + 3)
  assert(tid == t:id())
  if i % 64 == 0 then
    collectgarbage("collect")
  end
end

print(("t-threading-stress OK: %d sequential spawn/join cycles"):format(reps))
