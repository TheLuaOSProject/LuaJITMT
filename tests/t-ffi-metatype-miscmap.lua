local th = require"threading"
local ffi = require"ffi"
local harness = require"thread_harness"

local nthreads = harness.arg_number(1, "LJ_M7_FFI_META_THREADS", 6)
local iters = harness.arg_number(2, "LJ_M7_FFI_META_ITERS", 60)

ffi.cdef[[
typedef struct { int x; } lj_m7_meta_shared_t;
]]

for tid = 1, nthreads do
  for i = 1, iters do
    ffi.cdef(("typedef struct { int x; } lj_m7_meta_%d_%d_t;"):format(tid, i))
  end
end

local ready, start = harness.channels(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
    local ffi = require"ffi"
    local shared_ok = 0
    local function grow_stack(n)
      if n == 0 then return 0 end
      return 1 + grow_stack(n - 1)
    end
    assert(pcall(grow_stack, 96))

    ready_ch:send(id)
    local token, ok = start_ch:recv(10)
    assert(ok == true and token == "go")

    local ok_shared, shared_ct = pcall(ffi.metatype, "lj_m7_meta_shared_t", {
      __index = {
        get = function(self) return self.x end,
      },
    })
    if ok_shared then
      shared_ok = 1
      assert(shared_ct(id):get() == id)
    end

    for i = 1, count do
      local name = ("lj_m7_meta_%d_%d_t"):format(id, i)
      local ct = ffi.metatype(name, {
        __index = {
          get = function(self) return self.x end,
        },
      })
      assert(ct(i):get() == i)
    end

    return shared_ok * 1000000 + count
  end, ready, start, tid, iters)
end

harness.wait_ready(ready, nthreads)
harness.release_start(start, nthreads)

local shared_winners = 0
local unique_total = 0
for tid = 1, nthreads do
  local ok, result = workers[tid]:join(30)
  assert(ok == true, tostring(result))
  assert(type(result) == "number")
  shared_winners = shared_winners + math.floor(result / 1000000)
  unique_total = unique_total + (result % 1000000)
end

assert(shared_winners == 1, ("shared metatype winners: %d"):format(shared_winners))

ffi.cdef[[
typedef struct { int x; } lj_m7_meta_root_t;
]]

do
  local mt = {
    __index = {
      get = function(self) return self.x + 7 end,
    },
  }
  local ct = ffi.metatype("lj_m7_meta_root_t", mt)
  mt = nil
  harness.fullgc()
  assert(ct(35):get() == 42)
end

harness.fullgc()

print(("t-ffi-metatype-miscmap OK: %d threads, %d unique metatypes"):format(
  nthreads, unique_total))
