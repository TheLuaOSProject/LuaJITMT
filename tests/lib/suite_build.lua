local utils = require("suite_utils")
local optutils = require("suite_opts")

local M = {}

local luajit_fixture_libs = utils.luajit_fixture_libs
local shell_quote = utils.shell_quote

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
