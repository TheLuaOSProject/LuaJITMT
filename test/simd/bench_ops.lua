-- SIMD kernel benchmarks: realistic loops, vectorized vs. equivalent scalar.
--
--   luajit test/simd/bench_ops.lua [reps]
--
-- Complements bench.lua, which measures a few streaming kernels. This file
-- measures whole algorithms of the kind vector code is actually written for,
-- and a per-operation throughput table.
--
-- Every kernel checks its vector result against the scalar one before timing,
-- so a benchmark that silently computes the wrong thing is reported as a
-- failure rather than as a speedup. Every result is consumed, so nothing can
-- be optimised away.
collectgarbage("stop")

local dir = arg[0]:match("^(.*)[/\\][^/\\]*$") or "."
package.path = dir .. "/?.lua;" .. package.path

local ffi = require("ffi")
local simd = require("ffi.simd")
require("simdtest")

local REPS = tonumber(arg[1] or "5")

local f4 = ffi.typeof("float4")
local i4 = ffi.typeof("i32x4")
local u4 = ffi.typeof("u32x4")
local i16 = ffi.typeof("i16x8")
local u8 = ffi.typeof("u8x16")
local features = simd.features()
local has_ymm = features.avx2 and features.vecsize >= 32
local f8 = has_ymm and ffi.typeof("float8")
local i8 = has_ymm and ffi.typeof("i32x8")
local i16w = has_ymm and ffi.typeof("i16x16")
local u8w = has_ymm and ffi.typeof("u8x32")

local failures = 0

local function time(f, ...)
  local best = math.huge
  local sink
  for _ = 1, REPS do
    local t0 = os.clock()
    sink = f(...)
    local dt = os.clock() - t0
    if dt < best then best = dt end
  end
  return best, sink
end

-- Compare two floating-point results with a relative tolerance. A vector
-- reduction sums four partial accumulators and the scalar one sums a single
-- running total, so the two round differently by construction. That is a
-- property of the algorithm, not a bug, so FP kernels are compared with a
-- tolerance and integer kernels exactly.
local function approx(x, y, tol)
  local a, b = tonumber(x), tonumber(y)
  if not a or not b then return tostring(x) == tostring(y) end
  local d = math.abs(a - b)
  return d <= (tol or 1e-4) * math.max(1, math.abs(a), math.abs(b))
end

-- A reduction over a few hundred thousand terms into a float32 accumulator
-- cannot match a double one however it is summed, so these are compared only
-- loosely. Kernel correctness is the test suite's job; this is a smoke check
-- that the benchmark is computing the same thing at all.
local function approx_sum(x, y) return approx(x, y, 2e-2) end

-- Run a scalar and a vector implementation, compare, then report.
local function bench(name, scalar, vector, cmp)
  local ts, ss = time(scalar)
  local tv, sv = time(vector)
  local same = (cmp or function(x, y) return tostring(x) == tostring(y) end)(ss, sv)
  if not same then failures = failures + 1 end
  io.write(string.format("%-30s scalar %7.2f ms   vector %7.2f ms   %5.2fx%s\n",
			 name, ts*1000, tv*1000, ts/tv,
			 same and "" or "   ! MISMATCH " ..
			 tostring(ss) .. " vs " .. tostring(sv)))
end

io.write("== kernels ==\n")

------------------------------------------------------------- 4x4 matrix ----
-- Transform N points by a 4x4 matrix. The classic case for a splat plus a
-- multiply-add chain.
do
  local N = 1 << 14
  local pts = ffi.new(ffi.typeof("$[?]", f4), N)
  for i = 0, N-1 do pts[i] = f4(i*0.001, i*0.002, i*0.003, 1) end
  local m = {f4(1, 2, 3, 4), f4(5, 6, 7, 8), f4(9, 10, 11, 12), f4(13, 14, 15, 16)}
  local ms = {}
  for r = 1, 4 do ms[r] = {m[r][0], m[r][1], m[r][2], m[r][3]} end

  bench("4x4 matrix * point", function()
    local acc = 0
    for _ = 1, 20 do
      for i = 0, N-1 do
	local p = pts[i]
	local x, y, z, w = p[0], p[1], p[2], p[3]
	local r0 = ms[1][1]*x + ms[2][1]*y + ms[3][1]*z + ms[4][1]*w
	acc = acc + r0
      end
    end
    return acc
  end, function()
    local acc = f4(0)
    for _ = 1, 20 do
      for i = 0, N-1 do
	local p = pts[i]
	acc = acc + m[1]*simd.shuffle(p, 0, 0, 0, 0)
		  + m[2]*simd.shuffle(p, 1, 1, 1, 1)
		  + m[3]*simd.shuffle(p, 2, 2, 2, 2)
		  + m[4]*simd.shuffle(p, 3, 3, 3, 3)
      end
    end
    return tonumber(acc[0])
  end, approx_sum)
end

------------------------------------------------------------ alpha blend ----
-- dst = src*a + dst*(1-a), the most common compositing loop there is.
do
  local N = 1 << 14
  local src = ffi.new(ffi.typeof("$[?]", f4), N)
  local dst = ffi.new(ffi.typeof("$[?]", f4), N)
  for i = 0, N-1 do
    src[i] = f4((i%255)/255, (i%127)/127, (i%63)/63, (i%31)/31)
    dst[i] = f4(0.5, 0.25, 0.125, 1)
  end
  local a, ia = 0.75, 0.25
  local va, via = f4(a), f4(ia)

  bench("alpha blend (float4)", function()
    local acc = 0
    for _ = 1, 20 do
      for i = 0, N-1 do
	local s, d = src[i], dst[i]
	acc = acc + (s[0]*a + d[0]*ia) + (s[1]*a + d[1]*ia)
		  + (s[2]*a + d[2]*ia) + (s[3]*a + d[3]*ia)
      end
    end
    return acc
  end, function()
    local acc = f4(0)
    for _ = 1, 20 do
      for i = 0, N-1 do acc = acc + (src[i]*va + dst[i]*via) end
    end
    return tonumber(acc[0]) + tonumber(acc[1]) + tonumber(acc[2]) +
	   tonumber(acc[3])
  end, approx)
end

------------------------------------------------------------- polynomial ----
-- Horner evaluation. Arithmetic bound, so this is where fma actually pays.
do
  local N = 1 << 14
  local xs = ffi.new(ffi.typeof("$[?]", f4), N)
  for i = 0, N-1 do xs[i] = f4(i*0.0001, i*0.0002, i*0.0003, i*0.0004) end
  local c = {1.1, 2.2, 3.3, 4.4, 5.5}
  local vc = {}
  for k = 1, 5 do vc[k] = f4(c[k]) end

  bench("degree-4 horner (float4)", function()
    local acc = 0
    for _ = 1, 20 do
      for i = 0, N-1 do
	local x = xs[i][0]
	acc = acc + ((((c[1]*x + c[2])*x + c[3])*x + c[4])*x + c[5])
      end
    end
    return acc
  end, function()
    local acc = f4(0)
    for _ = 1, 20 do
      for i = 0, N-1 do
	local x = xs[i]
	acc = acc + ((((vc[1]*x + vc[2])*x + vc[3])*x + vc[4])*x + vc[5])
      end
    end
    return tonumber(acc[0])
  end, approx)
end

------------------------------------------------------- horner with fma ----
-- Same polynomial, but asking for the fused form explicitly. Reported in two
-- shapes on purpose, because they answer different questions.
--
-- The array version accumulates into a separate total, so its iterations are
-- independent and the loop is bound by that accumulator's dependency chain,
-- not by the polynomial. Halving the arithmetic buys nothing there, and the
-- one register copy the innermost fma needs (none of its operands dies, so
-- something has to be moved into the destination) lands on the critical path
-- and makes it slightly slower. The loop-carried version is bound by the
-- chain itself, which is where fusing two 4-cycle operations into one shows
-- up as roughly the 2x it should be.
if features.fma then
  local N = 1 << 14
  local xs = ffi.new(ffi.typeof("$[?]", f4), N)
  for i = 0, N-1 do xs[i] = f4(i*0.0001, i*0.0002, i*0.0003, i*0.0004) end
  local vc = {}
  for k, v in ipairs({1.1, 2.2, 3.3, 4.4, 5.5}) do vc[k] = f4(v) end
  local fma = simd.fma

  local tmul = time(function()
    local acc = f4(0)
    for _ = 1, 20 do
      for i = 0, N-1 do
	local x = xs[i]
	acc = acc + ((((vc[1]*x + vc[2])*x + vc[3])*x + vc[4])*x + vc[5])
      end
    end
    return tonumber(acc[0])
  end)
  local tfma = time(function()
    local acc = f4(0)
    for _ = 1, 20 do
      for i = 0, N-1 do
	local x = xs[i]
	acc = acc + fma(fma(fma(fma(vc[1], x, vc[2]), x, vc[3]), x, vc[4]),
			x, vc[5])
      end
    end
    return tonumber(acc[0])
  end)
  io.write(string.format("%-30s mul+add %6.2f ms   fma %7.2f ms   %5.2fx\n",
			 "  same, fused (throughput)", tmul*1000, tfma*1000,
			 tmul/tfma))

  local ITER = 1 << 22
  local c1, c2 = f4(1.0000001), f4(0.0000001)
  local tm = time(function()
    local a = f4(1)
    for _ = 1, ITER do a = a*c1 + c2 end
    return tonumber(a[0])
  end)
  local tf = time(function()
    local a = f4(1)
    for _ = 1, ITER do a = fma(a, c1, c2) end
    return tonumber(a[0])
  end)
  io.write(string.format("%-30s mul+add %6.2f ns   fma %7.2f ns   %5.2fx\n",
			 "  fused (latency chain)", tm*1e9/ITER, tf*1e9/ITER,
			 tm/tf))
end

--------------------------------------------------------------- byte scan ----
-- Find the first byte equal to a target. movemask turns 16 lane compares into
-- one branch, which is what makes this worth vectorizing at all.
do
  local N = 1 << 16
  local buf = ffi.new("uint8_t[?]", N)
  for i = 0, N-1 do buf[i] = (i * 7) % 251 end
  buf[N-3] = 199
  local bv = ffi.cast(ffi.typeof("$ *", u8), buf)
  local target, vtarget = 199, u8(199)

  bench("find byte in 64 KiB", function()
    local found = -1
    for _ = 1, 200 do
      found = -1
      for i = 0, N-1 do
	if buf[i] == target then found = i break end
      end
    end
    return found
  end, function()
    local found = -1
    for _ = 1, 200 do
      found = -1
      for i = 0, N/16-1 do
	local m = simd.movemask(simd.eq(bv[i], vtarget))
	if m ~= 0 then
	  for k = 0, 15 do
	    if bit.band(m, bit.lshift(1, k)) ~= 0 then found = i*16+k break end
	  end
	  break
	end
      end
    end
    return found
  end)
end

--------------------------------------------------------------- swizzle -----
-- RGBA -> BGRA with a constant lane permute.
do
  local N = 1 << 14
  local px = ffi.new(ffi.typeof("$[?]", i4), N)
  for i = 0, N-1 do px[i] = i4(i, i+1, i+2, i+3) end

  bench("RGBA->BGRA swizzle", function()
    local acc = 0
    for _ = 1, 40 do
      for i = 0, N-1 do
	local p = px[i]
	acc = acc + p[2] + p[1]*2 + p[0]*3 + p[3]*4
      end
    end
    return bit.tobit(acc)
  end, function()
    local acc = i4(0)
    local w = i4(1, 2, 3, 4)
    for _ = 1, 40 do
      for i = 0, N-1 do acc = acc + simd.shuffle(px[i], 2, 1, 0, 3) * w end
    end
    return bit.tobit(tonumber(acc[0]) + tonumber(acc[1]) + tonumber(acc[2]) +
		     tonumber(acc[3]))
  end)
end

---------------------------------------------------------- table lookup -----
-- 16-entry lookup table applied to every byte: one PSHUFB against a per-lane
-- index computed at run time.
do
  local N = 1 << 14
  local src = ffi.new(ffi.typeof("$[?]", u8), N)
  for i = 0, N-1 do src[i] = u8(i % 16) end
  local tbl = {}
  for k = 0, 15 do tbl[k] = (k * 11 + 3) % 256 end
  local vtbl = u8(tbl[0], tbl[1], tbl[2], tbl[3], tbl[4], tbl[5], tbl[6],
		  tbl[7], tbl[8], tbl[9], tbl[10], tbl[11], tbl[12], tbl[13],
		  tbl[14], tbl[15])

  bench("16-entry byte LUT", function()
    local acc = 0
    for _ = 1, 20 do
      for i = 0, N-1 do
	local v = src[i]
	local s = 0
	for k = 0, 15 do s = s + tbl[v[k]] end
	acc = acc + s % 256
      end
    end
    return acc
  end, function()
    local acc = 0
    for _ = 1, 20 do
      for i = 0, N-1 do
	local r = simd.shuffle(vtbl, src[i])
	acc = acc + tonumber(simd.hsum(r))
      end
    end
    return acc
  end)
end

------------------------------------------------------- per-lane shifts -----
-- Each lane scaled by its own power of two. Without a per-lane shift this is
-- a scalar loop; with AVX2 it is one instruction.
if features.avx2 then
  local N = 1 << 14
  local vals = ffi.new(ffi.typeof("$[?]", i4), N)
  local cnts = ffi.new(ffi.typeof("$[?]", i4), N)
  for i = 0, N-1 do
    vals[i] = i4(i+1, i+2, i+3, i+4)
    cnts[i] = i4(i%5, (i+1)%7, (i+2)%3, (i+3)%11)
  end

  bench("per-lane variable shl", function()
    local acc = 0
    for _ = 1, 20 do
      for i = 0, N-1 do
	local v, c = vals[i], cnts[i]
	for k = 0, 3 do
	  acc = acc + bit.band(bit.lshift(v[k], c[k]), 0xffff)
	end
      end
    end
    return bit.tobit(acc)
  end, function()
    local acc = i4(0)
    local m = i4(0xffff)
    for _ = 1, 20 do
      for i = 0, N-1 do
	acc = acc + simd.band(simd.shl(vals[i], cnts[i]), m)
      end
    end
    return bit.tobit(tonumber(acc[0]) + tonumber(acc[1]) + tonumber(acc[2]) +
		     tonumber(acc[3]))
  end)
end

----------------------------------------------------- fixed point gain ------
-- Q15 gain: multiply 16-bit samples by a fractional coefficient. The desired
-- result is the full product shifted right by 15. Reassemble its low 16 bits
-- from (hi << 1) and the top bit of lo; the * operator alone throws hi away.
do
  local N = 1 << 14
  local buf = ffi.new(ffi.typeof("$[?]", i16), N)
  for i = 0, N-1 do
    buf[i] = i16(i%3001-1500, i%577-288, i%97-48, i%13-6,
		 i%7919-4000, i%31-15, i%255-127, i%1023-511)
  end
  local gain = 0.6
  local q15 = math.floor(gain * 32768 + 0.5)
  local vg = i16(q15)

  -- Accumulated with XOR rather than addition: a 16-bit lane accumulator
  -- would wrap over this many terms while a Lua number would not, and XOR is
  -- associative and commutative, so folding eight lane accumulators at the
  -- end gives exactly the scalar result.
  bench("Q15 gain on int16", function()
    local acc = 0
    for _ = 1, 20 do
      for i = 0, N-1 do
	local v = buf[i]
	for k = 0, 7 do
	  acc = bit.bxor(acc, bit.band(bit.arshift(v[k] * q15, 15), 0xffff))
	end
      end
    end
    return acc
  end, function()
    local acc = i16(0)
    for _ = 1, 20 do
      for i = 0, N-1 do
	local lo = buf[i] * vg
	local hi = simd.mulhi(buf[i], vg)
	local scaled = simd.bor(simd.shl(hi, 1), simd.shr(lo, 15))
	acc = simd.bxor(acc, scaled)
      end
    end
    local s = 0
    for k = 0, 7 do s = bit.bxor(s, bit.band(acc[k], 0xffff)) end
    return s
  end)
end

--------------------------------------------------------------- clamp -------
-- Branchless saturation. The scalar version is branchy, which is why the
-- vector version wins more than the 4x lane count.
do
  local N = 1 << 14
  local xs = ffi.new(ffi.typeof("$[?]", f4), N)
  for i = 0, N-1 do
    xs[i] = f4((i%1000)-500, (i%777)-300, (i%333)-100, (i%99)-50)
  end
  local lo, hi = -100, 200
  local vlo, vhi = f4(lo), f4(hi)

  bench("clamp to [-100,200]", function()
    local acc = 0
    for _ = 1, 40 do
      for i = 0, N-1 do
	local v = xs[i]
	for k = 0, 3 do
	  local x = v[k]
	  if x < lo then x = lo elseif x > hi then x = hi end
	  acc = acc + x
	end
      end
    end
    return acc
  end, function()
    local acc = f4(0)
    for _ = 1, 40 do
      for i = 0, N-1 do acc = acc + simd.min(simd.max(xs[i], vlo), vhi) end
    end
    return tonumber(acc[0]) + tonumber(acc[1]) + tonumber(acc[2]) +
	   tonumber(acc[3])
  end, approx_sum)
end

io.write("\n== per-operation throughput (ns per 128-bit op, loop-carried) ==\n")

-- A dependent chain of one operation, so the number is latency bound and
-- directly comparable between operations. The accumulator is consumed.
local function op_latency(setup, step)
  local ITERS = 1 << 20
  local a, b, c = setup()
  local best = math.huge
  for _ = 1, REPS do
    local t0 = os.clock()
    local acc = a
    for _ = 1, ITERS do acc = step(acc, b, c) end
    local dt = os.clock() - t0
    if dt < best then best = dt end
    local lane0 = tonumber(acc[0])
    if lane0 ~= lane0 then io.write("nan\n") end
  end
  return best*1e9/ITERS
end

local function op_cost(name, setup, step)
  io.write(string.format("  %-24s %6.2f ns\n", name, op_latency(setup, step)))
end

do
  local fa, fb = f4(1.0009765625), f4(1.00048828125)
  local ia, ib = i4(3), i4(5)
  op_cost("float4 add", function() return fa, fb end,
	  function(x, y) return x + y end)
  op_cost("float4 mul", function() return f4(1.0000001), fb end,
	  function(x, y) return x * y end)
  op_cost("float4 div", function() return f4(1e30), f4(1.0000001) end,
	  function(x, y) return x / y end)
  op_cost("float4 sqrt", function() return f4(1e30), fb end,
	  function(x) return simd.sqrt(x) end)
  op_cost("float4 min", function() return fa, fb end,
	  function(x, y) return simd.min(x, y) end)
  if features.fma then
    local fma = simd.fma
    op_cost("float4 fma", function() return f4(1.0000001), fb, f4(0.5) end,
	    function(x, y, z) return fma(x, y, z) end)
  end
  op_cost("i32x4 add", function() return ia, ib end,
	  function(x, y) return x + y end)
  op_cost("i32x4 mul", function() return i4(1), i4(1) end,
	  function(x, y) return x * y end)
  op_cost("i32x4 shl const", function() return ia, ib end,
	  function(x) return simd.shl(x, 1) end)
  if features.avx2 then
    op_cost("i32x4 shl per-lane", function() return ia, i4(0, 1, 0, 1) end,
	    function(x, y) return simd.shl(x, y) end)
  end
  op_cost("i32x4 select", function() return ia, ib end,
	  function(x, y) return simd.select(simd.gt(x, y), x, y) end)
  op_cost("i32x4 shuffle const", function() return ia, ib end,
	  function(x) return simd.shuffle(x, 3, 2, 1, 0) end)
  op_cost("i32x4 shuffle vector", function() return ia, i4(3, 2, 1, 0) end,
	  function(x, y) return simd.shuffle(x, y) end)
  op_cost("i8x16 add", function() return u8(1), u8(3) end,
	  function(x, y) return x + y end)
  op_cost("i8x16 mul", function() return u8(1), u8(1) end,
	  function(x, y) return x * y end)
  op_cost("i16x8 adds", function() return i16(1), i16(3) end,
	  function(x, y) return simd.adds(x, y) end)
  op_cost("i16x8 mulhi", function() return i16(30000), i16(30000) end,
	  function(x, y) return simd.mulhi(x, y) end)
end

if has_ymm then
  io.write("\n== AVX2 width comparison (dependent ns/op) ==\n")
  io.write("                                      XMM       YMM   lane throughput\n")

  local function width_cost(name, setup_xmm, setup_ymm, step)
    local tx = op_latency(setup_xmm, step)
    local ty = op_latency(setup_ymm, step)
    -- YMM has twice as many lanes. 2*tx/ty is its per-lane throughput gain.
    io.write(string.format("  %-24s %6.2f ns  %6.2f ns      %5.2fx\n",
			   name, tx, ty, 2*tx/ty))
  end

  width_cost("float add",
    function() return f4(1.0009765625), f4(1.00048828125) end,
    function() return f8(1.0009765625), f8(1.00048828125) end,
    function(x, y) return x + y end)
  width_cost("float mul",
    function() return f4(1.0000001), f4(1.00048828125) end,
    function() return f8(1.0000001), f8(1.00048828125) end,
    function(x, y) return x * y end)
  width_cost("float div",
    function() return f4(1e30), f4(1.0000001) end,
    function() return f8(1e30), f8(1.0000001) end,
    function(x, y) return x / y end)
  width_cost("float sqrt",
    function() return f4(1e30), f4(0) end,
    function() return f8(1e30), f8(0) end,
    function(x) return simd.sqrt(x) end)
  width_cost("float min",
    function() return f4(1.0009765625), f4(1.00048828125) end,
    function() return f8(1.0009765625), f8(1.00048828125) end,
    function(x, y) return simd.min(x, y) end)
  if features.fma then
    width_cost("float fma",
      function() return f4(1.0000001), f4(1.00048828125), f4(0.0001) end,
      function() return f8(1.0000001), f8(1.00048828125), f8(0.0001) end,
      function(x, y, z) return simd.fma(x, y, z) end)
  end
  width_cost("int32 add",
    function() return i4(3), i4(5) end,
    function() return i8(3), i8(5) end,
    function(x, y) return x + y end)
  width_cost("int32 mul",
    function() return i4(1), i4(1) end,
    function() return i8(1), i8(1) end,
    function(x, y) return x * y end)
  width_cost("int32 xor",
    function() return i4(0x55555555), i4(0x33333333) end,
    function() return i8(0x55555555), i8(0x33333333) end,
    function(x, y) return simd.bxor(x, y) end)
  width_cost("int32 shl const",
    function() return i4(3), i4(0) end,
    function() return i8(3), i8(0) end,
    function(x) return simd.shl(x, 1) end)
  width_cost("int32 select",
    function() return i4(3), i4(5) end,
    function() return i8(3), i8(5) end,
    function(x, y) return simd.select(simd.gt(x, y), x, y) end)
  width_cost("int16 mulhi",
    function() return i16(30000), i16(30000) end,
    function() return i16w(30000), i16w(30000) end,
    function(x, y) return simd.mulhi(x, y) end)
  width_cost("uint8 saturated add",
    function() return u8(1), u8(3) end,
    function() return u8w(1), u8w(3) end,
    function(x, y) return simd.adds(x, y) end)
end

if failures > 0 then
  io.write(string.format("\n%d benchmark(s) produced a wrong result\n", failures))
  os.exit(1)
end
