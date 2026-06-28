local th = require"threading"
local harness = require"thread_harness"

local reps = harness.env_number("LJ_M5_TAB_RESIZE_STRESS_REPS", 768)
local writers = harness.env_number("LJ_M5_TAB_RESIZE_STRESS_THREADS", 3)
local jit_reps = harness.env_number("LJ_M5_TAB_RESIZE_STRESS_JIT_REPS", 2200)

local function assert_lua_value(v, label)
  local tv = type(v)
  assert(tv ~= "userdata", label .. " exposed an internal userdata sentinel")
  assert(tv ~= "cdata", label .. " exposed an internal cdata sentinel")
end

local function ready_start(n)
  return th.channel(n), th.channel(n)
end

local function collect_while_working(rounds)
  for i = 1, rounds do
    collectgarbage(i % 3 == 0 and "collect" or "step")
    if i % 8 == 0 then th.sleep(0.001) end
  end
end

local function weak_resize_writer(tbl, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    local k = id * 1000000 + i
    tbl[k] = { id, i }
    tbl[-k] = i
    if i > 4 and i % 5 == 0 then tbl[k - 4] = nil end
    if i % 64 == 0 then collectgarbage("step") end
  end
  return true
end

local function exercise_weak_clear_resize()
  local weak = setmetatable({}, { __mode = "v" })
  local ready, start = ready_start(writers)
  local workers = {}
  for i = 1, writers do
    workers[i] = th.spawn(weak_resize_writer, weak, ready, start, i, reps)
  end
  harness.wait_ready(ready, writers, 10, "weak resize")
  harness.release_start(start, writers, 10)
  collect_while_working(96)
  harness.join_all(workers, 30)
  harness.fullgc(2)
  for k, v in pairs(weak) do
    assert(type(k) == "number", "weak resize left non-number key")
    local tv = type(v)
    assert(tv == "number" or tv == "table",
	   "weak resize left unexpected value type " .. tv)
    assert_lua_value(v, "weak resize")
  end
end

local function strong_resize_writer(strong, observer, ready, start, id, n)
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    strong[id * 1000000 + i] = i
    if i % 32 == 0 then
      local slot = id * 100 + (i / 32)
      local obj = { owner = id, round = i }
      strong["keep:" .. slot] = obj
      observer[slot] = obj
    end
    if i > 128 and i % 7 == 0 then
      strong[id * 1000000 + i - 96] = nil
    end
    if i % 96 == 0 then collectgarbage("step") end
  end
  return true
end

local function exercise_gc_mark_resize()
  local strong = {}
  local observer = setmetatable({}, { __mode = "v" })
  local ready, start = ready_start(writers)
  local workers = {}
  for i = 1, writers do
    workers[i] = th.spawn(strong_resize_writer, strong, observer,
			  ready, start, i, reps)
  end
  harness.wait_ready(ready, writers, 10, "strong resize")
  harness.release_start(start, writers, 10)
  collect_while_working(128)
  harness.join_all(workers, 30)
  harness.fullgc(3)
  for id = 1, writers do
    for i = 32, reps, 32 do
      local slot = id * 100 + (i / 32)
      local obj = strong["keep:" .. slot]
      assert(type(obj) == "table", "strong resize lost table-owned object")
      assert(observer[slot] == obj,
	     "GC missed table-owned object during resize forwarding")
      assert_lua_value(obj, "strong resize")
    end
  end
end

local function jit_store_worker(tbl, ready, start, key, n)
  local okjit, jitmod = pcall(require, "jit")
  if okjit and jitmod.status() then
    jitmod.opt.start("hotloop=1", "hotexit=1")
  end
  assert(ready:send(true, 10) == true)
  local _, ok = start:recv(10)
  assert(ok == true)
  for i = 1, n do
    tbl[key] = i
    if i % 257 == 0 then tbl[key + 64] = nil end
  end
  return true
end

local function exercise_jit_store_resize()
  local t = {}
  for i = 1, 256 do t[i] = 0 end
  local nworkers = writers
  local ready, start = ready_start(nworkers)
  local workers = {}
  for i = 1, nworkers do
    workers[i] = th.spawn(jit_store_worker, t, ready, start, i, jit_reps)
  end
  harness.wait_ready(ready, nworkers, 10, "jit resize")
  harness.release_start(start, nworkers, 10)
  for i = 1, reps do
    local k = 10000 + i
    t[k] = i
    if i % 3 == 0 then t[k - 1] = nil end
    if i % 32 == 0 then collectgarbage("step") end
  end
  harness.join_all(workers, 30)
  for i = 1, nworkers do
    assert(t[i] == jit_reps, "traced store lost update across resize")
    assert_lua_value(t[i], "jit resize")
  end
  harness.fullgc(2)
end

exercise_weak_clear_resize()
exercise_gc_mark_resize()
exercise_jit_store_resize()

print(("t-tab-resize-stress OK: %d writers, %d resize rounds"):format(
  writers, reps))
