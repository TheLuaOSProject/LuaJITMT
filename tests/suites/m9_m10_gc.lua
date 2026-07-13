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
local clean_build = build.clean_build
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

local function trim(s)
  return (s or ""):gsub("^%s+", ""):gsub("%s+$", "")
end

local function command_first_line(cmd)
  local p = io.popen(cmd)
  if not p then return nil end
  local line = p:read("*l")
  local ok = p:close()
  if not ok then return nil end
  line = trim(line)
  if line == "" then return nil end
  return line
end

local function is_executable(path)
  return path and path ~= "" and
    utils.command_succeeded("test -x " .. shell_quote(path) .. " 2>/dev/null")
end

local function same_file(a, b)
  if not is_executable(a) or not is_executable(b) then return false end
  return utils.command_succeeded("test " .. shell_quote(a) .. " -ef " ..
                                 shell_quote(b) .. " 2>/dev/null")
end

local function find_stock_luajit(current)
  local explicit = os.getenv("LJ_BENCH_STOCK_BIN")
  if explicit and explicit ~= "" then
    if same_file(explicit, current) then
      error("LJ_BENCH_STOCK_BIN must name stock LuaJIT, not current build: " ..
            explicit, 2)
    end
    return explicit, true
  end

  local candidates = {
    "/usr/bin/luajit",
    "/usr/local/bin/luajit",
    "/opt/homebrew/bin/luajit",
    command_first_line("command -v luajit 2>/dev/null")
  }

  for i = 1, #candidates do
    local candidate = candidates[i]
    if is_executable(candidate) and not same_file(candidate, current) then
      return candidate, false
    end
  end
  return nil, false
end

local function build_and_run_alloc_account(t)
  compile_and_run_c(t, t:tmp("lj_t-gc2-alloc-account-m10"),
                    "t-gc2-alloc-account.c", { timeout = "20s" })
end

local function run_gc_stats(t)
  clean_build(t)

  run_luajit_script_jit_modes(t, "t-gc-stats.lua")
  print("M9 GC stats behavior passed")
end

local function run_trace_hard_assist_cadence(t)
  clean_build(t)

  with_temp_paths(t, { "lj-gc2-hard-cadence.lua" }, function(script)
    local luajit = shell_quote(t:path("src", "luajit"))
    local lua_path = "LUA_PATH=" .. shell_quote(runtime.lua_path(t))
    write_file(script, table.concat({
      'local th = require"threading"',
      'assert(jit and jit.status())',
      'jit.opt.start("hotloop=1", "hotexit=1", "-sink")',
      'local function run(n)',
      '  local s = 0',
      '  local i = 1',
      '  while i <= n do',
      '    local x = i',
      '    local f = function() x = x + 1; return x end',
      '    s = s + f()',
      '    i = i + 1',
      '  end',
      '  return s',
      'end',
      'run(1000)',
      'collectgarbage("collect")',
      'local before = th.gcstats()',
      'local result = run(100000)',
      'local after = th.gcstats()',
      'assert(result == 5000150000)',
      'local allocated = after.alloc_total_bytes - before.alloc_total_bytes',
      'local assists = after.assist_runs - before.assist_runs',
      'assert(allocated > 4 * 1024 * 1024, allocated)',
      'assert(assists <= 64, assists)',
      'print("assist_delta=" .. assists .. " allocated=" .. allocated)',
      'local unique_seed = 0',
      'local function unique_keys(n)',
      '  unique_seed = unique_seed + 1',
      '  local prefix = "trace_unique_" .. unique_seed .. "_"',
      '  local t = {}',
      '  local i = 1',
      '  while i <= n do',
      '    t[prefix .. i] = i',
      '    i = i + 1',
      '  end',
      '  return t, prefix',
      'end',
      'unique_keys(1000)',
      'collectgarbage("collect")',
      'local before_unique = th.gcstats()',
      'local keep, prefix = unique_keys(40000)',
      'local after_unique = th.gcstats()',
      'assert(keep[prefix .. 40000] == 40000)',
      'local worker_delta = after_unique.worker_runs - before_unique.worker_runs',
      'local major_root_delta = after_unique.major_root_scans - before_unique.major_root_scans',
      'assert(worker_delta <= 160, worker_delta)',
      'assert(major_root_delta <= 32, major_root_delta)',
      'print("unique_worker_delta=" .. worker_delta ..',
      '      " unique_major_root_delta=" .. major_root_delta)',
      'collectgarbage("collect")',
      'collectgarbage("stop")',
      'assert(collectgarbage("isrunning") == false)',
      'local before_stopped = th.gcstats()',
      'local stopped_keep, stopped_prefix = unique_keys(40000)',
      'local after_stopped = th.gcstats()',
      'assert(stopped_keep[stopped_prefix .. 40000] == 40000)',
      'local stopped_cycles = after_stopped.cycle_requests - before_stopped.cycle_requests',
      'local stopped_roots = after_stopped.major_root_scans - before_stopped.major_root_scans',
      'assert(stopped_cycles == 0, stopped_cycles)',
      'assert(stopped_roots == 0, stopped_roots)',
      'collectgarbage("restart")',
      'print("stopped_cycle_delta=" .. stopped_cycles ..',
      '      " stopped_major_root_delta=" .. stopped_roots)',
      ""
    }, "\n"))
    assert_command_output_contains(
      lua_path .. " " .. luajit .. " " .. shell_quote(script),
      "assist_delta=",
      { timeout = "20s", stderr = true })
  end)
  print("M9 trace hard-assist cadence behavior passed")
end

local function run_fullgc_smr_reclaim(t)
  clean_build(t)

  with_temp_paths(t, { "lj-fullgc-smr-reclaim.lua" }, function(script)
    local luajit = shell_quote(t:path("src", "luajit"))
    local lua_path = "LUA_PATH=" .. shell_quote(runtime.lua_path(t))
    write_file(script, table.concat({
      'local th = require"threading"',
      'assert(jit and jit.status())',
      'local function churn(n)',
      '  local t = {}',
      '  for i = 1, n do t["fullgc_smr_" .. i] = i end',
      'end',
      'collectgarbage("collect")',
      'local before = th.gcstats()',
      'churn(20000)',
      'for _ = 1, 5 do collectgarbage("collect") end',
      'local after = th.gcstats()',
      'local reclaim_runs = after.smr_reclaim_runs - before.smr_reclaim_runs',
      'local reclaimed = after.smr_reclaimed - before.smr_reclaimed',
      'local major_cycles = after.major_cycle_starts - before.major_cycle_starts',
      'assert(major_cycles >= 5, "full GC did not run GC2 major cycles")',
      'if reclaimed > 0 then',
      '  assert(reclaim_runs > 0, "full GC reclaimed without a reclaim drain")',
      '  local before_flush_kb = collectgarbage("count")',
      '  jit.flush()',
      '  local after_flush_kb = collectgarbage("count")',
      '  assert(before_flush_kb - after_flush_kb < 512,',
      '         "SMR-retired tables were left for a later safepoint")',
      'end',
      'print("smr_reclaimed_delta=" .. reclaimed .. " reclaim_runs_delta=" .. reclaim_runs)',
      ""
    }, "\n"))
    assert_command_output_contains(
      lua_path .. " " .. luajit .. " " .. shell_quote(script),
      "smr_reclaimed_delta=",
      { timeout = "20s", stderr = true })
  end)
  print("M9 full-GC SMR reclaim behavior passed")
end

local function run_newkey_barrier_scope(t)
  clean_build(t)

  with_temp_paths(t, { "lj-newkey-barrier-scope.lua" }, function(script)
    local luajit = shell_quote(t:path("src", "luajit"))
    local lua_path = "LUA_PATH=" .. shell_quote(runtime.lua_path(t))
    write_file(script, table.concat({
      'local th = require"threading"',
      'local n = 12000',
      'collectgarbage("collect")',
      'local before = th.gcstats()',
      'local t = {}',
      'for i = 1, n do t["newkey_barrier_" .. i] = i end',
      'local after_insert = th.gcstats()',
      'local insert_ssb = after_insert.worker_ssb_converted - before.worker_ssb_converted',
      'local insert_grey = after_insert.worker_grey_drained - before.worker_grey_drained',
      'assert(insert_ssb < 1024, "fresh-key insert requeued the table via SSB")',
      'assert(insert_grey < 1024, "fresh-key insert rescanned the table")',
      'before = th.gcstats()',
      'collectgarbage("collect")',
      'local after_collect = th.gcstats()',
      'local collect_grey = after_collect.worker_grey_drained - before.worker_grey_drained',
      'assert(collect_grey < 4096, "full GC drained one table traversal per key")',
      'print("insert_ssb=" .. insert_ssb .. " collect_grey=" .. collect_grey)',
      ""
    }, "\n"))
    assert_command_output_contains(
      lua_path .. " " .. luajit .. " " .. shell_quote(script),
      "insert_ssb=",
      { timeout = "20s", stderr = true })
  end)
  print("M9 fresh-key barrier scope behavior passed")
end

local function run_table_rescan_pressure(t)
  clean_build(t)

  with_temp_paths(t, { "lj-table-rescan-pressure.lua" }, function(script)
    local luajit = shell_quote(t:path("src", "luajit"))
    local lua_path = "LUA_PATH=" .. shell_quote(runtime.lua_path(t))
    write_file(script, table.concat({
      'local th = require"threading"',
      'local rounds = 5',
      'local n = 10000',
      'local keep = {}',
      'collectgarbage("collect")',
      'local before = th.gcstats()',
      'for r = 1, rounds do',
      '  local t = {}',
      '  local prefix = "table_rescan_pressure_" .. r .. "_"',
      '  for i = 1, n do t[i] = prefix .. i end',
      '  keep[r] = t',
      'end',
      'local mid = th.gcstats()',
      'collectgarbage("collect")',
      'local after = th.gcstats()',
      'assert(keep[rounds][n] == "table_rescan_pressure_" .. rounds .. "_" .. n)',
      'local build_ssb = mid.worker_ssb_converted - before.worker_ssb_converted',
      'local total_ssb = after.worker_ssb_converted - before.worker_ssb_converted',
      'local total_grey = after.worker_grey_drained - before.worker_grey_drained',
      'assert(total_ssb < 4096, "table stores flooded SSB: " .. total_ssb)',
      'assert(total_grey < 4096, "table stores flooded grey rescans: " .. total_grey)',
      'print("table_rescan_build_ssb=" .. build_ssb ..',
      '      " total_ssb=" .. total_ssb .. " total_grey=" .. total_grey)',
      ""
    }, "\n"))
    assert_command_output_contains(
      lua_path .. " " .. luajit .. " " .. shell_quote(script),
      "table_rescan_build_ssb=",
      { timeout = "20s", stderr = true })
  end)
  print("M9 active table rescan pressure behavior passed")
end

local function run_bench_smoke(t)
  clean_build(t)

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
  print("M9 benchmark smoke passed")
end

local function run_bench_regression(t)
  clean_build(t)

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

  do
    local missing = t:tmp("lj-suite-utils-missing")
    t:remove(missing)
    assert(utils.read_file_or_nil(missing) == nil,
           "read_file_or_nil must tolerate absent helper artifacts")

    local ok, err = pcall(function()
      capture_command("sh -c " .. shell_quote("printf capture-failed; exit 7"),
                      { stderr = true })
    end)
    err = tostring(err)
    assert(not ok and err:find("command failed %(7%)") and
           err:find("capture%-failed"),
           "capture_command must report failing child status and output")

    ok, err = pcall(function()
      bench_driver.command_output({
        "/bin/sh", "-c", "printf bench-failed; exit 7"
      }, { stderr = true })
    end)
    err = tostring(err)
    assert(not ok and err:find("command failed %(7%)") and
           err:find("bench%-failed"),
           "bench_driver must report failing child status and output")

    ok, err = pcall(function()
      capture_command("sh -c " .. shell_quote("sleep 2"),
                      { timeout = "1s", stderr = true })
    end)
    err = tostring(err)
    assert(not ok and err:find("command failed %("),
           "capture_command must report timeout child status")
  end

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
  print("M9 benchmark regression accounting behavior passed")
end

local function run_bench_stock_compare(t)
  local current = t:path("src", "luajit")
  clean_build(t)

  local stock, explicit = find_stock_luajit(current)
  if not stock then
    print("M9 stock benchmark check skipped; no stock luajit found")
    return
  end

  local bench_lua = t:path("aux", "bench", "bench.lua")
  local filters = os.getenv("LJ_BENCH_STOCK_FILTERS") or
    "arith_loop fib30 tab_hash_write tab_store_existing " ..
    "tab_insert_newkey alloc_tables closures_upval"
  -- The b1.2.0 comparison is a catastrophic-cliff guard, not a parity gate.
  -- Ordinary multi-x gaps remain visible in the output and are b1.2.1 debt;
  -- only regressions on the scale of roughly 100x fail by default.
  local max = tonumber(os.getenv("LJ_BENCH_STOCK_MAX") or
                       os.getenv("BENCH_GEOMEAN_MAX") or "100.0")
  local timeout = os.getenv("LJ_BENCH_STOCK_TIMEOUT") or "60s"
  local samples = tonumber(os.getenv("LJ_BENCH_STOCK_SAMPLES") or "1") or 1
  local closure_samples =
    tonumber(os.getenv("LJ_BENCH_STOCK_CLOSURE_SAMPLES") or "3") or samples
  -- Keep enough iterations for allocation-heavy probes like tab_insert_newkey
  -- and closures_upval; smaller samples are dominated by timer and scheduler
  -- noise and can fail the stock gate without a repeatable throughput cliff.
  local scale = os.getenv("LJ_BENCH_STOCK_SCALE") or
    os.getenv("BENCH_SCALE") or "0.5"

  for filter in filters:gmatch("%S+") do
    local filter_samples = samples
    if filter == "closures_upval" and closure_samples > filter_samples then
      filter_samples = closure_samples
    end
    local result = bench_driver.compare_bins(stock, current, bench_lua, {
      filter = filter,
      max = max,
      timeout = timeout,
      stderr = true,
      env = { BENCH_SCALE = scale },
      samples = filter_samples
    })
    if not result.ok then
      error("stock benchmark regression for " .. filter .. ":\n" ..
            bench_csv.format_compare(result), 2)
    end
    io.write("stock benchmark ", filter, " geomean ",
             ("%.6f"):format(result.geomean or 0), "\n")
  end
  print("M9 stock benchmark check passed with " .. stock ..
        (explicit and "" or " (autodetected)"))
end

local function run_generational(t)
  clean_build(t)

  run_luajit_script_jit_modes(t, "t-gc-generational-mode.lua")
  build_and_run_alloc_account(t)
  print("M10 generational mode behavior passed")
end

local m9_m10_deps = {
  "m9_gc_stats",
  "m9_trace_hard_assist_cadence",
  "m9_fullgc_smr_reclaim",
  "m9_newkey_barrier_scope",
  "m9_table_rescan_pressure",
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
    description = "trace allocation hard-assist cadence behavior",
    run = run_trace_hard_assist_cadence
  })

  add({
    name = "m9_fullgc_smr_reclaim",
    description = "full GC drains SMR-retired side tables",
    run = run_fullgc_smr_reclaim
  })

  add({
    name = "m9_newkey_barrier_scope",
    description = "fresh hash-key barrier does not requeue the whole table",
    run = run_newkey_barrier_scope
  })

  add({
    name = "m9_table_rescan_pressure",
    description = "active table rescans are coalesced while stores continue",
    run = run_table_rescan_pressure
  })

  add({
    name = "m9_bench_smoke",
    description = "benchmark harness smoke",
    run = run_bench_smoke
  })

  add({
    name = "m9_bench_regression",
    description = "benchmark CSV/geomean accounting behavior",
    run = run_bench_regression
  })

  add({
    name = "m9_bench_stock_compare",
    description = "optional stock LuaJIT performance check",
    run = run_bench_stock_compare
  })

  add({
    name = "m10_generational",
    description = "fork-local generational GC mode and accounting behavior",
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
