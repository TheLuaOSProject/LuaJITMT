return function(add)
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
end
