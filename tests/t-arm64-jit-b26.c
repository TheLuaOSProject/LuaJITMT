/*
** Pure boundary contract for address-safe ARM64 unconditional branches.
*/

#include <assert.h>
#include <stdint.h>

#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_target.h"
#include "lj_asm.h"

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && LJ_HASJIT

#define B26_MASK	UINT32_C(0x03ffffff)
#define B26_OPCODE_MASK	UINT32_C(0xfc000000)
#define B26_MIN_BYTES	UINT32_C(0x08000000)
#define B26_MAX_BYTES	UINT32_C(0x07fffffc)

static int decode_b26(uintptr_t source, MCode ins, uintptr_t *targetp)
{
  uint32_t field = (uint32_t)ins & B26_MASK;
  uintptr_t distance;
  if (source == 0 || (source & 3u) != 0 || targetp == NULL ||
	((uint32_t)ins & B26_OPCODE_MASK) != A64I_B)
    return 0;
  if ((field & UINT32_C(0x02000000)) != 0) {
    distance = (uintptr_t)(UINT32_C(0x04000000)-field) << 2;
    if (source < distance)
      return 0;
    *targetp = source - distance;
  } else {
    distance = (uintptr_t)field << 2;
    if (source > UINTPTR_MAX-distance)
      return 0;
    *targetp = source + distance;
  }
  return *targetp != 0 && (*targetp & 3u) == 0;
}

static void expect_ok(uintptr_t source, uintptr_t target, uint32_t field)
{
  MCode ins = UINT32_C(0xdeadbeef);
  uintptr_t decoded = 0;
  unsigned int bit;
  assert(lj_asm_arm64_b26_encode(source, target, &ins));
  assert((uint32_t)ins == (A64I_B | field));
  assert(decode_b26(source, ins, &decoded));
  assert(decoded == target);

  /* Every single-bit instruction mutation must either cease to be B26 or
  ** decode to a different address. This covers all opcode and immediate
  ** bits, including the signed-immediate bit. */
  for (bit = 0; bit < 32; bit++) {
    MCode mutant = (MCode)((uint32_t)ins ^ (UINT32_C(1) << bit));
    decoded = target;
    assert(!decode_b26(source, mutant, &decoded) || decoded != target);
  }
}

static void expect_reject(uintptr_t source, uintptr_t target)
{
  MCode ins = UINT32_C(0xdeadbeef);
  assert(!lj_asm_arm64_b26_encode(source, target, &ins));
  assert((uint32_t)ins == UINT32_C(0xdeadbeef));
}

static void test_exact_boundaries(void)
{
  const uintptr_t source = UINT32_C(0x40000000);
  uintptr_t distance;

  expect_ok(source, source-B26_MIN_BYTES, UINT32_C(0x02000000));
  expect_ok(source, source-B26_MIN_BYTES+4u, UINT32_C(0x02000001));
  expect_ok(source, source-4u, UINT32_C(0x03ffffff));
  expect_ok(source, source, 0);
  expect_ok(source, source+4u, 1);
  expect_ok(source, source+B26_MAX_BYTES-4u, UINT32_C(0x01fffffe));
  expect_ok(source, source+B26_MAX_BYTES, UINT32_C(0x01ffffff));
  expect_reject(source, source-B26_MIN_BYTES-4u);
  expect_reject(source, source+B26_MAX_BYTES+4u);

  /* Exhaust every aligned displacement in a sixteen-byte window on both
  ** sides of each signed limit. */
  for (distance = B26_MIN_BYTES-16u;
       distance <= B26_MIN_BYTES+16u; distance += 4u) {
    uintptr_t target = source-distance;
    if (distance <= B26_MIN_BYTES) {
      uint32_t words = (uint32_t)(distance >> 2);
      expect_ok(source, target, (0u-words) & B26_MASK);
    } else {
      expect_reject(source, target);
    }
  }
  for (distance = B26_MAX_BYTES-16u;
       distance <= B26_MAX_BYTES+16u; distance += 4u) {
    uintptr_t target = source+distance;
    if (distance <= B26_MAX_BYTES)
      expect_ok(source, target, (uint32_t)(distance >> 2));
    else
      expect_reject(source, target);
  }
}

static void test_invalid_addresses_and_wrap_edges(void)
{
  const uintptr_t source = UINT32_C(0x40000000);
  const uintptr_t highest = UINTPTR_MAX & ~(uintptr_t)3u;
  unsigned int lowbits;

  expect_reject(0, source);
  expect_reject(source, 0);
  assert(!lj_asm_arm64_b26_encode(source, source, NULL));
  for (lowbits = 1; lowbits <= 3; lowbits++) {
    expect_reject(source+lowbits, source);
    expect_reject(source, source+lowbits);
  }

  expect_ok(4u, 4u+B26_MAX_BYTES, UINT32_C(0x01ffffff));
  expect_ok(highest, highest-B26_MIN_BYTES, UINT32_C(0x02000000));
  expect_ok(highest, highest-4u, UINT32_C(0x03ffffff));
  expect_reject(4u, highest);
  expect_reject(highest, 4u);
}

int main(void)
{
  test_exact_boundaries();
  test_invalid_addresses_and_wrap_edges();
  return 0;
}

#else

int main(void)
{
  return 77;
}

#endif
