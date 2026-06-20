local bench_csv = require("bench_csv")

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
else
  usage()
end
