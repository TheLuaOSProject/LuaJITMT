local bench_csv = require("bench_csv")
local bench_driver = require("bench_driver")

local function read_file(path)
  local f, err = io.open(path, "rb")
  if not f then
    io.stderr:write(path .. ": " .. tostring(err) .. "\n")
    os.exit(2)
  end
  local data = f:read("*a")
  f:close()
  return data
end

local function usage()
  io.stderr:write(table.concat({
    "usage:",
    "  bench_csv_cli.lua baseline-csv <jit.txt> <interp.txt>",
    "  bench_csv_cli.lua ns-csv <bench.txt> <column>",
    "  bench_csv_cli.lua compare-csv <baseline.csv> <current.csv>",
    "  bench_csv_cli.lua compare-bench-text <baseline.txt> <current.txt>",
    "  bench_csv_cli.lua run-baseline <bin> <bench.lua> <out.csv>",
    "  bench_csv_cli.lua aux-baseline <bin> <bench.lua> <jit.csv> <interp.csv>",
    "  bench_csv_cli.lua aux-compare <old-bin> <new-bin> <bench.lua>",
    "  bench_csv_cli.lua aux-scaling <bin> <bench_mt.lua>",
    "  bench_csv_cli.lua aux-run <aux-bench-dir> <mode> [args...]",
    ""
  }, "\n"))
  os.exit(2)
end

local function compare_exit_code(result)
  if result.ok then return 0 end
  for i = 1, #result.errors do
    if result.errors[i]:match("^missing column") then return 2 end
  end
  return 1
end

local function write_compare(result)
  local out = bench_csv.format_compare(result)
  if result.ok then
    io.write(out)
  else
    io.stderr:write(out)
  end
  os.exit(compare_exit_code(result))
end

local function driver_opts()
  return {
    filter = os.getenv("BENCH_FILTER"),
    threads = os.getenv("BENCH_THREADS"),
    column = os.getenv("BENCH_COLUMN") or "jit_ns_per_op",
    max = tonumber(os.getenv("BENCH_GEOMEAN_MAX") or "1.10")
  }
end

local function run_driver(fn)
  local ok, err = pcall(fn)
  if not ok then
    io.stderr:write(tostring(err), "\n")
    os.exit(1)
  end
end

local function path_join(a, b)
  if a:sub(-1) == "/" then return a .. b end
  return a .. "/" .. b
end

local function trim(s)
  return (s:gsub("%s+$", ""))
end

local function run_aux_mode(dir, mode)
  if mode == "baseline" then
    if not arg[4] or arg[5] then usage() end
    local host = trim(bench_driver.command_output({ "hostname" }))
    bench_driver.write_aux_baselines(
      arg[4],
      path_join(dir, "bench.lua"),
      path_join(dir, "baseline_jit_" .. host .. ".csv"),
      path_join(dir, "baseline_interp_" .. host .. ".csv"),
      driver_opts())
    io.write("wrote baseline CSVs\n")
  elseif mode == "compare" then
    if not arg[4] or not arg[5] or arg[6] then usage() end
    write_compare(bench_driver.compare_bins(arg[4], arg[5],
                                            path_join(dir, "bench.lua"),
                                            driver_opts()))
  elseif mode == "scaling" then
    if not arg[4] or arg[5] then usage() end
    bench_driver.run_scaling(arg[4], path_join(dir, "bench_mt.lua"),
                             driver_opts())
  else
    io.stderr:write("unknown mode ", tostring(mode), "\n")
    os.exit(2)
  end
end

local cmd = arg and arg[1]
if cmd == "baseline-csv" then
  if not arg[2] or not arg[3] or arg[4] then usage() end
  io.write(bench_csv.baseline_csv_from_text(read_file(arg[2]), read_file(arg[3])))
elseif cmd == "ns-csv" then
  if not arg[2] or not arg[3] or arg[4] then usage() end
  io.write(bench_csv.ns_csv_from_text(read_file(arg[2]), arg[3]))
elseif cmd == "compare-csv" then
  if not arg[2] or not arg[3] or arg[4] then usage() end
  write_compare(bench_csv.compare(read_file(arg[2]), read_file(arg[3]), {
    column = os.getenv("BENCH_COLUMN") or "jit_ns_per_op",
    max = tonumber(os.getenv("BENCH_GEOMEAN_MAX") or "1.10")
  }))
elseif cmd == "compare-bench-text" then
  if not arg[2] or not arg[3] or arg[4] then usage() end
  write_compare(bench_csv.compare_bench_text(read_file(arg[2]), read_file(arg[3]), {
    column = os.getenv("BENCH_COLUMN") or "jit_ns_per_op",
    max = tonumber(os.getenv("BENCH_GEOMEAN_MAX") or "1.10")
  }))
elseif cmd == "run-baseline" then
  if not arg[2] or not arg[3] or not arg[4] or arg[5] then usage() end
  run_driver(function()
    bench_driver.write_baseline_csv(arg[2], arg[3], arg[4], driver_opts())
    io.write("wrote ", arg[4], "\n")
  end)
elseif cmd == "aux-baseline" then
  if not arg[2] or not arg[3] or not arg[4] or not arg[5] or arg[6] then
    usage()
  end
  run_driver(function()
    bench_driver.write_aux_baselines(arg[2], arg[3], arg[4], arg[5],
                                     driver_opts())
    io.write("wrote baseline CSVs\n")
  end)
elseif cmd == "aux-compare" then
  if not arg[2] or not arg[3] or not arg[4] or arg[5] then usage() end
  run_driver(function()
    write_compare(bench_driver.compare_bins(arg[2], arg[3], arg[4],
                                            driver_opts()))
  end)
elseif cmd == "aux-scaling" then
  if not arg[2] or not arg[3] or arg[4] then usage() end
  run_driver(function()
    bench_driver.run_scaling(arg[2], arg[3], driver_opts())
  end)
elseif cmd == "aux-run" then
  if not arg[2] or not arg[3] then usage() end
  run_driver(function()
    run_aux_mode(arg[2], arg[3])
  end)
else
  usage()
end
