/* Controlled no-callback ABI targets for the authentic generic CALLXS gate. */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

static int32_t lj_callxs_auth_counter;

int32_t lj_callxs_auth_add(int32_t a, int32_t b)
{
  return a + b + 3;
}

uint32_t lj_callxs_auth_u32(uint32_t a, int32_t b)
{
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
  return a + (float)b + 0.25f;
}

int8_t lj_callxs_auth_i8(int32_t value)
{
  (void)value;
  return INT8_C(-101);
}

uint8_t lj_callxs_auth_u8(int32_t value)
{
  (void)value;
  return UINT8_C(201);
}

int16_t lj_callxs_auth_i16(int32_t value)
{
  (void)value;
  return INT16_C(-12345);
}

uint16_t lj_callxs_auth_u16(int32_t value)
{
  (void)value;
  return UINT16_C(54321);
}

int32_t lj_callxs_auth_errno(int32_t value)
{
  errno = 1000 + (value & 31);
  return value + 1;
}

void lj_callxs_auth_store(int32_t *p, int32_t index, int32_t value)
{
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
  return sum;
}

int32_t *lj_callxs_auth_ptr(int32_t *p)
{
  return p;
}

_Bool lj_callxs_auth_bool(int32_t value)
{
  return value != 0;
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

int64_t lj_callxs_auth_i64_result(int32_t value)
{
  (void)value;
  return -INT64_C(123456789);
}

uint64_t lj_callxs_auth_u64_result(int32_t value)
{
  (void)value;
  return UINT64_C(4000000000);
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
