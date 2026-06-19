return function(add)
  add({
    name = "m5_math_random_tg",
    description = "per-TG math.random regression test",
    run = function(t)
      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", t:path("tests", "t-math-random-tg.lua") })
    end
  })

  add({
    name = "m5_parser_capture_meta",
    description = "parser captured-local metadata and source cell emission guard",
    run = function(t)
      local parse = t:path("src", "lj_parse.c")
      t:build({ clean = true, quiet = true, xcflags = "-DLUA_USE_ASSERT" })
      t:luajit({ t:path("tests", "t-parser-capture-meta.lua") })

      for _, needle in ipairs({
        "VSTACK_VAR_CAPTURED",
        "var_mark_captured(fs, reg)",
        "unmarked captured local",
        "BC_CGET",
        "BC_CSET",
        "One-pass capture discovery can happen after earlier loop bytecode",
        "CSET stores raw slots unchanged and updates cells after FNEW promotion"
      }) do
        t:assert_contains(parse, needle)
      end

      t:assert_not_match(parse, "#if%s+LJ_MT", "#if LJ_MT")
      t:assert_not_match(parse, "#ifdef%s+LJ_MT", "#ifdef LJ_MT")
      t:assert_not_contains(parse, "LUAJIT_THREADSAFE")
    end
  })
end
