local utils = require("suite_utils")
local build = require("suite_build")
local runtime = require("suite_runtime")

local getenv = utils.getenv
local compile_and_run_sources = build.compile_and_run_sources

return function(add)
  runtime.add_luajit_script_cases(add, {
    {
      name = "m4_threading_api",
      description = "Lua-visible threading API behavior test",
      script = "t-threading-api.lua",
      opts = { joff = true }
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
      name = "m4_chan_stress",
      description = "focused M4 channel substrate stress C fixture",
      output = "lj_t-chan-stress",
      cfile = "t-chan-stress.c",
      message = "M4 channel stress tests passed"
    },
    {
      name = "m4_threading_capi",
      description = "public C threading API behavior fixture",
      output = "lj_t-threading-capi",
      cfile = "t-threading-capi.c",
      message = "M4 public C threading API tests passed"
    }
  })

  add({
    name = "m4_threading_shutdown",
    description = "VM shutdown interrupts unjoined parked and CPU-bound threads",
    run = function(t)
      local marker = t:tempname("m4-shutdown")
      local spin_marker = t:tempname("m4-shutdown-spin")
      t:remove(marker)
      t:remove(spin_marker)
      runtime.build_and_run_luajit_script(t, "t-threading-shutdown.lua", {
        marker,
        spin_marker
      }, { joff = true })

      for _, path in ipairs({ marker, spin_marker }) do
        local data = t:read(path)
        if not data:find("^false\n") then
          error(path .. ": expected first marker line to be false")
        end
        if not data:find("thread interrupted: VM shutdown", 1, true) then
          error(path .. ": missing shutdown interruption message")
        end
        t:remove(path)
      end
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
    }
  })

  add({
    name = "m4_tsan_drivers",
    description = "M4 C unit drivers under ThreadSanitizer",
    run = function(t)
      local cflags = getenv("CFLAGS",
        "-std=gnu99 -O1 -g -Wall -Wextra -Wno-tsan -mcx16 " ..
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
