local function contains(s, needle)
  return s:find(needle, 1, true) ~= nil
end

local function count_plain(s, needle)
  local count, pos = 0, 1
  while true do
    local first, last = s:find(needle, pos, true)
    if not first then return count end
    count = count + 1
    pos = last + 1
  end
end

local function assert_no_lines(t, label, paths, pred)
  local hits = {}
  for i = 1, #paths do
    local path = paths[i]
    local n = 0
    for line in (t:read(path) .. "\n"):gmatch("(.-)\n") do
      n = n + 1
      if pred(line, path, n) then
        hits[#hits + 1] = path .. ":" .. n .. ": " .. line
      end
    end
  end
  if #hits > 0 then
    error(label .. ":\n" .. table.concat(hits, "\n"), 2)
  end
end

local function build_and_run_table_c(t, out, cfile)
  t:build({ clean = true, quiet = true })
  t:cc(out, { t:path("tests", cfile) }, {
    link_luajit = true,
    libs = { "-lm", "-ldl", "-pthread" }
  })
  t:run({ out }, { timeout = "20s" })
end

local function src_ch_files(t)
  return t:files(t:path("src"), { extensions = { ".c", ".h" } })
end

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

  add({
    name = "m5_tab_slot_snapshot",
    description = "table hash-node TValue snapshot C fixture and guards",
    run = function(t)
      build_and_run_table_c(t, t:tmp("lj_t-tab-slot-snapshot"),
                            "t-tab-slot-snapshot.c")

      t:assert_all_any_contains(src_ch_files(t), {
        "lj_tv_load_acq",
        "tv_rawload_acq(src)",
        "lj_tv_isnil_acq",
        "lj_tv_load_acq(&nk, &n->key)",
        "lj_tv_load_acq(&val, &n->val)",
        "lj_tv_load_acq(&key, &n->key)",
        "BCWriteHashSnap",
        "gc_marktv(g, &key)",
        "gc2_mark_tv_worker(g, &key)"
      })

      assert_no_lines(t, "table hash key/value decisions must use snapshots",
                      {
                        t:path("src", "lj_tab.c"),
                        t:path("src", "lib_table.c"),
                        t:path("src", "lj_bcwrite.c"),
                        t:path("src", "lj_serialize.c"),
                        t:path("src", "lj_gc.c"),
                        t:path("src", "lj_gc2.c"),
                        t:path("src", "lj_record.c"),
                        t:path("src", "lj_parse.c")
                      }, function(line)
        return contains(line, "lj_obj_equal(&n->key") or
               contains(line, "tvisnum(&n->key") or
               contains(line, "tvisstr(&n->key") or
               contains(line, "strV(&n->key") or
               contains(line, "numV(&n->key") or
               contains(line, "gcV(&n->key") or
               contains(line, "itype2irt(&n->key") or
               contains(line, "n->key.n") or
               contains(line, "tvisnil(&n->val") or
               contains(line, "tvisnil(&node->val") or
               (contains(line, "tvisnil(&hashnode[") and contains(line, ".val")) or
               (contains(line, "tvisnil(&node[") and contains(line, ".val")) or
               (contains(line, "numberVnum(&node[") and contains(line, "].key")) or
               contains(line, "serialize_put(w, sbx, &node->key") or
               contains(line, "bcwrite_ktabk(ctx, &node->key") or
               contains(line, "gc_marktv(g, &n->key") or
               contains(line, "gc2_mark_tv_worker(g, &n->key") or
               contains(line, "gc_mayclear(g, &n->key") or
               (contains(line, "copyTV(L, &tmp, &node[") and contains(line, "].val"))
      end)

      print("M5 table hash-node TValue snapshot tests passed")
    end
  })

  add({
    name = "m5_tab_keylock_lookup",
    description = "table KEYLOCK lookup filtering C fixture and guards",
    run = function(t)
      build_and_run_table_c(t, t:tmp("lj_t-tab-keylock-lookup"),
                            "t-tab-keylock-lookup.c")

      local files = src_ch_files(t)
      files[#files + 1] = t:path("tests", "t-tab-keylock-lookup.c")
      t:assert_all_any_contains(files, {
        "tab_key_islocked(cTValue *key)",
        "tab_key_retry_once(cTValue *key, int *retry)",
        "tab_try_claim_nil_key(TValue *dst)",
        "lj_tab_node_free_reserve(Node *node)",
        "lj_tab_node_free_release(Node *node)",
        "lj_tab_node_freecount_acq(const Node *node)",
        "tab_claim_free_node_scan(Node *nodebase, MSize hmask,",
        "tab_findkey_or_keylock(Node *anchor, cTValue *key, int *locked,",
        "tab_findkey_or_keylock(n, key, &locked, &chainlen)",
        "lj_tab_node_free_reserve(nodebase)",
        "tab_try_claim_nil_key(&n->key)",
        "tab_claim_free_node_scan(nodebase, hmask, n, &locked)",
        "tab_release_claimed_free(nodebase, freenode)",
        "lj_tab_node_freecount_acq(node) == freecount0",
        "lj_tab_node_freecount_acq(node) == freecount0 - 1u",
        "if (tab_key_retry_once(&nk, &retry))",
        "if (tab_key_islocked(&key))",
        "if (tab_key_islocked(&nk))",
        "tviskeylock(&key)",
        "tviskeylock(&out[0])",
        "lj_tab_newkey(L, t, &keyv) == &node[0].val",
        "exercise_tombstone_anchor_insert(L)",
        "strV(&node[0].key) == anchor",
        "getfreetop(t, node) == freetop0",
        "assert_tabnum(t, replacement, 33)"
      })

      local newkey = t:c_block(t:path("src", "lj_tab.c"),
                               "TValue *lj_tab_newkey")
      if count_plain(newkey, "lj_tab_node_free_reserve(nodebase)") < 1 or
         not contains(newkey, "tab_try_claim_nil_key(&n->key)") or
         not contains(newkey, "tab_claim_free_node_scan(nodebase, hmask, n, &locked)") or
         count_plain(newkey, "tab_release_claimed_free(nodebase, freenode)") < 2 or
         count_plain(newkey, "lj_tab_node_free_release(nodebase)") < 2 or
         not contains(newkey, "tab_storekeyrel(L, &n->key, key)") or
         not contains(newkey, "tab_storekeyrel(L, &freenode->key, key)") or
         contains(newkey, "setfreetop(t, nodebase, freenode)") then
        error("lj_tab_newkey must reserve freecount, KEYLOCK-claim nil keys, and avoid freetop mutation")
      end

      t:assert_contains(t:path("tools", "ci", "m5_concurrent_objects.sh"),
                        "m5_tab_keylock_lookup.sh")
      print("M5 table KEYLOCK lookup filtering tests passed")
    end
  })
end
