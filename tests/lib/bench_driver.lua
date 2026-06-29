local bench_csv = require("bench_csv")
local utils = require("suite_utils")

local M = {}

local shell_quote = utils.shell_quote

local function status_ok(ok, why, code)
  if type(ok) == "number" then return ok == 0, ok end
  return ok == true and (code == nil or code == 0), code or why or ok
end

local function argv_command(argv, opts)
  opts = opts or {}
  local parts = {}
  if opts.timeout then
    parts[#parts + 1] = utils.timeout_prefix(opts.timeout)
  end
  for i = 1, #argv do
    parts[#parts + 1] = shell_quote(argv[i])
  end
  local cmd = table.concat(parts, " ")
  if opts.stderr then cmd = cmd .. " 2>&1" end
  return cmd
end

function M.command_output(argv, opts)
  local cmd = argv_command(argv, opts)
  local p, err = io.popen(cmd)
  if not p then error("command failed to start: " .. tostring(err), 2) end
  local out = p:read("*a")
  local ok, why, code = p:close()
  local success, status = status_ok(ok, why, code)
  if not success then
    error("command failed (" .. tostring(status) .. "): " .. cmd ..
          "\n" .. out, 2)
  end
  return out
end

function M.write_file(path, data)
  local f, err = io.open(path, "wb")
  if not f then error(path .. ": " .. tostring(err), 2) end
  local ok, werr = f:write(data)
  f:close()
  if not ok then error(path .. ": " .. tostring(werr), 2) end
end

function M.assert_file(path, what)
  local f = io.open(path, "rb")
  if f then f:close(); return end
  error((what or "file") .. " not found: " .. tostring(path), 2)
end

function M.assert_executable(path, what)
  local ok, why, code = os.execute("test -x " .. shell_quote(path))
  local success = status_ok(ok, why, code)
  if not success then
    error((what or "executable") .. " is not executable: " .. tostring(path), 2)
  end
end

function M.bench_text(bin, bench_lua, jitflag, opts)
  opts = opts or {}
  local argv = { bin }
  if jitflag and jitflag ~= "" then argv[#argv + 1] = jitflag end
  argv[#argv + 1] = bench_lua
  if opts.filter and opts.filter ~= "" then argv[#argv + 1] = opts.filter end
  return M.command_output(argv, { timeout = opts.timeout, stderr = opts.stderr })
end

function M.baseline_csv(bin, bench_lua, opts)
  opts = opts or {}
  local jit_text = M.bench_text(bin, bench_lua, nil, opts)
  local interp_text = M.bench_text(bin, bench_lua, "-joff", opts)
  return bench_csv.baseline_csv_from_text(jit_text, interp_text)
end

function M.write_baseline_csv(bin, bench_lua, out, opts)
  M.assert_executable(bin, "baseline LuaJIT binary")
  M.assert_file(bench_lua, "benchmark harness")
  M.write_file(out, M.baseline_csv(bin, bench_lua, opts))
end

function M.write_aux_baselines(bin, bench_lua, jit_out, interp_out, opts)
  opts = opts or {}
  M.assert_executable(bin, "benchmark LuaJIT binary")
  M.assert_file(bench_lua, "benchmark harness")
  local jit_text = M.bench_text(bin, bench_lua, nil, opts)
  local interp_text = M.bench_text(bin, bench_lua, "-joff", opts)
  M.write_file(jit_out, bench_csv.ns_csv_from_text(jit_text, "jit_ns_per_op"))
  M.write_file(interp_out,
               bench_csv.ns_csv_from_text(interp_text, "interp_ns_per_op"))
end

function M.compare_bins(old_bin, new_bin, bench_lua, opts)
  opts = opts or {}
  M.assert_executable(old_bin, "old benchmark LuaJIT binary")
  M.assert_executable(new_bin, "new benchmark LuaJIT binary")
  M.assert_file(bench_lua, "benchmark harness")
  return bench_csv.compare_bench_text(
    M.bench_text(old_bin, bench_lua, nil, opts),
    M.bench_text(new_bin, bench_lua, nil, opts),
    opts)
end

local function split_words(s)
  local out = {}
  for w in tostring(s or ""):gmatch("%S+") do out[#out + 1] = w end
  return out
end

function M.run_scaling(bin, bench_mt_lua, opts)
  opts = opts or {}
  M.assert_executable(bin, "scaling LuaJIT binary")
  M.assert_file(bench_mt_lua, "MT benchmark harness")
  local threads = split_words(opts.threads or "1 2 4 8")
  if #threads == 0 then error("empty BENCH_THREADS", 2) end
  for i = 1, #threads do
    local argv = { bin, bench_mt_lua, threads[i] }
    if opts.filter and opts.filter ~= "" then argv[#argv + 1] = opts.filter end
    io.write("== ", threads[i], " threads ==\n")
    io.write(M.command_output(argv, { timeout = opts.timeout, stderr = true }))
  end
end

M.shell_quote = shell_quote

return M
