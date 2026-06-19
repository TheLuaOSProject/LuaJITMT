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

  add({
    name = "m5_tab_alloc_publish",
    description = "table allocation root publication source guards",
    run = function(t)
      local files = { t:path("src", "lj_tab.c"), t:path("src", "lj_tab.h") }
      t:assert_all_any_contains(files, {
        "static LJ_AINLINE void tab_init_empty(global_State *g, GCtab *t)",
        "static LJ_AINLINE void tab_publish_new(global_State *g, GCtab *t)",
        "static LJ_AINLINE void tab_publish_array(GCtab *t, TValue *array,",
        "GCtab * LJ_FASTCALL lj_tab_new0(lua_State *L)",
        "lj_mem_newgco_unlinked(L, sizetabcolo(asize))",
        "lj_mem_newgco_unlinked(L, sizeof(GCtab))",
        "tab_init_empty(g, t)",
        "cleararray(array, asize)",
        "tab_publish_array(t, array, asize, asize)",
        "newwhite(g, t)",
        "lj_gc_linkobj(g, obj2gco(t));  /* CAS-publish table after body init. */"
      })

      local publish = t:c_block(t:path("src", "lj_tab.c"),
                                "static LJ_AINLINE void tab_publish_array")
      t:assert_text_ordered("tab_publish_array", publish, {
        "t->acap = acap",
        "lj_tab_array_rel(t, array)",
        "lj_tab_asize_rel(t, asize)"
      })

      local newtab = t:c_block(t:path("src", "lj_tab.c"),
                               "static GCtab *newtab")
      for _, needle in ipairs({
        "lj_tab_array_set(t, array)",
        "t->asize = asize",
        "t->acap = asize"
      }) do
        if contains(newtab, needle) then
          error("newtab must release-publish fresh arrays instead of raw metadata stores: " .. needle)
        end
      end
      t:assert_text_ordered("newtab", newtab, {
        "tab_publish_new(g, t)",
        "newhpart(L, t, hbits)"
      })

      t:assert_all_any_contains({
        t:path("src", "lj_dispatch.h"),
        t:path("src", "vm_x64.dasc")
      }, {
        "_(lj_tab_new) _(lj_tab_new0)",
        "call extern lj_tab_new0  // (lua_State *L)"
      })

      t:assert_not_contains(t:path("src", "lj_tab.c"),
                            "lj_mem_newgco(L, sizetabcolo(asize))")
      t:assert_not_contains(t:path("src", "lj_tab.c"),
                            "lj_mem_newobj(L, GCtab)")
      print("M5 table allocation publication guard passed")
    end
  })

  add({
    name = "m5_tab_chain_order",
    description = "stable table-node/hash-chain ordering C fixture and guards",
    run = function(t)
      build_and_run_table_c(t, t:tmp("lj_t-tab-chain-order"),
                            "t-tab-chain-order.c")

      t:assert_all_any_contains(src_ch_files(t), {
        "lj_tab_nextnode_acq",
        "lj_tab_nextnode_rel",
        "la_load64_acq(&n->next.ptr64)",
        "la_store64_rel(&n->next.ptr64",
        "Nodes are never moved within a hash generation",
        "lj_tab_nextnode_rel(n, freenode)",
        "tab_storekeyrel(L, &freenode->key, key)",
        "return &freenode->val",
        "lj_tab_nextnode_acq(n)",
        "LJ_TAB_MAXCHAIN",
        "tab_rehash_chain_overflow",
        "chainlen >= LJ_TAB_MAXCHAIN"
      })
      t:assert_all_contains(t:path("tests", "t-tab-chain-order.c"), {
        "exercise_chainlen_resize(L)",
        "chain_len(&node[0]) == 8",
        "lj_tab_node_hmask_acq(node) > oldhmask"
      })

      local newkey = t:c_block(t:path("src", "lj_tab.c"),
                               "TValue *lj_tab_newkey")
      if count_plain(newkey, "tab_findkey_or_keylock(n, key, &locked, &chainlen)") < 2 or
         count_plain(newkey, "chainlen >= LJ_TAB_MAXCHAIN") < 2 or
         count_plain(newkey, "tab_rehash_chain_overflow(L, t, key, hmask)") < 2 then
        error("new-key collision insertion must grow on max chain length")
      end

      assert_no_lines(t, "table hash-chain walks/stores must use ordered helpers",
                      {
                        t:path("src", "lj_tab.c"),
                        t:path("src", "lj_serialize.c")
                      }, function(line)
        if contains(line, "next_gen") then return false end
        return contains(line, "nextnode(") or
               (contains(line, "setmref(") and contains(line, "->next")) or
               (contains(line, "setmrefr(") and contains(line, "->next")) or
               (contains(line, "noderef(") and contains(line, "->next"))
      end)

      assert_no_lines(t, "table insertion must not move existing hash nodes",
                      { t:path("src", "lj_tab.c") }, function(line)
        return contains(line, "freenode->val = n->val") or
               contains(line, "freenode->key = n->key") or
               contains(line, "Colliding node not the main node") or
               contains(line, "Use Brent")
      end)
      print("M5 stable table-node/hash-chain ordering tests passed")
    end
  })

  add({
    name = "m5_tab_node_publish",
    description = "table hash-vector publication C fixture and guards",
    run = function(t)
      build_and_run_table_c(t, t:tmp("lj_t-tab-node-publish"),
                            "t-tab-node-publish.c")

      t:assert_all_any_contains(src_ch_files(t), {
        "lj_tab_node_acq",
        "lj_tab_node_rel",
        "la_load64_acq(&t->node.ptr64)",
        "la_store64_rel(&t->node.ptr64",
        "hashmask(const GCtab *t, uint32_t hash)",
        "Node *n = lj_tab_node_snapshot_acq(t, &hmask)",
        "return hashmask_node(n, hmask, hash)",
        "lj_tab_node_rel(t, node)",
        "lj_tab_node_rel(t, &g->nilnode)",
        "newhpart_alloc",
        "newhpart_publish",
        "tab_rehash_hashcount",
        "tab_rehash_arrayslot",
        "tab_rehash_slot",
        "tab_rehash_insert",
        "n = hashnum_node(node, hmask, &k)",
        "n = hashstr_node(node, hmask, key)",
        "n = hashkey_node(node, hmask, key)"
      })

      assert_no_lines(t, "table node vectors must use lj_tab_node_* helpers",
                      src_ch_files(t), function(line, path)
        if path == t:path("src", "lj_obj.h") or
           contains(path, "/src/host/") then return false end
        return (contains(line, "noderef(") and
                (contains(line, "->node") or contains(line, "(node"))) or
               (contains(line, "setmref(") and
                (contains(line, "->node") or contains(line, "(node")))
      end)

      assert_no_lines(t, "lj_tab.c hash lookups must use explicit node-header snapshots",
                      { t:path("src", "lj_tab.c") }, function(line)
        return contains(line, "hashkey(t") or
               contains(line, "hashstr(t") or
               contains(line, "hashnum(t") or
               contains(line, "hashmask(t") or
               contains(line, "hashgcref(t")
      end)

      local resize = t:c_block(t:path("src", "lj_tab.c"),
                               "void lj_tab_resize")
      for _, needle in ipairs({
        "newhpart(L, t, hbits)",
        "lj_tab_setint(L, t,",
        "lj_tab_setinth(L, t,"
      }) do
        if contains(resize, needle) then
          error("resize must rebuild hash vectors before publication: " .. needle)
        end
      end
      t:assert_text_ordered("lj_tab_resize", resize, {
        "tab_rehash_hashcount(oldnode, oldhmask, oldarray,",
        "tab_rehash_slot(L, array, asize, newnode, newhmask,",
        "newhpart_publish(t, newnode, newhmask, newfreetop)",
        "tab_retire_arm(G(L), oldret)"
      })
      print("M5 table hash-vector publication tests passed")
    end
  })

  add({
    name = "m5_tab_retire",
    description = "table hash-vector retirement C fixture and guards",
    run = function(t)
      build_and_run_table_c(t, t:tmp("lj_t-tab-retire"),
                            "t-tab-retire.c")

      t:assert_all_any_contains(src_ch_files(t), {
        "TabNodeRetire",
        "retired_nodes",
        "tab_retire_reserve",
        "tab_retire_arm",
        "lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING)",
        "lj_tab_reclaim_retired",
        "lj_tab_freeretired",
        "lj_tab_reclaim_retired(g, epoch)",
        "lj_gc2_reclaim_retired(g, epoch)",
        "gc_mark_tab_retired_mem",
        "gc2_mark_tab_retired_mem"
      })
      t:assert_contains(t:path("tests", "t-tab-retire.c"),
                        "lj_tab_node_hdr_flags_acq(oldnode) == TABNODE_FLAG_RETIRING")
      t:assert_not_contains(t:path("src", "lj_tab.c"),
                            "lj_mem_freevec(g, oldnode, oldhmask+1, Node)")
      print("M5 table hash-vector retirement tests passed")
    end
  })
end
