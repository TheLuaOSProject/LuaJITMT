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

local function ctype_name_smoke()
  return [=[
local ffi = require"ffi"
for i = 1, 40 do
  ffi.cdef(([[typedef struct { int a; double b; } lj_ctype_name_s_%d;
typedef enum { LJ_CTYPE_NAME_E_%d = %d } lj_ctype_name_e_%d;]]):format(i, i, i, i))
  local ct = ffi.typeof(("lj_ctype_name_s_%d"):format(i))
  local x = ct(i, i + 0.5)
  assert(x.a == i and x.b == i + 0.5)
  local et = ffi.typeof(("lj_ctype_name_e_%d"):format(i))
  assert(tonumber(et(i)) == i)
  collectgarbage("collect")
end
local mt = ffi.metatype("struct { int x; }", {
  __index = { value = function(self) return self.x end }
})
assert(mt(7):value() == 7)
print("ctype-name-publish-smoke OK")
]=]
end

local function jit_hash_store_smoke()
  return [[
local util = require("jit.util")

local function traces()
  local n = 0
  for i = 1, 200 do
    if util.traceinfo(i) then n = n + 1 end
  end
  return n
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = 0 }
for i = 1, 200 do
  h.stable = i
end
assert(h.stable == 200)
assert(traces() > 0, "existing hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local a = { 0 }
for i = 1, 200 do
  a[1] = i
end
assert(a[1] == 200)
assert(traces() > 0, "existing array table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local hn = {}
for i = 1, 200 do
  hn["k" .. i] = i
end
assert(hn.k200 == 200)
assert(traces() > 0, "new string hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function array_insert(n)
  local out = { 0 }
  for i = 1, n do
    local an = {}
    an[1] = i
    out = an
  end
  return out
end
local an = array_insert(80)
assert(an[1] == 80)
assert(traces() > 0, "fresh array slot table store did not trace")
]]
end

local function jit_href_node_order_smoke()
  return [[
jit.opt.start("hotloop=1")
local t, keys = {}, {}
for i = 1, 128 do
  local k = "dyn" .. i
  keys[i] = k
  t[k] = i
end
local sum = 0
for i = 1, 800 do
  local k = keys[(i % 128) + 1]
  sum = sum + (t[k] or 0)
end
assert(sum > 0)
]]
end

return function(add)
  add({
    name = "m5_buffer_publish",
    description = "string.buffer acquire/release publication and thread smoke",
    run = function(t)
      t:build({ quiet = true })

      local lj_buf_h = t:read(t:path("src", "lj_buf.h"))
      if not (contains(lj_buf_h, "la_loadptr_acq") or
              contains(lj_buf_h, "la_storeptr_rel") or
              contains(lj_buf_h, "lj_bufx_data_acq")) then
        error("lj_buf.h must expose acquire/release buffer accessors")
      end

      assert_no_lines(t, "shared string.buffer users must use lj_buf accessors",
                      {
                        t:path("src", "lib_buffer.c"),
                        t:path("src", "lib_base.c"),
                        t:path("src", "lj_meta.c"),
                        t:path("src", "lj_serialize.c"),
                        t:path("src", "lj_cconv.c")
                      }, function(line)
        return contains(line, "sbx->r") or contains(line, "sbx->w") or
               contains(line, "sbx->b") or contains(line, "sbx->e")
      end)

      t:assert_contains(t:path("tools", "ci", "m5_concurrent_objects.sh"),
                        "m5_buffer_publish.sh")
      t:luajit({ "-joff", t:path("tests", "t-buffer-thread-safety.lua") })
      t:luajit({ "-jon", t:path("tests", "t-buffer-thread-safety.lua") })
      print("M5 string.buffer publication guard passed")
    end
  })

  add({
    name = "m5_ctype_name_publish",
    description = "CType.name acquire/release publication guard and smoke test",
    run = function(t)
      t:assert_all_contains(t:path("src", "lj_ctype.h"), {
        "ctype_name_acq(const CType *ct)",
        "gcref_acq(ct->name)",
        "setgcrefrel(ct->name, obj2gco(s))",
        "ctype_clearname(CType *ct)",
        "setgcrefnullrel(ct->name)"
      })

      assert_no_lines(t, "CType.name readers/writers must use helpers",
                      {
                        t:path("src", "lj_ctype.h"),
                        t:path("src", "lj_ctype.c"),
                        t:path("src", "lj_cparse.c"),
                        t:path("src", "lj_cconv.c"),
                        t:path("src", "lj_clib.c"),
                        t:path("src", "lj_crecord.c"),
                        t:path("src", "lib_ffi.c")
                      }, function(line)
        return (contains(line, "gcref(") and contains(line, "->name")) or
               contains(line, "gcref(ct->name") or
               (contains(line, "setgcref(") and contains(line, "->name")) or
               contains(line, "setgcref(ct->name") or
               contains(line, "setgcrefnull(ct->name") or
               (contains(line, "gco2str(gcref(") and contains(line, "->name"))
      end)

      t:assert_contains(t:path("tools", "ci", "m5_concurrent_objects.sh"),
                        "m5_ctype_name_publish.sh")
      t:build({ quiet = true })
      t:luajit({ "-joff", "-e", ctype_name_smoke() })
      print("M5 CType.name publication guard passed")
    end
  })

  add({
    name = "m5_jit_hash_store_nyi",
    description = "JIT table-store bridge smoke and stale NYI guards",
    run = function(t)
      t:build({ clean = true, quiet = true })
      t:luajit({ "-e", jit_hash_store_smoke() })

      local record = t:path("src", "lj_record.c")
      t:assert_contains(record,
                        "M6: numeric NEWREF/HSTORE uses the generic returned-slot helper")
      t:assert_contains(record,
                        "M6: previous-nil in-bounds ASTORE/HSTORE uses the helper bridge")
      for _, needle in ipairs({
        "M6: no new/nil HSTORE bridge",
        "M6: no new HSTORE bridge",
        "M6: no numeric new HSTORE bridge",
        "M6: no nil ASTORE bridge"
      }) do
        t:assert_not_contains(record, needle)
      end
      print("M5 JIT table-store bridge guard passed")
    end
  })

  add({
    name = "m5_jit_href_node_order",
    description = "x64 JIT HREF table node/hmask load ordering guard",
    run = function(t)
      t:build({ clean = true, quiet = true })
      t:luajit({ "-e", jit_href_node_order_smoke() })

      local asm = t:path("src", "lj_asm_x86.h")
      t:assert_all_contains(asm, {
        "TABNODE_HMASK_OFS",
        "TABNODE_FLAGS_OFS",
        "emit_rmro(as, XO_GROUP3, XOg_TEST, node, TABNODE_FLAGS_OFS);",
        "Reg idx;",
        "idx = ra_scratch(as, iallow);",
        "emit_rr(as, XO_ARITH(XOg_ADD), dest|REX_GC64, idx);",
        "emit_rmro(as, XO_MOV, idx, dest, TABNODE_HMASK_OFS);",
        "emit_rmro(as, XO_ARITH(XOg_AND), idx, dest,",
        "emit_rmro(as, XO_MOV, dest|REX_GC64, tab, offsetof(GCtab, node));"
      })

      local data = t:read(asm)
      if count_plain(data, "asm_tabnode_retiring_guard(as, dest);") < 2 then
        error("dynamic HREF must guard both node-load paths against retiring generations")
      end

      for _, reject in ipairs({
        "emit_rmro(as, XO_ARITH(XOg_ADD), dest|REX_GC64, tab, offsetof(GCtab,node))",
        "emit_rmro(as, XO_MOV, dest, tab, offsetof(GCtab, hmask))",
        "offsetof(GCtab, hmask)",
        "emit_rmro(as, XO_ARITH(XOg_AND), dest, tab, offsetof(GCtab, hmask))",
        "emit_rmro(as, XO_ARITH(XOg_AND), dest, key, offsetof(GCstr, sid))"
      }) do
        t:assert_not_contains(asm, reject)
      end
      print("M5 JIT HREF node-header hmask guard passed")
    end
  })

  add({
    name = "m5_math_random_tg",
    description = "per-TG math.random regression test",
    run = function(t)
      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", t:path("tests", "t-math-random-tg.lua") })
    end
  })

  add({
    name = "m5_os_reentrant",
    description = "POSIX os.date/tmpname reentrancy and setlocale guard",
    run = function(t)
      local lib_os = t:path("src", "lib_os.c")
      local date_block = t:text_between(lib_os, "LJLIB_CF(os_date)",
                                        "LJLIB_CF(os_time)")
      t:assert_text_contains("os_date", date_block, "gmtime_r")
      t:assert_text_contains("os_date", date_block, "localtime_r")

      local tmpname_block = t:c_block(lib_os, "static int os_native_mkstemp")
      t:assert_text_contains("os_native_mkstemp", tmpname_block, "mkstemp")

      local setlocale_block = t:text_between(lib_os, "LJLIB_CF(os_setlocale)",
                                             "#include \"lj_libdef.h\"")
      t:assert_text_contains("os_setlocale", setlocale_block,
                             "la_load32_acq(&G(L)->mt_active)")
      t:assert_text_contains("os_setlocale", setlocale_block,
                             "os.setlocale mutation disabled after threading activation")
      t:assert_text_contains("os_setlocale", setlocale_block,
                             "setlocale(opt, str)")

      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", t:path("tests", "t-os-reentrant.lua") }, {
        env = {
          LJ_M5_OS_THREADS = os.getenv("LJ_M5_OS_THREADS") or "8",
          LJ_M5_OS_ITERS = os.getenv("LJ_M5_OS_ITERS") or "200"
        }
      })
    end
  })

  add({
    name = "m5_parser_capture_meta",
    description = "parser captured-local metadata and source cell emission guard",
    run = function(t)
      local parse = t:path("src", "lj_parse.c")
      t:build({ clean = true, quiet = true, xcflags = "-DLUA_USE_ASSERT" })
      t:luajit({ t:path("tests", "t-parser-capture-meta.lua") })

      for _, needle in ipairs({
        "VSTACK_VAR_CAPTURED",
        "var_mark_captured(fs, reg)",
        "unmarked captured local",
        "BC_CGET",
        "BC_CSET",
        "One-pass capture discovery can happen after earlier loop bytecode",
        "CSET stores raw slots unchanged and updates cells after FNEW promotion"
      }) do
        t:assert_contains(parse, needle)
      end

      t:assert_not_match(parse, "#if%s+LJ_MT", "#if LJ_MT")
      t:assert_not_match(parse, "#ifdef%s+LJ_MT", "#ifdef LJ_MT")
      t:assert_not_contains(parse, "LUAJIT_THREADSAFE")
    end
  })
end
