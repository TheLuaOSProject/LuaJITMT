local th = require"threading"

collectgarbage("stop")
jit.off(true)  -- Keep orchestration interpreted; only hot() may trace.

local function env_count(name, default)
  local value = os.getenv(name)
  local count = value and tonumber(value) or default
  assert(count and count >= 1 and count == math.floor(count),
	 "invalid " .. name)
  return count
end
jit.off(env_count, true)

local nworkers = env_count("LJ_M6_JIT_ATOMIC_REDISPATCH_WORKERS", 1)
local worker_iters =
  env_count("LJ_M6_JIT_ATOMIC_REDISPATCH_WORKER_ITERS", 50000)
local flush_iters =
  env_count("LJ_M6_JIT_ATOMIC_REDISPATCH_FLUSH_ITERS", 5000)

local function hot(seed)
  local sum = seed
  for i = 1, 96 do
    sum = sum + i
  end
  return sum
end

local function expected(seed)
  return seed + 4656
end
jit.off(expected, true)

-- Every flush restores FORL's branch offset and the next recording publishes
-- JFORL's trace number. A TG returning from vm_hotloop must observe one whole
-- instruction generation, never an opcode from one and D from the other.
jit.flush()
jit.opt.start("hotloop=1", "hotexit=100000", "minstitch=1")
for i = 1, 64 do
  assert(hot(i) == expected(i))
end

local ready = th.channel(nworkers * 2)
local start = th.channel(nworkers * 2)
local worker_body = function(ready_ch, start_ch, worker_id, iterations)
  assert(ready_ch:send(worker_id, 10) == true)
  local token, ok = start_ch:recv(10)
  assert(ok == true and token == "go")
  for i = 1, iterations do
    local seed = worker_id * 1000000 + i
    local got = hot(seed)
    local want = expected(seed)
    assert(got == want,
	   "worker " .. worker_id .. " mismatch at " .. i ..
	   ": got " .. tostring(got) .. ", want " .. tostring(want))
  end
  return worker_id
end
jit.off(worker_body, true)

local workers = {}
for i = 1, nworkers do
  workers[i] = th.spawn(worker_body, ready, start, i, worker_iters)
end
for _ = 1, nworkers do
  local value, ok = ready:recv(10)
  assert(ok == true and value >= 1 and value <= nworkers)
end
for _ = 1, nworkers do
  assert(start:send("go", 10) == true)
end

for i = 1, flush_iters do
  local seed = 100000000 + i
  local got = hot(seed)
  local want = expected(seed)
  assert(got == want,
	 "main mismatch at " .. i .. ": got " .. tostring(got) ..
	 ", want " .. tostring(want))
  jit.flush()
end

for i = 1, nworkers do
  local joined, result = workers[i]:join(45)
  assert(joined == true, tostring(result))
  assert(result == i)
end

print("t-jit-flush-atomic-redispatch OK: " .. tostring(nworkers) ..
      " workers, " .. tostring(worker_iters) .. " hot calls, " ..
      tostring(flush_iters) .. " flushes")
