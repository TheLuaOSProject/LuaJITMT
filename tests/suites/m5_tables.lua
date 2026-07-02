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
      name = "m5_tab_keylock_lookup",
      description = "table KEYLOCK lookup filtering C fixture",
      output = "lj_t-tab-keylock-lookup",
      cfile = "t-tab-keylock-lookup.c",
      opts = { timeout = "20s" },
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
      opts = { timeout = "20s" },
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
      opts = { timeout = "20s" },
      message = "M5 table hash-vector retirement tests passed"
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
                            "t-tab-struct-owner.c", {
        clean = true,
        cflags = "-DLJ_TAB_TEST_HELPERS",
        timeout = "20s",
        xcflags = "-DLJ_TAB_TEST_HELPERS"
      })
      print("M5 per-table structural owner tests passed")
    end
  })

  add({
    name = "m5_tab_finreg_newkey_stale",
    description = "FINREG new-key helpers abandon stale table generations",
    run = function(t)
      t:build({
        clean = true,
        jobs = false,
        quiet = true,
        xcflags = "-DLJ_TAB_TEST_HELPERS"
      })
      build.compile_and_run_c(t, t:tmp("lj_t-tab-finreg-newkey-stale"),
                              "t-tab-finreg-newkey-stale.c", {
        cflags = "-DLJ_TAB_TEST_HELPERS",
        timeout = "20s"
      })
      print("M5 FINREG new-key stale-generation guard passed")
    end
  })

  add({
    name = "m5_tab_next_snapshot",
    description = "table next cursor scans stay generation-bound across resize",
    run = function(t)
      t:build({
        clean = true,
        jobs = false,
        quiet = true,
        xcflags = "-DLJ_TAB_TEST_HELPERS"
      })
      build.compile_and_run_c(t, t:tmp("lj_t-tab-next-snapshot"),
                              "t-tab-next-snapshot.c", {
        cflags = "-DLJ_TAB_TEST_HELPERS",
        timeout = "20s"
      })
      print("M5 table next snapshot guard passed")
    end
  })

  runtime.add_luajit_script_cases(add, {
    {
      name = "m5_tab_resize_stress",
      description = "table resize forwarding stress across GC, traversal, length, weak clear, metatables, metamethod dispatch, table-library shifts, and JIT reads/stores/iterators",
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
	  LJ_M5_TAB_RESIZE_STRESS_CASES =
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_CASES") or "",
	  LJ_M5_TAB_RESIZE_TRAVERSAL_MODES =
	    os.getenv("LJ_M5_TAB_RESIZE_TRAVERSAL_MODES") or ""
	}
      },
      message = "M5 table resize forwarding stress passed"
    }
  })
end
