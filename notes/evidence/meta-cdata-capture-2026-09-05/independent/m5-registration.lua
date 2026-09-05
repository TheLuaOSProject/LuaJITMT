  add({
    name = "m5_meta_cdata_capture_protocol",
    description = "cdata method capture replacement, refusal and queue pressure",
    run = function(t)
      if jit.os ~= "Linux" or jit.arch ~= "x64" then
        print("M5 cdata method capture protocol fixture requires Linux x64")
        return
      end
      local flags = "-DLJ_FUNC_TEST_HELPERS -DLJ_GC2_TEST_HELPERS" ..
        " -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS" ..
        " -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT"
      local modes = {
        "basic", "alias-source", "alias-key", "same-source-key", "set-alias",
        "retry-source", "retry-key", "retry-mt", "retry-method",
        "replace", "growth", "fail-growth", "throw"
      }
      build.with_default_build_restore(t, function()
        build.clean_build(t, { quiet = true, xcflags = flags })
        local out = t:tmp("lj_t-meta-cdata-capture")
        t:cc(out, { t:path("tests", "t-meta-cdata-capture.c") }, {
          default_cflags = false,
          cflags = "-D_GNU_SOURCE -std=gnu11 -O2 -g -Wall -Wextra -Werror" ..
                   " -mcx16 " .. flags,
          link_luajit = true,
          libs = {
            "-lm", "-ldl", "-pthread",
            "-Wl,--wrap=lj_gc2_tv_lease_acquire",
            "-Wl,--wrap=lj_gc2_lease_release",
            "-Wl,--wrap=lj_tab_wait_l",
            "-Wl,--wrap=lj_gc2_smr_read_enter",
            "-Wl,--wrap=lj_vm_call", "-Wl,--wrap=lj_vm_pcall",
            "-Wl,--wrap=lj_vm_cpcall", "-Wl,--wrap=lj_vm_resume"
          }
        })
        -- Global one-shot hooks start fresh in each independently bounded mode.
        for i = 1, #modes do
          t:run({ out, modes[i] }, { timeout = "15s" })
        end
      end)
      print("M5 cdata method capture protocol schedules passed")
    end
  })

