/* SysV x64 aggregate ABI targets for CALLXS. */

#include <stdint.h>
#include <stdarg.h>

#if defined(_WIN32)
#define LJ_CALLXS_EXPORT __declspec(dllexport)
#else
#define LJ_CALLXS_EXPORT __attribute__((visibility("default")))
#endif

typedef struct lj_callxs_sysv_int_pair {
  uint32_t lo;
  uint32_t hi;
} lj_callxs_sysv_int_pair;

typedef struct lj_callxs_sysv_sse_pair {
  float x;
  float y;
} lj_callxs_sysv_sse_pair;

typedef struct lj_callxs_sysv_nested_int {
  struct {
    uint16_t lo;
    uint16_t hi;
  } half;
  uint32_t tag;
} lj_callxs_sysv_nested_int;

typedef struct lj_callxs_sysv_nested_sse {
  struct {
    float lane[2];
  } inner;
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

typedef struct lj_callxs_sysv_int16 {
  int64_t a, b;
} lj_callxs_sysv_int16;
typedef struct lj_callxs_sysv_sse16 {
  float a, b, c, d;
} lj_callxs_sysv_sse16;
typedef struct lj_callxs_sysv_mix16 {
  int64_t a;
  double b;
} lj_callxs_sysv_mix16;
typedef struct lj_callxs_sysv_reverse16 {
  double a;
  int64_t b;
} lj_callxs_sysv_reverse16;
typedef struct lj_callxs_sysv_result24 {
  int64_t a, b, c;
} lj_callxs_sysv_result24;

typedef char lj_callxs_sysv_int_pair_size[
  sizeof(lj_callxs_sysv_int_pair) == 8 ? 1 : -1];
typedef char lj_callxs_sysv_sse_pair_size[
  sizeof(lj_callxs_sysv_sse_pair) == 8 ? 1 : -1];
typedef char lj_callxs_sysv_nested_int_size[
  sizeof(lj_callxs_sysv_nested_int) == 8 ? 1 : -1];
typedef char lj_callxs_sysv_nested_sse_size[
  sizeof(lj_callxs_sysv_nested_sse) == 8 ? 1 : -1];
typedef char lj_callxs_sysv_mixed_union_size[
  sizeof(lj_callxs_sysv_mixed_union) == 8 ? 1 : -1];
typedef char lj_callxs_sysv_word_size[
  sizeof(lj_callxs_sysv_word) == 4 ? 1 : -1];
typedef char lj_callxs_sysv_float_size[
  sizeof(lj_callxs_sysv_float) == 4 ? 1 : -1];
typedef char lj_callxs_sysv_byte_size[
  sizeof(lj_callxs_sysv_byte) == 1 ? 1 : -1];
typedef char lj_callxs_sysv_half_size[
  sizeof(lj_callxs_sysv_half) == 2 ? 1 : -1];

enum {
  LJ_CALLXS_SYSV_MAKE_INT,
  LJ_CALLXS_SYSV_TAKE_INT,
  LJ_CALLXS_SYSV_MAKE_SSE,
  LJ_CALLXS_SYSV_TAKE_SSE,
  LJ_CALLXS_SYSV_NESTED_INT,
  LJ_CALLXS_SYSV_NESTED_SSE,
  LJ_CALLXS_SYSV_SPILL_INT,
  LJ_CALLXS_SYSV_SPILL_SSE,
  LJ_CALLXS_SYSV_MIXED_UNION,
  LJ_CALLXS_SYSV_WORD,
  LJ_CALLXS_SYSV_FLOAT,
  LJ_CALLXS_SYSV_REPLAY_PROBE,
  LJ_CALLXS_SYSV_BYTE,
  LJ_CALLXS_SYSV_HALF,
  LJ_CALLXS_SYSV_REG_PAIRS,
  LJ_CALLXS_SYSV_PAIR_GPR_ROLLBACK,
  LJ_CALLXS_SYSV_PAIR_FPR_ROLLBACK,
  LJ_CALLXS_SYSV_PAIR_SRET,
  LJ_CALLXS_SYSV_PAIR_SRET_ROLLBACK,
  LJ_CALLXS_SYSV_PAIR_VARARG,
  LJ_CALLXS_SYSV_NCOUNTERS
};

static volatile uint32_t lj_callxs_sysv_counters[LJ_CALLXS_SYSV_NCOUNTERS];
static volatile uint32_t lj_callxs_sysv_probe_mode;

LJ_CALLXS_EXPORT void lj_callxs_sysv_reset(void)
{
  uint32_t i;
  for (i = 0; i < LJ_CALLXS_SYSV_NCOUNTERS; i++)
    lj_callxs_sysv_counters[i] = 0;
  lj_callxs_sysv_probe_mode = 0;
}

LJ_CALLXS_EXPORT uint32_t lj_callxs_sysv_count(uint32_t which)
{
  return which < LJ_CALLXS_SYSV_NCOUNTERS ?
    lj_callxs_sysv_counters[which] : UINT32_MAX;
}

LJ_CALLXS_EXPORT lj_callxs_sysv_int_pair
lj_callxs_sysv_make_int(uint32_t seed)
{
  lj_callxs_sysv_int_pair result;
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_MAKE_INT]++;
  result.lo = seed * 3u + 1u;
  result.hi = seed * 5u + 2u;
  return result;
}

LJ_CALLXS_EXPORT uint32_t
lj_callxs_sysv_take_int(lj_callxs_sysv_int_pair value, uint32_t salt)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_TAKE_INT]++;
  return value.lo + value.hi * 2u + salt;
}

LJ_CALLXS_EXPORT lj_callxs_sysv_sse_pair
lj_callxs_sysv_make_sse(uint32_t seed)
{
  lj_callxs_sysv_sse_pair result;
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_MAKE_SSE]++;
  result.x = (float)seed + 0.25f;
  result.y = (float)seed * 2.0f + 0.5f;
  return result;
}

LJ_CALLXS_EXPORT double
lj_callxs_sysv_take_sse(lj_callxs_sysv_sse_pair value, double salt)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_TAKE_SSE]++;
  return (double)value.x + (double)value.y + salt;
}

LJ_CALLXS_EXPORT lj_callxs_sysv_nested_int
lj_callxs_sysv_step_nested_int(lj_callxs_sysv_nested_int value,
			       uint16_t delta)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_NESTED_INT]++;
  value.half.lo = (uint16_t)(value.half.lo + delta);
  value.half.hi = (uint16_t)(value.half.hi + 2u * delta);
  value.tag += 3u * (uint32_t)delta;
  return value;
}

LJ_CALLXS_EXPORT lj_callxs_sysv_nested_sse
lj_callxs_sysv_step_nested_sse(lj_callxs_sysv_nested_sse value, float delta)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_NESTED_SSE]++;
  value.inner.lane[0] += delta;
  value.inner.lane[1] += 2.0f * delta;
  return value;
}

LJ_CALLXS_EXPORT uint64_t
lj_callxs_sysv_spill_int(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
			 uint64_t e, uint64_t f,
			 lj_callxs_sysv_int_pair value, uint64_t tail)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_SPILL_INT]++;
  return a + b + c + d + e + f + value.lo + value.hi + tail;
}

LJ_CALLXS_EXPORT double
lj_callxs_sysv_spill_sse(double a, double b, double c, double d, double e,
			 double f, double g, double h,
			 lj_callxs_sysv_sse_pair value, double tail)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_SPILL_SSE]++;
  return a + b + c + d + e + f + g + h +
    (double)value.x + (double)value.y + tail;
}

LJ_CALLXS_EXPORT lj_callxs_sysv_mixed_union
lj_callxs_sysv_twist_union(lj_callxs_sysv_mixed_union value, uint64_t mask)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_MIXED_UNION]++;
  value.u ^= mask;
  return value;
}

LJ_CALLXS_EXPORT lj_callxs_sysv_word
lj_callxs_sysv_step_word(lj_callxs_sysv_word value, uint32_t delta)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_WORD]++;
  value.value += delta;
  return value;
}

LJ_CALLXS_EXPORT lj_callxs_sysv_float
lj_callxs_sysv_step_float(lj_callxs_sysv_float value, float delta)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_FLOAT]++;
  value.value += delta;
  return value;
}

LJ_CALLXS_EXPORT void lj_callxs_sysv_set_probe_mode(uint32_t mode)
{
  lj_callxs_sysv_probe_mode = mode;
}

LJ_CALLXS_EXPORT lj_callxs_sysv_word
lj_callxs_sysv_replay_probe(lj_callxs_sysv_word value)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_REPLAY_PROBE]++;
  value.value += lj_callxs_sysv_probe_mode;
  return value;
}

LJ_CALLXS_EXPORT lj_callxs_sysv_byte
lj_callxs_sysv_step_byte(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
			 uint64_t e, uint64_t f, lj_callxs_sysv_byte value)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_BYTE]++;
  value.value = (uint8_t)(value.value + a + b + c + d + e + f);
  return value;
}

LJ_CALLXS_EXPORT lj_callxs_sysv_half
lj_callxs_sysv_step_half(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
			 uint64_t e, uint64_t f, lj_callxs_sysv_half value)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_HALF]++;
  value.value = (uint16_t)(value.value + a + b + c + d + e + f);
  return value;
}

LJ_CALLXS_EXPORT double
lj_callxs_sysv_register_pairs(double bias, lj_callxs_sysv_int16 ii,
			      lj_callxs_sysv_sse16 ss, lj_callxs_sysv_mix16 is,
			      lj_callxs_sysv_reverse16 si, int32_t salt)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_REG_PAIRS]++;
  return bias + 2 * ii.a + 3 * ii.b + 5 * ss.a + 7 * ss.b +
    11 * ss.c + 13 * ss.d + 17 * is.a + 19 * is.b +
    23 * si.a + 29 * si.b + salt + lj_callxs_sysv_probe_mode;
}

LJ_CALLXS_EXPORT double
lj_callxs_sysv_pair_gpr_rollback(int a, int b, int c, int d, int e,
				 lj_callxs_sysv_int16 ii, int tail)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_PAIR_GPR_ROLLBACK]++;
  return a + b + c + d + e + 2 * ii.a + 3 * ii.b + 5 * tail;
}

LJ_CALLXS_EXPORT double
lj_callxs_sysv_pair_fpr_rollback(double a, double b, double c, double d,
				 double e, double f, double g,
				 lj_callxs_sysv_sse16 ss, double tail)
{
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_PAIR_FPR_ROLLBACK]++;
  return a + b + c + d + e + f + g + ss.a + 2 * ss.b + 3 * ss.c +
    4 * ss.d + 5 * tail;
}

LJ_CALLXS_EXPORT lj_callxs_sysv_result24
lj_callxs_sysv_pair_sret(int a, int b, int c, lj_callxs_sysv_int16 ii,
			 int tail)
{
  lj_callxs_sysv_result24 result = { a + b + c, ii.a + 2 * ii.b, tail };
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_PAIR_SRET]++;
  return result;
}

LJ_CALLXS_EXPORT lj_callxs_sysv_result24
lj_callxs_sysv_pair_sret_rollback(int a, int b, int c, int d,
				  lj_callxs_sysv_int16 ii, int tail)
{
  lj_callxs_sysv_result24 result = { a + b + c + d, ii.a + 2 * ii.b, tail };
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_PAIR_SRET_ROLLBACK]++;
  return result;
}

LJ_CALLXS_EXPORT double
lj_callxs_sysv_pair_vararg(lj_callxs_sysv_sse16 ss, int count, ...)
{
  va_list ap;
  double sum = ss.a + 2 * ss.b + 3 * ss.c + 4 * ss.d;
  lj_callxs_sysv_counters[LJ_CALLXS_SYSV_PAIR_VARARG]++;
  va_start(ap, count);
  for (int i = 0; i < count; i++)
    sum += (i + 1) * va_arg(ap, double);
  va_end(ap);
  return sum;
}
