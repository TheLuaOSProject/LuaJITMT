local buffer = require("string.buffer")
local th = require("threading")

-- Release all workers together, and join every worker before reporting a
-- failure. Each worker has a fixed amount of work, including the readers.
local function parallel(work)
  local ready, start = th.channel(#work), th.channel(#work)
  local workers, results = {}, {}
  for i = 1, #work do
    workers[i] = th.spawn(function(run, ready, start)
      assert(ready:send(true, 30))
      local token, ok = start:recv(30)
      assert(token == true and ok == true, "worker start timed out")
      return run()
    end, work[i], ready, start)
  end
  local failure
  for _ = 1, #work do
    local token, ok = ready:recv(30)
    if token ~= true or ok ~= true then
      failure = "worker readiness timed out"
      break
    end
  end
  for _ = 1, #work do start:send(true) end
  for i = 1, #workers do
    local ok, result = workers[i]:join()
    if ok ~= true and not failure then
      failure = "worker " .. i .. ": " .. tostring(result)
    end
    results[i] = result
  end
  ready:close()
  start:close()
  assert(not failure, failure)
  return results
end

local record_size, per_worker = 64, 512
local function record(tag, id)
  return string.format("%s%06d:%s:%06d%s", tag, id, tag:rep(48), id, tag)
end
assert(#record("A", 1) == record_size)

do
  local shared = buffer.new()
  local function appender(tag)
    return function()
      for i = 1, per_worker do
        -- A complete record is one put argument: multi-argument put need not
        -- make all of its arguments one indivisible append.
        shared:put(record(tag, i))
        if i % 16 == 0 then th.sleep(0) end
        if i % 128 == 0 then collectgarbage("step", 32) end
      end
      return per_worker
    end
  end
  local function check_suffix(text, first, counts)
    assert(#text % record_size == 0, "snapshot contains a partial record")
    for pos = first, #text, record_size do
      local tag = text:sub(pos, pos)
      assert(counts[tag], "snapshot contains an unknown record")
      local id = counts[tag] + 1
      assert(id <= per_worker, "snapshot contains an extra record")
      assert(text:sub(pos, pos + record_size - 1) == record(tag, id),
             "snapshot contains a torn, duplicated, or reordered record")
      counts[tag] = id
    end
  end
  local results = parallel({appender("A"), appender("B"), function()
    local previous, counts = "", {A = 0, B = 0}
    for i = 1, per_worker * 2 do
      local text = tostring(shared)
      assert(#text >= #previous and text:sub(1, #previous) == previous,
             "an append changed an earlier snapshot prefix")
      check_suffix(text, #previous + 1, counts)
      previous = text
      if i % 8 == 0 then th.sleep(0) end
    end
    return true
  end})
  assert(results[1] == per_worker and results[2] == per_worker and results[3])
  local text, counts = tostring(shared), {A = 0, B = 0}
  check_suffix(text, 1, counts)
  assert(counts.A == per_worker and counts.B == per_worker, "lost append")
  assert(#shared == 2 * per_worker * record_size)
  assert(shared:get() == text and #shared == 0)
end

local function check_exactly_once(results, total)
  local seen, count = {}, 0
  for _, values in ipairs(results) do
    assert(#values == per_worker)
    for _, value in ipairs(values) do
      assert(type(value) == "number" and value >= 1 and value <= total and
             value % 1 == 0, "consumer returned an invalid record id")
      assert(not seen[value], "record consumed more than once: " .. value)
      seen[value], count = true, count + 1
    end
  end
  assert(count == total)
  for i = 1, total do assert(seen[i], "record was not consumed: " .. i) end
end

do
  local shared, records = buffer.new(), {}
  for i = 1, per_worker * 2 do records[i] = record("G", i) end
  shared:put(table.concat(records))
  local function consumer()
    local values, previous = {}, 0
    for i = 1, per_worker do
      local text = shared:get(record_size)
      assert(#text == record_size, "get returned a partial or missing record")
      local id = tonumber(text:sub(2, 7))
      assert(id and id > previous and text == record("G", id),
             "get returned a torn or reordered record")
      values[i], previous = id, id
      if i % 16 == 0 then th.sleep(0) end
    end
    return values
  end
  check_exactly_once(parallel({consumer, consumer}), per_worker * 2)
  assert(#shared == 0 and shared:get() == "")
end

do
  local shared, encoded = buffer.new(), {}
  for i = 1, per_worker * 2 do encoded[i] = buffer.encode(i) end
  shared:set(table.concat(encoded))
  local function consumer()
    local values, previous = {}, 0
    -- Fixed counts guarantee one object per call even if the other consumer
    -- runs ahead. A separate length check would race with consumption.
    for i = 1, per_worker do
      local value = shared:decode()
      assert(type(value) == "number" and value > previous,
             "decode returned a missing or reordered value")
      values[i], previous = value, value
      if i % 16 == 0 then th.sleep(0) end
    end
    return values
  end
  check_exactly_once(parallel({consumer, consumer}), per_worker * 2)
  assert(#shared == 0 and shared:get() == "")
end

local function fails(fn, pattern)
  local ok, err = pcall(fn)
  assert(not ok, "expected a buffer error")
  assert(tostring(err):match(pattern), tostring(err))
end

do
  local b, other = buffer.new(1), buffer.new():put("other\0")
  local printable = setmetatable({}, {__tostring = function() return "object" end})
  assert(#b == 0 and b:tostring() == "" and b:put() == b)
  assert(b:put("a", 12, printable, other) == b)
  assert(b:tostring() == "a12objectother\0" and tostring(other) == "other\0")
  assert(b:putf("/%04d/%s", 7, "end") == b)
  assert(b:get() == "a12objectother\0/0007/end")
  assert(b:get(0) == "" and b:get(10) == "" and b:get() == "")
  local source = "ab\0cdef"
  assert(b:put("discarded"):set(source) == b and #b == #source)
  local first, empty, rest = b:get(2, 0, nil)
  assert(first == "ab" and empty == "" and rest == "\0cdef" and #b == 0)
  assert(b:set(source):skip(3):put("tail"):get() == "cdeftail")
  assert(source == "ab\0cdef")
  assert(b:set(source):skip(100) == b and #b == 0)
  assert(b:put("again"):reset() == b and #b == 0)
  assert(b:put("again"):free() == b and #b == 0)
  assert(b:free():put("reused"):get() == "reused")
  fails(function() b:put(b) end, "itself")
  fails(function() b:skip(-1) end, "out of range")
  fails(function() b:get(-1) end, "out of range")
  fails(function() buffer.new(-1) end, "out of range")
  fails(function() b:put({}) end, "string/number/__tostring")
  fails(function() b:encode(function() end) end, "cannot serialize")
  local cycle = {}; cycle[1] = cycle
  fails(function() b:encode(cycle) end, "too deep")
  local encoded = buffer.encode({payload = "binary\0data"})
  fails(function() b:set(encoded:sub(1, -2)):decode() end, "end of buffer")
  fails(function() buffer.decode(encoded .. encoded) end, "left%-over")
  fails(function() b:reset():decode() end, "end of buffer")
  assert(b:reset():encode(false):encode(nil):encode(42):decode() == false)
  assert(b:decode() == nil and b:decode() == 42 and #b == 0)
end

do
  local mt = {__index = {kind = "point"}}
  local options = {
    dict = {"coordinate_x", "coordinate_y", "label", "child", "origin"},
    metatable = {{}, mt},
  }
  local enc, dec = buffer.new(1, options), buffer.new(options)
  options = nil
  collectgarbage()
  local original = setmetatable({coordinate_x = 7, coordinate_y = -3,
    label = "origin", child = setmetatable({coordinate_x = 11}, mt)}, mt)
  local function check(value)
    assert(value.coordinate_x == 7 and value.coordinate_y == -3)
    assert(value.label == "origin" and value.kind == "point")
    assert(getmetatable(value) == mt and getmetatable(value.child) == mt)
    assert(value.child.coordinate_x == 11 and value.child.kind == "point")
  end
  local encoded = enc:encode(original):get()
  assert(#encoded < #buffer.encode(original))
  check(dec:set(encoded):decode())
  assert(#dec == 0)
  -- Free/reset replace the contents but preserve serialization options.
  check(dec:reset():put(enc:free():encode(original):get()):decode())
  local unknown = setmetatable({answer = 42}, {})
  local plain = dec:set(enc:reset():encode(unknown):get()):decode()
  assert(plain.answer == 42 and getmetatable(plain) == nil)
  fails(function() buffer.new({dict = {17}}) end, "bad options")
  fails(function() buffer.new({metatable = {true}}) end, "bad options")
  local dropped = buffer.new({dict = {"known"}}):encode({known = true}):get()
  fails(function() buffer.new({dict = {false}}):set(dropped):decode() end,
        "dictionary index")
  local dropped_mt = buffer.new({metatable = {mt}}):encode(original):get()
  fails(function() buffer.new({metatable = {false}}):set(dropped_mt):decode() end,
        "dictionary index")
end

local has_ffi, ffi = pcall(require, "ffi")
if has_ffi then
  local b = buffer.new()
  local bytes = "ab\0cde"
  local source = ffi.new("uint8_t[?]", #bytes)
  ffi.copy(source, bytes, #bytes)
  assert(b:set(source, #bytes) == b)
  source[0] = string.byte("A")
  assert(tostring(b) == "Ab\0cde", "set(cdata) did not reference its source")
  b:skip(2)
  do
    local ptr, len = b:ref()
    assert(len == 4 and ffi.string(ptr, len) == "\0cde")
  end
  do
    local ptr, available = b:reserve(4)
    assert(available >= 4)
    ffi.copy(ptr, "tail", 4)
    assert(b:commit(4) == b)
  end
  source[2] = string.byte("X")
  assert(tostring(b) == "\0cdetail", "reserve did not detach the cdata source")
  assert(ffi.string(source, #bytes) == "AbXcde")
  b:reset():putcdata(source, #bytes)
  source[0] = string.byte("Z")
  assert(tostring(b) == "AbXcde", "putcdata did not copy its source")
  do
    local ptr, len = b:ref()
    assert(len == #bytes and ffi.string(ptr, len) == "AbXcde")
    ptr[1] = string.byte("B")
  end
  assert(b:get() == "ABXcde")
  do
    local ptr, available = b:reserve(3)
    assert(available >= 3)
    ffi.fill(ptr, available, string.byte("R"))
    b:commit(available)
    assert(b:get() == ("R"):rep(available))
  end
  local immutable = "str\0ing"
  b:set(immutable):skip(3)
  do
    local ptr, available = b:reserve(1)
    assert(available >= 1)
    ptr[0] = string.byte("!")
    b:commit(1)
  end
  assert(b:get() == "\0ing!" and immutable == "str\0ing")
  do
    local _, available = b:reserve(0)
    assert(available >= 0 and b:commit(0) == b and #b == 0)
  end
  do
    local _, available = b:reserve(1)
    fails(function() b:commit(available + 1) end, "out of range")
  end
  fails(function() b:reserve(-1) end, "out of range")
  fails(function() b:putcdata(source, -1) end, "out of range")
  fails(function() b:set(source, -1) end, "out of range")
  assert(b:reset():put("recovered"):get() == "recovered")

  local finalized = false
  do
    local owner = ffi.gc(ffi.new("uint8_t[?]", #bytes), function() finalized = true end)
    ffi.copy(owner, bytes, #bytes)
    b:set(owner, #bytes)
  end
  collectgarbage()
  collectgarbage()
  assert(not finalized, "set did not keep the cdata owner alive")
  assert(b:skip(2):get() == bytes:sub(3))
  b:free()
end

-- Opt in to multi-megabyte backing storage without increasing every run's
-- memory footprint: LJ_BUFFER_LARGE=1 luajit tests/t-buffer-concurrent-public.lua
if os.getenv("LJ_BUFFER_LARGE") == "1" then
  local payload = ("large\0buffer!"):rep(174763)
  local b = buffer.new(#payload)
  b:put(payload):skip(#payload - 37):put(payload)
  collectgarbage()
  assert(b:get(37) == payload:sub(-37) and b:get() == payload)
  assert(b:free():set(payload):skip(#payload - 31):put("end"):get() ==
         payload:sub(-31) .. "end")
  assert(b:free():put("reused"):get() == "reused")
end

print("t-buffer-concurrent-public OK")
