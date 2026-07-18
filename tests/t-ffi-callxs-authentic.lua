local bit = require "bit"
local ffi = require "ffi"
local util = require "jit.util"
local vmdef = require "jit.vmdef"

if jit.arch ~= "x64" then
  print("t-ffi-callxs-authentic SKIP: x64-only lowering")
  return
end

ffi.cdef [[
int32_t lj_callxs_auth_add(int32_t, int32_t);
uint32_t lj_callxs_auth_u32(uint32_t, int32_t);
double lj_callxs_auth_mix(int32_t, double, float, uint64_t,
                          const int32_t *, double);
float lj_callxs_auth_float(float, int32_t);
int8_t lj_callxs_auth_i8(int32_t);
uint8_t lj_callxs_auth_u8(int32_t);
int16_t lj_callxs_auth_i16(int32_t);
uint16_t lj_callxs_auth_u16(int32_t);
int32_t lj_callxs_auth_errno(int32_t);
void lj_callxs_auth_store(int32_t *, int32_t, int32_t);
double lj_callxs_auth_vararg(int32_t, ...);
int32_t *lj_callxs_auth_ptr(int32_t *);
_Bool lj_callxs_auth_bool(int32_t);
enum lj_callxs_auth_enum {
  LJ_CALLXS_AUTH_ENUM_ZERO,
  LJ_CALLXS_AUTH_ENUM_SEVEN = 7
};
enum lj_callxs_auth_enum lj_callxs_auth_enum_result(int32_t);
int64_t lj_callxs_auth_i64_result(int32_t);
uint64_t lj_callxs_auth_u64_result(int32_t);
]]

local lib = ffi.load(assert(os.getenv("LJ_M7_FFI_CALLXS_SO")))

local function trace_op_counts()
  local counts = {}
  for tr = 1, 256 do
    local info = util.traceinfo(tr)
    if info then
      for ref = 1, info.nins do
        local _, ot = util.traceir(tr, ref)
        if ot then
          local opidx = bit.rshift(ot, 8)
          local op = vmdef.irnames:sub(opidx * 6 + 1, opidx * 6 + 6)
          counts[op] = (counts[op] or 0) + 1
        end
      end
    end
  end
  return counts
end
jit.off(trace_op_counts, true)

local p = ffi.new("int32_t[4]", { 11, 22, 33, 44 })
local u64 = ffi.new("uint64_t", 259)

local function run(n)
  local si, su, sd, sf, sv, s8, u8, s16, u16, se =
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0
  for i = 1, n do
    si = si + lib.lj_callxs_auth_add(i, 2)
    su = su + lib.lj_callxs_auth_u32(i, 5)
    sd = sd + lib.lj_callxs_auth_mix(i, 1.5, 2.25, u64, p, 4.25)
    sf = sf + lib.lj_callxs_auth_float(1.5, i)
    sv = sv + lib.lj_callxs_auth_vararg(3, i + 0.0, 1.25, 2.5)
    s8 = s8 + lib.lj_callxs_auth_i8(i)
    u8 = u8 + lib.lj_callxs_auth_u8(i)
    s16 = s16 + lib.lj_callxs_auth_i16(i)
    u16 = u16 + lib.lj_callxs_auth_u16(i)
    se = se + lib.lj_callxs_auth_errno(i)
    if ffi.errno() ~= 1000 + bit.band(i, 31) then return false end
    lib.lj_callxs_auth_store(p, i, i + 100)
  end
  return si, su, sd, sf, sv, s8, u8, s16, u16, se
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local n = 120
local si, su, sd, sf, sv, s8, u8, s16, u16, se = run(n)
local triangle = n * (n + 1) / 2
assert(si == triangle + 5 * n)
assert(su == n * 0x80000000 + triangle + 5 * n)
-- d&3 is 3 and store() changes p[3] only when i&3 == 3, after mix().
local expected_mix = 0
local expected_p = { 11, 22, 33, 44 }
for i = 1, n do
  expected_mix = expected_mix + i + 1.5 + 2.25 + 3 + expected_p[4] + 4.25
  expected_p[bit.band(i, 3) + 1] = i + 100
end
assert(sd == expected_mix)
assert(sf == triangle + 1.75 * n)
assert(sv == triangle + 3.75 * n)
assert(s8 == -101 * n)
assert(u8 == 201 * n)
assert(s16 == -12345 * n)
assert(u16 == 54321 * n)
assert(se == triangle + n)
assert(ffi.errno() == 1000 + bit.band(n, 31))

local counts = trace_op_counts()
assert((counts["XSAVE "] or 0) >= 11, "authentic path omitted XSAVE")
assert((counts["CALLXS"] or 0) >= 11, "authentic path omitted CALLXS")

-- Boxed pointer results deliberately retain the interpreted fallback until
-- pre-rooted result storage and forced-unwind coverage land. This is a return-
-- class boundary, not a declaration/signature matcher.
jit.flush()
local function boxed(nbox)
  local q, b, e, i64, u64
  for i = 1, nbox do
    q = lib.lj_callxs_auth_ptr(p)
    b = lib.lj_callxs_auth_bool(i)
    e = lib.lj_callxs_auth_enum_result(i)
    i64 = lib.lj_callxs_auth_i64_result(i)
    u64 = lib.lj_callxs_auth_u64_result(i)
  end
  return q, b, e, i64, u64
end
local q, b, e, i64, boxed_u64 = boxed(200)
assert(q == p)
assert(b == true)
assert(tonumber(e) == 7)
assert(i64 == ffi.new("int64_t", -123456789))
assert(boxed_u64 == ffi.new("uint64_t", 4000000000))
local boxed_counts = trace_op_counts()
assert((boxed_counts["CALLXS"] or 0) == 0,
       "boxed result crossed the pre-rooting gate")

print("t-ffi-callxs-authentic OK: generic XSAVE/native/CALLXS path executed")
