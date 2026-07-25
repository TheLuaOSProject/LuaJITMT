-- Interpreter vs JIT differential tests.
--
-- Every case runs the *same* closure interpreted once and compiled many
-- times, then compares bit for bit. This is what catches recorder and
-- backend bugs that only show up after a trace has been linked.
local T = require("simdtest")
local ffi, simd, test, check, checkeq = T.ffi, T.simd, T.test, T.check, T.checkeq
local jit_ = require("jit")

local SEED = tonumber(os.getenv("SIMD_SEED") or "20260725")

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
    diff(ti.name .. " lanes", function(n)
      local s = 0
      local acc = ct(0)
      for _ = 1, n do
	acc = acc + a
	s = s + tonumber(acc[0]) + tonumber(acc[ti.lanes-1])
      end
      return s, acc
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

return T
