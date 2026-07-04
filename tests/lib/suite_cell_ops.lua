local runtime = require("suite_runtime")
local probes = require("local_cell_probes")
local utils = require("suite_utils")

local M = {}

local luajit = runtime.luajit
local luajit_code = runtime.luajit_code

local function plain_count(s, needle)
  local n, pos = 0, 1
  while true do
    local first, last = s:find(needle, pos, true)
    if not first then return n end
    n = n + 1
    pos = last + 1
  end
end

local function luajit_jdump(t, code)
  return utils.capture_command(
    "LUA_PATH=" .. utils.shell_quote(runtime.lua_path(t)) .. " " ..
    utils.shell_quote(t:path("src", "luajit")) .. " -jdump -e " ..
    utils.shell_quote(code),
    { timeout = "20s", stderr = true })
end

function M.run_publication_behavior_checks(t)
  luajit(t, { "-e", probes.dumped_closure_behavior() })
  luajit(t, { "-e", probes.debug_local_behavior() })
  luajit(t, { "-e", probes.owner_numeric({
    trace_assert = "expected traced CGET/CSET owner loop",
    second_run = true
  }) })
  luajit(t, { "-e", probes.owner_gc({
    trace_assert = "expected traced GC-valued CSET owner loop",
    second_run = true
  }) })
  luajit(t, { "-e", probes.owner_primitive({
    trace_assert = "expected traced primitive CSET owner loop",
    second_run = true
  }) })
  luajit(t, { "-e", probes.loaded_owner_numeric({
    trace_assert = "expected loaded owner CGET/CSET trace",
    second_run = true
  }) })
  luajit(t, { "-e", probes.child_numeric({
    trace_assert = "expected traced child numeric upvalue loop",
    second_run = true
  }) })
  luajit(t, { "-e", probes.child_gc({
    trace_assert = "expected traced child GC upvalue loop",
    second_run = true
  }) })
  luajit(t, { "-e", probes.loaded_child_numeric({
    trace_assert = "expected loaded child upvalue trace",
    second_run = true
  }) })
  luajit(t, { "-e", probes.loaded_cnew_fnew({
    trace_assert = "expected loaded CNEW creation trace"
  }) })
end

function M.run_jit_trace_behavior_checks(t)
  luajit_code(t, probes.owner_numeric({
    hotexit = true,
    trace_assert = "owner numeric upvalue loop should trace"
  }))
  luajit_code(t, probes.owner_primitive({
    hotexit = true,
    trace_assert = "owner primitive upvalue loop should trace"
  }))
  luajit_code(t, probes.owner_gc({
    hotexit = true,
    trace_assert = "owner GC-valued upvalue loop should trace"
  }))
  luajit_code(t, probes.loaded_owner_numeric({
    hotexit = true,
    trace_assert = "loaded owner upvalue loop should trace"
  }))
  luajit_code(t, probes.source_cnew_fnew({
    trace_assert = "parsed-chunk CNEW/FNEW creation should trace"
  }))
  luajit_code(t, probes.loaded_cnew_fnew({
    trace_assert = "loaded CNEW/FNEW creation should trace"
  }))
  luajit_code(t, probes.self_recursive_call({
    hotexit = true,
    trace_assert = "self-recursive immutable call should trace"
  }))
  luajit_code(t, probes.source_mixed_raw_local({
    hotexit = true,
    trace_assert = "parsed-chunk mixed raw-local CNEW/FNEW should trace"
  }))
  luajit_code(t, probes.loaded_mixed_raw_local({
    hotexit = true,
    trace_assert = "loaded mixed raw-local CNEW/FNEW should trace"
  }))
  luajit_code(t, probes.source_first_promotion({
    hotexit = true,
    trace_assert = "parsed-chunk first-promotion FNEW should trace"
  }))
  luajit_code(t, probes.loaded_first_promotion({
    hotexit = true,
    trace_assert = "loaded first-promotion FNEW should trace"
  }))
  luajit_code(t, probes.assigned_before_fnew({
    hotexit = true,
    trace_assert = "assigned-before-FNEW creation should trace"
  }))
  luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local util = require"jit.util"
local s = 0
for i = 1, 80 do
  local x = i
  local f = function()
    x = x + 1
    return x
  end
  s = s + f()
end
assert(s > 0)
assert(util.traceinfo(1), "same-trace numeric FNEW loop should trace")
]=])
end

function M.run_jit_closed_upvalue_store_shape_checks(t)
  local nil_store = luajit_jdump(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function make()
  local x = false
  return function(n)
    for i = 1, n do x = nil end
    return x
  end
end
local f = make()
assert(f(40) == nil)
]=])
  assert(nil_store:find("nil USTORE", 1, true),
         "nil closed-upvalue store did not record USTORE")
  assert(plain_count(nil_store, "lj_func_storeuv_forjit") == 0,
         "nil closed-upvalue store used lj_func_storeuv_forjit")

  local true_store = luajit_jdump(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function make()
  local x = false
  return function(n)
    for i = 1, n do x = true end
    return x
  end
end
local f = make()
assert(f(40) == true)
]=])
  assert(true_store:find("tru USTORE", 1, true),
         "true closed-upvalue store did not record USTORE")
  assert(plain_count(true_store, "lj_func_storeuv_forjit") == 0,
         "true closed-upvalue store used lj_func_storeuv_forjit")

  local gc_store = luajit_jdump(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local pool = { "even", "odd" }
local function make()
  local x = pool[1]
  return function(n)
    for i = 1, n do x = pool[(i % 2) + 1] end
    return x
  end
end
local f = make()
assert(f(40) == pool[1])
]=])
  assert(plain_count(gc_store, "lj_func_storeuv_forjit") > 0,
         "GC-valued closed-upvalue store skipped lj_func_storeuv_forjit")
  assert(plain_count(gc_store, "lj_gc_pubuv") > 0,
         "GC-valued closed-upvalue store skipped lj_gc_pubuv")
end

function M.run_jit_runtime_checks(t)
  luajit_code(t, probes.pre_fnew_update({
    hotexit = true,
    trace_assert = "pre-FNEW promoted local update should trace"
  }))
  luajit_code(t, probes.post_fnew_update({
    hotexit = true,
    trace_assert = "post-FNEW promoted local update should trace"
  }))
  luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local s = 0
local last
for i = 1, 80 do
  local x = i
  local function f()
    x = x + 1
    return x + x
  end
  s = s + f()
  last = f
end
assert(s == 6640, s)
assert(last() == 164, last())
]=])
  luajit_code(t, [=[
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
_G.__lc_target = nil
_G.__lc_reset = function()
  local name = debug.setupvalue(_G.__lc_target, 1, 100)
  assert(name == "x", name)
end
local s = 0
for i = 1, 80 do
  local x = i
  local function f()
    __lc_reset()
    x = x + 1
    return x
  end
  _G.__lc_target = f
  s = s + f()
end
_G.__lc_target = nil
_G.__lc_reset = nil
assert(s == 8080, s)
]=])
  luajit_code(t, [=[
local util = require"jit.util"
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local t = {}
for i = 1, 100 do
  local x = i
  t[i] = function()
    x = x + 1
    return x
  end
end
assert(util.traceinfo(1), "escaped numeric FNEW creation should trace")
assert(t[1]() == 2)
assert(t[2]() == 3)
assert(t[100]() == 101)
assert(t[1]() == 3)
assert(debug.upvalueid(t[1], 1) ~= debug.upvalueid(t[2], 1))
local name = debug.setupvalue(t[1], 1, 50)
assert(name == "x", name)
assert(t[1]() == 51)
assert(t[2]() == 4)
]=])
end

return M
