local th = require"threading"
local ffi = require"ffi"
local harness = require"thread_harness"

local nthreads = harness.arg_number(1, "LJ_M7_FFI_SHARED_THREADS", 6)
local iters = harness.arg_number(2, "LJ_M7_FFI_SHARED_ITERS", 24000)

ffi.cdef[[
typedef struct {
  int x;
  int y;
} lj_m7_shared_inner_t;

typedef struct {
  int owner;
  int counter;
  int arr[4];
  _Bool b;
  _Bool bf:1;
  unsigned int nibble:3;
  lj_m7_shared_inner_t inner;
} lj_m7_shared_outer_t;
]]

local shared = ffi.new("lj_m7_shared_outer_t")
local keepalive = { shared }

local function assert_domain(obj, max_threads, max_iters)
  local owner = tonumber(obj.owner)
  local counter = tonumber(obj.counter)
  assert(owner >= 0 and owner <= max_threads, "owner left writer domain")
  assert(counter >= 0 and counter <= max_iters, "counter left writer domain")

  for i = 0, 3 do
    local v = tonumber(obj.arr[i])
    assert(v >= 0 and v <= max_threads * 100000 + max_iters,
	   "array field left writer domain")
  end

  assert(type(obj.b) == "boolean", "bool field produced non-boolean")
  assert(type(obj.bf) == "boolean", "bitfield bool produced non-boolean")
  local nibble = tonumber(obj.nibble)
  assert(nibble >= 0 and nibble < 8, "nibble bitfield left domain")

  local ix = tonumber(obj.inner.x)
  local iy = tonumber(obj.inner.y)
  assert(ix >= 0 and ix <= max_threads, "inner.x left writer domain")
  assert(iy >= 0 and iy <= max_iters, "inner.y left writer domain")
end

local ready, start = harness.channels(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(obj, ready_ch, start_ch, id, count, total)
    ready_ch:send(id)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    for i = 1, count do
      obj.owner = id
      obj.counter = i
      obj.arr[(i + id) % 4] = id * 100000 + i
      obj.b = ((i + id) % 2) == 0
      obj.bf = (i % 2) == 1
      obj.nibble = (i + id) % 8
      obj.inner.x = id
      obj.inner.y = i

      if i % 17 == 0 then
	assert_domain(obj, total, count)
      end
      if i % 257 == 0 then
	collectgarbage("step")
      end
    end

    return true
  end, shared, ready, start, tid, iters, nthreads)
end

harness.wait_ready(ready, nthreads)
harness.release_start(start, nthreads)
harness.join_all(workers)
assert_domain(shared, nthreads, iters)

shared.owner = 1
shared.counter = iters
shared.arr[0] = 11
shared.arr[1] = 22
shared.arr[2] = 33
shared.arr[3] = 44
shared.b = true
shared.bf = false
shared.nibble = 7
shared.inner.x = 1
shared.inner.y = iters

harness.fullgc(3)
assert(keepalive[1] == shared)
assert(shared.owner == 1)
assert(shared.counter == iters)
assert(shared.arr[0] == 11 and shared.arr[1] == 22)
assert(shared.arr[2] == 33 and shared.arr[3] == 44)
assert(shared.b == true and shared.bf == false and shared.nibble == 7)
assert(shared.inner.x == 1 and shared.inner.y == iters)

print(("t-ffi-cdata-shared-hammer OK: %d threads, %d iterations"):format(
  nthreads, iters))
