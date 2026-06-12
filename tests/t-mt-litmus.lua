local th = require"threading"

local reps = tonumber(os.getenv("LJ_M4_LITMUS_REPS") or "100")

for _ = 1, reps do
  do
    local x = 0
    local t = th.spawn(function()
      x = 99
    end)
    local ok = t:join()
    assert(ok == true)
    assert(x == 99)
  end

  do
    local y = 0
    local ch = th.channel(1)
    local t = th.spawn(function()
      y = 42
      th.fence()
      ch:send(true)
    end)
    local v, ok = ch:recv()
    assert(v == true and ok == true)
    assert(y == 42)
    assert(({ t:join() })[1] == true)
  end

  do
    local n = 64
    local ch = th.channel(8)
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
    local m = th.mutex()
    local x = 0
    m:lock()
    local t = th.spawn(function(mu)
      mu:lock()
      x = x + 1
      mu:unlock()
    end, m)
    th.sleep(0.001)
    assert(x == 0)
    m:unlock()
    assert(({ t:join() })[1] == true)
    assert(x == 1)
  end
end

print(("t-mt-litmus OK: %d repetitions"):format(reps))
