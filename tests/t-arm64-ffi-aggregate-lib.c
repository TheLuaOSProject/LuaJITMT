/* Native ABI oracle for the ARM64 interpreter aggregate-call fixture. */
#include <errno.h>
#include <stdarg.h>

typedef struct { float v[2][2]; } Float4;
typedef struct { double a, b; } Double2;
typedef struct { Double2 inner; double c; } Double3;
typedef union { double a; double b[2]; } DoubleUnion;
typedef struct { double a; int b; } Mixed;
typedef struct { long long a, b, c; } Large;
typedef struct { double _Complex z; } Complex;

Float4 arm64_float4(Float4 x)
{
  x.v[0][0] += 1; x.v[0][1] += 2;
  x.v[1][0] += 3; x.v[1][1] += 4;
  return x;
}

Double3 arm64_double3(Double3 x)
{
  x.inner.a += 1; x.inner.b += 2; x.c += 3;
  return x;
}

DoubleUnion arm64_union(DoubleUnion x)
{
  x.b[0] += 1; x.b[1] += 2;
  return x;
}

Mixed arm64_mixed(Mixed x)
{
  x.a += 0.5; x.b += 7;
  return x;
}

Large arm64_large(Large x)
{
  x.a += 10; x.b += 20; x.c += 30;
  errno = 73;
  return x;
}

Complex arm64_complex(Complex x)
{
  x.z += 1.0 + 2.0i;
  return x;
}

double arm64_fp_overflow(double a, double b, double c, double d,
                         double e, double f, double g, Double2 h, double i)
{
  return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h.a + 9*h.b + 10*i;
}

double arm64_gp_overflow(int a, int b, int c, int d, int e, int f, int g,
                         Mixed h, int i)
{
  return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h.a + 9*h.b + 10*i;
}

/* Unnamed Apple ARM64 arguments use 8-byte slots even for promoted ints. */
int arm64_vararg_ints(int count, ...)
{
  va_list ap;
  int i, result = 0;
  va_start(ap, count);
  for (i = 1; i <= count; i++) result += i * va_arg(ap, int);
  va_end(ap);
  return result;
}

double arm64_vararg_mixed(double named, ...)
{
  va_list ap;
  int a, c;
  double b, d;
  const Float4 *e;
  const Large *f;
  va_start(ap, named);
  a = va_arg(ap, int);
  b = va_arg(ap, double);
  c = va_arg(ap, int);
  d = va_arg(ap, double);
  e = va_arg(ap, const Float4 *);
  f = va_arg(ap, const Large *);
  va_end(ap);
  return named + a + 2*b + 3*c + 4*d +
    5*e->v[0][0] + 6*e->v[0][1] + 7*e->v[1][0] + 8*e->v[1][1] +
    9*f->a + 10*f->b + 11*f->c;
}

/* Exhaust GPRs, pack two named bytes, then align the unnamed arguments. */
int arm64_vararg_after_stack(int a, int b, int c, int d,
                            int e, int f, int g, int h,
                            int i, signed char j, signed char k,
                            double named, ...)
{
  va_list ap;
  int l, m, n;
  va_start(ap, named);
  l = va_arg(ap, int);
  m = va_arg(ap, int);
  n = va_arg(ap, int);
  va_end(ap);
  return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h +
    9*i + 10*j + 11*k + 12*l + 13*m + 14*n + 15*(int)named;
}

/* Fixed narrow arguments must retain the packed Apple stack convention. */
int arm64_named_stack(int a, int b, int c, int d,
                      int e, int f, int g, int h,
                      signed char i, unsigned char j, short k, int l)
{
  return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h +
    9*i + 10*j + 11*k + 12*l;
}
