local th = require"threading"
local harness = require"thread_harness"

local nthreads = harness.env_number("LJ_M5_STRTAB_GC_THREADS", 4)
local niter = harness.env_number("LJ_M5_STRTAB_GC_ITERS", 5000)
local shared_mod = harness.env_number("LJ_M5_STRTAB_GC_SHARED", 384)
local gc_rounds = harness.env_number("LJ_M5_STRTAB_GC_ROUNDS", 384)

local ready, start = harness.channels(nthreads)
local done = th.channel(nthreads)
local workers = {}
local old_gcworkers

local ok_workers, old = pcall(th.gcworkers, 2)
if ok_workers then old_gcworkers = old end

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, done_ch, id, n, mod)
    local th = require"threading"
    local seen = {}
    local total = 0

    assert(ready_ch:send(id, 10) == true)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    for i = 1, n do
      local shared = "m5-strtab-gc-shared-" .. (i % mod)
      local again = "m5-strtab-gc-shared-" .. (i % mod)
      local unique = "m5-strtab-gc-unique-" .. id .. "-" .. i ..
		     "-" .. string.rep("x", i % 17)

      assert(shared == again)
      seen[shared] = (seen[shared] or 0) + 1
      total = total + #shared + #unique

      if i % 31 == 0 then
	seen["m5-strtab-gc-drop-" .. id .. "-" .. i] = unique
      end
      if i % 37 == 0 then
	seen["m5-strtab-gc-drop-" .. id .. "-" .. (i - 6)] = nil
      end
      if i % 64 == 0 then
	collectgarbage("step")
	th.sleep(0)
      end
    end

    for i = 0, mod - 1 do
      local key = "m5-strtab-gc-shared-" .. i
      assert(type(seen[key]) == "number", "shared interned key missing")
    end

    assert(done_ch:send(id, 10) == true)
    return total
  end, ready, start, done, tid, niter, shared_mod)
end

harness.wait_ready(ready, nthreads, 10, "strtab GC stress")
harness.release_start(start, nthreads, 10)

local completed = 0
for round = 1, gc_rounds do
  local _, ok = done:recv(0.001)
  if ok == true then completed = completed + 1 end
  if round % 9 == 0 then
    collectgarbage("collect")
  else
    collectgarbage("step")
  end
  if completed == nthreads then break end
end

local total = harness.join_count(workers, 30)
harness.fullgc(3)

if old_gcworkers ~= nil then
  assert(th.gcworkers(old_gcworkers) >= 0)
end

assert(total > nthreads * niter)

print(("t-strtab-gc-stress OK: %d workers x %d interns, %d GC rounds"):format(
  nthreads, niter, gc_rounds))
