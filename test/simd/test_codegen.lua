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
  if m then check(count(m, "imul") == 0, "i8x16 mul: no scalar IMUL") end
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

test("mulhi is a single packed multiply", function()
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
end)

test("shuffle and insert are packed", function()
  local i4 = T.T.i32x4.ct
  local a = i4(1, 2, 3, 4)
  checkloop("shuffle", {"pshufb"}, NOCALL, function()
    local acc = i4(0)
    for _ = 1, 400 do acc = simd.shuffle(acc + a, 3, 2, 1, 0) end
    return acc
  end)
  checkloop("shuffle2", {"pshufb", "por"}, NOCALL, function()
    local acc = i4(0)
    local b = i4(9, 8, 7, 6)
    for _ = 1, 400 do acc = simd.shuffle2(acc + a, b, 0, 4, 1, 5) end
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
    check(body:find("vmovups ymm", 1, true) ~= nil,
	  "float8 load must move 32 bytes: " .. body:gsub("\n", " | "))

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

    body = checkymm("i32x8 same-half shuffle", {"vpshufb ymm"}, function()
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

    body = checkymm("i64x4 same-half shuffle", {"vpshufb ymm"}, function()
      local acc = q4(0)
      for _ = 1, 400 do acc = simd.shuffle(acc + q, 1, 0, 3, 2) end
      return acc
    end)
    check(not body:find("permq ymm", 1, true) and
	  not body:find("vperm2i128 ymm", 1, true),
	  "a same-half 64 bit constant must use one lane-local shuffle")

    local b = i8(11, 12, 13, 14, 15, 16, 17, 18)
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
  end)
end

return T
