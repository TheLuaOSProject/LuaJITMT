local checks = require("suite_assert")
local build = require("suite_build")
local runtime = require("suite_runtime")
local jitutils = require("suite_jit")
local cellops = require("suite_cell_ops")

local contains = checks.contains
local lines = checks.iter_lines
local assert_dump_contains = checks.assert_dump_contains
local assert_dump_contains_count = checks.assert_dump_contains_count
local assert_dump_match_count = checks.assert_dump_match_count
local assert_trace1_ir = jitutils.assert_trace1_ir
local x64_cmp_poll_pattern = jitutils.x64_cmp_poll_pattern
local assert_x64_loop_poll_count = jitutils.assert_x64_loop_poll_count
local assert_loop_ir_markers = jitutils.assert_loop_ir_markers
local assert_loop_after_xpoll = jitutils.assert_loop_after_xpoll
local assert_call_after_loop_polls = jitutils.assert_call_after_loop_polls
local run_ir_dump_probe = jitutils.run_ir_dump_probe
local assert_ir_dump_probe_contains = jitutils.assert_ir_dump_probe_contains
local assert_ir_dump_probe_all_contains = jitutils.assert_ir_dump_probe_all_contains
local luajit_code = runtime.luajit_code
local luajit_file = runtime.luajit_file
local luajit_dump = runtime.luajit_dump
local build_default = build.build_default
local clean_build = build.clean_build
local build_and_run_c = build.build_and_run_c
local run_lua_test_case = runtime.run_lua_test_case

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
  "m6_jit_flush_hs",
  "m6_jit_tmpbuf_thread_format",
  "m6_jit_perftools_native"
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
  wk.stable = i + 0.5
end
assert(wk.stable == 200.5)
assert(util.traceinfo(1), "weak-key numeric table store did not trace")

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

local function jit_tmpbuf_thread_format_smoke()
  return [=[
local th = require"threading"

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")

local workers = {}
for id = 1, 4 do
  workers[id] = th.spawn(function(worker)
    jit.opt.start("hotloop=1", "hotexit=1")
    local keep = {}
    for i = 1, 2000 do
      local s = ("gc2-pacing-%d-%d"):format(worker, i)
      local expect = "gc2-pacing-" .. worker .. "-" .. i
      assert(s == expect, "format result changed across tmpbuf reuse")
      keep[i] = s
    end
    return #keep, keep[1], keep[#keep]
  end, id)
end

for id = 1, 4 do
  local ok, n, first, last = workers[id]:join(30)
  assert(ok == true, "worker failed")
  assert(n == 2000, "worker format count mismatch")
  assert(first == ("gc2-pacing-%d-1"):format(id), "first format mismatch")
  assert(last == ("gc2-pacing-%d-2000"):format(id), "last format mismatch")
end

print("jit-tmpbuf-thread-format-smoke OK")
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
      luajit_file(t, t:path("tests", "t-jit-secondary.lua"),
                  { lua_path = true, timeout = "20s" })

      local dump = t:tmp("lj_t-jit-xpoll.dump")
      luajit_dump(t, dump, "-jdump=im", [=[
jit.opt.start("hotloop=1","hotexit=1")
local s=0.0
for i=1,64 do s=s+i end
assert(s==2080.0)
]=], { timeout = "20s" })
      assert_dump_contains(t, dump, "XPOLL", "x64 loop trace")
      assert_x64_loop_poll_count(t, dump,
        "x64 IR_XPOLL must lower to a TG poll at the loop label", 1)
      local root_mcode = t:read(dump):match("TRACE 1 mcode.-TRACE 1 stop")
      if root_mcode and contains(root_mcode, "push rcx") then
        error("trace-head vmstate publish must not save rcx in simple traces", 2)
      end
      if root_mcode and contains(root_mcode, "mov rcx,") then
        error("trace-head vmstate publish must not mirror to global vmstate", 2)
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
      assert_dump_contains_count(t, funcf_dump, "XPOLL", 4,
                                 "deep inlined FUNCF traces")
      assert_dump_match_count(t, funcf_dump, x64_cmp_poll_pattern(), 4,
                              "FUNCF-depth IR_XPOLL lowering")
      print("M6 JIT recorder token behavior passed")
    end
  })

  add({
    name = "m6_jit_cell_ops",
    description = "M6 local-cell JIT recording behavior",
    run = function(t)
      build_default(t)
      local dump = t:tmp("lj_m6_jit_cell_ops.dump")
      cellops.run_jit_dump_guards(t, dump)
      cellops.run_jit_runtime_guards(t)
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
      local copy_dump = t:tmp("lj-m6-xbar-copy-ir.dump")
      run_ir_dump_probe(t, copy_dump, [=[
local ffi = require("ffi")
local dst = ffi.new("uint8_t[512]")
local src = ffi.new("uint8_t[512]")
jit.opt.start("hotloop=1", "hotexit=1")
for i = 1, 80 do ffi.copy(dst, src, 512) end
]=])
      assert_loop_after_xpoll(t, copy_dump, "FFI copy XBAR loop",
                              { "CALLS  memcpy", "XBAR" })

      local load_dump = t:tmp("lj-m6-xbar-xload-ir.dump")
      run_ir_dump_probe(t, load_dump, [=[
local ffi = require("ffi")
local a = ffi.new("int[256]")
jit.opt.start("hotloop=1", "hotexit=1")
local s = 0
for i = 1, 120 do s = s + a[i % 128] end
assert(s == 0)
]=])
      assert_loop_after_xpoll(t, load_dump, "FFI XLOAD loop", { "XLOAD" })

      local store_dump = t:tmp("lj-m6-xbar-xstore-ir.dump")
      run_ir_dump_probe(t, store_dump, [=[
local ffi = require("ffi")
local a = ffi.new("int[256]")
jit.opt.start("hotloop=1", "hotexit=1")
for i = 1, 120 do a[i % 128] = i end
assert(a[119 % 128] == 119)
]=])
      assert_loop_after_xpoll(t, store_dump, "FFI XSTORE loop", { "XSTORE" })
      run_lua_test_case(t, "m5_jit_hash_store_nyi")
      print("M6 JIT XBAR/XPOLL alias guard passed")
    end
  })

  add({
    name = "m6_jit_table_store_helper",
    description = "M6 helper-backed table store behavior",
    run = function(t)
      t:build({ clean = true, quiet = true, xcflags = "-DLJ_TAB_TEST_HELPERS" })
      build_and_run_c(t, t:tmp("lj_t-jit-forward-store"),
                      "t-jit-forward-store.c", {
                        build = false,
                        cflags = "-DLJ_TAB_TEST_HELPERS",
                        timeout = "20s"
                      })
      luajit_code(t, table_store_smoke())

      assert_ir_dump_probe_all_contains(t, "lj-m6-hstore-ir.dump", [=[
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
]=], { "TDUP", "HSTORE", "XPOLL" }, "trace-local hash store")

      assert_ir_dump_probe_all_contains(t, "lj-m6-new-hstore-ir.dump", [=[
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
]=], { "TNEW", "NEWREF", "HSTORE", "XPOLL" }, "trace-local new hash store")

      assert_ir_dump_probe_all_contains(t, "lj-m6-oldnil-hstore-ir.dump", [=[
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
]=], { "HSTORE", "TBAR", "XPOLL" }, "previous-nil hash store")

      assert_ir_dump_probe_all_contains(t, "lj-m6-astore-ir.dump", [=[
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
]=], { "TDUP", "ASTORE", "XPOLL" }, "trace-local array store")

      assert_ir_dump_probe_all_contains(t, "lj-m6-new-array-hstore-ir.dump", [=[
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
]=], { "TNEW", "NEWREF", "HSTORE", "XPOLL" },
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

      assert_ir_dump_probe_all_contains(t, "lj-m6-oldnil-astore-ir.dump", [=[
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
]=], { "ASTORE", "XPOLL" }, "previous-nil array store")

      assert_ir_dump_probe_all_contains(t, "lj-m6-shared-hstore-ir.dump", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local h = { stable = 0 }
for i = 1, 80 do
  h.stable = i
end
assert(h.stable == 80)
assert(util.traceinfo(1), "shared existing hash store did not trace")
]=], { "HSTORE", "XPOLL" }, "shared existing hash store")

      assert_ir_dump_probe_all_contains(t, "lj-m6-shared-astore-ir.dump", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local a = { 0 }
for i = 1, 80 do
  a[1] = i
end
assert(a[1] == 80)
assert(util.traceinfo(1), "shared existing array store did not trace")
]=], { "ASTORE", "XPOLL" }, "shared existing array store")

      local array_forward_dump = t:tmp("lj-m6-astore-forward-read.dump")
      luajit_dump(t, array_forward_dump, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.off()
local a = {}
for i = 1, 256 do a[i] = 0 end
jit.on()
local s = 0
for i = 1, 80 do
  local j = (i % 256) + 1
  a[j] = i + 0.5
  s = s + a[j]
end
assert(s == 3280 and a[81] == 80.5)
]=], { timeout = "20s" })
      assert_trace1_ir(t, array_forward_dump,
                       "same-slot array store/read must forward ASTORE",
                       function(st)
        return st.astore and st.aref and not st.aload and st.xpoll and
               st.array == 2 and st.xload == 2
      end)

      local hash_forward_dump = t:tmp("lj-m6-hstore-forward-read.dump")
      luajit_dump(t, hash_forward_dump, "-jdump=ir", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
jit.off()
local h = { stable = 0 }
jit.on()
local s = 0
for i = 1, 80 do
  h.stable = i + 0.5
  s = s + h.stable
end
assert(s == 3280 and h.stable == 80.5)
]=], { timeout = "20s" })
      assert_trace1_ir(t, hash_forward_dump,
                       "same-slot hash store/read must forward HSTORE",
                       function(st)
        return st.hstore and st.hrefk and not st.hload and st.xpoll
      end)

      local route_dump = t:tmp("lj-m6-table-store-helper-routes.dump")
      luajit_dump(t, route_dump, "-jdump=im", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local a = { 0 }
for i = 1, 80 do
  a[1] = i + 0.5
end
assert(a[1] == 80.5)
assert(util.traceinfo(1), "numeric array store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local b = {}
for i = 1, 256 do b[i] = 0 end
for i = 1, 80 do
  local j = (i % 256) + 1
  b[j] = i + 0.5
end
assert(b[81] == 80.5)
assert(util.traceinfo(1), "separated numeric array store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = 0 }
for i = 1, 80 do
  h.stable = i + 0.5
end
assert(h.stable == 80.5)
assert(util.traceinfo(1), "numeric hash store did not trace")
]=])
      assert_dump_contains(t, route_dump,
                           "lj_tab_storetv_forjit_array_nogc",
                           "numeric ASTORE no-GC helper")
      assert_dump_contains(t, route_dump,
                           "lock cmpxchg",
                           "numeric table-store inline CAS fallback gate")
      assert_dump_contains(t, route_dump,
                           "lj_tab_storetv_forjit_hash",
                           "numeric HSTORE helper fallback")

      local hash_route_dump = t:tmp("lj-m6-hstore-inline-cas.dump")
      luajit_dump(t, hash_route_dump, "-jdump=im", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local h = { stable = 0 }
for i = 1, 80 do
  h.stable = i + 0.5
end
assert(h.stable == 80.5)
assert(util.traceinfo(1), "numeric hash store did not trace")
]=])
      assert_dump_contains(t, hash_route_dump,
                           "lock cmpxchg",
                           "numeric HSTORE inline CAS fallback gate")
      assert_dump_contains(t, hash_route_dump,
                           "lj_tab_storetv_forjit_hash",
                           "numeric HSTORE helper fallback")
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
    name = "m6_jit_tmpbuf_thread_format",
    description = "M6 JIT uses the running TG tmpbuf for threaded string.format traces",
    run = function(t)
      build_default(t)
      local dump = t:tmp("lj-m6-tmpbuf-format-ir.dump")
      luajit_dump(t, dump, "-jdump=im", [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local s = 0
for i = 1, 80 do
  s = s + #string.format("%d:%s", i, "x")
end
assert(s > 0)
]=])
      do
        local data = t:read(dump)
        if not (contains(data, "LREF") and data:match("BUFHDR%s+%d+%s+RESET")) then
          error("tmpbuf format trace did not use runtime LREF BUFHDR reset", 2)
        end
        if contains(data, "lj_buf_tmp_reset") then
          error("tmpbuf format trace still calls lj_buf_tmp_reset", 2)
        end
        if not contains(data, "->lj_strfmt_putint_tg") then
          error("tmpbuf format trace did not use TG integer-format helper", 2)
        end
        if not contains(data, "->lj_buf_putstr_tg") then
          error("tmpbuf format trace did not use TG string-append helper", 2)
        end
        if not data:match("mov byte %[[^%]]+%], 0x3a") or
           not data:match("mov byte %[[^%]]+%+0x1%], 0x78") then
          error("tmpbuf format trace did not inline literal byte appends", 2)
        end
        if not contains(data, "->lj_buf_tostr_tg") then
          error("tmpbuf format trace did not use TG buffer-finalize helper", 2)
        end
        if data:match("%->lj_strfmt_putint[^_%w]") then
          error("tmpbuf format trace still calls generic integer-format helper", 2)
        end
        if data:match("%->lj_buf_putstr[^_%w]") then
          error("tmpbuf format trace still calls generic string-append helper", 2)
        end
        if data:match("%->lj_buf_tostr[^_%w]") then
          error("tmpbuf format trace still calls generic buffer-finalize helper", 2)
        end
      end
      luajit_code(t, jit_tmpbuf_thread_format_smoke(), { timeout = "30s" })
      print("M6 JIT threaded string.format tmpbuf smoke passed")
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

      assert_ir_dump_probe_all_contains(t, "lj_t-jit-gc2-readiness-tnew.dump", [=[
jit.opt.start("hotloop=1","hotexit=1")
local x
for i=1,100 do x={} end
assert(type(x)=="table")
]=], { "TNEW", "XPOLL", "GCSTEP" }, "TNEW readiness")

      assert_ir_dump_probe_all_contains(t, "lj_t-jit-gc2-readiness-cnew.dump", [=[
local ffi=require("ffi")
ffi.cdef("typedef struct { int x; } lj_gc2_dump_cnew_t;")
local ct=ffi.typeof("lj_gc2_dump_cnew_t")
jit.opt.start("hotloop=1","hotexit=1","-sink")
local x
for i=1,100 do x=ct(i) end
assert(x.x==100)
]=], { "CNEW", "XPOLL" }, "CNEW readiness")

      assert_ir_dump_probe_all_contains(t, "lj_t-jit-gc2-readiness-snew.dump", [=[
jit.opt.start("hotloop=1","hotexit=1","-sink")
local s="abcdef"
local x
for i=1,100 do x=string.sub(s,1,3) end
assert(x=="abc")
]=], { "SNEW", "XPOLL" }, "SNEW readiness")
      print("M6 JIT GC2 readiness behavior passed")
    end
  })

  add({
    name = "m6_jit_gcstep_guard",
    description = "legacy JIT GC-step pacing behavior",
    run = function(t)
      build_default(t)
      assert_ir_dump_probe_contains(t, "lj_t-jit-gcstep.dump", [=[
jit.opt.start("hotloop=1","hotexit=1")
local x
for i=1,100 do x={} end
assert(type(x)=="table")
]=], "GCSTEP", "sunk allocation replay")
      luajit_file(t, t:path("tests", "stock", "test", "misc", "gcstep.lua"),
                  { timeout = "20s" })
      luajit_code(t, [=[
local clock = os.clock
local function run(n)
  local s = 0
  for i = 1, n do
    local x = i
    local f = function()
      x = x + 1
      return x
    end
    s = s + f()
  end
  return s
end
local best = math.huge
for _ = 1, 5 do
  collectgarbage("collect")
  local t0 = clock()
  assert(run(5000) == 12507500)
  local dt = clock() - t0
  if dt < best then best = dt end
end
assert(best >= 0)
]=], { timeout = "10s" })
      print("M6 JIT GC-step behavior passed")
    end
  })

  add({
    name = "m6_jit_mcode_publish",
    description = "Linux/x64 mcode sync-core publication ordering",
    run = function(t)
      clean_build(t)
      build_and_run_c(t, t:tmp("lj_t-jit-mcode-prot"),
                      "t-jit-mcode-prot.c",
                      { build = false, timeout = "20s" })
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
                  { lua_path = true, timeout = timeout })
      print("M6 JIT mcode publication guard passed")
    end
  })

  add({
    name = "m6_jit_flush_hs",
    description = "JIT flush safepoint-scoped publication and retirement",
    run = function(t)
      build_default(t)
      run_lua_test_case(t, "m5_jit_trace_publish")
      run_lua_test_case(t, "m3_vm_safepoint")
      luajit_file(t, t:path("tests", "stock", "test", "misc", "jit_flush.lua"))
      print("M6 JIT flush handshake guard passed")
    end
  })

  add({
    name = "m6_jit_perftools_native",
    description = "Linux perf-map writer native-state STOPREQ behavior",
    run = function(t)
      local ok, err = pcall(function()
        t:build({
          clean = true,
          quiet = true,
          xcflags = "-DLUAJIT_USE_PERFTOOLS"
        })
        build_and_run_c(t, t:tmp("lj_t-jit-perftools-native"),
                        "t-jit-perftools-native.c", {
          build = false,
          timeout = "30s"
        })
      end)
      t:build({ clean = true, quiet = true })
      if not ok then error(err, 0) end
      print("M6 JIT perf-map native-state STOPREQ behavior passed")
    end
  })

  add({
    name = "m6_jit",
    description = "M6 JIT aggregate scaffold gates",
    deps = m6_cases,
    run = function(t)
      runtime.run_lua_test_cases(t, m6_cases)
      print("M6 JIT scaffold tests passed")
    end
  })
end
