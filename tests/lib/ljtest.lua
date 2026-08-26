local utils = require("suite_utils")

local M = {}

local Test = {}
Test.__index = Test

local getenv = utils.getenv
local shell_quote = utils.shell_quote
local read_file = utils.read_file
local read_file_or_nil = utils.read_file_or_nil
local write_file = utils.write_file
local file_exists = utils.file_exists
local command_succeeded = utils.command_succeeded

local function build_profile_signature(xcflags, env)
  local deploy = env and env.MACOSX_DEPLOYMENT_TARGET or ""
  return "default\nXCFLAGS=" .. (xcflags or "") ..
         "\nMACOSX_DEPLOYMENT_TARGET=" .. deploy .. "\n"
end

local function build_signature_path(self)
  return self:path("src", ".lj-test-build-signature")
end

local function clear_build_signature(self)
  self.build_signature = nil
  os.remove(build_signature_path(self))
end

local function command_first_line(cmd)
  local f = io.popen(cmd, "r")
  if not f then return nil end
  local line = f:read("*l")
  f:close()
  return line
end

local function tracked_build_input_newer_than(self, output)
  local script =
    "ref=$1; shift; " ..
    "for f do " ..
      "if [ \"$f\" -nt \"$ref\" ]; then printf '%s\\n' \"$f\"; exit 0; fi; " ..
    "done"
  local cmd = "cd " .. shell_quote(self.root) ..
    " && git ls-files -z -- src dynasm | xargs -0 sh -c " ..
    shell_quote(script) .. " sh " .. shell_quote(output)
  return command_first_line(cmd) ~= nil
end

local function build_outputs_current(self)
  return not tracked_build_input_newer_than(self, self:path("src", "luajit")) and
    not tracked_build_input_newer_than(self, self:path("src", "libluajit.a"))
end

local function build_signature_covers_outputs(self, stamp)
  local luajit = self:path("src", "luajit")
  local libluajit = self:path("src", "libluajit.a")
  -- The stamp is written after Test:build completes. If a developer runs make
  -- outside the harness, the outputs become newer than the stamp and the cached
  -- XCFLAGS profile can no longer describe the archive being linked.
  return file_exists(stamp) and
    not command_succeeded("test " .. shell_quote(luajit) .. " -nt " ..
                          shell_quote(stamp)) and
    not command_succeeded("test " .. shell_quote(libluajit) .. " -nt " ..
                          shell_quote(stamp))
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

local function normalize_target_arch(arch)
  if arch == "x64" or arch == "x86_64" or arch == "amd64" then
    return "x64"
  elseif arch == "arm64" or arch == "aarch64" then
    return "arm64"
  end
  return nil
end

local function detect_compiler_target_arch(compiler)
  local override = getenv("LJ_TEST_TARGET_ARCH", nil)
  if override then
    local arch = normalize_target_arch(override:lower())
    if not arch then
      error("unsupported LJ_TEST_TARGET_ARCH: " .. override ..
            " (expected x64 or arm64)", 2)
    end
    return arch
  end

  -- Probe the compiler command, rather than the machine running the harness.
  -- This keeps `CC="clang --target=..."` and prefixed cross compilers honest.
  local cmd = compiler .. " -dM -E -x c /dev/null"
  local ok, defines = pcall(utils.capture_command, cmd, { stderr = true })
  if not ok then
    error("cannot detect the test compiler target; set " ..
          "LJ_TEST_TARGET_ARCH=x64 or arm64\n" .. tostring(defines), 2)
  end
  if defines:find("#define __x86_64__ 1", 1, true) or
     defines:find("#define _M_X64 ", 1, true) then
    return "x64"
  elseif defines:find("#define __aarch64__ 1", 1, true) or
         defines:find("#define _M_ARM64 ", 1, true) then
    return "arm64"
  end
  error("unsupported test compiler target; set " ..
        "LJ_TEST_TARGET_ARCH=x64 or arm64", 2)
end

local function target_arch_flags(arch)
  -- CMPXCHG16B must be enabled explicitly on the fork's x64 baseline. ARM64
  -- has native paired/exclusive 128-bit CAS and rejects the x86-only option.
  if arch == "x64" then return "-mcx16" end
  if arch == "arm64" then return "" end
  error("unsupported test target architecture: " .. tostring(arch), 2)
end

local function join_flags(flags, extra)
  if flags == nil or flags == "" then return extra or "" end
  if extra == nil or extra == "" then return flags end
  return flags .. " " .. extra
end

M.target_arch_flags = target_arch_flags

function M.new(root)
  local compiler = getenv("CC", "cc")
  local target_arch = detect_compiler_target_arch(compiler)
  local arch_flags = target_arch_flags(target_arch)
  local default_cflags = join_flags(
    "-std=gnu11 -O2 -Wall -Wextra -Werror", arch_flags)
  local self = {
    root = root,
    compiler = compiler,
    target_arch = target_arch,
    target_arch_flags = arch_flags,
    cflags = getenv("CFLAGS", default_cflags),
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

function Test:with_target_arch_flags(flags)
  return join_flags(flags, self.target_arch_flags)
end

function Test:tmp(name)
  return self.tmpdir .. "/" .. name
end

function Test:read(path)
  return read_file(path)
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
  local build_env = {}
  if opts.env then
    for k, v in pairs(opts.env) do build_env[k] = v end
  end
  if jit and jit.os == "OSX" and
     build_env.MACOSX_DEPLOYMENT_TARGET == nil then
    build_env.MACOSX_DEPLOYMENT_TARGET =
      getenv("MACOSX_DEPLOYMENT_TARGET", "13.0")
  end
  if next(build_env) == nil then build_env = nil end
  local stamp = build_signature_path(self)
  local signature = build_profile_signature(opts.xcflags, build_env)
  local disk_signature = self.build_signature
  local have_outputs = file_exists(self:path("src", "luajit")) and
    file_exists(self:path("src", "libluajit.a"))
  if disk_signature == nil then disk_signature = read_file_or_nil(stamp) end
  if opts.clean and disk_signature == signature and have_outputs and
     build_signature_covers_outputs(self, stamp) and
     build_outputs_current(self) and
     not getenv("LJ_TEST_DISABLE_BUILD_CACHE", nil) then
    self.build_signature = signature
    return
  end
  if opts.clean or disk_signature ~= signature then
    local clean_args = { "clean" }
    -- Target selection happens while parsing the Makefile, even for `clean`.
    -- Preserve the requested build profile so opt-in architectures do not hit
    -- the default target guard before their stale artifacts are removed.
    if opts.xcflags then
      clean_args[#clean_args + 1] = "XCFLAGS=" .. opts.xcflags
    end
    self:make(clean_args, {
      quiet = opts.quiet,
      jobs = false,
      env = build_env
    })
  end
  if opts.xcflags then args[#args + 1] = "XCFLAGS=" .. opts.xcflags end
  self:make(args, {
    quiet = opts.quiet,
    jobs = opts.jobs,
    env = build_env,
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
