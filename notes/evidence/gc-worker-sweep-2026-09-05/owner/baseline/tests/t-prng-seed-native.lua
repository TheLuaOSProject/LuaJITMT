local th = require"threading"

local function reseed_rounds(n)
  for i = 1, n do
    assert(math.randomseed() == nil)
    local x = math.random()
    local y = math.random(1, 32)
    assert(x >= 0 and x < 1)
    assert(y >= 1 and y <= 32)
  end
  return true
end

assert(reseed_rounds(12))

local workers = {}
for i = 1, 4 do
  workers[i] = th.spawn(function(n)
    return reseed_rounds(n)
  end, 8)
end

for i = 1, #workers do
  local ok, res = workers[i]:join()
  assert(ok == true)
  assert(res == true)
end
