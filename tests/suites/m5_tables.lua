local build = require("suite_build")
local runtime = require("suite_runtime")

return function(add)
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

  runtime.add_luajit_script_cases(add, {
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
