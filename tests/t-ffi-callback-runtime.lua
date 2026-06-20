local th = require"threading"
local harness = require"thread_harness"

local nthreads = harness.arg_number(1, "LJ_M7_FFI_CBACK_RT_THREADS", 6)
local iters = harness.arg_number(2, "LJ_M7_FFI_CBACK_RT_ITERS", 220)

collectgarbage("stop")

local ready, start = harness.channels(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
    local ffi = require"ffi"
    ffi.cdef[[
    typedef int (*lj_m7_cback_runtime_int_t)(int, int, int, int, int,
                                             int, int, int, int, int);
    typedef double (*lj_m7_cback_runtime_fp_t)(double, float, double);
    typedef int (*lj_m7_cback_runtime_cmp_t)(const void *, const void *);
    void qsort(void *base, size_t nmemb, size_t size,
               lj_m7_cback_runtime_cmp_t compar);
    ]]

    local intp = ffi.typeof("const int *")
    local int_arr_t = ffi.typeof("int[12]")
    local int_cb_t = ffi.typeof("lj_m7_cback_runtime_int_t")
    local fp_cb_t = ffi.typeof("lj_m7_cback_runtime_fp_t")
    local cmp_cb_t = ffi.typeof("lj_m7_cback_runtime_cmp_t")

    local function grow_stack(n)
      if n == 0 then return 0 end
      return 1 + grow_stack(n - 1)
    end
    assert(pcall(grow_stack, 96))

    local int_cb = ffi.cast(int_cb_t, function(a, b, c, d, e, f, g, h, i, j)
      return a + b + c + d + e + f + g + h + i + j + id
    end)
    local fp_cb = ffi.cast(fp_cb_t, function(a, b, c)
      return a + b + c + id * 0.25
    end)
    local cmp_cb = ffi.cast(cmp_cb_t, function(a, b)
      local x = ffi.cast(intp, a)[0]
      local y = ffi.cast(intp, b)[0]
      if x < y then return -1 end
      if x > y then return 1 end
      return 0
    end)

    ready_ch:send(id)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    for iter = 1, count do
      local seed = id * 1000 + iter
      local isum = int_cb(seed, seed + 1, seed + 2, seed + 3, seed + 4,
                          seed + 5, seed + 6, seed + 7, seed + 8, seed + 9)
      assert(isum == seed * 10 + 45 + id)

      local fgot = fp_cb(seed + 0.5, 1.25, id + 0.75)
      local fexp = seed + 0.5 + 1.25 + id + 0.75 + id * 0.25
      assert(math.abs(fgot - fexp) < 0.0001)

      local arr = int_arr_t()
      for i = 0, 11 do
        arr[i] = seed + ((i * 7 + id * 3) % 17)
      end
      ffi.C.qsort(arr, 12, ffi.sizeof("int"), cmp_cb)
      for i = 1, 11 do
        assert(arr[i - 1] <= arr[i])
      end
    end

    int_cb:free()
    fp_cb:free()
    cmp_cb:free()
    return count
  end, ready, start, tid, iters)
  harness.wait_ready(ready, 1)
end

harness.release_start(start, nthreads)
local total = harness.join_count(workers)

collectgarbage("restart")
harness.fullgc()

print(("t-ffi-callback-runtime OK: %d threads, %d callback rounds"):format(
  nthreads, total))
