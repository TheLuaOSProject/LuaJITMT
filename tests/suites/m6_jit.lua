local build = require("suite_build")
local runtime = require("suite_runtime")
local cellops = require("suite_cell_ops")
local utils = require("suite_utils")

local luajit_code = runtime.luajit_code
local luajit_file = runtime.luajit_file
local build_default = build.build_default
local clean_build = build.clean_build
local build_and_run_c = build.build_and_run_c
local run_lua_test_case = runtime.run_lua_test_case
local gc2_test_cflags = build.gc2_test_helper_flag
local shell_quote = utils.shell_quote

local m6_cases = {
  "m6_dispatch_redispatch",
  "m6_jit_token",
  "m6_jit_hotcall_missing_args",
  "m6_jit_recursive_call_unroll",
  "m6_jit_recursive_retention",
  "m6_jit_trace_proto_gc",
  "m6_jit_cell_ops",
  "m6_jit_fnew_bump",
  "m6_jit_barrier_xpoll",
  "m6_jit_xbar_xpoll",
  "m6_jit_xsave",
  "m6_jit_table_store_helper",
  "m6_jit_entering_table_store",
  "m6_jit_tbar_gc2_black_gate",
  "m6_jit_aref_pair_boundary",
  "m6_jit_hrefk_nodehdr",
  "m6_jit_href_nodehdr",
  "m6_jit_alloc_account",
  "m6_jit_gc2_readiness",
  "m6_jit_gcstep_pacing",
  "m6_jit_mcode_native",
  "m6_jit_mcode_publish",
  "m6_jit_flush_hs",
  "m6_jit_flush_gc_current_stack",
  "m6_jit_util_flush_race",
  "m6_jit_flush_thread_stress",
  "m6_jit_flush_join_token_liveness",
  "m6_jit_park_vmevent_reentrant",
  "m6_jit_flush_thread_heavy_stress",
  "m6_jit_mt_activation_flush",
  "m6_jit_gcworkers_activation_flush",
  "m6_jit_vmevent_flush",
  "m6_jit_traceerr_format",
  "m6_jit_gdbjit_publish",
  "m6_jit_tmpbuf_thread_format",
  "m6_jit_perftools_native",
  "m6_jit_io_native_stopreq",
  "m6_jit_cclosure_upvalue_flush",
  "m6_jit_env_mutation_flush",
  "m6_jit_threading_nyi_boundary",
  "m6_jit_buffer_method_shared_nyi"
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
local hits = 0
local mh = setmetatable({ stable = 0 }, {
  __newindex = function()
    hits = hits + 1
  end
})
for i = 1, 200 do
  mh.stable = i
end
assert(mh.stable == 200 and hits == 0)
assert(util.traceinfo(1),
       "metatable existing hash table store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local array_hits = 0
local ma = setmetatable({ 0 }, {
  __newindex = function()
    array_hits = array_hits + 1
  end
})
for i = 1, 200 do
  ma[1] = i
end
assert(ma[1] == 200 and array_hits == 0)
assert(util.traceinfo(1),
       "metatable existing array table store did not trace")

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

local function table_read_pubroot_smoke()
  return [=[
local th = require("threading")
local trace_count = require("jit_harness").trace_count

local nmarkers = tonumber(os.getenv("LJ_M6_JIT_TABLE_READ_PUBROOT_N")) or 192
local rounds = tonumber(os.getenv("LJ_M6_JIT_TABLE_READ_PUBROOT_ROUNDS")) or 8
local passes = tonumber(os.getenv("LJ_M6_JIT_TABLE_READ_PUBROOT_PASSES")) or 64

local function producer(n)
  jit.opt.start("hotloop=1", "hotexit=1")
  local out = {}
  for i = 1, n do
    local marker = { kind = "readpub", token = "readpub:" .. i }
    out[#out + 1] = marker
    if i % 32 == 0 then collectgarbage("step") end
  end
  return out
end

for _ = 1, rounds do
  local ok, result = th.spawn(producer, nmarkers):join(30)
  assert(ok == true)
  local tokens = {}
  local weak = setmetatable({}, { __mode = "v" })
  collectgarbage("collect")
  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")
  for pass = 1, passes do
    for i = 1, #result do
      local marker = result[i]
      assert(marker.kind == "readpub")
      local token = marker.token
      assert(type(token) == "string")
      tokens[#tokens + 1] = token
      assert(type(marker.token) == "string")
      weak[marker.token] = marker
    end
    if pass % 4 == 0 then collectgarbage("step") end
  end
  assert(trace_count(200) > 0, "helper-backed marker-token read loop did not trace")
  collectgarbage("collect")
  for i = 1, #result do
    assert(type(result[i].token) == "string")
    assert(weak[result[i].token] == result[i])
  end
end
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

local function jit_tmpbuf_concat_append_smoke()
  return [=[
local util = require("jit.util")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")

local function loop(n)
  local out = ""
  for i = 1, n do
    out = out .. "/" .. i
  end
  return out
end

for k = 1, 20 do
  local got = loop(3)
  assert(got == "/1/2/3", "JIT tmpbuf concat loop lost prefix: " .. got)
end
assert(util.traceinfo(1), "tmpbuf concat loop did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")

local function path(root, ...)
  local out = root
  for i = 1, select("#", ...) do
    out = out .. "/" .. select(i, ...)
  end
  return out
end

for k = 1, 20 do
  local got = path("", "a", "b", "c")
  assert(got == "/a/b/c", "JIT vararg concat loop lost prefix: " .. got)
end
local shorter = path("", "x", "y")
assert(shorter == "/x/y", "JIT vararg concat arity change failed: " .. shorter)
assert(util.traceinfo(1), "tmpbuf vararg concat loop did not trace")
]=]
end

local function jit_flush_gc_current_stack_smoke()
  return [=[
local th = require("threading")
local floor = math.floor
local scale = tonumber(os.getenv("LJ_M6_JIT_GC_STACK_SCALE")) or 0.02
local nclosure = floor(5e6 * scale + 0.5)
if nclosure < 1 then nclosure = 1 end

local function make_hash_keys(n, first)
  local keys = {}
  first = first or 0
  for i = 1, n do keys[i] = "k" .. (first + i - 1) end
  return keys
end

local benches = {
  arith_loop = function(n)
    local x = 0
    for i = 1, n do x = x + i * 0.5 end
    return x
  end,
  fib30 = function()
    local function fib(n)
      if n < 2 then return n end
      return fib(n - 1) + fib(n - 2)
    end
    return fib(30)
  end,
  tab_hash_write = function(n)
    local t = {}
    for i = 1, n do t["k" .. (i % 8192)] = i end
    return t
  end,
  tab_store_existing = function(n)
    local keys = make_hash_keys(8192, 0)
    local t = {}
    for i = 1, 8192 do t[keys[i]] = 0 end
    for i = 1, n do t[keys[(i % 8192) + 1]] = i end
    return t
  end,
  tab_insert_newkey = function(n)
    local t = {}
    for i = 1, n do t["newk" .. i] = i end
    return t
  end,
}

local iters = {
  arith_loop = 5e7,
  fib30 = 1,
  tab_hash_write = 2e6,
  tab_store_existing = 2e7,
  tab_insert_newkey = 2e5,
}

local function closure(n)
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

local function scaled(name)
  return math.max(1, floor((iters[name] or 1) * scale + 0.5))
end

local function stat(label)
  local s = th.gcstats()
  assert(type(s) == "table")
  assert(type(s.phase) == "number")
  assert(type(s.total_kbytes) == "number")
  print(label,
        "phase=" .. s.phase,
        "kb=" .. s.total_kbytes,
        "roots=" .. s.root_spine_objects,
        "sweep=" .. s.sweep_owner_runs,
        "arenas=" .. s.sweep_owner_arenas,
        "since=" .. s.alloc_since_trigger,
        "cycle_alloc=" .. s.cycle_alloc_bytes,
        "trigger=" .. s.trigger_bytes,
        "hard=" .. s.hard_bytes,
        "live=" .. s.live_estimate)
end

collectgarbage("collect")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
for _, name in ipairs({
  "arith_loop",
  "fib30",
  "tab_hash_write",
  "tab_store_existing",
  "tab_insert_newkey",
}) do
  collectgarbage("collect")
  local n = scaled(name)
  local t0 = os.clock()
  benches[name](n)
  local dt = os.clock() - t0
  local line = string.format("ns=%.2f", dt / n * 1e9)
  assert(line:sub(1, 3) == "ns=", "pre format corrupt: " .. line)
  print("pre", name, line)
  io.stdout:flush()
end

collectgarbage("collect")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
stat("before_closure")

local best = math.huge
for r = 1, 5 do
  collectgarbage("collect")
  local t0 = os.clock()
  local sum = closure(nclosure)
  local dt = os.clock() - t0
  best = math.min(best, dt)
  local line = string.format("ns=%.2f", dt / nclosure * 1e9)
  assert(line:sub(1, 3) == "ns=", "closure format corrupt: " .. line)
  print("closure_run", r, line, sum)
  io.stdout:flush()
end

stat("after_closure")
local line = string.format("best_ns=%.2f", best / nclosure * 1e9)
assert(line:sub(1, 8) == "best_ns=", "best format corrupt: " .. line)
print(line)
print("jit-flush-gc-current-stack OK")
]=]
end

local function cclosure_upvalue_flush_smoke()
  return [=[
local trace_count = require"jit_harness".trace_count

local function assert_traced(label)
  assert(trace_count(200) > 0, label .. " did not trace")
end

local function assert_flushed(label)
  assert(trace_count(200) == 0, label .. " did not flush existing traces")
end

local _, orig_nil_name = debug.getupvalue(type, 1)
local function heat_type(n)
  local x
  for i = 1, n do x = type(nil) end
  return x
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
assert(heat_type(120) == orig_nil_name)
assert_traced("type(nil)")
assert(debug.setupvalue(type, 1, "mutnil"))
assert_flushed("type() C upvalue mutation")
assert(heat_type(1) == "mutnil")
assert(debug.setupvalue(type, 1, orig_nil_name))

local _, orig_pairs_iter = debug.getupvalue(pairs, 1)
local function heat_pairs(n, tab)
  local k, v
  for i = 1, n do
    local it, state = pairs(tab)
    k, v = it(state, nil)
  end
  return k, v
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local k, v = heat_pairs(120, { a = 1 })
assert(k == "a" and v == 1)
assert_traced("pairs()")
assert(debug.setupvalue(pairs, 1, function() return "mut", 42 end))
assert_flushed("pairs() C upvalue mutation")
k, v = heat_pairs(1, { a = 1 })
assert(k == "mut" and v == 42)
assert(debug.setupvalue(pairs, 1, orig_pairs_iter))

local _, orig_ipairs_iter = debug.getupvalue(ipairs, 1)
local function heat_ipairs(n, tab)
  local k, v
  for i = 1, n do
    local it, state, start = ipairs(tab)
    k, v = it(state, start)
  end
  return k, v
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
k, v = heat_ipairs(120, { 17 })
assert(k == 1 and v == 17)
assert_traced("ipairs()")
assert(debug.setupvalue(ipairs, 1, function() return "imut", 24 end))
assert_flushed("ipairs() C upvalue mutation")
k, v = heat_ipairs(1, { 17 })
assert(k == "imut" and v == 24)
assert(debug.setupvalue(ipairs, 1, orig_ipairs_iter))

local function make_counter(step)
  local x = step
  return function(n)
    local y = 0
    for i = 1, n do y = y + x end
    return y
  end
end

local f = make_counter(1)
local g = make_counter(7)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
assert(f(120) == 120)
assert_traced("Lua upvalue load")
assert(debug.setupvalue(f, 1, 3))
assert_flushed("debug.setupvalue() Lua upvalue mutation")
assert(f(100) == 300)

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
assert(f(120) == 360)
assert_traced("Lua upvalue reload")
debug.upvaluejoin(f, 1, g, 1)
assert_flushed("debug.upvaluejoin() Lua upvalue mutation")
assert(f(100) == 700)

jit.flush()
print("jit-cclosure-upvalue-flush OK")
]=]
end

local function env_mutation_flush_smoke()
  return [=[
local trace_count = require"jit_harness".trace_count

local function assert_traced(label)
  assert(trace_count(200) > 0, label .. " did not trace")
end

local function assert_flushed(label)
  assert(trace_count(200) == 0, label .. " did not flush existing traces")
end

local env_a = { x = 1 }
local env_b = { x = 2 }

local function read_global_x(n)
  local s = 0
  for i = 1, n do s = s + x end
  return s
end

setfenv(read_global_x, env_a)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
assert(read_global_x(120) == 120)
assert_traced("function environment global load")
setfenv(read_global_x, env_b)
assert_flushed("setfenv(function)")
assert(read_global_x(2) == 4)

local api_env_a = { y = 3 }
local api_env_b = { y = 4 }

local function read_global_y(n)
  local s = 0
  for i = 1, n do s = s + y end
  return s
end

debug.setfenv(read_global_y, api_env_a)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
assert(read_global_y(120) == 360)
assert_traced("API function environment global load")
debug.setfenv(read_global_y, api_env_b)
assert_flushed("debug.setfenv(function)")
assert(read_global_y(2) == 8)

local oldenv = getfenv(0)
local env_t1 = setmetatable({ marker = "A" }, { __index = oldenv })
local env_t2 = setmetatable({ marker = "B" }, { __index = oldenv })

local function read_thread_env(n)
  local v
  for i = 1, n do
    v = getfenv(0).marker
  end
  return v
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
setfenv(0, env_t1)
assert(read_thread_env(120) == "A")
assert_traced("thread environment getfenv(0)")
setfenv(0, env_t2)
assert_flushed("setfenv(0)")
assert(read_thread_env(2) == "B")
setfenv(0, oldenv)

jit.flush()
print("jit-env-mutation-flush OK")
]=]
end

return function(add)
  add({
    name = "m6_dispatch_redispatch",
    description = "M6 dispatch redispatch and x64 TG-local dispatch behavior",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build_and_run_c(t, t:tmp("lj_t_safepoint_handshake"),
                      "t-safepoint-handshake.c",
                      {
        build = false,
        cflags = gc2_test_cflags
      })
      print("M6 dispatch redispatch behavior passed")
    end
  })

  add({
    name = "m6_jit_token",
    description = "M6 JIT recorder token and x64 XPOLL behavior",
    run = function(t)
      clean_build(t, build.gc2_test_helper_opts({ quiet = true }))
      build_and_run_c(t, t:tmp("lj_t-jit-token"), "t-jit-token.c",
                      build.gc2_test_helper_opts({
                        build = false, clean = false, timeout = "20s"
                      }))
      build_and_run_c(t, t:tmp("lj_t-jit-startins-sidecar"),
                      "t-jit-startins-sidecar.c",
                      build.gc2_test_helper_opts({
                        build = false, clean = false, timeout = "20s"
                      }))
      luajit_file(t, t:path("tests", "t-jit-secondary.lua"),
                  { lua_path = true, timeout = "20s" })
      luajit_file(t, t:path("tests", "t-jit-explicit-exit.lua"),
                  { lua_path = true, timeout = "20s" })

      luajit_code(t, [=[
local util = require("jit.util")
jit.opt.start("hotloop=1","hotexit=1")
local s=0.0
for i=1,64 do s=s+i end
assert(s==2080.0)
assert(util.traceinfo(1), "simple numeric loop did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local util = require("jit.util")
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
assert(util.traceinfo(1), "deep inlined FUNCF loop did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "callunroll=32")
local function leaf(x) return x + 1 end
local function tail(x) return leaf(x) end
local sum = 0
for i = 1, 80 do sum = sum + tail(i) end
assert(sum == 3320)
assert(util.traceinfo(1), "Lua tailcall loop did not trace")
]=], { timeout = "20s" })
      print("M6 JIT recorder token behavior passed")
    end
  })

  add({
    name = "m6_jit_hotcall_missing_args",
    description = "FUNCF hot-call roots preserve missing fixed parameters",
    run = function(t)
      build.with_default_build_restore(t, function()
        clean_build(t, build.gc2_paranoia_opts({ quiet = true }))
        luajit_file(t, t:path("tests", "t-jit-hotcall-missing-args.lua"), {
          lua_path = true,
          timeout = "20s"
        })
        runtime.run_stock(t, {
          "test.lua", "--quiet", "lib/string/format/num.lua", "5"
        }, { timeout = "20s" })
      end)
      print("M6 JIT hot-call missing-argument behavior passed")
    end
  })

  add({
    name = "m6_jit_cell_ops",
    description = "M6 local-cell JIT recording behavior",
    run = function(t)
      build_default(t)
      cellops.run_jit_trace_behavior_checks(t)
      cellops.run_jit_closed_upvalue_store_behavior_checks(t)
      cellops.run_jit_runtime_checks(t)
      print("M6 JIT local-cell behavior passed")
    end
  })

  add({
    name = "m6_jit_fnew_bump",
    description = "local-cell CNEW/FNEW allocation and publication behavior",
    run = function(t)
      clean_build(t, build.func_helper_build_opts({ quiet = true }))
      build_and_run_c(t, t:tmp("lj_t-jit-fnew-bump"),
                      "t-jit-fnew-bump.c", build.func_helper_c_opts({
        build = false,
        timeout = "20s"
      }))
      print("M6 local-cell CNEW/FNEW allocation behavior passed")
    end
  })

  add({
    name = "m6_jit_recursive_call_unroll",
    description = "recursive JIT workload remains stable and traceable",
    run = function(t)
      build_default(t)
      luajit_code(t, [=[
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=56", "hotexit=10")
local function trace_stats()
  local n, uprec = 0, 0
  for i = 1, 256 do
    local info = util.traceinfo(i)
    if info then
      n = n + 1
      if info.linktype == "up-recursion" then uprec = uprec + 1 end
    end
  end
  return n, uprec
end
jit.off(trace_stats, true)
local function fib(n)
  if n < 2 then return n end
  return fib(n-1) + fib(n-2)
end
assert(fib(30) == 832040)
assert(util.traceinfo(1), "recursive fib workload did not trace")
local after_first, uprec_first = trace_stats()
assert(uprec_first > 0, "recursive fib did not publish an up-recursion trace")
assert(fib(30) == 832040)
local after_second, uprec_second = trace_stats()
assert(uprec_second > 0, "recursive fib lost its up-recursion graph")
for _ = 1, 4 do
  assert(fib(24) == 46368)
end
for _ = 1, 8 do
  assert(fib(30) == 832040)
end
local after_warm, uprec_warm = trace_stats()
assert(uprec_warm > 0, "recursive fib lost its stable up-recursion trace")
-- GC2 may retire dead side traces between samples, so a lower live count is
-- expected. Bound growth from the larger warmup sample instead of requiring
-- the live trace count to be monotonic.
local warm_base = math.max(after_first, after_second)
assert(after_warm <= warm_base + 4,
       "recursive fib kept recording after warmup: " ..
       warm_base .. " -> " .. after_warm)
assert(after_warm < 64, "recursive fib trace graph grew unexpectedly")
print("jit-recursive-call-unroll OK")
]=], { timeout = "20s" })
      print("M6 JIT recursive workload behavior passed")
    end
  })

  add({
    name = "m6_jit_recursive_retention",
    description = "recursive call-unroll trace retention instrumentation",
    run = function(t)
      clean_build(t, build.trace_helper_build_opts({ quiet = true }))
      build_and_run_c(t, t:tmp("lj_t-jit-recursive-retention"),
                      "t-jit-recursive-retention.c", build.trace_helper_c_opts({
        build = false,
        timeout = "20s"
      }))
      print("M6 JIT recursive trace-retention instrumentation passed")
    end
  })

  add({
    name = "m6_jit_barrier_xpoll",
    description = "x64 trace barrier behavior across XPOLL poll regions",
    run = function(t)
      build_default(t)
      luajit_code(t, [=[
local threading = require("threading")
local util = require("jit.util")
jit.opt.start("hotloop=1","hotexit=1")
jit.off()
local t={}
local mts={}
for i=1,80 do mts[i]={} end
assert(threading.gcworkers(1) == 0)
jit.on()
for i=1,64 do setmetatable(t, mts[i]) end
assert(getmetatable(t) == mts[64])
assert(util.traceinfo(1), "setmetatable loop did not trace")
assert(threading.gcworkers(0) == 1)
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
local util = require("jit.util")
jit.opt.start("hotloop=1","hotexit=1")
jit.off()
local uv
local vals={}
for i=1,80 do vals[i]={} end
assert(threading.gcworkers(1) == 0)
jit.on()
local function f()
  for i=1,64 do uv=vals[i] end
end
f()
assert(uv==vals[64])
assert(util.traceinfo(1), "upvalue barrier loop did not trace")
assert(threading.gcworkers(0) == 1)
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local x = 0
local function inc()
  x = x + 1
end
assert(threading.gcworkers(1) == 0)
for i = 1, 80 do inc() end
assert(x == 80)
assert(util.traceinfo(1), "numeric upvalue loop did not trace")
assert(threading.gcworkers(0) == 1)
]=], { timeout = "20s" })
      print("M6 JIT XPOLL barrier behavior passed")
    end
  })

  add({
    name = "m6_jit_xsave",
    description = "dormant XSAVE snapshots retain materialized allocation roots",
    run = function(t)
      local flags = "-DLUA_USE_ASSERT -DLJ_XSAVE_TEST_HELPERS"
      build.with_default_build_restore(t, function()
        clean_build(t, { quiet = true, xcflags = flags })
        build_and_run_c(t, t:tmp("lj_t-jit-xsave"), "t-jit-xsave.c", {
          build = false,
          cflags = flags,
          timeout = "20s"
        })
      end)
      print("M6 JIT XSAVE snapshot behavior passed")
    end
  })

  add({
    name = "m6_jit_xbar_xpoll",
    description = "FFI XBAR aliasing respects XPOLL poll regions",
    run = function(t)
      build_default(t)
      luajit_code(t, [=[
local ffi = require("ffi")
local threading = require("threading")
local util = require("jit.util")
local dst = ffi.new("uint8_t[512]")
local src = ffi.new("uint8_t[512]")
jit.opt.start("hotloop=1", "hotexit=1")
assert(threading.gcworkers(1) == 0)
for i = 1, 80 do ffi.copy(dst, src, 512) end
assert(util.traceinfo(1), "FFI copy loop did not trace")
assert(threading.gcworkers(0) == 1)
]=])

      luajit_code(t, [=[
local ffi = require("ffi")
local threading = require("threading")
local util = require("jit.util")
local a = ffi.new("int[256]")
jit.opt.start("hotloop=1", "hotexit=1")
local s = 0
assert(threading.gcworkers(1) == 0)
for i = 1, 120 do s = s + a[i % 128] end
assert(s == 0)
assert(not util.traceinfo(1),
       "runtime-shared cdata bypassed MT trace safety gate")
assert(threading.gcworkers(0) == 1)
]=])

      luajit_code(t, [=[
local ffi = require("ffi")
local threading = require("threading")
local util = require("jit.util")
ffi.cdef("typedef struct { double x, y; } xpoll_point_t;")
local p = ffi.new("xpoll_point_t", 1, 2)
jit.opt.start("hotloop=1", "hotexit=1")
local s = 0
assert(threading.gcworkers(1) == 0)
for i = 1, 120 do
  p.x = p.x + 1
  s = s + p.y
end
assert(s == 240 and p.x == 121)
assert(not util.traceinfo(1),
       "runtime-shared cdata bypassed MT trace safety gate")
assert(threading.gcworkers(0) == 1)
]=])

      luajit_code(t, [=[
local ffi = require("ffi")
local threading = require("threading")
local util = require("jit.util")
local a = ffi.new("int[256]")
jit.opt.start("hotloop=1", "hotexit=1")
assert(threading.gcworkers(1) == 0)
for i = 1, 120 do a[i % 128] = i end
assert(a[119 % 128] == 119)
assert(not util.traceinfo(1),
       "runtime-shared cdata bypassed MT trace safety gate")
assert(threading.gcworkers(0) == 1)
]=])

      luajit_code(t, [=[
local threading = require("threading")
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = { 1, 2, 3 }
local s = 0
assert(threading.gcworkers(1) == 0)
for _ = 1, 80 do s = s + #t end
assert(s == 240)
assert(util.traceinfo(1), "table length loop did not trace")
assert(threading.gcworkers(0) == 1)
]=])
      run_lua_test_case(t, "m5_jit_hash_store_nyi")
      print("M6 JIT XBAR/XPOLL alias behavior passed")
    end
  })

  add({
    name = "m6_jit_table_store_helper",
    description = "M6 helper-backed table store behavior",
    run = function(t)
      t:build(build.tab_helper_build_opts({ quiet = true }))
      build_and_run_c(t, t:tmp("lj_t-jit-forward-store"),
                      "t-jit-forward-store.c", build.tab_helper_c_opts({
                        build = false,
                        timeout = "20s"
                      }))
      luajit_code(t, table_store_smoke())
      luajit_code(t, table_read_pubroot_smoke(), { timeout = "20s" })

      luajit_code(t, [=[
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

      luajit_code(t, [=[
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

      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
jit.off()
local keys = {}
for i = 1, 128 do keys[i] = "k" .. i end
jit.on()
local h = {}
for i = 1, 80 do
  h[keys[(i % 128) + 1]] = i
end
assert(h.k1 == nil and h.k80 == 79)
assert(util.traceinfo(1), "pre-MT new hash store did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
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

      luajit_code(t, [=[
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

      luajit_code(t, [=[
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

      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
local keys = {}
jit.off()
for k in pairs(package) do keys[#keys+1] = k end
jit.on()
local t = {}
for i = 1, 80 do
  local k = keys[((i - 1) % #keys) + 1]
  local s = tostring(k)
  t[#t+1] = s
  assert(t[#t] == s and type(t[#t]) == "string",
         "numeric NEWREF helper crossed value/key TValue temps")
end
assert(util.traceinfo(1), "numeric NEWREF append did not trace")
]=])

      luajit_code(t, [=[
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

      luajit_code(t, [=[
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

      luajit_code(t, [=[
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

      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
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
assert(util.traceinfo(1), "same-slot array store/read did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
jit.off()
local h = { stable = 0 }
jit.on()
local s = 0
for i = 1, 80 do
  h.stable = i + 0.5
  s = s + h.stable
end
assert(s == 3280 and h.stable == 80.5)
assert(util.traceinfo(1), "same-slot hash store/read did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
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
jit.off()
local b = {}
for i = 1, 256 do b[i] = 0 end
jit.on()
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
]=], { timeout = "20s" })

      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require("jit.util")
jit.off()
local values = {}
for i = 1, 128 do values[i] = { i } end
jit.on()
local a = { false }
for i = 1, 80 do
  a[1] = values[i]
end
assert(a[1][1] == 80)
assert(util.traceinfo(1), "single-thread GC array store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = false }
for i = 1, 80 do
  h.stable = values[i]
end
assert(h.stable[1] == 80)
assert(util.traceinfo(1), "single-thread GC hash store did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
assert(({ threading.spawn(function() return true end):join(5) })[1] == true)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local trace_count = require("jit_harness").trace_count
local function hash(n)
  local out = { stable = 0 }
  for i = 1, n do
    local t = { stable = 0 }
    t.stable = i + 0.5
    out = t
  end
  return out
end
local h = hash(80)
assert(h.stable == 80.5)
assert(trace_count(200) > 0, "trace-local hash store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function array(n)
  local out = { 0 }
  for i = 1, n do
    local t = { 0 }
    t[1] = i + 0.5
    out = t
  end
  return out
end
local a = array(80)
assert(a[1] == 80.5)
assert(trace_count(200) > 0, "trace-local array store did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
assert(({ threading.spawn(function() return true end):join(5) })[1] == true)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local trace_count = require("jit_harness").trace_count
jit.off()
local values = {}
for i = 1, 128 do values[i] = { i } end
jit.on()
local function hash(n)
  local out
  for i = 1, n do
    local t = { stable = false }
    t.stable = values[i]
    out = t
  end
  return out
end
local h = hash(80)
assert(h.stable[1] == 80)
assert(trace_count(200) > 0, "trace-local GC hash store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local function array(n)
  local out
  for i = 1, n do
    local t = { false }
    t[1] = values[i]
    out = t
  end
  return out
end
local a = array(80)
assert(a[1][1] == 80)
assert(trace_count(200) > 0, "trace-local GC array store did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
assert(({ threading.spawn(function() return true end):join(5) })[1] == true)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local trace_count = require("jit_harness").trace_count
local a = { 0 }
for i = 1, 80 do
  a[1] = i + 0.5
end
assert(a[1] == 80.5)
assert(trace_count(200) > 0, "numeric array store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local b = {}
for i = 1, 256 do b[i] = 0 end
for i = 1, 80 do
  local j = (i % 256) + 1
  b[j] = i + 0.5
end
assert(b[81] == 80.5)
assert(trace_count(200) > 0, "separated numeric array store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = 0 }
for i = 1, 80 do
  h.stable = i + 0.5
end
assert(h.stable == 80.5)
assert(trace_count(200) > 0, "numeric hash store did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
assert(({ threading.spawn(function() return true end):join(5) })[1] == true)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local trace_count = require("jit_harness").trace_count
local h = { stable = 0 }
for i = 1, 80 do
  h.stable = i + 0.5
end
assert(h.stable == 80.5)
assert(trace_count(200) > 0, "numeric hash store did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
assert(({ threading.spawn(function() return true end):join(5) })[1] == true)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local trace_count = require("jit_harness").trace_count
local function make_escaped_store()
  local sink
  return function(n)
    for i = 1, n do
      local t = { stable = 0 }
      sink = t
      t.stable = i + 0.5
    end
    return sink.stable
  end
end
local escaped_store = make_escaped_store()
assert(escaped_store(80) == 80.5)
assert(trace_count(200) > 0, "escaped trace-local store did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
assert(({ threading.spawn(function() return true end):join(5) })[1] == true)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local trace_count = require("jit_harness").trace_count
local a = { 0 }
for i = 1, 80 do
  a[1] = i
end
assert(a[1] == 80)
assert(trace_count(200) > 0, "integer array store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = 0 }
for i = 1, 80 do
  h.stable = i
end
assert(h.stable == 80)
assert(trace_count(200) > 0, "integer hash store did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
assert(({ threading.spawn(function() return true end):join(5) })[1] == true)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local trace_count = require("jit_harness").trace_count
local a = { false }
for i = 1, 80 do
  a[1] = (i % 2) == 0
end
assert(a[1] == true)
assert(trace_count(200) > 0, "boolean array store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = false }
for i = 1, 80 do
  h.stable = (i % 2) == 0
end
assert(h.stable == true)
assert(trace_count(200) > 0, "boolean hash store did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
assert(({ threading.spawn(function() return true end):join(5) })[1] == true)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local trace_count = require("jit_harness").trace_count
local h = { stable = 0 }
h.stable = nil
for i = 1, 80 do
  h.stable = i
  h.stable = nil
end
assert(h.stable == nil)
assert(trace_count(200) > 0, "active-MT previous-nil hash store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local a = { 0, nil, 0 }
for i = 1, 80 do
  a[2] = i
  a[2] = nil
end
assert(a[2] == nil)
assert(trace_count(200) > 0, "active-MT previous-nil array store did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
assert(({ threading.spawn(function() return true end):join(5) })[1] == true)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local trace_count = require("jit_harness").trace_count
jit.off()
local keys = {}
for i = 1, 128 do keys[i] = "k" .. i end
jit.on()
local h = {}
for i = 1, 80 do
  h[keys[(i % 128) + 1]] = i
end
assert(h.k80 == 79)
assert(trace_count(200) > 0, "active-MT new hash store did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local a = {}
for i = 1, 80 do
  a[i] = i
end
assert(a[80] == 80)
assert(trace_count(200) > 0, "active-MT new numeric array store did not trace")
]=], { timeout = "20s" })
      print("M6 JIT table-store helper behavior passed")
    end
  })

  add({
    name = "m6_jit_entering_table_store",
    description = "JIT table stores use shared route during mt_entering",
    run = function(t)
      build_and_run_c(t, t:tmp("lj_t-jit-entering-table-store"),
                      "t-jit-entering-table-store.c", {
        clean = true,
        timeout = "20s"
      })
      print("M6 JIT mt_entering table-store route passed")
    end
  })

  add({
    name = "m6_jit_aref_pair_boundary",
    description = "M6 x64 pre-MT direct AREF and active-MT read-helper behavior",
    run = function(t)
      build_default(t)
      luajit_code(t, [=[
local util = require("jit.util")
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
assert(util.traceinfo(1), "separated array read did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local util = require("jit.util")
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
assert(util.traceinfo(1), "split array read did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local util = require("jit.util")
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
assert(util.traceinfo(1), "colocated array read did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local util = require("jit.util")
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
assert(util.traceinfo(1), "out-of-array miss loop did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
local trace_count = require("jit_harness").trace_count
assert(({ threading.spawn(function() return true end):join(5) })[1] == true)
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
assert(trace_count(200) > 0, "active-MT shared array read did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local threading = require("threading")
local trace_count = require("jit_harness").trace_count
assert(({ threading.spawn(function() return true end):join(5) })[1] == true)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local h = { stable = 7 }
local s = 0
for i = 1, 80 do
  s = s + h.stable
end
assert(s == 560)
assert(trace_count(200) > 0, "active-MT shared hash read did not trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local k = "stable"
s = 0
for i = 1, 80 do
  s = s + h[k]
end
assert(s == 560)
assert(trace_count(200) > 0, "active-MT shared dynamic hash read did not trace")
]=], { timeout = "20s" })

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
      print("M6 JIT pre-MT AREF and active-MT read-helper behavior passed")
    end
  })

  add({
    name = "m6_jit_tbar_gc2_black_gate",
    description = "M6 JIT numeric table barriers only cover required edges",
    run = function(t)
      build_default(t)
      luajit_code(t, [=[
local th = require("threading")
jit.off()
local keys = {}
for i = 0, 8191 do keys[i + 1] = "k" .. i end
jit.on()
collectgarbage("collect")
local base = th.gcstats()
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = {}
for i = 1, 200000 do
  t[keys[(i % 8192) + 1]] = i
end
assert(t.k1 == 196609)
local before = th.gcstats()
assert(before.cycle_starts > base.cycle_starts,
       "JIT table-store probe did not trigger a GC2 cycle")
local ssb0 = base.worker_ssb_converted + base.assist_ssb_converted
local ssb1 = before.worker_ssb_converted + before.assist_ssb_converted
assert(ssb1 - ssb0 < 10000,
       "TBAR queued one GC2 SSB entry per numeric hash store")
local grey0 = before.worker_grey_drained + before.assist_grey_drained
collectgarbage("step", 0)
local after = th.gcstats()
local grey1 = after.worker_grey_drained + after.assist_grey_drained
assert(grey1 - grey0 < 10000,
       "mark completion revisited numeric hash-store table per iteration")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local weak = setmetatable({}, { __mode = "k" })
do
  local key = { tag = "weak-key-tbar" }
  weak[key] = 1
  for i = 1, 80 do
    weak[key] = nil
  end
end
collectgarbage("collect")
assert(next(weak) == nil, "key-only TBAR kept a weak key alive")
]=], { timeout = "20s" })
      luajit_code(t, [=[
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = {}
for i = 1, 128 do t["k" .. i] = 0 end
for i = 1, 80 do
  local k = "k" .. ((i % 128) + 1)
  t[k] = i
end
assert(t.k1 == 0 and t.k81 == 80)
assert(util.traceinfo(1), "existing hash-store TBAR probe did not trace")
]=], { timeout = "20s" })
      build.with_default_build_restore(t, function()
        build_default(t, {
          args = { "XCFLAGS=" .. build.gc2_paranoia_flags }
        })
        luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local count = 0
local t = setmetatable({ foo = nil },
  { __newindex = function() count = count + 1 end })
for j = 1, 2 do
  for i = 1, 100 do t.foo = 1 end
  rawset(t, "foo", 1)
end
assert(count == 100)
]=], { timeout = "20s" })
      end)
      print("M6 JIT GC2 TBAR black gate behavior passed")
    end
  })

  add({
    name = "m6_jit_tmpbuf_thread_format",
    description = "M6 JIT uses the running TG tmpbuf for threaded string.format traces",
    run = function(t)
      build_default(t)
      luajit_code(t, [=[
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local s = 0
for i = 1, 80 do
  s = s + #string.format("%d:%s", i, "x")
end
assert(s > 0)
assert(util.traceinfo(1), "string.format loop did not trace")
]=])
      luajit_code(t, [=[
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function loop(n)
  local out = ""
  for i = 1, n do
    out = out .. "/" .. i
  end
  return out
end
for k = 1, 3 do
  assert(loop(3) == "/1/2/3")
end
assert(util.traceinfo(1), "string concat loop did not trace")
]=])
      luajit_code(t, [=[
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function loop(n)
  local h = {}
  for i = 1, n do
    h["k" .. (i % 8192)] = i
  end
  return h
end
local h = loop(20000)
assert(h.k1)
assert(util.traceinfo(1), "source-local FORL string key loop did not trace")
]=])
      luajit_code(t, jit_tmpbuf_concat_append_smoke())
      luajit_code(t, jit_tmpbuf_thread_format_smoke(), { timeout = "30s" })
      build_and_run_c(t, t:tmp("lj_t-jit-tg-tmpbuf-reset"),
                      "t-jit-tg-tmpbuf-reset.c", { build = false })
      print("M6 JIT threaded string.format tmpbuf smoke passed")
    end
  })

  add({
    name = "m6_jit_hrefk_nodehdr",
    description = "M6 x64 HREFK node-header behavior",
    run = function(t)
      build_default(t)
      luajit_code(t, [=[
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = { foo = 1, bar = 2, baz = 3 }
local s = 0
for i = 1, 60 do
  s = s + t.foo
end
assert(s == 60)
assert(util.traceinfo(1), "constant-key hash lookup did not trace")
]=])
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
      luajit_code(t, [=[
local util = require("jit.util")
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
assert(util.traceinfo(1), "dynamic string-key lookup did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local util = require("jit.util")
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
assert(util.traceinfo(1), "empty-hash miss loop did not trace")
]=], { timeout = "20s" })
      print("M6 JIT dynamic HREF node-header behavior passed")
    end
  })

  add({
    name = "m6_jit_alloc_account",
    description = "M6 allocator accounting behavior",
    run = function(t)
      clean_build(t, { xcflags = gc2_test_cflags })
      build_and_run_c(t, t:tmp("lj_t-gc2-alloc-account"),
                      "t-gc2-alloc-account.c",
                      {
                        build = false,
                        timeout = "20s",
                        cflags = gc2_test_cflags
                      })
      build_and_run_c(t, t:tmp("lj_t-gc2-interp-hard-check"),
                      "t-gc2-interp-hard-check.c",
                      {
                        build = false,
                        timeout = "20s",
                        cflags = gc2_test_cflags
                      })
      build_and_run_c(t, t:tmp("lj_t-jit-idle-reclaim-entry"),
                      "t-jit-idle-reclaim-entry.c",
                      {
                        build = false,
                        timeout = "20s",
                        cflags = gc2_test_cflags
                      })
      build_and_run_c(t, t:tmp("lj_t-gc2-jit-sweep-coop-helper"),
                      "t-gc2-jit-sweep-coop.c",
                      {
                        build = false,
                        timeout = "30s",
                        cflags = gc2_test_cflags
                      })
      build_and_run_c(t, t:tmp("lj_t-gc2-jit-mark-coop-helper"),
                      "t-gc2-jit-mark-coop.c",
                      {
                        build = false,
                        timeout = "40s",
                        cflags = gc2_test_cflags
                      })
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
      build_and_run_c(t, t:tmp("lj_t-gc2-jit-sweep-coop"),
                      "t-gc2-jit-sweep-coop.c", { build = false, timeout = "30s" })
      build_and_run_c(t, t:tmp("lj_t-gc2-jit-mark-coop"),
                      "t-gc2-jit-mark-coop.c", { build = false, timeout = "40s" })

      luajit_code(t, [=[
local util = require("jit.util")
jit.opt.start("hotloop=1","hotexit=1")
local x
local i = 1
while i <= 100 do x = {}; i = i + 1 end
assert(type(x)=="table")
assert(util.traceinfo(1), "TNEW readiness loop did not trace")
]=])

      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local util = require("jit.util")
local keep = {}
local i = 1
while i <= 80 do
  keep[i] = {}
  i = i + 1
end
assert(type(keep[80]) == "table")
assert(util.traceinfo(1), "empty-table TNEW did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "-sink")
local util = require("jit.util")
local keep = {}
local i = 1
while i <= 80 do
  keep[i] = { i }
  i = i + 1
end
assert(keep[80][1] == 80)
assert(util.traceinfo(1), "non-empty TNEW did not trace")
]=], { timeout = "20s" })

      luajit_code(t, [=[
local ffi=require("ffi")
local util = require("jit.util")
ffi.cdef("typedef struct { int x; } lj_gc2_dump_cnew_t;")
local ct=ffi.typeof("lj_gc2_dump_cnew_t")
jit.opt.start("hotloop=1","hotexit=1","-sink")
local x
local i = 1
while i <= 100 do x = ct(i); i = i + 1 end
assert(x.x==100)
assert(util.traceinfo(1), "CNEW readiness loop did not trace")
]=])

      luajit_code(t, [=[
local util = require("jit.util")
jit.opt.start("hotloop=1","hotexit=1","-sink")
local s="abcdef"
local x
local i = 1
while i <= 100 do x = string.sub(s,1,3); i = i + 1 end
assert(x=="abc")
assert(util.traceinfo(1), "SNEW readiness loop did not trace")
]=])
      print("M6 JIT GC2 readiness behavior passed")
    end
  })

  add({
    name = "m6_jit_gcstep_pacing",
    description = "color-state JIT GC-step pacing behavior",
    run = function(t)
      clean_build(t)
      luajit_code(t, [=[
local util = require("jit.util")
jit.opt.start("hotloop=1","hotexit=1")
local x
local i = 1
while i <= 100 do x = {}; i = i + 1 end
assert(type(x)=="table")
assert(util.traceinfo(1), "GC-step allocation loop did not trace")
]=])
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
      luajit_code(t, [=[
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
local n = 200000
local want = n * (n + 1) / 2 + n
for _ = 1, 3 do
  collectgarbage("collect")
  assert(run(n) == want)
end
]=], { joff = true, timeout = "10s" })
      print("M6 JIT GC-step behavior passed")
    end
  })

  add({
    name = "m6_jit_mcode_native",
    description = "Linux/x64 mcode allocation and sync-core native boundary",
    run = function(t)
      clean_build(t)
      luajit_code(t, [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "sizemcode=4", "maxmcode=64")
local function make(seed)
  return assert(loadstring(("return function(n) local s=%d; for i=1,n do s=s+i end return s end"):format(seed)))()
end
for n = 1, 16 do
  local seed = n * 31
  local f = make(seed)
  for _ = 1, 8 do
    assert(f(40) == seed + 820)
  end
end
local live = 0
for tr = 1, 64 do
  if util.traceinfo(tr) then live = live + 1 end
end
assert(live >= 8, live)
]=], { timeout = os.getenv("M6_MCODE_TIMEOUT") or "60s" })
      print("M6 JIT mcode native boundary behavior passed")
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
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1","hotexit=1")
local s=0
for i=1,80 do s=s+i end
assert(s==3240)
assert(util.traceinfo(1), "mcode publication loop did not trace")
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
assert(live >= 4, live)
]=], { timeout = timeout })
      luajit_file(t, t:path("tests", "t-jit-mcode-fresh.lua"),
                  { lua_path = true, timeout = timeout })
      print("M6 JIT mcode publication behavior passed")
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
      print("M6 JIT flush handshake behavior passed")
    end
  })

  add({
    name = "m6_jit_flush_gc_current_stack",
    description = "JIT flush and full GC preserve the active stack roots",
    run = function(t)
      build_default(t)
      luajit_code(t, jit_flush_gc_current_stack_smoke(),
                  { timeout = "45s" })
      print("M6 JIT flush active-stack GC behavior passed")
    end
  })

  add({
    name = "m6_jit_util_flush_race",
    description = "jit.util trace readers tolerate concurrent trace flushes",
    run = function(t)
      build_default(t)
      luajit_file(t, t:path("tests", "t-jit-util-flush-race.lua"),
                  { lua_path = true,
                    timeout = os.getenv("LJ_M6_JIT_UTIL_FLUSH_RACE_TIMEOUT") or
                      "120s" })
      print("M6 jit.util concurrent flush reader behavior passed")
    end
  })

  add({
    name = "m6_jit_flush_thread_stress",
    description = "threaded JIT flush preserves stale bytecode and trace slots",
    run = function(t)
      build_default(t)
      luajit_file(t, t:path("tests", "t-jit-flush-thread-stress.lua"),
                  { lua_path = true, timeout = "60s" })
      print("M6 JIT threaded flush stress passed")
    end
  })

  add({
    name = "m6_jit_flush_join_token_liveness",
    description = "blocking join releases asynchronously aborted recorder ownership",
    run = function(t)
      build_default(t)
      -- This is the deterministic reducer for the heavy stress failure: the
      -- top-level churn FORL owns the recorder token at round 40 while its
      -- short-lived peer enters jit.flush() and must finish before join.
      luajit_file(t, t:path("tests", "t-jit-flush-thread-stress.lua"), {
        lua_path = true,
        timeout = "20s",
        env = {
          LJ_M6_JIT_FLUSH_THREAD_THREADS = "1",
          LJ_M6_JIT_FLUSH_THREAD_ROUNDS = "1",
          LJ_M6_JIT_FLUSH_THREAD_CHURN = "96",
          LJ_M6_JIT_FLUSH_THREAD_JOIN_TIMEOUT = "5"
        }
      })
      print("M6 JIT blocking-join recorder-token liveness passed")
    end
  })

  add({
    name = "m6_jit_park_vmevent_reentrant",
    description = "blocking parks preserve active VM-event recorder frames",
    run = function(t)
      build_default(t)
      luajit_file(t, t:path("tests", "t-jit-park-vmevent-reentrant.lua"),
                  { lua_path = true, timeout = "20s" })
      print("M6 JIT VM-event park reentrancy passed")
    end
  })

  add({
    name = "m6_jit_flush_thread_heavy_stress",
    description = "heavier threaded JIT flush stress with progress diagnostics",
    run = function(t)
      build_default(t)
      luajit_file(t, t:path("tests", "t-jit-flush-thread-stress.lua"), {
        lua_path = true,
        timeout = os.getenv("LJ_M6_JIT_FLUSH_THREAD_HEAVY_TIMEOUT") or "90s",
        env = {
          LJ_M6_JIT_FLUSH_THREAD_THREADS =
            os.getenv("LJ_M6_JIT_FLUSH_THREAD_HEAVY_THREADS") or "4",
          LJ_M6_JIT_FLUSH_THREAD_ROUNDS =
            os.getenv("LJ_M6_JIT_FLUSH_THREAD_HEAVY_ROUNDS") or "96",
          LJ_M6_JIT_FLUSH_THREAD_CHURN =
            os.getenv("LJ_M6_JIT_FLUSH_THREAD_HEAVY_CHURN") or "192",
          LJ_M6_JIT_FLUSH_THREAD_JOIN_TIMEOUT =
            os.getenv("LJ_M6_JIT_FLUSH_THREAD_HEAVY_JOIN_TIMEOUT") or "60"
        }
      })
      print("M6 JIT threaded flush heavy stress passed")
    end
  })

  add({
    name = "m6_jit_mt_activation_flush",
    description = "pre-MT JIT traces are flushed before first thread activation",
    run = function(t)
      build_default(t)
      luajit_code(t, [=[
local threading = require("threading")
local trace_count = require("jit_harness").trace_count

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "maxtrace=8")
local function hot(n)
  local s = 0
  for i = 1, n do s = s + i end
  return s
end
for _ = 1, 20 do assert(hot(80) == 3240) end
assert(trace_count(200) > 0, "pre-MT loop did not trace")

local worker = threading.spawn(function() return true end)
assert(({ worker:join(5) })[1] == true)
assert(trace_count(200) == 0, "first thread activation did not flush traces")

-- mt_active remains sticky after the worker generation. Exercise more full
-- flush generations than the deliberately tiny trace namespace: retired slots
-- must age through the safepoint grace period and become reusable instead of
-- accumulating forever between worker generations.
for round = 1, 24 do
  for _ = 1, 4 do assert(hot(80) == 3240) end
  assert(trace_count(32) > 0,
         "sticky-MT trace namespace exhausted at churn round " .. round)
  jit.flush()
end
]=], { timeout = "20s" })
      print("M6 JIT MT activation flush and retired-slot reuse passed")
    end
  })

  add({
    name = "m6_jit_gcworkers_activation_flush",
    description = "pre-worker JIT traces are flushed before GC worker activation",
    run = function(t)
      build_default(t)
      luajit_code(t, [=[
local threading = require("threading")
local trace_count = require("jit_harness").trace_count

assert(threading.gcworkers(0) >= 0)
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function hot(n)
  local s = 0
  for i = 1, n do s = s + i end
  return s
end
for _ = 1, 20 do assert(hot(80) == 3240) end
assert(trace_count(200) > 0, "pre-worker loop did not trace")

assert(threading.gcworkers(1) == 0)
assert(trace_count(200) == 0, "GC worker activation did not flush traces")
assert(threading.gcworkers(0) == 1)
]=], { timeout = "20s" })
      print("M6 JIT GC-worker activation flush behavior passed")
    end
  })

  add({
    name = "m6_jit_vmevent_flush",
    description = "JIT trace event hooks run with a valid TG dispatch",
    run = function(t)
      build_default(t)
      luajit_file(t, t:path("tests", "t-jit-vmevent-flush.lua"),
                  { lua_path = true, timeout = "20s" })
      print("M6 JIT VM event flush hook behavior passed")
    end
  })

  add({
    name = "m6_jit_traceerr_format",
    description = "JIT trace diagnostic formatters tolerate missing NYI bytecode info",
    run = function(t)
      build_default(t)
      luajit_file(t, t:path("tests", "t-jit-traceerr-format.lua"),
                  { lua_path = true, timeout = "20s" })
      print("M6 JIT trace diagnostic formatter behavior passed")
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
    name = "m6_jit_gdbjit_publish",
    description = "opt-in GDBJIT trace descriptor publication behavior",
    run = function(t)
      local ok, err = pcall(function()
        t:build({
          clean = true,
          quiet = true,
          xcflags = "-DLUAJIT_USE_GDBJIT"
        })
        luajit_code(t, [=[
local util = require("jit.util")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")

local function heat(seed)
  local s = seed
  for i = 1, 140 do
    s = s + i
  end
  return s
end

for n = 1, 8 do
  assert(heat(n) == n + 9870)
end
assert(util.traceinfo(1), "GDBJIT build did not publish a trace")

jit.flush()
for n = 1, 8 do
  assert(heat(n * 3) == n * 3 + 9870)
end
assert(util.traceinfo(1), "GDBJIT build did not republish after flush")
]=], { timeout = "20s" })
        luajit_code(t, [=[
local threading = require("threading")
local done = threading.channel(4)

local function worker(id)
  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")
  local acc = id
  for round = 1, 6 do
    for i = 1, 120 do
      acc = acc + i + round
    end
  end
  assert(acc > id)
  jit.flush()
  done:send(true)
end

local threads = {}
for i = 1, 4 do
  threads[i] = threading.spawn(worker, i)
end
for i = 1, 4 do
  local ok = done:recv(5)
  assert(ok == true)
end
for i = 1, 4 do
  assert(({ threads[i]:join(5) })[1] == true)
end
]=], { timeout = "30s" })
      end)
      t:build({ clean = true, quiet = true })
      if not ok then error(err, 0) end
      print("M6 GDBJIT descriptor publication behavior passed")
    end
  })

  add({
    name = "m6_jit_io_native_stopreq",
    description = "JIT IO write/flush native-state behavior",
    run = function(t)
      build_default(t)
      luajit_file(t, t:path("tests", "t-jit-io-native-stopreq.lua"), {
        env = { LJ_JIT_IO_STOPREQ_OUT = t:tmp("lj_jit_io_stopreq.out") },
        timeout = "20s"
      })
      print("M6 JIT IO native-state STOPREQ behavior passed")
    end
  })

  add({
    name = "m6_jit_cclosure_upvalue_flush",
    description = "JIT traces over C and Lua upvalues flush on debug/API mutation",
    run = function(t)
      build_default(t)
      luajit_code(t, cclosure_upvalue_flush_smoke(), { timeout = "20s" })
      build_and_run_c(t, t:tmp("lj_t-lua-upvaluejoin-trace"),
                      "t-lua-upvaluejoin-trace.c", {
        build = false,
        env = { LUA_PATH = runtime.lua_path(t) },
        timeout = "20s"
      })
      print("M6 JIT upvalue mutation flush behavior passed")
    end
  })

  add({
    name = "m6_jit_trace_proto_gc",
    description = "stock trace/prototype GC lifetime and record callback reachability",
    run = function(t)
      build_default(t)
      luajit_file(t, t:path("tests", "t-jit-trace-gc-pressure.lua"), {
        timeout = "20s"
      })
      runtime.run_stock(t, { "misc/gc_trace.lua" }, { timeout = "20s" })
      print("M6 JIT trace/prototype GC stock oracle passed")
    end
  })

  add({
    name = "m6_jit_env_mutation_flush",
    description = "JIT traces over function/thread environments flush on replacement",
    run = function(t)
      build_default(t)
      luajit_code(t, env_mutation_flush_smoke(), { timeout = "20s" })
      print("M6 JIT environment mutation flush behavior passed")
    end
  })

  add({
    name = "m6_jit_threading_nyi_boundary",
    description = "simple threading fast functions use generic JIT NYI boundaries",
    run = function(t)
      build_default(t)
      luajit_code(t, [=[
local threading = require("threading")
local util = require("jit.util")

local current = threading.current()
local current_id = current:id()
local mutex = threading.mutex()
local channel = threading.channel(1)

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1", "minstitch=1")

local seen_now = 0
local seen_status = 0
local seen_names = 0
local cpu = 0
local acc = 0
for i = 1, 80 do
  acc = acc + i
  local now = threading.now()
  if now then seen_now = seen_now + 1 end
  cpu = threading.cpucount()
  if current:id() == current_id and current:running() then
    seen_status = seen_status + 1
  end
  if tostring(current) == "threading.thread" and
     tostring(mutex) == "threading.mutex" and
     tostring(channel) == "threading.channel" then
    seen_names = seen_names + 1
  end
end

assert(seen_now == 80, "threading.now() lost successful clock reads")
assert(seen_status == 80, "thread status fastfuncs changed behavior")
assert(seen_names == 80, "threading __tostring fastfuncs changed behavior")
assert(cpu >= 1, "threading.cpucount() returned an invalid CPU count")
assert(acc == 3240, "loop body changed while tracing threading fastfuncs")
assert(util.traceinfo(1), "threading.now/cpucount loop did not trace")
]=], { timeout = "20s" })
      print("M6 JIT threading NYI boundary behavior passed")
    end
  })

  add({
    name = "m6_jit_buffer_method_shared_nyi",
    description = "JIT string.buffer method behavior",
    run = function(t)
      build_default(t)
      luajit_code(t, [=[
local buffer = require("string.buffer")
local util = require("jit.util")
local ffi_ok, ffi = pcall(require, "ffi")
if ffi_ok then ffi.cdef("typedef unsigned char lj_m6_buf_u8;") end

local function heat(label, fn)
  print(label)
  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")
  for i = 1, 80 do fn(i) end
end

heat("buffer.method.reset", function(i)
  local b = buffer.new()
  b:put("abc", i)
  b:reset()
  assert(#b == 0)
end)

heat("buffer.method.skip", function()
  local b = buffer.new()
  b:set("abcdef")
  b:skip(2)
  assert(tostring(b) == "cdef")
end)

print("buffer.method.reset.traced")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
do
  local b = buffer.new()
  b:set("abcdef")
  local n = 0
  for _ = 1, 80 do
    if b:reset() == b then n = n + 1 end
  end
  assert(n == 80)
  assert(#b == 0)
end

print("buffer.method.skip.traced")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
do
  local b = buffer.new()
  b:set(string.rep("a", 96))
  local n = 0
  for _ = 1, 80 do
    if b:skip(1) == b then n = n + 1 end
  end
  assert(n == 80)
  assert(#b == 16)
end

heat("buffer.method.set", function(i)
  local _ = i
  local b = buffer.new()
  b:set("setter")
  assert(tostring(b) == "setter")
end)

print("buffer.method.set.traced")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
do
  local b = buffer.new()
  local vals = { "left", "right" }
  local n = 0
  for i = 1, 80 do
    local s = vals[(i % 2) + 1]
    if b:set(s) == b and tostring(b) == s then n = n + 1 end
  end
  assert(n == 80)
end

heat("buffer.method.put", function(i)
  local b = buffer.new()
  b:put("x", i)
  assert(tostring(b) == "x" .. i)
end)

print("buffer.method.put-string.traced")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
do
  local b = buffer.new()
  local n = 0
  for _ = 1, 80 do
    if b:put("a") == b then n = n + 1 end
  end
  assert(n == 80)
  assert(#b == 80)
end

heat("buffer.method.putf", function(i)
  local b = buffer.new()
  b:putf("%d:%s", i, "q")
  assert(tostring(b) == i .. ":q")
end)

heat("buffer.method.get", function()
  local b = buffer.new()
  b:set("abcdef")
  assert(b:get(3) == "abc")
  assert(tostring(b) == "def")
end)

print("buffer.method.get.traced")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
do
  local b = buffer.new()
  b:set(string.rep("a", 96))
  local n = 0
  for _ = 1, 80 do
    if b:get(1) == "a" then n = n + 1 end
  end
  assert(n == 80)
  assert(#b == 16)
end

heat("buffer.method.tostring", function(i)
  local _ = i
  local b = buffer.new()
  b:set("traced-buffer")
  local s = ""
  for _ = 1, 4 do s = tostring(b) end
  assert(s == "traced-buffer")
end)

heat("buffer.method.len", function()
  local b = buffer.new()
  b:set("abcd")
  local n = 0
  for _ = 1, 4 do n = n + #b end
  assert(n == 16)
end)

print("buffer.concat.traced")
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
do
  local b = buffer.new()
  b:set("main")
  local s = ""
  local n = 0
  for i = 1, 80 do
    s = "pre:" .. b .. ":" .. i
    if b .. ":post" == "main:post" then n = n + 1 end
  end
  assert(s == "pre:main:80")
  assert(n == 80)
  assert(util.traceinfo(1), "buffer concat loop did not trace")
end

heat("buffer.method.encode.decode", function(i)
  local b = buffer.new()
  b:encode({answer = i})
  assert(b:decode().answer == i)
end)

if ffi_ok then
  heat("buffer.method.putcdata", function()
    local b = buffer.new()
    local p = ffi.new("lj_m6_buf_u8[3]", 65, 66, 67)
    b:putcdata(p, 3)
    assert(tostring(b) == "ABC")
  end)

  heat("buffer.method.reserve.commit.ref", function()
    local b = buffer.new()
    local p, n = b:reserve(4)
    assert(n >= 4)
    p[0], p[1], p[2], p[3] = 76, 74, 33, 10
    b:commit(4)
    local r, len = b:ref()
    assert(len == 4)
    assert(string.char(r[0], r[1], r[2], r[3]) == "LJ!\n")
  end)
end

print("t-jit-buffer-method-shared-nyi OK")
]=])
      print("M6 JIT string.buffer method behavior passed")
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
