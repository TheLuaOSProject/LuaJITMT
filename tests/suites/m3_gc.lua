local utils = require("suite_utils")
local runtime = require("suite_runtime")

local make_clean = runtime.make_clean
local make_default = runtime.build_default
local compile_and_run_c = runtime.compile_and_run_c
local run_c_fixtures = runtime.run_c_fixtures

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
      t:luajit({ "-joff", t:path("tests", "t-gc-workers.lua") })
      t:luajit({ t:path("tests", "t-gc-workers.lua") })

      print("M3 GC2 worker scheduler test passed")
    end
  })

  register({
    name = "m3_safepoint_handshake",
    description = "C-level safepoint handshake fixture",
    run = function(t)
      local pthread = os.getenv("PTHREAD") or "-pthread"

      make_clean(t)
      make_default(t)
      compile_and_run_c(t, t:tmp("lj_t_safepoint_handshake"),
                        "t-safepoint-handshake.c", {
        cflags = pthread,
        pthread = pthread
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

      utils.run_registered_case(cases, t, "m3_gc2_worker_scheduler")
      utils.run_registered_case(cases, t, "m3_safepoint_handshake")
      utils.run_registered_case(cases, t, "m3_vm_safepoint")
      utils.run_registered_case(cases, t, "m3_gc2_paranoia")
      runtime.run_lua_test_case(t, "m2_arena_all")

      make_clean(t)
      make_default(t, { jobs = false })
      make_clean(t)
      t:make({ "amalg" }, { quiet = true, jobs = false })

      t:run({ t:path("tools", "ci", "m0_matrix.sh") })
      print("M3 GC2 scaffold tests passed")
    end
  })
end
