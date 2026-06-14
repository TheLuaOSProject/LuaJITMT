local th = require"threading"
local ffi = require"ffi"

local nthreads = tonumber((arg and arg[1]) or os.getenv("LJ_M7_FFI_CDATA_THREADS")) or 6
local iters = tonumber((arg and arg[2]) or os.getenv("LJ_M7_FFI_CDATA_ITERS")) or 400

ffi.cdef[[
typedef struct { int x; double y; } lj_m7_cdata_alloc_t;
]]

assert(ffi.sizeof("lj_m7_cdata_alloc_t") == 16)
local ready = th.channel(nthreads)
local start = th.channel(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
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

for _ = 1, nthreads do
  local _, ok = ready:recv(10)
  assert(ok == true)
end

for _ = 1, nthreads do
  assert(start:send("go", 10) == true)
end

for tid = 1, nthreads do
  local ok, result = workers[tid]:join(30)
  assert(ok == true, tostring(result))
  assert(result == true)
end

collectgarbage("collect")
collectgarbage("collect")

print(("t-ffi-cdata-alloc OK: %d threads, %d iterations"):format(nthreads, iters))
