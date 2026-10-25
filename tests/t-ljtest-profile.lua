local utils = require("suite_utils")
local ljtest = require("ljtest")

local root = os.tmpname() .. "-ljtest-profile"
os.remove(root)
local src = root .. "/src"
assert(os.execute("mkdir -p " .. utils.shell_quote(src)) == 0)
assert(os.execute("git -C " .. utils.shell_quote(root) .. " init -q") == 0)

local t = ljtest.new(root)
local builds = 0
local run_command, run_options, make_options

function t:make(_, options)
  builds = builds + 1
  make_options = options
  utils.write_file(src .. "/luajit", "runtime " .. builds)
  utils.write_file(src .. "/libluajit.a", "archive " .. builds)
end

function t:run(command, options)
  run_command, run_options = command, options
end

local common = {
  clean = true,
  target_sys = "Darwin",
  target_flags = "-arch arm64",
  xcflags = "-DPROFILE_ONE",
  env = {
    CC = "clang -pipe",
    CFLAGS = "-O0 -DUSER_C",
    CPPFLAGS = "-DUSER_CPP",
    LDFLAGS = "-Wl,-headerpad_max_install_names",
    SDKROOT = "/test/sdk",
    MACOSX_DEPLOYMENT_TARGET = "14.4"
  }
}

t:build(common)
local after_first_build = builds
t:build(common)
assert(builds == after_first_build, "identical stamped profile rebuilt")
assert(t:profile_id():find("XCFLAGS=-DPROFILE_ONE", 1, true))

t:cc("/tmp/ljtest-profile-fixture", { "/tmp/fixture.c" }, {
  link_luajit = true,
  libs = { "-lm" }
})
assert(run_command:find("clang %-pipe"))
assert(run_command:find("%-O0 %-DUSER_C"))
assert(run_command:find("%-DUSER_CPP"))
assert(run_command:find("%-DPROFILE_ONE"))
assert(run_command:find("%-arch arm64"))
assert(run_options.env.SDKROOT == "/test/sdk")
assert(run_options.env.MACOSX_DEPLOYMENT_TARGET == "14.4")
t:cc("/tmp/ljtest-profile-fixture", {"/tmp/fixture.cpp"}, {
  compiler = "clang++ -pipe", default_cflags = false, link_luajit = true,
  cflags = "-std=gnu++11"
})
assert(run_command:find("clang%+%+ %-pipe"))
assert(run_command:find("%-DPROFILE_ONE"))
assert(run_command:find("%-arch arm64"))

local archive = utils.read_file(src .. "/libluajit.a")
local runtime = utils.read_file(src .. "/luajit")
utils.write_file(src .. "/libluajit.a", archive .. " tampered")
assert(not pcall(function()
  t:cc("/tmp/ljtest-profile-fixture", { "/tmp/fixture.c" }, {
    link_luajit = true
  })
end), "archive replacement was accepted")
utils.write_file(src .. "/libluajit.a", archive)
utils.write_file(src .. "/luajit", runtime .. " tampered")
assert(not pcall(function()
  t:cc("/tmp/ljtest-profile-fixture", { "/tmp/fixture.c" }, {
    link_luajit = true
  })
end), "runtime replacement was accepted")
utils.write_file(src .. "/luajit", runtime)

local function rebuild_with(opts, field, value)
  local old = builds
  opts[field] = value
  t:build(opts)
  assert(builds > old, field .. " change reused prior profile")
end

local changed = {}
for k, v in pairs(common) do changed[k] = v end
changed.env = common.env
rebuild_with(changed, "xcflags", "-DPROFILE_TWO")
changed = {}
for k, v in pairs(common) do changed[k] = v end
changed.target_flags = "-arch arm64e -mbranch-protection=bti"
rebuild_with(changed, "target_flags", changed.target_flags)
changed = {}
for k, v in pairs(common) do changed[k] = v end
changed.env = {}
for k, v in pairs(common.env) do changed.env[k] = v end
changed.env.CC = "clang -pipe -fno-common"
rebuild_with(changed, "env", changed.env)

-- A fresh fixture loader must recover the stamped macro/compiler profile;
-- an older instance must not reuse flags after another valid build replaces it.
local fresh = ljtest.new(root)
function fresh:run(command, options)
  run_command, run_options = command, options
end
fresh:cc("/tmp/ljtest-profile-fixture", { "/tmp/fixture.c" }, { link_luajit = true })
assert(run_command:find("clang %-pipe %-fno%-common"))
assert(run_command:find("%-DUSER_C"))
assert(run_command:find("%-DPROFILE_ONE"))
local stale = ljtest.new(root)
function stale:run() end
stale:cc("/tmp/ljtest-profile-fixture", { "/tmp/fixture.c" }, { link_luajit = true })
t:build(common)
assert(not pcall(function()
  stale:cc("/tmp/ljtest-profile-fixture", { "/tmp/fixture.c" }, { link_luajit = true })
end), "valid replacement profile reused stale fixture flags")
assert(not pcall(function() stale:luajit({"-e", "return"}) end),
       "valid replacement runtime profile was accepted")

t.target_sys = ""
t:build({clean = true})
assert(make_options.env.TARGET_SYS == nil,
       "empty TARGET_SYS disabled Make host detection")

-- Externally built archives use the same stamp format as Test:build.
t:record_build_profile(common)
local external = ljtest.new(root)
function external:run(command) run_command = command end
external:cc("/tmp/ljtest-profile-fixture", {"/tmp/fixture.c"}, {link_luajit = true})
assert(run_command:find("%-DPROFILE_ONE"))

-- Target-only sanitizer flags must invalidate the profile and reach the
-- linked fixture as well, including when loaded by another harness instance.
local sanitizer = {clean = true, env = {TARGET_CFLAGS = "-fsanitize=address",
  TARGET_LDFLAGS = "-fsanitize=address", TARGET_SHLDFLAGS = "-fsanitize=address"}}
local old = builds
t:build(sanitizer)
assert(builds > old)
assert(t:profile_id():find("TARGET_CFLAGS=-fsanitize=address", 1, true))
assert(make_options.env.TARGET_CFLAGS == "-fsanitize=address")
local sanitized = ljtest.new(root)
function sanitized:run(command) run_command = command end
sanitized:cc("/tmp/ljtest-profile-fixture", {"/tmp/fixture.c"}, {link_luajit = true})
local _, count = run_command:gsub("%-fsanitize=address", "")
assert(count == 2, "sanitizer compile/link flags were not inherited")
old = builds
t:build({clean = false})
assert(builds > old, "normal build reused sanitizer outputs")

os.execute("rm -rf " .. utils.shell_quote(root))
print("t-ljtest-profile OK: profile flags, output hashes and mismatch rebuilds")
