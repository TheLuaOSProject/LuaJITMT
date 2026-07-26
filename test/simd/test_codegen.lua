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

-- This file needs a working JIT compiler.
if not pcall(jit_.on) then
  test("JIT compiler", function()
    check(true, "JIT permanently disabled by a build option, code generation tests skipped")
  end)
  return T
end

-- A fixed or second-resolution name collides when configurations run in
-- parallel. os.tmpname() reserves a process-unique path; dump.on() can
-- truncate and reuse it for each isolated trace below.
local dumpfile = os.tmpname()
if package.config:sub(1, 1) == "\\" then
  -- The Windows CRT returns a root-relative name (e.g. "\\s123."), which
  -- Wine and restricted Windows environments may not permit creating.
  dumpfile = dumpfile:gsub("^[\\/]+", "")
end

-- Compile f in a fresh trace and return the requested dump ("m" = machine
-- code, "i" = IR, plus a colour mode letter).
local function rawdump(mode, f, ...)
  jit_.off()
  jit_.flush()
  collectgarbage()
  dump.on(mode, dumpfile)
  jit_.on()
  f(...)
  jit_.off()
  dump.off()
  local fh = assert(io.open(dumpfile))
  local text = fh:read("*a")
  fh:close()
  os.remove(dumpfile)
  return text
end

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

-- The backend emits the VEX form of every packed instruction when the CPU has
-- AVX, so "addps" and "vaddps" are the same instruction as far as these tests
-- are concerned.
local function count(m, w)
  return (m[w] or 0) + (m["v" .. w] or 0)
end

local function checkloop(name, want, unwanted, f, ...)
  local body, isloop = loopcode(f, ...)
  local m = mnemonics(body)
  if not check(body ~= "", name .. ": nothing was compiled") then return end
  for _, w in ipairs(want) do
    check(count(m, w) > 0,
	  name .. ": expected '" .. w .. "' in the compiled code, got: " ..
	  body:gsub("\n", " | "))
  end
  -- The "must not appear" list only makes sense for a real loop body: a whole
  -- trace always contains the prologue, which may call the GC step.
  if isloop then
    for _, u in ipairs(unwanted or {}) do
      check(count(m, u) == 0,
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
  if m then check(count(m, "addps") == 1,
		  "float4 add: exactly one packed add per iteration") end
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

test("vector loads fuse only into alignment-safe AVX arithmetic", function()
  local i4 = T.T.i32x4.ct
  local c = ffi.new("i32x4[1]")
  c[0] = i4(1, 2, 3, 4)
  local body = rawdump("m", function()
    local acc = i4(0)
    for _ = 1, 400 do acc = acc + c[0] end
    return acc
  end)
  local memadd = false
  for line in body:gmatch("[^\n]+") do
    if line:find("paddd", 1, true) and line:find("[", 1, true) then
      memadd = true
      break
    end
  end
  if simd.features().avx then
    check(memadd, "AVX vector load did not fuse into arithmetic: " ..
	  body:gsub("\n", " | "))
  else
    check(not memadd and body:find("movups", 1, true),
	  "legacy SSE must load an unaligned vector separately: " ..
	  body:gsub("\n", " | "))
  end
end)

test("vector loads fuse into AVX unary operations", function()
  local cases = {
    {"float4 sqrt", T.T.float4.ct, T.T.float4.ct(1, 4, 9, 16),
      "sqrtps", simd.sqrt},
    {"i32x4 abs", T.T.i32x4.ct, T.T.i32x4.ct(-1, 2, -3, 4),
      "pabsd", simd.abs},
    {"float4 round", T.T.float4.ct, T.T.float4.ct(1.25, 2.5, -3.75, 4),
      "roundps", simd.round},
  }
  if simd.features().avx2 then
    cases[#cases+1] = {
      "float8 sqrt", T.W.float8.ct, T.W.float8.ct(1, 4, 9, 16, 25, 36, 49, 64),
      "vsqrtps", simd.sqrt,
    }
    cases[#cases+1] = {
      "i32x8 abs", T.W.i32x8.ct, T.W.i32x8.ct(-1, 2, -3, 4, -5, 6, -7, 8),
      "vpabsd", simd.abs,
    }
    cases[#cases+1] = {
      "float8 round", T.W.float8.ct,
      T.W.float8.ct(1.25, 2.5, -3.75, 4, 5.5, -6.25, 7.75, 8),
      "vroundps", simd.round,
    }
  end
  for _, c in ipairs(cases) do
    local values = ffi.new(ffi.typeof("$[1]", c[2]))
    values[0] = c[3]
    local body = rawdump("m", function()
      local acc = c[2](0)
      for _ = 1, 400 do acc = acc + c[5](values[0]) end
      return acc
    end)
    local memop = false
    for line in body:gmatch("[^\n]+") do
      if line:find(c[4], 1, true) and line:find("[", 1, true) then
	memop = true
	break
      end
    end
    if simd.features().avx then
      check(memop, c[1] .. " load did not fuse into the unary operation: " ..
	    body:gsub("\n", " | "))
    else
      check(not memop and body:find("movups", 1, true),
	    c[1] .. " must load an unaligned vector separately without AVX: " ..
	    body:gsub("\n", " | "))
    end
  end
end)

test("native two-source shuffles consume their final array load", function()
  local cases = {
    {
      "i32x4 SHUFPS", T.T.i32x4.ct, "shufps",
      function(a, b) return simd.shuffle2(a, b, 3, 0, 6, 5) end,
    },
    {
      "i64x2 SHUFPD", T.T.i64x2.ct, "shufpd",
      function(a, b) return simd.shuffle2(a, b, 1, 2) end,
    },
    {
      "i32x4 PALIGNR", T.T.i32x4.ct, "palignr",
      function(a, b) return simd.shuffle2(a, b, 5, 6, 7, 0) end,
    },
  }
  if simd.features().sse4_1 then
    cases[#cases+1] = {
      "i32x4 PBLENDW", T.T.i32x4.ct, "pblendw",
      function(a, b) return simd.shuffle2(a, b, 0, 5, 2, 7) end,
    }
  end
  if simd.features().avx2 then
    cases[#cases+1] = {
      "i32x8 VSHUFPS", T.W.i32x8.ct, "vshufps",
      function(a, b)
	return simd.shuffle2(a, b, 3, 0, 10, 9, 7, 4, 14, 13)
      end,
    }
    cases[#cases+1] = {
      "i64x4 VSHUFPD", T.W.i64x4.ct, "vshufpd",
      function(a, b) return simd.shuffle2(a, b, 1, 4, 2, 7) end,
    }
    cases[#cases+1] = {
      "i32x8 VPERM2I128", T.W.i32x8.ct, "vperm2i128",
      function(a, b)
	return simd.shuffle2(a, b, 4, 5, 6, 7, 8, 9, 10, 11)
      end,
    }
    cases[#cases+1] = {
      "i32x8 VPBLENDW", T.W.i32x8.ct, "vpblendw",
      function(a, b)
	return simd.shuffle2(a, b, 0, 9, 2, 11, 4, 13, 6, 15)
      end,
    }
    cases[#cases+1] = {
      "i32x8 VPBLENDD", T.W.i32x8.ct, "vpblendd",
      function(a, b)
	return simd.shuffle2(a, b, 0, 9, 2, 3, 12, 5, 14, 7)
      end,
    }
    cases[#cases+1] = {
      "i32x8 VPALIGNR", T.W.i32x8.ct, "vpalignr",
      function(a, b)
	return simd.shuffle2(a, b, 9, 10, 11, 0, 13, 14, 15, 4)
      end,
    }
    cases[#cases+1] = {
      "i32x8 full aligned window", T.W.i32x8.ct, "vperm2i128",
      function(a, b)
	return simd.shuffle2(a, b, 1, 2, 3, 4, 5, 6, 7, 8)
      end,
    }
  end
  for _, c in ipairs(cases) do
    local lhs = ffi.new(ffi.typeof("$[400]", c[2]))
    local rhs = ffi.new(ffi.typeof("$[400]", c[2]))
    for i = 0, 399 do lhs[i], rhs[i] = c[2](i+1), c[2](i+9) end
    local body, isloop = loopcode(function()
      local acc = c[2](0)
      for i = 0, 399 do acc = c[4](acc + lhs[i], rhs[i]) end
      return acc
    end)
    local memop = false
    for line in body:gmatch("[^\n]+") do
      if line:find(c[3], 1, true) and line:find("[", 1, true) then
	memop = true
	break
      end
    end
    check(isloop, c[1] .. " memory-source test must compile as a loop")
    if simd.features().avx then
      check(memop, c[1] .. " did not consume its final load: " ..
	    body:gsub("\n", " | "))
    else
      check(not memop, c[1] ..
	    " legacy SSE must keep the unaligned load separate: " ..
	    body:gsub("\n", " | "))
    end
  end
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
  local m = checkloop("i8x16 mul",
    {"pmullw", "psrlw", "psllw", "pand", "por"}, NOCALL,
    function()
      local acc = i8(1)
      local k = i8(3)
      for i = 1, 400 do acc = (acc + i8(1)) * k end
      return acc
    end)
  if m then
    check(count(m, "imul") == 0, "i8x16 mul: no scalar IMUL")
    check(count(m, "pmullw") == 2 and count(m, "psrlw") == 1 and
	  count(m, "psllw") == 1 and count(m, "pand") == 1 and
	  count(m, "por") == 1,
	  "i8x16 mul: expected the masked-word lowering, got: " ..
	  tostring(count(m, "pmullw")) .. "/" ..
	  tostring(count(m, "psrlw")) .. "/" ..
	  tostring(count(m, "psllw")) .. "/" ..
	  tostring(count(m, "pand")) .. "/" .. tostring(count(m, "por")))
  end
end)

test("byte multiply constants use memory only under register pressure", function()
  if not simd.features().avx then
    check(true, "no AVX on this CPU, pressure-sensitive VEX constant skipped")
    return
  end
  local ct = T.T.i8x16.ct
  local body = loopcode(function()
    local a1,a2,a3,a4,a5,a6,a7 = ct(1),ct(2),ct(3),ct(4),ct(5),ct(6),ct(7)
    local a8,a9,a10,a11,a12,a13,a14 =
      ct(8),ct(9),ct(10),ct(11),ct(12),ct(13),ct(14)
    local one, k = ct(1), ct(3)
    for _ = 1, 400 do
      a1=(a1+one)*k; a2=(a2+one)*k; a3=(a3+one)*k
      a4=(a4+one)*k; a5=(a5+one)*k; a6=(a6+one)*k
      a7=(a7+one)*k; a8=(a8+one)*k; a9=(a9+one)*k
      a10=(a10+one)*k; a11=(a11+one)*k; a12=(a12+one)*k
      a13=(a13+one)*k; a14=(a14+one)*k
    end
    return a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14
  end)
  local memmask = false
  for line in body:gmatch("[^\n]+") do
    if line:find("pand", 1, true) and line:find("[rip", 1, true) then
      memmask = true
      break
    end
  end
  check(memmask, "a pressured byte-multiply mask must use VPAND memory: " ..
	body:gsub("\n", " | "))
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

test("byte-aligned rotate idioms become one packed shuffle", function()
  if not simd.features().ssse3 then
    check(true, "no SSSE3 on this CPU, rotate folding skipped")
    return
  end
  local cases = {
    {"i16x8", function(x)
       return simd.bor(simd.shl(x, 8), simd.shr(x, 8))
     end, "psllw", "psrlw"},
    {"i32x4", function(x)
       return simd.bor(simd.shl(x, 8), simd.shr(x, 24))
     end, "pslld", "psrld"},
    {"i64x2", function(x)
       return simd.bor(simd.shl(x, 32), simd.shr(x, 32))
     end, "psllq", "psrlq"},
  }
  for _, c in ipairs(cases) do
    local ct, rot = T.T[c[1]].ct, c[2]
    local m = checkloop(c[1] .. " byte rotate", {"pshufb"},
      {"call", c[3], c[4], "por"}, function()
	local acc, add = ct(0x01020304), ct(0x10101)
	for _ = 1, 400 do
	  local x = acc + add
	  acc = rot(x)
	end
	return acc
      end)
    check(count(m, "pshufb") == 1,
	  c[1] .. " byte rotate must be exactly one packed shuffle")
  end
  local ct = T.T.i32x4.ct
  local m = checkloop("i32x4 xor byte rotate", {"pshufb"},
    {"call", "pslld", "psrld", "pxor"}, function()
      local acc, add = ct(0x01020304), ct(0x10101)
      for _ = 1, 400 do
	local x = acc + add
	acc = simd.bxor(simd.shr(x, 24), simd.shl(x, 8))
      end
      return acc
    end)
  check(count(m, "pshufb") == 1,
	"i32x4 xor byte rotate must be exactly one packed shuffle")
end)

test("constant rotate masks fuse into memory operands under pressure", function()
  if not simd.features().ssse3 then
    check(true, "no SSSE3 on this CPU, memory-mask lowering skipped")
    return
  end
  local ct = T.T.u32x4.ct
  local function rot(x)
    return simd.bor(simd.shl(x, 8), simd.shr(x, 24))
  end
  local body = loopcode(function()
    local a1,a2,a3,a4,a5,a6,a7 = ct(1),ct(2),ct(3),ct(4),ct(5),ct(6),ct(7)
    local a8,a9,a10,a11,a12,a13,a14 =
      ct(8),ct(9),ct(10),ct(11),ct(12),ct(13),ct(14)
    for _ = 1, 400 do
      a1=rot(a1+ct(1)); a2=rot(a2+ct(2)); a3=rot(a3+ct(3))
      a4=rot(a4+ct(4)); a5=rot(a5+ct(5)); a6=rot(a6+ct(6))
      a7=rot(a7+ct(7)); a8=rot(a8+ct(8)); a9=rot(a9+ct(9))
      a10=rot(a10+ct(10)); a11=rot(a11+ct(11)); a12=rot(a12+ct(12))
      a13=rot(a13+ct(13)); a14=rot(a14+ct(14))
    end
    return a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14
  end)
  local fused = false
  for line in body:gmatch("[^\n]+") do
    if line:find("pshufb", 1, true) and line:find("[rip", 1, true) then
      fused = true
      break
    end
  end
  check(fused, "register-pressure shuffle mask was repeatedly loaded: " ..
	body:gsub("\n", " | "))
end)

test("64 bit lane min/max is packed", function()
  -- There is no PMINSQ before AVX-512, so this must lower to a compare and a
  -- blend rather than falling back to the interpreter or to scalar code.
  for _, name in ipairs({"i64x2", "u64x2"}) do
    local ct = T.T[name].ct
    local a = ct(3, 5)
    checkloop(name .. " min/max", {"pcmpgtq", "pand", "pandn", "por"},
      NOCALL, function()
	local acc = ct(1)
	for _ = 1, 400 do acc = simd.max(simd.min(acc + a, ct(100)), ct(-100)) end
	return acc
      end)
    checkloop(name .. " hmin", {"pcmpgtq"}, NOCALL, function()
      local s, v = 0, ct(0)
      for _ = 1, 400 do v = v + a; s = s + tonumber(simd.hmin(v)) end
      return s
    end)
  end
end)

test("the IR dump renders vector types and constants", function()
  -- jit.dump must survive a trace containing vector IR in every colour mode,
  -- and a 128 bit constant has to be printed in full. Both used to break: the
  -- ANSI colour table had no entry for the vector types, so the dump stopped
  -- at the first vector instruction, and the generic string formatting cut
  -- the hex constant down to 20 characters.
  local bit_ = require("bit")
  local i8 = T.T.i8x16.ct
  local a = i8(1, -2, 3, -4, 127, -128, 0, -1, 55, -99, 17, -17, 64, -64, 7, -7)
  local f4 = T.T.float4.ct
  local b = f4(1.5, 2.5, 3.5, 4.5)
  local function work()
    local acc, facc = i8(0), f4(0)
    for i = 1, 400 do
      acc = acc + simd.sar(a, bit_.band(i, 7))
      facc = facc + b * 2
    end
    return acc, facc
  end
  for _, mode in ipairs({"i", "iT", "iH"}) do
    local ok, text = pcall(rawdump, mode, work)
    if check(ok, "jit.dump mode " .. mode .. " failed: " .. tostring(text)) then
      check(text:find("vi1", 1, true) ~= nil,
	    mode .. ": no 8 bit vector type in the IR dump")
      check(text:find("vf4", 1, true) ~= nil,
	    mode .. ": no float vector type in the IR dump")
      -- A full 128 bit constant is "0x" plus exactly 32 hex digits, and must
      -- not be followed by the truncation marker.
      local k = text:match("0x(%x%x%x%x%x%x%x%x%x+)")
      check(k ~= nil and #k == 32,
	    mode .. ": vector constant is not 32 hex digits, got " ..
	    tostring(k and #k))
    end
  end
end)

test("mulhi uses packed multiplication", function()
  -- 16 bit lanes have PMULHW/PMULHUW. Signedness picks the instruction, so
  -- both have to be checked: using the wrong one is a wrong answer, not a
  -- slow one, and the differential test alone would not say which was used.
  -- One operand has to be loop-carried, or the whole multiply is hoisted out
  -- of the loop as invariant and the body proves nothing.
  local i16 = T.T.i16x8.ct
  checkloop("i16x8 mulhi", {"pmulhw"}, {"call", "pmulhuw"}, function()
    local acc = i16(1)
    local b = i16(400)
    for _ = 1, 400 do acc = simd.mulhi(acc, b) + i16(7) end
    return acc
  end)
  local u16 = T.T.u16x8.ct
  checkloop("u16x8 mulhi", {"pmulhuw"}, {"call", "pmulhw"}, function()
    local acc = u16(1)
    local b = u16(50000)
    for _ = 1, 400 do acc = simd.mulhi(acc, b) + u16(7) end
    return acc
  end)

  local i8 = T.T.i8x16.ct
  checkloop("i8x16 mulhi", {"pmulhw", "psraw", "psllw"},
    {"call", "pmulhuw"}, function()
      local acc = i8(-128, 127, -119, 113, -107, 101, -97, 89,
		     -83, 79, -73, 67, -61, 59, -53, 47)
      local b = i8(-117)
      for _ = 1, 400 do acc = simd.mulhi(acc + i8(3), b) end
      return acc
    end)
  local mi8sq = checkloop("i8x16 mulhi square",
    {"pabsb", "pmullw", "psrlw", "pand", "por"},
    {"call", "pmulhw", "pmulhuw"}, function()
      local acc = i8(-128, 127, -119, 113, -107, 101, -97, 89,
		     -83, 79, -73, 67, -61, 59, -53, 47)
      for _ = 1, 400 do
	local x = acc + i8(3)
	acc = simd.mulhi(x, x) + i8(1)
      end
      return acc
    end)
  check(count(mi8sq, "pmullw") == 2 and count(mi8sq, "pabsb") == 1,
	"i8x16 mulhi square must use absolute bytes and two low products")

  local u8 = T.T.u8x16.ct
  checkloop("u8x16 mulhi", {"pmulhuw", "psrlw", "psllw"},
    {"call", "pmulhw"}, function()
      local acc = u8(255, 241, 233, 227, 211, 199, 193, 181,
		      173, 167, 157, 149, 139, 131, 127, 113)
      local b = u8(239)
      for _ = 1, 400 do acc = simd.mulhi(acc + u8(3), b) end
      return acc
    end)
  local mu8sq = checkloop("u8x16 mulhi square",
    {"pmullw", "psrlw", "pand", "por"},
    {"call", "pmulhuw", "pmulhw"}, function()
      local acc = u8(255, 241, 233, 227, 211, 199, 193, 181,
		     173, 167, 157, 149, 139, 131, 127, 113)
      for _ = 1, 400 do
	local x = acc + u8(3)
	acc = simd.mulhi(x, x) + u8(1)
      end
      return acc
    end)
  check(count(mu8sq, "pmullw") == 2,
	"u8x16 mulhi square must use two low word products")

  local i32 = T.T.i32x4.ct
  checkloop("i32x4 mulhi", {"pmuldq", "pblendw", "psrlq"},
    {"call", "pmuludq"}, function()
      local acc = i32(0x40000000, -2000000000, 123456789, -987654321)
      local b = i32(-7, 13, -12345, 0x40000000)
      for _ = 1, 400 do acc = simd.mulhi(acc + i32(3), b) end
      return acc
    end)

  local u32 = T.T.u32x4.ct
  checkloop("u32x4 mulhi", {"pmuludq", "pblendw", "psrlq"},
    {"call", "pmuldq xmm"}, function()
      local acc = u32(0xffffffff, 0x80000000, 123456789, 987654321)
      local b = u32(7, 13, 12345, 0xc0000000)
      for _ = 1, 400 do acc = simd.mulhi(acc + u32(3), b) end
      return acc
    end)

  local i64 = T.T.i64x2.ct
  local mi64 = checkloop("i64x2 mulhi",
    {"pmuludq", "paddq", "psrlq", "psubq", "psrad", "pshufd"},
    {"call"}, function()
      local acc = i64(-9223372036854775807LL-1, 0x7000000000000001LL)
      local b = i64(-12345678901234567LL, 0x4000000000000003LL)
      for _ = 1, 400 do acc = simd.mulhi(acc + i64(3), b) end
      return acc
    end)
  check(count(mi64, "pmuludq") == 4,
	"i64x2 mulhi must use four unsigned dword products")
  check(count(mi64, "psubq") == 1,
	"i64x2 mulhi must combine both signed corrections before one subtract")
  local msi64 = checkloop("i64x2 mulhi square",
    {"pmuludq", "paddq", "psrlq", "psubq", "psrad", "pshufd"},
    {"call"}, function()
      local acc = i64(-9223372036854775807LL-1,
		      0x7000000000000001LL)
      for _ = 1, 400 do
	local x = acc + i64(3)
	acc = simd.mulhi(x, x) + i64(7)
      end
      return acc
    end)
  check(count(msi64, "pmuludq") == 3 and count(msi64, "psrad") == 1 and
	count(msi64, "pshufd") == 1 and count(msi64, "psubq") == 1,
	"i64x2 mulhi square must reuse its cross product and sign mask")

  local u64 = T.T.u64x2.ct
  local mu64 = checkloop("u64x2 mulhi", {"pmuludq", "paddq", "psrlq"},
    {"call", "psubq"}, function()
      local acc = u64(0xffffffffffffffffULL, 0x8000000000000001ULL)
      local b = u64(0xc000000000000001ULL, 0x7000000000000003ULL)
      for _ = 1, 400 do acc = simd.mulhi(acc + u64(3), b) end
      return acc
    end)
  check(count(mu64, "pmuludq") == 4,
	"u64x2 mulhi must use four unsigned dword products")
  local msu64 = checkloop("u64x2 mulhi square",
    {"pmuludq", "paddq", "psrlq"}, {"call", "psubq"}, function()
      local acc = u64(0xffffffffffffffffULL, 0x8000000000000001ULL)
      for _ = 1, 400 do
	local x = acc + u64(3)
	acc = simd.mulhi(x, x) + u64(7)
      end
      return acc
    end)
  check(count(msu64, "pmuludq") == 3,
	"u64x2 mulhi square must reuse its cross product")
end)

test("fma compiles to a single fused instruction", function()
  if not simd.features().fma then
    check(true, "no FMA on this CPU, codegen skipped")
    return
  end
  -- Any of VFMADD132/213/231: which one is chosen depends on where the
  -- accumulator lives, and all three are equally fused.
  local function fused(m)
    local n = 0
    for k, v in pairs(m or {}) do if k:find("fmadd", 1, true) then n = n + v end end
    return n
  end
  local f4 = T.T.float4.ct
  local a, b = f4(1.5, 2.5, 3.5, 4.5), f4(0.25)
  -- Accumulator as the addend: fma(a, b, acc). This must be one instruction
  -- with no register copy. Using a fixed form instead of picking the one
  -- whose operand is already in the destination cost two MOVAPS per
  -- iteration here and made the fused chain slower than mul+add.
  local m = checkloop("float4 fma acc", {}, {"call", "mulps", "movaps"},
    function()
      local acc = f4(0)
      for _ = 1, 400 do acc = simd.fma(a, b, acc) end
      return acc
    end)
  check(fused(m) == 1,
	"float4 fma acc: expected exactly one fused instruction, got " ..
	fused(m))
  -- Accumulator as the first multiplicand: fma(acc, b, c). Also one
  -- instruction, via a different form.
  local m2 = checkloop("float4 fma chain", {}, {"call", "mulps", "movaps"},
    function()
      local acc = f4(1)
      for _ = 1, 400 do acc = simd.fma(acc, b, a) end
      return acc
    end)
  check(fused(m2) == 1,
	"float4 fma chain: expected exactly one fused instruction, got " ..
	fused(m2))
  local d2 = T.T.double2.ct
  local a2, b2 = d2(1.5, 2.5), d2(0.25)
  local m3 = checkloop("double2 fma", {}, {"call", "mulpd"}, function()
    local acc = d2(0)
    for _ = 1, 400 do acc = simd.fma(a2, b2, acc) end
    return acc
  end)
  check(fused(m3) == 1,
	"double2 fma: expected exactly one fused instruction, got " .. fused(m3))

  local function check_memfma(name, ct)
    local values = ffi.new(ffi.typeof("$[512]", ct))
    for i = 0, 511 do values[i] = ct(i * 0.0001 + 0.25) end
    local function has_memfma(body)
      for line in body:gmatch("[^\n]+") do
	if line:find("fmadd", 1, true) and line:find("[", 1, true) then
	  return true
	end
      end
      return false
    end
    local addend = rawdump("m", function()
      local acc, k = ct(1), ct(1.00001)
      for i = 0, 399 do acc = simd.fma(acc, k, values[i]) end
      return acc
    end)
    check(has_memfma(addend),
	  name .. " FMA addend load did not fuse: " ..
	  addend:gsub("\n", " | "))
    local multiplier = rawdump("m", function()
      local acc, c = ct(1), ct(0.0001)
      for i = 0, 399 do acc = simd.fma(acc, values[i], c) end
      return acc
    end)
    check(has_memfma(multiplier),
	  name .. " FMA multiplier load did not fuse: " ..
	  multiplier:gsub("\n", " | "))
  end

  check_memfma("float4", f4)
  if simd.features().avx2 then
    check_memfma("float8", T.W.float8.ct)
  end
end)

test("per-lane shift counts use the AVX2 variable shifts", function()
  -- Without this the trace just aborts and the interpreter answers, which a
  -- differential test cannot tell apart from working compiled code.
  if not simd.features().avx2 then
    check(true, "no AVX2 on this CPU, per-lane shift codegen skipped")
    return
  end
  local i4 = T.T.i32x4.ct
  local a4, c4 = i4(1, -8, 256, -1), i4(0, 1, 2, 3)
  checkloop("i32x4 shl vec", {"psllvd"}, NOCALL, function()
    local acc = i4(0)
    for k = 1, 400 do acc = acc + simd.shl(a4, c4 + i4(k)) end
    return acc
  end)
  checkloop("i32x4 shr vec", {"psrlvd"}, NOCALL, function()
    local acc = i4(0)
    for k = 1, 400 do acc = acc + simd.shr(a4, c4 + i4(k)) end
    return acc
  end)
  checkloop("i32x4 sar vec", {"psravd"}, NOCALL, function()
    local acc = i4(0)
    for k = 1, 400 do acc = acc + simd.sar(a4, c4 + i4(k)) end
    return acc
  end)
  local i64 = T.T.i64x2.ct
  local a8, c8 = i64(-16, 8), i64(2, 3)
  checkloop("i64x2 shl vec", {"psllvq"}, NOCALL, function()
    local acc = i64(0)
    for k = 1, 400 do acc = acc + simd.shl(a8, c8 + i64(k)) end
    return acc
  end)
  -- There is no VPSRAVQ before AVX-512, so the 64 bit arithmetic shift is
  -- built from a clamped VPSRLVQ plus the sign-bias trick. Still no call.
  checkloop("i64x2 sar vec", {"psrlvq", "pcmpeqq", "pxor", "psubq"}, NOCALL,
	    function()
    local acc = i64(0)
    for k = 1, 400 do acc = acc + simd.sar(a8, c8 + i64(k)) end
    return acc
  end)

  local i16 = T.T.i16x8.ct
  local a16 = i16(-32768, -123, -1, 0, 1, 255, 12345, 32767)
  local c16 = i16(0, 1, 15, 16, 17, -1, 3, 9)
  local m16 = checkloop("i16x8 shl vec invariant", {"pmullw"}, NOCALL,
    function()
      local acc = i16(0)
      for _ = 1, 400 do acc = simd.shl(acc + a16, c16) end
      return acc
    end)
  check(count(m16, "psllvd") == 0 and count(m16, "pmullw") == 1,
	"invariant i16x8 shl must hoist the factor and use one word multiply")
  local md16 = checkloop("i16x8 shl vec dynamic",
    {"psllvd", "pmullw", "pand", "por"}, NOCALL, function()
      local acc, c = i16(0), c16
      for _ = 1, 400 do
	acc = simd.shl(acc + a16, c)
	c = c + i16(1)
      end
      return acc + c
    end)
  check(count(md16, "psllvd") == 2 and count(md16, "pmullw") == 1,
	"dynamic i16x8 shl must build two factors and use one word multiply")
  local mr16 = checkloop("i16x8 shr vec invariant",
    {"pmulhuw", "pand", "por"}, NOCALL, function()
      local acc = i16(0)
      for _ = 1, 400 do acc = simd.shr(acc + a16, c16) end
      return acc
    end)
  check(count(mr16, "psrlvd") == 0 and count(mr16, "pmulhuw") == 1,
	"invariant i16x8 shr must hoist the factor and use one high multiply")
  local ma16 = checkloop("i16x8 sar vec",
    {"psravd", "psrad", "pand"}, NOCALL, function()
      local acc = i16(0)
      for _ = 1, 400 do acc = simd.sar(acc + a16, c16) end
      return acc
    end)
  check(count(ma16, "psravd") == 2 and count(ma16, "pmulhw") == 0,
	"i16x8 sar must retain the faster two-shift XMM decomposition")
  local mrd16 = checkloop("i16x8 shr vec dynamic",
    {"psrlvd", "pmulhuw", "pcmpeqw", "pand", "por"}, NOCALL, function()
      local acc, c = i16(0), c16
      for _ = 1, 400 do
	acc = simd.shr(acc + a16, c)
	c = c + i16(1)
      end
      return acc + c
    end)
  check(count(mrd16, "psrlvd") == 2 and count(mrd16, "pmulhuw") == 1,
	"dynamic i16x8 shr must build two factors and use one high multiply")

  local i8 = T.T.i8x16.ct
  local av8 = i8(-128, -99, -17, -1, 0, 1, 7, 15,
		  31, 63, 99, 127, -64, -7, 3, 42)
  local cv8 = i8(0, 1, 7, 8, 9, -1, 3, 6, 2, 5, 15, 4, 1, 7, 8, 0)
  local ml8 = checkloop("i8x16 shl vec lookup",
    {"paddusb", "pshufb", "pmullw", "pand", "por"}, NOCALL,
    function()
      local acc, c = i8(0), cv8
      for _ = 1, 400 do
	acc = simd.shl(acc + av8, c)
	c = c + i8(1)
      end
      return acc + c
    end)
  check(count(ml8, "psllvd") == 0 and count(ml8, "pmullw") == 2 and
	count(ml8, "paddusb") == 1 and count(ml8, "pshufb") == 1,
	"i8x16 shl must use one lookup and two word multiplies")
  local mr8 = checkloop("i8x16 shr vec lookup",
    {"paddusb", "pshufb", "pmullw", "psrlw", "pand", "por"}, NOCALL,
    function()
      local acc, c = i8(0), cv8
      for _ = 1, 400 do
	acc = simd.shr(acc + av8, c)
	c = c + i8(1)
      end
      return acc + c
    end)
  check(count(mr8, "psrlvd") == 0 and count(mr8, "pmullw") == 2 and
	count(mr8, "paddusb") == 1 and count(mr8, "pshufb") == 1,
	"i8x16 shr must use one lookup and two word multiplies")
  local ma8 = checkloop("i8x16 sar vec lookup",
    {"pminub", "pshufb", "pmullw", "psraw", "pand", "por"}, NOCALL,
    function()
      local acc, c = i8(0), cv8
      for _ = 1, 400 do
	acc = simd.sar(acc + av8, c)
	c = c + i8(1)
      end
      return acc + c
    end)
  check(count(ma8, "psravd") == 0 and count(ma8, "pmullw") == 2 and
	count(ma8, "pminub") == 1 and count(ma8, "pshufb") == 1,
	"i8x16 sar must use one lookup and two word multiplies")

  local function check_regcount(name, ct)
    local counts = ffi.new(ffi.typeof("$[512]", ct))
    for i = 0, 511 do counts[i] = ct(i % 7) end
    local body = rawdump("m", function()
      local acc, one = ct(1), ct(1)
      for i = 0, 399 do acc = simd.shl(acc + one, counts[i]) end
      return acc
    end)
    local memshift = false
    for line in body:gmatch("[^\n]+") do
      if line:find("psllv", 1, true) and line:find("[", 1, true) then
	memshift = true
	break
      end
    end
    check(not memshift, name .. " low-pressure count should be prefetched: " ..
	  body:gsub("\n", " | "))
  end

  check_regcount("i32x4", i4)
  check_regcount("i32x8", T.W.i32x8.ct)

  local counts = ffi.new("i32x4[?]", 512 * 15)
  for i = 0, 512 * 15 - 1 do counts[i] = i4(i % 3) end
  local pressured = rawdump("m", function()
    local a1,a2,a3,a4 = i4(1),i4(2),i4(3),i4(4)
    local a5,a6,a7,a8 = i4(5),i4(6),i4(7),i4(8)
    local a9,a10,a11,a12 = i4(9),i4(10),i4(11),i4(12)
    local a13,a14,a15,one = i4(13),i4(14),i4(15),i4(1)
    for i = 0, 399 do
      local p = i*15
      a1=simd.shl(a1+one,counts[p]); a2=simd.shl(a2+one,counts[p+1])
      a3=simd.shl(a3+one,counts[p+2]); a4=simd.shl(a4+one,counts[p+3])
      a5=simd.shl(a5+one,counts[p+4]); a6=simd.shl(a6+one,counts[p+5])
      a7=simd.shl(a7+one,counts[p+6]); a8=simd.shl(a8+one,counts[p+7])
      a9=simd.shl(a9+one,counts[p+8]); a10=simd.shl(a10+one,counts[p+9])
      a11=simd.shl(a11+one,counts[p+10])
      a12=simd.shl(a12+one,counts[p+11])
      a13=simd.shl(a13+one,counts[p+12])
      a14=simd.shl(a14+one,counts[p+13])
      a15=simd.shl(a15+one,counts[p+14])
    end
    return a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12+a13+a14+a15
  end)
  local memshift = false
  for line in pressured:gmatch("[^\n]+") do
    if line:find("psllv", 1, true) and line:find("[", 1, true) then
      memshift = true
      break
    end
  end
  check(memshift, "pressured VPSLLV count must use memory: " ..
	pressured:gsub("\n", " | "))
end)

test("a runtime index permute is packed", function()
  -- A byte vector is a direct PSHUFB. A wider lane needs the index scaled to
  -- a byte offset and spread over its lane first, which is still packed: mask,
  -- shift, replicate, add, permute. Neither may fall back to a call.
  local i8 = T.T.i8x16.ct
  local a8, x8 = i8(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16), i8(3)
  checkloop("i8x16 permute", {"pshufb"}, NOCALL, function()
    local acc = i8(0)
    for k = 1, 400 do acc = acc + simd.shuffle(a8, x8 + i8(k)) end
    return acc
  end)
  local i4 = T.T.i32x4.ct
  local a4, x4 = i4(10, 20, 30, 40), i4(3, 1, 0, 2)
  checkloop("i32x4 permute", {"pshufb", "pslld", "paddb", "pand"}, NOCALL,
	    function()
    local acc = i4(0)
    for k = 1, 400 do acc = acc + simd.shuffle(a4, x4 + i4(k)) end
    return acc
  end)
end)

test("extended numeric conversions stay call-free", function()
  local f4, u4 = T.T.float4.ct, T.T.u32x4.ct
  local mu32 = checkloop("u32x4 to float4",
    {"psrld", "por", "subps", "addps"}, {"call", "cvtdq2ps"}, function()
      local acc = f4(0)
      local v = u4(0, 16777217, 0x80000001, 0xffffffff)
      for _ = 1, 400 do
	acc = acc + simd.convert(f4, v)
	v = v + u4(65537)
      end
      return acc
    end)
  if mu32 then
    check(count(mu32, "cvtdq2ps") == 0,
	  "u32x4 conversion must not use the signed instruction")
  end

  local d2, i2, u2 = T.T.double2.ct, T.T.i64x2.ct, T.T.u64x2.ct
  checkloop("i64x2 to double2",
    {"psrlq", "por", "pxor", "subpd", "addpd"},
    {"call", "cvtsi2sd"}, function()
      local acc = d2(0)
      local v = i2(-9007199254740993LL, 0x7000000000000001LL)
      for _ = 1, 400 do
	acc = acc + simd.convert(d2, v)
	v = v + i2(65537)
      end
      return acc
    end)
  checkloop("u64x2 to double2", {"psrlq", "por", "subpd", "addpd"},
    {"call", "cvtsi2sd"}, function()
      local acc = d2(0)
      local v = u2(9007199254740993ULL, 0xf000000000000001ULL)
      for _ = 1, 400 do
	acc = acc + simd.convert(d2, v)
	v = v + u2(65537)
      end
      return acc
    end)

  local mf64 = checkloop("double2 to i64x2",
    {"cvttsd2si", "punpcklqdq"}, {"call"}, function()
      local acc = i2(0)
      local v = d2(123456789.75, -987654321.25)
      local step = d2(0.5, -0.25)
      for _ = 1, 400 do
	acc = simd.bxor(acc, simd.convert(i2, v))
	v = v + step
      end
      return acc
    end)
  if mf64 then
    check(count(mf64, "cvttsd2si") == 2,
	  "double2 conversion must issue one scalar conversion per lane")
  end
end)

test("a bitcast never produces a conversion between vector types", function()
  -- simd.bitcast re-boxes a value under a different lane type, so the box's
  -- XSTORE is typed with the *destination* lane type while the stored value
  -- still carries the source one. Reading that box back used to make
  -- lj_opt_fwd_xload() synthesise a CONV between two vector types, which the
  -- backend cannot assemble: asm_conv takes the integer path and allocates a
  -- GPR for a value that lives in an XMM register. In an assert build that
  -- tripped "vector constant needs an FP register" in emit_loadk128; in a
  -- release build it silently used the wrong register.
  local i4 = T.T.i32x4.ct
  local f4 = T.T.float4.ct
  local function work()
    local acc = i4(0)
    for _ = 1, 400 do
      -- f4(2.5) is a constant splat, so the value boxed by the bitcast is a
      -- 128 bit constant, and abs then reloads that box with the other type.
      acc = acc + simd.abs(simd.bitcast(i4, f4(2.5)))
    end
    return acc
  end
  local want = work()
  local text = rawdump("i", work)
  -- "vi4.vf4" and friends: a CONV whose source or destination is a vector.
  local bad = text:match("CONV%s+%S+%s+(v%w+%.%w+)") or
	      text:match("CONV%s+%S+%s+(%w+%.v%w+)")
  check(bad == nil, "CONV between vector types in the IR: " .. tostring(bad))
  check(T.same(work(), want), "bitcast value changed once compiled")
end)

test("shifts with a variable count are packed", function()
  -- 8 bit lanes have no shift instruction at all, and there is no 64 bit
  -- arithmetic shift before AVX-512. Both are rewritten by the recorder. A
  -- constant count folds the masks away; these loops use a *variable* count,
  -- so the masks have to be built at runtime and the whole rewrite has to
  -- stay packed instead of aborting the trace back to the interpreter.
  local bit_ = require("bit")
  local i8 = T.T.i8x16.ct
  local a8 = i8(1, -2, 3, -4, 127, -128, 0, -1, 55, -99, 17, -17, 64, -64, 7, -7)
  checkloop("i8x16 shl var", {"psllw", "pmullw", "pand"}, NOCALL, function()
    local acc = i8(0)
    for i = 1, 400 do acc = acc + simd.shl(a8, bit_.band(i, 7)) end
    return acc
  end)
  checkloop("i8x16 shr var", {"psrlw", "pmullw", "pand"}, NOCALL, function()
    local acc = i8(0)
    for i = 1, 400 do acc = acc + simd.shr(a8, bit_.band(i, 7)) end
    return acc
  end)
  checkloop("i8x16 sar var", {"psrlw", "pmullw", "pxor", "psubb"}, NOCALL,
    function()
      local acc = i8(0)
      for i = 1, 400 do acc = acc + simd.sar(a8, bit_.band(i, 7)) end
      return acc
    end)
  local i64 = T.T.i64x2.ct
  local a64 = i64(-1234567890123LL, 9007199254740993LL)
  checkloop("i64x2 sar var", {"psrlq", "pxor", "psubq"}, NOCALL, function()
    local acc = i64(0)
    for i = 1, 400 do acc = acc + simd.sar(a64, bit_.band(i, 63)) end
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
    check(count(m, "psrldq") == 2, "hsum float4: two halving steps, got " ..
	  tostring(count(m, "psrldq")))
  end
  for _, name in ipairs({"i8x16", "u8x16"}) do
    local ct = T.T[name].ct
    local b = ct(1)
    local bm = checkloop(name .. " hsum", {"psadbw", "paddq"},
      NOCALL, function()
	local s, v = 0, ct(0)
	for _ = 1, 400 do
	  v = v + b
	  s = s + tonumber(simd.hsum(v))
	end
	return s
      end)
    if bm then
      check(count(bm, "psadbw") == 1,
	    name .. " hsum: exactly one packed byte partial sum")
      check(count(bm, "paddq") == 1,
	    name .. " hsum: exactly one qword combine")
      check(count(bm, "paddb") == 1,
	    name .. " hsum: byte add is only the loop's vector update")
    end
  end
  for _, spec in ipairs({
    {"i16x8", T.T.i16x8.ct,
      {-32768, 32767, -12345, 23456, -30000, 11111, -22222, 9999}},
    {"u16x8", T.T.u16x8.ct,
      {65535, 32768, 12345, 54321, 60000, 11111, 44444, 9999}},
  }) do
    local name, ct = spec[1], spec[2]
    local a = ct(unpack(spec[3], 1, 8))
    local b = ct(3, 5, 7, 11, 13, 17, 19, 23)
    local wm = checkloop(name .. " hsum product",
      {"pmaddwd", "paddd", "psrldq"}, NOCALL, function()
	local s, v = 0, ct(0)
	for _ = 1, 400 do
	  v = v + a
	  s = s + tonumber(simd.hsum(v * b))
	end
	return s
      end)
    if wm then
      check(count(wm, "pmaddwd") == 1,
	    name .. " hsum product: exactly one packed pair dot")
      check(count(wm, "paddd") == 2 and count(wm, "pmullw") == 0,
	    name .. " hsum product: word multiply/reduction tree must be gone")
    end
  end
  local dotct = T.T.i16x8.ct
  local dotarr = ffi.new(ffi.typeof("$[?]", dotct), 512)
  local dotk = dotct(3, -5, 7, -11, 13, -17, 19, -23)
  local dotbody = loopcode(function()
    local s = 0
    for i = 0, 399 do s = s + tonumber(simd.hsum(dotarr[i] * dotk)) end
    return s
  end)
  if simd.features().avx then
    check(dotbody:match("vpmaddwd xmm[^\n]*%[") ~= nil,
      "AVX word dot must fuse one array operand: " ..
      dotbody:gsub("\n", " | "))
  else
    check(dotbody:match("pmaddwd xmm[^\n]*%[") == nil and
	  dotbody:find("movups", 1, true),
      "legacy word dot must retain its separate unaligned load: " ..
      dotbody:gsub("\n", " | "))
  end
end)

test("unsigned word extrema use horizontal min-position", function()
  local ct = T.T.u16x8.ct
  local a = ct(1, 3, 5, 7, 11, 13, 17, 19)
  local m = checkloop("u16x8 hmin", {"phminposuw"}, NOCALL, function()
    local s, v = 0, ct(0)
    for _ = 1, 400 do
      v = v + a
      s = s + tonumber(simd.hmin(v))
    end
    return s
  end)
  if m then
    check(count(m, "phminposuw") == 1,
	  "u16x8 hmin: exactly one horizontal min-position")
    check(count(m, "pminuw") == 0 and count(m, "psrldq") == 0,
	  "u16x8 hmin: generic shuffle tree must be gone")
  end
  m = checkloop("u16x8 hmax", {"phminposuw", "pxor"}, NOCALL, function()
    local s, v = 0, ct(0)
    for _ = 1, 400 do
      v = v + a
      s = s + tonumber(simd.hmax(v))
    end
    return s
  end)
  if m then
    check(count(m, "phminposuw") == 1,
	  "u16x8 hmax: exactly one complemented horizontal minimum")
    check(count(m, "pmaxuw") == 0 and count(m, "psrldq") == 0,
	  "u16x8 hmax: generic shuffle tree must be gone")
  end
  local arr = ffi.new(ffi.typeof("$[?]", ct), 512)
  local body = loopcode(function()
    local s = 0
    for i = 0, 399 do s = s + tonumber(simd.hmin(arr[i])) end
    return s
  end)
  if simd.features().avx then
    check(body:match("vphminposuw xmm[^\n]*%[") ~= nil,
	  "AVX u16 hmin must encode its array load as a real memory operand: " ..
	  body:gsub("\n", " | "))
  else
    check(body:match("phminposuw xmm[^\n]*%[") == nil,
	  "legacy SSE u16 hmin must retain its separate unaligned load: " ..
	  body:gsub("\n", " | "))
  end
end)

test("signed word extrema use biased horizontal min-position", function()
  local ct = T.T.i16x8.ct
  local a = ct(-1, 3, -5, 7, -11, 13, -17, 19)
  for _, op in ipairs({"hmin", "hmax"}) do
    local m = checkloop("i16x8 " .. op, {"pxor", "phminposuw"},
      NOCALL, function()
	local s, v = 0, ct(0)
	for _ = 1, 400 do
	  v = v + a
	  s = s + tonumber(simd[op](v))
	end
	return s
      end)
    if m then
      check(count(m, "phminposuw") == 1,
	    "i16x8 " .. op .. ": exactly one biased horizontal minimum")
      check(count(m, "pminsw") == 0 and count(m, "pmaxsw") == 0 and
	    count(m, "psrldq") == 0,
	    "i16x8 " .. op .. ": signed shuffle tree must be gone")
    end
  end
end)

test("byte extrema widen into horizontal word minimum", function()
  for _, spec in ipairs({
    {"u8x16", T.T.u8x16.ct, false},
    {"i8x16", T.T.i8x16.ct, true},
  }) do
    local name, ct, signed = spec[1], spec[2], spec[3]
    local a = ct(1)
    for _, op in ipairs({"hmin", "hmax"}) do
      local want = {"punpcklbw", "punpckhbw", "pminuw", "phminposuw"}
      if signed or op == "hmax" then want[#want+1] = "pxor" end
      local m = checkloop(name .. " " .. op, want, NOCALL, function()
	local s, v = 0, ct(0)
	for _ = 1, 400 do
	  v = v + a
	  s = s + tonumber(simd[op](v))
	end
	return s
      end)
      if m then
	check(count(m, "phminposuw") == 1,
	      name .. " " .. op .. ": exactly one horizontal word minimum")
	check(count(m, "pminub") == 0 and count(m, "pmaxub") == 0 and
	      count(m, "pminsb") == 0 and count(m, "pmaxsb") == 0 and
	      count(m, "psrldq") == 0,
	      name .. " " .. op .. ": byte shuffle tree must be gone")
      end
    end
  end
end)

test("shuffle and insert are packed", function()
  local i4 = T.T.i32x4.ct
  local a = i4(1, 2, 3, 4)
  checkloop("shuffle", {"pshufd"}, {"call", "pshufb"}, function()
    local acc = i4(0)
    for _ = 1, 400 do acc = simd.shuffle(acc + a, 3, 2, 1, 0) end
    return acc
  end)
  checkloop("identity shuffle", {"paddd"},
    {"call", "pshufb", "pshufd"}, function()
      local acc = i4(0)
      for _ = 1, 400 do acc = simd.shuffle(acc + a, 0, 1, 2, 3) end
      return acc
    end)
  checkloop("shuffle2", {"punpckldq"}, {"call", "pshufb", "por"}, function()
    local acc = i4(0)
    local b = i4(9, 8, 7, 6)
    for _ = 1, 400 do acc = simd.shuffle2(acc + a, b, 0, 4, 1, 5) end
    return acc
  end)
  checkloop("shuffle2 one source", {"pshufd"},
    {"call", "pshufb", "por"}, function()
      local acc = i4(0)
      local b = i4(9, 8, 7, 6)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + a, b, 3, 2, 1, 0)
      end
      return acc
    end)
  checkloop("shuffle2 second source", {"pshufd"},
    {"call", "pshufb", "por"}, function()
      local acc = i4(0)
      local b = i4(9, 8, 7, 6)
      for _ = 1, 400 do
	acc = simd.shuffle2(a, acc + b, 7, 6, 5, 4)
      end
      return acc
    end)
  checkloop("shuffle2 shufps", {"shufps"},
    {"call", "pshufb", "por"}, function()
      local acc = i4(0)
      local b = i4(9, 8, 7, 6)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + a, b, 3, 0, 6, 5)
      end
      return acc
    end)
  local q2 = T.T.i64x2.ct
  checkloop("shuffle2 shufpd", {"shufpd"},
    {"call", "pshufb", "por"}, function()
      local acc, add, b = q2(0), q2(1, 2), q2(9, 8)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + add, b, 1, 2)
      end
      return acc
    end)
  local b16 = T.T.i8x16.ct
  checkloop("shuffle2 pblendw", {"pblendw"},
    {"call", "pshufb", "por"}, function()
      local acc, add, other = b16(0), b16(1), b16(9)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + add, other,
	  0,1,18,19,4,5,22,23,8,9,26,27,12,13,30,31)
      end
      return acc
    end)
  checkloop("shuffle2 aligned window", {"palignr"},
    {"call", "pshufb", "por"}, function()
      local acc = i4(0)
      local b = i4(9, 8, 7, 6)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + a, b, 5, 6, 7, 0)
      end
      return acc
    end)
  -- A variable lane index builds the mask with a packed compare instead of
  -- falling back to the interpreter.
  checkloop("insert var lane", {"pcmpeqd", "pandn", "por"}, NOCALL, function()
    local acc = i4(0)
    for i = 1, 400 do acc = simd.insert(acc + a, i % 4, 42) end
    return acc
  end)
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
  local body = loopcode(function()
    local k = ti.ct(2)
    for r = 1, 40 do
      local base = 0
      for i = 0, N-1 do dst[i] = src[i] * k end
    end
  end)
  local m = mnemonics(body)
  check(count(m, "movups") > 0 and count(m, "mulps") > 0,
	"array map must use packed unaligned memory operations: " ..
	body:gsub("\n", " | "))
  if simd.features().avx then
    check(body:match("vmulps [^\n]*%[") ~= nil,
	  "a sunk source box must not prevent AVX load fusion: " ..
	  body:gsub("\n", " | "))
  else
    check(body:match("mulps [^\n]*%[") == nil,
	  "legacy SSE must keep the unaligned array load separate: " ..
	  body:gsub("\n", " | "))
  end
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
  check(count(m, "addps") == 1 and count(m, "mulps") <= 1 and
	count(m, "addss") == 0 and count(m, "mulss") == 0,
	"loop body should be one packed multiply-add, got: " ..
	body:gsub("\n", " | "))
end)

if simd.features().avx2 then
  local function checkymm(name, want, f)
    local body, isloop = loopcode(f)
    check(isloop, name .. " must compile as a loop: " .. body:gsub("\n", " | "))
    for _, needle in ipairs(want) do
      check(body:find(needle, 1, true) ~= nil,
	    name .. " must contain '" .. needle .. "': " ..
	    body:gsub("\n", " | "))
    end
    check(not body:find("call", 1, true),
	  name .. " must not call a scalar helper: " .. body:gsub("\n", " | "))
    return body
  end

  test("256-bit arithmetic uses YMM encodings", function()
    local f8 = T.W.float8.ct
    local a = f8(1, 2, 3, 4, 5, 6, 7, 8)
    local body = loopcode(function()
      local acc = f8(0)
      for _ = 1, 400 do acc = acc + a - f8(1) end
      return acc
    end)
    check(body:find("vaddps ymm", 1, true) ~= nil,
	  "float8 add must target YMM registers: " .. body:gsub("\n", " | "))
    check(body:find("vsubps ymm", 1, true) ~= nil,
	  "float8 sub must target YMM registers: " .. body:gsub("\n", " | "))
    check(body:match("vaddps ymm[^\n]*%[") ~= nil,
	  "float8 load must fuse into YMM arithmetic: " ..
	  body:gsub("\n", " | "))

    local i8 = T.W.i32x8.ct
    local b = i8(1, 2, 3, 4, 5, 6, 7, 8)
    body = loopcode(function()
      local acc = i8(0)
      for _ = 1, 400 do acc = acc + b - i8(1) end
      return acc
    end)
    check(body:find("vpaddd ymm", 1, true) ~= nil,
	  "i32x8 add must target YMM registers: " .. body:gsub("\n", " | "))
    check(body:find("vpsubd ymm", 1, true) ~= nil,
	  "i32x8 sub must target YMM registers: " .. body:gsub("\n", " | "))
  end)

  test("256-bit byte rotate stays one YMM shuffle", function()
    local ct = T.W.i32x8.ct
    local body = checkymm("i32x8 byte rotate", {"vpshufb ymm"}, function()
      local acc, add = ct(0x01020304), ct(0x10101)
      for _ = 1, 400 do
	local x = acc + add
	acc = simd.bor(simd.shl(x, 8), simd.shr(x, 24))
      end
      return acc
    end)
    check(not body:find("vpslld", 1, true) and
	  not body:find("vpsrld", 1, true) and
	  not body:find("vpor", 1, true),
	  "i32x8 byte rotate retained shift/or instructions: " ..
	  body:gsub("\n", " | "))
  end)

  test("256-bit direct operations stay in YMM registers", function()
    local i8 = T.W.i32x8.ct
    local a, b = i8(3), i8(17)
    checkymm("i32x8 direct operations",
      {"vpcmpgtd ymm", "vpminsd ymm", "vpmaxsd ymm", "vpslld ymm",
       "vpbroadcastd ymm", "vmovmskps"},
      function()
	local acc, bits = i8(1), 0
	for i = 1, 400 do
	  local m = simd.gt(acc, b)
	  bits = bits + simd.movemask(m)
	  acc = simd.select(m, simd.min(acc, b), simd.max(acc, a))
	  acc = simd.shl(acc, 1) + i
	end
	return acc, bits
      end)

    checkymm("i32x8 whole equality",
      {"vpcmpeqb ymm", "vpmovmskb"},
      function()
	local acc, hits = i8(1), 0
	for _ = 1, 400 do
	  acc = acc + a
	  if acc == b then hits = hits + 1 end
	end
	return acc, hits
      end)

    local f8 = T.W.float8.ct
    local x = f8(1.25, 2.5, 3.75, 4, 5.25, 6.5, 7.75, 8)
    checkymm("float8 unary and fma",
      {"vsqrtps ymm", "vroundps ymm", "vfmadd"},
      function()
	local acc, k, c = f8(1), f8(1.0001), f8(0.0003)
	for _ = 1, 400 do
	  acc = simd.fma(simd.round(simd.sqrt(simd.abs(acc + x))), k, c)
	end
	return acc
      end)

    local h16 = T.W.i16x16.ct
    checkymm("i16x16 saturating and mulhi",
      {"vpaddsw ymm", "vpsubsw ymm", "vpmulhw ymm"},
      function()
	local acc, a16, k16 = h16(0), h16(30000), h16(-23123)
	for _ = 1, 400 do
	  acc = simd.mulhi(simd.subs(simd.adds(acc, a16), h16(-7)), k16)
	end
	return acc
      end)

    local hc = h16(0, 1, 15, 16, 17, -1, 3, 9,
		    2, 14, 20, 7, 4, 12, 8, 31)
    local hbody = checkymm("i16x16 invariant per-lane shl",
      {"vpmullw ymm"}, function()
	local acc = h16(1)
	for _ = 1, 400 do acc = simd.shl(acc + h16(3), hc) end
	return acc
      end)
    local _, hmul = hbody:gsub("vpmullw ymm", "")
    check(hmul == 1 and not hbody:find("vpsllvd ymm", 1, true),
	  "invariant i16x16 shl must hoist the factor and use one YMM multiply")
    local hdbody = checkymm("i16x16 dynamic per-lane shl",
      {"vpsllvd ymm", "vpmullw ymm", "vpand ymm", "vpor ymm"}, function()
	local acc, c = h16(1), hc
	for _ = 1, 400 do
	  acc = simd.shl(acc + h16(3), c)
	  c = c + h16(1)
	end
	return acc + c
      end)
    local _, hshift = hdbody:gsub("vpsllvd ymm", "")
    local _, hdmul = hdbody:gsub("vpmullw ymm", "")
    check(hshift == 2 and hdmul == 1,
	  "dynamic i16x16 shl must build two factors and use one YMM multiply")
    local hrbody = checkymm("i16x16 invariant per-lane shr",
      {"vpmulhuw ymm", "vpand ymm", "vpor ymm"}, function()
	local acc = h16(1)
	for _ = 1, 400 do acc = simd.shr(acc + h16(3), hc) end
	return acc
      end)
    local _, hrmul = hrbody:gsub("vpmulhuw ymm", "")
    check(hrmul == 1 and not hrbody:find("vpsrlvd ymm", 1, true),
	  "invariant i16x16 shr must hoist the factor and high-multiply once")
    local habody = checkymm("i16x16 invariant per-lane sar",
      {"vpmulhw ymm", "vpand ymm", "vpaddw ymm"}, function()
	local acc = h16(1)
	for _ = 1, 400 do acc = simd.sar(acc + h16(3), hc) end
	return acc
      end)
    local _, hamul = habody:gsub("vpmulhw ymm", "")
    check(hamul == 1 and not habody:find("vpsrlvd ymm", 1, true),
	  "invariant i16x16 sar must hoist the factor and high-multiply once")
    local hrdbody = checkymm("i16x16 dynamic per-lane shr",
      {"vpsrlvd ymm", "vpmulhuw ymm", "vpcmpeqw ymm", "vpand ymm",
       "vpor ymm"}, function()
	local acc, c = h16(1), hc
	for _ = 1, 400 do
	  acc = simd.shr(acc + h16(3), c)
	  c = c + h16(1)
	end
	return acc + c
      end)
    local _, hrdshift = hrdbody:gsub("vpsrlvd ymm", "")
    local _, hrdmul = hrdbody:gsub("vpmulhuw ymm", "")
    check(hrdshift == 2 and hrdmul == 1,
	  "dynamic i16x16 shr must build two factors and high-multiply once")
    local hadbody = checkymm("i16x16 dynamic per-lane sar",
      {"vpminuw ymm", "vpsrlvd ymm", "vpcmpgtw ymm", "vpmulhw ymm",
       "vpand ymm", "vpaddw ymm"}, function()
	local acc, c = h16(1), hc
	for _ = 1, 400 do
	  acc = simd.sar(acc + h16(3), c)
	  c = c + h16(1)
	end
	return acc + c
      end)
    local _, hadshift = hadbody:gsub("vpsrlvd ymm", "")
    local _, hadmul = hadbody:gsub("vpmulhw ymm", "")
    check(hadshift == 2 and hadmul == 1,
	  "dynamic i16x16 sar must clamp, build two factors and high-multiply")

    local sb32 = T.W.i8x32.ct
    checkymm("i8x32 mulhi emulation",
      {"vpmulhw ymm", "vpsraw ymm", "vpsllw ymm"},
      function()
	local acc, k = sb32(-119), sb32(117)
	for _ = 1, 400 do acc = simd.mulhi(acc + sb32(3), k) end
	return acc
      end)

    local sbsqbody = checkymm("i8x32 mulhi square",
      {"vpabsb ymm", "vpmullw ymm", "vpsrlw ymm", "vpand ymm", "vpor ymm"},
      function()
	local acc = sb32(-119)
	for _ = 1, 400 do
	  local x = acc + sb32(3)
	  acc = simd.mulhi(x, x) + sb32(1)
	end
	return acc
      end)
    local _, nsb = sbsqbody:gsub("vpmullw ymm", "")
    check(nsb == 2 and not sbsqbody:find("vpmulhw", 1, true),
	  "i8x32 mulhi square must use absolute bytes and two low products")

    local ub32 = T.W.u8x32.ct
    local ubbody = checkymm("u8x32 mulhi square",
      {"vpmullw ymm", "vpsrlw ymm", "vpand ymm", "vpor ymm"},
      function()
	local acc = ub32(241)
	for _ = 1, 400 do
	  local x = acc + ub32(3)
	  acc = simd.mulhi(x, x) + ub32(1)
	end
	return acc
      end)
    local _, nub = ubbody:gsub("vpmullw ymm", "")
    check(nub == 2 and not ubbody:find("vpmulhuw", 1, true),
	  "u8x32 mulhi square must use two low word products")

    local bc = sb32(0, 1, 7, 8, 9, -1, 3, 6)
    local blbody = checkymm("i8x32 per-lane shl lookup",
      {"vpaddusb ymm", "vpshufb ymm", "vpmullw ymm"}, function()
	local acc, c = sb32(1), bc
	for _ = 1, 400 do
	  acc = simd.shl(acc + sb32(3), c)
	  c = c + sb32(1)
	end
	return acc + c
      end)
    local _, nbl = blbody:gsub("vpmullw ymm", "")
    check(nbl == 2 and not blbody:find("vpsllvd", 1, true),
	  "i8x32 shl must use two YMM word multiplies and no dword shift")

    local brbody = checkymm("i8x32 per-lane shr lookup",
      {"vpaddusb ymm", "vpshufb ymm", "vpmullw ymm", "vpsrlw ymm"},
      function()
	local acc, c = sb32(-119), bc
	for _ = 1, 400 do
	  acc = simd.shr(acc + sb32(3), c)
	  c = c + sb32(1)
	end
	return acc + c
      end)
    local _, nbr = brbody:gsub("vpmullw ymm", "")
    check(nbr == 2 and not brbody:find("vpsrlvd", 1, true),
	  "i8x32 shr must use two YMM word multiplies and no dword shift")

    local babody = checkymm("i8x32 per-lane sar lookup",
      {"vpminub ymm", "vpshufb ymm", "vpmullw ymm", "vpsraw ymm"},
      function()
	local acc, c = sb32(-119), bc
	for _ = 1, 400 do
	  acc = simd.sar(acc + sb32(3), c)
	  c = c + sb32(1)
	end
	return acc + c
      end)
    local _, nba = babody:gsub("vpmullw ymm", "")
    check(nba == 2 and not babody:find("vpsravd", 1, true),
	  "i8x32 sar must use two YMM word multiplies and no dword shift")

    local d8, ud8 = T.W.i32x8.ct, T.W.u32x8.ct
    checkymm("i32x8 mulhi emulation",
      {"vpmuldq ymm", "vpblendw ymm", "vpsrlq ymm"},
      function()
	local acc, k = d8(0x40000000), d8(-123456789)
	for _ = 1, 400 do acc = simd.mulhi(acc + d8(7), k) end
	return acc
      end)
    checkymm("u32x8 mulhi emulation",
      {"vpmuludq ymm", "vpblendw ymm", "vpsrlq ymm"},
      function()
	local acc, k = ud8(0xf0000000), ud8(0xc0000001)
	for _ = 1, 400 do acc = simd.mulhi(acc + ud8(7), k) end
	return acc
      end)

    local sq4, uq4 = T.W.i64x4.ct, T.W.u64x4.ct
    local sbody = checkymm("i64x4 mulhi emulation",
      {"vpmuludq ymm", "vpaddq ymm", "vpsubq ymm", "vpsrad ymm"},
      function()
	local acc = sq4(-9223372036854775807LL-1, -7, 7,
			0x7000000000000001LL)
	local k = sq4(-12345678901234567LL)
	for _ = 1, 400 do acc = simd.mulhi(acc + sq4(3), k) end
	return acc
      end)
    local _, sn = sbody:gsub("vpmuludq ymm", "")
    local _, nsub = sbody:gsub("vpsubq ymm", "")
    check(sn == 4 and nsub == 1,
	  "i64x4 mulhi must use four products and one signed correction")

    local ssqbody = checkymm("i64x4 mulhi square",
      {"vpmuludq ymm", "vpaddq ymm", "vpsubq ymm", "vpsrad ymm"},
      function()
	local acc = sq4(-9223372036854775807LL-1, -7, 7,
			0x7000000000000001LL)
	for _ = 1, 400 do
	  local x = acc + sq4(3)
	  acc = simd.mulhi(x, x) + sq4(7)
	end
	return acc
      end)
    local _, ssqn = ssqbody:gsub("vpmuludq ymm", "")
    local _, ssqsign = ssqbody:gsub("vpsrad ymm", "")
    local _, ssqsub = ssqbody:gsub("vpsubq ymm", "")
    check(ssqn == 3 and ssqsign == 1 and ssqsub == 1,
	  "i64x4 mulhi square must reuse its product and sign mask")

    local ubody = checkymm("u64x4 mulhi emulation",
      {"vpmuludq ymm", "vpaddq ymm", "vpsrlq ymm"}, function()
	local acc = uq4(0xffffffffffffffffULL, 0x8000000000000001ULL,
			7, 0xc000000000000003ULL)
	local k = uq4(0xd000000000000001ULL)
	for _ = 1, 400 do acc = simd.mulhi(acc + uq4(3), k) end
	return acc
      end)
    local _, un = ubody:gsub("vpmuludq ymm", "")
    check(un == 4, "u64x4 mulhi must use four YMM dword products")

    local usqbody = checkymm("u64x4 mulhi square",
      {"vpmuludq ymm", "vpaddq ymm", "vpsrlq ymm"}, function()
	local acc = uq4(0xffffffffffffffffULL, 0x8000000000000001ULL,
			7, 0xc000000000000003ULL)
	for _ = 1, 400 do
	  local x = acc + uq4(3)
	  acc = simd.mulhi(x, x) + uq4(7)
	end
	return acc
      end)
    local _, usqn = usqbody:gsub("vpmuludq ymm", "")
    check(usqn == 3 and not usqbody:find("vpsubq", 1, true),
	  "u64x4 mulhi square must reuse its cross product")

    local fi, ii = T.W.float8.ct, T.W.i32x8.ct
    local fv = fi(-7.5, -1.25, 0, 1.25, 7.5, 100.75, 1234.5, 9999)
    checkymm("float8 i32x8 conversion",
      {"vcvttps2dq ymm", "vcvtdq2ps ymm"},
      function()
	local acc, v = fi(0), fv
	for _ = 1, 400 do
	  v = v + fi(0.25)
	  acc = acc + simd.convert(fi, simd.convert(ii, v))
	end
	return acc
      end)

    local ui = T.W.u32x8.ct
    checkymm("u32x8 to float8 conversion",
      {"vpsrld ymm", "vpor ymm", "vsubps ymm", "vaddps ymm"},
      function()
	local acc = fi(0)
	local v = ui(0, 1, 16777217, 0x7fffffff,
		     0x80000001, 0xffffff01, 0xfffffffe, 0xffffffff)
	for _ = 1, 400 do
	  acc = acc + simd.convert(fi, v)
	  v = v + ui(65537)
	end
	return acc
      end)

    local fd, siq, uiq = T.W.double4.ct, T.W.i64x4.ct, T.W.u64x4.ct
    checkymm("i64x4 to double4 conversion",
      {"vpsrlq ymm", "vpor ymm", "vsubpd ymm", "vaddpd ymm"},
      function()
	local acc = fd(0)
	local v = siq(-9007199254740993LL, 9007199254740993LL,
		      -0x7000000000000000LL, 0x7000000000000001LL)
	for _ = 1, 400 do
	  acc = acc + simd.convert(fd, v)
	  v = v + siq(65537)
	end
	return acc
      end)
    checkymm("u64x4 to double4 conversion",
      {"vpsrlq ymm", "vpor ymm", "vsubpd ymm", "vaddpd ymm"},
      function()
	local acc = fd(0)
	local v = uiq(0, 9007199254740993ULL, 0x8000000000000001ULL,
		      0xffffffffffffffffULL)
	for _ = 1, 400 do
	  acc = acc + simd.convert(fd, v)
	  v = v + uiq(65537)
	end
	return acc
      end)

    local cvq = checkymm("double4 to i64x4 conversion",
      {"vextractf128", "vcvttsd2si", "vpunpcklqdq", "vinsertf128"},
      function()
	local acc = siq(0)
	local v = fd(123456789.75, -987654321.25, 9007199254740991,
		     -9007199254740991)
	local step = fd(0.5, -0.25, 1, -1)
	for _ = 1, 400 do
	  acc = simd.bxor(acc, simd.convert(siq, v))
	  v = v + step
	end
	return acc
      end)
    local _, ncvq = cvq:gsub("vcvttsd2si", "")
    check(ncvq == 4, "double4 conversion must issue one conversion per lane")

    local b8, q4 = T.W.i8x32.ct, T.W.i64x4.ct
    checkymm("i8x32 multiply emulation", {"vpmullw ymm"}, function()
      local acc, k = b8(1), b8(3)
      for _ = 1, 400 do acc = acc * k + b8(1) end
      return acc
    end)
    checkymm("i64x4 multiply emulation", {"vpmuludq ymm"}, function()
      local acc, k = q4(1), q4(3)
      for _ = 1, 400 do acc = acc * k + q4(1) end
      return acc
    end)
  end)

  test("cross-width conversions use native AVX2 sequences", function()
    local b16, ub16 = T.T.i8x16.ct, T.T.u8x16.ct
    local h16, uh16 = T.W.i16x16.ct, T.W.u16x16.ct
    checkymm("signed byte widening", {"vpmovsxbw ymm"}, function()
      local v, acc = b16(-127), h16(0)
      for _ = 1, 400 do
	v = v + b16(3)
	acc = simd.bxor(acc, simd.convert(h16, v))
      end
      return acc
    end)
    checkymm("unsigned byte widening", {"vpmovzxbw ymm"}, function()
      local v, acc = ub16(129), uh16(0)
      for _ = 1, 400 do
	v = v + ub16(3)
	acc = simd.bxor(acc, simd.convert(uh16, v))
      end
      return acc
    end)
    checkymm("integer lane narrowing", {"vpshufb ymm", "permq ymm"},
      function()
	local v, acc = h16(0x1234), b16(0)
	for _ = 1, 400 do
	  v = v + h16(257)
	  acc = simd.bxor(acc, simd.convert(b16, v))
	end
	return acc
      end)

    local f4, i4, u4 = T.T.float4.ct, T.T.i32x4.ct, T.T.u32x4.ct
    local d4 = T.W.double4.ct
    checkymm("float4 to double4", {"vcvtps2pd ymm"}, function()
      local v, acc = f4(1.25), d4(0)
      for _ = 1, 400 do
	v = v + f4(0.25)
	acc = simd.bxor(acc, simd.convert(d4, v))
      end
      return acc
    end)
    local mixedbody = checkymm("double4 to float4", {"vcvtpd2ps"}, function()
      local v, acc = d4(1.25), f4(0)
      for _ = 1, 400 do
	v = v + d4(0.25)
	acc = simd.bxor(acc, simd.convert(f4, v))
      end
      return acc
    end)
    for _, legacy in ipairs({" movaps xmm", " movups xmm"}) do
      check(not mixedbody:find(legacy, 1, true),
	    "mixed XMM/YMM loop must not contain legacy '" .. legacy .. "': " ..
	    mixedbody:gsub("\n", " | "))
    end
    checkymm("i32x4 to double4", {"vcvtdq2pd ymm"}, function()
      local v, acc = i4(-1000, -1, 1, 1000), d4(0)
      for _ = 1, 400 do
	v = v + i4(3)
	acc = simd.bxor(acc, simd.convert(d4, v))
      end
      return acc
    end)
    checkymm("u32x4 to double4",
      {"vpmovzxdq ymm", "vpor ymm", "vsubpd ymm"}, function()
	local v = u4(0, 1, 0x80000001, 0xffffffff)
	local acc = d4(0)
	for _ = 1, 400 do
	  v = v + u4(65537)
	  acc = simd.bxor(acc, simd.convert(d4, v))
	end
	return acc
      end)

    local h8, f8 = T.T.i16x8.ct, T.W.float8.ct
    checkymm("i16x8 to float8",
      {"vpmovsxwd ymm", "vcvtdq2ps ymm"}, function()
	local v, acc = h8(-30000, -7, -1, 0, 1, 7, 100, 30000), f8(0)
	for _ = 1, 400 do
	  v = v + h8(3)
	  acc = simd.bxor(acc, simd.convert(f8, v))
	end
	return acc
      end)
    checkymm("float8 to i16x8",
      {"vcmpps ymm", "vcvttps2dq ymm", "vpshufb ymm", "permq ymm"},
      function()
	local v = f8(-32768, -32768.5, 32767, 32767.5,
		     0/0, 1/0, -1/0, -1.9)
	local step = f8(0, 0, 0, 0, 0, 0, 0, 0.01)
	local acc = h8(0)
	for _ = 1, 400 do
	  v = v + step
	  acc = simd.bxor(acc, simd.convert(h8, v))
	end
	return acc
      end)

    local sq4, uq4 = T.W.i64x4.ct, T.W.u64x4.ct
    local sbody = checkymm("i64x4 to float4",
      {"vextractf128", "vcvtsi2ss", "vunpcklps", "vshufps"}, function()
	local v = sq4(0x4000004000000001LL, -0x4000004000000001LL, 0, 1)
	local acc = f4(0)
	for _ = 1, 400 do
	  v = v + sq4(17)
	  acc = simd.bxor(acc, simd.convert(f4, v))
	end
	return acc
      end)
    local _, nscvt = sbody:gsub("vcvtsi2ss", "")
    check(nscvt == 4, "i64x4 to float4 must issue one conversion per lane")

    local ubody = checkymm("u64x4 to float4",
      {"vextractf128", "vcvtsi2ss", "vaddss", "vunpcklps"}, function()
	local v = uq4(0x8000008000000001ULL, 0x4000004000000001ULL, 0, 1)
	local acc = f4(0)
	for _ = 1, 400 do
	  v = v + uq4(17)
	  acc = simd.bxor(acc, simd.convert(f4, v))
	end
	return acc
      end)
    local _, nucvt = ubody:gsub("vcvtsi2ss", "")
    check(nucvt == 8,
	  "u64x4 to float4 needs signed/high-half paths for every lane")
  end)

  test("AVX2 shuffles and conversions consume array loads directly", function()
    local i8 = T.T.i8x16.ct
    local h16 = T.W.i16x16.ct
    local i32 = T.W.i32x8.ct
    local f8 = T.W.float8.ct
    local f4 = T.T.float4.ct
    local d4 = T.W.double4.ct
    local ix = i32(7, 0, 6, 1, 5, 2, 4, 3)
    local cases = {
      {"runtime permute", i32, i32(10, 20, 30, 40, 50, 60, 70, 80),
       i32, "vpermd", function(x) return simd.shuffle(x, ix) end},
      {"qword permute", T.W.i64x4.ct, T.W.i64x4.ct(10, 20, 30, 40),
       T.W.i64x4.ct, "permq",
       function(x) return simd.shuffle(x, 3, 1, 2, 0) end},
      {"i32 to float", i32, i32(-7, 2, 30, -4, 5, 60, -8, 9),
       f8, "vcvtdq2ps", function(x) return simd.convert(f8, x) end},
      {"float to i32", f8, f8(-7.75, 2.5, 30.25, -4, 5, 60, -8, 9),
       i32, "vcvttps2dq", function(x) return simd.convert(i32, x) end},
      {"signed byte widening", i8,
       i8(-1, 2, -3, 4, -5, 6, -7, 8, -9, 10, -11, 12, -13, 14, -15, 16),
       h16, "vpmovsxbw", function(x) return simd.convert(h16, x) end},
      {"float to double", f4, f4(-1.5, 2.25, -3.75, 4),
       d4, "vcvtps2pd", function(x) return simd.convert(d4, x) end},
      {"double to float", d4, d4(-1.5, 2.25, -3.75, 4),
       f4, "vcvtpd2ps", function(x) return simd.convert(f4, x) end},
    }
    for _, c in ipairs(cases) do
      local src = ffi.new(ffi.typeof("$[1]", c[2]))
      src[0] = c[3]
      local one = c[6](src[0])
      local got
      local body = rawdump("m", function()
	local acc = c[4](0)
	for _ = 1, 400 do acc = acc + c[6](src[0]) end
	got = acc
	return acc
      end)
      local memop = false
      for line in body:gmatch("[^\n]+") do
	if line:find(c[5], 1, true) and line:find("[", 1, true) then
	  memop = true
	  break
	end
      end
      check(memop, c[1] .. " did not consume its array load directly: " ..
	    body:gsub("\n", " | "))
      check(T.same(got, one * c[4](400)),
	    c[1] .. " memory-source result changed")
    end
  end)

  test("mixed scalar and YMM loops stay entirely VEX-clean", function()
    local f8 = T.W.float8.ct
    local p = ffi.new("double[2]", 1.000001, 0)
    local inc = f8(0.001)
    local body, isloop = loopcode(function()
      local v, s = f8(1), 1.0
      for _ = 1, 400 do
	v = v + inc
	s = (s + p[0]) * 0.5
	p[1] = s
	s = math.sqrt(s*s + 1)
	if s < 0 then s = -s end
      end
      return tonumber(v[0]) + s + p[1]
    end)
    local m = mnemonics(body)
    check(isloop, "mixed scalar/YMM test must compile as a loop")
    for _, op in ipairs({"vaddps", "vaddsd", "vmulsd", "vsqrtsd",
			 "vucomisd", "vmovsd"}) do
      check((m[op] or 0) > 0,
	    "mixed scalar/YMM loop expected " .. op .. ": " ..
	    body:gsub("\n", " | "))
    end
    local legacy = {}
    for line in body:gmatch("[^\n]+") do
      local op = line:match("^%x+%s+(%a[%w]*)")
      if op and op:sub(1, 1) ~= "v" and line:find("xmm", 1, true) then
	legacy[#legacy+1] = op
      end
    end
    check(#legacy == 0,
	  "mixed scalar/YMM loop contains legacy SSE: " ..
	  table.concat(legacy, ", ") .. " | " .. body:gsub("\n", " | "))
  end)

  test("constant modulo stays inline in YMM loops", function()
    local bit_ = require("bit")
    local i8 = T.W.i32x8.ct
    local inc = i8(1, 2, 3, 4, 5, 6, 7, 8)
    local body, isloop = loopcode(function()
      local v, s = i8(0), 0
      for i = 1, 400 do
	v = v + inc
	s = bit_.bxor(s, i % 37)
      end
      return v, s
    end)
    local m = mnemonics(body)
    check(isloop, "constant modulo/YMM test must compile as a loop")
    check((m.vpaddd or 0) > 0,
	  "constant modulo/YMM loop lost packed add: " .. body:gsub("\n", " | "))
    check((m.imul or 0) >= 2,
	  "constant modulo must use reciprocal multiplies: " ..
	  body:gsub("\n", " | "))
    check((m.idiv or 0) == 0 and (m.call or 0) == 0,
	  "constant modulo must not divide or call: " .. body:gsub("\n", " | "))
  end)

  test("256-bit cross-lane operations stay in YMM registers", function()
    local i8 = T.W.i32x8.ct
    local a = i8(1, 2, 3, 4, 5, 6, 7, 8)
    local body = checkymm("i32x8 reverse", {"vpermd ymm"}, function()
	local acc = i8(0)
	for _ = 1, 400 do
	  acc = simd.shuffle(acc + a, 7, 6, 5, 4, 3, 2, 1, 0)
	end
	return acc
      end)
    check(not body:find("vpshufb ymm", 1, true),
	  "a cross-half 32 bit constant must use one VPERMD")

    body = checkymm("i32x8 same-half shuffle", {"vpshufd ymm"}, function()
      local acc = i8(0)
      for _ = 1, 400 do
	acc = simd.shuffle(acc + a, 3, 2, 1, 0, 7, 6, 5, 4)
      end
      return acc
    end)
    check(not body:find("vpermd ymm", 1, true) and
	  not body:find("vperm2i128 ymm", 1, true),
	  "a same-half constant must use one lane-local shuffle")

    body = checkymm("i32x8 mixed constant permute", {"vpermd ymm"}, function()
      local acc = i8(0)
      for _ = 1, 400 do
	acc = simd.shuffle(acc + a, 0, 5, 2, 7, 4, 1, 6, 3)
      end
      return acc
    end)
    check(not body:find("vpshufb ymm", 1, true),
	  "a mixed 32 bit constant must use one VPERMD")

    local ix = i8(4, 5, 6, 7, 0, 1, 2, 3)
    body = checkymm("i32x8 runtime permute", {"vpermd ymm"}, function()
	local acc = i8(0)
	for _ = 1, 400 do acc = simd.shuffle(acc + a, ix) end
	return acc
      end)
    check(not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpand ymm", 1, true),
	  "a 32 bit runtime permute must be one direct packed operation")

    local q4 = T.W.i64x4.ct
    local q = q4(1, 2, 3, 4)
    body = checkymm("i64x4 constant permute", {"permq ymm"}, function()
      local acc = q4(0)
      for _ = 1, 400 do acc = simd.shuffle(acc + q, 3, 1, 2, 0) end
      return acc
    end)
    check(not body:find("vpshufb ymm", 1, true),
	  "a constant 64 bit permute must use VPERMQ")

    body = checkymm("i64x4 same-half shuffle", {"vpshufd ymm"}, function()
      local acc = q4(0)
      for _ = 1, 400 do acc = simd.shuffle(acc + q, 1, 0, 3, 2) end
      return acc
    end)
    check(not body:find("permq ymm", 1, true) and
	  not body:find("vperm2i128 ymm", 1, true),
	  "a same-half 64 bit constant must use one lane-local shuffle")

    body = checkymm("i32x8 half swap", {"vperm2i128 ymm"}, function()
      local acc = i8(0)
      for _ = 1, 400 do
	acc = simd.shuffle(acc + a, 4, 5, 6, 7, 0, 1, 2, 3)
      end
      return acc
    end)
    check(not body:find("vpermd ymm", 1, true) and
	  not body:find("vpshufb ymm", 1, true),
	  "an exact half swap must not load a lane-control vector")

    local b32 = T.W.i8x32.ct
    local bv = b32(1, 2, 3, 4, 5, 6, 7, 8,
		   9, 10, 11, 12, 13, 14, 15, 16,
		   17, 18, 19, 20, 21, 22, 23, 24,
		   25, 26, 27, 28, 29, 30, 31, 32)
    body = checkymm("i8x32 half swap", {"vperm2i128 ymm"}, function()
      local acc = b32(0)
      for _ = 1, 400 do
	acc = simd.shuffle(acc + bv,
	  16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
	  0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15)
      end
      return acc
    end)
    check(not body:find("vpshufb ymm", 1, true),
	  "a byte-vector half swap must not retain an identity VPSHUFB")

    body = checkymm("i32x8 low unpack", {"vpunpckldq ymm"}, function()
      local acc = i8(0)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + a, a, 0, 8, 1, 9, 4, 12, 5, 13)
      end
      return acc
    end)
    check(not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "a lane-local low interleave must be one unpack")

    body = checkymm("i32x8 high unpack", {"vpunpckhdq ymm"}, function()
      local acc = i8(0)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + a, a, 2, 10, 3, 11, 6, 14, 7, 15)
      end
      return acc
    end)
    check(not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "a lane-local high interleave must be one unpack")

    local ub32 = T.W.u8x32.ct
    local uv = ub32(1)
    body = checkymm("u8x32 low unpack", {"vpunpcklbw ymm"}, function()
      local acc = ub32(0)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + uv, uv,
	  0,32,1,33,2,34,3,35,4,36,5,37,6,38,7,39,
	  16,48,17,49,18,50,19,51,20,52,21,53,22,54,23,55)
      end
      return acc
    end)
    check(not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "a byte interleave must use PUNPCKLBW")

    body = checkymm("i8x32 word blend", {"vpblendw ymm"}, function()
      local acc, add, other = b32(0), b32(1), b32(9)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + add, other,
	  0,1,34,35,4,5,38,39,8,9,42,43,12,13,46,47,
	  16,17,50,51,20,21,54,55,24,25,58,59,28,29,62,63)
      end
      return acc
    end)
    check(not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "a representable byte blend must use VPBLENDW")

    body = checkymm("i32x8 independent blend", {"vpblendd ymm"}, function()
      local acc, other = i8(0), i8(11, 12, 13, 14, 15, 16, 17, 18)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc+a, other, 0,9,2,3,12,5,14,7)
      end
      return acc
    end)
    check(not body:find("vpblendw ymm", 1, true) and
	  not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "a non-repeating dword mask must use one VPBLENDD")

    body = checkymm("i64x4 independent blend", {"vpblendd ymm"}, function()
      local q = T.W.i64x4.ct
      local acc, add, other = q(0), q(1), q(11, 12, 13, 14)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc+add, other, 4,1,2,7)
      end
      return acc
    end)
    check(not body:find("vpblendw ymm", 1, true) and
	  not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "a non-repeating qword mask must use one VPBLENDD")

    body = checkymm("i8x32 unpaired byte blend",
      {"vpshufb ymm", "vpor ymm"}, function()
      local acc, add, other = b32(0), b32(1), b32(9)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + add, other,
	  0,33,2,35,4,37,6,39,8,41,10,43,12,45,14,47,
	  16,49,18,51,20,53,22,55,24,57,26,59,28,61,30,63)
      end
      return acc
    end)
    check(not body:find("vpblendw ymm", 1, true),
	  "independent byte selections must not use a word blend")

    body = checkymm("i32x8 lane-local aligned window",
      {"vpalignr ymm"}, function()
      local acc, other = i8(0), i8(11, 12, 13, 14, 15, 16, 17, 18)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc+a, other, 9,10,11,0,13,14,15,4)
      end
      return acc
    end)
    check(not body:find("vperm2i128 ymm", 1, true) and
	  not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "a lane-local aligned window must be one VPALIGNR")

    body = checkymm("i32x8 full aligned window",
      {"vperm2i128 ymm", "vpalignr ymm"}, function()
      local acc, other = i8(0), i8(11, 12, 13, 14, 15, 16, 17, 18)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc+a, other, 1,2,3,4,5,6,7,8)
      end
      return acc
    end)
    local am = mnemonics(body)
    check(count(am, "perm2i128") == 1 and count(am, "palignr") == 1 and
	  not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "a full aligned window must be VPERM2I128 plus VPALIGNR")

    body = checkymm("i32x8 high full aligned window",
      {"vperm2i128 ymm", "vpalignr ymm"}, function()
      local acc, other = i8(0), i8(11, 12, 13, 14, 15, 16, 17, 18)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc+a, other, 5,6,7,8,9,10,11,12)
      end
      return acc
    end)
    am = mnemonics(body)
    check(count(am, "perm2i128") == 1 and count(am, "palignr") == 1 and
	  not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "a high full aligned window must be VPERM2I128 plus VPALIGNR")

    body = checkymm("i32x8 shuffle2 one source", {"vpermd ymm"}, function()
      local acc = i8(0)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + a, a, 7, 6, 5, 4, 3, 2, 1, 0)
      end
      return acc
    end)
    check(not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "a one-source shuffle2 must reuse the direct permute lowering")

    body = checkymm("i32x8 shuffle2 equal source", {"vpermd ymm"}, function()
      local acc = i8(0)
      for _ = 1, 400 do
	local x = acc + a
	acc = simd.shuffle2(x, x, 7, 14, 5, 12, 3, 10, 1, 8)
      end
      return acc
    end)
    check(not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "equal shuffle2 sources must collapse before routing")

    local b = i8(11, 12, 13, 14, 15, 16, 17, 18)
    body = checkymm("i32x8 direct shuffle2", {"vshufps ymm"}, function()
      local acc = i8(0)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + a, b, 3, 0, 10, 9, 7, 4, 14, 13)
      end
      return acc
    end)
    check(not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "a direct dword shuffle2 must use VSHUFPS")

    body = checkymm("i32x8 half concatenate", {"vperm2i128 ymm"}, function()
      local acc = i8(0)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + a, b, 4, 5, 6, 7, 8, 9, 10, 11)
      end
      return acc
    end)
    check(not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "whole 128-bit source halves must use VPERM2I128")

    body = checkymm("i64x4 direct shuffle2", {"vshufpd ymm"}, function()
      local q = T.W.i64x4.ct
      local acc, add, other = q(0), q(1, 2, 3, 4), q(9, 8, 7, 6)
      for _ = 1, 400 do
	acc = simd.shuffle2(acc + add, other, 1, 4, 2, 7)
      end
      return acc
    end)
    check(not body:find("vpshufb ymm", 1, true) and
	  not body:find("vpor ymm", 1, true),
	  "a direct qword shuffle2 must use VSHUFPD")

    checkymm("i32x8 shuffle2",
      {"vperm2i128 ymm", "vpshufb ymm", "vpor ymm"},
      function()
	local acc = i8(0)
	for _ = 1, 400 do
	  acc = simd.shuffle2(acc + a, b, 7, 12, 5, 14, 3, 8, 1, 10)
	end
	return acc
      end)

    checkymm("i32x8 insert",
      {"vpbroadcastd ymm", "vpcmpeqd ymm", "vpand ymm", "vpandn ymm"},
      function()
	local acc = i8(0)
	for i = 1, 400 do acc = simd.insert(acc + a, i % 8, 42) end
	return acc
      end)

    local f8 = T.W.float8.ct
    local fv = f8(1, 2, 3, 4, 5, 6, 7, 8)
    checkymm("float8 hsum",
      {"vperm2i128 ymm", "vaddps ymm", "vpsrldq ymm"},
      function()
	local v, sum = f8(0), 0
	for _ = 1, 400 do
	  v = v + fv
	  sum = sum + simd.hsum(v)
	end
	return v, sum
      end)

    local b32 = T.W.i8x32.ct
    local bv = b32(1)
    local body = checkymm("i8x32 hsum",
      {"vpsadbw ymm", "vperm2i128 ymm", "vpaddq ymm", "vpsrldq ymm"},
      function()
	local v, sum = b32(0), 0
	for _ = 1, 400 do
	  v = v + bv
	  sum = sum + tonumber(simd.hsum(v))
	end
	return v, sum
      end)
    check(count(mnemonics(body), "paddb") == 1,
	  "i8x32 hsum must reduce qword partial sums, not every byte lane")

    for _, spec in ipairs({
      {"i16x16", T.W.i16x16.ct,
	{-32768,32767,-12345,23456,-30000,11111,-22222,9999,
	 -7777,6666,-5555,4444,-3333,2222,-1111,123}},
      {"u16x16", T.W.u16x16.ct,
	{65535,32768,12345,54321,60000,11111,44444,9999,
	 7777,6666,5555,4444,3333,2222,1111,123}},
    }) do
      local name, ct = spec[1], spec[2]
      local a = ct(unpack(spec[3], 1, 16))
      local k = ct(3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59)
      body = checkymm(name .. " hsum product",
	{"vpmaddwd ymm", "vperm2i128 ymm", "vpaddd ymm",
	 "vpsrldq ymm"}, function()
	  local v, sum = ct(0), 0
	  for _ = 1, 400 do
	    v = v + a
	    sum = sum + tonumber(simd.hsum(v * k))
	  end
	  return v, sum
	end)
      local dm = mnemonics(body)
      check(count(dm, "pmaddwd") == 1 and count(dm, "paddd") == 3 and
	    count(dm, "pmullw") == 0,
	    name .. " hsum product must use one pair dot and dword reduction")
    end

    local dotct = T.W.i16x16.ct
    local dotarr = ffi.new(ffi.typeof("$[?]", dotct), 512)
    local dotk = dotct(3,-5,7,-11,13,-17,19,-23,
		       29,-31,37,-41,43,-47,53,-59)
    body = loopcode(function()
      local s = 0
      for i = 0, 399 do s = s + tonumber(simd.hsum(dotarr[i] * dotk)) end
      return s
    end)
    check(body:match("vpmaddwd ymm[^\n]*%[") ~= nil,
	  "YMM word dot must fuse one array operand: " ..
	  body:gsub("\n", " | "))

    local w16 = T.W.u16x16.ct
    local wv = w16(1, 3, 5, 7, 11, 13, 17, 19,
		   23, 29, 31, 37, 41, 43, 47, 53)
    body = checkymm("u16x16 hmin",
      {"vperm2i128 ymm", "vpminuw ymm", "vphminposuw xmm"},
      function()
	local v, lo = w16(0), 0
	for _ = 1, 400 do
	  v = v + wv
	  lo = lo + tonumber(simd.hmin(v))
	end
	return v, lo
      end)
    check(not body:find("vpsrldq ymm", 1, true),
	  "u16x16 hmin must collapse its low half with VPHMINPOSUW")

    body = checkymm("u16x16 hmax",
      {"vpxor ymm", "vperm2i128 ymm", "vpminuw ymm",
       "vphminposuw xmm"},
      function()
	local v, hi = w16(0), 0
	for _ = 1, 400 do
	  v = v + wv
	  hi = hi + tonumber(simd.hmax(v))
	end
	return v, hi
      end)
    check(not body:find("vpmaxuw ymm", 1, true) and
	  not body:find("vpsrldq ymm", 1, true),
	  "u16x16 hmax must use the complemented horizontal minimum")

    local sw16 = T.W.i16x16.ct
    local swv = sw16(-1, 3, -5, 7, -11, 13, -17, 19,
		     -23, 29, -31, 37, -41, 43, -47, 53)
    for _, op in ipairs({"hmin", "hmax"}) do
      body = checkymm("i16x16 " .. op,
	{"vpxor ymm", "vperm2i128 ymm", "vpminuw ymm",
	 "vphminposuw xmm"},
	function()
	  local v, result = sw16(0), 0
	  for _ = 1, 400 do
	    v = v + swv
	    result = result + tonumber(simd[op](v))
	  end
	  return v, result
	end)
      check(not body:find("vpminsw ymm", 1, true) and
	    not body:find("vpmaxsw ymm", 1, true) and
	    not body:find("vpsrldq ymm", 1, true),
	    "i16x16 " .. op .. " must use the biased horizontal minimum")
    end

    for _, spec in ipairs({
      {"u8x32", T.W.u8x32.ct, false},
      {"i8x32", T.W.i8x32.ct, true},
    }) do
      local name, ct, signed = spec[1], spec[2], spec[3]
      local add = ct(1)
      for _, op in ipairs({"hmin", "hmax"}) do
	local want = {"vpunpcklbw ymm", "vpunpckhbw ymm",
		      "vpminuw ymm", "vperm2i128 ymm",
		      "vphminposuw xmm"}
	if signed or op == "hmax" then want[#want+1] = "vpxor ymm" end
	body = checkymm(name .. " " .. op, want, function()
	  local v, result = ct(0), 0
	  for _ = 1, 400 do
	    v = v + add
	    result = result + tonumber(simd[op](v))
	  end
	  return v, result
	end)
	check(not body:find("vpminub ymm", 1, true) and
	      not body:find("vpmaxub ymm", 1, true) and
	      not body:find("vpminsb ymm", 1, true) and
	      not body:find("vpmaxsb ymm", 1, true) and
	      not body:find("vpsrldq ymm", 1, true),
	      name .. " " .. op .. " must use widened word reduction")
      end
    end
  end)
end

return T
