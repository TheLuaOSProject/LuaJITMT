/* Foreign side of the exact Darwin ARM64 const char *(const char *) proof. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

static int32_t pointer_lifecycle_calls;
static int32_t pointer_lifecycle_first_errno;
static int32_t pointer_lifecycle_last_errno;
static int32_t pointer_lifecycle_shift;
static int32_t pointer_lifecycle_negative_calls;

static int32_t pointer_lifecycle_load(const int32_t *value)
{
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void pointer_lifecycle_store(int32_t *target, int32_t value)
{
  __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

void lj_callxs_arm64_pointer_lifecycle_configure(int32_t shift)
{
  pointer_lifecycle_store(&pointer_lifecycle_calls, 0);
  pointer_lifecycle_store(&pointer_lifecycle_first_errno, 0);
  pointer_lifecycle_store(&pointer_lifecycle_last_errno, 0);
  pointer_lifecycle_store(&pointer_lifecycle_shift, shift);
}

int32_t lj_callxs_arm64_pointer_lifecycle_count(void)
{
  return pointer_lifecycle_load(&pointer_lifecycle_calls);
}

int32_t lj_callxs_arm64_pointer_lifecycle_first_errno_value(void)
{
  return pointer_lifecycle_load(&pointer_lifecycle_first_errno);
}

int32_t lj_callxs_arm64_pointer_lifecycle_last_errno_value(void)
{
  return pointer_lifecycle_load(&pointer_lifecycle_last_errno);
}

const char *lj_callxs_arm64_pointer_lifecycle(const char *value)
{
  int32_t incoming_errno = errno;
  int32_t callno = __atomic_add_fetch(&pointer_lifecycle_calls, 1,
				      __ATOMIC_ACQ_REL);
  int32_t shift = pointer_lifecycle_load(&pointer_lifecycle_shift);
  if (callno == 1)
    pointer_lifecycle_store(&pointer_lifecycle_first_errno,
			    incoming_errno);
  pointer_lifecycle_store(&pointer_lifecycle_last_errno, incoming_errno);
  errno = EDOM;
  return value == NULL ? NULL : value + shift;
}

void lj_callxs_arm64_pointer_negative_configure(void)
{
  pointer_lifecycle_store(&pointer_lifecycle_negative_calls, 0);
}

int32_t lj_callxs_arm64_pointer_negative_count(void)
{
  return pointer_lifecycle_load(&pointer_lifecycle_negative_calls);
}

const void *lj_callxs_arm64_pointer_same_void(const void *value)
{
  (void)__atomic_add_fetch(&pointer_lifecycle_negative_calls, 1,
			   __ATOMIC_ACQ_REL);
  return value;
}

const char *lj_callxs_arm64_pointer_vararg(const char *value, ...)
{
  (void)__atomic_add_fetch(&pointer_lifecycle_negative_calls, 1,
			   __ATOMIC_ACQ_REL);
  return value;
}
