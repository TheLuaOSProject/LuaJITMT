local function assert_stdlib_roots()
  assert(type(print) == "function")
  assert(type(collectgarbage) == "function")
  assert(type(io) == "table")
  assert(io.stdout ~= nil)
  assert(type(string.format) == "function")
end

local function churn_closures(n)
  local s = 0
  for i = 1, n do
    local x = i
    local f = function()
      x = x + 1
      return x
    end
    s = s + f()
  end
  return s
end

assert_stdlib_roots()
collectgarbage("collect")
assert_stdlib_roots()

local sum = churn_closures(5000)
collectgarbage("collect")
assert_stdlib_roots()

assert(sum == 12507500)
print("t-gc-active-thread-roots OK")
