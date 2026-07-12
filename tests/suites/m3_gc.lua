local utils = require("suite_utils")
local checks = require("suite_assert")
local build = require("suite_build")
local runtime = require("suite_runtime")

local make_clean = build.make_clean
local make_default = build.build_default
local compile_and_run_c = build.compile_and_run_c
local compile_and_run_sources = build.compile_and_run_sources
local run_c_fixtures = build.run_c_fixtures
local build_shared_library = build.build_shared_library
local capture_luajit = runtime.capture_luajit
local run_lua_test_case = runtime.run_lua_test_case
local run_luajit_script_jit_modes = runtime.run_luajit_script_jit_modes
local gc2_test_cflags = build.gc2_test_helper_flag

local function build_loadlib_stopreq_so(t)
  return build_shared_library(t, t:tmp("lj_t-loadlib-stopreq.so"),
                              "t-loadlib-stopreq-lib.c")
end

local function gc2_retired_symbol_gate(t, artifacts)
  local argv = {
    "sh", t:path("tools", "ci", "gc2_no_retired_symbols.sh")
  }
  for i = 1, #artifacts do argv[#argv + 1] = artifacts[i] end
  t:run(argv, { timeout = "30s" })
end

local m3_scaffold_deps = {
  "m3_gc_root_pending",
  "m3_gc_root_pending_race",
  "m3_gcflags_atomic",
  "m3_gc2_markword_token_model",
  "m3_gc2_activation_runtime",
  "m3_gc2_no_legacy_runtime",
  "m3_gc2_internal_allocator_only",
  "m3_gc2_recovery",
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
    name = "m3_gc2_markword_token_model",
    description = "epoch markword and typed activation standalone model",
    run = function(t)
      compile_and_run_sources(t, t:tmp("lj_t-gc2-markword-token"),
        { t:path("tests", "t-gc2-markword-token.c") }, {
        default_cflags = false,
        include_src = true,
        link_luajit = false,
        libs = {},
        cflags = "-std=gnu11 -O2 -Wall -Wextra -Werror -pthread -mcx16"
      })
      print("M3 GC2 markword and activation model passed")
    end
  })

  register({
    name = "m3_gc2_activation_runtime",
    description = "veto-only typed GC2 activation runtime migration",
    run = function(t)
      make_clean(t)
      make_default(t, { jobs = false })
      compile_and_run_c(t, t:tmp("lj_t-gc2-activation-veto"),
                        "t-gc2-activation-veto.c", {
        cflags = gc2_test_cflags,
        timeout = "60s"
      })
      print("M3 veto-only GC2 activation runtime migration passed")
    end
  })

  register({
    name = "m3_gcflags_atomic",
    description = "concurrent GC header flag read/modify/write regression",
    run = function(t)
      make_clean(t)
      make_default(t, { jobs = false })
      compile_and_run_c(t, t:tmp("lj_t-gcflags-atomic"),
                        "t-gcflags-atomic.c")
      print("M3 atomic GC header flags passed")
    end
  })

  register({
    name = "m3_gc_root_pending",
    description = "per-TG pending GC root publication fixture",
    run = function(t)
      make_clean(t)
      make_default(t, { jobs = false })
      compile_and_run_c(t, t:tmp("lj_t-gc-root-pending"),
                        "t-gc-root-pending.c")
      print("M3 pending GC root publication test passed")
    end
  })

  register({
    name = "m3_gc_root_pending_race",
    description = "pending-root load/xchg/CAS activation-race regression",
    run = function(t)
      build.with_default_build_restore(t, function()
        build.build_and_run_c(t, t:tmp("lj_t-gc-root-pending-race"),
                              "t-gc-root-pending-race.c",
                              build.gc2_test_helper_opts({
          jobs = false,
          timeout = "30s"
        }))
      end, { jobs = false })
      print("M3 pending-root activation-race CAS test passed")
    end
  })

  register({
    name = "m3_gc2_no_legacy_runtime",
    description = "GC2-only runtime/shutdown and retired-symbol absence",
    run = function(t)
      build.with_default_build_restore(t, function()
        make_clean(t)
        make_default(t, { jobs = false })
        gc2_retired_symbol_gate(t, {
          t:path("src", "lj_gc.o"),
          t:path("src", "libluajit.a")
        })
        compile_and_run_c(t, t:tmp("lj_t-gc2-no-legacy-runtime"),
                          "t-gc2-no-legacy-runtime.c", {
          timeout = "60s"
        })

        make_clean(t)
        t:make({ "amalg" }, { quiet = true, jobs = false })
        gc2_retired_symbol_gate(t, {
          t:path("src", "ljamalg.o"),
          t:path("src", "libluajit.a")
        })
        compile_and_run_c(t, t:tmp("lj_t-gc2-no-legacy-runtime-amalg"),
                          "t-gc2-no-legacy-runtime.c", {
          timeout = "60s"
        })
      end, { jobs = false })
    end
  })

  register({
    name = "m3_gc2_internal_allocator_only",
    description = "temporary internal-arena-only lua_Alloc safety boundary",
    run = function(t)
      local sysmalloc = "-DLUAJIT_USE_SYSMALLOC -DLUA_USE_APICHECK"
      make_clean(t)
      make_default(t, { jobs = false })
      compile_and_run_c(t, t:tmp("lj_t-gc2-internal-allocator-only"),
                        "t-gc2-internal-allocator-only.c", {
        timeout = "60s"
      })
      build.with_default_build_restore(t, function()
        make_clean(t)
        make_default(t, {
          jobs = false,
          args = { "XCFLAGS=" .. sysmalloc }
        })
        compile_and_run_c(t,
                          t:tmp("lj_t-gc2-internal-allocator-only-sysmalloc"),
                          "t-gc2-internal-allocator-only.c", {
          cflags = sysmalloc,
          timeout = "60s"
        })
      end, { jobs = false })
    end
  })

  register({
    name = "m3_gc2_worker_scheduler",
    description = "staged GC2 parked-worker scheduler behavior and fixtures",
    run = function(t)
      local pthread = os.getenv("PTHREAD") or "-pthread"
      make_clean(t)
      make_default(t, { jobs = false })

      compile_and_run_c(t, t:tmp("lj_t-gc2-worker-scheduler"),
                        "t-gc2-worker-scheduler.c", {
        cflags = gc2_test_cflags,
        libs = {
          "-lm", "-ldl", pthread,
          "-Wl,--wrap=pthread_create", "-Wl,--wrap=pthread_join"
        }
      })
      run_luajit_script_jit_modes(t, "t-gc-workers.lua")

      print("M3 GC2 worker scheduler test passed")
    end
  })

  register({
    name = "m3_gc_active_thread_roots",
    description = "active thread GC roots and explicit GC assistance",
    run = function(t)
      make_clean(t)
      make_default(t, { jobs = false })
      compile_and_run_c(t, t:tmp("lj_t-gc-active-collect-assist"),
                        "t-gc-active-collect-assist.c", {
        cflags = gc2_test_cflags
      })
      run_luajit_script_jit_modes(t, "t-gc-active-thread-roots.lua", nil,
                                  { timeout = "60s" })
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
      compile_and_run_c(t, t:tmp("lj_t-vmevent-native-stopreq"),
                        "t-vmevent-native-stopreq.c", { timeout = "20s" })
      capture_luajit(t, { "-e", vmevent_native_stdio_smoke() }, out, {
        stderr_to_stdout = true
      })
      checks.assert_output_all_contains("VM-event native stdio output",
                                        t:read(out), {
        "VM handler failed: ",
        "vmevent native stdio smoke"
      }, "captured output")
      print("M3 VM-event native stdio behavior passed")
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
    name = "m3_interp_stock_joff",
    description = "interpreter-only stock LuaJIT suite under -joff",
    run = function(t)
      make_default(t, { jobs = false })
      runtime.run_stock(t, { "-joff", "test.lua", "--quiet" }, {
	timeout = "240s"
      })
      print("M3 interpreter-only stock suite passed under -joff")
    end
  })

  register({
    name = "m3_gc2_recovery",
    description = "GC2 allocation-free no-drop recovery and replay protocol",
    run = function(t)
      local paranoia_flags = gc2_test_cflags .. " " ..
        build.gc2_paranoia_flags
      build.with_default_build_restore(t, function()
        build.build_and_run_c(t, t:tmp("lj_t-gc2-recovery"),
                              "t-gc2-recovery.c",
                              build.gc2_test_helper_opts({
          jobs = false,
          timeout = "60s"
        }))

        build.clean_build(t, {
          jobs = false,
          quiet = true,
          xcflags = paranoia_flags
        })
        compile_and_run_c(t, t:tmp("lj_t-gc2-recovery-paranoia"),
                          "t-gc2-recovery.c", {
          cflags = paranoia_flags,
          timeout = "60s"
        })
      end, { jobs = false })
      print("M3 GC2 no-drop recovery protocol passed")
    end
  })

  register({
    name = "m3_gc2_paranoia",
    description = "GC2 paranoia build, oracle fixtures, and stock tests",
    run = function(t)
      build.with_default_build_restore(t, function()
        make_default(t, {
          args = { "XCFLAGS=" .. build.gc2_paranoia_flags }
        })
        run_c_fixtures(t, {
          "t-gc2-paranoia",
          "t-gc2-phase",
          "t-gc2-markbits",
          "t-gc2-traverse"
        }, {
          output_suffix = "_paranoia",
          cflags = build.gc2_paranoia_flags
        })
        runtime.run_stock(t, { "test.lua", "--quiet" })

        make_clean(t)
        make_default(t, {
          args = {
            "BUILDMODE=static",
            "XCFLAGS=" .. build.gc2_paranoia_nojit_flags
          }
        })
        runtime.run_stock(t, { "test.lua", "--quiet", "-jit" })
      end)
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
        "t-gcflags-atomic",
        "t-gc2-phase",
        "t-gc2-markbits",
        "t-gc2-traverse"
      }, {
        cflags = gc2_test_cflags
      })

      utils.run_case(cases, t, "m3_gc2_markword_token_model")
      utils.run_case(cases, t, "m3_gc2_recovery")
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
