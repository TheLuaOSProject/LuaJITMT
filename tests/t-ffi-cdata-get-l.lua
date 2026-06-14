local th = require"threading"
local ffi = require"ffi"

local nthreads = tonumber((arg and arg[1]) or os.getenv("LJ_M7_FFI_GET_THREADS")) or 6
local iters = tonumber((arg and arg[2]) or os.getenv("LJ_M7_FFI_GET_ITERS")) or 400

ffi.cdef[[
typedef struct { int x; } lj_m7_get_inner_t;
typedef struct {
  lj_m7_get_inner_t inner;
  int arr[2];
  _Bool b;
  _Bool bf:1;
  unsigned int nibble:3;
} lj_m7_get_outer_t;
int abs(int);
]]

local ready = th.channel(nthreads)
local start = th.channel(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
    local ffi = require"ffi"

    ready_ch:send(id)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    for i = 1, count do
      local seed = id * 100000 + i
      local obj = ffi.new("lj_m7_get_outer_t")

      obj.inner.x = seed
      obj.arr[0] = seed + 1
      obj.arr[1] = seed + 2
      obj.b = (i % 2) == 0
      obj.bf = (i % 2) == 1
      obj.nibble = i % 8

      local inner = obj.inner
      assert(inner.x == seed)

      local arr = obj.arr
      assert(arr[0] == seed + 1)
      assert(arr[1] == seed + 2)

      assert(obj.b == ((i % 2) == 0))
      assert(obj.bf == ((i % 2) == 1))
      assert(obj.nibble == i % 8)
      assert(ffi.C.abs(-seed) == seed)
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

print(("t-ffi-cdata-get-l OK: %d threads, %d iterations"):format(nthreads, iters))
