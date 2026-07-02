local utils = require("suite_utils")

local M = {}

local Test = {}
Test.__index = Test

local getenv = utils.getenv
local shell_quote = utils.shell_quote
local read_file = utils.read_file
local write_file = utils.write_file
local file_exists = utils.file_exists

local function read_raw_file(path)
  local f = io.open(path, "rb")
  if not f then return nil end
  local data = f:read("*a")
  f:close()
  return data
end

local function build_profile_signature(xcflags)
  return "default\nXCFLAGS=" .. (xcflags or "") .. "\n"
end

local function build_signature_path(self)
  return self:path("src", ".lj-test-build-signature")
end

local function clear_build_signature(self)
  self.build_signature = nil
  os.remove(build_signature_path(self))
end

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
    cflags = getenv("CFLAGS", "-std=gnu11 -O2 -Wall -Wextra -Werror -mcx16"),
    jobs = getenv("JOBS", getenv("MAKE_JOBS", utils.detect_jobs())),
    tmpdir = getenv("TMPDIR", "/tmp"),
    build_signature = nil
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
  if utils.is_repository_source_path(self.root, path) then
    error("tests must not read repository source as a pass/fail oracle: " ..
          path, 2)
  end
  return read_file(path)
end

function Test:files(dir, opts)
  error("repository file enumeration is not a supported test oracle; use " ..
        "behavior fixtures or generated artifacts", 2)
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
    append(parts, utils.timeout_prefix(opts.timeout))
  end
  if type(cmd) == "table" then
    append_argv(parts, cmd)
  else
    append(parts, cmd)
  end
  local full = table.concat(parts, " ")
  if opts.stdout then
    full = full .. " >" .. shell_quote(opts.stdout)
  end
  if opts.stderr_to_stdout then
    full = full .. " 2>&1"
  elseif opts.stderr then
    full = full .. " 2>" .. shell_quote(opts.stderr)
  end
  if opts.cwd then
    full = "cd " .. shell_quote(opts.cwd) .. " && " .. full
  end
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
  if not opts.keep_build_signature then clear_build_signature(self) end
end

function Test:build(opts)
  opts = opts or {}
  local args = {}
  local stamp = build_signature_path(self)
  local signature = build_profile_signature(opts.xcflags)
  local disk_signature = self.build_signature
  local have_outputs = file_exists(self:path("src", "luajit")) and
    file_exists(self:path("src", "libluajit.a"))
  if disk_signature == nil then disk_signature = read_raw_file(stamp) end
  if opts.clean and disk_signature == signature and have_outputs and
     not getenv("LJ_TEST_DISABLE_BUILD_CACHE", nil) then
    self.build_signature = signature
    return
  end
  if opts.clean or (have_outputs and disk_signature ~= signature) then
    self:make({ "clean" }, { quiet = opts.quiet, jobs = false })
  end
  if opts.xcflags then args[#args + 1] = "XCFLAGS=" .. opts.xcflags end
  self:make(args, {
    quiet = opts.quiet,
    jobs = opts.jobs,
    keep_build_signature = true
  })
  write_file(stamp, signature)
  self.build_signature = signature
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
