local utils = require("suite_utils")

local getenv = utils.getenv
local contains = utils.contains
local count_plain = utils.count_plain
local shell_quote = utils.shell_quote
local line_contains_any = utils.line_contains_any
local assert_no_lines = utils.assert_no_lines
local assert_text_not_contains = utils.assert_text_not_contains

local function source_files(t)
  return t:files(t:path("src"), {
    extensions = { ".c", ".h", ".dasc", ".lua", ".S", ".d" }
  })
end

local function src_ch_files(t)
  return t:files(t:path("src"), { extensions = { ".c", ".h" } })
end

local function lua_path(t)
  return t:path("src", "?.lua") .. ";" .. t:path("src", "jit", "?.lua") .. ";;"
end

local function assert_text_contains(label, data, needle)
  if not contains(data, needle) then
    error(label .. ": missing expected text: " .. needle, 2)
  end
end

local function assert_text_ordered(label, data, needles)
  local pos = 1
  for i = 1, #needles do
    local next_pos = data:find(needles[i], pos, true)
    if not next_pos then
      error(label .. ": missing expected text: " .. needles[i], 2)
    end
    pos = next_pos + #needles[i]
  end
end

local function find_plain(data, needle, start)
  return data:find(needle, start or 1, true)
end

local function assert_pos_order(label, data, needles)
  local last = 0
  for i = 1, #needles do
    local p = find_plain(data, needles[i], last + 1)
    if not p then error(label .. ": missing expected text: " .. needles[i], 2) end
    last = p
  end
end

local function count_line_hits(t, paths, pred)
  local count = 0
  for i = 1, #paths do
    local path = paths[i]
    for line in (t:read(path) .. "\n"):gmatch("(.-)\n") do
      if pred(line, path) then count = count + 1 end
    end
  end
  return count
end

local function build_and_run_c(t, out, cfile, opts)
  opts = opts or {}
  t:cc(out, { t:path("tests", cfile) }, {
    link_luajit = true,
    libs = { "-lm", "-ldl", "-pthread" }
  })
  t:run({ out }, { timeout = opts.timeout })
end

local function clean_build(t, opts)
  opts = opts or {}
  t:build({ clean = true, quiet = true, xcflags = opts.xcflags })
end

local function run_luajit_script(t, script, args, opts)
  args = args or {}
  opts = opts or {}
  local argv = {}
  if opts.joff then argv[#argv + 1] = "-joff" end
  argv[#argv + 1] = t:path("tests", script)
  for i = 1, #args do argv[#argv + 1] = args[i] end
  t:luajit(argv, { timeout = opts.timeout, env = opts.env })
end

local function run_dump_probe(t, dump, script)
  local cmd = "LUA_PATH=" .. shell_quote(lua_path(t)) .. " " ..
              "timeout 20s " .. shell_quote(t:path("src", "luajit")) ..
              " -jdump=ir -e " .. shell_quote(script) ..
              " >" .. shell_quote(dump)
  t:run(cmd)
end

local function assert_callback_install_order(t)
  local ccallback = t:path("src", "lj_ccallback.c")
  local lib_ffi = t:path("src", "lib_ffi.c")

  local clear = t:c_block(ccallback, "void lj_ccallback_func_clear(CTState *cts, MSize slot)")
  assert_text_not_contains("callback func clear", clear, "lj_tab_storenilraw(&func[slot])")
  assert_text_contains("callback func clear", clear, "setnilV(&nilv)")
  assert_text_contains("callback func clear", clear,
                       "copyTVrel(mainthread(cts->g), &func[slot], &nilv)")

  local misc = t:c_block(lib_ffi, "static TValue *ffi_miscmap_store(lua_State *L,")
  assert_text_not_contains("ffi_miscmap_store", misc, "lj_tab_storetab(L, dst,")
  for _, needle in ipairs({
    "for (;;)",
    "lj_tab_setstr(L, cts->miscmap, key)",
    "lj_tab_trystoretv_cas(L, dst, src) == LJ_TAB_STORE_CAS_OK",
    "FFI miscmap store saw FORWARD after lookup."
  }) do
    assert_text_contains("ffi_miscmap_store", misc, needle)
  end

  local open = t:c_block(lib_ffi, "LUALIB_API int luaopen_ffi(lua_State *L)")
  assert_text_not_contains("luaopen_ffi", open,
                           "lj_tab_storetab(L, lj_tab_setstr(L, cts->miscmap, &cts->g->strempty),")
  assert_text_ordered("luaopen_ffi", open, {
    "ffi_miscmap_store(L, cts, &cts->g->strempty, L->top-1)",
    "lj_gc_pubtabobj(L, cts->miscmap, tabV(L->top-1))"
  })

  local init = t:c_block(ccallback, "void lj_ccallback_init_l")
  assert_text_ordered("lj_ccallback_init_l", init, {
    "callback_slots_init_l(L, cts)",
    "callback_mcode_new_l(L, cts)"
  })

  local new = t:c_block(ccallback, "void *lj_ccallback_new_l")
  assert_no_lines(t, "callback new must not lazily initialize mcode/lock",
                  { ccallback }, function(line, path, n)
    return contains(new, line) and
           (contains(line, "callback_mcode_new_l") or
            contains(line, "lj_ctype_misc_lock") or
            contains(line, "lj_ctype_misc_unlock"))
  end)
  assert_text_ordered("lj_ccallback_new_l", new, {
    "callback_slot_claim_l(L, cts)",
    "lj_ccallback_func_store_l(L, cts, slot, fn)",
    "callback_cbid_store(cbid, slot, id)"
  })

  local claim = t:c_block(ccallback, "static MSize callback_slot_claim_l")
  assert_text_not_contains("callback_slot_claim_l", claim, "callback_slots_init_l")
  for _, needle in ipairs({
    "for (top = 0; top < sizeid; top++)",
    "TValue *func = callback_func_slots(cts)",
    "cbid == NULL || owner == NULL || func == NULL || sizeid == 0",
    "callback_cbid_load(cbid, top) == 0"
  }) do
    assert_text_contains("callback_slot_claim_l", claim, needle)
  end
  assert_text_ordered("callback_slot_claim_l", claim, {
    "callback_owner_load(owner, top) == NULL",
    "if (carrier == NULL)",
    "callback_carrier_new_l(L)",
    "callback_owner_claim(owner, top, carrier)"
  })

  local set = t:c_block(lib_ffi, "static int ffi_callback_set")
  assert_text_not_contains("ffi_callback_set", set, "lj_ctype_misc_lock(cts)")
  assert_text_ordered("ffi_callback_set disowned", set, {
    "11.5 disowned callback free",
    "lj_ccallback_func_clear(cts, slot)",
    "la_store16_rel(&cbid[slot], 0)"
  })
  assert_text_ordered("ffi_callback_set owned", set, {
    "11.5 owned callback free",
    "la_store16_rel(&cbid[slot], 0)",
    "lj_ccallback_func_clear(cts, slot)",
    "la_storeptr_rel((void **)&owner[slot], NULL)"
  })
end

local m7_cases = {
  "m7_ffi_cdef_token",
  "m7_ffi_cdef_dup_stack",
  "m7_ffi_cparse_rollback",
  "m7_ffi_ctype_intern_l",
  "m7_ffi_ctype_hash_publish",
  "m7_ffi_ctype_tab_retire",
  "m7_ffi_ctype_ticket_intern",
  "m7_ffi_ctype_name_claim",
  "m7_ffi_ctype_pointer_ids",
  "m7_ffi_cdata_alloc",
  "m7_ffi_jit_cnew",
  "m7_ffi_snap_restore_l",
  "m7_ffi_finreg",
  "m7_ffi_pin",
  "m7_ffi_metatype",
  "m7_ffi_cdata_get_l",
  "m7_ffi_cdata_set_l",
  "m7_ffi_carith_l",
  "m7_ffi_clib_cache",
  "m7_ffi_callback_install",
  "m7_ffi_callback_runtime",
  "m7_ffi_blocking"
}

return function(add)
  add({
    name = "m7_ffi_blocking",
    description = "FFI blocking recorder blacklist guard",
    run = function(t)
      local src = {
        t:path("src", "lib_ffi.c"),
        t:path("src", "lj_ctype.c"),
        t:path("src", "lj_crecord.c")
      }
      t:assert_all_any_contains(src, {
        "LJLIB_CF(ffi_blocking)",
        "ctype_isfunc(ct->info)",
        "lj_ctype_cb_blacklist(cts, cdata_getptr(cdataptr(cd), sz))",
        "lj_trace_flushall_hs(L)",
        "lj_ctype_cb_isblacklisted(cts,",
        "lj_trace_err(J, LJ_TRERR_BLACKL)"
      })
      assert_no_lines(t, "ffi.blocking must reuse pointer blacklist",
                      src, function(line)
        return line_contains_any(line, {
          "blocking_token",
          "ffi_blocking_lock",
          "ffi_blocking_unlock",
          "LJ_MT",
          "LUAJIT_THREADSAFE"
        }) or (contains(line, "lj_udata_new(") and contains(line, "blocking"))
      end)
      clean_build(t)
      run_luajit_script(t, "t-ffi-blocking.lua")
      print("M7 ffi.blocking guard passed")
    end
  })

  add({
    name = "m7_ffi_callback_install",
    description = "FFI callback slot claiming and publish/free ordering",
    run = function(t)
      local src = source_files(t)
      t:assert_all_any_contains(src, {
        "lj_ccallback_new_l(lua_State *L, CTState *cts",
        "callback_slot_claim_l(lua_State *L, CTState *cts)",
        "callback_checkfunc(CTState *cts, CType *ct, CTypeID *idp)",
        "*idp = ctype_rawid(cts, ctype_cid(ct->info))",
        "callback_mcode_new_l(lua_State *L, CTState *cts)",
        "callback_mcode_new_l(L, cts);  /* 11.5: mcode read-only after FFI init. */",
        "lj_ccallback_init_l(lua_State *L, CTState *cts)",
        "lj_ccallback_maxslot(void)",
        "callback_owner_claim(lua_State **owner, MSize slot,",
        "la_casptr((void **)&owner[slot], &expect, L,",
        "callback_owner_load(owner, top) == NULL",
        "if (carrier == NULL)",
        "callback_carrier_new_l(L)",
        "callback_owner_claim(owner, top, carrier)",
        "callback_owner_barrier_l(L, carrier)",
        "11.5 callback carrier side root",
        "TValue *func;",
        "if (cbid == NULL || owner == NULL || func == NULL || sizeid == 0)",
        "lj_mem_newvec(L, CALLBACK_MAX_SLOT, CTypeID1)",
        "la_storeptr_rel((void **)&cts->cb.cbid, cbid)",
        "lj_mem_newvec(L, CALLBACK_MAX_SLOT, TValue)",
        "setnilV(&func[i])",
        "la_storeptr_rel((void **)&cts->cb.func, func)",
        "la_store32_rel(&cts->cb.sizeid, CALLBACK_MAX_SLOT)",
        "callback_cbid_load(cbid, top)",
        "callback_cbid_store(cbid, slot, id)",
        "callback_func_load(CTState *cts, MSize slot,",
        "callback_func_load(cts, slot, &tv)",
        "lj_ccallback_func_store_l(lua_State *L, CTState *cts",
        "setfuncV(L, &tv, fn)",
        "copyTVrel(L, &func[slot], &tv)",
        "lj_gc_barrierroot(L, &func[slot])",
        "lj_ccallback_func_clear(CTState *cts, MSize slot)",
        "setnilV(&nilv)",
        "copyTVrel(mainthread(cts->g), &func[slot], &nilv)",
        "lj_gc_arena_markmem(g, func)",
        "lj_gc2_markmem(g, func)",
        "gc_markobj(g, obj2gco(th))",
        "lj_gc2_markobj(g, obj2gco(th))",
        "if (tvisfunc(&tv))",
        "ffi_miscmap_store(lua_State *L, CTState *cts, GCstr *key,",
        "FFI miscmap store saw FORWARD after lookup.",
        "ffi_miscmap_store(L, cts, &cts->g->strempty, L->top-1)",
        "lj_gc_pubtabobj(L, cts->miscmap, tabV(L->top-1))",
        "lj_ccallback_new_l(L, cts",
        "lj_ccallback_init_l(L, cts)",
        "lj_tab_new(L, 0, 1)",
        "lj_state_checkstack(L, 1)",
        "setcdataV(L, L->top++, cd)",
        "LJLIB_CF(ffi_callback_free)",
        "la_store16_rel(&cbid[slot], 0)",
        "la_loadptr_acq((void *const *)&owner[slot]) == NULL",
        "11.5 disowned callback free: nil function before cbid release.",
        "11.5 owned callback free: cbid release before owner release.",
        "lj_ccallback_func_store_l(L, cts, slot, fn)",
        "lj_ccallback_func_clear(cts, slot)",
        "la_storeptr_rel((void **)&owner[slot], NULL)"
      })
      assert_no_lines(t, "callback setup wrappers must stay explicit-L only",
                      src, function(line)
        return line_contains_any(line, {
          "lj_ccallback_new(CTState",
          "callback_slot_new",
          "callback_mcode_new(CTState",
          "misc_token",
          "lj_ctype_misc_lock",
          "lj_ctype_misc_unlock"
        })
      end)
      assert_no_lines(t, "callback function slots must stay in CTState side array",
                      src, function(line)
        return contains(line, "lj_tab_getint(cts->miscmap, (int32_t)slot)") or
               (contains(line, "lj_tab_setint(L,") and contains(line, "(int32_t)slot)")) or
               contains(line, "lj_tab_new(L, (uint32_t)lj_ccallback_maxslot(), 1)")
      end)
      assert_no_lines(t, "callback slot reuse must not depend on old topid cursor",
                      {
                        t:path("src", "lj_ccallback.c"),
                        t:path("src", "lj_ctype.h")
                      }, function(line)
        return contains(line, "cb.topid") or contains(line, "MSize topid")
      end)
      assert_callback_install_order(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-callback-install.lua", {
        getenv("LJ_M7_FFI_CBACK_THREADS", "6"),
        getenv("LJ_M7_FFI_CBACK_ITERS", "64")
      }, { joff = true })
      print("M7 FFI callback install guard passed")
    end
  })

  add({
    name = "m7_ffi_callback_runtime",
    description = "FFI callback runtime behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-callback-nested-native"),
                      "t-ffi-callback-nested-native.c")
      build_and_run_c(t, t:tmp("lj_t-ffi-callback-owner-lifetime"),
                      "t-ffi-callback-owner-lifetime.c")
      build_and_run_c(t, t:tmp("lj_t-ffi-callback-attached-carrier"),
                      "t-ffi-callback-attached-carrier.c")
      build_and_run_c(t, t:tmp("lj_t-ffi-callback-auto-attach"),
                      "t-ffi-callback-auto-attach.c")
      run_luajit_script(t, "t-ffi-callback-runtime.lua", {
        getenv("LJ_M7_FFI_CBACK_RT_THREADS", "6"),
        getenv("LJ_M7_FFI_CBACK_RT_ITERS", "220")
      }, { joff = true })
      t:luajit({ t:path("tests", "stock", "test", "lib", "ffi", "ffi_callback.lua") })
      print("M7 FFI callback runtime behavior passed")
    end
  })

  add({
    name = "m7_ffi_carith_l",
    description = "FFI arithmetic/raw conversion explicit-L bridge",
    run = function(t)
      local files = {
        t:path("src", "lj_carith.c"),
        t:path("src", "lj_cconv.c"),
        t:path("src", "lj_cconv.h")
      }
      t:assert_all_any_contains(files, {
        "lj_cconv_ct_ct_l(lua_State *L, CTState *cts, CType *d,",
        "CTypeID did, CType *s, CTypeID sid",
        "cconv_err_conv_l(lua_State *L, CTState *cts,",
        "lj_cconv_ct_ct_l(L, cts, ctype_get(cts, CTID_INT_PSZ), CTID_INT_PSZ",
        "lj_cconv_ct_ct_l(L, cts, ct, id, ca->ct[0], ca->id[0]",
        "lj_cconv_ct_ct_l(L, cts, ct, id, ca->ct[1], ca->id[1]",
        "lj_cconv_ct_ct_l(L, cts, ctype_get(cts, *id), *id, s, sid",
        "lj_cdata_new_l(L, cts, id, CTSIZE_PTR)",
        "lj_cdata_new_l(L, cts, id, 8)",
        "CTypeID id[2]",
        "CTypeID id0 = i ? ca->id[0] : 0",
        "repr[i] = strdata(lj_ctype_repr(L, ca->id[i], NULL))",
        "lj_cconv_ct_ct_l(L, cts, ctype_get(cts, CTID_INT32), CTID_INT32",
        "lj_cconv_ct_ct_l(L, cts, ctype_get(cts, CTID_DOUBLE), CTID_DOUBLE"
      })
      assert_no_lines(t, "legacy lj_cconv_ct_ct wrapper must stay removed",
                      files, function(line)
        return contains(line, "LJ_FUNC void lj_cconv_ct_ct(") or
               contains(line, "void lj_cconv_ct_ct(") or
               contains(line, "lj_cconv_ct_ct(cts")
      end)
      assert_no_lines(t, "runtime C-to-C conversion paths must carry CTypeIDs",
                      {
                        t:path("src", "lj_cconv.c"),
                        t:path("src", "lj_carith.c"),
                        t:path("src", "lj_cdata.c")
                      }, function(line)
        return contains(line, "ctype_typeid(cts")
      end)
      clean_build(t)
      run_luajit_script(t, "t-ffi-carith-l.lua", nil, { joff = true })
      print("M7 FFI arithmetic explicit-L guard passed")
    end
  })

  add({
    name = "m7_ffi_cdata_alloc",
    description = "concurrent FFI cdata allocation publication",
    run = function(t)
      t:assert_all_any_contains(source_files(t), {
        "void lj_gc_linkobj(global_State *g, GCobj *o)",
        "void *lj_mem_newgco_unlinked(lua_State *L, GCSize size)",
        "la_cas64(&g->gc.root.gcptr64",
        "la_cas32(&g->gc.root.gcptr32",
        "lj_mem_newgco_unlinked(L, sizeof(GCcdata) + sz",
        "lj_gc_linkobj(g, obj2gco(cd))",
        "lj_gc_linkobj(g, o);  /* CAS-requeue finalized cdata on root list. */",
        "lj_gc_linkobj(g, o);  /* CAS-publish closed upvalue on root list. */",
        "lj_gc_linkobj(g, obj2gco(T));  /* CAS-publish root after body init. */",
        "lj_cdata_new_(L, CTID_CTYPEID, 4)"
      })
      assert_no_lines(t, "runtime root-list publication must use lj_gc_linkobj()",
                      {
                        t:path("src", "lj_gc.c"),
                        t:path("src", "lj_trace.c")
                      }, function(line)
        return line_contains_any(line, {
          "setgcref(g->gc.root",
          "setgcrefp(J2G(J)->gc.root",
          "lj_obj_setgcwr(o, g->gc.root",
          "lj_obj_setgcwr(obj2gco(T), J2G(J)->gc.root"
        })
      end)
      clean_build(t)
      run_luajit_script(t, "t-ffi-cdata-alloc.lua", {
        getenv("LJ_M7_FFI_CDATA_THREADS", "6"),
        getenv("LJ_M7_FFI_CDATA_ITERS", "400")
      }, { joff = true })
      print("M7 FFI cdata allocation guard passed")
    end
  })

  add({
    name = "m7_ffi_cdata_get_l",
    description = "FFI cdata read paths pass active lua_State explicitly",
    run = function(t)
      local src = source_files(t)
      t:assert_all_any_contains(src, {
        "lj_cdata_newref_l(lua_State *L, CTState *cts",
        "lj_cdata_index_l(lua_State *L, CTState *cts",
        "lj_cdata_get_l(lua_State *L, CTState *cts",
        "lj_cconv_tv_ct_l(lua_State *L, CTState *cts",
        "lj_cconv_tv_bf_l(lua_State *L, CTState *cts",
        "L2TG(L)->tmptv2",
        "lj_cdata_index_l(L, cts",
        "lj_cdata_get_l(L, cts",
        "lj_cconv_tv_ct_l(L, cts, ct, sid",
        "lj_cconv_tv_ct_l(L, cts, ctr",
        "lj_cconv_tv_ct_l(L, cts, cta"
      })
      assert_no_lines(t, "active-L FFI conversion call site still uses cts->L wrapper",
                      {
                        t:path("src", "lib_ffi.c"),
                        t:path("src", "lj_ccall.c"),
                        t:path("src", "lj_ccallback.c")
                      }, function(line)
        return contains(line, "lj_cconv_tv_ct(cts")
      end)
      assert_no_lines(t, "read-side FFI conversion wrappers must stay explicit-L only",
                      src, function(line)
        return line_contains_any(line, {
          "lj_cdata_newref(CTState",
          "lj_cdata_index(CTState",
          "lj_cdata_get(CTState",
          "lj_cconv_tv_ct(CTState",
          "lj_cconv_tv_bf(CTState"
        })
      end)
      clean_build(t)
      run_luajit_script(t, "t-ffi-cdata-get-l.lua", {
        getenv("LJ_M7_FFI_GET_THREADS", "6"),
        getenv("LJ_M7_FFI_GET_ITERS", "400")
      }, { joff = true })
      print("M7 FFI cdata explicit-L read guard passed")
    end
  })

  add({
    name = "m7_ffi_cdata_set_l",
    description = "FFI write paths pass active lua_State explicitly",
    run = function(t)
      local src = source_files(t)
      t:assert_all_any_contains(src, {
        "lj_cconv_ct_ct_l(lua_State *L, CTState *cts, CType *d,",
        "CTypeID did, CType *s, CTypeID sid",
        "lj_cconv_ct_tv_l(lua_State *L, CTState *cts, CType *d,",
        "CTypeID did, uint8_t *dp",
        "lj_cconv_bf_tv_l(lua_State *L, CTState *cts",
        "lj_cconv_ct_init_l(lua_State *L, CTState *cts",
        "lj_cdata_set_l(lua_State *L, CTState *cts, CType *d, CTypeID did",
        "cconv_err_convtv_l(lua_State *L",
        "cconv_err_initov_l(lua_State *L",
        "lj_cdata_set_l(L, cts, ct, id",
        "lj_cconv_ct_init_l(L, cts",
        "lj_cconv_ct_tv_l(L, cts, d, did",
        "lj_cconv_bf_tv_l(L, cts",
        "lj_cconv_ct_tv_l(L, cts, ctr, rid",
        "ccall_struct_arg(cc, L, cts, d, did"
      })
      assert_no_lines(t, "write-side FFI conversion wrappers must stay explicit-L only",
                      src, function(line)
        return line_contains_any(line, {
          "lj_cconv_ct_tv(CTState",
          "lj_cconv_bf_tv(CTState",
          "lj_cconv_ct_init(CTState",
          "lj_cdata_set(CTState"
        })
      end)
      assert_no_lines(t, "active-L FFI write call site still uses cts->L wrapper",
                      {
                        t:path("src", "lib_ffi.c"),
                        t:path("src", "lib_base.c"),
                        t:path("src", "lib_buffer.c"),
                        t:path("src", "lib_bit.c"),
                        t:path("src", "lj_ccall.c"),
                        t:path("src", "lj_ccallback.c"),
                        t:path("src", "lj_cdata.c"),
                        t:path("src", "lj_cconv.c")
                      }, function(line)
        return line_contains_any(line, {
          "lj_cconv_ct_tv(cts",
          "lj_cconv_bf_tv(cts",
          "lj_cconv_ct_init(cts",
          "lj_cdata_set(cts"
        })
      end)
      local conv = t:c_block(t:path("src", "lj_cconv.c"),
                             "void lj_cconv_ct_tv_l(lua_State *L, CTState *cts, CType *d,")
      assert_text_not_contains("lj_cconv_ct_tv_l", conv, "ctype_typeid(cts, d)")
      clean_build(t)
      run_luajit_script(t, "t-ffi-cdata-set-l.lua", {
        getenv("LJ_M7_FFI_SET_THREADS", "6"),
        getenv("LJ_M7_FFI_SET_ITERS", "320")
      }, { joff = true })
      print("M7 FFI cdata explicit-L write guard passed")
    end
  })

  add({
    name = "m7_ffi_cdef_dup_stack",
    description = "duplicate ffi.cdef/string-ctype stack-growth race guard",
    run = function(t)
      t:assert_all_any_contains(source_files(t), {
        "lj_state_rehome_stack(lua_State *L)",
        "lj_arena_allocf(&tg->allocd, NULL, 0, sz)",
        "lj_gc2_account_alloc(g, tg, (GCSize)sz)",
        "lj_mem_freevec(g, oldst, stacksize, TValue)",
        "LJ_FUNC int lj_state_rehome_stack(lua_State *L)",
        "if (!lj_state_rehome_stack(L1))",
        "L1->tg_hint = L2TG(L)"
      })
      assert_text_ordered("spawned thread stack must move to worker TG before publication/start",
                          t:read(t:path("src", "lib_threading.c")), {
        "lj_tg_init_thread(G(L), tg, L1",
        "tg->alloc.owner_tid = th->thr.tid",
        "lj_state_rehome_stack(L1)",
        "threading_gc_enter(L)",
        "lj_thr_create(&th->thr"
      })
      t:assert_all_contains(t:path("src", "lj_ctype.c"), {
        "lj_native_enter(L2TG(L))",
        "lj_native_leave(L)",
        "lj_safepoint_checkstop(L, actions)"
      })
      clean_build(t)
      run_luajit_script(t, "t-ffi-cdef-dup-stack.lua", {
        getenv("LJ_M7_FFI_DUP_STACK_ROUNDS", "30"),
        getenv("LJ_M7_FFI_DUP_STACK_ITERS", "200")
      }, { joff = true, timeout = "30s" })
      print("M7 FFI duplicate cdef stack-growth guard passed")
    end
  })

  add({
    name = "m7_ffi_cdef_token",
    description = "parser-driven FFI CTState mutation serialization",
    run = function(t)
      local src = source_files(t)
      t:assert_all_any_contains(src, {
        "uint32_t parse_token",
        "lj_ctype_parse_lock(CTState *cts, lua_State *L)",
        "la_cas32(&cts->parse_token, &expect, 1, LA_ACQ_REL, LA_ACQ)",
        "la_futex_wait(&cts->parse_token, 1, 1000000)",
        "la_store32_rel(&cts->parse_token, 0)",
        "la_futex_wake(&cts->parse_token, 1)",
        "lj_ctype_new_l(cp->L, cp->cts",
        "cp_ctype_intern(cp,",
        "lj_ctype_parse_lock(cts, L)",
        "lj_ctype_parse_lock(cp.cts, L)",
        "lj_ctype_parse_lock(cp.cts, J->L)"
      })
      local count = count_line_hits(t, src, function(line)
        return contains(line, "lj_cparse(&cp)")
      end)
      if count ~= 3 then
        error("expected exactly 3 lj_cparse(&cp) call sites, found " .. count)
      end
      clean_build(t)
      run_luajit_script(t, "t-ffi-cdef-token.lua", {
        getenv("LJ_M7_FFI_CDEF_THREADS", "6"),
        getenv("LJ_M7_FFI_CDEF_ITERS", "120")
      }, { joff = true })
      print("M7 FFI cdef token guard passed")
    end
  })

  add({
    name = "m7_ffi_clib_cache",
    description = "FFI C library cache miss/fill concurrency",
    run = function(t)
      local src = {
        t:path("src", "lj_clib.c"),
        t:path("src", "lj_clib.h"),
        t:path("src", "lj_crecord.c"),
        t:path("src", "lj_gc.c"),
        t:path("src", "lj_gc2.c"),
        t:path("src", "lj_udata.c")
      }
      local impl = {
        t:path("src", "lj_clib.c"),
        t:path("src", "lj_gc.c"),
        t:path("src", "lj_gc2.c")
      }
      t:assert_all_any_contains(src, {
        "CLibCacheEntry *cache_head",
        "lj_clib_cache_next_acq(",
        "lj_clib_cache_next_rel(CLibCacheEntry *e,",
        "lj_clib_cache_name_acq(const CLibCacheEntry *e)",
        "lj_clib_cache_name_rel(CLibCacheEntry *e, GCstr *name)",
        "lj_clib_cache_val_acq(TValue *dst,",
        "lj_clib_cache_val_rel(lua_State *L, CLibCacheEntry *e,",
        "lj_clib_cache_head_acq(const CLibrary *cl)",
        "lj_clib_cache_head_cas_rel(CLibrary *cl,",
        "lj_clib_cache_head_xchg_acqrel(",
        "lj_clib_cache_get(CLibrary *cl, GCstr *name)",
        "clib_cache_publish(lua_State *L, CLibrary *cl, GCstr *name",
        "lj_tv_load_acq(&tv, ctv)",
        "la_casptr((void **)&cl->cache_head",
        "lj_gc_arena_markmem(G(L), e)",
        "lj_cdata_new_l(L, cts, id, CTSIZE_PTR)",
        "lj_gc_barrierroot(L, &e->val)",
        "gc_mark_clib_cache(global_State *g, CLibrary *cl)",
        "gc2_traverse_clib_cache(global_State *g, CLibrary *cl)",
        "lj_clib_unload(g, (CLibrary *)uddata(ud))",
        "lj_clib_cache_get(cl, name)"
      })
      assert_no_lines(t, "clib cache must use side cache without old token/table bridge",
                      src, function(line)
        return line_contains_any(line, {
          "LJ_MT",
          "LUAJIT_THREADSAFE",
          "uint32_t cache_token",
          "clib_cache_lock",
          "clib_cache_unlock",
          "lj_tab_setstr(L, cl->cache",
          "lj_tab_getstr(cl->cache",
          "lj_cdata_new(cts, id, CTSIZE_PTR)"
        })
      end)
      assert_no_lines(t, "clib cache payloads must use shared acquire/release helpers",
                      impl, function(line)
        return line_contains_any(line, {
          "e->name = name",
          "copyTV(L, &e->val",
          "la_loadptr_acq((void *const *)&e->name)",
          "lj_tv_load_acq(&tv, &e->val)"
        })
      end)
      assert_no_lines(t, "clib cache list links must use shared head/next helpers",
                      impl, function(line)
        return line:find("^%s*e%->next%s*=%s*head") ~= nil or
               line:find("[^%w_]e%->next%s*=%s*head") ~= nil or
               contains(line, "la_loadptr_acq((void *const *)&e->next)") or
               contains(line, "la_loadptr_acq((void *const *)&cl->cache_head)") or
               contains(line, "la_xchgptr_acqrel((void **)&cl->cache_head") or
               contains(line, "la_casptr((void **)&cl->cache_head")
      end)
      local rec = t:c_block(t:path("src", "lj_crecord.c"),
                            "void LJ_FASTCALL recff_clib_index")
      assert_text_contains("recff_clib_index", rec, "lj_tv_load_acq(&tv, ctv)")
      for _, bad in ipairs({ "tvisnil(ctv)", "tvisnil(tv)", "cdataV(ctv)", "cdataV(tv)" }) do
        assert_text_not_contains("recff_clib_index", rec, bad)
      end
      clean_build(t)
      run_luajit_script(t, "t-ffi-clib-cache.lua", {
        getenv("LJ_M7_FFI_CLIB_THREADS", "6"),
        getenv("LJ_M7_FFI_CLIB_ITERS", "300")
      }, { joff = true })
      run_luajit_script(t, "t-ffi-clib-cache.lua", {
        getenv("LJ_M7_FFI_CLIB_JIT_THREADS", "2"),
        getenv("LJ_M7_FFI_CLIB_JIT_ITERS", "180")
      })
      print("M7 FFI clib cache guard passed")
    end
  })

  add({
    name = "m7_ffi_cparse_rollback",
    description = "FFI cparser rollback behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-cparse-rollback"),
                      "t-ffi-cparse-rollback.c", { timeout = "20s" })
      run_luajit_script(t, "t-ffi-cparse-rollback-reader.lua", nil, { joff = true })
      run_luajit_script(t, "t-ffi-cparse-rollback-reader.lua")

      clean_build(t, { xcflags = "-DLUAJIT_CTYPE_CHECK_ANCHOR" })
      build_and_run_c(t, t:tmp("lj_t-ffi-cparse-rollback-anchor"),
                      "t-ffi-cparse-rollback.c", { timeout = "20s" })
      run_luajit_script(t, "t-ffi-cparse-rollback-reader.lua", nil, { joff = true })
      run_luajit_script(t, "t-ffi-cparse-rollback-reader.lua")
      run_luajit_script(t, "t-ffi-cdef-token.lua", { "2", "20" }, { joff = true })
      print("M7 FFI cparser rollback behavior passed")
    end
  })

  add({
    name = "m7_ffi_ctype_hash_publish",
    description = "FFI ctype hash-head publication",
    run = function(t)
      t:assert_all_any_contains(source_files(t), {
        "uint32_t hash[CTHASH_SIZE]",
        "ctype_hash_load(CTState *cts, uint32_t h)",
        "la_load32_acq(&cts->hash[h])",
        "ctype_hash_cas(CTState *cts, uint32_t h,",
        "la_cas32(&cts->hash[h], &old, (uint32_t)newid",
        "ctype_hash_try_prepend(CTState *cts, uint32_t h, CType *src,",
        "ctype_hash_prepend(CTState *cts, uint32_t h, CType *src, CTypeID id)",
        "CTypeID head = ctype_hash_load(cts, h)",
        "if (id == 0)",
        "if (dst != src)",
        "*dst = *src",
        "dst->next = (CTypeID1)next",
        "while (!ctype_hash_try_prepend(cts, h, src, id, &head))",
        "ctype_hash_prepend(cts, h, ct, id)",
        "ctype_hash_findname(cts,",
        "ctype_hash_load(cts, ct_hashname(name)), name, tmask)"
      })
      assert_no_lines(t, "ctype hash heads must publish through ctype_hash_prepend",
                      { t:path("src", "lj_ctype.c") }, function(line)
        return line:find("cts%->hash%[[^%]]+%]%s*=") ~= nil
      end)
      assert_no_lines(t, "ctype hash publication must stay CAS-prepend",
                      { t:path("src", "lj_ctype.c") }, function(line)
        return contains(line, "ctype_hash_store") or
               contains(line, "la_store32_rel(&cts->hash")
      end)
      assert_no_lines(t, "ctype hash heads must read through ctype_hash_load",
                      { t:path("src", "lj_ctype.c") }, function(line)
        return (contains(line, "CTypeID") and contains(line, "=") and
                contains(line, "cts->hash[")) or
               (contains(line, "ct->next") and contains(line, "= cts->hash["))
      end)
      clean_build(t)
      run_luajit_script(t, "t-ffi-cdef-token.lua", {
        getenv("LJ_M7_FFI_CDEF_THREADS", "6"),
        getenv("LJ_M7_FFI_CDEF_ITERS", "120")
      }, { joff = true })
      run_luajit_script(t, "t-ffi-cdata-set-l.lua", {
        getenv("LJ_M7_FFI_SET_THREADS", "6"),
        getenv("LJ_M7_FFI_SET_ITERS", "320")
      }, { joff = true })
      run_luajit_script(t, "t-ffi-metatype-miscmap.lua", {
        getenv("LJ_M7_FFI_META_THREADS", "6"),
        getenv("LJ_M7_FFI_META_ITERS", "60")
      }, { joff = true })
      print("M7 FFI ctype hash publication guard passed")
    end
  })

  add({
    name = "m7_ffi_ctype_intern_l",
    description = "FFI ctype allocation/interning passes active lua_State explicitly",
    run = function(t)
      local src = source_files(t)
      t:assert_all_any_contains(src, {
        "lj_ctype_new_l(lua_State *L, CTState *cts",
        "lj_ctype_intern_l(lua_State *L, CTState *cts",
        "lj_ctype_intern_new_l(lua_State *L, CTState *cts",
        "lj_ccall_ctid_vararg(lua_State *L, CTState *cts",
        "lj_ctype_new_l(cp->L, cp->cts",
        "cp_ctype_intern(cp,",
        "lj_ctype_intern_l(L, cts",
        "lj_ctype_intern_l(J->L, cts",
        "lj_ccall_ctid_vararg(L, cts",
        "lj_ccall_ctid_vararg(J->L, cts"
      })
      assert_no_lines(t, "ctype allocation/interning must not route through CTState L",
                      src, function(line)
        return line_contains_any(line, {
          "cts->L",
          "parse_L",
          "lj_ctype_new(",
          "lj_ctype_intern("
        })
      end)
      clean_build(t)
      run_luajit_script(t, "t-ffi-cdef-token.lua", {
        getenv("LJ_M7_FFI_CDEF_THREADS", "6"),
        getenv("LJ_M7_FFI_CDEF_ITERS", "120")
      }, { joff = true })
      run_luajit_script(t, "t-ffi-cdata-set-l.lua", {
        getenv("LJ_M7_FFI_SET_THREADS", "6"),
        getenv("LJ_M7_FFI_SET_ITERS", "320")
      }, { joff = true })
      run_luajit_script(t, "t-ffi-carith-l.lua", nil, { joff = true })
      print("M7 FFI ctype explicit-L intern guard passed")
    end
  })

  add({
    name = "m7_ffi_ctype_name_claim",
    description = "FFI ctype duplicate-name publication",
    run = function(t)
      local files = source_files(t)
      files[#files + 1] = t:path("tests", "t-ffi-ctype-name-claim.c")
      t:assert_all_any_contains(files, {
        "ctype_hash_findname(CTState *cts, CTypeID id, GCstr *name,",
        "lj_ctype_addname_unique(CTState *cts, CType *ct, CTypeID id,",
        "ctype_hash_findname(cts, head, name, tmask)",
        "ctype_abandon(cts, id)",
        "return winner;  /* 11.2 named ctype duplicate winner. */",
        "return id;  /* 11.2 CAS-prepend named ctype publication. */",
        "lj_ctype_addname_unique(cp->cts, ct, sid,",
        "lj_ctype_addname_unique(cp->cts, ct, constid, CPNS_DEFAULT)",
        "lj_ctype_addname_unique(cp->cts, ct, id, CPNS_DEFAULT)",
        "force_table_move_after_reserve(lua_State *L, CTState *cts)",
        "assert(ct3 != ctype_get(cts, id3))",
        "ffi.typeinfo exposed abandoned ctype",
        "parser struct tag namespace was shadowed",
        "parser typedef namespace was shadowed",
        "parser duplicate enum constant was accepted",
        "parser duplicate enum loser replaced winner"
      })
      assert_no_lines(t, "parser global name publication must use duplicate-aware claim helper",
                      { t:path("src", "lj_cparse.c") }, function(line)
        return contains(line, "lj_ctype_addname(cp->cts")
      end)
      local lib_ffi = t:path("src", "lib_ffi.c")
      local storeint = t:c_block(lib_ffi, "static void ffi_typeinfo_storeint(lua_State *L,")
      assert_text_not_contains("ffi_typeinfo_storeint", storeint, "lj_tab_storeint(L, dst,")
      for _, needle in ipairs({
        "for (;;)",
        "setintV(&tv, val)",
        "lj_tab_setstr(L, tab, key)",
        "lj_tab_trystoretv_cas(L, dst, &tv) == LJ_TAB_STORE_CAS_OK",
        "FFI typeinfo int store saw FORWARD after lookup."
      }) do
        assert_text_contains("ffi_typeinfo_storeint", storeint, needle)
      end
      local storestr = t:c_block(lib_ffi, "static void ffi_typeinfo_storestr(lua_State *L,")
      assert_text_not_contains("ffi_typeinfo_storestr", storestr, "lj_tab_storestr(L, dst,")
      for _, needle in ipairs({
        "for (;;)",
        "setstrV(L, &tv, val)",
        "lj_tab_setstr(L, tab, key)",
        "lj_tab_trystoretv_cas(L, dst, &tv) == LJ_TAB_STORE_CAS_OK",
        "FFI typeinfo string store saw FORWARD after lookup."
      }) do
        assert_text_contains("ffi_typeinfo_storestr", storestr, needle)
      end
      local typeinfo = t:c_block(lib_ffi, "LJLIB_CF(ffi_typeinfo)")
      assert_text_not_contains("ffi_typeinfo", typeinfo, "lj_tab_storeint(L, lj_tab_setstr")
      assert_text_not_contains("ffi_typeinfo", typeinfo, "lj_tab_storestr(L, lj_tab_setstr")
      assert_text_ordered("ffi_typeinfo", typeinfo, {
        "ctype_isabandoned(info)",
        "lua_createtable(L, 0, 4)",
        "ffi_typeinfo_storeint(L, t, lj_str_newlit(L, \"info\"), (int32_t)info)",
        "lj_gc_pubtab(L, t)"
      })
      for _, needle in ipairs({
        "ffi_typeinfo_storeint(L, t, lj_str_newlit(L, \"size\"), (int32_t)size)",
        "ffi_typeinfo_storeint(L, t, lj_str_newlit(L, \"sib\"), (int32_t)sib)",
        "ffi_typeinfo_storestr(L, t, lj_str_newlit(L, \"name\"), name)"
      }) do
        assert_text_contains("ffi_typeinfo", typeinfo, needle)
      end
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-ctype-name-claim"),
                      "t-ffi-ctype-name-claim.c", { timeout = "20s" })
      run_luajit_script(t, "t-ffi-cdef-dup-stack.lua", {
        getenv("LJ_M7_FFI_CDEF_DUP_ROUNDS", "30"),
        getenv("LJ_M7_FFI_CDEF_DUP_ITERS", "200")
      }, { joff = true })
      print("M7 FFI ctype name-claim guard passed")
    end
  })

  add({
    name = "m7_ffi_ctype_pointer_ids",
    description = "FFI ctype pointer-stability boundary behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-ctype-pointer-ids.lua", nil, { joff = true })
      clean_build(t, { xcflags = "-DLUAJIT_CTYPE_CHECK_ANCHOR" })
      run_luajit_script(t, "t-ffi-ctype-pointer-ids.lua", nil, { joff = true })
      run_luajit_script(t, "t-ffi-cdata-set-l.lua", {
        "1",
        getenv("LJ_M7_FFI_SET_ITERS", "80")
      }, { joff = true })
      print("M7 FFI ctype pointer-stability behavior passed")
    end
  })

  add({
    name = "m7_ffi_ctype_tab_retire",
    description = "FFI ctype-table RCU growth and SMR retirement",
    run = function(t)
      t:assert_all_any_contains(source_files(t), {
        "typedef struct CTypeTab",
        "CTypeTab *tabh",
        "CTypeTab *retiredtab",
        "ctype_tabh_acq(CTState *cts)",
        "ctype_tab_acq(CTState *cts)",
        "la_loadptr_acq((void *const *)&cts->tabh)",
        "ctype_tab_grow_l(lua_State *L, CTState *cts, CTypeID id)",
        "la_casptr((void **)&cts->tabh, &expect, newh",
        "ctype_tab_retire(cts, oldh)",
        "lj_ctype_reclaim_retired(global_State *g, uint64_t completed_epoch)",
        "lj_ctype_reclaim_retired(g, epoch)",
        "lj_gc2_markmem(g, ctype_tabh_acq(cts))",
        "lj_gc_arena_markmem(g, ctype_tabh_acq(cts))",
        "gc2_paranoia_checkmem(g, ctype_tabh_acq(cts), \"ctype table\")"
      })
      assert_no_lines(t, "ctype table growth must publish/retire, not realloc/free in place",
                      { t:path("src", "lj_ctype.c") }, function(line)
        return line_contains_any(line, {
          "lj_mem_growvec(L, cts->tab",
          "lj_mem_freevec(cts->g, cts->tab",
          "la_storeptr_rel((void **)&cts->tab,"
        })
      end)
      local cts = t:text_between(t:path("src", "lj_ctype.h"),
                                 "typedef struct CTState", "} CTState;")
      if contains(cts, "CType *tab") or contains(cts, "MSize sizetab") then
        error("CTState must not keep ctype table mirror fields")
      end
      clean_build(t, { xcflags = "-DLUAJIT_CTYPE_CHECK_ANCHOR" })
      build_and_run_c(t, t:tmp("lj_t-ffi-ctype-tab-retire"),
                      "t-ffi-ctype-tab-retire.c", { timeout = "20s" })
      print("M7 FFI ctype table-retirement guard passed")
    end
  })

  add({
    name = "m7_ffi_ctype_ticket_intern",
    description = "FFI ctype ticket allocation and duplicate-aware interning behavior",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-ffi-ctype-ticket-intern"),
                      "t-ffi-ctype-ticket-intern.c", { timeout = "20s" })
      run_luajit_script(t, "t-ffi-ctype-intern-race.lua", {
        getenv("LJ_M7_FFI_INTERN_THREADS", "6"),
        getenv("LJ_M7_FFI_INTERN_SHAPES", "48"),
        getenv("LJ_M7_FFI_INTERN_ROUNDS", "4")
      }, { joff = true })
      print("M7 FFI ctype ticket/intern behavior passed")
    end
  })

  add({
    name = "m7_ffi_finreg",
    description = "FFI cdata finalizer registry behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-gc-finreg.lua", {
        getenv("LJ_M7_FFI_FIN_THREADS", "6"),
        getenv("LJ_M7_FFI_FIN_ITERS", "240")
      }, { joff = true })
      run_luajit_script(t, "t-ffi-gc-finreg.lua", {
        getenv("LJ_M7_FFI_FIN_THREADS", "6"),
        getenv("LJ_M7_FFI_FIN_ITERS", "240")
      })
      run_luajit_script(t, "t-ffi-gc-trace.lua", nil, {
        timeout = "20s",
        env = { LUA_PATH = lua_path(t) }
      })
      local dump = t:tmp("lj_t-ffi-gc-trace.dump")
      t:run("LUA_PATH=" .. shell_quote(lua_path(t)) .. " timeout 20s " ..
            shell_quote(t:path("src", "luajit")) .. " -jdump=ir " ..
            shell_quote(t:path("tests", "t-ffi-gc-trace.lua")) ..
            " >" .. shell_quote(dump))
      t:assert_contains(dump, "lj_cdata_setfin")
      print("M7 FFI finalizer registry behavior passed")
    end
  })

  add({
    name = "m7_ffi_jit_cnew",
    description = "x64 JIT CNEW/CNEWI cdata publication behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-jit-cnew-alloc.lua", nil, {
        timeout = "20s",
        env = { LUA_PATH = lua_path(t) }
      })

      local dump = t:tmp("lj_t-ffi-jit-cnew.dump")
      local dumpi = t:tmp("lj_t-ffi-jit-cnewi.dump")
      run_dump_probe(t, dump, [[
local ffi = require"ffi"
ffi.cdef"typedef struct { int x; double y; } lj_m7_jit_dump_t;"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local struct_t = ffi.typeof("lj_m7_jit_dump_t")
local int64_t = ffi.typeof("int64_t")
local function make(n)
  local sum = 0
  for i = 1, n do
    local obj = struct_t(i, i + 0.25)
    local i64 = int64_t(i)
    sum = sum + obj.x + tonumber(i64)
  end
  return sum
end
for _ = 1, 30 do assert(make(80) == 6480) end
print("dump cnew ok")
]])
      run_dump_probe(t, dumpi, [[
local ffi = require"ffi"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local int64_t = ffi.typeof("int64_t")
local function make(n)
  local v = int64_t(0)
  for i = 1, n do v = int64_t(i) end
  return v
end
for _ = 1, 30 do assert(tonumber(make(80)) == 80) end
print("dump cnewi ok")
]])
      t:assert_contains(dump, "CNEW")
      t:assert_contains(dumpi, "CNEWI")
      t:assert_contains(dump, "dump cnew ok")
      t:assert_contains(dumpi, "dump cnewi ok")

      clean_build(t, { xcflags = "-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1" })
      t:run("(cd " .. shell_quote(t:path("tests", "stock", "test")) ..
            " && LUA_PATH=" .. shell_quote(lua_path(t)) ..
            " timeout 20s " .. shell_quote(t:path("src", "luajit")) ..
            " test.lua --quiet 340 341 358)")
      print("M7 FFI JIT CNEW allocation behavior passed")
    end
  })

  add({
    name = "m7_ffi_metatype",
    description = "FFI metatype side-map CAS publication",
    run = function(t)
      local src = source_files(t)
      t:assert_all_any_contains(src, {
        "GCRef *metamap",
        "MSize sizemeta",
        "ctype_metamap_init_l(lua_State *L, CTState *cts)",
        "lj_ctype_setmeta(CTState *cts, CTypeID id, GCtab *mt)",
        "la_cas64(&meta[id].gcptr64, &expect,",
        "ctype_meta_tab(CTState *cts, CTypeID id)",
        "lj_gc_barrierroot(L, &tmp);  /* 11.2 metatype side root. */",
        "lj_gc_arena_markmem(g, cts->metamap)",
        "lj_gc2_markmem(g, cts->metamap)",
        "gc_markobj(g, o)",
        "lj_gc2_markobj(g, o)",
        "lj_mem_freevec(g, cts->metamap, cts->sizemeta, GCRef)",
        "lj_err_caller(L, LJ_ERR_PROTMT)"
      })
      local meta = t:text_between(t:path("src", "lib_ffi.c"),
                                  "LJLIB_CF(ffi_metatype)", "LJLIB_CF(ffi_gc)")
      for _, bad in ipairs({
        "lj_ctype_misc_lock(cts)",
        "lj_ctype_misc_unlock(cts)",
        "lj_tab_setinth(L, t, -(int32_t)rid)"
      }) do
        assert_text_not_contains("ffi_metatype", meta, bad)
      end
      assert_text_contains("ffi_metatype", meta, "lj_ctype_setmeta(cts, rid, mt)")
      assert_text_contains("ffi_metatype", meta, "lj_gc_barrierroot(L, &tmp)")
      assert_no_lines(t, "metatype lookup/install must not use structural miscmap negative keys",
                      {
                        t:path("src", "lj_ctype.c"),
                        t:path("src", "lib_ffi.c")
                      }, function(line)
        return contains(line, "lj_tab_getinth(cts->miscmap, -") or
               contains(line, "tv = lj_tab_setinth(L, t, -(int32_t)rid)")
      end)
      t:assert_contains(t:path("tests", "t-ffi-metatype-miscmap.lua"),
                        "typedef struct { int x; } lj_m7_meta_root_t")
      clean_build(t)
      run_luajit_script(t, "t-ffi-metatype-miscmap.lua", {
        getenv("LJ_M7_FFI_META_THREADS", "6"),
        getenv("LJ_M7_FFI_META_ITERS", "60")
      }, { joff = true })
      print("M7 FFI metatype/miscmap guard passed")
    end
  })

  add({
    name = "m7_ffi_pin",
    description = "ffi.pin root publication and release behavior",
    run = function(t)
      clean_build(t)
      run_luajit_script(t, "t-ffi-pin.lua", {
        getenv("LJ_M7_FFI_PIN_THREADS", "4"),
        getenv("LJ_M7_FFI_PIN_ITERS", "80")
      }, { joff = true })
      print("M7 ffi.pin behavior passed")
    end
  })

  add({
    name = "m7_ffi_snap_restore_l",
    description = "FFI snapshot restore cdata allocation passes active lua_State",
    run = function(t)
      t:assert_all_any_contains(source_files(t), {
        "lj_cdata_newx_l(L, cts, id, sz, info)",
        "GCcdata *lj_cdata_newx_l(lua_State *L, CTState *cts"
      })
      assert_no_lines(t, "cdata allocation wrappers must stay explicit-L only",
                      {
                        t:path("src", "lj_cdata.h"),
                        t:path("src", "lj_cdata.c"),
                        t:path("src", "lj_snap.c")
                      }, function(line)
        return line_contains_any(line, {
          "lj_cdata_new(CTState",
          "lj_cdata_newx(CTState",
          "lj_cdata_new(cts",
          "lj_cdata_newx(cts"
        })
      end)
      clean_build(t)
      run_luajit_script(t, "t-ffi-snap-restore-l.lua", nil, {
        timeout = "20s",
        env = { LUA_PATH = lua_path(t) }
      })
      print("M7 FFI snapshot restore explicit-L guard passed")
    end
  })

  add({
    name = "m7_ffi",
    description = "M7 FFI aggregate concurrency gates",
    run = function(t)
      local cmd = { t:path("tools", "ci", "lua_test.sh") }
      for i = 1, #m7_cases do cmd[#cmd + 1] = m7_cases[i] end
      t:run(cmd)
      print("M7 FFI gates passed")
    end
  })
end
