local utils = require("suite_utils")
local optutils = require("suite_opts")

local M = {}

local luajit_fixture_libs = utils.luajit_fixture_libs
local shell_quote = utils.shell_quote
local tab_test_helper_flag = "-DLJ_TAB_TEST_HELPERS"
local func_test_helper_flag = "-DLJ_FUNC_TEST_HELPERS"
local trace_test_helper_flag = "-DLJ_TRACE_TEST_HELPERS"
local gc2_test_helper_flag = "-DLJ_GC2_TEST_HELPERS"
local assert_flag = "-DLUA_USE_ASSERT"
local gc2_paranoia_flags = assert_flag .. " -DLJ_GC2_PARANOIA=1"

M.assert_flag = assert_flag
M.gc2_test_helper_flag = gc2_test_helper_flag
M.gc2_paranoia_flags = gc2_paranoia_flags
M.gc2_paranoia_nojit_flags = gc2_paranoia_flags .. " -DLUAJIT_DISABLE_JIT"

local function copy_run_opts(opts)
  local out = optutils.copy(opts)
  if opts and opts.env then out.env = optutils.copy(opts.env) end
  return out
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
  local xcflags
  local make_args = opts.args or {}
  local passthrough = {}
  for i = 1, #make_args do
    local v = make_args[i]
    local flags = v:match("^XCFLAGS=(.*)$")
    if flags ~= nil then
      xcflags = flags
    else
      passthrough[#passthrough + 1] = v
    end
  end
  if #passthrough ~= 0 then
    t:make(opts.args, { quiet = quiet, jobs = jobs })
  else
    t:build({ quiet = quiet, jobs = jobs, xcflags = xcflags })
  end
end

function M.clean_build(t, opts)
  opts = opts or {}
  local quiet = opts.quiet
  if quiet == nil then quiet = true end
  t:build({
    clean = true,
    quiet = quiet,
    jobs = opts.jobs,
    xcflags = opts.xcflags
  })
end

local function helper_build_opts(opts, flag)
  local out = optutils.copy(opts)
  if out.clean == nil then out.clean = true end
  if out.xcflags == nil then out.xcflags = flag end
  return out
end

local function helper_c_opts(opts, flag)
  local out = optutils.copy(opts)
  if out.cflags == nil then out.cflags = flag end
  return out
end

local function helper_opts(opts, flag)
  return helper_c_opts(helper_build_opts(opts, flag), flag)
end

function M.assert_opts(opts)
  return helper_c_opts(helper_build_opts(opts, assert_flag), assert_flag)
end

function M.gc2_paranoia_opts(opts)
  return helper_c_opts(helper_build_opts(opts, gc2_paranoia_flags),
                       gc2_paranoia_flags)
end

function M.gc2_test_helper_opts(opts)
  return helper_opts(opts, gc2_test_helper_flag)
end

function M.tab_helper_build_opts(opts)
  return helper_build_opts(opts, tab_test_helper_flag)
end

function M.tab_helper_c_opts(opts)
  return helper_c_opts(opts, tab_test_helper_flag)
end

function M.tab_helper_opts(opts)
  return helper_opts(opts, tab_test_helper_flag)
end

function M.func_helper_build_opts(opts)
  return helper_build_opts(opts, func_test_helper_flag)
end

function M.func_helper_c_opts(opts)
  return helper_c_opts(opts, func_test_helper_flag)
end

function M.func_helper_opts(opts)
  return helper_opts(opts, func_test_helper_flag)
end

function M.trace_helper_build_opts(opts)
  return helper_build_opts(opts, trace_test_helper_flag)
end

function M.trace_helper_c_opts(opts)
  return helper_c_opts(opts, trace_test_helper_flag)
end

function M.trace_helper_opts(opts)
  return helper_opts(opts, trace_test_helper_flag)
end

function M.with_default_build_restore(t, fn, opts)
  opts = opts or {}
  local quiet = opts.quiet
  if quiet == nil then quiet = true end
  local ok, err = xpcall(fn, debug.traceback)
  local restore_ok, restore_err = xpcall(function()
    t:build({ clean = true, quiet = quiet, jobs = opts.jobs })
  end, debug.traceback)
  if not ok then
    if not restore_ok then
      err = err .. "\n\n(default build restore also failed)\n" .. restore_err
    end
    error(err, 0)
  end
  if not restore_ok then error(restore_err, 0) end
end

function M.compile_and_run_c(t, out, cfile, opts)
  opts = opts or {}
  local sources = opts.sources or
    (type(cfile) == "table" and cfile or { t:path("tests", cfile) })
  M.compile_and_run_sources(t, out, sources, opts)
end

function M.compile_and_run_sources(t, out, sources, opts)
  opts = opts or {}
  local run_opts = copy_run_opts(opts)
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
    env = run_opts.env,
    timeout = run_opts.timeout,
    quiet = run_opts.quiet
  })
end

function M.build_and_run_c(t, out, cfile, opts)
  opts = opts or {}
  local run_opts = copy_run_opts(opts)
  if opts.build ~= false then
    if opts.clean then
      M.clean_build(t, opts)
    else
      M.build_default(t)
    end
  end
  M.compile_and_run_c(t, out, cfile, run_opts)
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

function M.run_c_fixture_specs(t, specs, common_opts)
  common_opts = common_opts or {}
  for i = 1, #specs do
    local spec = specs[i]
    local opts = optutils.with(common_opts, spec.opts)
    local cfile = spec.cfile or (spec.name .. ".c")
    local output = spec.output or ("lj_" .. cfile:gsub("[.]c$", ""))
    M.compile_and_run_c(t, t:tmp(output), cfile, opts)
  end
end

function M.build_shared_library(t, out, cfile, opts)
  opts = opts or {}
  local cflags = opts.cflags or "-O2 -Wall -Wextra -Werror"
  local sources = opts.sources or
    (type(cfile) == "table" and cfile or { t:path("tests", cfile) })
  local parts = { t.compiler, "-shared", "-fPIC", cflags }
  for i = 1, #sources do parts[#parts + 1] = shell_quote(sources[i]) end
  parts[#parts + 1] = "-o"
  parts[#parts + 1] = shell_quote(out)
  t:run(table.concat(parts, " "), { quiet = opts.quiet ~= false })
  return out
end

function M.write_ld_script(path, inputs)
  if type(inputs) == "table" then inputs = table.concat(inputs, " ") end
  return utils.write_file(path, "/* GNU ld script\nINPUT(" .. inputs .. ")\n")
end

return M
