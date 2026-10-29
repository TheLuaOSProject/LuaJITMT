local ffi = require('ffi')
local jitutil = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
local threading = require('threading')

jit.off(true, true)
ffi.cdef[[
typedef struct { uint32_t lo, hi; } Arm64IntPair;
typedef struct { float lo, hi; } Arm64FloatPair;
typedef struct { float value; } Arm64FloatOne;
typedef struct { double value; } Arm64DoubleOne;
typedef Arm64FloatPair& Arm64FloatPairRef;
typedef uint32_t (*Arm64FloatPairVarargFn)(Arm64FloatPair, ...);
typedef struct { float f; double d; } Arm64MixedFloatHFA;
typedef struct { double lo, hi; } Arm64DoublePair;
typedef struct { float a, b, c; } Arm64FloatTriple;
typedef struct { double x, y, z, w; } Arm64DoubleQuad;
typedef Arm64FloatTriple& Arm64FloatTripleRef;
typedef uint32_t (*Arm64MixedFloatHFAFn)(Arm64MixedFloatHFA);
typedef uint32_t (*Arm64DoublePairFn)(Arm64DoublePair);
typedef uint32_t (*Arm64FloatTripleFn)(Arm64FloatTriple);
typedef double (*Arm64FloatTripleSaltFn)(Arm64FloatTriple, double);
typedef double (*Arm64DoubleQuadSaltFn)(Arm64DoubleQuad, double);
typedef double (*Arm64FloatTripleRefFn)(Arm64FloatTripleRef, double);
typedef double (*Arm64FloatPairStackFn)(double, double, double, double,
                                        double, double, double, double,
                                        Arm64FloatPair, double);
typedef double (*Arm64FprOverflowFn)(double, double, double, double,
                                     double, double, double,
                                     Arm64FloatTriple);
typedef struct { Arm64IntPair pair; uint32_t tag; } Arm64NestedPair;
typedef union { uint64_t u; double d; } Arm64MixedUnion;
typedef struct { uint8_t value; } Arm64Byte;
typedef struct { uint16_t value; } Arm64Half;
typedef struct { uint32_t value; } Arm64Word;
typedef struct { uint8_t value; } Arm64ByteResult;
typedef struct { uint16_t value; } Arm64HalfResult;
typedef struct { uint32_t value; } Arm64WordResult;
typedef struct { uint64_t value; } Arm64U64Result;
typedef struct { float value; } Arm64FloatResult;
typedef struct { double value; } Arm64DoubleResult;
typedef struct { float lo, hi; } Arm64FloatPairResult;
typedef struct __attribute__((aligned(16))) {
  float lo, hi;
} Arm64AlignedFloatPairResult;
typedef struct { double lo, hi; } Arm64DoublePairResult;
typedef struct { float x, y, z; } Arm64FloatTripleResult;
typedef struct { float x, y, z, w; } Arm64FloatQuadResult;
typedef struct { double x, y, z; } Arm64DoubleTripleResult;
typedef struct { double x, y, z, w; } Arm64DoubleQuadResult;
typedef struct { double x, y, z, w, v; } Arm64DoubleQuintResult;
typedef struct { double x, y, z, w, v; } Arm64DoubleQuint;
typedef struct __attribute__((aligned(16))) {
  double x, y, z;
} Arm64AlignedDoubleTripleResult;
typedef struct __attribute__((aligned(16))) {
  double x, y, z;
} Arm64AlignedDoubleTriple;
typedef Arm64DoubleQuintResult (*Arm64DoubleQuintResultFn)(uint32_t);
typedef double (*Arm64DoubleQuintFn)(Arm64DoubleQuint, double);
typedef Arm64AlignedDoubleTripleResult (*Arm64AlignedDoubleTripleResultFn)(uint32_t);
typedef double (*Arm64AlignedDoubleTripleFn)(Arm64AlignedDoubleTriple, double);
typedef Arm64DoublePairResult (*Arm64DoublePairResultFPRFn)(double, uint32_t);
typedef Arm64AlignedFloatPairResult (*Arm64AlignedFloatPairResultFn)(uint32_t);
typedef Arm64DoublePairResult (*Arm64DoublePairResultFn)(uint32_t);
typedef struct __attribute__((aligned(16))) {
  float value;
} Arm64AlignedFloatResult;
typedef Arm64AlignedFloatResult (*Arm64AlignedFloatResultFn)(uint32_t);
typedef struct __attribute__((aligned(8))) {
  float value;
} Arm64PaddedFloatResult;
typedef Arm64PaddedFloatResult (*Arm64PaddedFloatResultFn)(uint32_t);
typedef Arm64WordResult Arm64WordResultAlias;
typedef struct __attribute__((aligned(16))) {
  uint32_t value;
} Arm64AlignedResult;
typedef Arm64WordResultAlias (*Arm64WordResultFn)(uint32_t);
typedef Arm64AlignedResult (*Arm64AlignedResultFn)(uint32_t);
typedef struct { uint64_t lo, hi; } Arm64BigPair;
typedef struct { uint32_t value:5; } Arm64Bitfield;
void arm64_aggregate_reset(void);
uint32_t arm64_aggregate_count(uint32_t);
uint32_t arm64_take_int_pair(Arm64IntPair, uint32_t);
double arm64_take_int_pair_double_salt(Arm64IntPair, uint32_t);
uint32_t arm64_take_float_pair(Arm64FloatPair);
double arm64_take_float_pair_double_salt(Arm64FloatPair, double);
double arm64_take_float_one_double_salt(Arm64FloatOne, double);
double arm64_take_double_one_double_salt(Arm64DoubleOne, double);
double arm64_take_float_triple_double_salt(Arm64FloatTriple, double);
double arm64_take_double_quad_double_salt(Arm64DoubleQuad, double);
double arm64_take_float_pair_stack(double, double, double, double,
                                   double, double, double, double,
                                   Arm64FloatPair, double);
double arm64_take_float_pair_stack_alt(double, double, double, double,
                                       double, double, double, double,
                                       Arm64FloatPair, double);
int32_t arm64_vararg_i32(int32_t, ...);
uint32_t arm64_take_nested_pair(Arm64NestedPair);
uint32_t arm64_take_union(Arm64MixedUnion);
uint32_t arm64_take_byte(Arm64Byte, uint32_t);
uint32_t arm64_take_half(Arm64Half, uint32_t);
uint32_t arm64_take_word(Arm64Word, uint32_t);
uint32_t arm64_take_big_pair(Arm64BigPair);
uint32_t arm64_take_bitfield(Arm64Bitfield);
uint32_t arm64_take_pair_no_gpr(uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, uint32_t,
                                Arm64IntPair);
uint32_t arm64_take_pair_stack(uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, uint32_t,
                                Arm64IntPair, uint32_t);
uint32_t arm64_take_pair_stack_alt(uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, uint32_t,
                                Arm64IntPair, uint32_t);
Arm64ByteResult arm64_return_byte(uint32_t);
Arm64HalfResult arm64_return_half(uint32_t);
Arm64WordResult arm64_return_word(uint32_t);
Arm64WordResultAlias arm64_return_word_alias(uint32_t);
Arm64U64Result arm64_return_u64(uint32_t);
Arm64FloatResult arm64_return_float_hfa(uint32_t);
Arm64DoubleResult arm64_return_double_hfa(uint32_t);
Arm64FloatPairResult arm64_return_float_pair_hfa(uint32_t);
Arm64FloatPairResult arm64_return_float_pair_hfa_fpr(double, uint32_t);
Arm64DoublePairResult arm64_return_double_pair_hfa(uint32_t);
Arm64DoublePairResult arm64_return_double_pair_hfa_fpr(double, uint32_t);
Arm64FloatTripleResult arm64_return_float_triple_hfa(uint32_t);
Arm64FloatQuadResult arm64_return_float_quad_hfa(uint32_t);
Arm64DoubleTripleResult arm64_return_double_triple_hfa(uint32_t);
Arm64DoubleQuadResult arm64_return_double_quad_hfa(uint32_t);
double arm64_return_float_pair_scalar_fpr(double, uint32_t);
uint32_t arm64_return_scalar(uint32_t);
]]
local lib = ffi.load(assert(os.getenv('LJ_ARM64_INT_AGGREGATE_SO')))
_G.__arm64_return_byte_fn = lib.arm64_return_byte
_G.__arm64_return_half_fn = lib.arm64_return_half
_G.__arm64_return_word_fn = lib.arm64_return_word
_G.__arm64_return_u64_fn = lib.arm64_return_u64
_G.__arm64_return_float_hfa_fn = lib.arm64_return_float_hfa
_G.__arm64_return_double_hfa_fn = lib.arm64_return_double_hfa
_G.__arm64_return_float_pair_hfa_fn = lib.arm64_return_float_pair_hfa
_G.__arm64_return_float_pair_hfa_fpr_fn = lib.arm64_return_float_pair_hfa_fpr
_G.__arm64_return_double_pair_hfa_fn = lib.arm64_return_double_pair_hfa
_G.__arm64_return_double_pair_hfa_fpr_fn =
  lib.arm64_return_double_pair_hfa_fpr
_G.__arm64_return_float_triple_hfa_fn = lib.arm64_return_float_triple_hfa
_G.__arm64_return_float_quad_hfa_fn = lib.arm64_return_float_quad_hfa
_G.__arm64_return_double_triple_hfa_fn = lib.arm64_return_double_triple_hfa
_G.__arm64_return_double_quad_hfa_fn = lib.arm64_return_double_quad_hfa
_G.__arm64_double_quint_result_fn =
  ffi.cast('Arm64DoubleQuintResultFn', nil)
_G.__arm64_aligned_double_triple_result_fn =
  ffi.cast('Arm64AlignedDoubleTripleResultFn', nil)
_G.__arm64_double_quint_arg_fn = ffi.cast('Arm64DoubleQuintFn', nil)
_G.__arm64_aligned_double_triple_arg_fn =
  ffi.cast('Arm64AlignedDoubleTripleFn', nil)
_G.__arm64_aligned_float_result_fn = ffi.cast('Arm64AlignedFloatResultFn', nil)
_G.__arm64_padded_float_result_fn = ffi.cast('Arm64PaddedFloatResultFn', nil)
_G.__arm64_aligned_float_pair_result_fn =
  ffi.cast('Arm64AlignedFloatPairResultFn', nil)
_G.__arm64_double_pair_result_fn = ffi.cast('Arm64DoublePairResultFn', nil)
_G.__arm64_word_result_alias_fn = ffi.cast('Arm64WordResultFn', nil)
_G.__arm64_aligned_result_fn = ffi.cast('Arm64AlignedResultFn', nil)
_G.__arm64_int_pair_fn = lib.arm64_take_int_pair
_G.__arm64_float_pair_fn = lib.arm64_take_float_pair
_G.__arm64_float_pair_double_salt_fn = lib.arm64_take_float_pair_double_salt
_G.__arm64_float_one_double_salt_fn = lib.arm64_take_float_one_double_salt
_G.__arm64_double_one_double_salt_fn = lib.arm64_take_double_one_double_salt
_G.__arm64_float_triple_double_salt_fn = lib.arm64_take_float_triple_double_salt
_G.__arm64_double_quad_double_salt_fn = lib.arm64_take_double_quad_double_salt
_G.__arm64_float_triple_ref_fn = ffi.cast('Arm64FloatTripleRefFn', nil)
_G.__arm64_hfa_fpr_overflow_fn = ffi.cast('Arm64FprOverflowFn', nil)
_G.__arm64_vararg_i32_fn = lib.arm64_vararg_i32
_G.__arm64_mixed_float_hfa_fn = ffi.cast('Arm64MixedFloatHFAFn', nil)
_G.__arm64_double_pair_fn = ffi.cast('Arm64DoublePairFn', nil)
_G.__arm64_float_triple_fn = ffi.cast('Arm64FloatTripleFn', nil)
_G.__arm64_float_vararg_fn = ffi.cast('Arm64FloatPairVarargFn',
                                      lib.arm64_take_float_pair)
_G.__arm64_nested_pair_fn = lib.arm64_take_nested_pair
_G.__arm64_mixed_union_fn = lib.arm64_take_union
_G.__arm64_byte_fn = lib.arm64_take_byte
_G.__arm64_half_fn = lib.arm64_take_half
_G.__arm64_word_fn = lib.arm64_take_word
_G.__arm64_big_pair_fn = lib.arm64_take_big_pair
_G.__arm64_bitfield_fn = lib.arm64_take_bitfield
_G.__arm64_pair_no_gpr_fn = lib.arm64_take_pair_no_gpr
_G.__arm64_pair_stack_fn = lib.arm64_take_pair_stack
_G.__arm64_word_stack_fn = ffi.cast([[uint32_t (*)(uint32_t,uint32_t,
  uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,Arm64Word)]], 0)
_G.__arm64_pair_stack_vararg_fn = ffi.cast([[uint32_t (*)(uint32_t,uint32_t,
  uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,Arm64IntPair,...)]], 0)
_G.__arm64_float_pair_stack_fn = lib.arm64_take_float_pair_stack

local candidate = assert(loadstring([[
return function(n, fn, value)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, i) end
  return sum
end
]], '@integer-aggregate-candidate'))()
local oracle = assert(loadstring([[
return function(n, fn, value)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, i) end
  return sum
end
]], '@integer-aggregate-oracle'))()
local clone = assert(loadstring([[
return function(n, fn, value)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, i) end
  return sum
end
]], '@integer-aggregate-clone'))()
jit.off(oracle, true)
jit.off(clone, true)
_G.__arm64_integer_aggregate_candidate = candidate
_G.__arm64_integer_aggregate_clone = clone
_G.__arm64_integer_aggregate_oracle = oracle
_set_aggregate_candidate(candidate)

local hfa_candidate = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@float-hfa-candidate'))()
local hfa_triple_candidate = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@float-triple-hfa-candidate'))()
local hfa_quad_candidate = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@double-quad-hfa-candidate'))()
local hfa_ref_candidate = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@float-hfa-reference-candidate'))()
local hfa_oracle = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@float-hfa-oracle'))()
local hfa_clone = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@float-hfa-clone'))()
local function hfa_pair()
  return function(n, fn, value, salt)
    local sum = 0
    for i = 1, n do sum = sum + fn(value, salt) end
    return sum
  end
end
local float_one_candidate = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@float-one-hfa-candidate'))()
local float_one_oracle_chunk = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@float-one-hfa-oracle'))
jit.off(float_one_oracle_chunk, true)
local float_one_oracle = float_one_oracle_chunk()
local float_one_clone = hfa_pair()
local double_one_candidate = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@double-one-hfa-candidate'))()
local double_one_oracle_chunk = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@double-one-hfa-oracle'))
jit.off(double_one_oracle_chunk, true)
local double_one_oracle = double_one_oracle_chunk()
local double_one_clone = hfa_pair()
local integer_callm_candidate = assert(loadstring([[
return function(n, producer, consumer, value, aggregate)
  local sum = 0
  for i = 1, n do sum = sum + consumer(aggregate, producer(value, 1)) end
  return sum
end
]], '@integer-callm-candidate'))()
local integer_callm_oracle_chunk = assert(loadstring([[
return function(n, producer, consumer, value, aggregate)
  local sum = 0
  for i = 1, n do sum = sum + consumer(aggregate, producer(value, 1)) end
  return sum
end
]], '@integer-callm-oracle'))
jit.off(integer_callm_oracle_chunk, true)
local integer_callm_oracle = integer_callm_oracle_chunk()
local integer_callm_clone = assert(loadstring([[
return function(n, producer, consumer, value, aggregate)
  local sum = 0
  for i = 1, n do sum = sum + consumer(aggregate, producer(value, 1)) end
  return sum
end
]], '@integer-callm-clone'))()
jit.off(hfa_oracle, true)
jit.off(hfa_clone, true)
jit.off(float_one_oracle, true)
jit.off(float_one_clone, true)
jit.off(double_one_oracle, true)
jit.off(double_one_clone, true)
jit.off(integer_callm_oracle, true)
jit.off(integer_callm_clone, true)
jit.off(hfa_ref_candidate, true)
_G.__arm64_float_hfa_candidate = hfa_candidate
_G.__arm64_float_triple_hfa_candidate = hfa_triple_candidate
_G.__arm64_double_quad_hfa_candidate = hfa_quad_candidate
_G.__arm64_float_hfa_oracle = hfa_oracle
_G.__arm64_float_hfa_clone = hfa_clone
_G.__arm64_float_one_candidate = float_one_candidate
_G.__arm64_float_one_clone = float_one_clone
_G.__arm64_double_one_candidate = double_one_candidate
_G.__arm64_double_one_clone = double_one_clone
_G.__arm64_integer_callm_candidate = integer_callm_candidate
_G.__arm64_integer_callm_clone = integer_callm_clone
_set_hfa_candidate(hfa_candidate)
jit.on(hfa_ref_candidate, true)

local pair = ffi.new('Arm64IntPair', 7, 11)
local byte = ffi.new('Arm64Byte', 7)
local half = ffi.new('Arm64Half', 701)
local word = ffi.new('Arm64Word', 70001)
local fn = lib.arm64_take_int_pair
local alt = lib.arm64_take_int_pair_double_salt
local function roots(fn)
  fn = fn or candidate
  local found = {}
  for pc = 0, jitutil.funcinfo(fn).bytecodes-1 do
    local ins = jitutil.funcbc(fn, pc)
    if ins then
      local op = bit.band(ins, 255)
      local name = vmdef.bcnames:sub(op*6+1, op*6+6)
      if name == 'JFORL ' or name == 'JLOOP ' then
        found[bit.rshift(ins, 16)] = true
      end
    end
  end
  return found
end
jit.off(roots, true)
local function hfa_roots()
  return roots(hfa_candidate)
end
jit.off(hfa_roots, true)
local selected, exits, hfa_exits = {}, 0, 0
local tracked_exit_roots
local function observe(no, exitno)
  if (tracked_exit_roots or roots())[no] then exits = exits + 1 end
  if hfa_roots()[no] then hfa_exits = hfa_exits + 1 end
end
jit.off(observe, true)

local ready, stop = threading.channel(1), threading.channel(1)
local peer = assert(threading.spawn(function(r, s)
  assert(r:send(true, 5))
  assert(select(2, s:recv(120)))
  return true
end, ready, stop))
assert(select(2, ready:recv(5)), 'live peer did not start')

jit.flush()
jit.opt.start('hotloop=1', 'hotexit=1000', 'maxtrace=64')
jit.attach(observe, 'texit')
jit.on(hfa_candidate, true)
jit.on(candidate, true)
lib.arm64_aggregate_reset()
local function has_native_aggregate_root(xload_type)
  for no in pairs(roots()) do
    local info = jitutil.traceinfo(no)
    if info then
      local callxs, xload = false, false
      for ref = 1, info.nins do
        local _, ot = jitutil.traceir(no, ref)
        if ot then
          local op = bit.rshift(ot, 8)
          local name = vmdef.irnames:sub(op*6+1, op*6+6)
          callxs = callxs or name == 'CALLXS'
          xload = xload or
            (name == 'XLOAD ' and bit.band(ot, 255) == xload_type)
        end
      end
      if callxs and xload then return true end
    end
  end
  return false
end
jit.off(has_native_aggregate_root, true)
local small_cases = {
  { lib.arm64_take_byte, byte, 6, 16 },
  { lib.arm64_take_half, half, 7, 18 },
  { lib.arm64_take_word, word, 8, 20 },
}
for _, case in ipairs(small_cases) do
  jit.flush(candidate)
  exits = 0
  for pass = 1, 8 do
    assert(candidate(16, case[1], case[2]) ==
           oracle(16, case[1], case[2]))
  end
  assert(has_native_aggregate_root(case[4]),
    'small aggregate has no native CALLXS/XLOAD width-matched root')
  assert(exits > 0, 'small aggregate trace did not return through native code')
  assert(lib.arm64_aggregate_count(case[3]) == 256,
    'small aggregate effects were lost or replayed: ' ..
    tostring(lib.arm64_aggregate_count(case[3])))
end
jit.flush(hfa_candidate)
local normal_hfa_candidate = hfa_candidate
hfa_candidate = hfa_ref_candidate
_G.__arm64_float_hfa_candidate = hfa_candidate
_set_hfa_candidate(hfa_candidate)
local hfa_ref_seed = ffi.new('Arm64FloatPair', 1.25, 2.5)
local hfa_ref = ffi.cast('Arm64FloatPairRef', hfa_ref_seed)
lib.arm64_aggregate_reset()
for pass = 1, 8 do
  assert(hfa_candidate(16, lib.arm64_take_float_pair_double_salt,
                       hfa_ref, 3.75) ==
         hfa_oracle(16, lib.arm64_take_float_pair_double_salt,
                    hfa_ref, 3.75))
end
assert(_hfa_trace_count() == 0,
  'reference-valued HFA argument published a native root')
assert(lib.arm64_aggregate_count(3) == 256,
  'reference-valued HFA fallback effects were lost or replayed')

hfa_candidate = normal_hfa_candidate
_G.__arm64_float_hfa_candidate = hfa_candidate
_set_hfa_candidate(hfa_candidate)
jit.flush(hfa_candidate)
exits = 0
lib.arm64_aggregate_reset()
_begin_hfa_witness()
local hfa_seed = ffi.new('Arm64FloatPair', 1.25, 2.5)
local hfa_fn = lib.arm64_take_float_pair_double_salt
for pass = 1, 8 do
  assert(hfa_candidate(16, hfa_fn, hfa_seed, 3.75) ==
         hfa_oracle(16, hfa_fn, hfa_seed, 3.75))
end
assert(hfa_exits > 0, 'HFA root did not take a native exit')
assert(_hfa_trace_count() > 0, 'HFA native root was not published')
assert(_hfa_native_count() > 0, 'HFA root did not finish a native call')
assert(lib.arm64_aggregate_count(3) == 256,
  'HFA warm native/interpreter effects were lost or replayed')
local hfa_before_gc = _hfa_native_count()
collectgarbage('collect')
exits = 0
hfa_exits = 0
local hfa_pair = ffi.new('Arm64FloatPair', 1.25, 2.5)
assert(hfa_candidate(32, lib.arm64_take_float_pair_double_salt,
                     hfa_pair, 3.75) ==
       hfa_oracle(32, lib.arm64_take_float_pair_double_salt,
                  hfa_pair, 3.75))
assert(_hfa_trace_count() > 0, 'HFA root was lost after full GC')
assert(_hfa_native_count() > hfa_before_gc,
  'HFA root did not re-enter native code after full GC')
assert(hfa_exits > 0, 'HFA root did not exit after full-GC re-entry')
assert(lib.arm64_aggregate_count(3) == 320,
  'HFA post-GC effects were lost or replayed')

-- Darwin passes an HFA wholly on the stack when all FPRs are consumed.
-- Keep the added candidate setup adjacent to this probe, after the baseline
-- two-float HFA root checks.
local hfa_stack_candidate = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do
    sum = sum + fn(1, 2, 3, 4, 5, 6, 7, 8, value, salt)
  end
  return sum
end
]], '@float-pair-hfa-stack-candidate'))()
local hfa_stack_oracle_chunk = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do
    sum = sum + fn(1, 2, 3, 4, 5, 6, 7, 8, value, salt)
  end
  return sum
end
]], '@float-pair-hfa-stack-oracle'))
jit.off(hfa_stack_oracle_chunk, true)
local hfa_stack_oracle = hfa_stack_oracle_chunk()
local hfa_stack_clone_chunk = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do
    sum = sum + fn(1, 2, 3, 4, 5, 6, 7, 8, value, salt)
  end
  return sum
end
]], '@float-pair-hfa-stack-clone'))
jit.off(hfa_stack_clone_chunk, true)
local hfa_stack_clone = hfa_stack_clone_chunk()
jit.off(hfa_stack_oracle, true)
jit.off(hfa_stack_clone, true)
_G.__arm64_float_pair_stack_candidate = hfa_stack_candidate
_G.__arm64_float_pair_stack_clone = hfa_stack_clone
local saved_hfa_candidate = hfa_candidate
hfa_candidate = hfa_stack_candidate
_G.__arm64_float_hfa_candidate = hfa_candidate
_set_hfa_candidate(hfa_candidate)
jit.flush(hfa_candidate)
jit.on(hfa_candidate, true)
exits, hfa_exits = 0, 0
lib.arm64_aggregate_reset()
_begin_hfa_witness()
local stack_pair = ffi.new('Arm64FloatPair', 1.25, 2.5)
local stack_fn = lib.arm64_take_float_pair_stack
local stack_native_before = _hfa_native_count()
for pass = 1, 8 do
  assert(hfa_candidate(16, stack_fn, stack_pair, 3.75) ==
         hfa_stack_oracle(16, stack_fn, stack_pair, 3.75),
         'stack-passed HFA differs from the interpreter')
end
assert(_hfa_trace_count() > 0 and
       _hfa_native_count() > stack_native_before,
       'stack-passed HFA root did not finish natively')
assert(hfa_exits > 0, 'stack-passed HFA root did not exit natively')
assert(lib.arm64_aggregate_count(34) == 256,
       'stack-passed HFA effects were lost or replayed: ' ..
       tostring(lib.arm64_aggregate_count(34)))
stack_native_before = _hfa_native_count()
collectgarbage('collect')
exits, hfa_exits = 0, 0
assert(hfa_candidate(32, stack_fn, stack_pair, 3.75) ==
       hfa_stack_oracle(32, stack_fn, stack_pair, 3.75),
       'stack-passed HFA changed after full GC')
assert(_hfa_native_count() > stack_native_before and hfa_exits > 0,
       'stack-passed HFA did not re-enter and exit natively after GC')
assert(lib.arm64_aggregate_count(34) == 320,
       'stack-passed HFA post-GC effects were lost or replayed')
tracked_exit_roots = roots(hfa_candidate)
exits = 0
assert(hfa_candidate(32, lib.arm64_take_float_pair_stack_alt,
                     stack_pair, 3.75) ==
       hfa_stack_oracle(32, lib.arm64_take_float_pair_stack_alt,
                        stack_pair, 3.75),
       'stack-passed HFA callable-change fallback differs')
assert(exits > 0, 'stack-passed HFA callable change did not force an exit')
assert(lib.arm64_aggregate_count(35) == 64,
       'stack-passed HFA callable-change effects were lost or replayed: ' ..
       tostring(lib.arm64_aggregate_count(35)))
tracked_exit_roots = nil
hfa_candidate = saved_hfa_candidate
_G.__arm64_float_hfa_candidate = hfa_candidate
_set_hfa_candidate(hfa_candidate)


-- A full-width integer aggregate spills after the eighth GPR.
local int_stack_candidate = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do
    sum = sum + fn(1, 2, 3, 4, 5, 6, 7, 8, value, salt)
  end
  return sum
end
]], '@integer-pair-stack-candidate'))()
local int_stack_oracle_chunk = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do
    sum = sum + fn(1, 2, 3, 4, 5, 6, 7, 8, value, salt)
  end
  return sum
end
]], '@integer-pair-stack-oracle'))
jit.off(int_stack_oracle_chunk, true)
local int_stack_oracle = int_stack_oracle_chunk()
local int_stack_clone_chunk = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do
    sum = sum + fn(1, 2, 3, 4, 5, 6, 7, 8, value, salt)
  end
  return sum
end
]], '@integer-pair-stack-clone'))
jit.off(int_stack_clone_chunk, true)
local int_stack_clone = int_stack_clone_chunk()
jit.off(int_stack_oracle, true)
jit.off(int_stack_clone, true)
_G.__arm64_integer_pair_stack_candidate = int_stack_candidate
_G.__arm64_integer_pair_stack_clone = int_stack_clone
local saved_integer_candidate = candidate
candidate = int_stack_candidate
_set_aggregate_candidate(candidate)
jit.flush(candidate)
-- Reset hashed warmup counters after the preceding independent probes.
jit.opt.start('hotloop=1')
jit.on(candidate, true)
exits = 0
lib.arm64_aggregate_reset()
local stack_pair = ffi.new('Arm64IntPair', 17, 43)
local stack_fn = lib.arm64_take_pair_stack
local stack_native_before = _aggregate_native_width_count(8)
for pass = 1, 8 do
  assert(candidate(16, stack_fn, stack_pair, 19) ==
         int_stack_oracle(16, stack_fn, stack_pair, 19),
         'stack-passed integer pair differs from the interpreter')
end
assert(next(roots(candidate)) and
       _aggregate_native_width_count(8) > stack_native_before,
       'stack-passed integer pair root did not finish natively: root='..tostring(next(roots(candidate)))..', before='..stack_native_before..', after='.._aggregate_native_width_count(8))
assert(exits > 0, 'stack-passed integer pair root did not exit natively')
assert(lib.arm64_aggregate_count(36) == 256,
       'stack-passed integer pair effects were lost or replayed: ' ..
       tostring(lib.arm64_aggregate_count(36)))
stack_native_before = _aggregate_native_width_count(8)
collectgarbage('collect')
exits = 0
assert(candidate(32, stack_fn, stack_pair, 19) ==
       int_stack_oracle(32, stack_fn, stack_pair, 19),
       'stack-passed integer pair changed after full GC')
assert(_aggregate_native_width_count(8) > stack_native_before and exits > 0,
       'stack-passed integer pair did not re-enter and exit natively after GC')
assert(lib.arm64_aggregate_count(36) == 320,
       'stack-passed integer pair post-GC effects were lost or replayed')
tracked_exit_roots = roots(candidate)
exits = 0
assert(candidate(32, lib.arm64_take_pair_stack_alt,
                     stack_pair, 19) ==
       int_stack_oracle(32, lib.arm64_take_pair_stack_alt,
                        stack_pair, 19),
       'stack-passed integer pair callable-change fallback differs')
assert(exits > 0, 'stack-passed integer pair callable change did not force an exit')
assert(lib.arm64_aggregate_count(37) == 64,
       'stack-passed integer pair callable-change effects were lost or replayed: ' ..
       tostring(lib.arm64_aggregate_count(37)))
tracked_exit_roots = nil
candidate = saved_integer_candidate
_set_aggregate_candidate(candidate)


local function run_one_hfa(candidate_fn, oracle_fn, type_name, ffi_fn, counter,
			   label)
  hfa_candidate = candidate_fn
  _set_hfa_candidate(candidate_fn)
  jit.flush(candidate_fn)
  jit.on(candidate_fn, true)
  exits, hfa_exits = 0, 0
  lib.arm64_aggregate_reset()
  _begin_hfa_witness()
  local value = ffi.new(type_name, 1.25)
  local before = _hfa_native_count()
  for pass = 1, 8 do
    assert(candidate_fn(16, ffi_fn, value, 3.75) ==
           oracle_fn(16, ffi_fn, value, 3.75), label..' differs from oracle')
  end
  assert(_hfa_trace_count() > 0, label..' root was not published')
  assert(_hfa_native_count() > before, label..' did not finish a native call')
  assert(hfa_exits > 0, label..' root did not take a native exit')
  assert(lib.arm64_aggregate_count(counter) == 256,
	 label..' effects were lost or replayed: counter='..tostring(counter)..
	 ', value='..tostring(lib.arm64_aggregate_count(counter)))
  before = _hfa_native_count()
  collectgarbage('collect')
  exits, hfa_exits = 0, 0
  assert(candidate_fn(32, ffi_fn, value, 3.75) ==
         oracle_fn(32, ffi_fn, value, 3.75), label..' differs after GC')
  assert(_hfa_trace_count() > 0, label..' root was lost after full GC')
  assert(_hfa_native_count() > before,
         label..' did not re-enter native code after full GC')
  assert(hfa_exits > 0, label..' did not exit after full-GC re-entry')
  assert(lib.arm64_aggregate_count(counter) == 320,
         label..' post-GC effects were lost or replayed')
end

run_one_hfa(float_one_candidate, float_one_oracle, 'Arm64FloatOne',
	    lib.arm64_take_float_one_double_salt, 12, 'one-float HFA')
run_one_hfa(double_one_candidate, double_one_oracle, 'Arm64DoubleOne',
	    lib.arm64_take_double_one_double_salt, 13, 'one-double HFA')

local function run_multilane_hfa_argument(candidate_fn, type_name, ffi_fn,
					  counter, values, label)
  hfa_candidate = candidate_fn
  _set_hfa_candidate(candidate_fn)
  jit.flush(candidate_fn)
  jit.on(candidate_fn, true)
  exits, hfa_exits = 0, 0
  lib.arm64_aggregate_reset()
  _begin_hfa_witness()
  local value = ffi.new(type_name, unpack(values))
  local before = _hfa_native_count()
  for pass = 1, 8 do
    assert(candidate_fn(16, ffi_fn, value, 3.75) ==
           hfa_oracle(16, ffi_fn, value, 3.75),
           label..' differs from interpreter oracle')
  end
  assert(_hfa_trace_count() > 0, label..' root was not published')
  assert(_hfa_native_count() > before,
         label..' did not execute the selected root natively')
  assert(hfa_exits > 0, label..' did not take a selected-root exit')
  assert(lib.arm64_aggregate_count(counter) == 256,
	 label..' effects were lost or replayed: counter='..counter..
	 ', value='..tostring(lib.arm64_aggregate_count(counter)))
  before = _hfa_native_count()
  collectgarbage('collect')
  exits, hfa_exits = 0, 0
  assert(candidate_fn(32, ffi_fn, value, 3.75) ==
         hfa_oracle(32, ffi_fn, value, 3.75),
         label..' differs after full GC')
  assert(_hfa_native_count() > before,
         label..' did not re-enter native code after full GC')
  assert(hfa_exits > 0, label..' did not exit after full-GC re-entry')
  assert(lib.arm64_aggregate_count(counter) == 320,
         label..' post-GC effects were lost or replayed')
end

run_multilane_hfa_argument(hfa_triple_candidate, 'Arm64FloatTriple',
	lib.arm64_take_float_triple_double_salt, 32,
	{1.25, 2.5, 3.75}, 'three-float HFA argument')
run_multilane_hfa_argument(hfa_quad_candidate, 'Arm64DoubleQuad',
	lib.arm64_take_double_quad_double_salt, 33,
	{1.125, 2.25, 3.5, 4.75}, 'four-double HFA argument')

candidate = integer_callm_candidate
_set_aggregate_candidate(integer_callm_candidate)
jit.flush(integer_callm_candidate)
jit.on(integer_callm_candidate, true)
exits = 0
lib.arm64_aggregate_reset()
local integer_callm_pair = ffi.new('Arm64IntPair', 7, 11)
local integer_callm_native_before = _aggregate_native_width_count(8)
for pass = 1, 8 do
  assert(integer_callm_candidate(16, lib.arm64_vararg_i32,
				 lib.arm64_take_int_pair, 5,
				 integer_callm_pair) ==
	 integer_callm_oracle(16, lib.arm64_vararg_i32,
			       lib.arm64_take_int_pair, 5,
			       integer_callm_pair),
	 'scalar vararg producer CALLM differs from oracle')
end
assert(exits > 0, 'scalar vararg producer CALLM root did not exit')
assert(_aggregate_native_width_count(8) > integer_callm_native_before,
	'scalar vararg producer CALLM did not finish a native aggregate call')
assert(lib.arm64_aggregate_count(0) == 256,
       'scalar vararg CALLM consumer effects were lost or replayed')
assert(lib.arm64_aggregate_count(14) == 256,
       'scalar vararg CALLM producer effects were lost or replayed')
candidate = assert(loadstring([[
return function(n, fn, value)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, i) end
  return sum
end
]], '@integer-aggregate-candidate-restored'))()
_G.__arm64_integer_aggregate_candidate = candidate
_set_aggregate_candidate(candidate)
jit.flush(candidate)
for turn = 1, 24 do
  local n = turn % 2 == 0 and 48 or 64
  local use = turn < 12 and fn or alt
  local got = candidate(n, use, pair)
  local want = oracle(n, use, pair)
  assert(got == want, 'native aggregate call disagrees with interpreter')
end
local native_before_gc = _aggregate_native_width_count(8)
collectgarbage('collect')
exits = 0
assert(candidate(32, fn, pair) == oracle(32, fn, pair))
assert(has_native_aggregate_root(22),
  'pair aggregate CALLXS root was not retained after GC')
assert(exits > 0, 'pair aggregate trace did not exit after GC reuse')
assert(_aggregate_native_width_count(8) > native_before_gc,
  'pair aggregate did not re-enter native code after GC')
jit.attach(observe, 'texit')

local function make_result_pair(label)
  local candidate_loader = assert(loadstring([[
return function(n, fn, seed)
  local result
  for i = 1, n do result = fn(seed+i) end
  return result
end
]], '@'..label..'-candidate-loader'))
  local oracle_loader = assert(loadstring([[
return function(n, fn, seed)
  local result
  for i = 1, n do result = fn(seed+i) end
  return result
end
]], '@'..label..'-oracle-loader'))
  local clone_loader = assert(loadstring([[
return function(n, fn, seed)
  local result
  for i = 1, n do result = fn(seed+i) end
  return result
end
]], '@'..label..'-clone-loader'))
  jit.off(oracle_loader, true)
  if not _G.__arm64_integer_result_clone then
    _G.__arm64_integer_result_clone = clone_loader()
    jit.off(_G.__arm64_integer_result_clone, true)
  end
  return candidate_loader(), oracle_loader()
end

local function run_integer_result(width, result_type, fn, counter, label)
  local result_candidate, result_oracle = make_result_pair(label)
  candidate = result_candidate
  _set_result_candidate(width, result_candidate)
  _G['__arm64_integer_result_candidate_'..width] = result_candidate
  jit.flush(result_candidate)
  jit.on(result_candidate, true)
  exits = 0
  local before_native = _aggregate_result_native_count(width)
  local before_effect = lib.arm64_aggregate_count(counter)
  for pass = 1, 6 do
    local got = result_candidate(32, fn, pass*7)
    local want = result_oracle(32, fn, pass*7)
    assert(ffi.istype(result_type, got) and ffi.istype(result_type, want),
           label..' returned the wrong declared CData type')
    assert(got.value == want.value, label..' result differs from interpreter')
  end
  assert(_aggregate_result_native_count(width) > before_native,
         label..' had no selected-root native CALLXS/CNEW/XSTORE finish')
  tracked_exit_roots = roots(result_candidate)
  assert(next(tracked_exit_roots), label..' published no selected loop root')
  local mismatched = result_candidate(32, lib.arm64_return_scalar, 13)
  assert(type(mismatched) == 'number',
         label..' mismatched-result fallback returned the wrong Lua type')
  assert(exits > 0, label..' selected result root did not force an exit')
  local native_before_gc = _aggregate_result_native_count(width)
  local effect_before_gc = lib.arm64_aggregate_count(counter)
  collectgarbage('collect')
  exits = 0
  local got = result_candidate(24, fn, 13)
  local want = result_oracle(24, fn, 13)
  assert(ffi.istype(result_type, got) and got.value == want.value,
         label..' result/root changed after full GC')
  assert(exits > 0, label..' selected result root did not exit after GC')
  assert(_aggregate_result_native_count(width) > native_before_gc,
         label..' did not re-enter native code after full GC')
  assert(lib.arm64_aggregate_count(counter)-effect_before_gc == 48,
         label..' native/interpreter effects were replayed after GC')
  assert(lib.arm64_aggregate_count(counter)-before_effect == 432,
         label..' native/interpreter effects were lost or replayed')
end

local function run_hfa_result(width, result_type, fn, counter, label)
  local result_candidate, result_oracle = make_result_pair(label)
  _set_hfa_result_candidate(width, result_candidate)
  _G['__arm64_hfa_result_candidate_'..width] = result_candidate
  jit.flush(result_candidate)
  jit.on(result_candidate, true)
  exits = 0
  local before_native = _hfa_result_native_count(width)
  local before_effect = lib.arm64_aggregate_count(counter)
  for pass = 1, 6 do
    local got = result_candidate(32, fn, pass*7)
    local want = result_oracle(32, fn, pass*7)
    assert(ffi.istype(result_type, got) and ffi.istype(result_type, want),
           label..' returned the wrong declared CData type')
    assert(got.value == want.value, label..' result differs from interpreter')
  end
  assert(_hfa_result_native_count(width) > before_native,
         label..' had no selected-root native CALLXS/CNEW/XSTORE finish')
  tracked_exit_roots = roots(result_candidate)
  assert(next(tracked_exit_roots), label..' published no selected loop root')
  local mismatched = result_candidate(32, lib.arm64_return_scalar, 13)
  assert(type(mismatched) == 'number',
         label..' mismatched-result fallback returned the wrong Lua type')
  assert(exits > 0, label..' selected result root did not force an exit')
  local native_before_gc = _hfa_result_native_count(width)
  local effect_before_gc = lib.arm64_aggregate_count(counter)
  collectgarbage('collect')
  exits = 0
  local got = result_candidate(24, fn, 13)
  local want = result_oracle(24, fn, 13)
  assert(ffi.istype(result_type, got) and got.value == want.value,
         label..' result/root changed after full GC')
  assert(exits > 0, label..' selected result root did not exit after GC')
  assert(_hfa_result_native_count(width) > native_before_gc,
         label..' did not re-enter native code after full GC')
  assert(lib.arm64_aggregate_count(counter)-effect_before_gc == 48,
         label..' native/interpreter effects were replayed after GC')
  assert(lib.arm64_aggregate_count(counter)-before_effect == 432,
         label..' native/interpreter effects were lost or replayed')
end

run_integer_result(1, 'Arm64ByteResult', lib.arm64_return_byte, 15,
                   'one-byte aggregate result')
run_integer_result(2, 'Arm64HalfResult', lib.arm64_return_half, 16,
                   'two-byte aggregate result')
run_integer_result(4, 'Arm64WordResult', lib.arm64_return_word, 17,
                   'four-byte aggregate result')
run_integer_result(4, 'Arm64WordResultAlias',
                   lib.arm64_return_word_alias, 20,
                   'typedef four-byte aggregate result')
run_integer_result(8, 'Arm64U64Result', lib.arm64_return_u64, 18,
                   'eight-byte aggregate result')
run_hfa_result(4, 'Arm64FloatResult', lib.arm64_return_float_hfa, 21,
               'one-float HFA result')
run_hfa_result(8, 'Arm64DoubleResult', lib.arm64_return_double_hfa, 22,
               'one-double HFA result')
local function run_multi_hfa_result(width, result_type, fn, counter, fields,
                                    expected, label)
  local candidate, oracle = make_result_pair(label)
  _set_hfa_result_candidate(width, candidate)
  _G['__arm64_hfa_result_candidate_'..width] = candidate
  jit.flush(candidate)
  jit.on(candidate, true)
  local native_before = _hfa_result_native_count(width)
  local effects_before = lib.arm64_aggregate_count(counter)
  for pass = 1, 6 do
    local got = candidate(32, fn, pass*7)
    local want = oracle(32, fn, pass*7)
    assert(ffi.istype(result_type, got) and ffi.istype(result_type, want),
           label..' returned the wrong declared CType')
    for i, field in ipairs(fields) do
      assert(got[field] == want[field], label..' lane '..field..' differs')
    end
  end
  assert(_hfa_result_native_count(width) > native_before,
         label..' had no selected-root native entry')
  tracked_exit_roots = roots(candidate)
  exits = 0
  local mismatch = candidate(32, lib.arm64_return_scalar, 13)
  assert(type(mismatch) == 'number' and exits > 0,
         label..' did not take a result-signature exit')
  local after_gc = _hfa_result_native_count(width)
  local effects_after_gc = lib.arm64_aggregate_count(counter)
  collectgarbage('collect')
  exits = 0
  local got = candidate(24, fn, 13)
  local want = oracle(24, fn, 13)
  assert(ffi.istype(result_type, got) and exits > 0 and
         _hfa_result_native_count(width) > after_gc,
         label..' failed native reentry/exit after full GC')
  for i, field in ipairs(fields) do
    assert(got[field] == want[field] and
           got[field] == expected(37)[i],
           label..' lane '..field..' changed after full GC')
  end
  assert(lib.arm64_aggregate_count(counter)-effects_after_gc == 48 and
         lib.arm64_aggregate_count(counter)-effects_before == 432,
         label..' native/interpreter effects were lost or replayed')
end
run_multi_hfa_result(32, 'Arm64FloatTripleResult',
  lib.arm64_return_float_triple_hfa, 28, {'x','y','z'},
  function(n) return {n+.25, 2*n+.5, 3*n+.75} end,
  'three-float HFA result')
run_multi_hfa_result(64, 'Arm64FloatQuadResult',
  lib.arm64_return_float_quad_hfa, 29, {'x','y','z','w'},
  function(n) return {n+.25, 2*n+.5, 3*n+.75, 4*n+1} end,
  'four-float HFA result')
run_multi_hfa_result(24, 'Arm64DoubleTripleResult',
  lib.arm64_return_double_triple_hfa, 30, {'x','y','z'},
  function(n) return {n+.125, 2*n+.25, 3*n+.5} end,
  'three-double HFA result')
run_multi_hfa_result(40, 'Arm64DoubleQuadResult',
  lib.arm64_return_double_quad_hfa, 31, {'x','y','z','w'},
  function(n) return {n+.125, 2*n+.25, 3*n+.5, 4*n+1} end,
  'four-double HFA result')
local pair_candidate, pair_oracle = make_result_pair('two-float HFA result')
_set_hfa_result_candidate(12, pair_candidate)
_G.__arm64_hfa_result_candidate_12 = pair_candidate
jit.flush(pair_candidate)
jit.on(pair_candidate, true)
local pair_before = _hfa_result_native_count(12)
local pair_effect_before = lib.arm64_aggregate_count(23)
for pass = 1, 6 do
  local got = pair_candidate(32, lib.arm64_return_float_pair_hfa, pass*7)
  local want = pair_oracle(32, lib.arm64_return_float_pair_hfa, pass*7)
  assert(ffi.istype('Arm64FloatPairResult', got) and
         ffi.istype('Arm64FloatPairResult', want),
         'two-float HFA returned the wrong declared CData type')
  assert(got.lo == want.lo and got.hi == want.hi,
         'two-float HFA result differs from interpreter')
end
assert(_hfa_result_native_count(12) > pair_before,
       'two-float HFA had no selected-root native CALLXS/HIOP finish')
tracked_exit_roots = roots(pair_candidate)
exits = 0
local pair_mismatch = pair_candidate(32, lib.arm64_return_scalar, 13)
assert(type(pair_mismatch) == 'number',
       'two-float HFA mismatched-result fallback returned the wrong Lua type')
assert(exits > 0, 'two-float HFA selected result root did not force an exit')
local pair_native_before_gc = _hfa_result_native_count(12)
local pair_effect_before_gc = lib.arm64_aggregate_count(23)
collectgarbage('collect')
exits = 0
local pair_got = pair_candidate(24, lib.arm64_return_float_pair_hfa, 13)
local pair_want = pair_oracle(24, lib.arm64_return_float_pair_hfa, 13)
assert(ffi.istype('Arm64FloatPairResult', pair_got) and
       pair_got.lo == pair_want.lo and pair_got.hi == pair_want.hi,
       'two-float HFA result/root changed after full GC')
assert(_hfa_result_native_count(12) > pair_native_before_gc,
       'two-float HFA did not re-enter native code after full GC')
tracked_exit_roots = roots(pair_candidate)
local pair_mismatch_after_gc = pair_candidate(32, lib.arm64_return_scalar, 15)
assert(type(pair_mismatch_after_gc) == 'number' and exits > 0,
       'two-float HFA did not take a forced exit after GC')
assert(lib.arm64_aggregate_count(23)-pair_effect_before_gc == 48,
       'two-float HFA native/interpreter effects were replayed after GC')
assert(lib.arm64_aggregate_count(23)-pair_effect_before == 432,
       'two-float HFA native/interpreter effects were lost or replayed')
local raw_pair_edges = {
  string.char(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80),
  string.char(0x00, 0x00, 0x80, 0x7f, 0x00, 0x00, 0x80, 0xff),
  string.char(0x45, 0x23, 0xc1, 0x7f, 0x21, 0x43, 0xc5, 0x7f),
}
for edge, seed in ipairs({-30, -29, -28}) do
  local native_before = _hfa_result_native_count(12)
  -- The final iteration returns special C payloads 2, 3 and 4 while the
  -- candidate loop remains long enough to enter the selected native root.
  local got = pair_candidate(32, lib.arm64_return_float_pair_hfa, seed)
  local want = pair_oracle(32, lib.arm64_return_float_pair_hfa, seed)
  assert(ffi.string(got, ffi.sizeof('Arm64FloatPairResult')) ==
         ffi.string(want, ffi.sizeof('Arm64FloatPairResult')) and
         ffi.string(got, ffi.sizeof('Arm64FloatPairResult')) ==
         raw_pair_edges[edge],
         'two-float HFA did not preserve exact lane bits')
  assert(_hfa_result_native_count(12) > native_before,
         'two-float HFA edge-lane case did not execute the selected native root')
end
local double_pair_candidate, double_pair_oracle =
  make_result_pair('two-double HFA result')
_set_hfa_result_candidate(16, double_pair_candidate)
_G.__arm64_hfa_result_candidate_16 = double_pair_candidate
jit.flush(double_pair_candidate)
jit.on(double_pair_candidate, true)
local double_pair_native_before = _hfa_result_native_count(16)
local double_pair_effect_before = lib.arm64_aggregate_count(26)
for pass = 1, 6 do
  local got = double_pair_candidate(32, lib.arm64_return_double_pair_hfa,
                                    pass*7)
  local want = double_pair_oracle(32, lib.arm64_return_double_pair_hfa,
                                  pass*7)
  assert(ffi.istype('Arm64DoublePairResult', got) and
         ffi.istype('Arm64DoublePairResult', want) and
         got.lo == want.lo and got.hi == want.hi,
         'two-double HFA result differs from interpreter/type')
end
assert(_hfa_result_native_count(16) > double_pair_native_before,
       'two-double HFA result had no selected native CALLXS/HIOP finish')
tracked_exit_roots = roots(double_pair_candidate)
exits = 0
assert(type(double_pair_candidate(32, lib.arm64_return_scalar, 13)) == 'number' and
       exits > 0, 'two-double HFA result did not take a signature exit')
local double_pair_native_before_gc = _hfa_result_native_count(16)
local double_pair_effect_before_gc = lib.arm64_aggregate_count(26)
collectgarbage('collect')
exits = 0
local double_pair_got = double_pair_candidate(
    24, lib.arm64_return_double_pair_hfa, 13)
local double_pair_want = double_pair_oracle(
    24, lib.arm64_return_double_pair_hfa, 13)
assert(ffi.istype('Arm64DoublePairResult', double_pair_got) and
       double_pair_got.lo == double_pair_want.lo and
       double_pair_got.hi == double_pair_want.hi and exits > 0 and
       _hfa_result_native_count(16) > double_pair_native_before_gc,
       'two-double HFA result failed forced exit/native reentry after GC')
assert(lib.arm64_aggregate_count(26)-double_pair_effect_before_gc == 48 and
       lib.arm64_aggregate_count(26)-double_pair_effect_before == 432,
       'two-double HFA native/interpreter effects were lost or replayed')
local raw_double_pair_edges = {
  string.char(0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,128),
  string.char(0,0,0,0,0,0,240,127, 0,0,0,0,0,0,240,255),
  string.char(188,154,120,86,52,18,248,127,
              84,118,152,186,220,254,248,127),
}
for edge, seed in ipairs({-30, -29, -28}) do
  local before = _hfa_result_native_count(16)
  local got = double_pair_candidate(32, lib.arm64_return_double_pair_hfa,
                                    seed)
  local want = double_pair_oracle(32, lib.arm64_return_double_pair_hfa,
                                  seed)
  local bytes = ffi.string(got, ffi.sizeof('Arm64DoublePairResult'))
  assert(bytes == ffi.string(want, ffi.sizeof('Arm64DoublePairResult')) and
         bytes == raw_double_pair_edges[edge] and
         _hfa_result_native_count(16) > before,
         'two-double HFA edge bits lacked oracle/native agreement')
end
_set_hfa_result_candidate(16, double_pair_candidate)

local double_fpr_candidate_loader = assert(loadstring([[
return function(n, fn, seed, bias)
  local result
  for i = 1, n do result = fn(bias, seed+i) end
  return result
end
]], '@two-double-HFA-FPR-result-candidate-loader'))
local double_fpr_oracle_loader = assert(loadstring([[
return function(n, fn, seed, bias)
  local result
  for i = 1, n do result = fn(bias, seed+i) end
  return result
end
]], '@two-double-HFA-FPR-result-oracle-loader'))
jit.off(double_fpr_oracle_loader, true)
local double_fpr_candidate, double_fpr_oracle =
  double_fpr_candidate_loader(), double_fpr_oracle_loader()
_set_hfa_result_candidate(16, double_fpr_candidate)
_G.__arm64_hfa_result_candidate_16 = double_fpr_candidate
jit.flush(double_fpr_candidate)
jit.on(double_fpr_candidate, true)
local double_fpr_native_before = _hfa_result_native_count(16)
local double_fpr_effect_before = lib.arm64_aggregate_count(27)
for pass = 1, 6 do
  local got = double_fpr_candidate(32,
      lib.arm64_return_double_pair_hfa_fpr, pass*7, 1.5)
  local want = double_fpr_oracle(32,
      lib.arm64_return_double_pair_hfa_fpr, pass*7, 1.5)
  assert(ffi.istype('Arm64DoublePairResult', got) and
         ffi.string(got, ffi.sizeof('Arm64DoublePairResult')) ==
         ffi.string(want, ffi.sizeof('Arm64DoublePairResult')),
         'two-double HFA with live D0 argument differs from oracle')
end
assert(_hfa_result_native_count(16) > double_fpr_native_before,
       'two-double HFA with live D0 input did not enter its selected root')
tracked_exit_roots = roots(double_fpr_candidate)
exits = 0
assert(type(double_fpr_candidate(32,
    lib.arm64_return_float_pair_scalar_fpr, 13, 1.5)) == 'number' and
    exits > 0, 'two-double HFA FPR-input root did not force a signature exit')
local double_fpr_before_gc = _hfa_result_native_count(16)
local double_fpr_effect_before_gc = lib.arm64_aggregate_count(27)
collectgarbage('collect')
exits = 0
local double_fpr_got = double_fpr_candidate(24,
    lib.arm64_return_double_pair_hfa_fpr, 13, 1.5)
local double_fpr_want = double_fpr_oracle(24,
    lib.arm64_return_double_pair_hfa_fpr, 13, 1.5)
assert(ffi.string(double_fpr_got, ffi.sizeof('Arm64DoublePairResult')) ==
       ffi.string(double_fpr_want, ffi.sizeof('Arm64DoublePairResult')) and
       _hfa_result_native_count(16) > double_fpr_before_gc and exits > 0,
       'two-double HFA FPR-input result failed native GC re-entry/exit')
assert(lib.arm64_aggregate_count(27)-double_fpr_effect_before_gc == 48 and
       lib.arm64_aggregate_count(27)-double_fpr_effect_before == 432,
       'two-double HFA FPR-input effects were lost or replayed')
_set_hfa_result_candidate(16, double_pair_candidate)
_G.__arm64_hfa_result_candidate_16 = double_pair_candidate

local fpr_candidate_loader = assert(loadstring([[
return function(n, fn, seed, bias)
  local result
  for i = 1, n do result = fn(bias, seed+i) end
  return result
end
]], '@two-float-HFA-FPR-result-candidate-loader'))
local fpr_oracle_loader = assert(loadstring([[
return function(n, fn, seed, bias)
  local result
  for i = 1, n do result = fn(bias, seed+i) end
  return result
end
]], '@two-float-HFA-FPR-result-oracle-loader'))
jit.off(fpr_oracle_loader, true)
local pair_fpr_candidate, pair_fpr_oracle = fpr_candidate_loader(),
                                                    fpr_oracle_loader()
_set_hfa_result_candidate(12, pair_fpr_candidate)
_G.__arm64_hfa_result_candidate_12 = pair_fpr_candidate
jit.flush(pair_fpr_candidate)
jit.on(pair_fpr_candidate, true)
local fpr_before = _hfa_result_native_count(12)
local fpr_effect_before = lib.arm64_aggregate_count(24)
for pass = 1, 6 do
  local got = pair_fpr_candidate(32, lib.arm64_return_float_pair_hfa_fpr,
                                 pass*7, 1.5)
  local want = pair_fpr_oracle(32, lib.arm64_return_float_pair_hfa_fpr,
                               pass*7, 1.5)
  assert(ffi.istype('Arm64FloatPairResult', got) and
         ffi.string(got, ffi.sizeof('Arm64FloatPairResult')) ==
         ffi.string(want, ffi.sizeof('Arm64FloatPairResult')),
         'two-float HFA with FPR argument differs from interpreter')
end
assert(_hfa_result_native_count(12) > fpr_before,
       'two-float HFA with a live D0 argument had no native root')
tracked_exit_roots = roots(pair_fpr_candidate)
exits = 0
local fpr_mismatch = pair_fpr_candidate(32,
    lib.arm64_return_float_pair_scalar_fpr, 13, 1.5)
assert(type(fpr_mismatch) == 'number' and exits > 0,
       'two-float HFA FPR-input root did not force a signature exit')
local fpr_native_before_gc = _hfa_result_native_count(12)
local fpr_effect_before_gc = lib.arm64_aggregate_count(24)
collectgarbage('collect')
exits = 0
local fpr_got = pair_fpr_candidate(24,
    lib.arm64_return_float_pair_hfa_fpr, 13, 1.5)
local fpr_want = pair_fpr_oracle(24,
    lib.arm64_return_float_pair_hfa_fpr, 13, 1.5)
assert(ffi.string(fpr_got, ffi.sizeof('Arm64FloatPairResult')) ==
       ffi.string(fpr_want, ffi.sizeof('Arm64FloatPairResult')) and
       _hfa_result_native_count(12) > fpr_native_before_gc,
       'two-float HFA with FPR argument did not re-enter after GC')
tracked_exit_roots = roots(pair_fpr_candidate)
local fpr_mismatch_after_gc = pair_fpr_candidate(32,
    lib.arm64_return_float_pair_scalar_fpr, 15, 1.5)
assert(type(fpr_mismatch_after_gc) == 'number' and exits > 0,
       'two-float HFA FPR-input root did not force an exit after GC')
assert(lib.arm64_aggregate_count(24)-fpr_effect_before_gc == 48 and
       lib.arm64_aggregate_count(24)-fpr_effect_before == 432,
       'two-float HFA FPR-input effects were lost or replayed')
for edge, seed in ipairs({-30, -29, -28}) do
  local native_before = _hfa_result_native_count(12)
  local got = pair_fpr_candidate(32,
      lib.arm64_return_float_pair_hfa_fpr, seed, 1.5)
  local want = pair_fpr_oracle(32,
      lib.arm64_return_float_pair_hfa_fpr, seed, 1.5)
  assert(ffi.string(got, ffi.sizeof('Arm64FloatPairResult')) ==
         ffi.string(want, ffi.sizeof('Arm64FloatPairResult')) and
         ffi.string(got, ffi.sizeof('Arm64FloatPairResult')) ==
         raw_pair_edges[edge],
         'two-float HFA FPR-input case changed exact lane bits')
  assert(_hfa_result_native_count(12) > native_before,
         'two-float HFA FPR edge case did not execute the native root')
end
_set_hfa_result_candidate(12, pair_candidate)
_G.__arm64_hfa_result_candidate_12 = pair_candidate

local candidate_roots = roots()
for no in pairs(candidate_roots) do
  local info = jitutil.traceinfo(no)
  if info then
    local count = 0
    for ref = 1, info.nins do
      local _, ot = jitutil.traceir(no, ref)
      if ot then
        local pos = 6*bit.rshift(ot, 8)
        if vmdef.irnames:sub(pos+1, pos+6) == 'CALLXS' then
          count = count + 1
        end
      end
    end
    if count > 0 then selected[no] = count end
  end
end
assert(next(selected), 'no selected candidate root contains CALLXS')
assert(exits > 0, 'selected candidate root did not take a native exit')
assert(lib.arm64_aggregate_count(0) == 1568,
  'integer aggregate effects were lost or replayed: ' ..
  tostring(lib.arm64_aggregate_count(0)))
assert(lib.arm64_aggregate_count(1) == 1440,
  'alternate-signature effects were lost or replayed: ' ..
  tostring(lib.arm64_aggregate_count(1)))


assert(stop:send(true, 5))
local joined, result = peer:join(5)
assert(joined and result, 'live peer did not stop and join')
