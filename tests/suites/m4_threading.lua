local utils = require("suite_utils")

local getenv = utils.getenv

return function(add)
  local function build_and_run(name, script, env)
    add({
      name = name,
      description = script .. " under the built VM",
      run = function(t)
        t:build({ clean = true, quiet = true })
        t:luajit({ "-joff", t:path("tests", script) }, { env = env })
      end
    })
  end

  add({
    name = "m4_threading_api",
    description = "Lua-visible threading API behavior test",
    run = function(t)
      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", t:path("tests", "t-threading-api.lua") })
    end
  })

  add({
    name = "m4_thr_substrate",
    description = "focused M4 thread substrate C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-thr-substrate"),
                              "t-thr-substrate.c")
      print("M4 thread substrate tests passed")
    end
  })

  add({
    name = "m4_chan_stress",
    description = "focused M4 channel substrate stress C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-chan-stress"),
                              "t-chan-stress.c")
      print("M4 channel stress tests passed")
    end
  })

  add({
    name = "m4_threading_capi",
    description = "public C threading API behavior fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-threading-capi"),
                              "t-threading-capi.c")
      print("M4 public C threading API tests passed")
    end
  })

  add({
    name = "m4_threading_shutdown",
    description = "VM shutdown interrupts unjoined parked and CPU-bound threads",
    run = function(t)
      local marker = t:tempname("m4-shutdown")
      local spin_marker = t:tempname("m4-shutdown-spin")
      t:remove(marker)
      t:remove(spin_marker)
      t:build({ clean = true, quiet = true })
      t:luajit({
        "-joff",
        t:path("tests", "t-threading-shutdown.lua"),
        marker,
        spin_marker
      })

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

  add({
    name = "m4_threading_smoke",
    description = "pure-compute threading smoke test under the built VM",
    run = function(t)
      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", t:path("tests", "t-mt-smoke.lua") }, {
        env = {
          LJ_M4_MT_SMOKE_THREADS = os.getenv("LJ_M4_MT_SMOKE_THREADS") or "8"
        }
      })
    end
  })

  build_and_run("m4_threading_litmus", "t-mt-litmus.lua", {
    LJ_M4_LITMUS_REPS = os.getenv("LJ_M4_LITMUS_REPS") or "100"
  })

  build_and_run("m4_threading_stress", "t-threading-stress.lua", {
    LJ_M4_THREAD_STRESS_REPS = os.getenv("LJ_M4_THREAD_STRESS_REPS") or "1000"
  })

  build_and_run("m4_threading_upvalue", "t-threading-upvalue.lua")

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

      t:cc(thr_out, { t:path("tests", "t-thr-substrate.c") }, {
        default_cflags = false,
        cflags = cflags,
        link_luajit = true,
        libs = { "-lm", "-ldl", "-pthread" }
      })
      t:run({ thr_out }, { env = env })

      t:cc(chan_out, { t:path("tests", "t-chan-stress.c") }, {
        default_cflags = false,
        cflags = cflags,
        link_luajit = true,
        libs = { "-lm", "-ldl", "-pthread" }
      })
      t:run({ chan_out }, { env = env })

      print("M4 TSAN driver tests passed")
    end
  })
end
