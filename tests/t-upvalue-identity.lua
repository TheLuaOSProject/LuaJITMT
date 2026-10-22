-- Stock LuaJIT upvalue identity, including source read-only captures.
local function check_owner()
  local value = 13
  local function a() return value end
  local function b() return value end
  assert(debug.upvalueid(a, 1) == debug.upvalueid(b, 1))
  assert(debug.setupvalue(a, 1, 29) == "value")
  assert(a() == 29 and b() == 29 and value == 29)
  local name, current = debug.getlocal(1, 1)
  assert(name == "value" and current == 29)
  assert(debug.setlocal(1, 1, 33) == "value")
  assert(a() == 33 and b() == 33 and value == 33)
  assert(debug.setupvalue(b, 1, 29) == "value")
  return a, b
end

local function check_multi()
  local number, object = 7, {marker = 11}
  local function a() return number, object end
  local function b() return number, object end
  assert(debug.upvalueid(a, 1) == debug.upvalueid(b, 1))
  assert(debug.upvalueid(a, 2) == debug.upvalueid(b, 2))
  local replacement = {marker = 37}
  assert(debug.setupvalue(a, 2, replacement) == "object")
  local n, t = b()
  assert(n == 7 and t == replacement and object == replacement)
  return a, b
end

local function check_scope()
  local functions = {}
  for i = 1, 32 do
    local value = i + 0.5
    functions[i] = {function() return value end, function() return value end}
  end
  for i = 1, 32 do
    local a, b = functions[i][1], functions[i][2]
    assert(debug.upvalueid(a, 1) == debug.upvalueid(b, 1))
    if i > 1 then
      assert(debug.upvalueid(a, 1) ~= debug.upvalueid(functions[i-1][1], 1))
    end
    assert(debug.setupvalue(a, 1, i + 100) == "value")
    assert(a() == i + 100 and b() == i + 100)
  end
end

local function check_join()
  local a, b = check_owner()
  local c, d = check_owner()
  assert(debug.upvalueid(a, 1) ~= debug.upvalueid(c, 1))
  debug.upvaluejoin(a, 1, c, 1)
  assert(debug.setupvalue(c, 1, 41) == "value")
  assert(a() == 41 and c() == 41 and d() == 41 and b() == 29)
end

local function run()
  for _ = 1, 120 do
    local a, b = check_owner()
    collectgarbage("step", 1)
    assert(debug.setupvalue(b, 1, 31) == "value")
    assert(a() == 31 and b() == 31)
    check_multi()
    check_scope()
    check_join()
  end
  local copy = assert(loadstring(string.dump(check_owner)))
  local a, b = copy()
  assert(debug.upvalueid(a, 1) == debug.upvalueid(b, 1))
end

if jit and jit.status() then jit.opt.start("hotloop=3", "hotexit=2") end
run()

local ok, threading = pcall(require, "threading")
if ok then
  local value = 17
  local a = function() return value end
  local b = function() return value end
  local worker = assert(threading.spawn(function(a, b)
    assert(debug.upvalueid(a, 1) == debug.upvalueid(b, 1))
    assert(debug.setupvalue(a, 1, 43) == "value")
    collectgarbage("collect")
    assert(b() == 43)
    return true
  end, a, b))
  local joined, result = worker:join(20)
  assert(joined and result == true)
  assert(value == 43 and a() == 43 and b() == 43)
  run()
end

print("t-upvalue-identity OK: shared read-only cells, owner writes, scope, joins and dump roundtrip")
