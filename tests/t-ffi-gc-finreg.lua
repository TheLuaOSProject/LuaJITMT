local th = require"threading"
local ffi = require"ffi"

local nthreads = tonumber((arg and arg[1]) or os.getenv("LJ_M7_FFI_FIN_THREADS")) or 6
local iters = tonumber((arg and arg[2]) or os.getenv("LJ_M7_FFI_FIN_ITERS")) or 240

local ready = th.channel(nthreads)
local start = th.channel(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
    local ffi = require"ffi"
    local cleared = 0

    local function fin(_) end

    ready_ch:send(id)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    local function allocate()
      local keep = {}
      for i = 1, count do
        local cd = ffi.gc(ffi.new("uint8_t[?]", 16 + (i % 11)), fin)
        if i % 4 == 0 then
          ffi.gc(cd, nil)
          cleared = cleared + 1
        else
          keep[#keep + 1] = cd
        end
      end
      return count - cleared
    end

    local expected = allocate()
    collectgarbage("collect")
    collectgarbage("collect")
    return expected
  end, ready, start, tid, iters)
end

for _ = 1, nthreads do
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

collectgarbage("collect")
collectgarbage("collect")

do
  local finalized = 0
  local cleared = 0
  local function fin(_)
    finalized = finalized + 1
  end
  local function allocate()
    local keep = {}
    for i = 1, iters do
      local cd = ffi.gc(ffi.new("uint8_t[?]", 24 + (i % 13)), fin)
      if i % 5 == 0 then
        ffi.gc(cd, nil)
        cleared = cleared + 1
      else
        keep[#keep + 1] = cd
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
  local cd = ffi.gc(ffi.new("uint8_t[1]"), fin)
  cd = nil
  collectgarbage("collect")
  collectgarbage("collect")
  assert(finalized == 1,
         ("nested finalizer GC: finalized %d, expected 1"):format(finalized))
end

print(("t-ffi-gc-finreg OK: %d threads, %d iterations, %d worker registrations"):format(
  nthreads, iters, total))
