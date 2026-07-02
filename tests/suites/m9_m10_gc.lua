local utils = require("suite_utils")
local build = require("suite_build")
local runtime = require("suite_runtime")
local bench_csv = require("bench_csv")
local bench_driver = require("bench_driver")

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

local function run_trace_hard_assist_cadence(t)
  t:build({ clean = true, quiet = true })

  with_temp_paths(t, { "lj-gc2-hard-cadence.lua" }, function(script)
    local luajit = shell_quote(t:path("src", "luajit"))
    local lua_path = "LUA_PATH=" .. shell_quote(runtime.lua_path(t))
    write_file(script, table.concat({
      'local th = require"threading"',
      'assert(jit and jit.status())',
      'jit.opt.start("hotloop=1", "hotexit=1", "-sink")',
      'local function run(n)',
      '  local s = 0',
      '  for i = 1, n do',
      '    local x = i',
      '    local f = function() x = x + 1; return x end',
      '    s = s + f()',
      '  end',
      '  return s',
      'end',
      'run(1000)',
      'collectgarbage("collect")',
      'local before = th.gcstats()',
      'local result = run(100000)',
      'local after = th.gcstats()',
      'assert(result == 5000150000)',
      'local allocated = after.alloc_since_trigger - before.alloc_since_trigger',
      'local assists = after.assist_runs - before.assist_runs',
      'assert(allocated > 4 * 1024 * 1024, allocated)',
      'assert(assists <= 64, assists)',
      'print("assist_delta=" .. assists .. " allocated=" .. allocated)',
      ""
    }, "\n"))
    assert_command_output_contains(
      lua_path .. " " .. luajit .. " " .. shell_quote(script),
      "assist_delta=",
      { timeout = "20s", stderr = true })
  end)
  print("M9 trace hard-assist cadence guard passed")
end

local function run_bench_smoke(t)
  t:build({ clean = true, quiet = true })

  luajit_script(t, "t-threading-api.lua")

  local luajit = shell_quote(t:path("src", "luajit"))
  local bench_mt = shell_quote(t:path("aux", "bench", "bench_mt.lua"))
  local bench_lua = shell_quote(t:path("aux", "bench", "bench.lua"))
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
    "weak_bridge_skipped=")
  assert_command_output_all_contains(
    "BENCH_SCALE=0.05 " .. luajit .. " -joff " .. bench_lua ..
      " alloc_tables",
    { "alloc_tables", "ns/op" },
    { timeout = "20s" })
  assert_command_output_all_contains(
    "BENCH_SCALE=0.01 " .. luajit .. " -joff " .. bench_lua ..
      " closures_upval",
    { "closures_upval", "ns/op" },
    { timeout = "20s" })
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
      assert_compare_ok("generated mini raw benchmark self-compare",
                        bench_csv.compare_bench_text(jit_out, jit_out))
      assert_compare_ok("direct Lua driver binary self-compare",
                        bench_driver.compare_bins(t:path("src", "luajit"),
                                                  t:path("src", "luajit"),
                                                  mini,
                                                  { timeout = "20s" }))
    end)

  with_temp_paths(t, {
    "lj-bench-mini.lua",
    "lj-bench-jit.txt",
    "lj-bench-interp.txt",
    "lj-bench-baseline.csv",
    "lj-bench-aux-jit.csv",
    "lj-bench-aux-interp.csv"
  }, function(mini, jit_txt, interp_txt, out_csv, aux_jit_csv, aux_interp_csv)
    local luajit = shell_quote(t:path("src", "luajit"))
    local lua_path = "LUA_PATH=" .. shell_quote(runtime.lua_path(t))
    local cli = shell_quote(t:path("bench", "bench_csv_cli.lua"))
    local run_baseline = shell_quote(t:path("bench", "run_baseline.sh"))
    local compare_baseline = shell_quote(t:path("bench", "compare_baseline.sh"))
    local aux_run = shell_quote(t:path("aux", "bench", "run.sh"))

    write_mini_benchmark(t, mini)
    write_file(jit_txt, capture_command(luajit .. " " .. shell_quote(mini),
                                        { timeout = "20s" }))
    write_file(interp_txt,
               capture_command(luajit .. " -joff " .. shell_quote(mini),
                               { timeout = "20s" }))

    local cli_csv = capture_command(lua_path .. " " .. luajit .. " " ..
                                      cli .. " baseline-csv " ..
                                      shell_quote(jit_txt) .. " " ..
                                      shell_quote(interp_txt),
                                    { timeout = "20s", stderr = true })
    assert_compare_ok("benchmark CLI baseline self-compare",
                      bench_csv.compare(cli_csv, cli_csv))

    local cli_compare = capture_command(lua_path .. " " .. luajit .. " " ..
                                          cli .. " compare-bench-text " ..
                                          shell_quote(jit_txt) .. " " ..
                                          shell_quote(jit_txt),
                                        { timeout = "20s", stderr = true })
    assert(cli_compare:find("PASS: geomean", 1, true))

    local cli_run = capture_command(lua_path .. " " .. luajit .. " " ..
                                      cli .. " run-baseline " ..
                                      luajit .. " " .. shell_quote(mini) ..
                                      " " .. shell_quote(out_csv),
                                    { timeout = "20s", stderr = true })
    assert(cli_run:find("wrote " .. out_csv, 1, true))
    assert_compare_ok("benchmark CLI run-baseline self-compare",
                      bench_csv.compare(t:read(out_csv), t:read(out_csv)))

    local cli_aux = capture_command(lua_path .. " " .. luajit .. " " ..
                                      cli .. " aux-baseline " ..
                                      luajit .. " " .. shell_quote(mini) ..
                                      " " .. shell_quote(aux_jit_csv) ..
                                      " " .. shell_quote(aux_interp_csv),
                                    { timeout = "20s", stderr = true })
    assert(cli_aux:find("wrote baseline CSVs", 1, true))
    assert(t:read(aux_jit_csv):find("jit_ns_per_op", 1, true))
    assert(t:read(aux_interp_csv):find("interp_ns_per_op", 1, true))

    capture_command("BASELINE_BENCH_LUA=" .. shell_quote(mini) ..
                    " BASELINE_OUT=" .. shell_quote(out_csv) .. " " ..
                    run_baseline .. " " .. luajit,
                    { timeout = "20s", stderr = true })
    assert_compare_ok("run_baseline shell output self-compare",
                      bench_csv.compare(t:read(out_csv), t:read(out_csv)))

    local compare_out = capture_command(compare_baseline .. " " ..
                                          shell_quote(out_csv) .. " " ..
                                          shell_quote(out_csv),
                                        { timeout = "20s", stderr = true })
    assert(compare_out:find("PASS: geomean", 1, true))

    local aux_compare = capture_command(
      "BENCH_SCALE=0.0001 BENCH_FILTER=arith_loop BENCH_GEOMEAN_MAX=1000 " ..
        aux_run .. " compare " .. luajit .. " " .. luajit,
      { timeout = "20s", stderr = true })
    assert(aux_compare:find("geomean", 1, true))
  end)
  print("M9 benchmark regression accounting guard passed")
end

local function run_bench_stock_compare(t)
  local stock = os.getenv("LJ_BENCH_STOCK_BIN")
  if not stock or stock == "" then
    print("M9 stock benchmark guard skipped; LJ_BENCH_STOCK_BIN not set")
    return
  end

  t:build({ clean = true, quiet = true })

  local bench_lua = t:path("aux", "bench", "bench.lua")
  local current = t:path("src", "luajit")
  local filters = os.getenv("LJ_BENCH_STOCK_FILTERS") or
    "arith_loop fib30 tab_hash_write alloc_tables closures_upval"
  local max = tonumber(os.getenv("LJ_BENCH_STOCK_MAX") or
                       os.getenv("BENCH_GEOMEAN_MAX") or "100")
  local timeout = os.getenv("LJ_BENCH_STOCK_TIMEOUT") or "30s"

  for filter in filters:gmatch("%S+") do
    local result = bench_driver.compare_bins(stock, current, bench_lua, {
      filter = filter,
      max = max,
      timeout = timeout,
      stderr = true
    })
    if not result.ok then
      error("stock benchmark regression for " .. filter .. ":\n" ..
            bench_csv.format_compare(result), 2)
    end
    io.write("stock benchmark ", filter, " geomean ",
             ("%.6f"):format(result.geomean or 0), "\n")
  end
  print("M9 stock benchmark guard passed")
end

local function run_generational(t)
  t:build({ clean = true, quiet = true })

  run_luajit_script_jit_modes(t, "t-gc-generational-mode.lua")
  build_and_run_alloc_account(t)
  print("M10 generational mode guard passed")
end

local m9_m10_deps = {
  "m9_gc_stats",
  "m9_trace_hard_assist_cadence",
  "m9_bench_smoke",
  "m9_bench_regression",
  "m9_bench_stock_compare",
  "m10_generational"
}

return function(add)
  add({
    name = "m9_gc_stats",
    description = "GC stats telemetry table and smoke test",
    run = run_gc_stats
  })

  add({
    name = "m9_trace_hard_assist_cadence",
    description = "trace allocation hard-assist cadence guard",
    run = run_trace_hard_assist_cadence
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
    name = "m9_bench_stock_compare",
    description = "optional stock LuaJIT performance guard",
    run = run_bench_stock_compare
  })

  add({
    name = "m10_generational",
    description = "fork-local generational GC mode and accounting guard",
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
