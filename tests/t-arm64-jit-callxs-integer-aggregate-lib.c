#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#define LJ_EXPORT __declspec(dllexport)
#else
#define LJ_EXPORT __attribute__((visibility("default")))
#endif

typedef struct { uint32_t lo, hi; } arm64_int_pair;
typedef struct { float lo, hi; } arm64_float_pair;
typedef struct { arm64_float_pair inner; } arm64_nested_float_pair;
typedef struct { float mid, tail; } arm64_float_pair_tail;
typedef struct { float head; arm64_float_pair_tail nested; } arm64_sibling_nested_float_triple;
typedef struct { double lo, hi; } arm64_double_pair;
typedef struct { arm64_double_pair left, right; } arm64_nested_double_quad;
typedef struct { float x, y, z; } arm64_float_triple;
typedef struct { double x, y, z, w; } arm64_double_quad;
typedef struct { float value; } arm64_float_one;
typedef struct { double value; } arm64_double_one;
typedef struct { arm64_int_pair pair; uint32_t tag; } arm64_nested_pair;
typedef union { uint64_t u; double d; } arm64_mixed_union;
typedef struct { uint8_t value; } arm64_byte;
typedef struct { uint16_t value; } arm64_half;
typedef struct { uint32_t value; } arm64_word;
typedef struct { uint64_t lo, hi; } arm64_big_pair;
typedef struct { uint8_t value; } arm64_byte_result;
typedef struct { uint16_t value; } arm64_half_result;
typedef struct { uint32_t value; } arm64_word_result;
typedef arm64_word_result arm64_word_result_alias;
typedef struct { uint64_t value; } arm64_u64_result;
typedef struct { float value; } arm64_float_result;
typedef struct { double value; } arm64_double_result;
typedef struct { float lo, hi; } arm64_float_pair_result;
typedef struct { double lo, hi; } arm64_double_pair_result;
typedef struct { float x, y, z; } arm64_float_triple_result;
typedef struct { float x, y, z, w; } arm64_float_quad_result;
typedef struct { double x, y, z; } arm64_double_triple_result;
typedef struct { double x, y, z, w; } arm64_double_quad_result;
typedef struct { uint32_t value:5; } arm64_bitfield;

enum {
  ARM64_INT_PAIR,
  ARM64_INT_PAIR_DOUBLE_SALT,
  ARM64_FLOAT_PAIR,
  ARM64_FLOAT_PAIR_SALT,
  ARM64_NESTED_PAIR,
  ARM64_MIXED_UNION,
  ARM64_BYTE,
  ARM64_HALF,
  ARM64_WORD,
  ARM64_BIG_PAIR,
  ARM64_BITFIELD,
  ARM64_PAIR_NO_GPR,
  ARM64_FLOAT_ONE_SALT,
  ARM64_DOUBLE_ONE_SALT,
  ARM64_VARARG_I32,
  ARM64_RETURN_BYTE,
  ARM64_RETURN_HALF,
  ARM64_RETURN_WORD,
  ARM64_RETURN_U64,
  ARM64_RETURN_SCALAR,
  ARM64_RETURN_WORD_ALIAS,
  ARM64_RETURN_FLOAT_HFA,
  ARM64_RETURN_DOUBLE_HFA,
  ARM64_RETURN_FLOAT_PAIR_HFA,
  ARM64_RETURN_FLOAT_PAIR_HFA_FPR,
  ARM64_RETURN_FLOAT_PAIR_SCALAR_FPR,
  ARM64_RETURN_DOUBLE_PAIR_HFA,
  ARM64_RETURN_DOUBLE_PAIR_HFA_FPR,
  ARM64_RETURN_FLOAT_TRIPLE_HFA,
  ARM64_RETURN_FLOAT_QUAD_HFA,
  ARM64_RETURN_DOUBLE_TRIPLE_HFA,
  ARM64_RETURN_DOUBLE_QUAD_HFA,
  ARM64_FLOAT_TRIPLE_SALT,
  ARM64_DOUBLE_QUAD_SALT,
  ARM64_FLOAT_PAIR_STACK,
  ARM64_FLOAT_PAIR_STACK_ALT,
  ARM64_INT_PAIR_STACK,
  ARM64_INT_PAIR_STACK_ALT,
  ARM64_FLOAT_QUAD_STACK,
  ARM64_DOUBLE_PAIR_STACK,
  ARM64_DOUBLE_TRIPLE_STACK,
  ARM64_DOUBLE_QUAD_STACK,
  ARM64_DOUBLE_ONE_STACK,
  ARM64_FLOAT_QUAD_STACK_ALT,
  ARM64_DOUBLE_PAIR_STACK_ALT,
  ARM64_DOUBLE_TRIPLE_STACK_ALT,
  ARM64_DOUBLE_QUAD_STACK_ALT,
  ARM64_DOUBLE_ONE_STACK_ALT,
  ARM64_FLOAT_PAIR_PARTIAL_STACK,
  ARM64_FLOAT_PAIR_PARTIAL_STACK_ALT,
  ARM64_BYTE_STACK,
  ARM64_HALF_STACK,
  ARM64_WORD_STACK,
  ARM64_BYTE_STACK_ALT,
  ARM64_HALF_STACK_ALT,
  ARM64_WORD_STACK_ALT,
  ARM64_SMALL_HFA_STACK,
  ARM64_SMALL_HFA_STACK_ALT,
  ARM64_FLOAT_TRIPLE_PARTIAL_STACK,
  ARM64_FLOAT_ONE_STACK_PREFIX,
  ARM64_NARROW_CONSTANT_STACK,
  ARM64_NARROW_CONSTANT_STACK_ALT,
  ARM64_NESTED_FLOAT_PAIR,
  ARM64_NESTED_FLOAT_PAIR_ALT,
  ARM64_SIBLING_NESTED_FLOAT_TRIPLE,
  ARM64_NESTED_DOUBLE_QUAD,
  ARM64_AGGREGATE_COUNT
};

static volatile uint32_t calls[ARM64_AGGREGATE_COUNT];

/* The compact scalar prefix leaves a gap before the eight-byte composite
** stack slot; the scalar suffix must begin after that entire slot. */
#define SMALL_STACK_ARG(name, type, counter, extra) \
LJ_EXPORT uint32_t name(uint32_t a, uint32_t b, uint32_t c, uint32_t d, \
    uint32_t e, uint32_t f, uint32_t g, uint32_t h, uint8_t prefix, \
    type value, uint32_t salt) \
{ \
  calls[counter]++; \
  return a+2u*b+3u*c+4u*d+5u*e+6u*f+7u*g+8u*h + \
         9u*prefix+3u*value.value+5u*salt+(extra); \
}
SMALL_STACK_ARG(arm64_take_byte_stack, arm64_byte, ARM64_BYTE_STACK, 0u)
SMALL_STACK_ARG(arm64_take_half_stack, arm64_half, ARM64_HALF_STACK, 0u)
SMALL_STACK_ARG(arm64_take_word_stack, arm64_word, ARM64_WORD_STACK, 0u)
SMALL_STACK_ARG(arm64_take_byte_stack_alt, arm64_byte, ARM64_BYTE_STACK_ALT, 17u)
SMALL_STACK_ARG(arm64_take_half_stack_alt, arm64_half, ARM64_HALF_STACK_ALT, 17u)
SMALL_STACK_ARG(arm64_take_word_stack_alt, arm64_word, ARM64_WORD_STACK_ALT, 17u)
#undef SMALL_STACK_ARG

/* Alternating narrow constants occupy packed stack bytes/halfwords around a
** small composite whose stack slot is still eight bytes wide. */
#define NARROW_CONSTANT_STACK(name, counter, extra) \
LJ_EXPORT uint32_t name(uint32_t a, uint32_t b, uint32_t c, uint32_t d, \
    uint32_t e, uint32_t f, uint32_t g, uint32_t h, uint8_t ub, int8_t sb, \
    uint16_t uh, int16_t sh, arm64_byte value, uint8_t ua, int8_t sa, \
    uint16_t uh2, int16_t sh2, uint32_t salt) \
{ \
  calls[counter]++; \
  return a+2u*b+3u*c+4u*d+5u*e+6u*f+7u*g+8u*h + \
         11u*ub+13u*(uint8_t)sb+17u*uh+19u*(uint16_t)sh+ \
         23u*value.value+29u*ua+31u*(uint8_t)sa+ \
         37u*uh2+41u*(uint16_t)sh2+43u*salt+(extra); \
}
NARROW_CONSTANT_STACK(arm64_take_narrow_constants_stack,
                     ARM64_NARROW_CONSTANT_STACK, 0u)
NARROW_CONSTANT_STACK(arm64_take_narrow_constants_stack_alt,
                     ARM64_NARROW_CONSTANT_STACK_ALT, 17u)
#undef NARROW_CONSTANT_STACK

LJ_EXPORT void arm64_aggregate_reset(void)
{
  unsigned i;
  for (i = 0; i < ARM64_AGGREGATE_COUNT; i++) calls[i] = 0;
}

LJ_EXPORT uint32_t arm64_aggregate_count(uint32_t which)
{
  return which < ARM64_AGGREGATE_COUNT ? calls[which] : UINT32_MAX;
}

LJ_EXPORT uint32_t arm64_take_int_pair(arm64_int_pair value, uint32_t salt)
{
  calls[ARM64_INT_PAIR]++;
  return value.lo + value.hi * 2u + salt;
}

LJ_EXPORT double arm64_take_int_pair_double_salt(arm64_int_pair value,
						 uint32_t salt)
{
  calls[ARM64_INT_PAIR_DOUBLE_SALT]++;
  return (double)value.lo + (double)value.hi * 2.0 + (double)salt;
}

LJ_EXPORT uint32_t arm64_take_float_pair(arm64_float_pair value)
{
  calls[ARM64_FLOAT_PAIR]++;
  return (uint32_t)value.lo + (uint32_t)value.hi;
}

LJ_EXPORT double arm64_take_float_pair_double_salt(arm64_float_pair value,
						    double salt)
{
  calls[ARM64_FLOAT_PAIR_SALT]++;
  return (double)value.lo + (double)value.hi*2.0 + salt;
}

LJ_EXPORT double arm64_take_nested_float_pair(arm64_nested_float_pair value,
						       double salt)
{
  calls[ARM64_NESTED_FLOAT_PAIR]++;
  return (double)value.inner.lo + (double)value.inner.hi*2.0 + salt;
}

LJ_EXPORT double arm64_take_nested_float_pair_alt(arm64_nested_float_pair value,
							   double salt)
{
  calls[ARM64_NESTED_FLOAT_PAIR_ALT]++;
  return (double)value.inner.lo + (double)value.inner.hi*2.0 + salt + 17.0;
}

LJ_EXPORT double arm64_take_sibling_nested_float_triple(
    arm64_sibling_nested_float_triple value, double salt)
{
  calls[ARM64_SIBLING_NESTED_FLOAT_TRIPLE]++;
  return (double)value.head + (double)value.nested.mid*2.0 +
         (double)value.nested.tail*3.0 + salt;
}

LJ_EXPORT double arm64_take_nested_double_quad(arm64_nested_double_quad value,
                                                double salt)
{
  calls[ARM64_NESTED_DOUBLE_QUAD]++;
  return value.left.lo + value.left.hi*2.0 +
         value.right.lo*3.0 + value.right.hi*4.0 + salt;
}

LJ_EXPORT double arm64_take_float_one_double_salt(arm64_float_one value,
						   double salt)
{
  calls[ARM64_FLOAT_ONE_SALT]++;
  return (double)value.value + salt;
}

LJ_EXPORT double arm64_take_double_one_double_salt(arm64_double_one value,
						    double salt)
{
  calls[ARM64_DOUBLE_ONE_SALT]++;
  return value.value + salt;
}

LJ_EXPORT double arm64_take_float_triple_double_salt(arm64_float_triple value,
						     double salt)
{
  calls[ARM64_FLOAT_TRIPLE_SALT]++;
  return (double)value.x + (double)value.y*2.0 +
	 (double)value.z*3.0 + salt;
}

LJ_EXPORT double arm64_take_double_quad_double_salt(arm64_double_quad value,
						     double salt)
{
  calls[ARM64_DOUBLE_QUAD_SALT]++;
  return value.x + value.y*2.0 + value.z*3.0 + value.w*4.0 + salt;
}

LJ_EXPORT double arm64_take_float_pair_stack(double a, double b, double c,
					     double d, double e, double f,
					     double g, double h,
					     arm64_float_pair value, double salt)
{
  calls[ARM64_FLOAT_PAIR_STACK]++;
  return a+b+c+d+e+f+g+h+(double)value.lo+2.0*(double)value.hi+salt;
}

LJ_EXPORT double arm64_take_float_pair_stack_alt(double a, double b, double c,
						 double d, double e, double f,
						 double g, double h,
						 arm64_float_pair value, double salt)
{
  calls[ARM64_FLOAT_PAIR_STACK_ALT]++;
  return a+b+c+d+e+f+g+h+(double)value.lo+2.0*(double)value.hi+salt;
}

LJ_EXPORT double arm64_take_float_pair_partial_stack(double a, double b,
						     double c, double d,
						     double e, double f,
						     double g,
						     arm64_float_pair value,
						     double salt)
{
  calls[ARM64_FLOAT_PAIR_PARTIAL_STACK]++;
  return a+b+c+d+e+f+g+(double)value.lo+2.0*(double)value.hi+salt;
}

LJ_EXPORT double arm64_take_float_pair_partial_stack_alt(double a, double b,
							 double c, double d,
							 double e, double f,
							 double g,
							 arm64_float_pair value,
							 double salt)
{
  calls[ARM64_FLOAT_PAIR_PARTIAL_STACK_ALT]++;
  return a+b+c+d+e+f+g+(double)value.lo+2.0*(double)value.hi+salt+1.0;
}

/* Darwin packs float-sized stack arguments at four-byte boundaries. With
** eight preceding doubles in v0-v7, prefix occupies stack+0, the one-float
** HFA stack+4, middle stack+8, the three-float HFA stack+12..20, and suffix
** stack+24. The scalar terms make each boundary independently observable. */
LJ_EXPORT double arm64_take_small_hfa_stack(double a, double b, double c,
    double d, double e, double f, double g, double h, float prefix,
    arm64_float_one one, float middle, arm64_float_triple triple, float suffix)
{
  calls[ARM64_SMALL_HFA_STACK]++;
  return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h + 9.0*prefix +
         10.0*(double)one.value + 11.0*middle +
         12.0*(double)triple.x + 13.0*(double)triple.y +
         14.0*(double)triple.z + 15.0*suffix;
}

LJ_EXPORT double arm64_take_small_hfa_stack_alt(double a, double b, double c,
    double d, double e, double f, double g, double h, float prefix,
    arm64_float_one one, float middle, arm64_float_triple triple, float suffix)
{
  calls[ARM64_SMALL_HFA_STACK_ALT]++;
  return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h + 9.0*prefix +
         10.0*(double)one.value + 11.0*middle +
         12.0*(double)triple.x + 13.0*(double)triple.y +
         14.0*(double)triple.z + 15.0*suffix + 1.0;
}

LJ_EXPORT double arm64_take_float_triple_partial_stack(
    double a, double b, double c, double d, double e, double f, double g,
    arm64_float_triple value, double salt)
{
  calls[ARM64_FLOAT_TRIPLE_PARTIAL_STACK]++;
  return a+2*b+3*c+4*d+5*e+6*f+7*g +
         8.0*(double)value.x + 9.0*(double)value.y +
         10.0*(double)value.z + salt;
}

LJ_EXPORT double arm64_take_float_one_stack_prefix(
    double a, double b, double c, double d, double e, double f, double g,
    double h, float prefix, arm64_float_one one, float suffix)
{
  calls[ARM64_FLOAT_ONE_STACK_PREFIX]++;
  return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h +
         9.0*prefix + 10.0*(double)one.value + 11.0*suffix;
}

#define DEFINE_WIDE_HFA_STACK(name, type, members, counter) \
  LJ_EXPORT double arm64_take_##name##_stack(double a, double b, double c, \
      double d, double e, double f, double g, double h, type value, double salt) \
  { calls[counter]++; return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+(members)+salt; } \
  LJ_EXPORT double arm64_take_##name##_stack_alt(double a, double b, double c, \
      double d, double e, double f, double g, double h, type value, double salt) \
  { calls[counter+5]++; return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+(members)+salt+1; }

DEFINE_WIDE_HFA_STACK(float_quad, arm64_float_quad_result,
  value.x+2*value.y+3*value.z+4*value.w, ARM64_FLOAT_QUAD_STACK)
DEFINE_WIDE_HFA_STACK(double_pair, arm64_double_pair_result,
  value.lo+2*value.hi, ARM64_DOUBLE_PAIR_STACK)
DEFINE_WIDE_HFA_STACK(double_triple, arm64_double_triple_result,
  value.x+2*value.y+3*value.z, ARM64_DOUBLE_TRIPLE_STACK)
DEFINE_WIDE_HFA_STACK(double_quad, arm64_double_quad_result,
  value.x+2*value.y+3*value.z+4*value.w, ARM64_DOUBLE_QUAD_STACK)
DEFINE_WIDE_HFA_STACK(double_one, arm64_double_result,
  value.value, ARM64_DOUBLE_ONE_STACK)
#undef DEFINE_WIDE_HFA_STACK

LJ_EXPORT int32_t arm64_vararg_i32(int32_t first, ...)
{
  calls[ARM64_VARARG_I32]++;
  return first;
}

LJ_EXPORT uint32_t arm64_take_nested_pair(arm64_nested_pair value)
{
  calls[ARM64_NESTED_PAIR]++;
  return value.pair.lo + value.pair.hi + value.tag;
}

LJ_EXPORT uint32_t arm64_take_union(arm64_mixed_union value)
{
  calls[ARM64_MIXED_UNION]++;
  return (uint32_t)value.u;
}

LJ_EXPORT uint32_t arm64_take_byte(arm64_byte value, uint32_t salt)
{
  calls[ARM64_BYTE]++;
  return (uint32_t)value.value + salt;
}

LJ_EXPORT uint32_t arm64_take_half(arm64_half value, uint32_t salt)
{
  calls[ARM64_HALF]++;
  return (uint32_t)value.value + salt;
}

LJ_EXPORT uint32_t arm64_take_word(arm64_word value, uint32_t salt)
{
  calls[ARM64_WORD]++;
  return value.value + salt;
}

LJ_EXPORT uint32_t arm64_take_big_pair(arm64_big_pair value)
{
  calls[ARM64_BIG_PAIR]++;
  return (uint32_t)(value.lo + value.hi);
}

LJ_EXPORT uint32_t arm64_take_bitfield(arm64_bitfield value)
{
  calls[ARM64_BITFIELD]++;
  return value.value;
}

LJ_EXPORT uint32_t arm64_take_pair_no_gpr(uint32_t a, uint32_t b,
					  uint32_t c, uint32_t d,
					  uint32_t e, uint32_t f,
					  uint32_t g, uint32_t h,
					  arm64_int_pair value)
{
  calls[ARM64_PAIR_NO_GPR]++;
  return a+b+c+d+e+f+g+h+value.lo+value.hi;
}

LJ_EXPORT uint32_t arm64_take_pair_stack(uint32_t a, uint32_t b,
    uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h,
    arm64_int_pair value, uint32_t salt)
{
  calls[ARM64_INT_PAIR_STACK]++;
  return a+b+c+d+e+f+g+h+value.lo+value.hi*2u+salt;
}

LJ_EXPORT uint32_t arm64_take_pair_stack_alt(uint32_t a, uint32_t b,
    uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h,
    arm64_int_pair value, uint32_t salt)
{
  calls[ARM64_INT_PAIR_STACK_ALT]++;
  return a+b+c+d+e+f+g+h+value.lo*2u+value.hi+salt;
}

LJ_EXPORT arm64_byte_result arm64_return_byte(uint32_t value)
{
  arm64_byte_result result;
  calls[ARM64_RETURN_BYTE]++;
  result.value = (uint8_t)(value*3u+1u);
  return result;
}

LJ_EXPORT arm64_half_result arm64_return_half(uint32_t value)
{
  arm64_half_result result;
  calls[ARM64_RETURN_HALF]++;
  result.value = (uint16_t)(value*17u+3u);
  return result;
}

LJ_EXPORT arm64_word_result arm64_return_word(uint32_t value)
{
  arm64_word_result result;
  calls[ARM64_RETURN_WORD]++;
  result.value = value*101u+7u;
  return result;
}

LJ_EXPORT arm64_word_result_alias arm64_return_word_alias(uint32_t value)
{
  arm64_word_result_alias result;
  calls[ARM64_RETURN_WORD_ALIAS]++;
  result.value = value*101u+7u;
  return result;
}

LJ_EXPORT arm64_u64_result arm64_return_u64(uint32_t value)
{
  arm64_u64_result result;
  calls[ARM64_RETURN_U64]++;
  result.value = UINT64_C(0x1234567800000000) | (uint64_t)value*313u+11u;
  return result;
}

LJ_EXPORT arm64_float_result arm64_return_float_hfa(uint32_t value)
{
  arm64_float_result result;
  calls[ARM64_RETURN_FLOAT_HFA]++;
  result.value = (float)value * 0.5f + 1.25f;
  return result;
}

LJ_EXPORT arm64_double_result arm64_return_double_hfa(uint32_t value)
{
  arm64_double_result result;
  calls[ARM64_RETURN_DOUBLE_HFA]++;
  result.value = (double)value * 0.25 + 2.5;
  return result;
}

LJ_EXPORT arm64_float_pair_result arm64_return_float_pair_hfa(uint32_t value)
{
  arm64_float_pair_result result;
  calls[ARM64_RETURN_FLOAT_PAIR_HFA]++;
  if (value >= 2u && value <= 4u) {
    static const uint32_t lane_bits[3][2] = {
      { 0x00000000u, 0x80000000u },
      { 0x7f800000u, 0xff800000u },
      { 0x7fc12345u, 0x7fc54321u }
    };
    memcpy(&result.lo, &lane_bits[value-2u][0], sizeof(float));
    memcpy(&result.hi, &lane_bits[value-2u][1], sizeof(float));
  } else {
    result.lo = (float)value * 0.5f + 1.25f;
    result.hi = (float)value * 0.25f + 2.5f;
  }
  return result;
}

LJ_EXPORT arm64_float_pair_result arm64_return_float_pair_hfa_fpr(
    double bias, uint32_t value)
{
  arm64_float_pair_result result;
  calls[ARM64_RETURN_FLOAT_PAIR_HFA_FPR]++;
  if (value >= 2u && value <= 4u) {
    static const uint32_t lane_bits[3][2] = {
      { 0x00000000u, 0x80000000u },
      { 0x7f800000u, 0xff800000u },
      { 0x7fc12345u, 0x7fc54321u }
    };
    memcpy(&result.lo, &lane_bits[value-2u][0], sizeof(float));
    memcpy(&result.hi, &lane_bits[value-2u][1], sizeof(float));
  } else {
    result.lo = (float)bias + (float)value * 0.5f;
    result.hi = (float)bias - (float)value * 0.25f;
  }
  return result;
}

LJ_EXPORT double arm64_return_float_pair_scalar_fpr(double bias,
                                                     uint32_t value)
{
  calls[ARM64_RETURN_FLOAT_PAIR_SCALAR_FPR]++;
  return bias + (double)value;
}

LJ_EXPORT arm64_double_pair_result arm64_return_double_pair_hfa(uint32_t value)
{
  arm64_double_pair_result result;
  calls[ARM64_RETURN_DOUBLE_PAIR_HFA]++;
  if (value >= 2u && value <= 4u) {
    static const uint64_t lane_bits[3][2] = {
      { UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000) },
      { UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000) },
      { UINT64_C(0x7ff8123456789abc), UINT64_C(0x7ff8fedcba987654) }
    };
    memcpy(&result.lo, &lane_bits[value-2u][0], sizeof(double));
    memcpy(&result.hi, &lane_bits[value-2u][1], sizeof(double));
  } else {
    result.lo = (double)value * 0.5 + 1.25;
    result.hi = (double)value * 0.25 + 2.5;
  }
  return result;
}

LJ_EXPORT arm64_double_pair_result arm64_return_double_pair_hfa_fpr(
    double bias, uint32_t value)
{
  arm64_double_pair_result result;
  calls[ARM64_RETURN_DOUBLE_PAIR_HFA_FPR]++;
  if (value >= 2u && value <= 4u) {
    static const uint64_t lane_bits[3][2] = {
      { UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000) },
      { UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000) },
      { UINT64_C(0x7ff8123456789abc), UINT64_C(0x7ff8fedcba987654) }
    };
    memcpy(&result.lo, &lane_bits[value-2u][0], sizeof(double));
    memcpy(&result.hi, &lane_bits[value-2u][1], sizeof(double));
  } else {
    result.lo = bias + (double)value * 0.5;
    result.hi = bias - (double)value * 0.25;
  }
  return result;
}

LJ_EXPORT arm64_float_triple_result
arm64_return_float_triple_hfa(uint32_t value)
{
  arm64_float_triple_result result;
  calls[ARM64_RETURN_FLOAT_TRIPLE_HFA]++;
  result.x = (float)value + 0.25f;
  result.y = (float)value * 2.0f + 0.5f;
  result.z = (float)value * 3.0f + 0.75f;
  return result;
}

LJ_EXPORT arm64_float_quad_result
arm64_return_float_quad_hfa(uint32_t value)
{
  arm64_float_quad_result result;
  calls[ARM64_RETURN_FLOAT_QUAD_HFA]++;
  result.x = (float)value + 0.25f;
  result.y = (float)value * 2.0f + 0.5f;
  result.z = (float)value * 3.0f + 0.75f;
  result.w = (float)value * 4.0f + 1.0f;
  return result;
}

LJ_EXPORT arm64_double_triple_result
arm64_return_double_triple_hfa(uint32_t value)
{
  arm64_double_triple_result result;
  calls[ARM64_RETURN_DOUBLE_TRIPLE_HFA]++;
  result.x = (double)value + 0.125;
  result.y = (double)value * 2.0 + 0.25;
  result.z = (double)value * 3.0 + 0.5;
  return result;
}

LJ_EXPORT arm64_double_quad_result
arm64_return_double_quad_hfa(uint32_t value)
{
  arm64_double_quad_result result;
  calls[ARM64_RETURN_DOUBLE_QUAD_HFA]++;
  result.x = (double)value + 0.125;
  result.y = (double)value * 2.0 + 0.25;
  result.z = (double)value * 3.0 + 0.5;
  result.w = (double)value * 4.0 + 1.0;
  return result;
}

LJ_EXPORT uint32_t arm64_return_scalar(uint32_t value)
{
  calls[ARM64_RETURN_SCALAR]++;
  return value+1u;
}
