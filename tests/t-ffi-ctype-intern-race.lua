local th = require"threading"
local ffi = require"ffi"
local harness = require"thread_harness"

local nthreads = harness.arg_number(1, "LJ_M7_FFI_INTERN_THREADS", 6)
local nshapes = harness.arg_number(2, "LJ_M7_FFI_INTERN_SHAPES", 48)
local rounds = harness.arg_number(3, "LJ_M7_FFI_INTERN_ROUNDS", 4)

collectgarbage("stop")

do
  local decls = {}
  for i = 1, nshapes do
    decls[#decls + 1] = ([[
typedef struct { int x; double y; } lj_m7_intern_inner_%d_t;
typedef struct {
  lj_m7_intern_inner_%d_t inner;
  int arr[4];
} lj_m7_intern_outer_%d_t;
]]):format(i, i, i)
  end
  ffi.cdef(table.concat(decls, "\n"))
end

local ready, start = harness.channels(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, shapes, passes)
    local ffi = require"ffi"
    local outer = {}

    for i = 1, shapes do
      outer[i] = ffi.typeof(("lj_m7_intern_outer_%d_t"):format(i))
    end
    require"thread_harness".assert_stack_grows(96)

    ready_ch:send(id)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    for pass = 1, passes do
      for i = 1, shapes do
	local seed = id * 1000000 + pass * 1000 + i
	local obj = outer[i]()
	obj.inner.x = seed
	obj.inner.y = seed + 0.5
	obj.arr[0] = seed + 1
	obj.arr[1] = seed + 2
	obj.arr[2] = seed + 3
	obj.arr[3] = seed + 4

	local inner = obj.inner
	assert(inner.x == seed)
	assert(inner.y == seed + 0.5)

	local arr = obj.arr
	assert(arr[0] == seed + 1)
	assert(arr[1] == seed + 2)
	assert(arr[2] == seed + 3)
	assert(arr[3] == seed + 4)
      end
    end

    return shapes * passes
  end, ready, start, tid, nshapes, rounds)
end

harness.wait_ready(ready, nthreads)
harness.release_start(start, nthreads)
local total = harness.join_count(workers)

collectgarbage("restart")
harness.fullgc()

print(("t-ffi-ctype-intern-race OK: %d threads, %d fresh shapes, %d ref reads"):format(
  nthreads, nshapes, total * 2))
