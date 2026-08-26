/*
** t-arm64-cas128-contract.c - native Apple AArch64 128-bit CAS contract.
**
** This is intentionally an artifact fixture as well as a runtime smoke.  The
** CI wrapper disassembles arm64_cas128_probe() and rejects compiler atomic
** helper imports, because a functionally correct lock-based fallback would
** violate the lockless runtime contract.
*/

#include <assert.h>
#include <stdint.h>

#include "lj_atomic.h"

#if !defined(__APPLE__) || !defined(__aarch64__)
#error "t-arm64-cas128-contract requires native Apple AArch64"
#endif

__attribute__((noinline))
int arm64_cas128_probe(la_u128 *value, la_u128 *expected, la_u128 desired)
{
  return la_cas128(value, expected, desired);
}

int main(void)
{
  la_u128 value, expected, desired, observed;

  value.lo = value.hi = 0;
  expected = value;
  desired.lo = UINT64_C(0x1122334455667788);
  desired.hi = UINT64_C(0x8877665544332211);
  assert(arm64_cas128_probe(&value, &expected, desired));
  assert(value.lo == desired.lo && value.hi == desired.hi);

  expected.lo = expected.hi = 0;
  assert(!arm64_cas128_probe(&value, &expected, value));
  assert(expected.lo == value.lo && expected.hi == value.hi);

  observed = la_load128_acq(&value);
  assert(observed.lo == value.lo && observed.hi == value.hi);
  return 0;
}
