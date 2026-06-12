local th = require"threading"

local function assert_join_true(t)
  local ok, v = t:join()
  assert(ok == true and v == true)
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
  local function first_spawn_preserves_source_v4_uv()
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

  local shared, parent = first_spawn_preserves_source_v4_uv()
  assert(shared == 9 and parent == 9)
end

do
  local function source_v4_post_latch()
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

  local a, b, c, parent = source_v4_post_latch()
  assert(a == 7 and b == 8 and c == 8 and parent == 8)
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

print("t-threading-upvalue OK")
