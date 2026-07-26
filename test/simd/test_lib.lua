-- The ffi.simd module, checked against scalar references.
local T = require("simdtest")
local ffi, simd, test, check, checkeq = T.ffi, T.simd, T.test, T.check, T.checkeq

local SEED = tonumber(os.getenv("SIMD_SEED") or "20260725")

local bit = require("bit")
local alltypes = {}
for _, types in ipairs({T.T, T.W}) do
  for _, ti in ipairs(types) do alltypes[#alltypes+1] = ti end
end

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
  for _, ti in ipairs(alltypes) do
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
  for _, ti in ipairs(alltypes) do
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
  for _, ti in ipairs(alltypes) do
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

-- Independent 64x64 -> high64 oracle. Base-2^16 schoolbook multiplication
-- keeps every partial sum exactly representable as a Lua number; the tested
-- implementation uses a different base-2^32 decomposition.
local function mulhi64_ref(x, y, signed)
  local xa = ffi.new("uint64_t[1]", ffi.cast("uint64_t", x))
  local ya = ffi.new("uint64_t[1]", ffi.cast("uint64_t", y))
  local xw = ffi.cast("uint16_t *", xa)
  local yw = ffi.cast("uint16_t *", ya)
  local p = {[0]=0, 0, 0, 0, 0, 0, 0, 0}
  local base = 65536
  for i = 0, 3 do
    for j = 0, 3 do p[i+j] = p[i+j] + tonumber(xw[i])*tonumber(yw[j]) end
  end
  for i = 0, 7 do
    local carry = math.floor(p[i] / base)
    p[i] = p[i] - carry*base
    if i < 7 then p[i+1] = p[i+1] + carry end
  end
  local ha = ffi.new("uint64_t[1]")
  local hw = ffi.cast("uint16_t *", ha)
  for i = 0, 3 do hw[i] = p[i+4] end
  local hi = ha[0]
  if signed then
    if xw[3] >= 0x8000 then hi = hi - ya[0] end
    if yw[3] >= 0x8000 then hi = hi - xa[0] end
  end
  return hi
end

test("mulhi is the high half of the product", function()
  -- The * operator keeps the low half of each lane product; mulhi keeps the
  -- high half. Together they are the full-width product, which is what the
  -- reference checks: low + high*2^bits must equal the exact product.
  for _, ti in ipairs(T.T) do
    if not ti.fp and ti.bits <= 32 then
      local rnd = T.rng(SEED + ti.bits * 811 + (ti.signed and 3 or 0))
      for _ = 1, 40 do
        local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
        local lo, hi = a * b, simd.mulhi(a, b)
        for i = 0, ti.lanes-1 do
          -- Exact product in int64, which is wide enough for 32x32.
          local x = ffi.cast("int64_t", a[i])
          local y = ffi.cast("int64_t", b[i])
          if not ti.signed then
            x = ffi.cast("int64_t", ffi.cast("uint64_t", x) %
                         (2ULL^ti.bits))
            y = ffi.cast("int64_t", ffi.cast("uint64_t", y) %
                         (2ULL^ti.bits))
          end
          local want = x * y
          -- Reassemble from the two halves the same way the hardware splits.
          local l = ffi.cast("uint64_t", ffi.cast("uint64_t", lo[i]) %
                             (2ULL^ti.bits))
          local h = ffi.cast("int64_t", hi[i])
          local got = h * ffi.cast("int64_t", 2ULL^ti.bits) +
                      ffi.cast("int64_t", l)
          checkeq(tostring(got), tostring(want),
                  ti.name .. " mulhi lane " .. i .. " a=" .. tostring(a[i]) ..
                  " b=" .. tostring(b[i]))
        end
      end
    elseif not ti.fp then
      local rnd = T.rng(SEED + ti.bits * 811 + (ti.signed and 3 or 0))
      for _ = 1, 120 do
	local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
	local hi = simd.mulhi(a, b)
	for i = 0, ti.lanes-1 do
	  local got = ffi.cast("uint64_t", hi[i])
	  local want = mulhi64_ref(a[i], b[i], ti.signed)
	  checkeq(tostring(got), tostring(want),
		  ti.name .. " mulhi64 lane " .. i .. " a=" .. tostring(a[i]) ..
		  " b=" .. tostring(b[i]))
	end
      end
    elseif ti.fp then
      check(not pcall(simd.mulhi, ti.ct(1), ti.ct(1)),
            ti.name .. " float mulhi rejected")
    end
  end
end)

test("fma is a single rounding", function()
  -- fma(a, b, -(a*b)) is exactly the rounding error of the product, so it is
  -- non-zero only if the multiply and the add really are fused. An unfused
  -- a*b + -(a*b) is exactly zero. That is the whole point of the operation,
  -- so check it directly instead of trusting a value comparison.
  for _, ti in ipairs(T.T) do
    if ti.fp then
      local rnd = T.rng(SEED + ti.bits * 641)
      local ulp = ti.bits == 32 and 2^-23 or 2^-52
      -- Values just above 1 with many mantissa bits: the product always has
      -- to round, and can never overflow, so the error is finite.
      local function near1()
        local t = {}
        for i = 1, ti.lanes do t[i] = 1 + (rnd() % 1000000) * ulp end
        return T.vec(ti, t)
      end
      local nonzero = 0
      for _ = 1, 60 do
        local a, b = near1(), near1()
        local err = simd.fma(a, b, -(a * b))
        for i = 0, ti.lanes-1 do
          check(err[i] == err[i], ti.name .. " fma rounding error is NaN")
          if err[i] ~= 0 then nonzero = nonzero + 1 end
        end
        -- Multiplying by 2 is exact, so there is nothing left to fuse and the
        -- fused and unfused forms must agree bit for bit.
        local two = ti.ct(2)
        checkeq(simd.fma(a, two, b), a * two + b, ti.name .. " exact fma")
      end
      check(nonzero > 0,
            ti.name .. " fma never produced a rounding error: not fused")
      -- Scalar operands splat, like every other binary entry.
      checkeq(simd.fma(ti.ct(2), 3, 4), ti.ct(10), ti.name .. " fma scalars")
    else
      check(not pcall(simd.fma, ti.ct(1), ti.ct(1), ti.ct(1)),
            ti.name .. " integer fma rejected")
    end
  end
end)

test("shifts with a per-lane count vector", function()
  -- simd.shl/shr/sar accept a vector count: every lane shifts by its own
  -- amount. Counts are unsigned, and one at or above the lane width flushes
  -- to zero (shl/shr) or to a full sign fill (sar), same as a scalar count.
  for _, ti in ipairs(T.T) do
    if not ti.fp then
      local it = T.masktype(ti)
      local rnd = T.rng(SEED + ti.bits * 577 + (ti.signed and 5 or 0))
      -- Counts small enough to be exact Lua numbers, so the reference can be
      -- the scalar form (independently tested) applied one lane at a time.
      -- They still straddle the lane width, which is the interesting boundary.
      local pool = {0, 1, ti.bits-1, ti.bits, ti.bits+7, 2*ti.bits}
      for _, op in ipairs({"shl", "shr", "sar"}) do
	for _ = 1, 25 do
	  local a = T.rand(ti, rnd)
	  local raw = {}
	  for i = 1, ti.lanes do
	    local r = rnd() % (#pool + 2)
	    raw[i] = pool[r+1] or (rnd() % ti.bits)
	  end
	  local nv = it.ct(unpack(raw, 1, ti.lanes))
	  local got = simd[op](a, nv)
	  for i = 0, ti.lanes-1 do
	    local want = simd[op](a, raw[i+1])
	    checkeq(tostring(got[i]), tostring(want[i]),
		    ti.name .. " " .. op .. " lane " .. i ..
		    " count " .. tostring(raw[i+1]))
	  end
	end
      end
      -- A count with bits set far above the lane width is still just a large
      -- unsigned number: shl/shr flush to zero, sar fills with the sign.
      local a = T.rand(ti, rnd)
      local huge = it.ct(-1)  -- All ones: the largest unsigned count there is.
      local zero = it.ct(0)
      checkeq(simd.shl(a, huge), ti.ct(0), ti.name .. " shl huge count")
      checkeq(simd.shr(a, huge), ti.ct(0), ti.name .. " shr huge count")
      checkeq(simd.sar(a, huge), simd.sar(a, ti.bits),
	      ti.name .. " sar huge count is a sign fill")
      checkeq(simd.shl(a, zero), a, ti.name .. " shl by zero is identity")
    end
  end
end)

test("shift count vectors are type checked", function()
  local a = T.T.i32x4.ct(1, 2, 3, 4)
  check(not pcall(simd.shl, a, T.T.float4.ct(1)),
	"a float count vector must be rejected")
  check(not pcall(simd.shl, a, T.T.i16x8.ct(1)),
	"a count vector of the wrong shape must be rejected")
  check(not pcall(simd.shl, T.T.float4.ct(1), T.T.i32x4.ct(1)),
	"shifting a float vector must be rejected")
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
  checkeq(simd.convert(T.T.u64x2.ct, T.T.double2.ct(2.9, -2.9)),
	  T.T.u64x2.ct(2, 0xfffffffffffffffeULL),
	  "f64->u64 uses the signed truncation bits")
  checkeq(simd.convert(f.ct,
	  T.T.u32x4.ct(0, 1, 16777217, 0xffffffff)),
	  f.ct(0, 1, 16777216, 4294967296), "u32->f32 rounds correctly")
  checkeq(simd.convert(T.T.double2.ct,
	  T.T.i64x2.ct(-9007199254740993LL, 9007199254740993LL)),
	  T.T.double2.ct(-9007199254740992, 9007199254740992),
	  "i64->f64 rounds correctly")
  checkeq(simd.convert(T.T.double2.ct,
	  T.T.u64x2.ct(9007199254740993ULL, 0xffffffffffffffffULL)),
	  T.T.double2.ct(9007199254740992, 18446744073709551616),
	  "u64->f64 rounds correctly")
  checkeq(simd.bitcast(T.T.u32x4.ct,
	  simd.convert(f.ct, T.W.u64x4.ct(
	    0x8000008000000001ULL, 0x4000004000000001ULL, 0, 1))),
	  T.T.u32x4.ct(0x5f000001, 0x5e800001, 0, 0x3f800000),
	  "u64->f32 rounds once instead of through double")
  checkeq(simd.bitcast(T.T.u32x4.ct,
	  simd.convert(f.ct, T.W.i64x4.ct(
	    0x4000004000000001LL, -0x4000004000000001LL, 0, 1))),
	  T.T.u32x4.ct(0x5e800001, 0xde800001, 0, 0x3f800000),
	  "i64->f32 rounds once instead of through double")
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
  checkeq(simd.convert(T.T.u64x2.ct, T.T.double2.ct(0/0, 1e300)),
	  T.T.u64x2.ct(0x8000000000000000ULL, 0x8000000000000000ULL),
	  "f64->u64 has the same signed indefinite bits")
  checkeq(simd.convert(T.T.i64x2.ct,
	  T.T.double2.ct(9223372036854774784, 9223372036854775808)),
	  T.T.i64x2.ct(9223372036854774784LL, -9223372036854775807LL-1),
	  "f64->i64 signed boundary")
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
  checkeq(ft.vecsize, ft.avx2 and 32 or ft.sse2 and 16 or 0,
	  "features.vecsize matches the usable native width")
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
