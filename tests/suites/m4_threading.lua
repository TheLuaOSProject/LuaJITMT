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
    description = "Lua-visible threading API smoke test and join cleanup guard",
    run = function(t)
      local block = t:c_block(t:path("src", "lib_threading.c"),
                              "static int threading_join_core")
      t:assert_text_contains("threading_join_core", block,
                             "join_actions = lj_native_leave(L)")
      t:assert_text_ordered("threading_join_core", block, {
        "lj_state_checkstack(L, th->nresults + 1u)",
        "lua_State *child = lj_thread_state_load_acq(th)",
        "copyTV(L, L->top++, child->base + i)",
        "threading_live_remove(th)",
        "lj_safepoint_checkstop(L, join_actions)"
      })

      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", t:path("tests", "t-threading-api.lua") })
    end
  })

  add({
    name = "m4_threading_shutdown",
    description = "VM shutdown interrupts unjoined parked and CPU-bound threads",
    run = function(t)
      local vm_safepoints = table.concat({
        t:read(t:path("src", "lj_safepoint.c")),
        t:read(t:path("src", "lj_safepoint.h")),
        t:read(t:path("src", "vm_x64.dasc"))
      }, "\n")
      for _, needle in ipairs({
        "lj_safepoint_ack_check",
        "call extern lj_safepoint_ack_check",
        "TGPOLL, dword [DISPATCH+DISPATCH_TG(poll)]",
        "cmp TGPOLL, 0"
      }) do
        t:assert_text_contains("VM safepoint sources", vm_safepoints, needle)
      end

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
end
