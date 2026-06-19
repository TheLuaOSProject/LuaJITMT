local buffer = require"string.buffer"
local th = require"threading"

local b = buffer.new()
b:put("main")

local reader = th.spawn(function(shared)
  assert(#shared == 4)
  assert(tostring(shared) == "main")
  assert("pre:" .. shared == "pre:main")
  assert(string.format("[%s]", shared) == "[main]")
  return #shared
end, b)

local ok, len = reader:join()
assert(ok == true and len == 4)

local writer = th.spawn(function(shared)
  shared:reset()
  shared:put("worker", 17)
  return tostring(shared), #shared
end, b)

local wok, text, wlen = writer:join()
assert(wok == true and text == "worker17" and wlen == 8)
assert(tostring(b) == "worker17")

local source = buffer.new()
source:put("return 21 * 2")
local loader = th.spawn(function(shared)
  local fn = assert(load(shared, "shared-buffer"))
  return fn()
end, source)
local lok, value = loader:join()
assert(lok == true and value == 42)

local encoded = buffer.encode({answer = 42})
local decoder = th.spawn(function(blob)
  local decoded = buffer.decode(blob)
  return decoded.answer
end, encoded)
local dok, answer = decoder:join()
assert(dok == true and answer == 42)

print("t-buffer-thread-safety OK")
