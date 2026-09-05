local th = require"threading"
local ffi = require"ffi"
local harness = require"thread_harness"

ffi.cdef[[
typedef int (*lj_m7_cback_install_t)(int);
]]

local nthreads = harness.arg_number(1, "LJ_M7_FFI_CBACK_THREADS", 6)
local iters = harness.arg_number(2, "LJ_M7_FFI_CBACK_ITERS", 64)

collectgarbage("stop")

local ready, start = harness.channels(nthreads)
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
  harness.wait_ready(ready, 1)
end

harness.release_start(start, nthreads)
local total = harness.join_count(workers)

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

do
  local stale = ffi.cast("lj_m7_cback_install_t", function(x)
    return x + 1
  end)
  stale:free()

  local live = ffi.cast("lj_m7_cback_install_t", function(x)
    return x + 2
  end)
  assert(live(10) == 12)

  local ok, err = pcall(function()
    stale:set(function(x)
      return x + 99
    end)
  end)
  assert(not ok and tostring(err):match("bad callback"), tostring(err))
  assert(live(10) == 12)

  ok, err = pcall(function()
    stale:free()
  end)
  assert(not ok and tostring(err):match("bad callback"), tostring(err))

  live:free()
end

collectgarbage("restart")
harness.fullgc()

print(("t-ffi-callback-install OK: %d threads, %d callbacks"):format(nthreads, total))
