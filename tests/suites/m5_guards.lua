local LEGACY_BARRIERS = {
  "lj_gc_objbarrier",
  "lj_gc_objbarriert",
  "lj_gc_anybarriert",
  "lj_gc_barrieruv",
  "lj_gc_barriert",
  "lj_gc_barrier"
}

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

local function contains_token(s, token)
  local pos = 1
  while true do
    local first, last = s:find(token, pos, true)
    if not first then return false end
    local nextc = s:sub(last + 1, last + 1)
    if nextc == "" or not nextc:match("[%w_]") then return true end
    pos = last + 1
  end
end

local function contains_legacy_barrier(s)
  for i = 1, #LEGACY_BARRIERS do
    if contains_token(s, LEGACY_BARRIERS[i]) then return true end
  end
  return false
end

local function line_hits(t, paths, pred)
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
  return hits
end

local function assert_no_lines(t, label, paths, pred)
  local hits = line_hits(t, paths, pred)
  if #hits > 0 then
    error(label .. ":\n" .. table.concat(hits, "\n"), 2)
  end
end

local function source_files(t)
  return t:files(t:path("src"), { extensions = { ".c", ".h" } })
end

local function source_c_files(t)
  return t:files(t:path("src"), { extensions = ".c", recursive = false })
end

local function assert_block_contains(label, block, needle)
  if not contains(block, needle) then
    error(label .. ": missing expected text: " .. needle, 2)
  end
end

local function assert_block_absent(label, block, needle)
  if contains(block, needle) then
    error(label .. ": forbidden text present: " .. needle, 2)
  end
end

return function(add)
  add({
    name = "m5_gcroot_publish",
    description = "gcroot/base-metatable publication source guards",
    run = function(t)
      local src = source_files(t)

      assert_no_lines(t, "gcroot publications must use setgcrefroot()",
                      src, function(line)
        return contains(line, "setgcref(") and
               (contains(line, "gcroot") or contains(line, "basemt_"))
      end)

      assert_no_lines(t, "gcroot readers must acquire-load published roots",
                      src, function(line)
        return contains(line, "gcref(g->gcroot[") or
               (contains(line, "gcref(G(") and contains(line, ")->gcroot")) or
               contains(line, "tabref(g->gcroot") or
               (contains(line, "tabref(G(") and contains(line, ")->gcroot")) or
               (contains(line, "&gcref(") and contains(line, "gcroot"))
      end)

      assert_no_lines(t, "FINREG bootstrap must not use legacy gcroot slot",
                      src, function(line)
        return contains(line, "GCROOT_FFI_FIN") or
               contains(line, "lj_ctype_initfin")
      end)

      t:assert_all_any_contains({
        t:path("src", "lj_gc.c"),
        t:path("src", "lj_gc2.c"),
        t:path("src", "lj_cdata.c"),
        t:path("src", "lib_ffi.c"),
        t:path("src", "lib_io.c")
      }, {
        "GCobj *o = gcref_acq(g->gcroot[i])",
        "gco2ud(gcref_acq(G(L)->gcroot[(id)]))"
      })
    end
  })

  add({
    name = "m5_meta_snapshot",
    description = "Lua and ctype metamethod lookup snapshot guards",
    run = function(t)
      t:assert_all_any_contains({
        t:path("src", "lj_meta.c"),
        t:path("src", "lj_meta.h"),
        t:path("src", "lj_ctype.c"),
        t:path("src", "lj_ctype.h")
      }, {
        "cTValue *lj_meta_cachetv(GCtab *mt, MMS mm, GCstr *name,",
        "lj_tv_load_acq(out, mo)",
        "cTValue *lj_meta_lookuptv(lua_State *L, TValue *out,",
        "lj_meta_fasttv(g, mt, mm, out)",
        "cTValue *lj_ctype_metatv(CTState *cts, TValue *out,",
        "lj_tv_load_acq(&tabv, tv)"
      })

      local src = source_files(t)
      assert_no_lines(t, "Lua metamethod users must use lj_meta_lookuptv",
                      src, function(line, path)
        if not contains(line, "lj_meta_lookup(") then return false end
        if path == t:path("src", "lj_meta.c") and
           contains(line, "cTValue *lj_meta_lookup") then return false end
        if path == t:path("src", "lj_meta.h") then return false end
        return true
      end)

      assert_no_lines(t, "Lua fast metamethod users must use lj_meta_fasttv",
                      src, function(line, path)
        if not (contains(line, "lj_meta_fast(") or
                contains(line, "lj_meta_fastg(")) then return false end
        if path == t:path("src", "lj_meta.h") then return false end
        if path == t:path("src", "lj_meta.c") and
           contains(line, "See the lj_meta_fast") then return false end
        return true
      end)

      assert_no_lines(t, "ctype metamethod users must use lj_ctype_metatv",
                      src, function(line, path)
        if not contains(line, "lj_ctype_meta(") then return false end
        if path == t:path("src", "lj_ctype.c") and
           contains(line, "cTValue *lj_ctype_meta") then return false end
        if path == t:path("src", "lj_ctype.h") then return false end
        return true
      end)

      for _, file in ipairs({ "lj_gc.c", "lj_gc2.c" }) do
        t:assert_contains(t:path("src", file),
                          "mode = lj_meta_fasttv(g, mt, MM_mode, &modev)")
      end
      t:assert_contains(t:path("src", "lj_gc.c"),
                        "mo = lj_meta_fasttv(g, tabref_acq(gco2ud(o)->metatable), MM_gc, &motv)")
    end
  })

  add({
    name = "m5_jit_attach_publish",
    description = "jit.attach event table CAS publication guards",
    run = function(t)
      local lib_jit = t:path("src", "lib_jit.c")
      local store = t:c_block(lib_jit, "static TValue *jit_attach_event_store")
      assert_block_absent("jit_attach_event_store", store,
                          "lj_tab_storenil(L, dst)")
      assert_block_contains("jit_attach_event_store", store, "for (;;) {")
      assert_block_contains("jit_attach_event_store", store,
                            "dst = lj_tab_set(L, tab, key)")
      assert_block_contains("jit_attach_event_store", store,
                            "lj_tab_trystoretv_cas(L, dst, src) == LJ_TAB_STORE_CAS_OK")
      assert_block_contains("jit_attach_event_store", store,
                            "jit.attach event table saw FORWARD after lookup.")

      local attach = t:c_block(lib_jit, "LJLIB_CF(jit_attach)")
      assert_block_absent("jit_attach", attach,
                          "lj_tab_storenil(L, lj_tab_set(L, tabV(L->top-2), L->top-1))")
      assert_block_contains("jit_attach", attach,
                            "jit_attach_event_store(L, tabV(L->top-2), L->top-1, niltv(L))")
    end
  })

  add({
    name = "m5_jit_profile_publish",
    description = "jit.profile registry CAS publication guards",
    run = function(t)
      local lib_jit = t:path("src", "lib_jit.c")
      assert_no_lines(t, "lib_jit must use M5 publication wrappers",
                      { lib_jit }, contains_legacy_barrier)

      local store = t:c_block(lib_jit,
                              "static TValue *jit_profile_registry_store")
      assert_block_absent("jit_profile_registry_store", store,
                          "copyTVrel(L, dst, tv)")
      assert_block_absent("jit_profile_registry_store", store,
                          "lj_tab_storenil(L, dst)")
      assert_block_contains("jit_profile_registry_store", store, "for (;;) {")
      assert_block_contains("jit_profile_registry_store", store,
                            "dst = lj_tab_set(L, registry, key)")
      assert_block_contains("jit_profile_registry_store", store,
                            "lj_tab_trystoretv_cas(L, dst, tv) == LJ_TAB_STORE_CAS_OK")
      assert_block_contains("jit_profile_registry_store", store,
                            "jit.profile registry saw FORWARD after lookup.")

      local start = t:c_block(lib_jit, "LJLIB_CF(jit_profile_start)")
      assert_block_absent("jit_profile_start", start,
                          "copyTVrel(L, lj_tab_set(L, registry, &key), &tv)")
      if count_plain(start, "jit_profile_registry_store(L, registry, &key, &tv)") < 2 then
        error("jit_profile_start: expected two registry CAS stores")
      end
      if count_plain(start, "lj_gc2_barrier_weak_write(L, registry, &key, &tv)") < 2 then
        error("jit_profile_start: expected two weak barrier writes")
      end
      assert_block_contains("jit_profile_start", start,
                            "lj_gc_pubtab(L, registry)")

      local stop = t:c_block(lib_jit, "LJLIB_CF(jit_profile_stop)")
      assert_block_absent("jit_profile_stop", stop,
                          "lj_tab_storenil(L, lj_tab_set(L, registry, &key))")
      if count_plain(stop, "jit_profile_registry_store(L, registry, &key, niltv(L))") < 2 then
        error("jit_profile_stop: expected two registry clear stores")
      end
      assert_block_contains("jit_profile_stop", stop,
                            "lj_gc_pubtab(L, registry)")
    end
  })

  add({
    name = "m5_table_parser_publish",
    description = "table/parser publication wrapper source guards",
    run = function(t)
      local files = {
        t:path("src", "lj_tab.c"),
        t:path("src", "lj_parse.c"),
        t:path("src", "lj_bcread.c")
      }
      assert_no_lines(t, "table/parser files must use M5 publication wrappers",
                      files, contains_legacy_barrier)

      t:assert_all_any_contains(files, {
        "tab_storekeyrel(L, &n->key, key)",
        "copyTVrel(L, dst, &k)",
        "lj_gc_pubtab(L, t)",
        "setgcrefrel(pt->chunkname, obj2gco(ls->chunkname));",
        "lj_gc_pubobjobj(L, pt, ls->chunkname);",
        "setgcrefrel(((GCRef *)kptr)[~kidx], o)",
        "lj_gc_pubobjobj(fs->L, pt, o)",
        "setgcrefrel(*kr, o);",
        "lj_gc_pubobjobj(ls->L, pt, o);",
        "lj_gc_pubobjobj(ls->L, pt, ls->chunkname);",
        "copyTVrel(fs->L, v, &tv)",
        "lj_gc_pubtab(fs->L, t)",
        "parse_keep_storebool(L, ls->fs->kt, &key)",
        "parse_keep_storebool(L, ls->fs->kt, tv)"
      })

      assert_no_lines(t, "proto chunkname/KGC refs must use release stores",
                      { t:path("src", "lj_parse.c"),
                        t:path("src", "lj_bcread.c") }, function(line)
        return contains(line, "setgcref(") and
               (contains(line, "pt->chunkname") or contains(line, "*kr"))
      end)

      assert_no_lines(t, "parser anchor stores must use nil-only CAS helper",
                      { t:path("src", "lj_parse.c") }, function(line)
        return contains(line, "lj_tab_storebool(L, tv") or
               contains(line, "lj_tab_storebool(L, lj_tab_set(L, ls->fs->kt")
      end)

      local root_c = source_c_files(t)
      assert_no_lines(t, "legacy barrier call sites remain outside lj_gc.c",
                      root_c, function(line, path)
        return path ~= t:path("src", "lj_gc.c") and
               contains_legacy_barrier(line)
      end)
    end
  })

  add({
    name = "m5_tmpbuf_tg",
    description = "per-TG temporary string buffer routing guards",
    run = function(t)
      local tmp = t:c_block(t:path("src", "lj_buf.h"),
                            "static LJ_AINLINE SBuf *lj_buf_tmp_")
      assert_block_contains("lj_buf_tmp_", tmp, "L2TG(L)->tmpbuf")
      assert_block_contains("lj_buf_tmp_", tmp, "lj_buf_reset(sb)")

      local cat = t:c_block(t:path("src", "lj_meta.c"), "TValue *lj_meta_cat")
      assert_block_contains("lj_meta_cat", cat, "lj_buf_tmp_(L)")
      assert_block_contains("lj_meta_cat", cat,
                            "setstrV(L, top, lj_buf_str(L, sb))")

      local fmt = t:c_block(t:path("src", "lj_strfmt.c"),
                            "const char *lj_strfmt_pushvf")
      assert_block_contains("lj_strfmt_pushvf", fmt, "lj_buf_tmp_(L)")

      local excluded_global = {
        [t:path("src", "lj_gc.c")] = true,
        [t:path("src", "lj_gc2.c")] = true,
        [t:path("src", "lj_state.c")] = true,
        [t:path("src", "lj_tg.c")] = true
      }
      assert_no_lines(t, "global tmpbuf access is limited to init/GC ownership code",
                      source_files(t), function(line, path)
        return not excluded_global[path] and contains(line, "g->tmpbuf")
      end)

      local allowed_direct = {
        [t:path("src", "lj_buf.c")] = true,
        [t:path("src", "lj_buf.h")] = true,
        [t:path("src", "lj_serialize.c")] = true
      }
      assert_no_lines(t, "ordinary runtime tmpbuf access must go through lj_buf_tmp_",
                      source_files(t), function(line, path)
        return not allowed_direct[path] and contains(line, "&L2TG(L)->tmpbuf")
      end)
    end
  })
end
