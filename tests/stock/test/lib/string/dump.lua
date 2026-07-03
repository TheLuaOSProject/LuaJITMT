local loadstring = loadstring or load

do --- Must unpatch modified bytecode with ILOOP/JLOOP etc.
  local function foo()
    local t = {}
    for i=1,100 do t[i] = i end
    for a,b in ipairs(t) do end
    local m = 0
    while m < 100 do m = m + 1 end
  end

  local function check_dump(d)
    local f = assert(loadstring(d, ""))
    f()
    return f
  end

  local d1 = string.dump(foo)
  check_dump(d1)
  foo()
  check_dump(string.dump(foo))
  if jit then jit.off(foo) end
  foo()
  check_dump(string.dump(foo))
  local d2 = string.dump(loadstring(d1, ""), true)
  local f2 = check_dump(d2)
  local d3 = string.dump(f2, true)
  check_dump(d3)
  assert(loadstring(string.dump(assert(loadstring(d2, "")))))
end

do --- roundtrip constants
  local function f1() return -0x80000000 end
  local function f2() return 0.971234567 end
  assert(f1() == -0x80000000)
  assert(loadstring(string.dump(f1), "")() == -0x80000000)
  assert(f2() == 0.971234567)
  assert(loadstring(string.dump(f2), "")() == 0.971234567)
end
