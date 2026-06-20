local utils = require("suite_utils")
local build = require("suite_build")
local runtime = require("suite_runtime")
local bench_csv = require("bench_csv")

local shell_quote = utils.shell_quote
local capture_command = utils.capture_command
local assert_command_output_contains = utils.assert_command_output_contains
local assert_command_output_all_contains = utils.assert_command_output_all_contains
local write_file = utils.write_file
local with_temp_paths = utils.with_temp_paths
local compile_and_run_c = build.compile_and_run_c
local luajit_script = runtime.luajit_script
local run_luajit_script_jit_modes = runtime.run_luajit_script_jit_modes

local function rewrite_benchmark_csv(data, fn)
  local parsed = bench_csv.parse_csv(data)
  local rows = {}
  for i = 1, #parsed.rows do
    local cols = {}
    for j = 1, #parsed.rows[i].cols do cols[j] = parsed.rows[i].cols[j] end
    local replacement = fn(cols, i)
    if replacement ~= false then rows[#rows + 1] = replacement or cols end
  end
  return bench_csv.encode_csv(parsed.header.cols, rows)
end

local function assert_compare_ok(label, result)
  if result.ok then return end
  error(label .. " failed unexpectedly:\n" .. bench_csv.format_compare(result), 2)
end

local function assert_compare_error(label, result, needle)
  if result.ok then
    error(label .. " passed unexpectedly:\n" .. bench_csv.format_compare(result), 2)
  end
  for i = 1, #result.errors do
    if result.errors[i]:find(needle, 1, true) then return end
  end
  error(label .. " missing error " .. needle .. ":\n" ..
        bench_csv.format_compare(result), 2)
end

local function write_mini_benchmark(t, path)
  write_file(path, table.concat({
    'print(string.format("%-18s %12s %10s", "benchmark", "total_s", "ns/op"))',
    'print(string.format("%-18s %12.4f %10.2f", "mini_loop", 0.0001, 10.00))',
    ""
  }, "\n"))
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
  local base_csv = t:read(base)
  assert_compare_ok("pinned baseline self-compare",
                    bench_csv.compare(base_csv, base_csv))

  local bad_csv = rewrite_benchmark_csv(base_csv, function(cols)
    cols[3] = ("%.2f"):format(assert(tonumber(cols[3])) * 2)
    return cols
  end)
  assert_compare_error("geomean regression",
                       bench_csv.compare(base_csv, bad_csv),
                       "FAIL: geomean")

  local missing_csv = rewrite_benchmark_csv(base_csv, function(cols, i)
    if i == 1 then return false end
    return cols
  end)
  assert_compare_error("missing current benchmark",
                       bench_csv.compare(base_csv, missing_csv),
                       "current is missing benchmark")

  local extra_csv = base_csv .. "extra_case,0.0010,1.00,0.0010,1.00\n"
  assert_compare_error("extra current benchmark",
                       bench_csv.compare(base_csv, extra_csv),
                       "current has no pinned baseline")

  local zero_csv = rewrite_benchmark_csv(base_csv, function(cols, i)
    if i == 1 then cols[3] = "0" end
    return cols
  end)
  assert_compare_error("non-positive current benchmark",
                       bench_csv.compare(base_csv, zero_csv),
                       "non-positive benchmark value")

  with_temp_paths(t, { "lj-bench-mini.lua" },
    function(mini)
      local luajit = shell_quote(t:path("src", "luajit"))
      write_mini_benchmark(t, mini)
      local jit_out = capture_command(luajit .. " " .. shell_quote(mini),
                                      { timeout = "20s" })
      local interp_out = capture_command(luajit .. " -joff " .. shell_quote(mini),
                                         { timeout = "20s" })
      local mini_csv = bench_csv.baseline_csv_from_text(jit_out, interp_out)
      assert_compare_ok("generated mini benchmark self-compare",
                        bench_csv.compare(mini_csv, mini_csv))
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
      runtime.run_lua_test_cases(t, m9_m10_deps)
      print("M9/M10 GC gates passed")
    end
  })
end
