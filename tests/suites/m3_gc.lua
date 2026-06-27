local utils = require("suite_utils")
local checks = require("suite_assert")
local build = require("suite_build")
local runtime = require("suite_runtime")

local make_clean = build.make_clean
local make_default = build.build_default
local compile_and_run_c = build.compile_and_run_c
local run_c_fixtures = build.run_c_fixtures
local build_shared_library = build.build_shared_library
local capture_luajit = runtime.capture_luajit
local run_lua_test_case = runtime.run_lua_test_case
local run_luajit_script_jit_modes = runtime.run_luajit_script_jit_modes
local gc2_test_cflags = "-DLJ_GC2_TEST_HELPERS"

local function assert_x64_vm_static_guards(t)
  local vm = utils.read_file(t:path("src", "vm_x64.dasc"))
  local safepoints = checks.count_plain(vm, "vm_safepoint")
  if safepoints < 5 then
    error("x64 VM safepoint surface regressed: saw " .. safepoints ..
          " vm_safepoint references", 2)
  end
  if vm:find("DISPATCH_GL%(gc%.") then
    error("x64 VM must not load global GC state through DISPATCH_GL(gc.*)", 2)
  end
  if vm:find("barrierback", 1, true) then
    error("x64 VM must not reintroduce inline legacy barrierback", 2)
  end
  local tnew = vm:match("case BC_TNEW:(.-)case BC_TDUP:")
  if not tnew then
    error("x64 VM BC_TNEW block not found", 2)
  end
  if not tnew:find("call extern lj_tab_new0", 1, true) then
    error("x64 VM empty BC_TNEW path must stay on lj_tab_new0", 2)
  end
  if not tnew:find("call extern lj_tab_new", 1, true) then
    error("x64 VM non-empty BC_TNEW slow path must stay reachable", 2)
  end
end

local function build_loadlib_stopreq_so(t)
  return build_shared_library(t, t:tmp("lj_t-loadlib-stopreq.so"),
                              "t-loadlib-stopreq-lib.c")
end

local m3_scaffold_deps = {
  "m3_gc2_worker_scheduler",
  "m3_gc_active_thread_roots",
  "m3_safepoint_handshake",
  "m3_vmevent_native_stdio",
  "m3_vm_safepoint",
  "m3_interp_stock_joff",
  "m3_gc2_paranoia",
  "m2_arena_all",
  "m0_matrix"
}

local function vmevent_native_stdio_smoke()
  return [=[
jit.attach(function()
  error("vmevent native stdio smoke")
end, "bc")
local f = assert(loadstring("return 1"))
assert(f() == 1)
]=]
end

return function(add)
  local cases, register = utils.case_registry(add)

  register({
    name = "m3_gc2_worker_scheduler",
    description = "staged GC2 parked-worker scheduler guard and fixtures",
    run = function(t)
      make_clean(t)
      make_default(t, { jobs = false })

      compile_and_run_c(t, t:tmp("lj_t-gc2-worker-scheduler"),
                        "t-gc2-worker-scheduler.c", {
        cflags = gc2_test_cflags
      })
      run_luajit_script_jit_modes(t, "t-gc-workers.lua")

      print("M3 GC2 worker scheduler test passed")
    end
  })

  register({
    name = "m3_gc_active_thread_roots",
    description = "active thread GC roots preserve standard-library globals",
    run = function(t)
      make_clean(t)
      make_default(t, { jobs = false })
      run_luajit_script_jit_modes(t, "t-gc-active-thread-roots.lua", nil,
                                  { timeout = "10s" })
      print("M3 active thread root GC regression passed")
    end
  })

  register({
    name = "m3_safepoint_handshake",
    description = "C-level safepoint handshake fixture",
    run = function(t)
      local pthread = os.getenv("PTHREAD") or "-pthread"
      local loadlib_so = build_loadlib_stopreq_so(t)

      make_clean(t)
      make_default(t)
      compile_and_run_c(t, t:tmp("lj_t_safepoint_handshake"),
                        "t-safepoint-handshake.c", {
        cflags = { gc2_test_cflags, pthread },
        pthread = pthread,
        env = { LJ_LOADLIB_STOPREQ_SO = loadlib_so }
      })

      print("M3 safepoint handshake tests passed")
    end
  })

  register({
    name = "m3_vmevent_native_stdio",
    description = "VM-event failure reporting uses native stdio boundary",
    run = function(t)
      local out = t:tmp("lj_m3_vmevent_native_stdio.out")
      t:build({ quiet = true })
      capture_luajit(t, { "-e", vmevent_native_stdio_smoke() }, out, {
        stderr_to_stdout = true
      })
      checks.assert_file_all_contains(t, out, {
        "VM handler failed: ",
        "vmevent native stdio smoke"
      }, "VM-event native stdio output")
      print("M3 VM-event native stdio behavior passed")
    end
  })

  register({
    name = "m3_vm_safepoint",
    description = "focused x64 VM safepoint poll fixture",
    run = function(t)
      assert_x64_vm_static_guards(t)
      make_clean(t)
      make_default(t)
      compile_and_run_c(t, t:tmp("lj_t_vm_safepoint"),
                        "t-vm-safepoint.c", { timeout = "20s" })
    end
  })

  register({
    name = "m3_interp_stock_joff",
    description = "interpreter-only stock LuaJIT suite under -joff",
    run = function(t)
      make_default(t, { jobs = false })
      t:run({
        t:path("src", "luajit"),
        "-joff",
        "test.lua",
        "--quiet"
      }, {
        cwd = t:path("tests", "stock", "test"),
        env = { LUA_PATH = runtime.lua_path(t) },
        timeout = "240s"
      })
      print("M3 interpreter-only stock suite passed under -joff")
    end
  })

  register({
    name = "m3_gc2_paranoia",
    description = "GC2 paranoia build, oracle fixtures, and stock tests",
    run = function(t)
      make_clean(t)
      make_default(t, {
        args = { "XCFLAGS=-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1" }
      })
      run_c_fixtures(t, {
        "t-gc2-paranoia",
        "t-gc2-phase",
        "t-gc2-markbits",
        "t-gc2-traverse"
      }, {
        output_suffix = "_paranoia",
        cflags = "-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1"
      })
      runtime.run_stock(t, { "test.lua", "--quiet" })

      make_clean(t)
      make_default(t, {
        args = {
          "BUILDMODE=static",
          "XCFLAGS=-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1 -DLUAJIT_DISABLE_JIT"
        }
      })
      runtime.run_stock(t, { "test.lua", "--quiet", "-jit" })
    end
  })

  register({
    name = "m3_gc2_scaffold",
    description = "focused M3 GC2 scaffold tests and dependent gates",
    deps = m3_scaffold_deps,
    run = function(t)
      make_clean(t)
      make_default(t, { jobs = false })

      run_c_fixtures(t, {
        "t-gc2-phase",
        "t-gc2-markbits",
        "t-gc2-traverse"
      }, {
        cflags = gc2_test_cflags
      })

      utils.run_case(cases, t, "m3_gc2_worker_scheduler")
      utils.run_case(cases, t, "m3_gc_active_thread_roots")
      utils.run_case(cases, t, "m3_safepoint_handshake")
      utils.run_case(cases, t, "m3_vm_safepoint")
      utils.run_case(cases, t, "m3_interp_stock_joff")
      utils.run_case(cases, t, "m3_gc2_paranoia")
      runtime.run_lua_test_case(t, "m2_arena_all")

      make_clean(t)
      make_default(t, { jobs = false })
      make_clean(t)
      t:make({ "amalg" }, { quiet = true, jobs = false })

      run_lua_test_case(t, "m0_matrix")
      print("M3 GC2 scaffold tests passed")
    end
  })
end
