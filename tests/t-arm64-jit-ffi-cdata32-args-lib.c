/* Exact C bit-pattern, compact-stack and errno oracle for CData32 arguments. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

enum { CDATA32_KINDS = 2, CDATA32_SLOTS = 8 };

static int32_t selected_kind, selected_stack, expected_active, mismatch_count;
static int32_t calls[CDATA32_KINDS], first_errno[CDATA32_KINDS];
static int32_t last_errno[CDATA32_KINDS];
static uint32_t captured[CDATA32_KINDS][CDATA32_SLOTS];
static uint32_t expected_bits;

static int32_t load32(const int32_t *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store32(int32_t *p, int32_t v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static uint32_t loadu32(const uint32_t *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void storeu32(uint32_t *p, uint32_t v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

void lj_ffi_cdata32_configure(int32_t kind, int32_t stack)
{
  unsigned i, j;
  if (kind < 0 || kind >= CDATA32_KINDS) return;
  store32(&selected_kind, kind);
  store32(&selected_stack, stack != 0);
  store32(&mismatch_count, 0);
  storeu32(&expected_bits, 0);
  for (i = 0; i < CDATA32_KINDS; i++) {
    store32(&calls[i], 0);
    store32(&first_errno[i], 0);
    store32(&last_errno[i], 0);
    for (j = 0; j < CDATA32_SLOTS; j++) storeu32(&captured[i][j], 0);
  }
}

void lj_ffi_cdata32_expect_enable(int32_t active)
{
  store32(&expected_active, active != 0);
}

void lj_ffi_cdata32_save_expected(int32_t kind, uint32_t bits)
{
  if (kind >= 0 && kind < CDATA32_KINDS) storeu32(&expected_bits, bits);
}

int32_t lj_ffi_cdata32_mismatch_count(void) { return load32(&mismatch_count); }
int32_t lj_ffi_cdata32_count(int32_t kind)
{
  return kind < 0 || kind >= CDATA32_KINDS ? -1 : load32(&calls[kind]);
}
int32_t lj_ffi_cdata32_first_errno(int32_t kind)
{
  return kind < 0 || kind >= CDATA32_KINDS ? -1 : load32(&first_errno[kind]);
}
int32_t lj_ffi_cdata32_last_errno(int32_t kind)
{
  return kind < 0 || kind >= CDATA32_KINDS ? -1 : load32(&last_errno[kind]);
}
uint32_t lj_ffi_cdata32_captured(int32_t kind, int32_t slot)
{
  if (kind < 0 || kind >= CDATA32_KINDS || slot < 0 || slot >= CDATA32_SLOTS)
    return 0;
  return loadu32(&captured[kind][slot]);
}

static double record(int32_t kind, uint32_t bits, const int32_t *prefix,
                     uint8_t p8, int16_t p9, uint32_t p10, int stack)
{
  static const int32_t wanted_prefix[8] = {101,102,103,104,105,106,107,108};
  int32_t incoming = errno;
  int32_t index;
  unsigned i;
  if (kind != load32(&selected_kind) || stack != load32(&selected_stack))
    (void)__atomic_add_fetch(&mismatch_count, 1, __ATOMIC_ACQ_REL);
  if (stack) {
    for (i = 0; i < 8; i++)
      if (prefix[i] != wanted_prefix[i])
        (void)__atomic_add_fetch(&mismatch_count, 1, __ATOMIC_ACQ_REL);
    if (p8 != 31 || p9 != -505 || p10 != UINT32_C(1985229328))
      (void)__atomic_add_fetch(&mismatch_count, 1, __ATOMIC_ACQ_REL);
  }
  index = __atomic_fetch_add(&calls[kind], 1, __ATOMIC_ACQ_REL);
  if (index == 0) store32(&first_errno[kind], incoming);
  store32(&last_errno[kind], incoming);
  storeu32(&captured[kind][(uint32_t)index % CDATA32_SLOTS], bits);
  if (load32(&expected_active) && bits != loadu32(&expected_bits))
    (void)__atomic_add_fetch(&mismatch_count, 1, __ATOMIC_ACQ_REL);
  errno = EDOM;
  return 1.25;
}

double lj_ffi_cdata32_i32(int32_t v)
{
  return record(0, (uint32_t)v, NULL, 0, 0, 0, 0);
}
double lj_ffi_cdata32_u32(uint32_t v)
{
  return record(1, v, NULL, 0, 0, 0, 0);
}

#define STACK_DECL \
  int32_t a0,int32_t a1,int32_t a2,int32_t a3,int32_t a4,int32_t a5, \
  int32_t a6,int32_t a7,uint8_t a8,int16_t a9,uint32_t a10
#define PREFIX_VALUES {a0,a1,a2,a3,a4,a5,a6,a7}
#define STACK_HELPER(name, kind, type) \
  double lj_ffi_cdata32_stack_##name(STACK_DECL, type v) { \
    const int32_t p[8] = PREFIX_VALUES; \
    return record(kind, (uint32_t)v, p, a8, a9, a10, 1); \
  }
STACK_HELPER(i32, 0, int32_t)
STACK_HELPER(u32, 1, uint32_t)
