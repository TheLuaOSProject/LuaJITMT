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
assert("pre:" .. b .. ":post" == "pre:worker17:post")

-- The last writer's Lua state may have been collected before the next read.
local function handed_off()
  local thread = th.spawn(function()
    local owned = require("string.buffer").new():put("012345"):skip(2)
    local cow = require("string.buffer").new():set("ab\0cd"):skip(1)
    return owned, cow
  end)
  local joined, owned, cow = thread:join()
  assert(joined, owned)
  return owned, cow
end
local owned, cow = handed_off()
collectgarbage()
assert(tostring(owned) == "2345" and tostring(cow) == "b\0cd")
assert(buffer.new():put(owned, cow):get() == "2345b\0cd")
assert(string.format("[%6.3s][%q]", owned, cow) ==
       string.format("[%6.3s][%q]", "2345", "b\0cd"))
local file = assert(io.tmpfile())
file:write(owned, cow, buffer.new())
file:seek("set")
assert(file:read("*a") == "2345b\0cd")
file:close()

local function mixed_concat(first, second, n, convert)
  local out
  for i = 1, n do
    local prefix = "n=" .. i
    local middle = convert(first)
    out = prefix .. "/" .. middle .. "/" .. second .. "!"
  end
  return out
end
for i = 1, 20 do
  assert(mixed_concat(owned, cow, 90, owned.__tostring) == "n=90/2345/b\0cd!")
  assert(mixed_concat(cow, owned, 90, cow.__tostring) == "n=90/b\0cd/2345!")
end

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
    if i % 193 == 0 then shared:free() end
    if i % 127 == 0 then collectgarbage("step", 64) end
  end
  return true
end, raced)
local race_reader = th.spawn(function(shared)
  for i = 1, 8000 do
    local left = "<" .. shared
    local right = shared .. ">"
    local text = tostring(shared)
    assert(type(left) == "string" and type(right) == "string")
    assert(left:sub(1, 1) == "<" and right:sub(-1) == ">")
    assert(#left <= 9000 and #right <= 9000)
    assert(type(text) == "string" and #text <= 9000)
    if i % 131 == 0 then collectgarbage("step", 64) end
  end
  return true
end, raced)
local rwok, rwdone = race_writer:join()
local rrok, rrdone = race_reader:join()
assert(rwok == true and rwdone == true, rwdone)
assert(rrok == true and rrdone == true, rrdone)

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

if jit and jit.status() then
  local bit, util = require("bit"), require("jit.util")
  local names = require("jit.vmdef").bcnames
  local ready, done = th.channel(1), th.channel(1)
  local peer = th.spawn(function(ready, done)
    assert(ready:send(true, 10))
    return done:recv(10)
  end, ready, done)
  assert(ready:recv(10))
  jit.flush(mixed_concat)
  jit.opt.start("hotloop=1", "hotexit=100")
  local pc, seen
  for i = 1, util.funcinfo(mixed_concat).bytecodes - 1 do
    local op = bit.band(util.funcbc(mixed_concat, i), 255) * 6
    if names:sub(op+1, op+6) == "FORL  " then pc = i end
  end
  assert(pc)
  local function onexit(tr)
    local ins = util.funcbc(mixed_concat, pc)
    local op = bit.band(ins, 255) * 6
    if names:sub(op+1, op+6) == "JFORL " and bit.rshift(ins, 16) == tr then
      seen = true
    end
  end
  jit.off(onexit)
  jit.attach(onexit, "texit")
  for _ = 1, 128 do
    assert(mixed_concat(owned, cow, 90, owned.__tostring) == "n=90/2345/b\0cd!")
    if seen then break end
    collectgarbage("step", 256)
    th.sleep(0.001)
  end
  jit.attach(onexit)
  assert(done:send(true, 10))
  assert(peer:join())
  assert(seen, "no native exit from the buffer conversion and concat loop")
end

print("t-buffer-thread-safety OK")
