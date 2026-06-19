return function(add)
  add({
    name = "m5_tab_emptyhash",
    description = "empty-hash table insertion C fixture and source guard",
    run = function(t)
      local lj_tab = t:path("src", "lj_tab.c")

      t:build({ clean = true, quiet = true })
      t:cc(t:tmp("lj_t-tab-emptyhash"), {
        t:path("tests", "t-tab-emptyhash.c")
      }, {
        link_luajit = true,
        libs = { "-lm", "-ldl", "-pthread" }
      })
      t:run({ t:tmp("lj_t-tab-emptyhash") })

      t:assert_not_contains(lj_tab, "|| hmask == 0")
      t:assert_ordered(lj_tab, {
        "TValue *lj_tab_newkey",
        "nodebase = lj_tab_node_snapshot_acq(t, &hmask)",
        "if (hmask == 0)",
        "hashkey_node(nodebase, hmask, key)"
      })
      t:assert_contains(lj_tab, "nodebase != &G(L)->nilnode")
    end
  })
end
