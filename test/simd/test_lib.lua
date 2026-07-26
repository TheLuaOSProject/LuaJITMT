-- The ffi.simd module, checked against scalar references.
local T = require("simdtest")
local ffi, simd, test, check, checkeq = T.ffi, T.simd, T.test, T.check, T.checkeq

local SEED = tonumber(os.getenv("SIMD_SEED") or "20260725")

local bit = require("bit")

test("bitwise ops", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + ti.bits * 3)
    local bc = ffi.typeof(ti.bits == 8 and "u8x16" or ti.bits == 16 and "u16x8"
			  or ti.bits == 32 and "u32x4" or "u64x2")
    for _ = 1, 60 do
      local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
      local ua, ub = simd.bitcast(bc, a), simd.bitcast(bc, b)
      local function ref(f)
	local t = {}
	for i = 1, ti.lanes do t[i] = f(ua[i-1], ub[i-1]) end
	return simd.bitcast(ti.ct, bc(unpack(t, 1, ti.lanes)))
      end
      checkeq(simd.band(a, b), ref(function(x, y) return bit.band(x, y) end),
	      ti.name .. " band")
      checkeq(simd.bor(a, b), ref(function(x, y) return bit.bor(x, y) end),
	      ti.name .. " bor")
      checkeq(simd.bxor(a, b), ref(function(x, y) return bit.bxor(x, y) end),
	      ti.name .. " bxor")
      checkeq(simd.bandn(a, b),
	      ref(function(x, y) return bit.band(bit.bnot(x), y) end),
	      ti.name .. " bandn")
      checkeq(simd.bnot(a), ref(function(x) return bit.bnot(x) end),
	      ti.name .. " bnot")
    end
  end
end)

test("min/max", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + ti.bits * 5)
    for _ = 1, 100 do
      local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
      checkeq(simd.min(a, b), T.refbin(ti, "min", a, b), ti.name .. " min " ..
	      T.tostr(a) .. " " .. T.tostr(b))
      checkeq(simd.max(a, b), T.refbin(ti, "max", a, b), ti.name .. " max")
    end
  end
  -- MINPS semantics: the second operand wins for NaN and for equal values.
  local f = T.T.float4
  checkeq(T.tostr(simd.min(f.ct(0/0), f.ct(1))), "{1,1,1,1}", "min NaN,1")
  checkeq(T.tostr(simd.min(f.ct(1), f.ct(0/0))), "{nan,nan,nan,nan}", "min 1,NaN")
  checkeq(T.tostr(simd.min(f.ct(0), f.ct(-0.0))), "{-0,-0,-0,-0}", "min 0,-0")
end)

test("comparisons and masks", function()
  local cmps = {"eq", "ne", "lt", "le", "gt", "ge"}
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + ti.bits * 11)
    for _ = 1, 60 do
      local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
      for _, op in ipairs(cmps) do
	checkeq(simd[op](a, b), T.refcmp(ti, op, a, b),
		ti.name .. " " .. op .. " " .. T.tostr(a) .. " " .. T.tostr(b))
      end
      -- a == a is all-ones except for NaN lanes.
      local m = simd.eq(a, a)
      local expect = 0
      for i = 0, ti.lanes-1 do
	local x = a[i]
	if x == x then expect = expect + 2^i end
      end
      checkeq(simd.movemask(m), expect, ti.name .. " movemask")
      check(simd.anyof(m) == (expect ~= 0), ti.name .. " anyof")
      check(simd.allof(m) == (expect == 2^ti.lanes - 1), ti.name .. " allof")
    end
  end
end)

test("select", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + ti.bits * 13)
    for _ = 1, 60 do
      local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
      local m = simd.lt(a, b)
      local got = simd.select(m, a, b)
      local t = {}
      for i = 1, ti.lanes do
	local x, y = a[i-1], b[i-1]
	t[i] = (x < y) and x or y
      end
      checkeq(got, T.vec(ti, t), ti.name .. " select == min-by-lt")
    end
  end
end)

test("shifts", function()
  for _, ti in ipairs(T.T) do
    if not ti.fp then
      local rnd = T.rng(SEED + ti.bits * 17)
      for _ = 1, 40 do
	local a = T.rand(ti, rnd)
	for _, n in ipairs({0, 1, ti.bits-1, ti.bits, ti.bits+9}) do
	  local function ref(f)
	    local t = {}
	    for i = 1, ti.lanes do t[i] = f(a[i-1]) end
	    return T.vec(ti, t)
	  end
	  local m = n >= ti.bits
	  checkeq(simd.shl(a, n), ref(function(x)
	    if m then return 0 end
	    return ffi.cast(ti.ect, ffi.cast("int64_t", x) * 2LL^n)
	  end), ti.name .. " shl " .. n)
	  checkeq(simd.sar(a, n), ref(function(x)
	    local s = m and ti.bits-1 or n
	    local w = ffi.cast("int64_t", ffi.cast(
	      ti.bits == 8 and "int8_t" or ti.bits == 16 and "int16_t" or
	      ti.bits == 32 and "int32_t" or "int64_t", x))
	    local q = w / 2LL^s
	    if w < 0 and q * 2LL^s ~= w then q = q - 1 end
	    return ffi.cast(ti.ect, q)
	  end), ti.name .. " sar " .. n)
	  checkeq(simd.shr(a, n), ref(function(x)
	    if m then return 0 end
	    local w = ffi.cast("uint64_t", ffi.cast(
	      ti.bits == 8 and "uint8_t" or ti.bits == 16 and "uint16_t" or
	      ti.bits == 32 and "uint32_t" or "uint64_t", x))
	    return ffi.cast(ti.ect, w / 2ULL^n)
	  end), ti.name .. " shr " .. n)
	end
      end
    else
      check(not pcall(simd.shl, ti.ct(1), 1), ti.name .. " shift rejected")
    end
  end
end)

test("abs/sqrt/rounding", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + ti.bits * 19)
    for _ = 1, 40 do
      local a = T.rand(ti, rnd)
      if ti.fp then
	-- |x| clears the sign bit, which also covers NaN payloads and -0.
	checkeq(simd.abs(a), simd.bandn(ti.ct(-0.0), a), ti.name .. " abs")
      else
	local t = {}
	for i = 1, ti.lanes do
	  local x = a[i-1]
	  if not ti.signed then
	    t[i] = x
	  else
	    local w = ti.bits == 64 and x or ffi.cast("int64_t", x)
	    t[i] = ffi.cast(ti.ect, w < 0 and -w or w)
	  end
	end
	checkeq(simd.abs(a), T.vec(ti, t), ti.name .. " abs " .. T.tostr(a))
      end
    end
    if ti.fp then
      checkeq(simd.sqrt(ti.ct(4)), ti.ct(2), ti.name .. " sqrt")
      checkeq(simd.sqrt(ti.ct(0)), ti.ct(0), ti.name .. " sqrt 0")
      check(simd.sqrt(ti.ct(-1))[0] ~= simd.sqrt(ti.ct(-1))[0],
	    ti.name .. " sqrt(-1) is NaN")
      local vals = {-2.5, -1.5, -0.5, 0.5, 1.5, 2.5, 3.25, -3.25}
      local n = ti.lanes
      local v = T.vec(ti, vals)
      local function want(f)
	local t = {}
	for i = 1, n do t[i] = f(vals[i]) end
	return T.vec(ti, t)
      end
      checkeq(simd.floor(v), want(math.floor), ti.name .. " floor")
      checkeq(simd.ceil(v), want(math.ceil), ti.name .. " ceil")
      checkeq(simd.trunc(v), want(function(x)
		return x < 0 and math.ceil(x) or math.floor(x) end),
	      ti.name .. " trunc")
      checkeq(simd.round(v), want(function(x)
		local f = math.floor(x)
		local d = x - f
		local r
		if d > 0.5 then r = f + 1 elseif d < 0.5 then r = f
		else r = (f % 2 == 0) and f or f + 1 end
		if r == 0 and x < 0 then r = -0.0 end  -- ROUNDPS keeps the sign
		return r
	      end), ti.name .. " round (ties to even)")
    else
      check(not pcall(simd.sqrt, ti.ct(1)), ti.name .. " sqrt rejected")
      check(not pcall(simd.floor, ti.ct(1)), ti.name .. " floor rejected")
    end
  end
end)

test("saturating arithmetic", function()
  local i8, u8 = T.T.i8x16, T.T.u8x16
  checkeq(tonumber(simd.adds(i8.ct(120), i8.ct(120))[0]), 127, "i8 adds")
  checkeq(tonumber(simd.adds(i8.ct(-120), i8.ct(-120))[0]), -128, "i8 adds neg")
  checkeq(tonumber(simd.subs(i8.ct(-120), i8.ct(120))[0]), -128, "i8 subs")
  checkeq(tonumber(simd.adds(u8.ct(250), u8.ct(20))[0]), 255, "u8 adds")
  checkeq(tonumber(simd.subs(u8.ct(10), u8.ct(20))[0]), 0, "u8 subs")
  local i16 = T.T.i16x8
  checkeq(tonumber(simd.adds(i16.ct(32000), i16.ct(1000))[0]), 32767, "i16 adds")
  check(not pcall(simd.adds, T.T.i32x4.ct(1), T.T.i32x4.ct(1)),
	"32-bit saturating add rejected")
  check(not pcall(simd.adds, T.T.float4.ct(1), T.T.float4.ct(1)),
	"float saturating add rejected")
end)

test("reductions", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + ti.bits * 23)
    for _ = 1, 60 do
      local a = T.rand(ti, rnd)
      for _, op in ipairs({"sum", "min", "max"}) do
	local got = simd["h" .. op](a)
	local want = T.refreduce(ti, op, a)
	checkeq(got, want, ti.name .. " h" .. op .. " " .. T.tostr(a))
      end
    end
  end
end)

test("insert and shuffle", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + ti.bits * 29)
    local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
    for lane = 0, ti.lanes-1 do
      local v = simd.insert(a, lane, ti.fp and 1.5 or 7)
      for i = 0, ti.lanes-1 do
	if i == lane then
	  checkeq(tonumber(v[i]), ti.fp and 1.5 or 7, ti.name .. " insert value")
	else
	  checkeq(tostring(v[i]), tostring(a[i]), ti.name .. " insert keeps lane")
	end
      end
    end
    check(not pcall(simd.insert, a, ti.lanes, 1), ti.name .. " insert range")
    check(not pcall(simd.insert, a, -1, 1), ti.name .. " insert negative")
    -- Reverse shuffle.
    local idx = {}
    for i = 1, ti.lanes do idx[i] = ti.lanes - i end
    local r = simd.shuffle(a, unpack(idx, 1, ti.lanes))
    for i = 0, ti.lanes-1 do
      checkeq(tostring(r[i]), tostring(a[ti.lanes-1-i]), ti.name .. " reverse")
    end
    -- Interleave the low half of a and b.
    local idx2 = {}
    for i = 1, ti.lanes do
      idx2[i] = (i % 2 == 1) and (i-1)/2 or ti.lanes + (i-2)/2
    end
    local r2 = simd.shuffle2(a, b, unpack(idx2, 1, ti.lanes))
    for i = 0, ti.lanes-1 do
      local src = (i % 2 == 0) and a[i/2] or b[(i-1)/2]
      checkeq(tostring(r2[i]), tostring(src), ti.name .. " interleave")
    end
    check(not pcall(simd.shuffle, a, unpack(idx, 1, ti.lanes-1)),
	  ti.name .. " too few indices")
  end
end)

test("shuffle with a runtime index vector", function()
  -- simd.shuffle(a, idxvec) permutes by an index chosen per lane at run time.
  -- Indices are reduced modulo the lane count, so every value is defined and
  -- no guard is needed; the reference reduces them the same way.
  local bit_ = require("bit")
  for _, ti in ipairs(T.T) do
    local it = T.masktype(ti)
    local rnd = T.rng(SEED + ti.bits * 131 + (ti.fp and 3 or 0))
    for _ = 1, 30 do
      local a = T.rand(ti, rnd)
      -- Draw raw index lanes, including out-of-range and negative ones.
      local raw = {}
      for i = 1, ti.lanes do raw[i] = rnd() end
      local ix = it.ct(unpack(raw, 1, ti.lanes))
      local r = simd.shuffle(a, ix)
      for i = 0, ti.lanes-1 do
	local j = tonumber(bit_.band(ix[i], ti.lanes-1))
	checkeq(tostring(r[i]), tostring(a[j]),
		ti.name .. " permute lane " .. i .. " idx " .. tostring(ix[i]))
      end
    end
    -- An identity permutation must return the input unchanged.
    local id = {}
    for i = 1, ti.lanes do id[i] = i-1 end
    local a = T.rand(ti, rnd)
    checkeq(simd.shuffle(a, it.ct(unpack(id, 1, ti.lanes))), a,
	    ti.name .. " identity permute")
  end
end)

test("runtime index vectors are type checked", function()
  local a = T.T.i32x4.ct(1, 2, 3, 4)
  check(not pcall(simd.shuffle, a, T.T.float4.ct(0, 1, 2, 3)),
	"a float index vector must be rejected")
  check(not pcall(simd.shuffle, a, T.T.i16x8.ct(0)),
	"an index vector of the wrong shape must be rejected")
  check(not pcall(simd.shuffle, a, "0"), "a string index must be rejected")
end)

test("bitcast and convert", function()
  local f, i = T.T.float4, T.T.i32x4
  checkeq(tonumber(simd.bitcast(i.ct, f.ct(1))[0]), 0x3f800000, "bitcast f->i")
  checkeq(simd.bitcast(f.ct, simd.bitcast(i.ct, f.ct(1.5))), f.ct(1.5),
	  "bitcast round trip")
  check(not pcall(simd.bitcast, T.T.i8x16.ct, ffi.new("float2")),
	"bitcast size mismatch")
  checkeq(simd.convert(f.ct, i.ct(1, -2, 3, -4)), f.ct(1, -2, 3, -4), "i32->f32")
  checkeq(simd.convert(i.ct, f.ct(1.9, -1.9, 2.1, -2.1)), i.ct(1, -1, 2, -2),
	  "f32->i32 truncates")
  checkeq(simd.convert(T.T.u32x4.ct, i.ct(-1, 0, 1, 2)),
	  T.T.u32x4.ct(0xffffffff, 0, 1, 2), "i32->u32 is a reinterpretation")
  checkeq(simd.convert(T.T.i32x4.ct, T.T.u32x4.ct(0xffffffff, 1, 2, 3)),
	  i.ct(-1, 1, 2, 3), "u32->i32 is a reinterpretation")
  checkeq(simd.convert(T.T.double2.ct, T.T.i64x2.ct(1, -2)),
	  T.T.double2.ct(1, -2), "i64->f64")
  checkeq(simd.convert(T.T.i64x2.ct, T.T.double2.ct(2.9, -2.9)),
	  T.T.i64x2.ct(2, -2), "f64->i64 truncates")
  check(not pcall(simd.convert, T.T.i16x8.ct, i.ct(1)),
	"convert with mismatched lane counts is rejected")
  -- Float to integer follows the packed instruction: truncate toward zero,
  -- indefinite value (the minimum signed value) for NaN or out of range.
  checkeq(simd.convert(i.ct, f.ct(0/0, 1/0, -1/0, 3e9)),
	  i.ct(-2147483648, -2147483648, -2147483648, -2147483648),
	  "f32->i32 indefinite for NaN and out of range")
  checkeq(simd.convert(i.ct, f.ct(2147483520, -2147483648, 0.9, -0.9)),
	  i.ct(2147483520, -2147483648, 0, 0), "f32->i32 in range truncates")
  checkeq(simd.convert(T.T.i64x2.ct, T.T.double2.ct(0/0, 1e300)),
	  T.T.i64x2.ct(-9223372036854775807LL-1, -9223372036854775807LL-1),
	  "f64->i64 indefinite")
  -- Rounding quiets a signalling NaN, exactly like ROUNDPS does.
  local snan = simd.bitcast(f.ct, i.ct(0x7fa00000, 0xffa00000, 0x7fc00000, 1))
  checkeq(simd.bitcast(i.ct, simd.floor(snan)),
	  i.ct(0x7fe00000, -2097152, 0x7fc00000, 0),
	  "floor quiets a signalling NaN")
end)

test("introspection", function()
  checkeq(simd.lanes(T.T.i8x16.ct), 16, "lanes(ctype)")
  checkeq(simd.lanes(T.T.i8x16.ct()), 16, "lanes(cdata)")
  checkeq(tostring(simd.elementtype(T.T.u16x8.ct)), "ctype<unsigned short>",
	  "elementtype")
  local ft = simd.features()
  check(type(ft) == "table", "features is a table")
  check(type(ft.vecsize) == "number", "features.vecsize")
end)

test("negative arguments", function()
  local a = T.T.i32x4.ct(1)
  local function bad(...) return not (pcall(...)) end
  check(bad(simd.band, 1, 2), "band on numbers")
  check(bad(simd.band, a, ffi.new("int[4]")), "band with a plain array")
  check(bad(simd.min, a, T.T.float4.ct(1)), "min across element types")
  check(bad(simd.select, a, a), "select needs three arguments")
  check(bad(simd.movemask, 1), "movemask on a number")
  check(bad(simd.bitcast, ffi.typeof("int"), a), "bitcast to a non-vector")
  check(bad(simd.lanes, ffi.typeof("int")), "lanes of a non-vector")
  check(bad(simd.hsum, ffi.new("int64_t")), "hsum of a scalar cdata")
end)

return T
