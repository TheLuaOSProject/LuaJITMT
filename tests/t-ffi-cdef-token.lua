local th = require"threading"
local ffi = require"ffi"
local harness = require"thread_harness"

local nthreads = harness.arg_number(1, "LJ_M7_FFI_CDEF_THREADS", 6)
local iters = harness.arg_number(2, "LJ_M7_FFI_CDEF_ITERS", 120)

ffi.cdef[[
typedef struct { int x; double y; } lj_m7_cdef_token_parent_t;
]]
assert(ffi.sizeof("lj_m7_cdef_token_parent_t") == 16)

local ready, start = harness.channels(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
    ready_ch:send(id)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    for i = 1, count do
      local name = ("lj_m7_cdef_token_%d_%d_t"):format(id, i)
      ffi.cdef(("typedef struct { int x; double y; } %s;"):format(name))
    end

    return true
  end, ready, start, tid, iters)
end

harness.wait_ready(ready, nthreads)
harness.release_start(start, nthreads)
harness.join_all(workers)

print(("t-ffi-cdef-token OK: %d threads, %d iterations"):format(nthreads, iters))
