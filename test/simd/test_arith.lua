-- Ordinary Lua operators on vector cdata, checked against scalar references.
local T = require("simdtest")
local ffi, simd, test, check, checkeq = T.ffi, T.simd, T.test, T.check, T.checkeq

local SEED = tonumber(os.getenv("SIMD_SEED") or "20260725")

local ops = {
  { "add", function(a, b) return a + b end },
  { "sub", function(a, b) return a - b end },
  { "mul", function(a, b) return a * b end },
  { "div", function(a, b) return a / b end },
}

-- The order the operators run in decides the order their traces are recorded
-- and linked, which is worth varying. It must not vary *per process* though:
-- this used to be a pairs() loop over a string-keyed table, and with
-- LUAJIT_SECURITY_STRHASH the hash seed differs on every run, so the order
-- did too. A failure then could not be replayed from its seed. Derive the
-- permutation from a separate RNG stream instead, which leaves the operand
-- values for a given seed exactly as they were.
local function shuffled(rnd, out)
  for i = 1, #ops do out[i] = ops[i] end
  for i = #ops, 2, -1 do
    local j = rnd() % i + 1
    out[i], out[j] = out[j], out[i]
  end
  return out
end

test("randomized vector/vector arithmetic", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + ti.bits + (ti.fp and 1000 or 0) +
		      (ti.signed and 7 or 0))
    local ord = T.rng(SEED + 990001 + ti.bits + (ti.fp and 1000 or 0) +
		      (ti.signed and 7 or 0))
    local order = {}
    for iter = 1, 200 do
      local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
      for _, op in ipairs(shuffled(ord, order)) do
	local name, f = op[1], op[2]
	if name ~= "div" or ti.fp then
	  local got = f(a, b)
	  local want = T.refbin(ti, name, a, b)
	  checkeq(got, want, string.format("%s %s iter=%d a=%s b=%s",
		  ti.name, name, iter, T.tostr(a), T.tostr(b)))
	end
      end
      checkeq(-a, T.refneg(ti, a), ti.name .. " unm " .. T.tostr(a))
    end
  end
end)

test("scalar splat operands", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + 31 * ti.bits)
    for _ = 1, 50 do
      local a = T.rand(ti, rnd)
      local k = ti.fp and 2.5 or 3
      local sv = ti.ct(k)
      checkeq(a + k, T.refbin(ti, "add", a, sv), ti.name .. " v+k")
      checkeq(k + a, T.refbin(ti, "add", sv, a), ti.name .. " k+v")
      checkeq(a - k, T.refbin(ti, "sub", a, sv), ti.name .. " v-k")
      checkeq(k - a, T.refbin(ti, "sub", sv, a), ti.name .. " k-v")
      checkeq(a * k, T.refbin(ti, "mul", a, sv), ti.name .. " v*k")
      if ti.fp then
	checkeq(a / k, T.refbin(ti, "div", a, sv), ti.name .. " v/k")
	checkeq(k / a, T.refbin(ti, "div", sv, a), ti.name .. " k/v")
      end
    end
  end
end)

test("scalar conversion follows FFI rules", function()
  local i = T.T.i32x4
  checkeq(i.ct(1, 2, 3, 4) + 2.9, i.ct(3, 4, 5, 6), "double truncates to int")
  local u = T.T.u8x16
  checkeq(tonumber((u.ct(1) + 300)[0]), 45, "int wraps into uint8 lane")
  local f = T.T.float4
  checkeq((f.ct(0) + 1LL)[0], 1, "int64 cdata scalar")
end)

test("whole-vector equality", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + ti.bits)
    local a = T.randfinite(ti, rnd)
    check(a == ti.ct(a), ti.name .. " equal copies")
    check(not (a ~= ti.ct(a)), ti.name .. " ~= copies")
    check(not (a == a + ti.ct(1)), ti.name .. " unequal")
    if ti.fp then
      check(not (ti.ct(0/0) == ti.ct(0/0)), ti.name .. " NaN is never equal")
      check(ti.ct(0) == ti.ct(-0.0), ti.name .. " +0 == -0")
    end
  end
  -- Different vector types are never equal, and never raise.
  check(not (T.T.i32x4.ct(1) == T.T.float4.ct(1)), "cross-type equality")
  check(not (T.T.i32x4.ct(1) == nil), "vector vs nil")
  check(not (T.T.i32x4.ct(1) == "x"), "vector vs string")
  check(not (T.T.i32x4.ct(1) == {}), "vector vs table")
  -- A scalar operand is splatted, exactly as it is for arithmetic.
  check(T.T.i32x4.ct(1) == 1, "vector vs matching number")
  check(not (T.T.i32x4.ct(1, 1, 1, 2) == 1), "vector vs differing number")
end)

test("integer wraparound", function()
  local i8 = T.T.i8x16
  checkeq(tonumber((i8.ct(127) + i8.ct(1))[0]), -128, "int8 add wraps")
  checkeq(tonumber((i8.ct(-128) - i8.ct(1))[0]), 127, "int8 sub wraps")
  checkeq(tonumber((i8.ct(16) * i8.ct(16))[0]), 0, "int8 mul wraps")
  local u32 = T.T.u32x4
  checkeq(tonumber((u32.ct(0xffffffff) + u32.ct(2))[0]), 1, "uint32 add wraps")
  checkeq(tostring((T.T.u64x2.ct(0xffffffffffffffffULL) + T.T.u64x2.ct(3))[0]),
	  "2ULL", "uint64 add wraps")
end)

test("float edge cases", function()
  local f = T.T.float4
  local v = f.ct(1/0, -1/0, 0/0, -0.0)
  local r = v + f.ct(1)
  check(r[0] == math.huge, "inf + 1")
  check(r[1] == -math.huge, "-inf + 1")
  check(r[2] ~= r[2], "nan + 1")
  checkeq(r[3], 1, "-0 + 1")
  local z = f.ct(0) * f.ct(-1)
  checkeq(T.tostr(z), "{-0,-0,-0,-0}", "0 * -1 keeps the sign")
  check((f.ct(1) / f.ct(0))[0] == math.huge, "1/0")
  local d = T.T.double2
  check((d.ct(0) / d.ct(0))[0] ~= (d.ct(0) / d.ct(0))[0], "0/0 is NaN")
end)

test("unsupported operators raise", function()
  local a, b = T.T.float4.ct(1), T.T.float4.ct(2)
  local i = T.T.i32x4.ct(1)
  local function bad(f) local ok, e = pcall(f); return not ok, tostring(e) end
  check(bad(function() return a % b end), "modulo")
  check(bad(function() return a ^ b end), "power")
  check(bad(function() return a < b end), "less than")
  check(bad(function() return a <= b end), "less equal")
  check(bad(function() return a + i end), "mixed element types")
  check(bad(function() return i / i end), "integer division")
  check(bad(function() return a .. b end), "concat")
  check(bad(function() return #a end), "length")
  check(bad(function() return a + "x" end), "string operand")
  check(bad(function() return a + nil end), "nil operand")
  check(bad(function() return a + {} end), "table operand")
end)

test("results are fresh objects", function()
  local a = T.T.i32x4.ct(1, 2, 3, 4)
  local b = a + T.T.i32x4.ct(0)
  check(a == b, "value equal")
  check(tostring(a) ~= tostring(b), "distinct cdata objects")
  checkeq(ffi.sizeof(b), 16, "result size")
  checkeq(tostring(ffi.typeof(b)), tostring(ffi.typeof(a)), "result ctype")
end)

return T
