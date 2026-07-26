-- SIMD microbenchmarks: vectorized vs. equivalent scalar code.
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
local i4 = ffi.typeof("i32x4")

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
  io.write(string.format("%-26s scalar %7.1f ms   vector %7.1f ms   %5.2fx\n",
			 name, tscalar*1000, tvector*1000, tscalar/tvector))
  if ss ~= sv then
    io.write(string.format("   ! results differ: %s vs %s\n",
			   tostring(ss), tostring(sv)))
  end
end

------------------------------------------------------------------ saxpy ----

local xs = ffi.new("float[?]", N)
local ys = ffi.new("float[?]", N)
for i = 0, N-1 do xs[i] = i % 17; ys[i] = i % 13 end

local xv = ffi.cast(ffi.typeof("$ *", f4), xs)
local yv = ffi.cast(ffi.typeof("$ *", f4), ys)
local NV = N / 4

local function saxpy_scalar()
  local a = 2.5
  local s = 0
  for _ = 1, PASSES do
    for i = 0, N-1 do ys[i] = a * xs[i] + ys[i] end
    s = s + ys[0]
  end
  return s
end

local function saxpy_vector()
  local a = f4(2.5)
  local s = 0
  for _ = 1, PASSES do
    for i = 0, NV-1 do yv[i] = a * xv[i] + yv[i] end
    s = s + yv[0][0]
  end
  return s
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

-------------------------------------------------------------- integer max ---

local is = ffi.new("int32_t[?]", N)
for i = 0, N-1 do is[i] = (i * 2654435761) % 1000003 - 500000 end
local iv = ffi.cast(ffi.typeof("$ *", i4), is)

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

--------------------------------------------------------------------------

io.write(string.format("N=%d, %d passes, best of %d\n", N, PASSES, REPS))
local ts, ss = bench("saxpy scalar", saxpy_scalar)
local tv, sv = bench("saxpy vector", saxpy_vector)
report("saxpy (float)", ts, tv, ss, sv)

ts, ss = bench("dot scalar", dot_scalar)
tv, sv = bench("dot vector", dot_vector)
report("dot product (float)", ts, tv, nil, nil)

ts, ss = bench("max scalar", max_scalar)
tv, sv = bench("max vector", max_vector)
report("horizontal max (int32)", ts, tv, ss, sv)

ts, ss = bench("clamp scalar", clamp_scalar)
tv, sv = bench("clamp vector", clamp_vector)
report("clamp (float)", ts, tv, ss, sv)
