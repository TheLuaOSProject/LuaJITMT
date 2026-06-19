return function(add)
  add({
    name = "m2_arena_alloc",
    description = "arena allocation, reallocation, and allocf C fixtures",
    run = function(t)
      local src = t:path("src")
      local tests = t:path("tests")
      local common = {
        t:path("src", "lj_arena.c"),
        t:path("src", "lj_prng.c")
      }

      t:cc(t:tmp("lj_t_arena_alloc.alloc"), {
        t:path("tests", "t-arena-alloc.c"),
        common[1],
        common[2]
      })
      t:run({ t:tmp("lj_t_arena_alloc.alloc") })

      t:cc(t:tmp("lj_t_arena_alloc.realloc"), {
        tests .. "/t-arena-realloc.c",
        src .. "/lj_arena.c",
        src .. "/lj_prng.c"
      })
      t:run({ t:tmp("lj_t_arena_alloc.realloc") })

      t:cc(t:tmp("lj_t_arena_alloc.allocf"), {
        tests .. "/t-arena-allocf.c",
        src .. "/lj_arena.c",
        src .. "/lj_prng.c"
      })
      t:run({ t:tmp("lj_t_arena_alloc.allocf") })
    end
  })
end
