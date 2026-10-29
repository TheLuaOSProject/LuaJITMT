local ffi = require('ffi')
local jitutil = require('jit.util')
local vmdef = require('jit.vmdef')
local bit = require('bit')
local threading = require('threading')

jit.off(true, true)
ffi.cdef[[
typedef struct { uint32_t lo, hi; } Arm64IntPair;
typedef struct { float lo, hi; } Arm64FloatPair;
typedef struct { Arm64FloatPair inner; } Arm64NestedFloatPair;
typedef struct { float lane[2]; } Arm64ArrayFloatPair;
typedef struct __attribute__((aligned(8))) {
  float lo, hi;
} Arm64OverAlignedNestedPair;
typedef struct { float head; Arm64OverAlignedNestedPair tail; }
  Arm64NestedOverAlignedPair;
typedef union { float lo, hi; } Arm64FloatUnion;
typedef struct { Arm64FloatUnion inner; } Arm64NestedFloatUnion;
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
typedef double (*Arm64NestedFloatPairFn)(Arm64NestedFloatPair, double);
typedef Arm64NestedFloatPair (*Arm64NestedFloatPairResultFn)(uint32_t);
typedef double (*Arm64NestedOverAlignedPairFn)(Arm64NestedOverAlignedPair,
                                                double);
typedef double (*Arm64NestedFloatUnionFn)(Arm64NestedFloatUnion, double);
typedef double (*Arm64ArrayFloatPairFn)(Arm64ArrayFloatPair, double);
typedef double (*Arm64DoubleQuadSaltFn)(Arm64DoubleQuad, double);
typedef double (*Arm64FloatTripleRefFn)(Arm64FloatTripleRef, double);
typedef double (*Arm64FloatPairStackFn)(double, double, double, double,
                                        double, double, double, double,
                                        Arm64FloatPair, double);
typedef double (*Arm64FloatPairPartialStackFn)(double, double, double, double,
                                               double, double, double,
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
double arm64_take_nested_float_pair(Arm64NestedFloatPair, double);
double arm64_take_nested_float_pair_alt(Arm64NestedFloatPair, double);
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
double arm64_take_float_pair_partial_stack(double, double, double, double,
                                           double, double, double,
                                           Arm64FloatPair, double);
double arm64_take_float_pair_partial_stack_alt(double, double, double, double,
                                               double, double, double,
                                               Arm64FloatPair, double);
double arm64_take_small_hfa_stack(double, double, double, double,
  double, double, double, double, float, Arm64FloatOne, float,
  Arm64FloatTriple, float);
double arm64_take_small_hfa_stack_alt(double, double, double, double,
  double, double, double, double, float, Arm64FloatOne, float,
  Arm64FloatTriple, float);
double arm64_take_float_triple_partial_stack(double, double, double, double,
  double, double, double, Arm64FloatTriple, double);
double arm64_take_float_one_stack_prefix(double, double, double, double,
  double, double, double, double, float, Arm64FloatOne, float);
double arm64_take_float_quad_stack(double,double,double,double,
  double,double,double,double,Arm64FloatQuadResult,double);
double arm64_take_float_quad_stack_alt(double,double,double,double,
  double,double,double,double,Arm64FloatQuadResult,double);
double arm64_take_double_pair_stack(double,double,double,double,
  double,double,double,double,Arm64DoublePairResult,double);
double arm64_take_double_pair_stack_alt(double,double,double,double,
  double,double,double,double,Arm64DoublePairResult,double);
double arm64_take_double_triple_stack(double,double,double,double,
  double,double,double,double,Arm64DoubleTripleResult,double);
double arm64_take_double_triple_stack_alt(double,double,double,double,
  double,double,double,double,Arm64DoubleTripleResult,double);
double arm64_take_double_quad_stack(double,double,double,double,
  double,double,double,double,Arm64DoubleQuadResult,double);
double arm64_take_double_quad_stack_alt(double,double,double,double,
  double,double,double,double,Arm64DoubleQuadResult,double);
double arm64_take_double_one_stack(double,double,double,double,
  double,double,double,double,Arm64DoubleResult,double);
double arm64_take_double_one_stack_alt(double,double,double,double,
  double,double,double,double,Arm64DoubleResult,double);
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
uint32_t arm64_take_byte_stack(uint32_t,uint32_t,uint32_t,uint32_t,
  uint32_t,uint32_t,uint32_t,uint32_t,uint8_t,Arm64Byte,uint32_t);
uint32_t arm64_take_half_stack(uint32_t,uint32_t,uint32_t,uint32_t,
  uint32_t,uint32_t,uint32_t,uint32_t,uint8_t,Arm64Half,uint32_t);
uint32_t arm64_take_word_stack(uint32_t,uint32_t,uint32_t,uint32_t,
  uint32_t,uint32_t,uint32_t,uint32_t,uint8_t,Arm64Word,uint32_t);
uint32_t arm64_take_byte_stack_alt(uint32_t,uint32_t,uint32_t,uint32_t,
  uint32_t,uint32_t,uint32_t,uint32_t,uint8_t,Arm64Byte,uint32_t);
uint32_t arm64_take_half_stack_alt(uint32_t,uint32_t,uint32_t,uint32_t,
  uint32_t,uint32_t,uint32_t,uint32_t,uint8_t,Arm64Half,uint32_t);
uint32_t arm64_take_word_stack_alt(uint32_t,uint32_t,uint32_t,uint32_t,
  uint32_t,uint32_t,uint32_t,uint32_t,uint8_t,Arm64Word,uint32_t);
uint32_t arm64_take_narrow_constants_stack(uint32_t,uint32_t,uint32_t,uint32_t,
  uint32_t,uint32_t,uint32_t,uint32_t,uint8_t,int8_t,uint16_t,int16_t,
  Arm64Byte,uint8_t,int8_t,uint16_t,int16_t,uint32_t);
uint32_t arm64_take_narrow_constants_stack_alt(uint32_t,uint32_t,uint32_t,uint32_t,
  uint32_t,uint32_t,uint32_t,uint32_t,uint8_t,int8_t,uint16_t,int16_t,
  Arm64Byte,uint8_t,int8_t,uint16_t,int16_t,uint32_t);
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
_G.__arm64_nested_float_pair_fn = ffi.cast('Arm64NestedFloatPairFn', nil)
_G.__arm64_nested_float_pair_result_fn =
  ffi.cast('Arm64NestedFloatPairResultFn', nil)
_G.__arm64_nested_overaligned_pair_fn =
  ffi.cast('Arm64NestedOverAlignedPairFn', nil)
_G.__arm64_nested_float_union_fn =
  ffi.cast('Arm64NestedFloatUnionFn', nil)
_G.__arm64_array_float_pair_fn =
  ffi.cast('Arm64ArrayFloatPairFn', nil)
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
_G.__arm64_partial_hfa_stack_fn = lib.arm64_take_float_pair_partial_stack
_G.__arm64_partial_hfa_stack_alt_fn =
  lib.arm64_take_float_pair_partial_stack_alt
_G.__arm64_small_hfa_stack_fn = lib.arm64_take_small_hfa_stack
_G.__arm64_small_hfa_stack_alt_fn = lib.arm64_take_small_hfa_stack_alt
_G.__arm64_float_triple_partial_stack_fn =
  lib.arm64_take_float_triple_partial_stack
_G.__arm64_float_one_stack_prefix_fn = lib.arm64_take_float_one_stack_prefix

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
-- Keep earlier rejected-call penalties out of this independent warmup.
jit.opt.start('hotloop=1')
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

-- One free FPR is insufficient for this two-lane HFA. AAPCS64 spills both
-- lanes and closes the FPR bank, so the trailing double is also stack-passed.
local function partial_hfa_source()
  return assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do
    sum = sum + fn(1, 2, 3, 4, 5, 6, 7, value, salt)
  end
  return sum
end
]], '@partial-float-pair-hfa-stack'))()
end
local partial_hfa_candidate = partial_hfa_source()
local partial_hfa_oracle = partial_hfa_source()
local partial_hfa_clone = partial_hfa_source()
jit.off(partial_hfa_oracle, true); jit.off(partial_hfa_clone, true)
local partial_pair = ffi.new('Arm64FloatPair', 1.25, 2.5)
local partial_fn = lib.arm64_take_float_pair_partial_stack
local partial_saved_candidate = hfa_candidate
-- Losing the ABI plan after semantic admission must abort both stack sizing
-- and emission. A successful later read cannot repair already split lanes.
for stage = 1, 2 do
  local rejected = partial_hfa_source()
  hfa_candidate = rejected
  _set_hfa_candidate(rejected)
  jit.flush(rejected)
  jit.opt.start('hotloop=1')
  jit.on(rejected, true)
  lib.arm64_aggregate_reset()
  _begin_hfa_witness()
  _inject_callx_abi_failure(stage)
  for pass = 1, 8 do
    assert(rejected(16, partial_fn, partial_pair, 3.75) ==
           partial_hfa_oracle(16, partial_fn, partial_pair, 3.75),
           'missing ABI plan changed interpreter results')
  end
  assert(_inject_callx_abi_failure(0), 'ABI failure injection was not reached')
  assert(_hfa_trace_count() == 0 and _hfa_native_count() == 0,
         'missing ABI plan published or executed a CALLXS root')
  assert(lib.arm64_aggregate_count(48) == 256,
         'missing ABI plan lost or replayed call effects')
end
_G.__arm64_partial_hfa_stack_candidate = partial_hfa_candidate
_G.__arm64_partial_hfa_stack_clone = partial_hfa_clone
hfa_candidate = partial_hfa_candidate
_G.__arm64_float_hfa_candidate = hfa_candidate
_set_hfa_candidate(hfa_candidate)
jit.flush(hfa_candidate)
jit.opt.start('hotloop=1')
jit.on(hfa_candidate, true)
exits, hfa_exits = 0, 0
lib.arm64_aggregate_reset()
_begin_hfa_witness()
local partial_native_before = _hfa_native_count()
for pass = 1, 8 do
  local got = hfa_candidate(16, partial_fn, partial_pair, 3.75)
  local want = partial_hfa_oracle(16, partial_fn, partial_pair, 3.75)
  assert(got == want,
         'partially spilled HFA differs from interpreter: got=' ..
         tostring(got) .. ' want=' .. tostring(want))
end
assert(_hfa_trace_count() > 0 and
       _hfa_native_count() > partial_native_before and hfa_exits > 0,
       'partially spilled HFA root did not finish and exit natively')
assert(lib.arm64_aggregate_count(48) == 256,
       'partial HFA effects were lost or replayed: ' ..
       tostring(lib.arm64_aggregate_count(48)))
partial_native_before = _hfa_native_count()
collectgarbage('collect')
exits, hfa_exits = 0, 0
assert(hfa_candidate(32, partial_fn, partial_pair, 3.75) ==
       partial_hfa_oracle(32, partial_fn, partial_pair, 3.75),
       'partially spilled HFA changed after full GC')
assert(_hfa_native_count() > partial_native_before and hfa_exits > 0,
       'partially spilled HFA did not re-enter after full GC')
assert(lib.arm64_aggregate_count(48) == 320,
       'partial HFA post-GC effects were lost or replayed')
tracked_exit_roots = roots(hfa_candidate)
exits = 0
assert(hfa_candidate(32, lib.arm64_take_float_pair_partial_stack_alt,
                     partial_pair, 3.75) ==
       partial_hfa_oracle(32,
         lib.arm64_take_float_pair_partial_stack_alt, partial_pair, 3.75),
       'partial HFA callable-change fallback differs')
assert(exits > 0 and lib.arm64_aggregate_count(49) == 64,
       'partial HFA callable change did not exit or replayed effects')
tracked_exit_roots = nil
hfa_candidate = partial_saved_candidate
_G.__arm64_float_hfa_candidate = hfa_candidate
_set_hfa_candidate(hfa_candidate)

-- Eight doubles consume v0-v7. Darwin then packs a scalar float, a one-float
-- HFA, another scalar, a three-float HFA, and a final scalar at four-byte
-- stack boundaries. The independent C oracle returns weighted inputs so a
-- wrong group start or lane offset changes the result.
local function small_hfa_stack_source()
  return assert(loadstring([[
return function(n, fn, one, triple)
  local sum = .5
  for i = 1, n do
    sum = sum + fn(1,2,3,4,5,6,7,8, 1.5, one, 2.5, triple, 3.5)
  end
  return sum
end
]], '@small-hfa-stack'))()
end
local small_hfa_candidate = small_hfa_stack_source()
local small_hfa_oracle = small_hfa_stack_source()
local small_hfa_clone = small_hfa_stack_source()
jit.off(small_hfa_oracle, true); jit.off(small_hfa_clone, true)
_G.__arm64_small_hfa_candidate = small_hfa_candidate
_G.__arm64_small_hfa_clone = small_hfa_clone
local small_one = ffi.new('Arm64FloatOne', 1.25)
local small_triple = ffi.new('Arm64FloatTriple', 2.25, 3.25, 4.25)
local small_hfa_fn = lib.arm64_take_small_hfa_stack
hfa_candidate = small_hfa_candidate
_set_hfa_candidate(small_hfa_candidate)
jit.flush(small_hfa_candidate)
jit.opt.start('hotloop=1', 'hotexit=1000')
jit.on(small_hfa_candidate, true)
exits, hfa_exits = 0, 0
lib.arm64_aggregate_reset()
_begin_hfa_witness()
local small_native_before = _hfa_native_count()
for pass = 1, 8 do
  assert(small_hfa_candidate(16, small_hfa_fn, small_one, small_triple) ==
         small_hfa_oracle(16, small_hfa_fn, small_one, small_triple),
         'small HFA stack groups differ from the C ABI oracle')
end
assert(_hfa_trace_count() > 0 and _hfa_native_count() > small_native_before and
       hfa_exits > 0,
       'small HFA stack groups did not publish, execute, and exit natively')
assert(lib.arm64_aggregate_count(56) == 256,
       'small HFA stack group effects were lost or replayed')
small_native_before = _hfa_native_count()
collectgarbage('collect')
exits, hfa_exits = 0, 0
assert(small_hfa_candidate(32, small_hfa_fn, small_one, small_triple) ==
       small_hfa_oracle(32, small_hfa_fn, small_one, small_triple),
       'small HFA stack groups changed after full GC')
assert(_hfa_native_count() > small_native_before and hfa_exits > 0,
       'small HFA stack groups did not re-enter and exit after full GC')
assert(lib.arm64_aggregate_count(56) == 320,
       'small HFA stack post-GC effects were lost or replayed')
hfa_candidate = saved_hfa_candidate
_set_hfa_candidate(hfa_candidate)

local function one_stack_hfa_source()
  return assert(loadstring([[
return function(n, fn, one)
  local sum=.5
  for i=1,n do sum=sum+fn(1,2,3,4,5,6,7,8,1.5,one,2.5) end
  return sum
end
]], '@one-float-hfa-stack'))()
end
local one_stack_candidate = one_stack_hfa_source()
local one_stack_oracle = one_stack_hfa_source()
local one_stack_clone = one_stack_hfa_source()
jit.off(one_stack_oracle, true); jit.off(one_stack_clone, true)
_G.__arm64_one_hfa_stack_candidate = one_stack_candidate
_G.__arm64_one_hfa_stack_clone = one_stack_clone
local one_stack_fn = lib.arm64_take_float_one_stack_prefix
local one_stack_value = ffi.new('Arm64FloatOne', 1.25)
hfa_candidate = one_stack_candidate
_set_hfa_candidate(one_stack_candidate)
jit.flush(one_stack_candidate)
jit.opt.start('hotloop=1', 'hotexit=1000')
jit.on(one_stack_candidate, true)
exits, hfa_exits = 0, 0
lib.arm64_aggregate_reset()
_begin_hfa_witness()
local one_stack_native_before = _hfa_native_count()
for pass = 1, 8 do
  assert(one_stack_candidate(16, one_stack_fn, one_stack_value) ==
         one_stack_oracle(16, one_stack_fn, one_stack_value),
         'one-float HFA between stack scalars differs from C oracle')
end
assert(_hfa_trace_count() > 0 and
       _hfa_native_count() > one_stack_native_before and hfa_exits > 0,
       'one-float HFA stack case did not execute and exit natively')
assert(lib.arm64_aggregate_count(59) == 256,
       'one-float stack HFA effects were lost or replayed')
one_stack_native_before = _hfa_native_count()
collectgarbage('collect')
exits, hfa_exits = 0, 0
assert(one_stack_candidate(32, one_stack_fn, one_stack_value) ==
       one_stack_oracle(32, one_stack_fn, one_stack_value),
       'one-float HFA stack case changed after full GC')
assert(_hfa_native_count() > one_stack_native_before and hfa_exits > 0,
       'one-float HFA stack case did not re-enter after GC')
assert(lib.arm64_aggregate_count(59) == 320,
       'one-float stack HFA post-GC effects were lost or replayed')
hfa_candidate = saved_hfa_candidate
_set_hfa_candidate(hfa_candidate)

local function partial_triple_hfa_source()
  return assert(loadstring([[
return function(n, fn, triple, salt)
  local sum = .5
  for i = 1, n do
    sum = sum + fn(1,2,3,4,5,6,7, triple, salt)
  end
  return sum
end
]], '@partial-triple-hfa-stack'))()
end
local partial_triple_candidate = partial_triple_hfa_source()
local partial_triple_oracle = partial_triple_hfa_source()
local partial_triple_clone = partial_triple_hfa_source()
jit.off(partial_triple_oracle, true); jit.off(partial_triple_clone, true)
_G.__arm64_partial_triple_hfa_candidate = partial_triple_candidate
_G.__arm64_partial_triple_hfa_clone = partial_triple_clone
local partial_triple_fn = lib.arm64_take_float_triple_partial_stack
local partial_triple_value = ffi.new('Arm64FloatTriple', 1.25, 2.5, 3.75)
hfa_candidate = partial_triple_candidate
_set_hfa_candidate(partial_triple_candidate)
jit.flush(partial_triple_candidate)
jit.opt.start('hotloop=1', 'hotexit=1000')
jit.on(partial_triple_candidate, true)
exits, hfa_exits = 0, 0
lib.arm64_aggregate_reset()
_begin_hfa_witness()
local partial_triple_native_before = _hfa_native_count()
for pass = 1, 8 do
  assert(partial_triple_candidate(16, partial_triple_fn,
         partial_triple_value, 3.75) ==
         partial_triple_oracle(16, partial_triple_fn,
         partial_triple_value, 3.75),
         'partially spilled three-float HFA differs from C oracle')
end
assert(_hfa_trace_count() > 0 and
       _hfa_native_count() > partial_triple_native_before and hfa_exits > 0,
       'partially spilled three-float HFA did not execute natively')
assert(lib.arm64_aggregate_count(58) == 256,
       'partial three-float HFA effects were lost or replayed')
partial_triple_native_before = _hfa_native_count()
collectgarbage('collect')
exits, hfa_exits = 0, 0
assert(partial_triple_candidate(32, partial_triple_fn,
       partial_triple_value, 3.75) ==
       partial_triple_oracle(32, partial_triple_fn,
       partial_triple_value, 3.75),
       'partially spilled three-float HFA changed after full GC')
assert(_hfa_native_count() > partial_triple_native_before and hfa_exits > 0,
       'partially spilled three-float HFA did not re-enter after full GC')
assert(lib.arm64_aggregate_count(58) == 320,
       'partial three-float HFA post-GC effects were lost or replayed')
hfa_candidate = saved_hfa_candidate
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

-- Darwin packs the scalar prefix but reserves eight bytes for each small
-- non-HFA composite. Verify both the gap before it and the following scalar.
local function small_stack_case(index, width, name, typename, seed)
  local source = [[return function(n,fn,v,s,p)
    local sum=0
    for i=1,n do sum=sum+fn(1,2,3,4,5,6,7,8,p,v,s) end
    return sum
  end]]
  local hot = assert(loadstring(source))()
  local cold = assert(loadstring(source))()
  local clone = assert(loadstring(source))()
  jit.off(cold,true); jit.off(clone,true)
  local saved = candidate
  candidate = hot; _set_aggregate_candidate(hot)
  _G['__arm64_small_stack_candidate_'..index] = hot
  _G['__arm64_small_stack_clone_'..index] = clone
  local fn = lib['arm64_take_'..name..'_stack']
  local alternate = lib['arm64_take_'..name..'_stack_alt']
  _G['__arm64_small_stack_fn_'..index] = fn
  local value = ffi.new(typename,seed)
  local expected = 204+9*29+3*seed+5*17
  assert(fn(1,2,3,4,5,6,7,8,29,value,17) == expected,
         name..' interpreter stack padding disagrees with C ABI')
  jit.flush(hot); jit.opt.start('hotloop=1'); jit.on(hot,true)
  lib.arm64_aggregate_reset(); exits=0
  local before = _aggregate_native_width_count(width)
  for pass=1,8 do
    assert(hot(16,fn,value,17,29)==16*expected)
    assert(cold(16,fn,value,17,29)==16*expected)
  end
  assert(next(roots(hot)) and _aggregate_native_width_count(width)>before and
         exits>0, name..' padded stack composite did not execute natively')
  assert(lib.arm64_aggregate_count(49+index)==256)
  before=_aggregate_native_width_count(width)
  collectgarbage('collect'); exits=0
  assert(hot(32,fn,value,17,29)==32*expected)
  assert(cold(32,fn,value,17,29)==32*expected)
  assert(_aggregate_native_width_count(width)>before and exits>0)
  assert(lib.arm64_aggregate_count(49+index)==320)
  tracked_exit_roots=roots(hot); exits=0
  assert(hot(32,alternate,value,17,29)==32*(expected+17))
  assert(cold(32,alternate,value,17,29)==32*(expected+17))
  assert(exits>0 and lib.arm64_aggregate_count(52+index)==64)
  tracked_exit_roots=nil
  candidate=saved; _set_aggregate_candidate(saved)
end
small_stack_case(1,1,'byte','Arm64Byte',201)
small_stack_case(2,2,'half','Arm64Half',50123)
small_stack_case(3,4,'word','Arm64Word',123456789)

-- Match the constant-valued Darwin stack prefix from the topology probe.
do
  local source=[[return function(n,fn,v,s)
    local sum=0
    for i=1,n do sum=sum+fn(1,2,3,4,5,6,7,8,29,v,s) end
    return sum
  end]]
  local hot,cold,clone=assert(loadstring(source))(),
    assert(loadstring(source))(),assert(loadstring(source))()
  jit.off(cold,true); jit.off(clone,true)
  local saved=candidate
  candidate=hot; _set_aggregate_candidate(hot)
  _G.__arm64_small_constant_candidate=hot
  _G.__arm64_small_constant_clone=clone
  local fn=lib.arm64_take_byte_stack
  local alternate=lib.arm64_take_byte_stack_alt
  local value=ffi.new('Arm64Byte',201)
  local expected=204+9*29+3*201+5*17
  assert(fn(1,2,3,4,5,6,7,8,29,value,17)==expected,
    'constant byte stack interpreter ABI disagrees with C')
  jit.flush(hot); jit.opt.start('hotloop=1'); jit.on(hot,true)
  local before_native=_aggregate_native_width_count(1)
  local before_effect=lib.arm64_aggregate_count(50)
  exits=0
  for pass=1,8 do
    assert(hot(16,fn,value,17)==16*expected)
    assert(cold(16,fn,value,17)==16*expected)
  end
  assert(next(roots(hot)) and _aggregate_native_width_count(1)>before_native and
         exits>0,'constant byte stack root did not execute natively')
  assert(lib.arm64_aggregate_count(50)-before_effect==256)
  before_native=_aggregate_native_width_count(1)
  before_effect=lib.arm64_aggregate_count(50)
  collectgarbage('collect'); exits=0
  assert(hot(32,fn,value,17)==32*expected)
  assert(cold(32,fn,value,17)==32*expected)
  assert(_aggregate_native_width_count(1)>before_native and exits>0)
  assert(lib.arm64_aggregate_count(50)-before_effect==64)
  tracked_exit_roots=roots(hot); exits=0
  before_effect=lib.arm64_aggregate_count(53)
  assert(hot(32,alternate,value,17)==32*(expected+17))
  assert(cold(32,alternate,value,17)==32*(expected+17))
  assert(exits>0 and lib.arm64_aggregate_count(53)-before_effect==64)
  tracked_exit_roots=nil
  candidate=saved; _set_aggregate_candidate(saved)
end

-- Fixed narrow literals fold to INT carriers. Their declared byte/halfword
-- widths must still control stack stores around the aligned composite slot.
do
  local source = [[return function(n,fn,v)
    local sum=0
    for i=1,n do
      sum=sum+fn(1,2,3,4,5,6,7,8,165,-7,50000,-12345,
                 v,91,-11,57635,-23456,17)
    end
    return sum
  end]]
  local hot, cold, clone = assert(loadstring(source))(),
    assert(loadstring(source))(), assert(loadstring(source))()
  jit.off(cold,true); jit.off(clone,true)
  local saved = candidate
  candidate=hot; _set_aggregate_candidate(hot)
  _G.__arm64_narrow_constant_candidate=hot
  _G.__arm64_narrow_constant_clone=clone
  local fn=lib.arm64_take_narrow_constants_stack
  local alternate=lib.arm64_take_narrow_constants_stack_alt
  local value=ffi.new('Arm64Byte',201)
  local expected=204+11*165+13*249+17*50000+19*53191+23*201+
    29*91+31*245+37*57635+41*42080+43*17
  assert(fn(1,2,3,4,5,6,7,8,165,-7,50000,-12345,
            value,91,-11,57635,-23456,17)==expected,
         'narrow constant interpreter stack ABI disagrees with C')
  jit.flush(hot); jit.opt.start('hotloop=1'); jit.on(hot,true)
  local before_native=_aggregate_native_width_count(1)
  local before_effect=lib.arm64_aggregate_count(60)
  exits=0
  for pass=1,8 do
    assert(hot(16,fn,value)==16*expected)
    assert(cold(16,fn,value)==16*expected)
  end
  assert(next(roots(hot)) and _aggregate_native_width_count(1)>before_native and
         exits>0,'narrow constant stack root did not execute natively')
  assert(lib.arm64_aggregate_count(60)-before_effect==256)
  before_native=_aggregate_native_width_count(1)
  before_effect=lib.arm64_aggregate_count(60)
  collectgarbage('collect'); exits=0
  assert(hot(32,fn,value)==32*expected)
  assert(cold(32,fn,value)==32*expected)
  assert(_aggregate_native_width_count(1)>before_native and exits>0)
  assert(lib.arm64_aggregate_count(60)-before_effect==64)
  tracked_exit_roots=roots(hot); exits=0
  before_effect=lib.arm64_aggregate_count(61)
  assert(hot(32,alternate,value)==32*(expected+17))
  assert(cold(32,alternate,value)==32*(expected+17))
  assert(exits>0 and lib.arm64_aggregate_count(61)-before_effect==64)
  tracked_exit_roots=nil
  candidate=saved; _set_aggregate_candidate(saved)
end


-- Full stack HFAs whose total size is a multiple of the ABI stack slot.
local function wide_stack_case(index, symbol, type_name, members)
  local source=[[return function(n,fn,value,salt)
    local sum=0
    for i=1,n do sum=sum+fn(1,2,3,4,5,6,7,8,value,salt) end
    return sum
  end]]
  local fn=lib['arm64_take_'..symbol..'_stack']
  local alternate=lib['arm64_take_'..symbol..'_stack_alt']
  local hot=assert(loadstring(source))()
  local cold=assert(loadstring(source))()
  local clone=assert(loadstring(source))()
  jit.off(cold,true); jit.off(clone,true)
  _G['__arm64_wide_stack_candidate_'..index]=hot
  _G['__arm64_wide_stack_clone_'..index]=clone
  _G['__arm64_wide_stack_fn_'..index]=fn
  local saved=hfa_candidate
  hfa_candidate=hot; _set_hfa_candidate(hot)
  jit.opt.start('hotloop=1'); jit.on(hot,true)
  local value=ffi.new(type_name,unpack(members))
  lib.arm64_aggregate_reset(); _begin_hfa_witness()
  exits,hfa_exits=0,0
  for pass=1,8 do
    assert(hot(16,fn,value,3.75)==cold(16,fn,value,3.75))
  end
  assert(_hfa_trace_count()>0 and _hfa_native_count()>0 and hfa_exits>0,
    symbol..' stack root requires native finish and selected exit')
  assert(lib.arm64_aggregate_count(37+index)==256)
  local before=_hfa_native_count()
  collectgarbage('collect'); hfa_exits=0
  assert(hot(32,fn,value,3.75)==cold(32,fn,value,3.75))
  assert(_hfa_native_count()>before and hfa_exits>0)
  assert(lib.arm64_aggregate_count(37+index)==320)
  tracked_exit_roots=roots(hot); exits=0
  assert(hot(32,alternate,value,3.75)==cold(32,alternate,value,3.75))
  assert(exits>0 and lib.arm64_aggregate_count(42+index)==64)
  tracked_exit_roots=nil
  hfa_candidate=saved; _set_hfa_candidate(saved)
end
wide_stack_case(1,'float_quad','Arm64FloatQuadResult',{1.25,2.5,3.75,4.25})
wide_stack_case(2,'double_pair','Arm64DoublePairResult',{1.25,2.5})
wide_stack_case(3,'double_triple','Arm64DoubleTripleResult',{1.25,2.5,3.75})
wide_stack_case(4,'double_quad','Arm64DoubleQuadResult',{1.25,2.5,3.75,4.25})
wide_stack_case(5,'double_one','Arm64DoubleResult',{1.25})
_G.__arm64_float_triple_stack_fn=ffi.cast([[double (*)(double,double,double,
  double,double,double,double,double,Arm64FloatTriple,double)]],0)
_G.__arm64_partial_float_quad_stack_fn=ffi.cast([[double (*)(double,
  double,double,double,double,double,double,Arm64FloatQuadResult,double)]],0)
_G.__arm64_partial_hfa_stack_unsupported_fn=ffi.cast([[double (*)(double,
  double,double,double,double,double,double,Arm64FloatTriple,double)]],0)


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

_G.__arm64_run_nested_hfa_probe = function()
  jit.opt.start('maxtrace=128')
  local nested_hfa_candidate = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@nested-float-hfa-candidate'))()
  local nested_hfa_clone = assert(loadstring([[
return function(n, fn, value, salt)
  local sum = 0
  for i = 1, n do sum = sum + fn(value, salt) end
  return sum
end
]], '@nested-float-hfa-clone'))()
  jit.off(nested_hfa_clone, true)
  _G.__arm64_nested_hfa_clone = nested_hfa_clone
  _G.__arm64_nested_hfa_candidate = nested_hfa_candidate
  _set_nested_hfa_candidate(nested_hfa_candidate)
  tracked_exit_roots = roots(nested_hfa_candidate)
  jit.on(nested_hfa_candidate, true)
  lib.arm64_aggregate_reset()
  local nested_hfa = ffi.new('Arm64NestedFloatPair')
  nested_hfa.inner.lo, nested_hfa.inner.hi = 1.25, 2.5
  local nested_hfa_fn = lib.arm64_take_nested_float_pair
  local nested_native_before = _nested_hfa_native_count()
  for pass = 1, 8 do
    assert(nested_hfa_candidate(16, nested_hfa_fn, nested_hfa, 3.75) ==
           hfa_oracle(16, nested_hfa_fn, nested_hfa, 3.75),
           'nested HFA argument disagreed with the interpreted oracle')
  end
    assert(_nested_hfa_native_count() > nested_native_before and exits > 0,
         'nested HFA argument did not publish, enter, and exit its native root')
  assert(lib.arm64_aggregate_count(62) == 256,
         'nested HFA C effects were lost or replayed')
  local nested_before_gc = _nested_hfa_native_count()
  collectgarbage('collect')
  tracked_exit_roots = roots(nested_hfa_candidate)
  exits = 0
  local nested_gc = ffi.new('Arm64NestedFloatPair')
  nested_gc.inner.lo, nested_gc.inner.hi = 1.25, 2.5
  assert(nested_hfa_candidate(32, nested_hfa_fn, nested_gc, 3.75) ==
         hfa_oracle(32, nested_hfa_fn, nested_gc, 3.75))
  assert(_nested_hfa_native_count() > nested_before_gc and exits > 0,
         'nested HFA root did not re-enter and exit after full GC: native='..
         tostring(_nested_hfa_native_count()-nested_before_gc)..
         ', exits='..tostring(exits))
  assert(lib.arm64_aggregate_count(62) == 320,
         'nested HFA post-GC effects were lost or replayed')
  exits = 0
  local nested_alt_fn = lib.arm64_take_nested_float_pair_alt
  assert(nested_hfa_candidate(32, nested_alt_fn, nested_gc, 3.75) ==
         hfa_oracle(32, nested_alt_fn, nested_gc, 3.75))
  assert(exits > 0,
         'nested HFA alternate callee did not exit the exact native root')
  assert(lib.arm64_aggregate_count(63) == 64,
         'nested HFA alternate callee effects were lost or replayed')
  tracked_exit_roots = nil
end

_G.__arm64_finish_peer = function()
  assert(stop:send(true, 5))
  local joined, result = peer:join(5)
  assert(joined and result, 'live peer did not stop and join')
end
