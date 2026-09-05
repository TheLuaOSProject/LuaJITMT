/*
** Shared-library target for ccall small-struct overflow fixtures.
*/

#if defined(_WIN32)
#define LJ_M7_EXPORT __declspec(dllexport)
#else
#define LJ_M7_EXPORT __attribute__((visibility("default")))
#endif

typedef struct lj_m7_ccall_struct_overflow_t {
  int x;
} lj_m7_ccall_struct_overflow_t;

LJ_M7_EXPORT int
lj_m7_ccall_struct_overflow(int a, int b, int c, int d, int e, int f,
			    lj_m7_ccall_struct_overflow_t s)
{
  return a + b + c + d + e + f + s.x;
}
