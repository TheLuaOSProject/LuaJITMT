local th = require"threading"
local harness = require"thread_harness"

local nthreads = harness.arg_number(1, "LJ_M7_FFI_CARITH_THREADS", 6)
local iters = harness.arg_number(2, "LJ_M7_FFI_CARITH_ITERS", 320)

collectgarbage("stop")

local ready, start = harness.channels(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
    local ffi = require"ffi"
    local bit = require"bit"
    local i64 = ffi.typeof("int64_t")
    local u64 = ffi.typeof("uint64_t")
    local iptr = ffi.typeof("int *")
    local arr = ffi.new("int[4]")
    local p = ffi.cast(iptr, arr)
    local function grow_stack(n)
      if n == 0 then return 0 end
      return 1 + grow_stack(n - 1)
    end
    assert(pcall(grow_stack, 96))

    ready_ch:send(id)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    for i = 1, count do
      local seed = id * 100000 + i
      local a = i64(seed)
      local b = i64(i)
      assert(a + b == i64(seed + i))
      assert(a - b == i64(seed - i))
      assert(b * i64(3) == i64(i * 3))
      assert(-b == i64(-i))
      assert((a == i64(seed)) == true)
      assert((a < i64(seed + 1)) == true)
      assert((a <= i64(seed)) == true)

      local ua = u64(seed)
      assert(ua + u64(1) == u64(seed + 1))
      assert(ua - u64(i) == u64(seed - i))
      assert(bit.band(u64(0xff00), u64(0x0ff0)) == 0x0f00)
      assert(bit.bor(u64(0xf000), u64(0x00f0)) == 0xf0f0)
      assert(bit.rshift(u64(0x80000000), 31) == 1)

      arr[0] = seed
      arr[1] = seed + 1
      arr[2] = seed + 2
      arr[3] = seed + 3
      local q = p + 2
      assert(q[0] == seed + 2)
      assert(q - p == 2)
      assert(p < q)
      assert(p <= p)
    end

    return count
  end, ready, start, tid, iters)
  harness.wait_ready(ready, 1)
end

harness.release_start(start, nthreads)
local total = harness.join_count(workers)

collectgarbage("restart")
harness.fullgc()

print(("t-ffi-carith-l OK: %d threads, %d iterations"):format(nthreads, total))
