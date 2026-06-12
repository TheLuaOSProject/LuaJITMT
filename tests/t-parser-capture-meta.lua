local function eq(a, b)
  assert(a == b, tostring(a) .. " ~= " .. tostring(b))
end

do
  local x = 1
  local f = function()
    return x
  end
  x = 2
  eq(f(), 2)
end

do
  local x = 0
  local function f()
    x = x + 1
    return x
  end
  eq(f(), 1)
  eq(f(), 2)
end

do
  local function outer()
    local x = 7
    return function()
      return function()
	return x
      end
    end
  end
  eq(outer()()(), 7)
end

do
  local function f()
    return f
  end
  eq(f(), f)
end

do
  local function compute(a, b)
    return a + b
  end
  local fs = {}
  for i = 1, 3 do
    eq(type(compute), "function")
    eq(compute(i, 10), i + 10)
    fs[i] = function(a, b)
      return compute(a, b)
    end
    eq(fs[i](i, 20), i + 20)
  end
end

do
  local x = 0
  local saved = {}
  for i = 1, 3 do
    eq(type(x), "number")
    eq(x, i - 1)
    local y = x + 1
    saved[i] = function()
      x = x + 1
      return x
    end
    eq(saved[i](), i)
    eq(y, i)
  end
  eq(saved[1](), 4)
  eq(saved[2](), 5)
  eq(saved[3](), 6)
end

do
  local x = 0
  if false then
    local function f()
      return x
    end
    eq(f(), nil)
  end
  x = 2
  eq(x, 2)
end

do
  local M = { n = 0 }
  function M.inc()
    M.n = M.n + 1
  end
  M.inc()
  eq(M.n, 1)
end

do
  local fs = {}
  for i = 1, 3 do
    local x = i
    fs[i] = function()
      return x
    end
  end
  eq(fs[1](), 1)
  eq(fs[2](), 2)
  eq(fs[3](), 3)
end

do
  local function outer()
    local x = 1
    return function()
      x = x + 1
      return x
    end
  end
  local dumped = string.dump(outer)
  eq(dumped:byte(4), 4)
  local f = assert(loadstring(dumped))()
  eq(f(), 2)
  eq(f(), 3)
end

print("t-parser-capture-meta OK")
