local utils = require("suite_utils")

local contains = utils.contains
local quote = utils.shell_quote
local count_plain = utils.count_plain
local append = utils.append_list
local assert_text_not_contains = utils.assert_text_not_contains
local assert_no_lines = utils.assert_no_lines

local function basename(path)
  return path:match("([^/]+)$") or path
end

local function filter_files(files, pred)
  local out = {}
  for i = 1, #files do
    if pred(files[i]) then out[#out + 1] = files[i] end
  end
  return out
end

local function src_files(t, extensions, opts)
  opts = opts or {}
  local files = t:files(t:path("src"), { extensions = extensions })
  return filter_files(files, function(path)
    local b = basename(path)
    if b == "lj_bcdef.h" or b == "lj_ffdef.h" or b == "lj_libdef.h" or
       b == "lj_recdef.h" or b == "lj_folddef.h" or b == "luajit.h" or
       b == "lj_vm.S" or (b == "vmdef.lua" and contains(path, "/src/jit/")) then
      return false
    end
    if opts.exclude_host and contains(path, "/host/") then return false end
    if opts.exclude_vm and basename(path):match("^vm_.*%.dasc$") then return false end
    if opts.exclude_asm and basename(path):match("^lj_asm_.*%.h$") then return false end
    if opts.exclude_names then
      for i = 1, #opts.exclude_names do
        if b == opts.exclude_names[i] then return false end
      end
    end
    return true
  end)
end

local function src_ch_files(t, opts)
  return src_files(t, { ".c", ".h" }, opts)
end

local function src_text_files(t, opts)
  return src_files(t, { ".c", ".h", ".dasc", ".lua", ".S", ".hpp", ".txt", ".md" }, opts)
end

local function test_text_files(t)
  return filter_files(t:files(t:path("tests"), {
    extensions = { ".c", ".h", ".lua" }
  }), function(path)
    return not contains(path, "/tests/suites/")
  end)
end

local function assert_block_excludes(label, block, rejects)
  for i = 1, #rejects do
    assert_text_not_contains(label, block, rejects[i])
  end
end

local function find_pos(label, data, needle)
  local p = data:find(needle, 1, true)
  if not p then error(label .. ": missing expected text: " .. needle, 2) end
  return p
end

local function assert_before(label, data, a, b)
  local pa = find_pos(label, data, a)
  local pb = find_pos(label, data, b)
  if pa >= pb then
    error(label .. ": expected `" .. a .. "` before `" .. b .. "`", 2)
  end
end

local function lua_path_guard(t)
  return t:path("src", "?.lua") .. ";" .. t:path("src", "jit", "?.lua") .. ";;"
end

local function run_luajit(t, args)
  t:luajit(args, { env = { LUA_PATH = lua_path_guard(t) } })
end

local function luajit_capture(t, args, out)
  local parts = { "LUA_PATH=" .. quote(lua_path_guard(t)), quote(t:path("src", "luajit")) }
  for i = 1, #args do parts[#parts + 1] = quote(args[i]) end
  t:run(table.concat(parts, " ") .. " >" .. quote(out))
end

local function run_stock(t, args)
  local parts = {
    "cd " .. quote(t:path("tests", "stock", "test")),
    "LUA_PATH=" .. quote(lua_path_guard(t)) .. " " .. quote(t:path("src", "luajit"))
  }
  for i = 1, #args do parts[2] = parts[2] .. " " .. quote(args[i]) end
  t:run(parts[1] .. " && " .. parts[2])
end

local function build_and_run_c(t, out, cfile, opts)
  opts = opts or {}
  t:cc(out, { t:path("tests", cfile) }, {
    link_luajit = true,
    libs = { "-lm", "-ldl", "-pthread" }
  })
  t:run({ out }, { timeout = opts.timeout })
end

local function block_has_all(label, block, needles)
  for i = 1, #needles do
    if not contains(block, needles[i]) then
      error(label .. ": missing expected text: " .. needles[i], 2)
    end
  end
end

local function table_value_smoke()
  return [=[
local util = require("jit.util")
local linfo = util.funcinfo(function() return 1 end)
assert(linfo.proto ~= nil and linfo.upvalues ~= nil)
local cinfo = util.funcinfo(print)
assert(cinfo.addr ~= nil and cinfo.upvalues ~= nil)
jit.flush()
jit.opt.start("hotloop=1")
local function hot(n)
  local s = 0
  for i = 1, n do s = s + i end
  return s
end
for _ = 1, 5 do hot(20) end
local traced
for tr = 1, 32 do
  local info = util.traceinfo(tr)
  if info then
    assert(type(info.nins) == "number" and type(info.linktype) == "string")
    traced = tr
    break
  end
end
assert(traced)
local snap
for sn = 0, 32 do
  snap = util.tracesnap(traced, sn)
  if snap then break end
end
assert(snap and type(snap[0]) == "number" and type(snap[1]) == "number")
local lines = debug.getinfo(function()
  local x = 1
  return x
end, "L").activelines
assert(type(lines) == "table")
local saw_line = false
for line, active in pairs(lines) do
  if type(line) == "number" and active == true then
    saw_line = true
    break
  end
end
assert(saw_line)
local t = { 1, 2, 3 }
t.name = "table-value-publish"
assert(t[3] == 3 and t.name == "table-value-publish")
assert(("table-value-publish"):sub(1, 5) == "table")
local function event_cb() end
jit.attach(event_cb, "bc")
jit.attach(event_cb)
do
  local ffi = require("ffi")
  local x = 1LL
  assert(type(x) == "cdata" and tonumber(x) == 1 and ffi.typeof(x))
end
do
  local buffer = require("string.buffer")
  local mt = {}
  local dict = { "key", "hello", "key", false }
  local dict_mt = { mt, mt, false }
  local b = buffer.new({ dict = dict, metatable = dict_mt })
  b:encode(setmetatable({ key = "hello" }, mt))
  local out = b:decode()
  assert(out.key == "hello" and getmetatable(out) == mt)
end
]=]
end

local function tset_nil_smoke()
  return [=[
local mt = {
  __newindex = function(t, k, v) rawset(t, "hit", tostring(k) .. ":" .. tostring(v)) end
}
local t = setmetatable({ a = 1 }, mt)
t.a = 2
assert(t.a == 2 and t.hit == nil)
t.b = 3
assert(t.hit == "b:3")
local a = { 1, 2 }
a[1] = 10
local k = 2
a[k] = 20
assert(a[1] == 10 and a[2] == 20)
local function many() return 1, 2, 3 end
local m = { many() }
assert(m[1] == 1 and m[2] == 2 and m[3] == 3)
local function spread(n)
  local r = {}
  for i = 1, n do r[i] = i end
  return unpack(r, 1, n)
end
local big = { spread(96) }
assert(#big == 96 and big[1] == 1 and big[96] == 96)
local s = { spread(96) }
s[64] = 640
local kk = 70
s[kk] = 700
for i = 80, 82 do s[i] = i * 10 end
assert(s[64] == 640 and s[70] == 700 and s[82] == 820)
]=]
end

local function jit_trace_publish_smoke()
  return [=[
local util = require"jit.util"
local function tracecount()
  local n = 0
  for i = 1, 200 do
    if util.traceinfo(i) then n = n + 1 end
  end
  return n
end
jit.off(tracecount, true)

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function f(n)
  local s = 0
  for i = 1, n do s = s + i end
  return s
end
for _ = 1, 40 do
  assert(f(200) == 20100)
end
assert(tracecount() > 0, "no root trace was published")
jit.flush()
assert(tracecount() == 0, "trace slots were not cleared")

jit.flush()
jit.opt.start("hotloop=1")
local function f1(a)
  if a > 0 then
    local b = f1(a - 1)
    return function()
      if type(b) == "function" then return a + b() end
      return a + b
    end
  end
  return a
end
local function f2(a) return f1(a)() end
for _ = 1, 41 do
  assert(f2(4) + f2(4) == 20)
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function side(n, flip)
  local s = 0
  for i = 1, n do
    if flip and i % 3 == 0 then s = s + i else s = s - 1 end
  end
  return s
end
local function expect(n, flip)
  local s = 0
  for i = 1, n do
    if flip and i % 3 == 0 then s = s + i else s = s - 1 end
  end
  return s
end
for _ = 1, 60 do
  assert(side(90, false) == expect(90, false))
end
local before = tracecount()
for _ = 1, 120 do
  assert(side(90, true) == expect(90, true))
end
assert(tracecount() > before, "no side trace was published")
local after_side = tracecount()
for _ = 1, 120 do
  assert(side(90, true) == expect(90, true))
end
assert(util.traceinfo(1), "missing root trace 1")
jit.flush(1)
assert(not util.traceinfo(1), "scoped root flush did not clear root slot")
assert(tracecount() < after_side, "scoped root flush did not retire any slots")
jit.flush()
assert(tracecount() == 0, "full flush after scoped root flush left traces")
for _ = 1, 20 do
  assert(side(90, true) == expect(90, true))
end
print("jit-trace-publish-smoke OK")
]=]
end

return function(add)
  add({
    name = "m5_state_owner",
    description = "lua_State owner claim behavior",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build_and_run_c(t, t:tmp("lj_t-state-owner"), "t-state-owner.c")
      print("M5 lua_State owner behavior passed")
    end
  })

  add({
    name = "m5_cell_ops",
    description = "local-cell bytecode and behavior guards",
    run = function(t)
      t:build({ clean = true, quiet = true })

      local out = t:tmp("lj_m5_cell_ops_bc")
      luajit_capture(t, { "-bl", "-e", [=[
local x = 0
local function f()
  x = x + 1
  return x
end
x = 7
local function g()
  local y = 1
  return function()
    y = y + 1
    return y
  end
end
return f, g, x
]=] }, out)
      local bc = t:read(out)
      if not (contains(bc, "CGET") or contains(bc, "CSET")) then
        error("captured local parser output must contain CGET/CSET")
      end
      luajit_capture(t, { "-bl", "-e", [=[
local function f()
  return f
end
return f
]=] }, out)
      bc = t:read(out)
      if not (contains(bc, "CNEW") and contains(bc, "CSET")) then
        error("self-captured local function must use CNEW/CSET")
      end
      t:remove(out)

      run_luajit(t, { "-e", [=[
local dumped = string.dump(function()
  local x = 0
  return function()
    x = x + 1
    return x
  end
end)
local outer = assert(loadstring(dumped))
local inner = outer()
assert(inner() == 1 and inner() == 2)
]=] })
      run_luajit(t, { "-e", [=[
jit.flush()
jit.opt.start("hotloop=1")
local util = require"jit.util"
local function run(n)
  local x = 0
  local function touch() return x end
  for i = 1, n do x = x + 1 end
  return x, touch
end
local v, f = run(200)
assert(v == 200 and f() == 200)
assert(util.traceinfo(1), "expected traced CGET/CSET owner loop")
local v2, f2 = run(20)
assert(v2 == 20 and f2() == 20)
]=] })
      run_luajit(t, { "-e", [=[
local util = require"jit.util"
local pool = { "even", "odd" }
jit.flush()
jit.opt.start("hotloop=1")
local function run(n)
  local x = pool[1]
  local function get() return x end
  for i = 1, n do x = pool[(i % 2) + 1] end
  return get()
end
assert(run(200) == pool[1])
assert(util.traceinfo(1), "expected traced GC-valued CSET owner loop")
collectgarbage()
assert(run(20) == pool[1])
]=] })
      run_luajit(t, { "-e", [=[
jit.flush()
jit.opt.start("hotloop=1")
local util = require"jit.util"
local src = function(n)
  local x = 0
  local function touch() return x end
  for i = 1, n do x = x + 1 end
  return x, touch
end
local run = assert(loadstring(string.dump(src)))
local v, f = run(200)
assert(v == 200 and f() == 200)
assert(util.traceinfo(1), "expected loaded owner CGET/CSET trace")
local v2, f2 = run(20)
assert(v2 == 20 and f2() == 20)
]=] })
      run_luajit(t, { "-e", [=[
jit.flush()
jit.opt.start("hotloop=1")
local util = require"jit.util"
local function make(seed)
  local x = seed
  return function()
    x = x + 1
    return x
  end
end
local function run(seed, n)
  local f = make(seed)
  local last
  for i = 1, n do last = f() end
  return last, f
end
local v, f = run(0, 200)
assert(v == 200 and f() == 201)
assert(util.traceinfo(1), "expected traced child numeric upvalue loop")
local v2, f2 = run(1000, 30)
assert(v2 == 1030 and f2() == 1031)
assert(f() == 202)
]=] })
      run_luajit(t, { "-e", [=[
local util = require"jit.util"
local pool = { "even", "odd" }
jit.flush()
jit.opt.start("hotloop=1")
local function make(seed)
  local n = seed
  local x = pool[1]
  return function()
    n = n + 1
    x = pool[(n % 2) + 1]
    return n, x
  end
end
local function run(seed, n)
  local f = make(seed)
  local last, lastx
  for i = 1, n do last, lastx = f() end
  return last, lastx, f
end
local v, xv, f = run(0, 200)
local fv, fx = f()
assert(v == 200 and xv == pool[1] and fv == 201 and fx == pool[2])
assert(util.traceinfo(1), "expected traced child GC upvalue loop")
collectgarbage()
local v2, xv2, f2 = run(1000, 30)
local f2v, f2x = f2()
local fv2, fx2 = f()
assert(v2 == 1030 and xv2 == pool[1] and f2v == 1031 and f2x == pool[2])
assert(fv2 == 202 and fx2 == pool[1])
]=] })
      run_luajit(t, { "-e", [=[
jit.flush()
jit.opt.start("hotloop=1")
local util = require"jit.util"
local src = function(seed, n)
  local x = seed
  local function bump()
    x = x + 1
    return x
  end
  local last
  for i = 1, n do last = bump() end
  return last, bump
end
local run = assert(loadstring(string.dump(src)))
local v, f = run(0, 200)
assert(v == 200 and f() == 201)
assert(util.traceinfo(1), "expected loaded child upvalue trace")
local v2, f2 = run(1000, 30)
assert(v2 == 1030 and f2() == 1031)
assert(f() == 202)
]=] })
      run_luajit(t, { "-e", [=[
jit.flush()
jit.opt.start("hotloop=1")
local util = require"jit.util"
local src = function(n)
  local keep
  for i = 1, n do
    local function f() return f end
    keep = f
  end
  return keep
end
local run = assert(loadstring(string.dump(src)))
local f = run(30)
assert(f() == f)
assert(util.traceinfo(1), "expected loaded CNEW creation trace")
]=] })
      run_stock(t, { "test.lua", "--quiet", "lang/upvalue" })
      run_stock(t, { "misc/uclo.lua" })
      run_stock(t, { "test.lua", "--quiet", "opt/fwd/upval.lua" })
      run_stock(t, { "test.lua", "--quiet", "lang/goto.lua" })
      print("M5 local-cell opcode substrate guard passed")
    end
  })

  add({
    name = "m5_upvalue_publish_gc",
    description = "closed-upvalue GC object publication behavior",
    run = function(t)
      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", t:path("tests", "t-threading-upvalue.lua") })
      print("M5 closed-upvalue GC publication behavior passed")
    end
  })

  add({
    name = "m5_jit_trace_publish",
    description = "JIT trace-slot and trace-link publication guards",
    run = function(t)
      local vm = t:path("src", "vm_x64.dasc")
      t:assert_all_any_contains({
        t:path("src", "lj_bc.h"),
        t:path("src", "lj_dispatch.c")
      }, {
        "bc_publish(const uint32_t *pc, uint32_t ins)",
        "la_store32_rel((uint32_t *)pc, ins)",
        "la_load32_acq((uint32_t *)pc)",
        "bc_publish_op(const uint32_t *pc, BCOp op)",
        "bc_publish_d(const uint32_t *pc, uint32_t d)",
        "lj_bc_publish_vm(uint32_t *pc, uint32_t ins)",
        "lj_bc_publish_op_vm(uint32_t *pc, BCOp op)"
      })
      t:assert_all_contains(t:path("src", "lj_target_x86.h"), {
        "EXITSTUB_TRACE_SPACING",
        "exitstub_trace_addr(T, exitno)"
      })
      t:assert_all_contains(t:path("src", "lj_asm_x86.h"), {
        "asm_exitstub_trace_setup(ASMState *as, ExitNo nexits)",
        "mov rax, moffs64",
        "xchg [rsp], rax; ret",
        "exitstub_trace_addr(as->T, as->snapno)"
      })

      t:assert_not_contains(t:path("src", "lj_safepoint.c"),
                            "Temporary single-mutator flush action")
      t:assert_not_contains(t:path("src", "lj_dispatch.c"),
                            "lj_gc2_handshake(g, LJ_GC2_HS_EXIT_TRACES);")
      assert_no_lines(t, "GCtrace.startpt must use trace_startpt acquire/release helpers",
                      src_ch_files(t, { exclude_host = true }), function(line)
        return contains(line, "startpt") and
               (contains(line, "gcref(") or contains(line, "setgcref(") or
                contains(line, "setgcrefnull("))
      end)
      assert_no_lines(t, "IR KGC constants must publish through release helpers", {
        t:path("src", "lj_ir.c"),
        t:path("src", "lj_asm.c")
      }, function(line)
        return contains(line, "setgcref(ir[LJ_GC64].gcr") or
               contains(line, "setgcref(IR(as->J->ktrace)[LJ_GC64].gcr") or
               contains(line, "IR(as->J->ktrace)->o = IR_KGC")
      end)
      assert_no_lines(t, "GC trace traversal must acquire-snapshot IR KGC constants", {
        t:path("src", "lj_trace.c"),
        t:path("src", "lj_gc.c"),
        t:path("src", "lj_gc2.c")
      }, function(line)
        return contains(line, "ir->o == IR_KGC") or contains(line, "ir_kgc(ir)")
      end)
      assert_no_lines(t, "public full flush callers must route through HS_FLUSHJ", {
        t:path("src", "lj_api.c"),
        t:path("src", "lj_dispatch.c"),
        t:path("src", "lj_profile.c")
      }, function(line)
        return contains(line, "lj_trace_flushall(L)")
      end)
      assert_no_lines(t, "J->trace slots must use acquire/release trace helpers", {
        t:path("src", "lj_trace.c"),
        t:path("src", "lj_jit.h")
      }, function(line)
        return contains(line, "setgcrefp(J->trace") or
               contains(line, "setgcrefnull(J->trace") or
               contains(line, "gcref(J->trace")
      end)
      assert_no_lines(t, "trace vectors must use TraceVec RCU helpers, not raw slot vectors",
                      append(src_text_files(t), test_text_files(t)), function(line)
        return contains(line, "lj_mem_growvec(J->L, J->trace") or
               contains(line, "lj_mem_freevec(g, J->trace") or
               contains(line, "gcref(J->trace")
      end)
      assert_no_lines(t, "shared trace-number fields must use acquire/release helpers", {
        t:path("src", "lj_trace.c"),
        t:path("src", "lj_gc.c"),
        t:path("src", "lj_gc2.c"),
        t:path("src", "lib_jit.c"),
        t:path("src", "lj_bcwrite.c")
      }, function(line)
        return line:find("pt%->trace%f[^%w_]") ~= nil or
               line:find("%->link%f[^%w_]") ~= nil or
               line:find("%->nextroot%f[^%w_]") ~= nil or
               line:find("%->nextside%f[^%w_]") ~= nil
      end)
      assert_no_lines(t, "live bytecode patches must use full-word release publication", {
        t:path("src", "lj_trace.c"),
        t:path("src", "lj_record.c"),
        t:path("src", "lj_dispatch.c")
      }, function(line)
        local compact = line:gsub("%s+", "")
        return contains(line, "setbc_op(") or contains(line, "setbc_d(") or
               contains(line, "setbc_j(") or contains(compact, "*J->patchpc=") or
               contains(compact, "*pc=T->startins")
      end)
      assert_no_lines(t, "x64 live bytecode patches must use full-word publication helpers",
                      vm, function(line)
        return contains(line, "mov PC_OP,") or contains(line, "mov byte [PC]") or
               contains(line, "mov dword [PC]")
      end)
      t:assert_not_contains(t:path("src", "lj_trace.c"), "lj_asm_patchexit(J, parent")
      t:assert_contains(t:path("src", "lj_asm_x86.h"),
                        "lnk == as->T->traceno ? as->T : traceref(as->J, lnk)")
      t:build({ quiet = true })
      t:assert_all_contains(t:path("src", "lj_vm.S"), {
        "call lj_bc_publish_op_vm",
        "call lj_bc_publish_vm"
      })
      build_and_run_c(t, t:tmp("lj_t-jit-tracevec"), "t-jit-tracevec.c",
                      { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-jit-mcode-retire"), "t-jit-mcode-retire.c",
                      { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-jit-trace-retire"), "t-jit-trace-retire.c",
                      { timeout = "20s" })
      run_luajit(t, { "-e", jit_trace_publish_smoke() })
      print("M5 JIT trace publication guard passed")
    end
  })

  add({
    name = "m5_tab_array_publish",
    description = "table array publication and retirement guards",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build_and_run_c(t, t:tmp("lj_t-tab-array-publish"),
                      "t-tab-array-publish.c", { timeout = "20s" })

      assert_no_lines(t, "GC table array readers must not gate on GCtab.acap", {
        t:path("src", "lj_gc.c"),
        t:path("src", "lj_gc2.c")
      }, function(line)
        return contains(line, "t->acap") or contains(line, "->acap > 0")
      end)

      local lj_tab = t:path("src", "lj_tab.c")
      local tab_free = t:c_block(lj_tab, "void LJ_FASTCALL lj_tab_free(global_State *g, GCtab *t)")
      block_has_all("lj_tab_free", tab_free, {
        "lj_tab_array_separated_snapshot_acq(t, &array)"
      })
      assert_block_excludes("lj_tab_free", tab_free, { "t->acap", "lj_tab_array_acq(t)" })

      local tab_dup = t:c_block(lj_tab, "GCtab * LJ_FASTCALL lj_tab_dup(lua_State *L, const GCtab *kt)")
      block_has_all("lj_tab_dup", tab_dup, {
        "lj_tab_array_snapshot_acq(t, &array)",
        "lj_tab_node_snapshot_acq(t, &thmask)"
      })
      assert_block_excludes("lj_tab_dup", tab_dup, {
        "lj_tab_asize_acq(t)",
        "lj_tab_array_acq(t)",
        "lj_tab_node_acq(t)"
      })

      assert_no_lines(t, "table arrays must use lj_tab_array_* helpers in C code",
                      src_text_files(t, {
                        exclude_host = true,
                        exclude_vm = true,
                        exclude_asm = true,
                        exclude_names = { "lj_obj.h" }
                      }), function(line)
        return (contains(line, "tvref(") and contains(line, "->array")) or
               (contains(line, "setmref(") and contains(line, "->array"))
      end)
      assert_no_lines(t, "integer table access must use snapshot inline functions",
                      t:path("src", "lj_tab.h"), function(line)
        return line:find("^#define") and
               (contains(line, "inarray") or contains(line, "arrayslot") or
                contains(line, "lj_tab_getint") or contains(line, "lj_tab_setint"))
      end)
      assert_no_lines(t, "table array header asize must be immutable after publish",
                      src_text_files(t), function(line)
        return contains(line, "lj_tab_array_hdr_asize_rel") or
               (contains(line, "la_store32_rel(&lj_tab_array_hdrw") and
                (contains(line, "->asize") or contains(line, "->acap")))
      end)
      t:assert_not_contains(lj_tab, "hdr->acap = acap")

      local array_mem = t:c_block(t:path("src", "lj_obj.h"),
                                  "static LJ_AINLINE void *lj_tab_array_mem_acq(const GCtab *t)")
      block_has_all("lj_tab_array_mem_acq", array_mem, {
        "lj_tab_array_snapshot_acq(t, &array)"
      })
      assert_block_excludes("lj_tab_array_mem_acq", array_mem, { "lj_tab_array_acq(t)" })

      assert_no_lines(t, "serializer dictionary array reads must use array snapshots",
                      t:path("src", "lj_serialize.c"), function(line)
        return contains(line, "asize = lj_tab_asize_acq(dict)") or
               contains(line, "array = lj_tab_array_acq(dict)") or
               contains(line, "idx < lj_tab_asize_acq(dict_") or
               contains(line, "lj_tab_array_acq(dict_")
      end)
      t:assert_not_contains(t:path("src", "lj_serialize.c"),
                            "lj_tab_resize(L, dict, lj_tab_asize_acq(dict)")
      t:assert_contains(t:path("src", "lj_serialize.c"),
                        "serialize_dict_storeint(L, dict, &tv, (int32_t)(i-1))")
      t:assert_not_contains(t:path("src", "lj_serialize.c"),
                            "lj_tab_storeint(L, lj_tab_newkey(L, dict, &tv)")

      local table_pack = t:c_block(t:path("src", "lib_table.c"), "LJLIB_CF(table_pack)")
      block_has_all("table_pack", table_pack, {
        "lj_tab_array_snapshot_acq(t, &array)",
        "table_pack_storeint_str(L, t, strV(lj_lib_upvalue(L, 1)),",
        "lj_gc_pubtab(L, t)"
      })
      assert_block_excludes("table_pack", table_pack, { "lj_tab_array_acq(t)" })
      local bcread = t:c_block(t:path("src", "lj_bcread.c"),
                               "static GCtab *bcread_ktab(LexState *ls)")
      block_has_all("bcread_ktab", bcread, { "lj_tab_array_snapshot_acq(t, &o)" })
      assert_block_excludes("bcread_ktab", bcread, { "lj_tab_array_acq(t)" })

      local decode = t:text_between(t:path("src", "lj_serialize.c"),
                                    "t = lj_tab_new(sbufL(sbx), narray, hsize2hbits(nhash))",
                                    "if (nhash)")
      block_has_all("serializer table decode", decode, { "lj_tab_array_snapshot_acq(t, &array)" })
      assert_block_excludes("serializer table decode", decode, { "lj_tab_array_acq(t)" })

      local parse_ctor = t:text_between(t:path("src", "lj_parse.c"), "if (!t) {",
                                        "lj_gc_check(fs->L)")
      block_has_all("parser template constructor", parse_ctor, {
        "lj_tab_array_snapshot_acq(t, &array)"
      })
      assert_block_excludes("parser template constructor", parse_ctor, { "lj_tab_asize_acq(t)" })

      t:assert_not_contains(t:path("src", "lj_record.c"),
                            "TValue *record_array = lj_tab_array_acq(t)")
      t:assert_not_contains(t:path("src", "lj_record.c"), "lj_tab_asize_acq(t)")
      local rec_bump = t:c_block(t:path("src", "lj_record.c"),
                                 "static void rec_idx_bump(jit_State *J, RecordIndex *ix)")
      if count_plain(rec_bump, "lj_tab_array_snapshot_acq(tb, &array)") +
         count_plain(rec_bump, "lj_tab_array_snapshot_acq(tpl, &array)") < 3 then
        error("rec_idx_bump must snapshot table-bump array shape")
      end
      assert_block_excludes("rec_idx_bump", rec_bump, {
        "lj_tab_asize_acq(tb)",
        "lj_tab_asize_acq(tpl)",
        "lj_tab_array_acq(tpl)"
      })
      local rec_tsetm = t:c_block(t:path("src", "lj_record.c"),
                                  "static void rec_tsetm(jit_State *J, BCReg ra, BCReg rn, int32_t i)")
      block_has_all("rec_tsetm", rec_tsetm, { "lj_tab_array_snapshot_acq(t, &array)" })
      assert_block_excludes("rec_tsetm", rec_tsetm, { "lj_tab_asize_acq(t)" })
      local rec_next = t:c_block(t:path("src", "lj_record.c"),
                                 "static IRType rec_next_types(GCtab *t, uint32_t idx)")
      block_has_all("rec_next_types", rec_next, { "lj_tab_array_snapshot_acq(t, &array)" })
      assert_block_excludes("rec_next_types", rec_next, {
        "lj_tab_asize_acq(t)",
        "lj_tab_array_acq(t)"
      })

      local resize = t:c_block(lj_tab, "void lj_tab_resize(lua_State *L,")
      assert_before("lj_tab_resize", resize,
                    "lj_tab_array_nextgen_rel(oldarray, array)",
                    "lj_tab_array_hdr_flags_or_rel(oldarray, TABARRAY_FLAG_RETIRING)")
      block_has_all("lj_tab_resize", resize, {
        "oldasize = (uint32_t)lj_tab_array_snapshot_acq(t, &oldarray)",
        "oldarray_separated = oldarray && !lj_tab_array_is_colocated(t, oldarray)"
      })
      assert_block_excludes("lj_tab_resize", resize, {
        "lj_tab_array_acq(t)",
        "lj_tab_asize_acq(t)",
        "oldacap = t->acap",
        "lj_tab_array_separated(t)"
      })
      print("M5 table array publication tests passed")
    end
  })

  add({
    name = "m5_tab_cas_store",
    description = "table CAS store behavior",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build_and_run_c(t, t:tmp("lj_t-tab-cas-store"),
                      "t-tab-cas-store.c", { timeout = "20s" })
      print("M5 table CAS store behavior passed")
    end
  })

  add({
    name = "m5_tab_value_publish",
    description = "C-side table value release-publication guards",
    run = function(t)
      t:build({ clean = true, quiet = true })
      run_luajit(t, { "-e", table_value_smoke() })

      t:assert_all_any_contains(src_text_files(t), {
        "lj_tab_storetv",
        "lj_tab_storenil",
        "lj_tab_storebool",
        "lj_tab_storeint",
        "lj_tab_storeintptr",
        "lj_tab_storestr",
        "lj_tab_storetab",
        "lj_tab_storethread",
        "lj_tab_storeproto",
        "lj_tab_storefunc",
        "lj_tab_storeudata",
        "lj_tab_storetvn",
        "lj_tab_storenilraw",
        "tab_storekeyrel",
        "copyTVrel(L, dst, src)",
        "copyTVrel(L, &dst[i], &src[i])",
        "copyTVrel(L, dst, &k)",
        "copyTVrel(L, slot, &val)",
        "copyTVrel(L, tab_rehash_insert(L, newnode, newhmask, &newfreetop, &key),",
        "lib_storetv_key(L, tab, L->top+1, L->top)",
        "copyTVrel(L, o, f)",
        "table_insert_shift_store(L, t, i)",
        "table_insert_value_store(L, t, i, L->top-1)",
        "table_pack_storeint_str(L, t, strV(lj_lib_upvalue(L, 1)), (int32_t)n)",
        "lj_tab_trystoretv_cas(L, dst, &val) == LJ_TAB_STORE_CAS_OK",
        "lj_tab_storetv(L, &array[i], &base[i])",
        'base_storestr_str(L, t, lj_str_newlit(L, "__mode"), lj_str_newlit(L, "kv"))',
        'base_storetab_str(L, env, lj_str_newlit(L, "_G"), env)',
        "string_storetab_str(L, mt, mmname_str(g, MM_index), strtab)",
        'ctype_storestr_str(L, t, lj_str_newlit(L, "__mode"), lj_str_newlit(L, "k"))',
        "gc_stats_storetv_str(L, t, name, &tv)",
        "gc_stats_storetv_int(L, bt, (int32_t)i + 1, &tv)",
        'gc_stats_storetv_str(L, t, "poll_ack_latency_buckets", &tv)',
        "jit_util_storetv_str(L, t, lj_str_newz(L, name), &tv)",
        "jit_util_storetv_int(L, t, key, &tv)",
        'setprotofield(L, t, lj_str_newlit(L, "proto"), pt)',
        'setintptrfield(L, t, lj_str_newlit(L, "addr"), (intptr_t)(void *)fn->c.f)',
        "setintindex(L, t, 0, (int32_t)snap->ref - REF_BIAS)",
        "debug_activelines_storebool(L, t, line)",
        "rec_rbchash_ref_acq(RBCHashEntry *rbc)",
        "rec_rbchash_pc_acq(RBCHashEntry *rbc)",
        "rec_rbchash_pt_acq(RBCHashEntry *rbc)",
        "rec_rbchash_publish(jit_State *J, TRef tr, const BCIns *pc)",
        "setmrefrel(rbc->pc, pc)",
        "setgcrefrel(rbc->pt, obj2gco(J->pt))",
        "la_store32_rel(&rbc->ref, tref_ref(tr))",
        "rec_rbchash_publish(J, tr, J->pc)",
        "rec_rbchash_publish(J, rc, pc)",
        "rec_template_mark_nil(J, tpl, &key)",
        "rec_template_mark_nil(J, tpl, &ix->keyv)",
        "lj_tab_storenil(J->L, &node[i].val)",
        "lj_tab_storenil(J->L, &array[i])",
        "copyTVrel(L, o, base+2)",
        "slot = lib_storefunc_str(L, tab, name, fn)",
        "jit_profile_registry_store(L, registry, &key, &tv)",
        "jit_profile_registry_store(L, registry, &key, niltv(L))",
        "jit_attach_event_store(L, tabV(L->top-2), L->top-1, niltv(L))",
        "lj_tab_storenilraw(&array[i])",
        "lj_tab_storenilraw(&n->val)",
        "lj_cdata_fin_storenil(L, tv)",
        "ffi_loaded_store(L, t, name, L->top-1)",
        "ffi_miscmap_store(L, cts, &cts->g->strempty, L->top-1)",
        'ffi_typeinfo_storeint(L, t, lj_str_newlit(L, "info"), (int32_t)info)',
        'ffi_typeinfo_storestr(L, t, lj_str_newlit(L, "name"), name)',
        "lj_tab_storenilraw(&n->key)",
        "const_slot_store(o, fs->nkn)",
        "const_slot_store(o, fs->nkgc)",
        "parse_keep_storebool(L, ls->fs->kt, &key)",
        "parse_keep_storebool(L, ls->fs->kt, tv)",
        "lj_tab_storetv(ls->L, o, &tv)",
        "lj_tab_storetv(ls->L, lj_tab_set(ls->L, t, &key), &tv)",
        "copyTVrel(sbufL(sbx), o, &tv)",
        "settabV(sbufL(sbx), &tv, t)",
        "copyTVrel(L, o, &tv)",
        "lj_tab_storetv(L, val, &tmp)",
        "serialize_dict_storeint(L, dict, &tv, (int32_t)(i-1))"
      })

      assert_no_lines(t, "raw table slot stores must use lj_tab_store* or copyTVrel",
                      src_text_files(t, { exclude_host = true }), function(line)
        return (line:find("set[%w_]*V%(") and contains(line, "lj_tab_set")) or
               (contains(line, "lj_tab_set") and contains(line, ")->u64")) or
               (contains(line, "lj_tab_newkey") and contains(line, ")->u64")) or
               (contains(line, "copyTV(") and contains(line, "lj_tab_set"))
      end)
      assert_no_lines(t, "table rehash/new-key publication must use release key/value stores",
                      t:path("src", "lj_tab.c"), function(line)
        return contains(line, "copyTV(L, slot") or
               contains(line, "copyTV(L, tab_rehash_insert") or
               contains(line, "copyTV(L, &freenode->key") or
               contains(line, "copyTV(L, &n->key") or
               contains(line, "node->key.u64 = 0") or
               contains(line, "n->key.u64 = 0")
      end)
      assert_no_lines(t, "API/table library direct table slot stores must release-publish", {
        t:path("src", "lj_api.c"),
        t:path("src", "lib_table.c")
      }, function(line)
        return contains(line, "copyTV(L, o, L->top+1)") or
               contains(line, "copyTV(L, o, --L->top)") or
               contains(line, "copyTV(L, dst, &val)") or
               contains(line, "setnilV(dst)") or
               contains(line, "copyTV(L, &array[i], &base[i])")
      end)
      t:assert_not_contains(t:path("src", "lib_table.c"),
                            "lj_tab_storeint(L, lj_tab_setstr(L, t, strV(lj_lib_upvalue(L, 1)))")
      assert_no_lines(t, "jit.util result fields must CAS-publish",
                      t:path("src", "lib_jit.c"), function(line)
        return (contains(line, "lj_tab_storeint(L, lj_tab_setstr(L, t") or
                contains(line, "lj_tab_storeint(L, lj_tab_setint(L, t") or
                contains(line, "lj_tab_storeintptr(L, lj_tab_setstr(L, t") or
                contains(line, "lj_tab_storeintptr(L, lj_tab_setint(L, t") or
                contains(line, "lj_tab_storeproto(L, lj_tab_setstr(L, t") or
                contains(line, "lj_tab_storeproto(L, lj_tab_setint(L, t"))
      end)
      t:assert_not_contains(t:path("src", "lj_debug.c"),
                            "lj_tab_storebool(L, lj_tab_setint(L, t, line), 1)")
      assert_no_lines(t, "recorder template table markers must release-publish",
                      t:path("src", "lj_record.c"), function(line)
        return contains(line, "settabV(J->L, &node[i].val, tpl)") or
               contains(line, "settabV(J->L, &array[i], tpl)") or
               contains(line, "settabV(J->L, o, tpl)") or
               contains(line, "lj_tab_storetab(J->L, &node[i].val, tpl)") or
               contains(line, "lj_tab_storetab(J->L, o, tpl)") or
               contains(line, "setnilV(&node[i].val)") or
               contains(line, "setnilV(&array[i])")
      end)
      assert_no_lines(t, "recorder table-bump rollback cache must use acquire/release helpers",
                      t:path("src", "lj_record.c"), function(line)
        return contains(line, "J->rbchash[") and contains(line, ".ref = tref_ref") or
               contains(line, "setmref(J->rbchash") or
               contains(line, "setgcref(J->rbchash") or
               contains(line, "mref(rbc->pc") or
               contains(line, "gcref(rbc->pt")
      end)
      assert_no_lines(t, "FFI/clib table aliases must release-publish", {
        t:path("src", "lib_ffi.c"),
        t:path("src", "lj_clib.c")
      }, function(line)
        return contains(line, "copyTV(L, o, base+2)") or
               contains(line, "setnilV(tv)") or contains(line, "setnumV(tv,") or
               contains(line, "setintV(tv,")
      end)
      assert_no_lines(t, "shared table clearing must release-publish nil", {
        t:path("src", "lj_tab.c"),
        t:path("src", "lj_gc.c")
      }, function(line)
        return contains(line, "setnilV(&array[i])") or
               contains(line, "setnilV(&n->val)") or contains(line, "setnilV(tv)") or
               contains(line, "setnilV(&node[i].val)")
      end)
      assert_no_lines(t, "parser constant table slot markers must release-publish",
                      t:path("src", "lj_parse.c"), function(line)
        return contains(line, "o->u64 = fs->nk") or contains(line, "lj_tab_storebool(L,")
      end)
      assert_no_lines(t, "bytecode template table slots must release-publish",
                      t:path("src", "lj_bcread.c"), function(line)
        return contains(line, "bcread_ktabk(ls, o, NULL)") or
               contains(line, "bcread_ktabk(ls, lj_tab_set(ls->L, t, &key), t)")
      end)
      assert_no_lines(t, "serializer decode outputs must release-publish",
                      t:path("src", "lj_serialize.c"), function(line)
        return contains(line, "copyTV(sbufL(sbx), o, &tv)") or
               contains(line, "settabV(sbufL(sbx), o, t)") or
               contains(line, "setstrV(sbufL(sbx), o,") or
               contains(line, "setintV(o,") or contains(line, "setpriV(o,") or
               contains(line, "setcdataV(sbufL(sbx), o,") or
               contains(line, "setrawlightudV(o,")
      end)
      t:assert_not_contains(t:path("src", "lj_serialize.c"),
                            "lj_tab_storeint(L, lj_tab_newkey(L, dict, &tv)")
      assert_no_lines(t, "snapshot table restore slots must release-publish",
                      t:path("src", "lj_snap.c"), function(line)
        return contains(line, "settabV(J->L, o, t)") or
               contains(line, "snap_restoreval(J, T, ex, snapno, rfilt, irs->op2, val)") or
               contains(line, "val->u32.hi")
      end)
      print("M5 table value publication guard passed")
    end
  })

  add({
    name = "m5_x64_tset_nil_snapshot",
    description = "x64 TSET previous-value nil behavior",
    run = function(t)
      t:build({ clean = true, quiet = true })
      run_luajit(t, { "-joff", "-e", tset_nil_smoke() })
      t:run_luajit_c_fixture(t:tmp("lj_t-x64-tset-forward"),
                             "t-x64-tset-forward.c", { build = false })
      print("M5 x64 TSET previous-value nil behavior passed")
    end
  })
end
