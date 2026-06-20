local utils = require("suite_utils")
local runtime = require("suite_runtime")

local contains = utils.contains
local run_luajit = runtime.luajit
local luajit_capture = runtime.capture_luajit
local run_stock = runtime.run_stock
local build_and_run_c = runtime.compile_and_run_c
local build_and_run_luajit_script = runtime.build_and_run_luajit_script

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
      build_and_run_luajit_script(t, "t-threading-upvalue.lua", nil,
                                  { joff = true })
      print("M5 closed-upvalue GC publication behavior passed")
    end
  })

  add({
    name = "m5_jit_trace_publish",
    description = "JIT trace-slot and trace-link publication guards",
    run = function(t)
      t:build({ quiet = true })
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
