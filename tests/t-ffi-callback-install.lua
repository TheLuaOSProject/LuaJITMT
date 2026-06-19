local th = require"threading"
local ffi = require"ffi"

ffi.cdef[[
typedef int (*lj_m7_cback_install_t)(int);
]]

local nthreads = tonumber((arg and arg[1]) or os.getenv("LJ_M7_FFI_CBACK_THREADS")) or 6
local iters = tonumber((arg and arg[2]) or os.getenv("LJ_M7_FFI_CBACK_ITERS")) or 64

collectgarbage("stop")

local ready = th.channel(nthreads)
local start = th.channel(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
    local ffi = require"ffi"
    ffi.cdef[[
    typedef int (*lj_m7_cback_install_t)(int);
    ]]
    local function cb(x)
      return x + id
    end
    local function grow_stack(n)
      if n == 0 then return 0 end
      return 1 + grow_stack(n - 1)
    end
    assert(pcall(grow_stack, 96))

    ready_ch:send(id)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    for i = 1, count do
      local cback = ffi.cast("lj_m7_cback_install_t", cb)
      cback:free()
    end

    return count
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
  workers[tid] = nil
end

do
  local held = {}
  local function cb(x)
    return x + 1
  end

  for i = 1, 10000 do
    local ok, res = pcall(ffi.cast, "lj_m7_cback_install_t", cb)
    if not ok then
      assert(tostring(res):match("too many callbacks"), tostring(res))
      break
    end
    held[i] = res
  end
  assert(#held > 0)
  assert(#held < 10000)

  for i = 1, #held do
    held[i]:free()
    held[i] = nil
  end
end

collectgarbage("restart")
collectgarbage("collect")
collectgarbage("collect")

print(("t-ffi-callback-install OK: %d threads, %d callbacks"):format(nthreads, total))
