local th = require("threading")

local function expect_join_ok(thread, ...)
  local got = { thread:join() }
  assert(got[1] == true, tostring(got[2]))
  for i = 1, select("#", ...) do
    assert(got[i + 1] == select(i, ...),
      ("join result %d: got %s expected %s"):format(
        i, tostring(got[i + 1]), tostring(select(i, ...))))
  end
end

do
  local co = coroutine.wrap(function(a)
    local b = coroutine.yield(a + 1)
    return b * 2
  end)
  assert(co(1) == 2)
  assert(co(10) == 20)
end

do
  local co = coroutine.create(function()
    local from_a = coroutine.yield("first")
    local from_b = coroutine.yield("second:" .. from_a)
    return "done:" .. from_b
  end)
  local pipe = th.channel(0)
  local worker_a = th.spawn(function(c, q)
    local ok, v = coroutine.resume(c)
    assert(ok and v == "first", tostring(v))
    assert(q:send(c, 1) == true)
    return "yielded-on-worker-a"
  end, co, pipe)
  local worker_b = th.spawn(function(q)
    local c, ok = q:recv(1)
    assert(ok == true and type(c) == "thread")
    local ok1, v1 = coroutine.resume(c, "A")
    assert(ok1 and v1 == "second:A", tostring(v1))
    local ok2, v2 = coroutine.resume(c, "B")
    assert(ok2 and v2 == "done:B", tostring(v2))
    return "resumed-on-worker-b"
  end, pipe)
  expect_join_ok(worker_a, "yielded-on-worker-a")
  expect_join_ok(worker_b, "resumed-on-worker-b")
end

do
  local co = coroutine.create(function()
    local token = coroutine.yield("yielded-from-worker")
    return "main-resumed:" .. token
  end)
  local worker = th.spawn(function(c)
    local ok, v = coroutine.resume(c)
    assert(ok and v == "yielded-from-worker", tostring(v))
    return "worker-yielded"
  end, co)
  expect_join_ok(worker, "worker-yielded")
  local ok, v = coroutine.resume(co, "token")
  assert(ok and v == "main-resumed:token", tostring(v))
end

do
  local co = coroutine.create(function()
    local value = "before-debug-setlocal"
    coroutine.yield("debug-paused")
    return value
  end)
  local worker = th.spawn(function(c)
    local ok, v = coroutine.resume(c)
    assert(ok and v == "debug-paused", tostring(v))
    return "debug-yielded"
  end, co)
  expect_join_ok(worker, "debug-yielded")
  local name, value = debug.getlocal(co, 1, 1)
  assert(name == "value", tostring(name))
  assert(value == "before-debug-setlocal", tostring(value))
  local info = debug.getinfo(co, 1, "Sl")
  assert(type(info) == "table" and type(info.currentline) == "number")
  local tb = debug.traceback(co, "debug handoff traceback")
  assert(type(tb) == "string" and tb:match("debug handoff traceback"))
  assert(tb:match("yield"))
  assert(debug.setlocal(co, 1, 1, "after-debug-setlocal") == "value")
  local ok, v = coroutine.resume(co)
  assert(ok and v == "after-debug-setlocal", tostring(v))
end

do
  local co = coroutine.create(function(expected_id)
    local current_id = th.current():id()
    return current_id, expected_id
  end)
  local worker = th.spawn(function(c)
    local worker_id = th.current():id()
    local ok, current_id, expected_id = coroutine.resume(c, worker_id)
    assert(ok, tostring(current_id))
    assert(current_id == worker_id, tostring(current_id))
    assert(expected_id == worker_id, tostring(expected_id))
    return worker_id
  end, co)
  expect_join_ok(worker)
  assert(coroutine.status(co) == "dead")
end

do
  local main_id = th.current():id()
  local pipe = th.channel(0)
  local worker = th.spawn(function(q)
    local c = coroutine.create(function(expected_id)
      local current_id = th.current():id()
      local junk = {}
      for i = 1, 64 do junk[i] = ("migrate-%d"):format(i) end
      th.sleep(0)
      return current_id, expected_id, type(math.random())
    end)
    assert(q:send(c, 1) == true)
    return "worker-created-coroutine"
  end, pipe)
  local co, recv_ok = pipe:recv(1)
  assert(recv_ok == true and type(co) == "thread")
  expect_join_ok(worker, "worker-created-coroutine")
  collectgarbage("collect")
  collectgarbage("collect")
  local ok, current_id, expected_id, random_type = coroutine.resume(co, main_id)
  assert(ok, tostring(current_id))
  assert(current_id == main_id, tostring(current_id))
  assert(expected_id == main_id, tostring(expected_id))
  assert(random_type == "number", tostring(random_type))
  assert(coroutine.status(co) == "dead")
end

do
  local wrap = coroutine.wrap(function(a)
    local b = coroutine.yield("wrapped:" .. a)
    return "wrapped-done:" .. b
  end)
  local pipe = th.channel(0)
  local worker_a = th.spawn(function(w, q)
    local v = w("A")
    assert(v == "wrapped:A", tostring(v))
    assert(q:send(w, 1) == true)
    return "wrap-yielded"
  end, wrap, pipe)
  local worker_b = th.spawn(function(q)
    local w, ok = q:recv(1)
    assert(ok == true and type(w) == "function")
    local v = w("B")
    assert(v == "wrapped-done:B", tostring(v))
    return "wrap-resumed"
  end, pipe)
  expect_join_ok(worker_a, "wrap-yielded")
  expect_join_ok(worker_b, "wrap-resumed")
end

do
  local co = coroutine.create(function()
    coroutine.yield("error-ready")
    error("cross-thread-resume-error", 0)
  end)
  local worker = th.spawn(function(c)
    local ok, v = coroutine.resume(c)
    assert(ok and v == "error-ready", tostring(v))
    return "error-yielded"
  end, co)
  expect_join_ok(worker, "error-yielded")
  local ok, err = coroutine.resume(co)
  assert(ok == false and tostring(err):match("cross%-thread%-resume%-error"),
    tostring(err))
  assert(coroutine.status(co) == "dead")
end

do
  local wrap = coroutine.wrap(function()
    coroutine.yield("wrapped-error-ready")
    error("cross-thread-wrap-error", 0)
  end)
  local pipe = th.channel(0)
  local worker = th.spawn(function(w, q)
    assert(w() == "wrapped-error-ready")
    assert(q:send(w, 1) == true)
    return "wrap-error-yielded"
  end, wrap, pipe)
  local w, recv_ok = pipe:recv(1)
  assert(recv_ok == true and type(w) == "function")
  expect_join_ok(worker, "wrap-error-yielded")
  local ok, err = pcall(w)
  assert(ok == false and tostring(err):match("cross%-thread%-wrap%-error"),
    tostring(err))
end

do
  local co = coroutine.create(function()
    local ok1, v1 = pcall(coroutine.yield, "yielded-from-pcall")
    assert(ok1 and v1 == "resume-pcall", tostring(v1))
    local ok2, v2 = xpcall(function()
      return coroutine.yield("yielded-from-xpcall")
    end, debug.traceback)
    assert(ok2 and v2 == "resume-xpcall", tostring(v2))
    return "pcall-yield-done"
  end)
  local worker_a = th.spawn(function(c)
    local ok, v = coroutine.resume(c)
    assert(ok and v == "yielded-from-pcall", tostring(v))
    return "pcall-yielded"
  end, co)
  expect_join_ok(worker_a, "pcall-yielded")
  local ok1, v1 = coroutine.resume(co, "resume-pcall")
  assert(ok1 and v1 == "yielded-from-xpcall", tostring(v1))
  local worker_b = th.spawn(function(c)
    local ok, v = coroutine.resume(c, "resume-xpcall")
    assert(ok and v == "pcall-yield-done", tostring(v))
    return "xpcall-resumed"
  end, co)
  expect_join_ok(worker_b, "xpcall-resumed")
end

do
  for _ = 1, 20 do
    local gate = th.channel(0)
    local entered = th.channel(1)
    local out = th.channel(2)
    local co = coroutine.create(function()
      assert(entered:send(true, 1) == true)
      local token, ok = gate:recv()
      assert(ok == true and token == "go")
      return "winner-result"
    end)
    local function racer(id, c, result)
      local okcall, okresume, value = pcall(coroutine.resume, c)
      result:send({ id, okcall, okresume, value })
      return true
    end
    local t1 = th.spawn(racer, "a", co, out)
    local t2 = th.spawn(racer, "b", co, out)
    local entered_value, entered_ok = entered:recv(1)
    assert(entered_ok == true and entered_value == true)
    local first, first_ok = out:recv(1)
    assert(first_ok == true and first[2] == false)
    assert(tostring(first[3]):match("thread busy"))
    local status_ok, status_or_err = pcall(coroutine.status, co)
    assert(status_ok == false and tostring(status_or_err):match("thread busy"))
    assert(gate:send("go", 1) == true)
    local second, second_ok = out:recv(1)
    assert(second_ok == true and second[2] == true and second[3] == true)
    assert(second[4] == "winner-result", tostring(second[4]))
    assert(first[1] ~= second[1])
    expect_join_ok(t1, true)
    expect_join_ok(t2, true)
    assert(coroutine.status(co) == "dead")
  end
end

print("t-threading-coroutine OK: coroutine handoff and contention semantics")
