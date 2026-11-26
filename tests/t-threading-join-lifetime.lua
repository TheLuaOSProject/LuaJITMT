local th = require("threading")
local weak = setmetatable({}, {__mode = "v"})

local function join_results()
  local ready, go = th.channel(1), th.channel(1)
  local handle = th.spawn(function()
    local payload = {answer = 42}
    assert(ready:send(true))
    assert(go:recv(10))
    return function() return payload end, nil, th.current()
  end)
  assert(ready:recv(10))
  collectgarbage()
  assert(go:send(true))
  local ok, value, hole, self = handle:join(10)
  assert(ok and value().answer == 42 and hole == nil and self == handle)
  weak[1], weak[2], weak[3] = handle, value, value()
  value, self = nil, nil
  collectgarbage()
  local again, same, gap, owner = handle:join(10)
  assert(again and same == weak[2] and same() == weak[3])
  assert(gap == nil and owner == handle)
end

local function join_error()
  local handle = th.spawn(function() error({reason = "worker error"}) end)
  local ok, err = handle:join(10)
  assert(ok == false and err.reason == "worker error")
  weak[4], weak[5] = handle, err
end

join_results()
join_error()
-- A finalizable handle and its children can survive the finalization cycle.
for _ = 1, 3 do collectgarbage() end
for i = 1, 5 do assert(weak[i] == nil, "joined object retained at " .. i) end
print("t-threading-join-lifetime OK")
