/* Foreign helpers for ordinary Lua-number CALLXS integer arguments. */

#include <errno.h>
#include <stdint.h>

enum { NUMERIC_KINDS = 9, NUMERIC_CAPTURES = 8 };

static int32_t numeric_kind;
static int32_t numeric_expected_active;
static int32_t numeric_mismatches;
static int32_t numeric_calls[NUMERIC_KINDS];
static int32_t numeric_first_errno[NUMERIC_KINDS];
static int32_t numeric_last_errno[NUMERIC_KINDS];
static uint64_t numeric_capture[NUMERIC_KINDS][NUMERIC_CAPTURES];
static uint64_t numeric_expected[NUMERIC_CAPTURES];

static int32_t load_i32(const int32_t *p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static void store_i32(int32_t *p, int32_t v)
{ __atomic_store_n(p, v, __ATOMIC_RELEASE); }

static uint64_t load_u64(const uint64_t *p)
{ return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

static void store_u64(uint64_t *p, uint64_t v)
{ __atomic_store_n(p, v, __ATOMIC_RELEASE); }

void lj_ffi_numeric_configure(int32_t kind)
{
  unsigned i, j;
  if (kind < 0 || kind >= NUMERIC_KINDS) return;
  store_i32(&numeric_kind, kind);
  store_i32(&numeric_mismatches, 0);
  for (i = 0; i < NUMERIC_KINDS; i++) {
    store_i32(&numeric_calls[i], 0);
    store_i32(&numeric_first_errno[i], 0);
    store_i32(&numeric_last_errno[i], 0);
    for (j = 0; j < NUMERIC_CAPTURES; j++)
      store_u64(&numeric_capture[i][j], 0);
  }
}

void lj_ffi_numeric_expect_enable(int32_t enabled)
{ store_i32(&numeric_expected_active, enabled != 0); }

int32_t lj_ffi_numeric_mismatch_count(void)
{ return load_i32(&numeric_mismatches); }

int32_t lj_ffi_numeric_count(int32_t kind)
{ return kind < 0 || kind >= NUMERIC_KINDS ? -1 : load_i32(&numeric_calls[kind]); }

int32_t lj_ffi_numeric_first_errno(int32_t kind)
{ return kind < 0 || kind >= NUMERIC_KINDS ? -1 : load_i32(&numeric_first_errno[kind]); }

int32_t lj_ffi_numeric_last_errno(int32_t kind)
{ return kind < 0 || kind >= NUMERIC_KINDS ? -1 : load_i32(&numeric_last_errno[kind]); }

uint32_t lj_ffi_numeric_capture_low(uint32_t slot)
{ return slot < NUMERIC_CAPTURES ? (uint32_t)load_u64(&numeric_capture[load_i32(&numeric_kind)][slot]) : 0; }

uint32_t lj_ffi_numeric_capture_high(uint32_t slot)
{ return slot < NUMERIC_CAPTURES ? (uint32_t)(load_u64(&numeric_capture[load_i32(&numeric_kind)][slot]) >> 32) : 0; }

uint32_t lj_ffi_numeric_expected_low(uint32_t slot)
{ return slot < NUMERIC_CAPTURES ? (uint32_t)load_u64(&numeric_expected[slot]) : 0; }

uint32_t lj_ffi_numeric_expected_high(uint32_t slot)
{ return slot < NUMERIC_CAPTURES ? (uint32_t)(load_u64(&numeric_expected[slot]) >> 32) : 0; }

void lj_ffi_numeric_save_expected(void)
{
  unsigned i;
  int32_t kind = load_i32(&numeric_kind);
  if (kind < NUMERIC_KINDS-1) {
    store_u64(&numeric_expected[0], load_u64(&numeric_capture[kind][0]));
  } else {
    for (i = 0; i < NUMERIC_CAPTURES; i++)
      store_u64(&numeric_expected[i], load_u64(&numeric_capture[kind][i]));
  }
}

int32_t lj_ffi_numeric_match_expected(void)
{
  unsigned i;
  int32_t kind = load_i32(&numeric_kind);
  if (load_i32(&numeric_calls[kind]) < NUMERIC_CAPTURES) return 0;
  if (kind < NUMERIC_KINDS-1) {
    for (i = 0; i < NUMERIC_CAPTURES; i++)
      if (load_u64(&numeric_capture[kind][i]) != load_u64(&numeric_expected[0]))
        return 0;
  } else {
    for (i = 0; i < NUMERIC_CAPTURES; i++)
      if (load_u64(&numeric_capture[kind][i]) != load_u64(&numeric_expected[i]))
        return 0;
  }
  return 1;
}

static double record_one(unsigned kind, uint64_t bits)
{
  int32_t index;
  int32_t incoming = errno;
  if (kind >= NUMERIC_KINDS || (int32_t)kind != load_i32(&numeric_kind))
    return -1.0;
  index = __atomic_fetch_add(&numeric_calls[kind], 1, __ATOMIC_ACQ_REL);
  if (index == 0)
    store_i32(&numeric_first_errno[kind], incoming);
  store_i32(&numeric_last_errno[kind], incoming);
  store_u64(&numeric_capture[kind][(uint32_t)index % NUMERIC_CAPTURES], bits);
  if (load_i32(&numeric_expected_active) &&
      bits != load_u64(&numeric_expected[0]))
    (void)__atomic_add_fetch(&numeric_mismatches, 1, __ATOMIC_ACQ_REL);
  errno = EDOM;
  return 1.25;
}

double lj_ffi_numeric_i8(int8_t value)
{ return record_one(0, (uint8_t)value); }
double lj_ffi_numeric_u8(uint8_t value)
{ return record_one(1, value); }
double lj_ffi_numeric_i16(int16_t value)
{ return record_one(2, (uint16_t)value); }
double lj_ffi_numeric_u16(uint16_t value)
{ return record_one(3, value); }
double lj_ffi_numeric_i32(int32_t value)
{ return record_one(4, (uint32_t)value); }
double lj_ffi_numeric_u32(uint32_t value)
{ return record_one(5, value); }
double lj_ffi_numeric_i64(int64_t value)
{ return record_one(6, (uint64_t)value); }
double lj_ffi_numeric_u64(uint64_t value)
{ return record_one(7, value); }

double lj_ffi_numeric_stack(int32_t p0, int32_t p1, int32_t p2,
                            int32_t p3, int32_t p4, int32_t p5,
                            int32_t p6, int32_t p7, int8_t s8, uint8_t u8,
                            int16_t s16, uint16_t u16, int32_t s32,
                            uint32_t u32, int64_t s64, uint64_t u64)
{
  const int32_t prefix[8] = { p0,p1,p2,p3,p4,p5,p6,p7 };
  uint64_t values[8];
  int32_t kind = 8;
  int32_t incoming = errno;
  int32_t index;
  unsigned i;
  for (i = 0; i < 8; i++)
    if (prefix[i] != (int32_t)(101u+i)) return -1.0;
  values[0] = (uint8_t)s8;
  values[1] = u8;
  values[2] = (uint16_t)s16;
  values[3] = u16;
  values[4] = (uint32_t)s32;
  values[5] = u32;
  values[6] = (uint64_t)s64;
  values[7] = u64;
  if (load_i32(&numeric_kind) != kind) return -1.0;
  index = __atomic_fetch_add(&numeric_calls[kind], 1, __ATOMIC_ACQ_REL);
  if (index == 0) store_i32(&numeric_first_errno[kind], incoming);
  store_i32(&numeric_last_errno[kind], incoming);
  for (i = 0; i < 8; i++)
    store_u64(&numeric_capture[kind][i], values[i]);
  if (load_i32(&numeric_expected_active)) {
    for (i = 0; i < 8; i++)
      if (values[i] != load_u64(&numeric_expected[i]))
        (void)__atomic_add_fetch(&numeric_mismatches, 1, __ATOMIC_ACQ_REL);
  }
  errno = EDOM;
  return 1.25;
}
