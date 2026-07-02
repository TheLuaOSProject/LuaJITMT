local runtime = require("suite_runtime")
local build = require("suite_build")
local utils = require("suite_utils")

local shell_quote = utils.shell_quote

local function env_or_empty(name)
  return os.getenv(name) or ""
end

local function command_exists(name)
  return utils.command_ok("command -v " .. shell_quote(name))
end

local function platform_required(name)
  local req = env_or_empty("LJ_M0_PLATFORM_REQUIRE"):lower()
  return req == "1" or req == "all" or
         req:find(name:lower(), 1, true) ~= nil
end

local function platform_enabled()
  local enabled = env_or_empty("LJ_M0_PLATFORM_ENABLE"):lower()
  local req = env_or_empty("LJ_M0_PLATFORM_REQUIRE"):lower()
  return enabled == "1" or enabled == "true" or enabled == "yes" or
         (req ~= "" and req ~= "0" and req ~= "false" and req ~= "no")
end

local function maybe_skip_platform(name, missing)
  if platform_required(name) then
    error("M0 " .. name .. " platform smoke missing: " .. missing, 2)
  end
  print("M0 " .. name .. " platform smoke skipped: " .. missing)
  return true
end

local function smoke_code()
  return table.concat({
    "print(jit.os, jit.arch)",
    "local threading = require('threading')",
    "assert(type(threading.spawn) == 'function')",
    "assert(type(threading.gcstats) == 'function')",
    "if jit.os == 'Windows' then local ffi = require('ffi'); ffi.cdef('unsigned long GetCurrentProcessId(void);'); local k = ffi.load('kernel32'); assert(k.GetCurrentProcessId() ~= 0) end"
  }, "; ")
end

local function assert_platform_output(label, out, osname)
  if not out:find(osname .. "%s+x64") then
    error(label .. " smoke did not report " .. osname .. " x64:\n" .. out, 2)
  end
end

local function run_windows_smoke(t)
  local cross = utils.getenv("LJ_M0_WINDOWS_CROSS",
                             "x86_64-w64-mingw32ucrt-")
  local cc = utils.getenv("LJ_M0_WINDOWS_CC", "gcc")
  local runner = utils.getenv("LJ_M0_WINDOWS_RUNNER", "wine")
  local compiler = cross .. cc

  if not command_exists(compiler) then
    return maybe_skip_platform("Windows UCRT", compiler .. " not in PATH")
  end
  if not command_exists(runner) then
    return maybe_skip_platform("Windows UCRT", runner .. " not in PATH")
  end

  t:make({ "clean" }, { quiet = true, jobs = false })
  t:make({
    "HOST_CC=gcc",
    "CROSS=" .. cross,
    "CC=" .. cc,
    "TARGET_SYS=Windows"
  }, { quiet = true })

  local out = utils.capture_command(
    "WINEDEBUG=-all " .. runner .. " " ..
      shell_quote(t:path("src", "luajit.exe")) ..
      " -e " .. shell_quote(smoke_code()),
    { timeout = "120s", stderr = true })
  assert_platform_output("Windows UCRT", out, "Windows")
  print("M0 Windows UCRT platform smoke passed")
end

local function darwin_host_path(path)
  return "/Volumes/SystemRoot" .. path
end

local function run_darwin_smoke(t)
  local cross = utils.getenv("LJ_M0_DARWIN_CROSS",
                            "x86_64-apple-darwin23.2-")
  local cc = utils.getenv("LJ_M0_DARWIN_CC", "cc")
  local deploy = utils.getenv("MACOSX_DEPLOYMENT_TARGET", "13.0")
  local target_flags = utils.getenv("LJ_M0_DARWIN_TARGET_FLAGS",
                                    "-arch x86_64")
  local osxcross_dir = utils.getenv("OSXCROSS_DIR",
                                    t:path(".devcontainer", "osxcross"))
  local osxcross_bin = utils.getenv("LJ_M0_OSXCROSS_BIN",
                                    osxcross_dir .. "/target/bin")
  local compiler = cross .. cc
  local have_compiler = command_exists(compiler) or
                        utils.file_exists(osxcross_bin .. "/" .. compiler)

  if not have_compiler then
    return maybe_skip_platform("Darwin", compiler .. " not in PATH")
  end
  if not command_exists("darling") then
    return maybe_skip_platform("Darwin", "darling not in PATH")
  end

  t:make({ "clean" }, {
    quiet = true,
    jobs = false,
    env = { PATH = osxcross_bin .. ":" .. env_or_empty("PATH") }
  })
  t:make({
    "HOST_CC=gcc",
    "CROSS=" .. cross,
    "CC=" .. cc,
    "TARGET_FLAGS=" .. target_flags,
    "TARGET_SYS=Darwin"
  }, {
    quiet = true,
    env = {
      PATH = osxcross_bin .. ":" .. env_or_empty("PATH"),
      MACOSX_DEPLOYMENT_TARGET = deploy
    }
  })

  local inner = "cd " .. shell_quote(darwin_host_path(t:path("src"))) ..
    " && ./luajit -e " .. shell_quote(smoke_code())
  local out = utils.capture_command(
    "darling shell /bin/bash -lc " .. shell_quote(inner),
    { timeout = "120s", stderr = true })
  assert_platform_output("Darwin", out, "OSX")
  print("M0 macOS x86_64 target " .. deploy .. " platform smoke passed")
end

local function run_platform_smoke(t)
  if not platform_enabled() then
    print("M0 platform cross smoke skipped: set LJ_M0_PLATFORM_ENABLE=1 to run")
    return
  end

  local ok, err = xpcall(function()
    run_windows_smoke(t)
    run_darwin_smoke(t)
  end, debug.traceback)

  t:build({ clean = true, quiet = true })
  if not ok then error(err, 0) end
  print("M0 platform cross smoke passed")
end

local function run_m0_combo(t, name, xcflags, stock_tags)
  print("== " .. name .. " ==")
  t:build({ clean = true, xcflags = xcflags })

  local stock_args = { "test.lua", "--quiet" }
  for i = 1, #stock_tags do stock_args[#stock_args + 1] = stock_tags[i] end
  runtime.run_stock(t, stock_args, {
    bin = t:path("src", "luajit"),
    check_executable = true
  })

  runtime.luajit_code(t, "require'ffi'; assert(2^31 == 2147483648)")
end

return function(add)
  add({
    name = "run_stock_tests",
    description = "vendored LuaJIT stock cleanup suite",
    run = function(t, args)
      runtime.run_stock_cli(t, args)
    end
  })

  add({
    name = "m0_build_profile_switch",
    description = "Lua test harness rebuilds when the XCFLAGS profile changes",
    run = function(t)
      build.build_default(t, {
        quiet = true,
        jobs = false,
        args = { "XCFLAGS=-DLUAJIT_DISABLE_JIT" }
      })
      t.build_signature = nil
      build.build_default(t, { quiet = true, jobs = false })
      runtime.luajit_code(t, "assert(jit.status())", { quiet = true })
      print("M0 build profile switch passed")
    end
  })

  add({
    name = "m0_matrix",
    description = "M0 default and no-JIT stock-test build matrix",
    run = function(t)
      run_m0_combo(t, "lockless JIT=1", "", {})
      run_m0_combo(t, "lockless JIT=0", "-DLUAJIT_DISABLE_JIT", { "-jit" })
      print("M0 matrix passed")
    end
  })

  add({
    name = "m0_lua52_compat",
    description = "optional stock Lua 5.2 compatibility build profile",
    run = function(t)
      run_m0_combo(t, "lockless Lua 5.2 compat",
                   "-DLUAJIT_ENABLE_LUA52COMPAT", {})
      runtime.luajit_code(t, [[
        assert(table.pack and table.unpack == unpack)
        local packed = table.pack("x", nil)
        assert(packed.n == 2 and packed[1] == "x" and packed[2] == nil)
        assert(rawlen({ 1, 2, 3 }) == 3)
        local _, ismain = coroutine.running()
        assert(type(ismain) == "boolean")
        local marker = {}
        local iter = pairs(setmetatable({}, {
          __pairs = function()
            local done
            return function()
              if done then return nil end
              done = true
              return marker, 42
            end
          end
        }))
        local k, v = iter()
        assert(k == marker and v == 42)
      ]])
      print("M0 Lua 5.2 compatibility build passed")
    end
  })

  add({
    name = "m0_platform_cross_smoke",
    description = "optional Windows UCRT/macOS x86_64 cross-build and runtime smoke",
    run = run_platform_smoke
  })
end
