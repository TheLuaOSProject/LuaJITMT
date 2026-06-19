local utils = require("suite_utils")

local contains = utils.contains
local count_plain = utils.count_plain
local assert_no_lines = utils.assert_no_lines

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

local function src_code_files(t)
  return t:files(t:path("src"), { extensions = { ".c", ".h", ".dasc" } })
end

local function assert_block_contains(t, label, path, start, needles)
  local block = t:c_block(path, start)
  for i = 1, #needles do
    t:assert_text_contains(label, block, needles[i])
  end
  return block
end

local function assert_block_excludes(t, label, block, rejects)
  for i = 1, #rejects do
    if contains(block, rejects[i]) then
      error(label .. ": forbidden text present: " .. rejects[i], 2)
    end
  end
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

      print("M5 table KEYLOCK lookup filtering tests passed")
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
    name = "m5_tab_nodehdr",
    description = "table hash-vector header C fixture and guards",
    run = function(t)
      build_and_run_table_c(t, t:tmp("lj_t-tab-nodehdr"),
                            "t-tab-nodehdr.c")

      t:assert_all_any_contains(src_code_files(t), {
        "typedef struct TabNodeHdr",
        "LJ_STATIC_ASSERT(sizeof(TabNodeHdr) == 16)",
        "TABNODE_FREECOUNT_MASK",
        "TABNODE_FLAG_RETIRING",
        "lj_tab_node_hmask_acq",
        "lj_tab_node_hdr_flags_acq",
        "lj_tab_node_freecount_acq",
        "lj_tab_node_free_reserve",
        "lj_tab_node_free_release",
        "lj_tab_node_nextgen_acq",
        "lj_tab_node_nextgen_rel",
        "lj_tab_node_hdr_flags_or_rel",
        "lj_tab_node_is_retiring",
        "lj_tab_node_snapshot_acq",
        "lj_tab_node_hdrw",
        "lj_tab_node_bytes",
        "TabNodeHdr nilnodehdr",
        "offsetof(global_State, nilnode)",
        "tab_node_new",
        "hdr->flags = (hmask + 1u) & TABNODE_FREECOUNT_MASK",
        "setmref(hdr->next_gen, NULL)",
        "g->nilnodehdr.flags = 0",
        "setmref(g->nilnodehdr.next_gen, NULL)",
        "tab_node_free"
      })

      t:assert_all_contains(t:path("tests", "t-tab-nodehdr.c"), {
        "lj_tab_node_hdr_flags_acq(oldnode) == TABNODE_FLAG_RETIRING",
        "lj_tab_node_freecount_acq(newnode) == newhmask + 1u - 5u",
        "lj_tab_node_nextgen_acq(oldnode) == newnode"
      })

      assert_no_lines(t, "table node header state must use flags, not unused",
                      src_code_files(t), function(line)
        return contains(line, "nilnodehdr.unused") or
               contains(line, "hdr->unused") or
               contains(line, ".unused = 0")
      end)

      assert_no_lines(t, "table node vectors must allocate/free with TabNodeHdr base",
                      { t:path("src", "lj_tab.c") }, function(line)
        return line:find("lj_mem_freevec%(g, [^,]*node") ~= nil or
               line:find("lj_mem_newvec%(L, [^,]*, Node") ~= nil
      end)

      assert_no_lines(t, "table node retiring retry must be centralized in lj_tab_node_snapshot_acq",
                      src_code_files(t), function(line)
        return contains(line, "tab_node_retry_if_retiring")
      end)

      local lj_tab = t:path("src", "lj_tab.c")
      for _, spec in ipairs({
        { "lj_tab_newkey", "TValue *lj_tab_newkey" },
        { "lj_tab_try_newkey_anchor", "int lj_tab_try_newkey_anchor" },
        { "lj_tab_try_newkey_chain", "int lj_tab_try_newkey_chain" },
        { "lj_tab_setinth", "TValue *lj_tab_setinth" },
        { "lj_tab_setstr", "TValue *lj_tab_setstr" },
        { "lj_tab_set", "TValue *lj_tab_set(lua_State *L," }
      }) do
        assert_block_contains(t, spec[1], lj_tab, spec[2], {
          "lj_tab_node_snapshot_acq(t, &hmask)"
        })
      end

      for _, spec in ipairs({
        { "clearhpart", "static LJ_AINLINE void clearhpart(GCtab *t)" },
        { "lj_tab_clear", "void LJ_FASTCALL lj_tab_clear" },
        { "lj_tab_free", "void LJ_FASTCALL lj_tab_free" }
      }) do
        local block = assert_block_contains(t, spec[1], lj_tab, spec[2], {
          "lj_tab_node_snapshot_acq(t, &hmask)"
        })
        assert_block_excludes(t, spec[1], block, {
          "lj_tab_node_acq(t)",
          "lj_tab_node_hmask_acq(node)"
        })
      end

      t:assert_text_ordered("lj_tab_resize", t:c_block(lj_tab,
                            "void lj_tab_resize"), {
        "lj_tab_node_nextgen_rel(oldnode,",
        "lj_tab_node_hdr_flags_or_rel(oldnode, TABNODE_FLAG_RETIRING)"
      })

      local resize = assert_block_contains(t, "lj_tab_resize", lj_tab,
                                           "void lj_tab_resize", {
        "oldnode = lj_tab_node_snapshot_acq(t, &oldhmask)"
      })
      assert_block_excludes(t, "lj_tab_resize", resize, {
        "lj_tab_node_acq(t)",
        "lj_tab_node_hmask_acq(oldnode)"
      })
      print("M5 table hash-vector header tests passed")
    end
  })

  add({
    name = "m5_tab_forward_filter",
    description = "table FORWARD value filtering C fixture and guards",
    run = function(t)
      build_and_run_table_c(t, t:tmp("lj_t-tab-forward-filter"),
                            "t-tab-forward-filter.c")

      t:assert_all_any_contains({
        t:path("src", "lj_tab.c"),
        t:path("src", "lj_tab.h"),
        t:path("tests", "t-tab-forward-filter.c")
      }, {
        "tab_val_absent(cTValue *val)",
        "return tvisnil(val) || tvisforward(val)",
        "tab_slot_absent_acq(const TValue *slot)",
        "tab_val_forward_retry_once(cTValue *val, int *retry)",
        "tab_node_forward_hop(Node **nodep, MSize *hmaskp)",
        "lj_tab_node_nextgen_acq(node)",
        "tab_forwarded_int_arrayslot(GCtab *t, int32_t key)",
        "tab_forwarded_setslot(GCtab *t, Node **nodep, MSize *hmaskp,",
        "tab_forwarded_hash_value(GCtab *t, Node **nodep,",
        "tab_array_slot_absent_acq(GCtab *t, TValue **arrayp,",
        "lj_tab_array_forward_hop(const GCtab *t, TValue **arrayp,",
        "lj_tab_array_nextgen_acq(array)",
        "lj_tab_array_hdr_asize_acq(next)",
        "tab_val_absent(&val)",
        "tab_slot_absent_acq(tv)",
        "tab_array_slot_absent_acq(t, &array, &asize, (MSize)hi)",
        "lj_tab_getint(t, 3) == NULL",
        "lj_tab_getstr(t, hidden) == NULL",
        "lj_tab_len(t) == 5",
        "lj_tab_len_hint(t, 5) == 5",
        "exercise_array_forward_hop(L)",
        "lj_tab_array_nextgen_acq(oldarray) == newarray",
        "lj_tab_storeint(L, lj_tab_setint(L, t, 5), 909)",
        "exercise_hash_forward_hop(L)",
        "lj_tab_storeint(L, lj_tab_setstr(L, t, hopstr), 404)",
        "lj_tab_storeint(L, lj_tab_set(L, t, &lightkey), 606)",
        "exercise_hash_to_array_forward_hop(L)",
        "assert_i32(&newarray[moveint], 909)",
        "lj_tab_node_nextgen_acq(oldnode) == newnode",
        "t-tab-forward-filter OK"
      })
      local lj_tab_h = t:path("src", "lj_tab.h")
      assert_block_contains(t, "lj_tab_getint", lj_tab_h,
                            "static LJ_AINLINE cTValue *lj_tab_getint", {
        "lj_tab_array_forward_hop(t, &array, &asize)",
        "goto genarray",
        "tvisforward(&val)",
        "goto retry_array",
        "return NULL"
      })
      assert_block_contains(t, "lj_tab_setint", lj_tab_h,
                            "static LJ_AINLINE TValue *lj_tab_setint", {
        "tvisforward(&val)",
        "lj_tab_array_forward_hop(t, &array, &asize)",
        "goto genarray",
        "goto retry_array",
        "return lj_tab_setinth(L, t, key)"
      })

      local lj_tab = t:path("src", "lj_tab.c")
      assert_block_contains(t, "lj_tab_getinth", lj_tab,
                            "cTValue * LJ_FASTCALL lj_tab_getinth", {
        "tab_node_forward_hop(&node, &hmask)",
        "tab_forwarded_int_arrayslot(t, key)",
        "tab_val_forward_retry_once(&val, &forward_retry)"
      })
      assert_block_contains(t, "lj_tab_getstr", lj_tab,
                            "cTValue *lj_tab_getstr", {
        "tab_node_forward_hop(&node, &hmask)",
        "tab_val_forward_retry_once(&val, &forward_retry)"
      })
      assert_block_contains(t, "lj_tab_get", lj_tab,
                            "cTValue *lj_tab_get(lua_State *L,", {
        "tab_node_forward_hop(&node, &hmask)",
        "tab_val_forward_retry_once(&val, &forward_retry)"
      })
      assert_block_contains(t, "lj_tab_setinth", lj_tab,
                            "TValue *lj_tab_setinth", {
        "tab_forwarded_setslot(t, &node, &hmask, &k)",
        "tab_val_forward_retry_once(&val, &forward_retry)"
      })
      assert_block_contains(t, "lj_tab_setstr", lj_tab,
                            "TValue *lj_tab_setstr", {
        "tab_forwarded_setslot(t, &node, &hmask, &k)",
        "tab_val_forward_retry_once(&val, &forward_retry)"
      })
      assert_block_contains(t, "lj_tab_set", lj_tab,
                            "TValue *lj_tab_set(lua_State *L,", {
        "tab_forwarded_setslot(t, &node, &hmask, key)",
        "tab_val_forward_retry_once(&val, &forward_retry)"
      })

      local next_block = assert_block_contains(t, "lj_tab_next", lj_tab,
                                               "int lj_tab_next", {
        "lj_tab_array_forward_hop(t, &nextarray, &nextasize)",
        "tab_forwarded_hash_value(t, &hopnode, &hophmask, &key, &val)"
      })
      if count_plain(next_block, "tab_val_absent(&val)") < 2 then
        error("lj_tab_next: expected array and hash FORWARD absence checks")
      end

      local absent_block = assert_block_contains(t, "tab_array_slot_absent_acq",
                                                 lj_tab,
                                                 "static LJ_AINLINE int tab_array_slot_absent_acq", {
        "lj_tab_array_forward_hop(t, &nextarray, &nextasize)",
        "tab_val_absent(&val)"
      })
      local len_blocks = absent_block ..
        t:c_block(lj_tab, "static MSize tab_len_slow") ..
        t:c_block(lj_tab, "MSize LJ_FASTCALL lj_tab_len") ..
        t:c_block(lj_tab, "MSize LJ_FASTCALL lj_tab_len_hint")
      local absent_checks = count_plain(len_blocks, "tab_slot_absent_acq") +
        count_plain(len_blocks, "tab_val_absent") +
        count_plain(len_blocks, "tab_array_slot_absent_acq")
      if absent_checks < 6 then
        error("table length helpers must hop/filter FORWARD values")
      end
      print("M5 table FORWARD filtering tests passed")
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
