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
  if req == "platform" or req == "platforms" then
    return name == "platform" or name == "linux" or
      name == "macos" or name == "windows"
  end
  return req ~= "" and (req == "1" or req == "all" or
    req:find(name, 1, true) ~= nil or name:find(req, 1, true) ~= nil
  )
end

local function maybe_skip(name, why)
  if release_required(name) then
    error("release " .. name .. " check missing: " .. why, 2)
  end
  print("release " .. name .. " check skipped: " .. why)
  return false
end

local function run_stock_suite()
  local v = (env("LJ_RELEASE_RUN_STOCK") or ""):lower()
  return v == "1" or v == "true" or v == "yes"
end

local function release_prefix()
  local p = env("LJ_RELEASE_PREFIX") or "/usr/local"
  p = p:gsub("/+$", "")
  if p == "" then return "" end
  if p:sub(1, 1) ~= "/" then p = "/" .. p end
  return p
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
    "assert(type(threading.gcstats) == 'function')",
    "if jit.os == 'Windows' then local ffi = require('ffi'); ffi.cdef('unsigned long GetCurrentProcessId(void);'); local k = ffi.load('kernel32'); assert(k.GetCurrentProcessId() ~= 0) end"
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
  if run_stock_suite() then
    runtime.run_stock(t, { "test.lua", "--quiet" }, {
      bin = bin,
      check_executable = true,
      timeout = "240s"
    })
    print("release " .. name .. " binary smoke and stock suite passed")
  else
    print("release " .. name .. " binary smoke passed")
  end
  return true
end

local function run_linux_binary(t)
  return run_direct_binary(t, "linux", env("LJ_RELEASE_LINUX_BIN"), "Linux")
end

local function run_windows_binary_at(t, bin)
  local runner = env("LJ_RELEASE_WINDOWS_RUNNER") or "wine"
  if not bin then return maybe_skip("windows", "LJ_RELEASE_WINDOWS_BIN unset") end
  if not utils.file_exists(bin) then return maybe_skip("windows", bin .. " not found") end
  if not command_exists(runner) then return maybe_skip("windows", runner .. " not in PATH") end

  local prefix = "WINEDEBUG=-all " .. runner .. " " .. shell_quote(bin)
  local out = utils.capture_command(
    prefix .. " -e " .. shell_quote(smoke_code()),
    { timeout = "120s", stderr = true })
  assert_platform_output("windows", out, "Windows")

  if run_stock_suite() then
    out = utils.capture_command(
      "cd " .. shell_quote(stock_dir(t)) .. " && " ..
        prefix .. " test.lua --quiet",
      { timeout = "240s", stderr = true })
    assert_stock_output("windows", out)
    print("release windows binary smoke and stock suite passed")
  else
    print("release windows binary smoke passed")
  end
  return true
end

local function run_windows_binary(t)
  return run_windows_binary_at(t, env("LJ_RELEASE_WINDOWS_BIN"))
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
  if run_stock_suite() then
    out = utils.capture_command(
      runner .. " shell /bin/bash -lc " ..
        shell_quote("cd " .. shell_quote(dstock) .. " && " ..
                    shell_quote(dbin) .. " test.lua --quiet"),
      { timeout = "240s", stderr = true })
    assert_stock_output("macos", out)
    print("release macos binary smoke and stock suite passed under Darling")
  else
    print("release macos binary smoke passed under Darling")
  end
  return true
end

local run_macos_binary_at

local function run_macos_binary(t)
  local bin = env("LJ_RELEASE_MACOS_BIN")
  if not bin then return maybe_skip("macos", "LJ_RELEASE_MACOS_BIN unset") end
  return run_macos_binary_at(t, bin)
end

run_macos_binary_at = function(t, bin)
  local runner = env("LJ_RELEASE_MACOS_RUNNER")
  if not utils.file_exists(bin) then return maybe_skip("macos", bin .. " not found") end
  if runner and runner ~= "" then
    return run_macos_darling(t, bin, runner)
  end
  return run_direct_binary(t, "macos", bin, "OSX")
end

local function strip_newline(s)
  return (s:gsub("%s+$", ""))
end

local function archive_extract_root(t, name, archive)
  if not archive then return maybe_skip(name, "LJ_RELEASE_" .. name:upper() .. "_ARCHIVE unset") end
  if not utils.file_exists(archive) then return maybe_skip(name, archive .. " not found") end

  local extract_dir = t:tempname("lj-release-" .. name .. "-archive")
  t:run({ "rm", "-rf", extract_dir }, { quiet = true })
  t:run({ "mkdir", "-p", extract_dir }, { quiet = true })

  if archive:find("%.zip$") then
    if not command_exists("unzip") then
      t:run({ "rm", "-rf", extract_dir }, { quiet = true })
      return maybe_skip(name, "unzip not in PATH")
    end
    t:run({ "unzip", "-q", archive, "-d", extract_dir }, {
      quiet = true,
      timeout = "120s"
    })
  else
    if not command_exists("tar") then
      t:run({ "rm", "-rf", extract_dir }, { quiet = true })
      return maybe_skip(name, "tar not in PATH")
    end
    t:run({ "tar", "-xf", archive, "-C", extract_dir }, {
      quiet = true,
      timeout = "120s"
    })
  end

  local top = strip_newline(utils.capture_command(
    "find " .. shell_quote(extract_dir) ..
      " -mindepth 1 -maxdepth 1 -type d -print | sort | sed -n '1p'"))
  if top == "" then
    t:run({ "rm", "-rf", extract_dir }, { quiet = true })
    error("release " .. name .. " archive did not contain a top-level directory", 2)
  end
  return extract_dir, top
end

local function archive_bin(root, candidates)
  for i = 1, #candidates do
    local bin = root .. candidates[i]
    if utils.file_exists(bin) then return bin end
  end
  return nil
end

local function assert_archive_paths(top, name, paths)
  for i = 1, #paths do
    local path = top .. paths[i]
    if not utils.file_exists(path) then
      error("release " .. name .. " archive missing install-layout path " ..
            paths[i], 2)
    end
  end
end

local function common_archive_paths(binname)
  local p = release_prefix()
  return {
    p .. "/bin/" .. binname,
    p .. "/include/luajit-2.1/lua.h",
    p .. "/include/luajit-2.1/lualib.h",
    p .. "/include/luajit-2.1/lauxlib.h",
    p .. "/include/luajit-2.1/luaconf.h",
    p .. "/include/luajit-2.1/lua.hpp",
    p .. "/include/luajit-2.1/luajit.h",
    p .. "/lib/pkgconfig/luajit.pc",
    p .. "/share/luajit-2.1/jit/vmdef.lua",
    p .. "/share/doc/luajitmt/README",
    p .. "/share/doc/luajitmt/COPYRIGHT",
    p .. "/share/doc/luajitmt/BUILDINFO"
  }
end

local function run_archive_binary(t, name, archive, candidates, required_paths, run)
  local extract_dir, top = archive_extract_root(t, name, archive)
  if not extract_dir then return false end
  local ok, err = pcall(function()
    assert_archive_paths(top, name, required_paths)
    local bin = archive_bin(top, candidates)
    if not bin then
      return maybe_skip(name, "installed binary not found in " .. archive)
    end
    return run(bin)
  end)
  t:run({ "rm", "-rf", extract_dir }, { quiet = true })
  if not ok then error(err, 0) end
  return err
end

local function run_linux_archive(t)
  local p = release_prefix()
  local required = common_archive_paths("luajit")
  required[#required + 1] = p .. "/lib/libluajit-5.1.a"
  required[#required + 1] = p .. "/lib/libluajit-5.1.so"
  return run_archive_binary(t, "linux", env("LJ_RELEASE_LINUX_ARCHIVE"), {
    p .. "/bin/luajit"
  }, required, function(bin)
    return run_direct_binary(t, "linux", bin, "Linux")
  end)
end

local function run_windows_archive(t)
  local p = release_prefix()
  local required = common_archive_paths("luajit.exe")
  required[#required + 1] = p .. "/bin/lua51.dll"
  required[#required + 1] = p .. "/lib/libluajit-5.1.dll.a"
  return run_archive_binary(t, "windows", env("LJ_RELEASE_WINDOWS_ARCHIVE"), {
    p .. "/bin/luajit.exe"
  }, required, function(bin)
    return run_windows_binary_at(t, bin)
  end)
end

local function run_macos_archive(t)
  local p = release_prefix()
  local required = common_archive_paths("luajit")
  required[#required + 1] = p .. "/lib/libluajit-5.1.a"
  required[#required + 1] = p .. "/lib/libluajit-5.1.dylib"
  return run_archive_binary(t, "macos", env("LJ_RELEASE_MACOS_ARCHIVE"), {
    p .. "/bin/luajit"
  }, required, function(bin)
    return run_macos_binary_at(t, bin)
  end)
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

local function run_release_platform_archives(t)
  local ran = 0
  if run_linux_archive(t) then ran = ran + 1 end
  if run_windows_archive(t) then ran = ran + 1 end
  if run_macos_archive(t) then ran = ran + 1 end
  if ran == 0 and release_required("platform") then
    error("no release platform archives were provided", 2)
  end
  print("release platform archive checks completed: " .. ran .. " platform(s)")
end

return function(add)
  add({
    name = "release_linux_binary",
    description = "Linux x64 binary smoke check for CI/release",
    run = run_linux_binary
  })

  add({
    name = "release_windows_binary",
    description = "Windows x64 binary smoke check for CI/release",
    run = run_windows_binary
  })

  add({
    name = "release_macos_binary",
    description = "macOS x64 binary smoke check for CI/release",
    run = run_macos_binary
  })

  add({
    name = "release_platform_binaries",
    description = "Linux/Windows/macOS binary checks for CI/release",
    run = run_release_platform_binaries
  })

  add({
    name = "release_linux_archive",
    description = "release-only Linux x64 archive extraction and smoke check",
    run = run_linux_archive
  })

  add({
    name = "release_windows_archive",
    description = "release-only Windows x64 archive extraction and smoke check",
    run = run_windows_archive
  })

  add({
    name = "release_macos_archive",
    description = "release-only macOS x64 archive extraction and smoke check",
    run = run_macos_archive
  })

  add({
    name = "release_platform_archives",
    description = "release-only Linux/Windows/macOS archive checks",
    run = run_release_platform_archives
  })
end
