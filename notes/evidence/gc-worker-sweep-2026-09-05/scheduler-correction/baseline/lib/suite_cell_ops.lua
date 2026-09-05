local runtime = require("suite_runtime")
local probes = require("local_cell_probes")

local M = {}

local luajit = runtime.luajit
local luajit_code = runtime.luajit_code

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

function M.run_jit_closed_upvalue_store_behavior_checks(t)
  -- The helper-vs-direct lowering choice is an implementation invariant and is
  -- documented in notes/x64-upvalue-store-all-tvalue.md. This test owns only
  -- the observable contract: traced closed-upvalue stores publish complete
  -- TValue slots and preserve Lua-visible values for primitive and GC payloads.
  luajit_code(t, [=[
local util = require"jit.util"

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
assert(util.traceinfo(1), "nil closed-upvalue store should trace")

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
assert(util.traceinfo(1), "true closed-upvalue store should trace")

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local pool = {
  { tag = "even" },
  { tag = "odd" }
}
local function make()
  local x = pool[1]
  return function(n)
    for i = 1, n do x = pool[(i % 2) + 1] end
    return x
  end
end
local f = make()
assert(f(40) == pool[1])
assert(util.traceinfo(1), "GC-valued closed-upvalue store should trace")
]=])
end

function M.run_jit_runtime_checks(t)
  luajit_code(t, probes.side_exit_cget_source_snapshot())
  luajit_code(t, probes.call_boundary_cget_source_snapshot())
  luajit_code(t, probes.side_exit_fnew_creation_snapshot())
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
