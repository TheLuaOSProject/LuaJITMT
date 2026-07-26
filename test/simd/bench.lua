-- SIMD benchmarks: scalar vs. 128-bit XMM vs. 256-bit AVX2 code.
--
--   luajit test/simd/bench.lua [reps]
--
-- Every benchmark consumes its result, so nothing can be optimised away.
collectgarbage("stop")

local dir = arg[0]:match("^(.*)[/\\][^/\\]*$") or "."
package.path = dir .. "/?.lua;" .. package.path

local ffi = require("ffi")
local simd = require("ffi.simd")
local T = require("simdtest")
local jit_ = require("jit")

local REPS = tonumber(arg[1] or "5")
local N = 1 << 16           -- elements per pass
local PASSES = 200

local f4 = ffi.typeof("float4")
local d2 = ffi.typeof("double2")
local i4 = ffi.typeof("i32x4")
local u4 = ffi.typeof("u32x4")
local i64x2 = ffi.typeof("i64x2")
local features = simd.features()
local has_ymm = features.avx2 and features.vecsize >= 32
local f8 = has_ymm and ffi.typeof("float8")
local d4 = has_ymm and ffi.typeof("double4")
local i8 = has_ymm and ffi.typeof("i32x8")
local u8 = has_ymm and ffi.typeof("u32x8")
local i64x4 = has_ymm and ffi.typeof("i64x4")
local f4p = ffi.typeof("$ *", f4)
local d2p = ffi.typeof("$ *", d2)
local f8p = has_ymm and ffi.typeof("$ *", f8)
local d4p = has_ymm and ffi.typeof("$ *", d4)
local failures = 0

local function bench(name, f, ...)
  local best, sink = math.huge, nil
  for _ = 1, REPS do
    local t0 = os.clock()
    sink = f(...)
    local dt = os.clock() - t0
    if dt < best then best = dt end
  end
  return best, sink
end

local function equal(a, b, cmp)
  return cmp and cmp(a, b) or a == b
end

local function approx(a, b, tol)
  a, b = tonumber(a), tonumber(b)
  local scale = math.max(1, math.abs(a), math.abs(b))
  return math.abs(a-b) <= (tol or 1e-5) * scale
end

local function report(name, tscalar, tvector, ss, sv, cmp)
  io.write(string.format("%-26s scalar %7.1f ms   XMM %7.1f ms   %5.2fx\n",
			 name, tscalar*1000, tvector*1000, tscalar/tvector))
  if not equal(ss, sv, cmp) then
    failures = failures + 1
    io.write(string.format("   ! results differ: %s vs %s\n",
			   tostring(ss), tostring(sv)))
  end
end

local function report_ymm(name, ts, tx, ty, ss, sx, sy, cmp)
  io.write(string.format(
    "%-26s scalar %7.1f ms   XMM %7.1f ms %5.2fx   YMM %7.1f ms %5.2fx (%4.2fx/XMM)\n",
    name, ts*1000, tx*1000, ts/tx, ty*1000, ts/ty, tx/ty))
  if not equal(ss, sx, cmp) or not equal(ss, sy, cmp) then
    failures = failures + 1
    io.write(string.format("   ! results differ: %s vs %s vs %s\n",
			   tostring(ss), tostring(sx), tostring(sy)))
  end
end

-- Read a sparse but deterministic checksum after a buffer-producing kernel.
-- Keeping this outside the hot loop prevents the validation reduction from
-- becoming the thing being measured.
local function sample_sum(buf, n)
  local sum = 0
  local step = math.max(1, math.floor(n / 257))
  for i = 0, n-1, step do sum = sum + tonumber(buf[i]) end
  return sum
end

------------------------------------------------------------------ saxpy ----

local xs = ffi.new("float[?]", N)
local ys = ffi.new("float[?]", N)
local ys_xmm = ffi.new("float[?]", N)
local ys_ymm = has_ymm and ffi.new("float[?]", N)
for i = 0, N-1 do
  xs[i] = i % 17
  ys[i] = i % 13
  ys_xmm[i] = ys[i]
  if has_ymm then ys_ymm[i] = ys[i] end
end

local xv = ffi.cast(ffi.typeof("$ *", f4), xs)
local yv = ffi.cast(ffi.typeof("$ *", f4), ys_xmm)
local NV = N / 4
local xw = has_ymm and ffi.cast(ffi.typeof("$ *", f8), xs)
local yw = has_ymm and ffi.cast(ffi.typeof("$ *", f8), ys_ymm)
local NW = N / 8

local function saxpy_scalar()
  local a = 2.5
  local s = 0
  for _ = 1, PASSES do
    for i = 0, N-1 do ys[i] = a * xs[i] + ys[i] end
    s = s + ys[0]
  end
  return s + ys[1]
end

local function saxpy_vector()
  local a = f4(2.5)
  local s = 0
  for _ = 1, PASSES do
    for i = 0, NV-1 do yv[i] = a * xv[i] + yv[i] end
    s = s + yv[0][0]
  end
  return s + yv[0][1]
end

local function saxpy_ymm()
  local a = f8(2.5)
  local s = 0
  for _ = 1, PASSES do
    for i = 0, NW-1 do yw[i] = a * xw[i] + yw[i] end
    s = s + yw[0][0]
  end
  return s + yw[0][1]
end

------------------------------------------------------------------- dot ------

local function dot_scalar()
  local s = 0
  for _ = 1, PASSES do
    local acc = 0
    for i = 0, N-1 do acc = acc + xs[i] * xs[i] end
    s = s + acc
  end
  return s
end

local function dot_vector()
  local s = 0
  for _ = 1, PASSES do
    local acc = f4(0)
    for i = 0, NV-1 do local v = xv[i]; acc = acc + v * v end
    s = s + simd.hsum(acc)
  end
  return s
end

local function dot_ymm()
  local s = 0
  for _ = 1, PASSES do
    local acc = f8(0)
    for i = 0, NW-1 do local v = xw[i]; acc = acc + v * v end
    for i = 0, 7 do s = s + acc[i] end
  end
  return s
end

-------------------------------------------------------------- integer max ---

local is = ffi.new("int32_t[?]", N)
for i = 0, N-1 do is[i] = (i * 2654435761) % 1000003 - 500000 end
local iv = ffi.cast(ffi.typeof("$ *", i4), is)
local iw = has_ymm and ffi.cast(ffi.typeof("$ *", i8), is)

local function max_scalar()
  local s = 0
  for _ = 1, PASSES do
    local m = -2147483648
    for i = 0, N-1 do local x = is[i]; if x > m then m = x end end
    s = s + m
  end
  return s
end

local function max_vector()
  local s = 0
  for _ = 1, PASSES do
    local m = i4(-2147483648)
    for i = 0, NV-1 do m = simd.max(m, iv[i]) end
    s = s + simd.hmax(m)
  end
  return s
end

local function max_ymm()
  local s = 0
  for _ = 1, PASSES do
    local m = i8(-2147483648)
    for i = 0, NW-1 do m = simd.max(m, iw[i]) end
    s = s + simd.hmax(m)
  end
  return s
end

----------------------------------------------------------------- clamp ------

local function clamp_scalar()
  local s = 0
  for _ = 1, PASSES do
    for i = 0, N-1 do
      local x = xs[i]
      if x < 1 then x = 1 elseif x > 9 then x = 9 end
      ys[i] = x
    end
    s = s + ys[0]
  end
  return s
end

local function clamp_vector()
  local lo, hi = f4(1), f4(9)
  local s = 0
  for _ = 1, PASSES do
    for i = 0, NV-1 do yv[i] = simd.min(simd.max(xv[i], lo), hi) end
    s = s + yv[0][0]
  end
  return s
end

local function clamp_ymm()
  local lo, hi = f8(1), f8(9)
  local s = 0
  for _ = 1, PASSES do
    for i = 0, NW-1 do yw[i] = simd.min(simd.max(xw[i], lo), hi) end
    s = s + yw[0][0]
  end
  return s
end

--------------------------------------------------------- delta encoding ----

-- First differences are used by time-series, audio and columnar codecs.
-- Carrying the current block forward means each iteration performs one new
-- load; shuffle2 constructs the one-lane-shifted window across the block
-- boundary. XMM maps that to PALIGNR, while YMM uses one half bridge plus
-- PALIGNR instead of reloading/scalarising the boundary lanes.
local DELTA_N = 1 << 18
local DELTA_PASSES = 80
local delta_in = ffi.new("int32_t[?]", DELTA_N + 8)
local delta_out_s = ffi.new("int32_t[?]", DELTA_N)
local delta_out_x = ffi.new("int32_t[?]", DELTA_N)
local delta_out_y = has_ymm and ffi.new("int32_t[?]", DELTA_N)
for i = 0, DELTA_N+7 do
  delta_in[i] = (i * 1103515245 + 12345) % 1000003
end
local delta_i4 = ffi.cast(ffi.typeof("$ *", i4), delta_in)
local delta_o4 = ffi.cast(ffi.typeof("$ *", i4), delta_out_x)
local delta_i8 = has_ymm and ffi.cast(ffi.typeof("$ *", i8), delta_in)
local delta_o8 = has_ymm and ffi.cast(ffi.typeof("$ *", i8), delta_out_y)

local function delta_scalar()
  for _ = 1, DELTA_PASSES do
    for i = 0, DELTA_N-1 do
      delta_out_s[i] = delta_in[i+1] - delta_in[i]
    end
  end
  return sample_sum(delta_out_s, DELTA_N)
end

local function delta_xmm()
  for _ = 1, DELTA_PASSES do
    local cur = delta_i4[0]
    for i = 0, DELTA_N/4-1 do
      local next = delta_i4[i+1]
      delta_o4[i] = simd.shuffle2(cur, next, 1, 2, 3, 4) - cur
      cur = next
    end
  end
  return sample_sum(delta_out_x, DELTA_N)
end

local function delta_ymm()
  for _ = 1, DELTA_PASSES do
    local cur = delta_i8[0]
    for i = 0, DELTA_N/8-1 do
      local next = delta_i8[i+1]
      delta_o8[i] = simd.shuffle2(cur, next, 1, 2, 3, 4, 5, 6, 7, 8) - cur
      cur = next
    end
  end
  return sample_sum(delta_out_y, DELTA_N)
end

---------------------------------------------------------- eight-tap FIR -----

-- Overlapping loads make this a useful test of sustained unaligned SIMD
-- throughput.  Every output performs the same ordered chain of eight
-- multiply-adds, so scalar, XMM, and YMM results are directly comparable.
local FIR_N = 1 << 15
local FIR_PASSES = 60
local fir_in = ffi.new("float[?]", FIR_N + 7)
local fir_scalar_out = ffi.new("float[?]", FIR_N)
local fir_xmm_out = ffi.new("float[?]", FIR_N)
local fir_ymm_out = has_ymm and ffi.new("float[?]", FIR_N)
for i = 0, FIR_N+6 do
  fir_in[i] = ((i * 37) % 1024 - 512) / 256
end

local fc0, fc1, fc2, fc3 = 0.125, -0.25, 0.375, 0.5
local fc4, fc5, fc6, fc7 = -0.5, 0.25, -0.125, 0.0625
local fir_x4 = {}
for k = 0, 7 do fir_x4[k] = ffi.cast(f4p, fir_in + k) end
local fir_y4 = ffi.cast(f4p, fir_xmm_out)
local fir_c4 = {
  [0] = f4(fc0), f4(fc1), f4(fc2), f4(fc3),
  f4(fc4), f4(fc5), f4(fc6), f4(fc7),
}

local fir_x8, fir_y8, fir_c8
if has_ymm then
  fir_x8 = {}
  for k = 0, 7 do fir_x8[k] = ffi.cast(f8p, fir_in + k) end
  fir_y8 = ffi.cast(f8p, fir_ymm_out)
  fir_c8 = {
    [0] = f8(fc0), f8(fc1), f8(fc2), f8(fc3),
    f8(fc4), f8(fc5), f8(fc6), f8(fc7),
  }
end

local function fir_scalar()
  local x, y = fir_in, fir_scalar_out
  for _ = 1, FIR_PASSES do
    for i = 0, FIR_N-1 do
      local acc = fc0 * x[i]
      acc = acc + fc1 * x[i+1]
      acc = acc + fc2 * x[i+2]
      acc = acc + fc3 * x[i+3]
      acc = acc + fc4 * x[i+4]
      acc = acc + fc5 * x[i+5]
      acc = acc + fc6 * x[i+6]
      y[i] = acc + fc7 * x[i+7]
    end
  end
  return y[0] + y[FIR_N/2] + y[FIR_N-1]
end

local function fir_xmm()
  local x0, x1, x2, x3 = fir_x4[0], fir_x4[1], fir_x4[2], fir_x4[3]
  local x4, x5, x6, x7 = fir_x4[4], fir_x4[5], fir_x4[6], fir_x4[7]
  local c0, c1, c2, c3 = fir_c4[0], fir_c4[1], fir_c4[2], fir_c4[3]
  local c4, c5, c6, c7 = fir_c4[4], fir_c4[5], fir_c4[6], fir_c4[7]
  for _ = 1, FIR_PASSES do
    for i = 0, FIR_N/4-1 do
      local acc = c0 * x0[i]
      acc = acc + c1 * x1[i]
      acc = acc + c2 * x2[i]
      acc = acc + c3 * x3[i]
      acc = acc + c4 * x4[i]
      acc = acc + c5 * x5[i]
      acc = acc + c6 * x6[i]
      fir_y4[i] = acc + c7 * x7[i]
    end
  end
  return fir_xmm_out[0] + fir_xmm_out[FIR_N/2] +
	 fir_xmm_out[FIR_N-1]
end

local function fir_ymm()
  local x0, x1, x2, x3 = fir_x8[0], fir_x8[1], fir_x8[2], fir_x8[3]
  local x4, x5, x6, x7 = fir_x8[4], fir_x8[5], fir_x8[6], fir_x8[7]
  local c0, c1, c2, c3 = fir_c8[0], fir_c8[1], fir_c8[2], fir_c8[3]
  local c4, c5, c6, c7 = fir_c8[4], fir_c8[5], fir_c8[6], fir_c8[7]
  for _ = 1, FIR_PASSES do
    for i = 0, FIR_N/8-1 do
      local acc = c0 * x0[i]
      acc = acc + c1 * x1[i]
      acc = acc + c2 * x2[i]
      acc = acc + c3 * x3[i]
      acc = acc + c4 * x4[i]
      acc = acc + c5 * x5[i]
      acc = acc + c6 * x6[i]
      fir_y8[i] = acc + c7 * x7[i]
    end
  end
  return fir_ymm_out[0] + fir_ymm_out[FIR_N/2] +
	 fir_ymm_out[FIR_N-1]
end

----------------------------------------------------- degree-11 polynomial ---

-- Horner evaluation is deliberately compute-heavy: twelve dependent
-- coefficients per element and very little memory traffic.
local POLY_N = 1 << 15
local POLY_PASSES = 60
local poly_in = ffi.new("double[?]", POLY_N)
local poly_scalar_out = ffi.new("double[?]", POLY_N)
local poly_xmm_out = ffi.new("double[?]", POLY_N)
local poly_ymm_out = has_ymm and ffi.new("double[?]", POLY_N)
for i = 0, POLY_N-1 do poly_in[i] = (i % 4096) / 2048 - 1 end

local pc0, pc1, pc2, pc3 = 0.25, -0.5, 0.75, -1
local pc4, pc5, pc6, pc7 = 0.125, 0.375, -0.625, 0.875
local pc8, pc9, pc10, pc11 = -0.03125, 0.0625, -0.09375, 0.015625
local poly_x2 = ffi.cast(d2p, poly_in)
local poly_y2 = ffi.cast(d2p, poly_xmm_out)
local poly_c2 = {
  [0] = d2(pc0), d2(pc1), d2(pc2), d2(pc3),
  d2(pc4), d2(pc5), d2(pc6), d2(pc7),
  d2(pc8), d2(pc9), d2(pc10), d2(pc11),
}
local poly_x4, poly_y4, poly_c4
if has_ymm then
  poly_x4 = ffi.cast(d4p, poly_in)
  poly_y4 = ffi.cast(d4p, poly_ymm_out)
  poly_c4 = {
    [0] = d4(pc0), d4(pc1), d4(pc2), d4(pc3),
    d4(pc4), d4(pc5), d4(pc6), d4(pc7),
    d4(pc8), d4(pc9), d4(pc10), d4(pc11),
  }
end

local function poly_scalar()
  local x, y = poly_in, poly_scalar_out
  for _ = 1, POLY_PASSES do
    for i = 0, POLY_N-1 do
      local xi = x[i]
      local acc = pc11
      acc = acc * xi + pc10
      acc = acc * xi + pc9
      acc = acc * xi + pc8
      acc = acc * xi + pc7
      acc = acc * xi + pc6
      acc = acc * xi + pc5
      acc = acc * xi + pc4
      acc = acc * xi + pc3
      acc = acc * xi + pc2
      acc = acc * xi + pc1
      y[i] = acc * xi + pc0
    end
  end
  return y[0] + y[POLY_N/2] + y[POLY_N-1]
end

local function poly_xmm()
  local c = poly_c2
  for _ = 1, POLY_PASSES do
    for i = 0, POLY_N/2-1 do
      local x = poly_x2[i]
      local acc = c[11]
      acc = acc * x + c[10]
      acc = acc * x + c[9]
      acc = acc * x + c[8]
      acc = acc * x + c[7]
      acc = acc * x + c[6]
      acc = acc * x + c[5]
      acc = acc * x + c[4]
      acc = acc * x + c[3]
      acc = acc * x + c[2]
      acc = acc * x + c[1]
      poly_y2[i] = acc * x + c[0]
    end
  end
  return poly_xmm_out[0] + poly_xmm_out[POLY_N/2] +
	 poly_xmm_out[POLY_N-1]
end

local function poly_ymm()
  local c = poly_c4
  for _ = 1, POLY_PASSES do
    for i = 0, POLY_N/4-1 do
      local x = poly_x4[i]
      local acc = c[11]
      acc = acc * x + c[10]
      acc = acc * x + c[9]
      acc = acc * x + c[8]
      acc = acc * x + c[7]
      acc = acc * x + c[6]
      acc = acc * x + c[5]
      acc = acc * x + c[4]
      acc = acc * x + c[3]
      acc = acc * x + c[2]
      acc = acc * x + c[1]
      poly_y4[i] = acc * x + c[0]
    end
  end
  return poly_ymm_out[0] + poly_ymm_out[POLY_N/2] +
	 poly_ymm_out[POLY_N-1]
end

------------------------------------------------ fixed-iteration Mandelbrot ---

-- The scalar baseline must branch to maintain per-point liveness.  SIMD keeps
-- divergent points in all-ones/zero lane masks and updates all lanes without
-- control-flow exits.
local MANDEL_N = 1 << 14
local MANDEL_PASSES = 4
local MANDEL_ITERS = 64
local mandel_cx = ffi.new("double[?]", MANDEL_N)
local mandel_cy = ffi.new("double[?]", MANDEL_N)
for i = 0, MANDEL_N-1 do
  local ix, iy = i % 256, math.floor(i / 256)
  mandel_cx[i] = -2 + 3 * (ix + 0.5) / 256
  mandel_cy[i] = -1 + 2 * (iy + 0.5) / 64
end

local mandel_x2 = ffi.cast(d2p, mandel_cx)
local mandel_y2 = ffi.cast(d2p, mandel_cy)
local mandel_x4 = has_ymm and ffi.cast(d4p, mandel_cx)
local mandel_y4 = has_ymm and ffi.cast(d4p, mandel_cy)

local function mandel_scalar()
  local total = 0
  for _ = 1, MANDEL_PASSES do
    for i = 0, MANDEL_N-1 do
      local cx, cy = mandel_cx[i], mandel_cy[i]
      local zr, zi, count, active = 0, 0, 0, true
      for _ = 1, MANDEL_ITERS do
	local zr2, zi2 = zr * zr, zi * zi
	if active and zr2 + zi2 <= 4 then
	  count = count + 1
	else
	  active = false
	end
	zi = 2 * zr * zi + cy
	zr = zr2 - zi2 + cx
      end
      total = total + count
    end
  end
  return total
end

local function mandel_xmm()
  local zero, two, four = d2(0), d2(2), d2(4)
  local izero, ones, total = i64x2(0), i64x2(-1), i64x2(0)
  for _ = 1, MANDEL_PASSES do
    for i = 0, MANDEL_N/2-1 do
      local cx, cy = mandel_x2[i], mandel_y2[i]
      local zr, zi, count, active = zero, zero, izero, ones
      for _ = 1, MANDEL_ITERS do
	local zr2, zi2 = zr * zr, zi * zi
	active = simd.band(active, simd.le(zr2 + zi2, four))
	count = count - active
	zi = two * zr * zi + cy
	zr = zr2 - zi2 + cx
      end
      total = total + count
    end
  end
  return tonumber(total[0]) + tonumber(total[1])
end

local function mandel_ymm()
  local zero, two, four = d4(0), d4(2), d4(4)
  local izero, ones, total = i64x4(0), i64x4(-1), i64x4(0)
  for _ = 1, MANDEL_PASSES do
    for i = 0, MANDEL_N/4-1 do
      local cx, cy = mandel_x4[i], mandel_y4[i]
      local zr, zi, count, active = zero, zero, izero, ones
      for _ = 1, MANDEL_ITERS do
	local zr2, zi2 = zr * zr, zi * zi
	active = simd.band(active, simd.le(zr2 + zi2, four))
	count = count - active
	zi = two * zr * zi + cy
	zr = zr2 - zi2 + cx
      end
      total = total + count
    end
  end
  return tonumber(total[0]) + tonumber(total[1]) +
	 tonumber(total[2]) + tonumber(total[3])
end

local real_benches = {}

-- Fully unrolled 16-element scalar min/max tree shared by the unsigned depth
-- and signed PCM range workloads. Their data keeps every comparison direction
-- stable, so LuaJIT records one representative path without side-exit noise.
local function extrema16_scalar(src, p)
  local x0, x1, x2, x3 = src[p], src[p+1], src[p+2], src[p+3]
  local x4, x5, x6, x7 = src[p+4], src[p+5], src[p+6], src[p+7]
  local x8, x9, xa, xb = src[p+8], src[p+9], src[p+10], src[p+11]
  local xc, xd, xe, xf = src[p+12], src[p+13], src[p+14], src[p+15]
  local l0, h0 = x0 < x1 and x0 or x1, x0 > x1 and x0 or x1
  local l1, h1 = x2 < x3 and x2 or x3, x2 > x3 and x2 or x3
  local l2, h2 = x4 < x5 and x4 or x5, x4 > x5 and x4 or x5
  local l3, h3 = x6 < x7 and x6 or x7, x6 > x7 and x6 or x7
  local l4, h4 = x8 < x9 and x8 or x9, x8 > x9 and x8 or x9
  local l5, h5 = xa < xb and xa or xb, xa > xb and xa or xb
  local l6, h6 = xc < xd and xc or xd, xc > xd and xc or xd
  local l7, h7 = xe < xf and xe or xf, xe > xf and xe or xf
  l0, h0 = l0 < l1 and l0 or l1, h0 > h1 and h0 or h1
  l2, h2 = l2 < l3 and l2 or l3, h2 > h3 and h2 or h3
  l4, h4 = l4 < l5 and l4 or l5, h4 > h5 and h4 or h5
  l6, h6 = l6 < l7 and l6 or l7, h6 > h7 and h6 or h7
  l0, h0 = l0 < l2 and l0 or l2, h0 > h2 and h0 or h2
  l4, h4 = l4 < l6 and l4 or l6, h4 > h6 and h4 or h6
  return l0 < l4 and l0 or l4, h0 > h4 and h0 or h4
end

------------------------------------------------------- RGBA channel merge --

do
-- Merge alternating channels from two full-HD RGBA layers. This is the
-- packed form of a common compositor/channel-routing pass: every iteration
-- reads two independent streams, selects whole channels, and writes one
-- stream. The vector form maps exactly to one native two-source blend whose
-- final input can be consumed directly from memory.
local RGBA_PIXELS = 1920 * 1080
local RGBA_N = RGBA_PIXELS * 4
local RGBA_PASSES = 3
local rgba_a = ffi.new("int32_t[?]", RGBA_N)
local rgba_b = ffi.new("int32_t[?]", RGBA_N)
local rgba_out = ffi.new("int32_t[?]", RGBA_N)
for i = 0, RGBA_N-1 do
  rgba_a[i] = (i * 17 + 3) % 65521
  rgba_b[i] = (i * 29 + 11) % 65521
end
local rgba_a4 = ffi.cast(ffi.typeof("$ *", i4), rgba_a)
local rgba_b4 = ffi.cast(ffi.typeof("$ *", i4), rgba_b)
local rgba_o4 = ffi.cast(ffi.typeof("$ *", i4), rgba_out)
local rgba_a8 = has_ymm and ffi.cast(ffi.typeof("$ *", i8), rgba_a)
local rgba_b8 = has_ymm and ffi.cast(ffi.typeof("$ *", i8), rgba_b)
local rgba_o8 = has_ymm and ffi.cast(ffi.typeof("$ *", i8), rgba_out)

local function rgba_merge_scalar()
  for _ = 1, RGBA_PASSES do
    for p = 0, RGBA_PIXELS-1 do
      local i = p*4
      rgba_out[i] = rgba_a[i]
      rgba_out[i+1] = rgba_b[i+1]
      rgba_out[i+2] = rgba_a[i+2]
      rgba_out[i+3] = rgba_b[i+3]
    end
  end
  return sample_sum(rgba_out, RGBA_N)
end

local function rgba_merge_xmm()
  for _ = 1, RGBA_PASSES do
    for i = 0, RGBA_PIXELS-1 do
      rgba_o4[i] = simd.shuffle2(rgba_a4[i], rgba_b4[i], 0, 5, 2, 7)
    end
  end
  return sample_sum(rgba_out, RGBA_N)
end

local function rgba_merge_ymm()
  for _ = 1, RGBA_PASSES do
    for i = 0, RGBA_PIXELS/2-1 do
      rgba_o8[i] = simd.shuffle2(rgba_a8[i], rgba_b8[i],
				 0, 9, 2, 11, 4, 13, 6, 15)
    end
  end
  return sample_sum(rgba_out, RGBA_N)
end

real_benches.rgba = {
  scalar = rgba_merge_scalar, xmm = rgba_merge_xmm, ymm = rgba_merge_ymm,
  name = "RGBA channel merge",
}
end

-------------------------------------------------------- block checksums --

do
-- Produce an 8-bit additive checksum for every 32-byte storage/network
-- block in a 16 MiB payload. Block checksums are used for quick corruption
-- screening, deduplication chunking and packet framing. The scalar loop is
-- explicitly unrolled so this measures packed byte reduction rather than
-- loop overhead; XMM consumes two vectors per block and YMM one.
local CHECKSUM_N = 16 * 1024 * 1024
local CHECKSUM_BLOCKS = CHECKSUM_N / 32
local CHECKSUM_PASSES = 3
local checksum_in = ffi.new("uint8_t[?]", CHECKSUM_N)
local checksum_s = ffi.new("uint8_t[?]", CHECKSUM_BLOCKS)
local checksum_x = ffi.new("uint8_t[?]", CHECKSUM_BLOCKS)
local checksum_y = has_ymm and ffi.new("uint8_t[?]", CHECKSUM_BLOCKS)
for i = 0, CHECKSUM_N-1 do checksum_in[i] = (i*29 + i%251 + 17) % 256 end
local byte16 = ffi.typeof("u8x16")
local byte32 = has_ymm and ffi.typeof("u8x32")
local checksum_v16 = ffi.cast(ffi.typeof("$ *", byte16), checksum_in)
local checksum_v32 = has_ymm and
		     ffi.cast(ffi.typeof("$ *", byte32), checksum_in)

local function checksum_scalar()
  local src, out = checksum_in, checksum_s
  for _ = 1, CHECKSUM_PASSES do
    for i = 0, CHECKSUM_BLOCKS-1 do
      local p = i*32
      out[i] =
	src[p]    + src[p+1]  + src[p+2]  + src[p+3]  +
	src[p+4]  + src[p+5]  + src[p+6]  + src[p+7]  +
	src[p+8]  + src[p+9]  + src[p+10] + src[p+11] +
	src[p+12] + src[p+13] + src[p+14] + src[p+15] +
	src[p+16] + src[p+17] + src[p+18] + src[p+19] +
	src[p+20] + src[p+21] + src[p+22] + src[p+23] +
	src[p+24] + src[p+25] + src[p+26] + src[p+27] +
	src[p+28] + src[p+29] + src[p+30] + src[p+31]
    end
  end
  return sample_sum(out, CHECKSUM_BLOCKS)
end

local function checksum_xmm()
  local src, out = checksum_v16, checksum_x
  for _ = 1, CHECKSUM_PASSES do
    for i = 0, CHECKSUM_BLOCKS-1 do
      local p = i*2
      out[i] = tonumber(simd.hsum(src[p])) +
	       tonumber(simd.hsum(src[p+1]))
    end
  end
  return sample_sum(out, CHECKSUM_BLOCKS)
end

local function checksum_ymm()
  local src, out = checksum_v32, checksum_y
  for _ = 1, CHECKSUM_PASSES do
    for i = 0, CHECKSUM_BLOCKS-1 do
      out[i] = tonumber(simd.hsum(src[i]))
    end
  end
  return sample_sum(out, CHECKSUM_BLOCKS)
end

real_benches.checksum = {
  scalar = checksum_scalar, xmm = checksum_xmm, ymm = checksum_ymm,
  name = "32-byte block checksum", mib = CHECKSUM_N/(1024*1024),
}
end

---------------------------------------------------- 4K depth tile range --

do
-- Build the 16-pixel min/max tiles used by hierarchical-Z occlusion,
-- depth-pyramid construction and range-based image codecs. This scans a full
-- 4K uint16 frame. The scalar comparison tree is explicitly unrolled and has
-- stable branch directions, so the SIMD result is not inflated by a generic
-- inner loop or repeated side exits.
local DEPTH_PIXELS = 3840 * 2160
local DEPTH_TILES = DEPTH_PIXELS / 16
local DEPTH_PASSES = 3
local depth_in = ffi.new("uint16_t[?]", DEPTH_PIXELS)
local depth_lo_s = ffi.new("uint16_t[?]", DEPTH_TILES)
local depth_hi_s = ffi.new("uint16_t[?]", DEPTH_TILES)
local depth_lo_x = ffi.new("uint16_t[?]", DEPTH_TILES)
local depth_hi_x = ffi.new("uint16_t[?]", DEPTH_TILES)
local depth_lo_y = has_ymm and ffi.new("uint16_t[?]", DEPTH_TILES)
local depth_hi_y = has_ymm and ffi.new("uint16_t[?]", DEPTH_TILES)
local depth_pattern = {
  9000, 1200, 14800, 5100, 300, 11700, 7300, 2600,
  13200, 6400, 10100, 50, 8200, 3900, 12500, 1800,
}
for i = 0, DEPTH_PIXELS-1 do
  depth_in[i] = (math.floor(i/16)*37)%50000 + depth_pattern[i%16+1]
end
local depth8 = ffi.typeof("u16x8")
local depth16 = has_ymm and ffi.typeof("u16x16")
local depth_v8 = ffi.cast(ffi.typeof("$ *", depth8), depth_in)
local depth_v16 = has_ymm and ffi.cast(ffi.typeof("$ *", depth16), depth_in)

local function depth_scalar()
  local src, lo, hi = depth_in, depth_lo_s, depth_hi_s
  for _ = 1, DEPTH_PASSES do
    for i = 0, DEPTH_TILES-1 do
      lo[i], hi[i] = extrema16_scalar(src, i*16)
    end
  end
  return sample_sum(lo, DEPTH_TILES) + sample_sum(hi, DEPTH_TILES)
end

local function depth_xmm()
  local src, lo, hi = depth_v8, depth_lo_x, depth_hi_x
  for _ = 1, DEPTH_PASSES do
    for i = 0, DEPTH_TILES-1 do
      local p = i*2
      local a, b = src[p], src[p+1]
      lo[i] = simd.hmin(simd.min(a, b))
      hi[i] = simd.hmax(simd.max(a, b))
    end
  end
  return sample_sum(lo, DEPTH_TILES) + sample_sum(hi, DEPTH_TILES)
end

local function depth_ymm()
  local src, lo, hi = depth_v16, depth_lo_y, depth_hi_y
  for _ = 1, DEPTH_PASSES do
    for i = 0, DEPTH_TILES-1 do
      local v = src[i]
      lo[i], hi[i] = simd.hmin(v), simd.hmax(v)
    end
  end
  return sample_sum(lo, DEPTH_TILES) + sample_sum(hi, DEPTH_TILES)
end

real_benches.depth = {
  scalar = depth_scalar, xmm = depth_xmm, ymm = depth_ymm,
  name = "4K depth tile extrema", pixels = DEPTH_PIXELS,
}
end

------------------------------------------------------ PCM peak envelope --

do
-- Generate min/max waveform metadata for one minute of signed 48 kHz PCM16,
-- sixteen samples per display/storage bucket. Audio editors, stream
-- visualisers and level-of-detail caches perform this exact reduction.
local PCM_SECONDS = 60
local PCM_SAMPLES = 48000 * PCM_SECONDS
local PCM_BLOCKS = PCM_SAMPLES / 16
local PCM_PASSES = 12
local pcm_in = ffi.new("int16_t[?]", PCM_SAMPLES)
local pcm_lo_s = ffi.new("int16_t[?]", PCM_BLOCKS)
local pcm_hi_s = ffi.new("int16_t[?]", PCM_BLOCKS)
local pcm_lo_x = ffi.new("int16_t[?]", PCM_BLOCKS)
local pcm_hi_x = ffi.new("int16_t[?]", PCM_BLOCKS)
local pcm_lo_y = has_ymm and ffi.new("int16_t[?]", PCM_BLOCKS)
local pcm_hi_y = has_ymm and ffi.new("int16_t[?]", PCM_BLOCKS)
local pcm_pattern = {
  17000, -13000, 9000, -19000, 3000, -7000, 15000, -11000,
  5000, -17000, 19000, -3000, 11000, -15000, 7000, -9000,
}
for i = 0, PCM_SAMPLES-1 do
  pcm_in[i] = (math.floor(i/16)*37)%20000 - 10000 + pcm_pattern[i%16+1]
end
local pcm8 = ffi.typeof("i16x8")
local pcm16 = has_ymm and ffi.typeof("i16x16")
local pcm_v8 = ffi.cast(ffi.typeof("$ *", pcm8), pcm_in)
local pcm_v16 = has_ymm and ffi.cast(ffi.typeof("$ *", pcm16), pcm_in)

local function pcm_scalar()
  local src, lo, hi = pcm_in, pcm_lo_s, pcm_hi_s
  for _ = 1, PCM_PASSES do
    for i = 0, PCM_BLOCKS-1 do
      lo[i], hi[i] = extrema16_scalar(src, i*16)
    end
  end
  return sample_sum(lo, PCM_BLOCKS) + sample_sum(hi, PCM_BLOCKS)
end

local function pcm_xmm()
  local src, lo, hi = pcm_v8, pcm_lo_x, pcm_hi_x
  for _ = 1, PCM_PASSES do
    for i = 0, PCM_BLOCKS-1 do
      local p = i*2
      local a, b = src[p], src[p+1]
      lo[i] = simd.hmin(simd.min(a, b))
      hi[i] = simd.hmax(simd.max(a, b))
    end
  end
  return sample_sum(lo, PCM_BLOCKS) + sample_sum(hi, PCM_BLOCKS)
end

local function pcm_ymm()
  local src, lo, hi = pcm_v16, pcm_lo_y, pcm_hi_y
  for _ = 1, PCM_PASSES do
    for i = 0, PCM_BLOCKS-1 do
      local v = src[i]
      lo[i], hi[i] = simd.hmin(v), simd.hmax(v)
    end
  end
  return sample_sum(lo, PCM_BLOCKS) + sample_sum(hi, PCM_BLOCKS)
end

real_benches.pcm = {
  scalar = pcm_scalar, xmm = pcm_xmm, ymm = pcm_ymm,
  name = "PCM16 peak envelope", seconds = PCM_SECONDS,
}
end

---------------------------------------------- PCM16 16-tap decimation --

do
-- A fixed-point 16:1 polyphase decimator over 16 MiB of PCM16 input. The
-- deliberately bounded samples and coefficients keep every full dot product
-- in int16 range, so scalar and SIMD paths compute the same mathematical
-- result without relying on overflow. This is also a sustained hsum(a*b)
-- workload rather than a single reduction after a long accumulator loop.
local PCM_FIR_SAMPLES = 8 * 1024 * 1024
local PCM_FIR_BLOCKS = PCM_FIR_SAMPLES / 16
local PCM_FIR_PASSES = 4
local pcm_fir_in = ffi.new("int16_t[?]", PCM_FIR_SAMPLES)
local pcm_fir_out_s = ffi.new("int16_t[?]", PCM_FIR_BLOCKS)
local pcm_fir_out_x = ffi.new("int16_t[?]", PCM_FIR_BLOCKS)
local pcm_fir_out_y = has_ymm and ffi.new("int16_t[?]", PCM_FIR_BLOCKS)
local pcm_fir_coeff = {
  -3, -2, -1, 0, 1, 2, 3, 4, 4, 3, 2, 1, 0, -1, -2, -3,
}
for i = 0, PCM_FIR_SAMPLES-1 do
  pcm_fir_in[i] = (i*29 + math.floor(i/16)*7) % 127 - 63
end
local pcm_fir8 = ffi.typeof("i16x8")
local pcm_fir16 = has_ymm and ffi.typeof("i16x16")
local pcm_fir_v8 = ffi.cast(ffi.typeof("$ *", pcm_fir8), pcm_fir_in)
local pcm_fir_v16 = has_ymm and
		    ffi.cast(ffi.typeof("$ *", pcm_fir16), pcm_fir_in)
local pcm_fir_c0 = pcm_fir8(unpack(pcm_fir_coeff, 1, 8))
local pcm_fir_c1 = pcm_fir8(unpack(pcm_fir_coeff, 9, 16))
local pcm_fir_c16 = has_ymm and pcm_fir16(unpack(pcm_fir_coeff, 1, 16))

local function pcm_fir_scalar()
  local src, out, c = pcm_fir_in, pcm_fir_out_s, pcm_fir_coeff
  for _ = 1, PCM_FIR_PASSES do
    for i = 0, PCM_FIR_BLOCKS-1 do
      local p = i*16
      out[i] =
	src[p]   *c[1]  + src[p+1] *c[2]  + src[p+2] *c[3]  + src[p+3] *c[4] +
	src[p+4] *c[5]  + src[p+5] *c[6]  + src[p+6] *c[7]  + src[p+7] *c[8] +
	src[p+8] *c[9]  + src[p+9] *c[10] + src[p+10]*c[11] + src[p+11]*c[12] +
	src[p+12]*c[13] + src[p+13]*c[14] + src[p+14]*c[15] + src[p+15]*c[16]
    end
  end
  return sample_sum(out, PCM_FIR_BLOCKS)
end

local function pcm_fir_xmm()
  local src, out = pcm_fir_v8, pcm_fir_out_x
  local c0, c1 = pcm_fir_c0, pcm_fir_c1
  for _ = 1, PCM_FIR_PASSES do
    for i = 0, PCM_FIR_BLOCKS-1 do
      local p = i*2
      out[i] = tonumber(simd.hsum(src[p]*c0)) +
	       tonumber(simd.hsum(src[p+1]*c1))
    end
  end
  return sample_sum(out, PCM_FIR_BLOCKS)
end

local function pcm_fir_ymm()
  local src, out, c = pcm_fir_v16, pcm_fir_out_y, pcm_fir_c16
  for _ = 1, PCM_FIR_PASSES do
    for i = 0, PCM_FIR_BLOCKS-1 do
      out[i] = simd.hsum(src[i]*c)
    end
  end
  return sample_sum(out, PCM_FIR_BLOCKS)
end

real_benches.pcmfir = {
  scalar = pcm_fir_scalar, xmm = pcm_fir_xmm, ymm = pcm_fir_ymm,
  name = "PCM16 16-tap decimator",
  seconds = PCM_FIR_SAMPLES/48000,
}
end

-------------------------------------------------- INT8 activation range --

do
-- Compute per-32-value range metadata over 16 MiB of quantized neural-network
-- activations. Calibration, saturation diagnostics and blockwise dynamic
-- quantization all scan signed INT8 tensors this way.
local ACT_N = 16 * 1024 * 1024
local ACT_BLOCKS = ACT_N / 32
local ACT_PASSES = 3
local act_in = ffi.new("int8_t[?]", ACT_N)
local act_lo_s = ffi.new("int8_t[?]", ACT_BLOCKS)
local act_hi_s = ffi.new("int8_t[?]", ACT_BLOCKS)
local act_lo_x = ffi.new("int8_t[?]", ACT_BLOCKS)
local act_hi_x = ffi.new("int8_t[?]", ACT_BLOCKS)
local act_lo_y = has_ymm and ffi.new("int8_t[?]", ACT_BLOCKS)
local act_hi_y = has_ymm and ffi.new("int8_t[?]", ACT_BLOCKS)
local act_pattern = {
  71,-63,45,-77,29,-51,80,-35,17,-69,57,-23,39,-55,67,-41,
  53,-75,31,-47,73,-19,43,-65,13,-57,61,-27,35,-71,49,-33,
}
for i = 0, ACT_N-1 do
  act_in[i] = (math.floor(i/32)*17)%80 - 40 + act_pattern[i%32+1]
end
local act16 = ffi.typeof("i8x16")
local act32 = has_ymm and ffi.typeof("i8x32")
local act_v16 = ffi.cast(ffi.typeof("$ *", act16), act_in)
local act_v32 = has_ymm and ffi.cast(ffi.typeof("$ *", act32), act_in)

local function act_scalar()
  local src, lo, hi = act_in, act_lo_s, act_hi_s
  for _ = 1, ACT_PASSES do
    for i = 0, ACT_BLOCKS-1 do
      local p = i*32
      local l0, h0 = extrema16_scalar(src, p)
      local l1, h1 = extrema16_scalar(src, p+16)
      lo[i], hi[i] = l0 < l1 and l0 or l1, h0 > h1 and h0 or h1
    end
  end
  return sample_sum(lo, ACT_BLOCKS) + sample_sum(hi, ACT_BLOCKS)
end

local function act_xmm()
  local src, lo, hi = act_v16, act_lo_x, act_hi_x
  for _ = 1, ACT_PASSES do
    for i = 0, ACT_BLOCKS-1 do
      local p = i*2
      local a, b = src[p], src[p+1]
      lo[i] = simd.hmin(simd.min(a, b))
      hi[i] = simd.hmax(simd.max(a, b))
    end
  end
  return sample_sum(lo, ACT_BLOCKS) + sample_sum(hi, ACT_BLOCKS)
end

local function act_ymm()
  local src, lo, hi = act_v32, act_lo_y, act_hi_y
  for _ = 1, ACT_PASSES do
    for i = 0, ACT_BLOCKS-1 do
      local v = src[i]
      lo[i], hi[i] = simd.hmin(v), simd.hmax(v)
    end
  end
  return sample_sum(lo, ACT_BLOCKS) + sample_sum(hi, ACT_BLOCKS)
end

real_benches.activations = {
  scalar = act_scalar, xmm = act_xmm, ymm = act_ymm,
  name = "INT8 activation range", mib = ACT_N/(1024*1024),
}
end

---------------------------------------------------- INT8 ternary filter --

do
-- Apply a 32-tap ternary/small-weight inference filter to 16 MiB of signed
-- INT8 activations. Inputs in {-1,0,1} and weights in {-2..2} keep every dot
-- in signed-byte range, so this measures exact arithmetic rather than using
-- wraparound to manufacture agreement.
local TERNARY_N = 16 * 1024 * 1024
local TERNARY_BLOCKS = TERNARY_N / 32
local TERNARY_PASSES = 4
local ternary_in = ffi.new("int8_t[?]", TERNARY_N)
local ternary_out_s = ffi.new("int8_t[?]", TERNARY_BLOCKS)
local ternary_out_x = ffi.new("int8_t[?]", TERNARY_BLOCKS)
local ternary_out_y = has_ymm and ffi.new("int8_t[?]", TERNARY_BLOCKS)
local ternary_weight = {
  -2,-1,0,1,2,1,0,-1, -2,0,2,0,-2,1,-1,2,
  2,1,0,-1,-2,-1,0,1, 2,0,-2,0,2,-1,1,-2,
}
for i = 0, TERNARY_N-1 do
  ternary_in[i] = (i*17 + math.floor(i/32)*7) % 3 - 1
end
local ternary16 = ffi.typeof("i8x16")
local ternary32 = has_ymm and ffi.typeof("i8x32")
local ternary_v16 = ffi.cast(ffi.typeof("$ *", ternary16), ternary_in)
local ternary_v32 = has_ymm and
		    ffi.cast(ffi.typeof("$ *", ternary32), ternary_in)
local ternary_w0 = ternary16(unpack(ternary_weight, 1, 16))
local ternary_w1 = ternary16(unpack(ternary_weight, 17, 32))
local ternary_w32 = has_ymm and
		    ternary32(unpack(ternary_weight, 1, 32))

local function ternary_scalar()
  local src, out, w = ternary_in, ternary_out_s, ternary_weight
  for _ = 1, TERNARY_PASSES do
    for i = 0, TERNARY_BLOCKS-1 do
      local p = i*32
      out[i] =
	src[p]   *w[1]  + src[p+1] *w[2]  + src[p+2] *w[3]  + src[p+3] *w[4] +
	src[p+4] *w[5]  + src[p+5] *w[6]  + src[p+6] *w[7]  + src[p+7] *w[8] +
	src[p+8] *w[9]  + src[p+9] *w[10] + src[p+10]*w[11] + src[p+11]*w[12] +
	src[p+12]*w[13] + src[p+13]*w[14] + src[p+14]*w[15] + src[p+15]*w[16] +
	src[p+16]*w[17] + src[p+17]*w[18] + src[p+18]*w[19] + src[p+19]*w[20] +
	src[p+20]*w[21] + src[p+21]*w[22] + src[p+22]*w[23] + src[p+23]*w[24] +
	src[p+24]*w[25] + src[p+25]*w[26] + src[p+26]*w[27] + src[p+27]*w[28] +
	src[p+28]*w[29] + src[p+29]*w[30] + src[p+30]*w[31] + src[p+31]*w[32]
    end
  end
  return sample_sum(out, TERNARY_BLOCKS)
end

local function ternary_xmm()
  local src, out = ternary_v16, ternary_out_x
  local w0, w1 = ternary_w0, ternary_w1
  for _ = 1, TERNARY_PASSES do
    for i = 0, TERNARY_BLOCKS-1 do
      local p = i*2
      out[i] = tonumber(simd.hsum(src[p]*w0)) +
	       tonumber(simd.hsum(src[p+1]*w1))
    end
  end
  return sample_sum(out, TERNARY_BLOCKS)
end

local function ternary_ymm()
  local src, out, w = ternary_v32, ternary_out_y, ternary_w32
  for _ = 1, TERNARY_PASSES do
    for i = 0, TERNARY_BLOCKS-1 do out[i] = simd.hsum(src[i]*w) end
  end
  return sample_sum(out, TERNARY_BLOCKS)
end

real_benches.ternary = {
  scalar = ternary_scalar, xmm = ternary_xmm, ymm = ternary_ymm,
  name = "INT8 32-tap ternary filter", mib = TERNARY_N/(1024*1024),
}
end

-------------------------------------------------------- 1080p Gaussian blur --

do
-- A separable 5x5 Gaussian is a production image-processing primitive. This
-- processes a full 1920x1080 float frame with padded borders, one horizontal
-- and one vertical pass. The source offsets are intentionally unaligned.
local BLUR_W, BLUR_H = 1920, 1080
local BLUR_STRIDE = BLUR_W + 4
local BLUR_ROWS = BLUR_H + 4
local BLUR_N = BLUR_W * BLUR_H
local BLUR_PASSES = 2
local blur_in = ffi.new("float[?]", BLUR_STRIDE * BLUR_ROWS)
local blur_tmp_s = ffi.new("float[?]", BLUR_W * BLUR_ROWS)
local blur_tmp_x = ffi.new("float[?]", BLUR_W * BLUR_ROWS)
local blur_out_s = ffi.new("float[?]", BLUR_N)
local blur_out_x = ffi.new("float[?]", BLUR_N)
local blur_tmp_y = has_ymm and ffi.new("float[?]", BLUR_W * BLUR_ROWS)
local blur_out_y = has_ymm and ffi.new("float[?]", BLUR_N)
for y = 0, BLUR_ROWS-1 do
  for x = 0, BLUR_STRIDE-1 do
    blur_in[y*BLUR_STRIDE+x] =
      ((x*13 + y*29 + (x*y) % 31) % 1024) / 1024
  end
end

local function blur_scalar()
  local src, tmp, out = blur_in, blur_tmp_s, blur_out_s
  local w, h, stride = BLUR_W, BLUR_H, BLUR_STRIDE
  for _ = 1, BLUR_PASSES do
    for y = 0, BLUR_ROWS-1 do
      local si, di = y*stride, y*w
      for x = 0, w-1 do
	local edge = src[si+x] + src[si+x+4]
	local near = (src[si+x+1] + src[si+x+3]) * 4
	tmp[di+x] = (edge + near + src[si+x+2] * 6) * 0.0625
      end
    end
    for y = 0, h-1 do
      local d, r0 = y*w, y*w
      local r1, r2, r3, r4 = r0+w, r0+2*w, r0+3*w, r0+4*w
      for x = 0, w-1 do
	local edge = tmp[r0+x] + tmp[r4+x]
	local near = (tmp[r1+x] + tmp[r3+x]) * 4
	out[d+x] = (edge + near + tmp[r2+x] * 6) * 0.0625
      end
    end
  end
  return sample_sum(out, BLUR_N)
end

local function blur_xmm()
  local four, six, inv = f4(4), f4(6), f4(0.0625)
  local w, h, stride = BLUR_W, BLUR_H, BLUR_STRIDE
  for _ = 1, BLUR_PASSES do
    for y = 0, BLUR_ROWS-1 do
      local si, di = y*stride, y*w
      local s0 = ffi.cast(f4p, blur_in + si)
      local s1 = ffi.cast(f4p, blur_in + si + 1)
      local s2 = ffi.cast(f4p, blur_in + si + 2)
      local s3 = ffi.cast(f4p, blur_in + si + 3)
      local s4 = ffi.cast(f4p, blur_in + si + 4)
      local d = ffi.cast(f4p, blur_tmp_x + di)
      for x = 0, w/4-1 do
	local edge = s0[x] + s4[x]
	local near = (s1[x] + s3[x]) * four
	d[x] = (edge + near + s2[x] * six) * inv
      end
    end
    for y = 0, h-1 do
      local r0 = y*w
      local s0 = ffi.cast(f4p, blur_tmp_x + r0)
      local s1 = ffi.cast(f4p, blur_tmp_x + r0 + w)
      local s2 = ffi.cast(f4p, blur_tmp_x + r0 + 2*w)
      local s3 = ffi.cast(f4p, blur_tmp_x + r0 + 3*w)
      local s4 = ffi.cast(f4p, blur_tmp_x + r0 + 4*w)
      local d = ffi.cast(f4p, blur_out_x + r0)
      for x = 0, w/4-1 do
	local edge = s0[x] + s4[x]
	local near = (s1[x] + s3[x]) * four
	d[x] = (edge + near + s2[x] * six) * inv
      end
    end
  end
  return sample_sum(blur_out_x, BLUR_N)
end

local function blur_ymm()
  local four, six, inv = f8(4), f8(6), f8(0.0625)
  local w, h, stride = BLUR_W, BLUR_H, BLUR_STRIDE
  for _ = 1, BLUR_PASSES do
    for y = 0, BLUR_ROWS-1 do
      local si, di = y*stride, y*w
      local s0 = ffi.cast(f8p, blur_in + si)
      local s1 = ffi.cast(f8p, blur_in + si + 1)
      local s2 = ffi.cast(f8p, blur_in + si + 2)
      local s3 = ffi.cast(f8p, blur_in + si + 3)
      local s4 = ffi.cast(f8p, blur_in + si + 4)
      local d = ffi.cast(f8p, blur_tmp_y + di)
      for x = 0, w/8-1 do
	local edge = s0[x] + s4[x]
	local near = (s1[x] + s3[x]) * four
	d[x] = (edge + near + s2[x] * six) * inv
      end
    end
    for y = 0, h-1 do
      local r0 = y*w
      local s0 = ffi.cast(f8p, blur_tmp_y + r0)
      local s1 = ffi.cast(f8p, blur_tmp_y + r0 + w)
      local s2 = ffi.cast(f8p, blur_tmp_y + r0 + 2*w)
      local s3 = ffi.cast(f8p, blur_tmp_y + r0 + 3*w)
      local s4 = ffi.cast(f8p, blur_tmp_y + r0 + 4*w)
      local d = ffi.cast(f8p, blur_out_y + r0)
      for x = 0, w/8-1 do
	local edge = s0[x] + s4[x]
	local near = (s1[x] + s3[x]) * four
	d[x] = (edge + near + s2[x] * six) * inv
      end
    end
  end
  return sample_sum(blur_out_y, BLUR_N)
end

real_benches.blur = {
  scalar = blur_scalar, xmm = blur_xmm, ymm = blur_ymm,
  name = "5x5 Gaussian (1080p)",
  cmp = function(a, b) return approx(a, b, 2e-5) end,
}
end

----------------------------------------------------------- 64-tap audio FIR --

do
-- A 64-tap windowed-sinc convolution over more than five seconds of 48 kHz
-- audio. Four independent accumulators hide multiply/add latency, which is
-- how a real convolution kernel keeps both SIMD execution pipes occupied.
local AUDIO_N = 1 << 18
local AUDIO_TAPS = 64
local AUDIO_PASSES = 2
local audio_in = ffi.new("float[?]", AUDIO_N + AUDIO_TAPS-1)
local audio_coeff = ffi.new("float[?]", AUDIO_TAPS)
local audio_out_s = ffi.new("float[?]", AUDIO_N)
local audio_out_x = ffi.new("float[?]", AUDIO_N)
local audio_out_y = has_ymm and ffi.new("float[?]", AUDIO_N)
for i = 0, AUDIO_N+AUDIO_TAPS-2 do
  audio_in[i] = math.sin(i*0.017) * 0.6 + math.sin(i*0.071) * 0.25
end
do
  local sum = 0
  for k = 0, AUDIO_TAPS-1 do
    local x = k - (AUDIO_TAPS-1)*0.5
    local sinc = math.sin(0.22*math.pi*x) / (math.pi*x)
    local window = 0.54 - 0.46*math.cos(2*math.pi*k/(AUDIO_TAPS-1))
    audio_coeff[k] = sinc * window
    sum = sum + audio_coeff[k]
  end
  for k = 0, AUDIO_TAPS-1 do audio_coeff[k] = audio_coeff[k] / sum end
end

local audio_x1, audio_c1 = {}, {}
local audio_x4, audio_c4 = {}, {}
for k = 0, AUDIO_TAPS-1 do
  audio_x1[k] = audio_in + k
  audio_c1[k] = tonumber(audio_coeff[k])
  audio_x4[k] = ffi.cast(f4p, audio_in + k)
  audio_c4[k] = f4(audio_coeff[k])
end
local audio_y4 = ffi.cast(f4p, audio_out_x)
local audio_x8, audio_c8, audio_y8
if has_ymm then
  audio_x8, audio_c8 = {}, {}
  for k = 0, AUDIO_TAPS-1 do
    audio_x8[k] = ffi.cast(f8p, audio_in + k)
    audio_c8[k] = f8(audio_coeff[k])
  end
  audio_y8 = ffi.cast(f8p, audio_out_y)
end

-- Literal tap indices let the recorder hoist all pointer/constant table
-- lookups. The four chains are independent, exposing instruction-level
-- parallelism without leaving a dynamic Lua loop around each SIMD operation.
local function audio_dot(i, x, c, zero)
  local a0, a1, a2, a3 = zero, zero, zero, zero
  a0 = a0 + x[0][i]*c[0]
  a1 = a1 + x[16][i]*c[16]
  a2 = a2 + x[32][i]*c[32]
  a3 = a3 + x[48][i]*c[48]
  a0 = a0 + x[1][i]*c[1]
  a1 = a1 + x[17][i]*c[17]
  a2 = a2 + x[33][i]*c[33]
  a3 = a3 + x[49][i]*c[49]
  a0 = a0 + x[2][i]*c[2]
  a1 = a1 + x[18][i]*c[18]
  a2 = a2 + x[34][i]*c[34]
  a3 = a3 + x[50][i]*c[50]
  a0 = a0 + x[3][i]*c[3]
  a1 = a1 + x[19][i]*c[19]
  a2 = a2 + x[35][i]*c[35]
  a3 = a3 + x[51][i]*c[51]
  a0 = a0 + x[4][i]*c[4]
  a1 = a1 + x[20][i]*c[20]
  a2 = a2 + x[36][i]*c[36]
  a3 = a3 + x[52][i]*c[52]
  a0 = a0 + x[5][i]*c[5]
  a1 = a1 + x[21][i]*c[21]
  a2 = a2 + x[37][i]*c[37]
  a3 = a3 + x[53][i]*c[53]
  a0 = a0 + x[6][i]*c[6]
  a1 = a1 + x[22][i]*c[22]
  a2 = a2 + x[38][i]*c[38]
  a3 = a3 + x[54][i]*c[54]
  a0 = a0 + x[7][i]*c[7]
  a1 = a1 + x[23][i]*c[23]
  a2 = a2 + x[39][i]*c[39]
  a3 = a3 + x[55][i]*c[55]
  a0 = a0 + x[8][i]*c[8]
  a1 = a1 + x[24][i]*c[24]
  a2 = a2 + x[40][i]*c[40]
  a3 = a3 + x[56][i]*c[56]
  a0 = a0 + x[9][i]*c[9]
  a1 = a1 + x[25][i]*c[25]
  a2 = a2 + x[41][i]*c[41]
  a3 = a3 + x[57][i]*c[57]
  a0 = a0 + x[10][i]*c[10]
  a1 = a1 + x[26][i]*c[26]
  a2 = a2 + x[42][i]*c[42]
  a3 = a3 + x[58][i]*c[58]
  a0 = a0 + x[11][i]*c[11]
  a1 = a1 + x[27][i]*c[27]
  a2 = a2 + x[43][i]*c[43]
  a3 = a3 + x[59][i]*c[59]
  a0 = a0 + x[12][i]*c[12]
  a1 = a1 + x[28][i]*c[28]
  a2 = a2 + x[44][i]*c[44]
  a3 = a3 + x[60][i]*c[60]
  a0 = a0 + x[13][i]*c[13]
  a1 = a1 + x[29][i]*c[29]
  a2 = a2 + x[45][i]*c[45]
  a3 = a3 + x[61][i]*c[61]
  a0 = a0 + x[14][i]*c[14]
  a1 = a1 + x[30][i]*c[30]
  a2 = a2 + x[46][i]*c[46]
  a3 = a3 + x[62][i]*c[62]
  a0 = a0 + x[15][i]*c[15]
  a1 = a1 + x[31][i]*c[31]
  a2 = a2 + x[47][i]*c[47]
  a3 = a3 + x[63][i]*c[63]
  return (a0+a1) + (a2+a3)
end

local function audio_fir_scalar()
  for _ = 1, AUDIO_PASSES do
    for i = 0, AUDIO_N-1 do
      audio_out_s[i] = audio_dot(i, audio_x1, audio_c1, 0)
    end
  end
  return sample_sum(audio_out_s, AUDIO_N)
end

local function audio_fir_xmm()
  local zero = f4(0)
  for _ = 1, AUDIO_PASSES do
    for i = 0, AUDIO_N/4-1 do
      audio_y4[i] = audio_dot(i, audio_x4, audio_c4, zero)
    end
  end
  return sample_sum(audio_out_x, AUDIO_N)
end

local function audio_fir_ymm()
  local zero = f8(0)
  for _ = 1, AUDIO_PASSES do
    for i = 0, AUDIO_N/8-1 do
      audio_y8[i] = audio_dot(i, audio_x8, audio_c8, zero)
    end
  end
  return sample_sum(audio_out_y, AUDIO_N)
end

real_benches.audio = {
  scalar = audio_fir_scalar, xmm = audio_fir_xmm, ymm = audio_fir_ymm,
  name = "64-tap audio FIR", seconds = AUDIO_N/48000,
  cmp = function(a, b) return approx(a, b, 5e-5) end,
}
end

------------------------------------------------------- particle simulation --

do
-- Independent particles orbit a point mass for 32 integration steps, with a
-- damped ground-plane collision. Positions and velocities stay in registers
-- for the whole trajectory. This combines sqrt/divide, multiply/add chains,
-- comparisons, masks and select instead of benchmarking one instruction.
local PARTICLE_N = 1 << 17
local PARTICLE_STEPS = 32
local particle_x = ffi.new("float[?]", PARTICLE_N)
local particle_y = ffi.new("float[?]", PARTICLE_N)
local particle_z = ffi.new("float[?]", PARTICLE_N)
local particle_vx = ffi.new("float[?]", PARTICLE_N)
local particle_vy = ffi.new("float[?]", PARTICLE_N)
local particle_vz = ffi.new("float[?]", PARTICLE_N)
local particle_out_s = ffi.new("float[?]", PARTICLE_N*3)
local particle_out_x = ffi.new("float[?]", PARTICLE_N*3)
local particle_out_y = has_ymm and ffi.new("float[?]", PARTICLE_N*3)
for i = 0, PARTICLE_N-1 do
  local a = (i % 4096) * (2*math.pi/4096)
  local ring = 1.5 + (i % 97) / 97
  particle_x[i] = math.cos(a) * ring
  -- A small subset crosses the floor, producing divergent masks in a batch.
  particle_y[i] = -1.99 + (i % 211) / 140
  particle_z[i] = math.sin(a) * ring
  particle_vx[i] = -math.sin(a) * 0.42
  particle_vy[i] = ((i % 31) - 15) * 0.03
  particle_vz[i] = math.cos(a) * 0.42
end

local function particle_scalar()
  local out = particle_out_s
  local dt, mu, soft, drag = 0.0125, -1.35, 0.15, 0.9992
  local floor, bounce = -2, 0.72
  for i = 0, PARTICLE_N-1 do
    local x, y, z = particle_x[i], particle_y[i], particle_z[i]
    local vx, vy, vz = particle_vx[i], particle_vy[i], particle_vz[i]
    for _ = 1, PARTICLE_STEPS do
      local r2 = (x*x + y*y) + (z*z + soft)
      local inv = 1 / math.sqrt(r2)
      local force = mu * ((inv*inv) * inv)
      vx = (vx + x*force*dt) * drag
      vy = (vy + y*force*dt) * drag
      vz = (vz + z*force*dt) * drag
      x, y, z = x + vx*dt, y + vy*dt, z + vz*dt
      if y < floor then
	y = 2*floor - y
	vy = -vy * bounce
      end
    end
    out[i], out[PARTICLE_N+i], out[2*PARTICLE_N+i] = x, y, z
  end
  return sample_sum(out, PARTICLE_N*3)
end

local particle_x4 = ffi.cast(f4p, particle_x)
local particle_y4 = ffi.cast(f4p, particle_y)
local particle_z4 = ffi.cast(f4p, particle_z)
local particle_vx4 = ffi.cast(f4p, particle_vx)
local particle_vy4 = ffi.cast(f4p, particle_vy)
local particle_vz4 = ffi.cast(f4p, particle_vz)
local particle_out_x4 = ffi.cast(f4p, particle_out_x)

local function particle_xmm()
  local dt, mu, soft, drag = f4(0.0125), f4(-1.35), f4(0.15), f4(0.9992)
  local one, floor, floor2, bounce = f4(1), f4(-2), f4(-4), f4(0.72)
  local n = PARTICLE_N/4
  for i = 0, n-1 do
    local x, y, z = particle_x4[i], particle_y4[i], particle_z4[i]
    local vx, vy, vz = particle_vx4[i], particle_vy4[i], particle_vz4[i]
    for _ = 1, PARTICLE_STEPS do
      local r2 = (x*x + y*y) + (z*z + soft)
      local inv = one / simd.sqrt(r2)
      local force = mu * ((inv*inv) * inv)
      vx = (vx + x*force*dt) * drag
      vy = (vy + y*force*dt) * drag
      vz = (vz + z*force*dt) * drag
      x, y, z = x + vx*dt, y + vy*dt, z + vz*dt
      local hit = simd.lt(y, floor)
      y = simd.select(hit, floor2-y, y)
      vy = simd.select(hit, -vy*bounce, vy)
    end
    particle_out_x4[i] = x
    particle_out_x4[n+i] = y
    particle_out_x4[2*n+i] = z
  end
  return sample_sum(particle_out_x, PARTICLE_N*3)
end

local particle_x8 = has_ymm and ffi.cast(f8p, particle_x)
local particle_y8 = has_ymm and ffi.cast(f8p, particle_y)
local particle_z8 = has_ymm and ffi.cast(f8p, particle_z)
local particle_vx8 = has_ymm and ffi.cast(f8p, particle_vx)
local particle_vy8 = has_ymm and ffi.cast(f8p, particle_vy)
local particle_vz8 = has_ymm and ffi.cast(f8p, particle_vz)
local particle_out_y8 = has_ymm and ffi.cast(f8p, particle_out_y)

local function particle_ymm()
  local dt, mu, soft, drag = f8(0.0125), f8(-1.35), f8(0.15), f8(0.9992)
  local one, floor, floor2, bounce = f8(1), f8(-2), f8(-4), f8(0.72)
  local n = PARTICLE_N/8
  for i = 0, n-1 do
    local x, y, z = particle_x8[i], particle_y8[i], particle_z8[i]
    local vx, vy, vz = particle_vx8[i], particle_vy8[i], particle_vz8[i]
    for _ = 1, PARTICLE_STEPS do
      local r2 = (x*x + y*y) + (z*z + soft)
      local inv = one / simd.sqrt(r2)
      local force = mu * ((inv*inv) * inv)
      vx = (vx + x*force*dt) * drag
      vy = (vy + y*force*dt) * drag
      vz = (vz + z*force*dt) * drag
      x, y, z = x + vx*dt, y + vy*dt, z + vz*dt
      local hit = simd.lt(y, floor)
      y = simd.select(hit, floor2-y, y)
      vy = simd.select(hit, -vy*bounce, vy)
    end
    particle_out_y8[i] = x
    particle_out_y8[n+i] = y
    particle_out_y8[2*n+i] = z
  end
  return sample_sum(particle_out_y, PARTICLE_N*3)
end

real_benches.particles = {
  scalar = particle_scalar, xmm = particle_xmm, ymm = particle_ymm,
  name = "gravity particles x32", count = PARTICLE_N,
  cmp = function(a, b) return approx(a, b, 2e-4) end,
}
end

-------------------------------------------------------- ChaCha20 block core --

do
-- ChaCha20 is a real integer-SIMD workload: sixteen live 32-bit state words,
-- 20 rounds of wrapping add/xor/rotate, and no scalar work in the vector
-- core. SIMD lanes are independent blocks, as in production implementations.
local CHACHA_BLOCKS = 1 << 13
local CHACHA_PASSES = 16
local bit_ = require("bit")
local bxor, rol, tobit = bit_.bxor, bit_.rol, bit_.tobit
local cc0, cc1, cc2, cc3 =
  tobit(0x61707865), tobit(0x3320646e),
  tobit(0x79622d32), tobit(0x6b206574)
local ck0, ck1, ck2, ck3 =
  tobit(0x03020100), tobit(0x07060504),
  tobit(0x0b0a0908), tobit(0x0f0e0d0c)
local ck4, ck5, ck6, ck7 =
  tobit(0x13121110), tobit(0x17161514),
  tobit(0x1b1a1918), tobit(0x1f1e1d1c)
local cn0, cn1, cn2 =
  tobit(0x09000000), tobit(0x4a000000), tobit(0x00000000)

local function chacha_qr_scalar(a, b, c, d)
  a = tobit(a+b); d = rol(bxor(d, a), 16)
  c = tobit(c+d); b = rol(bxor(b, c), 12)
  a = tobit(a+b); d = rol(bxor(d, a), 8)
  c = tobit(c+d); b = rol(bxor(b, c), 7)
  return a, b, c, d
end

local function chacha_scalar()
  local checksum, counter = 0, 0
  for _ = 1, CHACHA_PASSES do
    for _ = 1, CHACHA_BLOCKS do
      local x0, x1, x2, x3 = cc0, cc1, cc2, cc3
      local x4, x5, x6, x7 = ck0, ck1, ck2, ck3
      local x8, x9, x10, x11 = ck4, ck5, ck6, ck7
      local x12, x13, x14, x15 = counter, cn0, cn1, cn2
      for _ = 1, 10 do
	x0, x4, x8, x12 = chacha_qr_scalar(x0, x4, x8, x12)
	x1, x5, x9, x13 = chacha_qr_scalar(x1, x5, x9, x13)
	x2, x6, x10, x14 = chacha_qr_scalar(x2, x6, x10, x14)
	x3, x7, x11, x15 = chacha_qr_scalar(x3, x7, x11, x15)
	x0, x5, x10, x15 = chacha_qr_scalar(x0, x5, x10, x15)
	x1, x6, x11, x12 = chacha_qr_scalar(x1, x6, x11, x12)
	x2, x7, x8, x13 = chacha_qr_scalar(x2, x7, x8, x13)
	x3, x4, x9, x14 = chacha_qr_scalar(x3, x4, x9, x14)
      end
      x0, x1, x2, x3 = tobit(x0+cc0), tobit(x1+cc1),
		       tobit(x2+cc2), tobit(x3+cc3)
      x4, x5, x6, x7 = tobit(x4+ck0), tobit(x5+ck1),
		       tobit(x6+ck2), tobit(x7+ck3)
      x8, x9, x10, x11 = tobit(x8+ck4), tobit(x9+ck5),
			 tobit(x10+ck6), tobit(x11+ck7)
      x12, x13, x14, x15 = tobit(x12+counter), tobit(x13+cn0),
			   tobit(x14+cn1), tobit(x15+cn2)
      checksum = tobit(checksum + x0+x1+x2+x3 + x4+x5+x6+x7 +
		       x8+x9+x10+x11 + x12+x13+x14+x15)
      counter = counter + 1
    end
  end
  return checksum
end

local function chacha_qr_vec(a, b, c, d)
  a = a+b
  d = simd.bxor(d, a)
  d = simd.bor(simd.shl(d, 16), simd.shr(d, 16))
  c = c+d
  b = simd.bxor(b, c)
  b = simd.bor(simd.shl(b, 12), simd.shr(b, 20))
  a = a+b
  d = simd.bxor(d, a)
  d = simd.bor(simd.shl(d, 8), simd.shr(d, 24))
  c = c+d
  b = simd.bxor(b, c)
  b = simd.bor(simd.shl(b, 7), simd.shr(b, 25))
  return a, b, c, d
end

local function chacha_xmm()
  local C0, C1, C2, C3 = u4(cc0), u4(cc1), u4(cc2), u4(cc3)
  local K0, K1, K2, K3 = u4(ck0), u4(ck1), u4(ck2), u4(ck3)
  local K4, K5, K6, K7 = u4(ck4), u4(ck5), u4(ck6), u4(ck7)
  local N0, N1, N2 = u4(cn0), u4(cn1), u4(cn2)
  local counter, step, checksum = u4(0, 1, 2, 3), u4(4), u4(0)
  for _ = 1, CHACHA_PASSES*CHACHA_BLOCKS/4 do
    local x0, x1, x2, x3 = C0, C1, C2, C3
    local x4, x5, x6, x7 = K0, K1, K2, K3
    local x8, x9, x10, x11 = K4, K5, K6, K7
    local x12, x13, x14, x15 = counter, N0, N1, N2
    for _ = 1, 10 do
      x0, x4, x8, x12 = chacha_qr_vec(x0, x4, x8, x12)
      x1, x5, x9, x13 = chacha_qr_vec(x1, x5, x9, x13)
      x2, x6, x10, x14 = chacha_qr_vec(x2, x6, x10, x14)
      x3, x7, x11, x15 = chacha_qr_vec(x3, x7, x11, x15)
      x0, x5, x10, x15 = chacha_qr_vec(x0, x5, x10, x15)
      x1, x6, x11, x12 = chacha_qr_vec(x1, x6, x11, x12)
      x2, x7, x8, x13 = chacha_qr_vec(x2, x7, x8, x13)
      x3, x4, x9, x14 = chacha_qr_vec(x3, x4, x9, x14)
    end
    x0, x1, x2, x3 = x0+C0, x1+C1, x2+C2, x3+C3
    x4, x5, x6, x7 = x4+K0, x5+K1, x6+K2, x7+K3
    x8, x9, x10, x11 = x8+K4, x9+K5, x10+K6, x11+K7
    x12, x13, x14, x15 = x12+counter, x13+N0, x14+N1, x15+N2
    checksum = checksum + x0+x1+x2+x3 + x4+x5+x6+x7 +
	       x8+x9+x10+x11 + x12+x13+x14+x15
    counter = counter + step
  end
  return tobit(tonumber(checksum[0]) + tonumber(checksum[1]) +
	       tonumber(checksum[2]) + tonumber(checksum[3]))
end

local function chacha_ymm()
  local C0, C1, C2, C3 = u8(cc0), u8(cc1), u8(cc2), u8(cc3)
  local K0, K1, K2, K3 = u8(ck0), u8(ck1), u8(ck2), u8(ck3)
  local K4, K5, K6, K7 = u8(ck4), u8(ck5), u8(ck6), u8(ck7)
  local N0, N1, N2 = u8(cn0), u8(cn1), u8(cn2)
  local counter = u8(0, 1, 2, 3, 4, 5, 6, 7)
  local step, checksum = u8(8), u8(0)
  for _ = 1, CHACHA_PASSES*CHACHA_BLOCKS/8 do
    local x0, x1, x2, x3 = C0, C1, C2, C3
    local x4, x5, x6, x7 = K0, K1, K2, K3
    local x8, x9, x10, x11 = K4, K5, K6, K7
    local x12, x13, x14, x15 = counter, N0, N1, N2
    for _ = 1, 10 do
      x0, x4, x8, x12 = chacha_qr_vec(x0, x4, x8, x12)
      x1, x5, x9, x13 = chacha_qr_vec(x1, x5, x9, x13)
      x2, x6, x10, x14 = chacha_qr_vec(x2, x6, x10, x14)
      x3, x7, x11, x15 = chacha_qr_vec(x3, x7, x11, x15)
      x0, x5, x10, x15 = chacha_qr_vec(x0, x5, x10, x15)
      x1, x6, x11, x12 = chacha_qr_vec(x1, x6, x11, x12)
      x2, x7, x8, x13 = chacha_qr_vec(x2, x7, x8, x13)
      x3, x4, x9, x14 = chacha_qr_vec(x3, x4, x9, x14)
    end
    x0, x1, x2, x3 = x0+C0, x1+C1, x2+C2, x3+C3
    x4, x5, x6, x7 = x4+K0, x5+K1, x6+K2, x7+K3
    x8, x9, x10, x11 = x8+K4, x9+K5, x10+K6, x11+K7
    x12, x13, x14, x15 = x12+counter, x13+N0, x14+N1, x15+N2
    checksum = checksum + x0+x1+x2+x3 + x4+x5+x6+x7 +
	       x8+x9+x10+x11 + x12+x13+x14+x15
    counter = counter + step
  end
  local sum = 0
  for i = 0, 7 do sum = sum + tonumber(checksum[i]) end
  return tobit(sum)
end

real_benches.chacha = {
  scalar = chacha_scalar, xmm = chacha_xmm, ymm = chacha_ymm,
  name = "ChaCha20 block core", blocks = CHACHA_BLOCKS*CHACHA_PASSES,
}
end

--------------------------------------------------------------------------

io.write(string.format("N=%d, %d passes, best of %d\n", N, PASSES, REPS))
local ts, ss = bench("saxpy scalar", saxpy_scalar)
local tx, sx = bench("saxpy XMM", saxpy_vector)
if has_ymm then
  local ty, sy = bench("saxpy YMM", saxpy_ymm)
  report_ymm("saxpy (float)", ts, tx, ty, ss, sx, sy)
else
  report("saxpy (float)", ts, tx, ss, sx)
end

ts, ss = bench("dot scalar", dot_scalar)
tx, sx = bench("dot XMM", dot_vector)
if has_ymm then
  local ty = bench("dot YMM", dot_ymm)
  report_ymm("dot product (float)", ts, tx, ty, nil, nil, nil)
else
  report("dot product (float)", ts, tx, nil, nil)
end

ts, ss = bench("max scalar", max_scalar)
tx, sx = bench("max XMM", max_vector)
if has_ymm then
  local ty, sy = bench("max YMM", max_ymm)
  report_ymm("horizontal max (int32)", ts, tx, ty, ss, sx, sy)
else
  report("horizontal max (int32)", ts, tx, ss, sx)
end

ts, ss = bench("clamp scalar", clamp_scalar)
tx, sx = bench("clamp XMM", clamp_vector)
if has_ymm then
  local ty, sy = bench("clamp YMM", clamp_ymm)
  report_ymm("clamp (float)", ts, tx, ty, ss, sx, sy)
else
  report("clamp (float)", ts, tx, ss, sx)
end

ts, ss = bench("delta scalar", delta_scalar)
tx, sx = bench("delta XMM", delta_xmm)
if has_ymm then
  local ty, sy = bench("delta YMM", delta_ymm)
  report_ymm("delta encode (int32)", ts, tx, ty, ss, sx, sy)
else
  report("delta encode (int32)", ts, tx, ss, sx)
end

io.write(string.format(
  "\nHeavy kernels: FIR %dx%d, polynomial %dx%d, Mandelbrot %dx%d\n",
  FIR_N, FIR_PASSES, POLY_N, POLY_PASSES, MANDEL_N, MANDEL_PASSES))

ts, ss = bench("FIR scalar", fir_scalar)
tx, sx = bench("FIR XMM", fir_xmm)
if has_ymm then
  local ty, sy = bench("FIR YMM", fir_ymm)
  report_ymm("8-tap FIR (float)", ts, tx, ty, ss, sx, sy)
else
  report("8-tap FIR (float)", ts, tx, ss, sx)
end

ts, ss = bench("polynomial scalar", poly_scalar)
tx, sx = bench("polynomial XMM", poly_xmm)
if has_ymm then
  local ty, sy = bench("polynomial YMM", poly_ymm)
  report_ymm("degree-11 poly (double)", ts, tx, ty, ss, sx, sy)
else
  report("degree-11 poly (double)", ts, tx, ss, sx)
end

ts, ss = bench("Mandelbrot scalar", mandel_scalar)
tx, sx = bench("Mandelbrot XMM", mandel_xmm)
if has_ymm then
  local ty, sy = bench("Mandelbrot YMM", mandel_ymm)
  report_ymm("Mandelbrot 64-it (double)", ts, tx, ty, ss, sx, sy)
else
  report("Mandelbrot 64-it (double)", ts, tx, ss, sx)
end

io.write(string.format(
  "\nReal-world kernels: 1080p RGBA merge + blur, 4K depth tiles, %.0f MiB checksums, %.0f MiB INT8 ranges + %.0f MiB ternary dots, %.0fs PCM envelope, %.1fs fixed FIR, %.2fs float FIR, %d particles, %d ChaCha blocks\n",
  real_benches.checksum.mib, real_benches.activations.mib,
  real_benches.ternary.mib,
  real_benches.pcm.seconds, real_benches.pcmfir.seconds,
  real_benches.audio.seconds,
  real_benches.particles.count,
  real_benches.chacha.blocks))

local function run_real(b)
  -- Several kernels share a polymorphic helper between scalar, XMM and YMM
  -- forms. Start each measurement with a clean trace cache so a side trace
  -- specialised for the previous width cannot become the result.
  jit_.flush()
  local tsr, ssr = bench(b.name .. " scalar", b.scalar)
  jit_.flush()
  local txr, sxr = bench(b.name .. " XMM", b.xmm)
  if has_ymm then
    jit_.flush()
    local tyr, syr = bench(b.name .. " YMM", b.ymm)
    report_ymm(b.name, tsr, txr, tyr, ssr, sxr, syr, b.cmp)
  else
    report(b.name, tsr, txr, ssr, sxr, b.cmp)
  end
end

run_real(real_benches.rgba)
run_real(real_benches.checksum)
run_real(real_benches.depth)
run_real(real_benches.pcm)
run_real(real_benches.pcmfir)
run_real(real_benches.activations)
run_real(real_benches.ternary)
run_real(real_benches.blur)
run_real(real_benches.audio)
run_real(real_benches.particles)
run_real(real_benches.chacha)

if failures > 0 then
  io.write(string.format("\n%d benchmark(s) produced a wrong result\n", failures))
  os.exit(1)
end
