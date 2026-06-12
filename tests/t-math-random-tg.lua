local th = require"threading"

local function pair_for_seed(seed)
  math.randomseed(seed)
  return math.random(), math.random()
end

local e1, e2 = pair_for_seed(12345)

math.randomseed(12345)
local p1 = math.random()

local worker = th.spawn(function(seed)
  math.randomseed(seed)
  return math.random(), math.random()
end, 12345)

local ok, c1, c2 = worker:join()
assert(ok == true)
assert(c1 == e1 and c2 == e2)

local p2 = math.random()
assert(p1 == e1 and p2 == e2)

math.randomseed(777)
local parent_first = math.random()
local reseeder = th.spawn(function()
  math.randomseed(888)
  return math.random()
end)
assert(({ reseeder:join() })[1] == true)
math.randomseed(777)
assert(math.random() == parent_first)
