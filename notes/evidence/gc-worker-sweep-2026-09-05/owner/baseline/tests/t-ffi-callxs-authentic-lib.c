/* Controlled no-callback ABI targets for production generic CALLXS. */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

static int32_t lj_callxs_auth_counter;
static int32_t lj_callxs_auth_reference_value = 0x345678;
static uint32_t lj_callxs_auth_aggregate_alpha_counter;
static uint32_t lj_callxs_auth_aggregate_beta_counter;
static uint32_t lj_callxs_auth_aggregate_aligned_counter;
static uint32_t lj_callxs_auth_aggregate_zero_counter;
static uint32_t lj_callxs_auth_aggregate_union_counter;
static uint32_t lj_callxs_auth_aggregate_wide_counter;

/* These deliberately unrelated 24-byte return types both use the fixed-size
** aggregate sret ABI on the supported x64 targets. Their distinct field
** layouts prevent a recorder test from accidentally validating one explicit
** function/result shape twice. */
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

union lj_callxs_auth_aggregate_union {
  uint64_t lane[3];
  struct {
    int64_t signed_lane;
    double ratio;
    uint64_t token;
  } fields;
};

typedef char lj_callxs_auth_aggregate_alpha_size[
  sizeof(struct lj_callxs_auth_aggregate_alpha) == 24 ? 1 : -1];
typedef char lj_callxs_auth_aggregate_beta_size[
  sizeof(struct lj_callxs_auth_aggregate_beta) == 24 ? 1 : -1];
typedef char lj_callxs_auth_aggregate_aligned_size[
  sizeof(lj_callxs_auth_aggregate_aligned) == 24 ? 1 : -1];
typedef char lj_callxs_auth_aggregate_aligned_align[
  __alignof__(lj_callxs_auth_aggregate_aligned) == 32 ? 1 : -1];
typedef char lj_callxs_auth_aggregate_union_size[
  sizeof(union lj_callxs_auth_aggregate_union) == 24 ? 1 : -1];

int32_t lj_callxs_auth_add(int32_t a, int32_t b)
{
  return a + b + 3;
}

uint32_t lj_callxs_auth_u32(uint32_t a, int32_t b)
{
  lj_callxs_auth_counter++;
  return UINT32_C(0x80000000) + a + (uint32_t)b;
}

double lj_callxs_auth_mix(int32_t a, double b, float c, uint64_t d,
			  const int32_t *p, double e)
{
  return (double)a + b + (double)c + (double)(d & UINT64_C(255)) +
	 (double)p[d & UINT64_C(3)] + e;
}

float lj_callxs_auth_float(float a, int32_t b)
{
  lj_callxs_auth_counter++;
  return a + (float)b + 0.25f;
}

int8_t lj_callxs_auth_i8(int32_t value)
{
  (void)value;
  lj_callxs_auth_counter++;
  return INT8_C(-101);
}

uint8_t lj_callxs_auth_u8(int32_t value)
{
  (void)value;
  lj_callxs_auth_counter++;
  return UINT8_C(201);
}

int16_t lj_callxs_auth_i16(int32_t value)
{
  (void)value;
  lj_callxs_auth_counter++;
  return INT16_C(-12345);
}

uint16_t lj_callxs_auth_u16(int32_t value)
{
  (void)value;
  lj_callxs_auth_counter++;
  return UINT16_C(54321);
}

int32_t lj_callxs_auth_errno(int32_t value)
{
  errno = 1000 + (value & 31);
  return value + 1;
}

void lj_callxs_auth_store(int32_t *p, int32_t index, int32_t value)
{
  lj_callxs_auth_counter++;
  p[(uint32_t)index & 3u] = value;
}

double lj_callxs_auth_vararg(int32_t n, ...)
{
  double sum = 0.0;
  va_list ap;
  int32_t i;
  va_start(ap, n);
  for (i = 0; i < n; i++)
    sum += va_arg(ap, double);
  va_end(ap);
  lj_callxs_auth_counter++;
  return sum;
}

int32_t *lj_callxs_auth_ptr(int32_t *p)
{
  return p;
}

int32_t *lj_callxs_auth_attributed_ptr_result(int32_t *p)
{
  return p;
}

_Bool lj_callxs_auth_bool(int32_t value)
{
  lj_callxs_auth_counter++;
  return value != 0;
}

/* Export a byte-return ABI twin. The Lua declaration intentionally names it
** _Bool so the generated and interpreted paths must normalize any nonzero
** foreign return bits instead of assuming the producer canonicalized them. */
uint8_t lj_callxs_auth_bool_raw_bits(int32_t value)
{
  lj_callxs_auth_counter++;
  return (uint8_t)value;
}

_Bool lj_callxs_auth_bool_iter(int32_t state, int32_t control)
{
  (void)control;
  lj_callxs_auth_counter++;
  return state != 0;
}

enum lj_callxs_auth_enum {
  LJ_CALLXS_AUTH_ENUM_ZERO,
  LJ_CALLXS_AUTH_ENUM_SEVEN = 7
};

enum lj_callxs_auth_enum lj_callxs_auth_enum_result(int32_t value)
{
  (void)value;
  return LJ_CALLXS_AUTH_ENUM_SEVEN;
}

enum lj_callxs_auth_enum
lj_callxs_auth_attributed_enum_result(int32_t value)
{
  (void)value;
  return LJ_CALLXS_AUTH_ENUM_SEVEN;
}

int64_t lj_callxs_auth_i64_result(int32_t value)
{
  (void)value;
  return -INT64_C(123456789);
}

uint64_t lj_callxs_auth_u64_result(int32_t value)
{
  (void)value;
  lj_callxs_auth_counter++;
  return UINT64_C(4000000000);
}

uint64_t lj_callxs_auth_attributed_u64_result(int32_t value)
{
  (void)value;
  lj_callxs_auth_counter++;
  return UINT64_C(0xfedcba9876543210);
}

int32_t *lj_callxs_auth_reference_result(void)
{
  lj_callxs_auth_counter++;
  return &lj_callxs_auth_reference_value;
}

struct lj_callxs_auth_aggregate_alpha
lj_callxs_auth_aggregate_alpha_result(double bias, int32_t seed)
{
  struct lj_callxs_auth_aggregate_alpha result;
  lj_callxs_auth_counter++;
  lj_callxs_auth_aggregate_alpha_counter++;
  result.cookie = UINT64_C(0xfedcba9876540000) + (uint32_t)seed;
  result.weight = bias + (double)seed * 0.5;
  result.code = seed * 3 - 17;
  result.stamp = UINT32_C(0x13570000) + (uint32_t)seed;
  return result;
}

struct lj_callxs_auth_aggregate_beta
lj_callxs_auth_aggregate_beta_result(uint32_t seed, float scale)
{
  struct lj_callxs_auth_aggregate_beta result;
  lj_callxs_auth_counter++;
  lj_callxs_auth_aggregate_beta_counter++;
  result.debt = -INT64_C(0x0123456789abcdef);
  result.stamp = UINT32_C(0xa5a50000) + seed;
  result.ratio = scale + (float)seed * 0.25f;
  result.token = UINT64_C(0x0123456789ab0000) + seed;
  return result;
}

lj_callxs_auth_aggregate_aligned
lj_callxs_auth_aggregate_aligned_result(uint64_t seed)
{
  lj_callxs_auth_aggregate_aligned result;
  lj_callxs_auth_counter++;
  lj_callxs_auth_aggregate_aligned_counter++;
  result.lane[0] = UINT64_C(0x8000000000000000) + seed;
  result.lane[1] = UINT64_C(0x123456789abc0000) + seed;
  result.lane[2] = UINT64_C(0xfedcba9876540000) + seed;
  return result;
}

struct lj_callxs_auth_aggregate_alpha
lj_callxs_auth_aggregate_zero_result(void)
{
  struct lj_callxs_auth_aggregate_alpha result;
  lj_callxs_auth_counter++;
  lj_callxs_auth_aggregate_zero_counter++;
  result.cookie = UINT64_C(0x0102030405060708);
  result.weight = 6.25;
  result.code = INT32_C(-90210);
  result.stamp = UINT32_C(0xc001d00d);
  return result;
}

union lj_callxs_auth_aggregate_union
lj_callxs_auth_aggregate_union_result(uint64_t seed)
{
  union lj_callxs_auth_aggregate_union result;
  lj_callxs_auth_counter++;
  lj_callxs_auth_aggregate_union_counter++;
  result.lane[0] = UINT64_C(0x8899aabbccdd0000) + seed;
  result.lane[1] = UINT64_C(0x3ff4000000000000);
  result.lane[2] = UINT64_C(0x1020304050600000) + seed;
  return result;
}

struct lj_callxs_auth_aggregate_beta
lj_callxs_auth_aggregate_wide_result(uint64_t a, uint64_t b, uint64_t c,
				     uint64_t d, uint64_t e, uint64_t f,
				     uint64_t g, uint64_t h)
{
  struct lj_callxs_auth_aggregate_beta result;
  uint64_t sum = a + b + c + d + e + f + g + h;
  lj_callxs_auth_counter++;
  lj_callxs_auth_aggregate_wide_counter++;
  result.debt = -(int64_t)sum;
  result.stamp = UINT32_C(0x600d0000) + (uint32_t)sum;
  result.ratio = (float)sum * 0.25f;
  result.token = UINT64_C(0xabcdef0000000000) + sum;
  return result;
}

void lj_callxs_auth_aggregate_reset(void)
{
  lj_callxs_auth_aggregate_alpha_counter = 0;
  lj_callxs_auth_aggregate_beta_counter = 0;
  lj_callxs_auth_aggregate_aligned_counter = 0;
  lj_callxs_auth_aggregate_zero_counter = 0;
  lj_callxs_auth_aggregate_union_counter = 0;
  lj_callxs_auth_aggregate_wide_counter = 0;
}

uint32_t lj_callxs_auth_aggregate_alpha_count(void)
{
  return lj_callxs_auth_aggregate_alpha_counter;
}

uint32_t lj_callxs_auth_aggregate_beta_count(void)
{
  return lj_callxs_auth_aggregate_beta_counter;
}

uint32_t lj_callxs_auth_aggregate_aligned_count(void)
{
  return lj_callxs_auth_aggregate_aligned_counter;
}

uint32_t lj_callxs_auth_aggregate_zero_count(void)
{
  return lj_callxs_auth_aggregate_zero_counter;
}

uint32_t lj_callxs_auth_aggregate_union_count(void)
{
  return lj_callxs_auth_aggregate_union_counter;
}

uint32_t lj_callxs_auth_aggregate_wide_count(void)
{
  return lj_callxs_auth_aggregate_wide_counter;
}

void lj_callxs_auth_reset(void)
{
  lj_callxs_auth_counter = 0;
}

int32_t lj_callxs_auth_count(void)
{
  return lj_callxs_auth_counter;
}

int32_t lj_callxs_auth_once(int32_t value)
{
  lj_callxs_auth_counter++;
  return value + 9;
}

int32_t lj_callxs_auth_iter(int32_t state, int32_t control)
{
  (void)state;
  lj_callxs_auth_counter++;
  return control + 1;
}
