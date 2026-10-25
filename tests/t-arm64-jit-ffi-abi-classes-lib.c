/* Native ABI-class matrix functions with an optional active-frame witness. */
#include <stdint.h>

typedef void (*abi_observer)(unsigned id);

static abi_observer observer;
static unsigned call_count[8];

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
  return a+b+c+d+e+f+g+h+i;
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
