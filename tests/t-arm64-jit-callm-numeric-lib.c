/* C helpers for numeric producer -> fixed integer CALLM lifecycle tests. */

#include <errno.h>
#include <stdint.h>
#include <string.h>

enum { CALLM_NUM_KINDS = 8, CALLM_NUM_FAMILIES = 3,
       CALLM_NUM_CAPTURES = 8 };

static int32_t current_kind, current_family, current_stack;
static int32_t expected_enabled, mismatch_count;
static int32_t producer_count[CALLM_NUM_FAMILIES];
static int32_t consumer_count[CALLM_NUM_KINDS];
static int32_t producer_first_errno[CALLM_NUM_FAMILIES];
static int32_t producer_last_errno[CALLM_NUM_FAMILIES];
static int32_t consumer_first_errno[CALLM_NUM_KINDS];
static int32_t consumer_last_errno[CALLM_NUM_KINDS];
static uint64_t producer_capture[CALLM_NUM_FAMILIES][CALLM_NUM_CAPTURES];
static uint64_t consumer_capture[CALLM_NUM_KINDS][CALLM_NUM_CAPTURES];
static uint64_t producer_expected[CALLM_NUM_FAMILIES];
static uint64_t consumer_expected[CALLM_NUM_KINDS];

static int32_t load_i32(const int32_t *p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static void store_i32(int32_t *p, int32_t v)
{ __atomic_store_n(p, v, __ATOMIC_RELEASE); }

static uint64_t load_u64(const uint64_t *p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static void store_u64(uint64_t *p, uint64_t v)
{ __atomic_store_n(p, v, __ATOMIC_RELEASE); }

void lj_callm_numeric_configure(int32_t family, int32_t kind, int32_t stack)
{
  unsigned i, j;
  if (family < 0 || family >= CALLM_NUM_FAMILIES ||
      kind < 0 || kind >= CALLM_NUM_KINDS) return;
  store_i32(&current_family, family);
  store_i32(&current_kind, kind);
  store_i32(&current_stack, stack != 0);
  store_i32(&mismatch_count, 0);
  for (i = 0; i < CALLM_NUM_FAMILIES; i++) {
    store_i32(&producer_count[i], 0);
    store_i32(&producer_first_errno[i], 0);
    store_i32(&producer_last_errno[i], 0);
    for (j = 0; j < CALLM_NUM_CAPTURES; j++)
      store_u64(&producer_capture[i][j], 0);
  }
  for (i = 0; i < CALLM_NUM_KINDS; i++) {
    store_i32(&consumer_count[i], 0);
    store_i32(&consumer_first_errno[i], 0);
    store_i32(&consumer_last_errno[i], 0);
    for (j = 0; j < CALLM_NUM_CAPTURES; j++)
      store_u64(&consumer_capture[i][j], 0);
  }
}

void lj_callm_numeric_expected_enable(int32_t enabled)
{ store_i32(&expected_enabled, enabled != 0); }

int32_t lj_callm_numeric_mismatch_count(void)
{ return load_i32(&mismatch_count); }

int32_t lj_callm_numeric_producer_count(int32_t family)
{ return family < 0 || family >= CALLM_NUM_FAMILIES ? -1 : load_i32(&producer_count[family]); }

int32_t lj_callm_numeric_consumer_count(int32_t kind)
{ return kind < 0 || kind >= CALLM_NUM_KINDS ? -1 : load_i32(&consumer_count[kind]); }

int32_t lj_callm_numeric_producer_first_errno(int32_t family)
{ return family < 0 || family >= CALLM_NUM_FAMILIES ? -1 : load_i32(&producer_first_errno[family]); }

int32_t lj_callm_numeric_producer_last_errno(int32_t family)
{ return family < 0 || family >= CALLM_NUM_FAMILIES ? -1 : load_i32(&producer_last_errno[family]); }

int32_t lj_callm_numeric_consumer_first_errno(int32_t kind)
{ return kind < 0 || kind >= CALLM_NUM_KINDS ? -1 : load_i32(&consumer_first_errno[kind]); }

int32_t lj_callm_numeric_consumer_last_errno(int32_t kind)
{ return kind < 0 || kind >= CALLM_NUM_KINDS ? -1 : load_i32(&consumer_last_errno[kind]); }

void lj_callm_numeric_save_expected(void)
{
  int32_t family = load_i32(&current_family);
  int32_t kind = load_i32(&current_kind);
  store_u64(&producer_expected[family], load_u64(&producer_capture[family][0]));
  store_u64(&consumer_expected[kind], load_u64(&consumer_capture[kind][0]));
}

int32_t lj_callm_numeric_match_expected(void)
{
  int32_t family = load_i32(&current_family);
  int32_t kind = load_i32(&current_kind);
  unsigned i;
  if (load_i32(&producer_count[family]) < CALLM_NUM_CAPTURES ||
      load_i32(&consumer_count[kind]) < CALLM_NUM_CAPTURES) return 0;
  for (i = 0; i < CALLM_NUM_CAPTURES; i++) {
    if (load_u64(&producer_capture[family][i]) !=
        load_u64(&producer_expected[family])) return 0;
    if (load_u64(&consumer_capture[kind][i]) !=
        load_u64(&consumer_expected[kind])) return 0;
  }
  return 1;
}

static void record_producer(unsigned family, uint64_t bits)
{
  int32_t index, incoming = errno;
  if (family >= CALLM_NUM_FAMILIES ||
      family != (unsigned)load_i32(&current_family)) {
    (void)__atomic_add_fetch(&mismatch_count, 1, __ATOMIC_ACQ_REL);
    errno = ERANGE;
    return;
  }
  index = __atomic_fetch_add(&producer_count[family], 1, __ATOMIC_ACQ_REL);
  if (index == 0) store_i32(&producer_first_errno[family], incoming);
  store_i32(&producer_last_errno[family], incoming);
  store_u64(&producer_capture[family][(uint32_t)index % CALLM_NUM_CAPTURES], bits);
  if (load_i32(&expected_enabled) &&
      bits != load_u64(&producer_expected[family]))
    (void)__atomic_add_fetch(&mismatch_count, 1, __ATOMIC_ACQ_REL);
  errno = ERANGE;
}

double lj_callm_numeric_producer_double(double value)
{
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  record_producer(0, bits);
  return value;
}

float lj_callm_numeric_producer_float(float value)
{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  record_producer(1, bits);
  return value;
}

uint32_t lj_callm_numeric_producer_u32(uint32_t value)
{
  record_producer(2, value);
  return value;
}

static double record_consumer(unsigned kind, uint64_t bits,
                              const int32_t *prefix, uint8_t s8,
                              int16_t s16, uint32_t u32, int stack)
{
  int32_t index, incoming = errno;
  unsigned i;
  if (kind >= CALLM_NUM_KINDS || kind != (unsigned)load_i32(&current_kind)) {
    (void)__atomic_add_fetch(&mismatch_count, 1, __ATOMIC_ACQ_REL);
    errno = EDOM;
    return 1.25;
  }
  if (load_i32(&current_stack) != stack)
    (void)__atomic_add_fetch(&mismatch_count, 1, __ATOMIC_ACQ_REL);
  if (stack) {
    for (i = 0; i < 8; i++)
      if (prefix[i] != (int32_t)(101u+i))
        (void)__atomic_add_fetch(&mismatch_count, 1, __ATOMIC_ACQ_REL);
    if (s8 != 31u || s16 != -505 || u32 != UINT32_C(1985229328))
      (void)__atomic_add_fetch(&mismatch_count, 1, __ATOMIC_ACQ_REL);
  }
  index = __atomic_fetch_add(&consumer_count[kind], 1, __ATOMIC_ACQ_REL);
  if (index == 0) store_i32(&consumer_first_errno[kind], incoming);
  store_i32(&consumer_last_errno[kind], incoming);
  store_u64(&consumer_capture[kind][(uint32_t)index % CALLM_NUM_CAPTURES], bits);
  if (load_i32(&expected_enabled) &&
      bits != load_u64(&consumer_expected[kind]))
    (void)__atomic_add_fetch(&mismatch_count, 1, __ATOMIC_ACQ_REL);
  errno = EDOM;
  return 1.25;
}

double lj_callm_numeric_i8(int8_t v) { return record_consumer(0, (uint8_t)v, NULL, 0, 0, 0, 0); }
double lj_callm_numeric_u8(uint8_t v) { return record_consumer(1, v, NULL, 0, 0, 0, 0); }
double lj_callm_numeric_i16(int16_t v) { return record_consumer(2, (uint16_t)v, NULL, 0, 0, 0, 0); }
double lj_callm_numeric_u16(uint16_t v) { return record_consumer(3, v, NULL, 0, 0, 0, 0); }
double lj_callm_numeric_i32(int32_t v) { return record_consumer(4, (uint32_t)v, NULL, 0, 0, 0, 0); }
double lj_callm_numeric_u32(uint32_t v) { return record_consumer(5, v, NULL, 0, 0, 0, 0); }
double lj_callm_numeric_i64(int64_t v) { return record_consumer(6, (uint64_t)v, NULL, 0, 0, 0, 0); }
double lj_callm_numeric_u64(uint64_t v) { return record_consumer(7, v, NULL, 0, 0, 0, 0); }

#define STACK_ARGS int32_t p0, int32_t p1, int32_t p2, int32_t p3, \
  int32_t p4, int32_t p5, int32_t p6, int32_t p7, uint8_t s8, \
  int16_t s16, uint32_t u32
#define STACK_PREFIX { p0,p1,p2,p3,p4,p5,p6,p7 }
#define DEFINE_STACK(suffix, kind, type, expr) \
double lj_callm_numeric_stack_##suffix(STACK_ARGS, type v) { \
  const int32_t p[8] = STACK_PREFIX; \
  return record_consumer(kind, (uint64_t)(expr), p, s8, s16, u32, 1); \
}

DEFINE_STACK(i8, 0, int8_t, (uint8_t)v)
DEFINE_STACK(u8, 1, uint8_t, v)
DEFINE_STACK(i16, 2, int16_t, (uint16_t)v)
DEFINE_STACK(u16, 3, uint16_t, v)
DEFINE_STACK(i32, 4, int32_t, (uint32_t)v)
DEFINE_STACK(u32, 5, uint32_t, v)
DEFINE_STACK(i64, 6, int64_t, (uint64_t)v)
DEFINE_STACK(u64, 7, uint64_t, v)

#undef DEFINE_STACK
#undef STACK_PREFIX
#undef STACK_ARGS
