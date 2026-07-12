local buffer = require"string.buffer"
local th = require"threading"

local b = buffer.new()
b:put("main")
assert("pre:" .. b == "pre:main")
assert(b .. ":post" == "main:post")

local reader = th.spawn(function(shared)
  assert(#shared == 4)
  assert(tostring(shared) == "main")
  assert("pre:" .. shared == "pre:main")
  assert(shared .. ":post" == "main:post")
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

-- One writer may replace/compact/free backing storage while another state is
-- concatenating the same buffer. Racy contents are intentionally unspecified,
-- but every snapshot must remain memory-safe, bounded, and naturally usable as
-- a string operand.
local raced = buffer.new()
raced:put("seed")
local race_writer = th.spawn(function(shared)
  for i = 1, 4000 do
    shared:reset()
    if i % 3 == 0 then
      shared:put(string.rep("a", 257 + (i % 97)))
    elseif i % 3 == 1 then
      shared:put("b", i)
    else
      shared:put(string.rep("c", 8193 + (i % 31)))
    end
    if i % 127 == 0 then collectgarbage("step", 64) end
  end
  return true
end, raced)
local race_reader = th.spawn(function(shared)
  for i = 1, 8000 do
    local left = "<" .. shared
    local right = shared .. ">"
    assert(type(left) == "string" and type(right) == "string")
    assert(#left <= 9000 and #right <= 9000)
    if i % 131 == 0 then collectgarbage("step", 64) end
  end
  return true
end, raced)
local rwok, rwdone = race_writer:join()
local rrok, rrdone = race_reader:join()
assert(rwok == true and rwdone == true)
assert(rrok == true and rrdone == true)

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
