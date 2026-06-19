local utils = require("suite_utils")

local contains = utils.contains
local shell_quote = utils.shell_quote
local count_plain = utils.count_plain
local lines = utils.iter_lines
local assert_no_lines = utils.assert_no_lines

local function count_match(s, pattern)
  local count = 0
  for _ in s:gmatch(pattern) do count = count + 1 end
  return count
end

local function source_code_files(t)
  return t:files(t:path("src"), { extensions = { ".c", ".h", ".dasc" } })
end

local function source_and_test_files(t)
  local files = source_code_files(t)
  local tests = t:files(t:path("tests"), {
    extensions = { ".c", ".h", ".lua" }
  })
  for i = 1, #tests do files[#files + 1] = tests[i] end
  table.sort(files)
  return files
end

local function lua_path(t)
  return t:path("src", "?.lua") .. ";" .. t:path("src", "jit", "?.lua") .. ";;"
end

local function luajit_code(t, code, opts)
  opts = opts or {}
  t:luajit({ "-e", code }, {
    env = { LUA_PATH = lua_path(t) },
    timeout = opts.timeout
  })
end

local function luajit_file(t, file, opts)
  opts = opts or {}
  t:luajit({ file }, {
    env = opts.lua_path and { LUA_PATH = lua_path(t) } or nil,
    timeout = opts.timeout
  })
end

local function luajit_dump(t, dump, dumpopt, code, opts)
  opts = opts or {}
  local parts = { "LUA_PATH=" .. shell_quote(lua_path(t)) }
  if opts.timeout then parts[#parts + 1] = "timeout " .. shell_quote(opts.timeout) end
  parts[#parts + 1] = shell_quote(t:path("src", "luajit"))
  parts[#parts + 1] = shell_quote(dumpopt)
  parts[#parts + 1] = "-e " .. shell_quote(code)
  t:run(table.concat(parts, " ") .. " >" .. shell_quote(dump) .. " 2>&1",
        { quiet = opts.quiet })
end

local function build_default(t)
  t:make(nil, { quiet = true, jobs = false })
end

local function build_and_run_c(t, out, cfile, opts)
  opts = opts or {}
  if opts.build ~= false then
    if opts.clean then
      t:build({ clean = true, quiet = true })
    else
      build_default(t)
    end
  end
  t:cc(out, { t:path("tests", cfile) }, {
    link_luajit = true,
    libs = { "-lm", "-ldl", "-pthread" }
  })
  t:run({ out }, { timeout = opts.timeout })
end

local function assert_dump_contains(t, dump, needle, label)
  local data = t:read(dump)
  if not contains(data, needle) then
    error(label .. ": missing dump text: " .. needle, 2)
  end
end

local function assert_dump_match(t, dump, pattern, label)
  local data = t:read(dump)
  if not data:match(pattern) then
    error(label .. ": missing dump pattern: " .. pattern, 2)
  end
end

local function assert_dump_all_contains(t, dump, needles, label)
  for i = 1, #needles do
    assert_dump_contains(t, dump, needles[i], label)
  end
end

local function trace1_ir_state(t, dump)
  local st = {
    array = 0,
    hdradd = 0,
    xload = 0,
    asize = 0,
    eq = 0,
    aref = false,
    aload = false,
    xpoll = false,
    ule = false,
    href = false,
    hrefk = false,
    hmask = false,
    node = false,
    done = false
  }
  local inir = false
  for line in lines(t:read(dump)) do
    if contains(line, "---- TRACE 1 IR") then
      inir = true
    elseif inir and contains(line, "---- TRACE 1 stop") then
      st.done = true
      break
    elseif inir then
      if line:match("FLOAD .*tab[.]array") then st.array = st.array + 1 end
      if contains(line, " p64 ADD ") and contains(line, "-16") then
        st.hdradd = st.hdradd + 1
      end
      if contains(line, " XLOAD ") then st.xload = st.xload + 1 end
      if line:match("FLOAD .*tab[.]asize") then st.asize = st.asize + 1 end
      if contains(line, " p64 EQ ") then st.eq = st.eq + 1 end
      if contains(line, " ULE ") then st.ule = true end
      if contains(line, " HREF ") then st.href = true end
      if contains(line, " HREFK") then st.hrefk = true end
      if contains(line, " AREF ") then st.aref = true end
      if contains(line, " ALOAD ") then st.aload = true end
      if contains(line, " XPOLL ") or contains(line, "XPOLL") then st.xpoll = true end
      if contains(line, "tab.hmask") then st.hmask = true end
      if contains(line, "tab.node") then st.node = true end
    end
  end
  return st
end

local function assert_trace1_ir(t, dump, label, pred)
  local st = trace1_ir_state(t, dump)
  if not st.done or not pred(st) then
    io.stderr:write(t:read(dump))
    error(label, 2)
  end
end

local function assert_marker_set(t, paths, needles, label)
  for i = 1, #needles do
    local ok = false
    for j = 1, #paths do
      if contains(t:read(paths[j]), needles[i]) then
        ok = true
        break
      end
    end
    if not ok then error(label .. ": missing marker: " .. needles[i], 2) end
  end
end

local function assert_section_no_lines(t, label, path, start_text, end_text, pred)
  local data = t:text_between(path, start_text, end_text)
  local hits = {}
  local n = 0
  for line in lines(data) do
    n = n + 1
    if pred(line) then hits[#hits + 1] = tostring(n) .. ": " .. line end
  end
  if #hits > 0 then error(label .. ":\n" .. table.concat(hits, "\n"), 2) end
end

local function assert_block_no_lines(t, label, path, start_text, pred)
  local data = t:c_block(path, start_text)
  local hits = {}
  local n = 0
  for line in lines(data) do
    n = n + 1
    if pred(line) then hits[#hits + 1] = tostring(n) .. ": " .. line end
  end
  if #hits > 0 then error(label .. ":\n" .. table.concat(hits, "\n"), 2) end
end

local function assert_order_positions(label, data, ordered)
  local pos = 1
  local found = {}
  for i = 1, #ordered do
    local p = data:find(ordered[i], 1, true)
    if not p then error(label .. ": missing expected text: " .. ordered[i], 2) end
    found[i] = p
  end
  for i = 2, #found do
    if found[i - 1] >= found[i] then
      error(label .. ": wrong ordering near: " .. ordered[i], 2)
    end
  end
  pos = pos
end

local function raw_index_write(line, name_pattern)
  return line:match(name_pattern .. "%[[^%]]+%]%s*=[^=]") ~= nil
end

local function raw_cast_write(line)
  if contains(line, "lj_mcode_rw") then return false end
  local eq = line:find("=", 1, true)
  if not eq or line:sub(eq + 1, eq + 1) == "=" then return false end
  local lhs = line:sub(1, eq - 1)
  return contains(lhs, "*(uint16_t *)") or
         contains(lhs, "*(uint32_t *)") or
         contains(lhs, "*(uint64_t *)") or
         contains(lhs, "*(int16_t *)") or
         contains(lhs, "*(int32_t *)") or
         contains(lhs, "*(int64_t *)")
end

local function x64_cmp_poll_pattern()
  return "cmp dword %[r14%+0x[0-9a-f]+%], %+0x00"
end

local function assert_loop_ir_markers(t, dump, label, markers)
  local data = t:read(dump)
  local loop = false
  local seen = {}
  for line in lines(data) do
    if contains(line, "------ LOOP") then
      loop = true
    elseif contains(line, "---- TRACE 1 mcode") then
      break
    elseif loop then
      for i = 1, #markers do
        if contains(line, markers[i]) then seen[markers[i]] = true end
      end
    end
  end
  for i = 1, #markers do
    if not seen[markers[i]] then
      io.stderr:write(data)
      error(label .. ": missing loop marker: " .. markers[i], 2)
    end
  end
end

local function assert_call_after_loop_polls(t, dump, label, call, mincmp, extra)
  local data = t:read(dump)
  local loop, cmp, done = false, 0, false
  local state = {}
  for line in lines(data) do
    if contains(line, "->LOOP:") then loop = true end
    if loop and line:match(x64_cmp_poll_pattern()) then cmp = cmp + 1 end
    if loop and extra then extra(line, state) end
    if loop and contains(line, call) then
      done = true
      break
    end
  end
  if not done or cmp < mincmp or (extra and state.fail) then
    io.stderr:write(data)
    error(label, 2)
  end
  return state
end

local m6_cases = {
  "m6_dispatch_redispatch",
  "m6_jit_token",
  "m6_jit_cell_ops",
  "m6_jit_barrier_xpoll",
  "m6_jit_xbar_xpoll",
  "m6_jit_table_store_helper",
  "m6_jit_aref_pair_guard",
  "m6_jit_hrefk_nodehdr",
  "m6_jit_href_nodehdr",
  "m6_jit_alloc_account",
  "m6_jit_gc2_readiness",
  "m6_jit_gcstep_guard",
  "m6_jit_mcode_publish",
  "m6_jit_flush_hs"
}

local function table_store_smoke()
  return [=[
local util = require("jit.util")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = 0 }
for i = 1, 200 do
  h.stable = i
end
assert(h.stable == 200)
assert(util.traceinfo(1), "shared existing hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local hhole = { stable = 0 }
hhole.stable = nil
for i = 1, 200 do
  hhole.stable = i
  hhole.stable = nil
end
assert(hhole.stable == nil)
assert(util.traceinfo(1), "previous-nil hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local a = { 0 }
for i = 1, 200 do
  a[1] = i
end
assert(a[1] == 200)
assert(util.traceinfo(1), "shared existing array table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local ahole = { 0, nil, 0 }
for i = 1, 200 do
  ahole[2] = i
  ahole[2] = nil
end
assert(ahole[2] == nil)
assert(util.traceinfo(1), "previous-nil array table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local wk = setmetatable({ stable = 0 }, { __mode = "k" })
for i = 1, 200 do
  wk.stable = i
end
assert(wk.stable == 200)
assert(util.traceinfo(1), "weak-key existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local wv = setmetatable({ 0 }, { __mode = "v" })
for i = 1, 200 do
  wv[1] = i
end
assert(wv[1] == 200)
assert(util.traceinfo(1), "weak-value existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function hash_insert(n)
  local out = { stable = 0 }
  for i = 1, n do
    local t = {}
    t.stable = i
    out = t
  end
  return out
end
local hi = hash_insert(80)
assert(hi.stable == 80)
assert(util.traceinfo(1), "trace-local hash insertion did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function array_insert(n)
  local out = { 0 }
  for i = 1, n do
    local t = {}
    t[1] = i
    out = t
  end
  return out
end
local ai = array_insert(80)
assert(ai[1] == 80)
assert(util.traceinfo(1), "trace-local array insertion did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function phi_store(n)
  local a = { stable = 0 }
  local b = { stable = 0 }
  local t = a
  for i = 1, n do
    if i == 1 then t = a else t = b end
    t.stable = i
  end
  return a.stable + b.stable
end
assert(phi_store(80) == 81)
assert(util.traceinfo(1), "PHI-carried existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local up = { stable = 0 }
local function upvalue_store(n)
  for i = 1, n do
    up.stable = i
  end
  return up.stable
end
assert(upvalue_store(80) == 80)
assert(util.traceinfo(1), "upvalue-carried existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function make_escaped_store()
  local sink
  return function(n)
    for i = 1, n do
      local t = { stable = 0 }
      sink = t
      t.stable = i
    end
    return sink.stable
  end
end
local escaped_store = make_escaped_store()
assert(escaped_store(80) == 80)
assert(util.traceinfo(1), "closed-upvalue escaped existing table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function make_nested_escape()
  local sink
  return function(n)
    for i = 1, n do
      local outer = { inner = false }
      local t = { stable = 0 }
      outer.inner = t
      sink = outer
      t.stable = i
    end
    return sink.inner.stable
  end
end
local nested_escape = make_nested_escape()
assert(nested_escape(80) == 80)
assert(util.traceinfo(1), "nested escaped existing table store did not trace")
]=]
end

return function(add)
  add({
    name = "m6_dispatch_redispatch",
    description = "M6 dispatch redispatch and x64 TG-local dispatch guards",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build_and_run_c(t, t:tmp("lj_t_safepoint_handshake"),
                      "t-safepoint-handshake.c",
                      { build = false })

      local vm = t:path("src", "vm_x64.dasc")
      t:assert_not_contains(vm, "Secondary TGs interpret until RID_DISPATCH is local")
      assert_no_lines(t, "x64 VM must not derive g/J from fixed DISPATCH offsets",
                      { vm }, function(line)
        return line:match("DISPATCH_[GJ]%(") ~= nil
      end)
      assert_no_lines(t, "x64 VM must not derive g/J from fixed TG dispatch offsets",
                      { vm }, function(line)
        return contains(line, "TG_DISP2J") or contains(line, "TG_DISP2G")
      end)
      assert_no_lines(t, "x64 VM entry must use the running TG dispatch table",
                      { vm }, function(line)
        return contains(line, "GG_G2TGDISP") or
               (contains(line, "L:RB->glref") and contains(line, "dispatch")) or
               contains(line, "add DISPATCH, GG_G2TGDISP")
      end)
      assert_no_lines(t, "transitional TG dispatch offset macros must stay removed",
                      { t:path("src", "lj_dispatch.h") }, function(line)
        return contains(line, "GG_OFS_TGDISP") or contains(line, "GG_G2TGDISP") or
               contains(line, "TG_DISP2J") or contains(line, "TG_DISP2G")
      end)
      t:assert_not_contains(t:path("src", "lj_dispatch.c"), "DISPMODE_REC")
      print("M6 dispatch redispatch guard passed")
    end
  })

  add({
    name = "m6_jit_token",
    description = "M6 JIT recorder token and x64 XPOLL behavior",
    run = function(t)
      build_default(t)
      build_and_run_c(t, t:tmp("lj_t-jit-token"), "t-jit-token.c",
                      { build = false, timeout = "20s" })
      luajit_file(t, t:path("tests", "t-jit-secondary.lua"), { timeout = "20s" })

      local dump = t:tmp("lj_t-jit-xpoll.dump")
      luajit_dump(t, dump, "-jdump=im", [=[
jit.opt.start("hotloop=1","hotexit=1")
local s=0.0
for i=1,64 do s=s+i end
assert(s==2080.0)
]=], { timeout = "20s" })
      assert_dump_contains(t, dump, "XPOLL", "x64 loop trace")
      local d = t:read(dump)
      if not contains(d, "->LOOP:") or not d:match(x64_cmp_poll_pattern()) then
        error("x64 IR_XPOLL must lower to a TG poll at the loop label", 2)
      end

      local funcf_dump = t:tmp("lj_t-jit-xpoll-funcf.dump")
      luajit_dump(t, funcf_dump, "-jdump=im", [=[
jit.opt.start("hotloop=1","hotexit=1","callunroll=32","recunroll=32")
local function f10(x) return x+1 end
local function f9(x) return f10(x)+1 end
local function f8(x) return f9(x)+1 end
local function f7(x) return f8(x)+1 end
local function f6(x) return f7(x)+1 end
local function f5(x) return f6(x)+1 end
local function f4(x) return f5(x)+1 end
local function f3(x) return f4(x)+1 end
local function f2(x) return f3(x)+1 end
local function f1(x) return f2(x)+1 end
local s=0
for i=1,64 do s=s+f1(i) end
assert(s==2720)
]=], { timeout = "20s" })
      d = t:read(funcf_dump)
      if count_plain(d, "XPOLL") < 4 then
        error("deep inlined FUNCF traces must materialize depth XPOLL", 2)
      end
      if count_match(d, x64_cmp_poll_pattern()) < 4 then
        error("FUNCF-depth IR_XPOLL must lower to TG poll checks", 2)
      end
      print("M6 JIT recorder token behavior passed")
    end
  })

  add({
    name = "m6_jit_cell_ops",
    description = "M6 local-cell JIT recording behavior",
    run = function(t)
      build_default(t)
      local dump = t:tmp("lj_m6_jit_cell_ops.dump")
      luajit_dump(t, dump, "-jdump=i", [=[
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = 0
  local function touch() return x end
  for i = 1, n do x = x + 1 end
  return x, touch
end
local v, f = run(200)
assert(v == 200 and f() == 200)
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "owner numeric trace")
      assert_dump_contains(t, dump, "UREFC", "owner numeric UREFC")
      assert_dump_contains(t, dump, "ULOAD", "owner numeric ULOAD")
      assert_dump_contains(t, dump, "USTORE", "owner numeric USTORE")

      luajit_dump(t, dump, "-jdump=i", [=[
local pool = { "even", "odd" }
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = pool[1]
  local function get() return x end
  for i = 1, n do x = pool[(i % 2) + 1] end
  return get()
end
assert(run(200) == pool[1])
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "owner GC-valued trace")
      assert_dump_contains(t, dump, "UREFC", "owner GC-valued UREFC")
      assert_dump_contains(t, dump, "USTORE", "owner GC-valued USTORE")
      assert_dump_contains(t, dump, "OBAR", "owner GC-valued OBAR")

      luajit_dump(t, dump, "-jdump=i", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local src = function(n)
  local x = 0
  local function touch() return x end
  for i = 1, n do x = x + 1 end
  return x, touch
end
local run = assert(loadstring(string.dump(src)))
local v, f = run(200)
assert(v == 200 and f() == 200)
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "loaded v4 CGET/CSET trace")
      assert_dump_contains(t, dump, "UREFC", "loaded v4 CGET/CSET UREFC")
      assert_dump_contains(t, dump, "ULOAD", "loaded v4 CGET/CSET ULOAD")
      assert_dump_contains(t, dump, "USTORE", "loaded v4 CGET/CSET USTORE")

      luajit_dump(t, dump, "-jdump=i", [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1")
local function run(n)
  local keep
  for i = 1, n do
    local function f() return f end
    keep = f
  end
  return keep
end
local f = run(30)
assert(f() == f)
assert(util.traceinfo(1), "source CNEW/FNEW creation should trace")
]=])
      assert_dump_match(t, dump, "CALLS.*lj_func_newuvcell_forjit", "source CNEW helper call")
      assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "source FNEW helper call")
      assert_dump_contains(t, dump, "UREFC", "source CNEW/FNEW UREFC")
      assert_dump_contains(t, dump, "USTORE", "source CNEW/FNEW USTORE")
      assert_dump_contains(t, dump, "OBAR", "source CNEW/FNEW OBAR")

      luajit_dump(t, dump, "-jdump=i", [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1")
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
assert(util.traceinfo(1), "loaded CNEW/FNEW creation should trace")
]=])
      assert_dump_match(t, dump, "CALLS.*lj_func_newuvcell_forjit", "loaded CNEW helper call")
      assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "loaded FNEW helper call")
      assert_dump_contains(t, dump, "UREFC", "loaded CNEW/FNEW UREFC")
      assert_dump_contains(t, dump, "USTORE", "loaded CNEW/FNEW USTORE")
      assert_dump_contains(t, dump, "OBAR", "loaded CNEW/FNEW OBAR")

      luajit_dump(t, dump, "-jdump=i", [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = 1
  local keep
  for i = 1, n do
    local function f() return f, x end
    keep = f
  end
  return keep
end
local f = run(30)
local self, x = f()
assert(self == f and x == 1)
assert(util.traceinfo(1), "source mixed raw-local CNEW/FNEW should trace")
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "source mixed raw-local FNEW trace")
      assert_dump_contains(t, dump, "TMPREF", "source mixed raw-local TMPREF")
      assert_dump_match(t, dump, "CALLS.*lj_func_syncslot_forjit", "source mixed raw-local sync helper")
      assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "source mixed raw-local FNEW helper")
      assert_dump_contains(t, dump, "UREFC", "source mixed raw-local UREFC")
      assert_dump_contains(t, dump, "USTORE", "source mixed raw-local USTORE")

      luajit_dump(t, dump, "-jdump=i", [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local src = function(n)
  local x = 1
  local keep
  for i = 1, n do
    local function f() return f, x end
    keep = f
  end
  return keep
end
local run = assert(loadstring(string.dump(src)))
local f = run(30)
local self, x = f()
assert(self == f and x == 1)
assert(util.traceinfo(1), "loaded mixed raw-local CNEW/FNEW should trace")
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "loaded mixed raw-local FNEW trace")
      assert_dump_match(t, dump, "CALLS.*lj_func_syncslot_forjit", "loaded mixed raw-local sync helper")
      assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "loaded mixed raw-local FNEW helper")

      luajit_dump(t, dump, "-jdump=i", [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local dummy = function() end
  local x = 0
  local keep = dummy
  for i = 1, n do
    x = x + 1
    if i >= 2 then
      local function f() return f, x end
      keep = f
    end
  end
  return keep, x
end
local f, x = run(30)
local self, fx = f()
assert(self == f and fx == 30 and x == 30)
assert(util.traceinfo(1), "source first-promotion FNEW should trace")
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "source first-promotion FNEW trace")
      assert_dump_match(t, dump, "CALLS.*lj_func_promoteuv_forjit", "source first-promotion helper")
      assert_dump_contains(t, dump, "NULL", "source first-promotion stack snapshot argument")
      assert_dump_match(t, dump, "SLOAD.*I", "source first-promotion inherited cell reload")
      assert_dump_contains(t, dump, "UREFC", "source first-promotion UREFC")
      assert_dump_contains(t, dump, "USTORE", "source first-promotion USTORE")

      luajit_dump(t, dump, "-jdump=i", [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local src = function(n)
  local dummy = function() end
  local x = 0
  local keep = dummy
  for i = 1, n do
    x = x + 1
    if i >= 2 then
      local function f() return f, x end
      keep = f
    end
  end
  return keep, x
end
local run = assert(loadstring(string.dump(src)))
local f, x = run(30)
local self, fx = f()
assert(self == f and fx == 30 and x == 30)
assert(util.traceinfo(1), "loaded first-promotion FNEW should trace")
]=])
      assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "loaded first-promotion FNEW trace")
      assert_dump_match(t, dump, "CALLS.*lj_func_promoteuv_forjit", "loaded first-promotion helper")
      assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "loaded first-promotion FNEW helper")

      luajit_code(t, [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = 0
  local keep
  for i = 1, n do
    x = x + 1
    local function f() return f, x end
    keep = f
  end
  return keep
end
local f = run(30)
local self, x = f()
assert(self == f and x == 30)
assert(util.traceinfo(1), "pre-FNEW promoted local update should trace")
]=])
      luajit_code(t, [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function run(n)
  local x = 0
  local keep
  for i = 1, n do
    local function f() return f, x end
    x = x + 1
    keep = f
  end
  return keep, x
end
local f, x = run(30)
local self, fx = f()
assert(self == f and fx == 30 and x == 30)
assert(util.traceinfo(1), "post-FNEW promoted local update should trace")
]=])
      print("M6 JIT local-cell behavior passed")
    end
  })

  add({
    name = "m6_jit_barrier_xpoll",
    description = "x64 trace barrier behavior across XPOLL poll regions",
    run = function(t)
      build_default(t)
      local tbar = t:tmp("lj_t-jit-tbar-xpoll.dump")
      luajit_dump(t, tbar, "-jdump=im", [=[
jit.opt.start("hotloop=1","hotexit=1")
jit.off()
local t={}
local mts={}
for i=1,80 do mts[i]={} end
jit.on()
for i=1,64 do setmetatable(t, mts[i]) end
]=], { timeout = "20s" })
      assert_loop_ir_markers(t, tbar, "setmetatable loop", { "XPOLL", "FSTORE", "TBAR" })
      assert_call_after_loop_polls(t, tbar,
                                   "post-XPOLL TBAR must lower to poll+mark checks and GC2 call",
                                   "lj_gc2_barrier_tab_g", 2)

      local obar = t:tmp("lj_t-jit-obar-xpoll.dump")
      luajit_dump(t, obar, "-jdump=im", [=[
jit.opt.start("hotloop=1","hotexit=1")
jit.off()
local uv
local vals={}
for i=1,80 do vals[i]={} end
jit.on()
local function f()
  for i=1,64 do uv=vals[i] end
end
f()
assert(uv==vals[64])
]=], { timeout = "20s" })
      assert_loop_ir_markers(t, obar, "upvalue loop", { "XPOLL", "USTORE", "OBAR" })
      local data = t:read(obar)
      local loop, test, cmp, pubuv, store_before = false, 0, 0, false, false
      for line in lines(data) do
        if contains(line, "->LOOP:") then loop = true end
        if loop and line:match("test byte") then test = test + 1 end
        if loop and line:match(x64_cmp_poll_pattern()) then cmp = cmp + 1 end
        if loop and contains(line, "lj_func_storeuv_forjit") and not pubuv then
          store_before = true
        end
        if loop and contains(line, "lj_gc_pubuv") then
          pubuv = true
          break
        end
      end
      if not pubuv or test < 2 or cmp < 2 then
        error("post-XPOLL OBAR must lower to legacy tests, poll+mark checks and pubuv call", 2)
      end
      if not store_before then
        error("x64 upvalue USTORE must release-copy before OBAR publication", 2)
      end
      print("M6 JIT XPOLL barrier behavior passed")
    end
  })

  add({
    name = "m6_jit_xbar_xpoll",
    description = "FFI XBAR aliasing respects XPOLL poll regions",
    run = function(t)
      build_default(t)
      assert_marker_set(t, {
        t:path("src", "lj_opt_mem.c"),
        t:path("src", "lj_asm.c"),
        t:path("src", "lj_opt_fold.c")
      }, {
        "static LJ_AINLINE IRRef poll_alias_limit(jit_State *J, IRRef lim)",
        "J->chain[IR_XBAR] > lim",
        "J->chain[IR_XPOLL] > lim",
        "lim = poll_alias_limit(J, lim);",
        "case IR_NOP: case IR_XBAR:",
        "LJFOLD(XBAR)"
      }, "XBAR/XPOLL alias")
      if count_plain(t:read(t:path("src", "lj_opt_mem.c")),
                     "lim = poll_alias_limit(J, lim);") < 2 then
        error("XLOAD forwarding and XSTORE DSE must both honor XPOLL", 2)
      end
      t:run({ t:path("tools", "ci", "m5_jit_hash_store_nyi.sh") })
      print("M6 JIT XBAR/XPOLL alias guard passed")
    end
  })

  add({
    name = "m6_jit_table_store_helper",
    description = "M6 helper-backed table store behavior",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build_and_run_c(t, t:tmp("lj_t-jit-forward-store"),
                      "t-jit-forward-store.c", { build = false })
      luajit_code(t, table_store_smoke())

      local hash_ir = t:tmp("lj-m6-hstore-ir.dump")
      luajit_dump(t, hash_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local util = require("jit.util")
local function run(n)
  local out = { stable = 0 }
  for i = 1, n do
    local t = { stable = 0 }
    t.stable = i
    out = t
  end
  return out
end
local out = run(40)
assert(out.stable == 40)
assert(util.traceinfo(1), "trace-local hash store did not trace")
]=])
      assert_dump_all_contains(t, hash_ir, { "TDUP", "HSTORE", "XPOLL" },
                               "trace-local hash store")

      local new_hash_ir = t:tmp("lj-m6-new-hstore-ir.dump")
      luajit_dump(t, new_hash_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local function run(n)
  local out = { stable = 0 }
  for i = 1, n do
    local t = {}
    t.stable = i
    out = t
  end
  return out
end
local out = run(40)
assert(out.stable == 40)
assert(util.traceinfo(1), "trace-local new hash store did not trace")
]=])
      assert_dump_all_contains(t, new_hash_ir, { "TNEW", "NEWREF", "HSTORE", "XPOLL" },
                               "trace-local new hash store")

      local old_nil_hash_ir = t:tmp("lj-m6-oldnil-hstore-ir.dump")
      luajit_dump(t, old_nil_hash_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local h = { stable = 0 }
h.stable = nil
for i = 1, 80 do
  h.stable = i
  h.stable = nil
end
assert(h.stable == nil)
assert(util.traceinfo(1), "previous-nil hash store did not trace")
]=])
      assert_dump_all_contains(t, old_nil_hash_ir, { "HSTORE", "TBAR", "XPOLL" },
                               "previous-nil hash store")

      local array_ir = t:tmp("lj-m6-astore-ir.dump")
      luajit_dump(t, array_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local util = require("jit.util")
local function run(n)
  local out = { 0 }
  for i = 1, n do
    local t = { 0 }
    t[1] = i
    out = t
  end
  return out
end
local out = run(40)
assert(out[1] == 40)
assert(util.traceinfo(1), "trace-local array store did not trace")
]=])
      assert_dump_all_contains(t, array_ir, { "TDUP", "ASTORE", "XPOLL" },
                               "trace-local array store")

      local new_array_ir = t:tmp("lj-m6-new-array-hstore-ir.dump")
      luajit_dump(t, new_array_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local function run(n)
  local out = { 0 }
  for i = 1, n do
    local t = {}
    t[1] = i
    out = t
  end
  return out
end
local out = run(40)
assert(out[1] == 40)
assert(util.traceinfo(1), "trace-local new numeric store did not trace")
]=])
      assert_dump_all_contains(t, new_array_ir, { "TNEW", "NEWREF", "HSTORE", "XPOLL" },
                               "trace-local new numeric store")

      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local t = {}
for k in pairs(package) do
  local s = tostring(k)
  t[#t+1] = s
  assert(t[#t] == s and type(t[#t]) == "string",
         "numeric NEWREF helper crossed src/key TValue temps")
end
assert(util.traceinfo(1), "numeric NEWREF append did not trace")
]=])

      local old_nil_array_ir = t:tmp("lj-m6-oldnil-astore-ir.dump")
      luajit_dump(t, old_nil_array_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local a = { 0, nil, 0 }
for i = 1, 80 do
  a[2] = i
  a[2] = nil
end
assert(a[2] == nil)
assert(util.traceinfo(1), "previous-nil array store did not trace")
]=])
      assert_dump_all_contains(t, old_nil_array_ir, { "ASTORE", "XPOLL" },
                               "previous-nil array store")

      local shared_hash_ir = t:tmp("lj-m6-shared-hstore-ir.dump")
      luajit_dump(t, shared_hash_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local h = { stable = 0 }
for i = 1, 80 do
  h.stable = i
end
assert(h.stable == 80)
assert(util.traceinfo(1), "shared existing hash store did not trace")
]=])
      assert_dump_all_contains(t, shared_hash_ir, { "HSTORE", "XPOLL" },
                               "shared existing hash store")

      local shared_array_ir = t:tmp("lj-m6-shared-astore-ir.dump")
      luajit_dump(t, shared_array_ir, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local a = { 0 }
for i = 1, 80 do
  a[1] = i
end
assert(a[1] == 80)
assert(util.traceinfo(1), "shared existing array store did not trace")
]=])
      assert_dump_all_contains(t, shared_array_ir, { "ASTORE", "XPOLL" },
                               "shared existing array store")
      print("M6 JIT table-store helper behavior passed")
    end
  })

  add({
    name = "m6_jit_aref_pair_guard",
    description = "M6 x64 shared-array AREF generation-pair behavior",
    run = function(t)
      build_default(t)
      local dump = t:tmp("lj-m6-aref-pair.dump")
      luajit_dump(t, dump, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.off()
local t = {}
for i = 1, 128 do t[i] = i end
jit.on()
local s = 0
for i = 1, 80 do
  local k = (i % 128) + 1
  s = s + (t[k] or 0)
end
assert(s > 0)
]=], { timeout = "20s" })
      assert_trace1_ir(t, dump,
                       "separated shared array reads must load bounds from TabArrayHdr",
                       function(st)
        return st.array >= 2 and st.hdradd >= 2 and st.xload >= 2 and
               st.asize == 0 and st.eq == 0 and st.aref and st.aload and st.xpoll
      end)

      local split = t:tmp("lj-m6-aref-pair-split.dump")
      luajit_dump(t, split, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.off()
local t = { 1, 2, 3, 4 }
for i = 5, 64 do t[i] = i end
jit.on()
local s = 0
for i = 1, 80 do
  local k = (i % 64) + 1
  s = s + (t[k] or 0)
end
assert(s > 0)
]=], { timeout = "20s" })
      assert_trace1_ir(t, split,
                       "split-from-colocated arrays must use header bounds after publish",
                       function(st)
        return st.array >= 2 and st.hdradd >= 2 and st.xload >= 2 and
               st.asize == 0 and st.eq == 0 and st.aref and st.aload and st.xpoll
      end)

      local colo = t:tmp("lj-m6-aref-pair-colo.dump")
      luajit_dump(t, colo, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.off()
local t = { 1, 2, 3, 4 }
jit.on()
local s = 0
for i = 1, 80 do
  local k = (i % 4) + 1
  s = s + (t[k] or 0)
end
assert(s > 0)
]=], { timeout = "20s" })
      assert_trace1_ir(t, colo,
                       "colocated shared array reads must keep the legacy pair guard",
                       function(st)
        return st.array >= 4 and st.asize >= 2 and st.eq >= 2 and
               st.xload == 0 and st.aref and st.aload and st.xpoll
      end)

      local miss = t:tmp("lj-m6-aref-pair-miss.dump")
      luajit_dump(t, miss, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.off()
local t = {}
for i = 1, 128 do t[i] = i end
jit.on()
local s = 0
for i = 1, 80 do
  local k = 160 + (i % 2)
  if t[k] == nil then s = s + 1 end
end
assert(s == 80)
]=], { timeout = "20s" })
      assert_trace1_ir(t, miss,
                       "separated shared out-of-array guards must load bounds from TabArrayHdr",
                       function(st)
        return st.array >= 2 and st.hdradd >= 2 and st.xload >= 2 and
               st.asize == 0 and st.ule and st.href and not st.aref and st.xpoll
      end)

      luajit_code(t, [=[
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.off()
local t = {}
for i = 1, 32 do t[i] = i end
jit.on()
local idx = 1
local function read(n)
  local s = 0
  for _ = 1, n do
    s = s + (t[idx] or 0)
  end
  return s
end
assert(read(80) == 80)
assert(util.traceinfo(1), "shared array read did not trace")
jit.off()
for i = 33, 128 do t[i] = i end
jit.on()
idx = 64
assert(read(80) == 5120)
assert(util.traceinfo(1), "trace missing after array grow")
]=], { timeout = "20s" })
      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local keep = {}
for i = 1, 120 do
  local t = {}
  for j = 1, 80 do
    t[j] = "value-" .. i .. "-" .. j
  end
  keep[i] = t
end
assert(#keep == 120 and keep[120][80] == "value-120-80")
]=], { timeout = "20s" })
      print("M6 JIT shared AREF generation-pair behavior passed")
    end
  })

  add({
    name = "m6_jit_hrefk_nodehdr",
    description = "M6 x64 HREFK node-header behavior",
    run = function(t)
      build_default(t)
      local dump = t:tmp("lj-m6-hrefk-ir.dump")
      luajit_dump(t, dump, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = { foo = 1, bar = 2, baz = 3 }
local s = 0
for i = 1, 60 do
  s = s + t.foo
end
assert(s == 60)
]=])
      assert_trace1_ir(t, dump,
                       "TRACE 1 HREFK must use tab.node without tab.hmask mirror guard",
                       function(st)
        return st.node and st.hrefk and st.xpoll and not st.hmask
      end)
      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = { foo = 1, bar = 2, baz = 3 }
local function run(n)
  local s = 0
  for i = 1, n do
    s = s + t.foo
  end
  return s
end
assert(run(80) == 80)
for i = 1, 2000 do
  t["resize_" .. i] = i
end
t.foo = 7
assert(run(80) == 560)
]=])
      print("M6 JIT HREFK node-header behavior passed")
    end
  })

  add({
    name = "m6_jit_href_nodehdr",
    description = "M6 x64 dynamic HREF node-header behavior",
    run = function(t)
      local dump = t:tmp("lj-m6-href-nodehdr.dump")
      luajit_dump(t, dump, "-jdump=ir", [=[
jit.opt.start("hotloop=1", "hotexit=1")
local keys = {"a", "b"}
local t = {a = 10, b = 20}
local s = 0
local function f(k)
  return t[k]
end
for i = 1, 80 do
  s = s + f(keys[i % 2 + 1])
end
assert(s > 0)
]=], { timeout = "20s" })
      assert_trace1_ir(t, dump,
                       "dynamic string-key lookup must record HREF, not HREFK",
                       function(st) return st.href and not st.hrefk end)

      luajit_dump(t, dump, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = {}
local keys = {"missing_a", "missing_b"}
local seen = 0
for i = 1, 80 do
  local k = keys[i % 2 + 1]
  if t[k] == nil then seen = seen + 1 end
end
assert(seen == 80)
]=], { timeout = "20s" })
      assert_trace1_ir(t, dump,
                       "x64 empty-hash miss must fall through to HREF without tab.hmask",
                       function(st) return st.href and not st.hrefk and not st.hmask end)
      print("M6 JIT dynamic HREF node-header behavior passed")
    end
  })

  add({
    name = "m6_jit_alloc_account",
    description = "M6 allocator accounting behavior",
    run = function(t)
      build_default(t)
      build_and_run_c(t, t:tmp("lj_t-gc2-alloc-account"),
                      "t-gc2-alloc-account.c", { build = false, timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-gc2-interp-hard-check"),
                      "t-gc2-interp-hard-check.c", { build = false, timeout = "20s" })
      print("M6 JIT allocator accounting behavior passed")
    end
  })

  add({
    name = "m6_jit_gc2_readiness",
    description = "GC2 allocation-pacing readiness behavior",
    run = function(t)
      build_default(t)
      build_and_run_c(t, t:tmp("lj_t-gc2-jit-hard-check"),
                      "t-gc2-jit-hard-check.c", { build = false, timeout = "20s" })

      local tnew = t:tmp("lj_t-jit-gc2-readiness-tnew.dump")
      luajit_dump(t, tnew, "-jdump=ir", [=[
jit.opt.start("hotloop=1","hotexit=1")
local x
for i=1,100 do x={} end
assert(type(x)=="table")
]=], { timeout = "20s" })
      assert_dump_all_contains(t, tnew, { "TNEW", "XPOLL", "GCSTEP" }, "TNEW readiness")

      local cnew = t:tmp("lj_t-jit-gc2-readiness-cnew.dump")
      luajit_dump(t, cnew, "-jdump=ir", [=[
local ffi=require("ffi")
ffi.cdef("typedef struct { int x; } lj_gc2_dump_cnew_t;")
local ct=ffi.typeof("lj_gc2_dump_cnew_t")
jit.opt.start("hotloop=1","hotexit=1","-sink")
local x
for i=1,100 do x=ct(i) end
assert(x.x==100)
]=], { timeout = "20s" })
      assert_dump_all_contains(t, cnew, { "CNEW", "XPOLL" }, "CNEW readiness")

      local snew = t:tmp("lj_t-jit-gc2-readiness-snew.dump")
      luajit_dump(t, snew, "-jdump=ir", [=[
jit.opt.start("hotloop=1","hotexit=1","-sink")
local s="abcdef"
local x
for i=1,100 do x=string.sub(s,1,3) end
assert(x=="abc")
]=], { timeout = "20s" })
      assert_dump_all_contains(t, snew, { "SNEW", "XPOLL" }, "SNEW readiness")
      print("M6 JIT GC2 readiness behavior passed")
    end
  })

  add({
    name = "m6_jit_gcstep_guard",
    description = "legacy JIT GC-step pacing behavior",
    run = function(t)
      build_default(t)
      local dump = t:tmp("lj_t-jit-gcstep.dump")
      luajit_dump(t, dump, "-jdump=ir", [=[
jit.opt.start("hotloop=1","hotexit=1")
local x
for i=1,100 do x={} end
assert(type(x)=="table")
]=], { timeout = "20s" })
      assert_dump_contains(t, dump, "GCSTEP", "sunk allocation replay")
      luajit_file(t, t:path("tests", "stock", "test", "misc", "gcstep.lua"),
                  { timeout = "20s" })
      print("M6 JIT GC-step behavior passed")
    end
  })

  add({
    name = "m6_jit_mcode_publish",
    description = "Linux/x64 mcode sync-core publication ordering",
    run = function(t)
      build_default(t)
      local lj_mcode = t:path("src", "lj_mcode.c")
      do
        local inx64 = false
        for line in lines(t:read(lj_mcode)) do
          if contains(line, "#if defined(__linux__) && LJ_TARGET_X64") then
            inx64 = true
          elseif inx64 and contains(line, "#endif") then
            inx64 = false
          elseif inx64 and contains(line, "MCPROT_RWX") then
            error("secure Linux/x64 mcode bridge must not fall back to RWX", 2)
          end
        end
      end
      assert_no_lines(t, "mcode publication bridge must not be hidden behind LJ_MT",
                      { lj_mcode }, function(line)
        return line:match("#if%s+LJ_MT") or line:match("#ifdef%s+LJ_MT") or
               contains(line, "LUAJIT_THREADSAFE")
      end)
      assert_no_lines(t, "Linux/x64 mcode bridge must not retain the single-map scaffold",
                      { lj_mcode }, function(line)
        return contains(line, "single-map write view") or contains(line, "rw == rx") or
               contains(line, "rw = J->mcarea")
      end)
      assert_no_lines(t, "x64 mcode bottom writes must go through lj_mcode_rw helpers",
                      { t:path("src", "lj_emit_x86.h"), t:path("src", "lj_asm_x86.h") },
                      function(line)
        return contains(line, "*as->mcbot") or contains(line, "*mxp++") or
               contains(line, "*(uint64_t *)as->mcbot") or
               contains(line, "*(void **)mxp") or contains(line, "memcpy(mxp")
      end)
      assert_no_lines(t, "x64 core emitter writes must go through lj_mcode_rw helpers",
                      { t:path("src", "lj_emit_x86.h") }, function(line)
        return contains(line, "*--as->mcp") or
               raw_index_write(line, "as%->mcp") or
               raw_index_write(line, "source") or
               raw_index_write(" " .. line, "[^%w_]p") or
               raw_cast_write(line)
      end)

      do
        local bad, seen = false, false
        for line in lines(t:read(t:path("src", "lj_asm_x86.h"))) do
          if contains(line, "void lj_asm_patchexit(jit_State *J, GCtrace *T, ExitNo exitno, MCode *target)") then
            seen = true
            break
          end
          if contains(line, "*--as->mcp") or raw_index_write(line, "as%->mcp") or
             contains(line, "*--p") or
             (not contains(line, "MCode *patchnfpr") and line:match("%*patchnfpr%s*=[^=]")) or
             (not contains(line, "MCode *q") and line:match("%*q%s*[-+]?=[^=]")) or
             raw_index_write(" " .. line, "[^%w_]p") or raw_cast_write(line) then
            bad = true
            break
          end
        end
        if not seen then
          error("lj_asm_patchexit marker missing", 2)
        end
        if bad then
          error("x64 generation-time asm writes must go through lj_mcode_rw helpers", 2)
        end
      end
      assert_block_no_lines(t, "x64 trace tail fixups must go through lj_mcode_rw helpers",
                            t:path("src", "lj_asm_x86.h"),
                            "static void asm_tail_fixup(ASMState *as, TraceNo lnk)",
                            function(line)
        return contains(line, "*mcp++") or contains(line, "mcp += 4") or
               contains(line, "*(int32_t *)mcp") or
               contains(line, "*(int32_t *)(mcp-4)") or
               contains(line, "*--as->mctop")
      end)
      assert_block_no_lines(t, "x64 committed-code exit patches must go through lj_mcode_rw helpers",
                            t:path("src", "lj_asm_x86.h"),
                            "void lj_asm_patchexit(jit_State *J, GCtrace *T, ExitNo exitno, MCode *target)",
                            function(line)
        return raw_index_write(" " .. line, "[^%w_]p") or raw_cast_write(line)
      end)

      local reserve = t:c_block(lj_mcode, "MCode *lj_mcode_reserve(jit_State *J, MCode **lim)")
      t:assert_text_ordered("lj_mcode_reserve", reserve, {
        "if (!J->mcarea)",
        "mcode_allocarea(J, mcode_default_size(J))",
        "else",
        "mcode_protect(J, MCPROT_GEN)"
      })

      local stop = t:c_block(t:path("src", "lj_trace.c"), "static void trace_stop(jit_State *J)")
      assert_order_positions("trace_stop", stop, {
        "lj_mcode_commit(J, J->cur.mcode)",
        "lj_mcode_sync_core(J)",
        "trace_save(J, T)",
        "proto_trace_rel(pt, traceno)",
        "bc_publish(patchpc, patchins)",
        "trace_exittarget_rel(parent, J->exitno, T->mcode)",
        "trace_nextside_rel(root, traceno)",
        "trace_link_rel(parent, traceno)"
      })
      local timeout = os.getenv("M6_MCODE_TIMEOUT") or "60s"
      luajit_code(t, [=[
jit.opt.start("hotloop=1","hotexit=1")
local s=0
for i=1,80 do s=s+i end
assert(s==3240)
]=], { timeout = timeout })
      luajit_code(t, [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "sizemcode=4", "maxmcode=8")
local function make(seed)
  return assert(loadstring(("return function(n) local s=%d; for i=1,n do s=s+i end return s end"):format(seed)))()
end
for n = 1, 12 do
  local seed = n * 17
  local f = make(seed)
  for _ = 1, 8 do
    assert(f(32) == seed + 528)
  end
end
local live = 0
for tr = 1, 40 do
  if util.traceinfo(tr) then live = live + 1 end
end
assert(live >= 8, live)
]=], { timeout = timeout })
      luajit_file(t, t:path("tests", "t-jit-mcode-fresh.lua"),
                  { lua_path = true, timeout = "30s" })
      print("M6 JIT mcode publication guard passed")
    end
  })

  add({
    name = "m6_jit_flush_hs",
    description = "JIT flush safepoint-scoped publication and retirement",
    run = function(t)
      build_default(t)
      assert_no_lines(t, "full trace flush callers must route through HS_FLUSHJ",
                      {
                        t:path("src", "lj_trace.c"),
                        t:path("src", "lj_record.c"),
                        t:path("src", "lj_dispatch.c"),
                        t:path("src", "lj_api.c"),
                        t:path("src", "lj_profile.c"),
                        t:path("src", "lib_ffi.c")
                      }, function(line)
        return contains(line, "lj_trace_flushall(J->L)") or
               contains(line, "lj_trace_flushall(L)")
      end)
      t:run({ t:path("tools", "ci", "m5_jit_trace_publish.sh") })
      t:run({ t:path("tools", "ci", "m3_vm_safepoint.sh") })
      luajit_file(t, t:path("tests", "stock", "test", "misc", "jit_flush.lua"))
      print("M6 JIT flush handshake guard passed")
    end
  })

  add({
    name = "m6_jit",
    description = "M6 JIT aggregate scaffold gates",
    run = function(t)
      local cmd = { t:path("tools", "ci", "lua_test.sh") }
      for i = 1, #m6_cases do cmd[#cmd + 1] = m6_cases[i] end
      t:run(cmd)
      print("M6 JIT scaffold tests passed")
    end
  })
end
