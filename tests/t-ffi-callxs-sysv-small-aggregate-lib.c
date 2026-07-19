/* Generic SysV x64 one-class aggregate ABI targets for CALLXS. */

#include <stdint.h>

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
