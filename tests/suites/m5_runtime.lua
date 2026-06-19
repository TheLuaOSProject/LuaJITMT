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
    name = "m5_os_reentrant",
    description = "POSIX os.date/tmpname reentrancy and setlocale guard",
    run = function(t)
      local lib_os = t:path("src", "lib_os.c")
      local date_block = t:text_between(lib_os, "LJLIB_CF(os_date)",
                                        "LJLIB_CF(os_time)")
      t:assert_text_contains("os_date", date_block, "gmtime_r")
      t:assert_text_contains("os_date", date_block, "localtime_r")

      local tmpname_block = t:c_block(lib_os, "static int os_native_mkstemp")
      t:assert_text_contains("os_native_mkstemp", tmpname_block, "mkstemp")

      local setlocale_block = t:text_between(lib_os, "LJLIB_CF(os_setlocale)",
                                             "#include \"lj_libdef.h\"")
      t:assert_text_contains("os_setlocale", setlocale_block,
                             "la_load32_acq(&G(L)->mt_active)")
      t:assert_text_contains("os_setlocale", setlocale_block,
                             "os.setlocale mutation disabled after threading activation")
      t:assert_text_contains("os_setlocale", setlocale_block,
                             "setlocale(opt, str)")

      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", t:path("tests", "t-os-reentrant.lua") }, {
        env = {
          LJ_M5_OS_THREADS = os.getenv("LJ_M5_OS_THREADS") or "8",
          LJ_M5_OS_ITERS = os.getenv("LJ_M5_OS_ITERS") or "200"
        }
      })
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
