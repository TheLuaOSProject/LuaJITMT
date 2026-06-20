local utils = require("suite_utils")
local build = require("suite_build")
local runtime = require("suite_runtime")

local shell_quote = utils.shell_quote
local capture_command = utils.capture_command
local assert_command_output_contains = utils.assert_command_output_contains
local assert_command_output_all_contains = utils.assert_command_output_all_contains
local assert_command_fails = utils.assert_command_fails
local write_file = utils.write_file
local with_temp_paths = utils.with_temp_paths
local compile_and_run_c = build.compile_and_run_c
local luajit_script = runtime.luajit_script
local run_luajit_script_jit_modes = runtime.run_luajit_script_jit_modes

local function write_bad_benchmark_csv(t, base, bad)
  local rows = {}
  local n = 0
  for line in (t:read(base) .. "\n"):gmatch("(.-)\n") do
    if line ~= "" then
      n = n + 1
      if n == 1 then
        rows[#rows + 1] = line
      else
        local cols = {}
        for col in (line .. ","):gmatch("(.-),") do
          cols[#cols + 1] = col
        end
        cols[3] = ("%.2f"):format(assert(tonumber(cols[3])) * 2)
        rows[#rows + 1] = table.concat(cols, ",")
      end
    end
  end
  write_file(bad, table.concat(rows, "\n") .. "\n")
end

local function build_and_run_alloc_account(t)
  compile_and_run_c(t, t:tmp("lj_t-gc2-alloc-account-m10"),
                    "t-gc2-alloc-account.c", { timeout = "20s" })
end

local function run_gc_stats(t)
  t:build({ clean = true, quiet = true })

  run_luajit_script_jit_modes(t, "t-gc-stats.lua")
  print("M9 GC stats guard passed")
end

local function run_bench_smoke(t)
  t:build({ clean = true, quiet = true })

  luajit_script(t, "t-threading-api.lua")

  local luajit = shell_quote(t:path("src", "luajit"))
  local bench_mt = shell_quote(t:path("aux", "bench", "bench_mt.lua"))
  assert_command_output_all_contains(
    "BENCH_SCALE=0.0001 " .. luajit .. " " .. bench_mt ..
      " 1 chan_pingpong",
    { "chan_pingpong", "skipped: requires an even thread count >= 2" })
  assert_command_output_all_contains(
    "BENCH_SCALE=0.0001 " .. luajit .. " " .. bench_mt ..
      " 2 chan_pingpong",
    { "chan_pingpong", "ops/s" })
  assert_command_output_contains(
    "BENCH_SCALE=0.0001 BENCH_THREADS='1 2' BENCH_FILTER=arith-MT " ..
      shell_quote(t:path("aux", "bench", "run.sh")) .. " scaling " .. luajit,
    "GC stats:")
  print("M9 benchmark smoke guard passed")
end

local function run_bench_regression(t)
  t:build({ clean = true, quiet = true })

  local base = t:path("bench", "baseline_372b369b9afd_.csv")
  local compare = shell_quote(t:path("bench", "compare_baseline.sh"))
  assert_command_output_contains(compare .. " " .. shell_quote(base) .. " " ..
                                   shell_quote(base),
                                 "PASS: geomean 1.000000 <= 1.100000")

  with_temp_paths(t, { "lj-bench-current", "lj-bench-bad" },
    function(cur, bad)
      write_bad_benchmark_csv(t, base, bad)
      local bad_cmd = compare .. " " .. shell_quote(base) .. " " ..
                        shell_quote(bad) .. " >/dev/null 2>&1"
      assert_command_fails(bad_cmd)

      capture_command("BENCH_SCALE=0.001 BASELINE_OUT=" .. shell_quote(cur) ..
                      " " .. shell_quote(t:path("bench", "run_baseline.sh")) ..
                      " " .. shell_quote(t:path("src", "luajit")))
      assert_command_output_contains(compare .. " " .. shell_quote(cur) .. " " ..
                                       shell_quote(cur),
                                     "PASS: geomean 1.000000 <= 1.100000")
    end)
  print("M9 benchmark regression accounting guard passed")
end

local function run_generational(t)
  t:build({ clean = true, quiet = true })

  run_luajit_script_jit_modes(t, "t-gc-generational-mode.lua")
  build_and_run_alloc_account(t)
  print("M10 generational mode guard passed")
end

local m9_m10_deps = {
  "m9_gc_stats",
  "m9_bench_smoke",
  "m9_bench_regression",
  "m10_generational"
}

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
    deps = m9_m10_deps,
    run = function(t)
      run_gc_stats(t)
      run_bench_smoke(t)
      run_bench_regression(t)
      run_generational(t)
      print("M9/M10 GC gates passed")
    end
  })
end
