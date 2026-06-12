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
  eq(dumped:byte(4), 3)
  local f = assert(loadstring(dumped))()
  eq(f(), 2)
  eq(f(), 3)
end

print("t-parser-capture-meta OK")
