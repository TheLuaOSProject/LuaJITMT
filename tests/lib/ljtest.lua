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
local profile_make_vars = {
  "CCDEBUG", "TARGET_CFLAGS", "TARGET_LDFLAGS", "TARGET_SHLDFLAGS",
  "HOST_CC", "HOST_CFLAGS", "HOST_LDFLAGS", "CROSS", "BUILDMODE"
}

local function build_profile_signature(self, opts, env)
  local deploy = env and env.MACOSX_DEPLOYMENT_TARGET or ""
  local target_flags = (env and env.TARGET_FLAGS) or
                       opts.target_flags or self.target_flags or ""
  local target_sys = (env and env.TARGET_SYS) or
                     opts.target_sys or self.target_sys or ""
  local compiler = (env and env.CC) or self.compiler
  local cflags = (env and env.CFLAGS) or ""
  local cppflags = (env and env.CPPFLAGS) or self.cppflags
  local ldflags = (env and env.LDFLAGS) or self.ldflags
  local xcflags = (env and env.XCFLAGS) or opts.xcflags or ""
  local signature = "profile-v3\nCC=" .. compiler ..
         "\nTARGET_ARCH=" .. (self.build_profile_target_arch or self.target_arch) ..
         "\nTARGET_SYS=" .. target_sys ..
         "\nTARGET_FLAGS=" .. target_flags ..
         "\nCFLAGS=" .. cflags ..
         "\nTEST_CFLAGS=" .. (self.build_profile_cflags or self.cflags) ..
         "\nCPPFLAGS=" .. cppflags ..
         "\nLDFLAGS=" .. ldflags ..
         "\nXCFLAGS=" .. xcflags ..
         "\nSDKROOT=" .. ((env and env.SDKROOT) or self.sdkroot or "") ..
         "\nMACOSX_DEPLOYMENT_TARGET=" .. deploy .. "\n"
  for _, key in ipairs(profile_make_vars) do
    signature = signature .. key .. "=" .. ((env and env[key]) or "") .. "\n"
  end
  return signature
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

local file_sha256

local function build_signature_covers_outputs(self, stamp)
  local luajit = self:path("src", "luajit")
  local libluajit = self:path("src", "libluajit.a")
  if not file_exists(stamp) then return false end
  local contents = read_file_or_nil(stamp)
  if not contents then return false end
  local archive_sha = contents:match("\nOUTPUTS\nARCHIVE=([%da-fA-F]+)")
  local runtime_sha = contents:match("\nRUNTIME=([%da-fA-F]+)")
  if not archive_sha or not runtime_sha then return false end
  -- Hashes catch replacements whose timestamps were preserved or reset.
  return file_sha256(libluajit) == archive_sha:lower() and
         file_sha256(luajit) == runtime_sha:lower()
end

file_sha256 = function(path)
  local cmd = "(shasum -a 256 " .. shell_quote(path) ..
    " 2>/dev/null || sha256sum " .. shell_quote(path) .. ")"
  local ok, output = pcall(utils.capture_command, cmd)
  if not ok then return nil end
  local digest = output:match("^([%da-fA-F]+)")
  if not digest or #digest ~= 64 then return nil end
  return digest:lower()
end

local function output_stamp(self)
  local archive = file_sha256(self:path("src", "libluajit.a"))
  local runtime = file_sha256(self:path("src", "luajit"))
  assert(archive and runtime, "cannot hash LuaJIT build outputs")
  return "\nOUTPUTS\nARCHIVE=" .. archive .. "\nRUNTIME=" .. runtime .. "\n"
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
  local target_flags = getenv("TARGET_FLAGS", "")
  local target_arch = detect_compiler_target_arch(getenv("CROSS", "") ..
    compiler .. " " .. target_flags .. " " .. getenv("TARGET_CFLAGS", ""))
  local arch_flags = target_arch_flags(target_arch)
  local default_cflags = join_flags(
    "-std=gnu11 -O2 -Wall -Wextra -Werror", arch_flags)
  local self = {
    root = root,
    compiler = compiler,
    target_arch = target_arch,
    target_arch_flags = arch_flags,
    target_sys = getenv("TARGET_SYS", ""),
    target_flags = target_flags,
    cflags = getenv("CFLAGS", default_cflags),
    default_cflags = getenv("CFLAGS", nil) == nil,
    cppflags = getenv("CPPFLAGS", ""),
    ldflags = getenv("LDFLAGS", ""),
    sdkroot = getenv("SDKROOT", ""),
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

local function configure_build_profile(self, opts)
  local build_env = {}
  if opts.env then
    for k, v in pairs(opts.env) do build_env[k] = v end
  end
  for _, key in ipairs(profile_make_vars) do
    if build_env[key] == nil then build_env[key] = getenv(key, nil) end
  end
  if jit and jit.os == "OSX" and
     build_env.MACOSX_DEPLOYMENT_TARGET == nil then
    build_env.MACOSX_DEPLOYMENT_TARGET =
      getenv("MACOSX_DEPLOYMENT_TARGET", "13.0")
  end
  -- Make and linked fixtures receive the same effective profile explicitly;
  -- ambient shell defaults are not part of the build identity.
  build_env.CC = build_env.CC or self.compiler
  -- Leave Make's own CFLAGS default intact unless the caller explicitly
  -- overrides it. self.cflags is the fixture compiler profile, not Make's
  -- library-build default.
  build_env.CFLAGS = build_env.CFLAGS or getenv("CFLAGS", nil)
  build_env.CPPFLAGS = build_env.CPPFLAGS or self.cppflags
  build_env.LDFLAGS = build_env.LDFLAGS or self.ldflags
  build_env.XCFLAGS = opts.xcflags or build_env.XCFLAGS or getenv("XCFLAGS", "")
  build_env.TARGET_SYS = opts.target_sys or build_env.TARGET_SYS or self.target_sys
  build_env.TARGET_FLAGS = opts.target_flags or
                           build_env.TARGET_FLAGS or self.target_flags
  -- An empty TARGET_SYS in Make's environment disables its host-OS
  -- autodetection (and therefore Darwin's GC64 configuration).
  if build_env.TARGET_SYS == "" then build_env.TARGET_SYS = nil end
  build_env.SDKROOT = build_env.SDKROOT or self.sdkroot
  if build_env.SDKROOT == "" then build_env.SDKROOT = nil end
  self.build_profile_target_arch = detect_compiler_target_arch(
    (build_env.CROSS or "") .. build_env.CC .. " " ..
    (build_env.TARGET_FLAGS or "") .. " " .. (build_env.TARGET_CFLAGS or ""))
  self.build_profile_compiler = build_env and build_env.CC or self.compiler
  self.build_profile_cflags = build_env.CFLAGS or
    (self.default_cflags and join_flags("-std=gnu11 -O2 -Wall -Wextra -Werror",
      target_arch_flags(self.build_profile_target_arch)) or self.cflags)
  self.build_profile_target_flags = (build_env and build_env.TARGET_FLAGS) or
                                    opts.target_flags or self.target_flags or ""
  self.build_profile_target_sys = build_env.TARGET_SYS or ""
  self.build_profile_cppflags = (build_env and build_env.CPPFLAGS) or
                                self.cppflags
  self.build_profile_ldflags = (build_env and build_env.LDFLAGS) or
                               self.ldflags
  self.build_profile_xcflags = build_env.XCFLAGS or ""
  self.build_profile_sdkroot = (build_env and build_env.SDKROOT) or self.sdkroot
  self.build_profile_deployment_target = build_env and
    build_env.MACOSX_DEPLOYMENT_TARGET or ""
  local signature = build_profile_signature(self, opts, build_env)
  self.build_profile_id = signature
  self.build_profile_target_cflags = build_env.TARGET_CFLAGS or ""
  self.build_profile_target_ldflags = build_env.TARGET_LDFLAGS or ""
  self.build_profile_ccdebug = build_env.CCDEBUG or ""
  self.build_profile_cross = build_env.CROSS or ""
  return build_env, signature
end

-- Only the builder may call this, immediately after a successful external
-- build with these exact settings. Hashes bind later consumers to those
-- outputs; they cannot recover compiler flags from an arbitrary old archive.
function Test:record_build_profile(opts)
  local _, signature = configure_build_profile(self, opts or {})
  assert(build_outputs_current(self), "cannot stamp stale LuaJIT outputs")
  write_file(build_signature_path(self), signature .. output_stamp(self))
  self.build_signature = signature
end

function Test:build(opts)
  opts = opts or {}
  local args = {}
  local build_env, signature = configure_build_profile(self, opts)
  local stamp = build_signature_path(self)
  local disk_signature = self.build_signature
  local have_outputs = file_exists(self:path("src", "luajit")) and
    file_exists(self:path("src", "libluajit.a"))
  if disk_signature == nil then
    disk_signature = read_file_or_nil(stamp)
    if disk_signature then
      disk_signature = disk_signature:match("^(.-)\nOUTPUTS\n")
    end
  end
  if opts.clean and disk_signature == signature and have_outputs and
     build_signature_covers_outputs(self, stamp) and
     build_outputs_current(self) and
     not getenv("LJ_TEST_DISABLE_BUILD_CACHE", nil) then
    self.build_signature = signature
    return
  end
  if opts.clean or (have_outputs and
       (disk_signature ~= signature or
        not build_signature_covers_outputs(self, stamp))) then
    local clean_args = { "clean" }
    -- Target selection happens while parsing the Makefile, even for `clean`.
    -- Preserve the requested build profile so opt-in architectures do not hit
    -- the default target guard before their stale artifacts are removed.
    clean_args[#clean_args + 1] = "XCFLAGS=" .. self.build_profile_xcflags
    if opts.target_flags ~= nil then
      clean_args[#clean_args + 1] = "TARGET_FLAGS=" .. opts.target_flags
    end
    if opts.target_sys ~= nil then
      clean_args[#clean_args + 1] = "TARGET_SYS=" .. opts.target_sys
    elseif self.build_profile_target_sys ~= "" then
      clean_args[#clean_args + 1] =
        "TARGET_SYS=" .. self.build_profile_target_sys
    end
    clean_args[#clean_args + 1] = "CC=" .. self.build_profile_compiler
    if build_env.CFLAGS then
      clean_args[#clean_args + 1] = "CFLAGS=" .. build_env.CFLAGS
    end
    clean_args[#clean_args + 1] = "CPPFLAGS=" .. self.build_profile_cppflags
    clean_args[#clean_args + 1] = "LDFLAGS=" .. self.build_profile_ldflags
    for _, key in ipairs(profile_make_vars) do
      if build_env[key] then clean_args[#clean_args + 1] = key .. "=" .. build_env[key] end
    end
    self:make(clean_args, {
      quiet = opts.quiet,
      jobs = false,
      env = build_env
    })
  end
  args[#args + 1] = "XCFLAGS=" .. self.build_profile_xcflags
  if opts.target_flags ~= nil then
    args[#args + 1] = "TARGET_FLAGS=" .. opts.target_flags
  end
  if opts.target_sys ~= nil then
    args[#args + 1] = "TARGET_SYS=" .. opts.target_sys
  elseif self.build_profile_target_sys ~= "" then
    args[#args + 1] = "TARGET_SYS=" .. self.build_profile_target_sys
  end
  args[#args + 1] = "CC=" .. self.build_profile_compiler
  if build_env.CFLAGS then args[#args + 1] = "CFLAGS=" .. build_env.CFLAGS end
  args[#args + 1] = "CPPFLAGS=" .. self.build_profile_cppflags
  args[#args + 1] = "LDFLAGS=" .. self.build_profile_ldflags
  for _, key in ipairs(profile_make_vars) do
    if build_env[key] then args[#args + 1] = key .. "=" .. build_env[key] end
  end
  self:make(args, {
    quiet = opts.quiet,
    jobs = opts.jobs,
    env = build_env,
    keep_build_signature = true
  })
  write_file(stamp, signature .. output_stamp(self))
  self.build_signature = signature
  self.build_profile_id = signature
end

function Test:profile_id()
  if self.build_profile_id == nil then
    local contents = read_file_or_nil(build_signature_path(self))
    self.build_profile_id = contents and
      contents:match("^(.-)\nOUTPUTS\n") or contents
  end
  return self.build_profile_id
end

local function load_build_profile(self)
  local stamp = build_signature_path(self)
  if not build_signature_covers_outputs(self, stamp) then return false end
  local contents = read_file_or_nil(stamp)
  local profile = contents and contents:match("^(.-)\nOUTPUTS\n")
  if not profile or (self.build_profile_id and
                     self.build_profile_id ~= profile) then return false end
  local values = {}
  for key, value in profile:gmatch("\n([A-Z_]+)=([^\n]*)") do
    values[key] = value
  end
  if not values.CC or not values.CFLAGS or not values.TARGET_ARCH then
    return false
  end
  self.build_profile_compiler = values.CC
  self.build_profile_target_arch = values.TARGET_ARCH
  self.build_profile_id = profile
  self.build_profile_cflags = values.CFLAGS ~= "" and values.CFLAGS or
                              values.TEST_CFLAGS or self.cflags
  self.build_profile_cppflags = values.CPPFLAGS or ""
  self.build_profile_ldflags = values.LDFLAGS or ""
  self.build_profile_target_cflags = values.TARGET_CFLAGS or ""
  self.build_profile_target_ldflags = values.TARGET_LDFLAGS or ""
  self.build_profile_ccdebug = values.CCDEBUG or ""
  self.build_profile_cross = values.CROSS or ""
  self.build_profile_target_flags = values.TARGET_FLAGS or ""
  self.build_profile_xcflags = values.XCFLAGS or ""
  self.build_profile_target_sys = values.TARGET_SYS or ""
  self.build_profile_sdkroot = values.SDKROOT or ""
  self.build_profile_deployment_target =
    values.MACOSX_DEPLOYMENT_TARGET or ""
  return true
end

function Test:assert_target_arch(expected)
  local normalized = normalize_target_arch(expected)
  local actual = self.build_profile_target_arch or self.target_arch
  if normalized ~= actual then
    error("test compiler target mismatch: expected " .. tostring(expected) ..
          ", got " .. tostring(actual), 2)
  end
  return actual
end

function Test:assert_runtime_arch()
  local expected = self.build_profile_target_arch or self.target_arch
  self:luajit({"-e", "assert(jit.arch == " .. string.format("%q", expected) ..
              ", 'runtime/test compiler architecture mismatch')"},
              { quiet = true })
  return expected
end

function Test:cc(output, sources, opts)
  opts = opts or {}
  if opts.link_luajit then
    if not load_build_profile(self) or not build_outputs_current(self) then
      error("refusing to link a stale LuaJIT build profile", 2)
    end
  end
  local compiler = opts.compiler or
    ((self.build_profile_cross or "") .. (self.build_profile_compiler or self.compiler))
  local parts = { compiler }
  if opts.default_cflags ~= false then
    append(parts, self.build_profile_cflags or self.cflags)
  end
  append(parts, self.build_profile_target_flags or self.target_flags)
  append(parts, self.build_profile_cppflags or self.cppflags)
  append(parts, self.build_profile_xcflags or "")
  append(parts, self.build_profile_ccdebug or "")
  append(parts, self.build_profile_target_cflags or "")
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
  append(parts, self.build_profile_ldflags or self.ldflags)
  append(parts, self.build_profile_target_ldflags or "")
  append(parts, "-o " .. shell_quote(output))
  local runopts = { quiet = opts.quiet, env = {} }
  for k, v in pairs(opts.env or {}) do runopts.env[k] = v end
  if self.build_profile_sdkroot and self.build_profile_sdkroot ~= "" then
    runopts.env.SDKROOT = self.build_profile_sdkroot
  end
  if self.build_profile_deployment_target and
     self.build_profile_deployment_target ~= "" then
    runopts.env.MACOSX_DEPLOYMENT_TARGET =
      self.build_profile_deployment_target
  end
  if next(runopts.env) == nil then runopts.env = nil end
  self:run(table.concat(parts, " "), runopts)
end

function Test:luajit(args, opts)
  opts = opts or {}
  if self.build_profile_id and not load_build_profile(self) then
    error("refusing to execute a changed LuaJIT build profile", 2)
  end
  local argv = { self:path("src", "luajit") }
  for i = 1, #args do argv[#argv + 1] = args[i] end
  self:run(argv, opts)
end

return M
