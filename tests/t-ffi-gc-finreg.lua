local th = require"threading"
local ffi = require"ffi"

local nthreads = tonumber((arg and arg[1]) or os.getenv("LJ_M7_FFI_FIN_THREADS")) or 6
local iters = tonumber((arg and arg[2]) or os.getenv("LJ_M7_FFI_FIN_ITERS")) or 240

ffi.cdef[[
typedef struct { uint8_t bytes[32]; } lj_m7_fin_obj_t;
]]
local fin_obj_t = ffi.typeof("lj_m7_fin_obj_t")

collectgarbage("stop")

local ready = th.channel(nthreads)
local start = th.channel(nthreads)
local finalized = th.channel(nthreads * iters)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, finalized_ch, id, count)
    local ffi = require"ffi"
    local fin_obj_t = ffi.typeof("lj_m7_fin_obj_t")
    local cleared = 0

    local function fin(_)
      assert(finalized_ch:send(id, 0) == true)
    end
    local function grow_stack(n)
      if n == 0 then return 0 end
      return 1 + grow_stack(n - 1)
    end
    assert(pcall(grow_stack, 96))

    ready_ch:send(id)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    local function allocate()
      for i = 1, count do
        local cd = ffi.gc(fin_obj_t(), fin)
        if i % 4 == 0 then
          ffi.gc(cd, nil)
          cleared = cleared + 1
        end
        if i % 6 == 0 then
          ffi.gc(fin_obj_t(), nil)
        end
      end
      return count - cleared
    end

    local expected = allocate()
    return expected
  end, ready, start, finalized, tid, iters)
  local _, ok = ready:recv(10)
  assert(ok == true)
end

for _ = 1, nthreads do
  assert(start:send("go", 10) == true)
end

local total = 0
local expected_by_thread = {}
for tid = 1, nthreads do
  local ok, result = workers[tid]:join(30)
  assert(ok == true, tostring(result))
  assert(type(result) == "number")
  expected_by_thread[tid] = result
  total = total + result
end

collectgarbage("restart")
collectgarbage("collect")
collectgarbage("collect")

do
  local finalized_by_thread = {}
  for _ = 1, total do
    local id, ok = finalized:recv(10)
    assert(ok == true, "worker finalizer notification timed out")
    finalized_by_thread[id] = (finalized_by_thread[id] or 0) + 1
  end
  for tid = 1, nthreads do
    assert(finalized_by_thread[tid] == expected_by_thread[tid],
           ("worker %d: finalized %d, expected %d"):format(
             tid, finalized_by_thread[tid] or 0, expected_by_thread[tid]))
  end
  local extra, why = finalized:recv(0)
  assert(extra == nil and why == "timeout",
         "worker finalizer fired more times than registered")
end

collectgarbage("stop")

do
  local shared_finalized = 0
  local shared = ffi.gc(fin_obj_t(), function(_) end)
  local race_ready = th.channel(nthreads)
  local race_start = th.channel(nthreads)
  local race_workers = {}
  local race_iters = math.max(16, math.floor(iters / 2))

  for tid = 1, nthreads do
    race_workers[tid] = th.spawn(function(ready_ch, start_ch, cd, id, count)
      local ffi = require"ffi"
      local function fin(_) end
      ready_ch:send(id)
      local token, ok = start_ch:recv(10)
      assert(ok == true and token == "go")
      for i = 1, count do
	ffi.gc(cd, fin)
	if i % 3 == 0 then
	  ffi.gc(cd, nil)
	end
      end
      return count
    end, race_ready, race_start, shared, tid, race_iters)
    local _, ok = race_ready:recv(10)
    assert(ok == true)
  end

  for _ = 1, nthreads do
    assert(race_start:send("go", 10) == true)
  end

  for tid = 1, nthreads do
    local ok, result = race_workers[tid]:join(30)
    assert(ok == true, tostring(result))
    assert(result == race_iters)
  end

  ffi.gc(shared, function(_)
    shared_finalized = shared_finalized + 1
  end)
  shared = nil
  collectgarbage("restart")
  collectgarbage("collect")
  collectgarbage("collect")
  assert(shared_finalized == 1,
	 ("shared cdata race: finalized %d, expected 1"):format(shared_finalized))
  collectgarbage("stop")
end

collectgarbage("restart")
collectgarbage("collect")
collectgarbage("collect")

do
  local finalized = 0
  local cleared = 0
  local function fin(_)
    finalized = finalized + 1
  end
  local function allocate()
    for i = 1, iters do
      local cd = ffi.gc(fin_obj_t(), fin)
      if i % 5 == 0 then
        ffi.gc(cd, nil)
        cleared = cleared + 1
      end
      if i % 7 == 0 then
        ffi.gc(fin_obj_t(), nil)
      end
    end
    return iters - cleared
  end
  local expected = allocate()
  collectgarbage("collect")
  collectgarbage("collect")
  assert(finalized == expected,
         ("main: finalized %d, expected %d"):format(finalized, expected))
end

do
  local finalized = 0
  local function fin(_)
    finalized = finalized + 1
    collectgarbage("collect")
  end
  local cd = ffi.gc(fin_obj_t(), fin)
  cd = nil
  collectgarbage("collect")
  collectgarbage("collect")
  assert(finalized == 1,
         ("nested finalizer GC: finalized %d, expected 1"):format(finalized))
end

print(("t-ffi-gc-finreg OK: %d threads, %d iterations, %d worker registrations"):format(
  nthreads, iters, total))
