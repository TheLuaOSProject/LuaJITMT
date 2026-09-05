-- Cdata base-table methods are mutable even before the first MT activation.
-- A native loop must guard the current method, including a table-vector move.
local ffi = require("ffi")
local util = require("jit.util")
local ct = ffi.typeof("struct { int x; int y; }")
local basemt = debug.getmetatable(ct)
local oldcall, oldindex, oldnewindex = basemt.__call, basemt.__index, basemt.__newindex
local n, calls = 80, 0
local exits = {}
local resized = false

local function run(count)
  local sum = 0
  -- The first iteration deliberately skips cdata dispatch. The next FORL can
  -- enter the old native loop before a missing/nonfunction method is used.
  for i = 0, count do
    if i > 0 then
      local obj = ct()
      obj.x = i
      obj.y = i + 1
      sum = sum + obj.x + obj.y
    end
  end
  return sum
end

local function replacement_call(ctype, ...)
  calls = calls + 1
  return oldcall(ctype, ...)
end
local function replacement_index(obj, key)
  calls = calls + 1
  return oldindex(obj, key) + 1000
end
local function replacement_newindex(obj, key, value)
  calls = calls + 1
  return oldnewindex(obj, key, value + 1000)
end
local function make_unretained_call()
  return function(ctype, ...) return oldcall(ctype, ...) end
end
local function onexit(trace)
  exits[trace] = (exits[trace] or 0) + 1
end
local function traces()
  local ids = {}
  for i = 1, 200 do
    if util.traceinfo(i) then ids[#ids + 1] = i end
  end
  return ids
end
local function exitcount(ids)
  local count = 0
  for _, id in ipairs(ids) do count = count + (exits[id] or 0) end
  return count
end
local function cleanup()
  basemt.__call, basemt.__index, basemt.__newindex = oldcall, oldindex, oldnewindex
  if resized then
    for i = 1, 256 do basemt["guard_resize_" .. i] = nil end
    resized = false
  end
  debug.setmetatable(ct, basemt)
  jit.attach(onexit)
end
local function check(mode)
  cleanup()
  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1000")
  local weakmethod
  if mode == "methodlife" then
    basemt.__call = make_unretained_call()
    weakmethod = setmetatable({basemt.__call}, {__mode = "v"})
  end
  exits = {}
  jit.attach(onexit, "texit")
  assert(run(n) == n * (n + 2), "warm field semantics")
  local before = traces()
  local warm_exits = exitcount(before)
  if jit.status() then
    assert(#before > 0, "pre-mutation cdata loop must compile")
    assert(warm_exits > 0, "pre-mutation cdata loop must execute native code")
  end
  local expected_calls, expected_sum = n, n * (n + 2)
  local expects_error = false
  if mode == "call" then
    basemt.__call = replacement_call
  elseif mode == "index" then
    basemt.__index = replacement_index
    expected_calls, expected_sum = 2 * n, expected_sum + 2000 * n
  elseif mode == "newindex" then
    basemt.__newindex = replacement_newindex
    expected_calls, expected_sum = 2 * n, expected_sum + 2000 * n
  elseif mode == "missing" then
    basemt.__call = nil
    expects_error, expected_calls = true, 0
  elseif mode == "nonfunction" then
    basemt.__call = 42
    expects_error, expected_calls = true, 0
  elseif mode == "resize" then
    resized = true
    for i = 1, 256 do basemt["guard_resize_" .. i] = i end
    basemt.__call = replacement_call
    collectgarbage("collect")
  elseif mode == "methodlife" then
    basemt.__call = replacement_call
    collectgarbage("collect")
    if jit.status() then
      assert(weakmethod[1] ~= nil, "trace must retain the recorded method after replacement")
    end
  elseif mode == "replace" then
    local replacement = {}
    for key, value in pairs(basemt) do replacement[key] = value end
    replacement.__call = replacement_call
    debug.setmetatable(ct, replacement)
  else
    error("unknown mode: " .. tostring(mode))
  end
  local after_mutation = traces()
  if jit.status() then
    if mode == "replace" then
      assert(#after_mutation == 0, "base-root replacement must flush old traces")
    else
      assert(#after_mutation > 0, "in-place mutation must leave native guard to check")
    end
  end
  calls, exits = 0, {}
  local ok, result = pcall(run, n)
  local after_run = traces()
  local native_exits = exitcount(after_run)
  print("cdata-base-guard", mode, "warm-exits", warm_exits,
        "after-exits", native_exits, "calls", calls, "expected", expected_calls)
  if expects_error then
    assert(not ok, "native code skipped the current absent/nonfunction __call")
  else
    assert(ok, result)
    assert(result == expected_sum, "post-mutation cdata field semantics")
  end
  assert(calls == expected_calls, "native code skipped the current cdata base-table method")
  if jit.status() then
    assert(native_exits > 0, "mutation must be followed by actual native execution")
  end
end
for _, f in ipairs({onexit, traces, exitcount, cleanup, check}) do jit.off(f, true) end

local function main()
  local modes = arg[1] and {arg[1]} or
    {"call", "index", "newindex", "missing", "nonfunction", "resize", "methodlife", "replace"}
  for _, mode in ipairs(modes) do check(mode) end
end
jit.off(main, true)
local ok, err = pcall(main)
cleanup()
assert(ok, err)
print("cdata base-table native guards passed")
