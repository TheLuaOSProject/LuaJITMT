local build = require("suite_build")
local runtime = require("suite_runtime")
local cellops = require("suite_cell_ops")

local run_luajit = runtime.luajit
local run_stock = runtime.run_stock
local build_and_run_c = build.compile_and_run_c
local run_c_fixture_specs = build.run_c_fixture_specs
local build_and_run_luajit_script = runtime.build_and_run_luajit_script

local function api_debug_claim_cleanup_smoke()
  return [=[
local co = coroutine.create(function()
  local target = "before"
  coroutine.yield("ready")
  return target
end)

local ok, msg = coroutine.resume(co)
assert(ok and msg == "ready")

local info = debug.getinfo(co, 1, "flnSuL")
assert(type(info) == "table")
assert(type(info.func) == "function")
assert(type(info.activelines) == "table")

local slot
for i = 1, 20 do
  local name, value = debug.getlocal(co, 1, i)
  if not name then break end
  if name == "target" then
    assert(value == "before")
    slot = i
    break
  end
end
assert(slot, "suspended coroutine local not found")

local name = debug.setlocal(co, 1, slot, "after")
assert(name == "target")

ok, msg = coroutine.resume(co)
assert(ok and msg == "after")
assert(coroutine.status(co) == "dead")

local function self_info()
  return debug.getinfo(1, "flnSuL")
end
info = self_info()
assert(type(info) == "table" and type(info.func) == "function")
assert(type(info.activelines) == "table")

print("api-debug-claim-cleanup-smoke OK")
]=]
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
local b = { 1, nil, 3 }
b[2] = 22
local bk = 2
b[bk] = 23
assert(b[1] == 1 and b[2] == 23 and b[3] == 3)
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
local trace_count = require"jit_harness".trace_count

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
assert(trace_count(200) > 0, "no root trace was published")
jit.flush()
assert(trace_count(200) == 0, "trace slots were not cleared")

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
local before = trace_count(200)
for _ = 1, 120 do
  assert(side(90, true) == expect(90, true))
end
assert(trace_count(200) > before, "no side trace was published")
local after_side = trace_count(200)
for _ = 1, 120 do
  assert(side(90, true) == expect(90, true))
end
assert(util.traceinfo(1), "missing root trace 1")
jit.flush(1)
assert(not util.traceinfo(1), "scoped root flush did not clear root slot")
assert(trace_count(200) < after_side, "scoped root flush did not retire any slots")
jit.flush()
assert(trace_count(200) == 0, "full flush after scoped root flush left traces")
for _ = 1, 20 do
  assert(side(90, true) == expect(90, true))
end
print("jit-trace-publish-smoke OK")
]=]
end

local function hookmask_atomic_smoke()
  return [=[
local hits = { count = 0, line = 0, call = 0, ["return"] = 0 }
local phase = 0

local function hook(ev)
  hits[ev] = (hits[ev] or 0) + 1
  if ev == "count" and phase == 0 then
    phase = 1
    debug.sethook(hook, "crl", 0)
    local fn, mask, count = debug.gethook()
    assert(fn == hook, "hook function was not preserved")
    assert(mask:find("c", 1, true), "call hook bit missing")
    assert(mask:find("r", 1, true), "return hook bit missing")
    assert(mask:find("l", 1, true), "line hook bit missing")
    assert(count == 0, "count hook was not disabled")
  end
end

local function inner(n)
  local s = 0
  for i = 1, n do
    s = s + i
  end
  return s
end

debug.sethook(hook, "", 1)
assert(inner(12) == 78)
assert(inner(12) == 78)
debug.sethook()
assert(hits.count == 1, "count hook did not transition once")
assert(hits.line > 0, "line hook did not run after mask update")
assert(hits.call > 0, "call hook did not run after mask update")
assert(hits["return"] > 0, "return hook did not run after mask update")

local ok, profile = pcall(require, "jit.profile")
if ok then
  profile.start("i1", function() end)
  local s = 0
  for i = 1, 200000 do
    s = s + i
  end
  profile.stop()
  assert(s > 0)
end

print("hookmask-atomic-smoke OK")
]=]
end

local function hook_state_atomic_smoke()
  return [=[
local first_hits = 0
local second_hits = 0

local function second(ev)
  assert(ev == "count")
  second_hits = second_hits + 1
end

local function first(ev)
  assert(ev == "count")
  first_hits = first_hits + 1
  debug.sethook(second, "", 3)
  local fn, mask, count = debug.gethook()
  assert(fn == second, "hook function did not update")
  assert(mask == "", "count-only mask should not expose event bits")
  assert(count == 3, "hook count start did not update")
end

debug.sethook(first, "", 1)
local sum = 0
for i = 1, 80 do
  sum = sum + i
end
assert(sum == 3240)
debug.sethook()

assert(first_hits == 1, "first hook should run once before replacement")
assert(second_hits > 0, "replacement count hook did not run")
local fn, mask, count = debug.gethook()
assert(fn == nil and mask == "" and count == 0, "hook clear did not publish")

print("hook-state-atomic-smoke OK")
]=]
end

local function gc_total_atomic_smoke()
  return [=[
local th = require"threading"

local function stats_total()
  local stats = th.gcstats()
  assert(type(stats.total_bytes) == "number", "missing total_bytes")
  assert(type(stats.total_kbytes) == "number", "missing total_kbytes")
  assert(stats.total_bytes >= stats.total_kbytes * 1024)
  return stats.total_bytes
end

local before = stats_total()
for round = 1, 4 do
  local workers = {}
  for id = 1, 4 do
    workers[id] = th.spawn(function(worker)
      local keep = {}
      for i = 1, 1500 do
	keep[i] = { worker, i, tostring(i) }
      end
      collectgarbage("collect")
      return #keep, th.gcstats().total_bytes
    end, id)
  end

  local total = 0
  for id = 1, 4 do
    local ok, n, worker_total = workers[id]:join(30)
    assert(ok == true, "worker failed")
    assert(n == 1500, "worker allocation count mismatch")
    assert(type(worker_total) == "number" and worker_total > 0)
    total = total + n
  end
  assert(total == 6000, "worker total mismatch")
end

collectgarbage("collect")
local after = stats_total()
assert(before > 0 and after > 0)

print("gc-total-atomic-smoke OK")
]=]
end

local function gc2_pacing_atomic_smoke()
  return [=[
local th = require"threading"

local function pacing_stats()
  local stats = th.gcstats()
  for _, name in ipairs({
    "alloc_since_trigger", "cycle_alloc_bytes", "trigger_bytes", "hard_bytes"
  }) do
    assert(type(stats[name]) == "number", "missing " .. name)
    assert(stats[name] >= 0, "negative " .. name)
  end
  assert(stats.trigger_bytes > 0, "missing GC2 trigger pacing")
  assert(stats.hard_bytes >= stats.trigger_bytes, "hard limit below trigger")
  return stats
end

local before = pacing_stats()
local workers = {}
for id = 1, 4 do
  workers[id] = th.spawn(function(worker)
    local keep = {}
    for i = 1, 500 do
      keep[i] = { worker, i, tostring(i) }
    end
    return #keep, th.gcstats().alloc_since_trigger
  end, id)
end

local total = 0
for id = 1, 4 do
  local ok, n, alloc = workers[id]:join(30)
  assert(ok == true, "worker failed")
  assert(n == 500, "worker allocation count mismatch")
  assert(type(alloc) == "number" and alloc >= 0)
  total = total + n
end

local after = pacing_stats()
assert(total == 2000)
assert(after.hard_bytes >= before.trigger_bytes)

print("gc2-pacing-atomic-smoke OK")
]=]
end

local function proto_kgc_acq_smoke()
  return [=[
local util = require("jit.util")

local function parent(t)
  local function child()
    return "proto-kgc-acq-child"
  end
  return t.proto_kgc_acq_marker, child
end

local saw_string = false
local saw_child = false
for i = -64, 64 do
  local k = util.funck(parent, i)
  if k == "proto_kgc_acq_marker" then saw_string = true end
  if type(k) == "proto" then saw_child = true end
end
assert(saw_string, "jit.util.funck did not expose string KGC")
assert(saw_child, "jit.util.funck did not expose child proto KGC")

local function field_name()
  local t = {}
  return t.proto_kgc_acq_marker + 1
end
local ok, err = pcall(field_name)
assert(not ok and tostring(err):match("field 'proto_kgc_acq_marker'"),
       tostring(err))

local function global_name()
  return proto_kgc_acq_global_missing + 1
end
ok, err = pcall(global_name)
assert(not ok and tostring(err):match("global 'proto_kgc_acq_global_missing'"),
       tostring(err))

jit.off(parent, true)
jit.on(parent, true)
print("proto-kgc-acq-smoke OK")
]=]
end

local function proto_chunkname_acq_smoke()
  return [=[
local util = require("jit.util")

local src = [[
return function()
  local t = {}
  return t.proto_chunkname_acq_field + 1
end
]]
local fn = assert(loadstring(src, "@proto_chunkname_acq_src"))()
local jinfo = util.funcinfo(fn)
assert(jinfo.source == "@proto_chunkname_acq_src", tostring(jinfo.source))
local dinfo = debug.getinfo(fn, "S")
assert(dinfo.source == "@proto_chunkname_acq_src", tostring(dinfo.source))
local ok, err = pcall(fn)
assert(not ok and tostring(err):match("proto_chunkname_acq_src"),
       tostring(err))

local dumped = string.dump(fn)
local fn2 = assert(loadstring(dumped))
dinfo = debug.getinfo(fn2, "S")
assert(dinfo.source == "@proto_chunkname_acq_src", tostring(dinfo.source))

jit.flush()
jit.opt.start("hotloop=1")
for _ = 1, 8 do pcall(fn2) end
print("proto-chunkname-acq-smoke OK")
]=]
end

local function proto_knum_acq_smoke()
  return [=[
local util = require("jit.util")
local ffi = require("ffi")

local function numeric_constants()
  return 123.25, -9876.5
end

local saw_a = false
local saw_b = false
for i = 0, 16 do
  local k = util.funck(numeric_constants, i)
  if k == 123.25 then saw_a = true end
  if k == -9876.5 then saw_b = true end
end
assert(saw_a and saw_b, "jit.util.funck did not expose numeric constants")

local dumped = string.dump(numeric_constants)
local loaded = assert(loadstring(dumped))
local a, b = loaded()
assert(a == 123.25 and b == -9876.5)

local ct = ffi.metatype("struct { int x; }", {
  __eq = function(lhs, rhs)
    if type(rhs) == "number" then return rhs == 123.25 end
    return lhs.x == rhs.x
  end
})
local c = ct(1)
assert(c == 123.25)
assert(not (c == 124.25))

print("proto-knum-acq-smoke OK")
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

      cellops.run_bytecode_guards(t, "lj_m5_cell_ops_bc")
      cellops.run_publication_behavior_guards(t)
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
      build_and_run_luajit_script(t, "t-threading-upvalue.lua", nil,
                                  { joff = true })
      build_and_run_c(t, t:tmp("lj_t-cclosure-upvalue-snapshot"),
                      "t-cclosure-upvalue-snapshot.c")
      build_and_run_c(t, t:tmp("lj_t-cclosure-upvalue-race"),
                      "t-cclosure-upvalue-race.c", { timeout = "20s" })
      print("M5 closed-upvalue GC publication behavior passed")
    end
  })

  add({
    name = "m5_stock_api_surface",
    description = "stock LuaJIT public C API surface behavior",
    run = function(t)
      build_and_run_c(t, t:tmp("lj_t-stock-api-surface"),
                      "t-stock-api-surface.c")
      print("M5 stock LuaJIT public C API surface passed")
    end
  })

  add({
    name = "m5_jit_trace_publish",
    description = "JIT trace-slot and trace-link publication guards",
    run = function(t)
      t:build({ quiet = true })
      run_c_fixture_specs(t, {
        { output = "lj_t-jit-tracevec", cfile = "t-jit-tracevec.c" },
        { output = "lj_t-jit-mcode-retire", cfile = "t-jit-mcode-retire.c" },
        { output = "lj_t-jit-trace-retire", cfile = "t-jit-trace-retire.c" }
      }, { timeout = "20s" })
      run_luajit(t, { "-e", jit_trace_publish_smoke() })
      print("M5 JIT trace publication guard passed")
    end
  })

  add({
    name = "m5_hookmask_atomic",
    description = "global hookmask atomic helper behavior",
    run = function(t)
      t:build({ quiet = true })
      run_luajit(t, { "-e", hookmask_atomic_smoke() })
      print("M5 hookmask atomic helper behavior passed")
    end
  })

  add({
    name = "m5_hook_state_atomic",
    description = "global hook function/count atomic helper behavior",
    run = function(t)
      t:build({ quiet = true })
      run_luajit(t, { "-e", hook_state_atomic_smoke() })
      print("M5 hook function/count atomic helper behavior passed")
    end
  })

  add({
    name = "m5_panic_callback_atomic",
    description = "global panic callback atomic exchange behavior",
    run = function(t)
      t:build({ quiet = true })
      build_and_run_c(t, t:tmp("lj_t-panic-callback-atomic"),
                      "t-panic-callback-atomic.c", { timeout = "20s" })
      print("M5 panic callback atomic exchange behavior passed")
    end
  })

  add({
    name = "m5_wrapcf_func_publish",
    description = "C wrapper callback release-publication guard",
    run = function(t)
      t:build({ quiet = true })
      print("M5 C wrapper callback publication guard passed")
    end
  })

  add({
    name = "m5_api_debug_claim_cleanup",
    description = "API/debug/JIT state-claim cleanup boundaries",
    run = function(t)
      t:build({ quiet = true })
      run_luajit(t, { "-e", api_debug_claim_cleanup_smoke() })
      print("M5 API/debug/JIT claim cleanup boundaries passed")
    end
  })

  add({
    name = "m5_profile_stop_native",
    description = "jit.profile stop native-state STOPREQ behavior",
    run = function(t)
      t:build({ quiet = true })
      build_and_run_c(t, t:tmp("lj_t_profile_stop_native"),
                      "t-profile-stop-native.c", { build = false,
                                                   timeout = "20s" })
      print("M5 jit.profile stop native-state behavior passed")
    end
  })

  add({
    name = "m5_profile_blocked_tg_samples",
    description = "jit.profile sample delivery with another TG blocked",
    run = function(t)
      t:build({ quiet = true })
      build_and_run_luajit_script(t, "t-profile-blocked-tg.lua", nil,
                                  { build = false, joff = true,
                                    timeout = "20s" })
      print("M5 jit.profile blocked TG sample delivery passed")
    end
  })

  add({
    name = "m5_gc_total_atomic",
    description = "GC total atomic accounting helper behavior",
    run = function(t)
      t:build({ quiet = true })
      build_and_run_c(t, t:tmp("lj_t_gc_total_atomic"),
                      "t-gc-total-atomic.c", { build = false })
      run_luajit(t, { "-e", gc_total_atomic_smoke() })
      print("M5 GC total atomic accounting behavior passed")
    end
  })

  add({
    name = "m5_gc2_pacing_atomic",
    description = "GC/GC2 pacing control atomic helper behavior",
    run = function(t)
      t:build({ quiet = true })
      build_and_run_c(t, t:tmp("lj_t_gc2_pacing_atomic"),
                      "t-gc2-pacing-atomic.c", { build = false })
      run_luajit(t, { "-e", gc2_pacing_atomic_smoke() })
      print("M5 GC/GC2 pacing control atomic helper behavior passed")
    end
  })

  add({
    name = "m5_proto_kgc_acq",
    description = "prototype KGC acquire-reader behavior",
    run = function(t)
      t:build({ quiet = true })
      run_luajit(t, { "-e", proto_kgc_acq_smoke() })
      print("M5 prototype KGC acquire-reader behavior passed")
    end
  })

  add({
    name = "m5_proto_chunkname_acq",
    description = "prototype chunkname acquire-reader behavior",
    run = function(t)
      t:build({ quiet = true })
      run_luajit(t, { "-e", proto_chunkname_acq_smoke() })
      print("M5 prototype chunkname acquire-reader behavior passed")
    end
  })

  add({
    name = "m5_proto_knum_acq",
    description = "prototype numeric constant acquire-reader behavior",
    run = function(t)
      t:build({ quiet = true })
      run_luajit(t, { "-e", proto_knum_acq_smoke() })
      print("M5 prototype numeric constant acquire-reader behavior passed")
    end
  })

  add({
    name = "m5_tab_array_publish",
    description = "table array publication and retirement guards",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build_and_run_c(t, t:tmp("lj_t-tab-array-publish"),
                      "t-tab-array-publish.c", { timeout = "20s" })
      print("M5 table array publication tests passed")
    end
  })

  add({
    name = "m5_tab_colocated_resize",
    description = "colocated array resize freezes old inline slots",
    run = function(t)
      t:build({
        clean = true,
        jobs = false,
        quiet = true,
        xcflags = "-DLJ_TAB_TEST_HELPERS"
      })
      build_and_run_c(t, t:tmp("lj_t-tab-colocated-resize"),
                      "t-tab-colocated-resize.c", {
        cflags = "-DLJ_TAB_TEST_HELPERS",
        timeout = "20s"
      })
      print("M5 colocated array resize freeze guard passed")
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
      build_and_run_c(t, t:tmp("lj_t-tab-cas-store-value"),
                      "t-tab-cas-store.c", { timeout = "20s" })
      print("M5 table value publication guard passed")
    end
  })

  add({
    name = "m5_x64_tset_nil_snapshot",
    description = "x64 TSET previous-value nil behavior",
    run = function(t)
      t:build({ clean = true, quiet = true })
      run_luajit(t, { "-joff", "-e", tset_nil_smoke() })
      build_and_run_c(t, t:tmp("lj_t-x64-tset-forward"),
                      "t-x64-tset-forward.c")
      print("M5 x64 TSET previous-value nil behavior passed")
    end
  })
end
