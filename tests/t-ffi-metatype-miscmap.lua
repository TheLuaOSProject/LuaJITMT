local th = require"threading"
local ffi = require"ffi"

local nthreads = tonumber((arg and arg[1]) or os.getenv("LJ_M7_FFI_META_THREADS")) or 6
local iters = tonumber((arg and arg[2]) or os.getenv("LJ_M7_FFI_META_ITERS")) or 60

ffi.cdef[[
typedef struct { int x; } lj_m7_meta_shared_t;
]]

for tid = 1, nthreads do
  for i = 1, iters do
    ffi.cdef(("typedef struct { int x; } lj_m7_meta_%d_%d_t;"):format(tid, i))
  end
end

local ready = th.channel(nthreads)
local start = th.channel(nthreads)
local workers = {}

for tid = 1, nthreads do
  workers[tid] = th.spawn(function(ready_ch, start_ch, id, count)
    local ffi = require"ffi"
    local shared_ok = 0

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

for _ = 1, nthreads do
  local _, ok = ready:recv(10)
  assert(ok == true)
end

for _ = 1, nthreads do
  assert(start:send("go", 10) == true)
end

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

collectgarbage("collect")
collectgarbage("collect")

print(("t-ffi-metatype-miscmap OK: %d threads, %d unique metatypes"):format(
  nthreads, unique_total))
