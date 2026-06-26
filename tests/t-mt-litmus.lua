local th = require"threading"
local harness = require"thread_harness"

local reps = harness.env_number("LJ_M4_LITMUS_REPS", 100)

for _ = 1, reps do
  do
    local x = {}
    local ch = th.channel(1)
    local t = th.spawn(function()
      local v, ok = ch:recv()
      assert(v == true and ok == true)
      assert(x.v == 42)
    end)
    x.v = 42
    ch:send(true)
    assert(({ t:join() })[1] == true)
  end

  do
    local t = th.spawn(function()
      local u = {}
      for i = 1, 128 do
        u[i] = i
      end
      return u
    end)
    local ok, u = t:join()
    assert(ok == true)
    for i = 1, 128 do
      assert(u[i] == i)
    end
  end

  do
    local f = {a = 0, b = 0}
    local t1 = th.spawn(function()
      f.a = 1
      th.fence()
      return f.b
    end)
    local t2 = th.spawn(function()
      f.b = 1
      th.fence()
      return f.a
    end)
    local ok1, ra = t1:join()
    local ok2, rb = t2:join()
    assert(ok1 == true and ok2 == true)
    assert(not (ra == 0 and rb == 0))
  end

  do
    local ch = th.channel(1)
    if _ == 1 then collectgarbage("collect") end
    local stats0 = _ == 1 and collectgarbage("stats") or nil
    local t = th.spawn(function(q)
      q:recv()
    end, ch)
    assert(collectgarbage("isrunning") == true)
    collectgarbage("collect")
    assert(collectgarbage("step") == false)
    if stats0 then
      local stats1 = collectgarbage("stats")
      assert(stats1.cycle_requests >= stats0.cycle_requests + 1)
    end
    assert(collectgarbage("isrunning") == true)
    ch:send(true)
    assert(({ t:join() })[1] == true)
    if stats0 then collectgarbage("collect") end
  end

  do
    local n = 128
    local ch = th.channel(16)
    local t = th.spawn(function(q, count)
      for i = 1, count do
        q:send(i)
      end
      q:close()
    end, ch, n)
    for i = 1, n do
      local v, ok = ch:recv()
      assert(ok == true and v == i)
    end
    local v, ok = ch:recv()
    assert(v == nil and ok == false)
    assert(({ t:join() })[1] == true)
  end

  do
    local n = 128
    local ch = th.channel(32)
    local p1 = th.spawn(function(q, count)
      for i = 1, count do
        q:send(1000000 + i)
      end
    end, ch, n)
    local p2 = th.spawn(function(q, count)
      for i = 1, count do
        q:send(2000000 + i)
      end
    end, ch, n)
    local last1, last2 = 0, 0
    for _ = 1, n * 2 do
      local v, ok = ch:recv()
      assert(ok == true)
      local tag = math.floor(v / 1000000)
      local i = v % 1000000
      if tag == 1 then
        assert(i == last1 + 1)
        last1 = i
      elseif tag == 2 then
        assert(i == last2 + 1)
        last2 = i
      else
        error("unexpected producer tag")
      end
    end
    assert(last1 == n and last2 == n)
    assert(({ p1:join() })[1] == true)
    assert(({ p2:join() })[1] == true)
  end

  do
    local m = th.mutex()
    local ch = th.channel(1)
    m:lock()
    local t = th.spawn(function(mu, q)
      mu:lock()
      q:send(true)
      mu:unlock()
    end, m, ch)
    th.sleep(0.001)
    local v, ok = ch:peek()
    assert(v == nil and ok == false)
    m:unlock()
    v, ok = ch:recv()
    assert(v == true and ok == true)
    assert(({ t:join() })[1] == true)
  end
end

print(("t-mt-litmus OK: %d repetitions"):format(reps))
