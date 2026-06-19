local utils = require("suite_utils")

local contains = utils.contains
local assert_no_lines = utils.assert_no_lines

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

local function source_files(t)
  return t:files(t:path("src"), {
    extensions = { ".c", ".h", ".dasc" }
  })
end

local function compile_luajit_fixture(t, out, cfile, opts)
  opts = opts or {}
  t:cc(out, { t:path("tests", cfile) }, {
    cflags = opts.cflags,
    link_luajit = true,
    libs = { "-lm", "-ldl", "-pthread" }
  })
end

local function run_c_fixtures(t, suffix, cflags)
  for _, name in ipairs({
    "t-gc2-phase",
    "t-gc2-traverse",
    "t-m8-ffi-weak-newindex",
    "t-m8-close-finalizers",
    "t-m8-finalizer-state"
  }) do
    local out = t:tmp("lj_" .. name .. suffix)
    compile_luajit_fixture(t, out, name .. ".c", { cflags = cflags })
    t:run({ out })
  end
end

local function assert_preclaim_consumers(t)
  for _, item in ipairs({
    { t:path("src", "lj_gc.c"),
      "static void gc_mark_finreg_cdata_preclaims(global_State *g)" },
    { t:path("src", "lj_gc2.c"),
      "static void gc2_mark_finreg_cdata_preclaims(global_State *g)" },
    { t:path("src", "lj_gc2.c"),
      "int lj_gc2_finreg_cdata_preclaim_take(lua_State *L, global_State *g," }
  }) do
    local label = item[2]
    local block = t:c_block(item[1], item[2])
    assert_block_contains(label, block, "gc2_finreg_cdata_preclaim_ready(g)")
    if not (contains(block, "gc2_finreg_cdata_preclaim_obj_acq(g, i)") or
            contains(block, "gc2_finreg_cdata_preclaim_obj_acq(g, head)")) then
      error(label .. ": missing preclaim object acquire helper", 2)
    end
    if not (contains(block, "gc2_finreg_cdata_preclaim_fin_acq(g, i, &fin)") or
            contains(block, "gc2_finreg_cdata_preclaim_fin_acq(g, i, fin)")) then
      error(label .. ": missing preclaim finalizer acquire helper", 2)
    end
    assert_block_absent(label, block,
      "gcref_acq(g->gc2.finreg_cdata_preclaim_obj[")
    assert_block_absent(label, block,
      "lj_tv_load_acq(&fin, &g->gc2.finreg_cdata_preclaim_fin[")
  end
end

local function assert_chain_unlinks(t)
  local lj_gc = t:path("src", "lj_gc.c")
  local splice = t:c_block(lj_gc, "static int gc_chain_splice")
  if not splice:find("la_cas%d+%(&p%->gcptr") then
    error("gc_chain_splice must CAS-splice root/finalizer lists", 2)
  end

  for _, start in ipairs({
    "static int gc_unlink_udata_object(global_State *g, GCobj *target)",
    "static int gc_unlink_root_object(global_State *g, GCobj *target)"
  }) do
    local block = t:c_block(lj_gc, start)
    assert_block_contains(start, block, "gcref_acq(*p)")
    assert_block_contains(start, block, "gc_chain_splice(p, o)")
    assert_block_absent(start, block,
      "setgcrefr(*p, *lj_obj_gcwref(o))")
    assert_block_absent(start, block,
      "setgcrefrel(*p, *lj_obj_gcwref(o))")
  end

  assert_no_lines(t, "userdata chain publications must use lj_gc_linkobj_after()",
                  { t:path("src", "lj_gc.c"), t:path("src", "lj_udata.c") },
                  function(line)
    return contains(line, "setgcref(*lj_obj_gcwref(obj2gco(mainthread(g)))") or
           contains(line, "lj_obj_setgcwr(obj2gco(ud), *lj_obj_gcwref(obj2gco(mainthread(g)))") or
           contains(line, "lj_obj_setgcwr(o, *lj_obj_gcwref(obj2gco(mainthread(g)))")
  end)

  local link_after = t:c_block(lj_gc, "void lj_gc_linkobj_after")
  assert_block_contains("lj_gc_linkobj_after", link_after, "gcref_acq(*p)")
  if not link_after:find("la_cas%d+%(&p%->gcptr") then
    error("lj_gc_linkobj_after must CAS-insert after anchor", 2)
  end
end

local function assert_library_registration(t)
  local lj_lib = t:path("src", "lj_lib.c")
  local str = t:c_block(lj_lib, "static TValue *lib_storefunc_str")
  assert_block_absent("lib_storefunc_str", str, "lj_tab_storefunc(L, dst, fn)")
  assert_block_contains("lib_storefunc_str", str, "for (;;) {")
  assert_block_contains("lib_storefunc_str", str, "setfuncV(L, &tv, fn)")
  assert_block_contains("lib_storefunc_str", str, "lj_tab_setstr(L, tab, key)")
  assert_block_contains("lib_storefunc_str", str,
                        "lj_tab_trystoretv_cas(L, dst, &tv) == LJ_TAB_STORE_CAS_OK")
  assert_block_contains("lib_storefunc_str", str,
                        "Library string store saw FORWARD after lookup.")

  local generic = t:c_block(lj_lib, "static TValue *lib_storetv_key")
  assert_block_absent("lib_storetv_key", generic, "copyTVrel(L, dst, val)")
  assert_block_contains("lib_storetv_key", generic, "for (;;) {")
  assert_block_contains("lib_storetv_key", generic, "lj_tab_set(L, tab, key)")
  assert_block_contains("lib_storetv_key", generic,
                        "lj_tab_trystoretv_cas(L, dst, val) == LJ_TAB_STORE_CAS_OK")
  assert_block_contains("lib_storetv_key", generic,
                        "Library generic store saw FORWARD after lookup.")

  local read_lfunc = t:c_block(lj_lib, "static const uint8_t *lib_read_lfunc")
  assert_block_absent("lib_read_lfunc", read_lfunc,
                      "lj_tab_storefunc(L, lj_tab_setstr(L, tab, name), fn)")
  assert_block_contains("lib_read_lfunc", read_lfunc,
                        "lib_storefunc_str(L, tab, name, fn)")
  assert_block_contains("lib_read_lfunc", read_lfunc,
                        "lib_weak_write_str(L, tab, name, slot)")

  local register = t:c_block(lj_lib, "void lj_lib_register")
  assert_block_absent("lj_lib_register", register,
                      "lj_tab_storefunc(L, lj_tab_setstr(L, tab,")
  assert_block_absent("lj_lib_register", register,
                      "copyTVrel(L, lj_tab_set(L, tab, L->top+1), L->top)")
  assert_block_contains("lj_lib_register", register,
                        "lib_storefunc_str(L, tab, key, fn)")
  assert_block_contains("lj_lib_register", register,
                        "lib_storetv_key(L, tab, L->top+1, L->top)")
  assert_block_contains("lj_lib_register", register,
                        "lj_gc2_barrier_weak_write(L, tab, L->top+1, L->top)")
  assert_block_contains("lj_lib_register", register,
                        "lib_weak_write_str(L, tab, key, slot)")
end

local function assert_ffi_registration(t)
  local lib_ffi = t:path("src", "lib_ffi.c")
  local store = t:c_block(lib_ffi, "static TValue *ffi_loaded_store")
  assert_block_absent("ffi_loaded_store", store, "copyTVrel(L, dst, src)")
  assert_block_contains("ffi_loaded_store", store, "for (;;) {")
  assert_block_contains("ffi_loaded_store", store, "lj_tab_setstr(L, t, name)")
  assert_block_contains("ffi_loaded_store", store,
                        "lj_tab_trystoretv_cas(L, dst, src) == LJ_TAB_STORE_CAS_OK")
  assert_block_contains("ffi_loaded_store", store,
                        "FFI module registry saw FORWARD after lookup.")

  local register = t:c_block(lib_ffi, "static void ffi_register_module")
  assert_block_absent("ffi_register_module", register,
                      "copyTVrel(L, lj_tab_setstr(L, t, name), L->top-1)")
  assert_block_contains("ffi_register_module", register,
                        "ffi_loaded_store(L, t, name, L->top-1)")
  assert_block_contains("ffi_register_module", register,
                        "lj_gc2_barrier_weak_write(L, t, &key, L->top-1)")
  assert_block_contains("ffi_register_module", register, "lj_gc_pubtab(L, t)")
end

local function assert_finreg_preclaim_order(t)
  local lj_gc2 = t:path("src", "lj_gc2.c")
  local publish = t:c_block(lj_gc2, "static void gc2_finclaim_publish")
  t:assert_text_ordered("gc2_finclaim_publish", publish, {
    "copyTVrel(L, &g->gc2.finreg_cdata_preclaim_fin[idx], fin);",
    "setgcrefrel(g->gc2.finreg_cdata_preclaim_obj[idx], o);"
  })
end

local function assert_finalizer_dispatch(t)
  local lj_gc = t:path("src", "lj_gc.c")
  local call = t:c_block(lj_gc, "static int gc_call_finalizer")
  assert_no_lines(t, "gc_call_finalizer must not assign vmthread(g)",
                  { lj_gc }, function(line)
    return call:find(line, 1, true) and
           line:match("lua_State%s+%*[^=]+=%s*vmthread%(%s*g%s*%)")
  end)

  local close = t:c_block(t:path("src", "lj_state.c"),
                          "LUA_API void lua_close")
  t:assert_text_ordered("lua_close finalizer drain", close, {
    "lj_vm_cpcall(L, NULL, NULL, cpfinalize) == LUA_OK",
    "!lj_gc2_finalizer_queue_pending(g)",
    "!lj_gc_cdata_fin_pending(g)",
    "break;"
  })

  local cdata = t:c_block(lj_gc, "if (o->gch.gct == ~LJ_TCDATA)")
  assert_block_contains("cdata finalizer dispatch", cdata,
                        "gc_finalize_cdata_slot_owned(L, o, &key)")
  assert_block_absent("cdata finalizer dispatch", cdata,
                      "gc_call_finalizer(g, L,")

  local close_cdata = t:c_block(lj_gc, "void lj_gc_finalize_cdata")
  assert_block_absent("lj_gc_finalize_cdata", close_cdata,
                      "gc_call_finalizer(g, L,")
end

local function assert_source_predicates(t)
  assert_no_lines(t, "ordered FINREG object payload must use acquire/release helpers",
                  { t:path("src", "lj_ctype.c"), t:path("src", "lj_gc.c") },
                  function(line)
    return contains(line, "setgcref(ord->obj") or
           contains(line, "setgcrefnull(ord->obj") or
           contains(line, "gcref_acq(ord->obj")
  end)

  assert_no_lines(t, "userdata FINREG object payload must use acquire/release helpers",
                  { t:path("src", "lj_gc.c"), t:path("src", "lj_gc2.c") },
                  function(line)
    return contains(line, "gcref(node->obj") or
           contains(line, "gcref_acq(node->obj") or
           contains(line, "setgcref(node->obj") or
           contains(line, "setgcrefrel(node->obj") or
           contains(line, "setgcrefnull(node->obj") or
           contains(line, "setgcrefnullrel(node->obj")
  end)

  assert_preclaim_consumers(t)
  assert_chain_unlinks(t)
  assert_library_registration(t)
  assert_ffi_registration(t)

  assert_no_lines(t, "FINREG ordered discovery must not retain generation/root pending scans",
                  { t:path("src", "lj_gc.c") }, function(line)
    return contains(line, "gc_preclaim_cdata_finalizers_pweak_finreg") or
           contains(line, "gc_preclaim_cdata_finalizers_pweak_tab") or
           contains(line, "gc_cdata_finreg_pending_scan") or
           contains(line, "gc_cdata_fin_pending_tab") or
           contains(line, "gc_separate_cdata_finalizers_root") or
           contains(line, "gc_claim_cdata_finalizer_pweak") or
           line:match("finreg_cdata_pweak_root_fallbacks,%s*1") or
           line:match("ord%s*==%s*NULL")
  end)

  assert_finreg_preclaim_order(t)
  assert_finalizer_dispatch(t)

  assert_no_lines(t, "runtime finalizer queues must not use legacy mmudata",
                  source_files(t), function(line)
    return contains(line, "mmudata")
  end)
end

local function run_default_matrix(t)
  t:build({ clean = true, quiet = true })
  t:luajit({ "-joff", t:path("tests", "t-weak-modes.lua") })
  t:luajit({ t:path("tests", "t-weak-modes.lua") })
  t:luajit({ "-joff", t:path("tests", "t-m8-finalizer-spawn-live.lua") },
            { timeout = "10s" })
  t:luajit({ t:path("tests", "t-m8-finalizer-spawn-live.lua") },
            { timeout = "10s" })
  t:luajit({ "-joff", t:path("tests", "t-ffi-gc-finreg.lua"), "3", "72" })
  t:luajit({ t:path("tests", "t-ffi-gc-finreg.lua"), "3", "72" })
  run_c_fixtures(t, "_m8")
end

local function run_paranoia_matrix(t)
  local xcflags = "-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1"
  t:build({ clean = true, quiet = true, xcflags = xcflags })
  t:luajit({ "-joff", t:path("tests", "t-weak-modes.lua") }, {
    env = {
      LJ_M8_WEAK_RACE_ITERS = "0",
      LJ_M8_FINALIZER_SPAWN = "0"
    }
  })
  run_c_fixtures(t, "_m8_paranoia", xcflags)
end

return function(add)
  add({
    name = "m8_weak",
    description = "M8 weak-table/finalizer semantic gates",
    run = function(t)
      assert_source_predicates(t)
      run_default_matrix(t)
      run_paranoia_matrix(t)
      print("M8 weak/finalizer semantic gates passed")
    end
  })
end
