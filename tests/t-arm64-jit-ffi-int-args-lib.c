/* Exact argument and side-effect helpers for Lua INT fixed CALLXS calls. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

enum { INTARG_KINDS = 8, INTARG_SLOTS = 8 };

static int32_t current_kind, current_stack, expected_active, expected_sequence;
static int32_t mismatches;
static int32_t calls[INTARG_KINDS], first_errno[INTARG_KINDS];
static int32_t last_errno[INTARG_KINDS];
static uint64_t captured[INTARG_KINDS][INTARG_SLOTS];
static uint64_t first_captured[INTARG_KINDS];
static uint64_t expected[INTARG_SLOTS];
static int32_t producer_calls, producer_first_errno, producer_last_errno;
static uint32_t producer_captured[INTARG_SLOTS], producer_expected;

static int32_t ld32(const int32_t *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void st32(int32_t *p, int32_t v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static uint64_t ld64(const uint64_t *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void st64(uint64_t *p, uint64_t v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static uint32_t ld32u(const uint32_t *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void st32u(uint32_t *p, uint32_t v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

void lj_ffi_int_configure(int32_t kind, int32_t stack)
{
  unsigned i, j;
  if (kind < 0 || kind >= INTARG_KINDS)
    return;
  st32(&current_kind, kind);
  st32(&current_stack, stack != 0);
  st32(&mismatches, 0);
  st32(&producer_calls, 0);
  st32(&producer_first_errno, 0);
  st32(&producer_last_errno, 0);
  for (j = 0; j < INTARG_SLOTS; j++)
    st32u(&producer_captured[j], 0);
  for (i = 0; i < INTARG_KINDS; i++) {
    st32(&calls[i], 0);
    st32(&first_errno[i], 0);
    st32(&last_errno[i], 0);
    st64(&first_captured[i], 0);
    for (j = 0; j < INTARG_SLOTS; j++)
      st64(&captured[i][j], 0);
  }
}

void lj_ffi_int_expect_enable(int32_t active)
{
  st32(&expected_active, active != 0);
}
void lj_ffi_int_expect_sequence(int32_t active)
{
  st32(&expected_sequence, active != 0);
}
int32_t lj_ffi_int_mismatch_count(void) { return ld32(&mismatches); }
int32_t lj_ffi_int_count(int32_t kind)
{
  return kind < 0 || kind >= INTARG_KINDS ? -1 : ld32(&calls[kind]);
}
int32_t lj_ffi_int_first_errno(int32_t kind)
{
  return kind < 0 || kind >= INTARG_KINDS ? -1 : ld32(&first_errno[kind]);
}
int32_t lj_ffi_int_last_errno(int32_t kind)
{
  return kind < 0 || kind >= INTARG_KINDS ? -1 : ld32(&last_errno[kind]);
}
int32_t lj_ffi_int_producer_count(void) { return ld32(&producer_calls); }
int32_t lj_ffi_int_producer_first_errno(void)
{
  return ld32(&producer_first_errno);
}
int32_t lj_ffi_int_producer_last_errno(void)
{
  return ld32(&producer_last_errno);
}

void lj_ffi_int_save_expected(void)
{
  int32_t k = ld32(&current_kind);
  unsigned i;
  st64(&expected[0], ld32(&expected_sequence) ?
	ld64(&first_captured[k]) : ld64(&captured[k][0]));
  for (i = 1; i < INTARG_SLOTS; i++)
    st64(&expected[i], ld64(&captured[k][i]));
  st32u(&producer_expected, ld32u(&producer_captured[0]));
}

static uint64_t norm_i8(int8_t v) { return (uint8_t)v; }
static uint64_t norm_u8(uint8_t v) { return v; }
static uint64_t norm_i16(int16_t v) { return (uint16_t)v; }
static uint64_t norm_u16(uint16_t v) { return v; }
static uint64_t norm_i32(int32_t v) { return (uint32_t)v; }
static uint64_t norm_u32(uint32_t v) { return v; }
static uint64_t norm_i64(int64_t v) { return (uint64_t)v; }
static uint64_t norm_u64(uint64_t v) { return v; }

static double record(unsigned kind, uint64_t bits, const int32_t *prefix,
                     uint8_t p8, int16_t p9, uint32_t p10, int stack)
{
  static const int32_t expected_prefix[8] = {101, 102, 103, 104,
                                             105, 106, 107, 108};
  int32_t index, incoming = errno;
  unsigned i;
  if (kind >= INTARG_KINDS || kind != (unsigned)ld32(&current_kind) ||
      stack != ld32(&current_stack)) {
    (void)__atomic_add_fetch(&mismatches, 1, __ATOMIC_ACQ_REL);
    errno = EDOM;
    return 1.25;
  }
  if (stack) {
    for (i = 0; i < 8; i++)
      if (prefix[i] != expected_prefix[i])
        (void)__atomic_add_fetch(&mismatches, 1, __ATOMIC_ACQ_REL);
    if (p8 != 31 || p9 != -505 || p10 != UINT32_C(1985229328))
      (void)__atomic_add_fetch(&mismatches, 1, __ATOMIC_ACQ_REL);
  }
  index = __atomic_fetch_add(&calls[kind], 1, __ATOMIC_ACQ_REL);
  if (index == 0) {
    st32(&first_errno[kind], incoming);
    st64(&first_captured[kind], bits);
  }
  st32(&last_errno[kind], incoming);
  st64(&captured[kind][(uint32_t)index % INTARG_SLOTS], bits);
  if (ld32(&expected_active)) {
    uint64_t want = ld64(&expected[0]);
    if (ld32(&expected_sequence)) want += (uint32_t)index;
    if (bits != want) {
      (void)__atomic_add_fetch(&mismatches, 1, __ATOMIC_ACQ_REL);
    }
  }
  errno = EDOM;
  return 1.25;
}

#define UNARY(name, kind, type, norm)                                          \
  double lj_ffi_int_##name(type v)                                             \
  {                                                                            \
    return record(kind, norm(v), NULL, 0, 0, 0, 0);                            \
  }
UNARY(i8, 0, int8_t, norm_i8)
UNARY(u8, 1, uint8_t, norm_u8)
UNARY(i16, 2, int16_t, norm_i16)
UNARY(u16, 3, uint16_t, norm_u16)
UNARY(i32, 4, int32_t, norm_i32)
UNARY(u32, 5, uint32_t, norm_u32)
UNARY(i64, 6, int64_t, norm_i64)
UNARY(u64, 7, uint64_t, norm_u64)

#define STACK_ARGS                                                             \
  int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5,      \
      int32_t a6, int32_t a7, uint8_t a8, int16_t a9, uint32_t a10
#define STACK_PREFIX {a0, a1, a2, a3, a4, a5, a6, a7}
#define STACK(name, kind, type, norm)                                          \
  double lj_ffi_int_stack_##name(STACK_ARGS, type v)                           \
  {                                                                            \
    const int32_t p[8] = STACK_PREFIX;                                         \
    return record(kind, norm(v), p, a8, a9, a10, 1);                           \
  }
STACK(i8, 0, int8_t, norm_i8)
STACK(u8, 1, uint8_t, norm_u8)
STACK(i16, 2, int16_t, norm_i16)
STACK(u16, 3, uint16_t, norm_u16)
STACK(i32, 4, int32_t, norm_i32)
STACK(u32, 5, uint32_t, norm_u32)
STACK(i64, 6, int64_t, norm_i64)
STACK(u64, 7, uint64_t, norm_u64)

int32_t lj_ffi_int_producer(int32_t value)
{
  int32_t index, incoming = errno;
  index = __atomic_fetch_add(&producer_calls, 1, __ATOMIC_ACQ_REL);
  if (index == 0)
    st32(&producer_first_errno, incoming);
  st32(&producer_last_errno, incoming);
  st32u(&producer_captured[(uint32_t)index % INTARG_SLOTS], (uint32_t)value);
  if (ld32(&expected_active) && (uint32_t)value != ld32u(&producer_expected))
    (void)__atomic_add_fetch(&mismatches, 1, __ATOMIC_ACQ_REL);
  errno = ERANGE;
  return value;
}
