local function workers(...)
  return require"threading".gcworkers(...)
end

local function churn_worker_control(rounds)
  local th = require"threading"
  for round = 1, rounds do
    local threads = {}
    for id = 1, 6 do
      threads[id] = th.spawn(function(worker_id, round_id)
        local last
        for i = 1, 12 do
          local target = (worker_id + i + round_id) % 4
          last = th.gcworkers(target)
          assert(last == 0 or last == 1 or last == 2)
          if i % 3 == 0 then
            collectgarbage("step", 0)
          end
        end
        return true
      end, id, round)
    end
    for id = 1, #threads do
      local ok, done = threads[id]:join(30)
      assert(ok == true and done == true)
    end
  end
  local old = workers(0)
  assert(old == 0 or old == 1 or old == 2)
  assert(workers() == 0)
end

workers(0)
assert(workers() == 0)
assert(workers(0) == 0)
assert(workers() == 0)

local stats0 = require"threading".gcstats()
assert(type(stats0.worker_wakes) == "number")
assert(type(stats0.worker_async_progress) == "number")

assert(workers(1) == 0)
assert(workers() == 1)
assert(workers(2) == 1)
assert(workers() == 2)
assert(workers(3) == 2)
assert(workers() == 2)

collectgarbage("collect")
local stats1 = require"threading".gcstats()
assert(stats1.worker_wakes >= stats0.worker_wakes)
assert(stats1.worker_async_progress >= stats0.worker_async_progress)

assert(workers(0) == 2)
assert(workers() == 0)
assert(workers(0) == 0)

churn_worker_control(tonumber(os.getenv("LJ_GC_WORKERS_CHURN_ROUNDS")) or 1)

print("t-gc-workers OK")
