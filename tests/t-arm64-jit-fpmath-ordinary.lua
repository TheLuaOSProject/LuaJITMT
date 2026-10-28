-- Ordinary Lua companion: compare JIT-enabled results with JIT-off oracles.
local floor, ceil, sqrt, abs = math.floor, math.ceil, math.sqrt, math.abs
local min, max = math.min, math.max
local inf, nan = 1/0, 0/0
local cases = {
  {'floor', floor, {-2.75, -0.25, -0.0, 0.0, 0.25, 2.75,
                    -2147483649.25, -2147483648.0, 2147483647.75,
                    2147483648.25, 9007199254740994, -inf, inf, nan}},
  {'ceil', ceil, {-2.75, -0.25, -0.0, 0.0, 0.25, 2.75,
                  -2147483649.25, -2147483648.0, 2147483647.75,
                  2147483648.25, 9007199254740994, -inf, inf, nan}},
  {'sqrt', sqrt, {-2.75, -0.25, -0.0, 0.0, 0.25, 2.75,
                  -2147483649.25, -2147483648.0, 2147483647.75,
                  2147483648.25, 9007199254740994, -inf, inf, nan}}
}
for _, name in ipairs({'log','log10','exp','sin','cos','tan','asin','acos',
                       'atan','sinh','cosh','tanh'}) do
  cases[#cases+1] = {name, math[name], {-inf,-1e300,-2.75,-1,-0.0,0.0,
                                      1e-300,.25,1,2.75,1e300,inf,nan}}
end
local function same(a, b)
  if a ~= a then return b ~= b end
  if b ~= b or a ~= b then return false end
  if a == 0 then return 1/a == 1/b end
  return true
end
for _, case in ipairs(cases) do
  local name, fn, starts = case[1], case[2], case[3]
  local function workload(f, n, x)
    local s = 0
    for i = 1, n do s = s + f(x + i) end
    return s
  end
  local function scalar(f, n, x)
    local i, y = 0, 0
    while i < n do y = f(x); i = i + 1 end
    return y
  end
  for _, x in ipairs(starts) do
    jit.off(workload)
    local expected = workload(fn, 8, x)
    jit.on(workload)
    jit.flush()
    jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=1')
    local actual = workload(fn, 8, x)
    assert(same(actual, expected), name .. ' mismatch at ' .. x)
  end
  for _, x in ipairs(starts) do
    jit.off(scalar)
    local expected = scalar(fn, 8, x)
    jit.on(scalar)
    jit.flush()
    jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=1')
    local actual = scalar(fn, 8, x)
    assert(same(actual, expected), name .. ' scalar edge mismatch at ' .. x)
  end
  print('ORDINARY', name, 'interpreter/JIT results match')
end
local function unary(f, x)
  local i, y = 0, 0
  while i < 8 do y = f(x); i = i + 1 end
  return y
end
for _, x in ipairs({-0.0, 0.0, -2.75, 2.75, -2147483648.25,
                     2147483648.25, -inf, inf, nan}) do
  jit.off(unary); local expected = unary(abs, x); jit.on(unary)
  jit.flush(); jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=1')
  assert(same(unary(abs, x), expected), 'abs mismatch at ' .. x)
end
local function ternary(f, a, b, c)
  local i, y = 0, 0
  while i < 8 do y = f(a, b, c); i = i + 1 end
  return y
end
for _, op in ipairs({{'min', min}, {'max', max}}) do
  local rows = {
    {1, 2, 3}, {nan, 1, 2}, {1, nan, 2}, {1, 2, nan},
    {-0.0, 0.0, 1}, {0.0, -0.0, 1}, {-inf, inf, 0},
    {2147483648.25, -2147483648.25, 9007199254740994}
  }
  if op[1] == 'max' then
    rows[#rows+1] = {-0.0, 0.0, -1}
    rows[#rows+1] = {0.0, -0.0, -1}
  end
  for _, row in ipairs(rows) do
    local name, fn, a, b, c = op[1], op[2], row[1], row[2], row[3]
    jit.off(ternary); local expected = ternary(fn, a, b, c); jit.on(ternary)
    jit.flush(); jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=1')
    local actual = ternary(fn, a, b, c)
    assert(same(actual, expected), name .. ' mismatch')
    if actual == 0 then assert(1/actual == 1/expected, name .. ' signed zero mismatch') end
    if name == 'max' and c == -1 then
      assert(actual == 0, 'max signed-zero result mismatch')
      assert(1/actual == 1/expected, 'max signed-zero reciprocal mismatch')
    end
  end
end
print('ORDINARY', 'abs/min/max', 'interpreter/JIT results match')
