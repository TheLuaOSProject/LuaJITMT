local bit = require "bit"
local ffi = require "ffi"
local util = require "jit.util"
local vmdef = require "jit.vmdef"

if jit.arch ~= "x64" or jit.os == "Windows" then
  print("t-ffi-callxs-sysv-small-aggregate SKIP: SysV x64 only")
  return
end

ffi.cdef [[
typedef struct lj_callxs_sysv_int_pair {
  uint32_t lo;
  uint32_t hi;
} lj_callxs_sysv_int_pair;
typedef struct lj_callxs_sysv_sse_pair {
  float x;
  float y;
} lj_callxs_sysv_sse_pair;
typedef struct lj_callxs_sysv_nested_int {
  struct { uint16_t lo; uint16_t hi; } half;
  uint32_t tag;
} lj_callxs_sysv_nested_int;
typedef struct lj_callxs_sysv_nested_sse {
  struct { float lane[2]; } inner;
} lj_callxs_sysv_nested_sse;
typedef union lj_callxs_sysv_mixed_union {
  double d;
  uint64_t u;
} lj_callxs_sysv_mixed_union;
typedef struct lj_callxs_sysv_word {
  uint32_t value;
} lj_callxs_sysv_word;
typedef struct lj_callxs_sysv_float {
  float value;
} lj_callxs_sysv_float;
typedef struct lj_callxs_sysv_byte {
  uint8_t value;
} lj_callxs_sysv_byte;
typedef struct lj_callxs_sysv_half {
  uint16_t value;
} lj_callxs_sysv_half;

void lj_callxs_sysv_reset(void);
uint32_t lj_callxs_sysv_count(uint32_t);
lj_callxs_sysv_int_pair lj_callxs_sysv_make_int(uint32_t);
uint32_t lj_callxs_sysv_take_int(lj_callxs_sysv_int_pair, uint32_t);
lj_callxs_sysv_sse_pair lj_callxs_sysv_make_sse(uint32_t);
double lj_callxs_sysv_take_sse(lj_callxs_sysv_sse_pair, double);
lj_callxs_sysv_nested_int
  lj_callxs_sysv_step_nested_int(lj_callxs_sysv_nested_int, uint16_t);
lj_callxs_sysv_nested_sse
  lj_callxs_sysv_step_nested_sse(lj_callxs_sysv_nested_sse, float);
uint64_t lj_callxs_sysv_spill_int(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t,
                                  lj_callxs_sysv_int_pair, uint64_t);
double lj_callxs_sysv_spill_sse(double, double, double, double,
                                double, double, double, double,
                                lj_callxs_sysv_sse_pair, double);
lj_callxs_sysv_mixed_union
  lj_callxs_sysv_twist_union(lj_callxs_sysv_mixed_union, uint64_t);
lj_callxs_sysv_word
  lj_callxs_sysv_step_word(lj_callxs_sysv_word, uint32_t);
lj_callxs_sysv_float
  lj_callxs_sysv_step_float(lj_callxs_sysv_float, float);
void lj_callxs_sysv_set_probe_mode(uint32_t);
lj_callxs_sysv_word
  lj_callxs_sysv_replay_probe(lj_callxs_sysv_word);
lj_callxs_sysv_byte
  lj_callxs_sysv_step_byte(uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, lj_callxs_sysv_byte);
lj_callxs_sysv_half
  lj_callxs_sysv_step_half(uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, lj_callxs_sysv_half);
]]

assert(ffi.sizeof("lj_callxs_sysv_int_pair") == 8)
assert(ffi.sizeof("lj_callxs_sysv_sse_pair") == 8)
assert(ffi.sizeof("lj_callxs_sysv_nested_int") == 8)
assert(ffi.sizeof("lj_callxs_sysv_nested_sse") == 8)
assert(ffi.sizeof("lj_callxs_sysv_mixed_union") == 8)
assert(ffi.sizeof("lj_callxs_sysv_word") == 4)
assert(ffi.sizeof("lj_callxs_sysv_float") == 4)
assert(ffi.sizeof("lj_callxs_sysv_byte") == 1)
assert(ffi.sizeof("lj_callxs_sysv_half") == 2)

local lib = ffi.load(assert(os.getenv("LJ_M7_FFI_CALLXS_SYSV_AGG_SO")))

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

local function reset_effects()
  lib.lj_callxs_sysv_reset()
end
jit.off(reset_effects, true)

local function reset_case()
  jit.flush()
  reset_effects()
end
jit.off(reset_case, true)

local function assert_case(which, expected, label)
  assert(lib.lj_callxs_sysv_count(which) == expected,
         label .. " replayed or omitted a native effect")
  local counts = trace_op_counts()
  assert((counts["XSAVE "] or 0) > 0, label .. " omitted XSAVE")
  assert((counts["CALLXS"] or 0) > 0, label .. " omitted CALLXS")
end
jit.off(assert_case, true)

jit.opt.start("hotloop=1", "hotexit=1")
local n = 96
local triangle = n * (n + 1) / 2

local function run_int_results(count)
  local sum = 0
  for i = 1, count do
    local value = lib.lj_callxs_sysv_make_int(i)
    sum = sum + tonumber(value.lo) + tonumber(value.hi)
  end
  return sum
end

reset_case()
assert(run_int_results(n) == 8 * triangle + 3 * n)
assert_case(0, n, "INTEGER aggregate result")

local int_arg = ffi.new("lj_callxs_sysv_int_pair")
local function run_int_args(count)
  local sum = 0
  for i = 1, count do
    int_arg.lo = i + 10
    int_arg.hi = i + 20
    sum = sum + lib.lj_callxs_sysv_take_int(int_arg, i)
  end
  return sum
end

reset_case()
assert(run_int_args(n) == 4 * triangle + 50 * n)
assert_case(1, n, "INTEGER aggregate argument")

local int_ref = ffi.new("lj_callxs_sysv_int_pair[1]")
local function run_int_ref_args(count)
  local sum = 0
  for i = 1, count do
    int_ref[0].lo = i + 10
    int_ref[0].hi = i + 20
    sum = sum + lib.lj_callxs_sysv_take_int(int_ref[0], i)
  end
  return sum
end

reset_case()
assert(run_int_ref_args(n) == 4 * triangle + 50 * n)
assert_case(1, n, "INTEGER aggregate reference argument")

local function run_sse_results(count)
  local sum = 0
  for i = 1, count do
    local value = lib.lj_callxs_sysv_make_sse(i)
    sum = sum + value.x + value.y
  end
  return sum
end

reset_case()
assert(run_sse_results(n) == 3 * triangle + 0.75 * n)
assert_case(2, n, "SSE aggregate result")

local sse_arg = ffi.new("lj_callxs_sysv_sse_pair")
local function run_sse_args(count)
  local sum = 0
  for i = 1, count do
    sse_arg.x = i + 0.25
    sse_arg.y = i * 2 + 0.5
    sum = sum + lib.lj_callxs_sysv_take_sse(sse_arg, 0.75)
  end
  return sum
end

reset_case()
assert(run_sse_args(n) == 3 * triangle + 1.5 * n)
assert_case(3, n, "SSE aggregate argument")

local function run_nested_int(count)
  local value = ffi.new("lj_callxs_sysv_nested_int")
  value.half.lo = 1
  value.half.hi = 2
  value.tag = 3
  for _ = 1, count do
    value = lib.lj_callxs_sysv_step_nested_int(value, 1)
  end
  return value
end

reset_case()
local nested_int = run_nested_int(n)
assert(nested_int.half.lo == 1 + n)
assert(nested_int.half.hi == 2 + 2 * n)
assert(nested_int.tag == 3 + 3 * n)
assert_case(4, n, "recursive INTEGER aggregate")

local function run_nested_sse(count)
  local value = ffi.new("lj_callxs_sysv_nested_sse")
  value.inner.lane[0] = 1.25
  value.inner.lane[1] = 2.5
  for _ = 1, count do
    value = lib.lj_callxs_sysv_step_nested_sse(value, 0.25)
  end
  return value
end

reset_case()
local nested_sse = run_nested_sse(n)
assert(nested_sse.inner.lane[0] == 1.25 + 0.25 * n)
assert(nested_sse.inner.lane[1] == 2.5 + 0.5 * n)
assert_case(5, n, "recursive SSE aggregate")

int_arg.lo = 7
int_arg.hi = 11
local function run_spilled_int(count)
  local sum = 0
  for i = 1, count do
    sum = sum + tonumber(lib.lj_callxs_sysv_spill_int(
      1, 2, 3, 4, 5, 6, int_arg, i))
  end
  return sum
end

reset_case()
assert(run_spilled_int(n) == triangle + 39 * n)
assert_case(6, n, "stack-fallback INTEGER aggregate")

sse_arg.x = 1.25
sse_arg.y = 2.5
local function run_spilled_sse(count)
  local sum = 0
  for i = 1, count do
    sum = sum + lib.lj_callxs_sysv_spill_sse(
      1, 2, 3, 4, 5, 6, 7, 8, sse_arg, i + 0.25)
  end
  return sum
end

reset_case()
assert(run_spilled_sse(n) == triangle + 40 * n)
assert_case(7, n, "stack-fallback SSE aggregate")

local function run_mixed_union(count)
  local value = ffi.new("lj_callxs_sysv_mixed_union")
  value.u = 0x123456789abc0000ULL
  for _ = 1, count do
    value = lib.lj_callxs_sysv_twist_union(value, 0x55ULL)
  end
  return value
end

reset_case()
local mixed = run_mixed_union(n)
assert(mixed.u == 0x123456789abc0000ULL)
assert_case(8, n, "mixed eightbyte INTEGER precedence")

local function run_word(count)
  local value = ffi.new("lj_callxs_sysv_word")
  value.value = 7
  for _ = 1, count do
    value = lib.lj_callxs_sysv_step_word(value, 3)
  end
  return value
end

reset_case()
local word = run_word(n)
assert(word.value == 7 + 3 * n)
assert_case(9, n, "four-byte INTEGER aggregate argument/result")

local function run_float(count)
  local value = ffi.new("lj_callxs_sysv_float")
  value.value = 1.25
  for _ = 1, count do
    value = lib.lj_callxs_sysv_step_float(value, 0.25)
  end
  return value
end

reset_case()
local narrow_float = run_float(n)
assert(narrow_float.value == 1.25 + 0.25 * n)
assert_case(10, n, "four-byte SSE aggregate argument/result")

-- Flip a native-only result bit after recording the equal branch. The first
-- mismatching iteration exits after CALLXS; its exact effect count proves the
-- restored result box resumes after the foreign call instead of replaying it.
local replay_arg = ffi.new("lj_callxs_sysv_word")
replay_arg.value = 0x1234
local function run_replay_probe(count)
  local mismatches = 0
  for _ = 1, count do
    local value = lib.lj_callxs_sysv_replay_probe(replay_arg)
    if value.value ~= 0x1234 then mismatches = mismatches + 1 end
  end
  return mismatches
end

reset_case()
assert(run_replay_probe(n) == 0)
assert_case(11, n, "direct aggregate replay-probe warm path")
reset_effects()  -- Keep the recorded result guard live.
lib.lj_callxs_sysv_set_probe_mode(1)
local replay_n = 24
assert(run_replay_probe(replay_n) == replay_n)
assert_case(11, replay_n, "direct aggregate post-call side exit")

local function run_byte(count)
  local value = ffi.new("lj_callxs_sysv_byte")
  value.value = 7
  for _ = 1, count do
    value = lib.lj_callxs_sysv_step_byte(1, 0, 0, 0, 0, 0, value)
  end
  return value
end

reset_case()
local byte = run_byte(n)
assert(byte.value == 7 + n)
assert_case(12, n, "one-byte INTEGER aggregate stack argument/result")

local function run_half(count)
  local value = ffi.new("lj_callxs_sysv_half")
  value.value = 11
  for _ = 1, count do
    value = lib.lj_callxs_sysv_step_half(1, 1, 1, 0, 0, 0, value)
  end
  return value
end

reset_case()
local half = run_half(n)
assert(half.value == 11 + 3 * n)
assert_case(13, n, "two-byte INTEGER aggregate stack argument/result")

print("t-ffi-callxs-sysv-small-aggregate OK: generic one-class ABI path")
