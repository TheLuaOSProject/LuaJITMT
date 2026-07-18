local utils = require("suite_utils")
local checks = require("suite_assert")
local build = require("suite_build")
local runtime = require("suite_runtime")

local getenv = utils.getenv
local with_temp_paths = utils.with_temp_paths
local compile_and_run_sources = build.compile_and_run_sources

return function(add)
  local terminal_orphan_libs = {
    "-lm", "-ldl", os.getenv("PTHREAD") or "-pthread",
    "-Wl,--wrap=lj_arena_hugetab_transfer"
  }
  if jit and jit.os == "Linux" then
    terminal_orphan_libs[#terminal_orphan_libs + 1] = "-Wl,--wrap=munmap"
  end

  add({
    name = "m4_tgslot_token_model",
    description = "stable TG-slot incarnation and lease token model",
    run = function(t)
      compile_and_run_sources(t, t:tmp("lj_t-tgslot-token"),
        { t:path("tests", "t-tgslot-token.c") }, {
        default_cflags = false,
        include_src = true,
        link_luajit = false,
        libs = {},
        cflags = "-std=gnu11 -O2 -Wall -Wextra -Werror -pthread -mcx16",
        timeout = "20s"
      })
      print("M4 stable TG-slot token model passed")
    end
  })

  add({
    name = "m4_tgregistry_slot_model",
    description = "stable external TG registry-slot body and lease model",
    run = function(t)
      compile_and_run_sources(t, t:tmp("lj_t-tgregistry-slot"),
        { t:path("tests", "t-tgregistry-slot.c") }, {
        default_cflags = false,
        include_src = true,
        link_luajit = false,
        libs = {},
        cflags = "-std=gnu11 -O2 -Wall -Wextra -Werror -pthread -mcx16",
        timeout = "30s"
      })
      print("M4 stable external TG registry-slot model passed")
    end
  })

  add({
    name = "m4_universe_token_model",
    description = "exact universe admission, publication epoch, and close-owner model",
    run = function(t)
      compile_and_run_sources(t, t:tmp("lj_t-universe-token"),
        {
          t:path("tests", "t-universe-token.c"),
          t:path("src", "lj_universe.c")
        }, {
        default_cflags = false,
        include_src = true,
        link_luajit = false,
        libs = {},
        cflags = "-std=gnu11 -O2 -Wall -Wextra -Werror -pthread -mcx16 " ..
                 "-DLJ_UNIVERSE_TEST_HELPERS",
        timeout = "30s"
      })
      print("M4 exact universe admission token model passed")
    end
  })

  add({
    name = "m4_posix_signal_artifacts",
    description = "x86-64 signal getter disassembly and relocation contract",
    run = function(t)
      if jit and jit.os == "Linux" and jit.arch == "x64" then
        build.with_default_build_restore(t, function()
          t:build({
            clean = true,
            xcflags = "-DLJ_THR_SIGNAL_TEST_HELPERS " ..
                       "-DLJ_PROFILE_TIMER_TEST_HELPERS"
          })
          t:run({ "sh", t:path("tools", "ci",
                               "m4_posix_signal_artifacts.sh") }, {
            timeout = "20s"
          })
        end)
      elseif jit and jit.os == "OSX" and jit.arch == "x64" then
        build.with_default_build_restore(t, function()
          t:build({
            clean = true,
            xcflags = "-DLJ_THR_SIGNAL_TEST_HELPERS " ..
                       "-DLJ_PROFILE_TIMER_TEST_HELPERS"
          })
          t:run({ "sh", t:path("tools", "ci",
                               "m4_posix_signal_macho_artifacts.sh") }, {
            timeout = "20s"
          })
        end)
      end
      print("M4 POSIX signal getter artifact contract passed")
    end
  })

  runtime.add_luajit_script_cases(add, {
    {
      name = "m4_threading_api",
      description = "Lua-visible threading API behavior test",
      script = "t-threading-api.lua",
      opts = { joff = true }
    },
    {
      name = "m4_threading_coroutine",
      description = "coroutine yield/resume handoff across OS threads",
      script = "t-threading-coroutine.lua",
      opts = { timeout = "30s" }
    },
    {
      name = "m4_threading_coroutine_joff",
      description = "coroutine yield/resume handoff across OS threads under -joff",
      script = "t-threading-coroutine.lua",
      opts = { joff = true, timeout = "30s" }
    },
    {
      name = "m4_threading_hooks",
      description = "debug hook redispatch across live OS threads",
      script = "t-threading-hooks.lua",
      opts = { timeout = "30s" }
    },
    {
      name = "m4_threading_hooks_joff",
      description = "debug hook redispatch across live OS threads under -joff",
      script = "t-threading-hooks.lua",
      opts = { joff = true, timeout = "30s" }
    }
  })

  runtime.add_luajit_c_fixture_cases(add, {
    {
      name = "m4_thr_substrate",
      description = "focused M4 thread substrate C fixture",
      output = "lj_t-thr-substrate",
      cfile = "t-thr-substrate.c",
      message = "M4 thread substrate tests passed"
    },
    {
      name = "m4_threading_live_root",
      description = "threading.thread live-root candidate validation fixture",
      output = "lj_t-threading-live-root",
      cfile = "t-threading-live-root.c",
      message = "M4 threading live-root validation tests passed"
    },
    {
      name = "m4_chan_stress",
      description = "focused M4 channel substrate stress C fixture",
      output = "lj_t-chan-stress",
      cfile = "t-chan-stress.c",
      message = "M4 channel stress tests passed"
    },
    {
      name = "m4_threading_spawn_native",
      description = "threading.spawn pthread_create native STOPREQ fixture",
      output = "lj_t-threading-spawn-native",
      cfile = "t-threading-spawn-native.c",
      opts = {
        timeout = "20s",
        libs = {
          "-lm", "-ldl", os.getenv("PTHREAD") or "-pthread",
          "-Wl,--wrap=pthread_create", "-Wl,--wrap=lj_vm_cpcall",
          "-Wl,--wrap=lj_tg_fini_thread"
        }
      },
      message = "M4 threading.spawn native STOPREQ tests passed"
    },
    {
      name = "m4_threading_lifecycle",
      description = "foreign detach/reclaim and close/attach lifetime fixture",
      output = "lj_t-threading-lifecycle",
      cfile = "t-threading-lifecycle.c",
      opts = {
        timeout = "20s",
        libs = {
          "-lm", "-ldl", os.getenv("PTHREAD") or "-pthread",
          "-Wl,--wrap=lj_state_release",
          "-Wl,--wrap=lj_state_claim",
          "-Wl,--wrap=lj_tg_attach",
          "-Wl,--wrap=lj_vm_cpcall",
          "-Wl,--wrap=lj_threading_shutdown",
          "-Wl,--wrap=lj_arena_alloc_transfer"
        }
      },
      message = "M4 threading lifecycle barrier tests passed"
    },
    {
      name = "m4_tg_registry_lease",
      description = "TG registry SMR reader/reclaimer exclusion fixture",
      output = "lj_t-tg-registry-lease",
      cfile = "t-tg-registry-lease.c",
      opts = build.gc2_test_helper_opts({ timeout = "20s" }),
      message = "M4 TG registry lease tests passed"
    },
    {
      name = "m4_thread_gcprep",
      description = "terminal coroutine preparation and open-upvalue fixture",
      output = "lj_t-thread-gcprep",
      cfile = "t-thread-gcprep.c",
      opts = build.gc2_test_helper_opts({ timeout = "20s" }),
      message = "M4 terminal coroutine preparation tests passed"
    },
    {
      name = "m4_tg_tls_binding",
      description = "exact TG registry TLS handle-move and ABA fixture",
      output = "lj_t-tg-tls-binding",
      cfile = "t-tg-tls-binding.c",
      opts = {
        clean = true,
        xcflags = "-DLJ_THR_TLS_TEST_HELPERS",
        cflags = "-DLJ_THR_TLS_TEST_HELPERS",
        timeout = "20s"
      },
      message = "M4 exact TG TLS binding tests passed"
    },
    {
      name = "m4_posix_signal_safety",
      description = "exact POSIX TG signal cache and SIGPROF lifecycle fixture",
      output = "lj_t-posix-signal-safety",
      cfile = "t-posix-signal-safety.c",
      opts = {
        clean = true,
        xcflags = "-DLJ_THR_SIGNAL_TEST_HELPERS " ..
                  "-DLJ_PROFILE_TIMER_TEST_HELPERS",
        cflags = "-DLJ_THR_SIGNAL_TEST_HELPERS " ..
                 "-DLJ_PROFILE_TIMER_TEST_HELPERS",
        timeout = "20s"
      },
      message = "M4 POSIX signal cache/timer lifecycle tests passed"
    },
    {
      name = "m4_tg_terminal_orphan",
      description = "capacity-independent terminal TG allocator drain fixture",
      output = "lj_t-tg-terminal-orphan",
      cfile = "t-tg-terminal-orphan.c",
      opts = {
        timeout = "20s",
        libs = terminal_orphan_libs
      },
      message = "M4 terminal TG allocator drain tests passed"
    }
  })

  add({
    name = "m4_posix_signal_dso_lifetime",
    description = "SIGPROF containing-image permanent pin across dlclose",
    run = function(t)
      if jit and (jit.os == "Linux" or jit.os == "OSX") and
         jit.arch == "x64" then
        local helpers = "-DLJ_THR_SIGNAL_TEST_HELPERS " ..
                        "-DLJ_PROFILE_TIMER_TEST_HELPERS"
        local loader = t:tmp("lj_t-posix-signal-dso-loader")
        local image = t:path("src", "libluajit.so")
        t:build({ clean = true, xcflags = helpers })
        t:cc(loader, { t:path("tests", "t-posix-signal-safety.c") }, {
          cflags = "-DLJ_PROFILE_DSO_LOADER",
          include_src = true,
          link_luajit = false,
          libs = { "-ldl", os.getenv("PTHREAD") or "-pthread" }
        })
        for _, mode in ipairs({
          "pin-failure", "pin-mismatch", "success", "stop-failure"
        }) do
          t:run({ loader, image, mode }, { timeout = "20s" })
        end
      end
      print("M4 POSIX signal containing-image lifetime tests passed")
    end
  })

  add({
    name = "m4_threading_shutdown",
    description = "VM shutdown interrupts unjoined parked and CPU-bound threads",
    run = function(t)
      with_temp_paths(t, { "m4-shutdown", "m4-shutdown-spin" },
        function(marker, spin_marker)
          runtime.build_and_run_luajit_script(t, "t-threading-shutdown.lua", {
            marker,
            spin_marker
          }, { joff = true })

          for _, path in ipairs({ marker, spin_marker }) do
            local data = t:read(path)
            local first = checks.output_lines(data)[1]
            if first ~= "false" then
              error("shutdown marker first line: expected false, got " ..
                    tostring(first), 2)
            end
            checks.assert_output_contains("shutdown interruption marker",
                                          data,
                                          "thread interrupted: VM shutdown",
                                          "marker text")
          end
        end)
    end
  })

  add({
    name = "m4_loadlib_cache_race",
    description = "concurrent package.loadlib reuses one cached dlopen handle",
    run = function(t)
      local pthread = os.getenv("PTHREAD") or "-pthread"
      local so = build.build_shared_library(t, t:tmp("lj_t-loadlib-race.so"),
                                            "t-loadlib-stopreq-lib.c")
      compile_and_run_sources(t, t:tmp("lj_t-loadlib-cache-race"), {
        t:path("tests", "t-loadlib-cache-race.c")
      }, {
        libs = { "-lm", "-ldl", pthread, "-Wl,--wrap=dlopen" },
        env = { LJ_LOADLIB_RACE_SO = so },
        timeout = "20s"
      })
    end
  })

  runtime.add_luajit_script_cases(add, {
    {
      name = "m4_threading_smoke",
      description = "pure-compute threading smoke test under the built VM",
      script = "t-mt-smoke.lua",
      opts = {
        joff = true,
        env = {
          LJ_M4_MT_SMOKE_THREADS = getenv("LJ_M4_MT_SMOKE_THREADS", "8")
        }
      }
    },
    {
      name = "m4_threading_litmus",
      script = "t-mt-litmus.lua",
      opts = {
        joff = true,
        env = {
          LJ_M4_LITMUS_REPS = getenv("LJ_M4_LITMUS_REPS", "100")
        }
      }
    },
    {
      name = "m4_threading_stress",
      script = "t-threading-stress.lua",
      opts = {
        joff = true,
        env = {
          LJ_M4_THREAD_STRESS_REPS =
            getenv("LJ_M4_THREAD_STRESS_REPS", "1000")
        }
      }
    },
    {
      name = "m4_threading_upvalue",
      script = "t-threading-upvalue.lua",
      opts = { joff = true }
    },
    {
      name = "m4_threading_join_gcscan",
      description = "thread join makes progress while workers force GC root scans",
      script = "t-threading-join-gcscan.lua",
      opts = {
        joff = true,
        timeout = "45s",
        env = {
          LJ_M4_JOIN_GCSCAN_REPS =
            getenv("LJ_M4_JOIN_GCSCAN_REPS", "300")
        }
      }
    },
    {
      name = "m4_threading_require_once",
      description = "concurrent require executes one module body and " ..
        "preserves stock recursion behavior",
      script = "t-threading-require-once.lua",
      opts = {
        joff = true,
        env = {
          LJ_M4_REQUIRE_WORKERS =
            getenv("LJ_M4_REQUIRE_WORKERS", "8")
        }
      }
    }
  })

  add({
    name = "m4_tsan_drivers",
    description = "M4 C unit drivers under ThreadSanitizer",
    run = function(t)
      local cflags = getenv("CFLAGS",
        "-std=gnu11 -O1 -g -Wall -Wextra -Wno-tsan -mcx16 " ..
        "-fsanitize=thread -fno-omit-frame-pointer")
      local target_cflags = getenv("TARGET_TSAN_CFLAGS",
        "-O1 -g -Wno-tsan -fsanitize=thread -fno-omit-frame-pointer")
      local target_ldflags = getenv("TARGET_TSAN_LDFLAGS",
        "-fsanitize=thread")
      local tsan_options = getenv("TSAN_OPTIONS",
        "halt_on_error=1 second_deadlock_stack=1")

      t:make({ "clean" }, { quiet = true, jobs = false })
      t:make({
        "TARGET_CFLAGS=" .. target_cflags,
        "TARGET_LDFLAGS=" .. target_ldflags,
        "TARGET_SHLDFLAGS=" .. target_ldflags
      }, { quiet = true })

      local env = { TSAN_OPTIONS = tsan_options }
      local thr_out = t:tmp("lj_t-thr-substrate-tsan")
      local chan_out = t:tmp("lj_t-chan-stress-tsan")

      compile_and_run_sources(t, thr_out, {
        t:path("tests", "t-thr-substrate.c")
      }, {
        default_cflags = false,
        cflags = cflags,
        link_luajit = true,
        libs = { "-lm", "-ldl", "-pthread" },
        env = env
      })

      compile_and_run_sources(t, chan_out, {
        t:path("tests", "t-chan-stress.c")
      }, {
        default_cflags = false,
        cflags = cflags,
        link_luajit = true,
        libs = { "-lm", "-ldl", "-pthread" },
        env = env
      })

      print("M4 TSAN driver tests passed")
    end
  })
end
