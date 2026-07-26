-- Interpreter vs JIT differential tests.
--
-- Every case runs the *same* closure interpreted once and compiled many
-- times, then compares bit for bit. This is what catches recorder and
-- backend bugs that only show up after a trace has been linked.
local T = require("simdtest")
local ffi, simd, test, check, checkeq = T.ffi, T.simd, T.test, T.check, T.checkeq
local jit_ = require("jit")

local SEED = tonumber(os.getenv("SIMD_SEED") or "20260725")

-- This file needs a working JIT compiler.
if not pcall(jit_.on) then
  test("JIT compiler", function()
    check(true, "JIT permanently disabled by a build option, differential tests skipped")
  end)
  return T
end

-- Run f interpreted, then compiled, and compare all return values.
local function diff(name, f, ...)
  jit_.off()
  jit_.flush()
  local ref = {f(...)}
  jit_.on()
  local got
  for _ = 1, 4 do got = {f(...)} end
  jit_.off()
  check(#ref == #got, name .. ": result count")
  for i = 1, #ref do
    local a, b = ref[i], got[i]
    if type(a) == "cdata" and type(b) == "cdata" then
      checkeq(b, a, name .. " result " .. i)
    else
      checkeq(b, a, name .. " result " .. i)
    end
  end
end

test("loop-carried accumulator", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + ti.bits)
    local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
    local ct = ti.ct
    diff(ti.name .. " acc", function(n)
      local acc = ct(0)
      for _ = 1, n do acc = acc + a * b - ct(1) end
      return acc
    end, 500)
  end
end)

test("packed mulhi emulations", function()
  for _, name in ipairs({
    "i8x16", "u8x16", "i32x4", "u32x4", "i64x2", "u64x2",
  }) do
    local ti = T.T[name]
    local rnd = T.rng(SEED + (ti.signed and 811 or 977))
    local a, k, ct = T.rand(ti, rnd), T.rand(ti, rnd), ti.ct
    diff(name .. " mulhi", function(n)
      local acc = a
      for _ = 1, n do acc = simd.mulhi(acc + a, k) end
      return acc
    end, ti.bits == 64 and 120 or 240)
    if ti.bits == 8 or ti.bits >= 32 then
      diff(name .. " mulhi square", function(n)
	local acc = a
	for _ = 1, n do acc = simd.mulhi(acc, acc) + k end
	return acc
      end, ti.bits == 64 and 120 or 240)
    end
  end
end)

test("division and mixed scalars", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + 3 * ti.bits)
    local a = T.rand(ti, rnd)
    local ct = ti.ct
    if ti.fp then
      diff(ti.name .. " div", function(n)
	local acc = ct(1)
	for _ = 1, n do acc = (acc + a) / ct(2) end
	return acc
      end, 400)
    end
    diff(ti.name .. " scalar k", function(n)
      local acc = ct(0)
      for _ = 1, n do acc = acc + a * 3 - 1 end
      return acc
    end, 400)
    diff(ti.name .. " scalar k reversed", function(n)
      local acc = ct(0)
      for _ = 1, n do acc = 7 - (acc + a) end
      return acc
    end, 400)
    diff(ti.name .. " unary minus", function(n)
      local acc = ct(0)
      for _ = 1, n do acc = -(acc + a) end
      return acc
    end, 400)
  end
end)

test("non-constant scalar operand", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + 5 * ti.bits)
    local a = T.rand(ti, rnd)
    local ct = ti.ct
    diff(ti.name .. " variable scalar", function(n)
      local acc = ct(0)
      for i = 1, n do acc = acc + a * (i % 5) end
      return acc
    end, 300)
  end
end)

test("constant integer modulo is exact at signed extremes", function()
  local bit_ = require("bit")
  local values = {
    -2147483648, -2147483647, -1000000001, -38, -37, -8, -7, -2, -1,
    0, 1, 2, 7, 8, 37, 38, 1000000001, 2147483646, 2147483647,
  }
  diff("constant integer modulo", function(n)
    local a, b, c, d, e, f = 0, 0, 0, 0, 0, 0
    for i = 1, n do
      local x = values[(i % #values) + 1]
      a = bit_.bxor(a, x % 3)
      b = bit_.bxor(b, x % -3)
      c = bit_.bxor(c, x % 37)
      d = bit_.bxor(d, x % -37)
      e = bit_.bxor(e, x % 2147483647)
      f = bit_.bxor(f, x % -2147483648)
    end
    return a, b, c, d, e, f
  end, 600)
end)

test("byte-aligned integer rotate idioms", function()
  for _, c in ipairs({
    {T.T.i16x8, function(x)
       return simd.bor(simd.shl(x, 8), simd.shr(x, 8))
     end},
    {T.T.u32x4, function(x)
       return simd.bor(simd.shl(x, 8), simd.shr(x, 24))
     end},
    {T.T.i32x4, function(x)
       return simd.bxor(simd.shr(x, 24), simd.shl(x, 8))
     end},
    {T.T.i64x2, function(x)
       return simd.bor(simd.shl(x, 32), simd.shr(x, 32))
     end},
  }) do
    local ti, rot = c[1], c[2]
    local rnd = T.rng(SEED + 1709 + ti.bits)
    local a, step = T.rand(ti, rnd), T.rand(ti, rnd)
    diff(ti.name .. " byte rotate", function(n)
      local acc = a
      for _ = 1, n do
	local x = acc + step
	acc = rot(x)
      end
      return acc
    end, 300)
  end
end)

test("byte rotates remain exact under vector register pressure", function()
  local ct = T.T.u32x4.ct
  local function rot(x)
    return simd.bor(simd.shl(x, 8), simd.shr(x, 24))
  end
  local k1,k2,k3,k4,k5,k6,k7 = ct(1),ct(2),ct(3),ct(4),ct(5),ct(6),ct(7)
  local k8,k9,k10,k11,k12,k13,k14 =
    ct(8),ct(9),ct(10),ct(11),ct(12),ct(13),ct(14)
  diff("byte rotate pressure", function(n)
    local a1,a2,a3,a4,a5,a6,a7 = ct(0),ct(0),ct(0),ct(0),ct(0),ct(0),ct(0)
    local a8,a9,a10,a11,a12,a13,a14 =
      ct(0),ct(0),ct(0),ct(0),ct(0),ct(0),ct(0)
    for _ = 1, n do
      a1=rot(a1+k1); a2=rot(a2+k2); a3=rot(a3+k3); a4=rot(a4+k4)
      a5=rot(a5+k5); a6=rot(a6+k6); a7=rot(a7+k7); a8=rot(a8+k8)
      a9=rot(a9+k9); a10=rot(a10+k10); a11=rot(a11+k11)
      a12=rot(a12+k12); a13=rot(a13+k13); a14=rot(a14+k14)
    end
    return a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14
  end, 200)
end)

test("vectors from memory", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + 7 * ti.bits)
    local N = 64
    local arr = ffi.new(ffi.typeof("$[?]", ti.ct), N)
    for i = 0, N-1 do arr[i] = T.rand(ti, rnd) end
    local out = ffi.new(ffi.typeof("$[?]", ti.ct), N)
    local ct = ti.ct
    diff(ti.name .. " array map", function()
      for i = 0, N-1 do out[i] = arr[i] * arr[(i+1) % N] + ct(1) end
      local acc = ct(0)
      for i = 0, N-1 do acc = acc + out[i] end
      return acc
    end)
  end
end)

test("unaligned vector loads", function()
  local ti = T.T.float4
  local buf = ffi.new("float[68]")
  for i = 0, 67 do buf[i] = i * 0.5 end
  local vp = ffi.cast(ffi.typeof("$ *", ti.ct), ffi.cast("char *", buf) + 1)
  -- 1 byte offset: every load and store is unaligned.
  local raw = ffi.new("char[?]", 16*17+1)
  local vp2 = ffi.cast(ffi.typeof("$ *", ti.ct), raw + 1)
  diff("unaligned", function(n)
    local acc = ti.ct(0)
    for i = 0, 15 do vp2[i] = ti.ct(i) end
    for _ = 1, n do
      for i = 0, 15 do acc = acc + vp2[i] end
    end
    return acc
  end, 20)
end)

test("vectors live across a guard and a side exit", function()
  local ti = T.T.float4
  local ct = ti.ct
  local a = ct(1, 2, 3, 4)
  diff("guarded", function(n)
    local acc = ct(0)
    local hits = 0
    for i = 1, n do
      local v = acc + a
      -- The comparison exits the trace on every 37th iteration, with v live.
      if i % 37 == 0 then hits = hits + 1 end
      acc = v * ct(1.5)
    end
    return acc, hits
  end, 500)
end)

test("side exit rebuilds a sunk vector box", function()
  local ti = T.T.i32x4
  local ct = ti.ct
  local a = ct(1, 2, 3, 4)
  -- The accumulator's cdata box is sunk in the loop, so leaving the loop must
  -- materialise it from the exit state.
  diff("sunk box", function(n)
    local acc = ct(0)
    for i = 1, n do
      acc = acc + a
      if i == n then return acc, acc[0], acc[3] end
    end
  end, 300)
end)

test("high register pressure forces spills", function()
  local ti = T.T.float4
  local ct = ti.ct
  local v = {}
  local rnd = T.rng(SEED + 99)
  for i = 1, 20 do v[i] = T.rand(ti, rnd) end
  diff("pressure", function(n)
    local a1, a2, a3, a4, a5 = ct(0), ct(0), ct(0), ct(0), ct(0)
    local a6, a7, a8, a9, a10 = ct(0), ct(0), ct(0), ct(0), ct(0)
    local a11, a12, a13, a14 = ct(0), ct(0), ct(0), ct(0)
    for _ = 1, n do
      a1 = a1 + v[1]; a2 = a2 + v[2]; a3 = a3 + v[3]; a4 = a4 + v[4]
      a5 = a5 + v[5]; a6 = a6 + v[6]; a7 = a7 + v[7]; a8 = a8 + v[8]
      a9 = a9 + v[9]; a10 = a10 + v[10]; a11 = a11 + v[11]
      a12 = a12 + v[12]; a13 = a13 + v[13]; a14 = a14 + v[14]
    end
    return a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14
  end, 300)
end)

test("repeated exits and trace stitching", function()
  local ti = T.T.double2
  local ct = ti.ct
  local a, b = ct(1.25, -2.5), ct(0.5, 4)
  diff("stitch", function(n)
    local acc = ct(0)
    for i = 1, n do
      if i % 3 == 0 then acc = acc + a else acc = acc * b end
      if i % 11 == 0 then acc = acc - ct(0.125) end
    end
    return acc
  end, 400)
end)

test("whole-vector equality on trace", function()
  for _, ti in ipairs(T.T) do
    local ct = ti.ct
    diff(ti.name .. " eq", function(n)
      local hits = 0
      local a = ct(1)
      for i = 1, n do
	local v = a + ct(i % 3)
	if v == ct(1) then hits = hits + 1 end
	if v == 3 then hits = hits + 10 end
      end
      return hits
    end, 300)
  end
end)

test("lane reads on trace", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + 11 * ti.bits)
    local a = T.rand(ti, rnd)
    local ct = ti.ct
    -- Accumulate lane values exactly. Summing them as Lua doubles would trip
    -- over a pre-existing LuaJIT difference in tonumber() for uint64 values
    -- above 2^63, which has nothing to do with vectors (see SIMD_STATUS.md).
    local acc64 = ti.bits == 64 and not ti.fp
    diff(ti.name .. " lanes", function(n)
      local s = acc64 and 0LL or 0
      local acc = ct(0)
      for _ = 1, n do
	acc = acc + a
	if acc64 then
	  s = s + ffi.cast("int64_t", acc[0]) + ffi.cast("int64_t", acc[ti.lanes-1])
	else
	  s = s + tonumber(acc[0]) + tonumber(acc[ti.lanes-1])
	end
      end
      return tostring(s), acc
    end, 200)
  end
end)

test("constant vectors are rematerialised", function()
  local ti = T.T.i32x4
  local ct = ti.ct
  diff("remat", function(n)
    local acc = ct(0)
    for _ = 1, n do
      acc = ct(1) - acc + ct(0x7fffffff) * ct(3) - ct(-5)
    end
    return acc
  end, 400)
end)

test("nested loops and inner traces", function()
  local ti = T.T.float4
  local ct = ti.ct
  local a = ct(1, 2, 3, 4)
  diff("nested", function(n, m)
    local acc = ct(0)
    for _ = 1, n do
      local inner = ct(1)
      for _ = 1, m do inner = inner * ct(1.0009765625) end
      acc = acc + inner + a
    end
    return acc
  end, 60, 60)
end)

test("garbage collection during vector loops", function()
  local ti = T.T.float4
  local ct = ti.ct
  local a = ct(1, 2, 3, 4)
  diff("gc", function(n)
    local acc = ct(0)
    local keep = {}
    for i = 1, n do
      acc = acc + a
      keep[(i % 16) + 1] = {i, tostring(i)}  -- allocate to trigger GC steps
    end
    collectgarbage()
    return acc
  end, 400)
end)

-- ffi.simd operations inside hot loops ---------------------------------------

-- Run a unary/binary ffi.simd operation in a loop and diff interp vs JIT.
local function diffop(ti, name, fn, ...)
  diff(ti.name .. " " .. name, fn, ...)
end

test("ffi.simd binary ops on trace", function()
  local bin = {"band", "bor", "bxor", "bandn", "min", "max"}
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + 23 * ti.bits)
    local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
    local ct = ti.ct
    for _, op in ipairs(bin) do
      local f = simd[op]
      diffop(ti, op, function(n)
	local acc = ct(0)
	for _ = 1, n do acc = f(f(acc, a), b) end
	return acc
      end, 300)
    end
    if ti.bits <= 16 and not ti.fp then
      for _, op in ipairs({"adds", "subs"}) do
	local f = simd[op]
	diffop(ti, op, function(n)
	  local acc = ct(0)
	  for _ = 1, n do acc = f(f(acc, a), b) end
	  return acc
	end, 300)
      end
    end
  end
end)

test("ffi.simd unary ops on trace", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + 29 * ti.bits)
    local a = T.rand(ti, rnd)
    local ct = ti.ct
    diffop(ti, "bnot", function(n)
      local acc = ct(0)
      for _ = 1, n do acc = simd.bnot(simd.bxor(acc, a)) end
      return acc
    end, 300)
    diffop(ti, "abs", function(n)
      local acc = ct(0)
      for _ = 1, n do acc = simd.abs(simd.bxor(acc, a)) end
      return acc
    end, 300)
    if ti.fp then
      diffop(ti, "sqrt", function(n)
	local acc = ct(4)
	for _ = 1, n do acc = simd.sqrt(acc + ct(1)) end
	return acc
      end, 300)
      for _, op in ipairs({"floor", "ceil", "trunc", "round"}) do
	local f = simd[op]
	diffop(ti, op, function(n)
	  local acc = ct(0)
	  for i = 1, n do acc = f(acc + ct(1.5)) end
	  return acc
	end, 300)
      end
    end
  end
end)

test("ffi.simd comparisons and masks on trace", function()
  local cmps = {"eq", "ne", "lt", "le", "gt", "ge"}
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + 31 * ti.bits)
    local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
    local ct = ti.ct
    for _, op in ipairs(cmps) do
      local f = simd[op]
      diffop(ti, op, function(n)
	local acc, mm = 0, nil
	for _ = 1, n do
	  mm = f(a, b)
	  acc = acc + simd.movemask(mm)
	  if simd.anyof(mm) then acc = acc + 1 end
	  if simd.allof(mm) then acc = acc + 100 end
	end
	return acc, mm
      end, 300)
    end
    diffop(ti, "select", function(n)
      local acc = ct(0)
      for _ = 1, n do acc = simd.select(simd.lt(a, b), acc + a, b) end
      return acc
    end, 300)
  end
end)

test("comparison-select min/max idioms on trace", function()
  local tabs = {T.T}
  if simd.features().avx2 then tabs[#tabs+1] = T.W end
  for _, tab in ipairs(tabs) do
    for _, ti in ipairs(tab) do
      if ti.fp or ti.bits < 64 then
	local rnd = T.rng(SEED + 131 * ti.bits + ti.lanes)
	local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
	diffop(ti, "select max/min operands", function(n)
	  local hi, lo
	  for _ = 1, n do
	    local m = simd.gt(a, b)
	    hi = simd.select(m, a, b)
	    lo = simd.select(m, b, a)
	  end
	  return hi, lo
	end, 300)
	if not ti.fp then
	  diffop(ti, "select inclusive max/min operands", function(n)
	    local hi, lo
	    for _ = 1, n do
	      hi = simd.select(simd.ge(a, b), a, b)
	      lo = simd.select(simd.le(a, b), a, b)
	    end
	    return hi, lo
	  end, 300)
	end
      end
    end
  end

  -- MAXPS/MINPS choose their second operand for unordered inputs and equal
  -- zeros. These raw patterns prove that the fold preserves the exact false
  -- arm, including NaN payloads and the sign of zero.
  local fa = simd.bitcast(T.T.float4.ct,
    T.T.u32x4.ct(0x80000000, 0, 0x7f801234, 0x3f800000))
  local fb = simd.bitcast(T.T.float4.ct,
    T.T.u32x4.ct(0, 0x80000000, 0x40000000, 0x7fc05678))
  diff("float select max/min special bits", function(n)
    local hi, lo
    for _ = 1, n do
      local m = simd.gt(fa, fb)
      hi, lo = simd.select(m, fa, fb), simd.select(m, fb, fa)
    end
    return hi, lo
  end, 300)

  local da = simd.bitcast(T.T.double2.ct,
    T.T.u64x2.ct(0x8000000000000000ULL, 0x7ff0000000001234ULL))
  local db = simd.bitcast(T.T.double2.ct,
    T.T.u64x2.ct(0, 0x4000000000000000ULL))
  diff("double select max/min special bits", function(n)
    local hi, lo
    for _ = 1, n do
      local m = simd.gt(da, db)
      hi, lo = simd.select(m, da, db), simd.select(m, db, da)
    end
    return hi, lo
  end, 300)

  -- A mask may legally have the same byte size but different comparison
  -- semantics. Bitcasts make the data refs identical, so this catches an
  -- over-broad structural matcher that ignores signedness or FP lane type.
  local sa, sb = T.T.i32x4.ct(-1, 2, -3, 4), T.T.i32x4.ct(1, -2, 3, -4)
  local ua = simd.bitcast(T.T.u32x4.ct, sa)
  local ub = simd.bitcast(T.T.u32x4.ct, sb)
  diff("signed mask selecting unsigned operands", function(n)
    local r
    for _ = 1, n do r = simd.select(simd.gt(sa, sb), ua, ub) end
    return r
  end, 300)
  local ia = simd.bitcast(T.T.i32x4.ct, fa)
  local ib = simd.bitcast(T.T.i32x4.ct, fb)
  diff("integer mask selecting float operands", function(n)
    local r
    for _ = 1, n do r = simd.select(simd.gt(ia, ib), fa, fb) end
    return r
  end, 300)
end)

test("ffi.simd shifts on trace", function()
  -- The count is a *literal* in this first loop. Lane widths without a shift
  -- instruction (8 bit lanes, and the 64 bit arithmetic shift) are rewritten
  -- by the recorder, and the rewrite is a different one for a constant count
  -- than for a variable count, so both have to be covered separately.
  -- test_codegen.lua asserts that the variable count really does compile.
  for _, ti in ipairs(T.T) do
    if not ti.fp then
      local rnd = T.rng(SEED + 37 * ti.bits)
      local vals = {}
      for j = 1, 6 do vals[j] = T.rand(ti, rnd) end
      -- Corner values the random generator is unlikely to produce.
      local corner = {}
      for j = 1, ti.lanes do corner[j] = j end
      vals[#vals+1] = T.vec(ti, corner)
      for _, op in ipairs({"shl", "shr", "sar"}) do
	for _, k in ipairs({0, 1, 2, ti.bits-2, ti.bits-1, ti.bits, ti.bits+7}) do
	  local src = "local simd, a = ...\n" ..
		      "return function(n) local r\n" ..
		      "  for _ = 1, n do r = simd." .. op .. "(a, " .. k .. ") end\n" ..
		      "  return r end"
	  for _, a in ipairs(vals) do
	    local fi = loadstring(src)(simd, a)
	    local fj = loadstring(src)(simd, a)
	    jit_.off(); jit_.flush()
	    local ref = fi(2)
	    jit_.on()
	    local got
	    for _ = 1, 3 do got = fj(200) end
	    jit_.off()
	    checkeq(got, ref, string.format("%s %s %d of %s",
		    ti.name, op, k, T.tostr(a)))
	  end
	end
      end
      -- A variable count as well. The counts sweep past the lane width and
      -- go negative, because the interpreter reads the count as uint32_t and
      -- treats everything from the lane width upwards as a full shift.
      local a = T.rand(ti, rnd)
      local ct = ti.ct
      for _, op in ipairs({"shl", "shr", "sar"}) do
	local f = simd[op]
	diffop(ti, op .. " var", function(n)
	  local acc = ct(0)
	  for i = 1, n do acc = f(simd.bxor(acc, a), i % (ti.bits + 2)) end
	  return acc
	end, 200)
	diffop(ti, op .. " var wrap", function(n)
	  local acc = ct(0)
	  for i = 1, n do acc = f(simd.bxor(acc, a), (i * 7) % (2*ti.bits) - 3) end
	  return acc
	end, 200)
      end
    end
  end
  if simd.features().avx2 then
    -- AVX2 only has variable shifts at dword/qword granularity. Most word
    -- shifts build packed power-of-two factors and use low/high products
    -- (XMM sar retains its faster dword split), while byte lanes use
    -- lookup/multiply identities. Cover every narrow boundary and a huge
    -- unsigned count on linked traces.
    for _, ti in ipairs(T.T) do
      if not ti.fp then
	local it, raw = T.masktype(ti), {}
	local pool = {0, 1, ti.bits-1, ti.bits, ti.bits+1, 2*ti.bits, -1, 3}
	for i = 1, ti.lanes do raw[i] = pool[(i-1) % #pool + 1] end
	local ct, a = ti.ct, T.rand(ti, T.rng(SEED + 43 * ti.bits))
	local counts = it.ct(unpack(raw, 1, ti.lanes))
	for _, op in ipairs({"shl", "shr", "sar"}) do
	  local f = simd[op]
	  diffop(ti, op .. " per-lane", function(n)
	    local acc = ct(0)
	    for _ = 1, n do acc = f(simd.bxor(acc, a), counts) end
	    return acc
	  end, 220)
	end
      end
    end
  end
end)

test("ffi.simd reductions on trace", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + 41 * ti.bits)
    local a = T.rand(ti, rnd)
    local ct = ti.ct
    for _, op in ipairs({"hsum", "hmin", "hmax"}) do
      local f = simd[op]
      diffop(ti, op, function(n)
	local last
	for i = 1, n do last = f(a + ct(i % 3)) end
	return tostring(last)
      end, 200)
    end
    if ti.bits == 8 then
      local av, bv
      if ti.signed then
	av = {-128,127,-127,126,-96,95,-65,64,-33,32,-17,16,-3,2,-1,0}
	bv = {127,-128,126,-127,95,-96,64,-65,32,-33,16,-17,2,-3,0,-1}
      else
	av = {255,254,253,252,224,192,160,128,96,64,32,16,8,4,2,1}
	bv = {255,1,254,2,253,3,252,4,251,5,250,6,249,7,248,8}
      end
      local x = ct(unpack(av, 1, ti.lanes))
      local y = ct(unpack(bv, 1, ti.lanes))
      diffop(ti, "hsum product", function(n)
	local product, last
	for i = 1, n do
	  product = (x + ct(i % 7)) * y
	  last = simd.hsum(product)
	end
	-- Keeping both values live covers the ordinary byte multiply and the
	-- fused reduction of that multiply on the same trace.
	return product, tostring(last)
      end, 200)
    elseif ti.bits == 16 then
      local av, bv
      if ti.signed then
	av = {-32768, -32768, 32767, -1, 12345, -23456, 30000, -30000}
	bv = {-32768, -32768, -32768, 32767, -23456, 12345, -30000, 30000}
      else
	av = {65535, 32768, 32767, 1, 54321, 23456, 60000, 30000}
	bv = {32768, 65535, 32769, 65535, 23456, 54321, 30000, 60000}
      end
      local x = ct(unpack(av, 1, ti.lanes))
      local y = ct(unpack(bv, 1, ti.lanes))
      diffop(ti, "hsum product", function(n)
	local product, last
	for i = 1, n do
	  product = (x + ct(i % 7)) * y
	  last = simd.hsum(product)
	end
	return product, tostring(last)
      end, 200)
    end
  end
end)

test("ffi.simd insert and shuffle on trace", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + 43 * ti.bits)
    local a, b = T.rand(ti, rnd), T.rand(ti, rnd)
    local ct = ti.ct
    local kv = ti.fp and 1.5 or 7
    for lane = 0, ti.lanes-1 do
      diffop(ti, "insert " .. lane, function(n)
	local acc = ct(0)
	for _ = 1, n do acc = simd.insert(acc + a, lane, kv) end
	return acc
      end, 200)
      -- Generate a literal lane index while making the inserted scalar change
      -- on every iteration. This reaches the direct PINSR/INSERTPS lowering,
      -- and exercises every immediate lane value against the interpreter.
      local src = string.format([[
	local simd, ct, add = ...
	return function(n)
	  local acc, value = ct(0), 1
	  for _ = 1, n do
	    value = value + %s
	    acc = simd.insert(acc + add, %d, value)
	  end
	  return acc
	end
      ]], ti.fp and "0.25" or "13", lane)
      local direct = assert(loadstring(src))(simd, ct, a)
      diffop(ti, "insert dynamic scalar " .. lane, direct, 200)
    end
    local rev, mix = {}, {}
    for i = 1, ti.lanes do
      rev[i] = ti.lanes - i
      mix[i] = (i % 2 == 1) and (i-1)/2 or ti.lanes + (i-2)/2
    end
    diffop(ti, "shuffle", function(n)
      local acc = ct(0)
      for _ = 1, n do acc = simd.shuffle(acc + a, unpack(rev, 1, ti.lanes)) end
      return acc
    end, 200)
    diffop(ti, "shuffle2", function(n)
      local acc = ct(0)
      for _ = 1, n do
	acc = simd.shuffle2(acc + a, b, unpack(mix, 1, ti.lanes))
      end
      return acc
    end, 200)
    diffop(ti, "shuffle2 one source", function(n)
      local acc = ct(0)
      for _ = 1, n do
	acc = simd.shuffle2(acc + a, b, unpack(rev, 1, ti.lanes))
      end
      return acc
    end, 200)
    local breverse = {}
    for i = 1, ti.lanes do breverse[i] = ti.lanes + rev[i] end
    diffop(ti, "shuffle2 second source", function(n)
      local acc = ct(0)
      for _ = 1, n do
	acc = simd.shuffle2(acc + a, b, unpack(breverse, 1, ti.lanes))
      end
      return acc
    end, 200)
    diffop(ti, "shuffle2 equal sources", function(n)
      local acc = ct(0)
      for _ = 1, n do
	local x = acc + a
	acc = simd.shuffle2(x, x, unpack(mix, 1, ti.lanes))
      end
      return acc
    end, 200)
  end
end)

test("two-source immediate shuffles on trace", function()
  local cases = {
    {T.T.i32x4.ct, {3, 0, 6, 5}, {7, 4, 2, 1}},
    {T.T.i64x2.ct, {1, 2}, {3, 0}},
    {T.T.i16x8.ct, {0,9,2,11,4,13,6,15},
		    {8,1,10,3,12,5,14,7}},
    {T.T.i8x16.ct,
      {0,1,18,19,4,5,22,23,8,9,26,27,12,13,30,31},
      {16,17,2,3,20,21,6,7,24,25,10,11,28,29,14,15}},
  }
  for i, c in ipairs(cases) do
    local ct, direct, swapped = c[1], c[2], c[3]
    local a, b = ct(1), ct(11)
    diff("shuffle2 immediate " .. i, function(n)
      local x, y = ct(0), ct(0)
      for _ = 1, n do
	x = simd.shuffle2(x + a, b, unpack(direct))
	y = simd.shuffle2(y + a, b, unpack(swapped))
      end
      return x, y
    end, 200)
  end
end)

test("two-source aligned windows on trace", function()
  for _, ti in ipairs({T.T.i8x16, T.T.i16x8, T.T.i32x4}) do
    local n, forward, reverse = ti.lanes, {}, {}
    for i = 0, n-1 do
      forward[i+1] = i+1 < n and i+1 or n
      reverse[i+1] = i+1 < n and n+i+1 or 0
    end
    local ct, a, b = ti.ct, ti.ct(1), ti.ct(11)
    diff(ti.name .. " aligned two-source windows", function(iters)
      local x, y = ct(0), ct(0)
      for _ = 1, iters do
	x = simd.shuffle2(x+a, b, unpack(forward, 1, n))
	y = simd.shuffle2(y+a, b, unpack(reverse, 1, n))
      end
      return x, y
    end, 200)
  end
end)

if simd.features().avx2 then
  test("full-width AVX2 dword blends on trace", function()
    local cases = {
      {T.W.i32x8, {0,9,2,3,12,5,14,7}},
      {T.W.u32x8, {0,9,2,3,12,5,14,7}},
      {T.W.float8, {0,9,2,3,12,5,14,7}},
      {T.W.i64x4, {4,1,2,7}},
      {T.W.u64x4, {4,1,2,7}},
      {T.W.double4, {4,1,2,7}},
    }
    for _, c in ipairs(cases) do
      local ti, ctl = c[1], c[2]
      local ct, a, b = ti.ct, ti.ct(1), ti.ct(11)
      diff(ti.name .. " independent full-width blend", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc+a, b, unpack(ctl, 1, ti.lanes))
	end
	return acc
      end, 120)
    end
  end)
end

test("type punning does not confuse store-to-load forwarding", function()
  -- A bitcast boxes the value under a new ctype, so the boxing store and the
  -- next load of the same address have different vector IR types. Forwarding
  -- must not invent a conversion between them, and must not hand a value with
  -- the wrong lane type to an instruction that depends on it.
  local f4, i4, b16 = T.T.float4, T.T.i32x4, T.T.i8x16
  local v = f4.ct(1.5, -2.5, 3.5, -4.5)
  diff("bitcast then add", function(n)
    local acc = i4.ct(0)
    for _ = 1, n do acc = acc + simd.bitcast(i4.ct, v) end
    return acc
  end, 200)
  -- movemask picks a different instruction per lane width, so a value
  -- forwarded with the wrong lane type would silently change the result.
  diff("bitcast then movemask", function(n)
    local s = 0
    for _ = 1, n do
      s = s + simd.movemask(simd.bitcast(b16.ct, v))
      s = s + simd.movemask(simd.bitcast(i4.ct, v))
      s = s + simd.movemask(v)
    end
    return s
  end, 200)
  -- Same memory, read back through two different vector pointer types.
  local raw = ffi.new("char[64]")
  local pf = ffi.cast(ffi.typeof("$ *", f4.ct), raw)
  local pi = ffi.cast(ffi.typeof("$ *", i4.ct), raw)
  diff("aliasing vector pointers", function(n)
    local acc = i4.ct(0)
    for i = 1, n do
      pf[0] = f4.ct(i)
      acc = acc + pi[0]
    end
    return acc, pf[0]
  end, 200)
  -- Reductions extract lane 0 and the extract depends on the lane width too.
  diff("bitcast then reduce", function(n)
    local s = 0
    for _ = 1, n do
      s = s + tonumber(simd.hsum(simd.bitcast(i4.ct, v)))
      s = s + simd.hsum(v)
    end
    return s
  end, 200)
end)

test("fma memory operands do not cross an intervening store", function()
  if not simd.features().fma then return end
  local f4 = T.T.float4.ct
  local raw = ffi.new("float[4]")
  local p = ffi.cast(ffi.typeof("$ *", f4), raw)
  local q = ffi.cast(ffi.typeof("$ *", f4), raw)
  diff("aliased load, store, then fma", function(n)
    local acc, k = f4(1), f4(1)
    p[0] = f4(2)
    for i = 1, n do
      local old = p[0]
      q[0] = f4(i)
      acc = simd.fma(acc, k, old)
    end
    return acc, p[0]
  end, 200)
end)

test("shuffle2 memory operands do not cross an intervening store", function()
  local i4 = T.T.i32x4.ct
  local raw = ffi.new("int32_t[4]")
  local p = ffi.cast(ffi.typeof("$ *", i4), raw)
  local q = ffi.cast(ffi.typeof("$ *", i4), raw)
  diff("aliased load, store, then native shuffle2", function(n)
    local acc, one = i4(1), i4(1)
    p[0] = i4(2, 3, 4, 5)
    for i = 1, n do
      local old = p[0]
      q[0] = i4(i)
      acc = simd.shuffle2(acc + one, old, 3, 0, 6, 5)
    end
    return acc, p[0]
  end, 200)
end)

test("side traces replay sunk vector boxes", function()
  -- A rare branch inside a hot loop becomes a hot side exit and grows its own
  -- side trace, which has to replay the parent's snapshot. When the sunk box
  -- holds a constant vector that goes through snap_replay_const().
  local i4, f4 = T.T.i32x4.ct, T.T.float4.ct
  local sink
  local function run(f, n)
    jit_.off(); jit_.flush(); sink = 0
    local r1, s1 = f(n), sink
    jit_.on(); sink = 0
    local r2, s2 = f(n), sink
    jit_.off()
    return r1, s1, r2, s2
  end
  local cases = {
    {"constant box", function(n)
       local acc = i4(0)
       for i = 1, n do
	 local k = i4(7)
	 if i % 50 == 0 then sink = sink + k[0] + k[3] end
	 acc = acc + k
       end
       return acc[0]
     end},
    {"runtime box", function(n)
       local acc, a = i4(1), i4(3)
       for i = 1, n do
	 local k = acc + a
	 if i % 50 == 0 then sink = sink + k[0] end
	 acc = k
       end
       return acc[0]
     end},
    {"two sunk boxes", function(n)
       local acc, a = f4(0), f4(1.5)
       for i = 1, n do
	 local p = acc + a
	 local q = simd.min(p, f4(1000))
	 if i % 37 == 0 then sink = sink + p[0] + q[1] end
	 acc = q
       end
       return acc[0]
     end},
    {"mask box", function(n)
       local acc, a = i4(0), i4(2)
       for i = 1, n do
	 local m = simd.lt(acc, i4(500))
	 local k = simd.select(m, acc + a, i4(0))
	 if i % 41 == 0 then sink = sink + simd.movemask(m) + k[2] end
	 acc = k
       end
       return acc[0]
     end},
  }
  for _, c in ipairs(cases) do
    local r1, s1, r2, s2 = run(c[2], 4000)
    checkeq(r2, r1, "side trace " .. c[1] .. " result")
    checkeq(s2, s1, "side trace " .. c[1] .. " side-exit sum")
  end
end)

test("comparisons of different lane widths are not merged", function()
  -- CSE matches on opcode and operands. For vectors the lane type is part of
  -- the instruction (PCMPEQB is not PCMPEQD), so two compares of the same two
  -- vectors at different widths must stay separate.
  local i4, b16 = T.T.i32x4.ct, T.T.i8x16.ct
  local a = i4(0x00000001, 0x01020304, 5, 6)
  local b = i4(0x00000101, 0x01020305, 5, 7)
  diff("byte and dword compare of the same operands", function(n)
    local whole, m4, m16 = 0, 0, 0
    for _ = 1, n do
      if a == b then whole = whole + 1 end       -- byte-wise internally
      m4 = simd.movemask(simd.eq(a, b))          -- dword-wise
      m16 = simd.movemask(simd.eq(simd.bitcast(b16, a), simd.bitcast(b16, b)))
    end
    return whole, m4, m16
  end, 200)
  -- Same shape for the ordering compares and for the shifts.
  diff("gt at two widths", function(n)
    local m4, m16 = 0, 0
    for _ = 1, n do
      m4 = simd.movemask(simd.gt(a, b))
      m16 = simd.movemask(simd.gt(simd.bitcast(b16, a), simd.bitcast(b16, b)))
    end
    return m4, m16
  end, 200)
end)

test("mask predicates record the right guard polarity", function()
  -- allof/anyof return a boolean and the recorder turns them into a guard, so
  -- it has to know which way the interpreter actually answered. Cover both
  -- polarities of both predicates, and an alternating case that forces the
  -- guard to fail and take a side exit.
  local ti = T.T.i32x4
  local ct = ti.ct
  local none = simd.lt(ct(1), ct(0))          -- all lanes clear
  local all = simd.lt(ct(0), ct(1))           -- all lanes set
  local some = simd.lt(ct(0, 1, 0, 1), ct(1)) -- two lanes set
  local cases = { {"none", none}, {"all", all}, {"some", some} }
  for _, c in ipairs(cases) do
    local m = c[2]
    diff("anyof " .. c[1], function(n)
      local x = 0
      for _ = 1, n do if simd.anyof(m) then x = x + 1 end end
      return x
    end, 200)
    diff("allof " .. c[1], function(n)
      local x = 0
      for _ = 1, n do if simd.allof(m) then x = x + 1 end end
      return x
    end, 200)
  end
  -- The recorder learns the answer from the value the fast function left
  -- behind, so a preceding operation that answered the other way must not be
  -- able to leak into the guard.
  diff("anyof false after a true compare", function(n)
    local x, v = 0, ct(1)
    for _ = 1, n do
      if v == v then x = x + 1 end
      if simd.anyof(none) then x = x + 100 end
      if simd.allof(some) then x = x + 1000 end
    end
    return x
  end, 200)
  diff("anyof true after a false compare", function(n)
    local x, v = 0, ct(1)
    for _ = 1, n do
      if v == ct(2) then x = x + 1 end
      if simd.anyof(all) then x = x + 100 end
      if simd.allof(all) then x = x + 1000 end
    end
    return x
  end, 200)
  diff("alternating masks", function(n)
    local x = 0
    for i = 1, n do
      local m = (i % 2 == 0) and all or none
      if simd.anyof(m) then x = x + 1 end
      if simd.allof(m) then x = x + 10 end
    end
    return x
  end, 300)
end)

test("variable lane index and scalar cdata operands on trace", function()
  for _, ti in ipairs(T.T) do
    local rnd = T.rng(SEED + 47 * ti.bits)
    local a = T.rand(ti, rnd)
    local ct, lanes = ti.ct, ti.lanes
    local kv = ti.fp and 1.5 or 7
    -- The lane index changes every iteration, so it cannot be a constant.
    diffop(ti, "insert var lane", function(n)
      local acc = ct(0)
      for i = 1, n do acc = simd.insert(acc + a, i % lanes, kv) end
      return acc
    end, 200)
    -- An out-of-range index must still raise, from the trace as well.
    local ok = pcall(function()
      for i = 1, 200 do simd.insert(a, i % (lanes + 1), kv) end
    end)
    check(not ok, ti.name .. ": out-of-range insert index must raise")
    -- A scalar cdata as the second operand of an ffi.simd call.
    local sc = ffi.cast(ti.fp and "double" or "int64_t", 3)
    diffop(ti, "scalar cdata operand", function(n)
      local acc = ct(0)
      for _ = 1, n do acc = simd.max(simd.min(acc + a, sc), simd.bxor(acc, sc)) end
      return acc
    end, 200)
  end
end)

test("ffi.simd bitcast and convert on trace", function()
  local f4, i4, u4 = T.T.float4, T.T.i32x4, T.T.u32x4
  diff("bitcast", function(n)
    local acc = i4.ct(0)
    for i = 1, n do acc = acc + simd.bitcast(i4.ct, f4.ct(i % 7)) end
    return acc
  end, 300)
  diff("convert i32->f32", function(n)
    local acc = f4.ct(0)
    for i = 1, n do acc = acc + simd.convert(f4.ct, i4.ct(i % 11)) end
    return acc
  end, 300)
  diff("convert f32->i32", function(n)
    local acc = i4.ct(0)
    for i = 1, n do acc = acc + simd.convert(i4.ct, f4.ct((i % 11) + 0.75)) end
    return acc
  end, 300)
  -- Float to integer conversion follows the packed instruction: it truncates
  -- toward zero and yields the indefinite value for NaN or out of range.
  diff("convert f32->i32 corner values", function(n)
    local acc = i4.ct(0)
    local vals = {0/0, 1/0, -1/0, 3e9, -3e9, 2147483647, -2147483648, 0.5}
    for i = 1, n do
      local v = f4.ct(vals[(i % 8) + 1], vals[((i+1) % 8) + 1],
		      vals[((i+2) % 8) + 1], vals[((i+3) % 8) + 1])
      acc = simd.bxor(acc, simd.convert(i4.ct, v))
    end
    return acc
  end, 300)
  diff("convert i32->u32", function(n)
    local acc = u4.ct(0)
    for i = 1, n do acc = acc + simd.convert(u4.ct, i4.ct(-i)) end
    return acc
  end, 300)

  diff("convert u32->f32", function(n)
    local acc = f4.ct(0)
    local v = u4.ct(0, 16777217, 0x80000001, 0xffffffff)
    local step = u4.ct(1, 257, 65537, 1048577)
    for _ = 1, n do
      acc = acc + simd.convert(f4.ct, v)
      v = v + step
    end
    return acc
  end, 300)

  local d2, i2, u2 = T.T.double2, T.T.i64x2, T.T.u64x2
  diff("convert i64->f64", function(n)
    local acc = d2.ct(0)
    local v = i2.ct(-9007199254740993LL, 0x7000000000000001LL)
    for _ = 1, n do
      acc = acc + simd.convert(d2.ct, v)
      v = v + i2.ct(1048577, -65537)
    end
    return acc
  end, 180)
  diff("convert u64->f64", function(n)
    local acc = d2.ct(0)
    local v = u2.ct(9007199254740993ULL, 0xf000000000000001ULL)
    for _ = 1, n do
      acc = acc + simd.convert(d2.ct, v)
      v = v + u2.ct(1048577, 65537)
    end
    return acc
  end, 180)
  diff("convert f64->i64/u64", function(n)
    local si, ui = i2.ct(0), u2.ct(0)
    local v = d2.ct(0/0, 1e300)
    for _ = 1, n do
      si = simd.bxor(si, simd.convert(i2.ct, v))
      ui = simd.bxor(ui, simd.convert(u2.ct, v))
      v = v + d2.ct(1, -1)
    end
    return si, ui
  end, 180)
end)

test("extended conversions randomized bit patterns", function()
  local pairs = {
    {T.T.u32x4, T.T.float4},
    {T.T.i64x2, T.T.double2},
    {T.T.u64x2, T.T.double2},
    {T.T.double2, T.T.i64x2},
    {T.T.double2, T.T.u64x2},
  }
  for p, pair in ipairs(pairs) do
    local src, dst = pair[1], pair[2]
    local rnd = T.rng(SEED + 2503 + p * 137)
    local a = ffi.new(ffi.typeof("$[?]", src.ct), 64)
    for i = 0, 63 do a[i] = T.rand(src, rnd) end
    diff(src.name .. " to " .. dst.name .. " random", function(n)
      local acc = dst.ct(0)
      for i = 0, n-1 do
	acc = simd.bxor(acc, simd.convert(dst.ct, a[i % 64]))
      end
      return acc
    end, 512)
  end
end)

if simd.features().avx2 then
  test("256-bit byte-aligned integer rotate idioms", function()
    for _, c in ipairs({
      {T.W.i16x16, function(x)
	 return simd.bor(simd.shl(x, 8), simd.shr(x, 8))
       end},
      {T.W.u32x8, function(x)
	 return simd.bor(simd.shl(x, 8), simd.shr(x, 24))
       end},
      {T.W.i64x4, function(x)
	 return simd.bor(simd.shl(x, 32), simd.shr(x, 32))
       end},
    }) do
      local ti, rot = c[1], c[2]
      local rnd = T.rng(SEED + 1877 + ti.bits)
      local a, step = T.rand(ti, rnd), T.rand(ti, rnd)
      diff(ti.name .. " ymm byte rotate", function(n)
	local acc = a
	for _ = 1, n do
	  local x = acc + step
	  acc = rot(x)
	end
	return acc
      end, 300)
    end
  end)

  test("256-bit add/sub differential", function()
    for _, ti in ipairs(T.W) do
      local lanes = {}
      for i = 1, ti.lanes do lanes[i] = i * 3 - 11 end
      local ct, a, one = ti.ct, T.vec(ti, lanes), ti.ct(1)
      diff(ti.name .. " ymm add/sub", function(n)
	local acc = ct(0)
	for _ = 1, n do acc = acc + a - one end
	return acc
      end, 300)
    end
  end)

  test("256-bit multiply, division, and dynamic scalar splats", function()
    for _, ti in ipairs(T.W) do
      local ct = ti.ct
      diff(ti.name .. " ymm multiply and splat", function(n)
	local acc, k = ct(1), ct(3)
	for i = 1, n do acc = acc * k + (i % 5) end
	return acc
      end, 80)
    end
    for _, name in ipairs({"float8", "double4"}) do
      local ct = T.W[name].ct
      diff(name .. " ymm divide", function(n)
	local acc, a = ct(1), ct(0.25)
	for _ = 1, n do acc = (acc + a) / ct(2) end
	return acc
      end, 300)
    end
  end)

  test("256-bit loop invariants survive scalar helper calls", function()
    local ct = T.W.i32x8.ct
    local step = ct(1, 2, 3, 4, 5, 6, 7, 8)
    diff("i32x8 ymm across scalar call", function(n)
      local acc, sum = ct(0), 0
      for i = 1, n do
	acc = acc + step
	sum = sum + math.sin(i)
      end
      return acc, sum, acc[0], acc[7]
    end, 160)
  end)

  test("256-bit values survive allocation and GC helper calls", function()
    local ct = T.W.i32x8.ct
    local step = ct(1, 2, 3, 4, 5, 6, 7, 8)
    diff("i32x8 ymm across allocation and gc", function(n)
      local acc, keep = ct(0), {}
      for i = 1, n do
	acc = acc + step
	keep[(i % 16) + 1] = {i, tostring(i)}
      end
      collectgarbage()
      return acc, acc[0], acc[7], #keep
    end, 400)
  end)

  test("256-bit min/max, comparisons, masks, select, and equality", function()
    for _, ti in ipairs(T.W) do
      local ct = ti.ct
      local av, bv = {}, {}
      for i = 1, ti.lanes do
	av[i] = i % 2 == 0 and i * 3 or 20 - i
	bv[i] = i % 3 == 0 and i - 9 or i + 2
      end
      local a, b = ct(unpack(av, 1, ti.lanes)), ct(unpack(bv, 1, ti.lanes))
      diff(ti.name .. " ymm compare/select", function(n)
	local acc, bits, hits = a, 0, 0
	for i = 1, n do
	  local lt = simd.lt(acc, b)
	  local ge = simd.ge(acc, b)
	  local eq = simd.eq(acc, b)
	  local ne = simd.ne(acc, b)
	  bits = bits + simd.movemask(lt) * 3 + simd.movemask(ge) * 5 +
		 simd.movemask(eq) * 7 + simd.movemask(ne) * 11
	  acc = simd.select(lt, simd.max(acc, b), simd.min(acc, b))
	  if acc == b then hits = hits + 1 end
	end
	return acc, bits, hits, simd.movemask(simd.eq(acc, acc)),
	       simd.anyof(simd.ne(acc, b)), simd.allof(simd.eq(acc, acc))
      end, 120)
    end
  end)

  test("256-bit floating comparisons preserve NaN and signed-zero semantics", function()
    for _, name in ipairs({"float8", "double4"}) do
      local ti, av, bv = T.W[name], {}, {}
      for i = 1, ti.lanes do
	av[i] = i == ti.lanes and 0/0 or i % 2 == 0 and -0.0 or i
	bv[i] = i == ti.lanes-1 and 0/0 or i % 2 == 0 and 0.0 or ti.lanes-i
      end
      local ct = ti.ct
      local a, b = ct(unpack(av, 1, ti.lanes)), ct(unpack(bv, 1, ti.lanes))
      diff(name .. " ymm fp edges", function(n)
	local lo, hi, bits = a, b, 0
	for _ = 1, n do
	  bits = bits + simd.movemask(simd.eq(lo, hi)) * 3 +
		 simd.movemask(simd.lt(lo, hi)) * 5
	  lo, hi = simd.min(lo, hi), simd.max(lo, hi)
	end
	return lo, hi, bits
      end, 120)
    end
  end)

  test("256-bit shifts", function()
    for _, ti in ipairs(T.W) do
      if not ti.fp then
	local ct = ti.ct
	local lanes = {}
	for i = 1, ti.lanes do lanes[i] = i % 2 == 0 and -i * 17 or i * 29 end
	local a = ct(unpack(lanes, 1, ti.lanes))
	diff(ti.name .. " ymm scalar shifts", function(n)
	  local acc = a
	  for i = 1, n do
	    local sh = i % (ti.bits + 3)
	    acc = simd.bxor(simd.shl(acc, sh),
			    simd.bxor(simd.shr(acc, sh), simd.sar(acc, sh)))
	  end
	  return acc
	end, 180)
      end
    end
    for _, name in ipairs({
      "i8x32", "u8x32", "i16x16", "u16x16",
      "i32x8", "u32x8", "i64x4", "u64x4",
    }) do
      local ti, counts = T.W[name], {}
      for i = 1, ti.lanes do counts[i] = (i * 11) % (ti.bits + 7) end
      local ct, c = ti.ct, ti.ct(unpack(counts, 1, ti.lanes))
      diff(name .. " ymm per-lane shifts", function(n)
	local acc = ct(-12345)
	for _ = 1, n do
	  acc = simd.bxor(simd.shl(acc, c),
			  simd.bxor(simd.shr(acc, c), simd.sar(acc, c)))
	end
	return acc
      end, 180)
    end
  end)

  test("256-bit abs, sqrt, rounding, and fma", function()
    for _, ti in ipairs(T.W) do
      local ct, lanes = ti.ct, {}
      for i = 1, ti.lanes do lanes[i] = i % 2 == 0 and -i - 0.75 or i + 0.25 end
      local a = ct(unpack(lanes, 1, ti.lanes))
      if ti.fp then
	diff(ti.name .. " ymm fp unary", function(n)
	  local acc = ct(0)
	  for _ = 1, n do
	    acc = acc + simd.sqrt(simd.abs(a)) + simd.floor(a) +
		  simd.ceil(a) + simd.trunc(a) + simd.round(a)
	  end
	  return acc
	end, 120)
	diff(ti.name .. " ymm fma", function(n)
	  local acc, k, c = ct(1), ct(1.0001), ct(0.0003)
	  for _ = 1, n do acc = simd.fma(acc, k, c) end
	  return acc
	end, 180)
      else
	diff(ti.name .. " ymm integer abs", function(n)
	  local acc = ct(0)
	  for _ = 1, n do acc = acc + simd.abs(a) end
	  return acc
	end, 120)
      end
    end
  end)

  test("256-bit saturating arithmetic and mulhi", function()
    for _, name in ipairs({"i8x32", "u8x32", "i16x16", "u16x16"}) do
      local ti, lanes = T.W[name], {}
      for i = 1, ti.lanes do
	lanes[i] = ti.signed and (i % 2 == 0 and -120 or 120) or 245
      end
      local ct, a = ti.ct, ti.ct(unpack(lanes, 1, ti.lanes))
      diff(name .. " ymm saturating", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.subs(simd.adds(acc, a), ct(ti.signed and -7 or 11))
	end
	return acc
      end, 120)
      diff(name .. " ymm mulhi", function(n)
	local k = ti.bits == 8 and ct(ti.signed and -117 or 233)
			       or ct(ti.signed and -23123 or 53123)
	local acc = a
	for _ = 1, n do acc = simd.mulhi(acc, k) + a end
	return acc
      end, 120)
      if ti.bits == 8 then
	diff(name .. " ymm mulhi square", function(n)
	  local acc, k = a, ct(3)
	  for _ = 1, n do acc = simd.mulhi(acc, acc) + k end
	  return acc
	end, 120)
      end
    end
  end)

  test("256-bit 32-bit mulhi", function()
    for _, name in ipairs({"i32x8", "u32x8"}) do
      local ti = T.W[name]
      local rnd = T.rng(SEED + (ti.signed and 1217 or 1429))
      local a, k, ct = T.rand(ti, rnd), T.rand(ti, rnd), ti.ct
      diff(name .. " ymm mulhi", function(n)
	local acc = a
	for _ = 1, n do acc = simd.mulhi(acc + a, k) end
	return acc
      end, 180)
    end
  end)

  test("256-bit 64-bit mulhi", function()
    for _, name in ipairs({"i64x4", "u64x4"}) do
      local ti = T.W[name]
      local rnd = T.rng(SEED + (ti.signed and 1871 or 1999))
      local a, k, ct = T.rand(ti, rnd), T.rand(ti, rnd), ti.ct
      diff(name .. " ymm mulhi", function(n)
	local acc = a
	for _ = 1, n do acc = simd.mulhi(acc + a, k) end
	return acc
      end, 100)
      diff(name .. " ymm mulhi square", function(n)
	local acc = a
	for _ = 1, n do acc = simd.mulhi(acc, acc) + k end
	return acc
      end, 100)
    end
  end)

  test("256-bit lane conversion", function()
    local fi, ii, ui = T.W.float8.ct, T.W.i32x8.ct, T.W.u32x8.ct
    local iv = ii(-100, -7, -1, 0, 1, 7, 100, 123456)
    local fv = fi(-100.75, -7.5, -1.25, 0, 1.25, 7.5, 100.75, 123456.5)
    diff("i32x8 to float8", function(n)
      local acc = fi(0)
      for _ = 1, n do acc = acc + simd.convert(fi, iv) end
      return acc
    end, 120)
    diff("float8 to i32x8", function(n)
      local acc = ii(0)
      for _ = 1, n do acc = acc + simd.convert(ii, fv) end
      return acc
    end, 120)
    diff("float8 to u32x8 hardware-range semantics", function(n)
      local acc = ui(0)
      for _ = 1, n do acc = acc + simd.convert(ui, fv) end
      return acc
    end, 120)
    local uv = ui(0, 1, 16777217, 0x7fffffff,
		  0x80000001, 0xffffff01, 0xfffffffe, 0xffffffff)
    diff("u32x8 to float8", function(n)
      local acc = fi(0)
      for _ = 1, n do acc = acc + simd.convert(fi, uv) end
      return acc
    end, 120)

    local fd, si, su = T.W.double4.ct, T.W.i64x4.ct, T.W.u64x4.ct
    local sv = si(-9007199254740993LL, 9007199254740993LL,
		  -0x7000000000000000LL, 0x7000000000000001LL)
    local uiv = su(0, 9007199254740993ULL, 0x8000000000000001ULL,
		   0xffffffffffffffffULL)
    diff("i64x4 to double4", function(n)
      local acc = fd(0)
      for _ = 1, n do acc = acc + simd.convert(fd, sv) end
      return acc
    end, 100)
    diff("u64x4 to double4", function(n)
      local acc = fd(0)
      for _ = 1, n do acc = acc + simd.convert(fd, uiv) end
      return acc
    end, 100)
    diff("double4 to i64x4/u64x4", function(n)
      local a, b = si(0), su(0)
      local v = fd(2.9, -2.9, 0/0, 1e300)
      for _ = 1, n do
	a = simd.bxor(a, simd.convert(si, v))
	b = simd.bxor(b, simd.convert(su, v))
      end
      return a, b
    end, 100)
  end)

  test("256-bit extended conversions randomized bit patterns", function()
    local pairs = {
      {T.W.u32x8, T.W.float8},
      {T.W.i64x4, T.W.double4},
      {T.W.u64x4, T.W.double4},
      {T.W.double4, T.W.i64x4},
      {T.W.double4, T.W.u64x4},
    }
    for p, pair in ipairs(pairs) do
      local src, dst = pair[1], pair[2]
      local rnd = T.rng(SEED + 3251 + p * 173)
      local a = ffi.new(ffi.typeof("$[?]", src.ct), 32)
      for i = 0, 31 do a[i] = T.rand(src, rnd) end
      diff(src.name .. " to " .. dst.name .. " ymm random", function(n)
	local acc = dst.ct(0)
	for i = 0, n-1 do
	  acc = simd.bxor(acc, simd.convert(dst.ct, a[i % 32]))
	end
	return acc
      end, 256)
    end
  end)

  test("all AVX2 cross-width conversions", function()
    local groups = {
      {{T.T.i8x16, T.T.u8x16},
       {T.W.i16x16, T.W.u16x16}},
      {{T.T.i16x8, T.T.u16x8},
       {T.W.i32x8, T.W.u32x8, T.W.float8}},
      {{T.T.i32x4, T.T.u32x4, T.T.float4},
       {T.W.i64x4, T.W.u64x4, T.W.double4}},
    }
    local p = 0
    for _, group in ipairs(groups) do
      for _, narrow in ipairs(group[1]) do
	for _, wide in ipairs(group[2]) do
	  for _, pair in ipairs({{narrow, wide}, {wide, narrow}}) do
	    p = p + 1
	    local src, dst = pair[1], pair[2]
	    local rnd = T.rng(SEED + 4001 + p * 193)
	    local a = ffi.new(ffi.typeof("$[?]", src.ct), 48)
	    for i = 0, 47 do a[i] = T.rand(src, rnd) end
	    diff(src.name .. " to " .. dst.name .. " cross-width", function(n)
	      local acc = dst.ct(0)
	      for i = 0, n-1 do
		acc = simd.bxor(acc, simd.convert(dst.ct, a[i % 48]))
	      end
	      return acc
	    end, 384)
	  end
	end
      end
    end

    -- These values are one unit beyond qword-to-float rounding midpoints.
    -- Going through double first chooses the wrong neighbouring float.
    local f4 = T.T.float4.ct
    diff("cross-width i64/u64 to float rounds once", function(n)
      local si = T.W.i64x4.ct(0x4000004000000001LL,
			      -0x4000004000000001LL, 0, 1)
      local ui = T.W.u64x4.ct(0x8000008000000001ULL,
			      0x4000004000000001ULL, 0, 1)
      local acc = f4(0)
      for _ = 1, n do
	acc = simd.bxor(acc, simd.convert(f4, si))
	acc = simd.bxor(acc, simd.convert(f4, ui))
	si = si + T.W.i64x4.ct(17)
	ui = ui + T.W.u64x4.ct(17)
      end
      return acc
    end, 160)

    diff("cross-width float to i16 bounds", function(n)
      local v = T.W.float8.ct(-32768, -32768.5, 32767, 32767.5,
			      0/0, 1/0, -1/0, -1.9)
      local acc = T.T.i16x8.ct(0)
      for _ = 1, n do
	acc = simd.bxor(acc, simd.convert(T.T.i16x8.ct, v))
	v = v + T.W.float8.ct(0, 0, 0, 0, 0, 0, 0, 0.01)
      end
      return acc
    end, 160)
  end)

  test("256-bit constants and logical operations", function()
    local ct = T.W.u32x8.ct
    local a = ct(0xaaaaaaaa, 1, 2, 3, 0x55555555, 5, 6, 7)
    local b = ct(0x55555555, 8, 9, 10, 0xaaaaaaaa, 12, 13, 14)
    diff("u32x8 ymm logic", function(n)
      local acc = ct(0)
      for _ = 1, n do acc = simd.bxor(simd.bor(acc, a), b) end
      return acc
    end, 301)
  end)

  test("256-bit reductions cross the 128-bit boundary", function()
    for _, ti in ipairs(T.W) do
      local lanes = {}
      for i = 1, ti.lanes do
	if ti.fp or ti.signed then
	  lanes[i] = i == 1 and 90 or i == ti.lanes and -100 or i * 3 - 7
	else
	  lanes[i] = i == 1 and 1 or i == ti.lanes and 1000 or i * 3 + 7
	end
      end
      local ct, a = ti.ct, ti.ct(unpack(lanes, 1, ti.lanes))
      diff(ti.name .. " ymm reductions", function(n)
	local sum, lo, hi
	for i = 1, n do
	  local v = a + ct(i % 5)
	  sum, lo, hi = simd.hsum(v), simd.hmin(v), simd.hmax(v)
	end
	return sum, lo, hi
      end, 120)
      if ti.bits == 8 then
	local av, bv = {}, {}
	for i = 1, ti.lanes do
	  if ti.signed then
	    av[i] = (i % 4 == 0 and -128 or
		     i % 4 == 1 and 127 or i * 9 - 120)
	    bv[i] = (i % 4 == 0 and 127 or
		     i % 4 == 1 and -128 or 119 - i * 7)
	  else
	    av[i] = (i % 3 == 0 and 255 or (i * 37) % 256)
	    bv[i] = (i % 4 == 0 and 254 or (255 - i * 29) % 256)
	  end
	end
	local x = ct(unpack(av, 1, ti.lanes))
	local y = ct(unpack(bv, 1, ti.lanes))
	diff(ti.name .. " ymm hsum product", function(n)
	  local product, last
	  for i = 1, n do
	    product = (x + ct(i % 7)) * y
	    last = simd.hsum(product)
	  end
	  return product, last
	end, 120)
      elseif ti.bits == 16 then
	local av, bv = {}, {}
	for i = 1, ti.lanes do
	  if ti.signed then
	    av[i] = i % 3 == 0 and -32768 or i % 2 == 0 and 32767 or -30000+i
	    bv[i] = i % 4 == 0 and -32768 or i % 2 == 0 and -23456 or 30000-i
	  else
	    av[i] = i % 3 == 0 and 65535 or i % 2 == 0 and 32768 or 60000-i
	    bv[i] = i % 4 == 0 and 65535 or i % 2 == 0 and 32769 or 50000+i
	  end
	end
	local x = ct(unpack(av, 1, ti.lanes))
	local y = ct(unpack(bv, 1, ti.lanes))
	diff(ti.name .. " ymm hsum product", function(n)
	  local product, last
	  for i = 1, n do
	    product = (x + ct(i % 7)) * y
	    last = simd.hsum(product)
	  end
	  return product, last
	end, 120)
      end
    end
  end)

  test("256-bit constant and runtime shuffles cross halves", function()
    for _, ti in ipairs(T.W) do
      local lanes, identity, localrev, halfswap, halfcat, halfcatrev, blend,
	    unpklo, unpkhi, unpkswap, localalign, fullalign,
	    fullalignrev, fullhigh, rev, mix, raw =
	{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}
      local half = ti.lanes / 2
      local nph = 128 / ti.bits
      for i = 0, ti.lanes-1 do
	local h, out = (i-i%nph)/nph, i%nph
	local lolane = h*nph + (out-out%2)/2
	local hilane = lolane + nph/2
	local blendunit = ti.bits == 8 and (i-i%2)/2 or i
	lanes[i+1] = i + 1
	identity[i+1] = i
	localrev[i+1] = i-i%half + half-1-i%half
	halfswap[i+1] = (i+half) % ti.lanes
	halfcat[i+1] = i < half and i+half or ti.lanes+i-half
	halfcatrev[i+1] = i < half and ti.lanes+half+i or i-half
	blend[i+1] = blendunit%2 == 1 and ti.lanes+i or i
	unpklo[i+1] = lolane + (out%2 == 1 and ti.lanes or 0)
	unpkhi[i+1] = hilane + (out%2 == 1 and ti.lanes or 0)
	unpkswap[i+1] = lolane + (out%2 == 0 and ti.lanes or 0)
	localalign[i+1] = out+1 < nph and h*nph+out+1
					     or ti.lanes+h*nph
	fullalign[i+1] = i+1 < ti.lanes and i+1 or ti.lanes
	fullalignrev[i+1] = i+1 < ti.lanes and ti.lanes+i+1 or 0
	fullhigh[i+1] = i+nph+1
	rev[i+1] = ti.lanes - 1 - i
	mix[i+1] = i % 2 == 0 and ti.lanes - 1 - i
				      or ti.lanes + (i + half) % ti.lanes
	local src = (i + half) % ti.lanes
	raw[i+1] = i % 2 == 0 and src or src - ti.lanes
      end
      local ct = ti.ct
      local a = ct(unpack(lanes, 1, ti.lanes))
      local b = ct(101)
      local it = T.masktype(ti)
      local ix = it.ct(unpack(raw, 1, ti.lanes))
      diff(ti.name .. " ymm identity shuffle", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle(acc + a, unpack(identity, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm lane-local reverse", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle(acc + a, unpack(localrev, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm half swap", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle(acc + a, unpack(halfswap, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm half concatenate", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc + a, b, unpack(halfcat, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm half concatenate reverse", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc + a, b, unpack(halfcatrev, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm word blend", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc + a, b, unpack(blend, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm low unpack", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc + a, b, unpack(unpklo, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm high unpack", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc + a, b, unpack(unpkhi, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm swapped unpack", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc + a, b, unpack(unpkswap, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm lane-local aligned window", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc+a, b,
			     unpack(localalign, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm full aligned window", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc+a, b,
			     unpack(fullalign, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm reversed full aligned window", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc+a, b,
			     unpack(fullalignrev, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm high full aligned window", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc+a, b,
			     unpack(fullhigh, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm shuffle2 one source", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc + a, b, unpack(rev, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm shuffle2 equal sources", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  local x = acc + a
	  acc = simd.shuffle2(x, x, unpack(mix, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm constant reverse", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle(acc + a, unpack(rev, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm shuffle2", function(n)
	local acc = ct(0)
	for _ = 1, n do
	  acc = simd.shuffle2(acc + a, b, unpack(mix, 1, ti.lanes))
	end
	return acc
      end, 80)
      diff(ti.name .. " ymm runtime permute", function(n)
	local acc = ct(0)
	for _ = 1, n do acc = simd.shuffle(acc + a, ix) end
	return acc
      end, 80)
    end
    local ct = T.W.i32x8.ct
    diff("i32x8 ymm data is also its index", function(n)
      local acc = ct(7, 0, 6, 1, 5, 2, 4, 3)
      for _ = 1, n do acc = simd.shuffle(acc, acc) end
      return acc
    end, 80)
    local d8, q4 = T.W.i32x8.ct, T.W.i64x4.ct
    diff("i32x8 ymm immediate shuffle2", function(n)
      local a, b = d8(1), d8(11)
      local x, y = d8(0), d8(0)
      for _ = 1, n do
	x = simd.shuffle2(x+a, b, 3,0,10,9,7,4,14,13)
	y = simd.shuffle2(y+a, b, 11,8,2,1,15,12,6,5)
      end
      return x, y
    end, 80)
    diff("i64x4 ymm immediate shuffle2", function(n)
      local a, b = q4(1), q4(11)
      local x, y = q4(0), q4(0)
      for _ = 1, n do
	x = simd.shuffle2(x+a, b, 1,4,2,7)
	y = simd.shuffle2(y+a, b, 5,0,6,3)
      end
      return x, y
    end, 80)
  end)

  test("256-bit insert addresses both halves", function()
    for _, ti in ipairs(T.W) do
      local lanes = {}
      for i = 1, ti.lanes do lanes[i] = i end
      local ct = ti.ct
      local a = ct(unpack(lanes, 1, ti.lanes))
      local value = ti.fp and 1.5 or 7
      diff(ti.name .. " ymm insert", function(n)
	local dyn, high = ct(0), ct(0)
	for i = 1, n do
	  dyn = simd.insert(dyn + a, i % ti.lanes, value)
	  high = simd.insert(high + a, ti.lanes-1, value)
	end
	return dyn, high
      end, 120)
    end
  end)

  test("256-bit memory, guards, and sunk boxes", function()
    local ti, N = T.W.i32x8, 48
    local ct = ti.ct
    local src = ffi.new(ffi.typeof("$[?]", ct), N)
    local dst = ffi.new(ffi.typeof("$[?]", ct), N)
    for i = 0, N-1 do src[i] = ct(i, i+1, i+2, i+3, i+4, i+5, i+6, i+7) end
    diff("i32x8 ymm array", function(n)
      for _ = 1, n do
	for i = 0, N-1 do dst[i] = src[i] + ct(7) end
      end
      return dst[0], dst[N-1]
    end, 20)
    local a = ct(1, 2, 3, 4, 5, 6, 7, 8)
    diff("i32x8 ymm side exit", function(n)
      local acc, hits = ct(0), 0
      for i = 1, n do
	local v = acc + a
	if i % 37 == 0 then hits = hits + v[7] end
	acc = v - a + a
      end
      return acc, hits, acc[0], acc[7]
    end, 500)
    diff("i32x8 ymm sunk box", function(n)
      local acc = ct(0)
      for i = 1, n do
	acc = acc + a
	if i == n then return acc, acc[0], acc[7] end
      end
    end, 300)
  end)

  test("256-bit register pressure spills full YMM values", function()
    local ct = T.W.float8.ct
    local v = {}
    for i = 1, 20 do v[i] = ct(i, i+1, i+2, i+3, i+4, i+5, i+6, i+7) end
    diff("float8 ymm pressure", function(n)
      local a1,a2,a3,a4,a5 = ct(0),ct(0),ct(0),ct(0),ct(0)
      local a6,a7,a8,a9,a10 = ct(0),ct(0),ct(0),ct(0),ct(0)
      local a11,a12,a13,a14,a15 = ct(0),ct(0),ct(0),ct(0),ct(0)
      local a16,a17,a18,a19,a20 = ct(0),ct(0),ct(0),ct(0),ct(0)
      for _ = 1, n do
	a1=a1+v[1]; a2=a2+v[2]; a3=a3+v[3]; a4=a4+v[4]
	a5=a5+v[5]; a6=a6+v[6]; a7=a7+v[7]; a8=a8+v[8]
	a9=a9+v[9]; a10=a10+v[10]; a11=a11+v[11]; a12=a12+v[12]
	a13=a13+v[13]; a14=a14+v[14]; a15=a15+v[15]; a16=a16+v[16]
	a17=a17+v[17]; a18=a18+v[18]; a19=a19+v[19]; a20=a20+v[20]
      end
      return a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+
	     a11+a12+a13+a14+a15+a16+a17+a18+a19+a20
    end, 300)
  end)
end

return T
