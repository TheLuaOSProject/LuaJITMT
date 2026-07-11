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
          "-Wl,--wrap=pthread_create"
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
          "-Wl,--wrap=lj_tg_attach",
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
      opts = { timeout = "20s" },
      message = "M4 TG registry lease tests passed"
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
