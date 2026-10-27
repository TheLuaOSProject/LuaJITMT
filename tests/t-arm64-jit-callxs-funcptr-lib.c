/* Foreign functions for ARM64 CALLXS through cached function-pointer CTypes. */

#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

static int32_t zero_calls;
static int32_t vararg_calls;

static int32_t load_count(const int32_t *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

void lj_callxs_funcptr_configure(void)
{
  __atomic_store_n(&zero_calls, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&vararg_calls, 0, __ATOMIC_RELEASE);
}

int32_t lj_callxs_funcptr_zero_count(void)
{
  return load_count(&zero_calls);
}

int32_t lj_callxs_funcptr_vararg_count(void)
{
  return load_count(&vararg_calls);
}

int32_t lj_callxs_funcptr_zero(void)
{
  (void)__atomic_add_fetch(&zero_calls, 1, __ATOMIC_ACQ_REL);
  errno = EDOM;
  return 7;
}

int32_t lj_callxs_funcptr_vararg(int32_t prefix, ...)
{
  va_list ap;
  double number;
  int truth;
  const char *text;
  (void)__atomic_add_fetch(&vararg_calls, 1, __ATOMIC_ACQ_REL);
  va_start(ap, prefix);
  number = va_arg(ap, double);
  truth = va_arg(ap, int);
  text = va_arg(ap, const char *);
  va_end(ap);
  errno = EDOM;
  return prefix + (int32_t)number + truth +
         (text != NULL ? (int32_t)strlen(text) : -1000);
}
