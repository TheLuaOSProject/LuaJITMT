-- SIMD microbenchmarks: scalar vs. 128-bit XMM vs. 256-bit AVX2 code.
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

local REPS = tonumber(arg[1] or "5")
local N = 1 << 16           -- elements per pass
local PASSES = 200

local f4 = ffi.typeof("float4")
local d2 = ffi.typeof("double2")
local i4 = ffi.typeof("i32x4")
local i64x2 = ffi.typeof("i64x2")
local features = simd.features()
local has_ymm = features.avx2 and features.vecsize >= 32
local f8 = has_ymm and ffi.typeof("float8")
local d4 = has_ymm and ffi.typeof("double4")
local i8 = has_ymm and ffi.typeof("i32x8")
local i64x4 = has_ymm and ffi.typeof("i64x4")
local f4p = ffi.typeof("$ *", f4)
local d2p = ffi.typeof("$ *", d2)
local f8p = has_ymm and ffi.typeof("$ *", f8)
local d4p = has_ymm and ffi.typeof("$ *", d4)

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

local function report(name, tscalar, tvector, ss, sv)
  io.write(string.format("%-26s scalar %7.1f ms   XMM %7.1f ms   %5.2fx\n",
			 name, tscalar*1000, tvector*1000, tscalar/tvector))
  if ss ~= sv then
    io.write(string.format("   ! results differ: %s vs %s\n",
			   tostring(ss), tostring(sv)))
  end
end

local function report_ymm(name, ts, tx, ty, ss, sx, sy)
  io.write(string.format(
    "%-26s scalar %7.1f ms   XMM %7.1f ms %5.2fx   YMM %7.1f ms %5.2fx (%4.2fx/XMM)\n",
    name, ts*1000, tx*1000, ts/tx, ty*1000, ts/ty, tx/ty))
  if ss ~= sx or ss ~= sy then
    io.write(string.format("   ! results differ: %s vs %s vs %s\n",
			   tostring(ss), tostring(sx), tostring(sy)))
  end
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
