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
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
    local ffi = require"ffi"
    local fin_obj_t = ffi.typeof("lj_m7_fin_obj_t")
    local cleared = 0

    local function fin(_) end
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
  end, ready, start, tid, iters)
  local _, ok = ready:recv(10)
  assert(ok == true)
end

for _ = 1, nthreads do
  assert(start:send("go", 10) == true)
end

local total = 0
for tid = 1, nthreads do
  local ok, result = workers[tid]:join(30)
  assert(ok == true, tostring(result))
  assert(type(result) == "number")
  total = total + result
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
