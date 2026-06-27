local checks = require("suite_assert")
local build = require("suite_build")
local runtime = require("suite_runtime")

local capture_luajit = runtime.capture_luajit
local luajit_script = runtime.luajit_script
local run_luajit_script_jit_modes = runtime.run_luajit_script_jit_modes
local gc2_test_cflags = "-DLJ_GC2_TEST_HELPERS"

local M8_C_FIXTURES = {
  "t-gc2-phase",
  "t-gc2-traverse",
  "t-m8-ffi-weak-newindex",
  "t-m8-close-finalizers",
  "t-m8-finalizer-state"
}

local function finalizer_error_native_stdio_smoke()
  return [=[
local ffi = require("ffi")
ffi.cdef("typedef struct { int x; } lj_finalizer_error_native_stdio_t;")
do
  local x = ffi.gc(ffi.new("lj_finalizer_error_native_stdio_t"), function()
    error("finalizer native stdio smoke")
  end)
end
collectgarbage("collect")
collectgarbage("collect")
]=]
end

local function run_finalizer_error_native_stdio(t, opts)
  opts = opts or {}
  local out = t:tmp("lj_m8_finalizer_error_native_stdio.out")
  if opts.build ~= false then
    t:build({ quiet = true })
  end
  capture_luajit(t, { "-e", finalizer_error_native_stdio_smoke() }, out, {
    stderr_to_stdout = true
  })
  checks.assert_file_all_contains(t, out, {
    "ERROR in finalizer: ",
    "finalizer native stdio smoke"
  }, "finalizer error native stdio output")
end

local function run_default_matrix(t)
  t:build({ clean = true, quiet = true })
  run_finalizer_error_native_stdio(t, { build = false })
  run_luajit_script_jit_modes(t, "t-weak-modes.lua")
  run_luajit_script_jit_modes(t, "t-m8-finalizer-spawn-live.lua", nil,
                              { timeout = "10s" })
  run_luajit_script_jit_modes(t, "t-ffi-gc-finreg.lua", { "3", "72" })
  build.run_c_fixtures(t, M8_C_FIXTURES, {
    output_suffix = "_m8",
    cflags = gc2_test_cflags
  })
end

local function run_paranoia_matrix(t)
  local xcflags = "-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1"
  t:build({ clean = true, quiet = true, xcflags = xcflags })
  luajit_script(t, "t-weak-modes.lua", nil, {
    joff = true,
    env = {
      LJ_M8_WEAK_RACE_ITERS = "0",
      LJ_M8_FINALIZER_SPAWN = "0"
    }
  })
  build.run_c_fixtures(t, M8_C_FIXTURES, {
    output_suffix = "_m8_paranoia",
    cflags = xcflags
  })
end

return function(add)
  add({
    name = "m8_finalizer_error_native_stdio",
    description = "default finalizer error reporter uses native stdio boundary",
    run = function(t)
      run_finalizer_error_native_stdio(t)
      print("M8 finalizer error native stdio behavior passed")
    end
  })

  add({
    name = "m8_weak",
    description = "M8 weak-table/finalizer semantic gates",
    run = function(t)
      run_default_matrix(t)
      run_paranoia_matrix(t)
      print("M8 weak/finalizer semantic gates passed")
    end
  })
end
