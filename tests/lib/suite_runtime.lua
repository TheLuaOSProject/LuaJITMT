local utils = require("suite_utils")

local M = {}

local shell_quote = utils.shell_quote
local luajit_fixture_libs = utils.luajit_fixture_libs

local function luajit_bin(t, bin)
  bin = bin or t:path("src", "luajit")
  if bin:sub(1, 1) == "/" then return bin end
  return t:path(bin)
end

local function copy_env(env)
  local out = {}
  if env then
    for k, v in pairs(env) do out[k] = v end
  end
  return out
end

local function copy_opts(opts)
  local out = {}
  if opts then
    for k, v in pairs(opts) do out[k] = v end
  end
  return out
end

function M.lua_path(t)
  return utils.lua_path(t.root)
end

function M.lua_path_env(t, env)
  local out = copy_env(env)
  out.LUA_PATH = M.lua_path(t)
  return out
end

function M.luajit(t, args, opts)
  opts = opts or {}
  t:luajit(args, {
    env = opts.lua_path == false and opts.env or M.lua_path_env(t, opts.env),
    timeout = opts.timeout,
    quiet = opts.quiet
  })
end

function M.luajit_code(t, code, opts)
  M.luajit(t, { "-e", code }, opts)
end

function M.luajit_file(t, file, opts)
  opts = opts or {}
  local runopts = {
    env = opts.lua_path and M.lua_path_env(t, opts.env) or opts.env,
    timeout = opts.timeout,
    quiet = opts.quiet
  }
  t:luajit({ file }, runopts)
end

function M.luajit_script(t, script, args, opts)
  args = args or {}
  opts = opts or {}
  local argv = {}
  if opts.joff then argv[#argv + 1] = "-joff" end
  argv[#argv + 1] = t:path("tests", script)
  for i = 1, #args do argv[#argv + 1] = args[i] end
  t:luajit(argv, { timeout = opts.timeout, env = opts.env, quiet = opts.quiet })
end

function M.build_and_run_luajit_script(t, script, args, opts)
  opts = opts or {}
  if opts.build ~= false then
    local build_quiet = opts.build_quiet
    if build_quiet == nil then build_quiet = true end
    t:build({
      clean = opts.clean ~= false,
      quiet = build_quiet,
      xcflags = opts.xcflags
    })
  end
  M.luajit_script(t, script, args, opts)
end

function M.run_luajit_script_jit_modes(t, script, args, opts)
  local joff_opts = copy_opts(opts)
  local jit_opts = copy_opts(opts)
  joff_opts.joff = true
  jit_opts.joff = false
  M.luajit_script(t, script, args, joff_opts)
  M.luajit_script(t, script, args, jit_opts)
end

function M.build_and_run_luajit_script_jit_modes(t, script, args, opts)
  opts = opts or {}
  if opts.build ~= false then
    local build_quiet = opts.build_quiet
    if build_quiet == nil then build_quiet = true end
    t:build({
      clean = opts.clean ~= false,
      quiet = build_quiet,
      xcflags = opts.xcflags
    })
  end
  M.run_luajit_script_jit_modes(t, script, args, opts)
end

function M.luajit_dump(t, dump, dumpopt, code, opts)
  opts = opts or {}
  local parts = { "LUA_PATH=" .. shell_quote(M.lua_path(t)) }
  if opts.timeout then parts[#parts + 1] = "timeout " .. shell_quote(opts.timeout) end
  parts[#parts + 1] = shell_quote(t:path("src", "luajit"))
  parts[#parts + 1] = shell_quote(dumpopt)
  parts[#parts + 1] = "-e " .. shell_quote(code)
  local redirect = " >" .. shell_quote(dump)
  if opts.stderr ~= false then redirect = redirect .. " 2>&1" end
  t:run(table.concat(parts, " ") .. redirect, { quiet = opts.quiet })
end

function M.luajit_dump_file(t, dump, dumpopt, file, args, opts)
  args = args or {}
  opts = opts or {}
  local parts = { "LUA_PATH=" .. shell_quote(M.lua_path(t)) }
  if opts.timeout then parts[#parts + 1] = "timeout " .. shell_quote(opts.timeout) end
  parts[#parts + 1] = shell_quote(t:path("src", "luajit"))
  parts[#parts + 1] = shell_quote(dumpopt)
  parts[#parts + 1] = shell_quote(file)
  for i = 1, #args do parts[#parts + 1] = shell_quote(args[i]) end
  local redirect = " >" .. shell_quote(dump)
  if opts.stderr ~= false then redirect = redirect .. " 2>&1" end
  t:run(table.concat(parts, " ") .. redirect, { quiet = opts.quiet })
end

function M.capture_luajit(t, args, out, opts)
  opts = opts or {}
  local parts = { "LUA_PATH=" .. shell_quote(M.lua_path(t)),
                  shell_quote(t:path("src", "luajit")) }
  for i = 1, #args do parts[#parts + 1] = shell_quote(args[i]) end
  t:run(table.concat(parts, " ") .. " >" .. shell_quote(out),
        { quiet = opts.quiet })
end

function M.run_lua_test_case(t, name, opts)
  t:run({ t:path("tools", "ci", "lua_test.sh"), name }, opts)
end

function M.run_lua_test_cases(t, names, opts)
  local cmd = { t:path("tools", "ci", "lua_test.sh") }
  for i = 1, #names do cmd[#cmd + 1] = names[i] end
  t:run(cmd, opts)
end

function M.run_stock(t, args, opts)
  args = args or {}
  opts = opts or {}
  local bin = luajit_bin(t, opts.bin)
  if opts.check_executable then
    t:run({ "test", "-x", bin }, { quiet = true })
  end
  local parts = {
    "cd " .. shell_quote(t:path("tests", "stock", "test")),
    "LUA_PATH=" .. shell_quote(M.lua_path(t)) .. " "
  }
  if opts.timeout then parts[2] = parts[2] .. "timeout " .. shell_quote(opts.timeout) .. " " end
  parts[2] = parts[2] .. shell_quote(bin)
  for i = 1, #args do parts[2] = parts[2] .. " " .. shell_quote(args[i]) end
  t:run(parts[1] .. " && " .. parts[2], { quiet = opts.quiet })
end

function M.run_stock_cli(t, args, opts)
  args = args or {}
  opts = opts or {}
  local bin = args[1] or opts.bin
  local stock_args = { "test.lua" }
  for i = 2, #args do stock_args[#stock_args + 1] = args[i] end
  local runopts = copy_env(opts)
  runopts.bin = bin
  if runopts.check_executable == nil then runopts.check_executable = true end
  M.run_stock(t, stock_args, runopts)
end

function M.make_clean(t, opts)
  opts = opts or {}
  local quiet = opts.quiet
  if quiet == nil then quiet = true end
  t:make({ "clean" }, { quiet = quiet, jobs = false })
end

function M.build_default(t, opts)
  opts = opts or {}
  local quiet = opts.quiet
  if quiet == nil then quiet = true end
  local jobs = opts.jobs
  if jobs == nil then jobs = false end
  t:make(opts.args, { quiet = quiet, jobs = jobs })
end

function M.clean_build(t, opts)
  opts = opts or {}
  t:build({ clean = true, quiet = true, xcflags = opts.xcflags })
end

function M.compile_and_run_c(t, out, cfile, opts)
  opts = opts or {}
  local sources = opts.sources or
    (type(cfile) == "table" and cfile or { t:path("tests", cfile) })
  M.compile_and_run_sources(t, out, sources, opts)
end

function M.compile_and_run_sources(t, out, sources, opts)
  opts = opts or {}
  t:cc(out, sources, {
    cflags = opts.cflags,
    default_cflags = opts.default_cflags,
    include_src = opts.include_src,
    link_luajit = opts.link_luajit ~= false,
    objects = opts.objects,
    libs = luajit_fixture_libs(opts),
    quiet = opts.quiet
  })
  t:run({ out }, {
    env = opts.env,
    timeout = opts.timeout,
    quiet = opts.quiet
  })
end

function M.build_and_run_c(t, out, cfile, opts)
  opts = opts or {}
  if opts.build ~= false then
    if opts.clean then
      M.clean_build(t, opts)
    else
      M.build_default(t)
    end
  end
  M.compile_and_run_c(t, out, cfile, opts)
end

function M.run_c_fixtures(t, names, opts)
  opts = opts or {}
  local prefix = opts.output_prefix or "lj_"
  local suffix = opts.output_suffix or ""
  for i = 1, #names do
    local name = names[i]
    M.compile_and_run_c(t, t:tmp(prefix .. name .. suffix),
                        name .. ".c", opts)
  end
end

function M.add_luajit_c_fixture_cases(add, specs)
  for i = 1, #specs do
    local spec = specs[i]
    local name = spec.name
    local description = spec.description
    local output = spec.output
    local cfile = spec.cfile
    local opts = spec.opts
    local message = spec.message
    add({
      name = name,
      description = description,
      run = function(t)
        local runopts = copy_opts(opts)
        if runopts.clean == nil then runopts.clean = true end
        if runopts.quiet == nil then runopts.quiet = true end
        M.build_and_run_c(t, t:tmp(output), cfile, runopts)
        if message then print(message) end
      end
    })
  end
end

return M
