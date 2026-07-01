/*
** Shared library for traced FFI C-call native-state probes.
*/

#include <stdint.h>
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

int64_t lj_m7_ccall_jit_i64_0(void)
{
  return 17;
}

int64_t lj_m7_ccall_jit_i64_i32(int32_t a)
{
  return INT64_C(0x100000000) + (int64_t)a;
}

int64_t lj_m7_ccall_jit_i64_ptr(int *p)
{
  return INT64_C(0x100000000) + (int64_t)*p;
}

int64_t lj_m7_ccall_jit_i64_i32_ptr(int32_t offset, int *p)
{
  return INT64_C(0x100000000) + (int64_t)p[offset];
}

int64_t lj_m7_ccall_jit_i64_i64(int64_t a)
{
  return a + 1;
}

int8_t lj_m7_ccall_jit_i8_0(void)
{
  return -7;
}

int8_t lj_m7_ccall_jit_i8_i32(int32_t a)
{
  return (int8_t)(a - 8);
}

uint8_t lj_m7_ccall_jit_u8_0(void)
{
  return 250;
}

uint8_t lj_m7_ccall_jit_u8_ptr(int *p)
{
  return (uint8_t)(*p + 200);
}

int16_t lj_m7_ccall_jit_i16_0(void)
{
  return -1234;
}

int16_t lj_m7_ccall_jit_i16_i32_ptr(int32_t offset, int *p)
{
  return (int16_t)(p[offset] - 2000);
}

uint16_t lj_m7_ccall_jit_u16_0(void)
{
  return 60000;
}

uint16_t lj_m7_ccall_jit_u16_i32(int32_t a)
{
  return (uint16_t)(60000 + a);
}

int lj_m7_ccall_jit_i8_arg_i32(int8_t a)
{
  return (int)a + 5;
}

double lj_m7_ccall_jit_num0(void)
{
  return 1.5;
}

double lj_m7_ccall_jit_num_i32(int32_t a)
{
  return (double)a + 0.75;
}

double lj_m7_ccall_jit_num_ptr(int *p)
{
  return (double)*p + 0.25;
}

double lj_m7_ccall_jit_num_flt(float a)
{
  return (double)a + 0.125;
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

int32_t lj_m7_ccall_jit_i32_num(double a)
{
  return (int32_t)a + 3;
}

int32_t lj_m7_ccall_jit_i32_flt(float a)
{
  return (int32_t)a + 4;
}

void lj_m7_ccall_jit_void0(void)
{
  lj_m7_ccall_jit_void_count++;
}

void lj_m7_ccall_jit_void_num(double a)
{
  lj_m7_ccall_jit_void_count += (int)a;
}

void lj_m7_ccall_jit_void_flt(float a)
{
  lj_m7_ccall_jit_void_count += (int)a;
}

float lj_m7_ccall_jit_flt_num(double a)
{
  return (float)(a + 0.5);
}

void lj_m7_ccall_jit_store_i32(int *p, int v)
{
  *p = v + 9;
}

unsigned int lj_m7_ccall_jit_u32(unsigned int a)
{
  return a + 1u;
}

uint32_t lj_m7_ccall_jit_u32_i32(int32_t a)
{
  return 0xf0000000u + (uint32_t)a;
}

uint32_t lj_m7_ccall_jit_u32_ptr(int *p)
{
  return 0xf0000000u + (uint32_t)*p;
}

uint32_t lj_m7_ccall_jit_u32_i32_ptr(int32_t offset, int *p)
{
  return 0xf0000000u + (uint32_t)p[offset];
}

uint32_t lj_m7_ccall_jit_u32_0(void)
{
  return 0xf0000001u;
}

uint64_t lj_m7_ccall_jit_u64(uint64_t a)
{
  return a + 1;
}

uint64_t lj_m7_ccall_jit_u64_i32(int32_t a)
{
  return UINT64_C(0x100000000) + (uint64_t)(uint32_t)a;
}

uint64_t lj_m7_ccall_jit_u64_ptr(int *p)
{
  return UINT64_C(0x100000000) + (uint64_t)(uint32_t)*p;
}

uint64_t lj_m7_ccall_jit_u64_i32_ptr(int32_t offset, int *p)
{
  return UINT64_C(0x100000000) + (uint64_t)(uint32_t)p[offset];
}

uint64_t lj_m7_ccall_jit_u64_0(void)
{
  return ~(uint64_t)0;
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

int *lj_m7_ccall_jit_ptr_num(double a)
{
  return lj_m7_ccall_jit_values + ((int)a & 3);
}

int lj_m7_ccall_jit_i32_ptr_ulong_i32(int *p, unsigned long n, int offset)
{
  return p[offset] + (int)(n & 1023UL);
}
