local th = require"threading"
local ffi = require"ffi"
local harness = require"thread_harness"
local buffer = require"string.buffer"

local nthreads = harness.arg_number(1, "LJ_M7_FFI_GET_THREADS", 6)
local iters = harness.arg_number(2, "LJ_M7_FFI_GET_ITERS", 400)

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

do
  local i64 = ffi.new("int64_t", -42)
  local u64 = ffi.new("uint64_t", 42)
  local complex = ffi.new("complex", 12.5, -3.25)

  assert(buffer.decode(buffer.encode(i64)) == i64)
  assert(buffer.decode(buffer.encode(u64)) == u64)
  assert(tostring(buffer.decode(buffer.encode(complex))) == tostring(complex))
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

harness.wait_ready(ready, nthreads)
harness.release_start(start, nthreads)
harness.join_all(workers)
harness.fullgc()

print(("t-ffi-cdata-get-l OK: %d threads, %d iterations"):format(nthreads, iters))
