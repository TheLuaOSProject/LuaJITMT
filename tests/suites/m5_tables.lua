return function(add)
  add({
    name = "m5_tab_emptyhash",
    description = "empty-hash table insertion C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-tab-emptyhash"),
                              "t-tab-emptyhash.c")
    end
  })

  add({
    name = "m5_tab_slot_snapshot",
    description = "table hash-node TValue snapshot C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-tab-slot-snapshot"),
                              "t-tab-slot-snapshot.c", { timeout = "20s" })
      print("M5 table hash-node TValue snapshot tests passed")
    end
  })

  add({
    name = "m5_tab_keylock_lookup",
    description = "table KEYLOCK lookup filtering C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-tab-keylock-lookup"),
                              "t-tab-keylock-lookup.c", { timeout = "20s" })
      print("M5 table KEYLOCK lookup filtering tests passed")
    end
  })

  add({
    name = "m5_tab_chain_order",
    description = "stable table-node/hash-chain ordering C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-tab-chain-order"),
                              "t-tab-chain-order.c", { timeout = "20s" })
      print("M5 stable table-node/hash-chain ordering tests passed")
    end
  })

  add({
    name = "m5_tab_node_publish",
    description = "table hash-vector publication C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-tab-node-publish"),
                              "t-tab-node-publish.c", { timeout = "20s" })
      print("M5 table hash-vector publication tests passed")
    end
  })

  add({
    name = "m5_tab_nodehdr",
    description = "table hash-vector header C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-tab-nodehdr"),
                              "t-tab-nodehdr.c", { timeout = "20s" })
      print("M5 table hash-vector header tests passed")
    end
  })

  add({
    name = "m5_tab_forward_filter",
    description = "table FORWARD value filtering C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-tab-forward-filter"),
                              "t-tab-forward-filter.c", { timeout = "20s" })
      print("M5 table FORWARD filtering tests passed")
    end
  })

  add({
    name = "m5_tab_retire",
    description = "table hash-vector retirement C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-tab-retire"),
                              "t-tab-retire.c", { timeout = "20s" })
      print("M5 table hash-vector retirement tests passed")
    end
  })
end
