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
    }
  })

  runtime.add_luajit_script_cases(add, {
    {
      name = "m5_tab_resize_stress",
      description = "table resize forwarding stress across GC, weak clear, and JIT stores",
      script = "t-tab-resize-stress.lua",
      opts = {
	timeout = "30s",
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
	    os.getenv("LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS") or "192"
	}
      },
      message = "M5 table resize forwarding stress passed"
    }
  })
end
