local utils = require("suite_utils")
local build = require("suite_build")
local runtime = require("suite_runtime")

local make_clean = build.make_clean
local make_default = build.build_default
local compile_and_run_c = build.compile_and_run_c
local run_c_fixtures = build.run_c_fixtures
local run_lua_test_case = runtime.run_lua_test_case
local run_luajit_script_jit_modes = runtime.run_luajit_script_jit_modes
local shell_quote = utils.shell_quote

local function build_loadlib_stopreq_so(t)
  local out = t:tmp("lj_t-loadlib-stopreq.so")
  t:run(t.compiler .. " -shared -fPIC -O2 -Wall -Wextra -Werror " ..
        shell_quote(t:path("tests", "t-loadlib-stopreq-lib.c")) ..
        " -o " .. shell_quote(out), { quiet = true })
  return out
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
                        "t-gc2-worker-scheduler.c")
      run_luajit_script_jit_modes(t, "t-gc-workers.lua")

      print("M3 GC2 worker scheduler test passed")
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
        cflags = pthread,
        pthread = pthread,
        env = { LJ_LOADLIB_STOPREQ_SO = loadlib_so }
      })

      print("M3 safepoint handshake tests passed")
    end
  })

  register({
    name = "m3_vm_safepoint",
    description = "focused x64 VM safepoint poll fixture",
    run = function(t)
      make_clean(t)
      make_default(t)
      compile_and_run_c(t, t:tmp("lj_t_vm_safepoint"),
                        "t-vm-safepoint.c", { timeout = "20s" })
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
    run = function(t)
      make_clean(t)
      make_default(t, { jobs = false })

      run_c_fixtures(t, {
        "t-gc2-phase",
        "t-gc2-markbits",
        "t-gc2-traverse"
      })

      utils.run_case(cases, t, "m3_gc2_worker_scheduler")
      utils.run_case(cases, t, "m3_safepoint_handshake")
      utils.run_case(cases, t, "m3_vm_safepoint")
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
