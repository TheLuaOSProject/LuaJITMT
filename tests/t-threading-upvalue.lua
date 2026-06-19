local th = require"threading"

local function assert_join_true(t)
  local ok, v = t:join()
  assert(ok == true and v == true)
end

local function make_closed_gc_cell(tag)
  local value = { tag = tag, seq = 0 }
  local function store(v)
    value = v
    return value
  end
  local function load()
    return value
  end
  return store, load
end

local function exercise_closed_gc_cell(n, label)
  local store, load = make_closed_gc_cell(label)
  local last
  for i = 1, n do
    local obj = { seq = i, payload = { label, i } }
    last = store(obj)
    assert(load() == obj)
    if i % 9 == 0 then collectgarbage("collect") end
    assert(load().payload[1] == label and load().payload[2] == i)
  end
  collectgarbage("collect")
  assert(load() == last)
  assert(last.payload[1] == label and last.payload[2] == n)
  return store, load, last
end

local function exercise_jit_closed_gc_cell()
  if not jit then return end
  local util = require"jit.util"
  local store, load = make_closed_gc_cell("jit")
  local function hot(n)
    local last
    for i = 1, n do
      last = { seq = i, payload = { "jit", i } }
      store(last)
      local got = load()
      if got.payload[1] ~= "jit" or got.payload[2] ~= i then
        error("closed GC upvalue store lost JIT value")
      end
    end
    return last
  end

  jit.on()
  jit.flush()
  jit.opt.start("hotloop=1", "hotexit=1")
  local last = hot(160)
  assert(util.traceinfo(1), "expected closed GC upvalue store trace")
  collectgarbage("collect")
  assert(load() == last and load().payload[2] == 160)
  local later = hot(24)
  collectgarbage("collect")
  assert(load() == later and load().payload[2] == 24)
  jit.off()
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

do
  local function immutable_function_capture_spawn()
    local function compute(a, b)
      return a + b
    end
    local workers = {}
    for i = 1, 3 do
      assert(type(compute) == "function")
      assert(compute(i, 10) == i + 10)
      workers[i] = th.spawn(function(worker)
	return worker, compute(worker, 20)
      end, i)
    end
    for i = 1, 3 do
      local ok, worker, result = workers[i]:join()
      assert(ok == true)
      assert(worker == i)
      assert(result == i + 20)
    end
    assert(type(compute) == "function")
    return true
  end

  assert(immutable_function_capture_spawn())
end

do
  local function mutable_pre_capture_spawn()
    local x = 0
    for i = 1, 3 do
      assert(type(x) == "number")
      assert(x == i - 1)
      local t = th.spawn(function()
	x = x + 1
	return x
      end)
      local ok, v = t:join()
      assert(ok == true)
      assert(v == i)
      assert(x == i)
    end
    return x
  end

  assert(mutable_pre_capture_spawn() == 3)
end

do
  if jit then jit.off() end
  exercise_closed_gc_cell(60, "interp")
end

do
  local store, load = make_closed_gc_cell("thread")
  local parent = { seq = 1, payload = { "parent" } }
  store(parent)
  local worker = th.spawn(function()
    collectgarbage("collect")
    local before = load()
    assert(before.seq == 1 and before.payload[1] == "parent")
    local replacement = { seq = 2, payload = { before.payload[1], "worker" } }
    store(replacement)
    collectgarbage("collect")
    local after = load()
    return after.seq, after.payload[1], after.payload[2]
  end)
  local ok, seq, first, second = worker:join()
  assert(ok == true and seq == 2 and first == "parent" and second == "worker")
  collectgarbage("collect")
  assert(load().seq == 2 and load().payload[2] == "worker")
end

do
  exercise_jit_closed_gc_cell()
end

print("t-threading-upvalue OK")
