local M = {}

local function jit_setup(opts)
  opts = opts or {}
  local lines = {}
  if opts.trace_assert then
    lines[#lines + 1] = 'local util = require"jit.util"'
  end
  if opts.flush ~= false then
    lines[#lines + 1] = "jit.flush()"
  end
  if opts.hotexit then
    lines[#lines + 1] = 'jit.opt.start("hotloop=1", "hotexit=1")'
  else
    lines[#lines + 1] = 'jit.opt.start("hotloop=1")'
  end
  return table.concat(lines, "\n") .. "\n"
end

local function trace_assert(opts)
  if not (opts and opts.trace_assert) then return "" end
  return ("assert(util.traceinfo(1), %q)\n"):format(opts.trace_assert)
end

function M.parser_capture()
  return [=[
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
]=]
end

function M.self_capture()
  return [=[
local function f()
  return f
end
return f
]=]
end

function M.dumped_closure_behavior()
  return [=[
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
]=]
end

function M.owner_numeric(opts)
  opts = opts or {}
  local second = ""
  if opts.second_run then
    second = [=[
local v2, f2 = run(20)
assert(v2 == 20 and f2() == 20)
]=]
  end
  return jit_setup(opts) .. [=[
local function run(n)
  local x = 0
  local function touch() return x end
  for i = 1, n do x = x + 1 end
  return x, touch
end
local v, f = run(200)
assert(v == 200 and f() == 200)
]=] .. trace_assert(opts) .. second
end

function M.owner_gc(opts)
  opts = opts or {}
  local second = ""
  if opts.second_run then
    second = [=[
collectgarbage()
assert(run(20) == pool[1])
]=]
  end
  return jit_setup(opts) .. [=[
local pool = { "even", "odd" }
local function run(n)
  local x = pool[1]
  local function get() return x end
  for i = 1, n do x = pool[(i % 2) + 1] end
  return get()
end
assert(run(200) == pool[1])
]=] .. trace_assert(opts) .. second
end

function M.loaded_owner_numeric(opts)
  opts = opts or {}
  local second = ""
  if opts.second_run then
    second = [=[
local v2, f2 = run(20)
assert(v2 == 20 and f2() == 20)
]=]
  end
  return jit_setup(opts) .. [=[
local src = function(n)
  local x = 0
  local function touch() return x end
  for i = 1, n do x = x + 1 end
  return x, touch
end
local run = assert(loadstring(string.dump(src)))
local v, f = run(200)
assert(v == 200 and f() == 200)
]=] .. trace_assert(opts) .. second
end

function M.child_numeric(opts)
  opts = opts or {}
  local second = ""
  if opts.second_run then
    second = [=[
local v2, f2 = run(1000, 30)
assert(v2 == 1030 and f2() == 1031)
assert(f() == 202)
]=]
  end
  return jit_setup(opts) .. [=[
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
]=] .. trace_assert(opts) .. second
end

function M.child_gc(opts)
  opts = opts or {}
  local second = ""
  if opts.second_run then
    second = [=[
collectgarbage()
local v2, xv2, f2 = run(1000, 30)
local f2v, f2x = f2()
local fv2, fx2 = f()
assert(v2 == 1030 and xv2 == pool[1] and f2v == 1031 and f2x == pool[2])
assert(fv2 == 202 and fx2 == pool[1])
]=]
  end
  return jit_setup(opts) .. [=[
local pool = { "even", "odd" }
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
]=] .. trace_assert(opts) .. second
end

function M.loaded_child_numeric(opts)
  opts = opts or {}
  local second = ""
  if opts.second_run then
    second = [=[
local v2, f2 = run(1000, 30)
assert(v2 == 1030 and f2() == 1031)
assert(f() == 202)
]=]
  end
  return jit_setup(opts) .. [=[
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
]=] .. trace_assert(opts) .. second
end

function M.source_cnew_fnew(opts)
  opts = opts or {}
  return jit_setup(opts) .. [=[
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
]=] .. trace_assert(opts)
end

function M.loaded_cnew_fnew(opts)
  opts = opts or {}
  return jit_setup(opts) .. [=[
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
]=] .. trace_assert(opts)
end

function M.source_mixed_raw_local(opts)
  opts = opts or {}
  return jit_setup(opts) .. [=[
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
]=] .. trace_assert(opts)
end

function M.loaded_mixed_raw_local(opts)
  opts = opts or {}
  return jit_setup(opts) .. [=[
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
]=] .. trace_assert(opts)
end

function M.source_first_promotion(opts)
  opts = opts or {}
  return jit_setup(opts) .. [=[
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
]=] .. trace_assert(opts)
end

function M.loaded_first_promotion(opts)
  opts = opts or {}
  return jit_setup(opts) .. [=[
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
]=] .. trace_assert(opts)
end

function M.pre_fnew_update(opts)
  opts = opts or {}
  return jit_setup(opts) .. [=[
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
]=] .. trace_assert(opts)
end

function M.post_fnew_update(opts)
  opts = opts or {}
  return jit_setup(opts) .. [=[
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
]=] .. trace_assert(opts)
end

return M
