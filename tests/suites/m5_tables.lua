local build = require("suite_build")
local runtime = require("suite_runtime")
local resize_desc_flags =
  "-DLJ_TAB_TEST_HELPERS " .. build.gc2_test_helper_flag

return function(add)
  add({
    name = "m5_tab_scalar_next",
    description = "scalar array iteration, source authority and lifetime with a paused reclaimer",
    run = function(t)
      if jit.os ~= "Linux" or jit.arch ~= "x64" then
        print("M5 scalar-next reclaimer fixtures require Linux x64")
        return
      end
      local flags = build.gc2_test_helper_flag ..
        " -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLUA_USE_ASSERT -DLUA_USE_APICHECK"
      build.with_default_build_restore(t, function()
        build.clean_build(t, { quiet = true, xcflags = flags })
        local function run(name, modes)
          local out = t:tmp("lj_" .. name)
          t:cc(out, { t:path("tests", name .. ".c") }, {
            cflags = flags, link_luajit = true,
            libs = { "-lm", "-ldl", os.getenv("PTHREAD") or "-pthread" }
          })
          for _, mode in ipairs(modes) do
            local argv = { out }
            for _, value in ipairs(mode) do argv[#argv + 1] = value end
            t:run(argv, { timeout = "20s", env = {
              LUA_PATH = t:path("src", "?.lua") .. ";;"
            } })
          end
        end
        local progress = {}
        for _, mode in ipairs({ "next", "itern", "rooted", "cursor" }) do
          for _, shape in ipairs({ "dense", "sparse", "empty", "holes", "zero", "bool" }) do
            progress[#progress + 1] = { mode, shape }
          end
        end
        run("t-tab-scalar-next-progress", progress)
        local authority = {}
        for _, mode in ipairs({ "basic-colo", "basic-separate", "keys", "aliases",
          "opaque", "protected", "bounds", "hooks-found", "hooks-end", "resize", "plain" }) do
          authority[#authority + 1] = { mode }
        end
        run("t-tab-scalar-next-authority", authority)
        local retry = {}
        for _, mode in ipairs({ "found", "end" }) do
          for _, slots in ipairs({ { "2", "3" }, { "1", "3" }, { "0", "1" } }) do
            retry[#retry + 1] = { mode, slots[1], slots[2] }
          end
        end
        run("t-tab-scalar-next-stack-retry", retry)
        run("t-tab-scalar-next-lifetime", { { "preflight" }, { "retired" } })
        run("t-jit-idle-reclaim-entry", { {} })
      end)
      print("M5 scalar array-next tests passed")
    end
  })
  add({
    name = "m5_tab_scalar_hit",
    description = "Linux scalar table hits with a paused IDLE reclaimer",
    run = function(t)
      if jit.os ~= "Linux" then
        print("M5 scalar-hit reclaimer fixture requires Linux")
        return
      end
      local flags = build.gc2_test_helper_flag ..
        " -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLUA_USE_ASSERT"
      build.with_default_build_restore(t, function()
        build.clean_build(t, { quiet = true, xcflags = flags })
        build.build_and_run_c(t, t:tmp("lj_t-tab-scalar-hit"),
          "t-tab-scalar-hit.c", { build = false, cflags = flags, timeout = "45s" })
      end)
      print("M5 scalar table-hit tests passed")
    end
  })
  runtime.add_luajit_c_fixture_cases(add, {
    {
      name = "m5_tab_emptyhash",
      description = "empty-hash table insertion C fixture",
      output = "lj_t-tab-emptyhash",
      cfile = "t-tab-emptyhash.c"
    },
    {
      name = "m5_tab_slot_snapshot",
      description = "table hash-node TValue snapshot C fixture",
      output = "lj_t-tab-slot-snapshot",
      cfile = "t-tab-slot-snapshot.c",
      opts = { timeout = "20s" },
      message = "M5 table hash-node TValue snapshot tests passed"
    },
    {
      name = "m5_tab_rooted_reader",
      description = "authoritative-root table reader parent/key/result lifetime fixture",
      output = "lj_t-tab-rooted-reader",
      cfile = "t-tab-rooted-reader.c",
      opts = build.gc2_test_helper_opts({
        timeout = "20s",
        xcflags = build.gc2_test_helper_flag .. " -DLJ_TAB_TEST_HELPERS",
        cflags = build.gc2_test_helper_flag .. " -DLJ_TAB_TEST_HELPERS"
      }),
      message = "M5 authoritative-root table reader tests passed"
    },
    {
      name = "m5_tab_rooted_get_try",
      description = "bounded tri-state authoritative-root table point reads",
      output = "lj_t-tab-rooted-get-try",
      cfile = "t-tab-rooted-get-try.c",
      opts = build.gc2_test_helper_opts({
        timeout = "20s",
        xcflags = build.gc2_test_helper_flag .. " -DLJ_TAB_TEST_HELPERS",
        cflags = build.gc2_test_helper_flag .. " -DLJ_TAB_TEST_HELPERS"
      }),
      message = "M5 bounded rooted table point-read tests passed"
    },
    {
      name = "m5_tab_rooted_len_try",
      description = "bounded authoritative-root table length reads",
      output = "lj_t-tab-rooted-len-try",
      cfile = "t-tab-rooted-len-try.c",
      opts = build.gc2_test_helper_opts({
        timeout = "20s",
        xcflags = build.gc2_test_helper_flag .. " -DLJ_TAB_TEST_HELPERS",
        cflags = build.gc2_test_helper_flag .. " -DLJ_TAB_TEST_HELPERS"
      }),
      message = "M5 bounded rooted table length tests passed"
    },
    {
      name = "m5_meta_rooted_chain",
      description = "rooted __index/__newindex chains and explicit C result ABI",
      output = "lj_t-meta-rooted-chain",
      cfile = "t-meta-rooted-chain.c",
      opts = build.gc2_test_helper_opts({
        timeout = "20s",
        xcflags = build.gc2_test_helper_flag ..
                  " -DLJ_TG_ROOT_TEST_HELPERS",
        cflags = build.gc2_test_helper_flag ..
                 " -DLJ_TG_ROOT_TEST_HELPERS"
      }),
      message = "M5 rooted metamethod-chain tests passed"
    },
    {
      name = "m5_x64_rooted_reads",
      description = "x64 rawget/TGETR rooted VM reads and STOPREQ unwind fixture",
      output = "lj_t-x64-rooted-reads",
      cfile = "t-x64-rooted-reads.c",
      opts = build.gc2_test_helper_opts({
        timeout = "20s",
        xcflags = "-Werror " .. build.gc2_test_helper_flag ..
                  " -DLJ_TG_ROOT_TEST_HELPERS",
        cflags = build.gc2_test_helper_flag ..
                 " -DLJ_TG_ROOT_TEST_HELPERS"
      }),
      message = "M5 x64 rooted VM point-read tests passed"
    },
    {
      name = "m5_tab_keylock_lookup",
      description = "table KEYLOCK lookup filtering C fixture",
      output = "lj_t-tab-keylock-lookup",
      cfile = "t-tab-keylock-lookup.c",
      opts = build.tab_helper_opts({ timeout = "20s" }),
      message = "M5 table KEYLOCK lookup filtering tests passed"
    },
    {
      name = "m5_tab_newkey_monotonic",
      description = "owner-only monotonic shared table new-key publication",
      output = "lj_t-tab-newkey-monotonic",
      cfile = "t-tab-newkey-monotonic.c",
      opts = build.tab_helper_opts({ timeout = "20s" }),
      message = "M5 monotonic shared table new-key tests passed"
    },
    {
      name = "m5_tab_chain_order",
      description = "stable table-node/hash-chain ordering C fixture",
      output = "lj_t-tab-chain-order",
      cfile = "t-tab-chain-order.c",
      opts = { timeout = "20s" },
      message = "M5 stable table-node/hash-chain ordering tests passed"
    },
    {
      name = "m5_tab_node_publish",
      description = "table hash-vector publication C fixture",
      output = "lj_t-tab-node-publish",
      cfile = "t-tab-node-publish.c",
      opts = build.gc2_test_helper_opts({ timeout = "20s" }),
      message = "M5 table hash-vector publication tests passed"
    },
    {
      name = "m5_tab_nodehdr",
      description = "table hash-vector header C fixture",
      output = "lj_t-tab-nodehdr",
      cfile = "t-tab-nodehdr.c",
      opts = { timeout = "20s" },
      message = "M5 table hash-vector header tests passed"
    },
    {
      name = "m5_tab_forward_filter",
      description = "table FORWARD value filtering C fixture",
      output = "lj_t-tab-forward-filter",
      cfile = "t-tab-forward-filter.c",
      opts = { timeout = "20s" },
      message = "M5 table FORWARD filtering tests passed"
    },
    {
      name = "m5_tab_retire",
      description = "table hash-vector retirement C fixture",
      output = "lj_t-tab-retire",
      cfile = "t-tab-retire.c",
      opts = build.tab_helper_opts({ timeout = "20s" }),
      message = "M5 table hash-vector retirement tests passed"
    },
    {
      name = "m5_tab_resize_copy_helper",
      description = "table resize copy helper idempotence C fixture",
      output = "lj_t-tab-resize-copy-helper",
      cfile = "t-tab-resize-copy-helper.c",
      opts = build.tab_helper_opts({ timeout = "20s" }),
      message = "M5 table resize copy helper tests passed"
    },
    {
      name = "m5_tab_resize_descriptor",
      description = "persistent table resize descriptor identity fixture",
      output = "lj_t-tab-resize-descriptor",
      cfile = "t-tab-resize-descriptor.c",
      opts = build.tab_helper_opts({
        timeout = "20s",
        xcflags = resize_desc_flags,
        cflags = resize_desc_flags
      }),
      message = "M5 persistent table resize descriptor substrate passed"
    },
    {
      name = "m5_tab_capi_resize_stress",
      description = "public C API table setter stress across concurrent resize",
      output = "lj_t-tab-capi-resize-stress",
      cfile = "t-tab-capi-resize-stress.c",
      opts = { timeout = "20s" },
      message = "M5 public C API table resize stress passed"
    }
  })

  add({
    name = "m5_tab_struct_owner",
    description = "table structural ownership is per-table",
    run = function(t)
      build.build_and_run_c(t, t:tmp("lj_t-tab-struct-owner"),
                            "t-tab-struct-owner.c",
                            build.tab_helper_opts({ timeout = "20s" }))
      print("M5 per-table structural owner tests passed")
    end
  })

  add({
    name = "m5_tab_clear_entering",
    description = "table.clear uses shared path during mt_entering",
    run = function(t)
      build.build_and_run_c(t, t:tmp("lj_t-tab-clear-entering"),
                            "t-tab-clear-entering.c",
                            build.tab_helper_opts({ timeout = "20s" }))
      print("M5 table.clear mt_entering route passed")
    end
  })

  add({
    name = "m5_table_insert_entering",
    description = "table.insert owns structure during mt_entering",
    run = function(t)
      build.build_and_run_c(t, t:tmp("lj_t-table-insert-entering"),
                            "t-table-insert-entering.c",
                            build.tab_helper_opts({ timeout = "20s" }))
      print("M5 table.insert mt_entering route passed")
    end
  })

  add({
    name = "m5_tab_finreg_newkey_stale",
    description = "FINREG new-key helpers abandon stale table generations",
    run = function(t)
      t:build(build.tab_helper_build_opts({ jobs = false, quiet = true }))
      build.compile_and_run_c(t, t:tmp("lj_t-tab-finreg-newkey-stale"),
                              "t-tab-finreg-newkey-stale.c",
                              build.tab_helper_c_opts({ timeout = "20s" }))
      print("M5 FINREG new-key stale-generation behavior passed")
    end
  })

  add({
    name = "m5_tab_next_snapshot",
    description = "table next cursor scans stay generation-bound across resize",
    run = function(t)
      t:build(build.tab_helper_build_opts({ jobs = false, quiet = true }))
      build.compile_and_run_c(t, t:tmp("lj_t-tab-next-snapshot"),
                              "t-tab-next-snapshot.c",
                              build.tab_helper_c_opts({ timeout = "20s" }))
      print("M5 table next snapshot behavior passed")
    end
  })

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

  runtime.add_luajit_script_cases(add, {
    {
      name = "m5_meta_cdata_capture_joff",
      description = "cdata method lifetime across collection, replacement and callbacks",
      script = "t-meta-cdata-capture.lua",
      opts = { joff = true, timeout = "30s" }
    },
    {
      name = "m5_meta_cdata_capture",
      description = "cdata method lifetime with JIT enabled",
      script = "t-meta-cdata-capture.lua",
      opts = { timeout = "30s" }
    },
    {
      name = "m5_tab_rooted_readers",
      description = "unpack and table.concat retain protected results across resize and GC",
      script = "t-tab-rooted-readers.lua",
      opts = {
	timeout = os.getenv("LJ_M5_TAB_ROOTED_READER_TIMEOUT") or "40s",
	env = {
	  LJ_M5_TAB_ROOTED_READER_ROUNDS =
	    os.getenv("LJ_M5_TAB_ROOTED_READER_ROUNDS") or "2400"
	}
      },
      message = "M5 rooted unpack/table.concat stress passed"
    },
    {
      name = "m5_gc2_weakmeta_bridge",
      description = "GC2-discovered weak metatable chains survive bridge sweep",
      script = "t-gc2-weakmeta-bridge.lua",
      opts = { timeout = "20s" },
      message = "M5 GC2 weak metatable bridge passed"
    },
    {
      name = "m5_tab_resize_stress",
      description = "table resize forwarding stress across GC, traversal, length, weak clear, metatables, metamethod dispatch, and table-library shifts",
      script = "t-tab-resize-stress.lua",
      opts = {
	timeout = os.getenv("LJ_M5_TAB_RESIZE_STRESS_TIMEOUT") or "30s",
	env = {
	  LJ_M5_TAB_RESIZE_STRESS_REPS =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_REPS") or "768",
	  LJ_M5_TAB_RESIZE_STRESS_THREADS =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_THREADS") or "3",
	  LJ_M5_TAB_RESIZE_STRESS_JIT_REPS =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_JIT_REPS") or "2200",
	  LJ_M5_TAB_RESIZE_STRESS_JIT_READ_REPS =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_JIT_READ_REPS") or "2200",
	  LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS") or "192",
	  LJ_M5_TAB_RESIZE_STRESS_FIN_OBJECTS =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_FIN_OBJECTS") or "192",
	  LJ_M5_TAB_RESIZE_STRESS_KEY_OBJECTS =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_KEY_OBJECTS") or "192",
	  LJ_M5_TAB_RESIZE_STRESS_GCWORKERS =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_GCWORKERS") or "2",
	  LJ_M5_TAB_RESIZE_STRESS_CASES =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_CASES") or
	    "weak,gcmark,gckey,weakkey,weakmeta,finalizer,metatable," ..
	    "len,traversal,nextchurn,nextinvalid,tableclear,tablelib," ..
	    "tablelibshift,metadispatch",
	  LJ_M5_TAB_RESIZE_TRAVERSAL_MODES =
	    os.getenv("LJ_M5_TAB_RESIZE_TRAVERSAL_MODES") or ""
	}
      },
      message = "M5 table resize forwarding stress passed"
    },
    {
      name = "m5_tab_resize_jit_stress",
      description = "traced table resize reads, stores, and iterator observers publish real JIT traces",
      script = "t-tab-resize-stress.lua",
      opts = {
	timeout = os.getenv("LJ_M5_TAB_RESIZE_JIT_TIMEOUT") or "30s",
	env = {
	  LJ_M5_TAB_RESIZE_STRESS_REPS =
	    os.getenv("LJ_M5_TAB_RESIZE_JIT_REPS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_REPS") or "768",
	  LJ_M5_TAB_RESIZE_STRESS_THREADS =
	    os.getenv("LJ_M5_TAB_RESIZE_JIT_THREADS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_THREADS") or "3",
	  LJ_M5_TAB_RESIZE_STRESS_JIT_REPS =
	    os.getenv("LJ_M5_TAB_RESIZE_JIT_STORE_REPS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_JIT_REPS") or "2200",
	  LJ_M5_TAB_RESIZE_STRESS_JIT_READ_REPS =
	    os.getenv("LJ_M5_TAB_RESIZE_JIT_READ_REPS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_JIT_READ_REPS") or "2200",
	  LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS =
	    os.getenv("LJ_M5_TAB_RESIZE_JIT_TRAVERSAL_ROUNDS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS") or "192",
	  LJ_M5_TAB_RESIZE_STRESS_FIN_OBJECTS =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_FIN_OBJECTS") or "192",
	  LJ_M5_TAB_RESIZE_STRESS_KEY_OBJECTS =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_KEY_OBJECTS") or "192",
	  LJ_M5_TAB_RESIZE_STRESS_GCWORKERS =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_GCWORKERS") or "2",
	  LJ_M5_TAB_RESIZE_STRESS_CASES = "jitstore,jitread,jititer"
	}
      },
      message = "M5 traced table resize stress passed"
    },
    {
      name = "m5_tab_resize_weakfinjit_stress",
      description = "weak-key finalizer values survive resize-reader stress under GC2 workers",
      script = "t-tab-resize-stress.lua",
      opts = {
	timeout = os.getenv("LJ_M5_TAB_RESIZE_WEAKFINJIT_TIMEOUT") or "20s",
	env = {
	  LJ_M5_TAB_RESIZE_STRESS_REPS =
	    os.getenv("LJ_M5_TAB_RESIZE_WEAKFINJIT_REPS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_REPS") or "768",
	  LJ_M5_TAB_RESIZE_STRESS_THREADS =
	    os.getenv("LJ_M5_TAB_RESIZE_WEAKFINJIT_THREADS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_THREADS") or "3",
	  LJ_M5_TAB_RESIZE_STRESS_JIT_READ_REPS =
	    os.getenv("LJ_M5_TAB_RESIZE_WEAKFINJIT_JIT_READ_REPS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_JIT_READ_REPS") or "2200",
	  LJ_M5_TAB_RESIZE_STRESS_FIN_OBJECTS =
	    os.getenv("LJ_M5_TAB_RESIZE_WEAKFINJIT_FIN_OBJECTS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_FIN_OBJECTS") or "192",
	  LJ_M5_TAB_RESIZE_STRESS_KEY_OBJECTS =
	    os.getenv("LJ_M5_TAB_RESIZE_WEAKFINJIT_KEY_OBJECTS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_KEY_OBJECTS") or "192",
	  LJ_M5_TAB_RESIZE_STRESS_GCWORKERS =
	    os.getenv("LJ_M5_TAB_RESIZE_WEAKFINJIT_GCWORKERS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_GCWORKERS") or "2",
	  LJ_M5_TAB_RESIZE_STRESS_CASES = "weakfinjit"
	}
      },
      message = "M5 weak-key finalizer JIT resize stress passed"
    },
    {
      name = "m5_tab_resize_remote_stack_gc",
      description = "GC root scans preserve remote worker stack roots during traced table resize",
      script = "t-tab-resize-stress.lua",
      opts = {
	timeout = os.getenv("LJ_M5_TAB_RESIZE_REMOTE_STACK_TIMEOUT") or "20s",
	env = {
	  LJ_M5_TAB_RESIZE_STRESS_REPS =
	    os.getenv("LJ_M5_TAB_RESIZE_REMOTE_STACK_REPS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_REPS") or "768",
	  LJ_M5_TAB_RESIZE_STRESS_THREADS =
	    os.getenv("LJ_M5_TAB_RESIZE_REMOTE_STACK_THREADS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_THREADS") or "3",
	  LJ_M5_TAB_RESIZE_REMOTE_STACK_GC_ROUNDS =
	    os.getenv("LJ_M5_TAB_RESIZE_REMOTE_STACK_GC_ROUNDS") or "8",
	  LJ_M5_TAB_RESIZE_REMOTE_STACK_JIT_REPS =
	    os.getenv("LJ_M5_TAB_RESIZE_REMOTE_STACK_JIT_REPS") or "96",
	  LJ_M5_TAB_RESIZE_STRESS_GCWORKERS =
	    os.getenv("LJ_M5_TAB_RESIZE_REMOTE_STACK_GCWORKERS") or
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_GCWORKERS") or "2",
	  LJ_M5_TAB_RESIZE_STRESS_CASES = "remotejitgc"
	}
      },
      message = "M5 remote-stack GC table resize stress passed"
    }
  })
end
