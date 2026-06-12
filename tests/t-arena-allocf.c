/*
** Focused test for the arena lua_Alloc shim.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "lj_arch.h"
#include "lj_arena.h"
#include "lj_prng.h"

static void fill_seq(uint8_t *p, size_t n, uint8_t base)
{
  size_t i;
  for (i = 0; i < n; i++)
    p[i] = (uint8_t)(base + i);
}

static void check_seq(const uint8_t *p, size_t n, uint8_t base)
{
  size_t i;
  for (i = 0; i < n; i++)
    assert(p[i] == (uint8_t)(base + i));
}

int main(void)
{
  PRNGState rs;
  uint32_t round;

  lj_prng_seed_fixed(&rs);

  for (round = 0; round < 8; round++) {
    TGAlloc alloc;
    LJArenaAllocD ad;
    void *p, *q, *huge, *small;
    size_t hsize = LJ_HUGE_THRESHOLD + 100u + round;

    lj_arena_alloc_init(&alloc);
    lj_arena_allocd_init(&ad, &alloc, &rs, 0);

    assert(lj_arena_allocf(&ad, NULL, 0, 0) == NULL);
    p = lj_arena_allocf(&ad, NULL, 0, 64);
    assert(p != NULL);
    assert(!lj_arena_ishuge(lj_arena_of(p)));
    fill_seq((uint8_t *)p, 64, (uint8_t)(0x20u + round));

    q = lj_arena_allocf(&ad, p, 64, 256);
    assert(q != NULL);
    check_seq((uint8_t *)q, 64, (uint8_t)(0x20u + round));
    p = q;

    assert(lj_arena_allocf(&ad, p, 256, ~(size_t)0) == NULL);
    check_seq((uint8_t *)p, 64, (uint8_t)(0x20u + round));

    huge = lj_arena_allocf(&ad, p, 256, hsize);
    assert(huge != NULL);
    assert(lj_arena_ishuge(lj_arena_of(huge)));
    check_seq((uint8_t *)huge, 64, (uint8_t)(0x20u + round));
    fill_seq((uint8_t *)huge, 128, (uint8_t)(0x50u + round));

    small = lj_arena_allocf(&ad, huge, hsize, 128);
    assert(small != NULL);
    assert(!lj_arena_ishuge(lj_arena_of(small)));
    check_seq((uint8_t *)small, 128, (uint8_t)(0x50u + round));
    assert(lj_arena_allocf(&ad, small, 128, 0) == NULL);

    lj_arena_alloc_fini(&alloc);
    assert(alloc.owned[LJ_ARENAK_TRAVERSABLE] == NULL);
    assert(alloc.owned[LJ_ARENAK_PLAIN] == NULL);
  }

  printf("t-arena-allocf OK: lua_Alloc shim transitions verified\n");
  return 0;
}
