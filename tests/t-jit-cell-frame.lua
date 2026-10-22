-- Inlined frames must promote their own locals from current SSA values.
local sentinel_a, sentinel_b = {}, {}
local keep_a, keep_b = sentinel_a, sentinel_b

local function capture(depth)
  if depth > 0 then
    local child = capture(depth - 1)
    return function()
      if type(child) == "function" then return depth + child() end
      return depth + child
    end
  end
  return depth
end

local function evaluate(depth)
  return capture(depth)()
end

for i = 1, 200 do
  assert(evaluate(4) + evaluate(4) == 20)
  assert(sentinel_a == keep_a and sentinel_b == keep_b)
end

local function pair(seed)
  local value = seed
  local function increment() value = value + 1 end
  local function read() return value end
  return increment, read
end

for i = 1, 200 do
  local increment, read = pair(i)
  increment()
  assert(read() == i + 1)
  assert(debug.upvalueid(increment, 1) == debug.upvalueid(read, 1))
  assert(sentinel_a == keep_a and sentinel_b == keep_b)
end

if jit and jit.status() and jit.arch == "x64" then
  local util = require("jit.util")
  assert(util.traceinfo(1), "cell-frame fixture did not compile a native trace")
end
print("t-jit-cell-frame OK: recursive captures, fresh locals and caller sentinels")
