/*
** Shared library for traced FFI C-call native-state probes.
*/

#include <time.h>

static int lj_m7_ccall_jit_values[4] = { 11, 22, 33, 44 };
static int lj_m7_ccall_jit_void_count;

static void sleep_ms(int ms)
{
  struct timespec ts;
  if (ms <= 0)
    return;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0)
    ;
}

int lj_m7_ccall_jit_sleep_i32(int ms)
{
  sleep_ms(ms);
  return ms + 7;
}

int lj_m7_ccall_jit_add2_i32(int a, int b)
{
  return a + b + 3;
}

double lj_m7_ccall_jit_num0(void)
{
  return 1.5;
}

double lj_m7_ccall_jit_num1(double a)
{
  return a + 0.5;
}

double lj_m7_ccall_jit_num2(double a, double b)
{
  return a + b + 0.25;
}

float lj_m7_ccall_jit_flt0(void)
{
  return 1.5f;
}

float lj_m7_ccall_jit_flt1(float a)
{
  return a + 0.5f;
}

float lj_m7_ccall_jit_flt2(float a, float b)
{
  return a + b + 0.25f;
}

int lj_m7_ccall_jit_void_count_i32(void)
{
  return lj_m7_ccall_jit_void_count;
}

void lj_m7_ccall_jit_void0(void)
{
  lj_m7_ccall_jit_void_count++;
}

void lj_m7_ccall_jit_store_i32(int *p, int v)
{
  *p = v + 9;
}

unsigned int lj_m7_ccall_jit_u32(unsigned int a)
{
  return a + 1u;
}

int *lj_m7_ccall_jit_ptr0(void)
{
  return lj_m7_ccall_jit_values;
}

int lj_m7_ccall_jit_ptr_read_i32(int *p)
{
  return *p;
}

int lj_m7_ccall_jit_ptr_sum_i32(int *a, int *b)
{
  return *a + *b;
}

int lj_m7_ccall_jit_i32_ptr_read_i32(int offset, int *p)
{
  return p[offset];
}

int *lj_m7_ccall_jit_ptr_add_i32(int *p, int offset)
{
  return p + offset;
}
