local utils = require("suite_utils")
local runtime = require("suite_runtime")

local shell_quote = utils.shell_quote
local command_succeeded = utils.command_succeeded
local run_output_contains = utils.run_output_contains
local run_output_contains_all = utils.run_output_contains_all
local compile_and_run_c = runtime.compile_and_run_c
local run_luajit_script_jit_modes = runtime.run_luajit_script_jit_modes

local function write_bad_benchmark_csv(t, base, bad)
  local out = assert(io.open(bad, "wb"))
  local n = 0
  for line in (t:read(base) .. "\n"):gmatch("(.-)\n") do
    if line ~= "" then
      n = n + 1
      if n == 1 then
        out:write(line, "\n")
      else
        local cols = {}
        for col in (line .. ","):gmatch("(.-),") do
          cols[#cols + 1] = col
        end
        cols[3] = ("%.2f"):format(assert(tonumber(cols[3])) * 2)
        out:write(table.concat(cols, ","), "\n")
      end
    end
  end
  out:close()
end

local function build_and_run_alloc_account(t)
  compile_and_run_c(t, t:tmp("lj_t-gc2-alloc-account-m10"),
                    "t-gc2-alloc-account.c", { timeout = "20s" })
end

local function run_gc_stats(t)
  t:build({ quiet = true })

  run_luajit_script_jit_modes(t, "t-gc-stats.lua")
  print("M9 GC stats guard passed")
end

local function run_bench_smoke(t)
  t:build({ quiet = true })

  t:luajit({ t:path("tests", "t-threading-api.lua") })

  local luajit = shell_quote(t:path("src", "luajit"))
  local bench_mt = shell_quote(t:path("aux", "bench", "bench_mt.lua"))
  run_output_contains_all(t,
    "BENCH_SCALE=0.0001 " .. luajit .. " " .. bench_mt ..
      " 1 chan_pingpong",
    { "chan_pingpong", "skipped: requires an even thread count >= 2" })
  run_output_contains_all(t,
    "BENCH_SCALE=0.0001 " .. luajit .. " " .. bench_mt ..
      " 2 chan_pingpong",
    { "chan_pingpong", "ops/s" })
  run_output_contains(t,
    "BENCH_SCALE=0.0001 BENCH_THREADS='1 2' BENCH_FILTER=arith-MT " ..
      shell_quote(t:path("aux", "bench", "run.sh")) .. " scaling " .. luajit,
    "GC stats:")
  print("M9 benchmark smoke guard passed")
end

local function run_bench_regression(t)
  t:build({ quiet = true })

  local base = t:path("bench", "baseline_372b369b9afd_.csv")
  local cur = t:tempname("lj-bench-current")
  local bad = t:tempname("lj-bench-bad")

  local compare = shell_quote(t:path("bench", "compare_baseline.sh"))
  run_output_contains(t, compare .. " " .. shell_quote(base) .. " " ..
                         shell_quote(base),
                      "PASS: geomean 1.000000 <= 1.100000")

  write_bad_benchmark_csv(t, base, bad)
  local bad_cmd = compare .. " " .. shell_quote(base) .. " " ..
                    shell_quote(bad) .. " >/dev/null 2>&1"
  if command_succeeded(bad_cmd) then
    error("benchmark regression checker accepted a known bad CSV")
  end

  t:run("BENCH_SCALE=0.001 BASELINE_OUT=" .. shell_quote(cur) .. " " ..
        shell_quote(t:path("bench", "run_baseline.sh")) .. " " ..
        shell_quote(t:path("src", "luajit")) .. " >/dev/null")
  run_output_contains(t, compare .. " " .. shell_quote(cur) .. " " ..
                         shell_quote(cur),
                      "PASS: geomean 1.000000 <= 1.100000")
  t:remove(cur)
  t:remove(bad)
  print("M9 benchmark regression accounting guard passed")
end

local function run_generational(t)
  t:build({ quiet = true })

  run_luajit_script_jit_modes(t, "t-gc-generational-mode.lua")
  build_and_run_alloc_account(t)
  print("M10 generational mode guard passed")
end

return function(add)
  add({
    name = "m9_gc_stats",
    description = "GC stats telemetry table and smoke test",
    run = run_gc_stats
  })

  add({
    name = "m9_bench_smoke",
    description = "benchmark harness smoke and drift guard",
    run = run_bench_smoke
  })

  add({
    name = "m9_bench_regression",
    description = "benchmark CSV/geomean accounting guard",
    run = run_bench_regression
  })

  add({
    name = "m10_generational",
    description = "public generational GC mode and accounting guard",
    run = run_generational
  })

  add({
    name = "m9_m10_gc",
    description = "M9/M10 aggregate telemetry and generational gates",
    run = function(t)
      run_gc_stats(t)
      run_bench_smoke(t)
      run_bench_regression(t)
      run_generational(t)
      print("M9/M10 GC gates passed")
    end
  })
end
