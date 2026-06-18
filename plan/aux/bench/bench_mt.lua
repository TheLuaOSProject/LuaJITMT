-- bench_mt.lua — multi-thread scaling benchmarks for LuaJIT-MT.
-- Runnable from milestone M4 (threading API) onward; full numbers need M6
-- (JIT) — see 13_testing_and_benchmarks.md §13.8.
--
-- Usage: luajit bench_mt.lua [nthreads] [filter]
-- Prints ops/s total and per-thread; run with 1,2,4,8 to plot scaling.
-- NOTE: numbers are meaningless on the 1-vCPU report container; run on
-- real cores (13 §13.2 caveat).

local th = require("threading")
local clock = os.clock  -- wall clock preferred: use os.time fallback note
local function wall()
  -- os.clock is CPU time; for MT scaling we need wall time.
  -- M4 adds threading.now() (monotonic); fall back if absent.
  return th.now and th.now() or os.time()
end

local NT = tonumber(arg and arg[1]) or th.cpucount()
local filter = arg and arg[2]
local gc_stat_keys = {
  "cycle_starts",
  "assist_runs",
  "sweep_owner_runs",
  "sweep_live_updates",
  "live_estimate",
  "weak_clear_tables",
  "finalizer_queued",
}

local function print_gc_stats()
  local ok, stats = pcall(collectgarbage, "stats")
  if ok and type(stats) == "table" then
    local out = { "GC stats:" }
    for i = 1, #gc_stat_keys do
      local k = gc_stat_keys[i]
      out[#out+1] = k .. "=" .. tostring(stats[k])
    end
    print(table.concat(out, " "))
  else
    print("GC stats:", collectgarbage("count"), "KB est.")
  end
end

local function run(name, perthread_ops, mkworker, setup)
  if filter and not name:find(filter, 1, true) then return end
  collectgarbage("collect")
  local shared = setup and setup() or nil
  local start = th.channel(0)            -- rendezvous: aligned start
  local ts = {}
  for i = 1, NT do
    ts[i] = th.spawn(function(id, sh, n)
      start:recv()
      return mkworker(id, sh, n)
    end, i, shared, perthread_ops)
  end
  local t0
  -- release all workers as close together as possible
  for _ = 1, NT do start:send(true) end
  t0 = wall()
  local sum = 0
  for i = 1, NT do
    local ok, r = ts[i]:join()
    assert(ok, r)
    sum = sum + (r or 0)
  end
  local dt = wall() - t0
  local total_ops = perthread_ops * NT
  print(string.format("%-22s %2d thr %10.3f s  %12.0f ops/s  %12.0f ops/s/thr  (chk %s)",
    name, NT, dt, total_ops/dt, total_ops/dt/NT, tostring(sum)))
end

-- 1. Embarrassingly parallel arithmetic: ideal-scaling reference line.
run("arith-MT", 5e7, function(id, _, n)
  local x = 0
  for i = 1, n do x = x + i * 0.5 end
  return x
end)

-- 2. Shared table, read-mostly: gen-header acquire + chain walk scaling.
run("tab_read-shared", 2e7, function(id, t, n)
  local s = 0
  for i = 1, n do s = s + t["k" .. (i % 4096 + 1)] end
  return s
end, function()
  local t = {}
  for i = 1, 4096 do t["k"..i] = i end
  return t
end)

-- 3. Shared table, write-heavy: CAS contention + cooperative resize.
run("tab_write-shared", 2e6, function(id, t, n)
  for i = 1, n do t["k" .. ((i*NT+id) % 8192)] = i end
  return 0
end, function() return {} end)

-- 4. Sharded tables: per-thread table, the upper bound for table writes.
run("tab_write-sharded", 2e6, function(id, _, n)
  local t = {}
  for i = 1, n do t["k" .. (i % 8192)] = i end
  return 0
end)

-- 5. Allocator scalability: the ADR-4 headline (per-thread arenas).
run("alloc-MT", 5e6, function(id, _, n)
  local s = 0
  for i = 1, n do
    local t = { i, i+1, i+2 }
    s = s + t[1]
  end
  return s
end)

-- 6. String interning storm: the worst-case shared structure (06 §6.5).
run("intern-MT", 1e6, function(id, _, n)
  local s = 0
  for i = 1, n do
    s = s + #("prefix_" .. (i % 65536))
  end
  return s
end)

-- 7. Shared upvalue cell hammering: cell CAS/store visibility cost.
run("cell-shared", 5e6, function(id, sh, n)
  for i = 1, n do sh.inc() end
  return 0
end, function()
  local counter = 0
  return { inc = function() counter = counter + 1 end,
           get = function() return counter end }
end)

-- 8. Channel ping-pong: synchronization latency (pairs of threads).
run("chan_pingpong", 2e5, function(id, sh, n)
  local a, b = sh[1 + (id-1) % 2], sh[2 - (id-1) % 2]
  if id % 2 == 1 then
    for i = 1, n do a:send(i); b:recv() end
  else
    for i = 1, n do a:recv(); b:send(i) end
  end
  return 0
end, function() return { th.channel(0), th.channel(0) } end)

-- 9. Channel throughput: MPMC ring under load.
run("chan_throughput", 1e6, function(id, ch, n)
  if id % 2 == 1 then
    for i = 1, n do ch:send(i) end
  else
    local s = 0
    for i = 1, n do s = s + (ch:recv() or 0) end
    return s
  end
  return 0
end, function() return th.channel(1024) end)

-- 10. GC pressure under parallel churn: cycles must overlap mutators.
run("gc_churn-MT", 2e6, function(id, _, n)
  local keep = {}
  for i = 1, n do
    keep[i % 1000 + 1] = { tostring(i), { i, i } }
  end
  return #keep
end)

print(("-"):rep(72))
print_gc_stats()
