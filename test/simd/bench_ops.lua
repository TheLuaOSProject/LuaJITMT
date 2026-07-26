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
local jit_ = require("jit")
require("simdtest")

local REPS = tonumber(arg[1] or "5")

local f4 = ffi.typeof("float4")
local d2 = ffi.typeof("double2")
local i4 = ffi.typeof("i32x4")
local u4 = ffi.typeof("u32x4")
local s8 = ffi.typeof("i8x16")
local i16 = ffi.typeof("i16x8")
local u8 = ffi.typeof("u8x16")
local i64 = ffi.typeof("i64x2")
local u64 = ffi.typeof("u64x2")
local features = simd.features()
local has_ymm = features.avx2 and features.vecsize >= 32
local f8 = has_ymm and ffi.typeof("float8")
local d4 = has_ymm and ffi.typeof("double4")
local i8 = has_ymm and ffi.typeof("i32x8")
local u8d = has_ymm and ffi.typeof("u32x8")
local s8w = has_ymm and ffi.typeof("i8x32")
local i16w = has_ymm and ffi.typeof("i16x16")
local u8w = has_ymm and ffi.typeof("u8x32")
local i64w = has_ymm and ffi.typeof("i64x4")
local u64w = has_ymm and ffi.typeof("u64x4")

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

io.write("\n== dynamic vector construction (ns/vector, construct + add) ==\n")

-- Assemble vectors from independent runtime scalars. This is common at the
-- boundary between structure-of-arrays data and packed math (geometry,
-- colours, audio frames, packet fields). It used to allocate temporary cdata,
-- store every lane and reload the vector on each iteration.
local function ctor2(ct, i)
  return ct(i, i+1)
end

local function ctor0(ct)
  return ct()
end

local function ctor4(ct, i)
  return ct(i, i+1, i+2, i+3)
end

local function ctor8(ct, i)
  return ct(i, i+1, i+2, i+3, i+4, i+5, i+6, i+7)
end

local function ctor16(ct, i)
  return ct(i, i+1, i+2, i+3, i+4, i+5, i+6, i+7,
	    i+8, i+9, i+10, i+11, i+12, i+13, i+14, i+15)
end

local function ctor32(ct, i)
  return ct(i, i+1, i+2, i+3, i+4, i+5, i+6, i+7,
	    i+8, i+9, i+10, i+11, i+12, i+13, i+14, i+15,
	    i+16, i+17, i+18, i+19, i+20, i+21, i+22, i+23,
	    i+24, i+25, i+26, i+27, i+28, i+29, i+30, i+31)
end

local function constructor_latency(ct, make)
  local ITERS = 1 << 20
  jit_.flush()
  local best = math.huge
  for _ = 1, REPS do
    local acc = ct(0)
    local t0 = os.clock()
    for i = 1, ITERS do acc = acc + make(ct, i) end
    local dt = os.clock() - t0
    if dt < best then best = dt end
    local lane0 = tonumber(acc[0])
    if lane0 ~= lane0 then io.write("nan\n") end
  end
  return best*1e9/ITERS
end

if has_ymm then
  io.write("                                      XMM       YMM   lane throughput\n")
else
  io.write("                                      XMM\n")
end

local function constructor_cost(name, xct, xmake, yct, ymake)
  local tx = constructor_latency(xct, xmake)
  if yct then
    local ty = constructor_latency(yct, ymake)
    io.write(string.format("  %-24s %6.2f ns  %6.2f ns      %5.2fx\n",
			   name, tx, ty, 2*tx/ty))
  else
    io.write(string.format("  %-24s %6.2f ns\n", name, tx))
  end
end

constructor_cost("float lanes", f4, ctor4, f8, ctor8)
constructor_cost("double lanes", d2, ctor2, d4, ctor4)
constructor_cost("int32 lanes", i4, ctor4, i8, ctor8)
constructor_cost("int16 lanes", i16, ctor8, i16w, ctor16)
constructor_cost("int8 lanes", s8, ctor16, s8w, ctor32)
constructor_cost("int64 lanes", i64, ctor2, i64w, ctor4)
constructor_cost("int32 half-filled", i4, ctor2, i8, ctor4)
constructor_cost("int8 half-filled", s8, ctor8, s8w, ctor16)
constructor_cost("int32 zero", i4, ctor0, i8, ctor0)

io.write("\n== per-operation throughput (ns per 128-bit op, loop-carried) ==\n")

-- A dependent chain of one operation, so the number is latency bound and
-- directly comparable between operations. The accumulator is consumed.
local function op_latency(setup, step)
  local ITERS = 1 << 20
  -- This helper is deliberately polymorphic in `step`. Flush between rows so
  -- an NYI or heavily specialised earlier trace cannot blacklist the shared
  -- loop body and make a later packed operation appear interpreted.
  jit_.flush()
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
  op_cost("i32x4 add lane literal", function() return ia, ib end,
	  function(x) return x + i4(1, 2, 3, 4) end)
  op_cost("i32x4 mul", function() return i4(1), i4(1) end,
	  function(x, y) return x * y end)
  op_cost("i32x4 shl const", function() return ia, ib end,
	  function(x) return simd.shl(x, 1) end)
  op_cost("i32x4 rol 8 idiom", function() return ia, ib end,
	  function(x)
	    return simd.bor(simd.shl(x, 8), simd.shr(x, 24))
	  end)
  op_cost("i32x4 rol 7 idiom", function() return ia, ib end,
	  function(x)
	    return simd.bor(simd.shl(x, 7), simd.shr(x, 25))
	  end)
  if features.avx2 then
    op_cost("i32x4 shl per-lane", function() return ia, i4(0, 1, 0, 1) end,
	    function(x, y) return simd.shl(x, y) end)
    op_cost("i16x8 shl per-lane",
	    function() return i16(3), i16(0, 1, 2, 3, 4, 5, 6, 7) end,
	    function(x, y) return simd.shl(x, y) end)
    op_cost("i16x8 shr per-lane",
	    function()
	      return i16(-12345), i16(0, 1, 15, 16, 2, 7, 14, 20)
	    end,
	    function(x, y) return simd.shr(x, y) end)
    op_cost("i16x8 sar per-lane",
	    function()
	      return i16(-12345), i16(0, 1, 15, 16, 2, 7, 14, 20)
	    end,
	    function(x, y) return simd.sar(x, y) end)
    op_cost("i8x16 shl per-lane",
	    function()
	      return s8(3), s8(0, 1, 2, 3, 4, 5, 6, 7,
			       0, 1, 2, 3, 4, 5, 6, 7)
	    end,
	    function(x, y) return simd.shl(x, y) end)
    op_cost("i8x16 shr per-lane",
	    function()
	      return s8(-119), s8(0, 1, 2, 3, 4, 5, 6, 7,
				  0, 1, 2, 3, 4, 5, 6, 7)
	    end,
	    function(x, y) return simd.shr(x, y) end)
    op_cost("i8x16 sar per-lane",
	    function()
	      return s8(-119), s8(0, 1, 2, 3, 4, 5, 6, 7,
				  0, 1, 2, 3, 4, 5, 6, 7)
	    end,
	    function(x, y) return simd.sar(x, y) end)
  end
  op_cost("i32x4 select max", function() return ia, ib end,
	  function(x, y) return simd.select(simd.gt(x, y), x, y) end)
  op_cost("i32x4 select ge max", function() return ia, ib end,
	  function(x, y) return simd.select(simd.ge(x, y), x, y) end)
  op_cost("i32x4 select abs", function() return i4(-7) end,
	  function(x)
	    return simd.select(simd.gt(x, i4(0)), x, -x)
	  end)
  op_cost("i32x4 restore sign",
	  function() return i4(7), i4(-1, 0, 1, -2) end,
	  function(x, sign)
	    return simd.select(simd.ge(sign, i4(0)), x, -x)
	  end)
  op_cost("i32x4 select generic", function() return ia, ib, i4(1) end,
	  function(x, y, z)
	    return simd.select(simd.gt(x, y), x + z, y)
	  end)
  op_cost("i32x4 select false zero", function() return ia, ib, i4(1) end,
	  function(x, y, z)
	    return simd.select(simd.gt(x, y), x + z, i4(0))
	  end)
  op_cost("i32x4 select true zero", function() return ia, ib, i4(1) end,
	  function(x, y, z)
	    return simd.select(simd.gt(x, y), i4(0), x + z)
	  end)
  op_cost("i32x4 select true ones", function() return ia, ib, i4(1) end,
	  function(x, y, z)
	    return simd.select(simd.gt(x, y), i4(-1), x + z)
	  end)
  op_cost("i32x4 select const mask", function() return ia, ib, i4(1) end,
	  function(x, y, z)
	    return simd.select(i4(-1, 0, -1, 0), x + z, y)
	  end)
  op_cost("i32x4 select runtime mask",
	  function() return ia, ib, i4(-1, 0, -1, 0) end,
	  function(x, y, mask)
	    return simd.select(mask, x + i4(1), y)
	  end)
  op_cost("u32x4 select max",
	  function() return u4(3), u4(5) end,
	  function(x, y) return simd.select(simd.gt(x, y), x, y) end)
  op_cost("float4 select max", function() return fa, fb end,
	  function(x, y) return simd.select(simd.gt(x, y), x, y) end)
  op_cost("float4 select generic",
	  function() return fa, fb, f4(0.0001) end,
	  function(x, y, z)
	    return simd.select(simd.gt(x, y), x + z, y)
	  end)
  op_cost("float4 select false zero",
	  function() return fa, fb, f4(0.0001) end,
	  function(x, y, z)
	    return simd.select(simd.gt(x, y), x + z, f4(0))
	  end)
  op_cost("i32x4 insert const", function() return ia end,
	  function(x) return simd.insert(x, 2, 42) end)
  op_cost("i32x4 insert scalar", function() return ia, 7 end,
	  function(x, y) return simd.insert(x, 2, y) end)
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
  op_cost("i8x16 mulhi", function() return s8(-119), s8(117) end,
	  function(x, y) return simd.mulhi(x, y) end)
  op_cost("i8x16 mulhi square", function() return s8(-119) end,
	  function(x) return simd.mulhi(x, x) end)
  op_cost("u8x16 mulhi", function() return u8(241), u8(233) end,
	  function(x, y) return simd.mulhi(x, y) end)
  op_cost("u8x16 mulhi square", function() return u8(241) end,
	  function(x) return simd.mulhi(x, x) end)
  op_cost("i32x4 mulhi",
	  function() return i4(0x40000000), i4(-123456789) end,
	  function(x, y) return simd.mulhi(x, y) end)
  op_cost("u32x4 mulhi",
	  function() return u4(0xf0000000), u4(0xc0000001) end,
	  function(x, y) return simd.mulhi(x, y) end)
  op_cost("i64x2 mulhi",
	  function()
	    return i64(-0x7000000000000000LL), i64(-12345678901234567LL)
	  end,
	  function(x, y) return simd.mulhi(x, y) end)
  op_cost("u64x2 mulhi",
	  function()
	    return u64(0xf000000000000001ULL), u64(0xc000000000000003ULL)
	  end,
	  function(x, y) return simd.mulhi(x, y) end)
  op_cost("i64x2 mulhi square",
	  function() return i64(-0x7000000000000000LL) end,
	  function(x) return simd.mulhi(x, x) end)
  op_cost("u64x2 mulhi square",
	  function() return u64(0xf000000000000001ULL) end,
	  function(x) return simd.mulhi(x, x) end)
end

io.write("\n== conversion throughput (ns per vector, four independent chains) ==\n")

-- A conversion changes the accumulator's type, so it cannot form the same
-- simple dependency chain as the operations above. Keep four independent
-- source and destination chains live instead. This exposes packed throughput
-- without allowing a loop-invariant conversion to be folded away.
local conversion_sink
local function conversion_throughput(setup, convert)
  local ITERS = 1 << 18
  jit_.flush()
  local best = math.huge
  for _ = 1, REPS do
    local s0, s1, s2, s3, inc, zero = setup()
    local a0, a1, a2, a3 = zero, zero, zero, zero
    local t0 = os.clock()
    for _ = 1, ITERS do
      a0 = simd.bxor(a0, convert(s0))
      a1 = simd.bxor(a1, convert(s1))
      a2 = simd.bxor(a2, convert(s2))
      a3 = simd.bxor(a3, convert(s3))
      s0, s1, s2, s3 = s0+inc, s1+inc, s2+inc, s3+inc
    end
    local dt = os.clock() - t0
    if dt < best then best = dt end
    local sink = simd.bxor(simd.bxor(a0, a1), simd.bxor(a2, a3))
    conversion_sink = tonumber(sink[0])
  end
  return best*1e9/(ITERS*4)
end

local function conversion_cost(name, setup, convert)
  io.write(string.format("  %-24s %6.2f ns\n",
			 name, conversion_throughput(setup, convert)))
end

conversion_cost("i32x4 -> float4", function()
  return i4(-1000, -7, 11, 1001), i4(-999, -5, 14, 1005),
	 i4(-997, -3, 17, 1009), i4(-995, -1, 20, 1013),
	 i4(1, 3, 5, 7), f4(0)
end, function(x) return simd.convert(f4, x) end)
conversion_cost("u32x4 -> float4", function()
  return u4(0, 16777217, 0x80000001, 0xffffffff),
	 u4(1, 16777219, 0x80000003, 0xfffffffd),
	 u4(2, 16777221, 0x80000005, 0xfffffffb),
	 u4(3, 16777223, 0x80000007, 0xfffffff9),
	 u4(1, 3, 5, 7), f4(0)
end, function(x) return simd.convert(f4, x) end)
conversion_cost("float4 -> i32x4", function()
  return f4(-1000.75, -7.5, 11.25, 1001.75),
	 f4(-999.75, -5.5, 14.25, 1005.75),
	 f4(-997.75, -3.5, 17.25, 1009.75),
	 f4(-995.75, -1.5, 20.25, 1013.75),
	 f4(0.25, 0.5, 0.75, 1), i4(0)
end, function(x) return simd.convert(i4, x) end)
conversion_cost("float4 -> u32x4", function()
  return f4(1.75, 7.5, 101.25, 1001.75),
	 f4(2.75, 9.5, 104.25, 1005.75),
	 f4(3.75, 11.5, 107.25, 1009.75),
	 f4(4.75, 13.5, 110.25, 1013.75),
	 f4(0.25, 0.5, 0.75, 1), u4(0)
end, function(x) return simd.convert(u4, x) end)
conversion_cost("i64x2 -> double2", function()
  return i64(-9007199254740993LL, 0x7000000000000001LL),
	 i64(-9007199254740989LL, 0x7000000000000005LL),
	 i64(-9007199254740985LL, 0x7000000000000009LL),
	 i64(-9007199254740981LL, 0x700000000000000dLL),
	 i64(17, 31), d2(0)
end, function(x) return simd.convert(d2, x) end)
conversion_cost("u64x2 -> double2", function()
  return u64(9007199254740993ULL, 0xf000000000000001ULL),
	 u64(9007199254740997ULL, 0xf000000000000005ULL),
	 u64(9007199254741001ULL, 0xf000000000000009ULL),
	 u64(9007199254741005ULL, 0xf00000000000000dULL),
	 u64(17, 31), d2(0)
end, function(x) return simd.convert(d2, x) end)
conversion_cost("double2 -> i64x2", function()
  return d2(-1000000.75, 1000001.75), d2(-999999.5, 1000003.25),
	 d2(-999997.25, 1000005.5), d2(-999995.75, 1000007.25),
	 d2(0.25, 0.5), i64(0)
end, function(x) return simd.convert(i64, x) end)
conversion_cost("double2 -> u64x2", function()
  return d2(1000000.75, 2000001.75), d2(1000002.5, 2000003.25),
	 d2(1000004.25, 2000005.5), d2(1000006.75, 2000007.25),
	 d2(0.25, 0.5), u64(0)
end, function(x) return simd.convert(u64, x) end)

if has_ymm then
  io.write("\n== AVX2 cross-width conversion throughput (ns per vector) ==\n")

  conversion_cost("i8x16 -> i16x16", function()
    return s8(-127), s8(-63), s8(1), s8(65), s8(3), i16w(0)
  end, function(x) return simd.convert(i16w, x) end)
  conversion_cost("i16x16 -> i8x16", function()
    return i16w(0x1234), i16w(-1), i16w(0x80), i16w(0x7f),
	   i16w(257), s8(0)
  end, function(x) return simd.convert(s8, x) end)
  conversion_cost("i16x8 -> float8", function()
    return i16(-30000, -7, -1, 0, 1, 7, 100, 30000),
	   i16(-29997, -4, 2, 3, 4, 10, 103, 30003),
	   i16(-29994, -1, 5, 6, 7, 13, 106, 30006),
	   i16(-29991, 2, 8, 9, 10, 16, 109, 30009),
	   i16(3), f8(0)
  end, function(x) return simd.convert(f8, x) end)
  conversion_cost("float8 -> i16x8", function()
    return f8(-32768, -32768.5, 32767, 32767.5,
	      -100.75, -1.9, 1.9, 100.75),
	   f8(-32767, -32769, 32766, 32768,
	      -99.75, -0.9, 2.9, 101.75),
	   f8(-32766, -32770, 32765, 32769,
	      -98.75, 0.1, 3.9, 102.75),
	   f8(-32765, -32771, 32764, 32770,
	      -97.75, 1.1, 4.9, 103.75),
	   f8(0.25), i16(0)
  end, function(x) return simd.convert(i16, x) end)
  conversion_cost("float4 -> double4", function()
    return f4(-1000.75, -7.5, 11.25, 1001.75),
	   f4(-999.75, -5.5, 14.25, 1005.75),
	   f4(-997.75, -3.5, 17.25, 1009.75),
	   f4(-995.75, -1.5, 20.25, 1013.75),
	   f4(0.25, 0.5, 0.75, 1), d4(0)
  end, function(x) return simd.convert(d4, x) end)
  conversion_cost("double4 -> float4", function()
    return d4(-1000.75, -7.5, 11.25, 1001.75),
	   d4(-999.75, -5.5, 14.25, 1005.75),
	   d4(-997.75, -3.5, 17.25, 1009.75),
	   d4(-995.75, -1.5, 20.25, 1013.75),
	   d4(0.25, 0.5, 0.75, 1), f4(0)
  end, function(x) return simd.convert(f4, x) end)
  conversion_cost("u32x4 -> double4", function()
    return u4(0, 1, 0x80000001, 0xffffffff),
	   u4(1, 2, 0x80000003, 0xfffffffd),
	   u4(2, 3, 0x80000005, 0xfffffffb),
	   u4(3, 4, 0x80000007, 0xfffffff9),
	   u4(1, 3, 5, 7), d4(0)
  end, function(x) return simd.convert(d4, x) end)
  conversion_cost("i64x4 -> float4", function()
    return i64w(0x4000004000000001LL, -0x4000004000000001LL, 0, 1),
	   i64w(0x4000004000000005LL, -0x4000003ffffffffdLL, 4, 5),
	   i64w(0x4000004000000009LL, -0x4000003ffffffff9LL, 8, 9),
	   i64w(0x400000400000000dLL, -0x4000003ffffffff5LL, 12, 13),
	   i64w(17), f4(0)
  end, function(x) return simd.convert(f4, x) end)
  conversion_cost("u64x4 -> float4", function()
    return u64w(0x8000008000000001ULL, 0x4000004000000001ULL, 0, 1),
	   u64w(0x8000008000000005ULL, 0x4000004000000005ULL, 4, 5),
	   u64w(0x8000008000000009ULL, 0x4000004000000009ULL, 8, 9),
	   u64w(0x800000800000000dULL, 0x400000400000000dULL, 12, 13),
	   u64w(17), f4(0)
  end, function(x) return simd.convert(f4, x) end)

  io.write("\n== VEX-clean mixed scalar/YMM throughput ==\n")
  local transition_sink
  local function transition_cost(kind)
    local ITERS = 1 << 20
    local inc = f8(0.001)
    jit_.flush()
    local best = math.huge
    for _ = 1, REPS do
      local v, s = f8(1), 1.0
      local t0 = os.clock()
      if kind == "scalar" then
	for _ = 1, ITERS do v = v + inc; s = s + 0.25 end
      elseif kind == "mod37" then
	for i = 1, ITERS do v = v + inc; s = s + i % 37 end
      else
	for _ = 1, ITERS do v = v + inc end
      end
      local dt = os.clock() - t0
      if dt < best then best = dt end
      transition_sink = tonumber(v[0]) + s
    end
    return best*1e9/ITERS
  end
  local ymm_only = transition_cost("ymm")
  local ymm_scalar = transition_cost("scalar")
  local ymm_mod37 = transition_cost("mod37")
  io.write(string.format(
    "  YMM add only %6.2f ns  + Lua scalar add %6.2f ns  %5.2fx\n",
    ymm_only, ymm_scalar, ymm_scalar/ymm_only))
  io.write(string.format(
    "  YMM add only %6.2f ns  + integer %% 37  %6.2f ns  %5.2fx\n",
    ymm_only, ymm_mod37, ymm_mod37/ymm_only))

  io.write("\n== AVX2 width comparison (dependent ns/op) ==\n")
  io.write("                                      XMM       YMM   lane throughput\n")

  local function width_cost(name, setup_xmm, setup_ymm, step_xmm, step_ymm)
    local tx = op_latency(setup_xmm, step_xmm)
    local ty = op_latency(setup_ymm, step_ymm or step_xmm)
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
  width_cost("float select max",
    function() return f4(1.0009765625), f4(1.00048828125) end,
    function() return f8(1.0009765625), f8(1.00048828125) end,
    function(x, y) return simd.select(simd.gt(x, y), x, y) end)
  width_cost("float select generic",
    function()
      return f4(1.0009765625), f4(1.00048828125), f4(0.0001)
    end,
    function()
      return f8(1.0009765625), f8(1.00048828125), f8(0.0001)
    end,
    function(x, y, z) return simd.select(simd.gt(x, y), x + z, y) end)
  width_cost("float select zero",
    function()
      return f4(1.0009765625), f4(1.00048828125), f4(0.0001)
    end,
    function()
      return f8(1.0009765625), f8(1.00048828125), f8(0.0001)
    end,
    function(x, y, z)
      return simd.select(simd.gt(x, y), x + z, f4(0))
    end,
    function(x, y, z)
      return simd.select(simd.gt(x, y), x + z, f8(0))
    end)
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
  width_cost("int32 add lane literal",
    function() return i4(3), i4(0) end,
    function() return i8(3), i8(0) end,
    function(x)
      return x + i4(1, 2, 3, 4)
    end,
    function(x)
      return x + i8(1, 2, 3, 4, 5, 6, 7, 8)
    end)
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
  width_cost("int32 rol 8 idiom",
    function() return i4(0x1020304), i4(0) end,
    function() return i8(0x1020304), i8(0) end,
    function(x)
      return simd.bor(simd.shl(x, 8), simd.shr(x, 24))
    end)
  width_cost("int16 shl per-lane",
    function() return i16(3), i16(0, 1, 2, 3, 4, 5, 6, 7) end,
    function()
      return i16w(3), i16w(0, 1, 2, 3, 4, 5, 6, 7,
			    0, 1, 2, 3, 4, 5, 6, 7)
    end,
    function(x, y) return simd.shl(x, y) end)
  width_cost("int16 shr per-lane",
    function()
      return i16(-12345), i16(0, 1, 15, 16, 2, 7, 14, 20)
    end,
    function()
      return i16w(-12345), i16w(0, 1, 15, 16, 2, 7, 14, 20,
				   3, 6, 9, 12, 17, 18, 4, 11)
    end,
    function(x, y) return simd.shr(x, y) end)
  width_cost("int16 sar per-lane",
    function()
      return i16(-12345), i16(0, 1, 15, 16, 2, 7, 14, 20)
    end,
    function()
      return i16w(-12345), i16w(0, 1, 15, 16, 2, 7, 14, 20,
				   3, 6, 9, 12, 17, 18, 4, 11)
    end,
    function(x, y) return simd.sar(x, y) end)
  width_cost("int8 shl per-lane",
    function()
      return s8(3), s8(0, 1, 2, 3, 4, 5, 6, 7,
		       0, 1, 2, 3, 4, 5, 6, 7)
    end,
    function()
      return s8w(3), s8w(0, 1, 2, 3, 4, 5, 6, 7,
			 0, 1, 2, 3, 4, 5, 6, 7)
    end,
    function(x, y) return simd.shl(x, y) end)
  width_cost("int8 shr per-lane",
    function()
      return s8(-119), s8(0, 1, 2, 3, 4, 5, 6, 7,
			  0, 1, 2, 3, 4, 5, 6, 7)
    end,
    function()
      return s8w(-119), s8w(0, 1, 2, 3, 4, 5, 6, 7,
			      0, 1, 2, 3, 4, 5, 6, 7)
    end,
    function(x, y) return simd.shr(x, y) end)
  width_cost("int8 sar per-lane",
    function()
      return s8(-119), s8(0, 1, 2, 3, 4, 5, 6, 7,
			  0, 1, 2, 3, 4, 5, 6, 7)
    end,
    function()
      return s8w(-119), s8w(0, 1, 2, 3, 4, 5, 6, 7,
			      0, 1, 2, 3, 4, 5, 6, 7)
    end,
    function(x, y) return simd.sar(x, y) end)
  width_cost("int32 select max",
    function() return i4(3), i4(5) end,
    function() return i8(3), i8(5) end,
    function(x, y) return simd.select(simd.gt(x, y), x, y) end)
  width_cost("int32 select ge max",
    function() return i4(3), i4(5) end,
    function() return i8(3), i8(5) end,
    function(x, y) return simd.select(simd.ge(x, y), x, y) end)
  width_cost("int32 select abs",
    function() return i4(-7) end,
    function() return i8(-7) end,
    function(x) return simd.select(simd.gt(x, i4(0)), x, -x) end,
    function(x) return simd.select(simd.gt(x, i8(0)), x, -x) end)
  width_cost("int32 restore sign",
    function() return i4(7), i4(-1, 0, 1, -2) end,
    function() return i8(7), i8(-1, 0, 1, -2, 2, -3, 3, -4) end,
    function(x, sign)
      return simd.select(simd.ge(sign, i4(0)), x, -x)
    end,
    function(x, sign)
      return simd.select(simd.ge(sign, i8(0)), x, -x)
    end)
  width_cost("int32 select generic",
    function() return i4(3), i4(5), i4(1) end,
    function() return i8(3), i8(5), i8(1) end,
    function(x, y, z) return simd.select(simd.gt(x, y), x + z, y) end)
  width_cost("int32 select zero",
    function() return i4(3), i4(5), i4(1) end,
    function() return i8(3), i8(5), i8(1) end,
    function(x, y, z)
      return simd.select(simd.gt(x, y), x + z, i4(0))
    end,
    function(x, y, z)
      return simd.select(simd.gt(x, y), x + z, i8(0))
    end)
  width_cost("int32 select const mask",
    function() return i4(3), i4(5), i4(1) end,
    function() return i8(3), i8(5), i8(1) end,
    function(x, y, z)
      return simd.select(i4(-1, 0, -1, 0), x + z, y)
    end,
    function(x, y, z)
      return simd.select(i8(-1, 0, -1, 0, -1, 0, -1, 0), x + z, y)
    end)
  width_cost("uint32 select max",
    function() return u4(3), u4(5) end,
    function() return u8d(3), u8d(5) end,
    function(x, y) return simd.select(simd.gt(x, y), x, y) end)
  width_cost("int32 insert const",
    function() return i4(3) end,
    function() return i8(3) end,
    function(x) return simd.insert(x, 2, 42) end,
    function(x) return simd.insert(x, 5, 42) end)
  width_cost("int32 insert scalar",
    function() return i4(3), 7 end,
    function() return i8(3), 7 end,
    function(x, y) return simd.insert(x, 2, y) end,
    function(x, y) return simd.insert(x, 5, y) end)
  width_cost("int32 shuffle const",
    function() return i4(0, 1, 2, 3), i4(0) end,
    function() return i8(0, 1, 2, 3, 4, 5, 6, 7), i8(0) end,
    function(x) return simd.shuffle(x, 3, 2, 1, 0) end,
    function(x) return simd.shuffle(x, 7, 6, 5, 4, 3, 2, 1, 0) end)
  width_cost("int32 shuffle local",
    function() return i4(0, 1, 2, 3), i4(0) end,
    function() return i8(0, 1, 2, 3, 4, 5, 6, 7), i8(0) end,
    function(x) return simd.shuffle(x, 3, 2, 1, 0) end,
    function(x) return simd.shuffle(x, 3, 2, 1, 0, 7, 6, 5, 4) end)
  width_cost("int32 shuffle2 unpack",
    function() return i4(0, 1, 2, 3), i4(4, 5, 6, 7) end,
    function() return i8(0, 1, 2, 3, 4, 5, 6, 7),
		      i8(8, 9, 10, 11, 12, 13, 14, 15) end,
    function(x, y) return simd.shuffle2(x, y, 0, 4, 1, 5) end,
    function(x, y)
      return simd.shuffle2(x, y, 0, 8, 1, 9, 4, 12, 5, 13)
    end)
  width_cost("int32 shuffle2 one src",
    function() return i4(0, 1, 2, 3), i4(4, 5, 6, 7) end,
    function() return i8(0, 1, 2, 3, 4, 5, 6, 7),
		      i8(8, 9, 10, 11, 12, 13, 14, 15) end,
    function(x, y) return simd.shuffle2(x, y, 3, 2, 1, 0) end,
    function(x, y)
      return simd.shuffle2(x, y, 7, 6, 5, 4, 3, 2, 1, 0)
    end)
  width_cost("int32 shuffle2 direct",
    function() return i4(0, 1, 2, 3), i4(4, 5, 6, 7) end,
    function() return i8(0, 1, 2, 3, 4, 5, 6, 7),
		      i8(8, 9, 10, 11, 12, 13, 14, 15) end,
    function(x, y) return simd.shuffle2(x, y, 3, 0, 6, 5) end,
    function(x, y)
      return simd.shuffle2(x, y, 3, 0, 10, 9, 7, 4, 14, 13)
    end)
  width_cost("int32 shuffle2 blend",
    function() return i4(0, 1, 2, 3), i4(4, 5, 6, 7) end,
    function() return i8(0, 1, 2, 3, 4, 5, 6, 7),
		      i8(8, 9, 10, 11, 12, 13, 14, 15) end,
    function(x, y) return simd.shuffle2(x, y, 0, 5, 2, 7) end,
    function(x, y)
      return simd.shuffle2(x, y, 0, 9, 2, 11, 4, 13, 6, 15)
    end)
  width_cost("int32 shuffle2 blend full",
    function() return i4(0, 1, 2, 3), i4(4, 5, 6, 7) end,
    function() return i8(0, 1, 2, 3, 4, 5, 6, 7),
		      i8(8, 9, 10, 11, 12, 13, 14, 15) end,
    function(x, y) return simd.shuffle2(x, y, 0, 5, 2, 7) end,
    function(x, y)
      return simd.shuffle2(x, y, 0, 9, 2, 3, 12, 5, 14, 7)
    end)
  width_cost("int32 shuffle2 align lane",
    function() return i4(0, 1, 2, 3), i4(4, 5, 6, 7) end,
    function() return i8(0, 1, 2, 3, 4, 5, 6, 7),
		      i8(8, 9, 10, 11, 12, 13, 14, 15) end,
    function(x, y) return simd.shuffle2(x, y, 5, 6, 7, 0) end,
    function(x, y)
      return simd.shuffle2(x, y, 9, 10, 11, 0, 13, 14, 15, 4)
    end)
  width_cost("int32 shuffle2 align full",
    function() return i4(0, 1, 2, 3), i4(4, 5, 6, 7) end,
    function() return i8(0, 1, 2, 3, 4, 5, 6, 7),
		      i8(8, 9, 10, 11, 12, 13, 14, 15) end,
    function(x, y) return simd.shuffle2(x, y, 1, 2, 3, 4) end,
    function(x, y)
      return simd.shuffle2(x, y, 1, 2, 3, 4, 5, 6, 7, 8)
    end)
  width_cost("int32 shuffle vector",
    function() return i4(0, 1, 2, 3), i4(3, 2, 1, 0) end,
    function() return i8(0, 1, 2, 3, 4, 5, 6, 7),
		      i8(7, 6, 5, 4, 3, 2, 1, 0) end,
    function(x, y) return simd.shuffle(x, y) end)
  width_cost("int8 mul",
    function() return s8(1), s8(3) end,
    function() return s8w(1), s8w(3) end,
    function(x, y) return x * y end)
  width_cost("int16 mulhi",
    function() return i16(30000), i16(30000) end,
    function() return i16w(30000), i16w(30000) end,
    function(x, y) return simd.mulhi(x, y) end)
  width_cost("int32 mulhi",
    function() return i4(0x40000000), i4(-123456789) end,
    function() return i8(0x40000000), i8(-123456789) end,
    function(x, y) return simd.mulhi(x, y) end)
  width_cost("uint32 mulhi",
    function() return u4(0xf0000000), u4(0xc0000001) end,
    function() return u8d(0xf0000000), u8d(0xc0000001) end,
    function(x, y) return simd.mulhi(x, y) end)
  width_cost("int64 mulhi",
    function()
      return i64(-0x7000000000000000LL), i64(-12345678901234567LL)
    end,
    function()
      return i64w(-0x7000000000000000LL), i64w(-12345678901234567LL)
    end,
    function(x, y) return simd.mulhi(x, y) end)
  width_cost("uint64 mulhi",
    function()
      return u64(0xf000000000000001ULL), u64(0xc000000000000003ULL)
    end,
    function()
      return u64w(0xf000000000000001ULL), u64w(0xc000000000000003ULL)
    end,
    function(x, y) return simd.mulhi(x, y) end)
  width_cost("int64 mulhi square",
    function() return i64(-0x7000000000000000LL) end,
    function() return i64w(-0x7000000000000000LL) end,
    function(x) return simd.mulhi(x, x) end)
  width_cost("uint64 mulhi square",
    function() return u64(0xf000000000000001ULL) end,
    function() return u64w(0xf000000000000001ULL) end,
    function(x) return simd.mulhi(x, x) end)
  width_cost("int8 mulhi",
    function() return s8(-119), s8(117) end,
    function() return s8w(-119), s8w(117) end,
    function(x, y) return simd.mulhi(x, y) end)
  width_cost("int8 mulhi square",
    function() return s8(-119) end,
    function() return s8w(-119) end,
    function(x) return simd.mulhi(x, x) end)
  width_cost("uint8 mulhi",
    function() return u8(241), u8(233) end,
    function() return u8w(241), u8w(233) end,
    function(x, y) return simd.mulhi(x, y) end)
  width_cost("uint8 mulhi square",
    function() return u8(241) end,
    function() return u8w(241) end,
    function(x) return simd.mulhi(x, x) end)
  width_cost("uint8 saturated add",
    function() return u8(1), u8(3) end,
    function() return u8w(1), u8w(3) end,
    function(x, y) return simd.adds(x, y) end)

  io.write("\n== AVX2 conversion width comparison (ns/vector) ==\n")
  io.write("                                      XMM       YMM   lane throughput\n")

  local function conversion_width_cost(name, setup_xmm, setup_ymm,
				       convert_xmm, convert_ymm)
    local tx = conversion_throughput(setup_xmm, convert_xmm)
    local ty = conversion_throughput(setup_ymm, convert_ymm)
    io.write(string.format("  %-24s %6.2f ns  %6.2f ns      %5.2fx\n",
			   name, tx, ty, 2*tx/ty))
  end

  conversion_width_cost("uint32 -> float",
    function()
      return u4(0, 16777217, 0x80000001, 0xffffffff),
	     u4(1, 16777219, 0x80000003, 0xfffffffd),
	     u4(2, 16777221, 0x80000005, 0xfffffffb),
	     u4(3, 16777223, 0x80000007, 0xfffffff9),
	     u4(1, 3, 5, 7), f4(0)
    end,
    function()
      return u8d(0, 1, 16777217, 0x7fffffff,
		 0x80000001, 0xffffff01, 0xfffffffe, 0xffffffff),
	     u8d(1, 2, 16777219, 0x7ffffffd,
		 0x80000003, 0xfffffeff, 0xfffffffc, 0xfffffffd),
	     u8d(2, 3, 16777221, 0x7ffffffb,
		 0x80000005, 0xfffffefd, 0xfffffffa, 0xfffffffb),
	     u8d(3, 4, 16777223, 0x7ffffff9,
		 0x80000007, 0xfffffefb, 0xfffffff8, 0xfffffff9),
	     u8d(1, 3, 5, 7, 11, 13, 17, 19), f8(0)
    end,
    function(x) return simd.convert(f4, x) end,
    function(x) return simd.convert(f8, x) end)
  conversion_width_cost("int64 -> double",
    function()
      return i64(-9007199254740993LL, 0x7000000000000001LL),
	     i64(-9007199254740989LL, 0x7000000000000005LL),
	     i64(-9007199254740985LL, 0x7000000000000009LL),
	     i64(-9007199254740981LL, 0x700000000000000dLL),
	     i64(17, 31), d2(0)
    end,
    function()
      return i64w(-9007199254740993LL, 9007199254740993LL,
		    -0x7000000000000000LL, 0x7000000000000001LL),
	     i64w(-9007199254740989LL, 9007199254740997LL,
		    -0x6ffffffffffffffcLL, 0x7000000000000005LL),
	     i64w(-9007199254740985LL, 9007199254741001LL,
		    -0x6ffffffffffffff8LL, 0x7000000000000009LL),
	     i64w(-9007199254740981LL, 9007199254741005LL,
		    -0x6ffffffffffffff4LL, 0x700000000000000dLL),
	     i64w(17, 31, 47, 61), d4(0)
    end,
    function(x) return simd.convert(d2, x) end,
    function(x) return simd.convert(d4, x) end)
  conversion_width_cost("uint64 -> double",
    function()
      return u64(9007199254740993ULL, 0xf000000000000001ULL),
	     u64(9007199254740997ULL, 0xf000000000000005ULL),
	     u64(9007199254741001ULL, 0xf000000000000009ULL),
	     u64(9007199254741005ULL, 0xf00000000000000dULL),
	     u64(17, 31), d2(0)
    end,
    function()
      return u64w(0, 9007199254740993ULL, 0x8000000000000001ULL,
		    0xffffffffffffffffULL),
	     u64w(1, 9007199254740997ULL, 0x8000000000000005ULL,
		    0xfffffffffffffffbULL),
	     u64w(2, 9007199254741001ULL, 0x8000000000000009ULL,
		    0xfffffffffffffff7ULL),
	     u64w(3, 9007199254741005ULL, 0x800000000000000dULL,
		    0xfffffffffffffff3ULL),
	     u64w(17, 31, 47, 61), d4(0)
    end,
    function(x) return simd.convert(d2, x) end,
    function(x) return simd.convert(d4, x) end)
  conversion_width_cost("double -> int64",
    function()
      return d2(-1000000.75, 1000001.75), d2(-999999.5, 1000003.25),
	     d2(-999997.25, 1000005.5), d2(-999995.75, 1000007.25),
	     d2(0.25, 0.5), i64(0)
    end,
    function()
      return d4(-1000000.75, -7.5, 11.25, 1000001.75),
	     d4(-999999.5, -5.25, 14.5, 1000003.25),
	     d4(-999997.25, -3.5, 17.75, 1000005.5),
	     d4(-999995.75, -1.25, 20.5, 1000007.25),
	     d4(0.25, 0.5, 0.75, 1), i64w(0)
    end,
    function(x) return simd.convert(i64, x) end,
    function(x) return simd.convert(i64w, x) end)
  conversion_width_cost("double -> uint64",
    function()
      return d2(1000000.75, 2000001.75), d2(1000002.5, 2000003.25),
	     d2(1000004.25, 2000005.5), d2(1000006.75, 2000007.25),
	     d2(0.25, 0.5), u64(0)
    end,
    function()
      return d4(1000000.75, 2000001.75, 3000002.25, 4000003.5),
	     d4(1000002.5, 2000003.25, 3000004.5, 4000005.75),
	     d4(1000004.25, 2000005.5, 3000006.75, 4000007.25),
	     d4(1000006.75, 2000007.25, 3000008.5, 4000009.75),
	     d4(0.25, 0.5, 0.75, 1), u64w(0)
    end,
    function(x) return simd.convert(u64, x) end,
    function(x) return simd.convert(u64w, x) end)
end

if failures > 0 then
  io.write(string.format("\n%d benchmark(s) produced a wrong result\n", failures))
  os.exit(1)
end
