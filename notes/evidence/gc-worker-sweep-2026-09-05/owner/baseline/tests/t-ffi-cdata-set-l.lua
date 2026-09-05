local th = require"threading"
local ffi = require"ffi"
local harness = require"thread_harness"

local nthreads = harness.arg_number(1, "LJ_M7_FFI_SET_THREADS", 6)
local iters = harness.arg_number(2, "LJ_M7_FFI_SET_ITERS", 320)

ffi.cdef[[
typedef struct {
  int x;
  int arr[2];
  _Bool b;
  _Bool bf:1;
  unsigned int nibble:3;
} lj_m7_set_outer_t;
typedef struct lj_m7_set_node_t {
  struct lj_m7_set_node_t *next;
  int v;
} lj_m7_set_node_t;
int abs(int);
int snprintf(char *str, size_t size, const char *format, ...);
]]

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
      local obj = ffi.new("lj_m7_set_outer_t")

      obj.x = seed
      obj.arr[0] = seed + 1
      obj.arr[1] = seed + 2
      obj.b = (i % 2) == 0
      obj.bf = (i % 2) == 1
      obj.nibble = i % 8

      assert(obj.x == seed)
      assert(obj.arr[0] == seed + 1)
      assert(obj.arr[1] == seed + 2)
      assert(obj.b == ((i % 2) == 0))
      assert(obj.bf == ((i % 2) == 1))
      assert(obj.nibble == i % 8)

      local init = ffi.new("lj_m7_set_outer_t",
        { seed + 3, { seed + 4, seed + 5 }, true, false, 7 })
      assert(init.x == seed + 3)
      assert(init.arr[0] == seed + 4)
      assert(init.arr[1] == seed + 5)
      assert(init.b == true)
      assert(init.bf == false)
      assert(init.nibble == 7)

      assert(tonumber(ffi.new("int", seed)) == seed)
      assert(tonumber(ffi.cast("int", seed + 6)) == seed + 6)
      assert(ffi.C.abs(-seed) == seed)

      local n1 = ffi.new("lj_m7_set_node_t")
      local n2 = ffi.new("lj_m7_set_node_t")
      n1.v = seed
      n2.v = seed + 1
      n1.next = n2
      n2.next = n1
      assert(n1.next.v == seed + 1)
      assert(n2.next.v == seed)

      local n3 = ffi.new("lj_m7_set_node_t", n1, seed + 2)
      assert(n3.next.v == seed)
      assert(n3.v == seed + 2)

      local bytes = ffi.new("uint8_t[4]", 65, 66, 67, 0)
      assert(ffi.string(bytes, 3) == "ABC")

      local out = ffi.new("char[64]")
      local n = ffi.C.snprintf(out, 64, "%g %p", ffi.new("float", i + 0.5),
			       bytes)
      assert(n > 0 and n < 64)
    end

    return true
  end, ready, start, tid, iters)
end

harness.wait_ready(ready, nthreads)
harness.release_start(start, nthreads)
harness.join_all(workers)
harness.fullgc()

print(("t-ffi-cdata-set-l OK: %d threads, %d iterations"):format(nthreads, iters))
