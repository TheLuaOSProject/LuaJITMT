/* Foreign side of the exact Darwin ARM64 double(double) CALLXS proof. */

#include <errno.h>
#include <stdint.h>
#include <string.h>

static int32_t double_lifecycle_calls;
static int32_t double_lifecycle_first_errno;
static int32_t double_lifecycle_last_errno;
static int32_t double_lifecycle_mismatch_call;
static uint64_t double_lifecycle_last_input;
static uint64_t double_lifecycle_last_result;

static int32_t double_lifecycle_load_i32(const int32_t *value)
{
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void double_lifecycle_store_i32(int32_t *target, int32_t value)
{
  __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

static uint64_t double_lifecycle_bits(double value)
{
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static double double_lifecycle_value(uint64_t bits)
{
  double value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

void lj_callxs_arm64_double_lifecycle_configure(int32_t mismatch_call)
{
  double_lifecycle_store_i32(&double_lifecycle_calls, 0);
  double_lifecycle_store_i32(&double_lifecycle_first_errno, 0);
  double_lifecycle_store_i32(&double_lifecycle_last_errno, 0);
  double_lifecycle_store_i32(&double_lifecycle_mismatch_call,
			     mismatch_call);
  __atomic_store_n(&double_lifecycle_last_input, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&double_lifecycle_last_result, 0, __ATOMIC_RELEASE);
}

int32_t lj_callxs_arm64_double_lifecycle_count(void)
{
  return double_lifecycle_load_i32(&double_lifecycle_calls);
}

int32_t lj_callxs_arm64_double_lifecycle_first_errno_value(void)
{
  return double_lifecycle_load_i32(&double_lifecycle_first_errno);
}

int32_t lj_callxs_arm64_double_lifecycle_last_errno_value(void)
{
  return double_lifecycle_load_i32(&double_lifecycle_last_errno);
}

double lj_callxs_arm64_double_lifecycle_last_input_value(void)
{
  uint64_t bits = __atomic_load_n(&double_lifecycle_last_input,
				  __ATOMIC_ACQUIRE);
  return double_lifecycle_value(bits);
}

double lj_callxs_arm64_double_lifecycle_last_result_value(void)
{
  uint64_t bits = __atomic_load_n(&double_lifecycle_last_result,
				  __ATOMIC_ACQUIRE);
  return double_lifecycle_value(bits);
}

double lj_callxs_arm64_double_lifecycle(double value)
{
  int32_t incoming_errno = errno;
  int32_t callno = __atomic_add_fetch(&double_lifecycle_calls, 1,
				      __ATOMIC_ACQ_REL);
  double result = value;
  if (callno == 1)
    double_lifecycle_store_i32(&double_lifecycle_first_errno,
			       incoming_errno);
  double_lifecycle_store_i32(&double_lifecycle_last_errno, incoming_errno);
  if (callno == double_lifecycle_load_i32(
	&double_lifecycle_mismatch_call))
    result += 0.25;
  __atomic_store_n(&double_lifecycle_last_input,
		   double_lifecycle_bits(value), __ATOMIC_RELEASE);
  __atomic_store_n(&double_lifecycle_last_result,
		   double_lifecycle_bits(result), __ATOMIC_RELEASE);
  errno = EDOM;
  return result;
}
