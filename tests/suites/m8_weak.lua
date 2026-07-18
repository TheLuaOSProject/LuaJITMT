local checks = require("suite_assert")
local build = require("suite_build")
local runtime = require("suite_runtime")
local utils = require("suite_utils")

local capture_luajit = runtime.capture_luajit
local luajit_script = runtime.luajit_script
local run_luajit_script_jit_modes = runtime.run_luajit_script_jit_modes
local gc2_test_cflags = build.gc2_test_helper_flag

local M8_C_FIXTURES = {
  "t-gc2-phase",
  "t-gc2-sidecar-publication",
  "t-gc2-finreg-cdata-preclaim-roots",
  "t-gc2-finreg-udata-roots",
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
  checks.assert_output_all_contains("finalizer error native stdio output",
                                    t:read(out), {
    "ERROR in finalizer: ",
    "finalizer native stdio smoke"
  }, "captured output")
end

local function run_default_matrix(t)
  t:build({ clean = true, quiet = true, xcflags = gc2_test_cflags })
  run_finalizer_error_native_stdio(t, { build = false })
  run_luajit_script_jit_modes(t, "t-weak-modes.lua")
  run_luajit_script_jit_modes(t, "t-m8-finalizer-spawn-live.lua", nil,
                              { timeout = "10s" })
  run_luajit_script_jit_modes(t, "t-gc2-finalizer-peer-collect.lua", nil,
                              { timeout = "30s" })
  run_luajit_script_jit_modes(t, "t-ffi-gc-finreg.lua", { "3", "72" })
  build.run_c_fixtures(t, M8_C_FIXTURES, {
    output_suffix = "_m8",
    cflags = gc2_test_cflags,
    timeout = "20s"
  })
end

local function run_paranoia_matrix(t)
  local xcflags = gc2_test_cflags .. " " .. build.gc2_paranoia_flags
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
    cflags = xcflags,
    timeout = "20s"
  })
end

return function(add)
  add({
    name = "m8_gc2_sidecar_publication",
    description = "nonwaiting FINREG/threading raw-sidecar publication",
    run = function(t)
      local paranoia = gc2_test_cflags .. " " .. build.gc2_paranoia_flags
      t:build({ clean = true, quiet = true, xcflags = gc2_test_cflags })
      build.run_c_fixtures(t, { "t-gc2-sidecar-publication" }, {
        output_suffix = "_m8",
        cflags = gc2_test_cflags,
        timeout = "20s"
      })
      t:build({ clean = true, quiet = true, xcflags = paranoia })
      build.run_c_fixtures(t, { "t-gc2-sidecar-publication" }, {
        output_suffix = "_m8_paranoia",
        cflags = paranoia,
        timeout = "20s"
      })
      print("M8 FINREG/threading sidecar publication passed")
    end
  })

  add({
    name = "m8_gc2_finreg_cdata_preclaim_roots",
    description = "GC2 cdata FINREG fixed preclaim-vector lifetime",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build.run_c_fixtures(t, { "t-gc2-finreg-cdata-preclaim-roots" }, {
        output_suffix = "_m8",
        cflags = gc2_test_cflags,
        timeout = "20s"
      })
      print("M8 cdata FINREG preclaim-vector lifetime passed")
    end
  })

  add({
    name = "m8_gc2_finreg_udata_roots",
    description = "GC2 userdata FINREG active/retired raw-root lifetime",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build.run_c_fixtures(t, { "t-gc2-finreg-udata-roots" }, {
        output_suffix = "_m8",
        cflags = gc2_test_cflags,
        timeout = "20s"
      })
      print("M8 userdata FINREG raw-root lifetime passed")
    end
  })

  add({
    name = "m8_finalizer_error_native_stdio",
    description = "default finalizer error reporter uses native stdio boundary",
    run = function(t)
      run_finalizer_error_native_stdio(t)
      print("M8 finalizer error native stdio behavior passed")
    end
  })

  add({
    name = "m8_finalizer_peer_collect",
    description = "peer full collection defers across finalizer join",
    run = function(t)
      t:build({ quiet = true })
      run_luajit_script_jit_modes(t, "t-gc2-finalizer-peer-collect.lua", nil,
                                  { timeout = "30s" })
      print("M8 finalizer peer-collection liveness passed")
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
