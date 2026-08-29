/* Foreign side of the exact Darwin ARM64 CALLXS lifecycle proof. */

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t (*LJCallXSArm64LifecycleCallback)(int32_t value);

static int32_t lifecycle_calls;
static int32_t lifecycle_callback_calls;
static int32_t lifecycle_callback_errno;
static int32_t lifecycle_first_errno;
static int32_t lifecycle_second_errno;
static int32_t lifecycle_third_errno;
static int32_t lifecycle_last_errno;
static int32_t lifecycle_mismatch = INT32_MIN;
static int32_t lifecycle_callback_first;
static int32_t lifecycle_callback_second;
static LJCallXSArm64LifecycleCallback lifecycle_callback;

static int32_t lifecycle_load(const int32_t *value)
{
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void lifecycle_store(int32_t *target, int32_t value)
{
  __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

void lj_callxs_arm64_lifecycle_configure(int32_t mismatch,
	int32_t callback_first, int32_t callback_second)
{
  lifecycle_store(&lifecycle_calls, 0);
  lifecycle_store(&lifecycle_callback_calls, 0);
  lifecycle_store(&lifecycle_callback_errno, 0);
  lifecycle_store(&lifecycle_first_errno, 0);
  lifecycle_store(&lifecycle_second_errno, 0);
  lifecycle_store(&lifecycle_third_errno, 0);
  lifecycle_store(&lifecycle_last_errno, 0);
  lifecycle_store(&lifecycle_mismatch, mismatch);
  lifecycle_store(&lifecycle_callback_first, callback_first);
  lifecycle_store(&lifecycle_callback_second, callback_second);
}

void lj_callxs_arm64_lifecycle_set_callback(
	LJCallXSArm64LifecycleCallback callback)
{
  lifecycle_callback = callback;
}

int32_t lj_callxs_arm64_lifecycle_count(void)
{
  return lifecycle_load(&lifecycle_calls);
}

int32_t lj_callxs_arm64_lifecycle_callback_count(void)
{
  return lifecycle_load(&lifecycle_callback_calls);
}

int32_t lj_callxs_arm64_lifecycle_callback_errno_value(void)
{
  return lifecycle_load(&lifecycle_callback_errno);
}

int32_t lj_callxs_arm64_lifecycle_first_errno_value(void)
{
  return lifecycle_load(&lifecycle_first_errno);
}

int32_t lj_callxs_arm64_lifecycle_second_errno_value(void)
{
  return lifecycle_load(&lifecycle_second_errno);
}

int32_t lj_callxs_arm64_lifecycle_third_errno_value(void)
{
  return lifecycle_load(&lifecycle_third_errno);
}

int32_t lj_callxs_arm64_lifecycle_last_errno_value(void)
{
  return lifecycle_load(&lifecycle_last_errno);
}

int32_t lj_callxs_arm64_lifecycle(int32_t value)
{
  int32_t incoming_errno = errno;
  int32_t callback_first, callback_second;
  int32_t callno;
  int32_t result = value;
  callno = __atomic_add_fetch(&lifecycle_calls, 1, __ATOMIC_ACQ_REL);
  if (callno == 1)
    lifecycle_store(&lifecycle_first_errno, incoming_errno);
  if (callno == 2)
    lifecycle_store(&lifecycle_second_errno, incoming_errno);
  if (callno == 3)
    lifecycle_store(&lifecycle_third_errno, incoming_errno);
  lifecycle_store(&lifecycle_last_errno, incoming_errno);
  callback_first = lifecycle_load(&lifecycle_callback_first);
  callback_second = lifecycle_load(&lifecycle_callback_second);
  if (callback_first < 0 || callno == callback_first ||
      callno == callback_second) {
    LJCallXSArm64LifecycleCallback callback = lifecycle_callback;
    if (callback != NULL) {
      (void)__atomic_add_fetch(&lifecycle_callback_calls, 1,
			       __ATOMIC_ACQ_REL);
      errno = EAGAIN;
      result = callback(value);
      lifecycle_store(&lifecycle_callback_errno, errno);
    }
  }
  if (value == lifecycle_load(&lifecycle_mismatch))
    result++;
  errno = EDOM;
  return result;
}
