local runtime = require("suite_runtime")
local utils = require("suite_utils")

local shell_quote = utils.shell_quote

local function env(name)
  local v = os.getenv(name)
  if v == nil or v == "" then return nil end
  return v
end

local function release_required(name)
  local req = (env("LJ_RELEASE_REQUIRE") or ""):lower()
  name = name:lower()
  return req == "1" or req == "all" or req:find(name, 1, true) ~= nil
end

local function maybe_skip(name, why)
  if release_required(name) then
    error("release " .. name .. " binary test missing: " .. why, 2)
  end
  print("release " .. name .. " binary test skipped: " .. why)
  return false
end

local function command_exists(cmd)
  local exe = cmd:match("^%s*([^%s]+)")
  return exe and utils.command_ok("command -v " .. shell_quote(exe))
end

local function smoke_code()
  return table.concat({
    "print(jit.os, jit.arch)",
    "local threading = require('threading')",
    "assert(type(threading.spawn) == 'function')",
    "assert(type(threading.gcstats) == 'function')"
  }, "; ")
end

local function assert_platform_output(label, out, osname)
  if not out:find(osname .. "%s+x64") then
    error(label .. " smoke did not report " .. osname .. " x64:\n" .. out, 2)
  end
end

local function assert_stock_output(label, out)
  if not out:find("%d+%s+passed") then
    error(label .. " stock suite did not report a pass count:\n" .. out, 2)
  end
end

local function stock_dir(t)
  return t:path("tests", "stock", "test")
end

local function run_direct_binary(t, name, bin, osname)
  if not bin then return maybe_skip(name, "LJ_RELEASE_" .. name:upper() .. "_BIN unset") end
  if not utils.file_exists(bin) then return maybe_skip(name, bin .. " not found") end
  local out = utils.capture_command(
    shell_quote(bin) .. " -e " .. shell_quote(smoke_code()),
    { timeout = "120s", stderr = true })
  assert_platform_output(name, out, osname)
  runtime.run_stock(t, { "test.lua", "--quiet" }, {
    bin = bin,
    check_executable = true,
    timeout = "240s"
  })
  print("release " .. name .. " binary smoke and stock suite passed")
  return true
end

local function run_linux_binary(t)
  return run_direct_binary(t, "linux", env("LJ_RELEASE_LINUX_BIN"), "Linux")
end

local function run_windows_binary(t)
  local bin = env("LJ_RELEASE_WINDOWS_BIN")
  local runner = env("LJ_RELEASE_WINDOWS_RUNNER") or "wine"
  if not bin then return maybe_skip("windows", "LJ_RELEASE_WINDOWS_BIN unset") end
  if not utils.file_exists(bin) then return maybe_skip("windows", bin .. " not found") end
  if not command_exists(runner) then return maybe_skip("windows", runner .. " not in PATH") end

  local prefix = "WINEDEBUG=-all " .. runner .. " " .. shell_quote(bin)
  local out = utils.capture_command(
    prefix .. " -e " .. shell_quote(smoke_code()),
    { timeout = "120s", stderr = true })
  assert_platform_output("windows", out, "Windows")

  out = utils.capture_command(
    "cd " .. shell_quote(stock_dir(t)) .. " && " ..
      prefix .. " test.lua --quiet",
    { timeout = "240s", stderr = true })
  assert_stock_output("windows", out)
  print("release windows binary smoke and stock suite passed")
  return true
end

local function darling_path(path)
  return (env("LJ_RELEASE_DARLING_ROOT") or "/Volumes/SystemRoot") .. path
end

local function run_macos_darling(t, bin, runner)
  if not command_exists(runner) then return maybe_skip("macos", runner .. " not in PATH") end
  local dbin = darling_path(bin)
  local dstock = darling_path(stock_dir(t))
  local out = utils.capture_command(
    runner .. " shell /bin/bash -lc " ..
      shell_quote(dbin .. " -e " .. shell_quote(smoke_code())),
    { timeout = "120s", stderr = true })
  assert_platform_output("macos", out, "OSX")
  out = utils.capture_command(
    runner .. " shell /bin/bash -lc " ..
      shell_quote("cd " .. shell_quote(dstock) .. " && " ..
                  shell_quote(dbin) .. " test.lua --quiet"),
    { timeout = "240s", stderr = true })
  assert_stock_output("macos", out)
  print("release macos binary smoke and stock suite passed under Darling")
  return true
end

local function run_macos_binary(t)
  local bin = env("LJ_RELEASE_MACOS_BIN")
  local runner = env("LJ_RELEASE_MACOS_RUNNER")
  if not bin then return maybe_skip("macos", "LJ_RELEASE_MACOS_BIN unset") end
  if not utils.file_exists(bin) then return maybe_skip("macos", bin .. " not found") end
  if runner and runner ~= "" then
    return run_macos_darling(t, bin, runner)
  end
  return run_direct_binary(t, "macos", bin, "OSX")
end

local function run_release_platform_binaries(t)
  local ran = 0
  if run_linux_binary(t) then ran = ran + 1 end
  if run_windows_binary(t) then ran = ran + 1 end
  if run_macos_binary(t) then ran = ran + 1 end
  if ran == 0 and release_required("platform") then
    error("no release platform binaries were provided", 2)
  end
  print("release platform binary checks completed: " .. ran .. " platform(s)")
end

return function(add)
  add({
    name = "release_linux_binary",
    description = "release-only Linux x64 binary smoke and stock-suite check",
    run = run_linux_binary
  })

  add({
    name = "release_windows_binary",
    description = "release-only Windows x64 binary smoke and stock-suite check",
    run = run_windows_binary
  })

  add({
    name = "release_macos_binary",
    description = "release-only macOS x64 binary smoke and stock-suite check",
    run = run_macos_binary
  })

  add({
    name = "release_platform_binaries",
    description = "release-only Linux/Windows/macOS binary checks",
    run = run_release_platform_binaries
  })
end
