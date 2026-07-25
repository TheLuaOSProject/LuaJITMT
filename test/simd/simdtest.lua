-- Shared helpers for the SIMD test suite.
local ffi = require("ffi")
local simd = require("ffi.simd")
local bit = require("bit")

local M = {}

ffi.cdef[[
typedef float	  float4  __attribute__((vector_size(16)));
typedef double	  double2 __attribute__((vector_size(16)));
typedef int8_t	  i8x16   __attribute__((vector_size(16)));
typedef uint8_t	  u8x16   __attribute__((vector_size(16)));
typedef int16_t	  i16x8   __attribute__((vector_size(16)));
typedef uint16_t  u16x8   __attribute__((vector_size(16)));
typedef int32_t	  i32x4   __attribute__((vector_size(16)));
typedef uint32_t  u32x4   __attribute__((vector_size(16)));
typedef int64_t	  i64x2   __attribute__((vector_size(16)));
typedef uint64_t  u64x2   __attribute__((vector_size(16)));
typedef float	  float2  __attribute__((vector_size(8)));
typedef float	  float8  __attribute__((vector_size(32)));
]]

-- name -> { ct = ctype, lanes = n, fp = bool, signed = bool, bits = n }
M.T = {}
local function deftype(name, lanes, fp, signed, bits)
  local ct = ffi.typeof(name)
  M.T[name] = { name = name, ct = ct, lanes = lanes, fp = fp,
		signed = signed, bits = bits }
  M.T[#M.T+1] = M.T[name]
end

deftype("float4", 4, true, true, 32)
deftype("double2", 2, true, true, 64)
deftype("i8x16", 16, false, true, 8)
deftype("u8x16", 16, false, false, 8)
deftype("i16x8", 8, false, true, 16)
deftype("u16x8", 8, false, false, 16)
deftype("i32x4", 4, false, true, 32)
deftype("u32x4", 4, false, false, 32)
deftype("i64x2", 2, false, true, 64)
deftype("u64x2", 2, false, false, 64)

for _, ti in ipairs(M.T) do ti.ect = simd.elementtype(ti.ct) end

M.simd = simd
M.ffi = ffi

-- Scalar reference implementations ------------------------------------------

-- Widen an integer lane value so that the operation itself cannot lose bits.
local function wide(ti, x)
  if ti.bits == 64 then return x end  -- already an int64_t/uint64_t cdata
  return ffi.cast("int64_t", x)
end

local binfn = {
  add = function(x, y) return x + y end,
  sub = function(x, y) return x - y end,
  mul = function(x, y) return x * y end,
  div = function(x, y) return x / y end,
}

-- Lane-wise reference for an arithmetic op, returns a vector of type ti.
function M.refbin(ti, op, a, b)
  local t = {}
  local f = binfn[op]
  for i = 1, ti.lanes do
    local x, y = a[i-1], b[i-1]
    if op == "min" then
      -- Compare the lane values directly: they already carry the right
      -- signedness, and no widening cast is needed (or wanted, see notes).
      if x < y then t[i] = x else t[i] = y end
    elseif op == "max" then
      if x > y then t[i] = x else t[i] = y end
    elseif ti.fp then
      -- double arithmetic then a single rounding to float is exact for
      -- + - * / and sqrt, because 53 >= 2*24+2.
      t[i] = ffi.cast(ti.ect, f(x, y))
    else
      t[i] = ffi.cast(ti.ect, f(wide(ti, x), wide(ti, y)))
    end
  end
  return M.vec(ti, t)
end

function M.refneg(ti, a)
  local t = {}
  for i = 1, ti.lanes do
    local x = a[i-1]
    t[i] = ti.fp and ffi.cast(ti.ect, -x) or ffi.cast(ti.ect, -wide(ti, x))
  end
  return M.vec(ti, t)
end

-- Lane-wise reference for a comparison, returns the all-ones/zero mask.
function M.refcmp(ti, op, a, b)
  local mt = M.T[(ti.bits == 8 and "i8x16") or (ti.bits == 16 and "i16x8") or
		 (ti.bits == 32 and "i32x4") or "i64x2"]
  local t = {}
  for i = 1, ti.lanes do
    local x, y, r = a[i-1], b[i-1]
    if op == "eq" then r = (x == y)
    elseif op == "ne" then r = not (x == y)
    elseif op == "lt" then r = (x < y)
    elseif op == "le" then r = (x <= y)
    elseif op == "gt" then r = (x > y)
    else r = (x >= y) end
    t[i] = r and -1 or 0
  end
  return M.vec(mt, t)
end

-- Deterministic xorshift PRNG, so failures are reproducible from the seed.
function M.rng(seed)
  local s = bit.bxor(seed * 2654435761, 0x9e3779b9)
  if s == 0 then s = 1 end
  return function()
    s = bit.bxor(s, bit.lshift(s, 13))
    s = bit.bxor(s, bit.rshift(s, 17))
    s = bit.bxor(s, bit.lshift(s, 5))
    return s  -- full 32-bit signed range
  end
end

-- Render a vector as a stable string, bit-exact for floats.
function M.tostr(v)
  local n = simd.lanes(v)
  local t = {}
  for i = 0, n-1 do
    local x = v[i]
    if type(x) == "number" then
      if x ~= x then t[i+1] = "nan"
      elseif x == math.huge then t[i+1] = "inf"
      elseif x == -math.huge then t[i+1] = "-inf"
      else t[i+1] = string.format("%.17g", x) end
    else
      t[i+1] = tostring(x)
    end
  end
  return "{" .. table.concat(t, ",") .. "}"
end

-- Bit-exact vector comparison (so NaN and -0 mismatches are caught).
local boxcache = {}
local sbuf1, sbuf2 = ffi.new("uint8_t[64]"), ffi.new("uint8_t[64]")
function M.same(a, b)
  local ta, tb = tostring(ffi.typeof(a)), tostring(ffi.typeof(b))
  if ta ~= tb then return false end
  local bt = boxcache[ta]
  if not bt then bt = ffi.typeof("$[1]", ffi.typeof(a)); boxcache[ta] = bt end
  local n = ffi.sizeof(a)
  local x, y = bt(), bt()
  x[0] = a; y[0] = b
  ffi.copy(sbuf1, x, n)
  ffi.copy(sbuf2, y, n)
  for i = 0, n-1 do if sbuf1[i] ~= sbuf2[i] then return false end end
  return true
end

-- Build a vector from a table of lane values.
function M.vec(ti, t)
  return ti.ct(unpack(t, 1, ti.lanes))
end

-- Random lane values covering interesting corners.
function M.randlanes(ti, rnd)
  local t = {}
  for i = 1, ti.lanes do
    local r = rnd()
    local pick = bit.band(r, 7)
    if ti.fp then
      if pick == 0 then t[i] = 0
      elseif pick == 1 then t[i] = -0.0
      elseif pick == 2 then t[i] = math.huge
      elseif pick == 3 then t[i] = -math.huge
      elseif pick == 4 then t[i] = 0/0
      elseif pick == 5 then t[i] = 1
      else t[i] = rnd() / 65536.0 end
    else
      if pick == 0 then t[i] = 0
      elseif pick == 1 then t[i] = -1
      elseif pick == 2 then t[i] = 1
      elseif ti.bits == 64 then
	t[i] = ffi.cast(ti.signed and "int64_t" or "uint64_t", r) * 4294967296LL +
	       ffi.cast(ti.signed and "int64_t" or "uint64_t", rnd())
      else
	t[i] = r
      end
    end
  end
  return t
end

function M.rand(ti, rnd) return M.vec(ti, M.randlanes(ti, rnd)) end

-- Random vector without NaN lanes.
function M.randfinite(ti, rnd)
  local t = M.randlanes(ti, rnd)
  for i = 1, ti.lanes do if t[i] ~= t[i] then t[i] = i end end
  return M.vec(ti, t)
end

-- A lane value in the same representation that v[i] produces.
function M.tolane(ti, x)
  local c = ffi.cast(ti.ect, x)
  if ti.bits == 64 and not ti.fp then return c end
  return tonumber(c)
end

-- Reference reduction: the same pairwise halving tree the VM uses.
function M.refreduce(ti, op, a)
  local t = {}
  for i = 0, ti.lanes-1 do t[i] = a[i] end
  local n = ti.lanes
  local wt = ti.bits == 64 and (ti.signed and "int64_t" or "uint64_t") or "int64_t"
  while n > 1 do
    n = n / 2
    for i = 0, n-1 do
      local x, y = t[i], t[i+n]
      if op == "sum" then
	if ti.fp then t[i] = M.tolane(ti, x + y)
	else t[i] = M.tolane(ti, ffi.cast(wt, x) + ffi.cast(wt, y)) end
      elseif op == "min" then t[i] = (x < y) and x or y
      else t[i] = (x > y) and x or y end
    end
  end
  return t[0]
end

-- Test registry -------------------------------------------------------------

local tests, failures, ntests = {}, {}, 0

function M.test(name, fn) tests[#tests+1] = { name = name, fn = fn } end

local ctx = ""
function M.ctx(s) ctx = s end

function M.check(cond, msg)
  ntests = ntests + 1
  if not cond then
    failures[#failures+1] = (ctx ~= "" and (ctx .. ": ") or "") .. tostring(msg)
    if #failures > 40 then error("too many failures", 0) end
  end
  return cond
end

function M.checkeq(got, want, msg)
  local ok
  if type(got) == "cdata" and type(want) == "cdata" then
    ok = M.same(got, want)
    if not ok then
      msg = tostring(msg) .. " got=" .. M.tostr(got) .. " want=" .. M.tostr(want)
    end
  else
    ok = (got == want) or (got ~= got and want ~= want)
    if not ok then
      msg = tostring(msg) .. " got=" .. tostring(got) .. " want=" .. tostring(want)
    end
  end
  return M.check(ok, msg)
end

function M.run(name)
  local jit_ = require("jit")
  io.write("== ", name, "\n")
  for _, t in ipairs(tests) do
    ctx = t.name
    local ok, err = pcall(t.fn)
    if not ok then
      failures[#failures+1] = t.name .. ": error: " .. tostring(err)
    end
  end
  ctx = ""
  if #failures == 0 then
    io.write(string.format("   %d checks OK\n", ntests))
    return true
  end
  for _, f in ipairs(failures) do io.write("   FAIL ", f, "\n") end
  io.write(string.format("   %d/%d checks failed\n", #failures, ntests))
  return false
end

--[[
Run fn(...) both interpreted and JIT-compiled and require identical results.
`fn` must be a fresh closure per call site so it gets its own trace.
]]
function M.jitsame(mkfn, args, n)
  local jit_ = require("jit")
  n = n or 60
  jit_.off()
  jit_.flush()
  local fi = mkfn()
  local ref = { fi(unpack(args)) }
  jit_.on()
  local fj = mkfn()
  local got
  for _ = 1, n do got = { fj(unpack(args)) } end
  jit_.off()
  return ref, got
end

return M
