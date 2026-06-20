local utils = require("suite_utils")
local build = require("suite_build")
local optutils = require("suite_opts")

local M = {}

local shell_quote = utils.shell_quote

M.make_clean = build.make_clean
M.build_default = build.build_default
M.clean_build = build.clean_build
M.compile_and_run_c = build.compile_and_run_c
M.compile_and_run_sources = build.compile_and_run_sources
M.build_and_run_c = build.build_and_run_c
M.run_c_fixtures = build.run_c_fixtures

local function luajit_bin(t, bin)
  bin = bin or t:path("src", "luajit")
  if bin:sub(1, 1) == "/" then return bin end
  return t:path(bin)
end

local function append_jit_mode(argv, opts)
  if opts.joff then
    argv[#argv + 1] = "-joff"
  elseif opts.jon then
    argv[#argv + 1] = "-jon"
  end
end

function M.lua_path(t)
  return t:path("tests", "lib", "?.lua") .. ";" .. utils.lua_path(t.root)
end

function M.lua_path_env(t, env)
  local out = optutils.copy(env)
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
  opts = opts or {}
  local argv = {}
  append_jit_mode(argv, opts)
  argv[#argv + 1] = "-e"
  argv[#argv + 1] = code
  M.luajit(t, argv, opts)
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
  append_jit_mode(argv, opts)
  argv[#argv + 1] = t:path("tests", script)
  for i = 1, #args do argv[#argv + 1] = args[i] end
  t:luajit(argv, {
    timeout = opts.timeout,
    env = opts.lua_path == false and opts.env or M.lua_path_env(t, opts.env),
    quiet = opts.quiet
  })
end

function M.build_and_run_luajit_code(t, code, opts)
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
  M.luajit_code(t, code, opts)
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
  local joff_opts = optutils.copy(opts)
  local jit_opts = optutils.copy(opts)
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
  utils.run_case(require("init"), t, name)
end

function M.run_lua_test_cases(t, names, opts)
  for i = 1, #names do
    M.run_lua_test_case(t, names[i])
  end
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
  local runopts = optutils.copy(opts)
  runopts.bin = bin
  if runopts.check_executable == nil then runopts.check_executable = true end
  M.run_stock(t, stock_args, runopts)
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
        local runopts = optutils.copy(opts)
        if runopts.clean == nil then runopts.clean = true end
        if runopts.quiet == nil then runopts.quiet = true end
        M.build_and_run_c(t, t:tmp(output), cfile, runopts)
        if message then print(message) end
      end
    })
  end
end

function M.add_luajit_script_cases(add, specs)
  for i = 1, #specs do
    local spec = specs[i]
    local name = spec.name
    local description = spec.description
    local script = spec.script
    local args = spec.args
    local opts = spec.opts
    local message = spec.message
    add({
      name = name,
      description = description or (script .. " under the built VM"),
      run = function(t)
        M.build_and_run_luajit_script(t, script, args, optutils.copy(opts))
        if message then print(message) end
      end
    })
  end
end

return M
