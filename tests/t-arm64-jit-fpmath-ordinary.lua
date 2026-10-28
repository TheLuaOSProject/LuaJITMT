-- Ordinary Lua companion: compare JIT-enabled results with JIT-off oracles.
local floor, ceil, sqrt = math.floor, math.ceil, math.sqrt
local cases = {
  {'floor', floor, {-2.75, -0.25, 0, 0.25, 2.75, 2147483648.25}},
  {'ceil', ceil, {-2.75, -0.25, 0, 0.25, 2.75, 2147483648.25}},
  {'sqrt', sqrt, {0, 0.25, 2.75, 2147483648.25}}
}
for _, case in ipairs(cases) do
  local name, fn, starts = case[1], case[2], case[3]
  local function workload(f, n, x)
    local s = 0
    for i = 1, n do s = s + f(x + i) end
    return s
  end
  for _, x in ipairs(starts) do
    jit.off(workload)
    local expected = workload(fn, 8, x)
    jit.on(workload)
    jit.flush()
    jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=1')
    local actual = workload(fn, 8, x)
    assert(actual == expected, name .. ' mismatch at ' .. x)
  end
  print('ORDINARY', name, 'interpreter/JIT results match')
end
