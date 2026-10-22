/* Native ABI oracle for the ARM64 interpreter aggregate-call fixture. */
#include <errno.h>

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
