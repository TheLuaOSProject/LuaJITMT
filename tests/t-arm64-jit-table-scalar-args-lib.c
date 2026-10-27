/* Foreign validators for rooted table scalar CALLXS arguments. */

#include <errno.h>
#include <stdint.h>

static int32_t mode;
static int32_t i32_calls, double_calls, float_calls, bool_calls;
static int32_t i32_9_calls, mixed11_calls, total_calls;

static int32_t load_count(const int32_t *count)
{ return __atomic_load_n(count, __ATOMIC_ACQUIRE); }

static void reset_count(int32_t *count)
{ __atomic_store_n(count, 0, __ATOMIC_RELEASE); }

static void record_call(int32_t *count)
{
  (void)__atomic_add_fetch(count, 1, __ATOMIC_ACQ_REL);
  (void)__atomic_add_fetch(&total_calls, 1, __ATOMIC_ACQ_REL);
  errno = EDOM;
}

void lj_table_scalar_configure(int32_t new_mode)
{
  mode = new_mode;
  reset_count(&i32_calls);
  reset_count(&double_calls);
  reset_count(&float_calls);
  reset_count(&bool_calls);
  reset_count(&i32_9_calls);
  reset_count(&mixed11_calls);
  reset_count(&total_calls);
}

int32_t lj_table_scalar_i32_count(void) { return load_count(&i32_calls); }
int32_t lj_table_scalar_double_count(void) { return load_count(&double_calls); }
int32_t lj_table_scalar_float_count(void) { return load_count(&float_calls); }
int32_t lj_table_scalar_bool_count(void) { return load_count(&bool_calls); }
int32_t lj_table_scalar_i32_9_count(void) { return load_count(&i32_9_calls); }
int32_t lj_table_scalar_mixed11_count(void) { return load_count(&mixed11_calls); }
int32_t lj_table_scalar_total_count(void) { return load_count(&total_calls); }

int32_t lj_table_scalar_i32(int32_t value)
{
  int32_t expected = mode == 0 ? -17 : mode == 1 ? -19 : 1;
  record_call(&i32_calls);
  return value == expected ? value : -10000;
}

double lj_table_scalar_double(double value)
{
  int ok = (mode == 3 && value == 2.5) ||
           (mode == 8 && value == 4.25) ||
           (mode == 10 && value == 5.5);
  record_call(&double_calls);
  return ok ? value : -10000.0;
}

float lj_table_scalar_float(float value)
{
  int ok = (mode == 4 && value == 1.5f) || (mode == 9 && value == 2.75f);
  record_call(&float_calls);
  return ok ? value : -10000.0f;
}

int32_t lj_table_scalar_bool_i32(int32_t value)
{
  record_call(&bool_calls);
  return ((mode == 11 && value == 0) ||
          (mode != 11 && value == 1)) ? value : -10000;
}

#define DECL9 int32_t x0, int32_t x1, int32_t x2, int32_t x3, int32_t x4, \
              int32_t x5, int32_t x6, int32_t x7, int32_t x8

int32_t lj_table_scalar_i32_9(DECL9)
{
  const int32_t got[9] = { x0,x1,x2,x3,x4,x5,x6,x7,x8 };
  int32_t start = mode == 5 ? 11 : 1;
  int32_t sum = 0;
  unsigned i;
  for (i = 0; i < 9; i++) {
    if (got[i] != start + (int32_t)i) {
      record_call(&i32_9_calls);
      return -10000;
    }
    sum += got[i];
  }
  record_call(&i32_9_calls);
  return sum;
}

/* Nine GPR-class values cross the register/stack boundary with two FPRs. */
double lj_table_scalar_mixed11(int32_t a0, double d0, int32_t a1, float f0,
                               int32_t a2, int32_t a3, int32_t a4,
                               int32_t a5, int32_t a6, int32_t a7,
                               int32_t a8)
{
  const int32_t got[9] = { a0,a1,a2,a3,a4,a5,a6,a7,a8 };
  const double d = mode == 7 ? 2.5 : 1.25;
  const float f = mode == 7 ? 3.5f : 2.5f;
  const int32_t start = mode == 7 ? 11 : 1;
  double sum = d0 + (double)f0;
  unsigned i;
  for (i = 0; i < 9; i++) {
    if (got[i] != start + (int32_t)i) {
      record_call(&mixed11_calls);
      return -10000.0;
    }
    sum += (double)got[i];
  }
  if (d0 != d || f0 != f) {
    record_call(&mixed11_calls);
    return -10000.0;
  }
  record_call(&mixed11_calls);
  return sum;
}

#undef DECL9
