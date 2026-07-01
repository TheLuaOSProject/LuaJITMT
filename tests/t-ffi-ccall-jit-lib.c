/*
** Shared library for traced FFI C-call native-state probes.
*/

#include <time.h>

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
