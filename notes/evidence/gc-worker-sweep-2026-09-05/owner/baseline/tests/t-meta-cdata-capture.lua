-- Cdata metamethod handoff through collection, reentry, and caught errors.
local ffi = require("ffi")

ffi.cdef[[
typedef struct { int n; } lj_meta_capture_small_t;
typedef struct __attribute__((aligned(256))) {
  int n;
} lj_meta_capture_aligned_t;
typedef struct { int n; char payload[20000]; } lj_meta_capture_huge_t;
typedef struct { int n; } lj_meta_capture_chain_t;
]]

local callback_calls = 0
local callback = ffi.cast("int (*)(int)", function(n)
  callback_calls = callback_calls + 1
  collectgarbage("step")
  return n + 9
end)
local saved
local methods = {
  __index = function(self, key)
    collectgarbage("collect")
    if key == "value" then return { n = self.n } end
    if key == "nested" then return self.value end
    if key == "callback" then return callback(self.n) end
    error("capture read error")
  end,
  __newindex = function(self, key, value)
    collectgarbage("collect")
    if key == "saved" then
      assert(self.n > 0)
      saved = value
      return
    end
    error("capture write error")
  end,
}

local objects = {}
for _, name in ipairs({ "lj_meta_capture_small_t",
                       "lj_meta_capture_aligned_t",
                       "lj_meta_capture_huge_t" }) do
  local ct = ffi.metatype(name, methods)
  local obj = ct(1)
  objects[#objects + 1] = obj
  for n = 1, 12 do
    obj.n = n
    local result = obj.nested
    assert(result.n == n)
    assert(obj.callback == n + 9)
    obj.saved = result
    assert(saved == result)
    obj.saved = obj
    assert(saved == obj)
    local ok, err = pcall(function() return obj.missing end)
    assert(not ok and err:find("capture read error", 1, true))
    ok, err = pcall(function() obj.missing = result end)
    assert(not ok and err:find("capture write error", 1, true))
  end
end
assert(callback_calls == 36)

-- Table-valued methods keep the existing later-hop chain behavior.
local answer = { alive = true }
local index = setmetatable({}, {
  __index = function(_, key)
    collectgarbage("collect")
    assert(key == "answer")
    return answer
  end,
})
local newindex = setmetatable({}, {
  __newindex = function(_, key, value)
    collectgarbage("collect")
    assert(key == "saved")
    saved = value
  end,
})
local ct = ffi.metatype("lj_meta_capture_chain_t", {
  __index = index, __newindex = newindex,
})
local chained = ct(7)
assert(chained.answer == answer)
chained.saved = chained.answer
assert(saved == answer and saved.alive)

-- The shared cdata base metatable and its methods remain replaceable. Restore
-- it even when a semantic assertion fails, before the callback is released.
local obj = objects[1]
local original = debug.getmetatable(obj)
local replacement = {}
for key, value in pairs(original) do replacement[key] = value end
replacement.__index = function(_, key)
  collectgarbage("collect")
  assert(key == "changed")
  return answer
end
replacement.__newindex = function(_, key, value)
  collectgarbage("collect")
  assert(key == "changed")
  saved = value
end
debug.setmetatable(obj, replacement)
local ok, err = pcall(function()
  assert(obj.changed == answer)
  obj.changed = obj
  assert(saved == obj)
  replacement.__index = function() return false end
  assert(obj.changed == false)
  replacement.__index = { changed = answer }
  assert(obj.changed == answer)
  replacement.__index = nil
  assert(not pcall(function() return obj.changed end))
  replacement.__newindex = false
  assert(not pcall(function() obj.changed = answer end))
end)
debug.setmetatable(obj, original)
callback:free()
assert(ok, err)
collectgarbage("collect")
assert(obj.value.n == 12)
print("cdata method collection, callbacks, replacement and errors passed")
