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
_Bool lj_callxs_auth_bool_raw_bits(int32_t);
_Bool lj_callxs_auth_bool_iter(int32_t, int32_t);
enum lj_callxs_auth_enum {
  LJ_CALLXS_AUTH_ENUM_ZERO,
  LJ_CALLXS_AUTH_ENUM_SEVEN = 7
};
enum lj_callxs_auth_enum lj_callxs_auth_enum_result(int32_t);
int64_t lj_callxs_auth_i64_result(int32_t);
uint64_t lj_callxs_auth_u64_result(int32_t);
typedef uint64_t __attribute__((aligned(8))) lj_callxs_auth_attr_u64_t;
lj_callxs_auth_attr_u64_t lj_callxs_auth_attributed_u64_result(int32_t);
int32_t &lj_callxs_auth_reference_result(void);
struct lj_callxs_auth_aggregate_alpha {
  uint64_t cookie;
  double weight;
  int32_t code;
  uint32_t stamp;
};
struct lj_callxs_auth_aggregate_beta {
  int64_t debt;
  uint32_t stamp;
  float ratio;
  uint64_t token;
};
typedef struct {
  uint64_t lane[3];
} lj_callxs_auth_aggregate_aligned __attribute__((aligned(32)));
struct lj_callxs_auth_aggregate_alpha
  lj_callxs_auth_aggregate_alpha_result(double, int32_t);
struct lj_callxs_auth_aggregate_beta
  lj_callxs_auth_aggregate_beta_result(uint32_t, float);
lj_callxs_auth_aggregate_aligned
  lj_callxs_auth_aggregate_aligned_result(uint64_t);
void lj_callxs_auth_aggregate_reset(void);
uint32_t lj_callxs_auth_aggregate_alpha_count(void);
uint32_t lj_callxs_auth_aggregate_beta_count(void);
uint32_t lj_callxs_auth_aggregate_aligned_count(void);
void lj_callxs_auth_reset(void);
int32_t lj_callxs_auth_count(void);
int32_t lj_callxs_auth_once(int32_t);
int32_t lj_callxs_auth_iter(int32_t, int32_t);
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

local function hasbc(fn, wanted)
  for pc = 1, 1000 do
    local ins = util.funcbc(fn, pc)
    if not ins then return false end
    local op = bit.band(ins, 0xff)
    local name = vmdef.bcnames:sub(op * 6 + 1, op * 6 + 6):gsub("%s+$", "")
    if name == wanted then return true end
  end
  return false
end
jit.off(hasbc, true)

local function produce_one(x)
  return x
end

local function call_multres(x)
  local y = lib.lj_callxs_auth_once(produce_one(x))
  return y
end

local function tail_once(x)
  return lib.lj_callxs_auth_once(x)
end

local function tail_multres(x)
  return lib.lj_callxs_auth_once(produce_one(x))
end

local function run_wrapper(nrun, fn)
  local sum = 0
  for i = 1, nrun do sum = sum + fn(i) end
  return sum
end

local function run_iter(nrun)
  local sum = 0
  for value in lib.lj_callxs_auth_iter, 0, 0 do
    sum = sum + value
    if value == nrun then break end
  end
  return sum
end

assert(hasbc(call_multres, "CALLM"))
assert(hasbc(tail_once, "CALLT"))
assert(hasbc(tail_multres, "CALLMT"))
assert(hasbc(run_iter, "ITERC"))

for _, case in ipairs({
  { "CALLM", call_multres },
  { "CALLT", tail_once },
  { "CALLMT", tail_multres },
}) do
  jit.flush()
  lib.lj_callxs_auth_reset()
  assert(run_wrapper(120, case[2]) == triangle + 9 * n)
  assert(lib.lj_callxs_auth_count() == n)
  assert((trace_op_counts()["CALLXS"] or 0) > 0,
         case[1] .. " frame did not activate production CALLXS")
end

local function ignore_results(nrun)
  for i = 1, nrun do lib.lj_callxs_auth_once(i) end
  return true
end

local function excess_results(nrun)
  local sum = 0
  for i = 1, nrun do
    local value, extra1, extra2 = lib.lj_callxs_auth_once(i)
    if extra1 ~= nil or extra2 ~= nil then return false end
    sum = sum + value
  end
  return sum
end

local function forward_results(...)
  return select("#", ...), ...
end

local function open_results(nrun)
  local sum = 0
  for i = 1, nrun do
    local count, value, extra = forward_results(lib.lj_callxs_auth_once(i))
    if count ~= 1 or extra ~= nil then return false end
    sum = sum + value
  end
  return sum
end

for _, case in ipairs({
  { "ignored", ignore_results, true },
  { "excess fixed", excess_results, triangle + 9 * n },
  { "open", open_results, triangle + 9 * n },
}) do
  jit.flush()
  lib.lj_callxs_auth_reset()
  assert(case[2](n) == case[3], case[1] .. " result semantics changed")
  assert(lib.lj_callxs_auth_count() == n)
  assert((trace_op_counts()["CALLXS"] or 0) > 0,
         case[1] .. " result mode did not activate production CALLXS")
end

local function void_open_results(nrun)
  for i = 1, nrun do
    if select("#", lib.lj_callxs_auth_store(p, i, i + 200)) ~= 0 then
      return false
    end
  end
  return true
end

jit.flush()
assert(void_open_results(n))
assert(p[bit.band(n, 3)] == n + 200)
assert((trace_op_counts()["CALLXS"] or 0) > 0,
       "void open-result mode did not activate production CALLXS")

jit.flush()
lib.lj_callxs_auth_reset()
assert(run_iter(n) == triangle)
assert(lib.lj_callxs_auth_count() == n)
assert((trace_op_counts()["CALLXS"] or 0) > 0,
       "ITERC frame did not activate production CALLXS")

-- Unrelated fixed aggregates exercise the generic sret path. The third type
-- also requires the aligned CNEW/newv allocation path before its payload is
-- passed as the hidden ABI result pointer.
assert(ffi.sizeof("struct lj_callxs_auth_aggregate_alpha") == 24)
assert(ffi.sizeof("struct lj_callxs_auth_aggregate_beta") == 24)
assert(ffi.sizeof("lj_callxs_auth_aggregate_aligned") == 24)
assert(ffi.alignof("lj_callxs_auth_aggregate_aligned") == 32)

local function reset_aggregate_counts()
  lib.lj_callxs_auth_aggregate_reset()
end
jit.off(reset_aggregate_counts, true)

local function assert_aggregate_counts(alpha, beta, aligned)
  assert(lib.lj_callxs_auth_aggregate_alpha_count() == alpha)
  assert(lib.lj_callxs_auth_aggregate_beta_count() == beta)
  assert(lib.lj_callxs_auth_aggregate_aligned_count() == (aligned or 0))
end
jit.off(assert_aggregate_counts, true)

local function assert_aggregate_alpha(value, seed)
  assert(ffi.istype("struct lj_callxs_auth_aggregate_alpha", value))
  assert(value.cookie == 0xfedcba9876540000ULL + seed)
  assert(value.weight == 0.5 + seed * 0.5)
  assert(value.code == seed * 3 - 17)
  assert(value.stamp == 0x13570000 + seed)
end

local function assert_aggregate_beta(value, seed)
  assert(ffi.istype("struct lj_callxs_auth_aggregate_beta", value))
  assert(value.debt == -0x0123456789abcdefLL)
  assert(value.stamp == 0xa5a50000 + seed)
  assert(value.ratio == 1.25 + seed * 0.25)
  assert(value.token == 0x0123456789ab0000ULL + seed)
end

local function assert_aggregate_aligned(value, seed)
  assert(ffi.istype("lj_callxs_auth_aggregate_aligned", value))
  assert(value.lane[0] == 0x8000000000000000ULL + seed)
  assert(value.lane[1] == 0x123456789abc0000ULL + seed)
  assert(value.lane[2] == 0xfedcba9876540000ULL + seed)
end

local function aggregate_pair(nagg)
  local alpha, beta, aligned
  for i = 1, nagg do
    alpha = lib.lj_callxs_auth_aggregate_alpha_result(0.5, i)
    beta = lib.lj_callxs_auth_aggregate_beta_result(i, 1.25)
    aligned = lib.lj_callxs_auth_aggregate_aligned_result(i)
  end
  return alpha, beta, aligned
end

jit.flush()
reset_aggregate_counts()
local aggregate_alpha, aggregate_beta, aggregate_aligned = aggregate_pair(n)
assert_aggregate_alpha(aggregate_alpha, n)
assert_aggregate_beta(aggregate_beta, n)
assert_aggregate_aligned(aggregate_aligned, n)
assert_aggregate_counts(n, n, n)
local aggregate_counts = trace_op_counts()
assert((aggregate_counts["XSAVE "] or 0) >= 3,
       "aggregate path omitted XSAVE")
assert((aggregate_counts["CALLXS"] or 0) >= 3,
       "unrelated and over-aligned aggregate results omitted CALLXS")

local function ignore_aggregate_alpha(nagg)
  for i = 1, nagg do
    lib.lj_callxs_auth_aggregate_alpha_result(0.5, i)
  end
  return true
end

local function fixed_aggregate_beta(nagg)
  local result
  for i = 1, nagg do
    local value, extra1, extra2 =
      lib.lj_callxs_auth_aggregate_beta_result(i, 1.25)
    if extra1 ~= nil or extra2 ~= nil then return false end
    result = value
  end
  return result
end

local function open_aggregate_alpha(nagg)
  local result
  for i = 1, nagg do
    local count, value, extra = forward_results(
      lib.lj_callxs_auth_aggregate_alpha_result(0.5, i))
    if count ~= 1 or extra ~= nil then return false end
    result = value
  end
  return result
end

jit.flush()
reset_aggregate_counts()
assert(ignore_aggregate_alpha(n))
assert_aggregate_counts(n, 0)
assert((trace_op_counts()["CALLXS"] or 0) > 0,
       "ignored aggregate result omitted CALLXS")

jit.flush()
reset_aggregate_counts()
assert_aggregate_beta(fixed_aggregate_beta(n), n)
assert_aggregate_counts(0, n)
assert((trace_op_counts()["CALLXS"] or 0) > 0,
       "fixed aggregate result omitted CALLXS")

jit.flush()
reset_aggregate_counts()
assert_aggregate_alpha(open_aggregate_alpha(n), n)
assert_aggregate_counts(n, 0)
assert((trace_op_counts()["CALLXS"] or 0) > 0,
       "open aggregate result omitted CALLXS")

local function aggregate_alpha_call_multres(x)
  local value = lib.lj_callxs_auth_aggregate_alpha_result(
    0.5, produce_one(x))
  return value
end

local function aggregate_alpha_tail(x)
  return lib.lj_callxs_auth_aggregate_alpha_result(0.5, x)
end

local function aggregate_alpha_tail_multres(x)
  return lib.lj_callxs_auth_aggregate_alpha_result(0.5, produce_one(x))
end

local function run_aggregate_wrapper(nagg, fn)
  local result
  for i = 1, nagg do result = fn(i) end
  return result
end

assert(hasbc(aggregate_alpha_call_multres, "CALLM"))
assert(hasbc(aggregate_alpha_tail, "CALLT"))
assert(hasbc(aggregate_alpha_tail_multres, "CALLMT"))

for _, case in ipairs({
  { "aggregate CALLM", aggregate_alpha_call_multres },
  { "aggregate CALLT", aggregate_alpha_tail },
  { "aggregate CALLMT", aggregate_alpha_tail_multres },
}) do
  jit.flush()
  reset_aggregate_counts()
  assert_aggregate_alpha(run_aggregate_wrapper(n, case[2]), n)
  assert_aggregate_counts(n, 0)
  assert((trace_op_counts()["CALLXS"] or 0) > 0,
         case[1] .. " omitted CALLXS")
end

-- Pointer, enum and 64-bit results use a preallocated exact-CType box rooted
-- through XSAVE and the native frame. Bool uses a dynamic snapshot marker and
-- specializes only after native leave has closed or transferred that frame.
local function boxed_pointer(nbox)
  local value
  for i = 1, nbox do
    value = lib.lj_callxs_auth_ptr(p)
  end
  return value
end

local function bool_true(nbox)
  local value
  for _ = 1, nbox do value = lib.lj_callxs_auth_bool(1) end
  return value
end

local function bool_false(nbox)
  local value
  for _ = 1, nbox do value = lib.lj_callxs_auth_bool(0) end
  return value
end

local function bool_noncanonical(nbox)
  local value
  for _ = 1, nbox do value = lib.lj_callxs_auth_bool_raw_bits(0xa6) end
  return value
end

local function boxed_enum(nbox)
  local value
  for i = 1, nbox do value = lib.lj_callxs_auth_enum_result(i) end
  return value
end

local function boxed_i64(nbox)
  local value
  for i = 1, nbox do value = lib.lj_callxs_auth_i64_result(i) end
  return value
end

local function boxed_u64(nbox)
  local value
  for i = 1, nbox do value = lib.lj_callxs_auth_u64_result(i) end
  return value
end

local function boxed_attributed_u64(nbox)
  local value
  for i = 1, nbox do
    value = lib.lj_callxs_auth_attributed_u64_result(i)
  end
  return value
end

local function boxed_reference(nbox)
  local value
  for _ = 1, nbox do value = lib.lj_callxs_auth_reference_result() end
  return value
end

for _, case in ipairs({
  { "pointer", boxed_pointer, function(value) assert(value == p) end, true },
  { "bool true", bool_true, function(value)
      assert(type(value) == "boolean" and value == true)
    end, true },
  { "bool false", bool_false, function(value)
      assert(type(value) == "boolean" and value == false)
    end, true },
  { "bool noncanonical nonzero", bool_noncanonical, function(value)
      assert(type(value) == "boolean" and value == true)
    end, true },
  { "enum", boxed_enum, function(value)
      assert(ffi.istype("enum lj_callxs_auth_enum", value))
      assert(tonumber(value) == 7)
    end,
    true },
  { "i64", boxed_i64, function(value)
      assert(ffi.istype("int64_t", value))
      assert(value == ffi.new("int64_t", -123456789))
    end, true },
  { "u64", boxed_u64, function(value)
      assert(ffi.istype("uint64_t", value))
      assert(value == ffi.new("uint64_t", 4000000000))
    end, true },
  { "attributed u64", boxed_attributed_u64, function(value)
      assert(ffi.istype("lj_callxs_auth_attr_u64_t", value))
      assert(value == ffi.new("uint64_t", 0xfedcba9876543210ULL))
    end, true },
  { "reference", boxed_reference, function(value)
      assert(ffi.istype("int32_t &", value))
      assert(tonumber(value) == 0x345678)
    end, true },
}) do
  jit.flush()
  case[3](case[2](200))
  local boxed_counts = trace_op_counts()
  local has_callxs = (boxed_counts["CALLXS"] or 0) > 0
  assert(has_callxs == case[4],
         case[1] .. " result crossed the CALLXS admission boundary")
end

-- Reuse a trace recorded with true and feed it false. The post-leave value
-- guard must restore an actual Lua false (not integer zero), without replaying
-- the foreign call or growing a side trace across the DONE snapshot.
local function bool_value(value, nbool)
  local result
  for _ = 1, nbool do result = lib.lj_callxs_auth_bool(value) end
  return result
end

local function bool_raw_value(value, nbool)
  local result
  for _ = 1, nbool do
    result = lib.lj_callxs_auth_bool_raw_bits(value)
  end
  return result
end
jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
lib.lj_callxs_auth_reset()
assert(bool_value(1, 200) == true)
assert(lib.lj_callxs_auth_count() == 200)
assert((trace_op_counts()["CALLXS"] or 0) > 0,
       "dynamic boolean fixture omitted CALLXS")
lib.lj_callxs_auth_reset()
local mismatched_bool = bool_value(0, 20)
assert(type(mismatched_bool) == "boolean" and mismatched_bool == false)
assert(lib.lj_callxs_auth_count() == 20,
       "dynamic boolean mismatch replayed the foreign call")

-- Prove the inverse specialization too: EQ(marker, 0) must restore true when
-- a trace recorded false observes nonzero raw return bits.
jit.flush()
lib.lj_callxs_auth_reset()
assert(bool_raw_value(0, 200) == false)
assert(lib.lj_callxs_auth_count() == 200)
assert((trace_op_counts()["CALLXS"] or 0) > 0,
       "false-specialized boolean fixture omitted CALLXS")
lib.lj_callxs_auth_reset()
local inverse_mismatched_bool = bool_raw_value(0xa6, 20)
assert(type(inverse_mismatched_bool) == "boolean" and
       inverse_mismatched_bool == true)
assert(lib.lj_callxs_auth_count() == 20,
       "inverse boolean mismatch replayed the foreign call")

local function bool_call_multres(x)
  local value = lib.lj_callxs_auth_bool(produce_one(x))
  return value
end

local function bool_tail(x)
  return lib.lj_callxs_auth_bool(x)
end

local function bool_tail_multres(x)
  return lib.lj_callxs_auth_bool(produce_one(x))
end

local function run_bool_wrapper(nbool, fn, value)
  local result
  for _ = 1, nbool do result = fn(value) end
  return result
end

assert(hasbc(bool_call_multres, "CALLM"))
assert(hasbc(bool_tail, "CALLT"))
assert(hasbc(bool_tail_multres, "CALLMT"))

for _, case in ipairs({
  { "bool CALLM", bool_call_multres },
  { "bool CALLT", bool_tail },
  { "bool CALLMT", bool_tail_multres },
}) do
  jit.flush()
  lib.lj_callxs_auth_reset()
  assert(run_bool_wrapper(200, case[2], 1) == true)
  assert(lib.lj_callxs_auth_count() == 200)
  local before = trace_op_counts()["CALLXS"] or 0
  assert(before > 0, case[1] .. " omitted CALLXS")
  lib.lj_callxs_auth_reset()
  local value = run_bool_wrapper(20, case[2], 0)
  assert(type(value) == "boolean" and value == false)
  assert(lib.lj_callxs_auth_count() == 20,
         case[1] .. " mismatch replayed the call")
end

local function ignore_bool_results(nbool)
  for i = 1, nbool do lib.lj_callxs_auth_bool(bit.band(i, 1)) end
  return true
end

local function excess_bool_results(nbool)
  local result
  for _ = 1, nbool do
    local value, extra1, extra2 = lib.lj_callxs_auth_bool(0)
    assert(extra1 == nil and extra2 == nil)
    result = value
  end
  return result
end

local function open_bool_results(nbool)
  local result
  for _ = 1, nbool do
    local count, value, extra = forward_results(lib.lj_callxs_auth_bool(0))
    assert(count == 1 and extra == nil)
    result = value
  end
  return result
end

for _, case in ipairs({
  { "ignored bool", ignore_bool_results, true },
  { "excess fixed bool", excess_bool_results, false },
  { "open bool", open_bool_results, false },
}) do
  jit.flush()
  lib.lj_callxs_auth_reset()
  local value = case[2](200)
  assert(type(value) == "boolean" and value == case[3])
  assert(lib.lj_callxs_auth_count() == 200)
  assert((trace_op_counts()["CALLXS"] or 0) > 0,
         case[1] .. " result mode omitted CALLXS")
end

local function run_bool_iter(nbool, state)
  local result
  for _ = 1, nbool do
    for value in lib.lj_callxs_auth_bool_iter, state, 0 do
      result = value
      break
    end
  end
  return result
end
assert(hasbc(run_bool_iter, "ITERC"))
jit.flush()
lib.lj_callxs_auth_reset()
assert(run_bool_iter(200, 1) == true)
assert(lib.lj_callxs_auth_count() == 200)
local iter_callxs = trace_op_counts()["CALLXS"] or 0
assert(iter_callxs > 0, "bool ITERC omitted CALLXS")
lib.lj_callxs_auth_reset()
assert(run_bool_iter(20, 0) == false)
assert(lib.lj_callxs_auth_count() == 20)

print("t-ffi-callxs-authentic OK: production XSAVE/native/CALLXS path executed")
