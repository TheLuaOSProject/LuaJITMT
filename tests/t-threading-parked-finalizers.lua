local th = require("threading")
local ffi = require("ffi")

local function check_after_join(weak, calls, id, wait)
  for _ = 1, 12 do
    collectgarbage()
    if calls[id] == 1 then break end
  end
  -- Weak values can survive the cycle that runs their finalizer.
  for _ = 1, 4 do collectgarbage() end
  assert(calls[id] == 1, wait .. " finalizer count after join")
  assert(weak[id] == nil, wait .. " retained finalized cdata after join")
end

local function exercise(wait)
  local weak = setmetatable({}, {__mode="v"})
  local calls = {0, 0}
  local ready, gate = th.channel(1), th.channel(1)
  local mutex, target
  if wait == "send" then
    assert(gate:send(true))
  elseif wait == "join" then
    target = th.spawn(function() assert(gate:recv()) end)
  elseif wait == "lock" then
    mutex = th.mutex()
    mutex:lock()
  end

  local function make(id)
    local value = ffi.gc(ffi.new("int32_t[1]", id), function(v)
      assert(v[0] == id)
      calls[id] = calls[id] + 1
    end)
    weak[id] = value
    return value
  end

  -- Leave the first cdata in storage from a series of returned frames.
  local function nested(n)
    if n == 0 then return make(1)[0] end
    return nested(n - 1) + n
  end
  local released = false
  local worker = th.spawn(function()
    local live = make(2)
    assert(nested(28) == 407)
    assert(ready:send(true))
    if wait == "recv" then
      assert(gate:recv())
    elseif wait == "send" then
      assert(gate:send(ffi.new("int32_t[1]", 37), 30))
    elseif wait == "join" then
      assert(target:join(30))
    elseif wait == "lock" then
      mutex:lock()
      mutex:unlock()
    else
      repeat th.sleep(0.01) until released
    end
    assert(live[0] == 2 and weak[2] == live and calls[2] == 0)
  end)

  assert(ready:recv(30))
  for _ = 1, 12 do collectgarbage() end
  local collected = calls[1] == 1 and weak[1] == nil
  local retained = calls[2] == 0
  assert(worker:running())
  if wait == "send" then
    assert(gate:recv() == true and gate:recv()[0] == 37)
  elseif wait == "lock" then
    mutex:unlock()
  elseif wait == "sleep" then
    released = true
  else
    assert(gate:send(true))
  end
  assert(worker:join(30))
  assert(collected, wait .. " kept cdata from a returned frame")
  assert(retained, wait .. " lost a live caller slot")
  check_after_join(weak, calls, 2, wait)
  assert(calls[1] == 1)
end

ffi.cdef[[
  void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
]]
local function nested_callback()
  local ready, gate = th.channel(1), th.channel(1)
  local weak = setmetatable({}, {__mode="v"})
  local calls = {0}
  local function argument()
    local a = ffi.gc(ffi.new("int32_t[2]", 29, 11), function(v)
      assert(v[0] == 11 and v[1] == 29)
      calls[1] = calls[1] + 1
    end)
    weak[1] = a
    return a
  end
  local worker = th.spawn(function()
    local first = true
    local compare = ffi.cast("int (*)(const void *, const void *)", function(a, b)
      if first then
        first = false
        assert(ready:send(true) and gate:recv(30))
      end
      return ffi.cast("const int32_t *", a)[0] - ffi.cast("const int32_t *", b)[0]
    end)
    -- The comparator can still read both addresses after its channel wait.
    ffi.C.qsort(argument(), 2, ffi.sizeof("int32_t"), compare)
    compare:free()
  end)
  assert(ready:recv(30))
  for _ = 1, 12 do collectgarbage() end
  assert(calls[1] == 0 and weak[1] ~= nil)
  assert(gate:send(true) and worker:join(30))
  check_after_join(weak, calls, 1, "FFI callback")
end

for _, pool in ipairs({0, 2}) do
  th.gcworkers(pool)
  for _, wait in ipairs({"recv", "send", "join", "lock", "sleep"}) do
    exercise(wait)
  end
  nested_callback()
end
th.gcworkers(0)
print("t-threading-parked-finalizers OK")
