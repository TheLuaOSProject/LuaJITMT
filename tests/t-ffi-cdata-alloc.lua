local th = require"threading"
local ffi = require"ffi"
local bit = require"bit"
local harness = require"thread_harness"

local nthreads = harness.arg_number(1, "LJ_M7_FFI_CDATA_THREADS", 6)
local iters = harness.arg_number(2, "LJ_M7_FFI_CDATA_ITERS", 400)

ffi.cdef[[
typedef struct { int x; double y; } lj_m7_cdata_alloc_t;
]]

assert(ffi.sizeof("lj_m7_cdata_alloc_t") == 16)

do
  local oc = collectgarbage("count")
  for al = 0, 15 do
    local align = 2 ^ al
    local ct = ffi.typeof("struct { char __attribute__((aligned("..
                          align.."))) a; }")
    for _ = 1, 100 do
      local cd = ct()
      local addr = tonumber(ffi.cast("intptr_t", ffi.cast("void *", cd)))
      assert(bit.band(addr, align - 1) == 0)
    end
  end
  local nc = collectgarbage("count")
  assert(nc < oc + 3000, "GC step missing for ffi.new")
  collectgarbage("collect")
end

local ready, start = harness.channels(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
    local ffi = require"ffi"

    ready_ch:send(id)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    for i = 1, count do
      local obj = ffi.new("lj_m7_cdata_alloc_t")
      assert(ffi.sizeof(obj) == 16)

      local arr = ffi.new("uint8_t[?]", 32 + (i % 7))
      assert(ffi.sizeof(arr) == 32 + (i % 7))

      local ctype_obj = ffi.typeof("lj_m7_cdata_alloc_t")
      assert(ffi.sizeof(ctype_obj) == 16)
    end

    return true
  end, ready, start, tid, iters)
end

harness.wait_ready(ready, nthreads)
harness.release_start(start, nthreads)
harness.join_all(workers)
harness.fullgc()

print(("t-ffi-cdata-alloc OK: %d threads, %d iterations"):format(nthreads, iters))
