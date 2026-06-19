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
  opts.pthread = false
  t:run_luajit_c_fixture(out, cfile, opts)
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
    description = "runtime traversable arena sweep bridge C fixture",
    run = function(t)
      run_luajit_fixture(t, t:tmp("lj_t_arena_gcsweep"),
                         "t-arena-gcsweep.c")
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
