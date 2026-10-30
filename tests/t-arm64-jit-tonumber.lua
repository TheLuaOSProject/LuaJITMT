-- Semantic companion for the numeric identity case of tonumber.
local to = tonumber

local function while_form(fn, value, n)
  local i, result = 0, value
  while i < n do result = fn(value); i = i + 1 end
  return result
end

local function for_form(fn, value, n)
  local result = value
  for i = 1, n do result = fn(value) end
  return result
end

local bit = require("bit")

for _, value in ipairs({ 123, 123.25, 0/0, -0.0, math.huge, -math.huge }) do
  local a, b = while_form(to, value, 40), for_form(to, value, 40)
  if value ~= value then
    assert(a ~= a and b ~= b)
  else
    assert(a == value and b == value)
    if value == 0 then assert(1/a == 1/value and 1/b == 1/value) end
  end
end

assert(while_form(to, "123.5", 40) == 123.5)
assert(for_form(to, "not a number", 40) == nil)
assert(while_form(to, true, 40) == nil)
assert(for_form(math.min, 123.25, 40) == 123.25)
assert(while_form(math.max, 123, 40) == 123)
assert(while_form(math.floor, 123, 40) == 123)
assert(for_form(math.ceil, 123, 40) == 123)
assert(while_form(bit.tobit, 123, 40) == 123)
assert(for_form(bit.band, 123, 40) == 123)
assert(while_form(bit.bor, 123, 40) == 123)
assert(for_form(bit.bxor, 123, 40) == 123)

local function with_base(fn, value, base, n)
  local result
  for i = 1, n do result = fn(value, base) end
  return result
end
assert(with_base(to, "7b", 16, 40) == 123)

collectgarbage("collect")
assert(while_form(to, 123.25, 40) == 123.25)
