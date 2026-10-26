/* Native ABI-class matrix functions with an optional active-frame witness. */
#include <stdint.h>

typedef void (*abi_observer)(unsigned id);

static abi_observer observer;
static unsigned call_count[14];

void arm64_abi_install_observer(abi_observer fn)
{
  observer = fn;
}

static void note(unsigned id)
{
  if (id > 0 && id < sizeof(call_count)/sizeof(call_count[0]))
    call_count[id]++;
  if (observer != 0) observer(id);
}

unsigned arm64_abi_calls(unsigned id)
{
  return id > 0 && id < sizeof(call_count)/sizeof(call_count[0]) ?
    call_count[id] : 0;
}

int32_t arm64_abi_i32_six(int32_t a, int32_t b, int32_t c,
                          int32_t d, int32_t e, int32_t f)
{
  note(1);
  return a + 2*b + 3*c + 4*d + 5*e + 6*f;
}

double arm64_abi_mixed(double a, int32_t b, double c,
                       int32_t d, double e)
{
  note(2);
  return a + (double)b + c + (double)d + e;
}

int32_t arm64_abi_many_a(int32_t a, int32_t b)
{
  note(3);
  return a + b;
}

int32_t arm64_abi_many_b(int32_t a, int32_t b)
{
  note(4);
  return a - b;
}

int32_t arm64_abi_many_c(int32_t a, int32_t b)
{
  note(5);
  return a ^ b;
}

int32_t arm64_abi_unused_result(int32_t value)
{
  note(6);
  return value * 7;
}

float arm64_abi_float_result(float a, float b)
{
  note(7);
  return a + b;
}

typedef struct { int64_t a, b; } arm64_abi_pair;

int32_t arm64_abi_stack9(int32_t a, int32_t b, int32_t c, int32_t d,
                         int32_t e, int32_t f, int32_t g, int32_t h,
                         int32_t i)
{
  note(8);
  return b == a+1 && c == a+2 && d == a+3 && e == a+4 &&
         f == a+5 && g == a+6 && h == a+7 && i == a+8 ? a+108 : -8;
}

double arm64_abi_fpr9(double a, double b, double c, double d, double e,
                      double f, double g, double h, double i)
{
  note(9);
  return b == a+1.0 && c == a+2.0 && d == a+3.0 && e == a+4.0 &&
         f == a+5.0 && g == a+6.0 && h == a+7.0 && i == a+8.0 ?
         a*100.0+800.0 : -9.0;
}

int32_t arm64_abi_banks_exhausted(
    int32_t i0, double f0, int32_t i1, double f1,
    int32_t i2, double f2, int32_t i3, double f3,
    int32_t i4, double f4, int32_t i5, double f5,
    int32_t i6, double f6, int32_t i7, double f7,
    int32_t i8, double f8)
{
  note(10);
  return i1 == i0+1 && i2 == i0+2 && i3 == i0+3 &&
         i4 == i0+4 && i5 == i0+5 && i6 == i0+6 &&
         i7 == i0+7 && i8 == i0+8 &&
         f1 == f0+1.0 && f2 == f0+2.0 && f3 == f0+3.0 &&
         f4 == f0+4.0 && f5 == f0+5.0 && f6 == f0+6.0 &&
         f7 == f0+7.0 && f8 == f0+8.0 ? 1010 : -10;
}

float arm64_abi_fpr9_float(float a, float b, float c, float d, float e,
                           float f, float g, float h, float i)
{
  note(12);
  return a >= 1.09f && a < 1.2f && b >= 2.19f && b < 2.3f &&
         c >= 3.29f && c < 3.4f && d >= 4.39f && d < 4.5f &&
         e >= 5.49f && e < 5.6f && f >= 6.59f && f < 6.7f &&
         g >= 7.69f && g < 7.8f && h >= 8.79f && h < 8.9f &&
         i >= 9.89f && i < 10.1f ? i : (float)-12.0;
}

float arm64_abi_mixed_stack_float(
    int32_t i0, float f0, int32_t i1, float f1,
    int32_t i2, float f2, int32_t i3, float f3,
    int32_t i4, float f4, int32_t i5, float f5,
    int32_t i6, float f6, int32_t i7, float f7,
    int32_t i8, float f8)
{
  note(13);
  if (i1 != i0+1 || i2 != i0+2 || i3 != i0+3 || i4 != i0+4 ||
      i5 != i0+5 || i6 != i0+6 || i7 != i0+7 || i8 != i0+8 ||
      !(f0 >= 1.09f && f0 < 1.2f && f1 >= 2.19f && f1 < 2.3f &&
        f2 >= 3.29f && f2 < 3.4f && f3 >= 4.39f && f3 < 4.5f &&
        f4 >= 5.49f && f4 < 5.6f && f5 >= 6.59f && f5 < 6.7f &&
        f6 >= 7.69f && f6 < 7.8f && f7 >= 8.79f && f7 < 8.9f &&
        f8 >= 9.89f && f8 < 10.1f))
    return (float)-13.0;
  return f8;
}

int32_t arm64_abi_compact_stack(
    int32_t i0, int32_t i1, int32_t i2, int32_t i3,
    int32_t i4, int32_t i5, int32_t i6, int32_t i7,
    double f0, double f1, double f2, double f3,
    double f4, double f5, double f6, double f7,
    int8_t narrow8, int16_t narrow16, double tail_double,
    void *pointer, int32_t tail_int)
{
  note(11);
  return i1 == i0+1 && i2 == i0+2 && i3 == i0+3 &&
         i4 == i0+4 && i5 == i0+5 && i6 == i0+6 && i7 == i0+7 &&
         f1 == f0+1.0 && f2 == f0+2.0 && f3 == f0+3.0 &&
         f4 == f0+4.0 && f5 == f0+5.0 && f6 == f0+6.0 && f7 == f0+7.0 &&
         narrow8 == -7 && narrow16 == 1234 && tail_double == f0+8.0 &&
         pointer == (void *)(uintptr_t)0x12345678u && tail_int == 0x2468 ?
         1111 : -11;
}

arm64_abi_pair arm64_abi_aggregate_arg(arm64_abi_pair p)
{
  return p;
}

arm64_abi_pair arm64_abi_aggregate_result(int32_t value)
{
  arm64_abi_pair p = { value, value+1 };
  return p;
}

int32_t arm64_abi_vararg(int32_t fixed, ...)
{
  return fixed;
}
