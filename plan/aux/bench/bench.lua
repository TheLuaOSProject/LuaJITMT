-- Single-thread baseline benchmark harness for the LuaJIT-MT project.
-- Usage: luajit bench.lua [filter]
-- Each benchmark reports ns/op (best of 5 runs). These are the numbers the
-- multithreaded runtime is measured against (target: <=10% regression
-- single-threaded, see 13_testing_and_benchmarks.md).

local clock = os.clock
local getenv = os.getenv
local floor = math.floor
local benches = {}
local scale = tonumber(getenv("BENCH_SCALE")) or 1
if scale <= 0 then scale = 1 end
local function bench(name, iters, fn)
  iters = floor(iters * scale + 0.5)
  if iters < 1 then iters = 1 end
  benches[#benches+1] = { name = name, iters = iters, fn = fn }
end

-- 1. Pure arithmetic loop (interpreter+JIT loop perf, hotcount path)
bench("arith_loop", 5e7, function(n)
  local x = 0
  for i = 1, n do x = x + i * 0.5 end
  return x
end)

-- 2. Recursive calls (frame setup, hotcall path)
bench("fib30", 1, function()
  local function fib(n) if n < 2 then return n end return fib(n-1) + fib(n-2) end
  return fib(30)
end)

-- 3. Table hash writes (newkey + barrier path; GC churn)
bench("tab_hash_write", 2e6, function(n)
  local t = {}
  for i = 1, n do t["k"..(i % 8192)] = i end
  return t
end)

-- 4. Table hash reads (HREF/HREFK path)
bench("tab_hash_read", 2e7, function(n)
  local t = {}
  for i = 1, 4096 do t["k"..i] = i end
  local s = 0
  for i = 1, n do s = s + t["k"..(i % 4096 + 1)] end
  return s
end)

-- 5. Array part write/read (M5: writes fall back before traced ASTORE)
bench("tab_array", 3e7, function(n)
  local t = {}
  local s = 0
  for i = 1, n do
    local j = (i % 1000) + 1
    t[j] = i
    s = s + t[j]
  end
  return s
end)

-- 6. Allocation churn: short-lived tables (allocator + GC throughput)
bench("alloc_tables", 5e6, function(n)
  local s = 0
  for i = 1, n do
    local t = { i, i+1, i+2 }
    s = s + t[1]
  end
  return s
end)

-- 7. String interning churn (strtab contention point in MT build)
bench("string_intern", 2e6, function(n)
  local s = 0
  for i = 1, n do
    local str = "prefix_" .. (i % 65536)
    s = s + #str
  end
  return s
end)

-- 8. Closure creation + upvalue mutation (cell-model cost in MT build)
bench("closures_upval", 5e6, function(n)
  local s = 0
  for i = 1, n do
    local x = i
    local f = function() x = x + 1; return x end
    s = s + f()
  end
  return s
end)

-- 9. Shared upvalue mutation in a loop (ULOAD/USTORE hot path)
bench("upval_hot", 5e7, function(n)
  local x = 0
  local function inc() x = x + 1 end
  for i = 1, n do inc() end
  return x
end)

-- 10. FFI struct field access (CNEW/cdata load-store path)
bench("ffi_struct", 3e7, function(n)
  local ffi = require("ffi")
  ffi.cdef("typedef struct { double x, y; } point_t;")
  local p = ffi.new("point_t", 1, 2)
  local s = 0
  for i = 1, n do
    p.x = p.x + 1
    s = s + p.y
  end
  return s + p.x
end)

-- 11. Coroutine switch (frame save/restore; per-thread state in MT build)
bench("coroutine_switch", 3e6, function(n)
  local co = coroutine.wrap(function()
    while true do coroutine.yield(1) end
  end)
  local s = 0
  for i = 1, n do s = s + co() end
  return s
end)

-- 12. string.format via tmpbuf (per-thread SBuf in MT build)
bench("sbuf_format", 2e6, function(n)
  local s = 0
  for i = 1, n do
    s = s + #string.format("%d:%s", i, "x")
  end
  return s
end)

local filter = arg and arg[1]
local gc_mode = getenv("BENCH_GC_MODE")
if gc_mode == "generational" or gc_mode == "incremental" then
  require("threading").gcmode(gc_mode)
end
collectgarbage("collect")
print(string.format("%-18s %12s %10s", "benchmark", "total_s", "ns/op"))
for _, b in ipairs(benches) do
  if not filter or b.name:find(filter, 1, true) then
    local best = math.huge
    for run = 1, 5 do
      collectgarbage("collect")
      local t0 = clock()
      b.fn(b.iters)
      local dt = clock() - t0
      if dt < best then best = dt end
    end
    print(string.format("%-18s %12.4f %10.2f", b.name, best, best / b.iters * 1e9))
  end
end
