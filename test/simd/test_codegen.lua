-- Machine code inspection.
--
-- Compiles representative vector loops and checks the generated x86-64 code:
-- the packed instruction must be there, and there must be no per-lane
-- scalarisation and no helper call inside the loop.
local T = require("simdtest")
local ffi, simd, test, check = T.ffi, T.simd, T.test, T.check
local jit_ = require("jit")

-- jit.dump lives next to the sources when running from the repository.
package.path = "src/?.lua;./src/?.lua;" .. package.path
local ok_dump, dump = pcall(require, "jit.dump")

local tmp = os.getenv("TMPDIR") or "/tmp"
local dumpfile = tmp .. "/luajit_simd_codegen_" .. tostring(os.time()) .. ".txt"

-- Compile f in a fresh trace and return the machine code of its loop body.
local function loopcode(f, ...)
  jit_.off()
  jit_.flush()
  collectgarbage()
  dump.on("m", dumpfile)
  jit_.on()
  f(...)
  jit_.off()
  dump.off()
  local fh = assert(io.open(dumpfile))
  local text = fh:read("*a")
  fh:close()
  os.remove(dumpfile)
  -- Everything from the last loop marker to the end of that trace. Not every
  -- trace becomes a loop trace, so fall back to the whole machine code.
  local body = text:match("%-%>LOOP:\n(.-)\n%-%-%-%-") or text:match("%-%>LOOP:\n(.*)")
  if body then return body, true end
  return text, false
end

local function mnemonics(body)
  local t = {}
  for line in body:gmatch("[^\n]+") do
    local m = line:match("^%x+%s+(%a[%w]*)")
    if m then t[m] = (t[m] or 0) + 1 end
  end
  return t
end

local function checkloop(name, want, unwanted, f, ...)
  local body, isloop = loopcode(f, ...)
  local m = mnemonics(body)
  if not check(body ~= "", name .. ": nothing was compiled") then return end
  for _, w in ipairs(want) do
    check(m[w] and m[w] > 0,
	  name .. ": expected '" .. w .. "' in the compiled code, got: " ..
	  body:gsub("\n", " | "))
  end
  -- The "must not appear" list only makes sense for a real loop body: a whole
  -- trace always contains the prologue, which may call the GC step.
  if isloop then
    for _, u in ipairs(unwanted or {}) do
      check(not m[u],
	    name .. ": unexpected '" .. u .. "' in the loop body, got: " ..
	    body:gsub("\n", " | "))
    end
  end
  return m, isloop
end

if not ok_dump then
  test("jit.dump unavailable", function()
    check(false, "could not load jit.dump; run from the repository root")
  end)
  return T
end

local NOCALL = {"call"}

test("float arithmetic is packed", function()
  local f4 = T.T.float4.ct
  local a, b = f4(1, 2, 3, 4), f4(0.5)
  local m = checkloop("float4 add", {"addps"}, {"call", "addss"}, function()
    local acc = f4(0)
    for _ = 1, 400 do acc = acc + a end
    return acc
  end)
  if m then check(m.addps == 1, "float4 add: exactly one ADDPS per iteration") end
  checkloop("float4 mul/div", {"mulps", "divps"}, {"call", "mulss", "divss"},
    function()
      local acc = f4(1)
      for _ = 1, 400 do acc = (acc * a) / b end
      return acc
    end)
  local d2 = T.T.double2.ct
  local c = d2(1.5, 2.5)
  checkloop("double2 add", {"addpd"}, {"call", "addsd"}, function()
    local acc = d2(0)
    for _ = 1, 400 do acc = acc + c end
    return acc
  end)
end)

test("integer arithmetic is packed", function()
  local cases = {
    {"i8x16", "paddb", "psubb"},
    {"i16x8", "paddw", "psubw"},
    {"i32x4", "paddd", "psubd"},
    {"i64x2", "paddq", "psubq"},
  }
  for _, c in ipairs(cases) do
    local ct = T.T[c[1]].ct
    local a = ct(3)
    checkloop(c[1] .. " add/sub", {c[2], c[3]}, NOCALL, function()
      local acc = ct(0)
      for _ = 1, 400 do acc = acc + a - ct(1) end
      return acc
    end)
  end
  -- 16 bit multiply has a direct instruction.
  local i16 = T.T.i16x8.ct
  checkloop("i16x8 mul", {"pmullw"}, NOCALL, function()
    local acc = i16(1)
    for _ = 1, 400 do acc = acc * i16(3) end
    return acc
  end)
  -- 8 bit multiply has none, but must still be packed, not scalarised.
  local i8 = T.T.i8x16.ct
  local m = checkloop("i8x16 mul", {"pmullw", "psrlw", "psllw", "por"}, NOCALL,
    function()
      local acc = i8(1)
      local k = i8(3)
      for i = 1, 400 do acc = (acc + i8(1)) * k end
      return acc
    end)
  if m then check(not m.imul, "i8x16 mul: no scalar IMUL") end
end)

test("ffi.simd operations are packed", function()
  local f4 = T.T.float4.ct
  local a, b = f4(1, 2, 3, 4), f4(2.5)
  checkloop("min/max", {"minps", "maxps"}, {"call", "minss"}, function()
    local acc = f4(0)
    for _ = 1, 400 do acc = simd.max(simd.min(acc + a, b), f4(-1)) end
    return acc
  end)
  checkloop("sqrt", {"sqrtps"}, {"call", "sqrtss"}, function()
    local acc = f4(1)
    for _ = 1, 400 do acc = simd.sqrt(acc + a) end
    return acc
  end)
  checkloop("compare and select", {"cmpps"}, NOCALL, function()
    local acc = f4(0)
    for _ = 1, 400 do acc = simd.select(simd.lt(acc, b), acc + a, b) end
    return acc
  end)
  local i4 = T.T.i32x4.ct
  checkloop("integer compare", {"pcmpgtd"}, NOCALL, function()
    local acc = i4(0)
    for _ = 1, 400 do acc = simd.select(simd.gt(acc, i4(100)), i4(0), acc + i4(1)) end
    return acc
  end)
  checkloop("shifts", {"pslld", "psrad"}, NOCALL, function()
    local acc = i4(1)
    for _ = 1, 400 do acc = simd.sar(simd.shl(acc + i4(1), 3), 2) end
    return acc
  end)
  checkloop("bitwise", {"pand", "por", "pxor"}, NOCALL, function()
    local acc = i4(0)
    for _ = 1, 400 do
      acc = simd.bor(simd.band(acc + i4(1), i4(0xff)), simd.bxor(acc, i4(7)))
    end
    return acc
  end)
  checkloop("movemask", {"movmskps"}, NOCALL, function()
    local acc = 0
    local v = f4(0)
    for _ = 1, 400 do
      v = v + a
      acc = acc + simd.movemask(simd.lt(v, b))
    end
    return acc
  end)
end)

test("horizontal reduction uses shuffles, not lane loads", function()
  local f4 = T.T.float4.ct
  local a = f4(1, 2, 3, 4)
  local m = checkloop("hsum", {"psrldq", "addps"}, NOCALL, function()
    local s, v = 0, f4(0)
    for _ = 1, 400 do
      v = v + a
      s = s + simd.hsum(v)
    end
    return s
  end)
  if m then
    check(m.psrldq == 2, "hsum float4: two halving steps, got " ..
	  tostring(m.psrldq))
  end
end)

test("shuffle and insert are packed", function()
  local i4 = T.T.i32x4.ct
  local a = i4(1, 2, 3, 4)
  checkloop("shuffle", {"pshufb"}, NOCALL, function()
    local acc = i4(0)
    for _ = 1, 400 do acc = simd.shuffle(acc + a, 3, 2, 1, 0) end
    return acc
  end)
  -- simd.shuffle2 currently stays interpreted, see notes/SIMD_STATUS.md.
  -- The mask AND the splatted constant fold together, so only the ANDN and
  -- the OR survive in the loop.
  checkloop("insert", {"pandn", "por"}, NOCALL, function()
    local acc = i4(0)
    for _ = 1, 400 do acc = simd.insert(acc + a, 2, 42) end
    return acc
  end)
end)

test("vector loads and stores use MOVUPS", function()
  local ti = T.T.float4
  local N = 32
  local src = ffi.new(ffi.typeof("$[?]", ti.ct), N)
  local dst = ffi.new(ffi.typeof("$[?]", ti.ct), N)
  for i = 0, N-1 do src[i] = ti.ct(i) end
  checkloop("array map", {"movups", "mulps"}, NOCALL, function()
    local k = ti.ct(2)
    for r = 1, 40 do
      local base = 0
      for i = 0, N-1 do dst[i] = src[i] * k end
    end
  end)
end)

test("no boxing allocation remains in a hot vector loop", function()
  local f4 = T.T.float4.ct
  local a = f4(1, 2, 3, 4)
  local body = loopcode(function()
    local acc = f4(0)
    for _ = 1, 400 do acc = acc + a * f4(2) end
    return acc
  end)
  check(not body:find("call", 1, true),
	"allocation call left in the loop: " .. body:gsub("\n", " | "))
  local m = mnemonics(body)
  check(m.addps == 1 and (m.mulps or 0) <= 1 and not m.addss and not m.mulss,
	"loop body should be one packed multiply-add, got: " ..
	body:gsub("\n", " | "))
end)

return T
