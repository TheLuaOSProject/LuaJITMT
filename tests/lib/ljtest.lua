local utils = require("suite_utils")
local checks = require("suite_assert")

local M = {}

local Test = {}
Test.__index = Test

local getenv = utils.getenv
local shell_quote = utils.shell_quote
local read_file = utils.read_file
local has_extension = utils.has_extension
local assert_not_source_file_content = checks.assert_not_source_file_content

local function append(parts, value)
  if value == nil or value == "" then return end
  parts[#parts + 1] = value
end

local function append_argv(parts, argv)
  for i = 1, #argv do
    append(parts, shell_quote(argv[i]))
  end
end

local function append_flags(parts, flags)
  if type(flags) == "table" then
    for i = 1, #flags do append(parts, flags[i]) end
  else
    append(parts, flags)
  end
end

function M.new(root)
  local self = {
    root = root,
    compiler = getenv("CC", "cc"),
    cflags = getenv("CFLAGS", "-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"),
    jobs = getenv("JOBS", getenv("MAKE_JOBS", utils.detect_jobs())),
    tmpdir = getenv("TMPDIR", "/tmp")
  }
  return setmetatable(self, Test)
end

function Test:path(...)
  local out = self.root
  for i = 1, select("#", ...) do
    out = out .. "/" .. select(i, ...)
  end
  return out
end

function Test:tmp(name)
  return self.tmpdir .. "/" .. name
end

function Test:read(path)
  assert_not_source_file_content(path, 2)
  return read_file(path)
end

function Test:files(dir, opts)
  opts = opts or {}
  local cmd = "find " .. shell_quote(dir)
  if opts.recursive == false then cmd = cmd .. " -maxdepth 1" end
  cmd = cmd .. " -type f -print"
  local p, err = io.popen(cmd)
  if not p then error("find failed: " .. tostring(err), 2) end
  local files = {}
  for path in p:lines() do
    if has_extension(path, opts.extensions) then
      files[#files + 1] = path
    end
  end
  local ok, why, code = p:close()
  if not ok then
    error("find failed (" .. tostring(code or why or ok) .. "): " .. cmd, 2)
  end
  table.sort(files)
  return files
end

function Test:remove(path)
  os.remove(path)
end

function Test:tempname(prefix)
  prefix = prefix or "lj-test"
  local base = os.tmpname()
  os.remove(base)
  local name = base .. "-" .. prefix
  os.remove(name)
  return name
end

function Test:run(cmd, opts)
  opts = opts or {}
  local parts = {}
  if opts.env then
    for k, v in pairs(opts.env) do
      append(parts, k .. "=" .. shell_quote(v))
    end
  end
  if opts.timeout then
    append(parts, "timeout " .. shell_quote(opts.timeout))
  end
  if type(cmd) == "table" then
    append_argv(parts, cmd)
  else
    append(parts, cmd)
  end
  local full = table.concat(parts, " ")
  if not opts.quiet then
    io.stderr:write("+ " .. full .. "\n")
  end
  local ok, why, code = os.execute(full)
  local success
  if type(ok) == "number" then
    success = ok == 0
    code = ok
  else
    success = ok == true and (code == nil or code == 0)
  end
  if not success then
    error("command failed (" .. tostring(code or why or ok) .. "): " .. full, 2)
  end
end

function Test:make(args, opts)
  opts = opts or {}
  local cmd = { "make", "-C", self:path("src") }
  if opts.jobs ~= false then
    cmd[#cmd + 1] = "-j" .. self.jobs
  end
  if args then
    for i = 1, #args do cmd[#cmd + 1] = args[i] end
  end
  self:run(cmd, opts)
end

function Test:build(opts)
  opts = opts or {}
  if opts.clean then
    self:make({ "clean" }, { quiet = opts.quiet, jobs = false })
  end
  local args = {}
  if opts.xcflags then args[#args + 1] = "XCFLAGS=" .. opts.xcflags end
  self:make(args, { quiet = opts.quiet })
end

function Test:cc(output, sources, opts)
  opts = opts or {}
  local parts = { self.compiler }
  if opts.default_cflags ~= false then append(parts, self.cflags) end
  append_flags(parts, opts.cflags)
  if opts.include_src ~= false then
    append(parts, "-I" .. shell_quote(self:path("src")))
  end
  for i = 1, #sources do
    append(parts, shell_quote(sources[i]))
  end
  if opts.objects then
    for i = 1, #opts.objects do append(parts, shell_quote(opts.objects[i])) end
  end
  if opts.link_luajit then
    append(parts, shell_quote(self:path("src", "libluajit.a")))
  end
  if opts.libs then
    for i = 1, #opts.libs do append(parts, opts.libs[i]) end
  end
  append(parts, "-o " .. shell_quote(output))
  self:run(table.concat(parts, " "), { quiet = opts.quiet })
end

function Test:luajit(args, opts)
  opts = opts or {}
  local argv = { self:path("src", "luajit") }
  for i = 1, #args do argv[#argv + 1] = args[i] end
  self:run(argv, opts)
end

return M
