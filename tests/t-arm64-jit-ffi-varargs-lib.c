/* Runtime oracles for the Darwin ARM64 promoted-scalar varargs tranche. */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

typedef struct VarargSret24 {
  uint64_t lo, mid, hi;
} VarargSret24;

typedef struct VarargIntPair8 {
  uint32_t a, b;
} VarargIntPair8;

typedef void (*va_observer)(unsigned id);
static va_observer observer;
static unsigned call_count[14];

void arm64_va_install_observer(va_observer fn)
{
  observer = fn;
}

unsigned arm64_va_calls(unsigned id)
{
  return id > 0 && id < sizeof(call_count)/sizeof(call_count[0]) ?
    call_count[id] : 0;
}

static void note(unsigned id)
{
  if (id > 0 && id < sizeof(call_count)/sizeof(call_count[0]))
    call_count[id]++;
  if (observer != 0) observer(id);
}

double arm64_va_noextra(int32_t fixed, ...)
{
  note(1);
  return (double)fixed + 0.25;
}

double arm64_va_mixed(int32_t fixed, double prefix, ...)
{
  va_list ap;
  int32_t truth;
  double first, second, last;
  const char *text;
  void *nullp;
  note(2);
  va_start(ap, prefix);
  first = va_arg(ap, double);
  second = va_arg(ap, double);
  truth = va_arg(ap, int);
  text = va_arg(ap, const char *);
  nullp = va_arg(ap, void *);
  last = va_arg(ap, double);
  va_end(ap);
  if (prefix != (double)fixed+0.125 || first != (double)fixed+1.0 ||
      second != (double)fixed+0.25 || truth != 1 ||
      text == 0 || strcmp(text, "mixed") != 0 || nullp != 0 ||
      last != (double)fixed+0.5)
    return -2.0;
  return (double)fixed + prefix + first + second + truth + last;
}

int32_t arm64_va_gpr_overflow(int32_t a0, int32_t a1, int32_t a2,
                               int32_t a3, int32_t a4, int32_t a5,
                               int32_t a6, int32_t a7, int32_t a8, ...)
{
  va_list ap;
  double first, second;
  const char *text;
  void *nullp;
  note(3);
  va_start(ap, a8);
  first = va_arg(ap, double);
  second = va_arg(ap, double);
  text = va_arg(ap, const char *);
  nullp = va_arg(ap, void *);
  va_end(ap);
  return a1 == a0+1 && a2 == a0+2 && a3 == a0+3 &&
         a4 == a0+4 && a5 == a0+5 && a6 == a0+6 && a7 == a0+7 &&
         a8 == a0+8 && first == (double)a0+9.0 &&
         second == (double)a0+0.5 && text != 0 &&
         strcmp(text, "gpr-tail") == 0 && nullp == 0 ? a0+303 : -3;
}

double arm64_va_fpr_overflow(double f0, double f1, double f2, double f3,
                             double f4, double f5, double f6, double f7,
                             double f8, ...)
{
  va_list ap;
  int32_t truth;
  double tail0, tail1;
  const char *text;
  note(4);
  va_start(ap, f8);
  truth = va_arg(ap, int);
  tail0 = va_arg(ap, double);
  text = va_arg(ap, const char *);
  tail1 = va_arg(ap, double);
  va_end(ap);
  return f0 == 1.0 && f1 == 2.0 && f2 == 3.0 && f3 == 4.0 &&
         f4 == 5.0 && f5 == 6.0 && f6 == 7.0 && f7 == 8.0 && f8 == 9.0 &&
         truth == 1 && tail0 == 9.5 && text != 0 &&
         strcmp(text, "fpr-tail") == 0 && tail1 == 10.25 ? 404.0 : -4.0;
}

double arm64_va_pressure(int32_t fixed, ...)
{
  va_list ap;
  double i0;
  int32_t b0, b1;
  double d0, d1, d2, d3;
  const char *s0, *s1;
  void *nullp;
  note(5);
  va_start(ap, fixed);
  i0 = va_arg(ap, double);
  d0 = va_arg(ap, double);
  b0 = va_arg(ap, int);
  s0 = va_arg(ap, const char *);
  nullp = va_arg(ap, void *);
  d1 = va_arg(ap, double);
  b1 = va_arg(ap, int);
  s1 = va_arg(ap, const char *);
  d2 = va_arg(ap, double);
  d3 = va_arg(ap, double);
  va_end(ap);
  if (i0 != fixed || d0 != (double)fixed+0.25 || b0 != 1 ||
      s0 == 0 || strcmp(s0, "pressure-a") != 0 || nullp != 0 ||
      d1 != (double)fixed+0.5 || b1 != 0 || s1 == 0 ||
      strcmp(s1, "pressure-b") != 0 || d2 != (double)fixed+0.75 ||
      d3 != (double)fixed+1.0)
    return -5.0;
  return (double)fixed + i0 + d0 + d1 + d2 + d3 + b0 + b1;
}

int32_t arm64_va_rounding(int32_t fixed, ...)
{
  va_list ap;
  double negative_zero, decimal;
  uint64_t bits;
  note(6);
  va_start(ap, fixed);
  negative_zero = va_arg(ap, double);
  decimal = va_arg(ap, double);
  va_end(ap);
  memcpy(&bits, &decimal, sizeof(bits));
  return fixed == 6 && signbit(negative_zero) && negative_zero == 0.0 &&
         bits == UINT64_C(0x3fb999999999999a) ? 606 : -6;
}

double arm64_va_lua_producer(int32_t value)
{
  note(7);
  return (double)value + 0.5;
}

double arm64_va_callm_consumer(int32_t fixed, ...)
{
  va_list ap;
  double value;
  note(8);
  va_start(ap, fixed);
  value = va_arg(ap, double);
  va_end(ap);
  return fixed == 17 && value >= 0.5 && value < 100.5 ?
    (double)fixed + value : -8.0;
}

double arm64_va_ordinary_consumer(int32_t fixed, ...)
{
  va_list ap;
  double value;
  note(9);
  va_start(ap, fixed);
  value = va_arg(ap, double);
  va_end(ap);
  return fixed == 17 && value >= 0.5 && value < 100.5 ?
    (double)fixed + value : -9.0;
}

double arm64_va_cdata_tail(uint32_t fixed, ...)
{
  va_list ap;
  int8_t signed_byte;
  double promoted_float;
  uint16_t unsigned_half;
  double direct_double;
  long long signed_wide;
  int32_t signed_word;
  double second_double;
  int8_t second_byte;
  uint64_t unsigned_wide;
  note(10);
  va_start(ap, fixed);
  signed_byte = (int8_t)va_arg(ap, int);
  promoted_float = va_arg(ap, double);
  unsigned_half = (uint16_t)va_arg(ap, int);
  direct_double = va_arg(ap, double);
  signed_wide = va_arg(ap, long long);
  signed_word = va_arg(ap, int);
  second_double = va_arg(ap, double);
  second_byte = (int8_t)va_arg(ap, int);
  unsigned_wide = va_arg(ap, uint64_t);
  va_end(ap);
  if (fixed != 23 || signed_byte != -17 || promoted_float != 2.5 ||
      unsigned_half != 31 || direct_double != 3.25 ||
      signed_wide != 0x12345678LL || signed_word != 47 ||
      second_double != 4.5 || second_byte != -9 ||
      unsigned_wide != UINT64_C(0x23456789)) return -10.0;
  return 88.25;
}

VarargSret24 arm64_va_sret_cdata_tail(uint32_t fixed, ...)
{
  va_list ap;
  int32_t signed_word;
  double value;
  VarargSret24 result;
  note(11);
  va_start(ap, fixed);
  signed_word = va_arg(ap, int);
  value = va_arg(ap, double);
  va_end(ap);
  result.lo = fixed == 23 && signed_word == -17 && value == 2.5 ?
              UINT64_C(0x112233445566) : UINT64_C(0xdeadbeef);
  result.mid = UINT64_C(0x123456789abc);
  result.hi = UINT64_C(0x6543210);
  return result;
}

VarargSret24 arm64_va_sret_intagg_prefix(VarargIntPair8 pair,
                                         int32_t fixed, ...)
{
  va_list ap;
  int32_t signed_word;
  double value;
  VarargSret24 result;
  note(12);
  va_start(ap, fixed);
  signed_word = va_arg(ap, int);
  value = va_arg(ap, double);
  va_end(ap);
  result.lo = pair.a == 3 && pair.b == 5 && fixed == 7 &&
              signed_word == -17 && value == 2.5 ? 15u : UINT64_C(0xdeadbeef);
  result.mid = UINT64_C(20);
  result.hi = UINT64_C(0x4004000000000000);
  return result;
}

VarargSret24 arm64_va_sret_gpr8_intagg_prefix(
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
    VarargIntPair8 pair, int32_t fixed, ...)
{
  va_list ap;
  int32_t signed_word;
  double value;
  VarargSret24 result;
  note(13);
  va_start(ap, fixed);
  signed_word = va_arg(ap, int);
  value = va_arg(ap, double);
  va_end(ap);
  result.lo = pair.a == 3 && pair.b == 5 && fixed == 7 &&
              signed_word == -17 && value == 2.5 ?
              a0+a1+a2+a3+a4+a5+a6+a7+15u :
              UINT64_C(0xdeadbeef);
  result.mid = UINT64_C(20);
  result.hi = UINT64_C(0x4004000000000000);
  return result;
}
