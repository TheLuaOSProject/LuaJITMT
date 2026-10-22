-- Native AArch64 SIMD regression tests. A matching interpreter result alone
-- is not enough: each native case must emit the requested NEON instructions
-- in a compiled loop. Inspect instruction words so these tests do not depend
-- on jit.dis_arm64 knowing every Advanced SIMD mnemonic.
local dir = arg[0]:match("^(.*)[/\\][^/\\]*$") or "."
package.path = dir .. "/?.lua;" .. package.path
local T = require("simdtest")
local ffi, simd, test, check, checkeq = T.ffi, T.simd, T.test, T.check, T.checkeq
local jit_, util, bit = require("jit"), require("jit.util"), require("bit")

if jit_.arch ~= "arm64" then
  io.write("   AArch64 machine-code assertions skipped on ", jit_.arch, "\n")
  return T
end
if not ffi.abi("le") then
  io.write("   AArch64 machine-code assertions skipped: little-endian target required\n")
  return T
end
if not pcall(jit_.on) then
  io.write("   AArch64 machine-code assertions skipped: JIT disabled by build\n")
  return T
end

local function op(name, value, mask)
  return {name = name, value = value, mask = mask or 0xffe0fc00}
end
local FADD = op("FADD .4S", 0x4e20d400)
local DADD = op("FADD .2D", 0x4e60d400)
local FMUL = op("FMUL .4S", 0x6e20dc00)
local FMLA = op("FMLA .4S", 0x4e20cc00)
local DMLA = op("FMLA .2D", 0x4e60cc00)
local IADD = op("ADD .4S", 0x4ea08400)
local IMUL = op("MUL .4S", 0x4ea09c00)
local SHL = op("SHL immediate", 0x4f005400, 0xff80fc00)
local SSHR = op("SSHR immediate", 0x4f000400, 0xff80fc00)
local INS = op("INS vector lane", 0x6e000400, 0xffe08400)
local FCVTZS = op("FCVTZS .4S", 0x4ea1b800, 0xfffffc00)
local FCMGT = op("FCMGT .4S", 0x6ea0e400)
local QLOAD = op("LDR Q spill", 0x3dc003e0, 0xffc003e0)
local QSTORE = op("STR Q spill", 0x3d8003e0, 0xffc003e0)
local QLOADU = op("LDUR Q spill", 0x3cc003e0, 0xffe00fe0)
local QSTOREU = op("STUR Q spill", 0x3c8003e0, 0xffe00fe0)
local QFAR = op("LDR Q via full literal address", 0x3dc003c0, 0xffc003e0)
local BL = op("BL", 0x94000000, 0xfc000000)
local BLR = op("BLR", 0xd63f0000, 0xfffffc1f)

local function words(code, offset)
  local t = {}
  for i = (offset or 0) + 1, #code - 3, 4 do
    local a, b, c, d = code:byte(i, i+3)
    t[#t+1] = bit.bor(a, bit.lshift(b, 8), bit.lshift(c, 16), bit.lshift(d, 24))
  end
  return t
end

local function count(code, pattern)
  local n = 0
  for _, w in ipairs(code) do
    if bit.band(w, pattern.mask) == bit.tobit(pattern.value) then n = n + 1 end
  end
  return n
end

-- Run a fresh trace, compare every returned byte, and require actual native
-- code. warmargs lets a final call deliberately leave an already hot trace.
local function native(name, want, fn, args, opts)
  opts = opts or {}
  jit_.off()
  jit_.flush()
  collectgarbage()
  local ref = {fn(unpack(args))}
  local finished, parents, aborts = {}, {}, {}
  local function event(what, tr, _, _, parent, exitno)
    if what == "start" then parents[tr] = exitno ~= -1 and parent or nil
    elseif what == "stop" then finished[tr] = true
    elseif what == "abort" then aborts[#aborts+1] = tostring(parent) .. "/" .. tostring(exitno) end
  end
  jit_.attach(event, "trace")
  jit_.opt.start("hotloop=3", "hotexit=2")
  jit_.on()
  local got
  local ok, err = pcall(function()
    for _ = 1, 3 do fn(unpack(opts.warmargs or args)) end
    got = {fn(unpack(args))}
  end)
  jit_.off()
  jit_.attach(event)
  if not check(ok, name .. ": " .. tostring(err)) then return end
  check(#got == #ref, name .. ": result count")
  for i = 1, #ref do checkeq(got[i], ref[i], name .. " result " .. i) end
  local loops, all, side = {}, {}, 0
  for tr in pairs(finished) do
    local mc, _, loop = util.tracemc(tr)
    if mc then
      for _, w in ipairs(words(mc)) do all[#all+1] = w end
      if loop > 0 then
        for _, w in ipairs(words(mc, loop)) do loops[#loops+1] = w end
      end
      if parents[tr] and parents[tr] > 0 then side = side + 1 end
    end
  end
  if opts.fallback then
    check(#aborts > 0, name .. ": expected an unsupported-width trace abort")
    check(count(loops, FADD) == 0, name .. ": a 256-bit add must not truncate to 128 bits")
    return
  end
  if not check(#loops > 0, name .. ": no native loop; aborts=" .. table.concat(aborts, ",")) then return end
  for _, p in ipairs(want) do
    check(count(loops, p) > 0, name .. ": no " .. p.name .. " in native loop")
  end
  if opts.literal_fallback then
    check(count(all, QFAR) > 0, name .. ": no full-address load for a far vector literal")
  end
  if opts.side then check(side > 0, name .. ": no compiled side trace") end
  if opts.spills then
    check(count(loops, QLOAD) + count(loops, QLOADU) > 0, name .. ": no 128-bit stack reload")
    -- Invariant values may be spilled once in the preamble and reloaded on
    -- each iteration; the spill itself need not occur inside the loop.
    check(count(all, QSTORE) + count(all, QSTOREU) > 0, name .. ": no 128-bit stack spill")
  end
  if opts.call then
    check(count(loops, BL) + count(loops, BLR) > 0, name .. ": no native helper call")
  elseif opts.nocall then
    check(count(loops, BL) + count(loops, BLR) == 0, name .. ": helper call in packed loop")
  end
end

local function shellquote(s) return "'" .. s:gsub("'", "'\\''") .. "'" end

-- A 2-MiB mcode allocation puts the bottom-of-area vector pool outside the
-- signed 19-bit LDR literal range of instructions emitted near its top. Run
-- in a new process so no previously allocated mcode area can mask the bug.
if arg[1] == "--arm64-literal-child" then
  test("vector constants outside the LDR literal range", function()
    local ct = T.T.float4.ct
    native("far constants", {FADD}, function(n)
      local a = ct(.125, -2.25, 3.5, 7.75)
      for _ = 1, n do a = a + ct(1.25, -.75, 3.125, -5.5) end
      return a
    end, {400}, {nocall = true, literal_fallback = true})
  end)
  os.exit(T.run("AArch64 2-MiB mcode literals") and 0 or 1)
end

test("AArch64 native capabilities", function()
  local ft = simd.features()
  checkeq(ft.vecsize, 16, "AArch64 native vector width")
  checkeq(ft.neon, true, "AArch64 exposes NEON")
  checkeq(ft.fma, true, "AArch64 exposes native fused multiply add")
  check(not ft.avx2 and not ft.sse2, "AArch64 must not advertise x86 features")
end)

test("far vector literals in a fresh mcode area", function()
  local command = shellquote(arg[-1] or "luajit") .. " -Osizemcode=2048 " ..
    shellquote(dir .. "/test_arm64_codegen.lua") .. " --arm64-literal-child"
  local status = os.execute(command)
  check(status == 0 or status == true, "isolated far-literal regression failed")
end)

test("NEON disassembly preserves registers and vector widths", function()
  package.path = "src/?.lua;./src/?.lua;" .. package.path
  local dis = require("jit.dis_arm64")
  for _, c in ipairs({
    {0x4e22d420, "fadd v0.4s, v1.4s, v2.4s"},
    {0x6e184441, "ins v1.d[1], v2.d[1]"},
    {0x3dc00841, "ldr q1, [x2, #32]"},
    {0x9c000016, "ldr q22, 0x00001000"},
    {0xad414ff2, "ldp q18, q19, [sp, #32]"},
  }) do
    local line
    local word = ffi.new("uint32_t[1]", c[1])
    local ctx = dis.create(ffi.string(word, 4), 0x1000, function(s) line = s end)
    ctx.hexdump = 0
    ctx:disass()
    line = line:gsub("^%x+%s+", ""):gsub("%s+", " "):gsub("%s+$", "")
    checkeq(line, c[2], "decode " .. bit.tohex(c[1]))
  end
end)

test("packed arithmetic and full-width PHIs", function()
  local f4, d2, i4 = T.T.float4.ct, T.T.double2.ct, T.T.i32x4.ct
  native("float4", {FADD, FMUL}, function(n)
    local a, b = f4(1, 2, 3, 4), f4(4, 3, 2, 1)
    for _ = 1, n do a, b = b + f4(.5, 1, 1.5, 2), a * f4(-1) end
    return a, b
  end, {400}, {nocall = true})
  native("double2", {DADD}, function(n)
    local a = d2(1, -2)
    for _ = 1, n do a = a + d2(.5, -1.25) end
    return a
  end, {400}, {nocall = true})
  native("i32x4 multiply", {IADD, IMUL}, function(n)
    local a = i4(1, 0x7fffffff, -3, 0x12345678)
    for _ = 1, n do a = (a + i4(1, 3, 5, 7)) * i4(3, 5, 7, 9) end
    return a
  end, {400}, {nocall = true})
  for _, c in ipairs({{"i8x16", 0x4e208400}, {"i16x8", 0x4e608400},
                     {"i64x2", 0x4ee08400}}) do
    local ct = T.T[c[1]].ct
    native(c[1] .. " add", {op("ADD " .. c[1], c[2])}, function(n)
      local a = ct(1)
      for i = 1, n do a = a + ct(i) end
      return a
    end, {400}, {nocall = true})
  end
end)

test("fused multiply add", function()
  for _, c in ipairs({{T.T.float4.ct, FMLA}, {T.T.double2.ct, DMLA}}) do
    local ct = c[1]
    native(c[2].name, {c[2]}, function(n)
      local a = ct(1)
      for _ = 1, n do a = simd.fma(a, ct(.5), ct(.25)) end
      return a
    end, {400}, {nocall = true})
  end
end)

test("constant shifts and shuffles", function()
  local ct = T.T.i32x4.ct
  native("shifts", {IADD, SHL, SSHR}, function(n)
    local a = ct(0x7fffffff, -23, 0x12345678, -3001)
    for _ = 1, n do a = simd.sar(simd.shl(a + ct(1, 3, 5, 7), 3), 2) end
    return a
  end, {400}, {nocall = true})
  native("shuffle", {IADD, INS}, function(n)
    local a = ct(1, 3, 7, 15)
    for _ = 1, n do a = simd.shuffle(a + ct(1, 2, 4, 8), 3, 1, 0, 2) end
    return a
  end, {401}, {nocall = true})
end)

test("guard exit restores the upper 64 bits", function()
  local ct = T.T.i32x4.ct
  native("guarded sunk cdata", {IADD}, function(n, stop)
    local a = ct(1, -2, 0x12345678, -0x23456789)
    for i = 1, n do
      a = a + ct(1, 3, 5, 7)
      if i == stop then return a, a[2], a[3] end
    end
    return a, a[2], a[3]
  end, {400, 211}, {warmargs = {400, 0}, nocall = true})
end)

test("side traces restore and exchange vector PHIs", function()
  local ct = T.T.i32x4.ct
  native("side trace", {IADD}, function(n)
    local a, b = ct(1, 3, 5, 7), ct(11, 13, 17, 19)
    local hits = 0
    for i = 1, n do
      a = a + ct(1, 2, 3, 4)
      if i % 17 == 0 then a, b = b, a; hits = hits + 1 end
    end
    return a, b, hits
  end, {600}, {side = true})
end)

-- Accumulators plus distinct invariant input vectors exceed the FP register
-- file. Keep the accumulator count below the general 64-PHI limit (both the
-- cdata box and vector payload need PHIs). Require actual Q spills below.
-- Every returned accumulator has different upper lanes, so losing one cannot
-- be hidden by checking lane zero or by summing cancelling values.
local function pressured(call)
  -- Calls make additional spills, so stay within the 256-slot trace limit.
  local nvec = call and 14 or 20
  local lines, names = {"return function(ct, data, call) return function(n)"}, {}
  for i = 1, nvec do
    names[i] = "a" .. i
    lines[#lines+1] = "local a" .. i .. " = data[" .. (i-1) .. "]"
  end
  lines[#lines+1] = "local total = 0; for i = 1, n do"
  for i = 1, nvec do
    lines[#lines+1] = "a" .. i .. " = a" .. i .. " + data[" .. (i-1) .. "]"
  end
  if call then lines[#lines+1] = "total = total + call(i)" end
  lines[#lines+1] = "end; return " .. table.concat(names, ",") .. ", total end end"
  local ct = T.T.float4.ct
  local data = ffi.new("float4[?]", nvec)
  for i = 0, nvec-1 do data[i] = ct(i+1, -i-2, 3*i+101, -7*i-503) end
  return assert(loadstring(table.concat(lines, "\n")))()(ct, data, call)
end

test("128-bit register spills", function()
  native("register pressure", {FADD}, pressured(), {100}, {spills = true})
end)

test("vector values survive GC calls", function()
  local ct = T.T.float4.ct
  local pause = collectgarbage("setpause", 100)
  local step = collectgarbage("setstepmul", 1000)
  native("GC pressure", {FADD}, function(n)
    local a, keep = ct(1, -2, 1001, -2003), {}
    for i = 1, n do
      a = a + ct(1, -3, 7, -11)
      keep[(i % 8)+1] = {i, tostring(i)}
    end
    return a, keep[(n % 8)+1][1]
  end, {4000})
  collectgarbage("setpause", pause)
  collectgarbage("setstepmul", step)
end)

test("C calls clobber the upper halves of v8-v15", function()
  local base = os.tmpname()
  os.remove(base)
  local source, library = base .. ".c", base .. ".so"
  local instructions, clobbers = {}, {}
  for i = 0, 31 do
    instructions[#instructions+1] = '"movi v' .. i .. '.16b, #165\\n"'
    clobbers[#clobbers+1] = '"v' .. i .. '"'
  end
  local f = assert(io.open(source, "w"))
  f:write("int simd_arm64_clobber(int x) { __asm__ volatile (",
    table.concat(instructions, "\n"), " ::: ", table.concat(clobbers, ","),
    "); return x + 1; }\n")
  f:close()
  -- The compiler preserves the ABI-required low halves of v8-v15. Their
  -- upper halves and all caller-saved vectors are deliberately overwritten.
  local status = os.execute((os.getenv("CC") or "cc") .. " -O2 -shared -fPIC " ..
    shellquote(source) .. " -o " .. shellquote(library))
  os.remove(source)
  if not check(status == 0 or status == true, "could not build AArch64 clobber helper") then
    os.remove(library)
    return
  end
  ffi.cdef("int simd_arm64_clobber(int);")
  local C = ffi.load(library)
  native("C clobber", {FADD}, pressured(C.simd_arm64_clobber), {80},
    {spills = true, call = true})
  os.remove(library)
end)

test("min and max preserve x86 NaN and equal-lane selection", function()
  local f4, u4 = T.T.float4.ct, T.T.u32x4.ct
  local data = ffi.new("float4[4]")
  data[0] = simd.bitcast(f4, u4(0x7fc12345, 0x80000000, 0x3f800000, 0xffc54321))
  data[1] = f4(1, 0, 1, -2)
  data[2] = f4(-1, -0.0, math.huge, -math.huge)
  data[3] = simd.bitcast(f4, u4(0xffc23456, 0, 0xbf800000, 0x7fc34567))
  local out = ffi.new("float4[8]")
  native("min/max", {FCMGT}, function(n)
    for i = 1, n do
      local k = i % 4
      out[k] = simd.min(data[k], data[(k+1)%4])
      out[k+4] = simd.max(data[k], data[(k+1)%4])
    end
    return out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]
  end, {400}, {nocall = true})
end)

test("float conversion uses indefinite integer on invalid lanes", function()
  local f4, i4 = T.T.float4.ct, T.T.i32x4.ct
  local data = ffi.new("float4[4]", {
    f4(0/0, math.huge, -math.huge, 2147483648),
    f4(-2147483648, -2147483904, 3.75, -3.75),
    f4(2147483520, 0, -0.0, 1.5), f4(-1.5, 1e30, -1e30, 0/0)})
  local out = ffi.new("i32x4[4]")
  native("f32 to i32", {FCVTZS}, function(n)
    for i = 1, n do out[i%4] = simd.convert(i4, data[i%4]) end
    return out[0], out[1], out[2], out[3]
  end, {400}, {nocall = true})
end)

test("256-bit interpreter fallback retains every lane", function()
  local ct = T.W.float8.ct
  native("float8 fallback", {}, function(n)
    local a = ct(1, 2, 3, 4, 1001, 2002, 3003, 4004)
    for _ = 1, n do a = a + ct(1, 3, 5, 7, 11, 13, 17, 19) end
    return a
  end, {400}, {fallback = true})
end)

return T
