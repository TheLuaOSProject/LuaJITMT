local M2_ORDER = {
  "m2_arena_bitmap",
  "m2_arena_map",
  "m2_arena_alloc",
  "m2_arena_hugetab",
  "m2_arena_sweep",
  "m2_arena_state",
  "m2_arena_gcmark",
  "m2_arena_gcverify",
  "m2_arena_gcclose",
  "m2_arena_gcsweep",
  "m2_arena_gcphase"
}

local function arena_sources(t, cfile)
  return {
    t:path("tests", cfile),
    t:path("src", "lj_arena.c"),
    t:path("src", "lj_prng.c")
  }
end

local function run_standalone_fixture(t, out, cfile)
  t:cc(out, arena_sources(t, cfile))
  t:run({ out })
end

local function run_luajit_fixture(t, out, cfile, opts)
  opts = opts or {}
  t:build({ clean = true, quiet = true, xcflags = opts.xcflags })
  t:cc(out, { t:path("tests", cfile) }, {
    cflags = opts.cflags,
    link_luajit = true,
    libs = { "-lm", "-ldl" }
  })
  t:run({ out })
end

local function gcsweep_markers()
  return {
    "gc_arena_sweep_pending(global_State *g)",
    "lj_gc2_sweep_tg_ready(TGState *tg)",
    "lj_gc2_sweep_pending(global_State *g)",
    "gc_arena_finish_sweep_boundary(global_State *g, int drain)",
    "la_loadptr_acq((void *const *)&g->gc2.tg_list)",
    "lj_gc2_worker_drain(g, LJ_GC2_SWEEP_BATCH)",
    "lj_gc2_sweep_to_idle(g)",
    "minor = la_load32_acq(&g->gc2.cycle_sweep_minor) != 0",
    "la_add64_rlx(&g->gc2.minor_sweep_arenas, n)",
    "test_minor_sweep_identity_direct",
    "assert(ptr_state(live) == 3)",
    "05 section 5.6.3 worker-owned sweep bridge",
    "assert(lj_gc2_worker_drain(g, 1) == 1u)",
    "assert(lj_gc2_worker_drain(g, 1) == 0)",
    "worker_runs0 = la_load64_acq(&g->gc2.worker_runs)",
    "lj_arena_alloc_restore_sweep_kind(&extra_tg.alloc, LJ_ARENAK_PLAIN)",
    "lj_gc2_handshake(g, LJ_GC2_HS_RESET_ALLOC)",
    "seed_traversable_needsweep(&extra_tg, seeded)",
    "assert(g->gc.state == GCSsweep)",
    "assert(extra_tg.alloc.needsweep[LJ_ARENAK_TRAVERSABLE] != NULL)",
    "arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_PLAIN]",
    "arena_list_contains(extra_tg.alloc.owned[LJ_ARENAK_TRAVERSABLE]",
    "assert(extra_trav_a->hdr.sweep_epoch == sweep_cycle)",
    "assert(la_load64_acq(&g->gc2.sweep_to_idle) == sweep_to_idle0)",
    "assert(delta <= LJ_GC2_SWEEP_BATCH)"
  }
end

return function(add)
  local cases = {}

  local function register(test)
    cases[test.name] = test
    add(test)
  end

  register({
    name = "m2_arena_bitmap",
    description = "arena bitmap scaffold C fixture",
    run = function(t)
      run_standalone_fixture(t, t:tmp("lj_t_arena_bitmap"),
                             "t-arena-bitmap.c")
    end
  })

  register({
    name = "m2_arena_map",
    description = "arena mmap and huge-object scaffold C fixtures",
    run = function(t)
      local out = t:tmp("lj_t_arena_map")
      run_standalone_fixture(t, out .. ".map", "t-arena-map.c")
      run_standalone_fixture(t, out .. ".huge", "t-arena-huge.c")
    end
  })

  register({
    name = "m2_arena_alloc",
    description = "arena allocation, reallocation, and allocf C fixtures",
    run = function(t)
      local out = t:tmp("lj_t_arena_alloc")
      run_standalone_fixture(t, out .. ".alloc", "t-arena-alloc.c")
      run_standalone_fixture(t, out .. ".realloc", "t-arena-realloc.c")
      run_standalone_fixture(t, out .. ".allocf", "t-arena-allocf.c")
    end
  })

  register({
    name = "m2_arena_hugetab",
    description = "huge-object side-table scaffold C fixture",
    run = function(t)
      run_standalone_fixture(t, t:tmp("lj_t_arena_hugetab"),
                             "t-arena-hugetab.c")
    end
  })

  register({
    name = "m2_arena_sweep",
    description = "owner-local arena sweep scaffold C fixture",
    run = function(t)
      run_standalone_fixture(t, t:tmp("lj_t_arena_sweep"),
                             "t-arena-sweep.c")
    end
  })

  register({
    name = "m2_arena_state",
    description = "arena-backed lua_State lifecycle C fixture",
    run = function(t)
      run_luajit_fixture(t, t:tmp("lj_t_arena_state"),
                         "t-arena-state.c")
    end
  })

  register({
    name = "m2_arena_gcmark",
    description = "arena metadata mark mirror C fixture",
    run = function(t)
      run_luajit_fixture(t, t:tmp("lj_t_arena_gcmark"),
                         "t-arena-gcmark.c")
    end
  })

  register({
    name = "m2_arena_gcverify",
    description = "arena GC metadata verifier path under assertions",
    run = function(t)
      run_luajit_fixture(t, t:tmp("lj_t_arena_gcverify"),
                         "t-arena-gcmark.c", {
        xcflags = "-DLUA_USE_ASSERT",
        cflags = "-DLUA_USE_ASSERT"
      })
    end
  })

  register({
    name = "m2_arena_gcclose",
    description = "lua_close proto/closure churn under assertions",
    run = function(t)
      run_luajit_fixture(t, t:tmp("lj_t_arena_gcclose"),
                         "t-arena-gcclose.c", {
        xcflags = "-DLUA_USE_ASSERT",
        cflags = "-DLUA_USE_ASSERT"
      })
    end
  })

  register({
    name = "m2_arena_gcsweep",
    description = "runtime traversable arena sweep bridge C fixture and guards",
    run = function(t)
      run_luajit_fixture(t, t:tmp("lj_t_arena_gcsweep"),
                         "t-arena-gcsweep.c")
      t:assert_all_any_contains({
        t:path("src", "lj_gc.c"),
        t:path("src", "lj_gc2.c"),
        t:path("tests", "t-arena-gcsweep.c")
      }, gcsweep_markers())
    end
  })

  register({
    name = "m2_arena_gcphase",
    description = "arena allocation-color GC phase C fixture",
    run = function(t)
      run_luajit_fixture(t, t:tmp("lj_t_arena_gcphase"),
                         "t-arena-gcphase.c")
    end
  })

  add({
    name = "m2_arena_all",
    description = "all focused M2 arena scaffold tests",
    run = function(t)
      for i = 1, #M2_ORDER do
        local name = M2_ORDER[i]
        io.stderr:write("== " .. name .. " ==\n")
        cases[name].run(t)
        io.stderr:write("ok " .. name .. "\n")
      end
      print("M2 arena focused tests passed")
    end
  })
end
