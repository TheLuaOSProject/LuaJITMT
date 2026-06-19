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
end
