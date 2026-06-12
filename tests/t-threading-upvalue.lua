local th = require"threading"

local function assert_join_true(t)
  local ok, v = t:join()
  assert(ok == true and v == true)
end

local function legacy_dump(fn)
  local d = string.dump(fn)
  assert(d:byte(1) == 0x1b and d:sub(2, 3) == "LJ")
  assert(d:byte(4) == 3)
  return d:sub(1, 3) .. string.char(2) .. d:sub(5)
end

do
  local function pre_latch()
    local x = 0
    local function inc()
      x = x + 1
      return x
    end
    local function get()
      return x
    end
    assert(inc() == 1)
    assert(inc() == 2)
    return get()
  end

  assert(pre_latch() == 2)
end

do
  local function first_spawn_preserves_source_v3_uv()
    local x = 5
    local function get()
      return x
    end
    assert(get() == 5)
    assert_join_true(th.spawn(function()
      return true
    end))
    x = 9
    return get(), x
  end

  local shared, parent = first_spawn_preserves_source_v3_uv()
  assert(shared == 9 and parent == 9)
end

do
  local function source_v3_post_latch()
    local x = 0
    local function inc()
      x = x + 1
      return x
    end
    local function get()
      return x
    end
    x = 7
    return get(), inc(), get(), x
  end

  local a, b, c, parent = source_v3_post_latch()
  assert(a == 7 and b == 8 and c == 8 and parent == 8)
end

do
  local legacy_post_latch = assert(loadstring(legacy_dump(function()
    local x = 0
    local function inc()
      x = x + 1
      return x
    end
    local function get()
      return x
    end
    x = 7
    return get(), inc(), get(), x
  end)))

  local a, b, c, parent = legacy_post_latch()
  assert(a == 0 and b == 1 and c == 0 and parent == 7)
end

do
  local function upper_level_capture()
    local x = 3
    local function outer()
      local function inner()
        return x
      end
      return inner
    end
    x = 4
    return outer()()
  end

  assert(upper_level_capture() == 4)
end

do
  local legacy_upper_level_capture = assert(loadstring(legacy_dump(function()
    local x = 3
    local function outer()
      local function inner()
        return x
      end
      return inner
    end
    x = 4
    return outer()()
  end)))

  assert(legacy_upper_level_capture() == 3)
end

print("t-threading-upvalue OK")
