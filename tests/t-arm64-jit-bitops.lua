-- Runtime coverage for the bounded ARM64 scalar 32-bit bitops tranche.
-- The C harness supplies independent allocation and lifecycle probes.
-- Without it, the same result cases verify the ordinary runtime and trace metadata.
-- Every case is compared with the interpreter before the native-trace check.
local bit = require("bit")
local threading = require("threading")
local ready, release = threading.channel(1), threading.channel(1)
local peer = threading.spawn(function(rdy, rel)
  assert(rdy:send(true, 5))
  assert(select(2, rel:recv(120)))
end, ready, release)
assert(select(2, ready:recv(5)))
local unpack = unpack
local function pack(...)
  return { n = select("#", ...), ... }
end
local function equal_values(a, b)
  assert(a.n == b.n, "result count mismatch: " .. a.n .. " ~= " .. b.n)
  for i = 1, a.n do
    local x, y = a[i], b[i]
    assert(type(x) == type(y), "result type mismatch at " .. i)
    if type(x) == "number" and x ~= x and y ~= y then
      -- Stock LuaJIT's NaN comparison is false on both sides.
    else
      assert(x == y, "result mismatch at " .. i .. ": " .. tostring(x) .. " ~= " .. tostring(y))
    end
  end
end
local function invoke(f, args)
  return pack(f(unpack(args, 1, args.n)))
end
local function run(name, f, required, ...)
  local args = pack(...)
  print("CASE", name)
  jit.flush()
  jit.off(f)
  local expected = invoke(f, args)
  jit.on(f)
  jit.opt.start("hotloop=1", "hotexit=1000", "maxtrace=1", "-fma")
  equal_values(expected, invoke(f, args)) -- Record before measuring re-entry.
  if probe_reset then probe_reset() end
  local actual = invoke(f, args)
  equal_values(expected, actual)
  if probe_check then probe_check(f, required)
  else assert(require("jit.util").traceinfo(1), "native trace unavailable") end
  return expected[1]
end
jit.off(run, true)
jit.off(pack, true); jit.off(invoke, true); jit.off(equal_values, true)

-- Dynamic shift count in an SSA value, exercised at both LOOP and FORL sites.
-- The selected counts cover masked negative, zero, word-boundary and oversized shifts.
local function loop_mix(n, x, y, s)
  local i, acc = 0, 0
  while i < n do
    local q = bit.band(bit.bnot(x), bit.bswap(y))
    local l = bit.lshift(q, s)
    local r = bit.rshift(y, s)
    local a = bit.arshift(x, s)
    local rr = bit.ror(l, s)
    local rl = bit.rol(r, s) -- recorder lowers this through ror and NEG.int.
    local joined = bit.bor(bit.bxor(a, rr), rl)
    local twice = bit.tobit(x + x)
    local widened = bit.tobit(twice + y)
    local narrowed = bit.tobit(widened - 2147483648)
    local fused = bit.band(bit.bor(joined, narrowed), 2147483647)
    acc = bit.tobit(acc + fused)
    x, y = bit.tobit(y + 17), bit.bxor(x, fused)
    i = i + 1
  end
  return x, y, acc
end
local function forl_mix(n, x, y, s)
  local acc = 0
  for i = 1, n do
    local q = bit.band(bit.bnot(x), bit.bswap(y))
    local l = bit.lshift(q, s)
    local r = bit.rshift(y, s)
    local a = bit.arshift(x, s)
    local rr = bit.ror(l, s)
    local rl = bit.rol(r, s)
    local joined = bit.bor(bit.bxor(a, rr), rl)
    local twice = bit.tobit(x + x)
    local widened = bit.tobit(twice + y)
    local narrowed = bit.tobit(widened - 2147483648)
    local fused = bit.band(bit.bor(joined, narrowed), 2147483647)
    acc = bit.tobit(acc + fused)
    x, y = bit.tobit(y + 17), bit.bxor(x, fused)
  end
  return x, y, acc
end
local shifts = { -1, 0, 1, 15, 31, 32, 33, 63 }
for i = 1, #shifts do
  run("loop_dynamic_shift_" .. shifts[i], loop_mix,
      "BNOT,BSWAP,BAND,BOR,BXOR,BSHL,BSHR,BSAR,BROR,NEG", 18, 0x13579bdf, 0x2468ace0, shifts[i])
end
for i = 1, #shifts do
  run("forl_dynamic_shift_" .. shifts[i], forl_mix,
      "BNOT,BSWAP,BAND,BOR,BXOR,BSHL,BSHR,BSAR,BROR,NEG", 18, 0x13579bdf, 0x2468ace0, shifts[i])
end

-- Distinct fusion shapes and mixed INT arithmetic around logical operations.
local function fusion_loop(n, x, y, s, mask)
  local i, acc = 0, 0
  while i < n do
    local inverted = bit.band(bit.bnot(x), mask)
    local shifted = bit.band(bit.lshift(x, s), mask)
    local joined = bit.bor(bit.lshift(inverted, s), bit.rshift(shifted, s))
    local twice = bit.tobit(x + x)
    local added = bit.tobit(twice + y)
    local candidate = bit.band(bit.bor(joined, added), mask)
    local equal_mask = bit.band(candidate, mask) == bit.band(y, mask)
    if equal_mask then acc = bit.tobit(acc + candidate) end
    x = bit.tobit(x + 2147483647)
    y = bit.tobit(y - 2147483648)
    i = i + 1
  end
  return x, y, acc
end
run("fused_logic_and_compare", fusion_loop,
    "BNOT,BAND,BSHL,BSHR,BOR", 24, 0x76543210, 0x13579bdf, 31, 0x00ff00ff)

local function constant_fusion_loop(n, x, mask)
  local i, acc = 0, 0
  while i < n do
    local inverted = bit.band(bit.bnot(x), mask)
    local shifted = bit.band(bit.lshift(x, 31), mask)
    local joined = bit.bor(bit.lshift(inverted, 7), bit.rshift(shifted, 31))
    acc = bit.bxor(acc, joined)
    x = bit.tobit(x + 17)
    i = i + 1
  end
  return x, acc
end
run("constant_shift_logic_fusion", constant_fusion_loop,
    "BNOT,BAND,BSHL,BSHR,BOR,BXOR", 20, 0x81234567, 0x00ff00ff)

local function int_wrap_loop(n, x, y, step)
  local i, acc = 0, 0
  while i < n do
    local added = bit.tobit(x + y)
    local subtracted = bit.tobit(added - step)
    acc = bit.tobit(acc + subtracted)
    x, y = bit.tobit(subtracted + step), bit.tobit(added - x)
    i = i + 1
  end
  return x, y, acc
end
run("int_add_sub_wrap", int_wrap_loop, "ADD,SUB", 22, 0x7ffffffe, 0x70000003, 0x12345)

local function constant_shifts(n, x)
  local i, acc = 0, 0
  while i < n do
    local a = bit.lshift(x, -1)
    local b = bit.rshift(x, 0)
    local c = bit.arshift(x, 31)
    local d = bit.lshift(x, 32)
    local e = bit.rshift(x, 33)
    local f = bit.arshift(x, 63)
    local g = bit.ror(x, 31)
    acc = bit.tobit(bit.bxor(bit.bxor(bit.bxor(a, b), bit.bxor(c, d)), bit.bxor(bit.bxor(e, f), g)))
    x = bit.tobit(x + 1)
    i = i + 1
  end
  return x, acc
end
run("constant_shift_edges", constant_shifts,
    "BSHL,BSHR,BSAR,BROR,BXOR", 20, 0x81234567)

-- Keep many live 32-bit outputs across the loop to exercise register allocation/spills.
local locals, src = {}, { "return function(n,mask,s,bnot,band,bxor,lshift,tobit)", "local i=0" }
for i = 1, 24 do
  locals[i] = "v" .. i
  src[#src + 1] = "local " .. locals[i] .. "=" .. (i * 7919)
end
src[#src + 1] = "while i<n do"
for i = 1, 24 do
  local j = i % 24 + 1
  src[#src + 1] = locals[i] .. "=tobit(bxor(band(bnot(" .. locals[i] .. "),mask),lshift(" .. locals[j] .. ",s))+" .. i .. ")"
end
src[#src + 1] = "i=i+1 end"
src[#src + 1] = "return " .. table.concat(locals, ",")
src[#src + 1] = "end"
local pressure = assert(loadstring(table.concat(src, "\n")))()
run("many_live_integer_results", pressure, "BNOT,BAND,BSHL,BXOR", 20, 0x13579bdf, 31,
    bit.bnot, bit.band, bit.bxor, bit.lshift, bit.tobit)

-- Function arguments keep the body free of table/upvalue helper snapshots,
-- allowing the stock allocator to exercise actual instruction fusions.
local fusion_cases = {
  {"not_or", "x=bor(bnot(x),y); y=bxor(y,17)", 1},
  {"shift_xor", "x=bxor(y,lshift(x,7)); y=tobit(y+3)", 2},
  {"shift_not", "x=bnot(lshift(x,7))", 4},
  {"shift_pair", "x=rshift(lshift(x,7),4)", 8},
  {"signed_shift_pair", "x=arshift(lshift(x,7),4)", 256},
  {"not_xor", "x=bxor(bnot(x),y); y=bxor(y,17)", 512},
  {"shift_mask", "x=band(rshift(x,7),255)", 8},
  {"extract", "x=bor(lshift(x,7),rshift(y,25)); y=tobit(y+3)", 16},
  {"band_compare", "if band(x,mask)==0 then break end; x=bor(lshift(x,1),1)", 32},
  {"shift_add", "x=tobit(x+lshift(y,3)); y=bxor(y,17)", 64},
  {"shift_sub", "x=tobit(x-lshift(y,3)); y=bxor(y,17)", 128},
  {"rotate_xor", "x=bxor(y,ror(x,7)); y=tobit(y+3)", 2},
  {"double_xor", "x=bxor(y,tobit(x+x)); y=tobit(y+3)", 2},
}
for _, case in ipairs(fusion_cases) do
  local source = "return function(n,x,y,mask,bnot,band,bor,bxor,lshift,rshift,arshift,ror,tobit)\n" ..
    "local i=0\nwhile i<n do\n" .. case[2] .. "\ni=i+1 end\nreturn x,y\nend"
  local f = assert(loadstring(source, "bitop-fusion-" .. case[1]))()
  run("allocated_fusion_" .. case[1], f, "", 20, 0x456789ab, 0x12345, 1,
      bit.bnot, bit.band, bit.bor, bit.bxor, bit.lshift, bit.rshift,
      bit.arshift, bit.ror, bit.tobit)
  if probe_fused then probe_fused(case[3]) end
end

-- TOBIT conversion uses the stock interpreter as the oracle at rounding edges,
-- signed zero, large magnitudes, and non-finite inputs where stock defines it.
local function tobit_loop(n, x)
  local i, out = 0, 0
  while i < n do
    out = bit.tobit(x)
    i = i + 1
  end
  return out
end
local tobit_inputs = {
  0.5, 1.5, -0.5, -1.5,
  2147483646.5, 2147483647.5, -2147483647.5, -2147483648.5,
  4294967297.25, -4294967297.25, 0.0, -0.0,
  0/0, math.huge, -math.huge,
}
for i = 1, #tobit_inputs do
  run("tobit_edge_" .. i, tobit_loop, tobit_inputs[i] == 0 and "" or "TOBIT", 12, tobit_inputs[i])
end

-- Replacing a bit-function argument after a native trace must leave the new
-- identity observable and must not keep calling the old fast-function target.
local function identity_loop(n, x, target)
  local i = 0
  while i < n do
    x = target(x)
    i = i + 1
  end
  return x
end
run("function_identity_before_replace", identity_loop, "BNOT", 19, 0x12345678, bit.bnot)
local replacement = bit.bswap
local replace_args = pack(19, 0x12345678, replacement)
local function replacement_reference(n, x, fn)
  local i = 0
  while i < n do x = fn(x); i = i + 1 end
  return x
end
jit.off(replacement_reference, true)
local replaced_expected = replacement_reference(19, 0x12345678, replacement)
if probe_changed then
  probe_changed(identity_loop, replaced_expected, unpack(replace_args, 1, replace_args.n))
else
  assert(identity_loop(unpack(replace_args, 1, replace_args.n)) == replaced_expected)
end
print("CASE", "function_identity_replacement_exit")
local remat_source = [[
local bit = require("bit")
return function(n, x, targets)
  local i = 0
  while i < n do
    x = bit.tobit(targets[i+1](x) + 17)
    i = i + 1
  end
  return x
end
]]
local remat_loop = assert(loadstring(remat_source, "bitop-snapshot-conversion"))()
local remat_reference = assert(loadstring(remat_source, "bitop-snapshot-reference"))()
local function remat_targets(change)
  local t = {}
  for i = 1, 20 do t[i] = (change and i > 6) and bit.bswap or bit.bnot end
  return t
end
jit.off(remat_reference, true); jit.off(remat_targets, true)
local original_targets, changed_targets = remat_targets(false), remat_targets(true)
run("snapshot_conversion_before_exit", remat_loop, "BNOT", 20, 0x81234567, original_targets)
local remat_expected = remat_reference(20, 0x81234567, changed_targets)
if probe_changed_remat then
  probe_changed_remat(remat_loop, remat_expected, 20, 0x81234567, changed_targets)
else
  assert(remat_loop(20, 0x81234567, changed_targets) == remat_expected)
end

local function lifecycle_bitmix(n, x, y, s)
  local i = 0
  while i < n do
    x = bit.bxor(bit.band(bit.bnot(x), bit.bswap(y)), bit.lshift(y, s))
    y = bit.bor(bit.ror(x, s), bit.arshift(y, s))
    i = i + 1
  end
  return x
end
local lifecycle_expected = run("bitops_postadmission_lifecycle", lifecycle_bitmix,
  "BNOT,BSWAP,BAND,BSHL,BXOR,BOR,BROR,BSAR", 24, 0x13579bdf, 0x2468ace0, 7)
if probe_lifecycle then
  probe_lifecycle(lifecycle_bitmix, lifecycle_expected,
    24, 0x13579bdf, 0x2468ace0, 7)
end

local function refused(name, f, ...)
  local args = pack(...)
  jit.flush(); jit.off(f)
  local expected = invoke(f, args)
  jit.on(f)
  if probe_reset then probe_reset() end
  for i = 1, 24 do equal_values(expected, invoke(f, args)) end
  if probe_refused then probe_refused(f)
  else assert(not require("jit.util").traceinfo(1)) end
  print("CASE", "excluded_" .. name)
end
jit.off(refused, true)
local function unsupported_loop(n, x, op)
  local out = 0
  for i = 1, n do out = op(x) end
  return out
end
refused("string_conversion", unsupported_loop, 12, "123", bit.bnot)
refused("cdata_bit64", unsupported_loop, 12, require("ffi").new("int64_t", 123), bit.bswap)
refused("tohex_allocation", unsupported_loop, 12, 123, bit.tohex)
refused("funcf_tailcall", function(x, op) return op(x) end, 123, bit.bnot)
assert(release:send(true, 5)); peer:join()
if not probe_check then print("ordinary bitops results and published traces OK") end
