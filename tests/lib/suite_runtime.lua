local utils = require("suite_utils")

local M = {}

local shell_quote = utils.shell_quote

local function copy_env(env)
  local out = {}
  if env then
    for k, v in pairs(env) do out[k] = v end
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

function M.capture_luajit(t, args, out, opts)
  opts = opts or {}
  local parts = { "LUA_PATH=" .. shell_quote(M.lua_path(t)),
                  shell_quote(t:path("src", "luajit")) }
  for i = 1, #args do parts[#parts + 1] = shell_quote(args[i]) end
  t:run(table.concat(parts, " ") .. " >" .. shell_quote(out),
        { quiet = opts.quiet })
end

function M.build_default(t)
  t:make(nil, { quiet = true, jobs = false })
end

function M.clean_build(t, opts)
  opts = opts or {}
  t:build({ clean = true, quiet = true, xcflags = opts.xcflags })
end

function M.compile_and_run_c(t, out, cfile, opts)
  opts = opts or {}
  t:compile_luajit_c_fixture(out, cfile, {
    cflags = opts.cflags,
    default_cflags = opts.default_cflags,
    include_src = opts.include_src,
    link_luajit = opts.link_luajit,
    objects = opts.objects,
    libs = opts.libs,
    pthread = opts.pthread,
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

return M
