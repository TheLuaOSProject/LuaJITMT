-- Vector ctype representation, construction, indexing and immutability.
local T = require("simdtest")
local ffi, simd, test, check, checkeq = T.ffi, T.simd, T.test, T.check, T.checkeq

test("sizeof/alignof", function()
  for _, ti in ipairs(T.T) do
    checkeq(ffi.sizeof(ti.ct), 16, ti.name .. " sizeof")
    checkeq(ffi.alignof(ti.ct), 16, ti.name .. " alignof")
    checkeq(simd.lanes(ti.ct), ti.lanes, ti.name .. " lanes")
    check(simd.isvector(ti.ct), ti.name .. " isvector(ctype)")
    check(simd.isvector(ti.ct()), ti.name .. " isvector(cdata)")
  end
  check(not simd.isvector(ffi.new("int[4]")), "array is not a vector")
  check(not simd.isvector(1), "number is not a vector")
  check(not simd.isvector(ffi.typeof("int")), "int is not a vector")
end)

test("256-bit vector representation", function()
  for _, ti in ipairs(T.W) do
    checkeq(ffi.sizeof(ti.ct), 32, ti.name .. " sizeof")
    -- LuaJIT's cdata payload alignment is capped at 16; YMM memory operations
    -- are deliberately unaligned, so the native JIT does not require 32.
    checkeq(ffi.alignof(ti.ct), 16, ti.name .. " alignof")
    checkeq(simd.lanes(ti.ct), ti.lanes, ti.name .. " lanes")
    local v = ti.ct(3)
    for i = 0, ti.lanes-1 do
      checkeq(tonumber(v[i]), 3, ti.name .. " splat lane " .. i)
    end
  end
end)

test("construction", function()
  for _, ti in ipairs(T.T) do
    local z = ti.ct()
    for i = 0, ti.lanes-1 do checkeq(tonumber(z[i]), 0, ti.name .. " zero init") end
    local s = ti.ct(3)
    for i = 0, ti.lanes-1 do checkeq(tonumber(s[i]), 3, ti.name .. " splat") end
    local t = {}
    for i = 1, ti.lanes do t[i] = i end
    local v = T.vec(ti, t)
    for i = 0, ti.lanes-1 do checkeq(tonumber(v[i]), i+1, ti.name .. " init") end
    -- Copy construction keeps every bit.
    checkeq(ti.ct(v), v, ti.name .. " copy")
  end
end)

test("table initializer and arrays", function()
  local ti = T.T.i32x4
  local v = ffi.new(ti.ct, {1, 2, 3, 4})
  checkeq(tonumber(v[3]), 4, "table init")
  local arr = ffi.new(ffi.typeof("$[4]", ti.ct))
  arr[2] = v
  checkeq(arr[2], v, "store into vector array")
  checkeq(ffi.sizeof(arr), 64, "vector array size")
  -- Struct members.
  ffi.cdef[[ typedef struct { int32_t a; i32x4 v; } vstruct; ]]
  local s = ffi.new("vstruct")
  s.v = v
  checkeq(s.v, v, "struct member roundtrip")
  checkeq(ffi.offsetof("vstruct", "v"), 16, "struct member alignment")
end)

test("lane read", function()
  local v = T.T.float4.ct(1.5, -2.5, 3.5, -4.5)
  checkeq(v[0], 1.5, "lane 0")
  checkeq(v[3], -4.5, "lane 3")
  local u = T.T.u32x4.ct(0xffffffff, 1, 2, 3)
  checkeq(u[0], 4294967295, "unsigned lane")
  local w = T.T.i64x2.ct(-1, 2)
  checkeq(tostring(w[0]), "-1LL", "int64 lane is boxed")
end)

test("lanes are immutable", function()
  local v = T.T.i32x4.ct(1, 2, 3, 4)
  local ok, err = pcall(function() v[0] = 9 end)
  check(not ok, "lane assignment must fail")
  check(tostring(err):find("constant location", 1, true), "error message: " .. tostring(err))
  checkeq(tonumber(v[0]), 1, "value unchanged")
end)

test("pointer round trip", function()
  local ti = T.T.float4
  local buf = ffi.new("float[8]")
  for i = 0, 7 do buf[i] = i end
  local p = ffi.cast(ffi.typeof("$ *", ti.ct), buf)
  local v = p[0]
  checkeq(tonumber(v[2]), 2, "load from memory")
  p[1] = ti.ct(10, 11, 12, 13)
  checkeq(buf[5], 11, "store to memory")
end)

test("non-128-bit vectors still work", function()
  local f2 = ffi.typeof("float2")
  local a, b = f2(1, 2), f2(3, 4)
  checkeq(ffi.sizeof(f2), 8, "float2 size")
  local c = a + b
  checkeq(c[0], 4, "float2 add lane 0")
  checkeq(c[1], 6, "float2 add lane 1")
  local f8 = ffi.typeof("float8")
  checkeq(ffi.sizeof(f8), 32, "float8 size")
  local x = f8(1) + f8(2)
  for i = 0, 7 do checkeq(x[i], 3, "float8 add lane " .. i) end
end)

test("unsupported vector shapes are rejected cleanly", function()
  -- _Bool ignores vector_size in the C parser, so it stays a plain scalar.
  ffi.cdef[[ typedef _Bool boolvec __attribute__((vector_size(16))); ]]
  check(not simd.isvector(ffi.typeof("boolvec")), "bool vector is not SIMD")
  -- A single-lane vector has no packed meaning.
  ffi.cdef[[ typedef float float1 __attribute__((vector_size(4))); ]]
  local f1 = ffi.typeof("float1")
  check(not simd.isvector(f1), "1-lane vector is not SIMD")
  check(not pcall(function() return f1() + f1() end), "1-lane arithmetic fails")
  check(not pcall(simd.min, f1(), f1()), "1-lane simd.min fails")
end)

return T
